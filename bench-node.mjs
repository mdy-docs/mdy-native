/* The bench's other half: the same set, the same mdy-docs, over the WASM
 * engines in node. Run through `make bench`, which times both. */
import { renderDocumentSet } from '../../index.js';
import { SOURCE } from './bench-body.mjs';

const t0 = Date.now();
const html = await renderDocumentSet(SOURCE);
console.log(`node:   200 docs, ${html.length} chars, ${Date.now() - t0}ms`);
