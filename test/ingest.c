/*
 * A document, from text to a collection that can be queried.
 *
 * The whole ingest, with no JavaScript anywhere in it:
 *
 *     .mdy text
 *       -> mdy_data_extract        the ```data fences, and the body without them
 *       -> mdy_yaml_parse          front matter, and each fence
 *       -> mdy_bj_document         merged, with an _id, as binjson
 *       -> dc_insert_one           into a real nisaba collection
 *       -> dc_find                 and back out again
 *
 * mdy-docs inserts a document's DATA and never its text; this checks that the
 * data that arrives is the data that was written, including the merge order a
 * data fence depends on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "bplustree.h"
#include "ingest.h"
#include "mdydata.h"
#include "mdyyaml.h"

static int failures;

static void ok(const char *what, int passed) {
    printf("  %s  %s\n", passed ? "ok  " : "FAIL", what);
    if (!passed) failures++;
}

/* nis.c owns the real one; this is the same two calls without its slot table. */
extern int nis_open(void);

int main(void) {
    const char *source =
        "+++\n"
        "title: Uruk\n"
        "size: 4\n"
        "tags:\n"
        "  - city\n"
        "  - sumer\n"
        "+++\n"
        "Some body text that must NOT reach the database.\n"
        "\n"
        "```data\n"
        "size: 9\n"
        "founded: -4000\n"
        "```\n"
        "\n"
        "More body.\n";

    printf("--- ingest: a document into a collection ---\n");

    /* ---- the ```data fences, and the body without them ------------------- */
    /* Front matter first, as the engine does: everything after the closing
     * `+++` is the body the fences are pulled from. */
    const char *body_start = strstr(source, "+++\n") ;
    body_start = strstr(body_start + 4, "+++\n");
    body_start += 4;

    mdy_data *data = mdy_data_extract(body_start, strlen(body_start));
    ok("one data fence found", mdy_data_count(data) == 1);

    size_t body_len = 0;
    const char *body = mdy_data_body(data, &body_len);
    ok("the fence is gone from the body", memmem(body, body_len, "founded", 7) == NULL);
    ok("…and the prose is not", memmem(body, body_len, "More body.", 10) != NULL);

    /* ---- the YAML ------------------------------------------------------- */
    char err[256];
    const char *fm = "title: Uruk\nsize: 4\ntags:\n  - city\n  - sumer\n";
    mdy_yaml *front = mdy_yaml_parse(fm, strlen(fm), err, sizeof err);
    ok("front matter parses", front != NULL);

    const mdy_data_fence *f = mdy_data_at(data, 0);
    mdy_yaml *fence = mdy_yaml_parse(f->source, f->source_len, err, sizeof err);
    ok("the fence's YAML parses", fence != NULL);
    if (!front || !fence) return 1;

    /* ---- merged, with an _id, as binjson --------------------------------- */
    const mdy_yaml_node *maps[2] = { mdy_yaml_root(front), mdy_yaml_root(fence) };
    uint8_t oid[12];
    mdy_oid_next(oid);

    bj_builder *b = bj_builder_new();
    ok("the document encodes", b && mdy_bj_document(b, oid, maps, 2) == 0 && !bj_builder_error(b));

    size_t doc_len = 0;
    const uint8_t *doc = bj_builder_data(b, &doc_len);
    ok("it produced bytes", doc && doc_len > 0);

    /* ---- into a real collection ----------------------------------------- */
    int handle = nis_open();
    ok("a collection opens", handle >= 0);
    if (handle < 0) return 1;

    extern int nis_insert(int handle, const uint8_t *doc, uint32_t len);
    ok("dc_insert_one accepts it", nis_insert(handle, doc, (uint32_t)doc_len) == 0);

    /* ---- and back out again ---------------------------------------------- */
    bj_builder *q = bj_builder_new();
    bj_begin_object(q);
    bj_put_key(q, (const uint8_t *)"title", 5);
    bj_put_string(q, (const uint8_t *)"Uruk", 4);
    bj_end_object(q);
    size_t qlen = 0;
    const uint8_t *qbytes = bj_builder_data(q, &qlen);

    extern int nis_find(int handle, const uint8_t *filter, uint32_t filter_len,
                        uint8_t **out, size_t *out_len);
    uint8_t *found = NULL;
    size_t found_len = 0;
    int rc = nis_find(handle, qbytes, (uint32_t)qlen, &found, &found_len);
    ok("a query on `title` finds it", rc == 0 && found && found_len > 0);

    /*
     * The values that matter: `size` must be 9, not 4 — the data fence
     * overrode the front matter — and the body must be nowhere in it.
     */
    if (found) {
        int has_nine = 0;
        for (size_t i = 0; i + 1 < found_len; i++)
            if (found[i] == 9 && found[i + 1] == 0) { has_nine = 1; break; }
        ok("the fence's value won the merge (size: 9)", has_nine);
        ok("the body text is not in the database",
           memmem(found, found_len, "More body", 9) == NULL &&
           memmem(found, found_len, "must NOT reach", 14) == NULL);
        ok("a declared tag survived", memmem(found, found_len, "sumer", 5) != NULL);
        free(found);
    }

    bj_builder_free(q);
    bj_builder_free(b);
    mdy_yaml_free(fence);
    mdy_yaml_free(front);
    mdy_data_free(data);

    if (failures) { printf("\n%d failed\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
