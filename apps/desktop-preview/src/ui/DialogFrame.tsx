import { useId, useLayoutEffect, useRef, type ComponentProps, type ReactNode, type RefObject } from 'react';
import { XIcon } from '@phosphor-icons/react/dist/csr/X';
import { IconButton } from './Button';
import styles from './DialogFrame.module.css';

export type DialogChrome = {
  onDismiss: () => void;
  returnFocus?: RefObject<HTMLElement | null>;
  toolbar?: ReactNode;
};

export function canFocus(element: HTMLElement | null): element is HTMLElement {
  if (!element?.isConnected || element.matches(':disabled') || element.closest('[hidden], [inert], [aria-hidden="true"], dialog:not([open])')) return false;
  for (let node: HTMLElement | null = element; node; node = node.parentElement) {
    const style = getComputedStyle(node);
    if (style.display === 'none' || style.visibility === 'hidden') return false;
  }
  return true;
}

const fields = 'input:not([type="hidden"]), textarea, select, [data-autofocus]';
const focusables = 'a[href], button, input:not([type="hidden"]), textarea, select, summary, [tabindex]:not([tabindex="-1"]), [contenteditable="true"]';

// A radio group is one Tab stop: land on its checked option, as Tab would.
function tabStop(node: HTMLElement) {
  if (!(node instanceof HTMLInputElement) || node.type !== 'radio' || node.checked || !node.name) return node;
  const scope = node.form ?? node.closest('dialog');
  const checked = scope ? Array.from(scope.querySelectorAll<HTMLInputElement>('input[type="radio"]')).find((radio) => radio.name === node.name && radio.checked) : undefined;
  return checked && canFocus(checked) ? checked : node;
}

// Caller target, then the first field, then the first control in the task
// content (body, then footer), then the review toolbar, then Close. Stops at the
// first that takes focus.
function focusInitial(task: readonly (HTMLElement | null)[], toolbar: HTMLElement | null, preferred: HTMLElement | null | undefined, close: HTMLElement | null) {
  const regions = task.filter((region): region is HTMLElement => region !== null);
  const controls = [...regions.flatMap((region) => [...region.querySelectorAll<HTMLElement>(fields)]), ...regions.flatMap((region) => [...region.querySelectorAll<HTMLElement>(focusables)])].filter((node) => !toolbar?.contains(node));
  const candidates = [preferred, ...controls, ...(toolbar ? toolbar.querySelectorAll<HTMLElement>(focusables) : []), close];
  for (const candidate of candidates) {
    const target = candidate === preferred ? candidate : candidate && tabStop(candidate);
    if (!target || !canFocus(target)) continue;
    target.focus();
    if (target.ownerDocument.activeElement === target) return;
  }
}

// Input types that take typed text; an invalid `type` reads back as 'text'.
const textTypes: Readonly<Partial<Record<string, true>>> = { text: true, search: true, email: true, url: true, tel: true, password: true, number: true };
function takesText(node: Element): node is HTMLElement {
  return node instanceof HTMLTextAreaElement || node instanceof HTMLInputElement && textTypes[node.type] === true || node instanceof HTMLElement && node.isContentEditable;
}

// Chromium can leave a text field deaf after a modal dialog closes: focus that arrives while the
// closed dialog is still in the page, or in the same task it leaves, reports the field as focused,
// yet typing inserts nothing until the field is focused again. So on the next frame, once the dialog
// is gone, a focused text field (the one focus returned to, or one the host focused after the close)
// is blurred and focused again in place, and focus that fell to the page goes to the return target.
// Focus is never moved off an element. Only the latest close's frame acts: every open and close bumps
// the generation, so a dialog that opens before the frame voids it (no frame ids to cancel or outlive).
let settleGeneration = 0;
function settleFocus(returnTarget: () => HTMLElement | null | undefined) {
  const generation = ++settleGeneration;
  if (typeof requestAnimationFrame !== 'function') return;
  requestAnimationFrame(() => {
    if (generation !== settleGeneration) return;
    const focused = document.activeElement;
    if (focused && focused !== document.body) {
      if (!takesText(focused) || !canFocus(focused)) return;
      focused.blur();
      focused.focus({ preventScroll: true });
      return;
    }
    const target = returnTarget();
    if (target && target !== document.body && canFocus(target)) target.focus({ preventScroll: true });
  });
}

// Mount only while logically open. Native dialog owns modal focus and Tab; no retained exit.
// Layout is a column: header, the body that scrolls, then the footer, which always
// sits at the dialog's bottom edge and never covers the body. The top anchor is the
// default so a body that changes height (validation, filtering, a new step) never
// moves the title or the first field; pass anchor="center" only for fixed content.
export function DialogFrame({ title, subtitle, description, children, footer, contentProps, onDismiss, onBack, returnFocus, initialFocus, toolbar, wide = false, anchor = 'top' }: DialogChrome & {
  title: string;
  subtitle?: string;
  description?: string;
  children: ReactNode;
  /** The step's actions, pinned below the scrolling body. */
  footer?: ReactNode;
  /** Spread on the region that holds both body and footer, e.g. a transition-focus root. */
  contentProps?: Omit<ComponentProps<'div'>, 'className' | 'children'>;
  onBack?: () => void;
  initialFocus?: RefObject<HTMLElement | null>;
  wide?: boolean;
  anchor?: 'top' | 'center';
}) {
  const id = useId();
  const dialog = useRef<HTMLDialogElement>(null);
  const body = useRef<HTMLDivElement>(null);
  const actions = useRef<HTMLDivElement>(null);
  const bar = useRef<HTMLDivElement>(null);
  const close = useRef<HTMLButtonElement>(null);
  const active = useRef(false);
  const composing = useRef(false);
  const fallback = useRef(returnFocus);
  fallback.current = returnFocus;
  const preferred = useRef(initialFocus);
  preferred.current = initialFocus;
  useLayoutEffect(() => {
    const node = dialog.current!;
    const opener = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    active.current = true;
    // A dialog that opens owns focus: a settle still pending from the last close must not touch it.
    settleGeneration += 1;
    node.showModal();
    // Native showModal lands on the first control, which is Close; move to the task.
    focusInitial([body.current, actions.current], bar.current, preferred.current?.current, close.current);
    return () => {
      active.current = false;
      node.close();
      const returnTarget = () => canFocus(opener) ? opener : fallback.current?.current;
      const target = returnTarget();
      if (target && canFocus(target)) target.focus({ preventScroll: true });
      settleFocus(returnTarget);
    };
  }, []);
  function dismiss() {
    if (!active.current) return;
    active.current = false;
    onDismiss();
  }
  function escape() {
    if (!active.current || composing.current) return;
    if (onBack) onBack(); else dismiss();
  }
  const describedBy = [subtitle ? `${id}-subtitle` : '', description ? `${id}-description` : ''].filter(Boolean).join(' ') || undefined;
  return <dialog ref={dialog} className={styles.dialog} data-wide={wide || undefined} data-anchor={anchor === 'center' ? 'center' : 'top'} aria-labelledby={`${id}-title`} aria-describedby={describedBy}
    onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }}
    onKeyDown={(event) => {
      if (event.defaultPrevented || event.key !== 'Escape' || event.target instanceof Element && event.target.closest('dialog') !== event.currentTarget) return;
      event.preventDefault();
      event.stopPropagation();
      if (event.nativeEvent.isComposing || event.keyCode === 229 || composing.current) return;
      escape();
    }}
    onCancel={(event) => { event.preventDefault(); event.stopPropagation(); if (event.target === event.currentTarget) escape(); }}>
    <header className={styles.header}>
      <div className={styles.heading}>
        <h2 id={`${id}-title`} className={styles.title}>{title}</h2>
        {subtitle ? <p id={`${id}-subtitle`} className={styles.subtitle}>{subtitle}</p> : null}
        {description ? <p id={`${id}-description`} className="srOnly">{description}</p> : null}
      </div>
      <IconButton ref={close} variant="quiet" label="Close" onClick={dismiss}><XIcon size={15} aria-hidden="true" /></IconButton>
    </header>
    <div {...contentProps} className={styles.content}>
      <div ref={body} className={styles.body}>{toolbar ? <div ref={bar} className={styles.toolbar}>{toolbar}</div> : null}{children}</div>
      {footer ? <div ref={actions} className={styles.footer}>{footer}</div> : null}
    </div>
  </dialog>;
}
