import assert from 'node:assert/strict'
import { spawn } from 'node:child_process'
import { chmodSync, mkdirSync, mkdtempSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { isAbsolute, join } from 'node:path'
import { test } from 'node:test'
import { RpcSession } from '../src/shared/rpc-session.ts'

const enabled = process.env.AVA_DESKTOP_RPC_SMOKE === '1'

test('opt-in real RPC smoke against an explicit AVA binary', { skip: !enabled }, async (t) => {
  const binary = process.env.AVA_DESKTOP_BINARY
  assert.ok(binary && isAbsolute(binary), 'set AVA_DESKTOP_BINARY to an absolute built ava executable')
  const root = mkdtempSync(join(tmpdir(), 'ava-desktop-rpc-smoke-'))
  const home = join(root, 'home')
  const config = join(root, 'config')
  const state = join(root, 'state')
  const data = join(root, 'data')
  const workspace = join(root, 'workspace')
  for (const directory of [home, config, state, data, workspace, join(config, 'ava')]) {
    mkdirSync(directory, { recursive: true })
  }
  chmodSync(join(config, 'ava'), 0o700)
  const env: NodeJS.ProcessEnv = {
    ...process.env,
    HOME: home,
    XDG_CONFIG_HOME: config,
    XDG_STATE_HOME: state,
    XDG_DATA_HOME: data,
    NO_COLOR: '1'
  }
  delete env.AVA_DESKTOP_BINARY
  const child = spawn(binary, ['--rpc', '--offline'], {
    cwd: workspace,
    env,
    stdio: ['pipe', 'pipe', 'pipe'],
    shell: false
  })
  t.after(async () => {
    if (child.stdin.writable) {
      child.stdin.end()
    }
    if (child.exitCode === null) {
      child.kill('SIGTERM')
    }
  })
  child.stderr.on('data', () => {
    /* drain without logging */
  })
  const session = new RpcSession({
    write: (bytes) => {
      child.stdin.write(bytes)
    }
  })
  child.stdout.on('data', (chunk: Buffer) => {
    session.handleStdout(chunk)
  })
  child.stdout.on('end', () => {
    session.handleStdoutEnd()
  })
  const protocol = await session.request('get_protocol')
  assert.equal(protocol.protocol_version, 1)
  const stateResult = await session.request('get_state')
  assert.equal(typeof stateResult.workspace_dir, 'string')
  assert.equal(typeof stateResult.session_id, 'string')
  const initialSessionId = String(stateResult.session_id)
  const models = await session.request('list_models')
  assert.ok(Array.isArray(models.models))
  const messages = await session.request('get_messages')
  assert.ok(Array.isArray(messages.messages))
  const created = await session.request('new_session')
  assert.equal(typeof created.session_id, 'string')
  assert.notEqual(created.session_id, initialSessionId)
  const listed = await session.request('list_sessions')
  assert.ok(Array.isArray(listed.sessions))
  const opened = await session.request('open_session', { session_id: initialSessionId })
  assert.equal(opened.session_id, initialSessionId)
  if (child.stdin.writable) {
    child.stdin.end()
  }
})
