/*
 * `node:assert/strict`, the eleven methods mdy-docs' suite uses.
 *
 * Only what is used, deliberately — an assertion this does not implement
 * should be a missing method rather than something that silently passes. The
 * messages carry actual and expected because a native test failure is read
 * from CI output, with no debugger behind it.
 */

class AssertionError extends Error {
  constructor(message, actual, expected, operator) {
    super(message);
    this.name = 'AssertionError';
    this.actual = actual;
    this.expected = expected;
    this.operator = operator;
  }
}

/** Compact enough to read in a CI log, complete enough to diagnose from. */
function show(value, depth = 0) {
  if (value === null) return 'null';
  if (value === undefined) return 'undefined';
  const type = typeof value;
  if (type === 'string') return depth === 0 && value.length > 300
    ? JSON.stringify(value.slice(0, 300)) + `… (${value.length} chars)`
    : JSON.stringify(value);
  if (type !== 'object') return String(value);
  if (value instanceof RegExp) return String(value);
  if (value instanceof Date) return value.toISOString();
  if (value instanceof Error) return `${value.name}: ${value.message}`;
  try {
    const json = JSON.stringify(value);
    return json.length > 400 ? json.slice(0, 400) + '…' : json;
  } catch {
    return Object.prototype.toString.call(value);
  }
}

/** Structural equality, the shape `assert.deepEqual` promises. */
function deepEqual(a, b) {
  if (Object.is(a, b)) return true;
  if (a === null || b === null || typeof a !== 'object' || typeof b !== 'object') {
    // deepEqual is the strict variant here (node:assert/strict), so no coercion.
    return false;
  }
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  if (a instanceof Date || b instanceof Date) {
    return a instanceof Date && b instanceof Date && a.getTime() === b.getTime();
  }
  if (a instanceof RegExp || b instanceof RegExp) return String(a) === String(b);
  if (a instanceof Map || b instanceof Map) {
    if (!(a instanceof Map) || !(b instanceof Map) || a.size !== b.size) return false;
    for (const [k, v] of a) { if (!b.has(k) || !deepEqual(v, b.get(k))) return false; }
    return true;
  }
  if (a instanceof Set || b instanceof Set) {
    if (!(a instanceof Set) || !(b instanceof Set) || a.size !== b.size) return false;
    for (const v of a) if (!b.has(v)) return false;
    return true;
  }
  // A typed array and a plain array are not equal, matching node:assert/strict.
  if (ArrayBuffer.isView(a) !== ArrayBuffer.isView(b)) return false;
  const ka = Object.keys(a);
  const kb = Object.keys(b);
  if (ka.length !== kb.length) return false;
  for (const k of ka) {
    if (!Object.prototype.hasOwnProperty.call(b, k)) return false;
    if (!deepEqual(a[k], b[k])) return false;
  }
  return true;
}

function fail(message, fallback, actual, expected, operator) {
  throw new AssertionError(message ?? fallback, actual, expected, operator);
}

export function ok(value, message) {
  if (!value) fail(message, `expected a truthy value, got ${show(value)}`, value, true, '==');
}

export function equal(actual, expected, message) {
  if (!Object.is(actual, expected) && actual !== expected) {
    fail(message, `expected ${show(expected)}\n    actual   ${show(actual)}`, actual, expected, 'strictEqual');
  }
}
export const strictEqual = equal;

export function notEqual(actual, expected, message) {
  if (Object.is(actual, expected) || actual === expected) {
    fail(message, `expected NOT ${show(expected)}`, actual, expected, 'notStrictEqual');
  }
}
export const notStrictEqual = notEqual;

export function deepStrictEqual(actual, expected, message) {
  if (!deepEqual(actual, expected)) {
    fail(message, `expected ${show(expected)}\n    actual   ${show(actual)}`, actual, expected, 'deepStrictEqual');
  }
}
export { deepStrictEqual as deepEqual };

export function notDeepStrictEqual(actual, expected, message) {
  if (deepEqual(actual, expected)) {
    fail(message, `expected NOT ${show(expected)}`, actual, expected, 'notDeepStrictEqual');
  }
}
export { notDeepStrictEqual as notDeepEqual };

export function match(value, pattern, message) {
  if (!pattern.test(value)) {
    fail(message, `expected ${show(value)}\n    to match ${pattern}`, value, pattern, 'match');
  }
}

export function doesNotMatch(value, pattern, message) {
  if (pattern.test(value)) {
    fail(message, `expected ${show(value)}\n    NOT to match ${pattern}`, value, pattern, 'doesNotMatch');
  }
}

/** True when a thrown error satisfies node's `expected` argument: a regexp on
 * the message, a predicate, or an object of properties to match. */
function matchesExpectation(err, expected) {
  if (expected === undefined) return true;
  if (expected instanceof RegExp) return expected.test(String(err?.message ?? err));
  if (typeof expected === 'function') {
    // A class, or a predicate. `Error.prototype` on the prototype chain tells
    // them apart the same way node does.
    if (expected.prototype instanceof Error || expected === Error) return err instanceof expected;
    return expected(err) !== false;
  }
  if (expected && typeof expected === 'object') {
    return Object.entries(expected).every(([k, v]) =>
      v instanceof RegExp ? v.test(String(err?.[k])) : deepEqual(err?.[k], v));
  }
  return true;
}

export function throws(fn, expected, message) {
  let thrown;
  try { fn(); } catch (err) { thrown = err ?? new Error('threw a falsy value'); }
  if (!thrown) fail(message, 'expected the function to throw, and it did not', undefined, expected, 'throws');
  if (!matchesExpectation(thrown, expected)) {
    fail(message, `threw the wrong error: ${show(thrown)}`, thrown, expected, 'throws');
  }
}

export async function rejects(promiseOrFn, expected, message) {
  let thrown;
  try {
    await (typeof promiseOrFn === 'function' ? promiseOrFn() : promiseOrFn);
  } catch (err) {
    thrown = err ?? new Error('rejected with a falsy value');
  }
  if (!thrown) fail(message, 'expected a rejection, and it resolved', undefined, expected, 'rejects');
  if (!matchesExpectation(thrown, expected)) {
    fail(message, `rejected with the wrong error: ${show(thrown)}`, thrown, expected, 'rejects');
  }
}

export function doesNotThrow(fn, message) {
  try { fn(); } catch (err) { fail(message, `expected no throw, got ${show(err)}`, err, undefined, 'doesNotThrow'); }
}

const assert = ok;
Object.assign(assert, {
  ok, equal, strictEqual, notEqual, notStrictEqual,
  deepEqual: deepStrictEqual, deepStrictEqual,
  notDeepEqual: notDeepStrictEqual, notDeepStrictEqual,
  match, doesNotMatch, throws, rejects, doesNotThrow,
  AssertionError,
  fail: (m) => { throw new AssertionError(m ?? 'failed', undefined, undefined, 'fail'); },
});
export { AssertionError };
export default assert;
