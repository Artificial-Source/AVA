import { useId, useRef, useState, type Dispatch, type RefObject } from 'react';
import { createPortal } from 'react-dom';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import { CommandIcon } from '@phosphor-icons/react/dist/csr/Command';
import { DotsThreeIcon } from '@phosphor-icons/react/dist/csr/DotsThree';
import { FolderOpenIcon } from '@phosphor-icons/react/dist/csr/FolderOpen';
import { FolderSimpleIcon } from '@phosphor-icons/react/dist/csr/FolderSimple';
import { GearFineIcon } from '@phosphor-icons/react/dist/csr/GearFine';
import { MagnifyingGlassIcon } from '@phosphor-icons/react/dist/csr/MagnifyingGlass';
import { PlusIcon } from '@phosphor-icons/react/dist/csr/Plus';
import { PushPinIcon } from '@phosphor-icons/react/dist/csr/PushPin';
import { QuestionIcon } from '@phosphor-icons/react/dist/csr/Question';
import { SidebarSimpleIcon } from '@phosphor-icons/react/dist/csr/SidebarSimple';
import { XIcon } from '@phosphor-icons/react/dist/csr/X';
import { shortcutHint, type ShortcutPreferences } from '../screens/shortcuts';
import type { Chat, PreviewAction, PreviewState } from '../state/types';
import { IconButton } from '../ui/Button';
import { canFocus } from '../ui/DialogFrame';
import { Disclosure } from '../ui/Disclosure';
import { AvaMark } from './AvaMark';
import { anchorFromElement, anchorFromPointer, ChatActionsMenu, type ChatAction, type MenuAnchor } from './ChatActionsMenu';
import { ChatStatus } from './ChatStatus';
import styles from './ProjectSidebar.module.css';

type RowLocation = 'project' | 'pinned' | 'recent';
// `layer` is the app root: the sidebar clips its overflow, so the menu renders beside it, not inside.
type MenuState = Readonly<{ chatId: string; location: RowLocation; anchor: MenuAnchor; returnFocus: HTMLElement; layer: HTMLElement }>;

// A collapsed project still surfaces unsettled work, most urgent first.
const rollupOrder = [
  { status: 'approval', one: 'needs attention', many: 'need attention' },
  { status: 'failed', one: 'failed', many: 'failed' },
  { status: 'working', one: 'working', many: 'working' },
] as const;

interface ProjectSidebarProps {
  state: PreviewState;
  dispatch: Dispatch<PreviewAction>;
  onOpenSettings?: () => void;
  settingsActive?: boolean;
  settingsRef?: RefObject<HTMLButtonElement | null>;
  onNewChat?: (projectId?: string) => void;
  /** Opens Find a chat: titles and messages in every project, archived chats included. */
  onFindChat?: () => void;
  /** Opens the command palette. */
  onOpenCommands?: () => void;
  onChatAction?: (chatId: string, action: ChatAction) => void;
  /** Opens the project picker: the empty library's one action. */
  onOpenProject?: () => void;
  /** Opens Help. F1 reaches it from anywhere; it is not a remappable shortcut. */
  onOpenHelp?: () => void;
  collapsed?: boolean;
  onToggleCollapsed?: () => void;
  shortcuts?: ShortcutPreferences;
}

export function ProjectSidebar({ state, dispatch, onOpenSettings, settingsActive = false, settingsRef, onNewChat, onFindChat, onOpenCommands, onChatAction, onOpenProject, onOpenHelp, collapsed = false, onToggleCollapsed, shortcuts }: ProjectSidebarProps) {
  const libraryRef = useRef<HTMLDivElement>(null);
  const menuId = useId();
  const asideRef = useRef<HTMLElement>(null);
  const [menu, setMenu] = useState<MenuState | null>(null);
  const chats = new Map(state.chats.filter((chat) => !chat.archived).map((chat) => [chat.id, chat]));
  const projectNames = new Map(state.projects.map((project) => [project.id, project.name]));
  const pinnedChats = state.pinnedChatIds.flatMap((id) => chats.get(id) ?? []);
  // Shortcuts reference the library's entities, not copies. A pin needs no second recent shortcut.
  const recentChats = state.recentChatIds.flatMap((id) => state.pinnedChatIds.includes(id) ? [] : chats.get(id) ?? []);
  const active = settingsActive ? undefined : chats.get(state.activeChatId);
  // The open chat is marked once: its project row when visible, else its first shortcut, else its collapsed project.
  const currentIn: RowLocation | 'heading' | null = !active ? null
    : !state.collapsedProjectIds.includes(active.projectId) ? 'project'
      : pinnedChats.includes(active) ? 'pinned'
        : recentChats.includes(active) ? 'recent' : 'heading';
  const menuChat = menu && !collapsed && !settingsActive ? chats.get(menu.chatId) : undefined;
  const newChatHint = shortcutHint(shortcuts, 'newChat');
  const settingsHint = shortcutHint(shortcuts, 'settings');
  const findHint = shortcutHint(shortcuts, 'sessions');
  const commandsHint = shortcutHint(shortcuts, 'palette');

  function startChat(projectId?: string) {
    setMenu(null);
    if (onNewChat) onNewChat(projectId);
    else dispatch(projectId ? { type: 'newChat', projectId } : { type: 'newChat' });
  }

  function openMenu(chatId: string, location: RowLocation, anchor: MenuAnchor, returnFocus: HTMLElement) {
    const layer = asideRef.current?.parentElement ?? asideRef.current;
    if (layer) setMenu({ chatId, location, anchor, returnFocus, layer });
  }

  function dismissMenu() {
    if (!menu) return;
    if (canFocus(menu.returnFocus)) menu.returnFocus.focus({ preventScroll: true });
    setMenu(null);
  }

  function chatRow(chat: Chat, location: RowLocation) {
    const pinned = state.pinnedChatIds.includes(chat.id);
    const current = currentIn === location && chat.id === active?.id;
    const projectName = location === 'project' ? null : projectNames.get(chat.projectId) ?? null;
    const reachable = location !== 'project' || !state.collapsedProjectIds.includes(chat.projectId);
    const menuOpen = menuChat?.id === chat.id && menu?.location === location;
    return (
      <li
        className={styles.chatEntry}
        key={chat.id}
        data-chat-id={chat.id}
        data-current={current}
        data-menu-open={menuOpen || undefined}
        onContextMenu={onChatAction ? (event) => {
          const row = event.currentTarget.firstElementChild;
          if (!reachable || !(row instanceof HTMLElement)) return;
          event.preventDefault();
          // Keyboard-invoked context menus may report a point outside the row; anchor those to the row.
          const box = event.currentTarget.getBoundingClientRect();
          const atPointer = event.clientX >= box.left && event.clientX <= box.right && event.clientY >= box.top && event.clientY <= box.bottom;
          openMenu(chat.id, location, atPointer ? anchorFromPointer(event) : anchorFromElement(row), row);
        } : undefined}
      >
        <button
          className={styles.chatRow}
          type="button"
          title={projectName ? `${chat.title} · ${projectName}` : chat.title}
          aria-current={current ? 'page' : undefined}
          onClick={() => {
            setMenu(null);
            if (reachable) dispatch({ type: 'viewChat', chatId: chat.id });
          }}
          onKeyDown={(event) => {
            if (event.key !== 'F2' || event.repeat || event.shiftKey || event.altKey || event.ctrlKey || event.metaKey || !onChatAction || !reachable) return;
            event.preventDefault();
            onChatAction(chat.id, 'rename');
          }}
        >
          <span className={styles.chatLine}>
            <span className={styles.glyph}><ChatStatus status={chat.status} part="glyph" /></span>
            <span className={styles.chatTitle}>{chat.title}</span>
          </span>
          {chat.status !== 'idle' || projectName ? <>{' '}<span className={styles.chatMeta}>
            <ChatStatus status={chat.status} part="label" />
            {chat.status !== 'idle' && projectName ? <span className={styles.metaDot} aria-hidden="true">·</span> : null}
            {projectName ? <>{' '}<span className={styles.metaProject}>{projectName}</span></> : null}
          </span></> : null}
        </button>
        <div className={styles.rowActions}>
          <button
            className={styles.rowAction}
            type="button"
            data-persistent={pinned && location !== 'pinned' || undefined}
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
              title="Remove from Recent"
              onClick={() => {
                dispatch({ type: 'removeRecentChat', chatId: chat.id });
                libraryRef.current?.focus();
              }}
            >
              <XIcon size={12} aria-hidden="true" />
            </button>
          ) : null}
          {onChatAction ? (
            <button
              className={styles.rowAction}
              type="button"
              aria-label={`Chat actions for ${chat.title}`}
              aria-haspopup="menu"
              aria-expanded={menuOpen}
              aria-controls={menuOpen ? menuId : undefined}
              title="More actions"
              onClick={(event) => {
                if (menuOpen) setMenu(null);
                else if (reachable) openMenu(chat.id, location, anchorFromElement(event.currentTarget), event.currentTarget);
              }}
            >
              <DotsThreeIcon size={16} weight="bold" aria-hidden="true" />
            </button>
          ) : null}
        </div>
      </li>
    );
  }

  return (
    <aside ref={asideRef} className={`${styles.sidebar} material`} data-collapsed={collapsed} hidden={settingsActive} inert={settingsActive} aria-label="Projects and chats">
      <div className={styles.brand}>
        <AvaMark />
        <span>AVA</span>
        {onToggleCollapsed ? (
          <IconButton
            className={styles.collapseToggle}
            variant="quiet"
            label={collapsed ? 'Expand sidebar' : 'Collapse sidebar'}
            title={collapsed ? 'Expand sidebar' : 'Collapse sidebar'}
            onClick={() => {
              setMenu(null);
              onToggleCollapsed();
            }}
          >
            <SidebarSimpleIcon size={17} aria-hidden="true" />
          </IconButton>
        ) : null}
      </div>
      <div className={styles.actions}>
        <button
          className={styles.newChat}
          type="button"
          onClick={() => startChat()}
          title={newChatHint ? `New chat · ${newChatHint.label}` : 'New chat'}
          aria-keyshortcuts={newChatHint?.aria}
        >
          <PlusIcon size={17} aria-hidden="true" />
          <span className={collapsed ? 'srOnly' : undefined}>New chat</span>
        </button>
        {onFindChat || onOpenCommands ? (
          // Quiet companions to New chat: finding and commands, with the keys that reach them from anywhere.
          <div className={styles.find}>
            {onFindChat ? (
              <button
                className={styles.findChat}
                type="button"
                onClick={() => {
                  setMenu(null);
                  onFindChat();
                }}
                title={findHint ? `Find a chat · ${findHint.label}` : 'Find a chat'}
                aria-keyshortcuts={findHint?.aria}
              >
                <MagnifyingGlassIcon size={17} aria-hidden="true" />
                <span className={collapsed ? 'srOnly' : styles.findLabel}>Find a chat</span>
                {findHint && !collapsed ? <kbd className={styles.hint} aria-hidden="true">{findHint.label}</kbd> : null}
              </button>
            ) : null}
            {onOpenCommands ? (
              <IconButton
                className={styles.commands}
                variant="quiet"
                label="Commands"
                title={commandsHint ? `Commands · ${commandsHint.label}` : 'Commands'}
                aria-keyshortcuts={commandsHint?.aria}
                onClick={() => {
                  setMenu(null);
                  onOpenCommands();
                }}
              >
                <CommandIcon size={16} aria-hidden="true" />
              </IconButton>
            ) : null}
          </div>
        ) : null}
      </div>
      <div ref={libraryRef} className={styles.library} role="region" aria-label="Chat library" tabIndex={0} hidden={collapsed} onScroll={dismissMenu}>
        <nav aria-label="All project chats">
          <div className={styles.sectionLabel}>
            <h2>Projects</h2>
            {state.projects.length > 0 ? <span>{state.projects.length}</span> : null}
          </div>
          {state.projects.length === 0 ? (
            <div className={styles.emptyLibrary}>
              <p>Projects you open appear here.</p>
              {onOpenProject ? (
                <button className={styles.openProject} type="button" onClick={onOpenProject}>
                  <FolderOpenIcon size={16} aria-hidden="true" />
                  Open a project
                </button>
              ) : null}
            </div>
          ) : null}
          {state.projects.map((project) => {
            const expanded = !state.collapsedProjectIds.includes(project.id);
            const projectChats = state.chats.filter((chat) => chat.projectId === project.id && chats.has(chat.id));
            const rollup = expanded ? [] : rollupOrder.flatMap(({ status, one, many }) => {
              const count = projectChats.filter((chat) => chat.status === status).length;
              return count > 0 ? [{ status, count, text: `${count} ${count === 1 ? one : many}` }] : [];
            });
            const holdsCurrent = currentIn === 'heading' && active?.projectId === project.id;
            const spoken = [...rollup.map((entry) => entry.text), ...(holdsCurrent ? ['has the open chat'] : [])];
            return (
              <section className={styles.project} key={project.id} aria-label={`${project.name} project`} data-holds-current={holdsCurrent || undefined}>
                <Disclosure
                  id={`project-${project.id}`}
                  open={expanded}
                  triggerClassName={styles.projectHeading}
                  onOpenChange={() => {
                    setMenu(null);
                    dispatch({ type: 'toggleProject', projectId: project.id });
                  }}
                  trigger={<>
                    <FolderSimpleIcon size={17} aria-hidden="true" />
                    <span className={styles.projectName}>{project.name}</span>
                    {rollup.length > 0 ? (
                      <span className={styles.rollup} aria-hidden="true">
                        {rollup.map((entry) => <ChatStatus key={entry.status} status={entry.status} label={String(entry.count)} />)}
                      </span>
                    ) : null}
                    {spoken.length > 0 ? <span className="srOnly">{spoken.map((text) => `, ${text}`).join('')}</span> : null}
                    <CaretDownIcon className={styles.caret} size={12} aria-hidden="true" />
                  </>}
                  accessory={(
                    <button
                      className={styles.projectAction}
                      type="button"
                      aria-label={`New chat in ${project.name}`}
                      title={`New chat in ${project.name}`}
                      onClick={() => startChat(project.id)}
                    >
                      <PlusIcon size={14} aria-hidden="true" />
                    </button>
                  )}
                >
                  {projectChats.length > 0 ? (
                    <ul className={styles.chatList}>
                      {projectChats.map((chat) => chatRow(chat, 'project'))}
                    </ul>
                  ) : <p className={styles.emptyProject}>No chats yet</p>}
                </Disclosure>
              </section>
            );
          })}
        </nav>
        {pinnedChats.length > 0 ? (
          <nav className={styles.shortcuts} aria-label="Pinned chats">
            <div className={styles.sectionLabel}><h2>Pinned</h2></div>
            <ul className={styles.chatList}>
              {pinnedChats.map((chat) => chatRow(chat, 'pinned'))}
            </ul>
          </nav>
        ) : null}
        {recentChats.length > 0 ? (
          <nav className={styles.shortcuts} aria-label="Recent chats">
            <div className={styles.sectionLabel}><h2>Recent</h2></div>
            <ul className={styles.chatList}>
              {recentChats.map((chat) => chatRow(chat, 'recent'))}
            </ul>
          </nav>
        ) : null}
      </div>
      {onOpenSettings || onOpenHelp ? (
        // Settings and its quiet companion, Help: the footer's only controls.
        <div className={styles.settings}>
          {onOpenSettings ? (
            <button
              ref={settingsRef}
              className={styles.settingsButton}
              type="button"
              onClick={onOpenSettings}
              aria-current={settingsActive ? 'page' : undefined}
              title={settingsHint ? `Settings · ${settingsHint.label}` : 'Settings'}
              aria-keyshortcuts={settingsHint?.aria}
            >
              <GearFineIcon size={18} aria-hidden="true" />
              <span className={collapsed ? 'srOnly' : undefined}>Settings</span>
            </button>
          ) : null}
          {onOpenHelp ? (
            <IconButton
              className={styles.help}
              variant="quiet"
              label="Help"
              title="Help · F1"
              aria-keyshortcuts="F1"
              onClick={() => {
                setMenu(null);
                onOpenHelp();
              }}
            >
              <QuestionIcon size={18} aria-hidden="true" />
            </IconButton>
          ) : null}
        </div>
      ) : null}
      {menu && menuChat ? createPortal(
        <ChatActionsMenu
          id={menuId}
          label={`Chat actions for ${menuChat.title}`}
          anchor={menu.anchor}
          returnFocus={menu.returnFocus}
          actions={menuChat.messages.length > 0 ? undefined : ['rename', 'archive']}
          renameKey="F2"
          onSelect={(action) => {
            setMenu(null);
            // The archived row leaves the list; keep focus on the stable library instead of a vanishing button.
            if (action === 'archive') libraryRef.current?.focus({ preventScroll: true });
            onChatAction?.(menuChat.id, action);
          }}
          onClose={() => setMenu(null)}
        />,
        menu.layer,
      ) : null}
    </aside>
  );
}
