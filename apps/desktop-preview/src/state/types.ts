import type { PermissionDecision, PermissionView, ToolView, TurnAttribution, TurnState } from '../chat/contracts';
import type { RunEvent, RunIdentity, SuppliedRun } from './suppliedRun';

export type ChatStatus = 'idle' | 'working' | 'completed' | 'approval' | 'stopped' | 'failed';
export type ModelId = 'aster' | 'aster-mini' | 'orion';
export type ChatMode = 'build' | 'plan';
export type InspectorModule = 'agents' | 'changes' | 'context';
export type ChildStatus = 'working' | 'completed' | 'failed' | 'unavailable';
export type ContextId = 'search-file' | 'result-image' | 'navigation-notes';

export interface ContextReference {
  id: ContextId;
  kind: 'file' | 'image' | 'context';
  label: string;
}

// Fixed, committed examples only. No tool output, reasoning, or partial stream.
export type ChildMessage =
  | { id: string; role: 'user'; text: string }
  | { id: string; role: 'assistant'; markdown: string; readonly attribution?: TurnAttribution };

export interface ChildAgent {
  id: string;
  parentChatId: string;
  title: string;
  status: ChildStatus;
  model: ModelId;
  timing: string;
  messages: ChildMessage[];
}

export interface Project {
  id: string;
  name: string;
}

export type ToolRecord = ToolView;

export interface CodeExample {
  filename: string;
  code: string;
}

export type Message =
  | { id: string; role: 'user'; text: string; origin: 'fixture' | 'local'; readonly selectedModel?: Readonly<{ id: ModelId; label: string }> }
  | {
      id: string;
      role: 'assistant';
      markdown: string;
      lifecycle?: TurnState;
      readonly attribution?: TurnAttribution;
      activity?: ToolRecord[];
      code?: CodeExample;
      note?: string;
      readonly permission?: PermissionView;
      // The user's answer to `permission`, kept with the turn so the receipt survives remounts.
      readonly permissionDecision?: PermissionDecision;
    };

export interface MockChange {
  filename: string;
  description: string;
  before: string;
  after: string;
}

export interface Chat {
  id: string;
  projectId: string;
  title: string;
  nameFromFirstMessage?: boolean;
  // Hidden from the sidebar until restored; the conversation itself is kept.
  archived?: boolean;
  status: ChatStatus;
  draft: string;
  model: ModelId;
  mode: ChatMode;
  contextIds: ContextId[];
  composerExpanded: boolean;
  messages: Message[];
  changes: MockChange[];
}

export interface PreviewState {
  projects: Project[];
  chats: Chat[];
  activeChatId: string;
  pinnedChatIds: string[];
  recentChatIds: string[];
  collapsedProjectIds: string[];
  childAgents: ChildAgent[];
  inspectedChildId: string | null;
  inspector: InspectorModule | null;
  lastInspectorModule: InspectorModule;
  opaque: boolean;
  nextChatNumber: number;
  nextMessageNumber: number;
  announcement: string;
  // Undo target for the most recent archive; cleared by the next accepted action.
  archiveNotice?: ArchiveNotice;
  suppliedRun?: SuppliedRun;
}

// Undo is offered only until the next accepted action, so the pre-archive view can be restored exactly.
export type ArchiveNotice = Readonly<{ chatId: string; title: string; activeChatId: string; pinnedChatIds: readonly string[]; recentChatIds: readonly string[] }>;

export type PreviewAction =
  | { type: 'startSuppliedRun'; identity: RunIdentity }
  | { type: 'receiveSuppliedRun'; events: readonly RunEvent[] }
  | { type: 'stopSuppliedRun'; identity: RunIdentity }
  | { type: 'viewChat'; chatId: string }
  | { type: 'toggleProject'; projectId: string }
  | { type: 'pinChat'; chatId: string }
  | { type: 'unpinChat'; chatId: string }
  | { type: 'removeRecentChat'; chatId: string }
  | { type: 'newChat'; model?: ModelId; mode?: ChatMode; projectId?: string; titleFromFirstMessage?: boolean; inspector?: 'remember' | 'closed' | InspectorModule }
  | { type: 'renameChat'; chatId: string; title: string }
  | { type: 'archiveChat'; chatId: string }
  | { type: 'unarchiveChat'; chatId: string }
  | { type: 'dismissArchiveNotice' }
  | { type: 'resolvePermission'; chatId: string; messageId: string; decision: PermissionDecision }
  | { type: 'branchChat'; chatId: string; messageId: string }
  | { type: 'setDraft'; chatId: string; draft: string }
  | { type: 'setModel'; chatId: string; model: ModelId }
  | { type: 'setMode'; chatId: string; mode: ChatMode }
  | { type: 'addContext'; chatId: string; contextId: ContextId }
  | { type: 'removeContext'; chatId: string; contextId: ContextId }
  | { type: 'toggleComposerExpanded'; chatId: string }
  | { type: 'inspectChild'; childId: string }
  | { type: 'backToParent' }
  | { type: 'sendLocalMessage' }
  | { type: 'stopViewedMock' }
  | { type: 'toggleInspector'; module: InspectorModule }
  | { type: 'toggleInspectorVisibility' }
  | { type: 'showInspector'; module: InspectorModule }
  | { type: 'closeInspector' }
  | { type: 'toggleOpaque' }
  | { type: 'setOpaque'; opaque: boolean };
