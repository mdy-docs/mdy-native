/*
 * The lamassu side. Includes lamassu.h and nothing from QuickJS — see lam.h.
 *
 * Two things are being established here, and only these. A program compiles
 * and runs; and a program can call out to the host, with values crossing as
 * values. The WASM build routes every host call through one
 * `__hostcall(name, jsonArgs)` and pays JSON in both directions — that path
 * exists only in src/wasm_api.c, because it is a workaround for the WASM
 * boundary. Natively, functions are registered directly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lamassu.h"
#include "lamassu_compile.h"
#include "lam.h"

static lam_ask_fn g_ask;
static void *g_ask_ud;

/*
 * UTF-8 <-> UTF-16, properly. The first version of this truncated every byte
 * to a code unit, which is fine for `42` and wrong for every document this is
 * meant to serve — the reference corpus is full of Akkadian transliteration
 * (ā, š, ṣ) and the em dashes in its prose. It showed up as `???` in an error
 * message, which is the cheap way to find out.
 */
static uint16_t *to_utf16(const char *s, size_t *out_len) {
    size_t n = strlen(s);
    uint16_t *u = malloc((n + 1) * 2 * sizeof(uint16_t)); /* worst case: surrogates */
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int len;
        if (c < 0x80)            { cp = c;          len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else                     { cp = 0xFFFD;      len = 1; }
        if (i + (size_t)len > n) { cp = 0xFFFD; len = 1; }
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += len;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            u[o++] = (uint16_t)(0xD800 + (cp >> 10));
            u[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            u[o++] = (uint16_t)cp;
        }
    }
    *out_len = o;
    return u;
}

static char *from_utf16(const uint16_t *u, size_t len) {
    char *s = malloc(len * 4 + 1); /* worst case: 4 bytes per unit */
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[++i] - 0xDC00);
        }
        if (cp < 0x80) s[o++] = (char)cp;
        else if (cp < 0x800) { s[o++] = (char)(0xC0 | (cp >> 6)); s[o++] = (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            s[o++] = (char)(0xE0 | (cp >> 12));
            s[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            s[o++] = (char)(0xF0 | (cp >> 18));
            s[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            s[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            s[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    s[o] = '\0';
    return s;
}

static bool native_ask(JsContext *ctx, JsValue this_val, const JsValue *args, int argc,
                       JsValue *result) {
    (void)this_val;
    *result = js_undefined();
    if (argc < 1 || !g_ask) return true;

    size_t len = 0;
    const uint16_t *units = js_string_units(args[0], &len);
    if (!units) return true;
    char *name = from_utf16(units, len);

    char *args_json = NULL;
    if (argc > 1) {
        size_t alen = 0;
        const uint16_t *aunits = js_string_units(args[1], &alen);
        if (aunits) args_json = from_utf16(aunits, alen);
    }

    char *err = NULL;
    char *answer = g_ask(name, args_json ? args_json : "[]", g_ask_ud, &err);
    free(name);
    free(args_json);

    if (!answer) {
        /* false with *result = the value to throw — see JsNativeFn in
         * lamassu.h. The guest's own try/catch is what reads it. */
        size_t elen = 0;
        uint16_t *eunits = to_utf16(err ? err : "host call failed", &elen);
        *result = js_string_new(js_context_vm(ctx), eunits, elen);
        free(eunits);
        free(err);
        return false;
    }

    size_t alen = 0;
    uint16_t *aunits = to_utf16(answer, &alen);
    *result = js_string_new(js_context_vm(ctx), aunits, alen);
    free(aunits);
    free(answer);
    return true;
}

/* ---- guest `import` ------------------------------------------------------- */

/*
 * The module callbacks DO get a user pointer of their own — js_set_module_loader
 * takes one and hands it back — so unlike the host-call state above, this needs
 * no global and is re-entrancy-safe by construction. One of these lives on
 * lam_eval's stack for the duration of the eval.
 */
typedef struct {
    lam_module_fn fn;
    void *ud;
    uint16_t *canon;  /* the last canonical specifier, kept alive across the
                       * call that returned it: the engine copies, but only
                       * after the callback returns. */
} ModuleHost;

static bool canon_cb(void *ud, const uint16_t *specifier, size_t spec_len,
                     const uint16_t *referrer, size_t ref_len,
                     const uint16_t **out_specifier, size_t *out_spec_len) {
    ModuleHost *h = ud;
    if (!h || !h->fn) return false;

    char *spec = from_utf16(specifier, spec_len);
    char *ref = from_utf16(referrer, ref_len);
    char *err = NULL;
    char *canonical = h->fn(LAM_MODULE_CANON, spec, ref, h->ud, &err);
    free(spec);
    free(ref);
    if (!canonical) { free(err); return false; }

    free(h->canon);
    size_t len = 0;
    h->canon = to_utf16(canonical, &len);
    free(canonical);
    *out_specifier = h->canon;
    *out_spec_len = len;
    return true;
}

static JsValue load_cb(void *ud, JsContext *ctx, const uint16_t *specifier, size_t spec_len,
                       const uint16_t *referrer, size_t ref_len) {
    ModuleHost *h = ud;
    JsValue promise = js_promise_new(ctx);

    char *spec = from_utf16(specifier, spec_len);
    char *ref = from_utf16(referrer, ref_len);
    char *err = NULL;
    char *src = h && h->fn ? h->fn(LAM_MODULE_LOAD, spec, ref, h->ud, &err) : NULL;

    /*
     * Settled before it is returned. The host side of this is JavaScript and
     * its answer may be a promise — but host.c pumps QuickJS's job queue until
     * that settles, exactly as it does for a host call, so by the time control
     * is back here the source is a string. The guest never learns it waited,
     * which is the same contract `$.find` runs on.
     */
    if (src) {
        size_t len = 0;
        uint16_t *units = to_utf16(src, &len);
        js_resolve(ctx, promise, js_string_new(js_context_vm(ctx), units, len));
        free(units);
        free(src);
    } else {
        char buf[512];
        snprintf(buf, sizeof buf, "cannot load module %s: %s", spec, err ? err : "no loader installed");
        size_t len = 0;
        uint16_t *units = to_utf16(buf, &len);
        js_reject(ctx, promise, js_string_new(js_context_vm(ctx), units, len));
        free(units);
        free(err);
    }
    free(spec);
    free(ref);
    return promise;
}

/* ---- an instance ---------------------------------------------------------- */

struct LamVm {
    JsVm *vm;
    JsContext *ctx;
};

LamVm *lam_vm_new(void) {
    LamVm *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->vm = js_vm_new(NULL);
    if (!v->vm) { free(v); return NULL; }
    v->ctx = js_context_new(v->vm);

    /* Registered once per instance rather than once per eval: the native's
     * identity does not change, only the `ask` it forwards to, and that is
     * stacked around each eval below. */
    size_t nlen = 0;
    uint16_t *name = to_utf16("__hostcall", &nlen);
    js_register_native(v->ctx, name, nlen, native_ask, NULL);
    free(name);

    /*
     * Turning SOURCE into a module is a frontend capability and it is off by
     * default — a runtime-only build links no parser, so "this process cannot
     * compile source it is handed" is guaranteed by the link rather than by
     * policy, and even a build that has the parser must ask. Without this a
     * loader that resolves with source fails at the fetch with "source modules
     * unavailable in this build (precompile to bytecode)", which reads like a
     * missing library and is really a missing line.
     */
    js_enable_source_modules(v->ctx);
    return v;
}

void lam_vm_free(LamVm *v) {
    if (!v) return;
    js_vm_free(v->vm);
    free(v);
}

char *lam_eval(LamVm *v, const char *source, lam_ask_fn ask, lam_module_fn module,
               void *ud, char **err) {
    *err = NULL;
    if (!v) { *err = strdup("lamassu: no vm"); return NULL; }
    JsContext *ctx = v->ctx;

    /*
     * SAVE AND RESTORE, because this is re-entrant: `$.render` is a host call
     * whose answer comes from another lam_eval, so an inner call runs while an
     * outer one is suspended inside native_ask. lamassu hands a native no user
     * pointer of its own (the slot js_register_native takes is not passed back
     * to the callback), so the "who is asking" state is global — which is fine
     * as long as it is stacked. It was not, once, and the symptom was a second
     * nested render failing with `unknown native "render"`: the outer eval
     * resumed holding the inner's identity, and the inner's natives had already
     * been torn down by the pool.
     */
    lam_ask_fn saved_ask = g_ask;
    void *saved_ud = g_ask_ud;
    g_ask = ask;
    g_ask_ud = ud;

    /* Per-eval, and cleared on the way out: an instance outlives any one
     * render, and a loader left installed would answer a later render's
     * imports from the wrong package root. Same rule vm.js states. */
    ModuleHost mod = { .fn = module, .ud = ud, .canon = NULL };
    js_set_module_loader(ctx, module ? load_cb : NULL, module ? canon_cb : NULL,
                         module ? &mod : NULL);

#define LAM_DONE(result)                     \
    do {                                     \
        js_set_module_loader(ctx, NULL, NULL, NULL); \
        free(mod.canon);                     \
        g_ask = saved_ask;                   \
        g_ask_ud = saved_ud;                 \
        return (result);                     \
    } while (0)

    size_t slen = 0;
    uint16_t *src = to_utf16(source, &slen);
    const char *msg = NULL;
    uint32_t pos = 0;
    JsValue fn = js_compile_module(ctx, src, slen, &msg, &pos);
    free(src);
    if (msg) {
        char buf[512];
        snprintf(buf, sizeof buf, "lamassu compile: %s (at %u)", msg, pos);
        *err = strdup(buf);
        LAM_DONE(NULL);
    }

    JsValue done = js_run_module(ctx, fn);

    /*
     * A program with no `import` settles inside js_run_module and this loop
     * does not run. One with an import does not: the loader resolves its
     * promise, and the module graph only links and evaluates once the
     * microtask queue drains. js_has_pending_jobs is the termination
     * condition, so a promise nothing can settle is reported rather than spun
     * on — the same rule host.c applies to a host call.
     */
    while (js_is_promise(done) && js_promise_state(done) == 0) {
        if (!js_has_pending_jobs(ctx)) break;
        js_run_jobs(ctx);
    }

    if (js_is_promise(done) && js_promise_state(done) == 2) {
        /* A rejection here is a program that threw, or a module that would not
         * load, and the reason is the only thing that says which. Returning
         * "no completion value" is how a missing import used to look. */
        JsValue reason = js_promise_result(done);
        size_t rlen = 0;
        const uint16_t *runits = js_string_units(reason, &rlen);
        char *text = runits ? from_utf16(runits, rlen) : strdup("(non-string rejection)");
        char buf[640];
        snprintf(buf, sizeof buf, "lamassu: %s", text);
        free(text);
        *err = strdup(buf);
        LAM_DONE(NULL);
    }
    if (js_is_promise(done) && js_promise_state(done) == 0) {
        *err = strdup("lamassu: the program never settled — nothing left to run");
        LAM_DONE(NULL);
    }

    JsValue value = js_is_promise(done) ? js_promise_result(done) : done;
    size_t len = 0;
    const uint16_t *units = js_string_units(value, &len);
    char *out = units ? from_utf16(units, len) : strdup("(no string completion value)");
    LAM_DONE(out);
#undef LAM_DONE
}
