import { describe, expect, it } from 'vitest';
import { createInitialState } from './fixtures';
import { previewReducer } from './reducer';
import { inspectedChild, selectedParentChildren } from './selectors';
import type { ContextId, ModelId, PreviewAction, PreviewState } from './types';
import { previewContextInfo } from '../components/DetailInspector';

function deepFreeze<T>(value: T): T {
  if (value && typeof value === 'object') {
    Object.freeze(value);
    Object.values(value).forEach(deepFreeze);
  }
  return value;
}

function current(state: PreviewState) {
  return state.chats.find((chat) => chat.id === state.activeChatId)!;
}

describe('preview navigation invariants', () => {
  it('starts with no pins and only the initial view in recent history', () => {
    const state = createInitialState();
    expect(state.pinnedChatIds).toEqual([]);
    expect(state.recentChatIds).toEqual([state.activeChatId]);
    expect(state.chats.find((chat) => chat.id === 'keyboard-navigation')?.status).toBe('working');
    expect(state.inspector).toBe('agents');
    expect(state.lastInspectorModule).toBe('agents');
    expect(state.inspectedChildId).toBeNull();
  });

  it('viewing, pinning, unpinning, removing a recent shortcut, and returning never delete, cancel, or duplicate a chat', () => {
    const initial = deepFreeze(createInitialState());
    let state = previewReducer(initial, { type: 'viewChat', chatId: 'keyboard-navigation' });
    expect(state.pinnedChatIds).toBe(initial.pinnedChatIds);
    state = previewReducer(state, { type: 'pinChat', chatId: 'keyboard-navigation' });
    state = previewReducer(state, { type: 'unpinChat', chatId: 'keyboard-navigation' });
    state = previewReducer(state, { type: 'removeRecentChat', chatId: 'keyboard-navigation' });
    expect(state.activeChatId).toBe('keyboard-navigation');
    state = previewReducer(state, { type: 'viewChat', chatId: 'export-range' });
    state = previewReducer(state, { type: 'viewChat', chatId: 'keyboard-navigation' });
    expect(state.chats).toBe(initial.chats);
    expect(current(state)).toBe(initial.chats[1]);
    expect(current(state).status).toBe('working');
    expect(state.chats).toHaveLength(6);
  });

  it('supports no shortcuts and restores a recent shortcut only on another view', () => {
    let state = createInitialState();
    const originalChats = state.chats;
    state = previewReducer(state, { type: 'removeRecentChat', chatId: 'search-filters' });
    expect(state.activeChatId).toBe('search-filters');
    expect(state.recentChatIds).toEqual([]);
    expect(state.pinnedChatIds).toEqual([]);
    state = previewReducer(state, { type: 'viewChat', chatId: 'retry-behavior' });
    expect(state.recentChatIds).toEqual(['retry-behavior']);
    state = previewReducer(state, { type: 'viewChat', chatId: 'search-filters' });
    expect(state.recentChatIds).toEqual(['search-filters', 'retry-behavior']);
    expect(state.pinnedChatIds).toEqual([]);
    expect(state.chats).toBe(originalChats);
  });

  it('pins a specified chat only on request, deduplicates, and does not change the view or recency', () => {
    let state = deepFreeze(createInitialState());
    const originalRecent = state.recentChatIds;
    state = previewReducer(state, { type: 'pinChat', chatId: 'route-config' });
    const pinned = deepFreeze(state);
    expect(previewReducer(state, { type: 'pinChat', chatId: 'route-config' })).toBe(pinned);
    state = previewReducer(state, { type: 'pinChat', chatId: 'export-range' });
    expect(state.pinnedChatIds).toEqual(['route-config', 'export-range']);
    expect(state.activeChatId).toBe('search-filters');
    expect(state.recentChatIds).toBe(originalRecent);
    state = previewReducer(state, { type: 'unpinChat', chatId: 'route-config' });
    expect(state.pinnedChatIds).toEqual(['export-range']);
    expect(state.recentChatIds).toBe(originalRecent);
  });

  it('keeps at most five unique recent views, newest first, without pinning them', () => {
    const initial = deepFreeze(createInitialState());
    let state = initial;
    for (const chat of initial.chats) {
      state = previewReducer(state, { type: 'viewChat', chatId: chat.id });
    }
    expect(state.recentChatIds).toEqual(['retry-behavior', 'import-flow', 'export-range', 'route-config', 'keyboard-navigation']);
    state = previewReducer(state, { type: 'viewChat', chatId: 'export-range' });
    state = previewReducer(state, { type: 'viewChat', chatId: 'export-range' });
    expect(state.recentChatIds).toEqual(['export-range', 'retry-behavior', 'import-flow', 'route-config', 'keyboard-navigation']);
    state = previewReducer(state, { type: 'viewChat', chatId: 'search-filters' });
    expect(state.recentChatIds).toEqual(['search-filters', 'export-range', 'retry-behavior', 'import-flow', 'route-config']);
    expect(state.pinnedChatIds).toEqual([]);
    expect(state.chats).toBe(initial.chats);
  });

  it('does not add or reorder recent views for any non-navigation interaction', () => {
    let state = previewReducer(createInitialState(), { type: 'viewChat', chatId: 'keyboard-navigation' });
    state = previewReducer(state, { type: 'removeRecentChat', chatId: 'keyboard-navigation' });
    const originalRecent = state.recentChatIds;
    const actions: PreviewAction[] = [
      { type: 'setDraft', chatId: state.activeChatId, draft: 'A local draft' },
      { type: 'setModel', chatId: state.activeChatId, model: 'orion' },
      { type: 'setMode', chatId: state.activeChatId, mode: 'plan' },
      { type: 'pinChat', chatId: state.activeChatId },
      { type: 'unpinChat', chatId: state.activeChatId },
      { type: 'toggleProject', projectId: 'wayfinder' },
      { type: 'stopViewedMock' },
      { type: 'sendLocalMessage' },
      { type: 'addContext', chatId: state.activeChatId, contextId: 'search-file' },
      { type: 'removeContext', chatId: state.activeChatId, contextId: 'search-file' },
      { type: 'toggleComposerExpanded', chatId: state.activeChatId },
      { type: 'toggleInspector', module: 'changes' },
      { type: 'toggleInspectorVisibility' },
      { type: 'showInspector', module: 'agents' },
      { type: 'closeInspector' },
      { type: 'toggleOpaque' },
    ];
    for (const action of actions) {
      state = previewReducer(state, action);
      expect(state.recentChatIds).toBe(originalRecent);
    }
  });

  it('collapses projects independently without changing their chats or activity', () => {
    const initial = deepFreeze(createInitialState());
    const state = previewReducer(initial, { type: 'toggleProject', projectId: 'wayfinder' });
    expect(state.collapsedProjectIds).toEqual(['wayfinder']);
    expect(state.chats).toBe(initial.chats);
    expect(state.activeChatId).toBe(initial.activeChatId);
    expect(previewReducer(state, { type: 'toggleProject', projectId: 'wayfinder' }).collapsedProjectIds).toEqual([]);
  });

  it('ignores unknown navigation identities', () => {
    const state = deepFreeze(createInitialState());
    expect(previewReducer(state, { type: 'viewChat', chatId: 'missing' })).toBe(state);
    expect(previewReducer(state, { type: 'pinChat', chatId: 'missing' })).toBe(state);
    expect(previewReducer(state, { type: 'unpinChat', chatId: 'missing' })).toBe(state);
    expect(previewReducer(state, { type: 'removeRecentChat', chatId: 'missing' })).toBe(state);
    expect(previewReducer(state, { type: 'toggleProject', projectId: 'missing' })).toBe(state);
  });
});

describe('local chat state', () => {
  it('preserves immutable assistant attribution across model changes and snapshots only the selected model on local Send', () => {
    const before = deepFreeze(createInitialState());
    const assistant = before.chats[0].messages[1];
    expect(assistant).toMatchObject({ attribution: { modelLabel: 'Aster', thinkingLabel: 'High' } });
    let state = previewReducer(before, { type: 'setModel', chatId: 'search-filters', model: 'orion' });
    expect(current(state).messages[1]).toBe(assistant);
    state = previewReducer(deepFreeze(state), { type: 'sendLocalMessage' });
    const sent = current(state).messages.at(-1)!;
    expect(sent).toMatchObject({ role: 'user', selectedModel: { id: 'orion', label: 'Orion' } });
    expect(sent).not.toHaveProperty('attribution'); expect(sent).not.toHaveProperty('thinkingLabel');
    state = previewReducer(deepFreeze(state), { type: 'setModel', chatId: 'search-filters', model: 'aster-mini' });
    expect(current(state).messages.at(-1)).toBe(sent); expect(current(state).messages[1]).toBe(assistant);
    expect(state.childAgents).toBe(before.childAgents);
  });
  it('retains separate drafts, model, and mode across views, pins, recent removal, and project collapse', () => {
    let state = deepFreeze(createInitialState());
    state = previewReducer(state, { type: 'setDraft', chatId: 'search-filters', draft: 'A search draft' });
    state = previewReducer(state, { type: 'setModel', chatId: 'search-filters', model: 'aster-mini' });
    state = previewReducer(state, { type: 'setMode', chatId: 'search-filters', mode: 'plan' });
    const searchChat = current(state);
    state = previewReducer(state, { type: 'pinChat', chatId: 'search-filters' });
    state = previewReducer(state, { type: 'toggleProject', projectId: 'wayfinder' });
    state = previewReducer(state, { type: 'viewChat', chatId: 'export-range' });
    state = previewReducer(state, { type: 'setDraft', chatId: 'export-range', draft: 'An export draft' });
    expect(current(state)).toMatchObject({ draft: 'An export draft', model: 'orion', mode: 'plan', status: 'approval' });
    state = previewReducer(state, { type: 'unpinChat', chatId: 'search-filters' });
    state = previewReducer(state, { type: 'removeRecentChat', chatId: 'search-filters' });
    state = previewReducer(state, { type: 'viewChat', chatId: 'search-filters' });
    expect(current(state)).toBe(searchChat);
    expect(current(state)).toMatchObject({ draft: 'A search draft', model: 'aster-mini', mode: 'plan' });
  });

  it('creates a unique in-memory chat in the viewed project without pinning or starting work', () => {
    let state = previewReducer(createInitialState(), { type: 'viewChat', chatId: 'export-range' });
    state = previewReducer(state, { type: 'toggleProject', projectId: 'papertrail' });
    const before = deepFreeze(state);
    state = previewReducer(state, { type: 'newChat' });
    expect(current(state)).toMatchObject({ id: 'local-chat-1', projectId: 'papertrail', status: 'idle', messages: [], changes: [], draft: '' });
    expect(state.collapsedProjectIds).not.toContain('papertrail');
    expect(state.pinnedChatIds).toBe(before.pinnedChatIds);
    expect(state.recentChatIds).toEqual(['local-chat-1', 'export-range', 'search-filters']);
    expect(state.chats.slice(0, 6)).toEqual(before.chats);
    state = previewReducer(state, { type: 'newChat' });
    expect(current(state).id).toBe('local-chat-2');
    expect(state.chats).toHaveLength(8);
  });

  it.each(['aster', 'aster-mini', 'orion'] as const)('creates an immutable %s default-model chat without inheriting usage, cost or parent settings', (model) => {
    const before = deepFreeze(createInitialState());
    const state = previewReducer(before, { type: 'newChat', model });
    expect(current(state)).toMatchObject({ model, mode: 'build', status: 'idle', draft: '', contextIds: [], composerExpanded: false, messages: [], changes: [] });
    before.chats.forEach((chat, index) => expect(state.chats[index]).toBe(chat));
    expect(state.childAgents).toBe(before.childAgents);
    expect(state.pinnedChatIds).toBe(before.pinnedChatIds);
    const context = previewContextInfo(current(state));
    expect(context.usage.profile.modelId).toBe(model);
    expect(context.usage.snapshot).toBeNull();
    expect(context.cumulative).toEqual({});
    expect(previewReducer(state, { type: 'newChat' }).chats.at(-1)?.model).toBe('aster');
  });

  it.each([undefined, 'unknown', '', null])('bounds an absent or invalid new-chat model (%s) to Aster', (model) => {
    const state = previewReducer(deepFreeze(createInitialState()), { type: 'newChat', model: model as ModelId });
    expect(current(state).model).toBe('aster');
    expect(current(state).mode).toBe('build');
  });

  it('bounds recent history for newly created chats as well as viewed fixtures', () => {
    let state = createInitialState();
    for (let count = 0; count < 7; count++) state = previewReducer(state, { type: 'newChat' });
    expect(state.recentChatIds).toEqual(['local-chat-7', 'local-chat-6', 'local-chat-5', 'local-chat-4', 'local-chat-3']);
    expect(state.pinnedChatIds).toEqual([]);
    expect(new Set(state.chats.map((chat) => chat.id)).size).toBe(13);
  });

  it('Send adds one local user message only, preserving fixture status and background chats', () => {
    const before = deepFreeze(createInitialState());
    const state = previewReducer(before, { type: 'sendLocalMessage' });
    expect(current(state).messages.at(-1)).toEqual({
      id: 'local-message-1', role: 'user', origin: 'local', text: before.chats[0].draft, selectedModel: { id: 'aster', label: 'Aster' },
    });
    expect(current(state).messages).toHaveLength(3);
    expect(current(state).messages.filter((message) => message.role === 'assistant')).toHaveLength(1);
    expect(current(state).draft).toBe('');
    expect(current(state).status).toBe('idle');
    expect(state.chats.slice(1)).toEqual(before.chats.slice(1));
    expect(state.announcement).toContain('No model was called');
  });

  it('does not Send blank drafts or pretend that a Working fixture accepts a live turn', () => {
    let state = previewReducer(createInitialState(), { type: 'setDraft', chatId: 'search-filters', draft: '  \n ' });
    expect(previewReducer(state, { type: 'sendLocalMessage' })).toBe(state);
    state = previewReducer(state, { type: 'viewChat', chatId: 'keyboard-navigation' });
    state = previewReducer(state, { type: 'setDraft', chatId: state.activeChatId, draft: 'A future draft' });
    expect(previewReducer(state, { type: 'sendLocalMessage' })).toBe(state);
  });

  it('only explicit Stop changes the viewed Working fixture, never another Working fixture', () => {
    const initial = createInitialState();
    initial.chats[2].status = 'working';
    const before = deepFreeze(initial);
    expect(previewReducer(before, { type: 'stopViewedMock' })).toBe(before);
    const viewed = previewReducer(before, { type: 'viewChat', chatId: 'keyboard-navigation' });
    const state = previewReducer(viewed, { type: 'stopViewedMock' });
    expect(current(state).status).toBe('stopped');
    expect(state.chats[2]).toBe(before.chats[2]);
    expect(state.chats[2].status).toBe('working');
    expect(state.chats[3].status).toBe('approval');
    expect(state.chats).toHaveLength(6);
    expect(current(state).messages).toBe(before.chats[1].messages);
  });

  it('is deterministic and immutable when React StrictMode calls it twice', () => {
    const initial = deepFreeze(createInitialState());
    expect(createInitialState()).toEqual(initial);
    expect(previewReducer(initial, { type: 'newChat' })).toEqual(previewReducer(initial, { type: 'newChat' }));
    expect(previewReducer(initial, { type: 'sendLocalMessage' })).toEqual(previewReducer(initial, { type: 'sendLocalMessage' }));
    expect(previewReducer(initial, { type: 'viewChat', chatId: 'export-range' })).toEqual(previewReducer(initial, { type: 'viewChat', chatId: 'export-range' }));
    expect(initial.chats).toHaveLength(6);
    expect(initial.nextChatNumber).toBe(1);
  });

  it('toggles review and material without modifying any chat', () => {
    const before = deepFreeze(createInitialState());
    let state = previewReducer(before, { type: 'toggleInspector', module: 'changes' });
    state = previewReducer(state, { type: 'toggleOpaque' });
    expect(state.inspector).toBe('changes');
    expect(state.opaque).toBe(true);
    expect(state.chats).toBe(before.chats);
  });
});

describe('shared inspector visibility and module selection', () => {
  it('remembers the rail-selected module through visibility toggles, rail close, and panel close', () => {
    let state = deepFreeze(createInitialState());
    state = previewReducer(state, { type: 'toggleInspector', module: 'changes' });
    expect(state).toMatchObject({ inspector: 'changes', lastInspectorModule: 'changes' });
    for (const close of [
      { type: 'toggleInspectorVisibility' },
      { type: 'toggleInspector', module: 'changes' },
      { type: 'closeInspector' },
    ] satisfies PreviewAction[]) {
      state = previewReducer(deepFreeze(state), close);
      expect(state).toMatchObject({ inspector: null, lastInspectorModule: 'changes' });
      state = previewReducer(deepFreeze(state), { type: 'toggleInspectorVisibility' });
      expect(state).toMatchObject({ inspector: 'changes', lastInspectorModule: 'changes' });
    }
    state = previewReducer(state, { type: 'toggleInspector', module: 'agents' });
    state = previewReducer(state, { type: 'toggleInspectorVisibility' });
    state = previewReducer(state, { type: 'toggleInspectorVisibility' });
    expect(state).toMatchObject({ inspector: 'agents', lastInspectorModule: 'agents' });
  });

  it('explicitly shows each module from closed or open state, never toggling a repeated choice shut', () => {
    let state = deepFreeze(createInitialState());
    expect(previewReducer(state, { type: 'showInspector', module: 'agents' })).toBe(state);
    for (const module of ['changes', 'context', 'agents'] as const) {
      state = previewReducer(state, { type: 'closeInspector' });
      state = previewReducer(deepFreeze(state), { type: 'showInspector', module });
      expect(state).toMatchObject({ inspector: module, lastInspectorModule: module });
      expect(previewReducer(deepFreeze(state), { type: 'showInspector', module })).toBe(state);
      state = previewReducer(state, { type: 'toggleInspectorVisibility' });
      state = previewReducer(state, { type: 'toggleInspectorVisibility' });
      expect(state.inspector).toBe(module);
    }
    state = previewReducer(state, { type: 'showInspector', module: 'changes' });
    expect(state).toMatchObject({ inspector: 'changes', lastInspectorModule: 'changes' });
  });

  it.each([
    ['search-filters', 'search-return'],
    ['keyboard-navigation', 'keyboard-focus'],
  ])('changes only inspector fields while viewing %s or its child', (chatId, childId) => {
    let state = previewReducer(createInitialState(), { type: 'viewChat', chatId });
    for (const action of [
      { type: 'setDraft', chatId, draft: 'Keep this parent draft\nAnd its second line.' },
      { type: 'setModel', chatId, model: 'orion' },
      { type: 'setMode', chatId, mode: 'plan' },
      { type: 'addContext', chatId, contextId: 'search-file' },
      { type: 'toggleComposerExpanded', chatId },
      { type: 'pinChat', chatId },
    ] satisfies PreviewAction[]) state = previewReducer(state, action);
    const parent = deepFreeze(state);
    for (const inspectedChildId of [null, childId]) {
      const before = deepFreeze({ ...parent, inspectedChildId });
      state = before;
      for (const action of [
        { type: 'toggleInspectorVisibility' },
        { type: 'showInspector', module: 'changes' },
        { type: 'showInspector', module: 'changes' },
        { type: 'closeInspector' },
        { type: 'toggleInspectorVisibility' },
        { type: 'toggleInspector', module: 'agents' },
        { type: 'toggleInspector', module: 'agents' },
        { type: 'showInspector', module: 'agents' },
        { type: 'showInspector', module: 'context' },
        { type: 'showInspector', module: 'context' },
        { type: 'toggleInspector', module: 'context' },
        { type: 'toggleInspectorVisibility' },
      ] satisfies PreviewAction[]) {
        state = deepFreeze(previewReducer(state, action));
        for (const key of Object.keys(before) as (keyof PreviewState)[]) {
          if (key !== 'inspector' && key !== 'lastInspectorModule') expect(state[key]).toBe(before[key]);
        }
      }
    }
  });
});

describe('parent-owned child inspection', () => {
  it('derives a stable working-first roster and selected-parent count without reordering fixture data', () => {
    const state = deepFreeze(createInitialState());
    const children = selectedParentChildren(state);
    expect(children.map((child) => child.id)).toEqual(['search-return', 'search-outline', 'search-edge-cases']);
    expect(children.filter((child) => child.status === 'working')).toHaveLength(1);
    expect(state.childAgents[0].id).toBe('search-outline');
    expect(children[0]).toBe(state.childAgents[1]);
    expect(current(state).status).toBe('idle');
    const empty = previewReducer(state, { type: 'viewChat', chatId: 'route-config' });
    expect(selectedParentChildren(empty)).toEqual([]);
    expect(empty.childAgents).toBe(state.childAgents);
  });

  it('opens exactly one inspector or none, preserving the parent and child selection', () => {
    let state = previewReducer(deepFreeze(createInitialState()), { type: 'inspectChild', childId: 'search-return' });
    const before = deepFreeze(state);
    for (const [module, expected] of [['agents', null], ['changes', 'changes'], ['context', 'context'], ['context', null], ['agents', 'agents'], ['changes', 'changes']] as const) {
      state = previewReducer(state, { type: 'toggleInspector', module });
      expect(state.inspector).toBe(expected);
      expect(state.chats).toBe(before.chats);
      expect(state.inspectedChildId).toBe('search-return');
    }
    state = previewReducer(state, { type: 'closeInspector' });
    expect(state.inspector).toBeNull();
    expect(previewReducer(state, { type: 'closeInspector' })).toBe(state);
  });

  it('rejects unknown or other-parent children, including stale selection after a parent view', () => {
    const before = deepFreeze(createInitialState());
    for (const childId of ['missing', 'keyboard-focus', 'export-summary']) {
      expect(previewReducer(before, { type: 'inspectChild', childId })).toBe(before);
    }
    const selected = previewReducer(before, { type: 'inspectChild', childId: 'search-return' });
    expect(selected.activeChatId).toBe(before.activeChatId);
    expect(selected.chats).toBe(before.chats);
    expect(selected.pinnedChatIds).toBe(before.pinnedChatIds);
    expect(selected.recentChatIds).toBe(before.recentChatIds);
    expect(inspectedChild(selected)).toBe(before.childAgents[1]);
    expect(previewReducer(selected, { type: 'inspectChild', childId: 'search-return' })).toBe(selected);
    const viewed = previewReducer(selected, { type: 'viewChat', chatId: 'export-range' });
    expect(previewReducer(viewed, { type: 'inspectChild', childId: 'search-return' })).toBe(viewed);
    expect(inspectedChild({ ...viewed, inspectedChildId: 'search-return' })).toBeUndefined();
  });

  it.each([
    ['search-filters', 'search-return'],
    ['keyboard-navigation', 'keyboard-focus'],
  ])('guards Send and Stop in the reducer while inspecting a child of %s', (parentId, childId) => {
    let state = previewReducer(createInitialState(), { type: 'viewChat', chatId: parentId });
    state = previewReducer(state, { type: 'setDraft', chatId: parentId, draft: 'Retain this draft' });
    state = deepFreeze(previewReducer(state, { type: 'inspectChild', childId }));
    expect(previewReducer(state, { type: 'sendLocalMessage' })).toBe(state);
    expect(previewReducer(state, { type: 'stopViewedMock' })).toBe(state);
    for (const action of [
      { type: 'setDraft', chatId: parentId, draft: 'Not editable' },
      { type: 'setModel', chatId: parentId, model: 'orion' },
      { type: 'setMode', chatId: parentId, mode: 'plan' },
      { type: 'addContext', chatId: parentId, contextId: 'search-file' },
      { type: 'removeContext', chatId: parentId, contextId: 'search-file' },
      { type: 'toggleComposerExpanded', chatId: parentId },
    ] satisfies PreviewAction[]) expect(previewReducer(state, action)).toBe(state);
  });

  it('returns the identical parent entity with draft/model/mode/context/expansion and no new history or pins', () => {
    let state = createInitialState();
    for (const action of [
      { type: 'setDraft', chatId: 'search-filters', draft: 'Long parent draft\nMore detail.' },
      { type: 'setModel', chatId: 'search-filters', model: 'orion' },
      { type: 'setMode', chatId: 'search-filters', mode: 'plan' },
      { type: 'addContext', chatId: 'search-filters', contextId: 'result-image' },
      { type: 'toggleComposerExpanded', chatId: 'search-filters' },
    ] satisfies PreviewAction[]) state = previewReducer(state, action);
    const before = deepFreeze(state);
    state = previewReducer(state, { type: 'inspectChild', childId: 'search-edge-cases' });
    state = previewReducer(state, { type: 'backToParent' });
    expect(state.inspectedChildId).toBeNull();
    expect(state.chats).toBe(before.chats);
    expect(current(state)).toBe(current(before));
    expect(current(state).contextIds).toEqual(['result-image']);
    expect(current(state).composerExpanded).toBe(true);
    expect(state.pinnedChatIds).toBe(before.pinnedChatIds);
    expect(state.recentChatIds).toBe(before.recentChatIds);
    expect(state.childAgents).toBe(before.childAgents);
    expect(previewReducer(state, { type: 'backToParent' })).toBe(state);
  });

  it('clears inspection on any valid sidebar view or New, but not an unknown view', () => {
    const state = deepFreeze(previewReducer(createInitialState(), { type: 'inspectChild', childId: 'search-return' }));
    expect(previewReducer(state, { type: 'viewChat', chatId: 'missing' })).toBe(state);
    for (const chatId of ['search-filters', 'keyboard-navigation', 'export-range']) {
      const viewed = previewReducer(state, { type: 'viewChat', chatId });
      expect(viewed.inspectedChildId).toBeNull();
      expect(viewed.chats).toBe(state.chats);
      expect(viewed.inspector).toBe('agents');
    }
    const created = previewReducer(state, { type: 'newChat' });
    expect(created.inspectedChildId).toBeNull();
    expect(current(created)).toMatchObject({ projectId: 'wayfinder', contextIds: [], composerExpanded: false });
    expect(created.childAgents).toBe(state.childAgents);
    expect(selectedParentChildren(created)).toEqual([]);
  });

  it('does not couple parent Stop or Send with child lifecycle or other-parent state', () => {
    const initial = deepFreeze(createInitialState());
    let state = previewReducer(initial, { type: 'viewChat', chatId: 'keyboard-navigation' });
    state = previewReducer(state, { type: 'stopViewedMock' });
    expect(current(state).status).toBe('stopped');
    expect(selectedParentChildren(state)[0].status).toBe('working');
    expect(state.childAgents).toBe(initial.childAgents);
    state = previewReducer(state, { type: 'viewChat', chatId: 'search-filters' });
    state = previewReducer(state, { type: 'sendLocalMessage' });
    expect(current(state).status).toBe('idle');
    expect(state.childAgents).toBe(initial.childAgents);
    expect(state.chats[2]).toBe(initial.chats[2]);
  });

  it('keeps fixed terminal and attention status through visits rather than an unread or timer policy', () => {
    const before = deepFreeze(createInitialState());
    let state = before;
    for (const chat of before.chats) {
      state = previewReducer(state, { type: 'viewChat', chatId: chat.id });
      expect(current(state).status).toBe(chat.status);
    }
    expect(state.chats).toBe(before.chats);
    expect(before.chats.map((chat) => chat.status)).toEqual(['idle', 'working', 'completed', 'approval', 'stopped', 'failed']);
  });
});

describe('per-parent composer presentation and example context', () => {
  it('attaches unique known references in order, removes only the target, and never mutates another parent', () => {
    const before = deepFreeze(createInitialState());
    let state = previewReducer(before, { type: 'addContext', chatId: 'search-filters', contextId: 'search-file' });
    expect(previewReducer(state, { type: 'addContext', chatId: 'search-filters', contextId: 'search-file' })).toBe(state);
    state = previewReducer(state, { type: 'addContext', chatId: 'search-filters', contextId: 'result-image' });
    state = previewReducer(state, { type: 'addContext', chatId: 'export-range', contextId: 'navigation-notes' });
    expect(current(state).contextIds).toEqual(['search-file', 'result-image']);
    expect(state.chats[3].contextIds).toEqual(['navigation-notes']);
    const exportChat = state.chats[3];
    state = previewReducer(state, { type: 'removeContext', chatId: 'search-filters', contextId: 'search-file' });
    expect(current(state).contextIds).toEqual(['result-image']);
    expect(state.chats[3]).toBe(exportChat);
    expect(state.chats[1]).toBe(before.chats[1]);
    expect(current(state).messages).toBe(before.chats[0].messages);
    expect(state.recentChatIds).toBe(before.recentChatIds);
    expect(before.chats.every((chat) => chat.contextIds.length === 0)).toBe(true);
  });

  it('ignores unknown parent/reference IDs and missing removals', () => {
    const state = deepFreeze(createInitialState());
    expect(previewReducer(state, { type: 'addContext', chatId: 'missing', contextId: 'search-file' })).toBe(state);
    expect(previewReducer(state, { type: 'addContext', chatId: 'search-filters', contextId: 'missing' as ContextId })).toBe(state);
    expect(previewReducer(state, { type: 'removeContext', chatId: 'search-filters', contextId: 'search-file' })).toBe(state);
    expect(previewReducer(state, { type: 'toggleComposerExpanded', chatId: 'missing' })).toBe(state);
  });

  it('retains expansion and context on return to a parent without altering text or settings', () => {
    const before = deepFreeze(createInitialState());
    let state = previewReducer(before, { type: 'toggleComposerExpanded', chatId: 'search-filters' });
    state = previewReducer(state, { type: 'addContext', chatId: 'search-filters', contextId: 'navigation-notes' });
    const expanded = current(state);
    state = previewReducer(state, { type: 'viewChat', chatId: 'export-range' });
    expect(current(state).composerExpanded).toBe(false);
    expect(current(state).contextIds).toEqual([]);
    state = previewReducer(state, { type: 'viewChat', chatId: 'search-filters' });
    expect(current(state)).toBe(expanded);
    state = previewReducer(state, { type: 'toggleComposerExpanded', chatId: 'search-filters' });
    expect(current(state)).toMatchObject({ composerExpanded: false, draft: before.chats[0].draft, model: before.chats[0].model, mode: before.chats[0].mode });
  });
});

it('sets canonical sidebar opacity idempotently without touching chat or inspector state', () => {
  const state = createInitialState();
  const opaque = previewReducer(state, { type: 'setOpaque', opaque: true });
  expect(opaque).toEqual({ ...state, opaque: true });
  expect(opaque.chats).toBe(state.chats); expect(opaque.childAgents).toBe(state.childAgents);
  expect(previewReducer(opaque, { type: 'setOpaque', opaque: true })).toBe(opaque);
  expect(previewReducer(opaque, { type: 'setOpaque', opaque: 'glass' as unknown as boolean })).toBe(opaque);
  expect(previewReducer(opaque, { type: 'setOpaque', opaque: false })).toEqual(state);
});
