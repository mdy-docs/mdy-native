/*
 * The QuickJS side, and the program. Includes quickjs.h and nothing from
 * lamassu — see lam.h for why they cannot meet in one file.
 *
 * This is the shape mdy-native will keep: mdy-docs' own JavaScript runs here,
 * and the sandbox calls out to it. Today the "JavaScript" is three lines and
 * the call is `ask`; the real version replaces those with $.find, $.render and
 * the rest, unchanged, because that is the point of the design.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "lam.h"

typedef struct {
    JSContext *ctx;
    JSValue handler;
} Host;

/* lamassu asked; answer it in JavaScript. */
static char *ask_quickjs(const char *question, void *ud) {
    Host *h = ud;
    JSValue arg = JS_NewString(h->ctx, question);
    JSValue answer = JS_Call(h->ctx, h->handler, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(h->ctx, arg);
    if (JS_IsException(answer)) {
        JS_FreeValue(h->ctx, answer);
        return strdup("(host threw)");
    }
    const char *s = JS_ToCString(h->ctx, answer);
    char *out = s ? strdup(s) : NULL;
    if (s) JS_FreeCString(h->ctx, s);
    JS_FreeValue(h->ctx, answer);
    return out;
}

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);

    static const char *HOST_JS =
        "globalThis.answer = (q) => q === 'who serves this?'\n"
        "  ? 'quickjs, from inside the sandbox'\n"
        "  : 'no answer for: ' + q;\n";
    JSValue r = JS_Eval(ctx, HOST_JS, strlen(HOST_JS), "<host>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) { fprintf(stderr, "host js failed\n"); return 1; }
    JS_FreeValue(ctx, r);

    JSValue global = JS_GetGlobalObject(ctx);
    Host host = { .ctx = ctx, .handler = JS_GetPropertyStr(ctx, global, "answer") };
    JS_FreeValue(ctx, global);

    printf("--- mdy-native bridge ---\n");

    char *err = NULL;
    char *plain = lam_eval("'lamassu ran: ' + (6 * 7)", NULL, NULL, &err);
    printf("  lamassu alone  : %s\n", err ? err : plain);
    free(plain); free(err);

    char *bridged = lam_eval("'sandbox asked, host said: ' + ask('who serves this?')",
                             ask_quickjs, &host, &err);
    printf("  lamassu -> qjs : %s\n", err ? err : bridged);
    free(bridged); free(err);

    JS_FreeValue(ctx, host.handler);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return 0;
}
