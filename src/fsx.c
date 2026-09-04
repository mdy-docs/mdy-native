/*
 * The filesystem half. POSIX and nothing else — see fsx.h for the contract
 * and for why this is not called fs.c.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fsx.h"

/* ---- a growable string, since the listing's size is not known up front ---- */

typedef struct { char *s; size_t len, cap; } Buf;

static int buf_put(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *grown = realloc(b->s, cap);
        if (!grown) return -1;
        b->s = grown;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
    return 0;
}

/* ---- paths ---------------------------------------------------------------- */

/* `root` and a `/`-separated relative path as one path. A root of "/" and an
 * absolute-looking relative path is the shape mdy-docs' module loader uses
 * (`fs.read('/', '/abs/path')`), so it has to join to one slash, not two. */
static char *at(const char *root, const char *rel) {
    size_t rlen = strlen(root);
    while (rlen > 1 && root[rlen - 1] == '/') rlen--;
    while (*rel == '/') rel++;
    size_t need = rlen + 1 + strlen(rel) + 1;
    char *p = malloc(need);
    if (!p) return NULL;
    if (*rel == '\0') snprintf(p, need, "%.*s", (int)rlen, root);
    else if (rlen == 1 && root[0] == '/') snprintf(p, need, "/%s", rel);
    else snprintf(p, need, "%.*s/%s", (int)rlen, root, rel);
    return p;
}

/** True when `name` ends with one of the comma-separated suffixes in `exts`.
 * NULL exts means every name matches. */
static int matches(const char *name, const char *exts) {
    if (!exts || !*exts) return 1;
    size_t nlen = strlen(name);
    const char *p = exts;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t elen = comma ? (size_t)(comma - p) : strlen(p);
        if (elen > 0 && nlen >= elen && memcmp(name + nlen - elen, p, elen) == 0) return 1;
        if (!comma) break;
        p = comma + 1;
    }
    return 0;
}

/* ---- listing -------------------------------------------------------------- */

static int walk(const char *base, const char *rel, const char *exts, Buf *out) {
    char *dir = at(base, rel);
    if (!dir) return -1;
    DIR *d = opendir(dir);
    free(dir);
    /* Missing is empty, not an error — see fsx.h. */
    if (!d) return errno == ENOENT ? 0 : 0;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue; /* . .. and dotfiles, as walkVault does */
        size_t need = strlen(rel) + 1 + strlen(e->d_name) + 1;
        char *child = malloc(need);
        if (!child) { closedir(d); return -1; }
        if (*rel) snprintf(child, need, "%s/%s", rel, e->d_name);
        else snprintf(child, need, "%s", e->d_name);

        /*
         * DT_UNKNOWN is real: several filesystems (and every one reached
         * through some network layers) decline to answer from the directory
         * entry, and a walk that trusts d_type silently loses whole subtrees
         * there. Fall back to stat rather than guess.
         */
        int is_dir;
        if (e->d_type == DT_DIR) is_dir = 1;
        else if (e->d_type == DT_REG) is_dir = 0;
        else {
            char *full = at(base, child);
            struct stat st;
            is_dir = full && stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            free(full);
        }

        if (is_dir) {
            if (walk(base, child, exts, out) < 0) { free(child); closedir(d); return -1; }
        } else if (matches(e->d_name, exts)) {
            if (buf_put(out, child, strlen(child)) < 0 || buf_put(out, "\n", 1) < 0) {
                free(child); closedir(d); return -1;
            }
        }
        free(child);
    }
    closedir(d);
    return 0;
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

char *fsx_list(const char *root, const char *subdir, const char *exts) {
    char *base = at(root, subdir && strcmp(subdir, ".") != 0 ? subdir : "");
    if (!base) return NULL;

    Buf out = { 0 };
    int rc = walk(base, "", exts, &out);
    free(base);
    if (rc < 0) { free(out.s); return NULL; }
    if (!out.s) return calloc(1, 1);

    /* The contract says sorted, and readdir order is the filesystem's. Sorting
     * here rather than in JS keeps the listing one string across the boundary
     * instead of an array of thousands. */
    size_t lines = 0;
    for (size_t i = 0; i < out.len; i++) if (out.s[i] == '\n') lines++;
    if (lines < 2) return out.s;

    char **v = malloc(lines * sizeof *v);
    if (!v) return out.s; /* unsorted beats nothing */
    size_t n = 0;
    for (char *p = out.s, *nl; (nl = strchr(p, '\n')); p = nl + 1) { *nl = '\0'; v[n++] = p; }
    qsort(v, n, sizeof *v, by_name);

    Buf sorted = { 0 };
    for (size_t i = 0; i < n; i++) {
        if (buf_put(&sorted, v[i], strlen(v[i])) < 0 || buf_put(&sorted, "\n", 1) < 0) break;
    }
    free(v);
    free(out.s);
    return sorted.s ? sorted.s : calloc(1, 1);
}

/* ---- one file ------------------------------------------------------------- */

uint8_t *fsx_read(const char *root, const char *rel, size_t *len) {
    *len = 0;
    char *path = at(root, rel);
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);

    uint8_t *bytes = malloc((size_t)n + 1); /* +1 so a zero-length read still allocates */
    if (!bytes) { fclose(f); return NULL; }
    size_t got = fread(bytes, 1, (size_t)n, f);
    fclose(f);
    *len = got;
    return bytes;
}

int fsx_stat(const char *root, const char *rel, double *size, double *mtime_ms) {
    char *path = at(root, rel);
    if (!path) return -1;
    struct stat st;
    int rc = stat(path, &st);
    free(path);
    if (rc != 0) return -1;
    *size = (double)st.st_size;
#if defined(__APPLE__)
    *mtime_ms = (double)st.st_mtimespec.tv_sec * 1000.0 + st.st_mtimespec.tv_nsec / 1000000.0;
#else
    *mtime_ms = (double)st.st_mtim.tv_sec * 1000.0 + st.st_mtim.tv_nsec / 1000000.0;
#endif
    return 0;
}

/** mkdir -p on a file's parent. */
static int ensure_parent(char *path) {
    char *cut = strrchr(path, '/');
    if (!cut || cut == path) return 0;
    *cut = '\0';
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(path, 0777) != 0 && errno != EEXIST) { *p = '/'; *cut = '/'; return -1; }
        *p = '/';
    }
    int rc = (mkdir(path, 0777) != 0 && errno != EEXIST) ? -1 : 0;
    *cut = '/';
    return rc;
}

int fsx_write(const char *root, const char *rel, const uint8_t *bytes, size_t len) {
    char *path = at(root, rel);
    if (!path) return -1;
    if (ensure_parent(path) != 0) { free(path); return -1; }
    FILE *f = fopen(path, "wb");
    free(path);
    if (!f) return -1;
    size_t wrote = len ? fwrite(bytes, 1, len, f) : 0;
    return (fclose(f) != 0 || wrote != len) ? -1 : 0;
}

int fsx_remove(const char *root, const char *rel) {
    char *path = at(root, rel);
    if (!path) return -1;
    int rc = unlink(path);
    free(path);
    return (rc == 0 || errno == ENOENT) ? 0 : -1;
}

char *fsx_cwd(void) {
    char buf[4096];
    return getcwd(buf, sizeof buf) ? strdup(buf) : NULL;
}
