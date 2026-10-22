import { contextReferences, modelOptions } from './fixtures';
import type { Chat, ChatMode, Message, ModelId, PreviewAction, PreviewState } from './types';
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

// Visible chat titles: single spaces, no edge whitespace, bounded by code points.
export function chatTitle(value: string, limit = 80) {
  return Array.from(value.replace(/\s+/gu, ' ').trim()).slice(0, limit).join('').trim();
}

type NewChatOptions = { model?: ModelId; mode?: ChatMode; projectId?: string; titleFromFirstMessage?: boolean };

// Appends and views a chat. The caller stops any run first.
function appendChat(state: PreviewState, chat: Chat): PreviewState {
  return {
    ...state,
    chats: [...state.chats, chat],
    activeChatId: chat.id,
    inspectedChildId: null,
    recentChatIds: recordRecent(state.recentChatIds, chat.id),
    collapsedProjectIds: state.collapsedProjectIds.filter((id) => id !== chat.projectId),
    nextChatNumber: state.nextChatNumber + 1,
  };
}

function createChat(state: PreviewState, options: NewChatOptions, fallbackProjectId: string): PreviewState {
  return appendChat(state, {
    id: `local-chat-${state.nextChatNumber}`,
    projectId: state.projects.find((project) => project.id === options.projectId)?.id ?? fallbackProjectId,
    title: `New chat ${state.nextChatNumber}`,
    ...(options.titleFromFirstMessage === true ? { nameFromFirstMessage: true } : {}),
    status: 'idle',
    draft: '',
    model: modelOptions.find((option) => option.id === options.model)?.id ?? 'aster',
    mode: options.mode === 'plan' ? 'plan' : 'build',
    contextIds: [],
    composerExpanded: false,
    messages: [],
    changes: [],
  });
}

const branchSuffix = ' (branch)';
// A branch keeps the history, not the authority: a request left unanswered stays with its own chat.
function branchMessage(message: Message): Message {
  return message.role === 'assistant' && message.permission?.state === 'ready' && !message.permissionDecision
    ? { ...message, permission: { ...message.permission, state: 'closed' } }
    : message;
}

export function previewReducer(state: PreviewState, action: PreviewAction): PreviewState {
  const next = reduce(state, action);
  // An Undo offer lasts until the next accepted action. Run delivery is not a user action.
  if (next === state || !next.archiveNotice || action.type === 'archiveChat' || action.type === 'receiveSuppliedRun') return next;
  const { archiveNotice: _expired, ...rest } = next;
  return rest;
}

function reduce(state: PreviewState, action: PreviewAction): PreviewState {
  switch (action.type) {
    case 'startSuppliedRun': {
      const id = action.identity;
      if (!plain(id) || Object.keys(id).length !== 3 || !['chatId', 'runId', 'generation'].every((key) => Object.hasOwn(id, key)) || !validId(id.chatId) || !validId(id.runId) || !Number.isSafeInteger(id.generation) || id.generation !== (state.suppliedRun?.generation ?? 0) + 1 || id.runId === state.suppliedRun?.runId || state.activeChatId !== id.chatId || state.inspectedChildId || state.suppliedRun?.state === 'streaming') return state;
      const chat = state.chats.find((entry) => entry.id === id.chatId);
      if (!chat || chat.status === 'working' || chat.archived) return state;
      const messageId = `supplied-run-${id.generation}`;
      return { ...updateChat(state, chat.id, (entry) => ({ ...entry, status: 'working', messages: [...entry.messages, { id: messageId, role: 'assistant', markdown: '', lifecycle: 'streaming', attribution: { modelLabel: 'Aster', thinkingLabel: 'High' } }] })), suppliedRun: { chatId: id.chatId, runId: id.runId, generation: id.generation, messageId, sequence: 0, state: 'streaming' }, announcement: 'Run preview started.' };
    }
    case 'receiveSuppliedRun':
      return receiveSuppliedRun(state, action.events);
    case 'stopSuppliedRun':
      return plain(action.identity) && state.suppliedRun && sameRun(state.suppliedRun, action.identity) && state.activeChatId === action.identity.chatId && !state.inspectedChildId ? stopSuppliedRun(state) : state;
    case 'viewChat':
      return state.chats.some((chat) => chat.id === action.chatId && !chat.archived)
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
      if (!state.chats.some((chat) => chat.id === action.chatId && !chat.archived) || state.pinnedChatIds.includes(action.chatId)) return state;
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
        announcement: 'Chat unpinned.',
      };
    case 'removeRecentChat':
      if (!state.recentChatIds.includes(action.chatId)) return state;
      return {
        ...state,
        recentChatIds: state.recentChatIds.filter((id) => id !== action.chatId),
        announcement: 'Removed from recent chats.',
      };
    case 'newChat': {
      const stopped = stopSuppliedRun(state);
      const viewedChat = stopped.chats.find((chat) => chat.id === stopped.activeChatId)!;
      const inspector = action.inspector;
      return {
        ...createChat(stopped, action, viewedChat.projectId),
        ...(inspector === 'closed' ? { inspector: null } : inspector === 'agents' || inspector === 'changes' || inspector === 'context' ? { inspector, lastInspectorModule: inspector } : {}),
        announcement: 'New chat created.',
      };
    }
    case 'renameChat': {
      const chat = state.chats.find((entry) => entry.id === action.chatId);
      const title = typeof action.title === 'string' ? chatTitle(action.title) : '';
      if (!chat || !title || title === chat.title && !chat.nameFromFirstMessage) return state;
      return { ...updateChat(state, chat.id, (entry) => ({ ...entry, title, nameFromFirstMessage: false })), announcement: `Renamed to “${title}”.` };
    }
    case 'archiveChat': {
      const chat = state.chats.find((entry) => entry.id === action.chatId);
      if (!chat || chat.archived) return state;
      const notice = { chatId: chat.id, title: chat.title, activeChatId: state.activeChatId, pinnedChatIds: [...state.pinnedChatIds], recentChatIds: [...state.recentChatIds] };
      const base = state.suppliedRun?.chatId === chat.id ? stopSuppliedRun(state) : state;
      let next: PreviewState = {
        ...base,
        chats: base.chats.map((entry) => (entry.id === chat.id ? { ...entry, archived: true } : entry)),
        pinnedChatIds: base.pinnedChatIds.filter((id) => id !== chat.id),
        recentChatIds: base.recentChatIds.filter((id) => id !== chat.id),
      };
      if (base.activeChatId === chat.id) {
        const remaining = next.chats.filter((entry) => !entry.archived);
        // The most recently viewed remaining chat, then the newest one; with none left, start an empty chat.
        const fallback = next.recentChatIds.map((id) => remaining.find((entry) => entry.id === id)).find((entry) => entry !== undefined) ?? remaining.at(-1);
        next = fallback
          ? { ...next, activeChatId: fallback.id, inspectedChildId: null, recentChatIds: recordRecent(next.recentChatIds, fallback.id) }
          : createChat(next, {}, chat.projectId);
      }
      return { ...next, archiveNotice: notice, announcement: `Archived “${chat.title}”.` };
    }
    case 'unarchiveChat': {
      const chat = state.chats.find((entry) => entry.id === action.chatId);
      if (!chat?.archived) return state;
      const notice = state.archiveNotice?.chatId === chat.id ? state.archiveNotice : undefined;
      const restored = { ...updateChat(state, chat.id, ({ archived: _archived, ...entry }) => entry), announcement: `Restored “${chat.title}”.` };
      if (!notice) return restored;
      const { archiveNotice: _undone, ...rest } = restored;
      const returning = notice.activeChatId !== state.activeChatId && rest.chats.some((entry) => entry.id === notice.activeChatId && !entry.archived);
      return {
        ...(returning ? stopSuppliedRun(rest) : rest),
        ...(returning ? { activeChatId: notice.activeChatId, inspectedChildId: null } : {}),
        pinnedChatIds: notice.pinnedChatIds.filter((id) => rest.chats.some((entry) => entry.id === id && !entry.archived)),
        recentChatIds: notice.recentChatIds.filter((id) => rest.chats.some((entry) => entry.id === id && !entry.archived)),
        announcement: `Restored “${chat.title}”.`,
      };
    }
    case 'dismissArchiveNotice': {
      if (!state.archiveNotice) return state;
      const { archiveNotice: _dismissed, ...rest } = state;
      return rest;
    }
    case 'resolvePermission': {
      // Only the viewed parent's own request, once, with a decision it offered.
      if (state.inspectedChildId || action.chatId !== state.activeChatId) return state;
      const chat = state.chats.find((entry) => entry.id === action.chatId);
      const message = chat?.messages.find((entry) => entry.id === action.messageId);
      if (!chat || chat.archived || message?.role !== 'assistant' || !message.permission || message.permissionDecision
        || message.permission.state !== 'ready' || !message.permission.decisions.includes(action.decision)) return state;
      return updateChat(state, chat.id, (entry) => ({
        ...entry,
        // The answer is what Attention was waiting for; every surface follows the chat status.
        status: entry.status === 'approval' ? 'idle' : entry.status,
        messages: entry.messages.map((item) => (item === message ? { ...message, permissionDecision: action.decision } : item)),
      }));
    }
    case 'branchChat': {
      // Branch points are the user's own messages; the branch copies the history up to and including one.
      const source = state.chats.find((entry) => entry.id === action.chatId);
      const end = source?.messages.findIndex((message) => message.id === action.messageId && message.role === 'user') ?? -1;
      if (!source || end < 0) return state;
      const stopped = stopSuppliedRun(state);
      const title = `${chatTitle(source.title, 80 - branchSuffix.length)}${branchSuffix}`;
      return {
        ...appendChat(stopped, {
          id: `local-chat-${stopped.nextChatNumber}`,
          projectId: source.projectId,
          title,
          status: 'idle',
          draft: '',
          model: source.model,
          mode: source.mode,
          contextIds: [],
          composerExpanded: false,
          messages: source.messages.slice(0, end + 1).map(branchMessage),
          changes: [],
        }),
        announcement: `Created “${title}” from the chosen message.`,
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
      return next === state ? state : { ...next, announcement: 'Context added to this chat.' };
    }
    case 'removeContext': {
      if (state.inspectedChildId) return state;
      const next = updateChat(state, action.chatId, (chat) => !chat.contextIds.includes(action.contextId)
        ? chat : { ...chat, contextIds: chat.contextIds.filter((id) => id !== action.contextId) });
      return next === state ? state : { ...next, announcement: 'Context removed from this chat.' };
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
        announcement: `Viewing ${child.title}, read only. The parent chat is unchanged.`,
      };
    }
    case 'backToParent':
      return state.inspectedChildId ? { ...state, inspectedChildId: null, announcement: 'Back to the parent chat. Its draft and settings are unchanged.' } : state;
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
          ...(entry.nameFromFirstMessage ? { nameFromFirstMessage: false, title: chatTitle(text, 60) } : {}),
          messages: [
            ...entry.messages,
            // Selection at Send is not an invocation or an assistant attribution.
            { id: `local-message-${state.nextMessageNumber}`, role: 'user', origin: 'local', text, selectedModel: { ...selectedModel } },
          ],
        })),
        nextMessageNumber: state.nextMessageNumber + 1,
        announcement: 'Message added.',
      };
    }
    case 'stopViewedMock': {
      if (state.inspectedChildId) return state;
      if (state.suppliedRun?.state === 'streaming' && state.suppliedRun.chatId === state.activeChatId) return stopSuppliedRun(state);
      const chat = state.chats.find((entry) => entry.id === state.activeChatId)!;
      if (chat.status !== 'working') return state;
      return {
        ...updateChat(state, chat.id, (entry) => ({ ...entry, status: 'stopped' })),
        announcement: `Stopped ${chat.title}.`,
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
