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
#include "fsx.h"
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
static char *dispatch(const char *name, const char *args_json, void *ud, char **err) {
    long id = (long)ud;
    *err = NULL;
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
        *err = strdup(m ? m : "the host threw");
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
        JS_FreeValue(g_ctx, answer);
        return NULL;
    }
    /* On a rejection settle_to_string still hands back the reason's text,
     * which is the whole diagnostic — "the host rejected" alone says only that
     * something went wrong somewhere. */
    const char *why = NULL;
    char *out = settle_to_string(g_ctx, answer, &why);
    JS_FreeValue(g_ctx, answer);
    if (!out || why) {
        char buf[640];
        snprintf(buf, sizeof buf, "%s%s%s", why ? why : "no answer",
                 out ? ": " : "", out ? out : "");
        *err = strdup(buf);
        free(out);
        return NULL;
    }
    return out;
}

/* ---- the natives the shims call ------------------------------------------ */

/*
 * A guest `import`, answered in JavaScript. Same shape as `dispatch` above and
 * for the same reason — the answer may be a promise, and the job queue is
 * pumped until it settles — but the payload is raw source rather than JSON:
 * see lam.h.
 */
static char *module(lam_module_op op, const char *specifier, const char *referrer,
                    void *ud, char **err) {
    long id = (long)ud;
    *err = NULL;

    JSValue global = JS_GetGlobalObject(g_ctx);
    JSValue fn = JS_GetPropertyStr(g_ctx, global, "__lam_module");
    JS_FreeValue(g_ctx, global);

    JSValue argv[4] = { JS_NewInt32(g_ctx, (int)id), JS_NewInt32(g_ctx, (int)op),
                        JS_NewString(g_ctx, specifier), JS_NewString(g_ctx, referrer) };
    JSValue answer = JS_Call(g_ctx, fn, JS_UNDEFINED, 4, argv);
    for (int i = 0; i < 4; i++) JS_FreeValue(g_ctx, argv[i]);
    JS_FreeValue(g_ctx, fn);

    if (JS_IsException(answer)) {
        JSValue e = JS_GetException(g_ctx);
        const char *m = JS_ToCString(g_ctx, e);
        *err = strdup(m ? m : "the loader threw");
        if (m) JS_FreeCString(g_ctx, m);
        JS_FreeValue(g_ctx, e);
        JS_FreeValue(g_ctx, answer);
        return NULL;
    }

    const char *why = NULL;
    char *out = settle_to_string(g_ctx, answer, &why);
    JS_FreeValue(g_ctx, answer);
    if (!out || why) {
        /* On a rejection settle_to_string still hands back the reason's text,
         * which is the useful half of the message. */
        char buf[512];
        snprintf(buf, sizeof buf, "%s%s%s", why ? why : "no answer",
                 out ? ": " : "", out ? out : "");
        *err = strdup(buf);
        free(out);
        return NULL;
    }
    return out;
}

/*
 * The sandbox instances, one per createLamassu() on the JS side, indexed by
 * the id the shim mints. Held here rather than in a JSValue because they are C
 * objects with no JS identity, and because ../shims/lamassu.js only ever needs
 * to name one.
 */
#define MAX_VMS 64
static LamVm *g_vms[MAX_VMS];

static JSValue js_lam_vm_new(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    for (int i = 0; i < MAX_VMS; i++) {
        if (g_vms[i]) continue;
        g_vms[i] = lam_vm_new();
        return JS_NewInt32(ctx, g_vms[i] ? i : -1);
    }
    return JS_NewInt32(ctx, -1);
}

static JSValue js_lam_vm_free(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    int32_t id = -1;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id >= 0 && id < MAX_VMS) { lam_vm_free(g_vms[id]); g_vms[id] = NULL; }
    return JS_UNDEFINED;
}

static JSValue js_lam_eval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id < 0 || id >= MAX_VMS || !g_vms[id])
        return JS_ThrowInternalError(ctx, "lamassu: no such vm (%d)", (int)id);
    const char *program = JS_ToCString(ctx, argv[1]);
    if (!program) return JS_UNDEFINED;

    /* A loader is installed only when this eval has one: without it a guest
     * `import` fails at the import, which is the honest outcome, rather than
     * resolving to nothing. */
    int has_loader = argc > 2 && JS_ToBool(ctx, argv[2]) > 0;

    char *err = NULL;
    char *out = lam_eval(g_vms[id], program, dispatch, has_loader ? module : NULL,
                         (void *)(long)id, &err);
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

static JSValue js_nis_index(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 5) return JS_NewInt32(ctx, -1);
    int32_t handle = 0;
    JS_ToInt32(ctx, &handle, argv[0]);
    const char *name = JS_ToCString(ctx, argv[1]);
    size_t flen = 0;
    uint8_t *fields = JS_GetArrayBuffer(ctx, &flen, argv[2]);
    int rc = -1;
    if (name && fields) {
        rc = nis_create_index(handle, name, fields, (uint32_t)flen,
                              JS_ToBool(ctx, argv[3]) > 0, JS_ToBool(ctx, argv[4]) > 0);
    }
    if (name) JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, rc);
}

/* ---- the filesystem ------------------------------------------------------- */

/*
 * Five natives, and shims/fs.js builds the nine-method provider contract on
 * them. A listing crosses as ONE newline-separated string rather than an array
 * of strings: the corpus has thousands of files, and building that many
 * JSValues to immediately join them is work neither side needs.
 */
static JSValue js_fs_list(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_NULL;
    const char *root = JS_ToCString(ctx, argv[0]);
    const char *subdir = JS_ToCString(ctx, argv[1]);
    const char *exts = (argc > 2 && JS_IsString(argv[2])) ? JS_ToCString(ctx, argv[2]) : NULL;
    JSValue out = JS_NULL;
    if (root && subdir) {
        char *listing = fsx_list(root, subdir, exts);
        if (listing) { out = JS_NewString(ctx, listing); free(listing); }
    }
    if (root) JS_FreeCString(ctx, root);
    if (subdir) JS_FreeCString(ctx, subdir);
    if (exts) JS_FreeCString(ctx, exts);
    return out;
}

static JSValue js_fs_read(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_NULL;
    const char *root = JS_ToCString(ctx, argv[0]);
    const char *rel = JS_ToCString(ctx, argv[1]);
    JSValue out = JS_NULL;
    if (root && rel) {
        size_t len = 0;
        uint8_t *bytes = fsx_read(root, rel, &len);
        if (bytes) { out = JS_NewArrayBufferCopy(ctx, bytes, len); free(bytes); }
    }
    if (root) JS_FreeCString(ctx, root);
    if (rel) JS_FreeCString(ctx, rel);
    return out;
}

/* [size, mtimeMillis], or null when the path is not there. Both at once
 * because `size` and `mtime` are one stat, and a build asks for both. */
static JSValue js_fs_stat(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_NULL;
    const char *root = JS_ToCString(ctx, argv[0]);
    const char *rel = JS_ToCString(ctx, argv[1]);
    JSValue out = JS_NULL;
    double size = 0, mtime = 0;
    if (root && rel && fsx_stat(root, rel, &size, &mtime) == 0) {
        out = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, out, 0, JS_NewFloat64(ctx, size));
        JS_SetPropertyUint32(ctx, out, 1, JS_NewFloat64(ctx, mtime));
    }
    if (root) JS_FreeCString(ctx, root);
    if (rel) JS_FreeCString(ctx, rel);
    return out;
}

static JSValue js_fs_write(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 3) return JS_NewInt32(ctx, -1);
    const char *root = JS_ToCString(ctx, argv[0]);
    const char *rel = JS_ToCString(ctx, argv[1]);
    size_t len = 0;
    uint8_t *bytes = JS_GetArrayBuffer(ctx, &len, argv[2]);
    int rc = (root && rel && bytes) ? fsx_write(root, rel, bytes, len) : -1;
    if (root) JS_FreeCString(ctx, root);
    if (rel) JS_FreeCString(ctx, rel);
    return JS_NewInt32(ctx, rc);
}

static JSValue js_fs_remove(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 2) return JS_NewInt32(ctx, -1);
    const char *root = JS_ToCString(ctx, argv[0]);
    const char *rel = JS_ToCString(ctx, argv[1]);
    int rc = (root && rel) ? fsx_remove(root, rel) : -1;
    if (root) JS_FreeCString(ctx, root);
    if (rel) JS_FreeCString(ctx, rel);
    return JS_NewInt32(ctx, rc);
}

static JSValue js_fs_cwd(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *cwd = fsx_cwd();
    JSValue out = cwd ? JS_NewString(ctx, cwd) : JS_NULL;
    free(cwd);
    return out;
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
    JS_SetPropertyStr(ctx, global, "__lam_vm_new", JS_NewCFunction(ctx, js_lam_vm_new, "__lam_vm_new", 0));
    JS_SetPropertyStr(ctx, global, "__lam_vm_free", JS_NewCFunction(ctx, js_lam_vm_free, "__lam_vm_free", 1));
    JS_SetPropertyStr(ctx, global, "__lam_eval", JS_NewCFunction(ctx, js_lam_eval, "__lam_eval", 3));
    JS_SetPropertyStr(ctx, global, "__nis_open", JS_NewCFunction(ctx, js_nis_open, "__nis_open", 0));
    JS_SetPropertyStr(ctx, global, "__nis_insert", JS_NewCFunction(ctx, js_nis_insert, "__nis_insert", 2));
    JS_SetPropertyStr(ctx, global, "__nis_find", JS_NewCFunction(ctx, js_nis_find, "__nis_find", 2));
    JS_SetPropertyStr(ctx, global, "__nis_index", JS_NewCFunction(ctx, js_nis_index, "__nis_index", 5));
    JS_SetPropertyStr(ctx, global, "__fs_list", JS_NewCFunction(ctx, js_fs_list, "__fs_list", 3));
    JS_SetPropertyStr(ctx, global, "__fs_read", JS_NewCFunction(ctx, js_fs_read, "__fs_read", 2));
    JS_SetPropertyStr(ctx, global, "__fs_stat", JS_NewCFunction(ctx, js_fs_stat, "__fs_stat", 2));
    JS_SetPropertyStr(ctx, global, "__fs_write", JS_NewCFunction(ctx, js_fs_write, "__fs_write", 3));
    JS_SetPropertyStr(ctx, global, "__fs_remove", JS_NewCFunction(ctx, js_fs_remove, "__fs_remove", 2));
    JS_SetPropertyStr(ctx, global, "__fs_cwd", JS_NewCFunction(ctx, js_fs_cwd, "__fs_cwd", 0));

    /* Everything after the bundle path, so an entry can be told which site to
     * build without a second channel. */
    JSValue args = JS_NewArray(ctx);
    for (int i = 2, n = 0; i < argc; i++, n++)
        JS_SetPropertyUint32(ctx, args, (uint32_t)n, JS_NewString(ctx, argv[i]));
    JS_SetPropertyStr(ctx, global, "__argv", args);

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
