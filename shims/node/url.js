/*
 * `node:url`. Only fileURLToPath is used, and only to turn `import.meta.url`
 * into a directory a test can resolve a fixture against.
 */
export function fileURLToPath(url) {
  const s = String(url);
  if (!s.startsWith('file://')) return s;
  const path = decodeURIComponent(s.slice('file://'.length));
  // file:///C:/x on Windows: the leading slash is not part of the path.
  return /^\/[A-Za-z]:/.test(path) ? path.slice(1) : path;
}

export function pathToFileURL(path) {
  const p = String(path).replace(/\\/g, '/');
  return { href: `file://${p.startsWith('/') ? '' : '/'}${encodeURI(p)}`, toString() { return this.href; } };
}

export default { fileURLToPath, pathToFileURL };
