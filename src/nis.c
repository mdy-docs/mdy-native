/*
 * The nisaba side. Includes nisaba's headers and nothing from QuickJS.
 *
 * Storage is ours to supply: nisaba's shipped bjio_host(fd) reaches into
 * Module.bjioHandles, a table of JS FileSystemSyncAccessHandle objects, which
 * is the browser's storage bridged through emscripten. bj_io is four
 * callbacks, so this is a plain file descriptor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "binjson.h"
#include "bjio.h"
#include "bplustree.h"
#include "db.h"
#include "nis.h"

#define MAX_COLLECTIONS 8

#define MAX_INDEXES 4

typedef struct {
    int fd;
    bpt *tree;
    dc_collection *coll;
    /* One extra fd + tree per secondary index — each index is its own B+tree,
     * so it needs its own backing file. */
    int index_fds[MAX_INDEXES];
    bpt *index_trees[MAX_INDEXES];
    int indexes;
    int used;
} Slot;

static Slot g_slots[MAX_COLLECTIONS];

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

/** An unlinked temp file: it lives exactly as long as its descriptor, which is
 * the lifetime `MemoryStorageProvider` promises on the JS side. */
static int temp_fd(void) {
    char tmpl[] = "/tmp/mdy-nisaba-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) unlink(tmpl);
    return fd;
}

int nis_open(void) {
    int slot = -1;
    for (int i = 0; i < MAX_COLLECTIONS; i++) if (!g_slots[i].used) { slot = i; break; }
    if (slot < 0) return -1;

    int fd = temp_fd();
    if (fd < 0) return -1;

    g_slots[slot].fd = fd;
    bj_io io = { .ctx = &g_slots[slot].fd, .size = io_size, .read = io_read,
                 .write = io_write, .truncate = io_truncate };
    g_slots[slot].tree = bpt_create(&io, 64);
    if (!g_slots[slot].tree) { close(fd); return -1; }
    g_slots[slot].coll = dc_collection_open(g_slots[slot].tree);
    if (!g_slots[slot].coll) { close(fd); return -1; }
    g_slots[slot].used = 1;
    return slot;
}

static Slot *slot_of(int handle) {
    if (handle < 0 || handle >= MAX_COLLECTIONS || !g_slots[handle].used) return NULL;
    return &g_slots[handle];
}

int nis_insert(int handle, const uint8_t *doc, uint32_t len) {
    Slot *s = slot_of(handle);
    if (!s) return -1;
    return dc_insert_one(s->coll, doc, len) < 0 ? -1 : 0;
}

int nis_find(int handle, const uint8_t *filter, uint32_t filter_len, uint8_t **out, size_t *out_len) {
    Slot *s = slot_of(handle);
    if (!s) return -1;
    *out = NULL; *out_len = 0;
    return dc_find(s->coll, filter, filter_len, NULL, out, out_len) < 0 ? -1 : 0;
}

int nis_create_index(int handle, const char *name, const uint8_t *fields, uint32_t fields_len,
                     int unique, int sparse) {
    Slot *s = slot_of(handle);
    if (!s || s->indexes >= MAX_INDEXES) return -1;

    int n = s->indexes;
    s->index_fds[n] = temp_fd();
    if (s->index_fds[n] < 0) return -1;

    bj_io io = { .ctx = &s->index_fds[n], .size = io_size, .read = io_read,
                 .write = io_write, .truncate = io_truncate };
    s->index_trees[n] = bpt_create(&io, 64);
    if (!s->index_trees[n]) { close(s->index_fds[n]); return -1; }

    /* add_index rather than attach_index: this one is new, so nisaba
     * backfills it from the documents already inserted. attach_index is for
     * reopening a collection whose index file already holds the postings. */
    int rc = dc_collection_add_index(s->coll, name, (int)strlen(name), s->index_trees[n],
                                     fields, fields_len, unique, sparse, NULL, 0);
    if (rc != 0) { close(s->index_fds[n]); s->index_trees[n] = NULL; return rc; }
    s->indexes++;
    return 0;
}

void nis_close(int handle) {
    Slot *s = slot_of(handle);
    if (!s) return;
    dc_collection_free(s->coll);
    for (int i = 0; i < s->indexes; i++) close(s->index_fds[i]);
    close(s->fd);
    s->indexes = 0;
    s->used = 0;
}
