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
import { parse as parseYaml } from 'yaml';

import { normalizeDocuments } from '../../../src/parse/documents.js';
import { highlightCode, normalizeHighlight } from '../../../src/parse/highlight.js';
import { normalizeFrontmatter } from '../../../src/parse/matter.js';
import { collectReferences } from '../../../src/parse/reference.js';

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

/**
 * Reaches the parser, or is handled here on the way in or out.
 *
 * `frontmatter` is the one worth explaining. The C FINDS the block — the fence
 * rule belongs next to everything else that reads lines — and hands back its
 * source, which is parsed here, because it is YAML and a YAML reader is not a
 * thing to write twice.
 *
 * `highlight` is handled on the way out. Highlighting decorates a finished
 * tree — a <code> element's text becomes a run of <span class="hljs-…"> — so
 * it belongs after the parse rather than inside it, and mdy-docs' own
 * highlighter runs here on the tree the C returns.
 *
 * `file` takes the warnings the C raised, which is what makes a dropped
 * <script> something an author is told about rather than something that
 * silently vanishes.
 */
const MAPPED = new Set([
  'documents', 'frontmatter', 'autolink', 'sanitize', 'lineOffset', 'highlight',
  'file',
]);

/**
 * Accepted and ignored, because they change nothing this stage produces.
 *
 * `collect` is overridden by fromMdy itself — it builds its own and passes
 * that to the inline parser — so the option never reaches anything.
 * `headingState` shares heading ids across a stream's documents; the C keeps
 * its own per document, which differs only for a page built from several
 * documents that repeat a heading.
 */
const IGNORED = new Set(['collect', 'headingState']);

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
  /*
   * A schema of your own. The C's is generated from sanitize.js and fixed at
   * compile time, so this is the one MAPPED option that has a shape it cannot
   * take — and a narrower schema silently ignored is a security answer that is
   * wrong in the dangerous direction.
   */
  if (options.sanitize !== undefined && typeof options.sanitize !== 'boolean') {
    refuse('sanitize', 'only `true` and `false` are implemented, not a schema of your own');
  }

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

/*
 * `hljs` on a finished tree.
 *
 * The C emits `<pre><code class="language-x">` holding one text node, which is
 * exactly what an unhighlighted block is; this turns that text into the spans
 * and adds the second class. Anything without a language, or with a language
 * the highlighter does not know, is left alone — a language nothing knows is
 * not an error, the class still says what it was meant to be and the code
 * still reads.
 */
function highlight(node, highlighter) {
  if (!node || typeof node !== 'object') return;

  if (node.tagName === 'pre' && node.children?.length === 1) {
    const code = node.children[0];
    const classes = code?.properties?.className;
    const language = Array.isArray(classes)
      ? classes.find((name) => name.startsWith('language-'))?.slice(9)
      : undefined;

    if (language && code.children?.length === 1 && code.children[0].type === 'text') {
      const { children, highlighted } = highlightCode(
        code.children[0].value,
        language,
        highlighter
      );
      if (highlighted) {
        code.children = children;
        code.properties.className = [...classes, 'hljs'];
      }
      return;
    }
  }

  for (const child of node.children ?? []) highlight(child, highlighter);
}

/**
 * One document's front matter, read.
 *
 * `parse(source) ?? {}`, and a block that will not parse is REPORTED and then
 * treated as empty — the block comes out either way, because it was meant as
 * data and leaving it in as prose would be a stranger outcome than losing it.
 */
function readMatter(entry, file) {
  if (!entry) return undefined;

  let matter = {};
  try {
    matter = parseYaml(entry.source) ?? {};
  } catch (error) {
    file?.message(`Front matter failed to parse: ${error.message}`, {
      place: {
        start: { line: entry.open, column: 1 },
        end: { line: entry.close, column: entry.fenceLength + 1 },
      },
      ruleId: 'frontmatter',
      source: 'mdy',
    });
  }

  return matter !== null && typeof matter === 'object' ? matter : {};
}

export function fromMdy(document, options = {}) {
  check(options);

  const source = String(document ?? '');
  const stream = normalizeDocuments(options.documents);
  const matterSettings = normalizeFrontmatter(options.frontmatter);
  const highlighter = normalizeHighlight(options.highlight);
  const file = options.file;

  let flags = FLAG_POSITIONS;
  if (stream) flags |= FLAG_DOCUMENTS;
  if (matterSettings) flags |= FLAG_FRONTMATTER;
  if (options.autolink !== false) flags |= FLAG_AUTOLINK;
  if (options.sanitize !== false) flags |= FLAG_SANITIZE;

  // An empty string is `wrapper: false` — the documents run together.
  const wrapper = stream ? (stream.wrapper === false ? '' : stream.wrapper) : null;

  const result = globalThis.__mdy_parse(
    source,
    flags,
    options.lineOffset ?? 0,
    wrapper,
    matterSettings?.fence ?? null
  );
  if (result === null) {
    throw new Error('mdy-native: the C front end could not parse this document');
  }

  const tree = result.tree;

  for (const message of result.messages) {
    file?.message(message.reason, {
      place: message.place,
      ruleId: message.ruleId,
      source: 'mdy',
    });
  }

  if (highlighter) highlight(tree, highlighter);

  /*
   * Front matter, and what each document says it refers to.
   *
   * `tags`, `users` and `links` are on the data as empty arrays whether or not
   * the author named them, so a document may always be asked what it refers to
   * and always answers with a list. What the parser found is added to what the
   * author wrote, in the order the document reached it.
   */
  const fenceLength = matterSettings?.fence?.length ?? 3;
  const data = result.matter.map((entry) =>
    readMatter(entry && { ...entry, fenceLength }, file)
  );

  for (const [index, own] of data.entries()) {
    if (own === undefined) continue;
    const collect = collectReferences(own);
    for (const ref of result.refs) {
      if (ref.document === index) collect(ref.kind, ref.name);
    }
  }

  if (stream) {
    // Each document's own goes on its wrapper, when there is one to put it on.
    if (wrapper) {
      let index = 0;
      for (const child of tree.children) {
        if (child.type !== 'element') continue;
        if (data[index] !== undefined) child.data = { matter: data[index] };
        index += 1;
      }
    }
    // The file holds the FIRST, not whichever happened to be parsed last.
    if (file && data.some((own) => own !== undefined)) {
      file.data = { ...file.data, matter: data[0], documents: data };
    }
    return tree;
  }

  if (data[0] !== undefined) {
    tree.data = { ...tree.data, matter: data[0] };
    if (file) file.data = { ...file.data, matter: data[0] };
  }

  return tree;
}
