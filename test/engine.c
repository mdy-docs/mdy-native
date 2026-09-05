/*
 * One document, rendered — with no JavaScript engine but lamassu.
 *
 * The whole of mdy-docs' three passes, in C: the source split into documents
 * and each into data and body, the `%` and `{{ }}` lines compiled and run in
 * lamassu, the lines they produced parsed to hast, and the tree written as
 * HTML. QuickJS is not linked into this binary at all.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"
#include "fsx.h"

static int failures;

/* Where an `$.emit` lands, for the check below. */
static char last_emit_path[256];
static char last_emit_content[4096];

static void collect_emit(void *ud, const char *path, const char *content) {
    (void)ud;
    snprintf(last_emit_path, sizeof last_emit_path, "%s", path);
    snprintf(last_emit_content, sizeof last_emit_content, "%s", content);
}

static void check(const char *what, const char *source, const char *expected) {
    mdy_engine *e = mdy_engine_new();
    char err[256];
    char *html = NULL;
    if (mdy_engine_open(e, source, strlen(source), err, sizeof err) == 0)
        html = mdy_engine_render(e, 0, err, sizeof err);
    int ok = html && strcmp(html, expected) == 0;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        printf("      expected %s\n      actual   %s\n", expected,
               html ? html : (err[0] ? err : "(null)"));
        failures++;
    }
    free(html);
    mdy_engine_free(e);
}

/* A document that asks for something the engine cannot do yet must FAIL,
 * naming it — not render a page quietly missing it. */
static void refuses(const char *what, const char *source, const char *expected) {
    mdy_engine *e = mdy_engine_new();
    char err[256];
    char *html = NULL;
    if (mdy_engine_open(e, source, strlen(source), err, sizeof err) == 0)
        html = mdy_engine_render(e, 0, err, sizeof err);
    int ok = !html && strstr(err, expected) != NULL;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        printf("      expected an error containing %s\n      actual   %s\n", expected,
               html ? "(it rendered)" : err);
        failures++;
    }
    free(html);
    mdy_engine_free(e);
}


/* ---- a directory as a site --------------------------------------------------
 *
 * The natives first, because they are pure, and then the whole thing: a real
 * directory on disk, walked, rendered, and its emits collected — which is what
 * `mdy build` is once a caller decides where the files go.
 */

/* Emits from a whole-site render, in arrival order. */
static char emit_paths[64][256];
static char emit_bodies[64][8192];
static int emit_count;

static void collect_all(void *ud, const char *path, const char *content) {
    (void)ud;
    if (emit_count >= 64) return;
    snprintf(emit_paths[emit_count], 256, "%s", path);
    snprintf(emit_bodies[emit_count], 8192, "%s", content);
    emit_count++;
}

static const char *emitted(const char *path) {
    for (int i = 0; i < emit_count; i++)
        if (strcmp(emit_paths[i], path) == 0) return emit_bodies[i];
    return NULL;
}

static void write_file(const char *root, const char *rel, const char *text) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    char *slash = strrchr(path, '/');
    if (slash) { *slash = '\0'; fsx_mkdirp(path); *slash = '/'; }
    FILE *f = fopen(path, "wb");
    if (!f) { printf("      cannot write %s\n", path); failures++; return; }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void ok_(const char *what, int cond, const char *detail) {
    printf("  %s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) { printf("      actual %s\n", detail ? detail : "(null)"); failures++; }
}

static void site_checks(void) {
    printf("\n--- engine: a directory as a site ---\n");

    /* fsx_mkdtemp takes a PATH template, so the system temp directory has to
     * be part of it — a bare prefix makes the directory in the working one,
     * and a crashing test then leaves it in the source tree. */
    char *tmp = fsx_tmpdir();
    char prefix[1024];
    snprintf(prefix, sizeof prefix, "%s/mdy-site", tmp ? tmp : ".");
    free(tmp);
    char *root = fsx_mkdtemp(prefix);
    if (!root) { printf("  FAIL  cannot make a temp directory\n"); failures++; return; }

    /*
     * A site with one of each kind of file, so the dispatch is exercised by
     * what the entry can actually SEE rather than by asserting on the walk.
     */
    write_file(root, "main.mdy",
        "% for (const c of $.find({ role: 'city' })) {\n"
        "%   $.emit(c.slug + '/index.html', $.render({ path: 'layout.mdy' }, { who: c.who }))\n"
        "% }\n"
        "% const notes = $.findOne({ ext: '.md' })\n"
        "% $.emit('notes.txt', notes.body)\n"
        "% const conf = $.findOne({ path: 'site.yaml' })\n"
        "% $.emit('title.txt', conf.title)\n"
        /*
         * A data file's OWN fields win over the identity the walk derived —
         * identity is a default there, not an override. A record commonly
         * declares a `name` or a `size` of its own, and shadowing those would
         * make the file's data unreachable under the field it actually used.
         * `path` is the exception: everything resolves documents by it, so it
         * is always the real one.
         */
        "% $.emit('own.txt', [conf.name, conf.size, conf.ext, conf.path].join('|'))\n"
        "% $.emit('tags.txt', (notes.tags || []).join(','))\n"
        "% $.emit('words.txt', $.tokenize('The Walls of Uruk and the walls').join(','))\n"
        "% $.emit('date.txt', $.rfc822('2026-09-05'))\n");
    write_file(root, "layout.mdy", "= {{ req.who }}\n");
    write_file(root, "cities/uruk.yaml", "role: city\nwho: Uruk\nslug: uruk\n");
    write_file(root, "cities/babylon.yaml", "role: city\nwho: Babylon\nslug: babylon\n");
    write_file(root, "site.yaml", "title: A Directory\nname: Not The File Name\nsize: enormous\n");
    write_file(root, "notes.md",
        "A {{ literal }} in prose. #uruk and #Uruk again.\n"
        "\n"
        "---\n"
        "\n"
        "That rule above is prose, not a document separator.\n"
        "\n"
        "```\n"
        "# not a tag: a shell comment in a fence\n"
        "```\n");
    write_file(root, "dist/stale.mdy", "= Should not be here\n");
    write_file(root, ".hidden/secret.mdy", "= Nor this\n");

    mdy_engine *e = mdy_engine_new();
    char err[512];
    emit_count = 0;
    mdy_engine_on_emit(e, collect_all, NULL);

    if (mdy_engine_open_dir(e, root, err, sizeof err) != 0) {
        printf("  FAIL  open a directory\n      %s\n", err);
        failures++;
        mdy_engine_free(e);
        free(root);
        return;
    }
    ok_("a directory opens as a document set", mdy_engine_count(e) == 6, NULL);

    int entry = mdy_engine_entry(e, "main.mdy");
    ok_("the entry is found by path", entry >= 0, NULL);

    char *html = entry >= 0 ? mdy_engine_render(e, (size_t)entry, err, sizeof err) : NULL;
    if (!html) {
        printf("  FAIL  the entry renders\n      %s\n", err);
        failures++;
        mdy_engine_free(e);
        free(root);
        return;
    }
    free(html);

    /* A .yaml file's fields are its record: found by query, read by name. */
    const char *uruk = emitted("uruk/index.html");
    ok_("a .yaml file is a queryable document",
        uruk && strcmp(uruk, "<h1 id=\"uruk\">Uruk</h1>") == 0, uruk);
    ok_("...and so is every other one",
        emitted("babylon/index.html") != NULL, NULL);
    ok_("a data file's own fields beat the identity the walk derived",
        emitted("own.txt") &&
            strcmp(emitted("own.txt"),
                   "Not The File Name|enormous|.yaml|site.yaml") == 0,
        emitted("own.txt"));
    ok_("a data file's own field is readable",
        emitted("title.txt") && strcmp(emitted("title.txt"), "A Directory") == 0,
        emitted("title.txt"));

    /* A .md file's text is in `body`, and was NEVER compiled — the `---` and
     * the `{{ }}` in it are prose, and reach the entry as prose. */
    const char *notes = emitted("notes.txt");
    ok_("a .md file's text lands in body, uncompiled",
        notes && strncmp(notes, "A {{ literal }} in prose.", 24) == 0, notes);
    ok_("...a `---` line in it is prose, not a document separator",
        mdy_engine_count(e) == 6 && strstr(notes ? notes : "", "\n---\n") != NULL, notes);
    ok_("...byte for byte, fence and all",
        notes && strstr(notes, "```\n# not a tag: a shell comment in a fence\n```\n") != NULL,
        notes);
    ok_("a .md file's hashtags are extracted, lowercased and deduplicated",
        emitted("tags.txt") && strcmp(emitted("tags.txt"), "uruk") == 0,
        emitted("tags.txt"));

    ok_("dist/ and dotfiles are not sources", mdy_engine_count(e) == 6, NULL);

    ok_("$.tokenize drops stopwords, shorts and repeats",
        emitted("words.txt") && strcmp(emitted("words.txt"), "walls,uruk") == 0,
        emitted("words.txt"));
    ok_("$.rfc822 gives an RSS pubDate",
        emitted("date.txt") &&
            strcmp(emitted("date.txt"), "Sat, 05 Sep 2026 00:00:00 GMT") == 0,
        emitted("date.txt"));

    mdy_engine_free(e);
    fsx_rm_rf(root);
    free(root);
}


/* ---- the collector, and values this engine has just built --------------------
 *
 * The engine hands the VM values it makes itself: a record's keys, a tree's
 * nodes, a query's results. A value reachable only from the C stack is
 * invisible to the collector, and freeing one does not crash — the cell is
 * reused for the next string, and a property quietly becomes a DIFFERENT
 * property.
 *
 * The site that found this had a 642-key record come back with one key gone
 * and another present twice, so a single link out of nine hundred pointed at
 * the wrong page. Nothing else was wrong with the build.
 *
 * These run with the collector at every safe point, which is what makes the
 * failure certain rather than occasional. Run the whole suite that way too:
 * `MDY_GC_STRESS=1 make check-engine`.
 */
static void gc_checks(void) {
    printf("\n--- engine: values the collector can see ---\n");

    /*
     * The shape the real failure had: a large record read from the store,
     * handed to ANOTHER document as its `req`, and read back there after
     * enough rendering in between to have collected several times.
     */
    char *source = malloc(900000);
    if (!source) { printf("  FAIL  out of memory\n"); failures++; return; }
    size_t at = 0;
    at += (size_t)sprintf(source + at,
        "%% const d = $.findOne({ path: 'r.mdy' })\n"
        "%% for (let round = 0; round < 12; round++) {\n"
        "%%   $.text({ path: 'filler.mdy' }, { n: round })\n"
        "%% }\n"
        "%% $.emit('bad', $.text({ path: 'reader.mdy' }, { map: d.map }))\n"
        "---\n+++\npath: filler.mdy\n+++\n"
        "%% let junk = []\n"
        "%% for (let i = 0; i < 400; i++) { junk.push({ a: 'x' + i + req.n, b: [i], c: { d: 'y' + i } }) }\n"
        "filler\n"
        "---\n+++\npath: reader.mdy\n+++\n"
        "%% let bad = 0\n"
        "%% for (let i = 0; i < 800; i++) { if (req.map['k' + i] !== 'v' + i) bad++ }\n"
        "{{ String(bad) + '/' + Object.keys(req.map).length }}\n"
        "---\n+++\npath: r.mdy\nmap:\n");
    for (int i = 0; i < 800; i++)
        at += (size_t)sprintf(source + at, "  k%d: v%d\n", i, i);
    at += (size_t)sprintf(source + at, "+++\n");

    mdy_engine *e = mdy_engine_new();
    char err[256];
    emit_count = 0;
    mdy_engine_on_emit(e, collect_all, NULL);
    char *html = NULL;
    if (mdy_engine_open(e, source, at, err, sizeof err) == 0)
        html = mdy_engine_render(e, 0, err, sizeof err);
    free(source);
    free(html);

    const char *got = emitted("bad");
    ok_("every key of a large record survives collection",
        got && strcmp(got, "0/800") == 0, got ? got : err);
    mdy_engine_free(e);
}


/* Where a `$.publish` landed, for the checks below. */
static char last_message[512];
static int message_count;

static void collect_message(void *ud, const char *name, const char *data_json,
                            size_t doc_index) {
    (void)ud;
    snprintf(last_message, sizeof last_message, "%s %s from %zu", name, data_json, doc_index);
    message_count++;
}

static void natives_checks(void) {
    printf("\n--- engine: the rest of `$` ---\n");

    /*
     * The tree a document did not write itself. All of these end in a token
     * except `$.parse`, which hands back the TREE — its whole purpose is to be
     * looked at — and `$.html`, which is the way back out to text.
     *
     * Every expectation here was taken from `node bin/mdy.js build` on the
     * same source.
     */
    /*
     * `$.html` returns a STRING, and a string written into a document is
     * TEXT — so the markup in it is escaped when the document is read, and an
     * `= ` at the start of the line still opens a heading. That is the whole
     * reason `$.node` and the rest hand back tokens instead: a token is a
     * tree, and a tree goes in as nodes.
     *
     * Both expectations here were wrong the first time and both were taken
     * from `node bin/mdy.js build` on this exact source.
     */
    check("$.parse reads MDY and gives back a tree",
          "{{ $.html($.parse('= A heading')) }}",
          "<h1 id=\"a-heading\">A heading&#x3C;/h1></h1>");
    check("$.markdown is the other front end",
          "{{ $.markdown('# Md heading') }}",
          "<h1 id=\"md-heading\">Md heading</h1>");
    check("$.node parks a tree the document built",
          "{{ $.node({ type: 'element', tagName: 'p', properties: { className: ['x'] },"
          " children: [{ type: 'text', value: 'built' }] }) }}",
          "<p class=\"x\">built</p>");
    check("$.table, with a cell read as MDY",
          "{{ $.table([['Name'], ['**old**']]) }}",
          "<table><thead><tr><th>Name</th></tr></thead>"
          "<tbody><tr><td><strong>old</strong></td></tr></tbody></table>");
    check("...and an alignment per column",
          "{{ $.table([['A', 'B']], ['left', 'right']) }}",
          "<table><thead><tr><th style=\"text-align: left\">A</th>"
          "<th style=\"text-align: right\">B</th></tr></thead></table>");
    check("a table cell that is not one paragraph stays the text it was",
          "{{ $.table([['H'], ['- one\\n- two']]) }}",
          "<table><thead><tr><th>H</th></tr></thead>"
          "<tbody><tr><td>- one\n- two</td></tr></tbody></table>");
    check("$.html turns a token in a string into its HTML",
          "% const frag = $.render(1)\n{{ $.html('before ' + frag + ' after') }}\n"
          "---\n**frag**\n",
          "<p>before &#x3C;p>&#x3C;strong>frag&#x3C;/strong>&#x3C;/p> after</p>");

    /*
     * `$.toc()` returns a token before there is anything to put in it, and
     * that is the point: the list has to be able to name a heading a loop
     * writes BELOW it.
     */
    check("$.toc() names a heading written after it",
          "< nav\n  {{ $.toc() }}\n% for (const c of ['Uruk', 'Akkad']) {\n== {{ c }}\n% }\n",
          "<nav>\n<ul><li><a href=\"#uruk\">Uruk</a></li>"
          "<li><a href=\"#akkad\">Akkad</a></li></ul>\n</nav>"
          "<h2 id=\"uruk\">Uruk</h2><h2 id=\"akkad\">Akkad</h2>");
    check("...and nests a deeper heading, then comes back OUT of it",
          "{{ $.toc() }}\n= One\n== Two\n= Three\n",
          "<ul><li><a href=\"#one\">One</a><ul><li><a href=\"#two\">Two</a></li></ul></li>"
          "<li><a href=\"#three\">Three</a></li></ul>"
          "<h1 id=\"one\">One</h1><h2 id=\"two\">Two</h2><h1 id=\"three\">Three</h1>");
    /* A heading the parser did not give an id to cannot be linked, so it is
     * not listed. Raw HTML is how one gets written. */
    check("...and skips a heading with no id to link to",
          "{{ $.toc() }}\n<h2>Raw heading\n= Real\n",
          "<ul><li><a href=\"#real\">Real</a></li></ul>"
          "<h2>Raw heading</h2><h1 id=\"real\">Real</h1>");
    check("...and disappears entirely when nothing can be listed",
          "{{ $.toc() }}\n<p>plain\n", "<p>plain</p>");
    check("$.toc(text) is a question, not a token",
          "{{ JSON.stringify($.toc('= One\\n\\n== Two')) }}",
          "<p>[{\"depth\":1,\"text\":\"One\",\"slug\":\"one\"},"
          "{\"depth\":2,\"text\":\"Two\",\"slug\":\"two\"}]</p>");
    check("...and takes a rendered document too",
          "{{ JSON.stringify($.toc($.render(1))) }}\n---\n= Zed\n",
          "<p>[{\"depth\":1,\"text\":\"Zed\",\"slug\":\"zed\"}]</p>");

    refuses("$.node wants a hast node", "{{ $.node('nope') }}",
            "expects a hast node");
    refuses("$.table wants rows", "{{ $.table('nope') }}",
            "array of row arrays");
    refuses("$.toc wants something it can read", "{{ $.toc(42) }}",
            "expects MDY text, a hast node, or a rendered document");

    /* ---- publishing ---- */
    {
        const char *source =
            "% $.publish('handlers.invoice', { total: 3 })\n"
            "---\n+++\npath: handlers/invoice.mdy\next: .mdy\n+++\n= Invoice\n";
        mdy_engine *e = mdy_engine_new();
        char err[256];
        message_count = 0;
        last_message[0] = '\0';
        mdy_engine_on_publish(e, collect_message, NULL);
        char *html = NULL;
        if (mdy_engine_open(e, source, strlen(source), err, sizeof err) == 0)
            html = mdy_engine_render(e, 0, err, sizeof err);
        ok_("a page's name is its path without the extension",
            message_count == 1 &&
                strcmp(last_message, "handlers.invoice {\"total\":3} from 1") == 0,
            last_message[0] ? last_message : err);
        free(html);
        mdy_engine_free(e);
    }

    refuses("publishing to a name no document answers to",
            "% $.publish('nowhere.at.all', {})\n",
            "no document is named \"nowhere.at.all\"");
    refuses("...or to one that several share",
            "% $.publish('x', {})\n"
            "---\n+++\npath: a/one.mdy\next: .mdy\nmessageName: x\n+++\n= A\n"
            "---\n+++\npath: b/two.mdy\next: .mdy\nmessageName: x\n+++\n= B\n",
            "is ambiguous");
    refuses("...or to a name that is not one",
            "% $.publish('bad name!', {})\n",
            "may only contain letters, digits");
    /* A record with nothing to run is not an endpoint — which is also what
     * stops static/logo.png and static/logo.jpg colliding on static.logo. */
    refuses("a record that is not a page is not addressable",
            "% $.publish('static.logo', {})\n"
            "---\n+++\npath: static/logo.png\next: .png\n+++\n",
            "no document is named \"static.logo\"");
}


/* Bytes a `$.resize` produced, for the checks below. */
static char last_image_path[256];
static size_t last_image_len;
static int image_count;
static uint8_t last_image[65536];

static void collect_image(void *ud, const char *path, const uint8_t *bytes, size_t len) {
    (void)ud;
    snprintf(last_image_path, sizeof last_image_path, "%s", path);
    last_image_len = len < sizeof last_image ? len : sizeof last_image;
    memcpy(last_image, bytes, last_image_len);
    image_count++;
}

/* A real PNG, built here rather than checked in: 4 bytes of header a decoder
 * would reject is not a test of a decoder. */
static void write_png(const char *root, const char *rel, int w, int h) {
    /* Uncompressed deflate blocks, so no compressor is needed — a valid zlib
     * stream is a 2-byte header, stored blocks, and an Adler-32. */
    size_t raw_len = (size_t)h * (1 + (size_t)w * 4);
    uint8_t *raw = malloc(raw_len);
    if (!raw) return;
    size_t at = 0;
    for (int y = 0; y < h; y++) {
        raw[at++] = 0;                       /* filter: none */
        for (int x = 0; x < w; x++) {
            raw[at++] = (uint8_t)((x * 37) & 0xFF);
            raw[at++] = (uint8_t)((y * 53) & 0xFF);
            raw[at++] = 128;
            raw[at++] = 255;
        }
    }
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    uint32_t adler = (b << 16) | a;

    size_t z_cap = raw_len + raw_len / 65535 * 5 + 64;
    uint8_t *z = malloc(z_cap);
    if (!z) { free(raw); return; }
    size_t zn = 0;
    z[zn++] = 0x78; z[zn++] = 0x01;
    size_t left = raw_len, off = 0;
    while (left > 0 || raw_len == 0) {
        uint16_t chunk = left > 65535 ? 65535 : (uint16_t)left;
        z[zn++] = (uint8_t)((left <= 65535) ? 1 : 0);
        z[zn++] = (uint8_t)(chunk & 0xFF);
        z[zn++] = (uint8_t)(chunk >> 8);
        z[zn++] = (uint8_t)(~chunk & 0xFF);
        z[zn++] = (uint8_t)((~chunk >> 8) & 0xFF);
        memcpy(z + zn, raw + off, chunk);
        zn += chunk; off += chunk; left -= chunk;
        if (left == 0) break;
    }
    z[zn++] = (uint8_t)(adler >> 24); z[zn++] = (uint8_t)(adler >> 16);
    z[zn++] = (uint8_t)(adler >> 8);  z[zn++] = (uint8_t)adler;

    static const uint32_t CRC_POLY = 0xEDB88320u;
    uint32_t table[256];
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (CRC_POLY ^ (c >> 1)) : (c >> 1);
        table[i] = c;
    }
    uint8_t *png = malloc(zn + 128);
    if (!png) { free(raw); free(z); return; }
    size_t pn = 0;
    memcpy(png + pn, "\x89PNG\r\n\x1a\n", 8); pn += 8;
    /* One chunk: length, type, payload, CRC over type+payload. */
    #define PUT_CHUNK(type, payload, plen) do { \
        uint32_t L = (uint32_t)(plen); \
        png[pn++] = (uint8_t)(L >> 24); png[pn++] = (uint8_t)(L >> 16); \
        png[pn++] = (uint8_t)(L >> 8);  png[pn++] = (uint8_t)L; \
        size_t cs = pn; \
        memcpy(png + pn, (type), 4); pn += 4; \
        if (plen) memcpy(png + pn, (payload), (plen)); \
        pn += (plen); \
        uint32_t c = 0xFFFFFFFFu; \
        for (size_t i = cs; i < pn; i++) c = table[(c ^ png[i]) & 0xFF] ^ (c >> 8); \
        c ^= 0xFFFFFFFFu; \
        png[pn++] = (uint8_t)(c >> 24); png[pn++] = (uint8_t)(c >> 16); \
        png[pn++] = (uint8_t)(c >> 8);  png[pn++] = (uint8_t)c; \
    } while (0)
    uint8_t ihdr[13] = {
        (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
        8, 6, 0, 0, 0,
    };
    PUT_CHUNK("IHDR", ihdr, 13);
    PUT_CHUNK("IDAT", z, zn);
    PUT_CHUNK("IEND", NULL, 0);
    #undef PUT_CHUNK

    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    char *slash = strrchr(path, '/');
    if (slash) { *slash = '\0'; fsx_mkdirp(path); *slash = '/'; }
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(png, 1, pn, f); fclose(f); }
    free(raw); free(z); free(png);
}

static void resize_checks(void) {
    printf("\n--- engine: $.resize ---\n");

    char *tmp = fsx_tmpdir();
    char prefix[1024];
    snprintf(prefix, sizeof prefix, "%s/mdy-resize", tmp ? tmp : ".");
    free(tmp);
    char *root = fsx_mkdtemp(prefix);
    if (!root) { printf("  FAIL  cannot make a temp directory\n"); failures++; return; }

    write_png(root, "static/logo.png", 64, 40);
    write_file(root, "notes.md", "not an image\n");
    write_file(root, "main.mdy",
        "% const logo = $.findOne({ path: 'static/logo.png' })\n"
        "% $.emit('dims.txt', logo.width + 'x' + logo.height)\n"
        "% const a = $.resize(logo, { width: 20 })\n"
        "% $.emit('a.txt', [a.path, a.url, a.width, a.height].join('|'))\n"
        "% const b = $.resize(logo, { height: 10 })\n"
        "% $.emit('b.txt', [b.path, b.url, b.width, b.height].join('|'))\n"
        "% const again = $.resize(logo, { width: 20 })\n"
        "% $.emit('memo.txt', String(again.path === a.path))\n");

    mdy_engine *e = mdy_engine_new();
    char err[512];
    emit_count = 0;
    image_count = 0;
    last_image_path[0] = '\0';
    mdy_engine_on_emit(e, collect_all, NULL);
    mdy_engine_on_binary(e, collect_image, NULL);

    char *html = NULL;
    if (mdy_engine_open_dir(e, root, err, sizeof err) == 0) {
        int at = mdy_engine_entry(e, "main.mdy");
        if (at >= 0) html = mdy_engine_render(e, (size_t)at, err, sizeof err);
    }
    if (!html) {
        printf("  FAIL  the entry renders\n      %s\n", err);
        failures++;
        mdy_engine_free(e);
        fsx_rm_rf(root);
        free(root);
        return;
    }
    free(html);

    /* An image's dimensions are read from its header during the walk — a
     * record without them cannot be resized at all. */
    ok_("an image file's record carries its dimensions",
        emitted("dims.txt") && strcmp(emitted("dims.txt"), "64x40") == 0,
        emitted("dims.txt"));

    /*
     * The output path is DIST-relative: `static/` is stripped, because a build
     * copies static/'s contents to the output root and a resized file has to
     * land in the same flattened space or its URL would not match.
     */
    ok_("a resize names where it landed, with static/ flattened away",
        emitted("a.txt") && strcmp(emitted("a.txt"),
                                   "logo-20x13.png|/logo-20x13.png|20|13") == 0,
        emitted("a.txt"));
    ok_("...deriving the other side from the aspect ratio",
        emitted("b.txt") && strcmp(emitted("b.txt"),
                                   "logo-16x10.png|/logo-16x10.png|16|10") == 0,
        emitted("b.txt"));
    ok_("...and asking twice decodes once",
        emitted("memo.txt") && strcmp(emitted("memo.txt"), "true") == 0 && image_count == 2,
        emitted("memo.txt"));

    /* The bytes are a real PNG of the size asked for — checked by reading the
     * header back, because "it wrote something" is not the claim. */
    int ok_png = last_image_len > 24 &&
                 memcmp(last_image, "\x89PNG\r\n\x1a\n", 8) == 0;
    int w = 0, h = 0;
    if (ok_png) {
        w = (int)((last_image[16] << 24) | (last_image[17] << 16) |
                  (last_image[18] << 8) | last_image[19]);
        h = (int)((last_image[20] << 24) | (last_image[21] << 16) |
                  (last_image[22] << 8) | last_image[23]);
    }
    char detail[128];
    snprintf(detail, sizeof detail, "%s %dx%d (%zu bytes)",
             last_image_path, w, h, last_image_len);
    ok_("...and the bytes really are a PNG of that size",
        ok_png && w == 16 && h == 10, detail);

    mdy_engine_free(e);
    fsx_rm_rf(root);
    free(root);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[main]\n");
    printf("--- engine: a document, end to end ---\n");

    check("plain markup", "= Hello\n\ntext", "<h1 id=\"hello\">Hello</h1><p>text</p>");

    check("a `%` line runs, and writes nothing itself",
          "% const x = 1\n= Value {{ x }}",
          "<h1 id=\"value-1\">Value 1</h1>");

    check("a loop encloses the markup under it",
          "% for (const n of [1, 2, 3]) {\n- item {{ n }}\n% }",
          "<ul>\n<li>item 1</li>\n<li>item 2</li>\n<li>item 3</li>\n</ul>");

    check("front matter is data, not content",
          "+++\ntitle: A\n+++\n= Body",
          "<h1 id=\"body\">Body</h1>");

    check("a ```data fence comes out of the body",
          "= Title\n\n```data\nextra: 1\n```\n\ntail",
          "<h1 id=\"title\">Title</h1><p>tail</p>");

    check("an empty document renders to nothing", "", "");

    check("inline markup and a wiki link",
          "//em// and **strong** and [[ Ancient Egypt ]]",
          "<p><em>em</em> and <strong>strong</strong> and "
          "<a href=\"ancient-egypt\">Ancient Egypt</a></p>");

    /* The three characters a template literal cannot hold plainly — `$`, a
     * backtick and a backslash — survive the script layer's escaping. MDY's
     * code marker is a DOUBLE backtick; a single one is text, which is what
     * this checks and what I first got wrong. */
    check("a template literal's own escapes survive",
          "cost: $5 and `tick` and \\\\slash",
          "<p>cost: $5 and `tick` and \\slash</p>");
    check("…and a double backtick is code",
          "and ``tick`` here",
          "<p>and <code>tick</code> here</p>");

    printf("--- engine: several documents in one source ---\n");
    {
        const char *source = "= One\n---\n= Two";
        mdy_engine *e = mdy_engine_new();
        char open_err[256];
        mdy_engine_open(e, source, strlen(source), open_err, sizeof open_err);
        int ok = mdy_engine_count(e) == 2;
        printf("  %s  a source holds two documents\n", ok ? "ok  " : "FAIL");
        if (!ok) failures++;

        char err[256];
        char *second = mdy_engine_render(e, 1, err, sizeof err);
        ok = second && strcmp(second, "<h1 id=\"two\">Two</h1>") == 0;
        printf("  %s  …and the second one renders\n", ok ? "ok  " : "FAIL");
        if (!ok) { printf("      actual %s\n", second ? second : err); failures++; }
        free(second);
        mdy_engine_free(e);
    }

    printf("--- engine: the set, queried ---\n");

    /*
     * Every document's DATA goes into nisaba when the set is opened — its
     * front matter merged with its ```data fences, and never its text — and
     * `$.find` runs a real query against it.
     */
    {
        const char *set =
            "% for (const c of $.find({ role: 'city' })) {\n"
            "- {{ c.who }} of {{ c.era }}\n"
            "% }\n"
            "---\n+++\nrole: city\nwho: Uruk\nera: Sumer\n+++\n"
            "---\n+++\nrole: card\nwho: Ignored\n+++\n"
            "---\n+++\nrole: city\nwho: Babylon\nera: Akkad\n+++\n";
        check("a document finds its siblings by query", set,
              "<ul>\n<li>Uruk of Sumer</li>\n<li>Babylon of Akkad</li>\n</ul>");
    }

    check("findOne takes the first hit",
          "= {{ $.findOne({ role: 'city' }).who }}\n"
          "---\n+++\nrole: city\nwho: Uruk\n+++\n"
          "---\n+++\nrole: city\nwho: Babylon\n+++\n",
          "<h1 id=\"uruk\">Uruk</h1>");

    check("…and answers null when nothing matches",
          "= {{ $.findOne({ role: 'nowhere' }) === null ? 'none' : 'some' }}",
          "<h1 id=\"none\">none</h1>");

    check("an empty query finds every document",
          "= {{ $.find({}).length }}\n---\n+++\na: 1\n+++\n---\n+++\nb: 2\n+++\n",
          "<h1 id=\"3\">3</h1>");

    /*
     * Several hits, in the order the documents are written — with paths that
     * sort backwards against them, so a straight index walk would answer
     * three,two,one.
     *
     * BE CLEAR ABOUT WHAT THIS DOES NOT PROVE. It passes with the ordering
     * pass removed, so it does not isolate it: nisaba's ObjectIds are
     * monotonic, so the primary tree already walks in insertion order and an
     * index ties break by `_id`, which is the same order again. I could not
     * construct a case that tells them apart, and a check that cannot fail is
     * worth less than knowing it cannot. The sort stays because mdy-docs sorts
     * — it is defensive about an order neither of us should rely on — and the
     * `_id` to index map earns its place regardless: resolving a hit back to
     * its document is what `$.render({ … })` will need.
     */
    check("several hits come back with their documents, in order",
          "= {{ $.find({ path: { $exists: true } }).map((d) => d.n).join(',') }}\n"
          "---\n+++\npath: c.mdy\nn: one\n+++\n"
          "---\n+++\npath: b.mdy\nn: two\n+++\n"
          "---\n+++\npath: a.mdy\nn: three\n+++\n",
          "<h1 id=\"onetwothree\">one,two,three</h1>");

    check("a ```data fence is queryable too",
          "= {{ $.findOne({ kind: 'note' }).title }}\n"
          "---\ntext\n\n```data\nkind: note\ntitle: Found\n```\n",
          "<h1 id=\"found\">Found</h1>");

    check("a fence overrides the front matter it merges over",
          "= {{ $.findOne({ role: 'r' }).size }}\n"
          "---\n+++\nrole: r\nsize: 4\n+++\nbody\n\n```data\nsize: 9\n```\n",
          "<h1 id=\"9\">9</h1>");

    check("$.data reaches a document by index",
          "= {{ $.data(1).who }}\n---\n+++\nwho: Uruk\n+++\n",
          "<h1 id=\"uruk\">Uruk</h1>");

    check("a number in front matter stays a number",
          "= {{ typeof $.findOne({ n: 42 }).n }}\n---\n+++\nn: 42\n+++\n",
          "<h1 id=\"number\">number</h1>");

    check("the body text is not in the database",
          "= {{ JSON.stringify($.findOne({ role: 'r' })).includes('secret') ? 'leaked' : 'clean' }}\n"
          "---\n+++\nrole: r\n+++\nthe secret body\n",
          "<h1 id=\"clean\">clean</h1>");

    printf("--- engine: composition ---\n");

    /*
     * `$.render` returns a TOKEN, not HTML — a few private-use characters
     * standing for a tree the host parked. The token travels through the
     * document's own code like any other string, and the tree goes back in
     * once the text around it has been parsed. That is why a render needs no
     * indentation argument: the parser already knows which element is open
     * where the token landed.
     */
    check("a render on a line of its own becomes that document",
          "{{ $.render(1) }}\n---\n= Card\n",
          "<h1 id=\"card\">Card</h1>");

    check("…and is not wrapped in a paragraph",
          "before\n\n{{ $.render(1) }}\n\nafter\n---\n= Card\n",
          "<p>before</p><h1 id=\"card\">Card</h1><p>after</p>");

    /* A block cannot sit inside a sentence, so it gives up its wrapper and
     * lends its content instead. */
    check("a render inside a sentence gives up its blocks",
          "say {{ $.render(1) }} now\n---\ninner\n",
          "<p>say inner now</p>");

    check("a render by query",
          "{{ $.render({ role: 'card' }) }}\n---\n+++\nrole: card\n+++\n= Found\n",
          "<h1 id=\"found\">Found</h1>");

    check("a render of a document a find returned",
          "% const c = $.findOne({ role: 'card' })\n{{ $.render(c) }}\n"
          "---\n+++\nrole: card\n+++\n= By reference\n",
          "<h1 id=\"by-reference\">By reference</h1>");

    check("several renders in a loop",
          "% for (const c of $.find({ role: 'city' })) {\n"
          "{{ $.render(c) }}\n"
          "% }\n"
          "---\n+++\nrole: city\n+++\n= Uruk\n"
          "---\n+++\nrole: city\n+++\n= Babylon\n",
          "<h1 id=\"uruk\">Uruk</h1><h1 id=\"babylon\">Babylon</h1>");

    check("a render nested two deep",
          "{{ $.render(1) }}\n---\nouter {{ $.render(2) }}\n---\ninner\n",
          "<p>outer inner</p>");

    /*
     * `$.text` is the text a document's code WROTE, not its tree's text — it
     * is never read as MDY on the way past. So the markup comes back intact
     * and is read by whoever received it, here the heading.
     *
     * This check asserted the opposite until a real site disagreed: a document
     * that writes JSON had `\"` inside a caption turned into `"`, and what
     * came back was no longer JSON. `node bin/mdy.js build` on this same
     * source produces the markup below.
     */
    check("$.text gives what a document wrote, markup and all",
          "= {{ $.text(1) }}\n---\n**bold** and //em//\n",
          "<h1 id=\"bold-and-em\"><strong>bold</strong> and <em>em</em></h1>");

    /*
     * The reason it must not be parsed, and the shape a real site uses: a
     * document writes JSON, and its reader parses it. Read it as MDY on the
     * way past and `\"` inside a string comes back as `"` — no longer JSON,
     * and the failure lands in the caller, far from the cause.
     */
    check("...so JSON a document wrote parses back, escapes intact",
          "% const o = JSON.parse($.text(1))\n= {{ o.q }}\n---\n"
          "{{ JSON.stringify({ q: 'a \"b\" c' }) }}\n",
          "<h1 id=\"a-b-c\">a \"b\" c</h1>");

    check("a token in a transformed document is still composed",
          "%% transform((tree) => {\n"
          "  visit(tree, 'h1', (node) => { node.tagName = 'h2'; });\n"
          "})\n"
          "{{ $.render(1) }}\n---\n= Card\n",
          "<h2 id=\"card\">Card</h2>");

    /* `$.render(target, data)` hands the target its `req`. */
    check("a render passes its data as req",
          "{{ $.render(1, { who: 'Uruk' }) }}\n---\n= {{ req.who }}\n",
          "<h1 id=\"uruk\">Uruk</h1>");

    check("…and the same document answers differently each time",
          "% for (const who of ['Uruk', 'Babylon']) {\n"
          "{{ $.render(1, { who }) }}\n"
          "% }\n---\n= {{ req.who }}\n",
          "<h1 id=\"uruk\">Uruk</h1><h1 id=\"babylon\">Babylon</h1>");

    check("a document with no request sees an empty one",
          "= {{ Object.keys(req).length }}", "<h1 id=\"0\">0</h1>");

    refuses("a render that cycles", "{{ $.render(0) }}", "render depth exceeded");

    /*
     * The shape a site actually has: a loop over queried documents, each
     * rendered through a shared layout with its own data, each emitted to its
     * own path — and the layout transforming its own tree on the way.
     */
    {
        last_emit_path[0] = last_emit_content[0] = '\0';
        mdy_engine *e = mdy_engine_new();
        mdy_engine_on_emit(e, collect_emit, NULL);
        char err[256];
        const char *source =
            "% for (const c of $.find({ role: 'city' })) {\n"
            "%   $.emit(c.slug + '/index.html', $.render({ role: 'layout' }, { who: c.who }))\n"
            "% }\n"
            "---\n+++\nrole: layout\n+++\n"
            "%% transform((tree) => { visit(tree, 'h1', (n) => { n.properties.className = ['title']; }); })\n"
            "= {{ req.who }}\n"
            "---\n+++\nrole: city\nwho: Uruk\nslug: uruk\n+++\n"
            "---\n+++\nrole: city\nwho: Babylon\nslug: babylon\n+++\n";
        int ok = mdy_engine_open(e, source, strlen(source), err, sizeof err) == 0;
        if (ok) { char *html = mdy_engine_render(e, 0, err, sizeof err); ok = html != NULL; free(html); }
        ok = ok && strcmp(last_emit_path, "babylon/index.html") == 0 &&
             /* `id` then `class`: the parser sets the id, the transform adds
              * the class, and that is the order. Taken from what
              * `node bin/mdy.js build` writes for this exact site. */
             strcmp(last_emit_content, "<h1 id=\"babylon\" class=\"title\">Babylon</h1>") == 0;
        printf("  %s  a query, a shared layout, a transform and an emit each\n", ok ? "ok  " : "FAIL");
        if (!ok) { printf("      last path %s\n      last content %s\n      err %s\n",
                          last_emit_path, last_emit_content, err); failures++; }
        mdy_engine_free(e);
    }

    printf("--- engine: emit ---\n");
    {
        /* A build writes a file, a server holds it, this collects it — mdy
         * has no opinion on what producing an output means. */
        last_emit_path[0] = last_emit_content[0] = '\0';

        mdy_engine *e = mdy_engine_new();
        mdy_engine_on_emit(e, collect_emit, NULL);
        char err[256];
        const char *source =
            "% $.emit('index.html', $.render(1))\n"
            "---\n= Page\n";
        if (mdy_engine_open(e, source, strlen(source), err, sizeof err) == 0) {
            char *html = mdy_engine_render(e, 0, err, sizeof err);
            free(html);
        }
        int ok = strcmp(last_emit_path, "index.html") == 0 &&
                 strcmp(last_emit_content, "<h1 id=\"page\">Page</h1>") == 0;
        printf("  %s  a token in emitted content becomes its HTML\n", ok ? "ok  " : "FAIL");
        if (!ok) { printf("      path %s\n      content %s\n", last_emit_path, last_emit_content); failures++; }
        mdy_engine_free(e);
    }

    printf("--- engine: transform, the tree through lamassu and back ---\n");

    /*
     * The one place a document's own code sees its tree. The host parses the
     * lines, hands the tree to the guest as VALUES, the guest changes it, and
     * the host takes it back — no JSON in either direction.
     */
    check("a transform sees the tree and can change it",
          "%% transform((tree) => {\n"
          "  visit(tree, 'h1', (node) => { node.tagName = 'h2'; });\n"
          "})\n"
          "= Title",
          "<h2 id=\"title\">Title</h2>");

    check("…and can set a property",
          "%% transform((tree) => {\n"
          "  visit(tree, 'p', (node) => { node.properties.className = ['lead']; });\n"
          "})\n"
          "text",
          "<p class=\"lead\">text</p>");

    check("…and can read the text through the toolkit",
          "%% transform((tree) => {\n"
          "  visit(tree, 'h1', (node) => { node.properties.id = slug(toText(node)); });\n"
          "})\n"
          "= A Long Title",
          "<h1 id=\"a-long-title\">A Long Title</h1>");

    check("…and can return a new tree",
          "%% transform(() => ({ type: 'element', tagName: 'main', properties: {},\n"
          "  children: [{ type: 'text', value: 'replaced' }] }))\n"
          "= Gone",
          "<main>replaced</main>");

    check("…and can add a node with h()",
          "%% transform((tree) => {\n"
          "  tree.children.push(h('footer.note', 'end'));\n"
          "})\n"
          "text",
          "<p>text</p><footer class=\"note\">end</footer>");

    check("a document with no transform takes the shorter path",
          "= Untouched", "<h1 id=\"untouched\">Untouched</h1>");

    /*
     * ATTRIBUTE ORDER through the round trip, which is worth pinning because
     * it looks like a bug and is not.
     *
     * An element written `href class rel title` comes back `href title class
     * rel` after passing through a transform — and mdy-docs does exactly the
     * same, under node and under the QuickJS build alike. The expectation
     * below is what `node bin/mdy.js build` produces for this document, taken
     * from it rather than reasoned out.
     *
     * (lamassu does not keep objects in insertion order, which the language
     * requires — see js_object_key_at's note. It does not decide this case,
     * but it is a real gap and this is the check that would notice if it
     * started to.)
     */
    check("attribute order matches what mdy-docs produces",
          "%% transform((tree) => {})\n"
          "<a href=\"/x\" class=\"one two\" rel=\"noopener\" title=\"t\">link",
          "<a href=\"/x\" title=\"t\" class=\"one two\" rel=\"noopener\">link</a>");

    refuses("a transform that returns something that is not a node",
            "%% transform(() => 42)\n= x", "transform must return a hast node");

    printf("--- engine: what it refuses, loudly ---\n");
    /* Nothing refuses any more — every native mdy-docs documents is here.
     * What is left to check is that each one says clearly what it wanted. */
    refuses("$.resize wants a file document",
            "{{ $.resize('nope', {}) }}",
            "expected a file document (path/ext, from $.find/$.findOne)");
    refuses("a render of a document that is not there",
            "{{ $.render({ role: 'nowhere' }) }}", "found no such document");
    refuses("code that does not compile", "% const = \n= x", "did not compile");
    refuses("code that throws", "% throw 'nope'\n= x", "nope");
    {
        /* `refuses` always asks for document 0, and `= One` has one — so this
         * one asks for an index that genuinely is not there. */
        mdy_engine *e = mdy_engine_new();
        char err[256];
        mdy_engine_open(e, "= One", 5, err, sizeof err);
        char *html = mdy_engine_render(e, 5, err, sizeof err);
        int ok = !html && strstr(err, "no document at index") != NULL;
        printf("  %s  a document index that is not there\n", ok ? "ok  " : "FAIL");
        if (!ok) { printf("      actual %s\n", html ? html : err); failures++; }
        free(html);
        mdy_engine_free(e);
    }

    site_checks();
    natives_checks();
    resize_checks();
    gc_checks();

    if (failures) { printf("\n%d failed\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
