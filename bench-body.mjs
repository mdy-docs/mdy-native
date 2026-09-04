/* The set both halves of the bench render, so the comparison is of backends
 * and not of two slightly different workloads. 200 documents found by one
 * query and rendered through a nested render each: that exercises the query
 * engine, the host-call boundary and the second-VM recursion, which is where
 * the two backends actually differ. */
export const SOURCE = (() => {
  const parts = [
    '+++', 'title: Corpus', '+++',
    '== {{ res.data.title }}', '',
    '% for (const m of $.find({ role: "city" })) {',
    '{{ $.render({ role: "card" }, { who: m.who, era: m.era }) }}',
    '% }', '---',
    '+++', 'role: card', '+++',
    '- {{ req.who }} \u2014 {{ req.era }}',
  ];
  for (let i = 0; i < 200; i++) {
    parts.push('---', '+++', 'role: city', `who: City${i}`, `era: Era${i % 7}`, '+++');
  }
  return parts.join('\n');
})();
