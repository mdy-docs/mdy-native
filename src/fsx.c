/*
 * The filesystem half. POSIX and nothing else — see fsx.h for the contract
 * and for why this is not called fs.c.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>   /* _wgetcwd */
#  include <wchar.h>    /* _wfopen */
#  include <sys/types.h>
#  include "oswin.h"
#else
#  include <dirent.h>
#  include <unistd.h>
#endif

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
/*
 * Is this path already absolute? A leading `/` everywhere, and on Windows also
 * a drive letter — `C:/Users/…`. Both spellings reach here: mdy-docs' module
 * loader resolves a specifier to an absolute path and then asks for it with
 * `fs.read('/', thatPath)`, so on Windows `rel` arrives as `C:/…` while `root`
 * is a bare slash.
 */
static int is_absolute(const char *p) {
    if (p[0] == '/') return 1;
#ifdef _WIN32
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':' && (p[2] == '/' || p[2] == '\\')) return 1;
#endif
    return 0;
}

static char *at(const char *root, const char *rel) {
    /*
     * An absolute `rel` under a root of "/" IS the path — joining them would
     * produce `/C:/Users/…` on Windows, which names nothing. The POSIX case
     * happens to work either way; this makes both explicit rather than relying
     * on one of them being harmless.
     */
    if (is_absolute(rel) && root[0] == '/' && root[1] == '\0') return strdup(rel);

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

/*
 * fopen / stat / unlink / getcwd, but never through Win32's narrow entry
 * points. Those go via the process code page, and the reference corpus is full
 * of names the code page cannot spell — so on Windows each of these widens the
 * UTF-8 path and calls the wide variant. This is the whole reason a `_WIN32`
 * branch exists at file level rather than only around the directory walk.
 */
#ifdef _WIN32

static FILE *open_utf8(const char *path, const wchar_t *mode) {
    wchar_t *w = win_widen(path);
    if (!w) return NULL;
    FILE *f = _wfopen(w, mode);
    free(w);
    return f;
}
#  define FSX_FOPEN_R(p) open_utf8((p), L"rb")
#  define FSX_FOPEN_W(p) open_utf8((p), L"wb")

#else
#  define FSX_FOPEN_R(p) fopen((p), "rb")
#  define FSX_FOPEN_W(p) fopen((p), "wb")
#endif

/* ---- listing -------------------------------------------------------------- */

#ifdef _WIN32

/*
 * The walk, over FindFirstFileW. Windows has no readdir, and the pattern is
 * appended to the directory rather than passed separately — `dir\*` — so this
 * builds one wide string per level.
 *
 * FindFirstFileW answers ERROR_FILE_NOT_FOUND for an empty directory and
 * ERROR_PATH_NOT_FOUND for a missing one, and both are an empty listing to the
 * caller: the contract says a missing directory is [], and a site importing a
 * package with no static/ depends on it.
 */
static int walk(const char *base, const char *rel, const char *exts, Buf *out) {
    char *dir = at(base, rel);
    if (!dir) return -1;
    size_t need = strlen(dir) + 3;
    char *pattern = malloc(need);
    if (!pattern) { free(dir); return -1; }
    snprintf(pattern, need, "%s/*", dir);
    free(dir);

    wchar_t *wpattern = win_widen(pattern);
    free(pattern);
    if (!wpattern) return -1;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    free(wpattern);
    if (h == INVALID_HANDLE_VALUE) return 0;

    int rc = 0;
    do {
        char *name = win_narrow(fd.cFileName);
        if (!name) { rc = -1; break; }
        if (name[0] == '.') { free(name); continue; } /* . .. and dotfiles */

        size_t clen = strlen(rel) + 1 + strlen(name) + 1;
        char *child = malloc(clen);
        if (!child) { free(name); rc = -1; break; }
        if (*rel) snprintf(child, clen, "%s/%s", rel, name);
        else snprintf(child, clen, "%s", name);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            rc = walk(base, child, exts, out);
        } else if (matches(name, exts)) {
            if (buf_put(out, child, strlen(child)) < 0 || buf_put(out, "\n", 1) < 0) rc = -1;
        }
        free(child);
        free(name);
    } while (rc == 0 && FindNextFileW(h, &fd));

    FindClose(h);
    return rc;
}

#else

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

#endif

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
    FILE *f = FSX_FOPEN_R(path);
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

#ifdef _WIN32
    /* Second-resolution mtime, which is all _wstat64 offers and all the
     * provider contract promises — mdy-docs compares mtimes for equality
     * against a remembered one, never for sub-second ordering. */
    wchar_t *w = win_widen(path);
    free(path);
    if (!w) return -1;
    struct _stat64 st;
    int rc = _wstat64(w, &st);
    free(w);
    if (rc != 0) return -1;
    *size = (double)st.st_size;
    *mtime_ms = (double)st.st_mtime * 1000.0;
#else
    struct stat st;
    int rc = stat(path, &st);
    free(path);
    if (rc != 0) return -1;
    *size = (double)st.st_size;
#  if defined(__APPLE__)
    *mtime_ms = (double)st.st_mtimespec.tv_sec * 1000.0 + st.st_mtimespec.tv_nsec / 1000000.0;
#  else
    *mtime_ms = (double)st.st_mtim.tv_sec * 1000.0 + st.st_mtim.tv_nsec / 1000000.0;
#  endif
#endif
    return 0;
}

/** mkdir -p on a file's parent. */
#ifdef _WIN32
static int ensure_parent(char *path) { return win_ensure_parent(path); }
#else
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
#endif

int fsx_write(const char *root, const char *rel, const uint8_t *bytes, size_t len) {
    char *path = at(root, rel);
    if (!path) return -1;
    if (ensure_parent(path) != 0) { free(path); return -1; }
    FILE *f = FSX_FOPEN_W(path);
    free(path);
    if (!f) return -1;
    size_t wrote = len ? fwrite(bytes, 1, len, f) : 0;
    return (fclose(f) != 0 || wrote != len) ? -1 : 0;
}

int fsx_remove(const char *root, const char *rel) {
    char *path = at(root, rel);
    if (!path) return -1;
#ifdef _WIN32
    wchar_t *w = win_widen(path);
    free(path);
    if (!w) return -1;
    int rc = _wunlink(w);
    free(w);
#else
    int rc = unlink(path);
    free(path);
#endif
    return (rc == 0 || errno == ENOENT) ? 0 : -1;
}

/*
 * The working directory, `/`-separated even on Windows.
 *
 * Every path above this line is `/`-separated, because that is what the
 * provider contract speaks and what mdy-docs' import graph does string maths
 * on. Win32 accepts `/` in every path it is given, so the only place a
 * backslash can enter the system is here — where the OS hands one back. It is
 * translated once, at the boundary, rather than everywhere it would otherwise
 * surface. See docs/desktop-plan.md, Phase 4, for why that matters: imports.js
 * decides a module is inside its package by string prefix, and two spellings
 * of one path are two packages.
 */
char *fsx_cwd(void) {
#ifdef _WIN32
    wchar_t wbuf[4096];
    if (!_wgetcwd(wbuf, 4096)) return NULL;
    char *utf8 = win_narrow(wbuf);
    if (utf8) for (char *p = utf8; *p; p++) if (*p == '\\') *p = '/';
    return utf8;
#else
    char buf[4096];
    return getcwd(buf, sizeof buf) ? strdup(buf) : NULL;
#endif
}

/* ---- what the ported test suite needs ------------------------------------
 *
 * See fsx.h. Nothing in the backend proper calls any of this; it exists so
 * mdy-docs' own tests, which write real directories and build them, can run
 * against the native target rather than only against node.
 */

char *fsx_readdir(const char *path) {
    Buf out = { 0 };
#ifdef _WIN32
    size_t need = strlen(path) + 3;
    char *pattern = malloc(need);
    if (!pattern) return NULL;
    snprintf(pattern, need, "%s/*", path);
    wchar_t *wpattern = win_widen(pattern);
    free(pattern);
    if (!wpattern) return NULL;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    free(wpattern);
    if (h == INVALID_HANDLE_VALUE) {
        /* An empty directory still exists; a missing one does not. */
        return GetLastError() == ERROR_FILE_NOT_FOUND ? calloc(1, 1) : NULL;
    }
    do {
        char *name = win_narrow(fd.cFileName);
        if (!name) continue;
        if (strcmp(name, ".") && strcmp(name, "..")) {
            buf_put(&out, name, strlen(name));
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) buf_put(&out, "/", 1);
            buf_put(&out, "\n", 1);
        }
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) return NULL;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        buf_put(&out, e->d_name, strlen(e->d_name));

        int is_dir;
        if (e->d_type == DT_DIR) is_dir = 1;
        else if (e->d_type == DT_REG) is_dir = 0;
        else {
            char *full = at(path, e->d_name);
            struct stat st;
            is_dir = full && stat(full, &st) == 0 && S_ISDIR(st.st_mode);
            free(full);
        }
        if (is_dir) buf_put(&out, "/", 1);
        buf_put(&out, "\n", 1);
    }
    closedir(d);
#endif
    return out.s ? out.s : calloc(1, 1);
}

int fsx_mkdirp(const char *path) {
#ifdef _WIN32
    /* win_ensure_parent makes everything ABOVE its argument, so name a child
     * that will never be created to get the directory itself. */
    size_t need = strlen(path) + 3;
    char *child = malloc(need);
    if (!child) return -1;
    snprintf(child, need, "%s/x", path);
    int rc = win_ensure_parent(child);
    free(child);
    return rc;
#else
    char *copy = strdup(path);
    if (!copy) return -1;
    int rc = 0;
    for (char *p = copy + 1; *p && rc == 0; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(copy, 0777) != 0 && errno != EEXIST) rc = -1;
        *p = '/';
    }
    if (rc == 0 && mkdir(copy, 0777) != 0 && errno != EEXIST) rc = -1;
    free(copy);
    return rc;
#endif
}

/** Is this a directory? Used only to decide how to remove it. */
static int is_dir_path(const char *path) {
#ifdef _WIN32
    wchar_t *w = win_widen(path);
    if (!w) return 0;
    DWORD attr = GetFileAttributesW(w);
    free(w);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int remove_dir(const char *path) {
#ifdef _WIN32
    wchar_t *w = win_widen(path);
    if (!w) return -1;
    int rc = RemoveDirectoryW(w) ? 0 : -1;
    free(w);
    return rc;
#else
    return rmdir(path);
#endif
}

int fsx_rm_rf(const char *path) {
    if (!is_dir_path(path)) {
        /* A missing path is not an error — this is rm -rf. */
#ifdef _WIN32
        wchar_t *w = win_widen(path);
        if (!w) return -1;
        int rc = _wunlink(w);
        free(w);
        return (rc == 0 || errno == ENOENT) ? 0 : -1;
#else
        int rc = unlink(path);
        return (rc == 0 || errno == ENOENT) ? 0 : -1;
#endif
    }

    char *listing = fsx_readdir(path);
    if (!listing) return 0;
    int rc = 0;
    for (char *p = listing, *nl; (nl = strchr(p, '\n')); p = nl + 1) {
        *nl = '\0';
        size_t len = strlen(p);
        if (len && p[len - 1] == '/') p[len - 1] = '\0'; /* the dir marker */
        char *child = at(path, p);
        if (!child) { rc = -1; break; }
        if (fsx_rm_rf(child) != 0) rc = -1;
        free(child);
    }
    free(listing);
    return rc == 0 ? remove_dir(path) : rc;
}

char *fsx_mkdtemp(const char *prefix) {
    /*
     * Six X's, as mkdtemp wants. Windows has no mkdtemp, so there the name is
     * built from GetTempFileNameW's uniqueness — it creates a FILE, which is
     * deleted and replaced with a directory of the same name. That is the
     * documented way to do this, and the race it leaves is the same one
     * GetTempFileNameW already has.
     */
#ifdef _WIN32
    (void)prefix;
    wchar_t dir[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return NULL;
    wchar_t path[MAX_PATH + 1];
    if (GetTempFileNameW(dir, L"mdy", 0, path) == 0) return NULL;
    DeleteFileW(path);
    if (!CreateDirectoryW(path, NULL)) return NULL;
    char *utf8 = win_narrow(path);
    if (utf8) for (char *p = utf8; *p; p++) if (*p == '\\') *p = '/';
    return utf8;
#else
    size_t need = strlen(prefix) + 7;
    char *tmpl = malloc(need);
    if (!tmpl) return NULL;
    snprintf(tmpl, need, "%sXXXXXX", prefix);
    if (!mkdtemp(tmpl)) { free(tmpl); return NULL; }
    return tmpl;
#endif
}

char *fsx_tmpdir(void) {
#ifdef _WIN32
    wchar_t dir[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return NULL;
    char *utf8 = win_narrow(dir);
    if (!utf8) return NULL;
    for (char *p = utf8; *p; p++) if (*p == '\\') *p = '/';
    size_t len = strlen(utf8);
    while (len > 1 && utf8[len - 1] == '/') utf8[--len] = '\0';
    return utf8;
#else
    const char *env = getenv("TMPDIR");
    if (!env || !*env) env = "/tmp";
    char *out = strdup(env);
    size_t len = strlen(out);
    while (len > 1 && out[len - 1] == '/') out[--len] = '\0';
    return out;
#endif
}
