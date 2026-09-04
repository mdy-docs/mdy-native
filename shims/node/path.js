/*
 * `node:path`, POSIX-only — which is the right answer here rather than a
 * limitation. Every path in this system is `/`-separated: the provider
 * contract says so, src/imports.js does string maths on that assumption, and
 * the native host translates the one backslash Windows hands back (see
 * src/fsx.c's fsx_cwd). So `sep` is `/` on every platform, deliberately.
 */

export const sep = '/';
export const delimiter = ':';

/*
 * To `/`-separated, and with any leading slashes in front of a drive letter
 * removed — `/D:/x` and `//D:/x` both become `D:/x`.
 *
 * That second rule is the whole of this shim's Windows handling, and it earns
 * its place. mdy-docs says "this path is absolute" by passing it as
 * `fs.read('/', absolutePath)` — see loadModule in
 * ../../../../src/imports.js — and nodeFsProvider joins those two before
 * reaching the filesystem. On Windows the absolute path starts `D:/`, so the
 * join produces `/D:/…`, which names nothing. Node's own win32 join produces
 * the same thing; this is not a case it was built for. It is the same rule
 * fileURLToPath applies to `file:///C:/x`.
 */
const clean = (p) => String(p).replace(/\\/g, '/').replace(/^\/+(?=[A-Za-z]:\/)/, '');

export function normalize(p) {
  const s = clean(p);
  const absolute = s.startsWith('/');
  const drive = /^([A-Za-z]:)\//.exec(s);
  const out = [];
  for (const part of s.split('/')) {
    if (part === '' || part === '.') continue;
    if (part === '..' && out.length && out[out.length - 1] !== '..') out.pop();
    else out.push(part);
  }
  if (drive) return out.join('/');
  const joined = out.join('/');
  return absolute ? `/${joined}` : (joined || '.');
}

export function join(...parts) {
  const kept = parts.filter((p) => p !== '' && p !== undefined && p !== null).map(clean);
  if (kept.length === 0) return '.';
  return normalize(kept.join('/'));
}

export function dirname(p) {
  const s = clean(p).replace(/\/+$/, '');
  const cut = s.lastIndexOf('/');
  if (cut === -1) return '.';
  if (cut === 0) return '/';
  return s.slice(0, cut);
}

export function basename(p, ext) {
  const s = clean(p).replace(/\/+$/, '');
  const name = s.slice(s.lastIndexOf('/') + 1);
  return ext && name.endsWith(ext) && name !== ext ? name.slice(0, -ext.length) : name;
}

export function extname(p) {
  const name = basename(p);
  const dot = name.lastIndexOf('.');
  return dot <= 0 ? '' : name.slice(dot);
}

export function isAbsolute(p) {
  const s = clean(p);
  return s.startsWith('/') || /^[A-Za-z]:\//.test(s);
}

export function resolve(...parts) {
  let out = '';
  for (let i = parts.length - 1; i >= 0 && !isAbsolute(out); i--) {
    const part = clean(parts[i] ?? '');
    if (part === '') continue;
    out = out === '' ? part : `${part}/${out}`;
  }
  if (!isAbsolute(out)) out = `${globalThis.__fs_cwd()}/${out}`;
  return normalize(out);
}

export function relative(from, to) {
  const a = resolve(from).split('/');
  const b = resolve(to).split('/');
  while (a.length && b.length && a[0] === b[0]) { a.shift(); b.shift(); }
  return [...a.map(() => '..'), ...b].join('/');
}

export const posix = { sep, delimiter, normalize, join, dirname, basename, extname, isAbsolute, resolve, relative };
export const win32 = posix;
export default { ...posix, posix, win32 };
