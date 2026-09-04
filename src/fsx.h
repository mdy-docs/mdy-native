/*
 * The filesystem, in plain C.
 *
 * Same discipline as lam.h and nis.h: nothing from any engine's headers
 * crosses this line. `fsx` rather than `fs` because a header called fs.h in an
 * include path that already carries three C projects is asking for the wrong
 * one to be found.
 *
 * mdy-docs takes its filesystem as an argument — `renderSite(root, { fs })` —
 * and ships two providers of its own (node:fs, and an in-memory Map). This is
 * what the third is built on: ../shims/fs.js implements the nine-method
 * contract in ../../../src/fs-provider.js over these five calls.
 *
 * Paths are `/`-separated relative paths under a root, which is what the
 * provider contract speaks, and which POSIX takes verbatim.
 */
#ifndef MDY_FSX_H
#define MDY_FSX_H

#include <stddef.h>
#include <stdint.h>

/*
 * Every file under `root/subdir`, recursively, sorted, one per line, NUL
 * terminated. `exts` is a comma-separated suffix list (".mdy,.md") or NULL for
 * every file whatever its extension — the same distinction
 * `options.extensions: null` makes on the JS side.
 *
 * A missing directory is an empty list, not an error: the contract says so,
 * and a site that imports a package with no `static/` depends on it.
 * Returns NULL only on allocation failure. Caller frees.
 */
char *fsx_list(const char *root, const char *subdir, const char *exts);

/* A file's bytes. Caller frees. NULL if it cannot be read; *len is set on
 * success. Text and binary are the same call — the decoding is the caller's,
 * which is why `read` and `readBinary` differ only in the shim. */
uint8_t *fsx_read(const char *root, const char *rel, size_t *len);

/* Size in bytes and mtime in epoch milliseconds. 0 on success, -1 if absent. */
int fsx_stat(const char *root, const char *rel, double *size, double *mtime_ms);

/* Write, creating parent directories as needed. 0 on success. */
int fsx_write(const char *root, const char *rel, const uint8_t *bytes, size_t len);

/* Remove. A missing path is NOT an error — the contract says so. */
int fsx_remove(const char *root, const char *rel);

/* The working directory. A site root reaches the host as an argv string and
 * may be relative, but mdy-docs' import graph keys modules by absolute path —
 * so something has to make it absolute, and only the host knows where it is. */
char *fsx_cwd(void);

/*
 * ---- what the ported test suite needs -----------------------------------
 *
 * mdy-docs' own tests do real filesystem work — they write a site into a temp
 * directory and build it — so running them natively needs more than the
 * provider contract does. These back the `node:fs` shims in ../shims/node/,
 * and nothing in the backend proper calls them.
 */

/* One directory level, one entry per line, NUL terminated; a directory's name
 * carries a trailing `/` so a caller can tell them apart without a second
 * call. Missing directory → NULL (distinct from an empty one, which is ""),
 * because `readdir` on a missing path IS an error where `list` is not. */
char *fsx_readdir(const char *path);

/* mkdir -p. 0 on success, and an existing directory is success. */
int fsx_mkdirp(const char *path);

/* Remove a file, or a directory and everything under it. A missing path is
 * not an error — this is `rm -rf`, which is what the tests reach for. */
int fsx_rm_rf(const char *path);

/* Create a uniquely named directory whose path starts with `prefix`, as
 * mkdtemp does, and return it. Caller frees. NULL on failure. */
char *fsx_mkdtemp(const char *prefix);

/* The system temp directory, without a trailing separator. Caller frees. */
char *fsx_tmpdir(void);

#endif
