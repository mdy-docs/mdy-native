/* `Buffer` is a global in node, not an import, so esbuild injects it wherever
 * a bundled file mentions the name. See shims/node/buffer.js. */
export { Buffer } from './buffer.js';
