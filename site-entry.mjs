/*
 * `mdy build`, natively: the same buildSite the CLI calls, over a filesystem
 * reached through five C calls instead of node:fs.
 *
 *   ./build/mdy-native build/site.js <site-dir> [out-dir]
 *
 * There is no mdy-docs code in here — buildSite renders, writes every output
 * and copies each root's static/ through, exactly as it does for `mdy build`.
 * All this file supplies is the provider and the argument handling, which is
 * the whole claim: the backend is a host, not a fork.
 */
import { buildSite } from '../../index.js';
import { nativeFsProvider } from './shims/fs.js';

const [rawRoot, rawOut] = globalThis.__argv ?? [];
if (!rawRoot) {
  print('usage: mdy-native build/site.js <site-dir> [out-dir]');
  globalThis.__exit_status = 2;
} else {
  /*
   * mdy-docs keys a module's registry identity by absolute path — see
   * canonicalizeModule in src/imports.js — so a relative root has to be made
   * absolute before anything imports anything. node does this with
   * path.resolve; here only the host knows the cwd.
   *
   * The `..` collapse is not cosmetic. imports.js decides whether a module is
   * inside its package by comparing the resolved specifier to the package
   * directory as a STRING PREFIX, so a root spelled `../../examples/docs-site`
   * makes every one of that site's own modules look like an escape attempt.
   * That is the right check — it is what stops a template reaching outside its
   * package — and the fix belongs here, where the spelling comes from.
   */
  // "Absolute" is a leading slash on POSIX and a drive letter on Windows —
  // `C:/work/site`. Treating the latter as relative would prepend the cwd and
  // produce `C:/work/C:/work/site`.
  const isAbsolute = (p) => p.startsWith('/') || /^[A-Za-z]:[/\\]/.test(p);
  const absolute = (p) => {
    const raw = (isAbsolute(p) ? p : `${globalThis.__fs_cwd()}/${p}`).replace(/\\/g, '/');
    const drive = /^([A-Za-z]:)\//.exec(raw);
    const parts = raw.split('/');
    const out = [];
    for (const part of parts) {
      if (part === '' || part === '.') continue;
      if (part === '..') out.pop();
      else out.push(part);
    }
    return drive ? out.join('/') : `/${out.join('/')}`;
  };
  const root = absolute(rawRoot);
  const out = absolute(rawOut ?? `${root}/dist`);

  const started = Date.now();
  const written = [];
  const { pages, messages } = await buildSite(root, {
    fs: nativeFsProvider(),
    outDir: out,
    onWrite: (file) => written.push(file),
  });

  for (const file of written) print(`[write] ${file}`);
  for (const m of messages) print(`[publish] ${m.channel ?? '?'}`);
  print(`built ${pages} page(s) → ${out} (${Date.now() - started}ms)`);
}
