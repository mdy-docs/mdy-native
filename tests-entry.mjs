/*
 * mdy-docs' own test suite, run against the native backend.
 *
 * This is the strongest form of the claim this package makes. The backend's
 * whole premise is that it runs mdy-docs *unchanged*; the evidence for that is
 * mdy-docs' own tests passing on it, not a separate native suite that would
 * only ever check what someone thought to re-check.
 *
 * Nothing in test/ is modified or copied. These are the SAME FILES `npm test`
 * runs, imported in place — a copy would drift, and a suite that has drifted
 * proves nothing about the thing it is supposed to be checking. What changes
 * is only what `node:test`, `node:assert`, `node:fs` and friends resolve to:
 * see the aliases in scripts-build.mjs and the shims in shims/node/.
 *
 * WHAT IS NOT HERE, and why each one is a property of the runtime rather than
 * a gap in the port:
 *
 *   cli.test.js           spawns `bin/mdy.js` as a subprocess. There is no
 *                         child_process here and there should not be — the
 *                         backend IS the thing a CLI would spawn.
 *   serve.test.js         stands up a node:http server. The native backend
 *                         does not serve yet; that is the plan's Phase 1c.
 *   images.test.js        the @jsquash codecs are WebAssembly, which QuickJS
 *                         does not have. $.resize cannot work here at all.
 *   build.test.js         same reason, one step removed: it builds
 *                         examples/blog at module top level, and that example
 *                         calls $.resize — so the file cannot even be
 *                         imported here. site-memory-build.test.js covers
 *                         buildSite through a provider, which is the path
 *                         this backend actually takes.
 *   opfs-provider.test.js OPFS is a browser API.
 *   search-widget.test.js runs the shipped widget against a fake DOM in
 *                         node:vm; it is testing browser JavaScript.
 *
 * That is 91 of 776 — 63 in those files plus build.test.js's 28. The other
 * 685 run here, of which one more is skipped by name (see `unsupported`
 * below), so 684 actually execute.
 */
import { run } from './shims/node/test.js';

import '../../test/fs-provider.test.js';
import '../../test/imports.test.js';
import '../../test/mdy.test.js';
import '../../test/parse.test.js';
import '../../test/publish.test.js';
import '../../test/search.test.js';
import '../../test/site-memory-build.test.js';
import '../../test/site-vault.test.js';
import '../../test/vault.test.js';

/*
 * Individual tests this runtime cannot run, and why. Each is a property of the
 * runtime rather than of the port — the same test runs and passes under
 * `npm test`, so it is not skipped there.
 *
 * The runner fails if a name here matches nothing, so a stale entry cannot
 * quietly hide a real result.
 */
const unsupported = new Map([
  [
    'a script can $.resize a raw image record — a real, correctly-sized thumbnail lands in binaryOutputs',
    '$.resize needs WebAssembly for its @jsquash codecs, and QuickJS has none. ' +
      'Not a gap in the port: nothing can make this work without an image codec in C.',
  ],
]);

const started = Date.now();
const summary = await run({ unsupported });

print('');
print(`# native: ${summary.pass} passed, ${summary.fail} failed, ${summary.skip} skipped in ${Date.now() - started}ms`);
if (summary.fail) {
  print('');
  print('# failures');
  for (const f of summary.failures) print(`  ${f.label}`);
}
globalThis.__exit_status = summary.fail ? 1 : 0;
