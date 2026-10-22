import type { Ref } from 'react';
import { ArrowLeftIcon } from '@phosphor-icons/react/dist/csr/ArrowLeft';
import styles from './ConversationIdentity.module.css';

interface ConversationIdentityProps {
  title: string;
  titleRef: Ref<HTMLHeadingElement>;
  backDescription?: string;
  onBack?: () => void;
}

export function ConversationIdentity({ title, titleRef, backDescription, onBack }: ConversationIdentityProps) {
  return (
    <div className={styles.inset}>
      <div className={styles.identity}>
        <div className={styles.heading}>
          <h1 id="conversation-title" ref={titleRef} tabIndex={-1}>{title}</h1>
        </div>
        {onBack ? (
          <button type="button" onClick={onBack} title={backDescription}>
            <ArrowLeftIcon size={14} aria-hidden="true" />Back to parent
          </button>
        ) : null}
      </div>
    </div>
  );
}
