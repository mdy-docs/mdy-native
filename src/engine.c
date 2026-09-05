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
#include "mdybuild.h"
#include "mdyscript.h"
#include "toolkit.h"

struct mdy_engine {
    JsVm *vm;
    JsContext *ctx;
    /*
     * The render in progress. `$.compose` is a host call made from the middle
     * of one, and it needs the document being rendered; with one render at a
     * time this is where it lives.
     */
    mdy_doc *tree_owner;
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
        js_array_push(e->vm, array, child);
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
            js_object_set(e->vm, o, key(e->vm, "type"), str(e->vm, type, strlen(type)));
            const char *v = n->text ? n->text : "";
            js_object_set(e->vm, o, key(e->vm, "value"), str(e->vm, v, strlen(v)));
            break;
        }
        case MDY_DOCTYPE:
            js_object_set(e->vm, o, key(e->vm, "type"), str(e->vm, "doctype", 7));
            break;
        case MDY_ROOT:
            js_object_set(e->vm, o, key(e->vm, "type"), str(e->vm, "root", 4));
            js_object_set(e->vm, o, key(e->vm, "children"), children_to_js(e, n));
            break;
        case MDY_ELEMENT: {
            js_object_set(e->vm, o, key(e->vm, "type"), str(e->vm, "element", 7));
            js_object_set(e->vm, o, key(e->vm, "tagName"), str(e->vm, n->tag, strlen(n->tag)));
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
                            js_array_push(e->vm, v, str(e->vm, p->list[i], strlen(p->list[i])));
                        js_gc_unprotect(e->vm, &v);
                        break;
                    }
                    default: v = js_undefined();
                }
                js_object_set(e->vm, props, key(e->vm, p->name), v);
            }
            js_object_set(e->vm, o, key(e->vm, "properties"), props);
            js_gc_unprotect(e->vm, &props);
            js_object_set(e->vm, o, key(e->vm, "children"), children_to_js(e, n));
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

static void register_natives(mdy_engine *e) {
    register_one(e, "__native", refuse);
    register_one(e, "__compose", compose_native);
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
    e->vm = js_vm_new(&cfg);
    if (!e->vm) { free(e); return NULL; }
    e->ctx = js_context_new(e->vm);
    if (!e->ctx) { js_vm_free(e->vm); free(e); return NULL; }
    /* So a native can find the engine it belongs to. */
    js_context_set_userdata(e->ctx, e);
    register_natives(e);
    return e;
}

void mdy_engine_free(mdy_engine *e) {
    if (!e) return;
    mdy_free(e->tree_owner);
    js_context_free(e->ctx);
    js_vm_free(e->vm);
    free(e);
}

size_t mdy_engine_count(mdy_engine *e, const char *source, size_t len) {
    (void)e;
    mdy_documents *docs = mdy_split_documents(source, len);
    size_t n = mdy_documents_count(docs);
    mdy_documents_free(docs);
    return n;
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
        "  find: (q) => __native('find', q),\n"
        "  findOne: (q) => __native('findOne', q),\n"
        "  withTag: (t) => __native('withTag', t),\n"
        "  render: (t, d) => __native('render', t, d),\n"
        "  text: (t, d) => __native('text', t, d),\n"
        "  emit: (p, c) => __native('emit', p, c),\n"
        "  publish: (n, d) => __native('publish', n, d),\n"
        "  parse: (s) => __native('parse', s),\n"
        "  markdown: (s) => __native('markdown', s),\n"
        "  node: (t) => __native('node', t),\n"
        "  html: (v) => __native('html', v),\n"
        "  table: (r, a) => __native('table', r, a),\n"
        "  toc: (t) => __native('toc', t),\n"
        "  data: (i) => __native('data', i),\n"
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

char *mdy_engine_render(mdy_engine *e, const char *source, size_t len,
                        size_t index, char *error, size_t error_len) {
    if (error && error_len) error[0] = '\0';
    char *html = NULL;

#define FAIL(...) do { if (error && error_len) snprintf(error, error_len, __VA_ARGS__); goto done; } while (0)

    mdy_documents *docs = mdy_split_documents(source, len);
    if (!docs) return NULL;
    if (index >= mdy_documents_count(docs)) {
        if (error && error_len) snprintf(error, error_len, "no document at index %zu", index);
        mdy_documents_free(docs);
        return NULL;
    }

    mdy_chunk chunk = mdy_documents_at(docs, index);

    /* 1. the data at the top, and the fences in the body */
    mdy_chunk matter, body;
    mdy_split_frontmatter(chunk.text, chunk.len, &matter, &body);
    mdy_data *data = mdy_data_extract(body.text, body.len);

    size_t template_len = 0;
    const char *template_text = data ? mdy_data_body(data, &template_len) : body.text;
    if (!data) template_len = body.len;

    /* 2. the script layer */
    mdy_script *script = mdy_script_compile(template_text, template_len);
    if (!script) FAIL("the script layer could not compile this document");

    size_t src_len = 0;
    char *wrapped = wrap(mdy_script_source(script, &src_len));
    if (!wrapped) FAIL("out of memory");

    size_t ulen = 0;
    uint16_t *u = to_utf16(wrapped, strlen(wrapped), &ulen);
    const char *err_msg = NULL;
    uint32_t err_pos = 0;
    JsValue fn = js_compile_module(e->ctx, u, ulen, &err_msg, &err_pos);
    free(u);
    free(wrapped);
    if (js_is_undefined(fn)) FAIL("the document's code did not compile: %s", err_msg ? err_msg : "?");
    js_gc_protect(e->vm, &fn);

    JsValue promise = js_run_module(e->ctx, fn);
    js_gc_protect(e->vm, &promise);
    js_run_jobs(e->ctx);
    JsValue callable = js_promise_result(promise);
    if (!js_is_function(callable)) FAIL("the document's code did not produce a function");
    js_gc_protect(e->vm, &callable);

    /* the request, the response, and `$` */
    JsValue req = js_object_new(e->ctx);
    js_gc_protect(e->vm, &req);
    JsValue res = js_object_new(e->ctx);
    js_gc_protect(e->vm, &res);
    js_object_set(e->vm, res, key(e->vm, "data"), js_object_new(e->ctx));
    JsValue dollar = js_object_new(e->ctx);
    js_gc_protect(e->vm, &dollar);
    JsValue args[3] = { req, res, dollar };
    JsValue result = js_undefined();
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
        if (js_promise_state(result) != 1) {
            JsValue reason = js_promise_result(result);
            size_t mlen = 0;
            const uint16_t *mu = js_string_units(reason, &mlen);
            char *msg = mu ? from_utf16(mu, mlen) : NULL;
            if (error && error_len)
                snprintf(error, error_len, "%s", msg ? msg : "the document did not settle");
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
    JsValue transformed = js_object_get(e->vm, result, key(e->vm, "tree"));
    if (js_is_object(transformed)) {
        mdy_doc *doc = mdy_doc_new();
        if (!doc) FAIL("out of memory");
        mdy_node *root = js_to_tree(e, doc, transformed);
        html = mdy_to_html(root ? root : mdy_root(doc), NULL);
        mdy_free(doc);
    } else {
        JsValue out = js_object_get(e->vm, result, key(e->vm, "out"));
        if (!js_is_array(out)) FAIL("the document did not produce its lines");
        mdy_doc *tree = parse_lines(out, e);
        if (!tree) FAIL("the produced lines did not parse");
        html = mdy_to_html(mdy_root(tree), NULL);
        mdy_free(tree);
    }

done:
#undef FAIL
    mdy_free(e->tree_owner);
    e->tree_owner = NULL;
    mdy_script_free(script);
    mdy_data_free(data);
    mdy_documents_free(docs);
    return html;
}
