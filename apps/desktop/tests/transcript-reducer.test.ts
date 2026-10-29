import assert from 'node:assert/strict'
import { test } from 'node:test'
import type { RpcEvent } from '../src/shared/rpc-session.ts'
import {
  applyPromptFinalText,
  applyRpcEvent,
  applyRunError,
  createTranscriptState,
  loadHistoryMessages
} from '../src/shared/transcript-reducer.ts'

function event(name: string, payload: Record<string, unknown>, eventId = name): RpcEvent {
  return {
    name,
    type: name,
    eventId,
    schemaVersion: 1,
    requestId: 'prompt-1',
    correlationId: 'prompt-1',
    payload,
    raw: {}
  }
}

test('message_update text is a delta and message_end does not complete the run', () => {
  let state = createTranscriptState()
  state = applyRpcEvent(state, event('user_message', { text: 'hi' }, 'u1'))
  state = applyRpcEvent(state, event('message_update', { text: 'Hel' }, 'd1'))
  state = applyRpcEvent(state, event('message_update', { text: 'lo' }, 'd2'))
  const streaming = state.items.find((item) => item.kind === 'assistant')
  assert.equal(streaming?.kind, 'assistant')
  if (streaming?.kind === 'assistant') {
    assert.equal(streaming.text, 'Hello')
    assert.equal(streaming.streaming, true)
  }
  state = applyRpcEvent(state, event('message_end', { text: '' }, 'end1'))
  const frozen = state.items.find((item) => item.kind === 'assistant')
  assert.equal(frozen?.kind, 'assistant')
  if (frozen?.kind === 'assistant') {
    assert.equal(frozen.streaming, false)
    assert.equal(frozen.text, 'Hello')
  }
})

test('preserves intermediate assistant turns around tools and dedups canonical text', () => {
  let state = createTranscriptState()
  state = applyRpcEvent(state, event('message_update', { text: 'first' }, 'a1'))
  state = applyRpcEvent(state, event('message_end', {}, 'e1'))
  state = applyRpcEvent(state, event('tool_start', { call_id: 'c1', tool: 'write_file', text: 'a.txt', status: 'running' }, 't1'))
  state = applyRpcEvent(state, event('tool_result', { call_id: 'c1', tool: 'write_file', text: 'wrote file', status: 'success' }, 't2'))
  state = applyRpcEvent(state, event('message_update', { text: 'second' }, 'a2'))
  state = applyRpcEvent(state, event('message_end', {}, 'e2'))
  state = applyRpcEvent(state, event('assistant_message', { text: 'second' }, 'final'))
  state = applyPromptFinalText(state, 'second')
  const kinds = state.items.map((item) => item.kind)
  assert.deepEqual(kinds, ['assistant', 'tool', 'assistant'])
  const assistants = state.items.filter((item) => item.kind === 'assistant')
  assert.equal(assistants.length, 2)
  assert.equal(assistants[0] && assistants[0].kind === 'assistant' ? assistants[0].text : '', 'first')
  assert.equal(assistants[1] && assistants[1].kind === 'assistant' ? assistants[1].text : '', 'second')
})

test('error and cancel events do not replace earlier turns', () => {
  let state = createTranscriptState()
  state = applyRpcEvent(state, event('user_message', { text: 'go' }, 'u'))
  state = applyRpcEvent(state, event('message_update', { text: 'partial' }, 'd'))
  state = applyRpcEvent(state, event('error', { message: 'provider_error' }, 'err'))
  assert.equal(state.items.some((item) => item.kind === 'user'), true)
  assert.equal(state.items.some((item) => item.kind === 'assistant'), true)
  assert.equal(state.items.some((item) => item.kind === 'error'), true)
  state = applyRpcEvent(state, event('canceled', { text: 'stopped by user' }, 'c'))
  assert.equal(state.items.at(-1)?.kind, 'notice')
})

test('a host-side run error freezes the draft and leaves the previous state untouched', () => {
  const streaming = applyRpcEvent(createTranscriptState(), event('message_update', { text: 'partial' }, 'd'))
  const failed = applyRunError(streaming, 'prompt rejected')
  assert.deepEqual(failed.items.map((item) => item.kind), ['assistant', 'error'])
  assert.equal(failed.items[0]?.kind === 'assistant' && failed.items[0].streaming, false)
  assert.equal(failed.draftAssistantId, null)
  assert.equal(streaming.items.length, 1, 'reducers never mutate their input state')
  assert.equal(streaming.items[0]?.kind === 'assistant' && streaming.items[0].streaming, true)
})

test('tool summaries are truncated without splitting a surrogate pair', () => {
  const text = `${'a'.repeat(158)}😀${'b'.repeat(10)}`
  const state = applyRpcEvent(createTranscriptState(), event('tool_start', { call_id: 'c', tool: 'read', text }, 't'))
  const tool = state.items[0]
  assert.equal(tool?.kind === 'tool' ? tool.summary : '', `${'a'.repeat(158)}…`)
})

test('session history loads user and assistant entries without paths', () => {
  const state = loadHistoryMessages(createTranscriptState(), [
    { id: '1', type: 'user_message', data: { text: 'hello' } },
    { id: '2', type: 'assistant_message', data: { text: 'world' } }
  ])
  assert.deepEqual(
    state.items.map((item) => [item.kind, item.kind === 'user' || item.kind === 'assistant' ? item.text : '']),
    [
      ['user', 'hello'],
      ['assistant', 'world']
    ]
  )
})
