# mdy-native

The backend as a binary: mdy-docs' own JavaScript in QuickJS, with the engines
linked as C rather than loaded as WebAssembly. No renderer, no webview, no
memory ceiling. See [docs/desktop-plan.md](../../docs/desktop-plan.md) —
"The backend is not a webview" — for the measurements that chose this.

```sh
make native   # build both halves and run the checked render
make bench    # the same 200-document set, native and over WASM in node
```

## What is here right now

**mdy-docs renders, unmodified, on this backend.** `make native` bundles the
package with the two engine imports aliased to [shims/](shims/) and runs
`renderDocumentSet` inside the host:

```
--- mdy-native: mdy-docs on QuickJS, engines linked as C ---
  ok    the title, from the document's own front matter
  ok    both cities, found by query
  ok    …in the order they were written
  ok    each one through a nested render
```

Exit status is the verdict, so this is a test rather than a demo. The four
checks are chosen to cross every boundary the claim rests on: a `$.find` goes
guest → host → nisaba and back with a filter; a `$.render` recurses onto a
second lamassu VM while the first is suspended mid-host-call; and the strings
carry an em dash and a cuneiform sign, so the UTF-8/UTF-16 round trip through
both engines is checked rather than assumed.

Nothing in mdy-docs knows any of this. [entry.mjs](entry.mjs) imports
`renderDocumentSet` from the package the same way a node build does; the only
substitution is two esbuild aliases in [scripts-build.mjs](scripts-build.mjs),
and the shims behind them are 130 lines together.

### What it cost, measured

The same 200-document set — one `$.find` over all of them, each rendered
through a nested render — both ways, on the same machine:

|                    | wall  | peak RSS | runtime on disk |
| ------------------ | ----- | -------- | --------------- |
| node + WASM engines| 300ms | 138 MB   | ~110 MB (node)  |
| this               | 314ms | 21 MB    | 1.8 MB stripped |

**Time is a wash; memory is 6.6× smaller.** That is a better result than the
earlier estimate, and the reason is worth stating because it is not obvious.
QuickJS has no JIT and runs mdy-docs' own JavaScript several times slower than
V8 — the ingest phase, measured alone on the corpus, was 17.2s against 2.2s.
But a build of a real document set does not spend its time there: it spends it
inside lamassu, running templates. Natively that is C rather than WebAssembly,
and what it gains back is roughly what QuickJS gives up. The two effects very
nearly cancel.

The memory number does not cancel, and memory is what this was for. A webview
build died at page 45 of the corpus against a 1146 MB ceiling; 21 MB against
138 MB is the same work in a seventh of the space, with no ceiling above it.

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

## The shims

Two files, and both are short because mdy-docs asks the engines for very
little. [shims/lamassu.js](shims/lamassu.js) is `createLamassu()` with
`eval` / `setNatives` / `setModuleLoader` / `reset`, and it keeps the
`__hostcall(name, argsJson)` contract exactly as `buildProgram` generates it —
values could have crossed as values natively, but keeping the JSON means the
generated program is byte-identical on both backends, and one fewer thing
differs while both exist. [shims/nisaba.js](shims/nisaba.js) is `connect`, a
collection, `insertOne`, `find().toArray()` and `createIndex`, with documents
crossing as binjson encoded by the reference JS codec from nisaba's own
submodule.

Two adaptations worth knowing about, because both were silent failures first:

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

## Next

Two gaps stand between this and building the corpus natively: a filesystem
provider (the nine methods in [../../src/fs-provider.js](../../src/fs-provider.js),
over QuickJS's `std`/`os` modules) and a module loader for guest `import()`.
Neither is structural — the hard parts, async host calls and the two engines in
one process, are done.

Two smaller honesties, both marked in the code. `createIndex` is a no-op: the
sparse `path` index is an optimisation and a query without it is a scan, so the
answers are the same and only the timing differs. And `setModuleLoader` does
nothing, so a document that reaches for guest-side `import()` will fail rather
than quietly render without it.
