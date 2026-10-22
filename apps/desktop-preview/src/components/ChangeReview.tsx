import { ChangeReviewView } from '../chat/widgets/ChangeReviewView';
import type { Chat } from '../state/types';

export function ChangeReview({ chat }: { chat: Chat }) {
  return <ChangeReviewView
    notice="Invented excerpts for this chat. No files were accessed or changed."
    view={chat.changes.length === 0
      ? { state: 'empty', message: 'No changes in this chat.' }
      : { state: 'ready', changes: chat.changes.map((change) => ({ ...change, id: change.filename })) }}
  />;
}
