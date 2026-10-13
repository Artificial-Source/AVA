import type { PermissionView, QuestionView, RequestState, RunView, ToolView } from '../chat/contracts';
import type { BranchPoint, FinderItem, ListState, ModelChoice, ResourceInspections } from './contracts';
import { modelChoices } from './screenExamples';
import { toolChangeCases } from '../catalog/examples';
export { defaultPreferences, modelChoices, providers } from './screenExamples';

// This is the canonical review index. Cases are explicit, manual fixtures, never timers or remote outcomes.
export const reviewFamilies = {
  settings: { label: 'Settings', cases: ['General', 'Appearance', 'Models', 'Providers', 'Shortcuts', 'Conversation behavior', 'Notifications & sounds', 'Context management', 'Permissions & trust', 'Subagents', 'Skills', 'Prompts', 'Plugins', 'MCP', 'LSP', 'About', 'Search / no match', 'Changed preferences', 'Rules / loading', 'Rules / empty', 'Rules / error', 'Rules / denied', 'Rules / stale', 'Skills / error', 'Prompts / error', 'Plugins / error', 'MCP / error', 'LSP / error'] },
  models: { label: 'Model selector', cases: ['ready', 'loading', 'empty', 'no-match', 'error', 'stale', 'unavailable', 'busy', 'unknown', 'malformed'] },
  providers: { label: 'Providers & connect', cases: ['directory', 'method', 'key', 'browser', 'device', 'waiting', 'success', 'error', 'canceled', 'disconnect', 'externally-managed', 'directory-error'] },
  sessions: { label: 'Session finder', cases: ['ready', 'loading', 'empty', 'no-match', 'error', 'disabled', 'unknown'] },
  commands: { label: 'Command palette', cases: ['ready', 'loading', 'empty', 'no-match', 'error', 'disabled', 'unknown'] },
  rename: { label: 'Rename chat', cases: ['ready', 'empty', 'too-long', 'busy', 'stale', 'error'] },
  archive: { label: 'Archive preview', cases: ['ready', 'busy', 'stale', 'error', 'unavailable'] },
  branch: { label: 'Branch preview', cases: ['ready', 'busy', 'stale', 'error', 'empty'] },
  workspace: { label: 'Workspace & recovery', cases: ['choice', 'opening', 'ready', 'offline', 'error', 'unavailable', 'canceled'] },
  message: { label: 'Message projection', cases: ['assistant', 'user', 'failed', 'canceled', 'empty', 'unknown'] },
  tool: { label: 'Tool output', cases: ['queued', 'running', 'completed', 'failed', 'canceled', 'truncated', 'empty', ...Object.keys(toolChangeCases)] },
  permission: { label: 'Permission request', cases: ['once', 'session', 'pending', 'submitted', 'error', 'stale', 'closed', 'invalid', 'duplicate'] },
  question: { label: 'Question', cases: ['single', 'multiple', 'custom', 'secret', 'pending', 'submitted', 'error', 'stale', 'closed', 'invalid', 'duplicate'] },
  run: { label: 'Run notices', cases: ['error', 'retry', 'cancel-requested', 'canceled', 'completed', 'unknown'] },
} as const;
export const inspectionErrorSections: Readonly<Record<string, keyof ResourceInspections>> = { 'Skills / error': 'skills', 'Prompts / error': 'prompts', 'Plugins / error': 'plugins', 'MCP / error': 'mcp', 'LSP / error': 'lsp' };
export type ReviewFamily = keyof typeof reviewFamilies;
export const familyIds = Object.keys(reviewFamilies) as ReviewFamily[];
export function isFamily(value: string): value is ReviewFamily { return Object.hasOwn(reviewFamilies, value); }
export function isScenario(family: ReviewFamily, value: string) { return (reviewFamilies[family].cases as readonly string[]).includes(value); }

export function modelCase(scenario: string): readonly ModelChoice[] {
  if (scenario === 'empty' || scenario === 'loading') return [];
  if (scenario === 'malformed') return [...modelChoices, modelChoices[0]];
  if (scenario === 'unknown') return [...modelChoices, { id: 'unbound-example', label: 'Unbound model', providerId: 'example', providerLabel: 'Example provider', capabilities: [], reasoning: [], disabledReason: 'No bound model profile. This row cannot be selected.' }];
  if (scenario === 'unavailable') return modelChoices.map((choice) => ({ ...choice, disabledReason: 'Provider unavailable in this fixture.' }));
  return modelChoices;
}
export function listState(scenario: string): ListState {
  return ['loading', 'empty', 'error', 'stale', 'unavailable', 'busy', 'disabled'].includes(scenario) ? scenario as ListState : 'ready';
}
export const projects = [{ id: 'atlas', label: 'Atlas' }, { id: 'notebook', label: 'Notebook' }] as const;
export const sessions: readonly FinderItem[] = [
  { id: 'filters', label: 'Refine search filters', detail: 'Atlas · current chat · example', projectId: 'atlas' },
  { id: 'keyboard', label: 'Keyboard navigation', detail: 'Atlas · branch · example', projectId: 'atlas' },
  { id: 'notes', label: 'Organize reading notes', detail: 'Notebook · archived example', projectId: 'notebook' },
  { id: 'unavailable', label: 'Unavailable chat', detail: 'Notebook · status unknown', projectId: 'notebook', disabledReason: 'This example has no readable session view.' },
];
export const commands: readonly FinderItem[] = [
  { id: 'settings', command: 'settings', label: 'Open settings', detail: 'Review preferences and read-only scopes' },
  { id: 'models', command: 'models', label: 'Choose model', detail: 'Open the shared fictional model selector' },
  { id: 'providers', command: 'providers', label: 'Connect a provider', detail: 'Open the provider directory' },
  { id: 'sessions', command: 'sessions', label: 'Find a session', detail: 'Search supplied example chats' },
  { id: 'workspace', command: 'workspace', label: 'Choose workspace', detail: 'Open workspace fixtures, not a folder picker' },
  { id: 'export', label: 'Export session', detail: 'Unavailable', disabledReason: 'File export is not implemented in this browser review.' },
];
export const branchPoints: readonly BranchPoint[] = [{ id: 'request', label: '“Make the search filters easier to scan.”' }, { id: 'follow-up', label: '“Keep keyboard navigation predictable.”' }];
export const workspaceChoices = [{ id: 'atlas', label: 'Atlas', detail: 'Current example workspace · no directory access' }, { id: 'notebook', label: 'Notebook', detail: 'Another fictional workspace · no process launch' }] as const;
function requestState(scenario: string): RequestState {
  if (scenario === 'duplicate') return 'submitted';
  return ['pending', 'submitted', 'error', 'stale', 'closed'].includes(scenario) ? scenario as RequestState : 'ready';
}
export function permissionCase(scenario: string, version: number): PermissionView {
  return { kind: 'permission', id: 'review-permission', version: scenario === 'invalid' ? -1 : version, state: requestState(scenario), title: 'Read the example note?',
    summary: 'A supplied request to inspect an invented workspace note. No file is accessed.', target: 'Example note', sessionAllowed: scenario === 'session', decisions: scenario === 'session' ? ['allow', 'allow_session', 'deny'] : ['allow', 'deny'], error: 'Example request failed. A revised request is required.' };
}
export function questionCase(scenario: string, version: number): QuestionView {
  return { kind: 'question', id: 'review-question', version, state: requestState(scenario), title: 'How should the result be presented?', description: scenario === 'secret' ? 'Masked demo answer only. Never enter a real secret; input clears on submit or dismissal.' : 'Choose a supplied answer. This does not send a reply to a running agent.',
    options: scenario === 'secret' ? [] : scenario === 'invalid' ? [{ id: 'same', label: 'Duplicate A' }, { id: 'same', label: 'Duplicate B' }] : [{ id: 'brief', label: 'Brief summary' }, { id: 'steps', label: 'Numbered steps' }, { id: 'example', label: 'Include an example' }],
    selection: scenario === 'multiple' ? 'multiple' : 'single', required: true, searchable: scenario === 'multiple', placement: 'inline', custom: scenario === 'secret' ? { kind: 'secret', label: 'Dummy secret answer' } : scenario === 'custom' ? { kind: 'text', label: 'Custom example answer' } : { kind: 'none' }, error: 'Example question failed. Nothing was answered.' };
}
export function runCase(scenario: string): RunView {
  if (scenario === 'retry') return { state: 'retrying', attempt: 2, cause: 'Supplied retry example · no retry has been scheduled.' };
  if (scenario === 'cancel-requested' || scenario === 'canceled' || scenario === 'completed') return { state: scenario };
  return { state: 'error', code: scenario === 'unknown' ? 'unrecognized-example' : 'unavailable' };
}
export function toolCase(scenario: string): readonly ToolView[] {
  if (scenario === 'empty') return [];
  if (Object.hasOwn(toolChangeCases, scenario)) return toolChangeCases[scenario];
  const state = ['queued', 'running', 'completed', 'failed', 'canceled'].includes(scenario) ? scenario as ToolView['state'] : 'completed';
  return [{ id: 'example-tool', action: 'Inspect example note', detail: 'Read-only supplied projection', state,
    ...(state === 'queued' || state === 'running' ? {} : { output: { text: state === 'failed' ? 'Example output unavailable.' : 'Found two headings in the invented note.\n<example>Text is escaped, not executed.</example>', truncated: scenario === 'truncated' } }) }];
}
