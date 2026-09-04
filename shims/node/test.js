/*
 * `node:test`, enough of it to run mdy-docs' own suite on the native backend.
 *
 * The point of porting the suite rather than writing a new one: the native
 * backend's claim is that it runs mdy-docs *unchanged*, and the strongest
 * evidence for that is mdy-docs' own tests passing against it. A separate
 * native test suite would only ever check what someone thought to re-check.
 *
 * What the suite actually uses is `test`, `describe`, `before`, `after` and
 * `t.skip(reason)` — no subtests, no hooks beyond those two, no concurrency.
 * So this collects, then runs in order, and reports in TAP-ish lines.
 *
 * Deliberately NOT lazy about failure: an unimplemented feature should be a
 * missing method, not something that quietly passes.
 */

const suites = [];          /* { name, tests, before, after } */
let current = null;         /* the describe() being collected into */
const root = { name: null, tests: [], before: [], after: [] };
suites.push(root);

export function describe(name, fn) {
  const parent = current;
  current = { name, tests: [], before: [], after: [] };
  suites.push(current);
  fn();
  current = parent;
}
describe.skip = (name) => { (current ?? root).tests.push({ name, skip: 'describe.skip' }); };

export function test(name, optionsOrFn, maybeFn) {
  const fn = typeof optionsOrFn === 'function' ? optionsOrFn : maybeFn;
  (current ?? root).tests.push({ name, fn });
}
test.skip = (name) => { (current ?? root).tests.push({ name, skip: 'test.skip' }); };
export const it = test;

export function before(fn) { (current ?? root).before.push(fn); }
export function after(fn) { (current ?? root).after.push(fn); }
export const beforeEach = () => { throw new Error('node:test shim: beforeEach is not implemented'); };
export const afterEach = () => { throw new Error('node:test shim: afterEach is not implemented'); };

/** The `t` a test body receives. Only `skip` is used by this suite. */
function context() {
  const ctx = { skipped: null };
  return {
    ctx,
    t: {
      skip(reason) { ctx.skipped = reason ?? ''; },
      diagnostic() {},
      get name() { return ''; },
    },
  };
}

const summary = { pass: 0, fail: 0, skip: 0, failures: [] };

/**
 * Run everything collected, in declaration order. Returns the summary; the
 * caller decides the exit status.
 *
 * `unsupported` maps a test's name to WHY this runtime cannot run it. Those
 * are reported as skips with the reason attached, which is what `t.skip()`
 * would do if the test could be edited — but these files are shared with
 * `npm test`, where the same test runs and must not be skipped. Keeping the
 * list on this side is the only place it can live.
 *
 * A name in the list that turns out to PASS is an error, not a bonus: it means
 * the reason is stale and the list is now hiding a real result.
 */
export async function run({ onLine = print, unsupported = new Map() } = {}) {
  const unused = new Set(unsupported.keys());
  for (const suite of suites) {
    if (suite.tests.length === 0) continue;
    if (suite.name) onLine(`# ${suite.name}`);
    for (const hook of suite.before) await hook();

    for (const spec of suite.tests) {
      const label = suite.name ? `${suite.name} > ${spec.name}` : spec.name;
      if (spec.skip) { summary.skip++; onLine(`skip ${label} (${spec.skip})`); continue; }
      if (unsupported.has(spec.name)) {
        unused.delete(spec.name);
        summary.skip++;
        onLine(`skip ${label}\n    ${unsupported.get(spec.name)}`);
        continue;
      }
      const { ctx, t } = context();
      try {
        await spec.fn(t);
        if (ctx.skipped !== null) { summary.skip++; onLine(`skip ${label}${ctx.skipped ? ` (${ctx.skipped})` : ''}`); }
        else { summary.pass++; }
      } catch (err) {
        // A test that skipped and then threw is still a failure: the skip is
        // a claim about what was not run, not a shield over what was.
        summary.fail++;
        const message = String(err?.message ?? err).split('\n').slice(0, 6).join('\n    ');
        summary.failures.push({ label, message });
        onLine(`FAIL ${label}\n    ${message}`);
      }
    }

    for (const hook of suite.after) await hook();
  }

  // A listed test that never ran means the name is wrong — and a wrong name
  // silently runs a test the list claims is skipped, or hides one that no
  // longer exists.
  for (const name of unused) {
    summary.fail++;
    summary.failures.push({ label: name, message: 'listed as unsupported, but no test has this name' });
    onLine(`FAIL ${name}\n    listed as unsupported, but no test has this name`);
  }
  return summary;
}

export default { test, it, describe, before, after, run };
