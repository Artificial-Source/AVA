import type { ToolChangeView } from './contracts';

// Whole projection or nothing: one malformed or duplicate entry rejects every change in the record.
export function validToolChanges(value: unknown): value is readonly ToolChangeView[] {
  if (!Array.isArray(value)) return false;
  const ids = new Set<string>(); const files = new Set<string>();
  for (const change of value) {
    if (!change || typeof change.id !== 'string' || !change.id.trim() || ids.has(change.id) || typeof change.filename !== 'string' || !change.filename.trim() || files.has(change.filename) || !['created', 'modified', 'deleted'].includes(change.operation)) return false;
    if ([change.additions, change.deletions].some((count) => count !== undefined && (typeof count !== 'number' || !Number.isSafeInteger(count) || count < 0))) return false;
    const diff = change.diff;
    if (!diff || !['text', 'binary', 'unavailable'].includes(diff.state) || diff.state === 'text' && (typeof diff.text !== 'string' || typeof diff.truncated !== 'boolean')) return false;
    ids.add(change.id); files.add(change.filename);
  }
  return true;
}

export type DiffLine = Readonly<{ kind: 'add' | 'del' | 'context' | 'hunk' | 'meta'; text: string }>;

// Unified diff text only. File headers count as metadata until the first hunk; after that a
// leading +/- always marks a changed line, even when the code itself starts with -- or ++.
export function diffLines(text: string): DiffLine[] {
  let inHunk = false;
  return text.split('\n').map((line) => {
    if (line.startsWith('@@')) { inHunk = true; return { kind: 'hunk', text: line }; }
    if (line.startsWith('\\') || !inHunk && (line.startsWith('+++') || line.startsWith('---') || line.startsWith('diff ') || line.startsWith('index '))) return { kind: 'meta', text: line };
    if (line.startsWith('+')) return { kind: 'add', text: line };
    if (line.startsWith('-')) return { kind: 'del', text: line };
    return { kind: 'context', text: line };
  });
}

export function diffCounts(text: string) {
  let added = 0; let removed = 0;
  for (const line of diffLines(text)) {
    if (line.kind === 'add') added++;
    else if (line.kind === 'del') removed++;
  }
  return { added, removed };
}

// A before/after excerpt as one hunk, so excerpts and tool diffs share a single format.
export function excerptDiff(before: string, after: string) {
  const removed = before.split('\n'); const added = after.split('\n');
  const text = [`@@ -1,${removed.length} +1,${added.length} @@`, ...removed.map((line) => `-${line}`), ...added.map((line) => `+${line}`)].join('\n');
  return { state: 'text', text, truncated: false } as const;
}

// Paint bound shared by every diff view: at most 200 lines or 20,000 characters.
export function boundedDiffText(text: string) {
  return text.slice(0, 20000).split('\n').slice(0, 200).join('\n');
}
