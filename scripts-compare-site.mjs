/*
 * A whole site, built twice, diffed byte for byte.
 *
 * mdy-docs builds it the way it always has — its own JavaScript, with the
 * templates running in lamassu's wasm build. `mdy-build` builds it with the C
 * engine: no second JavaScript engine, the same lamassu compiled natively,
 * and every stage in
 * between written in C. The two must agree exactly.
 *
 * This is the only test that has ever caught most of what it catches. A
 * property silently becoming another property, a number's last digit, a
 * lowercase mapping that expands, a tree spliced twice arriving empty the
 * second time — none of these fail a unit test, and every one of them changes
 * a page.
 *
 *   node scripts-compare-site.mjs <site-dir> [--entry main.mdy]
 */
import { execFile } from 'node:child_process';
import { mkdtemp, readdir, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, relative, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';

const run = promisify(execFile);
const here = dirname(fileURLToPath(import.meta.url));

async function walk(root, at = root, out = []) {
  for (const entry of await readdir(at, { withFileTypes: true })) {
    const path = join(at, entry.name);
    if (entry.isDirectory()) await walk(root, path, out);
    else out.push(relative(root, path));
  }
  return out.sort();
}

const [, , site, ...rest] = process.argv;
if (!site) {
  console.error('usage: node scripts-compare-site.mjs <site-dir> [--entry main.mdy]');
  process.exit(2);
}
const entryAt = rest.indexOf('--entry');
const entry = entryAt === -1 ? 'main.mdy' : rest[entryAt + 1];
/*
 * How many files are EXPECTED to differ. Zero unless a site exercises one of
 * the two known divergences, and the check fails if the count moves in either
 * direction — a difference that goes away is as much a change as one that
 * appears, and both want looking at.
 */
const expectAt = rest.indexOf('--expect');
const expect = expectAt === -1 ? 0 : Number(rest[expectAt + 1]);

const work = await mkdtemp(join(tmpdir(), 'mdy-compare-'));
const jsOut = join(work, 'js');
const cOut = join(work, 'c');

try {
  /* A build that fails is a result, not a crash — say which one and why. */
  const build = async (what, cmd, args) => {
    try {
      await run(cmd, args, { maxBuffer: 64 * 1024 * 1024 });
    } catch (err) {
      const why = String(err.stderr || err.message).trim().split('\n').slice(-3).join('\n      ');
      console.log(`the ${what} build failed:\n      ${why}`);
      process.exitCode = 1;
      return false;
    }
    return true;
  };

  if (!await build('JavaScript', process.execPath,
        [join(here, '../../bin/mdy.js'), 'build', site, '--entry', entry, '--out', jsOut])) {
    process.exit();
  }
  if (!await build('C', join(here, 'build/mdy-build'),
        [site, '--entry', entry, '--out', cOut, '--quiet'])) {
    process.exit();
  }

  const [a, b] = await Promise.all([walk(jsOut), walk(cOut)]);
  const differ = [];
  const images = [];
  const PNG_MAGIC = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  for (const name of new Set([...a, ...b])) {
    if (!a.includes(name)) { differ.push(`${name}: only the C build wrote it`); continue; }
    if (!b.includes(name)) { differ.push(`${name}: only the JavaScript build wrote it`); continue; }
    const [x, y] = await Promise.all([
      readFile(join(jsOut, name)), readFile(join(cOut, name)),
    ]);
    /*
     * A resized PNG is the one output that cannot match byte for byte: the
     * JavaScript resizes with Squoosh's codecs and the C engine with stb, so
     * the same request gives a visually equivalent image in a different file.
     * Both are still checked — that each is a PNG, and that both claim the
     * same dimensions, which is what a page's markup was written against.
     */
    if (!x.equals(y) && name.endsWith('.png')) {
      const size = (b) => (b.length > 24 && b.subarray(0, 8).equals(PNG_MAGIC)
        ? `${b.readUInt32BE(16)}x${b.readUInt32BE(20)}` : 'not a PNG');
      const a2 = size(x), b2 = size(y);
      if (a2 === b2 && a2 !== 'not a PNG') { images.push(`${name} (${a2})`); continue; }
      differ.push(`${name}: ${a2} against ${b2}`);
      continue;
    }
    if (!x.equals(y)) {
      /*
       * A WINDOW around the first character that differs, not the start of the
       * line: a rendered page is one line of a hundred thousand characters,
       * and printing its first 160 shows two identical prefixes.
       */
      const xs = x.toString('utf8').split('\n');
      const ys = y.toString('utf8').split('\n');
      let line = 0;
      while (line < xs.length && line < ys.length && xs[line] === ys[line]) line++;
      const xl = xs[line] ?? '', yl = ys[line] ?? '';
      let col = 0;
      while (col < xl.length && col < yl.length && xl[col] === yl[col]) col++;
      const from = Math.max(0, col - 40);
      const cut = (t) => (from > 0 ? '…' : '') + t.slice(from, col + 120) +
                         (t.length > col + 120 ? '…' : '');
      differ.push(
        `${name}: line ${line + 1}, column ${col + 1}\n` +
        `    js: ${xs[line] === undefined ? '(end of file)' : cut(xl)}\n` +
        `    c : ${ys[line] === undefined ? '(end of file)' : cut(yl)}`);
    }
  }

  const total = new Set([...a, ...b]).size;
  const note = images.length
    ? ` (${images.length} resized image(s) equivalent, not identical)`
    : '';
  if (differ.length === expect) {
    console.log(differ.length === 0
      ? `all ${total} file(s) identical${note}`
      : `${total} file(s), ${differ.length} differing as expected${note}`);
  } else {
    console.log(`${differ.length} of ${total} file(s) differ, expected ${expect}\n`);
    for (const line of differ.slice(0, 10)) console.log(`  ${line}`);
    if (differ.length > 10) console.log(`  … and ${differ.length - 10} more`);
    process.exitCode = 1;
  }
} finally {
  await rm(work, { recursive: true, force: true });
}
