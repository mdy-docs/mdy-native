/*
 * What the native backend runs, and the check that it ran correctly.
 *
 * This is ordinary mdy-docs — the same `renderDocumentSet` and `buildSite` a
 * node build calls — and nothing in it is aware that lamassu and nisaba are
 * linked as C beneath it, or that the filesystem is five C functions rather
 * than node:fs. That is the whole claim, so the checks are chosen to cross
 * every boundary it depends on rather than to exercise mdy-docs, which has its
 * own 776 tests for that.
 *
 * Exit status is the verdict: `make native` is a test, not a demo.
 */
import { buildSite, renderDocumentSet } from '../../index.js';
import { nativeFsProvider } from './shims/fs.js';

const checks = [];
const check = (what, ok, detail) => checks.push({ what, ok: Boolean(ok), detail });

/* ---- the engines: a document set in memory -------------------------------- */

const SOURCE = [
  '+++', 'title: The Tablet House', '+++',
  '== {{ res.data.title }}',
  '',
  '% for (const m of $.find({ role: "city" })) {',
  '{{ $.render({ role: "card" }, { who: m.who, era: m.era }) }}',
  '% }',
  '---',
  '+++', 'role: card', '+++',
  '- {{ req.who }} — {{ req.era }} 𒀭',
  '---',
  '+++', 'role: city', 'who: Uruk', 'era: Sumer', '+++',
  '---',
  '+++', 'role: city', 'who: Babylon', 'era: Akkad', '+++',
].join('\n');

const html = await renderDocumentSet(SOURCE);
check('the title, from the document’s own front matter', /<h2 id="the-tablet-house">The Tablet House<\/h2>/.test(html));
check('both cities, found by query', /Uruk/.test(html) && /Babylon/.test(html));
check('…in the order they were written', /Uruk[\s\S]*Babylon/.test(html));
// A nested render recurses onto a SECOND lamassu instance while the first is
// suspended mid-host-call; the cuneiform sign checks the UTF-8/UTF-16 round
// trip through both engines rather than assuming it.
check('each one through a nested render', /<li>Babylon — Akkad 𒀭<\/li>/.test(html));

/* ---- the filesystem: a real directory, built to disk ---------------------- */

const fs = nativeFsProvider();
const here = globalThis.__fs_cwd();
const root = `${here}/fixture`;
const out = `${here}/build/entry-out`;

const listed = await fs.list(root, '.');
check('the provider walks a directory recursively', listed.length === 5, listed.join(' '));
check('…and filters by extension', listed.every((p) => p.endsWith('.mdy')), listed.join(' '));
check('…and sorts', String(listed) === String([...listed].sort()));
check('a missing directory lists empty, not an error', (await fs.list(root, 'nope')).length === 0);
check('a text file reads back as text', (await fs.read(root, 'main.mdy')).startsWith('+++'));
check('stat answers size and mtime', (await fs.size(root, 'main.mdy')) > 0 &&
  (await fs.mtime(root, 'main.mdy')) instanceof Date);

// Write / read / remove, including a directory that does not exist yet.
await fs.write(root, '../build/scratch/deep/note.txt', 'wrote 𒀭');
check('write creates parent directories', (await fs.read(root, '../build/scratch/deep/note.txt')) === 'wrote 𒀭');
await fs.remove(root, '../build/scratch/deep/note.txt');
await fs.remove(root, '../build/scratch/deep/note.txt'); // twice: missing is not an error
check('remove is idempotent', true);

const { pages } = await buildSite(root, { fs, outDir: out });
const built = await fs.list(out, '.', { extensions: null });
check('buildSite renders and writes through the provider', pages === 4, `${pages} page(s)`);
check('…every emitted output', ['babylon/index.html', 'cities.json', 'uruk/index.html'].every((f) => built.includes(f)), built.join(' '));
check('…and static/ copied through', built.includes('style.css'), built.join(' '));

// Both of these are only true if the engine's module loader ran: the JSON is
// built by slug() from lib/util.js, and the banner by shout(), which calls
// upper() from lib/case.js — a module importing a module, through the loader,
// twice, the second time asked for by a module rather than by a document.
const cities = await fs.read(out, 'cities.json');
check('a guest `import` loaded a JS module', cities === '["babylon","uruk"]', cities);
const index = await fs.read(out, 'index.html');
check('…and that module imported its own dependency', index.includes('BUILT NATIVELY!'), index.trim());

/* ---- the verdict ---------------------------------------------------------- */

print('--- mdy-native: mdy-docs on QuickJS, engines and filesystem in C ---');
let failed = 0;
for (const c of checks) {
  if (!c.ok) failed++;
  print(`  ${c.ok ? 'ok  ' : 'FAIL'}  ${c.what}${c.ok || !c.detail ? '' : `  (${c.detail})`}`);
}
print(failed ? `\n${failed} of ${checks.length} check(s) failed` : `\nall ${checks.length} checks passed`);
globalThis.__exit_status = failed ? 1 : 0;
