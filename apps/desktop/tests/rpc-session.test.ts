import assert from 'node:assert/strict'
import { test } from 'node:test'
import { RpcError, RpcSession, type RpcEvent } from '../src/shared/rpc-session.ts'

const encoder = new TextEncoder()

function sessionHarness() {
  const writes: string[] = []
  const events: RpcEvent[] = []
  const session = new RpcSession({
    write: (bytes) => {
      writes.push(new TextDecoder().decode(bytes))
    },
    onEvent: (event) => {
      events.push(event)
    },
    generateId: (prefix) => `${prefix}-1`
  })
  return { session, writes, events }
}

test('correlates out-of-order responses by id and ignores unknown events', async () => {
  const writes: string[] = []
  let seq = 0
  const events: RpcEvent[] = []
  const session = new RpcSession({
    write: (bytes) => {
      writes.push(new TextDecoder().decode(bytes))
    },
    onEvent: (event) => {
      events.push(event)
    },
    generateId: (prefix) => {
      seq += 1
      return `${prefix}-${seq}`
    }
  })
  const first = session.request('get_state')
  const second = session.request('list_models')
  session.handleStdout(
    encoder.encode(
      `${JSON.stringify({ schema_version: 1, event_id: 'e1', name: 'future_event', type: 'future_event', payload: { x: 1 } })}\n`
    )
  )
  session.handleStdout(
    encoder.encode(`${JSON.stringify({ id: 'req-2', type: 'response', success: true, result: { models: [] } })}\n`)
  )
  session.handleStdout(
    encoder.encode(`${JSON.stringify({ id: 'req-1', type: 'response', success: true, result: { session_id: 's' } })}\n`)
  )
  assert.deepEqual(await second, { models: [] })
  assert.deepEqual(await first, { session_id: 's' })
  assert.equal(events.length, 1)
  assert.equal(events[0]?.name, 'future_event')
  assert.equal(session.pendingSize, 0)
})

test('malformed JSON fails the session and rejects pending work', async () => {
  const { session } = sessionHarness()
  const pending = session.request('get_protocol')
  session.handleStdout(encoder.encode('not-json\n'))
  await assert.rejects(pending, /not valid JSON/)
  assert.equal(session.pendingSize, 0)
  await assert.rejects(session.request('get_state'), /not valid JSON/)
})

test('stdout end rejects pending promises', async () => {
  const { session } = sessionHarness()
  const pending = session.request('prompt', { message: 'hi' }, 'prompt')
  session.handleStdoutEnd()
  await assert.rejects(pending, /stdout closed/)
  assert.equal(session.pendingSize, 0)
})

test('write failure rejects callers and pending work', async () => {
  const session = new RpcSession({
    write: () => {
      throw new Error('stdin closed')
    },
    generateId: () => 'req-1'
  })
  await assert.rejects(session.request('cancel'), /stdin closed/)
  await assert.rejects(session.request('get_state'), /stdin closed/)
})

test('unsuccessful RPC responses become RpcError', async () => {
  const { session } = sessionHarness()
  const pending = session.request('prompt', { message: 'hi' })
  session.handleStdout(
    encoder.encode(
      `${JSON.stringify({
        id: 'req-1',
        type: 'response',
        success: false,
        error: { category: 'permission_denied', code: 'permission_denied', message: 'offline mode is enabled', details: 'hint' }
      })}\n`
    )
  )
  await assert.rejects(pending, (error: unknown) => error instanceof RpcError && error.code === 'permission_denied')
})
