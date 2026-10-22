import type { ChatSummary } from '../screens/contracts';
import { openableProjects } from './fixtures';
import type { Chat, ChildAgent, PreviewState, Project } from './types';

// The viewed parent chat. Undefined only while nothing is open (after Start fresh).
export function activeChat(state: PreviewState): Chat | undefined {
  return state.chats.find((chat) => chat.id === state.activeChatId);
}

// Projects the project picker can still open: not in the sidebar yet.
export function projectsToOpen(state: PreviewState): readonly Project[] {
  return openableProjects.filter((project) => !state.projects.some((entry) => entry.id === project.id));
}

export function selectedParentChildren(state: PreviewState): ChildAgent[] {
  return state.childAgents
    .filter((child) => child.parentChatId === state.activeChatId)
    .sort((left, right) => Number(right.status === 'working') - Number(left.status === 'working'));
}

export function inspectedChild(state: PreviewState): ChildAgent | undefined {
  return state.childAgents.find((child) => child.id === state.inspectedChildId && child.parentChatId === state.activeChatId);
}

// A one-line preview bounded by code points. A cut ends on a word boundary and says so with an ellipsis.
function messagePreview(text: string, limit = 60) {
  const characters = Array.from(text.replace(/\s+/gu, ' ').trim());
  if (characters.length <= limit) return characters.join('');
  const cut = characters.slice(0, limit - 1).join('');
  const space = cut.lastIndexOf(' ');
  const head = (space > cut.length / 2 ? cut.slice(0, space) : cut).replace(/[\s.,;:!?…-]+$/u, '');
  return `${head || cut}…`;
}

// Markdown reduced to the words a reader sees: link text, no block, emphasis, code or table markers.
function plainText(markdown: string) {
  return markdown
    .replace(/\[([^\]]*)\]\([^)]*\)/gu, '$1')
    .replace(/^ {0,3}(?:#{1,6}|>|[-+*]|\d+[.)]) +/gmu, '')
    .replace(/^[ \t|:]*-[ \t|:-]*$/gmu, '')
    .replace(/[*`|~]+/gu, ' ');
}

// Every chat, archived included, as finders and chat dialogs see it. Branch points are the user's own messages:
// a one-line label to pick from, and the whole message (line breaks kept) to confirm the pick.
// searchText is one line each for title, project and every message, so a finder can quote the matching line.
export function chatSummaries(state: PreviewState): ChatSummary[] {
  return state.chats.map((chat) => {
    const projectLabel = state.projects.find((project) => project.id === chat.projectId)?.name ?? '';
    return {
      id: chat.id,
      title: chat.title,
      projectId: chat.projectId,
      projectLabel,
      status: chat.status,
      archived: chat.archived === true,
      current: chat.id === state.activeChatId,
      branchPoints: chat.messages.flatMap((message) => message.role === 'user' ? [{ id: message.id, label: messagePreview(message.text), text: message.text.trim() }] : []),
      searchText: [chat.title, projectLabel, ...chat.messages.map((message) => (message.role === 'user' ? message.text : plainText(message.markdown)).replace(/\s+/gu, ' ').trim())].filter(Boolean).join('\n'),
    };
  });
}
