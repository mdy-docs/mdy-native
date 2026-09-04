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

## Next

The async question. mdy-docs' natives return promises — a nested render, a
query — and the WASM build leans on Asyncify to make that look synchronous to
the guest. lamassu's C API has a promise path for it (`js_promise_new`,
`js_resolve`, `js_run_jobs`), so the pieces exist; joining them to QuickJS's
job queue is the work after this. Then nisaba, then mdy-docs' own bundle.
