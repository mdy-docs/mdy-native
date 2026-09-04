# mdy-native

The backend as a binary: mdy-docs' own JavaScript in QuickJS, with the engines
linked as C rather than loaded as WebAssembly. No renderer, no webview, no
memory ceiling. See [docs/desktop-plan.md](../../docs/desktop-plan.md) —
"The backend is not a webview" — for the measurements that chose this.

```sh
make run
```

## What is here right now

The bridge, and only the bridge:

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

**Re-entrancy is fine.** A nested `$.render` means sandbox → host → sandbox.
The inner run gets its own `JsVm`, exactly as [../../src/vm.js](../../src/vm.js)
gives each nesting level its own pooled instance, so nothing re-enters a
suspended VM.

**A promise that cannot settle says so.** Pumping finds no job, and rather than
hang, the deadlock is reported. In a native backend the filesystem is
synchronous C so it should not arise — but a backend that hangs silently is the
failure this whole exercise has been paying for.

**Unicode survives, including astral.** That last line is not decoration: the
first conversion truncated every byte to a code unit, and it was an em dash
coming back as `???` that gave it away. The corpus is Akkadian transliteration
and cuneiform, so this had to be right before anything real crossed.

## Next

The remaining question. mdy-docs' natives return promises — a nested render, a
query — and the WASM build leans on Asyncify to make that look synchronous to
the guest. nisaba, bound the same way lamassu now is, and then mdy-docs' own bundle
loaded into QuickJS with `@mdy-docs/lamassu-js` and `@mdy-docs/nisaba-db`
replaced by these bindings. At that point the corpus builds outside a browser
and the ~2x estimate becomes a measurement.
