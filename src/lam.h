/*
 * The neutral seam between lamassu and QuickJS.
 *
 * They cannot share a translation unit: both use the `JS_` prefix, and
 * lamassu's JS_TAG_STRING is a macro where QuickJS's is an enum member, so
 * including both turns the enum into `UINT64_C(0xFFFA000000000000) = -7`.
 * Discovering that is not a nuisance — it is the shape the host wanted
 * regardless. Each engine is wrapped in its own file, and only plain C crosses
 * between them.
 *
 * lam.c includes lamassu.h and nothing else. host.c includes quickjs.h and
 * nothing else. This header is the only thing both see, and it names no type
 * belonging to either.
 */
#ifndef MDY_LAM_H
#define MDY_LAM_H

#include <stddef.h>

/* What a sandboxed program's call OUT to the host looks like, in neutral
 * terms: a question in, an answer out, both UTF-8, the answer owned by the
 * caller. This is what every `$` native becomes — find, render, emit — once
 * the host side is mdy-docs' own JavaScript. */
/* A native that fails returns NULL with *err set (caller frees), and the
 * message is THROWN inside the sandbox at the call site — which is what the
 * WASM binding does, and what mdy-docs' generated program is written for: its
 * try/catch turns it into "document N failed: <message>". Swallowing it and
 * answering null instead turns one legible error into a cascade of unrelated
 * ones about null trees. */
typedef char *(*lam_ask_fn)(const char *name, const char *args_json, void *ud, char **err);

/*
 * Guest `import`, in the same neutral terms. Two operations rather than two
 * function pointers, because they always travel together and a host that
 * implements one implements both:
 *
 *   CANON  map (specifier, referrer) to the canonical specifier that becomes
 *          the module's registry identity. Synchronous by contract, and it
 *          must be deterministic — the engine resolves identity with it
 *          before dedupe, so an unstable answer loads the same file twice.
 *   LOAD   the canonical specifier's source.
 *
 * Answers are RAW UTF-8, not JSON — unlike lam_ask_fn, whose payload is a
 * host call's arguments and whose JSON the guest itself parses. A module's
 * source has no such wrapper to hide in, and a C host has no JSON parser to
 * take one off with.
 *
 * Returns NULL with *err set (caller frees) to fail the load; the reason
 * reaches the guest at the import.
 */
typedef enum { LAM_MODULE_CANON = 0, LAM_MODULE_LOAD = 1 } lam_module_op;
typedef char *(*lam_module_fn)(lam_module_op op, const char *specifier,
                               const char *referrer, void *ud, char **err);

/*
 * A sandbox instance, opaque here because its type belongs to lamassu.
 *
 * Instances are POOLED by the caller, not made per eval — ../../src/vm.js
 * already does this for the WASM binding, and for the same reason: creating
 * one allocates a heap and builds the global object, which measured at most of
 * a build's wall clock when every eval made its own. A suspended instance must
 * not be re-entered, so each nesting level of $.render holds its own; that is
 * vm.js's rule and this backend inherits it.
 */
typedef struct LamVm LamVm;

LamVm *lam_vm_new(void);
void lam_vm_free(LamVm *vm);

/* Evaluate one lamassu program with `__hostcall(name, argsJson)` available to
 * it — the same contract buildProgram already generates, so mdy-docs' own
 * programs run unchanged. `module` may be NULL, and then a guest `import` is
 * an error rather than a silent nothing. Returns the completion value as UTF-8
 * (caller frees), or NULL with *err set (caller frees). */
char *lam_eval(LamVm *vm, const char *source, lam_ask_fn ask, lam_module_fn module,
               void *ud, char **err);

#endif
