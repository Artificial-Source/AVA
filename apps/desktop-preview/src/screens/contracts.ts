import type { RequestIdentity } from '../chat/contracts';
import type { ModelId } from '../state/types';
import type { StatusValue } from '../ui/Status';
import type { GeneralPreferences, ConversationPreferences, NotificationPreferences, ContextPolicy } from './behaviorPreferences';
import type { ShortcutAction, ShortcutPreferences } from './shortcutBindings';

// First-party presentation contracts only. No RPC/config/auth schemas or execution authority.
export type ListState = 'ready' | 'loading' | 'empty' | 'error' | 'stale' | 'unavailable' | 'busy' | 'disabled';
export type ModelScope = 'current-chat' | 'new-chat-default';
export type ModelChoice = Readonly<{
  id: string; label: string; providerId: string; providerLabel: string;
  family?: Readonly<{ id: string; label: string }>;
  modelId?: ModelId; windowTokens?: number; capabilities: readonly string[];
  reasoning: readonly string[]; disabledReason?: string;
}>;
export type ModelPickerView = RequestIdentity & Readonly<{
  state: ListState; scope: ModelScope; currentId: string; defaultId: string;
  currentReasoning?: string | null; defaultReasoning?: string | null;
  reasoningEditable?: boolean;
  choices: readonly ModelChoice[];
}>;
export type ModelSelection = RequestIdentity & Readonly<{ modelId: ModelId; scope: ModelScope; reasoning: string | null }>;
// Captured main-view identity; this is UI scope, not a backend request or session lease.
export type MainScreenRequest = RequestIdentity & Readonly<{
  kind: 'settings' | 'models' | 'sessions' | 'commands' | 'rename' | 'archive' | 'branch' | 'workspace' | 'providers';
  parentChatId: string; inspectedChildId: string | null;
  // Rename/archive/branch target; defaults to parentChatId.
  targetChatId?: string;
}>;
// `id` is the message id: a branch keeps the chat up to and including that message.
// `label` is a one-line preview; `text`, when present, is the whole message.
export type BranchPoint = Readonly<{ id: string; label: string; text?: string }>;
// One chat as the finder, palette and chat dialogs see it. Archived chats are included.
// `searchText` is what Find a chat searches beyond the title: the chat's message text.
export type ChatSummary = Readonly<{
  id: string; title: string; projectId: string; projectLabel: string; status: StatusValue;
  archived: boolean; current: boolean; branchPoints: readonly BranchPoint[]; searchText?: string;
}>;

export type ConnectMethod = 'key' | 'browser' | 'device';
export type Provider = Readonly<{
  id: string; label: string; state: 'available' | 'connected-example' | 'external' | 'error';
  source: 'none' | 'stored-example' | 'environment-example';
  methods: readonly ConnectMethod[]; description: string;
}>;
export type ConnectStep = 'method' | 'key' | 'browser' | 'device' | 'waiting' | 'success' | 'error' | 'canceled' | 'disconnect';
export type ConnectView = RequestIdentity & Readonly<{ provider: Provider; step: ConnectStep; method: ConnectMethod | null }>;
export type ConnectAction =
  | Readonly<{ action: 'choose-method'; method: ConnectMethod }>
  | Readonly<{ action: 'submit-demo-key' | 'show-waiting' | 'show-success' | 'show-error' | 'retry' | 'back' | 'disconnect' | 'choose-model' }>;
export type ConnectIntent = RequestIdentity & ConnectAction;

export type UICommand = ShortcutAction | 'providers' | 'workspace' | 'rename' | 'archive' | 'branch' | 'shortcuts';
export type FinderItem = Readonly<{
  id: string; label: string; detail: string; projectId?: string; command?: UICommand; disabledReason?: string;
  status?: StatusValue; archived?: boolean; current?: boolean; shortcut?: string; group?: string;
  // Searched as well as the label and detail; a match found only here is shown as a snippet.
  searchText?: string;
}>;
// Which chats Find a chat lists: every chat, only active ones, or only archived ones.
export type FinderScope = 'all' | 'active' | 'archived';
export type FinderView = RequestIdentity & Readonly<{
  kind: 'sessions' | 'commands'; state: ListState; items: readonly FinderItem[];
  projects: readonly Readonly<{ id: string; label: string }>[];
}>;
export type FinderIntent = RequestIdentity & Readonly<{ itemId: string }>;
export type Preferences = Readonly<{
  density: 'comfortable' | 'compact'; motion: 'system' | 'reduced';
  help: boolean; enter: 'send' | 'newline'; notifications: 'off' | 'attention';
  general?: GeneralPreferences; conversation?: ConversationPreferences; shortcuts?: ShortcutPreferences;
  notificationPolicy?: NotificationPreferences; contextPolicy?: ContextPolicy;
  // Visit-local presentation metadata, never backend configuration or model authority.
  palette?: 'default' | 'charcoal' | 'midnight'; uiFont?: 'geist' | 'system';
  proseSize?: 'small' | 'normal' | 'large'; proseSpacing?: 'compact' | 'normal' | 'relaxed';
  readingWidth?: 'narrow' | 'balanced' | 'wide'; contrast?: 'system' | 'higher';
}>;
export type SettingsSection = 'general' | 'appearance' | 'shortcuts' | 'notifications' | 'models' | 'conversation' | 'context' | 'permissions' | 'subagents' | 'extensions' | 'about';
export type InspectionView = Readonly<{ state: ListState; rows: readonly Readonly<{ title: string; detail: string; status: string }>[] }>;
export type McpInspection = Readonly<{
  state: ListState;
  servers: readonly Readonly<{ id: string; name: string; transport: 'stdio'; configEnabled: boolean | null; source: 'global' | 'project'; trust: 'not-required' | 'required' | 'unknown'; connection: 'not-connected' | 'unknown'; lifecycle: 'one-shot' }>[];
}>;
export type ResourceInspections = Readonly<{ skills: InspectionView; prompts: InspectionView; plugins: InspectionView; lsp: InspectionView; mcp: McpInspection }>;
// Local definition metadata only. No instruction body, path, backend model policy or job identity.
export type SubagentDraft = Readonly<{ name: string; description: string; modelPreference: 'inherit' | ModelId }>;
export type SubagentDefinition = SubagentDraft & Readonly<{ id: string; revision: number; source: 'builtin' | 'user' | 'project' }>;
export type SubagentWorkspace = Readonly<{ version: number; nextUserId: number; definitions: readonly SubagentDefinition[] }>;
export type SubagentIntent = SubagentDraft & Readonly<{ version: number; definitionId: string | null; revision: number | null }>;
export type ActionView = RequestIdentity & Readonly<{ state: ListState; title: string }>;
export type RenameIntent = RequestIdentity & Readonly<{ title: string }>;
export type ChatActionIntent = RequestIdentity & Readonly<{ action: 'archive' | 'branch'; pointId?: string }>;
export type WorkspaceState = 'choice' | 'opening' | 'ready' | 'offline' | 'error' | 'unavailable' | 'canceled';
export type WorkspaceView = RequestIdentity & Readonly<{
  state: WorkspaceState; selectedId: string; choices: readonly Readonly<{ id: string; label: string; detail: string }>[];
}>;
export type WorkspaceIntent = RequestIdentity & Readonly<{ action: 'choose' | 'use' | 'retry' | 'change' | 'cancel'; workspaceId?: string }>;
export type RequestReceipt = RequestIdentity & Readonly<{ kind: 'permission' | 'question-answer' | 'question-cancel'; decision?: 'allow' | 'allow_session' | 'deny' }>;
