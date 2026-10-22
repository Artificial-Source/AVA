import { LockSimpleIcon } from '@phosphor-icons/react/dist/csr/LockSimple';
import { modelOptions } from '../state/fixtures';
import type { ChildAgent } from '../state/types';
import { AvaMark } from './AvaMark';
import { ChatStatus } from './ChatStatus';
import { ModelMark } from './ModelMark';
import { MessageTurn } from '../chat/widgets/MessageTurn';
import transcript from './Conversation.module.css';
import styles from './ChildConversation.module.css';

export default function ChildConversation({ child }: { child: ChildAgent }) {
  const model = modelOptions.find((option) => option.id === child.model)!;
  return (
    <div className={transcript.transcript} role="region" tabIndex={0} aria-label={`Child conversation: ${child.title}`}>
      <div className={transcript.messages}>
        <div className={styles.snapshot} role="group" aria-label="Child agent">
          <ChatStatus status={child.status} />
          <span className={styles.facts}><ModelMark model={child.model} />{model.label}<span aria-hidden="true">·</span>{child.timing}</span>
          <span className={styles.readOnly}><LockSimpleIcon size={13} aria-hidden="true" />Read only</span>
        </div>
        {child.messages.map((message) => message.role === 'user' ? (
          <article key={message.id} className={transcript.userMessage} aria-label="Your message">
            <div className={transcript.userHeading}>You</div>
            <div className={transcript.userBubble}><p>{message.text}</p></div>
          </article>
        ) : (
          <MessageTurn key={message.id} turn={{ ...message, author: 'AVA · child', state: 'committed', label: 'Child agent response' }} mark={<AvaMark />} />
        ))}
        {child.status === 'unavailable' ? <p className={styles.unavailable}>No conversation to display.</p> : null}
      </div>
    </div>
  );
}
