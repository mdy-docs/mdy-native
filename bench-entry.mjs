/* One half of `make bench`: 200 documents, each rendered through a nested
 * render, driven by one $.find over the set. bench-node.mjs is the other half
 * — the same source over the WASM engines in node. */
import { renderDocumentSet } from '../../index.js';
import { SOURCE } from './bench-body.mjs';

const t0 = Date.now();
const html = await renderDocumentSet(SOURCE);
print(`native: 200 docs, ${html.length} chars, ${Date.now() - t0}ms`);
