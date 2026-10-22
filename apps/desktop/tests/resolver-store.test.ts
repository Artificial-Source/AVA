import assert from 'node:assert/strict'
import { randomUUID } from 'node:crypto'
import { test } from 'node:test'
import type { RpcEvent } from '../src/shared/rpc-session.ts'
import { ResolverStore } from '../src/shared/resolver-store.ts'

function event(name: string, payload: Record<string, unknown>, correlationId = 'prompt-1'): RpcEvent {
  return {
    name,
    type: name,
    eventId: 'event-1',
    schemaVersion: 1,
    requestId: 'prompt-1',
    correlationId,
    payload,
    raw: {}
  }
}

test('binds resolver ids from events and never auto-approves', () => {
  const store = new ResolverStore(randomUUID)
  store.bindEvent(
    event('permission_requested', {
      resolver_request_id: 'permission_1',
      permission_request_id: 'permreq_1',
      operation: 'edit',
      tool_name: 'edit_file',
      reason: 'approval required',
      risk: 'high'
    })
  )
  assert.equal(store.permissionCount, 1)
  assert.equal(store.pendingPermission?.toolName, 'edit_file')
  const consumed = store.beginPermission(store.pendingPermission?.displayToken ?? '')
  assert.equal(consumed?.resolverRequestId, 'permission_1')
  assert.equal(consumed?.correlationId, 'prompt-1')
  assert.equal(store.permissionCount, 1)
  assert.ok(consumed)
  assert.equal(store.finishPermission(consumed), true)
  assert.equal(store.permissionCount, 0)
})

test('questions bind pending resolvers independently of renderer input', () => {
  const store = new ResolverStore(randomUUID)
  store.bindEvent(
    event('question_requested', {
      resolver_request_id: 'question_1',
      header: 'Choose',
      question: 'Which file?',
      options: [{ value: 'a', label: 'A' }],
      multiple: false,
      allow_custom: true,
      secret: false
    })
  )
  const consumed = store.beginQuestion(store.pendingQuestion?.displayToken ?? '')
  assert.equal(consumed?.resolverRequestId, 'question_1')
  assert.equal(consumed?.view.allowCustom, true)
  store.clear()
  assert.equal(store.beginQuestion(store.pendingQuestion?.displayToken ?? ''), null)
  assert.equal(store.beginPermission(store.pendingPermission?.displayToken ?? ''), null)
})

test('missing resolver ids are not bound', () => {
  const store = new ResolverStore(randomUUID)
  store.bindEvent(event('permission_requested', { operation: 'edit' }, ''))
  assert.equal(store.permissionCount, 0)
})

test('peeking the current question never consumes it', () => {
  const store = new ResolverStore(randomUUID)
  store.bindEvent(
    event('question_requested', {
      resolver_request_id: 'question_1',
      options: [{ value: 'a', label: 'A' }],
      allow_custom: true
    })
  )
  const token = store.pendingQuestion?.displayToken ?? ''
  assert.equal(store.peekQuestion('other-token'), null)
  const peeked = store.peekQuestion(token)
  assert.equal(peeked?.resolverRequestId, 'question_1')
  assert.equal(store.pendingQuestion?.displayToken, token)
  const consumed = store.beginQuestion(token)
  assert.ok(consumed)
  assert.equal(store.peekQuestion(token), consumed, 'in-flight displays are still the current display')
})

for (const kind of ['Permission', 'Question'] as const) {
  test(`${kind}: queued displays reject rapid duplicates and old display decisions`, () => {
    const store = new ResolverStore(randomUUID)
    const begin = kind === 'Permission' ? store.beginPermission.bind(store) : store.beginQuestion.bind(store)
    const view = () => kind === 'Permission' ? store.pendingPermission : store.pendingQuestion
    const finish = (bound: NonNullable<ReturnType<typeof begin>>) => kind === 'Permission'
      ? store.finishPermission(bound as Parameters<typeof store.finishPermission>[0])
      : store.finishQuestion(bound as Parameters<typeof store.finishQuestion>[0])
    store.bindEvent(event(`${kind.toLowerCase()}_requested`, { resolver_request_id: 'A', displayToken: 'model-forged' }))
    const a = view()!.displayToken
    assert.notEqual(a, 'model-forged')
    store.bindEvent(event(`${kind.toLowerCase()}_requested`, { resolver_request_id: 'B' }))
    assert.equal(view()!.displayToken, a)
    const bound = begin(a)!
    assert.equal(bound.resolverRequestId, 'A')
    assert.equal(begin(a), null)
    assert.equal(view()!.displayToken, a, 'A remains displayed until reply succeeds')
    assert.equal(finish(bound), true)
    const b = view()!.displayToken
    assert.notEqual(b, a)
    assert.equal(begin(a), null)
    assert.equal(begin(b)!.resolverRequestId, 'B')
  })

  test(`${kind}: retiring the matched instance cannot resurrect or touch fresh work`, () => {
    const store = new ResolverStore(randomUUID)
    const begin = kind === 'Permission' ? store.beginPermission.bind(store) : store.beginQuestion.bind(store)
    const view = () => kind === 'Permission' ? store.pendingPermission : store.pendingQuestion
    const finish = (bound: NonNullable<ReturnType<typeof begin>>) => kind === 'Permission'
      ? store.finishPermission(bound as Parameters<typeof store.finishPermission>[0])
      : store.finishQuestion(bound as Parameters<typeof store.finishQuestion>[0])
    const retire = (bound: NonNullable<ReturnType<typeof begin>>) => kind === 'Permission'
      ? store.retirePermission(bound as Parameters<typeof store.retirePermission>[0])
      : store.retireQuestion(bound as Parameters<typeof store.retireQuestion>[0])
    store.bindEvent(event(`${kind.toLowerCase()}_requested`, { resolver_request_id: 'A' }))
    store.bindEvent(event(`${kind.toLowerCase()}_requested`, { resolver_request_id: 'B' }))
    const tokenA = view()!.displayToken
    const bound = begin(tokenA)!
    assert.equal(retire(bound), true)
    const tokenB = view()!.displayToken
    assert.notEqual(tokenB, tokenA, 'retiring drops only the matched display')
    assert.equal(retire(bound), false, 'one display is retired at most once')
    assert.equal(finish(bound), false)
    assert.equal(begin(tokenA), null)
    const boundB = begin(tokenB)!

    store.clear()
    store.bindEvent(event(`${kind.toLowerCase()}_requested`, { resolver_request_id: 'fresh' }))
    const fresh = view()!.displayToken
    assert.equal(retire(bound), false, 'a stale outcome cannot touch a fresh display')
    assert.equal(retire(boundB), false, 'clear invalidates replies still awaiting transport')
    assert.equal(view()!.displayToken, fresh)
  })
}
