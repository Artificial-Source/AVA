import assert from 'node:assert/strict'
import { test } from 'node:test'
import {
  IPC_CHANNELS,
  isAllowedIpcChannel,
  isTrustedSender,
  isExpectedDocument,
  validateChannelPayload
} from '../src/shared/ipc-contract.ts'

test('only allowlisted channels are accepted', () => {
  assert.equal(isAllowedIpcChannel(IPC_CHANNELS.prompt), true)
  assert.equal(isAllowedIpcChannel('ava:run_bash'), false)
  assert.equal(validateChannelPayload('ava:spawn', { binary: '/tmp/ava' }).ok, false)
})

test('navigation and IPC trust only the exact app document, with fragments allowed', () => {
  const sender = { senderId: 7, expectedSenderId: 7, isDestroyed: false, isMainFrame: true, url: '' }
  for (const expected of ['file:///tmp/out/renderer/index.html', 'http://localhost:5173/index.html']) {
    for (const url of [expected, `${expected}#question`]) {
      assert.equal(isExpectedDocument(url, expected), true)
      assert.equal(isTrustedSender({ ...sender, url }, expected), true)
      assert.equal(isTrustedSender({ ...sender, url, isMainFrame: false }, expected), false)
      assert.equal(isTrustedSender({ ...sender, url, senderId: 8 }, expected), false)
      assert.equal(isTrustedSender({ ...sender, url, isDestroyed: true }, expected), false)
    }
    for (const url of [
      'file:///workspace/evil.html', 'http://localhost:5173/other.html',
      expected.replace('index.html', '%69ndex.html'), `${expected}?route=evil`,
      `${expected}?`, 'javascript:alert(1)', 'data:text/html,evil', 'not a url',
      expected.replace('index.html', 'index.html%2fother'), 'https://evil.example/'
    ]) {
      assert.equal(isExpectedDocument(url, expected), false, url)
      assert.equal(isTrustedSender({ ...sender, url }, expected), false, url)
    }
  }
  assert.equal(isExpectedDocument('http://localhost:5173/?configured=1#ok', 'http://localhost:5173/?configured=1'), true)
  assert.equal(isExpectedDocument('http://localhost:5173/?configured=2', 'http://localhost:5173/?configured=1'), false)
  assert.equal(isExpectedDocument('data:text/html,evil', 'data:text/html,evil'), false)
})

test('payload validation rejects renderer-supplied correlation and unknown spawn fields', () => {
  assert.equal(validateChannelPayload(IPC_CHANNELS.prompt, { message: 'hello' }).ok, true)
  assert.equal(validateChannelPayload(IPC_CHANNELS.prompt, { message: '' }).ok, false)
  assert.equal(validateChannelPayload(IPC_CHANNELS.openSession, { sessionId: 'session_1' }).ok, true)
  const permission = validateChannelPayload(IPC_CHANNELS.replyPermission, {
    decision: 'allow',
    request_id: 'permission_1',
    correlation_id: 'prompt-1'
  })
  assert.equal(permission.ok, false)
  const question = validateChannelPayload(IPC_CHANNELS.replyQuestion, {
    selected: 'a',
    correlationId: 'prompt-1'
  })
  assert.equal(question.ok, false)
  assert.equal(validateChannelPayload(IPC_CHANNELS.replyPermission, { decision: 'allow_session' }).ok, false)
  assert.equal(validateChannelPayload(IPC_CHANNELS.replyPermission, { displayToken: 'opaque-display', decision: 'allow' }).ok, true)
  assert.equal(validateChannelPayload(IPC_CHANNELS.restart, { cwd: '/tmp', binary: '/bin/ava' }).ok, false)
  assert.equal(validateChannelPayload(IPC_CHANNELS.selectWorkspace, { workspace: '/tmp' }).ok, false)
})

test('prompt replies require an opaque display token, never RPC correlation', () => {
  for (const channel of [IPC_CHANNELS.replyPermission, IPC_CHANNELS.replyQuestion]) {
    const body = channel === IPC_CHANNELS.replyPermission ? { decision: 'deny' } : { selected: 'a' }
    assert.equal(validateChannelPayload(channel, body).ok, false)
    assert.equal(validateChannelPayload(channel, { ...body, displayToken: '' }).ok, false)
    assert.equal(validateChannelPayload(channel, { ...body, displayToken: 'opaque-display' }).ok, true)
    for (const field of ['request_id', 'requestId', 'correlation_id', 'correlationId']) {
      assert.equal(validateChannelPayload(channel, { ...body, displayToken: 'opaque-display', [field]: 'backend-id' }).ok, false)
    }
  }
})
