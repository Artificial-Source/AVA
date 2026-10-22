import { Fragment, useEffect, useLayoutEffect, useRef, type KeyboardEvent } from 'react';
import { ArchiveIcon } from '@phosphor-icons/react/dist/csr/Archive';
import { GitBranchIcon } from '@phosphor-icons/react/dist/csr/GitBranch';
import { PencilSimpleIcon } from '@phosphor-icons/react/dist/csr/PencilSimple';
import { canFocus } from '../ui/DialogFrame';
import styles from './ChatActionsMenu.module.css';

export type ChatAction = 'rename' | 'branch' | 'archive';
/** Viewport rect the menu opens from. `end` right-aligns to a trigger; `start` opens at a pointer. */
export type MenuAnchor = Readonly<{ top: number; bottom: number; left: number; right: number; align: 'start' | 'end' }>;
export type MenuCloseReason = 'escape' | 'tab' | 'outside' | 'dismiss';

const entries = [
  { action: 'rename', label: 'Rename', Icon: PencilSimpleIcon },
  { action: 'branch', label: 'Branch from a message…', Icon: GitBranchIcon },
  { action: 'archive', label: 'Archive', Icon: ArchiveIcon },
] as const;
const allActions: readonly ChatAction[] = ['rename', 'branch', 'archive'];
const GAP = 4;
const EDGE = 8;

export function anchorFromElement(element: Element): MenuAnchor {
  const { top, bottom, left, right } = element.getBoundingClientRect();
  return { top, bottom, left, right, align: 'end' };
}

export function anchorFromPointer({ clientX, clientY }: Readonly<{ clientX: number; clientY: number }>): MenuAnchor {
  return { top: clientY, bottom: clientY, left: clientX, right: clientX, align: 'start' };
}

interface ChatActionsMenuProps {
  id: string;
  label: string;
  anchor: MenuAnchor;
  /** Focus target on Escape, Tab, dismissal and selection. Pointer presses on it are not "outside". */
  returnFocus: HTMLElement | null;
  actions?: readonly ChatAction[];
  /** Shown beside Rename and exposed as aria-keyshortcuts when the owner handles that key. */
  renameKey?: string;
  onSelect: (action: ChatAction) => void;
  onClose: (reason: MenuCloseReason) => void;
}

// Render only while open, inside a positioned ancestor that does not clip the menu.
export function ChatActionsMenu({ id, label, anchor, returnFocus, actions = allActions, renameKey, onSelect, onClose }: ChatActionsMenuProps) {
  const ref = useRef<HTMLDivElement>(null);
  const latest = useRef({ returnFocus, onClose });
  latest.current = { returnFocus, onClose };

  useLayoutEffect(() => {
    const menu = ref.current;
    if (!menu) return;
    // Focus never depends on layout: without an offsetParent the menu keeps its default spot but still takes focus.
    const parent = menu.offsetParent;
    if (parent instanceof HTMLElement) {
      const box = parent.getBoundingClientRect();
      const width = menu.offsetWidth;
      const height = menu.offsetHeight;
      const preferredX = anchor.align === 'end' ? anchor.right - width : anchor.left;
      const x = Math.max(box.left + EDGE, Math.min(preferredX, box.right - EDGE - width));
      const below = anchor.bottom + GAP;
      const y = below + height <= box.bottom - EDGE ? below : Math.max(box.top + EDGE, anchor.top - GAP - height);
      menu.style.left = `${x - box.left - parent.clientLeft + parent.scrollLeft}px`;
      menu.style.top = `${y - box.top - parent.clientTop + parent.scrollTop}px`;
      menu.dataset.side = y < anchor.top ? 'above' : 'below';
    }
    menu.querySelector<HTMLElement>('[role="menuitem"]')?.focus({ preventScroll: true });
  }, [anchor]);

  useEffect(() => {
    function pointerDown(event: PointerEvent) {
      const target = event.target;
      if (target instanceof Node && (ref.current?.contains(target) || latest.current.returnFocus?.contains(target))) return;
      latest.current.onClose('outside');
    }
    function resize() { close('dismiss'); }
    document.addEventListener('pointerdown', pointerDown, true);
    window.addEventListener('resize', resize);
    return () => {
      document.removeEventListener('pointerdown', pointerDown, true);
      window.removeEventListener('resize', resize);
    };
  }, []);

  function restoreFocus() {
    const target = latest.current.returnFocus;
    if (canFocus(target)) target.focus({ preventScroll: true });
  }

  function close(reason: MenuCloseReason) {
    restoreFocus();
    latest.current.onClose(reason);
  }

  function handleKeyDown(event: KeyboardEvent<HTMLDivElement>) {
    if (event.nativeEvent.isComposing || event.keyCode === 229) return;
    const items = Array.from(event.currentTarget.querySelectorAll<HTMLElement>('[role="menuitem"]'));
    const index = items.indexOf(document.activeElement as HTMLElement);
    const focusAt = (next: number) => {
      event.preventDefault();
      items[(next + items.length) % items.length]?.focus();
    };
    if (event.key === 'ArrowDown') focusAt(index + 1);
    else if (event.key === 'ArrowUp') focusAt(index < 0 ? items.length - 1 : index - 1);
    else if (event.key === 'Home') focusAt(0);
    else if (event.key === 'End') focusAt(items.length - 1);
    else if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      close('escape');
    } else if (event.key === 'Tab') {
      // Focus returns to the trigger; the native Tab then continues from there.
      close('tab');
    } else if (event.key.length === 1 && event.key !== ' ' && !event.ctrlKey && !event.metaKey && !event.altKey) {
      const key = event.key.toLowerCase();
      const order = [...items.slice(index + 1), ...items.slice(0, index + 1)];
      const match = order.find((item) => item.dataset.label?.toLowerCase().startsWith(key));
      if (match) {
        event.preventDefault();
        match.focus();
      }
    }
  }

  const visible = entries.filter((entry) => actions.includes(entry.action));
  return (
    <div ref={ref} id={id} className={styles.menu} role="menu" aria-label={label} aria-orientation="vertical" onKeyDown={handleKeyDown}>
      {visible.map(({ action, label: itemLabel, Icon }, index) => {
        const key = action === 'rename' ? renameKey : undefined;
        return (
          <Fragment key={action}>
            {action === 'archive' && index > 0 ? <div className={styles.separator} role="separator" /> : null}
            <button
              className={styles.item}
              type="button"
              role="menuitem"
              tabIndex={-1}
              data-label={itemLabel}
              aria-keyshortcuts={key}
              onClick={() => {
                restoreFocus();
                onSelect(action);
              }}
            >
              <Icon size={15} aria-hidden="true" />
              <span>{itemLabel}</span>
              {key ? <kbd className={styles.key} aria-hidden="true">{key}</kbd> : null}
            </button>
          </Fragment>
        );
      })}
    </div>
  );
}
