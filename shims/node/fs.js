/*
 * `node:fs` and `node:fs/promises`, over the five natives in ../../src/fsx.c.
 *
 * Two things this is for. mdy-docs' own tests write a site into a temp
 * directory and build it, so running them natively needs real filesystem
 * calls. And `nodeFsProvider` in ../../../../src/fs-provider.js reaches for
 * `node:fs/promises` through a lazy dynamic import — so aliasing this makes
 * the DEFAULT provider work on the native backend too, which is why a test
 * that calls `renderScriptSite(dir)` with no provider runs here unchanged.
 *
 * Every call is synchronous underneath; the promise API is the same functions
 * behind an already-resolved promise. That costs nothing and keeps one
 * implementation.
 */

const utf8 = new TextDecoder();
const enc = new TextEncoder();

const bufferOf = (u8) =>
  u8.byteOffset === 0 && u8.byteLength === u8.buffer.byteLength ? u8.buffer : u8.slice().buffer;

/** node's errors are matched on `.code` in several places, so they carry one. */
function fsError(code, syscall, path) {
  const err = new Error(`${code}: ${syscall} '${path}'`);
  err.code = code;
  err.syscall = syscall;
  err.path = path;
  return err;
}

const split = (p) => {
  // Every native takes (root, relative); a whole path is relative to "/",
  // which src/fsx.c's `at` recognises and returns unchanged.
  const s = String(p).replace(/\\/g, '/');
  return ['/', s];
};

export function readFileSync(path, options) {
  const [root, rel] = split(path);
  const buf = globalThis.__fs_read(root, rel);
  if (buf === null) throw fsError('ENOENT', 'open', path);
  const bytes = new Uint8Array(buf);
  const encoding = typeof options === 'string' ? options : options?.encoding;
  return encoding ? utf8.decode(bytes) : bytes;
}

export function writeFileSync(path, data, options) {
  const [root, rel] = split(path);
  const bytes = typeof data === 'string' ? enc.encode(data) : new Uint8Array(data);
  if (globalThis.__fs_write(root, rel, bufferOf(bytes)) !== 0) throw fsError('EACCES', 'write', path);
}

export function existsSync(path) {
  const [root, rel] = split(path);
  // A directory has no size but does have a stat; readdir answers for one.
  return globalThis.__fs_stat(root, rel) !== null || globalThis.__fs_readdir(String(path)) !== null;
}

export function mkdirSync(path) {
  if (globalThis.__fs_mkdir(String(path)) !== 0) throw fsError('EACCES', 'mkdir', path);
}

export function mkdtempSync(prefix) {
  const made = globalThis.__fs_mkdtemp(String(prefix));
  if (made === null) throw fsError('EACCES', 'mkdtemp', prefix);
  return made;
}

export function rmSync(path) {
  if (globalThis.__fs_rm(String(path)) !== 0) throw fsError('EACCES', 'rm', path);
}

/** One directory level. `withFileTypes` gets the shape the suite reads. */
export function readdirSync(path, options) {
  const listing = globalThis.__fs_readdir(String(path));
  if (listing === null) throw fsError('ENOENT', 'scandir', path);
  const entries = listing === '' ? [] : listing.slice(0, -1).split('\n');
  if (!options?.withFileTypes) return entries.map((e) => e.replace(/\/$/, '')).sort();
  return entries
    .map((e) => {
      const isDir = e.endsWith('/');
      const name = isDir ? e.slice(0, -1) : e;
      return { name, parentPath: String(path), path: String(path),
               isDirectory: () => isDir, isFile: () => !isDir };
    })
    .sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0));
}

export function copyFileSync(from, to) {
  writeFileSync(to, readFileSync(from));
}

/** Recursive copy, the `{ recursive: true }` shape the suite uses. */
export function cpSync(from, to, options = {}) {
  const listing = globalThis.__fs_readdir(String(from));
  if (listing === null) { // a file, not a directory
    if (options.filter && !options.filter(String(from), String(to))) return;
    copyFileSync(from, to);
    return;
  }
  mkdirSync(to);
  const entries = listing === '' ? [] : listing.slice(0, -1).split('\n');
  for (const entry of entries) {
    const isDir = entry.endsWith('/');
    const name = isDir ? entry.slice(0, -1) : entry;
    const src = `${from}/${name}`;
    const dest = `${to}/${name}`;
    if (options.filter && !options.filter(src, dest)) continue;
    if (isDir) cpSync(src, dest, options);
    else copyFileSync(src, dest);
  }
}

export function statSync(path) {
  const [root, rel] = split(path);
  const st = globalThis.__fs_stat(root, rel);
  if (st === null) throw fsError('ENOENT', 'stat', path);
  return { size: st[0], mtime: new Date(st[1]), mtimeMs: st[1],
           isDirectory: () => globalThis.__fs_readdir(String(path)) !== null,
           isFile: () => globalThis.__fs_readdir(String(path)) === null };
}

/*
 * `fs.watch`, BY POLLING — which has to be said plainly, because node's is not.
 *
 * A real recursive watcher is kqueue on macOS, inotify on Linux and
 * ReadDirectoryChangesW on Windows: three implementations, and the plan's
 * Phase 3. This is the same answer mdy-docs already gives where a native
 * watcher is unavailable — `watchByPolling` in ../../../../src/fs-provider.js
 * backs the OPFS provider the same way — so it is the codebase's existing
 * position rather than a new one.
 *
 * What that costs is honest to state: changes are noticed on the next tick
 * rather than immediately, and every tick walks the tree. Fine for a document
 * set; not something to point at a large directory.
 */
export function watch(root, options, listener) {
  const cb = typeof options === 'function' ? options : listener;
  const interval = 60;

  const snapshot = () => {
    const seen = new Map();
    const listing = globalThis.__fs_list(String(root), '.', null);
    if (listing) {
      for (const rel of listing === '' ? [] : listing.slice(0, -1).split('\n')) {
        const st = globalThis.__fs_stat(String(root), rel);
        if (st) seen.set(rel, `${st[0]}:${st[1]}`);
      }
    }
    return seen;
  };

  let previous = snapshot();
  let stopped = false;
  let timer = null;

  const tick = () => {
    if (stopped) return;
    const next = snapshot();
    for (const [path, stamp] of next) {
      // node's fs.watch says 'rename' for appearing and disappearing, and
      // 'change' for content — the same two words this maps onto.
      if (!previous.has(path)) cb('rename', path);
      else if (previous.get(path) !== stamp) cb('change', path);
    }
    for (const path of previous.keys()) if (!next.has(path)) cb('rename', path);
    previous = next;
    timer = setTimeout(tick, interval);
  };
  timer = setTimeout(tick, interval);

  return {
    close() { stopped = true; if (timer !== null) clearTimeout(timer); },
    unref() { return this; },
  };
}

/* ---- the promise API: the same calls, already resolved -------------------- */

export const readFile = async (p, o) => readFileSync(p, o);
export const writeFile = async (p, d, o) => writeFileSync(p, d, o);
export const mkdir = async (p) => { mkdirSync(p); };
export const mkdtemp = async (p) => mkdtempSync(p);
export const rm = async (p) => { rmSync(p); };
export const readdir = async (p, o) => {
  // node's `{ recursive: true }` walks the whole tree; the provider's own
  // list() is exactly that, so it is used rather than re-walked here.
  if (o?.recursive) {
    const listing = globalThis.__fs_list(String(p), '.', null);
    const files = listing ? (listing === '' ? [] : listing.slice(0, -1).split('\n')) : [];
    if (!o.withFileTypes) return files;
    return files.map((rel) => {
      const cut = rel.lastIndexOf('/');
      const dir = cut === -1 ? String(p) : `${p}/${rel.slice(0, cut)}`;
      return { name: cut === -1 ? rel : rel.slice(cut + 1), parentPath: dir, path: dir,
               isDirectory: () => false, isFile: () => true };
    });
  }
  return readdirSync(p, o);
};
export const cp = async (f, t, o) => { cpSync(f, t, o); };
export const copyFile = async (f, t) => { copyFileSync(f, t); };
export const stat = async (p) => statSync(p);
export const access = async (p) => { if (!existsSync(p)) throw fsError('ENOENT', 'access', p); };

export const promises = {
  readFile, writeFile, mkdir, mkdtemp, rm, readdir, cp, copyFile, stat, access,
};

export default {
  readFileSync, writeFileSync, existsSync, mkdirSync, mkdtempSync, rmSync,
  readdirSync, copyFileSync, cpSync, statSync, watch, promises,
};
