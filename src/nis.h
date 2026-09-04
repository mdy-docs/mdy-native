/*
 * The nisaba seam. Same discipline as lam.h: nothing from either engine's
 * headers crosses, only plain C.
 *
 * Documents and filters cross as binjson bytes, which is what nisaba's C API
 * speaks. The encoding is done in JavaScript by the codec that already ships
 * in the submodule — a native host has no JSON parser to build one from, and
 * the JS codec is the reference implementation.
 */
#ifndef MDY_NIS_H
#define MDY_NIS_H

#include <stddef.h>
#include <stdint.h>

/* Open a collection backed by a fresh temp file. Returns a handle >= 0, or -1.
 * `MemoryStorageProvider` on the JS side becomes this: nisaba's on-disk
 * B+tree is its format, and a native host has a filesystem. */
int nis_open(void);

/* Insert one binjson-encoded document. 0 on success. */
int nis_insert(int handle, const uint8_t *doc, uint32_t len);

/* Every document matching a binjson-encoded filter, as a binjson ARRAY.
 * Caller frees *out. 0 on success. */
int nis_find(int handle, const uint8_t *filter, uint32_t filter_len, uint8_t **out, size_t *out_len);

void nis_close(int handle);

#endif
