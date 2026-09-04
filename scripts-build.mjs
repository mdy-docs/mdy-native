/*
 * Bundle mdy-docs for the native backend.
 *
 * platform: neutral, because this is neither node nor a browser — it is
 * QuickJS. The two engine packages are aliased to the shims that call the C
 * bindings, which is the only substitution the design needs: everything above
 * them is mdy-docs unchanged.
 */
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as esbuild from 'esbuild';

const here = dirname(fileURLToPath(import.meta.url));

/* Which entry to bundle: `node scripts-build.mjs bench` builds bench-entry.mjs
 * to build/bench.js. No argument means the checked render, build/mdy.js. */
const which = process.argv[2] ?? '';
const entry = which ? `${which}-entry.mjs` : 'entry.mjs';
const out = which ? `${which}.js` : 'mdy.js';

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
  },
  external: ['node:*'],
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
