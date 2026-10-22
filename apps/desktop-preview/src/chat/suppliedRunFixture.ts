import type { ToolView } from './contracts';
import type { RunEvent, RunIdentity } from '../state/suppliedRun';

export type PreviewExample = 'completion' | 'failure' | 'burst';
export function suppliedSnapshots(identity: RunIdentity, example: PreviewExample): readonly (readonly RunEvent[])[] {
  let sequence = 0;
  const event = (kind: RunEvent['kind'], fields = {}): RunEvent => ({ ...identity, sequence: ++sequence, kind, ...fields });
  const read: ToolView = { id: 'read', action: 'Read', detail: 'Saved filter state', resource: { kind: 'file', path: 'src/search-state.ts' }, state: 'queued' };
  const edit: ToolView = { id: 'edit', action: 'Update', detail: 'Restore saved filters', resource: { kind: 'file', path: 'src/search-state.ts' }, state: 'queued' };
  const check: ToolView = { id: 'check', action: 'Check', detail: 'Navigation test: return from a result', state: 'queued' };
  const change: ToolView = { ...edit, state: 'completed', changes: [{ id: 'restore', filename: 'src/search-state.ts', operation: 'modified', additions: 2, deletions: 1, diff: { state: 'text', text: '@@ restore filters @@\n-const filters = defaults;\n+const filters = savedFilters ?? defaults;\n+restoreSearch(filters);', truncated: false } }] };
  const snapshots = [
    [event('text', { delta: 'I’ll trace how search filters survive a return from a result.\n\n' }), event('tool', { tool: read }), event('tool', { tool: edit }), event('tool', { tool: check })],
    [event('tool', { tool: { ...read, state: 'running' } }), event('output', { toolId: 'read', delta: 'export function restoreSearch(filters) {\n', truncated: false })],
    [event('output', { toolId: 'read', delta: '  return { ...filters, page: 1 };\n}\n', truncated: false }), event('tool', { tool: { ...read, state: 'completed' } }), event('text', { delta: 'The saved filter value is restored before the result list is shown.\n\n```ts\nconst filters = savedFilters' }), event('tool', { tool: { ...edit, state: 'running' } })],
    [event('text', { delta: ' ?? defaults;\nrestoreSearch(filters);\n```\n\n' }), event('output', { toolId: 'edit', delta: 'Restored saved filters before the result list renders.\n', truncated: false }), event('tool', { tool: change }), event('tool', { tool: { ...check, state: 'running' } })],
    [event('output', { toolId: 'check', delta: 'Running navigation test: return from a result\nFilters match the saved selection.\n', truncated: false }), event('text', { delta: 'The change is in **search-state.ts**; the navigation test is checking it now. ' })],
  ];
  if (example === 'burst') {
    const burst = [
      ...Array.from({ length: 1000 }, (_, index) => event('text', { delta: index === 0 ? '\n\nReplaying the test across the full result list:\n' : index % 20 === 0 ? '\n' : '· ' })),
      ...Array.from({ length: 600 }, (_, index) => event('output', { toolId: 'check', delta: `PASS return from result ${index + 1}: query, sort order and tags match the saved selection\n`, truncated: false })),
    ];
    snapshots.push(burst.slice(0, 1050), burst.slice(1050));
  }
  if (example === 'failure') snapshots.push([event('output', { toolId: 'check', delta: 'Navigation test: no browser-history result to compare against.\n', truncated: false }), event('tool', { tool: { ...check, state: 'failed' } }), event('error', { error: 'The navigation test couldn’t read a browser-history result, so the restored filters aren’t verified yet.' })]);
  else snapshots.push([event('tool', { tool: { ...check, state: 'completed' } }), event('text', { delta: '\n\n**Saved filters now survive a return from a result.** Browser back/forward behavior still needs a manual check.' }), event('complete')]);
  return snapshots;
}
