# mdy-native

The mdy engine as a binary. The directory walk, the document store, the script
layer, the parser, composition and the HTML are C; **lamassu runs only the code
a document itself writes**, and no other JavaScript engine is linked.

```sh
make build/mdy-build             # the engine, as a command
./build/mdy-build <dir> --out <dir>

make check-engine                # its own checks, twice — see the target
make check-sites                 # every site here, built BOTH ways and diffed
make check-site SITE=<dir>       # one site, the same way
```

Everything is built from source and nothing comes from a system package:
lamassu, nisaba and the MDY front end are submodules, and stb is vendored.

**It builds this repository's own 93-page site byte-for-byte identically to
`node bin/mdy.js build`**, in about 6 seconds against node's 16.

## This repository is a package of mdy-docs

It is `packages/mdy-native` of [mdy-docs](https://github.com/mdy-docs/mdy-docs),
exported with its history. The engine stands alone — `make build/mdy-build`,
`make check-engine` and `make check-ingest` need only this checkout and its
submodules. `make check-sites` and `make check-site` do NOT: they build a site
with mdy-docs' own JavaScript to compare against, so they want this at
`packages/mdy-native` inside an mdy-docs checkout. That comparison is the
suite, so this is worth knowing before relying on the repository alone.

Inside an mdy-docs checkout, point the two engine paths at the copies already
there rather than checking out a second pair:

```sh
make NISABA=../../third_party/nisaba-db LAMASSU=../../third_party/lamassu-js …
```

## How it is tested

By building real sites both ways and diffing every byte. `make check-sites`
does that for the five in this repository, and it is the suite — not a
supplement to one.

That is a deliberate replacement. There used to be a second binary here that
ran mdy-docs' own JavaScript in an embedded interpreter, and `make test` ran
mdy-docs' 684 test files against it. It proved a claim about that binary —
"mdy-docs runs unchanged on it" — and that binary is gone. What matters now is
whether the C engine and mdy-docs agree on real input, which is a question only
a differential test can answer. Pointing it at those five sites found eight
bugs in an engine that already built a 93-page site identically, and **not one
of them failed a unit test**: a `.yaml` file's own fields losing to derived
identity, `mtime` as a number where mdy-docs has an ISO string, `.md` files not
going through the markdown front end, no guest module loader, a cross-package
`$.resize` reading from the wrong root, `.mdy` documents getting no `tags`,
sidecars published out of `static/`, and every file but the last losing its
trailing newline.

`make check-engine` runs the unit checks twice, the second time collecting at
every GC safe point. `make check-golden` and `make check-determinism` compare
against committed output on a platform where node cannot run the build at all.

**macOS, Linux and Windows all build and produce byte-identical output**, on
every push, via [.github/workflows/native.yml](../../.github/workflows/native.yml).
Windows through MSYS2/mingw-w64: the build is a GNU makefile driving a Unix
shell, so MSVC would want a real build file rather than a CI flag.

## What is left

Two things differ from mdy-docs on purpose, and `make check-sites` pins each to
an exact count so a change in either direction fails:

- **A resized image's bytes.** mdy-docs resizes with Squoosh's codecs and this
  uses stb — a different resampler and a different encoder. Same dimensions,
  same paths, visually equivalent, different file.
- **Raw HTML through the markdown front end.** md4c does not do rehype-raw's
  HTML5 round trip, so a `.md` file that emits raw HTML for remark to stitch
  back comes out with different blank lines around the block.

And one thing is simply not implemented: **mdy-docs memoises a render** on
(document, request), and this does not. It is observable — a composition
token's id depends on how many renders came before it, and a site that indexes
its own `$.text` output indexes that number — and it is why `examples/blog`'s
search index differs by one word. It is also the larger opportunity: the
memoisation is what a site like this repository's own leans on, and the engine
currently re-renders where mdy-docs would not.

## nisaba, natively

```
--- mdy-native: nisaba ---
  insert corpus/en/uruk.mdy     : ok (rc=0, 78 bytes)
  insert corpus/en/babylon.mdy  : ok (rc=0, 84 bytes)
  find_one path=uruk     : found (78 bytes)
  document carries title : Uruk
```

The query engine, answering the same filter shape `$.find({ path: … })`
produces. Three things learned getting there.

**All of nisaba's C compiles with `cc` and no changes** — including the files
named `*_wasm.c`, whose `EMSCRIPTEN_KEEPALIVE` is a no-op off-target and which
turned out to hold the regex entry points, not just exports. `db_wasm.c` is the
one genuine exception: it is the WASM export layer, and a native host is what
replaces it.

**Its storage is not portable, and that is the seam working.** The shipped
`bjio_host(fd)` reaches into `Module.bjioHandles` — a table of JS
`FileSystemSyncAccessHandle` objects. That is the browser's storage bridged
through emscripten and unreachable natively. But `bj_io` is four callbacks
(`size`, `read`, `write`, `truncate`), so a native host writes its own; ours is
a growable buffer. What the JS side calls `MemoryStorageProvider` is a memory
store here too — it was a temp file, described in a comment as having "the
lifetime MemoryStorageProvider promises", which is a memory store with a detour
through the kernel. A persistent store is still four callbacks away.

**Index specs are a binjson ARRAY OF STRINGS**, not MongoDB's `{ field: 1 }`
object. Writing the object gets `BJ_ERR_UNKNOWN_TYPE` back, and if the return
value is dropped the index simply does not exist — which is indistinguishable
from one that works until a corpus is large enough for the full scans to show.
Ours was missing for exactly that reason, and it was 62% of a 93-page build.

**`_id` must be minted by the host.** `dc_insert_one` refuses a document
without one; the JS binding generates the ObjectId, so a native binding has to.
That is the same rule met from the other side earlier: nisaba will not accept a
scalar `_id`, and the primary tree's keys are fixed-width OID bytes.


## The MDY front end, in C

The parser is [github.com/mdy-docs/parse](https://github.com/mdy-docs/parse), a
submodule at `third_party/parse`, and the application entries are bundled
against it instead of `src/parse/block.js`. It is where a native build's time
went: a profile put every frame in the JavaScript layer, and this was the
largest single thing in it.

```
corpus, 93 pages          JS front end   C front end
                              61.0 s        45.5 s
bundle                        1.62 mb       1.48 mb
```

Measured back to back on the same machine, both bundles from the same
sources — `MDY_PARSER=js|c node scripts-build.mjs site`. The two outputs agree
byte for byte.

The comparison that matters is not the clock, though — it is that the output
does not move. All 93 pages are **byte-identical** to what `node bin/mdy.js
build` produces, and the parse repo's own harness holds 87/87 documents,
284,872 nodes, 20,681 positions and 10,514 URL inputs against the JavaScript.

### What it does not do, measured

`make test-c-parser` runs mdy-docs' own suite against the C front end and
prints what is left:

```
$ make test-c-parser
failing with the C front end: 130
    87 cannot honour `script`
    18 cannot honour `tasks`
     6 cannot honour `arrows`
     …
```

**All 130 are refusals** — `shims/parse.js` throwing a named error for an
option it cannot honour, never a quiet difference in the output. There are no
remaining cases where the two front ends are asked the same question and
answer differently. Getting there closed 54 real differences, none of which
the reference corpus reached: comments, raw-text elements, table captions,
the sanitize schema's `strip` list, void elements, task boxes, arrows, tab
stops, `mailto:`, page-link tidying, front matter, warnings, and hast's
attribute-name rules among them.

`script` is the largest refusal by far and the least alarming: it is the `%`
and `{{ }}` template layer, which runs *before* block parsing, so the site path
never asks the C parser for it. Porting it would mean putting a JavaScript
engine inside the parser to feed a JavaScript engine.

### Where the work is divided

The C parser produces more than a tree, because a front end does: it hands back
the **warnings** it raised (a dropped `<script>` is something an author is told
about), the **front matter** it found — as source, because YAML belongs to a
YAML reader and there is no case for writing a second one in C — and the
**references** a document made, its tags, mentions and page links.

`shims/parse.js` reads the YAML, applies mdy-docs' own highlighter to the
finished tree, and puts the warnings on the vfile. Highlighting after the parse
rather than inside it is why lowlight is in the bundle at all — 328 KB of
grammars, which is most of the difference between the two bundle sizes above.

It costs nothing to run here: `normalizeHighlight` totals **2 ms over 428
calls** across the corpus, because the corpus has no fenced code. An earlier
note in this file blamed a slower build on highlighting; that was wrong, and
the numbers above replace it.

Which front end a bundle gets is a choice. The application entries take the C
one; the `tests` entry does not, so `make test` measures **mdy-docs'** behaviour
rather than this parser's subset of it, and stays at 684 passing.
`MDY_PARSER=c|js` overrides either way.

## The ingest, in C

mdy-docs puts a document's **data** into nisaba and never its text —
`collection.insertOne({ ...doc.data })` in src/mdy.js, where `doc.data` is the
file's identity merged with the YAML it declares. A measured build inserts 192
documents and 4.8 MB of it, with no body anywhere.

`make check-ingest` does that whole path in C, with no JavaScript in it:

```
.mdy text
  -> mdy_data_extract     the ```data fences, and the body without them
  -> mdy_yaml_parse       front matter, and each fence
  -> mdy_bj_document      merged, with an _id, as binjson
  -> dc_insert_one        into a real nisaba collection
  -> dc_find              and back out again
```

`src/ingest.c` is the bridge: `mdy_bj_put_yaml` encodes a YAML value as
binjson, and `mdy_bj_document` writes a document — `_id` first, then every
mapping merged the way `Object.assign` merges them, a key keeping the position
of its FIRST appearance and the value of its LAST. That last rule is what lets
a ```data fence override front matter without moving it, and the test checks
it by asserting the fence's `size: 9` beats the front matter's `size: 4`.

An integral number goes in as an INT rather than a float, because a query
written `{size: 4}` does not match a document that stored `4.0`.

`_id` is a fresh ObjectId because nisaba's primary tree is keyed on
fixed-width OID bytes and refuses anything else.

## The C engine: one document, end to end

`make check-engine` renders a document with **no JavaScript engine but
lamassu**. No other JavaScript engine is linked into that binary — `nm` finds zero of
its symbols in it.

It is mdy-docs' own three passes (src/mdy.js's file comment), done in C:

```
  1. the source splits into documents on bare `---`, and each into the YAML
     at its top and the body below it, with ```data fences pulled out
                                                   mdydoc.h, mdydata.h
  2. the body's `%` and `{{ }}` lines compile to one run of statements,
     which run inside lamassu and return the lines the document produced
                                                   mdyscript.h, lamassu
  3. those lines parse to hast — the last change of representation there is
                                                   mdyast.h
  …and the tree is written as HTML                 mdyhtml.h
```

The statements are wrapped as `(async (req, res, $$) => { … })`, so `req` and
`res` arrive as VALUES and one compiled function serves every render. That is
the whole reason the script layer produces statements that never mention the
request.

**`$` is complete but for one native.** `find`, `findOne`, `withTag`, `data`,
`render`, `text`, `emit`, `compose`, `parse`, `markdown`, `node`, `html`,
`table`, `toc`, `publish`, `tokenize` and `rfc822` all work, and each was
checked against `node bin/mdy.js build` on the same source rather than against
what it looked like it should do — twice that disagreed with the expectation
written here first, and node was right both times.

`$.resize` works too, over stb (third_party/stb) — decode, resample, encode,
three vendored headers and no platform binary to prebuild, which is the same
reason lamassu and nisaba are here as source. PNG only, which is parity rather
than a shortfall: mdy-docs' own CODECS table holds one entry, because
@jsquash's JPEG codec has a different init shape and was never wired.

**It is the one place this port does not produce the same bytes.** mdy-docs
resizes with Squoosh's codecs; this uses a different resampler and a different
encoder, so the same request gives a visually equivalent image and a different
file. Measured against the JavaScript on a deliberately harsh test pattern, the
decoded pixels differ by a mean of 1.3–3.6 out of 255. Every other observable
matches exactly — the output path, the URL, the derived dimension, the
memoisation, and each error message.

There is one more divergence, in reading an image's DIMENSIONS. mdy-docs uses
the `image-size` package and hands it a `Uint8Array`; for at least some TIFFs
that throws where the same bytes as a `Buffer` do not, so the record comes back
with no width or height. This reads the header itself and gets the size. Being
bug-compatible with a third-party library's buffer handling is not worth it,
so this one is deliberate — but it is a difference, and a site laying out a
TIFF would see it.

Two of them are worth knowing about because their shape is not the obvious one:

- **`$.parse` hands back a TREE; the rest hand back tokens.** A token is how a
  finished tree travels through a document's own code, and it goes back in
  where the parser knows what is open. `$.parse` is the exception because its
  whole purpose is to be looked at. `$.html` is the way back out to text — and
  a string of HTML written into a document is TEXT, so its markup is escaped.
  That is the reason the others do not return strings.
- **`$.toc()` returns a token before there is anything to put in it.** A
  document's headings are not known until its whole tree is, so a contents
  list at the top can name a heading a loop writes below it. It is filled last,
  after the transforms, which is also why its links use the parser's own ids
  and nothing has to agree with anything.

### The set, queried

`mdy_engine_open` is mdy-docs' `openDocumentSet`: every document's data — its
front matter merged with its ```data fences, and **never its text** — goes into
a nisaba collection, and `$.find`, `$.findOne`, `$.withTag` and `$.data` run
real queries against it.

A query object crosses as binjson built from the guest's own value, and the
documents come back as guest values through binjson's decoder — no JSON in
either direction. The `path` index is attached at open for the reason mdy-docs
gives: every `$.render({ path: … })` resolves through a query on it, and
without the index each one is a scan of the whole set.

Ten checks, including the ones that would be silently wrong: a fence overriding
the front matter it merges over, a number staying a number, and the body text
being nowhere in the database.

One of them is labelled for what it does NOT prove. "Several hits come back
with their documents, in order" passes with the ordering pass removed, because
nisaba's ObjectIds are monotonic — the primary tree already walks in insertion
order, and an index ties break by `_id`, which is that order again. I could not
construct a case that tells them apart. The sort stays because mdy-docs sorts,
and the `_id` to index map earns its place regardless: resolving a hit back to
its document is what `$.render({ … })` needs.

### Composition

`$.render` does not return HTML, or a tree, or text. It returns a **token** —
`U+E000 <id> U+E001`, a few private-use characters standing for a tree the host
has parked — and the token travels through the document's own code like any
other string: into a variable, a template literal, an attribute. The tree goes
back in once the text around it has been parsed.

That is why a render needs no indentation argument. The parser already knows
which element is open where the token landed, so there is no column for the
caller to compute.

Splicing has two halves, and the difference matters:

- a paragraph holding **nothing but tokens** is REPLACED by what they hold —
  a render on a line of its own is that document, not a paragraph wrapping it;
- a token **inside a sentence** gives up its blocks instead, as far down as the
  blocks go, because a block cannot sit inside a sentence.

`$.emit(path, content)` is the string-shaped half: tokens in the content become
the HTML they stood for, because a file is a string and that is the shape it
can hold. Where an emit goes is `mdy_engine_on_emit`'s business — a build
writes a file, a server holds it, a test collects it, and mdy has no opinion.

A nested render's failure is passed through rather than wrapped again. mdy-docs
wraps at every level, so a cycle reports "document 0 failed:" thirty times over
and the reason falls off the end.

### transform: the tree through lamassu and back

`transform((tree) => …)` is the one place a document's own code sees its tree,
so the tree has to reach the guest and come back. mdy-docs sends it as JSON in
both directions; here it is built as **values** — which is what `js_array_new`,
`js_object_key_at` and `js_context_userdata` were added to lamassu for.

The epilogue is mdy-docs' own: a document with no transform hands back its
LINES and the host composes them; one with a transform asks for its tree
through `$.compose`, runs each transform over it, and hands the TREE back. The
four names a document can use — `toText`, `slug`, `visit`, `h` — are spliced in
as guest source, generated from mdy.js by `scripts-toolkit.mjs` rather than
kept as a copy that can drift.

One result is worth recording because it looks like a bug and is not. An
element written `href class rel title` comes back `href title class rel` after
a transform — and mdy-docs does the same under node. `check-engine` pins the
order against what `node bin/mdy.js build` produces.

Two gaps remain. There is no `js_function_new`, only `js_register_native` for
globals — so `$` is built in the wrapper source over one global native rather
than as an object of native function values. And a render's positions point at
the GENERATED lines rather than the source ones: mdy-docs carries a line map
from the script layer into the parser and this does not yet, which changes no
HTML, only where a warning would say it came from.

## Next

The backend builds. What it does not yet do is *serve*, and the plan's Phase 1c
is the next thing: one narrow protocol — `open` / `build` / `outputs` / the
provider's nine / `watch` — specified once and implemented twice, over Tauri
commands here and over a Worker on the web. That is what makes the editor and
the preview one implementation for five targets instead of two applications
that share a renderer.

Watching (Phase 3) is where the missing `watch` method belongs, along with the
three platform watchers it needs.

`createIndex` used to be a no-op here and is now real —
`dc_collection_add_index` with a B+tree of its own, backfilled from what is
already inserted. That was worth doing and worth measuring: it took the corpus
from 68.4 s to 62.5 s, and **system time from 9.1 s to 1.7 s**, which is the
several-hundred full scans per build no longer walking the collection file. It
did not dent the other 60 seconds, which is exactly what the profile predicted.
