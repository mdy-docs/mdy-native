/*
 * Stands in for src/parse/block.js — the MDY front end — backed by the C
 * parser in github.com/mdy-docs/parse.
 *
 * That module exports exactly one thing, `fromMdy(document, options)`, which
 * is what makes this substitutable at all: everything above it takes a hast
 * tree and does not care who built it.
 *
 * WHAT THE C DOES NOT DO, and what happens when it is asked. Every option
 * below either reaches the parser or is checked here and refused. Silently
 * ignoring one would produce a tree that is subtly not what the caller asked
 * for, which is the worst failure this boundary can have — far worse than
 * throwing, because it looks like it worked.
 */
const FLAG_DOCUMENTS = 1;
const FLAG_FRONTMATTER = 2;
const FLAG_AUTOLINK = 4;
const FLAG_POSITIONS = 8;
const FLAG_SANITIZE = 16;

/*
 * Every option mdy-docs' own fromMdy accepts, and what this can do with it.
 * The point of listing them all is that an option nobody thought about is a
 * missing key here rather than a silently different tree.
 */

/** Passed through to the C. */
const MAPPED = new Set(['documents', 'frontmatter', 'autolink', 'sanitize', 'lineOffset']);

/**
 * Accepted and ignored, because they change nothing this stage produces.
 *
 * `file` collects warnings and the C emits none. `collect` gathers references
 * the document engine re-derives itself. `headingState` shares heading ids
 * across a stream's documents; the C keeps its own per document, which differs
 * only for a page built from several documents that repeat a heading.
 *
 * `highlight` is the one to know about: fenced code comes out UNHIGHLIGHTED.
 * That is deliberate — highlighting decorates a finished tree (a <code>
 * element's text becomes a run of <span class="hljs-…">), so it belongs after
 * the parse rather than inside it, and can run in the VM on the tree this
 * returns. Refusing the option instead would break every caller, since
 * mdy-docs' default is on.
 */
const IGNORED = new Set(['file', 'collect', 'headingState', 'highlight']);

/**
 * Accepted only when switched OFF, because the C does not do them at all.
 * `script` is the one that matters: `%` lines run JavaScript, which is the
 * document engine's job rather than the parser's — and the engine passes
 * `script: false` for exactly that reason.
 */
const OFF_ONLY = new Set(['script']);

/**
 * Accepted only at their default, because the C implements the default and
 * nothing else. Passing `markers` a table of your own, or turning footnotes
 * off, would produce a tree that is not what was asked for.
 */
const DEFAULT_ONLY = new Set([
  'markers', 'tags', 'mentions', 'tasks', 'footnotes', 'wikiLink', 'emoji',
  'emDash', 'ellipsis', 'arrows', 'maxHeadingDepth', 'headingId', 'tableAlign',
]);

const DEFAULTS = {
  tasks: false,
  maxHeadingDepth: 6,
  tableAlign: 'style',
};

function refuse(key, why) {
  throw new Error(
    `mdy-native: the C front end cannot honour \`${key}\` — ${why}. ` +
      `See packages/mdy-native/shims/parse.js.`
  );
}

function check(options) {
  for (const key of Object.keys(options)) {
    const value = options[key];
    if (value === undefined) continue;
    if (MAPPED.has(key) || IGNORED.has(key)) continue;

    if (OFF_ONLY.has(key)) {
      if (value) refuse(key, 'it is not implemented here at all');
      continue;
    }
    if (DEFAULT_ONLY.has(key)) {
      const fallback = key in DEFAULTS ? DEFAULTS[key] : true;
      if (value !== fallback) refuse(key, 'only its default is implemented');
      continue;
    }
    refuse(key, 'it is not an option this front end knows');
  }
}

export function fromMdy(document, options = {}) {
  check(options);

  let flags = 0;
  if (options.documents) flags |= FLAG_DOCUMENTS;
  if (options.frontmatter !== false) flags |= FLAG_FRONTMATTER;
  if (options.autolink !== false) flags |= FLAG_AUTOLINK;
  if (options.sanitize !== false) flags |= FLAG_SANITIZE;
  flags |= FLAG_POSITIONS;

  const tree = globalThis.__mdy_parse(String(document ?? ''), flags, options.lineOffset ?? 0);
  if (tree === null) throw new Error('mdy-native: the C front end could not parse this document');
  return tree;
}
