# mdy-native

The backend as a binary: mdy-docs' own JavaScript in QuickJS, with the engines
linked as C rather than loaded as WebAssembly. No renderer, no webview, no
memory ceiling. See [docs/desktop-plan.md](../../docs/desktop-plan.md) —
"The backend is not a webview" — for the measurements that chose this.

```sh
make native            # build both halves, run the checks
make test              # mdy-docs' OWN test suite, against this backend
make site SITE=<dir>   # `mdy build`, natively
make bench             # the same 200-document set, native and over WASM in node
```

Everything is built from source and nothing comes from a system package:
QuickJS is a submodule, lamassu and nisaba are the parent's.

**macOS, Linux and Windows all build and produce byte-identical output**, on
every push, via [.github/workflows/native.yml](../../.github/workflows/native.yml):

```
success  macOS      success  Linux      success  Windows
  all 17 checks passed
  fixture: identical to golden
  fixture-pkg: identical to golden
  messaging: identical to golden
```

Windows through MSYS2/mingw-w64 — a configuration QuickJS's own Makefile
supports (it has an `MSYSTEM` branch), which is why this is mingw and not MSVC.
Bellard's QuickJS does not build under MSVC at all; getting it would mean
switching to the `quickjs-ng` fork, and that is a larger decision than a build
system should make on its own.

## What is here right now

**mdy-docs builds a site on this backend, and the output is byte-identical to
the CLI's.** The reference corpus — 145 source files, an imported style
package, a JS module graph — comes out as the same 93 pages `mdy build`
produces, from a 2 MB binary that links no renderer and no node.

```
$ diff -r corpus-node corpus-native && echo IDENTICAL
IDENTICAL
```

The same is true of `examples/docs-site` (11 pages, guest `import()` included),
`examples/messaging`, and this package's own [fixture/](fixture/).

`make native` is the test — exit status is the verdict, not a demo:

```
--- mdy-native: mdy-docs on QuickJS, engines and filesystem in C ---
  ok    the title, from the document's own front matter
  ok    both cities, found by query
  ok    …in the order they were written
  ok    each one through a nested render
  ok    the provider walks a directory recursively
  …
  ok    a guest `import` loaded a JS module
  ok    …and that module imported its own dependency

all 17 checks passed
```

Each one crosses a boundary the claim rests on rather than exercising mdy-docs,
which has 776 tests of its own: a `$.find` goes guest → host → nisaba and back
with a filter; a `$.render` recurses onto a second lamassu instance while the
first is suspended mid-host-call; the strings carry an em dash and a cuneiform
sign, so the UTF-8/UTF-16 round trip through both engines is checked rather
than assumed; and `buildSite` renders, writes and copies `static/` through a
filesystem that is five C functions.

Nothing in mdy-docs knows any of this. [entry.mjs](entry.mjs) and
[site-entry.mjs](site-entry.mjs) import `renderDocumentSet` and `buildSite`
from the package the same way a node build does; the only substitution is two
esbuild aliases in [scripts-build.mjs](scripts-build.mjs), and the three shims
behind them are 260 lines together.

### What it cost, measured

Two workloads, because one number would be a lie in either direction. Both on
the same machine, both against `mdy build` on node with the WASM engines.

|                                   | node   | native | ratio  |
| --------------------------------- | ------ | ------ | ------ |
| **reference corpus**, 93 pages    | 10.6 s | 62.5 s | 5.9× slower |
| peak RSS                          | 816 MB | 593 MB | 1.4× smaller |
| **200 templates**, `make bench`   | 305 ms | 296 ms | a wash |
| peak RSS                          | 148 MB | 19 MB  | 7.8× smaller |
| runtime on disk                   | ~110 MB (node) | 2.0 MB | 55× smaller |

The spread between those two rows is the whole story, and it is not noise.
**QuickJS has no JIT**, and on mdy-docs' own JavaScript it runs several times
slower than V8. **lamassu is now C rather than WebAssembly**, and there it is
faster. Which effect wins depends entirely on what a site spends its time
doing:

- `make bench` is 200 documents of *templates* — one `$.find`, a nested render
  each. That is lamassu's work, and native lamassu pays for QuickJS exactly.
- The corpus is 145 files of long-form *prose*. That is micromark, remark,
  hast and the MDY front end — JavaScript, all of it — and QuickJS's cost
  shows through undiluted. A `sample` of the running build is unambiguous:
  every frame is `JS_CallInternal`, `js_array_flatten`, `js_array_every`,
  generators. No native call appears at all.

So the honest summary is: **prose-heavy sites are slower here, template-heavy
sites are not, and both use a fraction of the memory.** If the corpus number
ever needs to come down, the profile names the target — the MDY front end,
4,441 lines of our own producing hast directly, measured at 8.8× — and porting
that one component to C would not cost rehype, since markdown still arrives
through remark.

Memory is what this was actually for, and it holds on both workloads. A webview
build died at page 45 of this corpus against a 1146 MB ceiling. This finishes
all 93, in less space than node, with no ceiling above it.

### The bridge underneath

```
--- mdy-native bridge ---
  lamassu alone  : lamassu ran: 42
  lamassu -> qjs : sandbox asked, host said: quickjs, from inside the sandbox
```

Three things established. The two engines link and run in one process; QuickJS
can evaluate a lamassu program and read its value; and a lamassu program can
call **out** to a function implemented in QuickJS. The third is the one that
matters — every `$` a document uses is a call out of the sandbox into
mdy-docs' JavaScript, so the whole design rests on that direction.

Worth noting what this is not doing: the WASM build routes every host call
through one `__hostcall(name, jsonArgs)` and pays JSON in both directions.
That mechanism exists only in `src/wasm_api.c` — it is a workaround for the
WASM boundary, not part of lamassu. Natively, functions are registered
directly with `js_register_native` and values cross as values.

## Two collisions, and why the files are split this way

lamassu and QuickJS both use the `js_` namespace, and it bites twice.

**Headers cannot meet.** lamassu's `JS_TAG_STRING` is a macro; QuickJS's is an
enum member. Including both turns the enum into
`UINT64_C(0xFFFA000000000000) = -7`. So `lam.c` includes lamassu and nothing
else, `host.c` includes QuickJS and nothing else, and [src/lam.h](src/lam.h) is
the only header both see — it names no type belonging to either. That is the
right shape regardless; the collision only forced it sooner.

**Symbols cannot meet either — and the first fix for that was wrong.** Both
engines defined `js_dtoa`, and the original answer was to pre-link lamassu's
two archives into one object with `ld -r -all_load -unexported_symbol
_js_dtoa`, making the symbol local. That works, and it is ld64-only, so it
quietly made macOS the one platform this could be built on.

lamassu's `js_dtoa` is `static` now (52f0bfd), and the archives link directly.
Comparing the two symbol tables afterwards said how small the real problem was:
181 exports against 273, and `js_dtoa` was the only name in common.

**But the pre-link was hiding something worse.** nisaba vendors
`mdy-docs/regex-engine`; lamassu has moved to `mdy-docs/baru-re`, its successor
— same ancestry, *different version*. Four internal names collide. Because
`ld -r` loads every symbol unconditionally while an archive is pulled on
demand, nisaba's `regexp.o` was never pulled at all, and any call it made to
one of those four resolved to **lamassu's differently versioned
implementation**. No error, no warning.

They are renamed at compile time now (see `NIS_RENAME` in the Makefile), which
keeps each engine's calls inside its own engine and turns any new overlap into
a duplicate-symbol error rather than a new silent binding. The real fix is for
nisaba to use baru-re as well, so the binary holds one regex engine instead of
two.

## Async host calls — the hard part, and it works

A document calls `$.find(q)` synchronously; that is the language contract and
every template depends on it. But mdy-docs implements those natives in
JavaScript and several are async — a query awaits the database, a nested
`$.render` awaits another render. The WASM build hides that with Asyncify.
There is no Asyncify natively.

Three ways out, and they are not equal. Rewriting the natives to be synchronous
would diverge from the Node path forever. Making the guest `await` would break
the contract that `$.find(q)` returns documents. So the native does neither: it
calls into QuickJS and, if the answer is a promise, **pumps QuickJS's job queue
until it settles**, then hands the value back synchronously. The guest never
learns it waited, and mdy-docs' JavaScript is untouched.

```
--- mdy-native: async host calls ---
  sync native      : -> answered synchronously
  async native     : -> answered after awaiting
  re-entrant render: -> inner sandbox said: 42 * 2 = 84
  never settles    : -> (promise never settled — nothing left to run)
  unicode round trip: -> Ašared — Uruk’s scribes, ‰, 𒀭
```

Four of those matter.

**Re-entrancy is fine, but it has to be stacked.** A nested `$.render` means
sandbox → host → sandbox. The inner run gets its own `JsVm`, exactly as
[../../src/vm.js](../../src/vm.js) gives each nesting level its own pooled
instance, so nothing re-enters a suspended VM. What was NOT fine at first is
subtler: lamassu passes a native no user pointer of its own, so "which VM is
asking" lives in a global, and an inner `lam_eval` was overwriting the outer's.
The symptom was a *second* nested render failing with `unknown native
"render"` — the outer eval resumed holding the inner's identity, whose natives
the VM pool had already torn down. `lam_eval` now saves and restores it.

**A promise that cannot settle says so.** Pumping finds no job, and rather than
hang, the deadlock is reported. In a native backend the filesystem is
synchronous C so it should not arise — but a backend that hangs silently is the
failure this whole exercise has been paying for.

**Unicode survives, including astral.** That last line is not decoration: the
first conversion truncated every byte to a code unit, and it was an em dash
coming back as `???` that gave it away. The corpus is Akkadian transliteration
and cuneiform, so this had to be right before anything real crossed.

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
a plain file descriptor. What the JS side calls `MemoryStorageProvider` becomes
a temp file, which is not a compromise — the on-disk B+tree is nisaba's format.

**`_id` must be minted by the host.** `dc_insert_one` refuses a document
without one; the JS binding generates the ObjectId, so a native binding has to.
That is the same rule met from the other side earlier: nisaba will not accept a
scalar `_id`, and the primary tree's keys are fixed-width OID bytes.

## mdy-docs' own tests, on this backend

```
macOS    # native: 684 passed, 0 failed, 1 skipped in 807ms
Linux    # native: 684 passed, 0 failed, 1 skipped in 676ms
Windows  # native: 684 passed, 0 failed, 1 skipped in 907ms
```

Not a copy and not a rewrite: [tests-entry.mjs](tests-entry.mjs) imports the
same files `npm test` runs, in place. A copy would drift, and a suite that has
drifted proves nothing about the thing it is meant to be checking. What changes
is only what `node:test`, `node:assert`, `node:fs`, `node:path`, `node:os`,
`node:url`, `node:zlib` and `node:vm` resolve to — [shims/node/](shims/node/),
about 700 lines over the C in [src/fsx.c](src/fsx.c).

This is the strongest evidence for what this package actually claims. The
backend's premise is that it runs mdy-docs *unchanged*; mdy-docs' own tests
passing on it says that, where a separate native suite would only ever check
what someone thought to re-check.

Aliasing `node:fs/promises` has a second effect worth knowing: `nodeFsProvider`
reaches for it through a lazy dynamic import, so the DEFAULT provider works
natively too. A test that calls `renderScriptSite(dir)` with no provider runs
here with nothing changed.

**What does not run, and why each is the runtime rather than the port.** 91 of
776 tests, in five files plus one:

| | |
| --- | --- |
| `cli.test.js` (34) | spawns `bin/mdy.js` as a subprocess. There is no `child_process` here and there should not be — this backend *is* the thing a CLI would spawn. |
| `build.test.js` (28) | builds `examples/blog` at module top level, and that example calls `$.resize`. The file cannot be imported. `site-memory-build.test.js` covers `buildSite` through a provider, which is the path this backend takes. |
| `serve.test.js` (11) | stands up a `node:http` server. Serving is the plan's Phase 1c. |
| `images.test.js` (10) | the @jsquash codecs are WebAssembly. |
| `search-widget.test.js` (5) | runs the shipped widget against a fake DOM; it is testing browser JavaScript. |
| `opfs-provider.test.js` (3) | OPFS is a browser API. |

One further test is skipped **by name**, with its reason printed — `$.resize`
producing a real thumbnail, for the same WebAssembly reason. The runner fails
if a name on that list matches nothing, so a stale entry cannot quietly hide a
real result.

### Six things the port found

Porting a suite is worth it for what it turns up, and this turned up four.

- **A collection had no lifetime.** `nis_open` handed back an integer from a
  fixed table of eight, and nothing ever closed one — fine for a build, which
  opens one set per package, and wrong the moment a suite opened one per test.
  The table grows now, and more importantly the handle rides inside a JS object
  whose **finalizer** closes it, so a collection is reclaimed when the
  JavaScript that owned it becomes unreachable. That is the lifetime nisaba's
  WASM binding gets from its own GC; this is a native host earning the same.
- **A rejected host call was wrapped, not propagated.** Prefixing the reason
  with "the host rejected: " looked harmless until a cyclic `$.render` — every
  level of the recursion added another copy and the depth guard's message
  arrived buried under a dozen of them. The WASM binding rethrows verbatim, and
  now so does this.
- **There was no `setTimeout`.** QuickJS's timers live in quickjs-libc, which
  this host deliberately does not link. The job pump is now a small event loop:
  when no job is ready and a timer is pending, it waits for the earliest and
  fires it. Enough, because nothing here has I/O to wait on — the filesystem is
  synchronous C.
- **A test was coupled to V8's wording.** `missing is not defined` in V8,
  `'missing' is not defined` in QuickJS, for the same `ReferenceError` — and
  script blocks run in the *host* runtime (`new Function`), so which one that
  is depends on where mdy-docs is running. The test now matches either; its
  intent was that the failure names the missing identifier.

Two more came from running it on Windows, and only Windows could have found
them:

- **A timer that is not due yet is not "nothing left to run".** Its clock has
  ~15.6 ms granularity and `Sleep` can return early, so the event loop would
  wake from a 10 ms timer with the clock reading the same tick, fire nothing,
  and report the promise as unsettleable. A timer existing means progress is
  guaranteed, so waiting *is* progress.
- **`/C:/…` is a drive path with a spurious leading slash.** mdy-docs says
  "this path is absolute" by passing it as `fs.read('/', absolutePath)`, and
  `nodeFsProvider` joins the two before the filesystem sees them — producing
  `/D:/…` on Windows, which names nothing, so every guest `import` failed.
  Node's own win32 `join` does the same; it is not a case it was built for.

`fs.watch` is polled rather than native, and that is said plainly in the shim.
A real recursive watcher is kqueue, inotify and `ReadDirectoryChangesW` — the
plan's Phase 3 — and mdy-docs already ships `watchByPolling` as its own
fallback where a native watcher is unavailable, so this is the codebase's
existing position rather than a new one.

## Three platforms

The only files that know which operating system this is are
[fsx.c](src/fsx.c), [nis.c](src/nis.c) and [oswin.c](src/oswin.c). Both engines
are platform-clean — lamassu has no `#ifdef`s at all, and nisaba's I/O sits
behind its four `bj_io` callbacks — so Windows was a second version of those
and nothing else: `FindFirstFileW` for the walk, `OVERLAPPED` for
`pread`/`pwrite` (Windows has no positioned read; the offset rides in the
OVERLAPPED, which is how the "does not disturb the file pointer" guarantee
survives), `SetEndOfFile`, and `FILE_FLAG_DELETE_ON_CLOSE` for the collection's
backing file — the same self-deleting lifetime `mkstemp` + `unlink` gives.

Two rules hold it together and both are load-bearing:

- **Paths cross this boundary as UTF-8, and every Win32 call is the wide
  variant.** The narrow entry points go through the process code page, which
  cannot spell most of what the reference corpus is named.
- **`/` is the separator everywhere.** Win32 accepts it in every path it is
  given, so the only place a backslash can enter is `fsx_cwd`, where the OS
  hands one back — and it is translated there rather than in every place it
  would otherwise surface. This matters more than it looks: `src/imports.js`
  decides a module is inside its package by string prefix, so two spellings of
  one path are two packages. `golden/fixture-pkg` is the test for it.

The drive letter needs handling in exactly two places, both found by reasoning
rather than by a red build: `at()` in fsx.c, where an absolute `rel` under a
root of `/` would otherwise produce `/C:/Users/…`, and `site-entry.mjs`, where
`C:/work` would otherwise be treated as relative and have the cwd prepended.

Three things CI found that reasoning had not:

- **glibc hides POSIX declarations under `-std=c11`.** `strdup` came back as an
  implicit declaration and `st_mtim` as an unknown field — both fine on macOS,
  which exposes them regardless. `gnu11` now.
- **`npm install` runs node-gyp on Windows and fails** for want of Visual
  Studio: `@mdy-docs/nisaba-db` → `node-opfs` → `fs-ext` carries a
  `binding.gyp`. Nothing on the native path uses it, so CI installs with
  `--ignore-scripts`; node is here only to run esbuild, which resolves its
  platform binary from optionalDependencies and needs no install script.
- **Line endings are build output.** `static/` is a verbatim passthrough, so a
  stylesheet checked out with CRLF is emitted with CRLF, and Windows runners
  default to `core.autocrlf=true`. `.gitattributes` marks everything whose
  bytes reach the output, and the output itself, as `-text`.

## The filesystem, and guest `import`

Both were the last structural gaps, and both are now shipped.

**The filesystem** is [src/fsx.c](src/fsx.c) — five POSIX calls, held behind
[src/fsx.h](src/fsx.h) so nothing from any engine crosses — and
[shims/fs.js](shims/fs.js), which builds the nine-method contract in
[../../src/fs-provider.js](../../src/fs-provider.js) on them. The methods are
`async` because the contract is, not because anything waits.

A listing crosses as ONE newline-separated string, not an array: the corpus is
thousands of paths, and building that many JSValues to immediately join them is
work neither side needs. `d_type` is not trusted — several filesystems answer
`DT_UNKNOWN` from a directory entry, and a walk that believes it silently loses
whole subtrees, so an unknown falls back to `stat`.

`watch` is absent, deliberately. It is optional at every call site
(`fs.watch?.(…)`), and a native recursive watcher is kqueue, inotify and
`ReadDirectoryChangesW` — three implementations, which is the plan's Phase 3
and not a line to sneak in here. A build does not watch; `mdy dev` does.

See "Three platforms" above for how the rest of it reaches Windows.

**Guest `import`** is `js_set_module_loader` in [src/lam.c](src/lam.c), routed
out to mdy-docs' own loader (which reads through the provider and enforces the
package boundary — see `canonicalizeModule` in ../../src/imports.js). Two
things about it cost a cycle each:

- **Source modules are off by default.** `js_enable_source_modules` is a
  frontend call, and without it a loader that resolves with source fails at the
  fetch with "source modules unavailable in this build (precompile to
  bytecode)". That reads like a missing library and is really a missing line —
  the split exists so a runtime-only build cannot compile source it is handed,
  which is a link-time guarantee rather than a policy.
- **A root spelled with `..` breaks the package boundary check.** imports.js
  decides a module is inside its package by string prefix, which is the right
  check; a root of `../../examples/docs-site` then makes every one of that
  site's own modules look like an escape attempt. The normalisation belongs in
  the host, where the spelling comes from.

`buildSite` itself now writes through the provider rather than node:fs, so this
backend runs the CLI's own build function instead of reimplementing it beside
it. That was a change to mdy-docs, and the 776 tests cover it.

**`$.resize` does not work here, and cannot.** Its image codecs are
WebAssembly, and QuickJS has none. mdy-docs now says exactly that at the point
it is true, because the failure otherwise arrived as a missing `node:fs` and
then again, unrecognisably, as a null tree in whatever page used it.

## The shims

Three files, and all short because mdy-docs asks for very little.
[shims/lamassu.js](shims/lamassu.js) is `createLamassu()` with
`eval` / `setNatives` / `setModuleLoader` / `reset`, and it keeps the
`__hostcall(name, argsJson)` contract exactly as `buildProgram` generates it —
values could have crossed as values natively, but keeping the JSON means the
generated program is byte-identical on both backends, and one fewer thing
differs while both exist. [shims/nisaba.js](shims/nisaba.js) is `connect`, a
collection, `insertOne`, `find().toArray()` and `createIndex`, with documents
crossing as binjson encoded by the reference JS codec from nisaba's own
submodule. [shims/fs.js](shims/fs.js) is the provider above.

Instances are pooled, not made per eval — `createLamassu()` allocates a real
`JsVm`, as it does over WASM, and `../../src/vm.js`'s pool is what reuses them.
(Worth recording that this did NOT move the corpus number: creating a VM per
eval cost about 1% of the build, not the 60 s the profile was hiding. It is
still the right shape, and it is what `reset()` needs to be honest.)

Adaptations worth knowing about, because each was a silent failure first:

- **`lam_eval` answers with the completion value; `lamassu_eval` answers with a
  transcript** whose completion value is the line after `⇒ `. The latter is the
  WASM export layer, which a native host does not have, and it is the shape
  `src/vm.js` reads. The shim puts the marker back rather than teaching vm.js
  which backend it is talking to.
- **The `_id` has to be the codec's own `ObjectId`.** A hand-rolled
  `{ $oid: bytes }` encodes to something `dc_insert_one` rejects — and it
  rejects it by returning `-1`, which the shim was ignoring, so inserts
  "succeeded" and every query came back empty. The class also gives the 24-hex
  `toString` that `src/mdy.js` keys its index map by, so an inserted document
  and a found one agree without either side knowing about the other.
- **A native that fails must throw INSIDE the sandbox**, which is what the WASM
  binding does and what mdy-docs' generated program is written for — its
  try/catch turns it into "document N failed: …". Answering `null` instead
  turned one legible error into a cascade of unrelated ones about null trees.
- **`push(...array)` is an argument list**, and every engine caps those. V8
  allows enough that binjson's encoder looked correct for years; QuickJS stops
  at 65534, and the corpus has documents with more encoded pieces than that.
  Fixed in binjson itself — the count is a property of the data, so the only
  safe number of arguments is one.

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
lamassu**. QuickJS is not linked into that binary at all — `nm` finds zero of
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

**`$` refuses, loudly.** Every native is present and throws with its own name
— `$.render is not implemented in the C engine yet` — because a stub returning
undefined produces a page quietly missing whatever the document asked for,
which is the failure this project has been most careful to avoid.

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
a transform — and mdy-docs does the same, under node and under the QuickJS
build alike. `check-engine` pins the order against what `node bin/mdy.js build`
produces.

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
