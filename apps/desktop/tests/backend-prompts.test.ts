import assert from 'node:assert/strict'
import childProcess, { type ChildProcessWithoutNullStreams } from 'node:child_process'
import { EventEmitter } from 'node:events'
import { registerHooks, syncBuiltinESMExports } from 'node:module'
import { test, type TestContext } from 'node:test'
import { IPC_CHANNELS } from '../src/shared/ipc-contract.ts'
import type { JsonObject } from '../src/shared/types.ts'

// Node tests exercise main's prompt lifecycle without starting Electron or a GUI.
const electronHook = registerHooks({
  resolve(specifier, context, nextResolve) {
    if (specifier === 'electron') {
      return { url: 'data:text/javascript,export const dialog = {}', shortCircuit: true }
    }
    return nextResolve(specifier, context)
  }
})
const { AvaBackend } = await import('../src/main/backend.ts')
electronHook.deregister()

async function harness(t: TestContext) {
  const writes: JsonObject[] = []
  const stdout = new EventEmitter()
  const child = Object.assign(new EventEmitter(), {
    stdout, stderr: new EventEmitter(), exitCode: null as number | null, signalCode: null as NodeJS.Signals | null
  })
  function respond(request: JsonObject, success = true, error?: JsonObject) {
    const result = request.type === 'get_protocol'
      ? { protocol_version: 1, event_schema_version: 1, session_entry_version: 4 }
      : request.type === 'get_state' ? { workspace_dir: process.cwd(), session_id: 's' } : {}
    stdout.emit('data', Buffer.from(`${JSON.stringify({
      id: request.id, type: 'response', success, result,
      error: error ?? { category: 'validation', code: 'invalid_request', message: 'invalid answer', details: '' }
    })}\n`))
  }
  let writeFailure = false
  const stdin = Object.assign(new EventEmitter(), {
    writable: true,
    write(bytes: Uint8Array) {
      if (writeFailure) {
        throw new Error('stdin write failed')
      }
      const request = JSON.parse(Buffer.from(bytes).toString()) as JsonObject
      writes.push(request)
      if (!['prompt', 'permission_reply', 'question_reply', 'cancel'].includes(String(request.type))) {
        respond(request)
      }
    },
    end() { queueMicrotask(() => { child.exitCode = 0; child.emit('exit', 0, null) }) }
  })
  const fake = Object.assign(child, { stdin, kill() { child.emit('exit', null, 'SIGTERM') } })
  t.mock.method(childProcess, 'spawn', () => {
    child.exitCode = null
    child.signalCode = null
    return fake as unknown as ChildProcessWithoutNullStreams
  })
  syncBuiltinESMExports()
  const backend = new AvaBackend({ packageRoot: process.cwd(), cwd: process.cwd(), env: {
    AVA_DESKTOP_BINARY: process.execPath, AVA_DESKTOP_WORKSPACE: process.cwd()
  } })
  t.after(async () => {
    await backend.stop()
    t.mock.restoreAll()
    syncBuiltinESMExports()
  })
  await backend.start()
  assert.equal(backend.snapshot().connection, 'ready')
  function event(kind: string, resolver: string, payload: JsonObject = {}) {
    stdout.emit('data', Buffer.from(`${JSON.stringify({
      name: `${kind}_requested`, request_id: 'prompt-host', correlation_id: 'backend-correlation',
      payload: { resolver_request_id: resolver, question: 'Choose', allow_custom: true, ...payload }
    })}\n`))
  }
  return { backend, writes, respond, event, stdout, child, failWrite: () => { writeFailure = true } }
}

for (const kind of ['permission', 'question'] as const) {
  const channel = kind === 'permission' ? IPC_CHANNELS.replyPermission : IPC_CHANNELS.replyQuestion
  const answer = kind === 'permission' ? { decision: 'allow' } : { answer: 'custom' }
  const pending = (backend: InstanceType<typeof AvaBackend>) => kind === 'permission'
    ? backend.snapshot().pendingPermission : backend.snapshot().pendingQuestion

  test(`${kind}: deferred A, rapid repeat, and stale A never answer unseen B`, async (t) => {
    const h = await harness(t)
    h.event(kind, 'resolver-A')
    h.event(kind, 'resolver-B')
    const a = pending(h.backend)!.displayToken
    const first = h.backend.handle(channel, { ...answer, displayToken: a })
    const replyA = h.writes.at(-1)!
    assert.equal(replyA.request_id, 'resolver-A')
    assert.equal(replyA.correlation_id, 'backend-correlation')
    assert.equal(replyA.displayToken, undefined, 'display token is IPC-only, not RPC correlation')
    assert.equal(pending(h.backend)!.displayToken, a)
    const count = h.writes.length
    assert.equal((await h.backend.handle(channel, { ...answer, displayToken: a }) as { ok: boolean }).ok, false)
    assert.equal(h.writes.length, count)
    h.respond(replyA)
    assert.deepEqual(await first, { ok: true })
    const b = pending(h.backend)!.displayToken
    assert.notEqual(b, a)
    assert.equal((await h.backend.handle(channel, { ...answer, displayToken: a }) as { ok: boolean }).ok, false)
    assert.equal(h.writes.length, count)
    const second = h.backend.handle(channel, { ...answer, displayToken: b })
    assert.equal(h.writes.at(-1)!.request_id, 'resolver-B')
    h.respond(h.writes.at(-1)!)
    await second
    assert.equal(pending(h.backend), null)
  })

  test(`${kind}: local invalid input keeps the live display and sends nothing`, async (t) => {
    const h = await harness(t)
    h.event(kind, 'resolver-A')
    const token = pending(h.backend)!.displayToken
    const before = h.writes.length
    const invalid = kind === 'permission'
      ? { displayToken: token, decision: 'maybe' }
      : { displayToken: token, selected: 'not-an-option' }
    assert.equal((await h.backend.handle(channel, invalid) as { ok: boolean }).ok, false)
    assert.equal(h.writes.length, before, 'locally invalid input never reaches the backend')
    assert.equal(pending(h.backend)!.displayToken, token, 'the same display stays open for correction')
    assert.equal(h.backend.snapshot().lastActionError, null)
    const retry = h.backend.handle(channel, { ...answer, displayToken: token })
    assert.equal(h.writes.at(-1)!.request_id, 'resolver-A')
    h.respond(h.writes.at(-1)!)
    assert.deepEqual(await retry, { ok: true })
    assert.equal(pending(h.backend), null)
  })

  // The contract returns the same invalid_request code for bad user input and for
  // missing/correlated resolvers, so a remote rejection is never classified as retryable.
  const remoteErrors: { label: string; error: JsonObject }[] = [
    { label: 'invalid_request (no matching resolver)', error: { category: 'validation', code: 'invalid_request', message: 'RPC resolver reply has no matching pending request', details: '' } },
    { label: 'permission_denied', error: { category: 'permission', code: 'permission_denied', message: 'permission denied', details: '' } },
    { label: 'internal_error', error: { category: 'internal', code: 'internal_error', message: 'backend exploded', details: '' } },
    { label: 'unknown code', error: { category: 'future', code: 'unrecognized_code', message: 'unexpected code', details: '' } },
    { label: 'generic error', error: { category: '', code: '', message: 'generic failure', details: '' } }
  ]
  for (const { label, error: remoteError } of remoteErrors) {
    test(`${kind}: remote ${label} retires the display and fail-closes the same run`, async (t) => {
      const h = await harness(t)
      const run = h.backend.handle(IPC_CHANNELS.prompt, { message: 'test' })
      const prompt = h.writes.at(-1)!
      h.event(kind, 'resolver-A')
      h.event(kind, 'resolver-B')
      const token = pending(h.backend)!.displayToken
      const reply = h.backend.handle(channel, { ...answer, displayToken: token })
      const request = h.writes.at(-1)!
      assert.equal(request.request_id, 'resolver-A')
      h.respond(request, false, remoteError)
      const result = await reply as { ok: boolean; error: { title: string; message: string; hint: string } }
      assert.equal(result.ok, false)
      assert.equal(result.error.message, remoteError.message, 'the original reply error is surfaced')
      assert.equal(result.error.title, kind === 'permission' ? 'Permission reply failed' : 'Question reply failed')
      assert.ok(result.error.hint.length > 0, 'the surfaced error stays actionable')
      assert.equal(pending(h.backend), null, 'the rejected display is retired')
      assert.equal(h.backend.snapshot().busy, true, 'the active run is still current')
      assert.equal(h.backend.snapshot().canceling, true, "the run is failed closed with the backend's cancel API")
      const cancel = h.writes.at(-1)!
      assert.equal(cancel.type, 'cancel')
      assert.ok(cancel.id, 'the cancel is a real same-run RPC')
      assert.equal(h.writes.some((entry) => entry.request_id === 'resolver-B'), false, 'queued B is never answered')
      assert.equal(h.backend.snapshot().lastActionError?.message, remoteError.message)
      h.respond(cancel)
      h.respond(prompt)
      await run
      assert.equal(h.backend.snapshot().busy, false)
      assert.equal(h.backend.snapshot().canceling, false)
      assert.equal(h.backend.snapshot().pendingPermission, null)
      assert.equal(h.backend.snapshot().pendingQuestion, null)
      assert.equal(h.backend.snapshot().lastActionError?.message, remoteError.message, 'the reply error remains surfaced after the run settles')
      assert.equal(h.writes.filter((entry) => entry.type === 'cancel').length, 1, 'one rejection cancels once')
    })
  }

  test(`${kind}: restarting a failed transport cannot revive an old display`, async (t) => {
    const h = await harness(t)
    h.event(kind, 'old-resolver')
    const oldToken = pending(h.backend)!.displayToken
    const oldReply = h.backend.handle(channel, { ...answer, displayToken: oldToken })
    const oldRequest = h.writes.at(-1)!
    h.child.exitCode = 1
    h.child.emit('exit', 1, null)
    const restart = h.backend.handle(IPC_CHANNELS.restart, null)
    await oldReply
    await restart
    assert.equal(h.backend.snapshot().connection, 'ready')
    h.event(kind, 'fresh-resolver')
    const freshToken = pending(h.backend)!.displayToken
    h.respond(oldRequest, false)
    assert.equal(pending(h.backend)!.displayToken, freshToken)
    assert.equal(h.backend.snapshot().lastActionError, null)
    assert.equal(h.writes.filter((entry) => entry.type === 'cancel').length, 0, 'a stale transport error cannot cancel the new backend')
    assert.equal((await h.backend.handle(channel, { ...answer, displayToken: oldToken }) as { ok: boolean }).ok, false)
    assert.equal(pending(h.backend)!.displayToken, freshToken)
  })

  test(`${kind}: a late remote error after cancel cannot cancel the next run`, async (t) => {
    const h = await harness(t)
    const run = h.backend.handle(IPC_CHANNELS.prompt, { message: 'first' })
    const prompt = h.writes.at(-1)!
    h.event(kind, 'resolver-A')
    const a = pending(h.backend)!.displayToken
    const reply = h.backend.handle(channel, { ...answer, displayToken: a })
    const request = h.writes.at(-1)!
    const cancel = h.backend.handle(IPC_CHANNELS.cancel, null)
    h.respond(h.writes.at(-1)!)
    await cancel
    h.respond(prompt, false)
    await run

    const freshRun = h.backend.handle(IPC_CHANNELS.prompt, { message: 'second' })
    const freshPrompt = h.writes.at(-1)!
    h.event(kind, 'fresh-resolver')
    const fresh = pending(h.backend)!.displayToken
    const cancelsBefore = h.writes.filter((entry) => entry.type === 'cancel').length
    h.respond(request, false, { category: 'validation', code: 'invalid_request', message: 'no matching pending request', details: '' })
    assert.equal((await reply as { ok: boolean }).ok, false)
    assert.equal(pending(h.backend)!.displayToken, fresh, 'a stale rejection cannot retire the fresh display')
    assert.equal(h.writes.filter((entry) => entry.type === 'cancel').length, cancelsBefore, 'a stale rejection cannot cancel the fresh run')
    assert.equal(h.backend.snapshot().lastActionError, null)
    assert.equal(h.backend.snapshot().busy, true)
    h.respond(freshPrompt)
    await freshRun
    assert.equal(pending(h.backend), null)
  })

  for (const cleanup of ['exit', 'EOF', 'write failure', 'cancel', 'stop', 'run completion', 'run completion (late success)'] as const) {
    test(`${kind}: ${cleanup} invalidates deferred replies without resurrection`, async (t) => {
      const h = await harness(t)
      const run = h.backend.handle(IPC_CHANNELS.prompt, { message: 'test' })
      const prompt = h.writes.at(-1)!
      h.event(kind, 'resolver-A')
      const a = pending(h.backend)!.displayToken
      const reply = h.backend.handle(channel, { ...answer, displayToken: a })
      const request = h.writes.at(-1)!
      if (cleanup === 'exit') {
        h.child.exitCode = 1
        h.child.emit('exit', 1, null)
      } else if (cleanup === 'EOF') {
        h.stdout.emit('end')
      } else if (cleanup === 'write failure') {
        h.failWrite()
        await h.backend.handle(IPC_CHANNELS.cancel, null)
      } else if (cleanup === 'cancel') {
        const cancel = h.backend.handle(IPC_CHANNELS.cancel, null)
        assert.equal(pending(h.backend), null, 'clear happens before cancel response')
        h.respond(request, false)
        h.respond(h.writes.at(-1)!)
        await cancel
        h.respond(prompt, false)
      } else if (cleanup === 'stop') {
        await h.backend.stop()
      } else {
        h.respond(prompt)
        await run
        assert.equal(pending(h.backend), null)
        const freshRun = h.backend.handle(IPC_CHANNELS.prompt, { message: 'fresh run' })
        const freshPrompt = h.writes.at(-1)!
        h.event(kind, 'fresh-resolver')
        const fresh = pending(h.backend)!.displayToken
        h.respond(request, cleanup === 'run completion (late success)')
        await reply
        assert.equal(pending(h.backend)!.displayToken, fresh, 'late old reply cannot touch fresh run')
        assert.equal(h.backend.snapshot().busy, true)
        assert.equal(h.backend.snapshot().lastActionError, null)
        assert.equal(h.writes.filter((entry) => entry.type === 'cancel').length, 0, 'a late old reply cannot cancel the fresh run')
        h.respond(freshPrompt)
        await freshRun
        return
      }
      await reply
      await run
      assert.equal(h.backend.snapshot().pendingPermission, null)
      assert.equal(h.backend.snapshot().pendingQuestion, null)
    })
  }
}

test('permission: wrong decision, correlation, and token make no RPC and keep the display', async (t) => {
  const h = await harness(t)
  h.event('permission', 'resolver-A')
  const token = h.backend.snapshot().pendingPermission!.displayToken
  const before = h.writes.length
  const rejected = [
    { displayToken: token, decision: 'maybe' },
    { displayToken: token, decision: 'allow', correlation_id: 'backend-correlation' },
    { displayToken: token, decision: 'allow', request_id: 'resolver-A' },
    { displayToken: 'stale-token', decision: 'allow' }
  ]
  for (const payload of rejected) {
    assert.equal((await h.backend.handle(IPC_CHANNELS.replyPermission, payload) as { ok: boolean }).ok, false)
    assert.equal(h.writes.length, before, 'rejected input never reaches the backend')
    assert.equal(h.backend.snapshot().pendingPermission!.displayToken, token)
    assert.equal(h.backend.snapshot().lastActionError, null)
  }
  const retry = h.backend.handle(IPC_CHANNELS.replyPermission, { displayToken: token, decision: 'deny' })
  assert.equal(h.writes.at(-1)!.request_id, 'resolver-A')
  h.respond(h.writes.at(-1)!)
  assert.deepEqual(await retry, { ok: true })
  assert.equal(h.backend.snapshot().pendingPermission, null)
})

test('question: semantic validation against the bound view blocks RPC on invalid input', async (t) => {
  const h = await harness(t)
  h.event('question', 'resolver-A', {
    options: [{ value: 'a', label: 'A' }],
    multiple: false,
    allow_custom: false,
    secret: true
  })
  const token = h.backend.snapshot().pendingQuestion!.displayToken
  const before = h.writes.length
  const rejected = [
    { displayToken: token, answer: 'secret is not an answer' },
    { displayToken: token, selected: 'missing' },
    { displayToken: token, selectedOptions: [] },
    { displayToken: token, selected: 'a', selectedOptions: ['a'] },
    { displayToken: token, answer: 'x', selected: 'a' }
  ]
  for (const payload of rejected) {
    assert.equal((await h.backend.handle(IPC_CHANNELS.replyQuestion, payload) as { ok: boolean }).ok, false)
    assert.equal(h.writes.length, before, 'rejected input never reaches the backend')
    assert.equal(h.backend.snapshot().pendingQuestion!.displayToken, token)
    assert.equal(h.backend.snapshot().lastActionError, null)
  }
  const retry = h.backend.handle(IPC_CHANNELS.replyQuestion, { displayToken: token, selected: 'a' })
  assert.equal(h.writes.at(-1)!.request_id, 'resolver-A')
  h.respond(h.writes.at(-1)!)
  assert.deepEqual(await retry, { ok: true })
  assert.equal(h.backend.snapshot().pendingQuestion, null)
})

test('question: backend-parity edge cases stay sendable', async (t) => {
  const h = await harness(t)
  h.event('question', 'resolver-A', { options: [{ value: 'a', label: 'A' }], multiple: true, allow_custom: true })
  const token = h.backend.snapshot().pendingQuestion!.displayToken
  const emptyMulti = h.backend.handle(IPC_CHANNELS.replyQuestion, { displayToken: token, selectedOptions: [] })
  const emptyMultiRequest = h.writes.at(-1)!
  assert.deepEqual(emptyMultiRequest.selected_options, [], 'an empty multi-select is valid per the RPC contract')
  h.respond(emptyMultiRequest)
  assert.deepEqual(await emptyMulti, { ok: true })

  h.event('question', 'resolver-B', { multiple: false, allow_custom: true })
  const tokenB = h.backend.snapshot().pendingQuestion!.displayToken
  const emptyAnswer = h.backend.handle(IPC_CHANNELS.replyQuestion, { displayToken: tokenB, answer: '' })
  const emptyAnswerRequest = h.writes.at(-1)!
  assert.equal(emptyAnswerRequest.answer, '', 'an empty custom answer is valid when custom answers are allowed')
  h.respond(emptyAnswerRequest)
  assert.deepEqual(await emptyAnswer, { ok: true })
})
