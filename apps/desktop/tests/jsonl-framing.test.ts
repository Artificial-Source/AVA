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

test('a terminated record over the frame limit is fatal', () => {
  const framer = new JsonlFramer(8)
  assert.deepEqual(framer.push(encoder.encode('abcde')), [])
  assert.throws(
    () => framer.push(encoder.encode('fghij\n')),
    (error: unknown) => error instanceof FramingError && error.code === 'max_frame'
  )
})

test('a reused chunk buffer cannot corrupt a buffered partial record', () => {
  const framer = new JsonlFramer()
  const chunk = encoder.encode('{"id":')
  assert.deepEqual(framer.push(chunk), [])
  chunk.fill(0x20)
  assert.deepEqual(framer.push(encoder.encode('"1"}\n')), ['{"id":"1"}'])
})

test('a record delivered one byte at a time, splitting UTF-8 sequences, is reassembled', () => {
  const framer = new JsonlFramer()
  const record = '{"text":"héllo €"}'
  const records: string[] = []
  for (const byte of encoder.encode(`${record}\n`)) {
    records.push(...framer.push(Uint8Array.of(byte)))
  }
  assert.deepEqual(records, [record])
})
