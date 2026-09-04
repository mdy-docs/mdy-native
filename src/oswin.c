/*
 * See oswin.h. Nothing in here is interesting except that it is all wide-char.
 */
#ifdef _WIN32

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "oswin.h"

wchar_t *win_widen(const char *utf8) {
    if (!utf8) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof *w);
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n) <= 0) { free(w); return NULL; }
    return w;
}

char *win_narrow(const wchar_t *w) {
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t)n);
    if (!s) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) { free(s); return NULL; }
    return s;
}

int64_t win_pread(void *handle, uint64_t off, uint8_t *buf, uint32_t len) {
    OVERLAPPED ov = { 0 };
    ov.Offset = (DWORD)(off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    if (!ReadFile((HANDLE)handle, buf, len, &got, &ov)) {
        /* Reading at or past EOF is not an error to the caller — a B+tree
         * asks for a node-sized block and gets a short read at the end. */
        return GetLastError() == ERROR_HANDLE_EOF ? 0 : -1;
    }
    return (int64_t)got;
}

int32_t win_pwrite(void *handle, uint64_t off, const uint8_t *buf, uint32_t len) {
    OVERLAPPED ov = { 0 };
    ov.Offset = (DWORD)(off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD wrote = 0;
    if (!WriteFile((HANDLE)handle, buf, len, &wrote, &ov)) return -1;
    return wrote == len ? 0 : -1;
}

uint64_t win_fsize(void *handle) {
    LARGE_INTEGER size;
    return GetFileSizeEx((HANDLE)handle, &size) ? (uint64_t)size.QuadPart : 0;
}

int32_t win_ftruncate(void *handle, uint64_t len) {
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)len;
    if (!SetFilePointerEx((HANDLE)handle, pos, NULL, FILE_BEGIN)) return -1;
    return SetEndOfFile((HANDLE)handle) ? 0 : -1;
}

void *win_temp_file(void) {
    wchar_t dir[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0 || n > MAX_PATH) return NULL;
    wchar_t path[MAX_PATH + 1];
    if (GetTempFileNameW(dir, L"mdy", 0, path) == 0) return NULL;

    /* DELETE_ON_CLOSE is the equivalent of mkstemp followed by unlink: the
     * collection lives exactly as long as its handle and leaves nothing
     * behind if the process dies. */
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                           CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (void *)h;
}

void win_close(void *handle) {
    if (handle) CloseHandle((HANDLE)handle);
}

int win_ensure_parent(const char *utf8_path) {
    char *copy = strdup(utf8_path);
    if (!copy) return -1;
    char *cut = strrchr(copy, '/');
    if (!cut) { free(copy); return 0; }
    *cut = '\0';

    int rc = 0;
    for (char *p = copy + 1; *p && rc == 0; p++) {
        if (*p != '/') continue;
        *p = '\0';
        wchar_t *w = win_widen(copy);
        if (w) {
            if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) rc = -1;
            free(w);
        }
        *p = '/';
    }
    if (rc == 0) {
        wchar_t *w = win_widen(copy);
        if (w) {
            if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) rc = -1;
            free(w);
        } else rc = -1;
    }
    free(copy);
    return rc;
}

#endif /* _WIN32 */
