/*
 * Does nisaba work natively? Insert a document, query it back.
 *
 * All 16 of its C sources compile with cc and no changes — nothing about the
 * engine is WASM-specific; only the export shims (`*_wasm.c`) are, and those
 * are exactly what a native host replaces.
 *
 * One difference from the browser: storage is a file descriptor
 * (`bjio_host(fd)`), so what the JS side calls MemoryStorageProvider becomes a
 * temp file here. That is not a workaround — the on-disk B+tree IS nisaba's
 * format, and a native backend has a filesystem to put it on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "binjson.h"
#include "bjio.h"
#include "bplustree.h"
#include "db.h"

/*
 * Storage, natively.
 *
 * nisaba's shipped bj_io — `bjio_host(fd)` in hostio.h — is not POSIX: its
 * callbacks reach into `Module.bjioHandles[fd]`, a table of JS
 * FileSystemSyncAccessHandle objects. That is the browser's storage, bridged
 * through emscripten, and it is unreachable from a native host. Which is fine,
 * and is the seam doing its job: bj_io is four callbacks, so a native host
 * writes its own. This one is a plain file descriptor.
 */
static uint64_t io_size(void *ctx) {
    off_t end = lseek(*(int *)ctx, 0, SEEK_END);
    return end < 0 ? 0 : (uint64_t)end;
}
static int64_t io_read(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) {
    ssize_t n = pread(*(int *)ctx, buf, len, (off_t)off);
    return n < 0 ? -1 : (int64_t)n;
}
static int32_t io_write(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) {
    ssize_t n = pwrite(*(int *)ctx, buf, len, (off_t)off);
    return (n < 0 || (uint32_t)n != len) ? -1 : 0;
}
static int32_t io_truncate(void *ctx, uint64_t len) {
    return ftruncate(*(int *)ctx, (off_t)len) < 0 ? -1 : 0;
}

static void put_kv(bj_builder *b, const char *k, const char *v) {
    bj_put_key(b, (const uint8_t *)k, (uint32_t)strlen(k));
    bj_put_string(b, (const uint8_t *)v, (uint32_t)strlen(v));
}

int main(void) {
    printf("--- mdy-native: nisaba ---\n");

    char tmpl[] = "/tmp/mdy-nisaba-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) { printf("  could not open storage\n"); return 1; }
    unlink(tmpl); /* the tree lives as long as the fd does */

    bj_io io = { .ctx = &fd, .size = io_size, .read = io_read,
                 .write = io_write, .truncate = io_truncate };
    bpt *tree = bpt_create(&io, 64);
    if (!tree) { printf("  bpt_create failed\n"); return 1; }
    dc_collection *docs = dc_collection_open(tree);
    if (!docs) { printf("  dc_collection_open failed\n"); return 1; }

    /* Two documents, shaped like the ones mdy-docs inserts: a path and a title. */
    const char *paths[] = { "corpus/en/uruk.mdy", "corpus/en/babylon.mdy" };
    const char *titles[] = { "Uruk", "Babylon" };
    for (int i = 0; i < 2; i++) {
        bj_builder *b = bj_builder_new();
        bj_begin_object(b);
        uint8_t oid[12] = { 0x6a, 0x98, 0x23, 0xe6, 0, 0, 0, 0, 0, 0, 0, (uint8_t)i };
        bj_put_key(b, (const uint8_t *)"_id", 3);
        bj_put_oid(b, oid);
        put_kv(b, "path", paths[i]);
        put_kv(b, "title", titles[i]);
        bj_end_object(b);
        size_t len = 0;
        const uint8_t *bytes = bj_builder_data(b, &len);
        int rc = bytes ? dc_insert_one(docs, bytes, (uint32_t)len) : -99;
        printf("  insert %-22s : %s (rc=%d, %zu bytes, builder err=%d)\n",
               paths[i], rc >= 0 ? "ok" : "FAILED", rc, len, bj_builder_error(b));
        bj_builder_free(b);
    }

    /* Query it back by path — the same shape as $.find({ path: ... }). */
    bj_builder *q = bj_builder_new();
    bj_begin_object(q);
    put_kv(q, "path", "corpus/en/uruk.mdy");
    bj_end_object(q);
    size_t qlen = 0;
    const uint8_t *filter = bj_builder_data(q, &qlen);

    uint8_t *out = NULL;
    size_t out_len = 0;
    int found = 0;
    int rc = dc_find_one(docs, filter, (uint32_t)qlen, NULL, 0, &found, &out, &out_len);
    printf("  find_one path=uruk     : %s (%zu bytes)\n",
           rc < 0 ? "error" : (found ? "found" : "no match"), out_len);

    /* The title, read straight out of the returned binjson. */
    if (found && out) {
        char *needle = memmem(out, out_len, "Uruk", 4);
        printf("  document carries title : %s\n", needle ? "Uruk" : "(not visible)");
        free(out);
    }

    bj_builder_free(q);
    dc_collection_free(docs);
    close(fd);
    return 0;
}
