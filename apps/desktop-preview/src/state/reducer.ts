import { contextReferences, modelOptions } from './fixtures';
import type { Chat, PreviewAction, PreviewState } from './types';
import { plain, receiveSuppliedRun, sameRun, stopSuppliedRun, validId } from './suppliedRun';

function recordRecent(recentChatIds: string[], chatId: string): string[] {
  return [chatId, ...recentChatIds.filter((id) => id !== chatId)].slice(0, 5);
}

function updateChat(state: PreviewState, chatId: string, update: (chat: Chat) => Chat): PreviewState {
  const chat = state.chats.find((entry) => entry.id === chatId);
  if (!chat) return state;
  const nextChat = update(chat);
  return nextChat === chat ? state : { ...state, chats: state.chats.map((entry) => (entry.id === chatId ? nextChat : entry)) };
}

export function previewReducer(state: PreviewState, action: PreviewAction): PreviewState {
  switch (action.type) {
    case 'startSuppliedRun': {
      const id = action.identity;
      if (!plain(id) || Object.keys(id).length !== 3 || !['chatId', 'runId', 'generation'].every((key) => Object.hasOwn(id, key)) || !validId(id.chatId) || !validId(id.runId) || !Number.isSafeInteger(id.generation) || id.generation !== (state.suppliedRun?.generation ?? 0) + 1 || id.runId === state.suppliedRun?.runId || state.activeChatId !== id.chatId || state.inspectedChildId || state.suppliedRun?.state === 'streaming') return state;
      const chat = state.chats.find((entry) => entry.id === id.chatId);
      if (!chat || chat.status === 'working') return state;
      const messageId = `supplied-run-${id.generation}`;
      return { ...updateChat(state, chat.id, (entry) => ({ ...entry, status: 'working', messages: [...entry.messages, { id: messageId, role: 'assistant', markdown: '', lifecycle: 'streaming', attribution: { modelLabel: 'Aster', thinkingLabel: 'High' } }] })), suppliedRun: { chatId: id.chatId, runId: id.runId, generation: id.generation, messageId, sequence: 0, state: 'streaming' }, announcement: 'Run preview started. Supplied browser-local example; no model or tools called.' };
    }
    case 'receiveSuppliedRun':
      return receiveSuppliedRun(state, action.events);
    case 'stopSuppliedRun':
      return plain(action.identity) && state.suppliedRun && sameRun(state.suppliedRun, action.identity) && state.activeChatId === action.identity.chatId && !state.inspectedChildId ? stopSuppliedRun(state) : state;
    case 'viewChat':
      return state.chats.some((chat) => chat.id === action.chatId)
        ? {
            ...(action.chatId === state.activeChatId ? state : stopSuppliedRun(state)),
            activeChatId: action.chatId,
            inspectedChildId: null,
            recentChatIds: recordRecent(state.recentChatIds, action.chatId),
            announcement: '',
          }
        : state;
    case 'toggleProject':
      if (!state.projects.some((project) => project.id === action.projectId)) return state;
      return {
        ...state,
        collapsedProjectIds: state.collapsedProjectIds.includes(action.projectId)
          ? state.collapsedProjectIds.filter((id) => id !== action.projectId)
          : [...state.collapsedProjectIds, action.projectId],
      };
    case 'pinChat':
      if (!state.chats.some((chat) => chat.id === action.chatId) || state.pinnedChatIds.includes(action.chatId)) return state;
      return {
        ...state,
        pinnedChatIds: [...state.pinnedChatIds, action.chatId],
        announcement: 'Chat pinned in the sidebar.',
      };
    case 'unpinChat':
      if (!state.pinnedChatIds.includes(action.chatId)) return state;
      return {
        ...state,
        pinnedChatIds: state.pinnedChatIds.filter((id) => id !== action.chatId),
        announcement: 'Chat unpinned. The conversation and its mock status are unchanged.',
      };
    case 'removeRecentChat':
      if (!state.recentChatIds.includes(action.chatId)) return state;
      return {
        ...state,
        recentChatIds: state.recentChatIds.filter((id) => id !== action.chatId),
        announcement: 'Removed from recent chats. The conversation and its mock status are unchanged.',
      };
    case 'newChat': {
      state = stopSuppliedRun(state);
      const viewedChat = state.chats.find((chat) => chat.id === state.activeChatId)!;
      const inspector = action.inspector;
      const chat: Chat = {
        id: `local-chat-${state.nextChatNumber}`,
        projectId: state.projects.find((project) => project.id === action.projectId)?.id ?? viewedChat.projectId,
        title: `New chat ${state.nextChatNumber}`,
        ...(action.titleFromFirstMessage === true ? { nameFromFirstMessage: true } : {}),
        status: 'idle',
        draft: '',
        model: modelOptions.find((option) => option.id === action.model)?.id ?? 'aster',
        mode: action.mode === 'plan' ? 'plan' : 'build',
        contextIds: [],
        composerExpanded: false,
        messages: [],
        changes: [],
      };
      return {
        ...state,
        chats: [...state.chats, chat],
        activeChatId: chat.id,
        inspectedChildId: null,
        ...(inspector === 'closed' ? { inspector: null } : inspector === 'agents' || inspector === 'changes' || inspector === 'context' ? { inspector, lastInspectorModule: inspector } : {}),
        recentChatIds: recordRecent(state.recentChatIds, chat.id),
        collapsedProjectIds: state.collapsedProjectIds.filter((id) => id !== chat.projectId),
        nextChatNumber: state.nextChatNumber + 1,
        announcement: 'Local chat created. Nothing is saved outside this preview.',
      };
    }
    case 'setDraft':
      if (state.inspectedChildId) return state;
      return updateChat(state, action.chatId, (chat) => ({ ...chat, draft: action.draft }));
    case 'setModel':
      if (state.inspectedChildId) return state;
      return updateChat(state, action.chatId, (chat) => ({ ...chat, model: action.model }));
    case 'setMode':
      if (state.inspectedChildId) return state;
      return updateChat(state, action.chatId, (chat) => ({ ...chat, mode: action.mode }));
    case 'addContext': {
      if (state.inspectedChildId || !contextReferences.some((reference) => reference.id === action.contextId)) return state;
      const next = updateChat(state, action.chatId, (chat) => chat.contextIds.includes(action.contextId)
        ? chat : { ...chat, contextIds: [...chat.contextIds, action.contextId] });
      return next === state ? state : { ...next, announcement: 'Example context attached to this parent chat. No file was read or uploaded.' };
    }
    case 'removeContext': {
      if (state.inspectedChildId) return state;
      const next = updateChat(state, action.chatId, (chat) => !chat.contextIds.includes(action.contextId)
        ? chat : { ...chat, contextIds: chat.contextIds.filter((id) => id !== action.contextId) });
      return next === state ? state : { ...next, announcement: 'Example context removed from this parent chat.' };
    }
    case 'toggleComposerExpanded':
      if (state.inspectedChildId) return state;
      return updateChat(state, action.chatId, (chat) => ({ ...chat, composerExpanded: !chat.composerExpanded }));
    case 'inspectChild': {
      const child = state.childAgents.find((entry) => entry.id === action.childId && entry.parentChatId === state.activeChatId);
      if (!child || state.inspectedChildId === child.id) return state;
      return {
        ...stopSuppliedRun(state),
        inspectedChildId: child.id,
        announcement: `Inspecting ${child.title}. Read-only child example. The parent chat is unchanged.`,
      };
    }
    case 'backToParent':
      return state.inspectedChildId ? { ...state, inspectedChildId: null, announcement: 'Returned to the parent chat. Its draft and settings are unchanged.' } : state;
    case 'sendLocalMessage': {
      if (state.inspectedChildId) return state;
      const chat = state.chats.find((entry) => entry.id === state.activeChatId)!;
      const text = chat.draft.trim();
      const selectedModel = modelOptions.find((model) => model.id === chat.model);
      if (!text || chat.status === 'working' || !selectedModel) return state;
      return {
        ...updateChat(state, chat.id, (entry) => ({
          ...entry,
          draft: '',
          ...(entry.nameFromFirstMessage ? { nameFromFirstMessage: false, title: Array.from(text.replace(/\s+/gu, ' ')).slice(0, 60).join('') } : {}),
          messages: [
            ...entry.messages,
            // Selection at Send is not an invocation or an assistant attribution.
            { id: `local-message-${state.nextMessageNumber}`, role: 'user', origin: 'local', text, selectedModel: { ...selectedModel } },
          ],
        })),
        nextMessageNumber: state.nextMessageNumber + 1,
        announcement: 'Local preview message added. No model was called.',
      };
    }
    case 'stopViewedMock': {
      if (state.inspectedChildId) return state;
      if (state.suppliedRun?.state === 'streaming' && state.suppliedRun.chatId === state.activeChatId) return stopSuppliedRun(state);
      const chat = state.chats.find((entry) => entry.id === state.activeChatId)!;
      if (chat.status !== 'working') return state;
      return {
        ...updateChat(state, chat.id, (entry) => ({ ...entry, status: 'stopped' })),
        announcement: `Stopped the ${chat.title} mock fixture only. No real task was running.`,
      };
    }
    case 'toggleInspector':
      return { ...state, inspector: state.inspector === action.module ? null : action.module, lastInspectorModule: action.module };
    case 'toggleInspectorVisibility':
      return { ...state, inspector: state.inspector ? null : state.lastInspectorModule };
    case 'showInspector':
      return state.inspector === action.module ? state : { ...state, inspector: action.module, lastInspectorModule: action.module };
    case 'closeInspector':
      return state.inspector ? { ...state, inspector: null } : state;
    case 'toggleOpaque':
      return { ...state, opaque: !state.opaque };
    case 'setOpaque':
      return typeof action.opaque === 'boolean' && action.opaque !== state.opaque ? { ...state, opaque: action.opaque } : state;
  }
}
