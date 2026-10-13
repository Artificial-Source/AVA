import type { ChildAgent, PreviewState } from './types';

export function selectedParentChildren(state: PreviewState): ChildAgent[] {
  return state.childAgents
    .filter((child) => child.parentChatId === state.activeChatId)
    .sort((left, right) => Number(right.status === 'working') - Number(left.status === 'working'));
}

export function inspectedChild(state: PreviewState): ChildAgent | undefined {
  return state.childAgents.find((child) => child.id === state.inspectedChildId && child.parentChatId === state.activeChatId);
}
