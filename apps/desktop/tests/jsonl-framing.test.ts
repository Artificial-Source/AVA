import assert from 'node:assert/strict'
import { test } from 'node:test'
import { FramingError, JsonlFramer } from '../src/shared/jsonl-framing.ts'

const encoder = new TextEncoder()

test('splits records only on LF and supports chunked delivery', () => {
  const framer = new JsonlFramer()
  const first = encoder.encode('{"id":"1"}\n{"id":')
  const second = encoder.encode('"2"}\n')
  assert.deepEqual(framer.push(first), ['{"id":"1"}'])
  assert.deepEqual(framer.push(second), ['{"id":"2"}'])
})

test('strips a single CR before LF and accepts a final EOF record', () => {
  const framer = new JsonlFramer()
  assert.deepEqual(framer.push(encoder.encode('{"a":1}\r\n')), ['{"a":1}'])
  framer.push(encoder.encode('{"b":2}'))
  assert.equal(framer.finish(), '{"b":2}')
})

test('empty EOF does not emit a record', () => {
  const framer = new JsonlFramer()
  assert.equal(framer.finish(), undefined)
})

test('does not split on Unicode line separators', () => {
  const framer = new JsonlFramer()
  const record = '{"text":"a\u2028b"}'
  assert.deepEqual(framer.push(encoder.encode(`${record}\n`)), [record])
})

test('invalid UTF-8 in a complete record is fatal', () => {
  const framer = new JsonlFramer()
  assert.throws(
    () => framer.push(Uint8Array.from([0x7b, 0xff, 0x7d, 0x0a])),
    (error: unknown) => error instanceof FramingError && error.code === 'utf8'
  )
})

test('max framing without a newline is fatal', () => {
  const framer = new JsonlFramer(8)
  assert.throws(
    () => framer.push(encoder.encode('abcdefghij')),
    (error: unknown) => error instanceof FramingError && error.code === 'max_frame'
  )
})
