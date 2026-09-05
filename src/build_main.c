/*
 * `mdy build`, with no JavaScript engine anywhere in it: a directory in, a
 * directory of pages out.
 *
 * This is the whole native pipeline in one place — the walk, the document
 * set, the entry, and the emits — and it exists to be DIFFED. Its output goes
 * next to `node bin/mdy.js build`'s over the same site, and the two must be
 * byte-identical; a driver is the only way to compare whole sites rather than
 * strings in a test.
 *
 * Where an emit lands is the embedder's business, and this embedder writes
 * files. `static/` is copied through verbatim, last, exactly as buildSite
 * does — a page emitted to the same path wins, since a document that meant to
 * write a file should not be silently shadowed by a stray asset.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "fsx.h"

typedef struct { const char *out; int count; int failed; } Sink;

static void write_page(void *ud, const char *path, const char *content) {
    Sink *s = ud;
    if (fsx_write(s->out, path, (const uint8_t *)content, strlen(content)) != 0) {
        fprintf(stderr, "cannot write %s/%s\n", s->out, path);
        s->failed++;
        return;
    }
    s->count++;
}

/* Every file under `<root>/static/`, copied to the output root. */
static int copy_static(const char *root, const char *out) {
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/static", root);
    char *listing = fsx_list(dir, ".", NULL);
    if (!listing) return 0;
    int n = 0;
    for (char *rel = listing, *next; rel && *rel; rel = next) {
        char *nl = strchr(rel, '\n');
        next = nl ? nl + 1 : NULL;
        if (nl) *nl = '\0';
        if (!*rel) continue;
        size_t len = 0;
        uint8_t *bytes = fsx_read(dir, rel, &len);
        if (!bytes) continue;
        if (fsx_write(out, rel, bytes, len) == 0) n++;
        free(bytes);
    }
    free(listing);
    return n;
}

int main(int argc, char **argv) {
    const char *root = NULL, *out = NULL, *entry = "main.mdy";
    int quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out = argv[++i];
        else if (strcmp(argv[i], "--entry") == 0 && i + 1 < argc) entry = argv[++i];
        else if (strcmp(argv[i], "--quiet") == 0) quiet = 1;
        else if (!root) root = argv[i];
    }
    if (!root || !out) {
        fprintf(stderr, "usage: mdy-build <site-dir> --out <dir> [--entry main.mdy] [--quiet]\n");
        return 2;
    }

    mdy_engine *e = mdy_engine_new();
    if (!e) { fprintf(stderr, "out of memory\n"); return 1; }

    char err[1024];
    if (mdy_engine_open_dir(e, root, err, sizeof err) != 0) {
        fprintf(stderr, "%s\n", err);
        mdy_engine_free(e);
        return 1;
    }

    int at = mdy_engine_entry(e, entry);
    if (at < 0) {
        fprintf(stderr, "entry script not found at \"%s\" (looked among %zu document(s) under %s)\n",
                entry, mdy_engine_count(e), root);
        mdy_engine_free(e);
        return 1;
    }

    Sink sink = { out, 0, 0 };
    mdy_engine_on_emit(e, write_page, &sink);

    char *html = mdy_engine_render(e, (size_t)at, err, sizeof err);
    if (!html) {
        fprintf(stderr, "%s\n", err);
        mdy_engine_free(e);
        return 1;
    }
    free(html);

    /* Every root's static/, imports first — so the site's own copy of a name
     * is the one that survives. */
    int assets = 0;
    size_t roots = mdy_engine_root_count(e);
    for (size_t i = 0; i < roots; i++) assets += copy_static(mdy_engine_root_at(e, i), out);
    mdy_engine_free(e);
    if (!quiet)
        printf("built %d page(s)%s -> %s\n", sink.count,
               assets ? " and copied static/" : "", out);
    return sink.failed ? 1 : 0;
}
