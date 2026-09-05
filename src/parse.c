/*
#include <stdio.h>
 * The MDY front end, in C, exposed to QuickJS.
 *
 * WHY THIS IS THE ONE WORTH MOVING. A profile of a native corpus build put
 * every frame in JS_CallInternal, js_array_flatten, js_array_every and
 * generators — the JavaScript layer, with no native call appearing at all. The
 * front end is the largest single thing in there: 4,441 lines producing 285k
 * nodes from 6.5 MB, and measured at 8.8x slower under QuickJS than under V8.
 *
 * It is also the one that could move. hast is mdy-docs' extension point and
 * everything downstream reads it — rehype plugins, the query engine, a
 * template's `$.render` — so moving the PARSE does not move the extension
 * point. What comes out of here is the same tree the JavaScript built.
 *
 * The tree is built as QuickJS OBJECTS DIRECTLY rather than handed over as
 * JSON. JSON would be simpler and would mean parsing 285k nodes' worth of text
 * in the engine this is trying to keep out of the hot path — which is most of
 * the cost back.
 */
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "mdyast.h"

/*
 * Property NAMES are a closed set — nineteen of them across the whole
 * reference corpus — so their QuickJS atoms are worth keeping between calls:
 * 285k nodes otherwise pay a string hash per property.
 *
 * Keyed by the STRING, not by the parser's interned pointer, and that is not a
 * detail. Interning is per document and the arena dies with it, so a pointer
 * from one document is reused for a different name in the next — which turned
 * `dataFootnoteRef` into `height` in three of the corpus's pages, and only in
 * documents parsed after the first. A cache keyed on an address whose lifetime
 * it does not control is not a cache.
 */
#define ATOM_CACHE 64
static struct { char *name; JSAtom atom; } g_atoms[ATOM_CACHE];
static int g_atom_count;

static JSAtom atom_for(JSContext *ctx, const char *name) {
    for (int i = 0; i < g_atom_count; i++) {
        if (strcmp(g_atoms[i].name, name) == 0) return g_atoms[i].atom;
    }
    JSAtom a = JS_NewAtom(ctx, name);
    if (g_atom_count < ATOM_CACHE) {
        char *kept = strdup(name);      /* ours, so it outlives the parse */
        if (kept) {
            g_atoms[g_atom_count].name = kept;
            g_atoms[g_atom_count].atom = a;
            g_atom_count++;
        }
    }
    return a;
}

static JSValue build(JSContext *ctx, const mdy_node *n, int positions);

static JSValue build_children(JSContext *ctx, const mdy_node *n, int positions) {
    JSValue array = JS_NewArray(ctx);
    uint32_t i = 0;
    for (const mdy_node *c = n->first; c; c = c->next) {
        JS_SetPropertyUint32(ctx, array, i++, build(ctx, c, positions));
    }
    return array;
}

static JSValue point(JSContext *ctx, uint32_t line, uint32_t column) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "line", JS_NewUint32(ctx, line));
    JS_SetPropertyStr(ctx, o, "column", JS_NewUint32(ctx, column));
    return o;
}

static JSValue build(JSContext *ctx, const mdy_node *n, int positions) {
    JSValue node = JS_NewObject(ctx);

    switch (n->type) {
        case MDY_TEXT:
            JS_SetPropertyStr(ctx, node, "type", JS_NewString(ctx, "text"));
            JS_SetPropertyStr(ctx, node, "value", JS_NewString(ctx, n->text ? n->text : ""));
            return node;

        case MDY_DOCTYPE:
            /* `{type: "doctype"}` and nothing else — hast's doctype carries no
             * name. Omitting this case returned an object with no `type` at
             * all, which unist rejects with "Expected node, not [object
             * Object]" from somewhere far away. */
            JS_SetPropertyStr(ctx, node, "type", JS_NewString(ctx, "doctype"));
            return node;

        case MDY_ROOT:
            JS_SetPropertyStr(ctx, node, "type", JS_NewString(ctx, "root"));
            JS_SetPropertyStr(ctx, node, "children", build_children(ctx, n, positions));
            return node;

        case MDY_ELEMENT: {
            JS_SetPropertyStr(ctx, node, "type", JS_NewString(ctx, "element"));
            JS_SetPropertyStr(ctx, node, "tagName", JS_NewString(ctx, n->tag));

            JSValue props = JS_NewObject(ctx);
            for (const mdy_prop *p = n->props; p; p = p->next) {
                JSValue v;
                switch (p->type) {
                    case MDY_PROP_STRING: v = JS_NewString(ctx, p->as.string); break;
                    case MDY_PROP_NUMBER: v = JS_NewFloat64(ctx, p->as.number); break;
                    case MDY_PROP_BOOL:   v = JS_NewBool(ctx, p->as.boolean); break;
                    case MDY_PROP_LIST: {
                        v = JS_NewArray(ctx);
                        for (size_t k = 0; k < p->list_len; k++) {
                            JS_SetPropertyUint32(ctx, v, (uint32_t)k, JS_NewString(ctx, p->list[k]));
                        }
                        break;
                    }
                    default: v = JS_UNDEFINED;
                }
                JS_SetProperty(ctx, props, JS_DupAtom(ctx, atom_for(ctx, p->name)), v);
            }
            JS_SetPropertyStr(ctx, node, "properties", props);
            JS_SetPropertyStr(ctx, node, "children", build_children(ctx, n, positions));

            /* Positions go on last, which is where JSON.stringify puts them —
             * hast builds the node before attaching one, and anything
             * comparing serialised trees notices the difference. */
            if (positions && n->line) {
                JSValue pos = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, pos, "start", point(ctx, n->line, n->column));
                JS_SetPropertyStr(ctx, pos, "end", point(ctx, n->end_line, n->end_column));
                JS_SetPropertyStr(ctx, node, "position", pos);
            }
            return node;
        }
    }
    return node;
}

/*
 * `__mdy_parse(text, flags, lineOffset, wrapper, fence)` ->
 *   {tree, messages, matter, refs}
 *
 * More than the tree, because a front end produces more than a tree. The
 * warnings go on the vfile, the front matter is YAML for whoever embeds this
 * to read, and the references are what the document says it points at. Each
 * would otherwise have to be re-derived by reading the document again.
 *
 * The options that reach here are the ones that change what is produced.
 * Everything else mdy-docs' own `fromMdy` accepts belongs to a stage this does
 * not implement — see the shim, which is where that decision is written down
 * and where an unsupported option has to be noticed rather than ignored.
 */
#define MDY_FLAG_DOCUMENTS   1
#define MDY_FLAG_FRONTMATTER 2
#define MDY_FLAG_AUTOLINK    4
#define MDY_FLAG_POSITIONS   8
#define MDY_FLAG_SANITIZE   16

static JSValue messages_of(JSContext *ctx, const mdy_doc *doc) {
    JSValue out = JS_NewArray(ctx);
    for (size_t i = 0; i < mdy_message_count(doc); i++) {
        const mdy_message *m = mdy_message_at(doc, i);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "reason", JS_NewString(ctx, m->reason));
        JS_SetPropertyStr(ctx, o, "ruleId", JS_NewString(ctx, m->rule));
        /* A message with no place is not one with a wrong place — the inline
         * warnings genuinely have no line to point at. */
        if (m->line) {
            JSValue place = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, place, "start", point(ctx, m->line, m->column));
            JS_SetPropertyStr(ctx, place, "end", point(ctx, m->end_line, m->end_column));
            JS_SetPropertyStr(ctx, o, "place", place);
        }
        JS_SetPropertyUint32(ctx, out, (uint32_t)i, o);
    }
    return out;
}

static JSValue matter_of(JSContext *ctx, const mdy_doc *doc) {
    JSValue out = JS_NewArray(ctx);
    for (size_t i = 0; i < mdy_frontmatter_count(doc); i++) {
        const mdy_frontmatter *m = mdy_frontmatter_at(doc, i);
        if (!m->source) {
            JS_SetPropertyUint32(ctx, out, (uint32_t)i, JS_NULL);
            continue;
        }
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "source", JS_NewStringLen(ctx, m->source, m->source_len));
        JS_SetPropertyStr(ctx, o, "open", JS_NewInt32(ctx, (int32_t)m->open_line));
        JS_SetPropertyStr(ctx, o, "close", JS_NewInt32(ctx, (int32_t)m->close_line));
        JS_SetPropertyUint32(ctx, out, (uint32_t)i, o);
    }
    return out;
}

static JSValue refs_of(JSContext *ctx, const mdy_doc *doc) {
    static const char *const KIND[] = { "tag", "mention", "link" };
    JSValue out = JS_NewArray(ctx);
    for (size_t i = 0; i < mdy_reference_count(doc); i++) {
        const mdy_reference *r = mdy_reference_at(doc, i);
        JSValue o = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, KIND[r->kind]));
        JS_SetPropertyStr(ctx, o, "name", JS_NewStringLen(ctx, r->name, r->name_len));
        JS_SetPropertyStr(ctx, o, "document", JS_NewInt32(ctx, (int32_t)r->document));
        JS_SetPropertyUint32(ctx, out, (uint32_t)i, o);
    }
    return out;
}

JSValue mdy_native_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;

    size_t len = 0;
    const char *text = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!text) return JS_NULL;

    int32_t flags = MDY_FLAG_FRONTMATTER | MDY_FLAG_AUTOLINK | MDY_FLAG_POSITIONS | MDY_FLAG_SANITIZE;
    if (argc > 1) JS_ToInt32(ctx, &flags, argv[1]);
    int32_t line_offset = 0;
    if (argc > 2) JS_ToInt32(ctx, &line_offset, argv[2]);

    /* `documents: {wrapper}` — an empty string runs the documents together,
     * which is what `wrapper: false` means on the JavaScript's side. */
    const char *wrapper = NULL;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3]))
        wrapper = JS_ToCString(ctx, argv[3]);
    const char *fence = NULL;
    if (argc > 4 && !JS_IsUndefined(argv[4]) && !JS_IsNull(argv[4]))
        fence = JS_ToCString(ctx, argv[4]);

    mdy_options options;
    mdy_options_default(&options);
    options.documents   = (flags & MDY_FLAG_DOCUMENTS) != 0;
    options.frontmatter = (flags & MDY_FLAG_FRONTMATTER) != 0;
    options.autolink    = (flags & MDY_FLAG_AUTOLINK) != 0;
    options.positions   = (flags & MDY_FLAG_POSITIONS) != 0;
    options.sanitize    = (flags & MDY_FLAG_SANITIZE) != 0;
    options.line_offset = (uint32_t)line_offset;
    options.document_wrapper = wrapper;
    options.frontmatter_fence = fence;

    mdy_doc *doc = mdy_parse(text, len, &options);
    JS_FreeCString(ctx, text);
    if (wrapper) JS_FreeCString(ctx, wrapper);
    if (fence) JS_FreeCString(ctx, fence);
    if (!doc) return JS_NULL;

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "tree", build(ctx, mdy_root(doc), options.positions));
    JS_SetPropertyStr(ctx, out, "messages", messages_of(ctx, doc));
    JS_SetPropertyStr(ctx, out, "matter", matter_of(ctx, doc));
    JS_SetPropertyStr(ctx, out, "refs", refs_of(ctx, doc));
    mdy_free(doc);
    return out;
}
