import type { ChangeEntryView } from '../chat/contracts';
import { diffCounts, excerptDiff, validToolChanges } from '../chat/toolChanges';
import type { Chat } from './types';

export type LedgerEntry = ChangeEntryView & Readonly<{ added: number; removed: number; source: 'tool' | 'fixture' }>;

const statuses = { created: 'added', modified: 'modified', deleted: 'deleted' } as const;

// One spelling per file: surrounding space, `./` prefixes and repeated slashes never make a second
// entry. Anything else (including a different directory) is a different file.
export function canonicalPath(path: string) {
  return path.trim().replace(/\/{2,}/g, '/').replace(/^(?:\.\/)+/, '');
}

// Pure inventory for Changes: one entry per canonical path in first-seen order. Tool diffs from the
// transcript replace a fixture excerpt for the same path, and a later tool change replaces an
// earlier one, so Changes always shows the newest diff the transcript shows.
export function chatChanges(chat: Chat): readonly LedgerEntry[] {
  const entries = new Map<string, LedgerEntry>();
  for (const change of chat.changes) {
    const path = canonicalPath(change.filename);
    if (!path) continue;
    const diff = excerptDiff(change.before, change.after);
    entries.set(path, { id: `fixture:${path}`, path, status: 'modified', ...diffCounts(diff.text), description: change.description, diff, source: 'fixture' });
  }
  for (const message of chat.messages) {
    if (message.role !== 'assistant') continue;
    for (const record of message.activity ?? []) {
      // Invalid change metadata fails closed here exactly as it does in the transcript.
      if (record.changes === undefined || !validToolChanges(record.changes)) continue;
      for (const change of record.changes) {
        const path = canonicalPath(change.filename);
        if (!path) continue;
        const counts = change.diff.state === 'text' ? diffCounts(change.diff.text) : { added: 0, removed: 0 };
        entries.set(path, {
          id: `tool:${message.id}:${record.id}:${change.id}`,
          path,
          status: statuses[change.operation],
          added: change.additions ?? counts.added,
          removed: change.deletions ?? counts.removed,
          description: entries.get(path)?.description,
          diff: change.diff,
          source: 'tool',
        });
      }
    }
  }
  return [...entries.values()];
}
