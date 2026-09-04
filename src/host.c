/*
 * The QuickJS side, and the program. Includes quickjs.h and nothing from
 * lamassu — see lam.h for why they cannot meet in one file.
 *
 * THE ASYNC PROBLEM, which is the reason this file is interesting.
 *
 * A document calls `$.find(q)` synchronously — that is the language contract,
 * and every template depends on it. But mdy-docs implements those natives in
 * JavaScript and several are async: a query awaits the database, a nested
 * `$.render` awaits another render. The WASM build hides that with Asyncify:
 * the guest's call suspends mid-instruction while the JS host does its work.
 *
 * Natively there is no Asyncify, and the three ways out are not equal.
 * Rewriting mdy-docs' natives to be synchronous would diverge from the Node
 * path for the whole life of the project. Making the guest `await` would break
 * the contract that `$.find(q)` returns documents rather than a promise. So
 * this takes the third: the native calls into QuickJS, and if the answer is a
 * promise it PUMPS QUICKJS'S JOB QUEUE until that promise settles, then hands
 * the value back synchronously. The guest never learns it waited, and
 * mdy-docs' JavaScript is untouched.
 *
 * The re-entrancy that looks alarming turns out to be fine, and is checked
 * below. A nested `$.render` means: lamassu native -> QuickJS -> mdy-docs ->
 * lamassu again. The inner call gets its OWN JsVm, exactly as src/vm.js gives
 * each nesting level its own pooled instance, so nothing re-enters a suspended
 * VM.
 *
 * What this cannot do is wait for something only an outer event loop would
 * deliver — a socket, a timer. Pumping finds no job, the promise stays
 * pending, and rather than hang, that is reported as the deadlock it is. In a
 * native backend the filesystem is synchronous C, so the case should not
 * arise; if it ever does, it will say so instead of stopping.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "lam.h"

typedef struct {
    JSContext *ctx;
    JSValue handler;
} Host;

/*
 * A JS result to a string, waiting for it first if it is a promise.
 * Returns NULL only if the promise cannot be settled by running jobs.
 */
static char *settle_to_string(JSContext *ctx, JSValue value, const char **why) {
    *why = NULL;
    JSRuntime *rt = JS_GetRuntime(ctx);

    while (JS_PromiseState(ctx, value) == JS_PROMISE_PENDING) {
        JSContext *which = NULL;
        int ran = JS_ExecutePendingJob(rt, &which);
        if (ran < 0) { *why = "a queued job threw"; return NULL; }
        if (ran == 0) {
            /* Nothing left to run and still pending: this is waiting on
             * something the host cannot deliver by running jobs. Say so. */
            *why = "promise never settled — nothing left to run";
            return NULL;
        }
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

/* lamassu asked; answer it in JavaScript, waiting if the answer is a promise. */
static char *ask_quickjs(const char *question, void *ud) {
    Host *h = ud;
    JSValue arg = JS_NewString(h->ctx, question);
    JSValue answer = JS_Call(h->ctx, h->handler, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(h->ctx, arg);
    if (JS_IsException(answer)) {
        JS_FreeValue(h->ctx, answer);
        return strdup("(host threw)");
    }
    const char *why = NULL;
    char *out = settle_to_string(h->ctx, answer, &why);
    JS_FreeValue(h->ctx, answer);
    if (!out) { char buf[160]; snprintf(buf, sizeof buf, "(%s)", why ? why : "no answer"); return strdup(buf); }
    if (why) { char buf[256]; snprintf(buf, sizeof buf, "(%s: %s)", why, out); free(out); return strdup(buf); }
    return out;
}

/*
 * A native the HOST's JavaScript can call to run another lamassu program.
 * This stands in for a nested `$.render`, and exists to prove the re-entrant
 * path: sandbox -> host -> sandbox, with the inner run on its own VM.
 */
static JSValue js_run_sandbox(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *src = JS_ToCString(ctx, argv[0]);
    if (!src) return JS_UNDEFINED;
    char *err = NULL;
    char *out = lam_eval(src, NULL, NULL, &err);
    JSValue r = JS_NewString(ctx, err ? err : (out ? out : ""));
    free(out); free(err); JS_FreeCString(ctx, src);
    return r;
}

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "runSandbox",
                      JS_NewCFunction(ctx, js_run_sandbox, "runSandbox", 1));

    /*
     * The host half, in ordinary JavaScript — a stand-in for mdy-docs' `$`
     * natives. One synchronous, one async, and one async that re-enters the
     * sandbox the way a nested $.render does.
     */
    static const char *HOST_JS =
        "const sleep = (ms) => new Promise((r) => Promise.resolve().then(r));\n"
        "globalThis.answer = async (q) => {\n"
        "  if (q === 'sync') return 'answered synchronously';\n"
        "  if (q === 'unicode') return 'Ašared — Uruk’s scribes, ‰, 𒀭';\n"
        "  if (q === 'async') { await sleep(0); await sleep(0); return 'answered after awaiting'; }\n"
        "  if (q === 'nested') { await sleep(0); return 'inner sandbox said: ' + runSandbox(\"'42 * 2 = ' + (42 * 2)\"); }\n"
        "  if (q === 'stuck') return new Promise(() => {});\n"
        "  return 'no answer for: ' + q;\n"
        "};\n";
    JSValue r = JS_Eval(ctx, HOST_JS, strlen(HOST_JS), "<host>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) { fprintf(stderr, "host js failed\n"); return 1; }
    JS_FreeValue(ctx, r);

    Host host = { .ctx = ctx, .handler = JS_GetPropertyStr(ctx, global, "answer") };
    JS_FreeValue(ctx, global);

    printf("--- mdy-native: async host calls ---\n");
    struct { const char *label, *program; } cases[] = {
        { "sync native      ", "'-> ' + ask('sync')" },
        { "async native     ", "'-> ' + ask('async')" },
        { "re-entrant render", "'-> ' + ask('nested')" },
        { "never settles    ", "'-> ' + ask('stuck')" },
        { "unicode round trip", "'-> ' + ask('unicode')" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char *err = NULL;
        char *out = lam_eval(cases[i].program, ask_quickjs, &host, &err);
        printf("  %s: %s\n", cases[i].label, err ? err : out);
        free(out); free(err);
    }

    JS_FreeValue(ctx, host.handler);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
