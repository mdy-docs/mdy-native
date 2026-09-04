/*
 * What the native backend runs, and the check that it ran correctly.
 *
 * This is ordinary mdy-docs — the same `renderDocumentSet` a node build calls
 * — and nothing in it is aware that lamassu and nisaba are linked as C beneath
 * it rather than loaded as WebAssembly. That is the whole claim, so the
 * exercise is chosen to cross every boundary that claim depends on:
 *
 *   $.find      guest -> host -> nisaba -> back, as a query with a filter
 *   $.render    a nested render, which recurses on a SECOND lamassu instance
 *               while the first is suspended mid-host-call
 *   unicode     an em dash and a cuneiform sign, so a UTF-8/UTF-16 round trip
 *               through both engines is checked rather than assumed
 *
 * Exit status is the verdict: `make native` is a test, not a demo.
 */
import { renderDocumentSet } from '../../index.js';

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

const EXPECTED = [
  ['the title, from the document\'s own front matter', /<h2 id="the-tablet-house">The Tablet House<\/h2>/],
  ['both cities, found by query', /Uruk/],
  ['…in the order they were written', /Uruk[\s\S]*Babylon/],
  ['each one through a nested render', /<li>Babylon — Akkad 𒀭<\/li>/],
];

const html = await renderDocumentSet(SOURCE);

print('--- mdy-native: mdy-docs on QuickJS, engines linked as C ---');
let failed = 0;
for (const [what, pattern] of EXPECTED) {
  const ok = pattern.test(html);
  if (!ok) failed++;
  print(`  ${ok ? 'ok  ' : 'FAIL'}  ${what}`);
}
if (failed) print(`\n${html}`);
print(failed ? `\n${failed} check(s) failed` : '\nall checks passed');
globalThis.__exit_status = failed ? 1 : 0;
