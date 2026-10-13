import type { ChangeExcerpt, ComposerView, MessageTurnView, PermissionView, QuestionView, RunView, ToolChangeView, ToolView } from '../chat/contracts';

export const turns: Readonly<Record<string, MessageTurnView>> = {
  attributed: { id: 'turn', role: 'assistant', author: 'AVA', label: 'Supplied attribution example', state: 'committed', attribution: { modelLabel: 'GPT6.1Sol', thinkingLabel: 'xHigh' }, markdown: 'A display-only attribution example. No provider was called.' },
  committed: { id: 'turn', role: 'assistant', author: 'AVA', label: 'Committed assistant example', state: 'committed', markdown: 'Keep the reading column **bounded**. This is an invented response, not an executed task.', code: { filename: 'layout-example.ts', code: 'const readingWidth = 780;\n' } },
  streaming: { id: 'turn', role: 'assistant', author: 'AVA', label: 'Partial assistant example', state: 'streaming', markdown: 'A partial paragraph supplied by the host…' },
  empty: { id: 'turn', role: 'assistant', author: 'AVA', label: 'Empty partial example', state: 'streaming', markdown: '' },
  canceled: { id: 'turn', role: 'assistant', author: 'AVA', label: 'Canceled assistant example', state: 'canceled', markdown: 'This partial answer is preserved. No further text is invented.' },
  failed: { id: 'turn', role: 'assistant', author: 'AVA', label: 'Failed assistant example', state: 'failed', markdown: 'Only this supplied fragment is available.' },
  escaped: { id: 'turn', role: 'user', author: 'You', label: 'Escaped user example', state: 'committed', text: '<script>not executable</script>\nPlain user text stays plain.' },
};

export const tools: readonly ToolView[] = [
  { id: 'queued', action: 'Read example', detail: 'Queued fixture', state: 'queued' },
  { id: 'running', action: 'Inspect excerpt', detail: 'Running fixture', state: 'running', output: { text: 'A supplied partial line. No process is running.', truncated: false } },
  { id: 'completed', action: 'Summarize excerpt', detail: 'Completed fixture', state: 'completed', output: { text: '<sample>escaped output</sample>', truncated: false } },
  { id: 'failed', action: 'Check example', detail: 'Failed fixture', state: 'failed', output: { text: 'Only the supplied tail of this invented output is available.', truncated: true } },
  { id: 'canceled', action: 'Review example', detail: 'Canceled fixture', state: 'canceled' },
  { id: 'image', action: 'Inspect image', detail: 'No failure cause supplied.', state: 'failed', resource: { kind: 'image', path: 'C:\\Temp\\pi-clipboard-11223344-5566-7788-9900-aabbccddeeff.png' } },
  { id: 'unavailable', action: 'Read attachment', detail: 'No outcome supplied.', state: 'unavailable', resource: { kind: 'image', path: '/tmp/invented/11223344-5566-7788-9900-aabbccddeeff.webp' } },
];

const changedNote: ToolChangeView = { id: 'note', filename: 'example-note.txt', operation: 'modified', additions: 1, deletions: 1, diff: { state: 'text', text: '@@ -1 +1 @@\n-Old note\n+Reviewed note', truncated: false } };
function changeCase(changes?: readonly ToolChangeView[]): readonly ToolView[] {
  return [{ id: 'change-example', action: 'Update note', detail: 'Supplied file-change projection', state: 'completed', output: { text: 'Supplied tool output remains readable.', truncated: false }, changes }];
}
// Shared, explicit catalog/review projections; never joined to the main Changes inspector.
export const toolChangeCases: Readonly<Record<string, readonly ToolView[]>> = {
  'diff-modified': changeCase([changedNote]),
  'diff-created': changeCase([{ ...changedNote, operation: 'created', deletions: 0, diff: { state: 'text', text: '@@ -0,0 +1 @@\n+New note', truncated: false } }]),
  'diff-deleted': changeCase([{ ...changedNote, operation: 'deleted', additions: 0, diff: { state: 'text', text: '@@ -1 +0,0 @@\n-Old note', truncated: false } }]),
  'diff-missing': changeCase(),
  'diff-zero-counts': changeCase([{ ...changedNote, additions: 0, deletions: 0, diff: { state: 'text', text: '', truncated: false } }]),
  'diff-unknown-counts': changeCase([{ ...changedNote, additions: undefined, deletions: undefined }]),
  'diff-unavailable': changeCase([{ ...changedNote, diff: { state: 'unavailable' } }]),
  'diff-binary': changeCase([{ ...changedNote, filename: 'example-image.bin', additions: undefined, deletions: undefined, diff: { state: 'binary' } }]),
  'diff-truncated': changeCase([{ ...changedNote, additions: undefined, deletions: undefined, diff: { state: 'text', text: '@@ -1 +1 @@\n-Only the supplied tail', truncated: true } }]),
  'diff-escaped': changeCase([{ ...changedNote, filename: '<img src=x>.txt', diff: { state: 'text', text: '-<script>old</script>\n+<img src=x onerror=alert(1)>', truncated: false } }]),
  'diff-malformed': changeCase([{ ...changedNote, additions: -1 }]),
  'diff-duplicate': changeCase([changedNote, changedNote]),
};

export const permission: PermissionView = { kind: 'permission', id: 'catalog-permission', version: 1, state: 'ready', title: 'Review a proposed action', summary: 'An invented permission prompt. These choices do not grant access or run a tool.', target: 'Example workspace · display label only', decisions: ['allow', 'deny'], sessionAllowed: false };
export const question: QuestionView = { kind: 'question', id: 'catalog-question', version: 1, state: 'ready', title: 'Choose an example review focus', description: 'An invented host question. Answering records only a local UI intent.', options: [{ id: 'layout', label: 'Layout' }, { id: 'keyboard', label: 'Keyboard behavior' }, { id: 'copy', label: 'Copy and labels' }], selection: 'single', required: true, searchable: false, placement: 'inline', custom: { kind: 'none' } };
export const runs: Readonly<Record<string, RunView>> = {
  error: { state: 'error', code: 'unavailable' }, unknown: { state: 'error', code: 'future_code' },
  retrying: { state: 'retrying', attempt: 2, cause: 'Invented connection interruption. No retry is scheduled here.' },
  requested: { state: 'cancel-requested' }, canceled: { state: 'canceled' }, completed: { state: 'completed' },
};
export const composer: ComposerView = {
  title: 'Catalog draft', draft: 'Keep the next answer concise.', expanded: false, runState: 'idle',
  contextOptions: [{ id: 'note', label: 'Review notes', kind: 'context' }, { id: 'file', label: 'Layout example.ts', kind: 'file' }], selectedContextIds: [],
  modelOptions: [{ id: 'sample', label: 'Sample model' }, { id: 'compact', label: 'Compact example' }], modelId: 'sample',
  modeOptions: [{ id: 'build', label: 'Build' }, { id: 'plan', label: 'Plan' }], modeId: 'build',
};
export const changes: readonly ChangeExcerpt[] = [{ id: 'layout', filename: 'layout-example.ts', description: 'Invented excerpt, not a Git diff or authoritative change.', before: 'const readingWidth = 960;', after: 'const readingWidth = 780;' }];
