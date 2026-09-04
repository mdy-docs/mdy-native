/*
 * `node:vm`, for the one test that runs a compiled template's source outside
 * the module that produced it.
 *
 * IT IS NOT A SANDBOX HERE, and that has to be said rather than implied.
 * QuickJS as embedded by this host has one realm, so `runInNewContext` is an
 * indirect eval in the same global — the code runs, it cannot see the caller's
 * locals, and that is the whole of the isolation. The test that uses it is
 * checking that compiled template source is self-contained and evaluable, not
 * that node's contexts isolate; on the sandboxing that actually matters,
 * lamassu is the boundary and it is a real one.
 *
 * Anything needing genuine isolation should fail loudly rather than get this.
 */
export function runInNewContext(code, sandbox) {
  const names = Object.keys(sandbox ?? {});
  const values = names.map((n) => sandbox[n]);
  // eslint-disable-next-line no-new-func
  const make = new Function(...names, `return (${code});`);
  return make(...values);
}

export function createContext(o) { return o ?? {}; }
export function runInThisContext(code) { return (0, eval)(code); }
export const Script = class {
  constructor() { throw new Error('node:vm shim: Script is not implemented'); }
};
export default { runInNewContext, createContext, runInThisContext, Script };
