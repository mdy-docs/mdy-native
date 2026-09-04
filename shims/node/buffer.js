/*
 * `Buffer`, the handful of it the PNG fixture and the provider tests use.
 *
 * A Uint8Array subclass, which is what Buffer is in node — so anything that
 * only needs bytes (writeBinary, a comparison, a decode) works without knowing
 * the difference.
 */
export class Buffer extends Uint8Array {
  static alloc(size, fill = 0) {
    const b = new Buffer(size);
    if (fill) b.fill(fill);
    return b;
  }

  static from(value, encoding) {
    if (typeof value === 'string') {
      // 'ascii'/'latin1' are byte-per-char; anything else is UTF-8, which is
      // node's default and the only one this suite passes.
      if (encoding === 'ascii' || encoding === 'latin1' || encoding === 'binary') {
        const b = new Buffer(value.length);
        for (let i = 0; i < value.length; i++) b[i] = value.charCodeAt(i) & 0xff;
        return b;
      }
      return new Buffer(new TextEncoder().encode(value));
    }
    if (value instanceof ArrayBuffer) return new Buffer(new Uint8Array(value));
    return new Buffer(Uint8Array.from(value));
  }

  static concat(list, total) {
    const size = total ?? list.reduce((n, b) => n + b.length, 0);
    const out = new Buffer(size);
    let o = 0;
    for (const b of list) {
      if (o >= size) break;
      out.set(b.subarray(0, Math.min(b.length, size - o)), o);
      o += b.length;
    }
    return out;
  }

  static isBuffer(v) { return v instanceof Buffer; }

  writeUInt32BE(value, offset = 0) {
    this[offset] = (value >>> 24) & 0xff;
    this[offset + 1] = (value >>> 16) & 0xff;
    this[offset + 2] = (value >>> 8) & 0xff;
    this[offset + 3] = value & 0xff;
    return offset + 4;
  }

  readUInt32BE(offset = 0) {
    return ((this[offset] << 24) | (this[offset + 1] << 16) |
            (this[offset + 2] << 8) | this[offset + 3]) >>> 0;
  }

  /** node's signature: copy(target, targetStart, sourceStart, sourceEnd). */
  copy(target, targetStart = 0, sourceStart = 0, sourceEnd = this.length) {
    const slice = this.subarray(sourceStart, sourceEnd);
    target.set(slice, targetStart);
    return slice.length;
  }

  toString(encoding, start = 0, end = this.length) {
    const bytes = this.subarray(start, end);
    if (encoding === 'ascii' || encoding === 'latin1' || encoding === 'binary') {
      let s = '';
      for (const b of bytes) s += String.fromCharCode(b);
      return s;
    }
    if (encoding === 'hex') {
      return Array.from(bytes).map((b) => b.toString(16).padStart(2, '0')).join('');
    }
    return new TextDecoder().decode(bytes);
  }
}

export default { Buffer };
