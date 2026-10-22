import { describe, expect, it } from 'vitest';
import type { ToolView } from '../chat/contracts';
import { canonicalPath, chatChanges } from './changeLedger';
import { createInitialState } from './fixtures';
import type { Chat } from './types';

const edit: ToolView = { id: 'edit', action: 'Update', detail: 'Restore filters', state: 'completed', changes: [{ id: 'restore', filename: 'src/search-state.ts', operation: 'modified', additions: 2, deletions: 1, diff: { state: 'text', truncated: false, text: '@@ restore filters @@\n-const filters = defaults;\n+const filters = savedFilters ?? defaults;\n+restoreSearch(filters);' } }] };
function chat(activity: ToolView[], changes: Chat['changes'] = []): Chat {
  return { ...createInitialState().chats[0], messages: [{ id: 'run', role: 'assistant', markdown: '', lifecycle: 'committed', activity }], changes };
}

describe('Changes inventory from the transcript', () => {
  it('lists a file a run edited, with the exact tool diff and counts', () => {
    expect(chatChanges(chat([edit]))).toEqual([{ id: 'tool:run:edit:restore', path: 'src/search-state.ts', status: 'modified', added: 2, removed: 1, description: undefined, diff: edit.changes![0].diff, source: 'tool' }]);
    expect(chatChanges(chat([]))).toEqual([]);
  });
  it('lets tool diffs replace same-path excerpts in first-seen order while keeping the excerpt description', () => {
    const entries = chatChanges(createInitialState().chats[0]);
    expect(entries.map(({ path, source, added, removed }) => [path, source, added, removed])).toEqual([['src/search-state.ts', 'tool', 2, 2], ['src/SearchPage.tsx', 'tool', 1, 1]]);
    expect(entries[0].description).toBe('Read the search state from URL parameters.');
  });
  it('turns a before/after excerpt into the same unified format and derives missing counts from diff lines', () => {
    const [excerpt] = chatChanges(chat([], [{ filename: 'a.ts', description: 'Rename', before: 'old\nolder', after: 'new' }]));
    expect(excerpt).toMatchObject({ path: 'a.ts', status: 'modified', added: 1, removed: 2, source: 'fixture', diff: { state: 'text', text: '@@ -1,2 +1,1 @@\n-old\n-older\n+new' } });
    const [derived] = chatChanges(chat([{ ...edit, changes: [{ ...edit.changes![0], additions: undefined, deletions: undefined, operation: 'created' }] }]));
    expect(derived).toMatchObject({ status: 'added', added: 2, removed: 1 });
  });
  it('fails a malformed or duplicate change projection closed, exactly as the transcript does', () => {
    const duplicate = { ...edit, changes: [edit.changes![0], { ...edit.changes![0], id: 'again' }] };
    expect(chatChanges(chat([duplicate]))).toEqual([]);
    expect(chatChanges(chat([{ ...edit, changes: [{ ...edit.changes![0], additions: -1 }] }]))).toEqual([]);
  });
  it('lists one file once however its path is spelled, keeping first-seen order and the newest diff', () => {
    const respelled = (filename: string, id: string): ToolView => ({ ...edit, id, changes: [{ ...edit.changes![0], filename }] });
    const entries = chatChanges(chat([respelled('./src/search-state.ts', 'first'), respelled('src//search-state.ts', 'second'), respelled(' src/search-state.ts ', 'third')], [{ filename: 'src/search-state.ts', description: 'Restore filters', before: 'a', after: 'b' }]));
    expect(entries.map(({ id, path, description }) => [id, path, description])).toEqual([['tool:run:third:restore', 'src/search-state.ts', 'Restore filters']]);
    expect(canonicalPath('././src//a.ts')).toBe('src/a.ts'); expect(canonicalPath('lib/src/a.ts')).toBe('lib/src/a.ts');
    // A different directory is a different file, never merged by name.
    expect(chatChanges(chat([respelled('a/state.ts', 'one'), respelled('b/state.ts', 'two')])).map(({ path }) => path)).toEqual(['a/state.ts', 'b/state.ts']);
  });
});
