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

#ifdef _WIN32
#  include "oswin.h"
#else
#  include <unistd.h>
#endif

#include "binjson.h"
#include "bjio.h"
#include "bplustree.h"
#include "db.h"
#include "nis.h"

#define MAX_INDEXES 4

/* A descriptor on POSIX, a HANDLE on Windows; bj_io takes its address. */
#ifdef _WIN32
typedef void *FileRef;
#  define FILE_REF_BAD NULL
#else
typedef int FileRef;
#  define FILE_REF_BAD (-1)
#endif

typedef struct {
    FileRef fd;
    bpt *tree;
    dc_collection *coll;
    /* One extra fd + tree per secondary index — each index is its own B+tree,
     * so it needs its own backing file. */
    FileRef index_fds[MAX_INDEXES];
    bpt *index_trees[MAX_INDEXES];
    int indexes;
    int used;
} Slot;

/*
 * The slot table GROWS rather than being capped. It was eight, which was fine
 * for a build — one document set per package — and wrong the moment mdy-docs'
 * own test suite ran here: it opens a set per test, hundreds of them, and the
 * ninth failed with "could not open a collection".
 *
 * Growing alone would only move the failure to the file-descriptor limit,
 * since every collection holds an open temp file (and one more per index). The
 * other half of the fix is in host.c: the handle is wrapped in a JS object
 * whose finalizer closes it, so a collection is reclaimed when the JavaScript
 * that owned it becomes unreachable. That is the lifetime the WASM binding
 * gets from its own GC, and this is how a native host earns the same.
 */
static Slot *g_slots;
static int g_slot_count;

/*
 * The four bj_io callbacks, which are this file's entire platform surface —
 * and the reason nisaba itself needs no porting at all. `ctx` is a POSIX file
 * descriptor or a Win32 HANDLE depending on which of these is compiled; the
 * Slot below stores whichever, and nothing outside this file sees the
 * difference.
 */
#ifdef _WIN32

static uint64_t io_size(void *ctx) { return win_fsize(*(void **)ctx); }
static int64_t io_read(void *ctx, uint64_t off, uint8_t *buf, uint32_t len) {
    return win_pread(*(void **)ctx, off, buf, len);
}
static int32_t io_write(void *ctx, uint64_t off, const uint8_t *buf, uint32_t len) {
    return win_pwrite(*(void **)ctx, off, buf, len);
}
static int32_t io_truncate(void *ctx, uint64_t len) {
    return win_ftruncate(*(void **)ctx, len);
}

#else

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

#endif

/*
 * A temp file that leaves nothing behind: it lives exactly as long as its
 * handle, which is the lifetime `MemoryStorageProvider` promises on the JS
 * side. mkstemp+unlink on POSIX, FILE_FLAG_DELETE_ON_CLOSE on Windows.
 *
 * `/tmp` is hardcoded here and must not stay that way on iOS, where the only
 * writable temp directory is the app sandbox's — see docs/desktop-plan.md,
 * Phase 5. Phase 6 wants this to become a real, persistent file anyway, so
 * that a cold start reuses the previous ingest.
 */
static FileRef temp_fd(void) {
#ifdef _WIN32
    return win_temp_file();
#else
    char tmpl[] = "/tmp/mdy-nisaba-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) unlink(tmpl);
    return fd;
#endif
}

static void close_ref(FileRef f) {
#ifdef _WIN32
    win_close(f);
#else
    close(f);
#endif
}

static int ref_bad(FileRef f) {
#ifdef _WIN32
    return f == NULL;
#else
    return f < 0;
#endif
}

int nis_open(void) {
    int slot = -1;
    for (int i = 0; i < g_slot_count; i++) if (!g_slots[i].used) { slot = i; break; }
    if (slot < 0) {
        int grown = g_slot_count ? g_slot_count * 2 : 16;
        Slot *next = realloc(g_slots, (size_t)grown * sizeof *next);
        if (!next) return -1;
        memset(next + g_slot_count, 0, (size_t)(grown - g_slot_count) * sizeof *next);
        g_slots = next;
        slot = g_slot_count;
        g_slot_count = grown;
    }

    FileRef fd = temp_fd();
    if (ref_bad(fd)) return -1;

    g_slots[slot].fd = fd;
    bj_io io = { .ctx = &g_slots[slot].fd, .size = io_size, .read = io_read,
                 .write = io_write, .truncate = io_truncate };
    g_slots[slot].tree = bpt_create(&io, 64);
    if (!g_slots[slot].tree) { close_ref(fd); return -1; }
    g_slots[slot].coll = dc_collection_open(g_slots[slot].tree);
    if (!g_slots[slot].coll) { close_ref(fd); return -1; }
    g_slots[slot].used = 1;
    return slot;
}

static Slot *slot_of(int handle) {
    if (handle < 0 || handle >= g_slot_count || !g_slots[handle].used) return NULL;
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
    if (ref_bad(s->index_fds[n])) return -1;

    bj_io io = { .ctx = &s->index_fds[n], .size = io_size, .read = io_read,
                 .write = io_write, .truncate = io_truncate };
    s->index_trees[n] = bpt_create(&io, 64);
    if (!s->index_trees[n]) { close_ref(s->index_fds[n]); return -1; }

    /* add_index rather than attach_index: this one is new, so nisaba
     * backfills it from the documents already inserted. attach_index is for
     * reopening a collection whose index file already holds the postings. */
    int rc = dc_collection_add_index(s->coll, name, (int)strlen(name), s->index_trees[n],
                                     fields, fields_len, unique, sparse, NULL, 0);
    if (rc != 0) { close_ref(s->index_fds[n]); s->index_trees[n] = NULL; return rc; }
    s->indexes++;
    return 0;
}

void nis_close(int handle) {
    Slot *s = slot_of(handle);
    if (!s) return;
    dc_collection_free(s->coll);
    for (int i = 0; i < s->indexes; i++) close_ref(s->index_fds[i]);
    close_ref(s->fd);
    s->indexes = 0;
    s->used = 0;
}
