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

static int failures;

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

int main(void) {
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
    refuses("a native that is not implemented yet",
            "{{ $.render({ role: 'card' }) }}", "$.render is not implemented");
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

    if (failures) { printf("\n%d failed\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
