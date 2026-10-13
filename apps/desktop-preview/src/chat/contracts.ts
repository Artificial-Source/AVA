import type { ContextUsageView } from './contextUsage';
import type { ToolImagePreviewId } from './toolImages';

// Experimental presentation models, not AVA transport schemas or resolver authority.
export type TurnState = 'streaming' | 'committed' | 'failed' | 'canceled';
// Supplied invocation facts only; never derived from the current composer model.
export type TurnAttribution = Readonly<{ modelLabel?: string; thinkingLabel?: string }>;
export type ToolResource =
  | Readonly<{ kind: 'file'; path: string; previewId?: never }>
  | Readonly<{ kind: 'image'; path: string; previewId?: ToolImagePreviewId }>;
export type MessageTurnView = Readonly<{
  id: string;
  label: string;
  author: string;
  state: TurnState;
}> & (
  | Readonly<{ role: 'user'; text: string }>
  | Readonly<{ role: 'assistant'; markdown: string; attribution?: TurnAttribution; code?: Readonly<{ filename: string; code: string }>; note?: string }>
);

// Explicit historical presentation only, never inferred from output or current file state.
export type ToolChangeView = Readonly<{
  id: string;
  filename: string;
  operation: 'created' | 'modified' | 'deleted';
  additions?: number;
  deletions?: number;
  diff: Readonly<{ state: 'text'; text: string; truncated: boolean }> | Readonly<{ state: 'binary' | 'unavailable' }>;
}>;
export type ToolView = Readonly<{
  id: string;
  action: string;
  detail: string;
  state: 'queued' | 'running' | 'completed' | 'failed' | 'canceled' | 'unavailable';
  resource?: ToolResource;
  output?: Readonly<{ text: string; truncated: boolean }>;
  changes?: readonly ToolChangeView[];
}>;

export type RequestIdentity = Readonly<{ id: string; version: number }>;
export type RequestState = 'ready' | 'pending' | 'submitted' | 'error' | 'stale' | 'closed';
export type PermissionDecision = 'allow' | 'allow_session' | 'deny';
export type PermissionView = RequestIdentity & Readonly<{
  kind: 'permission';
  state: RequestState;
  title: string;
  summary: string;
  target?: string;
  decisions: readonly PermissionDecision[];
  sessionAllowed: boolean;
  error?: string;
}>;
export type PermissionIntent = RequestIdentity & Readonly<{ decision: PermissionDecision }>;

export type Choice = Readonly<{ id: string; label: string }>;
export type QuestionView = RequestIdentity & Readonly<{
  kind: 'question';
  state: RequestState;
  title: string;
  description: string;
  options: readonly Choice[];
  selection: 'single' | 'multiple';
  required: boolean;
  searchable: boolean;
  placement: 'inline';
  custom: Readonly<{ kind: 'none' }> | Readonly<{ kind: 'text' | 'secret'; label: string }>;
  error?: string;
}>;
export type QuestionAnswer = Readonly<{ selectedIds: readonly string[]; customText: string }>;
export type QuestionIntent = RequestIdentity & Readonly<{ answer: QuestionAnswer }>;

export type RunView =
  | Readonly<{ state: 'error'; code: string }>
  | Readonly<{ state: 'retrying'; attempt: number; cause: string }>
  | Readonly<{ state: 'cancel-requested' | 'canceled' | 'completed' }>;

export type ReferenceOption = Choice & Readonly<{ kind: string }>;
export type ComposerView = Readonly<{
  title: string;
  draft: string;
  expanded: boolean;
  runState: 'idle' | 'running';
  contextOptions: readonly ReferenceOption[];
  selectedContextIds: readonly string[];
  modelOptions: readonly Choice[];
  modelId: string;
  modeOptions: readonly Choice[];
  modeId: string;
  contextUsage?: ContextUsageView;
}>;

export type ChangeExcerpt = Readonly<{ id: string; filename: string; description: string; before: string; after: string }>;
export type ChangeReviewState =
  | Readonly<{ state: 'loading' }>
  | Readonly<{ state: 'empty'; message: string }>
  | Readonly<{ state: 'error'; message: string }>
  | Readonly<{ state: 'ready'; changes: readonly ChangeExcerpt[] }>
  | Readonly<{ state: 'stale'; changes: readonly ChangeExcerpt[]; message: string }>;
