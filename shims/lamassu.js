/*
 * Stands in for @mdy-docs/lamassu-js, backed by the native engine.
 *
 * Same shape src/vm.js expects — createLamassu() giving eval / setNatives /
 * setModuleLoader / reset — so nothing above it changes. `eval` returns the
 * engine's output text and runProgram picks the "⇒ " line out of it, exactly
 * as it does over WASM.
 *
 * The host-call contract is kept as buildProgram generates it:
 * `__hostcall(name, argsJson)` in, a JSON string out. It could have been
 * richer natively — values could cross as values — but keeping it means the
 * generated program is byte-identical to the one the WASM path runs, and one
 * fewer thing differs between the two backends while both exist.
 */
const natives = new Map(); /* vm id -> the natives installed on it */
let nextId = 1;

export function createLamassu() {
  const id = nextId++;
  natives.set(id, {});
  return {
    async eval(program) {
      // __lam_eval is synchronous: the C side pumps this runtime's job queue
      // whenever a host call returns a promise, so an async native settles
      // without the guest ever learning it waited.
      //
      // It answers with the completion VALUE. lamassu_eval — the WASM export
      // layer, which a native host does not have — answers with a transcript
      // whose completion value is the line after "⇒ ", and that is the shape
      // src/vm.js reads. So the marker goes back on here rather than vm.js
      // learning which backend it is talking to.
      return `⇒ ${globalThis.__lam_eval(id, program)}`;
    },
    setNatives(next) {
      natives.set(id, next ?? {});
    },
    setModuleLoader() {
      /* Guest `import()` is not wired natively yet — no mdy-docs path used by
       * a build reaches it, and pretending otherwise would hide that. */
    },
    reset() {
      natives.set(id, {});
    },
  };
}

/*
 * What the C side calls for every `__hostcall`. Returns a JSON string, or a
 * promise of one — the host pumps jobs until it settles.
 */
globalThis.__lam_dispatch = (id, name, argsJson) => {
  const fns = natives.get(id) ?? {};
  const fn = fns[name];
  if (typeof fn !== 'function') throw new Error(`unknown native "${name}"`);
  // Same coercions the WASM binding makes, for the same reason: a native is
  // called with an argument LIST, and a lone non-array value is one argument.
  const parsed = argsJson === '' || argsJson === undefined ? [] : JSON.parse(argsJson);
  const args = Array.isArray(parsed) ? parsed : [parsed];
  const out = fn(...args);
  return out && typeof out.then === 'function'
    ? out.then((v) => JSON.stringify(v === undefined ? null : v))
    : JSON.stringify(out === undefined ? null : out);
};
