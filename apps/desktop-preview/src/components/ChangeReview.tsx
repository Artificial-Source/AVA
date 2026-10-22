import { useMemo } from 'react';
import { ChangeReviewView } from '../chat/widgets/ChangeReviewView';
import { canonicalPath, chatChanges } from '../state/changeLedger';
import type { Chat } from '../state/types';

// Keyed by chat: which files are collapsed belongs to one chat's inventory, not to a path name.
export function ChangeReview({ chat, focusPath }: { chat: Chat; focusPath?: string | null }) {
  const changes = useMemo(() => chatChanges(chat), [chat]);
  return <ChangeReviewView key={chat.id} focusPath={focusPath ? canonicalPath(focusPath) : focusPath} view={changes.length ? { state: 'ready', changes } : { state: 'empty', message: 'No changes in this chat.' }} />;
}
