/* The contract, and what is not here yet, is in engine.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "lamassu_compile.h"

#include "engine.h"
#include "mdyast.h"
#include "mdydata.h"
#include "mdydoc.h"
#include "mdyhtml.h"
#include "binjson.h"
#include "bplustree.h"
#include "db.h"
#include "fsx.h"
#include "ingest.h"

#include "mdybuild.h"
#include "mdymarkdown.h"
#include "mdyscript.h"
#include "mdytext.h"
#include "toolkit.h"

/* A tree a `$.render` parked, and the id of the token standing for it. */
/*
 * A parked tree, or a promise of one. `tree == NULL` with `is_toc` set is a
 * contents list: the token exists before the list can, because a document's
 * headings are not known until its whole tree is — including the ones a loop
 * below the contents list writes.
 */
typedef struct { char id[24]; mdy_doc *doc; mdy_node *tree; int is_toc; } Held;

/* One document of an open set. Its TEXT is here; its DATA is in nisaba. */
typedef struct {
    mdy_chunk chunk;      /* the document's own text */
    mdy_chunk matter;     /* its front matter, unparsed */
    mdy_data *fences;     /* its ```data fences, and the body without them */
    uint8_t oid[12];
} Document;

/* A resolved `% import name from "spec"`. `set` is owned by the cache, not
 * by the importer — a package imported twice is one build. */
typedef struct {
    char *source_path;   /* the file that declared it */
    char *spec;          /* the literal string it wrote */
    struct mdy_engine *set;
} Import;

/* Every package built so far, by absolute directory, shared across the graph.
 * `roots` is post-order — an import lands before whoever imported it — which
 * is the order `static/` must be copied in for a site to win over its theme. */
typedef struct {
    char **dirs;
    struct mdy_engine **sets;
    size_t count, cap;
    /* Post-order: a package lands after everything IT imports. `static/` is
     * copied in this order, so a site's own files win over its theme's —
     * the same precedence Hugo and Jekyll give a site over a theme. */
    char **roots;
    size_t root_count, root_cap;
} ImportCache;

struct mdy_engine {
    JsVm *vm;
    JsContext *ctx;
    /*
     * The render in progress. `$.compose` is a host call made from the middle
     * of one, and it needs the document being rendered; with one render at a
     * time this is where it lives.
     */
    mdy_doc *tree_owner;

    /* The open set. */
    Document *docs;
    size_t count;
    int handle;                 /* nisaba's, from nis_open */
    mdy_documents *source_docs;

    /*
     * Trees a `$.render` parked, and the tokens standing for them.
     *
     * The table is shared by the WHOLE import graph — `tokens` points at the
     * graph's root engine, or at this one for a standalone set. A token minted
     * while rendering a site and written into the data an imported layout
     * receives has to resolve THERE, and a per-package table cannot do that:
     * the layout would find no such token and quietly drop the page's entire
     * body. mdy-docs shares one module-level registry for the same reason.
     */
    struct mdy_engine *tokens;
    Held *held;
    size_t held_count, held_cap;
    size_t next_token;
    int depth;                  /* renders inside renders */
    void (*on_emit)(void *ud, const char *path, const char *content);
    void *on_emit_ud;
    void (*on_publish)(void *ud, const char *name, const char *data_json, size_t doc_index);
    void *on_publish_ud;
    /* `_id` to index, in insertion order, so a hit maps back to its document. */
    uint8_t (*ids)[12];

    /*
     * The import graph. `root` is this package's own directory; `imports` is
     * every `% import name from "spec"` any of its files declared, resolved.
     *
     * `cache` is shared by the WHOLE graph and owned by whoever built it —
     * the same package imported twice is built once. `current` is the
     * document being rendered, which is what tells an `$.__import*` native
     * which file's import it is being asked about: the same spec written in
     * two files can resolve to two different packages.
     */
    /*
     * A document's file identity — path, name, ext, size, mtime — as a YAML
     * mapping, one per document, or NULL for a set that did not come from a
     * directory.
     *
     * It is a MAPPING MERGED LAST rather than front matter written into the
     * source, and the difference is not cosmetic: a file with its own `+++`
     * block would otherwise have two of them, and the second would be read as
     * body text. Merging last is also what makes identity win over a field of
     * the same name, which is the rule mdy-docs states — a document's `name`
     * is its file's, never its front matter's.
     */
    char **identity;
    size_t identity_count;

    char *root;
    Import *imports;
    size_t import_count, import_cap;
    ImportCache *cache;
    int owns_cache;
    size_t current;
};

/* ---- strings across the boundary ------------------------------------------- */

static uint16_t *to_utf16(const char *in, size_t len, size_t *out_len) {
    uint16_t *out = malloc((len + 1) * sizeof *out);
    if (!out) { *out_len = 0; return NULL; }
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)in[i];
        unsigned cp;
        size_t w;
        if (c < 0x80) { cp = c; w = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; w = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; w = 3; }
        else { cp = c & 0x07; w = 4; }
        if (i + w > len) { cp = 0xFFFD; w = 1; }
        else for (size_t k = 1; k < w; k++) cp = (cp << 6) | ((unsigned char)in[i + k] & 0x3F);
        i += w;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            out[o++] = (uint16_t)(0xD800 + (cp >> 10));
            out[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else out[o++] = (uint16_t)cp;
    }
    *out_len = o;
    return out;
}

static char *from_utf16(const uint16_t *u, size_t len) {
    char *out = malloc(len * 4 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[++i] - 0xDC00);
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else { out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
    }
    out[o] = '\0';
    return out;
}

static JsValue str(JsVm *vm, const char *s, size_t len) {
    size_t n = 0;
    uint16_t *u = to_utf16(s, len, &n);
    JsValue v = u ? js_string_new(vm, u, n) : js_undefined();
    free(u);
    return v;
}

static JsValue key(JsVm *vm, const char *s) {
    size_t n = 0;
    uint16_t *u = to_utf16(s, strlen(s), &n);
    JsValue v = u ? js_atom(vm, u, n) : js_undefined();
    free(u);
    return v;
}

/*
 * A property set and an array push that ROOT what they are handed.
 *
 * js_object_set takes a garbage-collection safe point before it writes, and
 * its contract is that the caller has rooted the object, the key and the
 * value. A value this file has just built is reachable only from the C stack,
 * which the collector does not scan — so a collection at that safe point frees
 * it, the map keeps a dangling pointer, and the cell is handed to the next
 * string that asks for one. The property then silently becomes a DIFFERENT
 * one, with no crash and no error.
 *
 * That is not hypothetical: `link-titles` in a 642-key record came back with
 * `guraeans` gone and `hajj` present twice, so one link in a 93-page site
 * pointed at the wrong place. It reproduced only in a build long enough to
 * collect part-way through decoding a record.
 *
 * The key is built INSIDE, after the value is rooted, because building it
 * allocates too — a key made first, in the caller's argument list, can be
 * collected while the value beside it is still being built.
 */
static void set_val(mdy_engine *e, JsValue obj, const char *name, JsValue v) {
    js_gc_protect(e->vm, &obj);
    js_gc_protect(e->vm, &v);
    JsValue k = key(e->vm, name);
    js_gc_protect(e->vm, &k);
    js_object_set(e->vm, obj, k, v);
    js_gc_unprotect(e->vm, &k);
    js_gc_unprotect(e->vm, &v);
    js_gc_unprotect(e->vm, &obj);
}

static void push_item(mdy_engine *e, JsValue array, JsValue v) {
    js_gc_protect(e->vm, &array);
    js_gc_protect(e->vm, &v);
    js_array_push(e->vm, array, v);
    js_gc_unprotect(e->vm, &v);
    js_gc_unprotect(e->vm, &array);
}


/* ---- a tree, across the boundary --------------------------------------------
 *
 * `transform((tree) => …)` is the one place a document's own code sees the
 * tree, so the tree has to reach the guest and come back. mdy-docs sends it as
 * JSON in both directions; here it is built as VALUES, which is what
 * js_array_new and js_object_new made possible.
 *
 * The shape is hast's, exactly as the JSON was: every node has a `type`, an
 * element has `tagName`, `properties` and `children`, and a text node has a
 * `value`. A transform written against one works against the other.
 */

static JsValue tree_to_js(mdy_engine *e, const mdy_node *n);

static JsValue children_to_js(mdy_engine *e, const mdy_node *n) {
    JsValue array = js_array_new(e->ctx, 0);
    js_gc_protect(e->vm, &array);
    for (const mdy_node *c = n->first; c; c = c->next) {
        JsValue child = tree_to_js(e, c);
        js_gc_protect(e->vm, &child);
        push_item(e, array, child);
        js_gc_unprotect(e->vm, &child);
    }
    js_gc_unprotect(e->vm, &array);
    return array;
}

static JsValue tree_to_js(mdy_engine *e, const mdy_node *n) {
    JsValue o = js_object_new(e->ctx);
    js_gc_protect(e->vm, &o);

    switch (n->type) {
        case MDY_TEXT:
        case MDY_RAW:
        case MDY_COMMENT: {
            const char *type = n->type == MDY_TEXT ? "text"
                             : n->type == MDY_RAW ? "raw" : "comment";
            set_val(e, o, "type", str(e->vm, type, strlen(type)));
            const char *v = n->text ? n->text : "";
            set_val(e, o, "value", str(e->vm, v, strlen(v)));
            break;
        }
        case MDY_DOCTYPE:
            set_val(e, o, "type", str(e->vm, "doctype", 7));
            break;
        case MDY_ROOT:
            set_val(e, o, "type", str(e->vm, "root", 4));
            set_val(e, o, "children", children_to_js(e, n));
            break;
        case MDY_ELEMENT: {
            set_val(e, o, "type", str(e->vm, "element", 7));
            set_val(e, o, "tagName", str(e->vm, n->tag, strlen(n->tag)));
            JsValue props = js_object_new(e->ctx);
            js_gc_protect(e->vm, &props);
            for (const mdy_prop *p = n->props; p; p = p->next) {
                JsValue v;
                switch (p->type) {
                    case MDY_PROP_STRING: v = str(e->vm, p->as.string, strlen(p->as.string)); break;
                    case MDY_PROP_NUMBER: v = js_number(p->as.number); break;
                    case MDY_PROP_BOOL:   v = js_bool(p->as.boolean != 0); break;
                    case MDY_PROP_LIST: {
                        v = js_array_new(e->ctx, (uint32_t)p->list_len);
                        js_gc_protect(e->vm, &v);
                        for (size_t i = 0; i < p->list_len; i++)
                            push_item(e, v, str(e->vm, p->list[i], strlen(p->list[i])));
                        js_gc_unprotect(e->vm, &v);
                        break;
                    }
                    default: v = js_undefined();
                }
                set_val(e, props, p->name, v);
            }
            set_val(e, o, "properties", props);
            js_gc_unprotect(e->vm, &props);
            set_val(e, o, "children", children_to_js(e, n));
            break;
        }
    }

    js_gc_unprotect(e->vm, &o);
    return o;
}

/* The string a JS value holds, as UTF-8. Caller frees. NULL when it is not a
 * string — which for a `type` or a `tagName` means the guest handed back
 * something that is not a node. */
static char *js_string_utf8(JsValue v) {
    size_t ulen = 0;
    const uint16_t *u = js_string_units(v, &ulen);
    return u ? from_utf16(u, ulen) : NULL;
}

static mdy_node *js_to_tree(mdy_engine *e, mdy_doc *doc, JsValue v);

static void js_children_to_tree(mdy_engine *e, mdy_doc *doc, mdy_node *parent, JsValue kids) {
    if (!js_is_array(kids)) return;
    uint32_t n = js_array_length(kids);
    for (uint32_t i = 0; i < n; i++) {
        mdy_node *child = js_to_tree(e, doc, js_array_get(kids, i));
        if (child) mdy_append(parent, child);
    }
}

static mdy_node *js_to_tree(mdy_engine *e, mdy_doc *doc, JsValue v) {
    if (!js_is_object(v)) return NULL;

    char *type = js_string_utf8(js_object_get(e->vm, v, key(e->vm, "type")));
    if (!type) return NULL;

    mdy_node *out = NULL;
    if (strcmp(type, "text") == 0 || strcmp(type, "raw") == 0 || strcmp(type, "comment") == 0) {
        char *value = js_string_utf8(js_object_get(e->vm, v, key(e->vm, "value")));
        out = mdy_new_text(doc, value ? value : "", value ? strlen(value) : 0);
        if (out) out->type = strcmp(type, "raw") == 0 ? MDY_RAW
                           : strcmp(type, "comment") == 0 ? MDY_COMMENT : MDY_TEXT;
        free(value);
    } else if (strcmp(type, "doctype") == 0) {
        out = mdy_new_text(doc, "", 0);
        if (out) out->type = MDY_DOCTYPE;
    } else if (strcmp(type, "root") == 0) {
        out = mdy_new_text(doc, "", 0);
        if (out) {
            out->type = MDY_ROOT;
            out->text = NULL;
            js_children_to_tree(e, doc, out, js_object_get(e->vm, v, key(e->vm, "children")));
        }
    } else if (strcmp(type, "element") == 0) {
        char *tag = js_string_utf8(js_object_get(e->vm, v, key(e->vm, "tagName")));
        out = mdy_new_element(doc, tag ? tag : "div", tag ? strlen(tag) : 3);
        free(tag);
        if (out) {
            /*
             * Properties come back by NAME, and the names a guest may have
             * added are not known in advance — so this walks whatever is
             * there rather than a fixed list. `className` is the one that is
             * an array; everything else is a string, a number or a boolean.
             */
            JsValue props = js_object_get(e->vm, v, key(e->vm, "properties"));
            if (js_is_object(props)) {
                for (size_t i = 0; i < js_object_size(props); i++) {
                    JsValue name = js_object_key_at(props, i);
                    char *pname = js_string_utf8(name);
                    if (!pname) continue;
                    JsValue pv = js_object_get(e->vm, props, name);
                    if (js_is_array(pv)) {
                        uint32_t n = js_array_length(pv);
                        for (uint32_t k = 0; k < n; k++) {
                            char *item = js_string_utf8(js_array_get(pv, k));
                            if (item && strcmp(pname, "className") == 0) mdy_add_class(doc, out, item);
                            free(item);
                        }
                    } else if (js_is_number(pv)) {
                        mdy_set_number(doc, out, pname, js_get_number(pv));
                    } else if (js_is_bool(pv)) {
                        mdy_set_bool(doc, out, pname, js_get_bool(pv));
                    } else {
                        char *sv = js_string_utf8(pv);
                        if (sv) mdy_set_string(doc, out, pname, sv, strlen(sv));
                        free(sv);
                    }
                    free(pname);
                }
            }
            js_children_to_tree(e, doc, out, js_object_get(e->vm, v, key(e->vm, "children")));
        }
    }

    free(type);
    return out;
}


/* ---- the document set --------------------------------------------------------
 *
 * Opening a source is mdy-docs' `openDocumentSet`: every document's data — its
 * front matter merged with its ```data fences — goes into a nisaba collection,
 * and `$.find` runs real queries against it.
 *
 * The `_id` to index map is not bookkeeping. A query answers in whatever order
 * the database walks its keys, and a document set answers in DOCUMENT order —
 * so a hit is mapped back to the document it came from and the answer sorted
 * by that. Without it a page's list of siblings would reorder between builds.
 */

/* ---- a JS value as binjson, for a query filter ------------------------------ */

static int js_to_binjson(mdy_engine *e, bj_builder *b, JsValue v) {
    if (js_is_null(v) || js_is_undefined(v)) return bj_put_null(b);
    if (js_is_bool(v)) return bj_put_bool(b, js_get_bool(v));
    if (js_is_number(v)) {
        double d = js_get_number(v);
        if (d == (double)(int64_t)d && d >= -9.2e18 && d <= 9.2e18)
            return bj_put_int(b, (int64_t)d);
        return bj_put_float(b, d);
    }
    if (js_is_string(v)) {
        char *s = js_string_utf8(v);
        if (!s) return -1;
        int rc = bj_put_string(b, (const uint8_t *)s, (uint32_t)strlen(s));
        free(s);
        return rc;
    }
    if (js_is_array(v)) {
        if (bj_begin_array(b) != 0) return -1;
        uint32_t n = js_array_length(v);
        for (uint32_t i = 0; i < n; i++)
            if (js_to_binjson(e, b, js_array_get(v, i)) != 0) return -1;
        return bj_end_array(b);
    }
    if (js_is_object(v)) {
        if (bj_begin_object(b) != 0) return -1;
        size_t n = js_object_size(v);
        for (size_t i = 0; i < n; i++) {
            JsValue k = js_object_key_at(v, i);
            char *name = js_string_utf8(k);
            if (!name) continue;
            int rc = bj_put_key(b, (const uint8_t *)name, (uint32_t)strlen(name));
            free(name);
            if (rc != 0) return -1;
            if (js_to_binjson(e, b, js_object_get(e->vm, v, k)) != 0) return -1;
        }
        return bj_end_object(b);
    }
    return bj_put_null(b);
}

/* ---- binjson back as JS values ----------------------------------------------
 *
 * The decoder is a visitor, so this keeps a stack of the containers it is
 * inside and hangs each finished value on whichever is on top.
 */
enum { BJ_STACK_MAX = 64 };

typedef struct {
    mdy_engine *e;
    JsValue stack[BJ_STACK_MAX];
    char *keys[BJ_STACK_MAX];
    int depth;
    JsValue result;
    int have_result;
} Decode;

static void decode_put(Decode *d, JsValue v) {
    if (d->depth == 0) { d->result = v; d->have_result = 1; return; }
    JsValue parent = d->stack[d->depth - 1];
    if (js_is_array(parent)) {
        push_item(d->e, parent, v);
    } else {
        char *k = d->keys[d->depth - 1];
        if (k) {
            set_val(d->e, parent, k, v);
            free(k);
            d->keys[d->depth - 1] = NULL;
        }
    }
}

static void d_null(void *ctx) { decode_put(ctx, js_null()); }
static void d_bool(void *ctx, int t) { decode_put(ctx, js_bool(t != 0)); }
static void d_int(void *ctx, double v) { decode_put(ctx, js_number(v)); }
static void d_float(void *ctx, double v) { decode_put(ctx, js_number(v)); }
static void d_date(void *ctx, double v) { decode_put(ctx, js_number(v)); }
static void d_pointer(void *ctx, double v) { decode_put(ctx, js_number(v)); }
static void d_string(void *ctx, const uint8_t *s, uint32_t n) {
    Decode *d = ctx;
    decode_put(d, str(d->e->vm, (const char *)s, n));
}
static void d_binary(void *ctx, const uint8_t *s, uint32_t n) {
    (void)s; (void)n;
    decode_put(ctx, js_null());
}
/* `_id` is an OID, and a document set's own key: guest code has no use for
 * the bytes, and a string of them is the shape mdy-docs hands over. */
static void d_oid(void *ctx, const uint8_t *b) {
    Decode *d = ctx;
    char hex[25];
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 12; i++) { hex[i * 2] = H[b[i] >> 4]; hex[i * 2 + 1] = H[b[i] & 15]; }
    hex[24] = '\0';
    decode_put(d, str(d->e->vm, hex, 24));
}
/*
 * A container under construction is rooted through ITS SLOT ON THE STACK, not
 * through a local.
 *
 * js_gc_protect records an ADDRESS and the collector dereferences it later; a
 * local's address is dead the moment the callback returns, so protecting `&a`
 * here left the root table pointing into a reused stack frame. Nothing went
 * wrong until a collection happened to land mid-decode, and then the GC read
 * whatever was in that slot and tried to mark it — a crash whose cause is
 * nowhere near where it lands. `Decode` lives for the whole decode, so its
 * slots are the addresses that are actually valid to hand out.
 */
static void d_array_begin(void *ctx, uint32_t count) {
    Decode *d = ctx;
    if (d->depth >= BJ_STACK_MAX) return;
    d->stack[d->depth] = js_array_new(d->e->ctx, count);
    js_gc_protect(d->e->vm, &d->stack[d->depth]);
    d->keys[d->depth] = NULL;
    d->depth++;
}
static void d_object_begin(void *ctx, uint32_t count) {
    Decode *d = ctx;
    (void)count;
    if (d->depth >= BJ_STACK_MAX) return;
    d->stack[d->depth] = js_object_new(d->e->ctx);
    js_gc_protect(d->e->vm, &d->stack[d->depth]);
    d->keys[d->depth] = NULL;
    d->depth++;
}
static void d_key(void *ctx, const uint8_t *s, uint32_t n) {
    Decode *d = ctx;
    if (d->depth == 0) return;
    free(d->keys[d->depth - 1]);
    d->keys[d->depth - 1] = malloc(n + 1);
    if (d->keys[d->depth - 1]) {
        memcpy(d->keys[d->depth - 1], s, n);
        d->keys[d->depth - 1][n] = '\0';
    }
}
static void d_end(void *ctx) {
    Decode *d = ctx;
    if (d->depth == 0) return;
    int at = --d->depth;
    free(d->keys[at]);
    d->keys[at] = NULL;
    /* Still rooted through its slot while decode_put allocates into the
     * parent: unprotecting first would let the finished value be collected by
     * the very push meant to keep it. */
    decode_put(d, d->stack[at]);
    js_gc_unprotect(d->e->vm, &d->stack[at]);
}

static JsValue binjson_to_js(mdy_engine *e, const uint8_t *bytes, size_t len, size_t *consumed) {
    Decode d = {0};
    d.e = e;
    d.result = js_undefined();
    bj_visitor v = {
        .on_null = d_null, .on_bool = d_bool, .on_int = d_int, .on_float = d_float,
        .on_string = d_string, .on_binary = d_binary, .on_oid = d_oid, .on_date = d_date,
        .on_pointer = d_pointer, .on_array_begin = d_array_begin, .on_array_end = d_end,
        .on_object_begin = d_object_begin, .on_key = d_key, .on_object_end = d_end,
        .ctx = &d,
    };
    /* The finished value is a root too — a container completing puts it here
     * while the decode is still allocating. */
    js_gc_protect(e->vm, &d.result);
    int rc = bj_decode(bytes, len, &v, consumed);
    js_gc_unprotect(e->vm, &d.result);
    return rc == 0 ? d.result : js_undefined();
}



/* ---- composition -------------------------------------------------------------
 *
 * `$.render` does not return HTML, or a tree, or text. It returns a TOKEN — a
 * few private-use characters standing for a tree the host has parked — and the
 * token travels through the document's own code like any other string, into a
 * variable, a template literal, an attribute. The tree goes back in once the
 * text around it has been parsed.
 *
 * That is what makes `$.render` need no indentation argument: the parser
 * already knows which element is open where the token landed, so there is no
 * column for the caller to compute.
 *
 *   U+E000 <id> U+E001
 *
 * The id is base36 so a counter and a content-derived identity are both
 * covered by one pattern — three regexes in compose.js have to agree about
 * what an id looks like, and once they did not.
 */
#define TOKEN_OPEN  "\xee\x80\x80"      /* U+E000 as UTF-8 */
#define TOKEN_CLOSE "\xee\x80\x81"      /* U+E001 */

/* A token's id at `s`, or 0. Writes the id and how many bytes it spanned. */
static size_t token_at(const char *s, size_t len, char *id, size_t id_cap) {
    if (len < 5 || memcmp(s, TOKEN_OPEN, 3) != 0) return 0;
    size_t i = 3;
    size_t n = 0;
    while (i < len && n + 1 < id_cap) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) { id[n++] = c; i++; continue; }
        break;
    }
    id[n] = '\0';
    if (n == 0) return 0;
    if (i + 3 > len || memcmp(s + i, TOKEN_CLOSE, 3) != 0) return 0;
    return i + 3;
}

/* Whoever owns the token table this engine writes into. */
static mdy_engine *token_table(mdy_engine *e) { return e->tokens ? e->tokens : e; }

static Held *held_find(mdy_engine *e, const char *id) {
    mdy_engine *t = token_table(e);
    for (size_t i = 0; i < t->held_count; i++)
        if (strcmp(t->held[i].id, id) == 0) return &t->held[i];
    return NULL;
}

/*
 * Park a tree and hand back the token that stands for it.
 *
 * Nothing is reclaimed within a render: a token can be written into a string,
 * kept in a variable, dropped, or used twice, and nothing here gets to decide
 * when the last of those happened.
 */
static char *hold_tree(mdy_engine *e, mdy_doc *doc, mdy_node *tree) {
    mdy_engine *t = token_table(e);
    if (t->held_count == t->held_cap) {
        size_t want = t->held_cap ? t->held_cap * 2 : 8;
        Held *grown = realloc(t->held, want * sizeof *grown);
        if (!grown) return NULL;
        t->held = grown;
        t->held_cap = want;
    }
    Held *h = &t->held[t->held_count++];
    snprintf(h->id, sizeof h->id, "%zu", t->next_token++);
    h->doc = doc;
    h->tree = tree;
    h->is_toc = 0;

    size_t n = strlen(TOKEN_OPEN) + strlen(h->id) + strlen(TOKEN_CLOSE) + 1;
    char *token = malloc(n);
    if (token) snprintf(token, n, "%s%s%s", TOKEN_OPEN, h->id, TOKEN_CLOSE);
    return token;
}

static void release_held(mdy_engine *e) {
    mdy_engine *t = token_table(e);
    for (size_t i = 0; i < t->held_count; i++) mdy_free(t->held[i].doc);
    free(t->held);
    t->held = NULL;
    t->held_count = t->held_cap = 0;
}

/* Whitespace, and nothing else. */
static int only_space(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r') return 0;
    return 1;
}

/* `^(?:\s*TOKEN)+\s*$` — a run that is nothing but tokens and space. */
static int only_tokens(const char *s, size_t len) {
    size_t i = 0;
    int found = 0;
    for (;;) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i >= len) return found;
        char id[24];
        size_t used = token_at(s + i, len - i, id, sizeof id);
        if (!used) return 0;
        i += used;
        found = 1;
    }
}

/* Elements that hold a line of their own, and so cannot hold one of somebody
 * else's — the ones a nested render is likely to produce at its top level. */
static int is_block_tag(const char *tag) {
    static const char *const BLOCK[] = { "p", "div", "section", "article", "main", "header", "footer" };
    for (size_t i = 0; i < sizeof BLOCK / sizeof BLOCK[0]; i++)
        if (strcmp(BLOCK[i], tag) == 0) return 1;
    return 0;
}

/*
 * Block wrappers off, phrasing content out. A block cannot sit inside a
 * sentence, so it gives up its wrapper and lends its content instead — as far
 * down as the blocks go, since unwrapping a <div> only to find a <p> under it
 * has solved nothing.
 */
static void unwrap_into(mdy_node *dest, mdy_node *source) {
    for (mdy_node *c = source->first; c;) {
        mdy_node *next = c->next;
        c->next = NULL;
        if (c->type == MDY_ELEMENT && is_block_tag(c->tag)) unwrap_into(dest, c);
        else if (c->type == MDY_TEXT && only_space(c->text ? c->text : "", c->text ? strlen(c->text) : 0)) ;
        else mdy_append(dest, c);
        c = next;
    }
}

/* What a held tree contributes where a BLOCK was expected: a root lends its
 * children, anything else stands for itself. */
static void block_content(mdy_node *dest, mdy_node *tree) {
    if (tree->type == MDY_ROOT) {
        for (mdy_node *c = tree->first; c;) {
            mdy_node *next = c->next;
            c->next = NULL;
            mdy_append(dest, c);
            c = next;
        }
    } else {
        mdy_append(dest, tree);
    }
}

/*
 * Tokens in a run of TEXT, as the inline content they become. Anything that is
 * not a token stays the text it was.
 */
static void inline_content(mdy_engine *e, mdy_doc *doc, mdy_node *dest,
                           const char *s, size_t len) {
    size_t i = 0, last = 0;
    while (i < len) {
        char id[24];
        size_t used = token_at(s + i, len - i, id, sizeof id);
        if (!used) { i++; continue; }
        if (i > last) mdy_append(dest, mdy_new_text(doc, s + last, i - last));
        Held *h = held_find(e, id);
        if (h && h->tree) {
            mdy_node *holder = mdy_new_element(doc, "span", 4);
            /*
             * A COPY, because a token can be used more than once — a site
             * renders its colophon once and passes the same token to every
             * page. Splicing moves nodes, so the second use would find the
             * tree already emptied and contribute only its whitespace: a
             * growing run of blank lines, one per page that reused it.
             */
            block_content(holder, mdy_clone(doc, h->tree));
            unwrap_into(dest, holder);
        } else {
            mdy_append(dest, mdy_new_text(doc, s + i, used));
        }
        i += used;
        last = i;
    }
    if (last < len) mdy_append(dest, mdy_new_text(doc, s + last, len - last));
}

/* The text of a node that is a `<p>` holding one text child, or a text node —
 * which is what `onlyTokens` asks about. */
static const char *sole_text(const mdy_node *n, size_t *len) {
    if (n->type == MDY_TEXT) { *len = n->text ? strlen(n->text) : 0; return n->text; }
    if (n->type == MDY_ELEMENT && strcmp(n->tag, "p") == 0 && n->first &&
        n->first == n->last && n->first->type == MDY_TEXT) {
        *len = n->first->text ? strlen(n->first->text) : 0;
        return n->first->text;
    }
    return NULL;
}

/*
 * Put the held trees back where their tokens are.
 *
 * A paragraph holding nothing but tokens is REPLACED by what they hold — a
 * render on a line of its own is that document, not a paragraph wrapping it.
 * A token inside a sentence gives up its blocks instead.
 */
static void splice_tree(mdy_engine *e, mdy_doc *doc, mdy_node *parent) {
    mdy_node *child = parent->first;
    parent->first = parent->last = NULL;

    while (child) {
        mdy_node *next = child->next;
        child->next = NULL;

        size_t len = 0;
        const char *text = sole_text(child, &len);

        if (text && only_tokens(text, len)) {
            size_t i = 0;
            int filled = 0;
            while (i < len) {
                char id[24];
                size_t used = token_at(text + i, len - i, id, sizeof id);
                if (!used) { i++; continue; }
                Held *h = held_find(e, id);
                if (h && h->tree) { block_content(parent, mdy_clone(doc, h->tree)); filled = 1; }
                i += used;
            }
            if (filled) { child = next; continue; }
        }

        if (child->type == MDY_TEXT && child->text && strstr(child->text, TOKEN_OPEN)) {
            inline_content(e, doc, parent, child->text, strlen(child->text));
            child = next;
            continue;
        }

        if (child->first) splice_tree(e, doc, child);
        mdy_append(parent, child);
        child = next;
    }
}

/*
 * A string holding tokens, as HTML — the string-shaped half of composition.
 * `$.emit(url, $.render(page))` writes a page because a file is a string and
 * that is the shape it can hold.
 */
static char *fill_tokens(mdy_engine *e, const char *s, size_t len) {
    size_t cap = len + 256, out = 0;
    char *result = malloc(cap);
    if (!result) return NULL;
    result[0] = '\0';

    size_t i = 0, last = 0;
    while (i < len) {
        char id[24];
        size_t used = token_at(s + i, len - i, id, sizeof id);
        if (!used) { i++; continue; }

        Held *h = held_find(e, id);
        char *html = h && h->tree ? mdy_to_html(h->tree, NULL) : NULL;
        size_t plain = i - last;
        size_t add = plain + (html ? strlen(html) : 0);
        if (out + add + 1 > cap) {
            while (out + add + 1 > cap) cap *= 2;
            char *grown = realloc(result, cap);
            if (!grown) { free(html); free(result); return NULL; }
            result = grown;
        }
        memcpy(result + out, s + last, plain);
        out += plain;
        if (html) { memcpy(result + out, html, strlen(html)); out += strlen(html); free(html); }
        result[out] = '\0';
        i += used;
        last = i;
    }

    size_t tail = len - last;
    if (out + tail + 1 > cap) {
        char *grown = realloc(result, out + tail + 1);
        if (!grown) { free(result); return NULL; }
        result = grown;
    }
    memcpy(result + out, s + last, tail);
    out += tail;
    result[out] = '\0';
    return result;
}



/* ---- a directory as a document set -------------------------------------------
 *
 * mdy-docs' `walkSources`: every file becomes a document, and what its own
 * FILE FORMAT means is applied — not a site-building convention like "posts
 * live in posts/", which stays the entry script's business.
 *
 *   .mdy         real text, compiled as a template
 *   .md          real text, NEVER compiled — a bare `---` or a literal `{{ }}`
 *                in prose must not be misread — so the text lands in
 *                `meta.body`, directly findable, and the document itself is a
 *                placeholder
 *   .yaml/.yml   parsed as a mapping and merged into the record; identity
 *                still wins for `path`
 *   anything     identity alone, so a file is still a queryable document
 *
 * dist/, node_modules/ and dotfiles are not sources.
 */

/* U+200B: survives "a whitespace-only document is dropped" while staying
 * invisible if anything ever did render it. */
#define PLACEHOLDER_BODY "\xe2\x80\x8b"

static int is_source(const char *rel) {
    const char *at = rel;
    for (;;) {
        if (strncmp(at, "dist/", 5) == 0 || strncmp(at, "node_modules/", 13) == 0 || at[0] == '.')
            return 0;
        const char *slash = strchr(at, '/');
        if (!slash) return 1;
        at = slash + 1;
    }
}

static const char *basename_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* The extension including the dot, or "" — a leading dot is a dotfile rather
 * than an extension. */
static const char *extension_of(const char *name) {
    const char *dot = strrchr(name, '.');
    return (!dot || dot == name) ? "" : dot;
}

static int ends_with_ci(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (m > n) return 0;
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}


/*
 * A .md file's text, written into front matter as a YAML literal block
 * scalar. Every line is indented by two, and the indentation indicator is
 * EXPLICIT (`|2`) rather than inferred: a body whose first line begins with a
 * space would otherwise set the block's indent from that line and silently
 * shift the whole thing. The chomping indicator carries the trailing newlines,
 * which are part of the text and must come back exactly — `-` strips, plain
 * clips to one, `+` keeps them all.
 */
static void put_block_scalar(char **buf, size_t *len, size_t *cap,
                             const char *keyname, const char *text, size_t tlen) {
    size_t need = *len + tlen * 2 + strlen(keyname) + 64;
    if (need > *cap) {
        while (need > *cap) *cap *= 2;
        char *grown = realloc(*buf, *cap);
        if (!grown) return;
        *buf = grown;
    }
    if (tlen == 0) {
        *len += (size_t)snprintf(*buf + *len, *cap - *len, "%s: \"\"\n", keyname);
        return;
    }
    size_t trailing = 0;
    while (trailing < tlen && text[tlen - 1 - trailing] == '\n') trailing++;
    const char *chomp = trailing == 0 ? "-" : (trailing == 1 ? "" : "+");
    *len += (size_t)snprintf(*buf + *len, *cap - *len, "%s: |2%s\n", keyname, chomp);

    size_t body_end = tlen - trailing;
    size_t at = 0;
    while (at < body_end) {
        const char *nl = memchr(text + at, '\n', body_end - at);
        size_t line_len = nl ? (size_t)(nl - (text + at)) : body_end - at;
        if (line_len > 0) {
            memcpy(*buf + *len, "  ", 2);
            *len += 2;
            memcpy(*buf + *len, text + at, line_len);
            *len += line_len;
        }
        (*buf)[(*len)++] = '\n';
        at += line_len + 1;
    }
    /* `+` keeps every trailing newline, so they have to be written out. */
    if (trailing > 1) for (size_t i = 1; i < trailing; i++) (*buf)[(*len)++] = '\n';
    (*buf)[*len] = '\0';
}

/*
 * `extractTags` from mdy.js, for a .md file — the `#tags` its prose mentions,
 * lowercased, each once, in the order they are reached.
 *
 * What is NOT prose is skipped first: script lines (asked of the real
 * scanner, so a `%` inside a template literal is not mistaken for one), fenced
 * blocks, `{{ }}` expressions and inline code spans. A `#tag` inside any of
 * those is not a tag, and the whole reason to strip them is that a shell
 * comment in a fenced example otherwise becomes one.
 */
static int tag_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '/' || c >= 0x80;
}

static void put_tags(char **buf, size_t *len, size_t *cap, const char *text, size_t tlen) {
    mdy_script *script = mdy_script_compile(text, tlen);

    char *prose = malloc(tlen + 1);
    if (!prose) { mdy_script_free(script); return; }
    size_t plen = 0;
    size_t at = 0, line_no = 0;
    int in_fence = 0;
    while (at <= tlen) {
        const char *nl = at < tlen ? memchr(text + at, '\n', tlen - at) : NULL;
        size_t line_len = nl ? (size_t)(nl - (text + at)) : tlen - at;
        const char *line = text + at;

        size_t lead = 0;
        while (lead < line_len && (line[lead] == ' ' || line[lead] == '\t')) lead++;
        int fence = line_len - lead >= 3 &&
                    (memcmp(line + lead, "```", 3) == 0 || memcmp(line + lead, "~~~", 3) == 0);

        int is_code = script && mdy_script_is_code(script, line_no);
        if (!is_code && fence) {
            in_fence = !in_fence;
        } else if (!is_code && !in_fence) {
            memcpy(prose + plen, line, line_len);
            plen += line_len;
            prose[plen++] = '\n';
        }
        if (!nl) break;
        at += line_len + 1;
        line_no++;
    }
    mdy_script_free(script);

    /* `{{ … }}` and `` `…` `` become a space, so a tag against one does not
     * join the text either side of it. */
    for (size_t i = 0; i < plen;) {
        if (i + 1 < plen && prose[i] == '{' && prose[i + 1] == '{') {
            size_t j = i + 2;
            while (j + 1 < plen && !(prose[j] == '}' && prose[j + 1] == '}')) j++;
            size_t end = j + 1 < plen ? j + 2 : plen;
            memset(prose + i, ' ', end - i);
            i = end;
        } else if (prose[i] == '`') {
            size_t j = i + 1;
            while (j < plen && prose[j] != '`' && prose[j] != '\n') j++;
            if (j < plen && prose[j] == '`') { memset(prose + i, ' ', j + 1 - i); i = j + 1; }
            else i++;
        } else i++;
    }

    char (*tags)[128] = NULL;
    size_t tag_count = 0, tag_cap = 0;
    for (size_t i = 0; i < plen; i++) {
        if (prose[i] != '#') continue;
        /* A tag starts at a boundary and its first character is a letter. */
        if (i > 0) {
            unsigned char prev = (unsigned char)prose[i - 1];
            if (tag_char(prev) || prev == '#') continue;
        }
        size_t j = i + 1;
        if (j >= plen) break;
        unsigned char first = (unsigned char)prose[j];
        if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first >= 0x80))
            continue;
        while (j < plen && tag_char((unsigned char)prose[j])) j++;
        size_t name_len = j - i - 1;
        if (name_len == 0 || name_len >= 128) { i = j; continue; }

        char name[128];
        for (size_t k = 0; k < name_len; k++) {
            char c = prose[i + 1 + k];
            name[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        name[name_len] = '\0';

        int seen = 0;
        for (size_t k = 0; k < tag_count && !seen; k++) seen = strcmp(tags[k], name) == 0;
        if (!seen) {
            if (tag_count == tag_cap) {
                tag_cap = tag_cap ? tag_cap * 2 : 8;
                void *grown = realloc(tags, tag_cap * sizeof *tags);
                if (!grown) break;
                tags = grown;
            }
            memcpy(tags[tag_count++], name, name_len + 1);
        }
        i = j - 1;
    }
    free(prose);

    if (tag_count > 0) {
        size_t need = *len + tag_count * 132 + 32;
        if (need > *cap) {
            while (need > *cap) *cap *= 2;
            char *grown = realloc(*buf, *cap);
            if (!grown) { free(tags); return; }
            *buf = grown;
        }
        *len += (size_t)snprintf(*buf + *len, *cap - *len, "tags:\n");
        for (size_t k = 0; k < tag_count; k++)
            *len += (size_t)snprintf(*buf + *len, *cap - *len, "  - \"%s\"\n", tags[k]);
    }
    free(tags);
}


/* ---- the import graph --------------------------------------------------------
 *
 * `% import style from "../style-antiquity"` — another mdy package, walked and
 * compiled into its OWN document set rather than merged into this one. The
 * imported package's own `$.find` and `$.render` keep working exactly as they
 * would standalone: its `layouts/base.mdy` does not collide with the
 * importer's file of the same name, and neither package has to know it is
 * importable. The importer reaches in explicitly, through the object the
 * import binds.
 *
 * `import` is rewritten HERE, not parsed by the JS engine, for the reason
 * imports.js gives: a real `import` statement is not legal inside a function
 * body, and every `%` line ends up inside one. So a recognised shape becomes
 * ordinary VM-legal JS before the compiler ever sees it — symmetrical to the
 * ```data fences.
 */

/* Pure POSIX string math. Every root this deals with is POSIX-shaped, and a
 * resolved directory may be virtual, so this is both correct and portable in
 * a way reaching for the platform's path handling would not be. */
static void dirname_of(const char *p, char *out, size_t out_len) {
    const char *slash = strrchr(p, '/');
    if (!slash) { snprintf(out, out_len, "."); return; }
    if (slash == p) { snprintf(out, out_len, "/"); return; }
    size_t n = (size_t)(slash - p);
    if (n >= out_len) n = out_len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* `spec` against `base`, with `.` and `..` collapsed. */
static void resolve_path(const char *base, const char *spec, char *out, size_t out_len) {
    char combined[4096];
    if (spec[0] == '/') snprintf(combined, sizeof combined, "%s", spec);
    else snprintf(combined, sizeof combined, "%s/%s", base, spec);

    int absolute = combined[0] == '/';
    char *stack[256];
    size_t depth = 0;
    for (char *seg = strtok(combined, "/"); seg; seg = strtok(NULL, "/")) {
        if (strcmp(seg, ".") == 0) continue;
        if (strcmp(seg, "..") == 0) { if (depth) depth--; continue; }
        if (depth < 256) stack[depth++] = seg;
    }
    size_t at = 0;
    if (absolute && out_len) out[at++] = '/';
    for (size_t i = 0; i < depth; i++) {
        if (i && at + 1 < out_len) out[at++] = '/';
        size_t n = strlen(stack[i]);
        if (at + n >= out_len) n = out_len - at - 1;
        memcpy(out + at, stack[i], n);
        at += n;
    }
    out[at < out_len ? at : out_len - 1] = '\0';
}

/* A relative root against the working directory, so cache keys and cycle
 * detection compare the same directory the same way however it was spelled. */
static void absolute_root(const char *root, char *out, size_t out_len) {
    if (root[0] == '/') { resolve_path("/", root, out, out_len); return; }
    char *cwd = fsx_cwd();
    resolve_path(cwd ? cwd : ".", root, out, out_len);
    free(cwd);
}

/*
 * `% import name from "spec"` — a whole code line and nothing else on it.
 * A line mixing an import with other code is NOT recognised, deliberately:
 * `import`/`from` are not legal as ordinary expression code, so mixing them
 * surfaces as a script error rather than a silent misparse.
 *
 * Returns 1 and fills the parts if `line` is one.
 */
static int import_line(const char *line, size_t len,
                       size_t *indent_len, char *name, size_t name_cap,
                       char *spec, size_t spec_cap) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    *indent_len = i;
    if (i >= len || line[i] != '%') return 0;
    i++;
    if (i < len && line[i] == '%') i++;                  /* `%%` is one too */
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (len - i < 6 || memcmp(line + i, "import", 6) != 0) return 0;
    i += 6;
    if (i >= len || (line[i] != ' ' && line[i] != '\t')) return 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;

    size_t nstart = i;
    if (i >= len) return 0;
    char c = line[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$')) return 0;
    while (i < len) {
        c = line[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '$')) break;
        i++;
    }
    size_t nlen = i - nstart;
    if (nlen == 0 || nlen >= name_cap) return 0;

    if (i >= len || (line[i] != ' ' && line[i] != '\t')) return 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (len - i < 4 || memcmp(line + i, "from", 4) != 0) return 0;
    i += 4;
    if (i >= len || (line[i] != ' ' && line[i] != '\t')) return 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;

    if (i >= len || (line[i] != '"' && line[i] != '\'')) return 0;
    char quote = line[i++];
    size_t sstart = i;
    while (i < len && line[i] != quote) {
        if (line[i] == '"' || line[i] == '\'') return 0;   /* [^"']+ */
        i++;
    }
    if (i >= len) return 0;
    size_t slen = i - sstart;
    if (slen == 0 || slen >= spec_cap) return 0;
    i++;

    /* Trailing `;` and space, and then the line must END. */
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i < len && line[i] == ';') i++;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i != len) return 0;

    memcpy(name, line + nstart, nlen); name[nlen] = '\0';
    memcpy(spec, line + sstart, slen); spec[slen] = '\0';
    return 1;
}

/*
 * Rewrite every import line in `text` to a plain object literal the VM can
 * run, recording the specs. One line in, one line out — a document's
 * positions still point where its author would look.
 */
static char *rewrite_imports(mdy_engine *e, const char *source_path,
                             const char *text, size_t len, size_t *out_len) {
    size_t cap = len + 1024, at_out = 0;
    char *out = malloc(cap);
    if (!out) return NULL;

    size_t at = 0;
    while (at <= len) {
        const char *nl = at < len ? memchr(text + at, '\n', len - at) : NULL;
        size_t line_len = nl ? (size_t)(nl - (text + at)) : len - at;
        const char *line = text + at;

        size_t indent = 0;
        char name[128], spec[1024];
        if (import_line(line, line_len, &indent, name, sizeof name, spec, sizeof spec)) {
            if (e->import_count == e->import_cap) {
                size_t want = e->import_cap ? e->import_cap * 2 : 8;
                Import *grown = realloc(e->imports, want * sizeof *grown);
                if (!grown) { free(out); return NULL; }
                e->imports = grown;
                e->import_cap = want;
            }
            Import *imp = &e->imports[e->import_count++];
            imp->source_path = strdup(source_path);
            imp->spec = strdup(spec);
            imp->set = NULL;

            char rewritten[4096];
            int n = snprintf(rewritten, sizeof rewritten,
                "%% const %s = { "
                "render: (target, ctx) => $.__importRender(\"%s\", target, ctx === undefined ? {} : ctx), "
                "find: (query) => $.__importFind(\"%s\", query === undefined ? {} : query), "
                "findOne: (query) => $.__importFindOne(\"%s\", query === undefined ? {} : query), "
                "resize: (record, options) => $.__importResize(\"%s\", record, options === undefined ? {} : options) };",
                name, spec, spec, spec, spec);

            size_t need = at_out + indent + (size_t)n + 2;
            if (need > cap) {
                while (need > cap) cap *= 2;
                char *grown = realloc(out, cap);
                if (!grown) { free(out); return NULL; }
                out = grown;
            }
            memcpy(out + at_out, line, indent);
            at_out += indent;
            memcpy(out + at_out, rewritten, (size_t)n);
            at_out += (size_t)n;
        } else {
            size_t need = at_out + line_len + 2;
            if (need > cap) {
                while (need > cap) cap *= 2;
                char *grown = realloc(out, cap);
                if (!grown) { free(out); return NULL; }
                out = grown;
            }
            memcpy(out + at_out, line, line_len);
            at_out += line_len;
        }
        if (!nl) break;
        out[at_out++] = '\n';
        at += line_len + 1;
    }
    out[at_out] = '\0';
    *out_len = at_out;
    return out;
}

/* ---- the cache, and the recursion ------------------------------------------- */

static mdy_engine *cache_get(ImportCache *c, const char *dir) {
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->dirs[i], dir) == 0) return c->sets[i];
    return NULL;
}

static void cache_put(ImportCache *c, const char *dir, mdy_engine *set) {
    if (c->count == c->cap) {
        size_t want = c->cap ? c->cap * 2 : 8;
        char **d = realloc(c->dirs, want * sizeof *d);
        mdy_engine **s = realloc(c->sets, want * sizeof *s);
        if (!d || !s) { free(d); free(s); return; }
        c->dirs = d; c->sets = s; c->cap = want;
    }
    c->dirs[c->count] = strdup(dir);
    c->sets[c->count] = set;
    c->count++;
}

/* The chain from the graph's root down to whoever is calling — a REAL cycle
 * has a directory reappear in its OWN ancestors. A diamond (two files
 * importing the same package) does not, and must dedupe through the cache
 * rather than error, which is why this is a chain and not a flat set. */
typedef struct Ancestors {
    const char *dir;
    const struct Ancestors *up;
} Ancestors;

static int in_ancestors(const Ancestors *a, const char *dir) {
    for (; a; a = a->up) if (strcmp(a->dir, dir) == 0) return 1;
    return 0;
}

static int open_dir_inner(mdy_engine *e, const char *root, ImportCache *cache,
                          const Ancestors *ancestors, char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    e->root = strdup(root);
    e->cache = cache;

    char *listing = fsx_list(root, ".", NULL);
    if (!listing) {
        if (error && error_len) snprintf(error, error_len, "cannot read %s", root);
        return -1;
    }

    /*
     * One source, built by hand: every file becomes a document, separated by
     * the `---` the splitter reads. Identity is written as front matter so it
     * reaches the record the same way a document's own data does.
     */
    size_t cap = 65536, len = 0;
    char *source = malloc(cap);
    if (!source) { free(listing); return -1; }
    source[0] = '\0';

    for (char *rel = listing, *next; rel && *rel; rel = next) {
        char *nl = strchr(rel, '\n');
        next = nl ? nl + 1 : NULL;
        if (nl) *nl = '\0';
        if (!*rel || !is_source(rel)) continue;

        const char *name = basename_of(rel);
        const char *ext = extension_of(name);

        double size = 0, mtime = 0;
        fsx_stat(root, rel, &size, &mtime);

        size_t body_len = 0;
        char *body = NULL;
        int is_mdy = ends_with_ci(rel, ".mdy");
        int is_md = ends_with_ci(rel, ".md");
        int is_yaml = ends_with_ci(rel, ".yaml") || ends_with_ci(rel, ".yml");

        uint8_t *bytes = NULL;
        if (is_mdy || is_md || is_yaml) bytes = fsx_read(root, rel, &body_len);

        /*
         * The record. `path` is written LAST of the identity fields for the
         * reason mdy-docs gives: a data file may declare its own `name` or
         * `size` and identity silently shadowing that would make the file's
         * own data unreachable — but `path` is structurally required to be
         * real, because everything resolves documents by it.
         */
        char head[2048];
        int head_len = snprintf(head, sizeof head, "---\n");
        /* Identity, kept OUT of the text — see `identity` on the engine. */
        char ident[4096];
        int ident_len = snprintf(ident, sizeof ident,
            "name: \"%s\"\next: \"%s\"\nsize: %.0f\nmtime: %.0f\npath: \"%s\"\n",
            name, ext, size, mtime, rel);
        (void)ident_len;

        size_t need = len + (size_t)head_len + body_len + 4096;
        if (need > cap) {
            while (need > cap) cap *= 2;
            char *grown = realloc(source, cap);
            if (!grown) { free(bytes); free(source); free(listing); return -1; }
            source = grown;
        }
        memcpy(source + len, head, (size_t)head_len);
        len += (size_t)head_len;

        /*
         * A front-matter block only for the kinds that HAVE no front matter of
         * their own and need one built. A .mdy file's text goes in untouched:
         * its own `+++` block must be the first thing the splitter sees, or it
         * is read as body text.
         */
        if (is_md || is_yaml) {
            len += (size_t)snprintf(source + len, cap - len, "+++\n");
            if (is_md && bytes) {
                /* Never compiled — a bare `---` or a literal `{{ }}` in prose
                 * must not be misread — so the text is DATA: findable in
                 * `body`, with the document itself a placeholder. */
                put_block_scalar(&source, &len, &cap, "body", (const char *)bytes, body_len);
                put_tags(&source, &len, &cap, (const char *)bytes, body_len);
            }
            if (is_yaml && bytes) {
                /* A data file's own fields — it IS the front matter. */
                size_t need2 = len + body_len + 64;
                if (need2 > cap) {
                    while (need2 > cap) cap *= 2;
                    char *grown = realloc(source, cap);
                    if (!grown) { free(bytes); free(source); free(listing); return -1; }
                    source = grown;
                }
                memcpy(source + len, bytes, body_len);
                len += body_len;
                if (body_len && source[len - 1] != '\n') source[len++] = '\n';
            }
            len += (size_t)snprintf(source + len, cap - len, "+++\n");
        }

        size_t doc_count = 1;
        if (is_mdy && bytes) {
            /* `% import` is rewritten before the compiler ever sees the text —
             * a real import statement is not legal inside a function body, and
             * every `%` line becomes one. */
            size_t rlen = 0;
            char *rewritten = rewrite_imports(e, rel, (const char *)bytes, body_len, &rlen);
            body = rewritten ? rewritten : (char *)bytes;
            size_t blen = rewritten ? rlen : body_len;

            /* How many documents this file is: only a .mdy can hold more than
             * one, and every one of them carries the same file identity. */
            mdy_documents *split = mdy_split_documents(body, blen);
            if (split) {
                doc_count = mdy_documents_count(split);
                mdy_documents_free(split);
            }
            if (doc_count == 0) doc_count = 1;

            size_t need2 = len + blen + 64;
            if (need2 > cap) {
                while (need2 > cap) cap *= 2;
                char *grown = realloc(source, cap);
                if (!grown) { if (rewritten) free(rewritten); free(bytes); free(source); free(listing); return -1; }
                source = grown;
            }
            memcpy(source + len, body, blen);
            len += blen;
            if (rewritten) free(rewritten);
        } else {
            memcpy(source + len, PLACEHOLDER_BODY, strlen(PLACEHOLDER_BODY));
            len += strlen(PLACEHOLDER_BODY);
        }
        if (len && source[len - 1] != '\n') source[len++] = '\n';
        source[len] = '\0';

        /* One identity per document this file became. */
        for (size_t k = 0; k < doc_count; k++) {
            char **grown = realloc(e->identity, (e->identity_count + 1) * sizeof *grown);
            if (!grown) break;
            e->identity = grown;
            e->identity[e->identity_count++] = strdup(ident);
        }
        free(bytes);
    }

    free(listing);
    int rc = mdy_engine_open(e, source, len, error, error_len);
    free(source);
    if (rc != 0) return rc;

    /*
     * Now the imports, each resolved relative to the FILE that declared it —
     * the same rule a real relative JS import follows, not relative to the
     * package root.
     */
    Ancestors here = { e->root, ancestors };
    for (size_t i = 0; i < e->import_count; i++) {
        Import *imp = &e->imports[i];

        char joined[4096];
        snprintf(joined, sizeof joined, "%s/%s", e->root, imp->source_path);
        char file_dir[4096];
        dirname_of(joined, file_dir, sizeof file_dir);
        char child_dir[4096];
        resolve_path(file_dir, imp->spec, child_dir, sizeof child_dir);

        if (in_ancestors(&here, child_dir)) {
            if (error && error_len)
                snprintf(error, error_len, "mdy: import cycle detected — %s -> %s",
                         e->root, child_dir);
            return -1;
        }

        mdy_engine *have = cache_get(cache, child_dir);
        if (have) { imp->set = have; continue; }

        mdy_engine *child = mdy_engine_new();
        if (!child) { if (error && error_len) snprintf(error, error_len, "out of memory"); return -1; }
        /* An `$.emit` from an imported package contributes to the SAME
         * outputs as the site that imported it. */
        child->on_emit = e->on_emit;
        child->on_emit_ud = e->on_emit_ud;
        child->on_publish = e->on_publish;
        child->on_publish_ud = e->on_publish_ud;
        child->tokens = token_table(e);
        /* In the cache before it is built, so a package that imports itself
         * through a diamond finds the one in progress rather than starting a
         * second build of it. */
        cache_put(cache, child_dir, child);
        if (open_dir_inner(child, child_dir, cache, &here, error, error_len) != 0) return -1;
        imp->set = child;
    }

    /* After its own imports: post-order. */
    if (cache->root_count == cache->root_cap) {
        size_t want = cache->root_cap ? cache->root_cap * 2 : 8;
        char **grown = realloc(cache->roots, want * sizeof *grown);
        if (grown) { cache->roots = grown; cache->root_cap = want; }
    }
    if (cache->root_count < cache->root_cap)
        cache->roots[cache->root_count++] = strdup(e->root);
    return 0;
}

size_t mdy_engine_root_count(mdy_engine *e) {
    return (e->cache && e->owns_cache) ? e->cache->root_count : (e->root ? 1 : 0);
}

const char *mdy_engine_root_at(mdy_engine *e, size_t i) {
    if (e->cache && e->owns_cache)
        return i < e->cache->root_count ? e->cache->roots[i] : NULL;
    return i == 0 ? e->root : NULL;
}

int mdy_engine_open_dir(mdy_engine *e, const char *root, char *error, size_t error_len) {
    char abs[4096];
    absolute_root(root, abs, sizeof abs);

    ImportCache *cache = calloc(1, sizeof *cache);
    if (!cache) { if (error && error_len) snprintf(error, error_len, "out of memory"); return -1; }
    e->owns_cache = 1;
    cache_put(cache, abs, e);
    return open_dir_inner(e, abs, cache, NULL, error, error_len);
}

static int index_of_id(mdy_engine *e, const char *hex);
static JsValue run_query(mdy_engine *e, JsValue query, int one);

/*
 * The document whose `path` is `entry` — where a directory starts. A query
 * rather than a scan, because the set already carries a unique index on
 * `path`, built for exactly this.
 */
int mdy_engine_entry(mdy_engine *e, const char *entry) {
    JsValue query = js_object_new(e->ctx);
    js_gc_protect(e->vm, &query);
    set_val(e, query, "path", str(e->vm, entry, strlen(entry)));
    JsValue hit = run_query(e, query, 1);
    js_gc_unprotect(e->vm, &query);
    if (!js_is_object(hit)) return -1;
    char *id = js_string_utf8(js_object_get(e->vm, hit, key(e->vm, "_id")));
    int at = id ? index_of_id(e, id) : -1;
    free(id);
    return at;
}


/* ---- the site's three small natives ------------------------------------------
 *
 * These are on `$` for the reason script-site.js gives: each is a primitive a
 * template cannot compute for itself, and none of them decides policy. The
 * script decides what to index and what goes in a feed; these only do the
 * arithmetic — which in `rfc822`'s case a template genuinely cannot, because
 * the VM forbids `new`.
 */

static const char *const STOPWORDS[] = {
    "a", "an", "and", "are", "as", "at", "be", "but", "by", "for", "if", "in",
    "into", "is", "it", "no", "not", "of", "on", "or", "such", "that", "the",
    "their", "then", "there", "these", "they", "this", "to", "was", "will",
    "with",
};

static int is_stopword(const char *w, size_t len) {
    for (size_t i = 0; i < sizeof STOPWORDS / sizeof *STOPWORDS; i++)
        if (strlen(STOPWORDS[i]) == len && memcmp(STOPWORDS[i], w, len) == 0) return 1;
    return 0;
}

/*
 * `$.tokenize` — the search widget's word list. Lowercased, split on anything
 * that is not [a-z0-9], words of length > 1, stopwords out, duplicates out,
 * in order of first appearance.
 *
 * The split is on the ASCII class exactly as the JavaScript regex is written,
 * so a byte >= 0x80 is a separator here just as it is there. That looks like a
 * bug against a Unicode corpus and is not one to fix HERE: the widget shipped
 * in `static/search.js` tokenizes a visitor's query with the same rule, and a
 * word list that disagrees with the query tokenizer is a search box that finds
 * nothing. The two move together or not at all.
 */
static bool tokenize_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                            int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    char *text = argc > 0 ? js_string_utf8(args[0]) : NULL;
    if (!text) { *result = js_array_new(ctx, 0); return true; }

    size_t len = strlen(text);
    char *word = malloc(len + 1);
    /*
     * The words are collected and deduplicated ON THIS SIDE, and the JS array
     * is built once at the end.
     *
     * Asking the array what it already holds means converting every entry back
     * out of UTF-16 for every word — quadratic, with an allocation per
     * comparison. On a real corpus that was half of the entire build.
     */
    char **words = NULL;
    size_t count = 0, cap = 0;
    /* Open-addressed index over `words`, power of two, kept under half full. */
    size_t *slots = NULL;
    size_t slot_cap = 0;

    if (!word) { free(text); *result = js_array_new(ctx, 0); return true; }

    size_t i = 0;
    while (i < len) {
        /*
         * The JavaScript lowercases the WHOLE string and only then splits on
         * `[^a-z0-9]`, so the case mapping runs first and can itself produce
         * an ASCII letter: `İ` folds to `i` followed by a combining dot, which
         * both keeps the letter and ENDS the word. Lowercasing only ASCII gets
         * both of those wrong, and the search index then disagrees with the
         * query the widget tokenizes.
         */
        size_t wlen = 0;
        while (i < len) {
            uint32_t cp = 0, lc[2];
            size_t w = mdy_utf8_decode(text + i, len - i, &cp);
            size_t n = mdy_lower_full(cp, lc);
            if (!((lc[0] >= 'a' && lc[0] <= 'z') || (lc[0] >= '0' && lc[0] <= '9'))) break;
            word[wlen++] = (char)lc[0];
            i += w;
            if (n > 1 && !((lc[1] >= 'a' && lc[1] <= 'z') || (lc[1] >= '0' && lc[1] <= '9')))
                break;
        }
        while (i < len) {                       /* the separator run */
            uint32_t cp = 0, lc[2];
            size_t w = mdy_utf8_decode(text + i, len - i, &cp);
            mdy_lower_full(cp, lc);
            if ((lc[0] >= 'a' && lc[0] <= 'z') || (lc[0] >= '0' && lc[0] <= '9')) break;
            i += w;
        }
        word[wlen] = '\0';
        if (wlen <= 1 || is_stopword(word, wlen)) continue;

        if (count * 2 + 2 > slot_cap) {         /* grow and rehash */
            size_t want = slot_cap ? slot_cap * 2 : 64;
            size_t *grown = malloc(want * sizeof *grown);
            if (!grown) break;
            for (size_t k = 0; k < want; k++) grown[k] = (size_t)-1;
            for (size_t k = 0; k < count; k++) {
                size_t h = 1469598103934665603u;
                for (const char *p = words[k]; *p; p++)
                    h = (h ^ (unsigned char)*p) * 1099511628211u;
                size_t at = h & (want - 1);
                while (grown[at] != (size_t)-1) at = (at + 1) & (want - 1);
                grown[at] = k;
            }
            free(slots);
            slots = grown;
            slot_cap = want;
        }

        size_t h = 1469598103934665603u;
        for (size_t k = 0; k < wlen; k++) h = (h ^ (unsigned char)word[k]) * 1099511628211u;
        size_t at = h & (slot_cap - 1);
        int seen = 0;
        while (slots[at] != (size_t)-1) {
            const char *have = words[slots[at]];
            if (strlen(have) == wlen && memcmp(have, word, wlen) == 0) { seen = 1; break; }
            at = (at + 1) & (slot_cap - 1);
        }
        if (seen) continue;

        if (count == cap) {
            size_t want = cap ? cap * 2 : 32;
            char **grown = realloc(words, want * sizeof *grown);
            if (!grown) break;
            words = grown;
            cap = want;
        }
        words[count] = malloc(wlen + 1);
        if (!words[count]) break;
        memcpy(words[count], word, wlen + 1);
        slots[at] = count;
        count++;
    }
    free(word);
    free(text);
    free(slots);

    /* In order of first appearance, which is what `new Set` preserves. */
    JsValue out = js_array_new(ctx, (uint32_t)count);
    js_gc_protect(e->vm, &out);
    for (size_t k = 0; k < count; k++) {
        push_item(e, out, str(e->vm, words[k], strlen(words[k])));
        free(words[k]);
    }
    js_gc_unprotect(e->vm, &out);
    free(words);
    *result = out;
    return true;
}

/*
 * `$.rfc822` — a canonical YYYY-MM-DD to the form an RSS `pubDate` needs.
 * Howard Hinnant's days_from_civil, which is exact for every proleptic
 * Gregorian date and needs no time.h: `timegm` is not portable and `mktime`
 * would read the machine's timezone, which would make a feed's contents
 * depend on where it was built.
 */
static bool rfc822_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                          int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    *result = js_undefined();
    char *s = argc > 0 ? js_string_utf8(args[0]) : NULL;
    if (!s) return true;

    int y = 0, m = 0, d = 0;
    if (sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) {
        free(s);
        return true;   /* `new Date('nonsense').toUTCString()` is "Invalid Date" */
    }
    free(s);

    long yy = y - (m <= 2);
    long era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned long yoe = (unsigned long)(yy - era * 400);
    unsigned long doy = (unsigned long)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + (long)doe - 719468;
    /* 1970-01-01 was a Thursday; C's % keeps the sign of the dividend. */
    int dow = (int)(((days % 7) + 11) % 7);

    static const char *const DAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char out[64];
    int n = snprintf(out, sizeof out, "%s, %02d %s %04d 00:00:00 GMT",
                     DAYS[dow], d, MONTHS[m - 1], y);
    *result = str(e->vm, out, (size_t)n);
    return true;
}

/* ---- rendering, and rendering from inside a render --------------------------- */

/* A render produces a TREE; HTML is what the outermost caller asks for at the
 * end. That is the whole reason `$.render` can return a token. */
/* `req` is what the caller is answering with — `$.render(target, data)`'s
 * second argument, and an empty object for a render nobody asked a question
 * of. MDY neither reads it nor cares what shape it is. */
/*
 * A render produces a tree, and — when `wrote` is given — the TEXT the
 * document's own code wrote, before any of it is read as MDY.
 *
 * Those are two different strings and `$.text` wants the second. mdy.js says
 * why in one line: "a feed rendered only for its text should not be read as
 * MDY on the way past". A JSON record written by a document is the case that
 * proves it — parse it and `\"` inside a caption comes back as `"`, and what
 * the caller gets is no longer JSON.
 */
static mdy_doc *render_tree_out(mdy_engine *e, size_t index, JsValue req,
                                char **wrote, char *error, size_t error_len);
static mdy_doc *render_tree(mdy_engine *e, size_t index, JsValue req,
                            char *error, size_t error_len);

/*
 * `$.render(target, data)` — the target resolved, rendered, parked, and a
 * token handed back.
 *
 * A target is an index, or a query, or a document a `$.find` already returned
 * (which carries its own `_id`). All three end at a document index, which is
 * what the `_id` to index map is for.
 */
static int resolve_target(mdy_engine *e, JsValue target) {
    if (js_is_number(target)) {
        double at = js_get_number(target);
        return (at >= 0 && at < (double)e->count) ? (int)at : -1;
    }
    if (!js_is_object(target)) return -1;

    /* A document from `$.find` carries the id it was inserted with. */
    char *id = js_string_utf8(js_object_get(e->vm, target, key(e->vm, "_id")));
    if (id) {
        int at = index_of_id(e, id);
        free(id);
        if (at >= 0) return at;
    }

    JsValue hit = run_query(e, target, 1);
    if (!js_is_object(hit)) return -1;
    char *hit_id = js_string_utf8(js_object_get(e->vm, hit, key(e->vm, "_id")));
    if (!hit_id) return -1;
    int at = index_of_id(e, hit_id);
    free(hit_id);
    return at;
}

static bool render_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                          int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    int at = argc > 0 ? resolve_target(e, args[0]) : -1;
    if (at < 0) {
        const char *msg = "mdy-engine: $.render found no such document";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }

    char err[256];
    mdy_doc *doc = render_tree(e, (size_t)at,
                               argc > 1 ? args[1] : js_undefined(), err, sizeof err);
    if (!doc) {
        /*
         * A nested render's failure is passed through rather than wrapped
         * again. mdy-docs wraps at every level, so a cycle reports
         * "document 0 failed: document 0 failed: …" thirty times over and the
         * reason falls off the end. The first message is the one that says
         * something.
         */
        char msg[320];
        if (strncmp(err, "mdy-engine:", 11) == 0)
            snprintf(msg, sizeof msg, "%s", err);
        else
            snprintf(msg, sizeof msg, "mdy-engine: document %d failed: %s", at, err);
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }

    char *token = hold_tree(e, doc, mdy_root(doc));
    if (!token) { *result = js_undefined(); return false; }
    *result = str(e->vm, token, strlen(token));
    free(token);
    return true;
}

/* `$.text(target)` — the same render, as the text it holds rather than a
 * token. What a document's own code wrote, with no markup around it. */
static void collect_text_into(const mdy_node *n, char **out, size_t *len, size_t *cap) {
    if (n->type == MDY_TEXT && n->text) {
        size_t add = strlen(n->text);
        if (*len + add + 1 > *cap) {
            while (*len + add + 1 > *cap) *cap = *cap ? *cap * 2 : 256;
            char *grown = realloc(*out, *cap);
            if (!grown) return;
            *out = grown;
        }
        memcpy(*out + *len, n->text, add);
        *len += add;
        (*out)[*len] = '\0';
    }
    if (n->type == MDY_COMMENT || n->type == MDY_DOCTYPE) return;
    for (const mdy_node *c = n->first; c; c = c->next) collect_text_into(c, out, len, cap);
}

static bool text_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                        int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    int at = argc > 0 ? resolve_target(e, args[0]) : -1;
    if (at < 0) {
        const char *msg = "mdy-engine: $.text found no such document";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    char err[256];
    char *text = NULL;
    mdy_doc *doc = render_tree_out(e, (size_t)at,
                                   argc > 1 ? args[1] : js_undefined(), &text,
                                   err, sizeof err);
    if (!doc) {
        char msg[320];
        if (strncmp(err, "mdy-engine:", 11) == 0) snprintf(msg, sizeof msg, "%s", err);
        else snprintf(msg, sizeof msg, "mdy-engine: document %d failed: %s", at, err);
        *result = str(e->vm, msg, strlen(msg));
        free(text);
        return false;
    }
    size_t len = text ? strlen(text) : 0;
    *result = str(e->vm, text ? text : "", len);
    free(text);
    /* The tree is held so it outlives this call, like any other render's. */
    char *token = hold_tree(e, doc, mdy_root(doc));
    free(token);
    return true;
}

/*
 * `$.emit(path, content)` — a named output. Tokens in the content become the
 * HTML they hold, because a file is a string and that is the shape it can
 * hold.
 */
static bool emit_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                        int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    if (argc < 2) { *result = js_null(); return true; }
    char *path = js_string_utf8(args[0]);
    char *content = js_string_utf8(args[1]);
    if (path && content) {
        char *filled = fill_tokens(e, content, strlen(content));
        if (filled && e->on_emit) e->on_emit(e->on_emit_ud, path, filled);
        free(filled);
    }
    free(path);
    free(content);
    *result = js_null();
    return true;
}

/* ---- opening a set ---------------------------------------------------------- */

/* nisaba keys its primary tree on OID bytes and needs a file behind it; a set
 * built from a source is in memory, so this is a temporary the OS reclaims. */
extern int nis_open(void);
extern int nis_insert(int handle, const uint8_t *doc, uint32_t len);
extern int nis_find(int handle, const uint8_t *filter, uint32_t filter_len,
                    uint8_t **out, size_t *out_len);
extern int nis_create_index(int handle, const char *name, const uint8_t *fields,
                            uint32_t fields_len, int unique, int sparse);

static void close_set(mdy_engine *e) {
    for (size_t i = 0; i < e->count; i++) mdy_data_free(e->docs[i].fences);
    free(e->docs);
    free(e->ids);
    mdy_documents_free(e->source_docs);
    e->docs = NULL;
    e->ids = NULL;
    e->source_docs = NULL;
    e->count = 0;
}

int mdy_engine_open(mdy_engine *e, const char *source, size_t len,
                    char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    close_set(e);

    e->source_docs = mdy_split_documents(source, len);
    if (!e->source_docs) return -1;

    size_t n = mdy_documents_count(e->source_docs);
    e->docs = calloc(n ? n : 1, sizeof *e->docs);
    e->ids = calloc(n ? n : 1, sizeof *e->ids);
    if (!e->docs || !e->ids) { close_set(e); return -1; }
    e->count = n;

    if (e->handle < 0) {
        e->handle = nis_open();
        if (e->handle < 0) {
            if (error && error_len) snprintf(error, error_len, "could not open a collection");
            close_set(e);
            return -1;
        }
    }

    /*
     * `path` is the natural key of a set built from a directory, and every
     * `$.render({ path: … })` resolves through a query on it — so without the
     * index each one is a scan of the whole set. SPARSE because a document
     * need not have a path at all, and NOT unique because one file can hold
     * several documents and they all carry its path.
     *
     * It is also what makes the document-order sort do real work: a query
     * answered from an index comes back in INDEX order, not `_id` order.
     */
    {
        bj_builder *spec = bj_builder_new();
        if (spec) {
            bj_begin_object(spec);
            bj_put_key(spec, (const uint8_t *)"path", 4);
            bj_put_int(spec, 1);
            bj_end_object(spec);
            size_t slen = 0;
            const uint8_t *bytes = bj_builder_data(spec, &slen);
            nis_create_index(e->handle, "path", bytes, (uint32_t)slen, 0, 1);
            bj_builder_free(spec);
        }
    }

    for (size_t i = 0; i < n; i++) {
        Document *d = &e->docs[i];
        d->chunk = mdy_documents_at(e->source_docs, i);
        mdy_chunk body;
        mdy_split_frontmatter(d->chunk.text, d->chunk.len, &d->matter, &body);
        d->fences = mdy_data_extract(body.text, body.len);

        /*
         * The document's DATA: its front matter, with each ```data fence
         * merged over it — `Object.assign({}, frontMatter, ...blocks)`. Its
         * text never goes in, which is what a measured build of mdy-docs
         * shows it doing.
         */
        char err[256];
        mdy_yaml *matter = NULL;
        if (d->matter.len) matter = mdy_yaml_parse(d->matter.text, d->matter.len, err, sizeof err);

        size_t fence_count = d->fences ? mdy_data_count(d->fences) : 0;
        /* front matter + every data fence + file identity */
        const mdy_yaml_node **maps = calloc(fence_count + 2, sizeof *maps);
        mdy_yaml **parsed = calloc(fence_count + 1, sizeof *parsed);
        if (!maps || !parsed) { free(maps); free(parsed); mdy_yaml_free(matter); close_set(e); return -1; }

        size_t used = 0;
        if (matter) maps[used++] = mdy_yaml_root(matter);
        for (size_t f = 0; f < fence_count; f++) {
            const mdy_data_fence *fence = mdy_data_at(d->fences, f);
            mdy_yaml *y = mdy_yaml_parse(fence->source, fence->source_len, err, sizeof err);
            if (!y) continue;
            parsed[f] = y;
            maps[used++] = mdy_yaml_root(y);
        }

        /*
         * File identity goes in LAST, so it wins: a document's `name` is its
         * file's, never a field of the same name in its front matter. `path`
         * especially — everything resolves documents by it.
         */
        mdy_yaml *ident = NULL;
        if (e->identity && i < e->identity_count && e->identity[i]) {
            ident = mdy_yaml_parse(e->identity[i], strlen(e->identity[i]), err, sizeof err);
            if (ident) maps[used++] = mdy_yaml_root(ident);
        }

        mdy_oid_next(d->oid);
        memcpy(e->ids[i], d->oid, 12);

        bj_builder *b = bj_builder_new();
        int ok = b && mdy_bj_document(b, d->oid, maps, used) == 0 && !bj_builder_error(b);
        if (ok) {
            size_t dlen = 0;
            const uint8_t *bytes = bj_builder_data(b, &dlen);
            ok = bytes && nis_insert(e->handle, bytes, (uint32_t)dlen) == 0;
        }
        bj_builder_free(b);
        mdy_yaml_free(matter);
        mdy_yaml_free(ident);
        for (size_t f = 0; f < fence_count; f++) mdy_yaml_free(parsed[f]);
        free(maps);
        free(parsed);

        if (!ok) {
            if (error && error_len)
                snprintf(error, error_len, "document %zu could not be inserted", i);
            close_set(e);
            return -1;
        }
    }

    return 0;
}

size_t mdy_engine_count(mdy_engine *e) { return e ? e->count : 0; }

void mdy_engine_on_emit(mdy_engine *e,
                        void (*fn)(void *ud, const char *path, const char *content),
                        void *ud) {
    if (!e) return;
    e->on_emit = fn;
    e->on_emit_ud = ud;
}

void mdy_engine_on_publish(mdy_engine *e,
                           void (*fn)(void *ud, const char *name,
                                      const char *data_json, size_t doc_index),
                           void *ud) {
    e->on_publish = fn;
    e->on_publish_ud = ud;
}

/* ---- querying ---------------------------------------------------------------
 *
 * A hit carries the `_id` it was inserted with, so it maps back to the
 * document it came from — and the answer is sorted by that, not by whatever
 * order the database walked its keys in. That is what makes a query's answer
 * the same on every build.
 */
static int index_of_id(mdy_engine *e, const char *hex) {
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < e->count; i++) {
        char have[25];
        for (int k = 0; k < 12; k++) {
            have[k * 2] = H[e->ids[i][k] >> 4];
            have[k * 2 + 1] = H[e->ids[i][k] & 15];
        }
        have[24] = '\0';
        if (strcmp(have, hex) == 0) return (int)i;
    }
    return -1;
}

/*
 * `vals` is the VM the result must be readable in; `store` is the set being
 * queried. They differ for a cross-package `find`: each package has its own
 * VM, so a value made in one is meaningless in the other and the documents
 * have to be rebuilt on the caller's side.
 */
static JsValue run_query_in(mdy_engine *vals, mdy_engine *store, JsValue query, int one) {
    mdy_engine *e = vals;
    bj_builder *b = bj_builder_new();
    if (!b) return js_undefined();
    if (js_is_object(query) && !js_is_array(query)) {
        if (js_to_binjson(e, b, query) != 0) { bj_builder_free(b); return js_undefined(); }
    } else {
        bj_begin_object(b);
        bj_end_object(b);
    }
    size_t flen = 0;
    const uint8_t *filter = bj_builder_data(b, &flen);

    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = nis_find(store->handle, filter, (uint32_t)flen, &out, &out_len);
    bj_builder_free(b);
    if (rc != 0 || !out) return one ? js_null() : js_array_new(e->ctx, 0);

    /* The result is a binjson ARRAY of documents. */
    JsValue hits = binjson_to_js(e, out, out_len, NULL);
    free(out);
    if (!js_is_array(hits)) return one ? js_null() : js_array_new(e->ctx, 0);
    js_gc_protect(e->vm, &hits);

    /* Back into document order. */
    uint32_t n = js_array_length(hits);
    JsValue ordered = js_array_new(e->ctx, n);
    js_gc_protect(e->vm, &ordered);
    for (size_t want = 0; want < store->count; want++) {
        for (uint32_t i = 0; i < n; i++) {
            JsValue hit = js_array_get(hits, i);
            char *id = js_string_utf8(js_object_get(e->vm, hit, key(e->vm, "_id")));
            if (!id) continue;
            int at = index_of_id(store, id);
            free(id);
            if (at == (int)want) push_item(e, ordered, hit);
        }
    }
    js_gc_unprotect(e->vm, &ordered);
    js_gc_unprotect(e->vm, &hits);

    if (!one) return ordered;
    return js_array_length(ordered) > 0 ? js_array_get(ordered, 0) : js_null();
}

/* The ordinary case: one set, queried in its own VM. */
static JsValue run_query(mdy_engine *e, JsValue query, int one) {
    return run_query_in(e, e, query, one);
}

static bool find_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                        int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    *result = run_query(e, argc > 0 ? args[0] : js_undefined(), 0);
    return true;
}

static bool find_one_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                            int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    *result = run_query(e, argc > 0 ? args[0] : js_undefined(), 1);
    return true;
}

/* `$.data(i)` — a document's own data, by index, without a query. */
/* One document's record, as the guest sees it. */
static JsValue document_record(mdy_engine *e, size_t at) {
    if (at >= e->count) return js_object_new(e->ctx);
    bj_builder *b = bj_builder_new();
    if (!b) return js_object_new(e->ctx);
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"_id", 3);
    bj_put_oid(b, e->ids[at]);
    bj_end_object(b);
    size_t flen = 0;
    const uint8_t *filter = bj_builder_data(b, &flen);
    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = nis_find(e->handle, filter, (uint32_t)flen, &out, &out_len);
    bj_builder_free(b);
    if (rc != 0 || !out) return js_object_new(e->ctx);
    JsValue hits = binjson_to_js(e, out, out_len, NULL);
    free(out);
    return js_is_array(hits) && js_array_length(hits) > 0
               ? js_array_get(hits, 0) : js_object_new(e->ctx);
}

static bool data_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                        int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    if (argc < 1 || !js_is_number(args[0])) { *result = js_null(); return true; }
    double at = js_get_number(args[0]);
    if (at < 0 || at >= (double)e->count) { *result = js_null(); return true; }

    bj_builder *b = bj_builder_new();
    if (!b) { *result = js_null(); return true; }
    bj_begin_object(b);
    bj_put_key(b, (const uint8_t *)"_id", 3);
    bj_put_oid(b, e->ids[(size_t)at]);
    bj_end_object(b);
    size_t flen = 0;
    const uint8_t *filter = bj_builder_data(b, &flen);
    uint8_t *out = NULL;
    size_t out_len = 0;
    int rc = nis_find(e->handle, filter, (uint32_t)flen, &out, &out_len);
    bj_builder_free(b);
    if (rc != 0 || !out) { *result = js_null(); return true; }
    JsValue hits = binjson_to_js(e, out, out_len, NULL);
    free(out);
    *result = js_is_array(hits) && js_array_length(hits) > 0 ? js_array_get(hits, 0) : js_null();
    return true;
}

/* ---- `$`, with every native refusing ---------------------------------------
 *
 * A native that is not implemented THROWS, naming itself. The alternative — a
 * stub returning undefined — produces a page that is quietly missing whatever
 * the document asked for, which is the failure this whole project has been
 * most careful to avoid.
 */
static bool refuse(JsContext *ctx, JsValue this_val, const JsValue *args, int argc,
                   JsValue *result) {
    (void)this_val;
    JsVm *vm = js_context_vm(ctx);
    char msg[160];
    char name[64] = "(unnamed)";
    if (argc > 0 && js_is_string(args[0])) {
        size_t ulen = 0;
        const uint16_t *u = js_string_units(args[0], &ulen);
        size_t n = 0;
        for (size_t i = 0; i < ulen && n < sizeof name - 1; i++)
            if (u[i] < 0x80) name[n++] = (char)u[i];
        name[n] = '\0';
    }
    snprintf(msg, sizeof msg,
             "mdy-engine: $.%s is not implemented in the C engine yet", name);
    *result = str(vm, msg, strlen(msg));
    return false;
}

/*
 * `$.compose(__out)` — the one native step 2 implements, and the reason the
 * tree conversions exist.
 *
 * A document with a `transform` needs its own finished tree, so the host takes
 * the lines it produced, parses them, and hands the tree BACK to the guest as
 * values. The guest transforms it and returns it; the host converts it back
 * and writes the HTML. mdy-docs does the same thing with JSON in both
 * directions.
 */
static mdy_doc *parse_lines(JsValue out, mdy_engine *e);

static bool compose_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                           int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    if (argc < 1 || !js_is_array(args[0])) {
        const char *msg = "mdy-engine: $.compose wants the lines a document produced";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    mdy_doc *tree = parse_lines(args[0], e);
    if (!tree) {
        const char *msg = "mdy-engine: the produced lines did not parse";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    /* Composed before the transforms see it: a transform works on the
     * document's FINISHED tree, renders and all. */
    splice_tree(e, tree, mdy_root(tree));
    /* The document owns the tree until the render finishes with it. */
    mdy_free(e->tree_owner);
    e->tree_owner = tree;
    *result = tree_to_js(e, mdy_root(tree));
    return true;
}

static void register_one(mdy_engine *e, const char *name, JsNativeFn fn) {
    size_t n = 0;
    uint16_t *u = to_utf16(name, strlen(name), &n);
    if (!u) return;
    js_register_native(e->ctx, u, n, fn, NULL);
    free(u);
}


/* ---- reaching into an imported package --------------------------------------
 *
 * The spec a document wrote, resolved against THAT document — imports are
 * recorded per source file, so the same spec in two files can be two packages.
 */
static mdy_engine *lookup_import(mdy_engine *e, const char *spec, const char **why) {
    static char path[1024];
    path[0] = '\0';
    if (e->current < e->count) {
        /* The record for THIS document, by its own id. */
        bj_builder *b = bj_builder_new();
        if (b) {
            bj_begin_object(b);
            bj_put_key(b, (const uint8_t *)"_id", 3);
            bj_put_oid(b, e->ids[e->current]);
            bj_end_object(b);
            size_t flen = 0;
            const uint8_t *filter = bj_builder_data(b, &flen);
            uint8_t *out = NULL;
            size_t out_len = 0;
            if (nis_find(e->handle, filter, (uint32_t)flen, &out, &out_len) == 0 && out) {
                JsValue hits = binjson_to_js(e, out, out_len, NULL);
                free(out);
                if (js_is_array(hits) && js_array_length(hits) > 0) {
                    char *p = js_string_utf8(js_object_get(e->vm, js_array_get(hits, 0),
                                                           key(e->vm, "path")));
                    if (p) { snprintf(path, sizeof path, "%s", p); free(p); }
                }
            }
            bj_builder_free(b);
        }
    }
    if (!path[0]) { *why = "a document with no path"; return NULL; }
    for (size_t i = 0; i < e->import_count; i++) {
        if (strcmp(e->imports[i].spec, spec) == 0 &&
            strcmp(e->imports[i].source_path, path) == 0)
            return e->imports[i].set;
    }
    *why = path;
    return NULL;
}

/*
 * A value from one package's VM, rebuilt in another's.
 *
 * Each package is its own VM, and a JsValue is only meaningful inside the one
 * that made it — handing an importer's object straight to an imported
 * document gives it something that is not an object there at all. So data
 * crosses as DATA, through the same encoding the document store uses, exactly
 * as mdy-docs' separate VMs make it cross as JSON.
 */
static JsValue cross_vm(mdy_engine *from, mdy_engine *to, JsValue v) {
    if (!js_is_object(v)) return js_object_new(to->ctx);
    bj_builder *b = bj_builder_new();
    if (!b) return js_object_new(to->ctx);
    if (js_to_binjson(from, b, v) != 0 || bj_builder_error(b)) {
        bj_builder_free(b);
        return js_object_new(to->ctx);
    }
    size_t len = 0;
    const uint8_t *bytes = bj_builder_data(b, &len);
    JsValue out = bytes ? binjson_to_js(to, bytes, len, NULL) : js_object_new(to->ctx);
    bj_builder_free(b);
    return out;
}

static bool import_render_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                                 int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    *result = js_undefined();
    char *spec = argc > 0 ? js_string_utf8(args[0]) : NULL;
    if (!spec) return true;

    const char *why = "";
    mdy_engine *set = lookup_import(e, spec, &why);
    if (!set) {
        char msg[512];
        snprintf(msg, sizeof msg, "mdy: import \"%s\" was not resolved (declared in %s)", spec, why);
        *result = str(e->vm, msg, strlen(msg));
        free(spec);
        return false;
    }
    free(spec);

    int at = resolve_target(set, argc > 1 ? args[1] : js_undefined());
    if (at < 0) {
        const char *msg = "$.render: no such document in the imported package";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }

    char err[512];
    /*
     * A cross-package render is a TREE, exactly as an in-set one is: the
     * imported document is parsed at its own boundary and comes back as a
     * node, so an imported layout cannot leak an unclosed tag into the page
     * that used it.
     */
    JsValue req = cross_vm(e, set, argc > 2 ? args[2] : js_undefined());
    js_gc_protect(set->vm, &req);
    mdy_doc *tree = render_tree(set, (size_t)at, req, err, sizeof err);
    js_gc_unprotect(set->vm, &req);
    if (!tree) {
        const char *msg = err[0] ? err : "the imported document failed to render";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }

    /* Parked by the IMPORTER, whose composition pass will splice it and whose
     * render owns it from here. */
    char *token = hold_tree(e, tree, (mdy_node *)mdy_root(tree));
    if (!token) { mdy_free(tree); return true; }
    *result = str(e->vm, token, strlen(token));
    free(token);
    return true;
}

static bool import_query_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                                int argc, JsValue *result, int one) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    *result = one ? js_null() : js_array_new(ctx, 0);
    char *spec = argc > 0 ? js_string_utf8(args[0]) : NULL;
    if (!spec) return true;
    const char *why = "";
    mdy_engine *set = lookup_import(e, spec, &why);
    free(spec);
    if (!set) {
        const char *msg = "mdy: import was not resolved";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }

    /*
     * The query runs in the IMPORTED set — its own nisaba handle, its own
     * documents — but the values must come back as the IMPORTER's, because
     * that is the VM the caller will read them in.
     */
    JsValue hits = run_query_in(e, set, argc > 1 ? args[1] : js_undefined(), one);
    *result = hits;
    return true;
}

static bool import_find_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                               int argc, JsValue *result) {
    return import_query_native(ctx, this_val, args, argc, result, 0);
}

static bool import_find_one_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                                   int argc, JsValue *result) {
    return import_query_native(ctx, this_val, args, argc, result, 1);
}


/* ---- the trees a document builds for itself ---------------------------------
 *
 * `$.parse`, `$.markdown`, `$.node`, `$.table` and `$.html`: the ways a
 * document gets a tree that did not come from its own text, and the way one
 * goes back out as HTML.
 *
 * All but `$.html` end in a token, for the reason `$.render` does — a tree
 * travels through a document's own code as a few private-use characters, and
 * goes back in where the parser knows what is open. `$.parse` is the
 * exception on the other side: it hands back the TREE, because its whole
 * purpose is to be looked at.
 */

/* The options the document engine asks the parser for: front matter, document
 * splitting and the script layer are all already done by the time a document
 * calls one of these. */
static void parse_options(mdy_options *options) {
    mdy_options_default(options);
    options->frontmatter = 0;
    options->documents = 0;
    options->sanitize = 0;
}

/* MDY text as a tree, with any tokens in it spliced — `$.parse` is handed to
 * code that will read the tree, so a `$.render` inside the text has to have
 * become its nodes by then. */
static bool parse_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                         int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    char *text = argc > 0 ? js_string_utf8(args[0]) : NULL;

    mdy_options options;
    parse_options(&options);
    mdy_doc *doc = mdy_parse(text ? text : "", text ? strlen(text) : 0, &options);
    free(text);
    if (!doc) {
        const char *msg = "mdy-engine: $.parse could not read that as MDY";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    splice_tree(e, doc, (mdy_node *)mdy_root(doc));
    *result = tree_to_js(e, mdy_root(doc));
    /* The document owns it until the render finishes: the value handed back is
     * a copy in the VM, but a token spliced into it points at held nodes. */
    hold_tree(e, doc, (mdy_node *)mdy_root(doc));
    return true;
}

/* Markdown as a tree — the OTHER front end, for a `.md` file's body or any
 * markdown a document holds and wants as nodes. */
static bool markdown_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                            int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    char *text = argc > 0 ? js_string_utf8(args[0]) : NULL;
    mdy_doc *doc = mdy_markdown_parse(text ? text : "", text ? strlen(text) : 0);
    free(text);
    if (!doc) {
        const char *msg = "mdy-engine: $.markdown could not read that as Markdown";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    char *token = hold_tree(e, doc, (mdy_node *)mdy_root(doc));
    if (!token) { *result = js_undefined(); return true; }
    *result = str(e->vm, token, strlen(token));
    free(token);
    return true;
}

/*
 * A tree the document built ITSELF — with `h`, by hand, or in a module it
 * imported — parked like any other and spliced where its token lands.
 *
 * This is what a helper that used to return a string of HTML returns instead:
 * hast is plain data, so it crosses as it is, and a fragment built this way is
 * a node from the start rather than text somebody has to parse back.
 */
static bool node_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                        int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    if (argc < 1 || !js_is_object(args[0]) ||
        !js_is_string(js_object_get(e->vm, args[0], key(e->vm, "type")))) {
        const char *msg = "mdy: $.node expects a hast node ({ type, … })";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    mdy_doc *doc = mdy_doc_new();
    if (!doc) { *result = js_undefined(); return true; }
    mdy_node *root = js_to_tree(e, doc, args[0]);
    /*
     * `mdy_doc` owns its root, so what came back is hung under it — a root
     * lends its children, and a single element becomes the document's one
     * child. The same thing `$.compose` does with a transform's return.
     */
    mdy_node *into = (mdy_node *)mdy_root(doc);
    if (root && root->type == MDY_ROOT) {
        for (mdy_node *c = root->first; c;) {
            mdy_node *next = c->next;
            c->next = NULL;
            mdy_append(into, c);
            c = next;
        }
    } else if (root) {
        mdy_append(into, root);
    }
    char *token = hold_tree(e, doc, into);
    if (!token) { *result = js_undefined(); return true; }
    *result = str(e->vm, token, strlen(token));
    free(token);
    return true;
}

/* A tree, or a token standing for one, as HTML text. */
static bool html_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                        int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);

    if (argc > 0 && js_is_string(args[0])) {
        /* Tokens in a string become the HTML of what they hold. */
        char *s = js_string_utf8(args[0]);
        char *filled = s ? fill_tokens(e, s, strlen(s)) : NULL;
        free(s);
        *result = str(e->vm, filled ? filled : "", filled ? strlen(filled) : 0);
        free(filled);
        return true;
    }
    if (argc < 1 || !js_is_object(args[0]) ||
        !js_is_string(js_object_get(e->vm, args[0], key(e->vm, "type")))) {
        const char *msg = "mdy: $.html expects a hast node ({ type, … }) or a string";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    mdy_doc *doc = mdy_doc_new();
    if (!doc) { *result = js_undefined(); return true; }
    mdy_node *root = js_to_tree(e, doc, args[0]);
    char *html = root ? mdy_to_html(root, NULL) : NULL;
    mdy_free(doc);
    *result = str(e->vm, html ? html : "", html ? strlen(html) : 0);
    free(html);
    return true;
}

/*
 * `$.table(rows, align)` — an array of row arrays, the first being the header.
 *
 * A cell's text is parsed as MDY, so a link or an emphasis in a cell is a
 * link or an emphasis. A cell that parses to exactly one paragraph gives up
 * that paragraph and contributes its children; anything else is kept as the
 * text it was, which is what stops a cell holding a list from breaking the
 * row apart.
 */
static const char *column_align(mdy_engine *e, JsValue align, uint32_t i) {
    if (!js_is_array(align) || i >= js_array_length(align)) return NULL;
    char *s = js_string_utf8(js_array_get(align, i));
    if (!s) return NULL;
    char first = s[0];
    free(s);
    if (first >= 'A' && first <= 'Z') first = (char)(first - 'A' + 'a');
    if (first == 'l') return "left";
    if (first == 'c') return "center";
    if (first == 'r') return "right";
    return NULL;
}

static void table_cell(mdy_engine *e, mdy_doc *doc, mdy_node *row,
                       JsValue value, uint32_t i, int header, JsValue align) {
    char *text = js_is_undefined(value) || js_is_null(value) ? NULL : js_string_utf8(value);
    const char *body = text ? text : "";

    mdy_node *cell = mdy_new_element(doc, header ? "th" : "td", 2);
    const char *at = column_align(e, align, i);
    if (at) {
        char style[32];
        int n = snprintf(style, sizeof style, "text-align: %s", at);
        mdy_set_string(doc, cell, "style", style, (size_t)n);
    }

    mdy_options options;
    parse_options(&options);
    mdy_doc *parsed = mdy_parse(body, strlen(body), &options);
    const mdy_node *root = parsed ? mdy_root(parsed) : NULL;
    const mdy_node *only = root ? root->first : NULL;
    int one_paragraph = only && !only->next && only->type == MDY_ELEMENT &&
                        strcmp(only->tag, "p") == 0;
    if (one_paragraph) {
        for (const mdy_node *c = only->first; c; c = c->next) {
            mdy_node *copy = mdy_clone(doc, c);
            if (copy) mdy_append(cell, copy);
        }
    } else {
        mdy_append(cell, mdy_new_text(doc, body, strlen(body)));
    }
    mdy_free(parsed);
    free(text);
    mdy_append(row, cell);
}

static bool table_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                         int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    JsValue rows = argc > 0 ? args[0] : js_undefined();
    JsValue align = argc > 1 ? args[1] : js_undefined();

    int shaped = js_is_array(rows);
    for (uint32_t i = 0; shaped && i < js_array_length(rows); i++)
        if (!js_is_array(js_array_get(rows, i))) shaped = 0;
    if (!shaped) {
        const char *msg = "mdy: $.table expects an array of row arrays (first row is the header)";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    if (!js_is_undefined(align) && !js_is_null(align) && !js_is_array(align)) {
        const char *msg = "mdy: $.table align must be an array like ['left', 'center', 'right']";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    uint32_t count = js_array_length(rows);
    if (count == 0) { *result = str(e->vm, "", 0); return true; }

    mdy_doc *doc = mdy_doc_new();
    if (!doc) { *result = js_undefined(); return true; }
    mdy_node *table = mdy_new_element(doc, "table", 5);
    mdy_node *thead = mdy_new_element(doc, "thead", 5);
    mdy_node *head_row = mdy_new_element(doc, "tr", 2);
    JsValue head = js_array_get(rows, 0);
    for (uint32_t i = 0; i < js_array_length(head); i++)
        table_cell(e, doc, head_row, js_array_get(head, i), i, 1, align);
    mdy_append(thead, head_row);
    mdy_append(table, thead);

    if (count > 1) {
        mdy_node *tbody = mdy_new_element(doc, "tbody", 5);
        for (uint32_t r = 1; r < count; r++) {
            mdy_node *tr = mdy_new_element(doc, "tr", 2);
            JsValue cells = js_array_get(rows, r);
            for (uint32_t i = 0; i < js_array_length(cells); i++)
                table_cell(e, doc, tr, js_array_get(cells, i), i, 0, align);
            mdy_append(tbody, tr);
        }
        mdy_append(table, tbody);
    }
    mdy_append((mdy_node *)mdy_root(doc), table);

    char *token = hold_tree(e, doc, table);
    if (!token) { *result = js_undefined(); return true; }
    *result = str(e->vm, token, strlen(token));
    free(token);
    return true;
}


/* ---- a document's own contents list ------------------------------------------
 *
 * `$.toc()` returns a token before there is anything to put in it. That is the
 * point: a document's headings are not known until its whole tree is, and a
 * contents list at the top has to be able to name a heading a loop writes
 * below it. So the token is parked empty and filled LAST, after the transforms
 * have had the tree — which is also why the ids in it are the parser's own,
 * and nothing has to agree with anything.
 */

static char *hold_toc(mdy_engine *e) {
    char *token = hold_tree(e, NULL, NULL);
    if (token) {
        mdy_engine *t = token_table(e);
        t->held[t->held_count - 1].is_toc = 1;
    }
    return token;
}

typedef struct { int depth; char *text; char *id; } Heading;

/* All the text under a node, which is what a heading's entry reads. */
static void heading_text(const mdy_node *n, char **buf, size_t *len, size_t *cap) {
    collect_text_into(n, buf, len, cap);
}

static int heading_depth(const mdy_node *n) {
    if (n->type != MDY_ELEMENT || !n->tag) return 0;
    if (n->tag[0] != 'h' || n->tag[1] < '1' || n->tag[1] > '6' || n->tag[2]) return 0;
    return n->tag[1] - '0';
}

static void collect_headings(const mdy_node *n, Heading **out, size_t *count, size_t *cap) {
    int depth = heading_depth(n);
    if (depth) {
        if (*count == *cap) {
            size_t want = *cap ? *cap * 2 : 16;
            Heading *grown = realloc(*out, want * sizeof *grown);
            if (!grown) return;
            *out = grown;
            *cap = want;
        }
        char *text = NULL;
        size_t tlen = 0, tcap = 0;
        heading_text(n, &text, &tlen, &tcap);
        const char *id = NULL;
        for (const mdy_prop *p = n->props; p; p = p->next)
            if (strcmp(p->name, "id") == 0 && p->type == MDY_PROP_STRING) id = p->as.string;
        (*out)[*count].depth = depth;
        (*out)[*count].text = text ? text : calloc(1, 1);
        (*out)[*count].id = id ? strdup(id) : NULL;
        (*count)++;
    }
    for (const mdy_node *c = n->first; c; c = c->next)
        collect_headings(c, out, count, cap);
}

/*
 * A nested `<ul>`, or nothing when no heading carries an id — one list per
 * depth, the deepest last: a heading goes in the list at its own level, and a
 * level that opens goes inside the item above it.
 */
static mdy_node *toc_list(mdy_doc *doc, Heading *entries, size_t count) {
    size_t listed = 0;
    int min = 7;
    for (size_t i = 0; i < count; i++)
        if (entries[i].id) { listed++; if (entries[i].depth < min) min = entries[i].depth; }
    if (listed == 0) return NULL;

    mdy_node *root = mdy_new_element(doc, "ul", 2);
    struct { int depth; mdy_node *list; } stack[8];
    size_t top = 0;
    stack[0].depth = min;
    stack[0].list = root;

    for (size_t i = 0; i < count; i++) {
        if (!entries[i].id) continue;
        while (top > 0 && entries[i].depth < stack[top].depth) top--;
        while (entries[i].depth > stack[top].depth && top + 1 < 8) {
            mdy_node *above = stack[top].list->last;
            mdy_node *nested = mdy_new_element(doc, "ul", 2);
            if (above) {
                mdy_append(above, nested);
            } else {
                mdy_node *li = mdy_new_element(doc, "li", 2);
                mdy_append(li, nested);
                mdy_append(stack[top].list, li);
            }
            top++;
            stack[top].depth = stack[top - 1].depth + 1;
            stack[top].list = nested;
        }
        mdy_node *li = mdy_new_element(doc, "li", 2);
        mdy_node *a = mdy_new_element(doc, "a", 1);
        char href[512];
        int n = snprintf(href, sizeof href, "#%s", entries[i].id);
        mdy_set_string(doc, a, "href", href, (size_t)n);
        mdy_append(a, mdy_new_text(doc, entries[i].text, strlen(entries[i].text)));
        mdy_append(li, a);
        mdy_append(stack[top].list, li);
    }
    return root;
}

static void free_headings(Heading *entries, size_t count) {
    for (size_t i = 0; i < count; i++) { free(entries[i].text); free(entries[i].id); }
    free(entries);
}

/*
 * Fill every contents token in a finished tree. Run LAST, on the whole tree,
 * so the headings are all of them and the ids are the parser's.
 */
static void splice_toc(mdy_engine *e, mdy_doc *doc, mdy_node *parent,
                       Heading *entries, size_t count) {
    mdy_node *child = parent->first;
    parent->first = parent->last = NULL;

    while (child) {
        mdy_node *next = child->next;
        child->next = NULL;

        size_t len = 0;
        const char *text = sole_text(child, &len);
        char id[24];
        if (text && only_tokens(text, len) && token_at(text, len, id, sizeof id)) {
            Held *h = held_find(e, id);
            if (h && h->is_toc) {
                mdy_node *list = toc_list(doc, entries, count);
                if (list) mdy_append(parent, list);
                child = next;
                continue;
            }
        }
        splice_toc(e, doc, child, entries, count);
        mdy_append(parent, child);
        child = next;
    }
}

static void fill_toc(mdy_engine *e, mdy_doc *doc) {
    /* Nothing to do unless a token asked for one — the walk is not free. */
    mdy_engine *t = token_table(e);
    int wanted = 0;
    for (size_t i = 0; i < t->held_count && !wanted; i++) wanted = t->held[i].is_toc;
    if (!wanted) return;

    Heading *entries = NULL;
    size_t count = 0, cap = 0;
    collect_headings(mdy_root(doc), &entries, &count, &cap);
    splice_toc(e, doc, (mdy_node *)mdy_root(doc), entries, count);
    free_headings(entries, count);
}

/*
 * `$.toc()` with no argument is the token above. With one it is a QUESTION:
 * the headings of what was passed, for a document that would rather build the
 * list itself. MDY text, a hast node, or a rendered document (which arrives
 * as a token, so that has to be accepted too).
 */
static bool toc_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                       int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);

    if (argc < 1 || js_is_undefined(args[0])) {
        char *token = hold_toc(e);
        if (!token) { *result = js_undefined(); return true; }
        *result = str(e->vm, token, strlen(token));
        free(token);
        return true;
    }

    const mdy_node *tree = NULL;
    mdy_doc *owned = NULL;

    if (js_is_string(args[0])) {
        char *s = js_string_utf8(args[0]);
        size_t slen = s ? strlen(s) : 0;
        char id[24];
        /* A token is how a rendered document travels, so `$.toc($.render(…))`
         * is asking about THAT document, not about three characters. */
        if (s && only_tokens(s, slen) && token_at(s, slen, id, sizeof id)) {
            Held *h = held_find(e, id);
            if (h) tree = h->tree;
        } else {
            mdy_options options;
            parse_options(&options);
            owned = mdy_parse(s ? s : "", slen, &options);
            tree = owned ? mdy_root(owned) : NULL;
        }
        free(s);
    } else if (js_is_object(args[0]) &&
               js_is_string(js_object_get(e->vm, args[0], key(e->vm, "type")))) {
        owned = mdy_doc_new();
        tree = owned ? js_to_tree(e, owned, args[0]) : NULL;
    }

    if (!tree) {
        mdy_free(owned);
        const char *msg = "mdy: $.toc expects MDY text, a hast node, or a rendered document";
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }

    Heading *entries = NULL;
    size_t count = 0, cap = 0;
    collect_headings(tree, &entries, &count, &cap);

    JsValue out = js_array_new(ctx, (uint32_t)count);
    js_gc_protect(e->vm, &out);
    for (size_t i = 0; i < count; i++) {
        JsValue entry = js_object_new(e->ctx);
        js_gc_protect(e->vm, &entry);
        set_val(e, entry, "depth", js_number(entries[i].depth));
        set_val(e, entry, "text", str(e->vm, entries[i].text, strlen(entries[i].text)));
        if (entries[i].id)
            set_val(e, entry, "slug", str(e->vm, entries[i].id, strlen(entries[i].id)));
        push_item(e, out, entry);
        js_gc_unprotect(e->vm, &entry);
    }
    js_gc_unprotect(e->vm, &out);
    free_headings(entries, count);
    mdy_free(owned);
    *result = out;
    return true;
}


/* ---- publishing to a page ----------------------------------------------------
 *
 * `$.emit`'s other tense. `$.emit` writes an output; `$.publish` sends a
 * message to a page of this set BY NAME, and what happens to it is the
 * embedder's business — exactly as an emitted output is.
 *
 * A page's name is its path with the extension dropped and the separators
 * turned into dots: `handlers/invoice.mdy` is `handlers.invoice`. A document
 * may override that with `messageName` in its front matter. It is NOT `name`,
 * which is already taken twice over — every walked source carries its file's
 * base name, and a data record commonly declares its own — and reusing it
 * would let an author's data silently readdress their messages.
 */

/* The grammar is a file name's, because a subject is one where these end up:
 * letters, digits, `_`, `.` and `-`, 1–128 of them, no leading or trailing
 * dot and no `..`. */
static const char *name_problem(const char *name, char *buf, size_t buf_len) {
    if (!name || !*name) return "must be a non-empty string";
    size_t n = strlen(name);
    /* One rule, so one message: the grammar is a single anchored pattern, and
     * a name that is too long fails it the same way one with a space does. */
    int legal = n <= 128;
    for (size_t i = 0; i < n && legal; i++) {
        char c = name[i];
        legal = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    }
    if (!legal) {
        snprintf(buf, buf_len,
                 "may only contain letters, digits, \"_\", \".\" and \"-\", and must be "
                 "1\u2013128 characters (got \"%s\")", name);
        return buf;
    }
    if (name[0] == '.' || name[n - 1] == '.') return "must not start or end with \".\"";
    if (strstr(name, "..")) return "must not contain \"..\"";
    return NULL;
}

/* A document's message name, or NULL — most documents are never published to,
 * and a name is only required of the ones that are. Caller frees. */
static char *message_name(mdy_engine *e, size_t at) {
    JsValue record = document_record(e, at);
    js_gc_protect(e->vm, &record);

    /*
     * A record carrying an `ext` that is not .mdy/.md is skipped: a set built
     * from a directory holds raw records too, and a message RENDERS the page
     * it names, so a record with nothing to run is not an endpoint. It also
     * stops static/logo.png and static/logo.jpg colliding on `static.logo`.
     * A set built from a string has no `ext` at all and stays addressable.
     */
    char *ext = js_string_utf8(js_object_get(e->vm, record, key(e->vm, "ext")));
    if (ext) {
        int runnable = ends_with_ci(ext, ".mdy") || ends_with_ci(ext, ".md");
        free(ext);
        if (!runnable) { js_gc_unprotect(e->vm, &record); return NULL; }
    }

    char *declared = js_string_utf8(js_object_get(e->vm, record, key(e->vm, "messageName")));
    if (declared && *declared) { js_gc_unprotect(e->vm, &record); return declared; }
    free(declared);

    char *path = js_string_utf8(js_object_get(e->vm, record, key(e->vm, "path")));
    js_gc_unprotect(e->vm, &record);
    if (!path || !*path) { free(path); return NULL; }

    /* Drop the extension — the last dot, but only in the last segment. */
    char *slash = strrchr(path, '/');
    char *dot = strrchr(slash ? slash : path, '.');
    if (dot && dot != path) *dot = '\0';
    for (char *p = path; *p; p++) if (*p == '/') *p = '.';
    if (!*path) { free(path); return NULL; }
    return path;
}

static bool publish_native(JsContext *ctx, JsValue this_val, const JsValue *args,
                           int argc, JsValue *result) {
    (void)this_val;
    mdy_engine *e = js_context_userdata(ctx);
    *result = js_null();

    char *name = argc > 0 ? js_string_utf8(args[0]) : NULL;
    char why[512];
    const char *problem = name_problem(name, why, sizeof why);
    if (problem) {
        char msg[768];
        snprintf(msg, sizeof msg, "mdy: publish: a message name %s", problem);
        free(name);
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }

    /* Which documents answer to it. Deciding that the name means a page of
     * this set is the whole of core's job here. */
    size_t found = 0, first = 0;
    char others[512];
    size_t used = 0;
    others[0] = '\0';
    for (size_t i = 0; i < e->count; i++) {
        char *have = message_name(e, i);
        if (have && strcmp(have, name) == 0) {
            if (found == 0) first = i;
            found++;
            char *path = js_string_utf8(
                js_object_get(e->vm, document_record(e, i), key(e->vm, "path")));
            if (path && used + strlen(path) + 3 < sizeof others)
                used += (size_t)snprintf(others + used, sizeof others - used,
                                         "%s%s", used ? ", " : "", path);
            free(path);
        }
        free(have);
    }

    if (found == 0) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "mdy: publish: no document is named \"%s\" (a page's name is its path "
                 "without the extension, \"/\" written as \".\")", name);
        free(name);
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }
    if (found > 1) {
        char msg[768];
        snprintf(msg, sizeof msg,
                 "mdy: publish: \"%s\" is ambiguous \u2014 %zu documents share it (%s); "
                 "give one of them a messageName", name, found, others);
        free(name);
        *result = str(e->vm, msg, strlen(msg));
        return false;
    }

    if (e->on_publish) {
        /*
         * The data arrives ALREADY serialised, by the guest's own
         * JSON.stringify — see the `$` wrapper. That is deliberate: a second
         * serialiser written here would need its own number formatting, and
         * getting that subtly wrong is exactly the kind of difference that
         * shows up as a last digit in a page and nowhere else.
         */
        char *json = argc > 1 ? js_string_utf8(args[1]) : NULL;
        e->on_publish(e->on_publish_ud, name, json ? json : "{}", first);
        free(json);
    }
    free(name);
    return true;
}

static void register_natives(mdy_engine *e) {
    register_one(e, "__native", refuse);
    register_one(e, "__compose", compose_native);
    register_one(e, "__find", find_native);
    register_one(e, "__findOne", find_one_native);
    register_one(e, "__data", data_native);
    register_one(e, "__render", render_native);
    register_one(e, "__text", text_native);
    register_one(e, "__emit", emit_native);
    register_one(e, "__tokenize", tokenize_native);
    register_one(e, "__rfc822", rfc822_native);
    register_one(e, "__importRender", import_render_native);
    register_one(e, "__importFind", import_find_native);
    register_one(e, "__importFindOne", import_find_one_native);
    register_one(e, "__parse", parse_native);
    register_one(e, "__markdown", markdown_native);
    register_one(e, "__node", node_native);
    register_one(e, "__html", html_native);
    register_one(e, "__table", table_native);
    register_one(e, "__toc", toc_native);
    register_one(e, "__publish", publish_native);
}

/*
 * `$` is built in the wrapper source rather than as an object of native
 * function VALUES, because lamassu's public API has no way to make one:
 * js_register_native defines a global, and there is no js_function_new. So
 * one global native stands behind every name, exactly as mdy-docs' own
 * `__call` does — minus the JSON, since arguments now cross as values.
 *
 * Closing that gap is worth doing when `$` starts doing real work; a native
 * per name is clearer than a name-dispatching one, and it is the same small
 * addition to lamassu that js_array_new was.
 */


/* ---- the pieces ------------------------------------------------------------- */

mdy_engine *mdy_engine_new(void) {
    mdy_engine *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    JsVmConfig cfg = {0};
    /*
     * Two knobs for testing, and they earn their place: this engine hands the
     * VM values it has just built, and a value reachable only from the C stack
     * is invisible to the collector. Such a bug shows up as a property
     * silently becoming a DIFFERENT one — no crash, no error — and only in a
     * run long enough to collect at the wrong moment.
     *
     * MDY_GC_STRESS=1 collects at every safe point, which turns that from a
     * once-in-a-93-page-build event into a certainty.
     * MDY_GC_THRESHOLD=<bytes> moves the first collection, and setting it
     * enormous is how to ask "is this a collector problem at all?".
     */
    if (getenv("MDY_GC_THRESHOLD"))
        cfg.gc_threshold = (size_t)strtoull(getenv("MDY_GC_THRESHOLD"), NULL, 10);
    if (getenv("MDY_GC_STRESS")) cfg.gc_stress = true;
    e->vm = js_vm_new(&cfg);
    if (!e->vm) { free(e); return NULL; }
    e->handle = -1;
    e->ctx = js_context_new(e->vm);
    if (!e->ctx) { js_vm_free(e->vm); free(e); return NULL; }
    /* So a native can find the engine it belongs to. */
    js_context_set_userdata(e->ctx, e);
    register_natives(e);
    return e;
}

void mdy_engine_free(mdy_engine *e) {
    if (!e) return;

    /*
     * The graph is freed by whoever owns the cache — every package in it,
     * including this one, is in there exactly once. An importer must not free
     * its imports directly: a package imported twice is one set with two
     * importers, and the second free would be of memory already gone.
     */
    if (e->owns_cache && e->cache) {
        ImportCache *c = e->cache;
        e->cache = NULL;
        for (size_t i = 0; i < c->count; i++) {
            free(c->dirs[i]);
            if (c->sets[i] != e) {
                c->sets[i]->cache = NULL;      /* it does not own it */
                mdy_engine_free(c->sets[i]);
            }
        }
        for (size_t i = 0; i < c->root_count; i++) free(c->roots[i]);
        free(c->roots);
        free(c->dirs);
        free(c->sets);
        free(c);
    }

    for (size_t i = 0; i < e->import_count; i++) {
        free(e->imports[i].source_path);
        free(e->imports[i].spec);
    }
    free(e->imports);
    for (size_t i = 0; i < e->identity_count; i++) free(e->identity[i]);
    free(e->identity);
    free(e->root);

    close_set(e);
    mdy_free(e->tree_owner);
    js_context_free(e->ctx);
    js_vm_free(e->vm);
    free(e);
}

/*
 * The statements, wrapped as a function of their data.
 *
 * `req`, `res` and `$` are ARGUMENTS, so the same compiled function serves
 * every render of the document — which is the whole reason the script layer
 * produces statements that never mention the request.
 */
static char *wrap(const char *statements) {
    /*
     * Every `$` native, wired to the one global that stands behind them. They
     * all refuse for now — see the note above `refuse` — and a document that
     * calls one gets an error naming it rather than a page quietly missing
     * whatever it asked for.
     */
    static const char OPEN[] =
        "(async (req, res, $$) => {\n"
        "const $ = {\n"
        "  find: (q) => __find(q === undefined ? {} : q),\n"
        "  findOne: (q) => __findOne(q === undefined ? {} : q),\n"
        "  withTag: (t) => __find({ tags: String(t).toLowerCase() }),\n"
        "  render: (t, d) => __render(t, d),\n"
        "  text: (t, d) => __text(t, d),\n"
        "  emit: (p, c) => __emit(p, c),\n"
        "  publish: (n, d) => __publish(n, JSON.stringify(d === undefined ? {} : d)),\n"
        /* The one native that is a DEPENDENCY rather than a port: resizing
         * needs JPEG and PNG codecs. It refuses by name so a document that
         * asks gets told, rather than a page quietly missing a picture. */
        "  resize: (r, o) => __native('resize', r, o),\n"
        "  parse: (s) => __parse(s),\n"
        "  markdown: (s) => __markdown(s),\n"
        "  node: (t) => __node(t),\n"
        "  html: (v) => __html(v),\n"
        "  table: (r, a) => __table(r, a),\n"
        "  toc: (t) => __toc(t),\n"
        "  tokenize: (s) => __tokenize(s),\n"
        "  rfc822: (d) => __rfc822(d),\n"
        "  __importRender: (s, t, c) => __importRender(s, t, c),\n"
        "  __importFind: (s, q) => __importFind(s, q),\n"
        "  __importFindOne: (s, q) => __importFindOne(s, q),\n"
        "  __importResize: (s, r, o) => __native('resize', r, o),\n"
        "  data: (i) => __data(i),\n"
        "  compose: (o) => __compose(o),\n"
        "};\n"
        "const __transforms = [];\n"
        "const transform = (fn) => { __transforms.push(fn); };\n";

    /*
     * The epilogue, which is mdy-docs' own: a document with no transform hands
     * back its LINES and the host composes them; one with a transform asks for
     * its tree, runs each transform over it, and hands the TREE back. The two
     * shapes are told apart by which key the result carries.
     */
    static const char CLOSE[] =
        "\nif (__transforms.length > 0) {\n"
        "  let __tree = $.compose(__out);\n"
        "  res.doc = __tree;\n"
        "  for (const fn of __transforms) {\n"
        "    const returned = fn(__tree);\n"
        "    if (returned !== undefined && returned !== null) {\n"
        "      if (typeof returned !== \"object\" || typeof returned.type !== \"string\") {\n"
        "        throw \"transform must return a hast node ({ type, ... }), or undefined after changing the tree in place\";\n"
        "      }\n"
        "      __tree = returned;\n"
        "    }\n"
        "    res.doc = __tree;\n"
        "  }\n"
        "  return { tree: __tree };\n"
        "}\n"
        "return { out: __out };\n})";
    size_t n = strlen(OPEN) + strlen(MDY_TOOLKIT) + strlen(statements) + strlen(CLOSE) + 1;
    char *out = malloc(n);
    if (out) snprintf(out, n, "%s%s%s%s", OPEN, MDY_TOOLKIT, statements, CLOSE);
    return out;
}

/*
 * `scriptOutput`: the `[line, text]` pairs the document produced, flattened
 * into lines. One push can still yield several, when what was interpolated had
 * newlines in it.
 */
static char *flatten(JsValue out, size_t *out_len) {
    size_t cap = 4096, len = 0;
    char *text = malloc(cap);
    if (!text) return NULL;
    text[0] = '\0';

    uint32_t n = js_array_length(out);
    for (uint32_t i = 0; i < n; i++) {
        JsValue pair = js_array_get(out, i);
        JsValue value = js_array_get(pair, 1);
        size_t ulen = 0;
        const uint16_t *u = js_string_units(value, &ulen);
        char *piece = u ? from_utf16(u, ulen) : NULL;
        if (!piece) continue;
        size_t plen = strlen(piece);
        if (len + plen + 2 > cap) {
            while (len + plen + 2 > cap) cap *= 2;
            char *grown = realloc(text, cap);
            if (!grown) { free(piece); free(text); return NULL; }
            text = grown;
        }
        if (len) text[len++] = '\n';
        memcpy(text + len, piece, plen);
        len += plen;
        text[len] = '\0';
        free(piece);
    }
    *out_len = len;
    return text;
}

/* The lines a document produced, parsed — what `$.compose` returns and what a
 * document with no transform gets at the end. */
static mdy_doc *parse_lines(JsValue out, mdy_engine *e) {
    (void)e;
    size_t text_len = 0;
    char *text = flatten(out, &text_len);
    if (!text) return NULL;

    mdy_options options;
    mdy_options_default(&options);
    /* What the document engine asks the parser for: it has already taken the
     * front matter off and split the documents, and the code has already run
     * — so all three are the parser's business no longer. */
    options.frontmatter = 0;
    options.documents = 0;
    options.sanitize = 0;

    mdy_doc *tree = mdy_parse(text, text_len, &options);
    free(text);
    return tree;
}

static mdy_doc *render_tree(mdy_engine *e, size_t index, JsValue request,
                            char *error, size_t error_len) {
    return render_tree_out(e, index, request, NULL, error, error_len);
}

static mdy_doc *render_tree_out(mdy_engine *e, size_t index, JsValue request,
                                char **wrote, char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    if (wrote) *wrote = NULL;
    mdy_doc *out = NULL;
    mdy_script *script = NULL;
    /*
     * Declared up here, and released together at `done`, because every one of
     * them is a GC ROOT and a root is an ADDRESS the collector keeps. A root
     * left registered when this frame returns points at reused stack memory,
     * and the next collection marks whatever now sits there — a crash with no
     * relation to the code that caused it. `FAIL` jumps straight to `done`, so
     * there is no path that can skip the release.
     */
    int rooted = 0;
    JsValue fn = js_undefined(), promise = js_undefined(), callable = js_undefined();
    JsValue req = js_undefined(), res = js_undefined(), dollar = js_undefined();
    JsValue result = js_undefined();

    /* A render inside a render inside a render is a cycle somebody wrote. */
    if (e->depth > 32) {
        if (error && error_len)
            snprintf(error, error_len, "mdy-engine: render depth exceeded (cyclic $.render?)");
        return NULL;
    }
    e->depth++;
    /* Which file is asking — an `$.__import*` native resolves its spec
     * against the document that wrote it, and the same spec in two files can
     * mean two packages. */
    size_t outer_current = e->current;
    e->current = index;

#define FAIL(...) do { if (error && error_len) snprintf(error, error_len, __VA_ARGS__); goto done; } while (0)

    if (index >= e->count) {
        if (error && error_len) snprintf(error, error_len, "no document at index %zu", index);
        return NULL;
    }
    Document *d = &e->docs[index];
    /* 1. the body, which `mdy_engine_open` already took the data out of */
    size_t template_len = 0;
    const char *template_text = mdy_data_body(d->fences, &template_len);

    /* 2. the script layer */
    script = mdy_script_compile(template_text, template_len);
    if (!script) FAIL("the script layer could not compile this document");

    size_t src_len = 0;
    char *wrapped = wrap(mdy_script_source(script, &src_len));
    if (!wrapped) FAIL("out of memory");

    size_t ulen = 0;
    uint16_t *u = to_utf16(wrapped, strlen(wrapped), &ulen);
    const char *err_msg = NULL;
    uint32_t err_pos = 0;
    fn = js_compile_module(e->ctx, u, ulen, &err_msg, &err_pos);
    free(u);
    free(wrapped);
    if (js_is_undefined(fn)) FAIL("the document's code did not compile: %s", err_msg ? err_msg : "?");
    js_gc_protect(e->vm, &fn);
    rooted = 1;

    promise = js_run_module(e->ctx, fn);
    js_gc_protect(e->vm, &promise);
    js_run_jobs(e->ctx);
    callable = js_promise_result(promise);
    if (!js_is_function(callable)) FAIL("the document's code did not produce a function");
    js_gc_protect(e->vm, &callable);

    /* the request, the response, and `$` */
    req = js_is_object(request) ? request : js_object_new(e->ctx);
    js_gc_protect(e->vm, &req);
    res = js_object_new(e->ctx);
    js_gc_protect(e->vm, &res);
    /*
     * `res.data` is the document's OWN data — its front matter, its data
     * fences and its file identity, the same record `$.data(index)` gives.
     * That is what lets a template write `req.x ?? res.data.x` and always be
     * able to reach its own declared value.
     */
    set_val(e, res, "data", document_record(e, index));
    dollar = js_object_new(e->ctx);
    js_gc_protect(e->vm, &dollar);
    JsValue args[3] = { req, res, dollar };
    if (!js_call(e->ctx, callable, js_undefined(), args, 3, &result)) {
        size_t mlen = 0;
        const uint16_t *mu = js_string_units(result, &mlen);
        char *msg = mu ? from_utf16(mu, mlen) : NULL;
        if (error && error_len) snprintf(error, error_len, "%s", msg ? msg : "the document threw");
        free(msg);
        goto done;
    }
    js_gc_protect(e->vm, &result);

    if (js_is_promise(result)) {
        js_run_jobs(e->ctx);
        int state = js_promise_state(result);
        if (state != 1) {
            /*
             * A rejection reason is not always a string — a thrown Error is an
             * object with `message`, and reporting "did not settle" for one
             * hides the actual fault behind a symptom. Pending and rejected
             * are also different problems and must not read the same.
             */
            JsValue reason = js_promise_result(result);
            char *msg = NULL;
            size_t mlen = 0;
            const uint16_t *mu = js_string_units(reason, &mlen);
            if (mu) msg = from_utf16(mu, mlen);
            if (!msg && js_is_object(reason)) {
                JsValue m = js_object_get(e->vm, reason, key(e->vm, "message"));
                mu = js_string_units(m, &mlen);
                if (mu) msg = from_utf16(mu, mlen);
            }
            if (error && error_len) {
                if (msg) snprintf(error, error_len, "%s", msg);
                else if (state == 0) snprintf(error, error_len,
                    "the document did not settle (a promise is still pending)");
                else snprintf(error, error_len, "the document was rejected with a non-string reason");
            }
            free(msg);
            goto done;
        }
        JsValue settled = js_promise_result(result);
        js_gc_unprotect(e->vm, &result);
        result = settled;
        js_gc_protect(e->vm, &result);
    }

    if (!js_is_object(result)) FAIL("the document did not produce a result");

    /*
     * 3. the tree. A document with a transform already has one — it asked for
     * it through `$.compose` and handed back what its transforms made of it —
     * and one without hands back its lines for the host to parse.
     */
    JsValue lines_out = js_object_get(e->vm, result, key(e->vm, "out"));
    if (wrote && js_is_array(lines_out)) {
        /* No transform: the text is what the code wrote, joined — mdy.js's
         * `scriptOutput(out).lines.join('\n')`, which is exactly `flatten`. */
        size_t n = 0;
        *wrote = flatten(lines_out, &n);
    }

    JsValue transformed = js_object_get(e->vm, result, key(e->vm, "tree"));
    if (js_is_object(transformed)) {
        /* Already composed: `$.compose` spliced it before the transforms saw
         * it, which is what let a transform work on the finished tree. */
        mdy_doc *doc = mdy_doc_new();
        if (!doc) FAIL("out of memory");
        mdy_node *root = js_to_tree(e, doc, transformed);
        /*
         * `mdy_doc` owns its root, so the tree that came back is hung under
         * it: a root lends its children, and a transform that returned a
         * single element becomes that document's one child. Which is what
         * `blockContent` does with a held tree, for the same reason.
         */
        mdy_node *into = (mdy_node *)mdy_root(doc);
        if (root && root->type == MDY_ROOT) {
            for (mdy_node *c = root->first; c;) {
                mdy_node *next = c->next;
                c->next = NULL;
                mdy_append(into, c);
                c = next;
            }
        } else if (root) {
            mdy_append(into, root);
        }
        out = doc;
        /* A transformed document has no lines left to hand back — it gave up
         * its `out` for a tree — so its text is that tree's HTML, which is
         * what mdy.js falls back to for exactly this case. */
        if (wrote && !*wrote) *wrote = mdy_to_html(mdy_root(doc), NULL);
    } else {
        JsValue lines = js_object_get(e->vm, result, key(e->vm, "out"));
        if (!js_is_array(lines)) FAIL("the document did not produce its lines");
        mdy_doc *tree = parse_lines(lines, e);
        if (!tree) FAIL("the produced lines did not parse");
        /* The held trees go back where their tokens are. */
        splice_tree(e, tree, mdy_root(tree));
        out = tree;
    }

    /* Last of all, on the finished tree: a contents list names every heading
     * the document ended up with, including ones written below it. */
    if (out) fill_toc(e, out);

done:
#undef FAIL
    if (rooted) {
        js_gc_unprotect(e->vm, &fn);
        js_gc_unprotect(e->vm, &promise);
        js_gc_unprotect(e->vm, &callable);
        js_gc_unprotect(e->vm, &req);
        js_gc_unprotect(e->vm, &res);
        js_gc_unprotect(e->vm, &dollar);
        js_gc_unprotect(e->vm, &result);
    }
    e->current = outer_current;
    e->depth--;
    mdy_script_free(script);
    return out;
}

char *mdy_engine_render(mdy_engine *e, size_t index, char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    mdy_doc *doc = render_tree(e, index, js_undefined(), error, error_len);
    if (!doc) { release_held(e); return NULL; }
    char *html = mdy_to_html(mdy_root(doc), NULL);
    mdy_free(doc);
    /* Nothing a render parked outlives the render that asked for it. */
    release_held(e);
    return html;
}
