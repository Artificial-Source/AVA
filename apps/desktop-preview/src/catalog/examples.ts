import type { ChangeEntryView, ComposerView, MessageTurnView, PermissionView, QuestionView, RunView, ToolChangeView, ToolView } from '../chat/contracts';

export const turns: Readonly<Record<string, MessageTurnView>> = {
  attributed: { id: 'turn', role: 'assistant', author: 'AVA', label: 'AVA response with model', state: 'committed', attribution: { modelLabel: 'GPT6.1Sol', thinkingLabel: 'xHigh' }, markdown: 'Grouped the filters by purpose and kept the keyboard order intact.' },
  committed: { id: 'turn', role: 'assistant', author: 'AVA', label: 'AVA response', state: 'committed', markdown: 'Keep the reading column **bounded** so long answers stay easy to follow.', code: { filename: 'layout.ts', code: 'const readingWidth = 780;\n' } },
  streaming: { id: 'turn', role: 'assistant', author: 'AVA', label: 'AVA response in progress', state: 'streaming', markdown: 'Checking how the filters are grouped today…' },
  empty: { id: 'turn', role: 'assistant', author: 'AVA', label: 'AVA response, no text yet', state: 'streaming', markdown: '' },
  canceled: { id: 'turn', role: 'assistant', author: 'AVA', label: 'Stopped AVA response', state: 'canceled', markdown: 'The first two filter groups are done. The third still needs' },
  failed: { id: 'turn', role: 'assistant', author: 'AVA', label: 'Failed AVA response', state: 'failed', markdown: 'Started regrouping the filters, then the connection dropped.' },
  escaped: { id: 'turn', role: 'user', author: 'You', label: 'Your message', state: 'committed', text: '<script>alert(1)</script>\nMarkup in a message stays plain text.' },
};

export const tools: readonly ToolView[] = [
  { id: 'queued', action: 'Read', detail: 'notes/filters.md', state: 'queued' },
  { id: 'running', action: 'Search', detail: '“filter” in src/', state: 'running', output: { text: 'src/filters/groups.ts: 3 matches', truncated: false } },
  { id: 'completed', action: 'Read', detail: 'src/filters/groups.ts', state: 'completed', output: { text: '<FilterGroup label="Status">', truncated: false } },
  { id: 'failed', action: 'Run', detail: 'npm test -- filters', state: 'failed', output: { text: '2 tests failed: FilterGroup keyboard order', truncated: true } },
  { id: 'canceled', action: 'Edit', detail: 'src/filters/groups.ts', state: 'canceled' },
  { id: 'image', action: 'Inspect image', detail: 'AVA didn’t report why this failed.', state: 'failed', resource: { kind: 'image', path: 'C:\\Temp\\pi-clipboard-11223344-5566-7788-9900-aabbccddeeff.png' } },
  { id: 'unavailable', action: 'Read attachment', detail: 'No result reported.', state: 'unavailable', resource: { kind: 'image', path: '/tmp/clipboard/11223344-5566-7788-9900-aabbccddeeff.webp' } },
];

const changedNote: ToolChangeView = { id: 'note', filename: 'notes/filters.md', operation: 'modified', additions: 1, deletions: 1, diff: { state: 'text', text: '@@ -1 +1 @@\n-Old note\n+Reviewed note', truncated: false } };
function changeCase(changes?: readonly ToolChangeView[]): readonly ToolView[] {
  return [{ id: 'change-example', action: 'Update note', detail: 'notes/filters.md', state: 'completed', output: { text: 'Updated notes/filters.md.', truncated: false }, changes }];
}
// Shared catalog and screen-review cases; never joined to the main Changes inspector.
export const toolChangeCases: Readonly<Record<string, readonly ToolView[]>> = {
  'diff-modified': changeCase([changedNote]),
  'diff-created': changeCase([{ ...changedNote, operation: 'created', deletions: 0, diff: { state: 'text', text: '@@ -0,0 +1 @@\n+New note', truncated: false } }]),
  'diff-deleted': changeCase([{ ...changedNote, operation: 'deleted', additions: 0, diff: { state: 'text', text: '@@ -1 +0,0 @@\n-Old note', truncated: false } }]),
  'diff-missing': changeCase(),
  'diff-zero-counts': changeCase([{ ...changedNote, additions: 0, deletions: 0, diff: { state: 'text', text: '', truncated: false } }]),
  'diff-unknown-counts': changeCase([{ ...changedNote, additions: undefined, deletions: undefined }]),
  'diff-unavailable': changeCase([{ ...changedNote, diff: { state: 'unavailable' } }]),
  'diff-binary': changeCase([{ ...changedNote, filename: 'assets/filter-icons.bin', additions: undefined, deletions: undefined, diff: { state: 'binary' } }]),
  'diff-truncated': changeCase([{ ...changedNote, additions: undefined, deletions: undefined, diff: { state: 'text', text: '@@ -1 +1 @@\n-Only the last lines are shown', truncated: true } }]),
  'diff-escaped': changeCase([{ ...changedNote, filename: '<img src=x>.txt', diff: { state: 'text', text: '-<script>old</script>\n+<img src=x onerror=alert(1)>', truncated: false } }]),
  'diff-malformed': changeCase([{ ...changedNote, additions: -1 }]),
  'diff-duplicate': changeCase([changedNote, changedNote]),
};

export const permission: PermissionView = { kind: 'permission', id: 'catalog-permission', version: 1, state: 'ready', title: 'Edit src/filters/groups.ts?', summary: 'AVA wants to change one file in Atlas.', requester: 'AVA', action: 'Edit file', target: 'src/filters/groups.ts', reason: 'To group the filters by purpose, as you asked.', decisions: ['allow', 'deny'], sessionAllowed: false };
export const question: QuestionView = { kind: 'question', id: 'catalog-question', version: 1, state: 'ready', title: 'What should the review focus on?', description: 'AVA will start with the area you pick.', options: [{ id: 'layout', label: 'Layout' }, { id: 'keyboard', label: 'Keyboard behavior' }, { id: 'copy', label: 'Copy and labels' }], selection: 'single', required: true, searchable: false, placement: 'inline', custom: { kind: 'none' } };
export const runs: Readonly<Record<string, RunView>> = {
  error: { state: 'error', code: 'unavailable' }, unknown: { state: 'error', code: 'future_code' },
  retrying: { state: 'retrying', attempt: 2, cause: 'The connection to the model dropped.' },
  running: { state: 'running' }, requested: { state: 'cancel-requested' }, canceled: { state: 'canceled' }, completed: { state: 'completed' },
};
export const composer: ComposerView = {
  title: 'Catalog draft', draft: 'Keep the next answer concise.', expanded: false, runState: 'idle',
  contextOptions: [{ id: 'note', label: 'Review notes', kind: 'context' }, { id: 'file', label: 'layout.ts', kind: 'file' }], selectedContextIds: [],
  modelOptions: [{ id: 'aster', label: 'Aster' }, { id: 'orion', label: 'Orion' }], modelId: 'aster',
  modeOptions: [{ id: 'build', label: 'Build' }, { id: 'plan', label: 'Plan' }], modeId: 'build',
};
export const changes: readonly ChangeEntryView[] = [{ id: 'layout', path: 'src/layout.ts', status: 'modified', added: 1, removed: 1, description: 'Narrower reading width', diff: { state: 'text', truncated: false, text: '@@ -1 +1 @@\n-const readingWidth = 960;\n+const readingWidth = 780;' } }];
