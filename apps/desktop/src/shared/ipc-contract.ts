import { asObject, asString, asStringArray, isNonEmptyString, isProtocolId } from './text.ts'
import type { PermissionDecision, QuestionReplyInput } from './types.ts'

export const IPC_CHANNELS = {
  getSnapshot: 'ava:getSnapshot',
  selectWorkspace: 'ava:selectWorkspace',
  restart: 'ava:restart',
  newSession: 'ava:newSession',
  openSession: 'ava:openSession',
  setModel: 'ava:setModel',
  prompt: 'ava:prompt',
  cancel: 'ava:cancel',
  replyPermission: 'ava:replyPermission',
  replyQuestion: 'ava:replyQuestion'
} as const

export const IPC_EVENTS = {
  snapshot: 'ava:snapshot'
} as const

export type IpcChannel = (typeof IPC_CHANNELS)[keyof typeof IPC_CHANNELS]

const CHANNEL_SET = new Set<string>(Object.values(IPC_CHANNELS))
const EVENT_SET = new Set<string>(Object.values(IPC_EVENTS))

export const MAX_PROMPT_BYTES = 1_048_576
export const MAX_MODEL_BYTES = 256
export const MAX_SESSION_ID_BYTES = 256

export type SenderView = {
  senderId: number
  expectedSenderId: number
  isDestroyed: boolean
  isMainFrame: boolean
  url: string
}

export type IpcValidationError = {
  ok: false
  reason: string
}

export function isAllowedIpcChannel(channel: string): channel is IpcChannel {
  return CHANNEL_SET.has(channel)
}

export function isAllowedIpcEvent(channel: string): boolean {
  return EVENT_SET.has(channel)
}

// Fragments identify locations within the trusted document, not new documents.
export function isExpectedDocument(url: string, expectedDocument: string): boolean {
  try {
    const actual = new URL(url)
    const expected = new URL(expectedDocument)
    if (!['file:', 'http:', 'https:'].includes(expected.protocol)) {
      return false
    }
    actual.hash = ''
    expected.hash = ''
    return actual.href === expected.href
  } catch {
    return false
  }
}

export function isTrustedSender(sender: SenderView, expectedDocument: string): boolean {
  return !sender.isDestroyed && sender.senderId === sender.expectedSenderId &&
    sender.isMainFrame && isExpectedDocument(sender.url, expectedDocument)
}

function emptyPayload(payload: unknown): boolean {
  if (payload === undefined || payload === null) {
    return true
  }
  const object = asObject(payload)
  return object !== null && Object.keys(object).length === 0
}

export function validateNoPayload(payload: unknown): IpcValidationError | { ok: true } {
  if (!emptyPayload(payload)) {
    return { ok: false, reason: 'this command does not take a payload' }
  }
  return { ok: true }
}

export function validateOpenSession(payload: unknown): IpcValidationError | { ok: true; sessionId: string } {
  const object = asObject(payload)
  if (!object) {
    return { ok: false, reason: 'openSession requires an object payload' }
  }
  const sessionId = object.sessionId
  if (!isProtocolId(sessionId, MAX_SESSION_ID_BYTES) && !isNonEmptyString(sessionId, MAX_SESSION_ID_BYTES)) {
    return { ok: false, reason: 'sessionId must be a non-empty string' }
  }
  return { ok: true, sessionId }
}

export function validateSetModel(
  payload: unknown
): IpcValidationError | { ok: true; model: string; provider?: string } {
  const object = asObject(payload)
  if (!object) {
    return { ok: false, reason: 'setModel requires an object payload' }
  }
  const model = object.model
  if (!isNonEmptyString(model, MAX_MODEL_BYTES)) {
    return { ok: false, reason: 'model must be a non-empty string' }
  }
  if (object.provider === undefined) {
    return { ok: true, model }
  }
  const provider = object.provider
  if (!isNonEmptyString(provider, MAX_MODEL_BYTES)) {
    return { ok: false, reason: 'provider must be a non-empty string when provided' }
  }
  return { ok: true, model, provider }
}

export function validatePrompt(payload: unknown): IpcValidationError | { ok: true; message: string } {
  const object = asObject(payload)
  if (!object) {
    return { ok: false, reason: 'prompt requires an object payload' }
  }
  const message = object.message
  if (!isNonEmptyString(message, MAX_PROMPT_BYTES)) {
    return { ok: false, reason: 'message must be a non-empty string' }
  }
  return { ok: true, message }
}

export function validatePermissionReply(
  payload: unknown
): IpcValidationError | { ok: true; displayToken: string; decision: PermissionDecision } {
  const object = asObject(payload)
  if (!object) {
    return { ok: false, reason: 'permission reply requires an object payload' }
  }
  if ('request_id' in object || 'correlation_id' in object || 'requestId' in object || 'correlationId' in object) {
    return { ok: false, reason: 'renderer may not supply permission correlation' }
  }
  if (!isNonEmptyString(object.displayToken, 256)) {
    return { ok: false, reason: 'displayToken must identify the displayed prompt' }
  }
  const decision = object.decision
  if (decision !== 'allow' && decision !== 'deny') {
    return { ok: false, reason: 'decision must be allow or deny' }
  }
  return { ok: true, displayToken: object.displayToken, decision }
}

export function validateQuestionReply(
  payload: unknown
): IpcValidationError | { ok: true; displayToken: string; reply: QuestionReplyInput } {
  const object = asObject(payload)
  if (!object) {
    return { ok: false, reason: 'question reply requires an object payload' }
  }
  if ('request_id' in object || 'correlation_id' in object || 'requestId' in object || 'correlationId' in object) {
    return { ok: false, reason: 'renderer may not supply question correlation' }
  }
  if (!isNonEmptyString(object.displayToken, 256)) {
    return { ok: false, reason: 'displayToken must identify the displayed prompt' }
  }
  const reply: QuestionReplyInput = {}
  if (object.answer !== undefined) {
    const answer = asString(object.answer)
    if (answer === null) {
      return { ok: false, reason: 'answer must be a string' }
    }
    reply.answer = answer
  }
  if (object.selected !== undefined) {
    const selected = asString(object.selected)
    if (selected === null) {
      return { ok: false, reason: 'selected must be a string' }
    }
    reply.selected = selected
  }
  if (object.selectedOptions !== undefined) {
    const selectedOptions = asStringArray(object.selectedOptions)
    if (selectedOptions === null) {
      return { ok: false, reason: 'selectedOptions must be an array of strings' }
    }
    reply.selectedOptions = selectedOptions
  }
  if (reply.answer === undefined && reply.selected === undefined && reply.selectedOptions === undefined) {
    return { ok: false, reason: 'question reply needs an answer, selected value, or selectedOptions' }
  }
  return { ok: true, displayToken: object.displayToken, reply }
}

export function validateChannelPayload(
  channel: string,
  payload: unknown
): IpcValidationError | { ok: true; value?: unknown } {
  if (!isAllowedIpcChannel(channel)) {
    return { ok: false, reason: 'channel is not allowlisted' }
  }
  switch (channel) {
    case IPC_CHANNELS.getSnapshot:
    case IPC_CHANNELS.selectWorkspace:
    case IPC_CHANNELS.restart:
    case IPC_CHANNELS.newSession:
    case IPC_CHANNELS.cancel:
      return validateNoPayload(payload)
    case IPC_CHANNELS.openSession:
      return validateOpenSession(payload)
    case IPC_CHANNELS.setModel:
      return validateSetModel(payload)
    case IPC_CHANNELS.prompt:
      return validatePrompt(payload)
    case IPC_CHANNELS.replyPermission:
      return validatePermissionReply(payload)
    case IPC_CHANNELS.replyQuestion:
      return validateQuestionReply(payload)
    default:
      return { ok: false, reason: 'channel is not allowlisted' }
  }
}
