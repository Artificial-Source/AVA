import assert from 'node:assert/strict'
import childProcess, { type ChildProcessWithoutNullStreams } from 'node:child_process'
import { EventEmitter } from 'node:events'
import { mkdtempSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { syncBuiltinESMExports } from 'node:module'
import { test, type TestContext } from 'node:test'
import { AvaBackend, type BackendOptions } from '../src/main/backend.ts'
import { IPC_CHANNELS } from '../src/shared/ipc-contract.ts'
import type { JsonObject } from '../src/shared/types.ts'

// Node tests exercise main's prompt lifecycle without starting Electron or a GUI.
function backendOptions(overrides: Partial<BackendOptions> = {}): BackendOptions {
  return {
    packageRoot: process.cwd(),
    cwd: process.cwd(),
    env: { AVA_DESKTOP_BINARY: process.execPath, AVA_DESKTOP_WORKSPACE: process.cwd() },
    chooseWorkspace: async () => null,
    ...overrides
  }
}

function mockSpawn(t: TestContext, spawned: () => unknown, spawnCwds: string[] = []) {
  t.mock.method(childProcess, 'spawn', (_file: string, _args: string[], options: { cwd: string }) => {
    spawnCwds.push(options.cwd)
    return spawned() as ChildProcessWithoutNullStreams
  })
  syncBuiltinESMExports()
}

// Successful RPC results for the handshake and catalog requests.
function resultFor(request: JsonObject): JsonObject {
  return request.type === 'get_protocol'
    ? { protocol_version: 1, event_schema_version: 1, session_entry_version: 4 }
    : request.type === 'get_state' ? { workspace_dir: process.cwd(), session_id: 's' } : {}
}

type FakeExitState = { exitCode: number | null; signalCode: NodeJS.Signals | null }
// The parts of a lifecycle fake the tests drive; spawn receives the full fake process.
type LifecycleChild = FakeExitState & { releaseExit(): void }

function exited(child: FakeExitState): boolean {
  return child.exitCode !== null || child.signalCode !== null
}

// One real-looking AVA process per spawn that answers every request. With `holdEof`, it exits
// on stdin EOF only once the test calls releaseExit(), so a restart can be caught mid-shutdown.
function lifecycleChild(pid: number, holdEof: boolean): LifecycleChild {
  const stdout = new EventEmitter()
  let releaseExit = () => {}
  const child = Object.assign(new EventEmitter(), {
    pid, stdout, stderr: new EventEmitter(), exitCode: null as number | null, signalCode: null as NodeJS.Signals | null
  })
  const exit = (code: number | null, signal: NodeJS.Signals | null) => {
    if (!exited(child)) {
      child.exitCode = code
      child.signalCode = signal
      child.emit('exit', code, signal)
    }
  }
  const stdin = Object.assign(new EventEmitter(), {
    writable: true,
    write(bytes: Uint8Array) {
      const request = JSON.parse(Buffer.from(bytes).toString()) as JsonObject
      stdout.emit('data', Buffer.from(`${JSON.stringify({ id: request.id, type: 'response', success: true, result: resultFor(request) })}\n`))
    },
    end() {
      this.writable = false
      if (holdEof) {
        releaseExit = () => exit(0, null)
      } else {
        queueMicrotask(() => exit(0, null))
      }
    }
  })
  return Object.assign(child, {
    stdin,
    kill(signal: NodeJS.Signals = 'SIGTERM') {
      exit(null, signal)
      return true
    },
    releaseExit: () => releaseExit()
  })
}

// `pid` makes the fake look like a real process; `stubborn` ignores stdin EOF and SIGTERM.
async function harness(
  t: TestContext,
  options: { pid?: number; stubborn?: boolean; chooseWorkspace?: () => Promise<string | null> } = {}
) {
  const writes: JsonObject[] = []
  const signals: NodeJS.Signals[] = []
  const spawnCwds: string[] = []
  const stdout = new EventEmitter()
  const child = Object.assign(new EventEmitter(), {
    pid: options.pid, stdout, stderr: new EventEmitter(), exitCode: null as number | null, signalCode: null as NodeJS.Signals | null
  })
  function respond(request: JsonObject, success = true, error?: JsonObject) {
    const result = resultFor(request)
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
    end() {
      if (!options.stubborn) {
        queueMicrotask(() => { child.exitCode = 0; child.emit('exit', 0, null) })
      }
    }
  })
  const fake = Object.assign(child, {
    stdin,
    kill(signal: NodeJS.Signals = 'SIGTERM') {
      signals.push(signal)
      if (!options.stubborn || signal === 'SIGKILL') {
        child.signalCode = signal
        child.emit('exit', null, signal)
      }
      return true
    }
  })
  mockSpawn(t, () => {
    child.exitCode = null
    child.signalCode = null
    return fake
  }, spawnCwds)
  const launch = backendOptions(options.chooseWorkspace ? { chooseWorkspace: options.chooseWorkspace } : {})
  const backend = new AvaBackend(launch)
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
  return {
    backend, writes, respond, event, stdout, child, signals, spawnCwds, env: launch.env,
    failWrite: () => { writeFailure = true }
  }
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

test('cancel after the run settled sends nothing and leaves the next run cancelable', async (t) => {
  const h = await harness(t)
  const before = h.writes.length
  const idleCancel = h.backend.handle(IPC_CHANNELS.cancel, null)
  const sent = h.writes.slice(before)
  for (const request of sent) {
    h.respond(request)
  }
  assert.deepEqual(await idleCancel, { ok: true })
  assert.equal(sent.length, 0, 'an idle cancel never reaches the backend')
  assert.equal(h.backend.snapshot().canceling, false, 'an idle cancel does not latch canceling')

  const run = h.backend.handle(IPC_CHANNELS.prompt, { message: 'go' })
  const prompt = h.writes.at(-1)!
  const cancel = h.backend.handle(IPC_CHANNELS.cancel, null)
  const cancelRequest = h.writes.at(-1)!
  assert.equal(cancelRequest.type, 'cancel')
  assert.equal(h.backend.snapshot().canceling, true)
  h.respond(cancelRequest)
  assert.deepEqual(await cancel, { ok: true })
  h.respond(prompt, false)
  await run
  assert.equal(h.backend.snapshot().canceling, false)
})

test('a completed prompt stays successful when the follow-up catalog refresh fails', async (t) => {
  const h = await harness(t)
  const run = h.backend.handle(IPC_CHANNELS.prompt, { message: 'go' })
  const prompt = h.writes.at(-1)!
  h.failWrite()
  h.respond(prompt)
  assert.deepEqual(await run, { ok: true }, 'the answered run is not reported as failed')
  assert.equal(h.backend.snapshot().busy, false)
})

test('an AVA that never answers the handshake reports an error instead of starting forever', async (t) => {
  const silent = Object.assign(new EventEmitter(), {
    stdout: new EventEmitter(),
    stderr: new EventEmitter(),
    stdin: Object.assign(new EventEmitter(), { writable: true, write() {}, end() {} }),
    exitCode: null,
    signalCode: null,
    kill: () => true
  })
  mockSpawn(t, () => silent)
  t.after(() => {
    t.mock.restoreAll()
    syncBuiltinESMExports()
  })
  const backend = new AvaBackend(backendOptions({ handshakeTimeoutMs: 20 }))
  await backend.start()
  assert.equal(backend.snapshot().connection, 'error')
  assert.match(backend.snapshot().connectionError?.message ?? '', /handshake within 20 ms/)
})

test('a chosen workspace restarts AVA there without mutating the launch environment', async (t) => {
  const workspace = mkdtempSync(join(tmpdir(), 'ava-desktop-workspace-'))
  t.after(() => rmSync(workspace, { recursive: true, force: true }))
  const h = await harness(t, { chooseWorkspace: async () => workspace })
  const launchWorkspace = h.env.AVA_DESKTOP_WORKSPACE
  assert.deepEqual(await h.backend.handle(IPC_CHANNELS.selectWorkspace, null), { ok: true })
  assert.equal(h.spawnCwds.at(-1), workspace)
  assert.equal(h.env.AVA_DESKTOP_WORKSPACE, launchWorkspace, 'the chosen workspace is backend state, not an env write')
  assert.equal(h.backend.snapshot().connection, 'ready')
})

test('stop escalates to SIGTERM, then SIGKILL, when AVA ignores stdin EOF', async (t) => {
  const h = await harness(t, { pid: 4242, stubborn: true })
  t.mock.timers.enable({ apis: ['setTimeout'] })
  const settle = () => new Promise((resolve) => setImmediate(resolve))
  const stopping = h.backend.stop()
  assert.equal(h.backend.snapshot().connection, 'stopped')
  assert.deepEqual(h.signals, [], 'EOF gets a grace period first')
  t.mock.timers.tick(1500)
  await settle()
  assert.deepEqual(h.signals, ['SIGTERM'])
  t.mock.timers.tick(1500)
  await settle()
  await stopping
  assert.deepEqual(h.signals, ['SIGTERM', 'SIGKILL'])
  t.mock.timers.reset()
})

// Each spawn gets its own process; only the first one holds its exit on EOF until released.
async function lifecycleHarness(t: TestContext) {
  const children: LifecycleChild[] = []
  mockSpawn(t, () => {
    const child = lifecycleChild(1000 + children.length, children.length === 0)
    children.push(child)
    return child
  })
  const backend = new AvaBackend(backendOptions())
  t.after(async () => {
    for (const child of children) {
      child.releaseExit()
    }
    await backend.stop()
    t.mock.restoreAll()
    syncBuiltinESMExports()
  })
  await backend.start()
  assert.equal(backend.snapshot().connection, 'ready')
  return { backend, children }
}

test('overlapping restarts are rejected: one live AVA, and the replaced one exits', async (t) => {
  const { backend, children } = await lifecycleHarness(t)
  const first = backend.handle(IPC_CHANNELS.restart, null)
  assert.equal(backend.snapshot().connection, 'starting', 'Restart stays disabled for the whole transaction')
  const second = await backend.handle(IPC_CHANNELS.restart, null) as { ok: boolean }
  assert.equal(second.ok, false, 'a second restart cannot launch a parallel AVA')
  const workspace = await backend.handle(IPC_CHANNELS.selectWorkspace, null) as { ok: boolean }
  assert.equal(workspace.ok, false, 'a workspace change cannot launch a parallel AVA either')
  assert.equal(children.length, 1, 'nothing spawns while the replaced AVA is still exiting')
  children[0]!.releaseExit()
  assert.deepEqual(await first, { ok: true })
  assert.equal(children.length, 2)
  assert.deepEqual(children.map(exited), [true, false], 'exactly one live AVA; the replaced one exited')
  assert.equal(backend.snapshot().connection, 'ready')
  await backend.stop()
  assert.deepEqual(children.map(exited), [true, true], 'no AVA outlives stop()')
})

test('quitting mid-restart launches nothing and waits for the replaced AVA to exit', async (t) => {
  const { backend, children } = await lifecycleHarness(t)
  const restart = backend.handle(IPC_CHANNELS.restart, null)
  let stopped = false
  const stopping = backend.stop('application quit').then(() => {
    stopped = true
  })
  await new Promise((resolve) => setImmediate(resolve))
  assert.equal(stopped, false, 'quit waits while the replaced AVA is still running')
  children[0]!.releaseExit()
  await stopping
  assert.equal((await restart as { ok: boolean }).ok, false, 'the interrupted restart is not reported as done')
  assert.equal(children.length, 1, 'no AVA is spawned after quit')
  assert.equal(exited(children[0]!), true)
})
