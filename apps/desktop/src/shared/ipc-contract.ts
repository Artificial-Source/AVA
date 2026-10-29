import { asObject, asStringArray, isBoundedString, isNonEmptyString, utf8ByteLength } from './text.ts'
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

export const MAX_PROMPT_BYTES = 1_048_576
export const MAX_MODEL_BYTES = 256
export const MAX_SESSION_ID_BYTES = 256
export const MAX_DISPLAY_TOKEN_BYTES = 256
// RPC v1 question_reply limits (docs/rpc-protocol.md, "Request text limits"). Rejecting an
// oversized reply here keeps the display open; the backend would reject it and fail the run.
export const MAX_QUESTION_VALUE_BYTES = 8192
export const MAX_QUESTION_SELECTED_OPTIONS = 64

const CORRELATION_FIELDS = ['request_id', 'correlation_id', 'requestId', 'correlationId']

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

type NoPayloadChannel =
  | typeof IPC_CHANNELS.getSnapshot
  | typeof IPC_CHANNELS.selectWorkspace
  | typeof IPC_CHANNELS.restart
  | typeof IPC_CHANNELS.newSession
  | typeof IPC_CHANNELS.cancel

// A renderer request after validation; the channel discriminates the typed payload.
export type IpcRequest =
  | { channel: NoPayloadChannel }
  | { channel: typeof IPC_CHANNELS.openSession; sessionId: string }
  | { channel: typeof IPC_CHANNELS.setModel; model: string; provider?: string }
  | { channel: typeof IPC_CHANNELS.prompt; message: string }
  | { channel: typeof IPC_CHANNELS.replyPermission; displayToken: string; decision: PermissionDecision }
  | { channel: typeof IPC_CHANNELS.replyQuestion; displayToken: string; reply: QuestionReplyInput }

type Validated<T> = IpcValidationError | ({ ok: true } & T)

export function isAllowedIpcChannel(channel: string): channel is IpcChannel {
  return CHANNEL_SET.has(channel)
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

function invalid(reason: string): IpcValidationError {
  return { ok: false, reason }
}

function isEmptyPayload(payload: unknown): boolean {
  if (payload === undefined || payload === null) {
    return true
  }
  const object = asObject(payload)
  return object !== null && Object.keys(object).length === 0
}

function suppliesCorrelation(object: Record<string, unknown>): boolean {
  return CORRELATION_FIELDS.some((field) => field in object)
}

function validateOpenSession(payload: unknown): Validated<{ sessionId: string }> {
  const object = asObject(payload)
  if (!object) {
    return invalid('openSession requires an object payload')
  }
  const sessionId = object.sessionId
  if (!isNonEmptyString(sessionId, MAX_SESSION_ID_BYTES)) {
    return invalid('sessionId must be a non-empty string')
  }
  return { ok: true, sessionId }
}

function validateSetModel(payload: unknown): Validated<{ model: string; provider?: string }> {
  const object = asObject(payload)
  if (!object) {
    return invalid('setModel requires an object payload')
  }
  const model = object.model
  if (!isNonEmptyString(model, MAX_MODEL_BYTES)) {
    return invalid('model must be a non-empty string')
  }
  if (object.provider === undefined) {
    return { ok: true, model }
  }
  const provider = object.provider
  if (!isNonEmptyString(provider, MAX_MODEL_BYTES)) {
    return invalid('provider must be a non-empty string when provided')
  }
  return { ok: true, model, provider }
}

function validatePrompt(payload: unknown): Validated<{ message: string }> {
  const object = asObject(payload)
  if (!object) {
    return invalid('prompt requires an object payload')
  }
  const message = object.message
  if (!isNonEmptyString(message, MAX_PROMPT_BYTES)) {
    return invalid('message must be a non-empty string')
  }
  return { ok: true, message }
}

function validatePermissionReply(payload: unknown): Validated<{ displayToken: string; decision: PermissionDecision }> {
  const object = asObject(payload)
  if (!object) {
    return invalid('permission reply requires an object payload')
  }
  if (suppliesCorrelation(object)) {
    return invalid('renderer may not supply permission correlation')
  }
  const displayToken = object.displayToken
  if (!isNonEmptyString(displayToken, MAX_DISPLAY_TOKEN_BYTES)) {
    return invalid('displayToken must identify the displayed prompt')
  }
  const decision = object.decision
  if (decision !== 'allow' && decision !== 'deny') {
    return invalid('decision must be allow or deny')
  }
  return { ok: true, displayToken, decision }
}

function validateQuestionReply(payload: unknown): Validated<{ displayToken: string; reply: QuestionReplyInput }> {
  const object = asObject(payload)
  if (!object) {
    return invalid('question reply requires an object payload')
  }
  if (suppliesCorrelation(object)) {
    return invalid('renderer may not supply question correlation')
  }
  const displayToken = object.displayToken
  if (!isNonEmptyString(displayToken, MAX_DISPLAY_TOKEN_BYTES)) {
    return invalid('displayToken must identify the displayed prompt')
  }
  const reply: QuestionReplyInput = {}
  if (object.answer !== undefined) {
    if (!isBoundedString(object.answer, MAX_QUESTION_VALUE_BYTES)) {
      return invalid(`answer must be a string of at most ${MAX_QUESTION_VALUE_BYTES} bytes`)
    }
    reply.answer = object.answer
  }
  if (object.selected !== undefined) {
    if (!isBoundedString(object.selected, MAX_QUESTION_VALUE_BYTES)) {
      return invalid(`selected must be a string of at most ${MAX_QUESTION_VALUE_BYTES} bytes`)
    }
    reply.selected = object.selected
  }
  if (object.selectedOptions !== undefined) {
    const selectedOptions = asStringArray(object.selectedOptions)
    if (
      selectedOptions === null ||
      selectedOptions.length > MAX_QUESTION_SELECTED_OPTIONS ||
      selectedOptions.some((value) => utf8ByteLength(value) > MAX_QUESTION_VALUE_BYTES)
    ) {
      return invalid(
        `selectedOptions must be at most ${MAX_QUESTION_SELECTED_OPTIONS} strings of at most ${MAX_QUESTION_VALUE_BYTES} bytes each`
      )
    }
    reply.selectedOptions = selectedOptions
  }
  if (reply.answer === undefined && reply.selected === undefined && reply.selectedOptions === undefined) {
    return invalid('question reply needs an answer, selected value, or selectedOptions')
  }
  return { ok: true, displayToken, reply }
}

export function validateChannelPayload(channel: string, payload: unknown): Validated<IpcRequest> {
  if (!isAllowedIpcChannel(channel)) {
    return invalid('channel is not allowlisted')
  }
  switch (channel) {
    case IPC_CHANNELS.getSnapshot:
    case IPC_CHANNELS.selectWorkspace:
    case IPC_CHANNELS.restart:
    case IPC_CHANNELS.newSession:
    case IPC_CHANNELS.cancel:
      return isEmptyPayload(payload) ? { ok: true, channel } : invalid('this command does not take a payload')
    case IPC_CHANNELS.openSession: {
      const checked = validateOpenSession(payload)
      return checked.ok ? { ...checked, channel } : checked
    }
    case IPC_CHANNELS.setModel: {
      const checked = validateSetModel(payload)
      return checked.ok ? { ...checked, channel } : checked
    }
    case IPC_CHANNELS.prompt: {
      const checked = validatePrompt(payload)
      return checked.ok ? { ...checked, channel } : checked
    }
    case IPC_CHANNELS.replyPermission: {
      const checked = validatePermissionReply(payload)
      return checked.ok ? { ...checked, channel } : checked
    }
    case IPC_CHANNELS.replyQuestion: {
      const checked = validateQuestionReply(payload)
      return checked.ok ? { ...checked, channel } : checked
    }
  }
}
