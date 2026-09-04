/*
 * Stands in for @mdy-docs/nisaba-db, backed by the native engine.
 *
 * Only the surface mdy-docs actually uses is here — connect, a collection,
 * insertOne, find().toArray(), createIndex — which is six things, and the
 * reason this shim is short rather than a reimplementation of a database
 * client.
 *
 * Documents cross as binjson, because that is what nisaba's C speaks. The
 * codec is the reference JS one from nisaba's own submodule: a native host has
 * no JSON parser to build an encoder from, and duplicating the format in C
 * would be two implementations to keep in step.
 */
import { encode, decode, ObjectId } from '../../../third_party/nisaba-db/third_party/binjson/js/binjson.js';

/* nisaba refuses a document without an ObjectId `_id` — the primary tree's
 * keys are fixed-width OID bytes, so nothing else fits — and it is the binding
 * that mints one. The codec ships the class, which matters twice: it is the
 * only spelling dc_insert_one accepts, and its toString is the 24-hex form
 * src/mdy.js keys its index map by, so a found document and an inserted one
 * agree without either side knowing about the other.
 */

/* An ArrayBuffer holding exactly these bytes. `.buffer` on a Uint8Array is the
 * WHOLE underlying buffer, which is the same thing only when the view starts
 * at 0 and runs to the end — true of what encode returns today, and a silent
 * corruption the day it is not. */
const bufferOf = (u8) =>
  u8.byteOffset === 0 && u8.byteLength === u8.buffer.byteLength
    ? u8.buffer
    : u8.slice().buffer;

/* Ignored: the argument to connect() names a storage back end for the WASM
 * build, where "memory" means an emscripten FS handle. Natively, nis_open
 * makes an unlinked temp file — the same lifetime (it dies with the process)
 * without holding the whole B+tree in the heap. */
export class MemoryStorageProvider {}

class Collection {
  constructor(handle) { this.handle = handle; }

  async insertOne(doc) {
    const withId = '_id' in doc ? doc : { _id: new ObjectId(), ...doc };
    const bytes = encode(withId);
    const rc = globalThis.__nis_insert(this.handle, bufferOf(bytes));
    if (rc !== 0) throw new Error(`nisaba: insert refused (rc=${rc}, ${bytes.length} bytes)`);
    return { insertedId: String(withId._id) };
  }

  find(query) {
    const handle = this.handle;
    return {
      async toArray() {
        const bytes = globalThis.__nis_find(handle, bufferOf(encode(query ?? {})));
        return bytes ? decode(new Uint8Array(bytes)) : [];
      },
    };
  }

  /* The sparse path index is an optimisation, not a semantic: without it a
   * query is a scan and the answers are the same. Honest no-op for now. */
  async createIndex() { return 'noop'; }
}

export async function connect() {
  return {
    async collection() {
      const handle = globalThis.__nis_open();
      if (handle < 0) throw new Error('nisaba: could not open a collection');
      return new Collection(handle);
    },
  };
}

export { ObjectId };
