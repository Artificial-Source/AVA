import { useId, useRef, useState, type ReactNode, type Ref } from 'react';
import { DotsThreeIcon } from '@phosphor-icons/react/dist/csr/DotsThree';
import { BackButton } from '../ui/Button';
import { anchorFromElement, ChatActionsMenu, type ChatAction, type MenuAnchor } from './ChatActionsMenu';
import styles from './ConversationIdentity.module.css';

interface ConversationIdentityProps {
  title: string;
  titleRef: Ref<HTMLHeadingElement>;
  backDescription?: string;
  onBack?: () => void;
  /** Parent chats only. Child views are read-only and have no chat actions. */
  onChatAction?: (action: ChatAction) => void;
  /** Trailing, right-aligned slot (the collapsed preview chip). */
  aside?: ReactNode;
}

export function ConversationIdentity({ title, titleRef, backDescription, onBack, onChatAction, aside }: ConversationIdentityProps) {
  const menuId = useId();
  const trigger = useRef<HTMLButtonElement>(null);
  const [anchor, setAnchor] = useState<MenuAnchor | null>(null);
  // A child view has no chat actions; drop any open menu rather than reviving it on return.
  if (anchor !== null && !onChatAction) setAnchor(null);
  const open = anchor !== null && onChatAction !== undefined;
  return (
    <div className={styles.inset}>
      <div className={styles.identity}>
        {/* F2 renames from the title or its ⋯ button, matching the sidebar row and the menu's hint. */}
        <div className={styles.heading} onKeyDown={onChatAction ? (event) => {
          if (event.key !== 'F2' || event.repeat || event.shiftKey || event.altKey || event.ctrlKey || event.metaKey || event.nativeEvent.isComposing) return;
          event.preventDefault();
          setAnchor(null);
          onChatAction('rename');
        } : undefined}>
          <h1 id="conversation-title" ref={titleRef} tabIndex={-1}>{title}</h1>
          {onChatAction ? (
            <button
              ref={trigger}
              className={styles.actions}
              type="button"
              aria-label="Chat actions"
              aria-haspopup="menu"
              aria-expanded={open}
              aria-controls={open ? menuId : undefined}
              title="Chat actions"
              onClick={(event) => setAnchor(open ? null : anchorFromElement(event.currentTarget))}
            ><DotsThreeIcon size={16} weight="bold" aria-hidden="true" /></button>
          ) : null}
        </div>
        {onBack ? (
          <BackButton onClick={onBack} title={backDescription} aria-keyshortcuts="Escape">Back to parent</BackButton>
        ) : null}
        {aside ? <div className={styles.aside}>{aside}</div> : null}
      </div>
      {anchor !== null && onChatAction ? (
        <ChatActionsMenu
          id={menuId}
          label={`Chat actions for ${title}`}
          anchor={anchor}
          returnFocus={trigger.current}
          renameKey="F2"
          onSelect={(action) => { setAnchor(null); onChatAction(action); }}
          onClose={() => setAnchor(null)}
        />
      ) : null}
    </div>
  );
}
