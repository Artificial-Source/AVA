import { useRef, type Dispatch, type RefObject } from 'react';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import { CircleHalfIcon } from '@phosphor-icons/react/dist/csr/CircleHalf';
import { GearFineIcon } from '@phosphor-icons/react/dist/csr/GearFine';
import { FolderSimpleIcon } from '@phosphor-icons/react/dist/csr/FolderSimple';
import { PlusIcon } from '@phosphor-icons/react/dist/csr/Plus';
import { PushPinIcon } from '@phosphor-icons/react/dist/csr/PushPin';
import { XIcon } from '@phosphor-icons/react/dist/csr/X';
import type { Chat, PreviewAction, PreviewState } from '../state/types';
import { Disclosure } from '../ui/Disclosure';
import { AvaMark } from './AvaMark';
import { ChatStatus } from './ChatStatus';
import styles from './ProjectSidebar.module.css';

interface ProjectSidebarProps {
  state: PreviewState;
  dispatch: Dispatch<PreviewAction>;
  onOpenSettings?: () => void;
  settingsActive?: boolean;
  settingsRef?: RefObject<HTMLButtonElement | null>;
  onNewChat?: () => void;
}

export function ProjectSidebar({ state, dispatch, onOpenSettings, settingsActive = false, settingsRef, onNewChat }: ProjectSidebarProps) {
  const libraryRef = useRef<HTMLDivElement>(null);
  // Shortcuts reference the library's entities, not copies. A pin needs no second recent shortcut.
  const recentChatIds = state.recentChatIds.filter((id) => !state.pinnedChatIds.includes(id));

  function chatRow(chat: Chat, location: 'project' | 'pinned' | 'recent') {
    const pinned = state.pinnedChatIds.includes(chat.id);
    const current = !settingsActive && chat.id === state.activeChatId;
    const projectName = location === 'project' ? null : state.projects.find((project) => project.id === chat.projectId)!.name;
    const reachable = location !== 'project' || !state.collapsedProjectIds.includes(chat.projectId);
    return (
      <li className={styles.chatEntry} key={chat.id} data-chat-id={chat.id} data-current={current}>
        <button
          className={styles.chatRow}
          type="button"
          title={projectName ? `${chat.title} · ${projectName}` : chat.title}
          aria-current={current ? 'page' : undefined}
          onClick={() => { if (reachable) dispatch({ type: 'viewChat', chatId: chat.id }); }}
        >
          <span className={styles.chatLine}>
            <ChatStatus status={chat.status} />{' '}
            <span className={styles.chatTitle}>{chat.title}</span>
          </span>{' '}
          {projectName ? <span className={styles.chatMeta}>{projectName}</span> : null}
        </button>
        <div className={styles.rowActions}>
          <button
            className={styles.rowAction}
            type="button"
            aria-label={`${pinned ? 'Unpin' : 'Pin'} ${chat.title}`}
            aria-pressed={pinned}
            title={pinned ? 'Unpin chat' : 'Pin chat'}
            onClick={() => {
              if (!reachable) return;
              dispatch({ type: pinned ? 'unpinChat' : 'pinChat', chatId: chat.id });
              if (location !== 'project') libraryRef.current?.focus();
            }}
          >
            <PushPinIcon size={13} weight={pinned ? 'fill' : 'regular'} aria-hidden="true" />
          </button>
          {location === 'recent' ? (
            <button
              className={styles.rowAction}
              type="button"
              aria-label={`Remove ${chat.title} from recent chats`}
              title="Remove recent shortcut only"
              onClick={() => {
                dispatch({ type: 'removeRecentChat', chatId: chat.id });
                libraryRef.current?.focus();
              }}
            >
              <XIcon size={12} aria-hidden="true" />
            </button>
          ) : null}
        </div>
      </li>
    );
  }

  return (
    <aside className={`${styles.sidebar} material`} hidden={settingsActive} inert={settingsActive} aria-label="Projects and chats">
      <div className={styles.brand}>
        <AvaMark />
        <span>AVA</span>
        <button
          className={`${styles.materialToggle} iconButton`}
          type="button"
          aria-label="Opaque materials"
          aria-pressed={state.opaque}
          onClick={() => dispatch({ type: 'toggleOpaque' })}
          title="Use opaque materials. System reduced-transparency preferences also apply."
        >
          <CircleHalfIcon size={16} aria-hidden="true" />
        </button>
      </div>
      <div className={styles.actions}>
        <button
          className={styles.newChat}
          type="button"
          onClick={onNewChat ?? (() => dispatch({ type: 'newChat' }))}
          title="Create an in-memory chat in the viewed project"
        >
          <PlusIcon size={17} aria-hidden="true" />
          New chat
        </button>
      </div>
      <div ref={libraryRef} className={styles.library} role="region" aria-label="Chat library" tabIndex={0}>
        <nav aria-label="All project chats">
          <div className={styles.sectionLabel}>
            <h2>Projects</h2>
            <span>{state.projects.length}</span>
          </div>
          {state.projects.map((project) => {
            const expanded = !state.collapsedProjectIds.includes(project.id);
            const chats = state.chats.filter((chat) => chat.projectId === project.id);
            return (
              <section className={styles.project} key={project.id} aria-label={`${project.name} project`}>
                <Disclosure
                  id={`project-${project.id}`}
                  open={expanded}
                  triggerClassName={styles.projectHeading}
                  onOpenChange={() => dispatch({ type: 'toggleProject', projectId: project.id })}
                  trigger={<>
                    <FolderSimpleIcon size={17} aria-hidden="true" />
                    <span>{project.name}</span>
                    <CaretDownIcon className={styles.caret} size={12} aria-hidden="true" />
                  </>}
                >
                  <ul className={styles.chatList}>
                    {chats.map((chat) => chatRow(chat, 'project'))}
                  </ul>
                </Disclosure>
              </section>
            );
          })}
        </nav>
        {state.pinnedChatIds.length > 0 ? (
          <nav className={styles.shortcuts} aria-label="Pinned chats">
            <div className={styles.sectionLabel}><h2>Pinned</h2></div>
            <ul className={styles.chatList}>
              {state.pinnedChatIds.map((id) => chatRow(state.chats.find((chat) => chat.id === id)!, 'pinned'))}
            </ul>
          </nav>
        ) : null}
        {recentChatIds.length > 0 ? (
          <nav className={styles.shortcuts} aria-label="Recent chats">
            <div className={styles.sectionLabel}><h2>Recent</h2></div>
            <ul className={styles.chatList}>
              {recentChatIds.map((id) => chatRow(state.chats.find((chat) => chat.id === id)!, 'recent'))}
            </ul>
          </nav>
        ) : null}
      </div>
      {onOpenSettings ? <div className={styles.settings}><button ref={settingsRef} type="button" onClick={onOpenSettings} aria-current={settingsActive ? 'page' : undefined}><GearFineIcon size={18} aria-hidden="true" />Settings</button></div> : null}
    </aside>
  );
}
