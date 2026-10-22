import type { ToolView } from './contracts';
import type { RunEvent, RunIdentity } from '../state/suppliedRun';

export type PreviewExample = 'completion' | 'failure' | 'burst';
export function suppliedSnapshots(identity: RunIdentity, example: PreviewExample): readonly (readonly RunEvent[])[] {
  let sequence = 0;
  const event = (kind: RunEvent['kind'], fields = {}): RunEvent => ({ ...identity, sequence: ++sequence, kind, ...fields });
  const read: ToolView = { id: 'read', action: 'Read', detail: 'Supplied search-state source excerpt', resource: { kind: 'file', path: 'src/search-state.ts' }, state: 'queued' };
  const edit: ToolView = { id: 'edit', action: 'Update', detail: 'Supplied filter restoration change', resource: { kind: 'file', path: 'src/search-state.ts' }, state: 'queued' };
  const check: ToolView = { id: 'check', action: 'Check', detail: 'Supplied navigation fixture', state: 'queued' };
  const change: ToolView = { ...edit, state: 'completed', changes: [{ id: 'restore', filename: 'src/search-state.ts', operation: 'modified', additions: 2, deletions: 1, diff: { state: 'text', text: '@@ restore filters @@\n-const filters = defaults;\n+const filters = savedFilters ?? defaults;\n+restoreSearch(filters);', truncated: false } }] };
  const snapshots = [
    [event('text', { delta: 'The supplied example traces how search filters survive a return from a result.\n\n' }), event('tool', { tool: read }), event('tool', { tool: edit }), event('tool', { tool: check })],
    [event('tool', { tool: { ...read, state: 'running' } }), event('output', { toolId: 'read', delta: 'export function restoreSearch(filters) {\n', truncated: false })],
    [event('output', { toolId: 'read', delta: '  return { ...filters, page: 1 };\n}\n', truncated: false }), event('tool', { tool: { ...read, state: 'completed' } }), event('text', { delta: 'The saved filter value is restored before the result list is shown.\n\n```ts\nconst filters = savedFilters' }), event('tool', { tool: { ...edit, state: 'running' } })],
    [event('text', { delta: ' ?? defaults;\nrestoreSearch(filters);\n```\n\n' }), event('output', { toolId: 'edit', delta: 'Filter restoration excerpt supplied.\n', truncated: false }), event('tool', { tool: change }), event('tool', { tool: { ...check, state: 'running' } })],
    [event('output', { toolId: 'check', delta: 'Fixture: return from a result\nFilters match the saved selection.\n', truncated: false }), event('text', { delta: 'The inline change and check output are supplied facts, not changes to your files. ' })],
  ];
  if (example === 'burst') {
    const burst = [
      ...Array.from({ length: 1000 }, (_, index) => event('text', { delta: index === 0 ? '\n\nBurst delivery sample:\n' : index % 20 === 0 ? '\n' : '· ' })),
      ...Array.from({ length: 600 }, (_, index) => event('output', { toolId: 'check', delta: `Supplied row ${index + 1}: the saved filter selection remains available in this invented return-path example.\n`, truncated: false })),
    ];
    snapshots.push(burst.slice(0, 1050), burst.slice(1050));
  }
  if (example === 'failure') snapshots.push([event('output', { toolId: 'check', delta: 'The supplied navigation fixture has no browser-history result.\n', truncated: false }), event('tool', { tool: { ...check, state: 'failed' } }), event('error', { error: 'Supplied example failure: browser-history result unavailable. Retry preview starts a new local example.' })]);
  else snapshots.push([event('tool', { tool: { ...check, state: 'completed' } }), event('text', { delta: '\n\n**Example complete.** Browser back/forward behavior still needs a real manual check.' }), event('complete')]);
  return snapshots;
}
