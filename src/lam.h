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
typedef char *(*lam_ask_fn)(const char *question, void *ud);

/* Evaluate one lamassu program with `ask` available to it.
 * Returns the completion value as UTF-8 (caller frees), or NULL with *err set
 * (caller frees). */
char *lam_eval(const char *source, lam_ask_fn ask, void *ud, char **err);

#endif
