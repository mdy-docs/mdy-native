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
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#  include <windows.h>
#endif
#include <stdlib.h>
#include <string.h>

#include "quickjs.h"
#include "fsx.h"

/* The MDY front end in C — see src/parse.c. */
JSValue mdy_native_parse(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

#include "lam.h"
#include "nis.h"

static JSContext *g_ctx;

/* ---- timers --------------------------------------------------------------- */

/*
 * setTimeout, which QuickJS does not have on its own — timers live in
 * quickjs-libc, and this host deliberately does not link it (see the Makefile:
 * that is the std/os module layer, and leaving it out leaves out its POSIX
 * assumptions).
 *
 * Two things need them. mdy-docs' test suite has a polling watcher and an
 * async-native test that both wait on one, and more generally a JavaScript
 * runtime with no setTimeout will surprise anything that assumes the language
 * comes with an event loop. So the pump below drains timers as well as jobs:
 * when no job is ready and a timer is pending, it waits for the earliest and
 * fires it. That is the whole event loop, and it is enough because nothing
 * here has I/O to wait on — the filesystem is synchronous C.
 */
typedef struct Timer {
    int64_t id;
    double due_ms;
    JSValue fn;
    struct Timer *next;
} Timer;

static Timer *g_timers;
static int64_t g_next_timer_id = 1;

static double now_ms(void) {
#ifdef _WIN32
    return (double)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
}

static void sleep_ms(double ms) {
    if (ms <= 0) return;
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { (time_t)(ms / 1000.0), (long)((ms - (long)(ms / 1000.0) * 1000.0) * 1000000.0) };
    nanosleep(&ts, NULL);
#endif
}

static JSValue js_set_timeout(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt64(ctx, 0);
    double delay = 0;
    if (argc > 1) JS_ToFloat64(ctx, &delay, argv[1]);

    Timer *timer = malloc(sizeof *timer);
    if (!timer) return JS_NewInt64(ctx, 0);
    timer->id = g_next_timer_id++;
    timer->due_ms = now_ms() + (delay > 0 ? delay : 0);
    timer->fn = JS_DupValue(ctx, argv[0]);
    timer->next = g_timers;
    g_timers = timer;
    return JS_NewInt64(ctx, timer->id);
}

static JSValue js_clear_timeout(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_UNDEFINED;
    int64_t id = 0;
    JS_ToInt64(ctx, &id, argv[0]);
    for (Timer **p = &g_timers; *p; p = &(*p)->next) {
        if ((*p)->id != id) continue;
        Timer *dead = *p;
        *p = dead->next;
        JS_FreeValue(ctx, dead->fn);
        free(dead);
        break;
    }
    return JS_UNDEFINED;
}

/* The earliest pending timer's deadline, or -1 when there are none. */
static double earliest_timer(void) {
    double best = -1;
    for (Timer *t = g_timers; t; t = t->next)
        if (best < 0 || t->due_ms < best) best = t->due_ms;
    return best;
}

/* Fire every timer already due. Returns how many ran. */
static int fire_due_timers(JSContext *ctx) {
    int fired = 0;
    for (;;) {
        double now = now_ms();
        Timer **found = NULL;
        for (Timer **p = &g_timers; *p; p = &(*p)->next) {
            if ((*p)->due_ms > now) continue;
            /* Earliest first, so ordering matches what a caller expects. */
            if (!found || (*p)->due_ms < (*found)->due_ms) found = p;
        }
        if (!found) return fired;

        Timer *timer = *found;
        *found = timer->next;
        JSValue r = JS_Call(ctx, timer->fn, JS_UNDEFINED, 0, NULL);
        if (JS_IsException(r)) {
            JSValue e = JS_GetException(ctx);
            const char *m = JS_ToCString(ctx, e);
            fprintf(stderr, "mdy-native: a timer threw: %s\n", m ? m : "?");
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, r);
        JS_FreeValue(ctx, timer->fn);
        free(timer);
        fired++;
    }
}

/**
 * One turn of the loop: run a queued job if there is one, otherwise fire a due
 * timer, otherwise wait for the earliest timer and fire that. Returns 1 if
 * anything ran, 0 if there is nothing left to do, -1 if a job threw.
 */
static int pump(JSRuntime *rt, JSContext *ctx) {
    JSContext *which = NULL;
    int ran = JS_ExecutePendingJob(rt, &which);
    if (ran != 0) return ran;
    if (fire_due_timers(ctx) > 0) return 1;
    double due = earliest_timer();
    if (due < 0) return 0;

    /*
     * A timer exists, so progress is guaranteed even if this particular wait
     * does not reach its deadline — report it as progress and let the caller
     * come round again.
     *
     * Returning 0 when nothing fired was wrong and Windows found it: its clock
     * has ~15.6 ms granularity and Sleep can return early, so a 10 ms timer
     * would wake with now_ms() still reading the same tick, fire nothing, and
     * be reported as "promise never settled — nothing left to run".
     */
    sleep_ms(due - now_ms());
    fire_due_timers(ctx);
    return 1;
}

/* ---- waiting for JavaScript ---------------------------------------------- */

static char *settle_to_string(JSContext *ctx, JSValue value, const char **why) {
    *why = NULL;
    JSRuntime *rt = JS_GetRuntime(ctx);

    while (JS_PromiseState(ctx, value) == JS_PROMISE_PENDING) {
        int ran = pump(rt, ctx);
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
    /*
     * A rejection propagates its reason VERBATIM, with no wrapper of our own.
     * The WASM binding rethrows the error the native threw, and mdy-docs is
     * written for that: its generated program catches and reports
     * "document N failed: <reason>". Adding "the host rejected: " here looked
     * harmless until a cyclic $.render — every level of the recursion added
     * another copy, and the depth guard's message arrived buried under a
     * dozen of them.
     */
    const char *why = NULL;
    char *out = settle_to_string(g_ctx, answer, &why);
    JS_FreeValue(g_ctx, answer);
    if (!out || why) {
        *err = out ? out : strdup(why ? why : "the host gave no answer");
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

/*
 * A COLLECTION HAS A LIFETIME, and this is where it gets one.
 *
 * nisaba's JS binding is garbage-collected: a collection nobody references is
 * reclaimed, and with it the memory or the file handle behind it. Handing back
 * a bare integer gave up that property, and it showed the moment mdy-docs' own
 * test suite ran here — a document set per test, hundreds of them, every
 * handle held forever, and eventually no descriptors left.
 *
 * So the handle rides inside a JS object of its own class, and that class has
 * a finalizer. When the Collection in shims/nisaba.js becomes unreachable, so
 * does this, and QuickJS closes the collection. The same lifetime the WASM
 * binding gets, earned rather than assumed.
 */
static JSClassID js_nis_class_id;

static void js_nis_finalizer(JSRuntime *rt, JSValue val) {
    (void)rt;
    void *p = JS_GetOpaque(val, js_nis_class_id);
    if (p) nis_close((int)(intptr_t)p - 1);   /* +1 on the way in, so 0 is "none" */
}

static JSClassDef js_nis_class = {
    "NisabaCollection",
    .finalizer = js_nis_finalizer,
};

static JSValue js_nis_open(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    int handle = nis_open();
    if (handle < 0) return JS_NULL;

    JSValue obj = JS_NewObjectClass(ctx, (int)js_nis_class_id);
    if (JS_IsException(obj)) { nis_close(handle); return obj; }
    JS_SetOpaque(obj, (void *)(intptr_t)(handle + 1));
    /* The number is what every other native takes; the object is what keeps
     * the collection alive. The shim holds both. */
    JS_SetPropertyStr(ctx, obj, "handle", JS_NewInt32(ctx, handle));
    return obj;
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

/*
 * Five more, for the ported test suite only — mdy-docs' own tests write real
 * directories and build them, which the provider contract never has to do.
 * See fsx.h.
 */
static JSValue js_fs_readdir(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_NULL;
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NULL;
    char *listing = fsx_readdir(path);
    JS_FreeCString(ctx, path);
    if (!listing) return JS_NULL;   /* missing — an error for readdir */
    JSValue out = JS_NewString(ctx, listing);
    free(listing);
    return out;
}

static JSValue js_fs_mkdir(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_NewInt32(ctx, -1);
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NewInt32(ctx, -1);
    int rc = fsx_mkdirp(path);
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, rc);
}

static JSValue js_fs_rm(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_NewInt32(ctx, -1);
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_NewInt32(ctx, -1);
    int rc = fsx_rm_rf(path);
    JS_FreeCString(ctx, path);
    return JS_NewInt32(ctx, rc);
}

static JSValue js_fs_mkdtemp(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t;
    if (argc < 1) return JS_NULL;
    const char *prefix = JS_ToCString(ctx, argv[0]);
    if (!prefix) return JS_NULL;
    char *made = fsx_mkdtemp(prefix);
    JS_FreeCString(ctx, prefix);
    if (!made) return JS_NULL;
    JSValue out = JS_NewString(ctx, made);
    free(made);
    return out;
}

static JSValue js_fs_tmpdir(JSContext *ctx, JSValueConst t, int argc, JSValueConst *argv) {
    (void)t; (void)argc; (void)argv;
    char *dir = fsx_tmpdir();
    JSValue out = dir ? JS_NewString(ctx, dir) : JS_NULL;
    free(dir);
    return out;
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

    JS_NewClassID(&js_nis_class_id);
    JS_NewClass(rt, js_nis_class_id, &js_nis_class);

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
    JS_SetPropertyStr(ctx, global, "__mdy_parse",
                      JS_NewCFunction(ctx, mdy_native_parse, "__mdy_parse", 5));
    JS_SetPropertyStr(ctx, global, "__fs_cwd", JS_NewCFunction(ctx, js_fs_cwd, "__fs_cwd", 0));
    JS_SetPropertyStr(ctx, global, "__fs_readdir", JS_NewCFunction(ctx, js_fs_readdir, "__fs_readdir", 1));
    JS_SetPropertyStr(ctx, global, "__fs_mkdir", JS_NewCFunction(ctx, js_fs_mkdir, "__fs_mkdir", 1));
    JS_SetPropertyStr(ctx, global, "__fs_rm", JS_NewCFunction(ctx, js_fs_rm, "__fs_rm", 1));
    JS_SetPropertyStr(ctx, global, "__fs_mkdtemp", JS_NewCFunction(ctx, js_fs_mkdtemp, "__fs_mkdtemp", 1));
    JS_SetPropertyStr(ctx, global, "__fs_tmpdir", JS_NewCFunction(ctx, js_fs_tmpdir, "__fs_tmpdir", 0));

    /* Everything after the bundle path, so an entry can be told which site to
     * build without a second channel. */
    JSValue args = JS_NewArray(ctx);
    for (int i = 2, n = 0; i < argc; i++, n++)
        JS_SetPropertyUint32(ctx, args, (uint32_t)n, JS_NewString(ctx, argv[i]));
    JS_SetPropertyStr(ctx, global, "__argv", args);

    JS_SetPropertyStr(ctx, global, "setTimeout", JS_NewCFunction(ctx, js_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(ctx, global, "clearTimeout", JS_NewCFunction(ctx, js_clear_timeout, "clearTimeout", 1));
    /* setInterval is deliberately absent rather than faked: nothing here needs
     * one, and a repeating timer that never repeats is worse than a missing
     * function. */

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
        int ran = pump(rt, ctx);
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
