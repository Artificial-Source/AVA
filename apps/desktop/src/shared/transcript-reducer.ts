import type { RpcEvent } from './rpc-session.ts'
import { asBoolean, asNumber, asObject, asString, readStringField, truncateText } from './text.ts'
import type { JsonObject, TranscriptItem } from './types.ts'

const TOOL_SUMMARY_CHARS = 160

export type TranscriptState = {
  items: TranscriptItem[]
  draftAssistantId: string | null
  lastAssistantId: string | null
  runSeen: boolean
  nextId: number
}

export function createTranscriptState(): TranscriptState {
  return {
    items: [],
    draftAssistantId: null,
    lastAssistantId: null,
    runSeen: false,
    nextId: 1
  }
}

function nextItemId(state: TranscriptState, prefix: string, fallback: string): string {
  if (fallback) {
    return fallback
  }
  const id = `${prefix}-${state.nextId}`
  state.nextId += 1
  return id
}

function replaceItem(state: TranscriptState, id: string, item: TranscriptItem): void {
  state.items = state.items.map((existing) => (existing.id === id ? item : existing))
}

function lastItem(state: TranscriptState): TranscriptItem | undefined {
  return state.items[state.items.length - 1]
}

function freezeDraft(state: TranscriptState): void {
  if (!state.draftAssistantId) {
    return
  }
  const current = state.items.find((item) => item.id === state.draftAssistantId)
  if (current?.kind === 'assistant') {
    replaceItem(state, current.id, { ...current, streaming: false })
    state.lastAssistantId = current.id
  }
  state.draftAssistantId = null
}

function upsertDraft(state: TranscriptState, delta: string, eventId: string): void {
  if (!delta) {
    if (!state.draftAssistantId) {
      const id = nextItemId(state, 'assistant', eventId)
      state.items = [...state.items, { kind: 'assistant', id, text: '', streaming: true }]
      state.draftAssistantId = id
    }
    return
  }
  if (state.draftAssistantId) {
    const current = state.items.find((item) => item.id === state.draftAssistantId)
    if (current?.kind === 'assistant') {
      replaceItem(state, current.id, { ...current, text: current.text + delta, streaming: true })
      return
    }
  }
  const id = nextItemId(state, 'assistant', eventId)
  state.items = [...state.items, { kind: 'assistant', id, text: delta, streaming: true }]
  state.draftAssistantId = id
}

function toolSummary(payload: JsonObject): string {
  const text = readStringField(payload, 'text')
  const message = readStringField(payload, 'message')
  return truncateText(text || message, TOOL_SUMMARY_CHARS)
}

function upsertTool(state: TranscriptState, payload: JsonObject, eventId: string): void {
  const callId = readStringField(payload, 'call_id')
  const tool = readStringField(payload, 'tool') || 'tool'
  const status = readStringField(payload, 'status') || 'running'
  const summary = toolSummary(payload)
  const existing = callId
    ? state.items.find((item) => item.kind === 'tool' && item.callId === callId)
    : undefined
  if (existing && existing.kind === 'tool') {
    replaceItem(state, existing.id, {
      ...existing,
      tool: tool || existing.tool,
      status,
      summary: summary || existing.summary
    })
    return
  }
  state.items = [
    ...state.items,
    {
      kind: 'tool',
      id: nextItemId(state, 'tool', eventId || callId),
      callId: callId || eventId,
      tool,
      status,
      summary
    }
  ]
}

function textsEquivalent(left: string, right: string): boolean {
  return left.replace(/\s+$/u, '') === right.replace(/\s+$/u, '')
}

function applyAssistantCanonical(state: TranscriptState, text: string, eventId: string): void {
  freezeDraft(state)
  if (!text) {
    return
  }
  if (state.lastAssistantId) {
    const last = state.items.find((item) => item.id === state.lastAssistantId)
    if (last?.kind === 'assistant') {
      if (textsEquivalent(last.text, text) || last.text.length === 0) {
        replaceItem(state, last.id, { ...last, text, streaming: false })
        return
      }
    }
  }
  const last = lastItem(state)
  if (last?.kind === 'assistant' && textsEquivalent(last.text, text)) {
    replaceItem(state, last.id, { ...last, text, streaming: false })
    state.lastAssistantId = last.id
    return
  }
  const id = nextItemId(state, 'assistant', eventId)
  state.items = [...state.items, { kind: 'assistant', id, text, streaming: false }]
  state.lastAssistantId = id
}

export function applyPromptFinalText(state: TranscriptState, finalText: string): TranscriptState {
  const next: TranscriptState = {
    ...state,
    items: [...state.items]
  }
  if (!finalText) {
    freezeDraft(next)
    return next
  }
  applyAssistantCanonical(next, finalText, '')
  return next
}

export function resetTranscript(state: TranscriptState): TranscriptState {
  return {
    items: [],
    draftAssistantId: null,
    lastAssistantId: null,
    runSeen: false,
    nextId: state.nextId
  }
}

export function loadHistoryMessages(state: TranscriptState, messages: unknown): TranscriptState {
  const next = resetTranscript(state)
  if (!Array.isArray(messages)) {
    return next
  }
  for (const entry of messages) {
    const object = asObject(entry)
    if (!object) {
      continue
    }
    const type = asString(object.type) ?? ''
    const data = asObject(object.data) ?? {}
    const id = asString(object.id) ?? ''
    if (type === 'user_message') {
      const text = readStringField(data, 'text')
      if (text) {
        next.items.push({ kind: 'user', id: nextItemId(next, 'user', id), text })
      }
      continue
    }
    if (type === 'assistant_message') {
      const text = readStringField(data, 'text')
      if (text) {
        const itemId = nextItemId(next, 'assistant', id)
        next.items.push({ kind: 'assistant', id: itemId, text, streaming: false })
        next.lastAssistantId = itemId
      }
      continue
    }
    if (type === 'tool_call' || type === 'tool_result') {
      const tool = readStringField(data, 'name') || readStringField(data, 'tool') || type
      const summary = truncateText(
        readStringField(data, 'text') || readStringField(data, 'summary') || tool,
        TOOL_SUMMARY_CHARS
      )
      next.items.push({
        kind: 'tool',
        id: nextItemId(next, 'tool', id),
        callId: readStringField(data, 'id') || id,
        tool,
        status: type === 'tool_result' ? 'success' : 'running',
        summary
      })
    }
  }
  return next
}

export function applyRpcEvent(state: TranscriptState, event: RpcEvent): TranscriptState {
  const next: TranscriptState = {
    ...state,
    items: [...state.items],
    runSeen: true
  }
  const payload = event.payload
  const eventId = event.eventId

  switch (event.name) {
    case 'user_message': {
      const text = readStringField(payload, 'text')
      if (text) {
        next.items.push({ kind: 'user', id: nextItemId(next, 'user', eventId), text })
      }
      break
    }
    case 'message_update':
      upsertDraft(next, readStringField(payload, 'text'), eventId)
      break
    case 'message_end':
      freezeDraft(next)
      break
    case 'assistant_message':
      applyAssistantCanonical(next, readStringField(payload, 'text'), eventId)
      break
    case 'tool_start':
    case 'tool_progress':
    case 'tool_result':
      freezeDraft(next)
      upsertTool(next, payload, eventId)
      break
    case 'error': {
      freezeDraft(next)
      const text =
        readStringField(payload, 'message') ||
        readStringField(payload, 'text') ||
        readStringField(payload, 'details') ||
        'Run failed'
      next.items.push({ kind: 'error', id: nextItemId(next, 'error', eventId), text })
      break
    }
    case 'canceled': {
      freezeDraft(next)
      const text = readStringField(payload, 'text') || readStringField(payload, 'reason') || 'Canceled'
      next.items.push({ kind: 'notice', id: nextItemId(next, 'notice', eventId), text })
      break
    }
    case 'done':
      freezeDraft(next)
      break
    default:
      break
  }
  return next
}

export function parseStateResult(result: JsonObject): {
  workspaceDir: string
  currentDir: string
  sessionId: string
  provider: string
  model: string
  mode: string
} {
  return {
    workspaceDir: readStringField(result, 'workspace_dir'),
    currentDir: readStringField(result, 'current_dir'),
    sessionId: readStringField(result, 'session_id'),
    provider: readStringField(result, 'provider'),
    model: readStringField(result, 'model'),
    mode: readStringField(result, 'mode')
  }
}

export function parseSessionsResult(result: JsonObject): {
  sessionId: string
  title: string
  lastUpdated: string
  entryCount: number
}[] {
  const sessions = result.sessions
  if (!Array.isArray(sessions)) {
    return []
  }
  return sessions.flatMap((entry) => {
    const object = asObject(entry)
    if (!object) {
      return []
    }
    const sessionId = asString(object.session_id)
    if (!sessionId) {
      return []
    }
    return [
      {
        sessionId,
        title: asString(object.title) ?? '',
        lastUpdated: asString(object.last_updated) ?? '',
        entryCount: asNumber(object.entry_count) ?? 0
      }
    ]
  })
}

export function parseModelsResult(result: JsonObject): {
  provider: string
  model: string
  displayName: string
  selectable: boolean
  selected: boolean
}[] {
  const models = result.models
  if (!Array.isArray(models)) {
    return []
  }
  return models.flatMap((entry) => {
    const object = asObject(entry)
    if (!object) {
      return []
    }
    const model = asString(object.model)
    const provider = asString(object.provider) ?? ''
    if (!model) {
      return []
    }
    return [
      {
        provider,
        model,
        displayName: asString(object.display_name) ?? model,
        selectable: asBoolean(object.selectable) !== false,
        selected: asBoolean(object.selected) === true
      }
    ]
  })
}

export function parseProtocolResult(result: JsonObject): {
  protocolVersion: number
  eventSchemaVersion: number
  sessionEntryVersion: number
} {
  return {
    protocolVersion: asNumber(result.protocol_version) ?? 1,
    eventSchemaVersion: asNumber(result.event_schema_version) ?? 1,
    sessionEntryVersion: asNumber(result.session_entry_version) ?? 0
  }
}
