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

static uint16_t *to_utf16(const char *s, size_t *out_len) {
    size_t n = strlen(s);
    uint16_t *u = malloc(n * sizeof(uint16_t) + 2);
    for (size_t i = 0; i < n; i++) u[i] = (uint16_t)(unsigned char)s[i];
    *out_len = n;
    return u;
}

/* ASCII for now: enough to prove the seam, and the place to widen when real
 * documents cross it. */
static char *from_utf16(const uint16_t *u, size_t len) {
    char *s = malloc(len + 1);
    for (size_t i = 0; i < len; i++) s[i] = u[i] < 128 ? (char)u[i] : '?';
    s[len] = '\0';
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

    char *question = from_utf16(units, len);
    char *answer = g_ask(question, g_ask_ud);
    free(question);
    if (!answer) return true;

    size_t alen = 0;
    uint16_t *aunits = to_utf16(answer, &alen);
    *result = js_string_new(js_context_vm(ctx), aunits, alen);
    free(aunits);
    free(answer);
    return true;
}

char *lam_eval(const char *source, lam_ask_fn ask, void *ud, char **err) {
    *err = NULL;
    g_ask = ask;
    g_ask_ud = ud;

    JsVm *vm = js_vm_new(NULL);
    if (!vm) { *err = strdup("lamassu: no vm"); return NULL; }
    JsContext *ctx = js_context_new(vm);

    size_t nlen = 0;
    uint16_t *name = to_utf16("ask", &nlen);
    js_register_native(ctx, name, nlen, native_ask, NULL);
    free(name);

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
        js_vm_free(vm);
        return NULL;
    }

    JsValue done = js_run_module(ctx, fn);
    JsValue value = js_is_promise(done) ? js_promise_result(done) : done;
    size_t len = 0;
    const uint16_t *units = js_string_units(value, &len);
    char *out = units ? from_utf16(units, len) : strdup("(no string completion value)");
    js_vm_free(vm);
    return out;
}
