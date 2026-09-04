# mdy-native

The backend as a binary: mdy-docs' own JavaScript in QuickJS, with the engines
linked as C rather than loaded as WebAssembly. No renderer, no webview, no
memory ceiling. See [docs/desktop-plan.md](../../docs/desktop-plan.md) —
"The backend is not a webview" — for the measurements that chose this.

```sh
make native            # build both halves, run the checks
make site SITE=<dir>   # `mdy build`, natively
make bench             # the same 200-document set, native and over WASM in node
```

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

**Symbols cannot meet either.** Both define `js_dtoa`. lamassu's is internal —
not in `lamassu.h` — so the Makefile pre-links its two archives into one object
with that symbol made local, and each engine then resolves its own. On this
toolchain `ld -r` also needs an explicit `-arch`, or it asserts inside an
Objective-C pass with "unknown objc arch", which is an ld64 bug and not
something you did.

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

Everything else here is portable by accident of where the seams already were:
lamassu and nisaba contain no platform `#ifdef`s at all, so [fsx.c](src/fsx.c)
and [nis.c](src/nis.c) — 361 lines — are the entire surface that knows which
operating system this is. Adding Windows means a second version of those two
files and nothing else. See the plan's Phase 4.

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
