import type { PermissionView, QuestionView, RequestState, RunView, ToolView } from '../chat/contracts';
import type { BranchPoint, FinderItem, ListState, ModelChoice, ResourceInspections } from './contracts';
import { modelChoices } from './screenExamples';
import { shortcutHint, type ShortcutAction, type ShortcutPreferences } from './shortcuts';
import { defaultBindings } from './shortcutBindings';
import { toolChangeCases } from '../catalog/examples';
export { defaultPreferences, modelChoices, providers } from './screenExamples';

// This is the canonical review index. Cases are explicit, manual fixtures, never timers or remote outcomes.
export const reviewFamilies = {
  settings: { label: 'Settings', cases: ['General', 'Appearance', 'Shortcuts', 'Notifications & sounds', 'Models & providers', 'Conversation behavior', 'Context management', 'Permissions & trust', 'Subagents', 'Extensions', 'About', 'Search / no match', 'Changed preferences', 'Rules / loading', 'Rules / empty', 'Rules / error', 'Rules / denied', 'Rules / stale', 'Skills / error', 'Prompts / error', 'Plugins / error', 'MCP / error', 'LSP / error'] },
  models: { label: 'Model selector', cases: ['ready', 'loading', 'empty', 'no-match', 'error', 'stale', 'unavailable', 'busy', 'unknown', 'malformed'] },
  providers: { label: 'Providers & connect', cases: ['directory', 'method', 'key', 'browser', 'device', 'waiting', 'success', 'error', 'canceled', 'disconnect', 'externally-managed', 'directory-error'] },
  sessions: { label: 'Find a chat', cases: ['ready', 'loading', 'empty', 'no-match', 'error', 'disabled', 'unknown'] },
  commands: { label: 'Command palette', cases: ['ready', 'loading', 'empty', 'no-match', 'error', 'disabled', 'unknown'] },
  rename: { label: 'Rename chat', cases: ['ready', 'empty', 'too-long', 'busy', 'stale', 'error'] },
  archive: { label: 'Archive chat', cases: ['ready', 'busy', 'stale', 'error', 'unavailable'] },
  branch: { label: 'Branch from a message', cases: ['ready', 'busy', 'stale', 'error', 'empty'] },
  workspace: { label: 'Open a project', cases: ['choice', 'opening', 'ready', 'offline', 'error', 'unavailable', 'canceled'] },
  message: { label: 'Message details', cases: ['assistant', 'user', 'failed', 'canceled', 'empty', 'unknown'] },
  tool: { label: 'Tool details', cases: ['queued', 'running', 'completed', 'failed', 'canceled', 'truncated', 'empty', ...Object.keys(toolChangeCases)] },
  permission: { label: 'Permission request', cases: ['once', 'session', 'pending', 'submitted', 'error', 'stale', 'closed', 'invalid', 'duplicate'] },
  question: { label: 'Question', cases: ['single', 'multiple', 'custom', 'secret', 'pending', 'submitted', 'error', 'stale', 'closed', 'invalid', 'duplicate'] },
  run: { label: 'Run status', cases: ['error', 'retry', 'running', 'cancel-requested', 'canceled', 'completed', 'unknown'] },
} as const;
// Every resource inspection lives on the Extensions page; each case fails one area.
export const inspectionErrorSections: Readonly<Record<string, keyof ResourceInspections>> = { 'Skills / error': 'skills', 'Prompts / error': 'prompts', 'Plugins / error': 'plugins', 'MCP / error': 'mcp', 'LSP / error': 'lsp' };
export type ReviewFamily = keyof typeof reviewFamilies;
export const familyIds = Object.keys(reviewFamilies) as ReviewFamily[];
export function isFamily(value: string): value is ReviewFamily { return Object.hasOwn(reviewFamilies, value); }
export function isScenario(family: ReviewFamily, value: string) { return (reviewFamilies[family].cases as readonly string[]).includes(value); }

export function modelCase(scenario: string): readonly ModelChoice[] {
  if (scenario === 'empty' || scenario === 'loading') return [];
  if (scenario === 'malformed') return [...modelChoices, modelChoices[0]];
  if (scenario === 'unknown') return [...modelChoices, { id: 'unbound-example', label: 'Unlisted model', providerId: 'self-hosted', providerLabel: 'Self-hosted', capabilities: [], reasoning: [], disabledReason: 'No model profile, so this can’t be selected.' }];
  if (scenario === 'unavailable') return modelChoices.map((choice) => ({ ...choice, disabledReason: 'Provider not connected.' }));
  return modelChoices;
}
export function listState(scenario: string): ListState {
  return ['loading', 'empty', 'error', 'stale', 'unavailable', 'busy', 'disabled'].includes(scenario) ? scenario as ListState : 'ready';
}
export const projects = [{ id: 'atlas', label: 'Atlas' }, { id: 'notebook', label: 'Notebook' }] as const;
// `searchText` stands in for each chat's messages, so Find a chat can show a matched snippet.
export const sessions: readonly FinderItem[] = [
  { id: 'filters', label: 'Refine search filters', detail: 'Grouping and keyboard order', projectId: 'atlas', status: 'working', current: true, searchText: 'Make the search filters easier to scan. I’d group the filters by purpose and keep the keyboard order.' },
  { id: 'keyboard', label: 'Keyboard navigation', detail: 'Branched from Refine search filters', projectId: 'atlas', status: 'approval', searchText: 'Arrow keys should move between results without leaving the search box. Escape clears the query first.' },
  { id: 'notes', label: 'Organize reading notes', detail: 'Highlights grouped by book', projectId: 'notebook', status: 'completed', archived: true, searchText: 'Group highlights by book, then by chapter. Dates follow the reader’s timezone, newest first.' },
  { id: 'release', label: 'Draft release notes', detail: 'Last opened in March', projectId: 'notebook', status: 'unavailable', disabledReason: 'This chat can’t be opened right now.', searchText: 'List the fixes since the March release and link each one to its issue.' },
];
// The palette lists only actions the review can carry out. Shortcut chips follow
// the user's current Settings → Shortcuts bindings.
const commandList: readonly FinderItem[] = [
  { id: 'newChat', command: 'newChat', label: 'New chat', detail: 'Uses your default model', group: 'Chat' },
  { id: 'models', command: 'models', label: 'Choose model', detail: 'For this chat', group: 'Chat' },
  { id: 'rename', command: 'rename', label: 'Rename chat…', detail: '', group: 'Chat' },
  { id: 'branch', command: 'branch', label: 'Branch from a message…', detail: '', group: 'Chat' },
  { id: 'archive', command: 'archive', label: 'Archive chat…', detail: '', group: 'Chat' },
  { id: 'sessions', command: 'sessions', label: 'Find a chat', detail: 'Search every project', group: 'Go to' },
  { id: 'workspace', command: 'workspace', label: 'Open a project…', detail: '', group: 'Go to' },
  { id: 'settings', command: 'settings', label: 'Open settings', detail: '', group: 'App' },
  { id: 'shortcuts', command: 'shortcuts', label: 'Keyboard shortcuts', detail: 'Settings → Shortcuts', group: 'App' },
  { id: 'providers', command: 'providers', label: 'Connect a provider', detail: 'API key or sign-in', group: 'App' },
];
export function commandItems(preferences?: ShortcutPreferences): readonly FinderItem[] {
  return commandList.map((item) => {
    const shortcut = item.command && Object.hasOwn(defaultBindings, item.command) ? shortcutHint(preferences, item.command as ShortcutAction)?.label : undefined;
    return shortcut ? { ...item, shortcut } : item;
  });
}
// Oldest first, as they appear in the chat.
export const branchPoints: readonly BranchPoint[] = [
  { id: 'request', label: 'Make the search filters easier to scan.' },
  { id: 'labels', label: 'Group them by purpose and keep the labels short.' },
  { id: 'follow-up', label: 'Keep keyboard navigation predictable.' },
];
export const workspaceChoices = [{ id: 'atlas', label: 'Atlas', detail: '~/Projects/atlas' }, { id: 'notebook', label: 'Notebook', detail: '~/Documents/notebook' }] as const;
function requestState(scenario: string): RequestState {
  if (scenario === 'duplicate') return 'submitted';
  return ['pending', 'submitted', 'error', 'stale', 'closed'].includes(scenario) ? scenario as RequestState : 'ready';
}
export function permissionCase(scenario: string, version: number): PermissionView {
  return { kind: 'permission', id: 'review-permission', version: scenario === 'invalid' ? -1 : version, state: requestState(scenario),
    title: 'Run the search filter tests?', summary: 'AVA wants to run a command in Atlas.', requester: 'AVA',
    action: 'npm test -- search-filters', target: '~/Projects/atlas', reason: 'To check that the regrouped filters didn’t break existing tests.',
    scopes: { allow: 'Only this command', allow_session: 'Test commands in this chat until AVA restarts', deny: 'AVA continues without running it' },
    sessionAllowed: scenario === 'session', decisions: scenario === 'session' ? ['allow', 'allow_session', 'deny'] : ['allow', 'deny'], error: 'This request couldn’t be loaded. Ask AVA to try again.' };
}
export function questionCase(scenario: string, version: number): QuestionView {
  const secret = scenario === 'secret';
  return { kind: 'question', id: 'review-question', version, state: requestState(scenario),
    title: secret ? 'Enter the staging access token' : 'How should the result be presented?',
    description: secret ? 'Hidden as you type. It isn’t saved to the chat.' : 'AVA will shape the summary around your choice.',
    options: secret ? [] : scenario === 'invalid' ? [{ id: 'same', label: 'Duplicate A' }, { id: 'same', label: 'Duplicate B' }] : [{ id: 'brief', label: 'Brief summary' }, { id: 'steps', label: 'Numbered steps' }, { id: 'example', label: 'Include an example' }],
    selection: scenario === 'multiple' ? 'multiple' : 'single', required: true, searchable: scenario === 'multiple', placement: 'inline',
    custom: secret ? { kind: 'secret', label: 'Access token' } : scenario === 'custom' ? { kind: 'text', label: 'Describe the format' } : { kind: 'none' },
    error: 'This question couldn’t be loaded. Nothing was answered.' };
}
export function runCase(scenario: string): RunView {
  if (scenario === 'retry') return { state: 'retrying', attempt: 2, cause: 'The connection to the model dropped.' };
  if (scenario === 'running' || scenario === 'cancel-requested' || scenario === 'canceled' || scenario === 'completed') return { state: scenario };
  return { state: 'error', code: scenario === 'unknown' ? 'future_code' : 'unavailable' };
}
export function toolCase(scenario: string): readonly ToolView[] {
  if (scenario === 'empty') return [];
  if (Object.hasOwn(toolChangeCases, scenario)) return toolChangeCases[scenario];
  const state = ['queued', 'running', 'completed', 'failed', 'canceled'].includes(scenario) ? scenario as ToolView['state'] : 'completed';
  return [{ id: 'review-tool', action: 'Read notes/search-filters.md', detail: 'notes/search-filters.md', state,
    ...(state === 'queued' || state === 'running' ? {} : { output: { text: state === 'failed' ? 'The file couldn’t be read.' : 'Found two headings: Filters, Keyboard.\n<h2>Markup in output stays text.</h2>', truncated: scenario === 'truncated' } }) }];
}
