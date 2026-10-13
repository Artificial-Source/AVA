import { describe, expect, it } from 'vitest';
import { createInitialState } from './fixtures';
import { previewReducer } from './reducer';
import type { PreviewAction } from './types';

const selected = (state: ReturnType<typeof createInitialState>) => state.chats.find((chat) => chat.id === state.activeChatId)!;
describe('future local chat defaults', () => {
  it('snapshots mode, validated project, model, naming and inspector without changing existing facts', () => {
    const before = createInitialState(); const snapshot = JSON.stringify(before); const projectId = before.projects[2].id;
    const next = previewReducer(before, { type: 'newChat', model: 'orion', mode: 'plan', projectId, titleFromFirstMessage: true, inspector: 'context' });
    expect(selected(next)).toMatchObject({ model: 'orion', mode: 'plan', projectId, nameFromFirstMessage: true, title: 'New chat 1', messages: [] });
    expect(next.inspector).toBe('context'); expect(next.lastInspectorModule).toBe('context');
    expect(next.chats.slice(0, -1)).toEqual(before.chats); expect(next.childAgents).toBe(before.childAgents); expect(next.pinnedChatIds).toBe(before.pinnedChatIds);
    expect(JSON.stringify(before)).toBe(snapshot);
  });
  it.each(['remember', undefined] as const)('remembers the layout by default (%s)', (inspector) => {
    const before = { ...createInitialState(), inspector: null, lastInspectorModule: 'changes' as const };
    expect(previewReducer(before, { type: 'newChat', inspector })).toMatchObject({ inspector: null, lastInspectorModule: 'changes' });
  });
  it('closes without forgetting the last tab and clears only child inspection', () => {
    const before = { ...createInitialState(), inspectedChildId: 'child', inspector: 'changes' as const, lastInspectorModule: 'changes' as const };
    const next = previewReducer(before, { type: 'newChat', inspector: 'closed' });
    expect(next).toMatchObject({ inspector: null, lastInspectorModule: 'changes', inspectedChildId: null });
    expect(next.childAgents).toBe(before.childAgents);
  });
  it('fails closed for invalid caller inputs', () => {
    const before = createInitialState();
    const next = previewReducer(before, { type: 'newChat', projectId: 'invented', mode: 'invented', model: 'invented', titleFromFirstMessage: 'true', inspector: 'invented' } as unknown as PreviewAction);
    expect(selected(next)).toMatchObject({ projectId: selected(before).projectId, mode: 'build', model: 'aster' });
    expect(selected(next).nameFromFirstMessage).toBeUndefined(); expect(next.inspector).toBe(before.inspector);
  });
  it('names once from the first actual local send, normalizing whitespace and bounding Unicode code points', () => {
    let state = previewReducer(createInitialState(), { type: 'newChat', titleFromFirstMessage: true });
    const first = '  A\n\t  local title ' + '🪐'.repeat(80);
    state = previewReducer(state, { type: 'setDraft', chatId: state.activeChatId, draft: first });
    state = previewReducer(state, { type: 'sendLocalMessage' });
    const title = selected(state).title;
    expect(Array.from(title)).toHaveLength(60); expect(title.startsWith('A local title 🪐')).toBe(true); expect(title.endsWith('🪐')).toBe(true);
    expect(selected(state).nameFromFirstMessage).toBe(false);
    expect(selected(state).messages[0]).toMatchObject({ role: 'user', origin: 'local', text: first.trim(), selectedModel: { id: 'aster' } });
    state = previewReducer(state, { type: 'setDraft', chatId: state.activeChatId, draft: 'Do not rename again' });
    state = previewReducer(state, { type: 'sendLocalMessage' }); expect(selected(state).title).toBe(title);
  });
  it('does not infer eligibility from a title, consume the flag on empty/working/child sends, or rename old chats', () => {
    let state = previewReducer(createInitialState(), { type: 'newChat', titleFromFirstMessage: true });
    expect(previewReducer(state, { type: 'sendLocalMessage' })).toBe(state);
    state = previewReducer(state, { type: 'setDraft', chatId: state.activeChatId, draft: 'A message' });
    const child = { ...state, inspectedChildId: 'child' }; expect(previewReducer(child, { type: 'sendLocalMessage' })).toBe(child);
    const working = { ...state, chats: state.chats.map((chat) => chat.id === state.activeChatId ? { ...chat, status: 'working' as const } : chat) }; expect(previewReducer(working, { type: 'sendLocalMessage' })).toBe(working);
    const original = createInitialState(); const next = previewReducer(original, { type: 'sendLocalMessage' }); expect(selected(next).title).toBe(selected(original).title);
    state = previewReducer(state, { type: 'newChat' }); state = previewReducer(state, { type: 'setDraft', chatId: state.activeChatId, draft: 'No naming flag' }); state = previewReducer(state, { type: 'sendLocalMessage' }); expect(selected(state).title).toBe('New chat 2');
  });
});
