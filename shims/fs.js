/*
 * The filesystem, natively — the third provider.
 *
 * mdy-docs takes its filesystem as an argument (`renderSite(root, { fs })`)
 * and ships two of its own: the real one over node:fs, and an in-memory Map.
 * This is the same nine-method contract, documented in
 * ../../../src/fs-provider.js, over the five natives in ../src/fsx.c.
 *
 * The methods are async because the contract is, not because anything here
 * waits: POSIX is synchronous and so is every call below. That costs nothing
 * — an already-resolved promise is a microtask — and it means the provider is
 * substitutable for the node one without mdy-docs knowing which it has.
 *
 * `watch` is absent, deliberately. It is optional at every call site
 * (`fs.watch?.(…)`), and a native recursive watcher is kqueue on macOS,
 * inotify on Linux and ReadDirectoryChangesW on Windows — three
 * implementations, which is Phase 3's work and not a line to sneak in here.
 * A build does not watch; `mdy dev` does.
 */

const utf8 = new TextDecoder();
const bytes = new TextEncoder();

/** An ArrayBuffer holding exactly these bytes — `.buffer` on a view is the
 * whole underlying buffer, which is only the same thing sometimes. */
const bufferOf = (u8) =>
  u8.byteOffset === 0 && u8.byteLength === u8.buffer.byteLength ? u8.buffer : u8.slice().buffer;

const missing = (root, rel) => new Error(`ENOENT: no such file or directory, '${root}/${rel}'`);

export function nativeFsProvider() {
  const readBytes = (root, relPath) => {
    const buf = globalThis.__fs_read(String(root), String(relPath));
    if (buf === null) throw missing(root, relPath);
    return new Uint8Array(buf);
  };

  const stat = (root, relPath) => {
    const st = globalThis.__fs_stat(String(root), String(relPath));
    if (st === null) throw missing(root, relPath);
    return st; // [size, mtimeMillis]
  };

  return {
    /**
     * Every file under `root/subdir`, recursively, as sorted `/`-separated
     * relative paths. Missing directory → []. `options.extensions` defaults to
     * ['.mdy']; `null` means every file whatever its extension.
     */
    async list(root, subdir, options = {}) {
      const extensions = 'extensions' in options ? options.extensions : ['.mdy'];
      // One string back, split here: a corpus listing is thousands of paths,
      // and the C side already sorted it (see fsx_list).
      const listing = globalThis.__fs_list(
        String(root),
        String(subdir ?? '.'),
        extensions ? extensions.join(',') : null
      );
      if (!listing) return [];
      return listing.length === 0 ? [] : listing.slice(0, -1).split('\n');
    },

    async read(root, relPath) {
      return utf8.decode(readBytes(root, relPath));
    },

    async readBinary(root, relPath) {
      return readBytes(root, relPath);
    },

    async mtime(root, relPath) {
      return new Date(stat(root, relPath)[1]);
    },

    async size(root, relPath) {
      return stat(root, relPath)[0];
    },

    async write(root, relPath, text) {
      const rc = globalThis.__fs_write(String(root), String(relPath), bufferOf(bytes.encode(String(text))));
      if (rc !== 0) throw new Error(`cannot write ${root}/${relPath}`);
    },

    async writeBinary(root, relPath, data) {
      const rc = globalThis.__fs_write(String(root), String(relPath), bufferOf(data));
      if (rc !== 0) throw new Error(`cannot write ${root}/${relPath}`);
    },

    /** A missing path is not an error — the contract says so. */
    async remove(root, relPath) {
      const rc = globalThis.__fs_remove(String(root), String(relPath));
      if (rc !== 0) throw new Error(`cannot remove ${root}/${relPath}`);
    },
  };
}
