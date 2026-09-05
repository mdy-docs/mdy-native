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
#include "mdyscript.h"

struct mdy_engine {
    JsVm *vm;
    JsContext *ctx;
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

static void register_natives(mdy_engine *e) {
    size_t n = 0;
    uint16_t *u = to_utf16("__native", 8, &n);
    if (!u) return;
    js_register_native(e->ctx, u, n, refuse, NULL);
    free(u);
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
    register_natives(e);
    return e;
}

void mdy_engine_free(mdy_engine *e) {
    if (!e) return;
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
        "  compose: (o) => __native('compose', o),\n"
        "};\n";
    static const char CLOSE[] = "\nreturn __out\n})";
    size_t n = strlen(OPEN) + strlen(statements) + strlen(CLOSE) + 1;
    char *out = malloc(n);
    if (out) snprintf(out, n, "%s%s%s", OPEN, statements, CLOSE);
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

    if (!js_is_array(result)) FAIL("the document did not produce its lines");

    /* 3. the lines, parsed */
    size_t text_len = 0;
    char *text = flatten(result, &text_len);
    if (!text) FAIL("out of memory");

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
    if (!tree) FAIL("the produced lines did not parse");

    html = mdy_to_html(mdy_root(tree), NULL);
    mdy_free(tree);

done:
#undef FAIL
    mdy_script_free(script);
    mdy_data_free(data);
    mdy_documents_free(docs);
    return html;
}
