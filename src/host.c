/*
 * mdy-native — the backend as a binary.
 *
 * mdy-docs' own JavaScript runs in QuickJS; lamassu and nisaba are linked as C
 * rather than loaded as WebAssembly. No renderer, no webview, no memory
 * ceiling. See ../../docs/desktop-plan.md for the measurements that chose
 * this, and ../README.md for what it cost.
 *
 * This file is the QuickJS half and includes nothing from either engine —
 * lam.h and nis.h are the seams, and they name no type belonging to one. That
 * separation was forced by a header collision (both engines use the `js_`
 * prefix, and JS_TAG_STRING is a macro in one and an enum member in the
 * other), and it is the right shape regardless.
 *
 * THE ASYNC CONTRACT. A document calls `$.find(q)` synchronously; mdy-docs
 * implements that native in JavaScript and it is async. There is no Asyncify
 * natively, so when a host call returns a promise this pumps QuickJS's job
 * queue until it settles and returns the value synchronously. The guest never
 * learns it waited, mdy-docs is untouched, and a promise that cannot settle is
 * reported rather than hung on.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "lam.h"
#include "nis.h"

static JSContext *g_ctx;

/* ---- waiting for JavaScript ---------------------------------------------- */

static char *settle_to_string(JSContext *ctx, JSValue value, const char **why) {
    *why = NULL;
    JSRuntime *rt = JS_GetRuntime(ctx);

    while (JS_PromiseState(ctx, value) == JS_PROMISE_PENDING) {
        JSContext *which = NULL;
        int ran = JS_ExecutePendingJob(rt, &which);
        if (ran < 0) { *why = "a queued job threw"; return NULL; }
        if (ran == 0) { *why = "promise never settled — nothing left to run"; return NULL; }
    }

    JSValue settled = value;
    bool owned = false;
    JSPromiseStateEnum state = JS_PromiseState(ctx, value);
    if (state == JS_PROMISE_FULFILLED || state == JS_PROMISE_REJECTED) {
        settled = JS_PromiseResult(ctx, value);
        owned = true;
        if (state == JS_PROMISE_REJECTED) *why = "the host rejected";
    }
    const char *s = JS_ToCString(ctx, settled);
    char *out = s ? strdup(s) : NULL;
    if (s) JS_FreeCString(ctx, s);
    if (owned) JS_FreeValue(ctx, settled);
    return out;
}

/* A `__hostcall(name, argsJson)` from inside the sandbox, answered in JS. */
static char *dispatch(const char *name, const char *args_json, void *ud) {
    long id = (long)ud;
    JSValue global = JS_GetGlobalObject(g_ctx);
    JSValue fn = JS_GetPropertyStr(g_ctx, global, "__lam_dispatch");
    JS_FreeValue(g_ctx, global);

    JSValue argv[3] = { JS_NewInt32(g_ctx, (int)id), JS_NewString(g_ctx, name),
                        JS_NewString(g_ctx, args_json) };
    JSValue answer = JS_Call(g_ctx, fn, JS_UNDEFINED, 3, argv);
    for (int i = 0; i < 3; i++) JS_FreeValue(g_ctx, argv[i]);
    JS_FreeValue(g_ctx, fn);

    if (JS_IsException(answer)) {
        JSValue e = JS_GetException(g_ctx);
        const char *m = JS_ToCString(g_ctx, e);
        fprintf(stderr, "mdy-native: host call %s threw: %s\n", name, m ? m : "?");
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
        JS_FreeValue(g_ctx, answer);
        return strdup("null");
    }
    const char *why = NULL;
    char *out = settle_to_string(g_ctx, answer, &why);
    JS_FreeValue(g_ctx, answer);
    if (!out || why) {
        fprintf(stderr, "mdy-native: host call %s: %s\n", name, why ? why : "no answer");
        free(out);
        return strdup("null");
    }
    return out;
}

/* ---- the natives the shims call ------------------------------------------ */

static JSValue js_lam_eval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    const char *program = JS_ToCString(ctx, argv[1]);
    if (!program) return JS_UNDEFINED;

    char *err = NULL;
    char *out = lam_eval(program, dispatch, (void *)(long)id, &err);
    JS_FreeCString(ctx, program);

    if (err) { JSValue e = JS_ThrowInternalError(ctx, "%s", err); free(err); free(out); return e; }
    JSValue r = JS_NewString(ctx, out ? out : "");
    free(out);
    return r;
}

static JSValue js_nis_open(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    return JS_NewInt32(ctx, nis_open());
}

static JSValue js_nis_insert(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_NewInt32(ctx, -1);
    int32_t handle = 0;
    JS_ToInt32(ctx, &handle, argv[0]);
    size_t len = 0;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &len, argv[1]);
    if (!bytes) return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, nis_insert(handle, bytes, (uint32_t)len));
}

static JSValue js_nis_find(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_NULL;
    int32_t handle = 0;
    JS_ToInt32(ctx, &handle, argv[0]);
    size_t flen = 0;
    uint8_t *filter = JS_GetArrayBuffer(ctx, &flen, argv[1]);
    if (!filter) return JS_NULL;

    uint8_t *out = NULL;
    size_t out_len = 0;
    if (nis_find(handle, filter, (uint32_t)flen, &out, &out_len) < 0 || !out) return JS_NULL;
    JSValue buf = JS_NewArrayBufferCopy(ctx, out, out_len);
    free(out);
    return buf;
}

/* Neither `print` nor `console` is part of QuickJS the library — they come
 * from quickjs-libc, which a host that supplies its own natives does not link.
 * mdy-docs writes to console in a few places, so both exist and both go to
 * stdout. */
static JSValue js_print(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        printf("%s%s", i ? " " : "", s ? s : "");
        if (s) JS_FreeCString(ctx, s);
    }
    printf("\n");
    fflush(stdout);
    return JS_UNDEFINED;
}

/* ---- the program --------------------------------------------------------- */

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    const char *bundle_path = argc > 1 ? argv[1] : "build/mdy.js";

    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    g_ctx = ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__lam_eval", JS_NewCFunction(ctx, js_lam_eval, "__lam_eval", 2));
    JS_SetPropertyStr(ctx, global, "__nis_open", JS_NewCFunction(ctx, js_nis_open, "__nis_open", 0));
    JS_SetPropertyStr(ctx, global, "__nis_insert", JS_NewCFunction(ctx, js_nis_insert, "__nis_insert", 2));
    JS_SetPropertyStr(ctx, global, "__nis_find", JS_NewCFunction(ctx, js_nis_find, "__nis_find", 2));

    JSValue print_fn = JS_NewCFunction(ctx, js_print, "print", 1);
    JS_SetPropertyStr(ctx, global, "print", JS_DupValue(ctx, print_fn));
    JSValue console = JS_NewObject(ctx);
    for (const char *const *m = (const char *const[]){ "log", "warn", "error", "info", "debug", NULL }; *m; m++)
        JS_SetPropertyStr(ctx, console, *m, JS_DupValue(ctx, print_fn));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, print_fn);
    JS_FreeValue(ctx, global);

    size_t len = 0;
    char *bundle = read_file(bundle_path, &len);
    if (!bundle) { fprintf(stderr, "mdy-native: cannot read %s\n", bundle_path); return 1; }

    JSValue r = JS_Eval(ctx, bundle, len, bundle_path, JS_EVAL_TYPE_MODULE);
    free(bundle);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        fprintf(stderr, "mdy-native: %s\n", m ? m : "bundle failed");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        return 1;
    }
    /*
     * A module evaluates to a promise, and a top-level await leaves work
     * queued behind it. Drain, then LOOK AT IT: an unhandled module rejection
     * is otherwise a silent exit 0, which is the least useful failure a host
     * can have.
     */
    for (;;) {
        JSContext *which = NULL;
        int ran = JS_ExecutePendingJob(rt, &which);
        if (ran < 0) {
            JSValue e = JS_GetException(ctx);
            const char *m = JS_ToCString(ctx, e);
            fprintf(stderr, "mdy-native: %s\n", m ? m : "a job threw");
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, e);
            JS_FreeValue(ctx, r);
            return 1;
        }
        if (ran == 0) break;
    }
    int status = 0;
    if (JS_IsObject(r) && JS_PromiseState(ctx, r) == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(ctx, r);
        const char *m = JS_ToCString(ctx, reason);
        JSValue stack = JS_GetPropertyStr(ctx, reason, "stack");
        const char *st = JS_ToCString(ctx, stack);
        fprintf(stderr, "mdy-native: %s\n%s", m ? m : "the bundle rejected", st ? st : "");
        if (m) JS_FreeCString(ctx, m);
        if (st) JS_FreeCString(ctx, st);
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, reason);
        status = 1;
    } else if (JS_IsObject(r) && JS_PromiseState(ctx, r) == JS_PROMISE_PENDING) {
        fprintf(stderr, "mdy-native: the bundle never finished — nothing left to run\n");
        status = 1;
    }
    JS_FreeValue(ctx, r);

    /* The bundle's own verdict, if it has one. */
    if (status == 0) {
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue v = JS_GetPropertyStr(ctx, g, "__exit_status");
        int32_t code = 0;
        if (!JS_IsUndefined(v) && JS_ToInt32(ctx, &code, v) == 0) status = code;
        JS_FreeValue(ctx, v);
        JS_FreeValue(ctx, g);
    }

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return status;
}
