/*
 * Bundle mdy-docs for the native backend.
 *
 * platform: neutral, because this is neither node nor a browser — it is
 * QuickJS. The two engine packages are aliased to the shims that call the C
 * bindings, which is the only substitution the design needs: everything above
 * them is mdy-docs unchanged.
 */
import { readFileSync } from 'node:fs';
import { readdir } from 'node:fs/promises';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const here = dirname(fileURLToPath(import.meta.url));

/* Which entry to bundle: `node scripts-build.mjs bench` builds bench-entry.mjs
 * to build/bench.js. No argument means the checked render, build/mdy.js. */
const which = process.argv[2] ?? '';
const entry = which ? `${which}-entry.mjs` : 'entry.mjs';
const out = which ? `${which}.js` : 'mdy.js';

/*
 * `import.meta.url`, for the `tests` entry.
 *
 * Bundling collapses every module into one file, so `import.meta.url` would be
 * the bundle's own location rather than each test file's — and every test that
 * uses it does the same thing with it: `dirname(fileURLToPath(import.meta.url))`
 * to find its own directory, then reaches for `../examples/…`. Defining it as
 * a file in test/ makes all of them right.
 *
 * That works because EVERY user of it is a file in test/, which is checked
 * below rather than assumed. If a file in src/ ever starts using it, this
 * would silently hand that file the test directory — so the check makes it a
 * build failure instead.
 *
 * src/images.js is exempt, and specifically: its real use is
 * `import.meta.resolve` for the @jsquash codec paths, which are WebAssembly —
 * a thing this runtime does not have, and ensureCodecsReady throws before
 * reaching it. It matches the check only because a comment above that line
 * mentions import.meta.url.
 */
const EXEMPT = new Set(['images.js']);
const testDirUrl = `file://${join(here, '..', '..', 'test')}/entry.js`;

const usesImportMeta = (await readdir(join(here, '..', '..', 'src'), { recursive: true }))
  .filter((f) => f.endsWith('.js'))
  .filter((f) => !EXEMPT.has(f.split('/').pop()))
  .filter((f) => /import\.meta\.url/.test(readFileSync(join(here, '..', '..', 'src', f), 'utf8')));
if (which === 'tests' && usesImportMeta.length > 0) {
  throw new Error(
    `scripts-build: src/ now uses import.meta (${usesImportMeta.join(', ')}), and the tests ` +
      `entry defines import.meta.url as the test directory — see the note above. ` +
      `Give those files their own resolution or stop bundling the tests.`
  );
}

await esbuild.build({
  entryPoints: [join(here, entry)],
  outfile: join(here, 'build', out),
  bundle: true,
  format: 'esm',
  platform: 'neutral',
  conditions: ['default'],
  minify: false,
  alias: {
    '@mdy-docs/lamassu-js': join(here, 'shims', 'lamassu.js'),
    '@mdy-docs/nisaba-db': join(here, 'shims', 'nisaba.js'),

    /*
     * node's builtins, for the `tests` entry — mdy-docs' own suite, run
     * unmodified against this backend (see tests-entry.mjs). They are aliased
     * for every entry rather than only that one, and that is deliberate:
     * `nodeFsProvider` reaches for node:fs/promises through a lazy dynamic
     * import, so aliasing it makes the DEFAULT provider work natively too.
     * A test that calls renderScriptSite(dir) with no provider then runs here
     * with nothing changed.
     */
    'node:test': join(here, 'shims', 'node', 'test.js'),
    'node:assert': join(here, 'shims', 'node', 'assert.js'),
    'node:assert/strict': join(here, 'shims', 'node', 'assert.js'),
    'node:path': join(here, 'shims', 'node', 'path.js'),
    'node:fs': join(here, 'shims', 'node', 'fs.js'),
    'node:fs/promises': join(here, 'shims', 'node', 'fs-promises.js'),
    'node:os': join(here, 'shims', 'node', 'os.js'),
    'node:url': join(here, 'shims', 'node', 'url.js'),
    'node:zlib': join(here, 'shims', 'node', 'zlib.js'),
    'node:vm': join(here, 'shims', 'node', 'vm.js'),
  },
  // Anything still spelled node:* after the aliases above is something no
  // native path reaches — child_process, http, worker_threads. Left external
  // so a bundle that somehow needs one fails loudly at that import.
  external: ['node:*'],
  define: which === 'tests' ? { 'import.meta.url': JSON.stringify(testDirUrl) } : {},
  inject: [join(here, 'shims', 'node', 'buffer-inject.js')],
  banner: {
    js: `/* Shims QuickJS lacks. structuredClone is used by mdy-docs' ingest memo;
   TextEncoder/TextDecoder by the binjson codec. Both are small and neither is
   a language feature — this is the whole gap. */
globalThis.structuredClone ??= (v) => JSON.parse(JSON.stringify(v));
globalThis.TextEncoder ??= class { encode(s) {
  const out = []; for (const ch of String(s)) { let c = ch.codePointAt(0);
    if (c < 0x80) out.push(c);
    else if (c < 0x800) out.push(0xc0 | c >> 6, 0x80 | c & 63);
    else if (c < 0x10000) out.push(0xe0 | c >> 12, 0x80 | (c >> 6) & 63, 0x80 | c & 63);
    else out.push(0xf0 | c >> 18, 0x80 | (c >> 12) & 63, 0x80 | (c >> 6) & 63, 0x80 | c & 63); }
  return new Uint8Array(out); } };
globalThis.TextDecoder ??= class { decode(b) {
  const u = b instanceof Uint8Array ? b : new Uint8Array(b ?? []); let s = '';
  for (let i = 0; i < u.length;) { const c = u[i];
    let cp, n; if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xe0) === 0xc0) { cp = c & 31; n = 2; }
    else if ((c & 0xf0) === 0xe0) { cp = c & 15; n = 3; }
    else { cp = c & 7; n = 4; }
    for (let k = 1; k < n; k++) cp = (cp << 6) | (u[i + k] & 63);
    s += String.fromCodePoint(cp); i += n; }
  return s; } };
`,
  },
  logLevel: 'info',
});
console.log(`→ build/${out}`);
