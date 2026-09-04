/*
 * `node:zlib`, enough for test/png-fixture.js to build a real PNG.
 *
 * deflateSync here emits STORED blocks — deflate's uncompressed encoding. That
 * is a complete, valid deflate stream that any decoder accepts; it just does
 * not compress. The fixture exists so tests have a decodable image to read
 * dimensions from, and nothing measures its size, so a compressor would be
 * several hundred lines spent on a property nothing checks.
 *
 * Wrapped in a zlib container (RFC 1950): the two-byte header, then the
 * blocks, then an Adler-32 of the uncompressed data.
 */

function adler32(bytes) {
  let a = 1;
  let b = 0;
  for (let i = 0; i < bytes.length; i++) {
    a = (a + bytes[i]) % 65521;
    b = (b + a) % 65521;
  }
  return ((b << 16) | a) >>> 0;
}

export function deflateSync(input) {
  const data = input instanceof Uint8Array ? input : new Uint8Array(input);
  const MAX = 65535; // a stored block's length field is 16 bits
  const blocks = Math.max(1, Math.ceil(data.length / MAX));
  const out = new Uint8Array(2 + blocks * 5 + data.length + 4);
  let o = 0;

  out[o++] = 0x78; // CMF: deflate, 32K window
  out[o++] = 0x01; // FLG: no dictionary, fastest — makes (CMF<<8|FLG) % 31 === 0

  for (let i = 0; i < blocks; i++) {
    const start = i * MAX;
    const len = Math.min(MAX, data.length - start);
    out[o++] = i === blocks - 1 ? 1 : 0;     // BFINAL, BTYPE=00 (stored)
    out[o++] = len & 0xff;
    out[o++] = (len >> 8) & 0xff;
    out[o++] = ~len & 0xff;                  // one's complement, as the format wants
    out[o++] = (~len >> 8) & 0xff;
    out.set(data.subarray(start, start + len), o);
    o += len;
  }

  const sum = adler32(data);
  out[o++] = (sum >>> 24) & 0xff;
  out[o++] = (sum >>> 16) & 0xff;
  out[o++] = (sum >>> 8) & 0xff;
  out[o++] = sum & 0xff;
  return out.subarray(0, o);
}

export default { deflateSync };
