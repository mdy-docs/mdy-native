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
const loaders = new Map(); /* vm id -> { load, canonicalize } for guest import() */

export function createLamassu() {
  /*
   * A real instance, not a number: creating one allocates a heap and builds a
   * global object, which is why src/vm.js pools them. It used to be made and
   * thrown away inside every eval, and on the reference corpus that was most
   * of the wall clock.
   */
  const id = globalThis.__lam_vm_new();
  if (id < 0) throw new Error('lamassu: no instance available');
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
      //
      // The third argument says whether a module loader is installed for THIS
      // eval. Without it the engine gets no loader at all, so a guest `import`
      // fails at the import rather than resolving to nothing — which is what
      // src/vm.js means by installing one per eval.
      return `⇒ ${globalThis.__lam_eval(id, program, loaders.has(id))}`;
    },
    setNatives(next) {
      natives.set(id, next ?? {});
    },
    /*
     * Per-eval, and cleared by src/vm.js in its `finally` — the pool outlives
     * any one render, and a loader left installed would answer a later render's
     * imports from the wrong package root.
     */
    setModuleLoader(load, canonicalize) {
      if (typeof load === 'function') loaders.set(id, { load, canonicalize });
      else loaders.delete(id);
    },
    /*
     * The engine's module registry caches evaluated source per canonical
     * specifier for the instance's lifetime, and a pooled instance outlives a
     * render — so without this a watch-mode rebuild after editing a .js module
     * could be served the stale copy by whichever instance loaded it first.
     * The registry belongs to the JsVm, and lamassu exposes no way to empty
     * one, so the instance itself is replaced. Only an eval that actually
     * loaded a module reaches here (vm.js tracks that), so this is rare.
     */
    reset() {
      natives.set(id, {});
      loaders.delete(id);
      globalThis.__lam_vm_free(id);
      const fresh = globalThis.__lam_vm_new();
      if (fresh !== id) {
        // The host reuses the lowest free slot, so freeing and immediately
        // reallocating gives the same one back. If it ever does not, the id
        // this closure captured would name someone else's instance.
        throw new Error(`lamassu: reset moved instance ${id} to ${fresh}`);
      }
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

/*
 * What the C side calls for a guest `import`. Two operations rather than two
 * callbacks, matching lam.h: 0 canonicalizes a specifier against its referrer,
 * 1 loads the canonical specifier's source.
 *
 * Raw text both ways, not JSON — a module's source is the payload, and there
 * is no C-side parser to unwrap it with. Either may return a promise; the host
 * pumps jobs until it settles, exactly as it does for a host call.
 */
globalThis.__lam_module = (id, op, specifier, referrer) => {
  const loader = loaders.get(id);
  if (!loader) throw new Error(`no module loader installed for "${specifier}"`);
  if (op === 0) {
    // Synchronous by contract: the engine resolves registry identity with the
    // answer BEFORE dedupe, so it cannot wait for one.
    return String(loader.canonicalize ? loader.canonicalize(specifier, referrer) : specifier);
  }
  const source = loader.load(specifier, referrer);
  return source && typeof source.then === 'function' ? source.then(String) : String(source);
};
