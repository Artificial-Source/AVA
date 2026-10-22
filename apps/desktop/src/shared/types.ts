export type JsonObject = Record<string, unknown>

export type RpcErrorShape = {
  category: string
  code: string
  message: string
  details: string
}

export type ProtocolInfo = {
  protocolVersion: number
  eventSchemaVersion: number
  sessionEntryVersion: number
}

export type AvaState = {
  workspaceDir: string
  currentDir: string
  sessionId: string
  provider: string
  model: string
  mode: string
}

export type SessionSummary = {
  sessionId: string
  title: string
  lastUpdated: string
  entryCount: number
}

export type ModelSummary = {
  provider: string
  model: string
  displayName: string
  selectable: boolean
  selected: boolean
}

export type QuestionOption = {
  value: string
  label: string
}

export type PermissionPromptView = {
  displayToken: string
  operation: string
  mode: string
  targetPath: string
  command: string
  toolName: string
  reason: string
  risk: string
  diffPreview: string
}

export type QuestionPromptView = {
  displayToken: string
  header: string
  question: string
  options: QuestionOption[]
  multiple: boolean
  allowCustom: boolean
  secret: boolean
}

export type TranscriptItem =
  | { kind: 'user'; id: string; text: string }
  | { kind: 'assistant'; id: string; text: string; streaming: boolean }
  | { kind: 'tool'; id: string; callId: string; tool: string; status: string; summary: string }
  | { kind: 'error'; id: string; text: string }
  | { kind: 'notice'; id: string; text: string }

export type ConnectionStatus = 'starting' | 'ready' | 'error' | 'stopped'

export type UserFacingError = {
  title: string
  message: string
  hint: string
}

export type HostSnapshot = {
  connection: ConnectionStatus
  connectionError: UserFacingError | null
  lastActionError: UserFacingError | null
  protocol: ProtocolInfo | null
  state: AvaState | null
  binaryPath: string
  workspace: string
  sessions: SessionSummary[]
  models: ModelSummary[]
  items: TranscriptItem[]
  busy: boolean
  canceling: boolean
  pendingPermission: PermissionPromptView | null
  pendingQuestion: QuestionPromptView | null
}

export type QuestionReplyInput = {
  answer?: string
  selected?: string
  selectedOptions?: string[]
}

export type PermissionDecision = 'allow' | 'deny'
