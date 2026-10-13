import { useId, useLayoutEffect, useRef, type ReactNode, type RefObject } from 'react';
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

// Mount only while logically open. Native dialog owns modal focus and Tab; no retained exit.
export function DialogFrame({ title, description, children, onDismiss, onBack, returnFocus, toolbar, wide = false }: DialogChrome & {
  title: string;
  description?: string;
  children: ReactNode;
  onBack?: () => void;
  wide?: boolean;
}) {
  const id = useId();
  const dialog = useRef<HTMLDialogElement>(null);
  const active = useRef(false);
  const composing = useRef(false);
  const fallback = useRef(returnFocus);
  fallback.current = returnFocus;
  useLayoutEffect(() => {
    const node = dialog.current!;
    const opener = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    active.current = true;
    node.showModal();
    return () => {
      active.current = false;
      node.close();
      const target = canFocus(opener) ? opener : fallback.current?.current;
      if (target && canFocus(target)) target.focus({ preventScroll: true });
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
  return <dialog ref={dialog} className={styles.dialog} data-wide={wide || undefined} aria-labelledby={`${id}-title`} aria-describedby={description ? `${id}-description` : undefined}
    onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }}
    onKeyDown={(event) => {
      if (event.defaultPrevented || event.key !== 'Escape' || event.target instanceof Element && event.target.closest('dialog') !== event.currentTarget) return;
      event.preventDefault();
      event.stopPropagation();
      if (event.nativeEvent.isComposing || event.keyCode === 229 || composing.current) return;
      escape();
    }}
    onCancel={(event) => { event.preventDefault(); event.stopPropagation(); if (event.target === event.currentTarget) escape(); }}>
    <header className={styles.header}><div className="srOnly"><h2 id={`${id}-title`}>{title}</h2>{description ? <p id={`${id}-description`}>{description}</p> : null}</div><IconButton label="Close dialog" onClick={dismiss}>×</IconButton></header>
    <div className={styles.body}>{toolbar ? <div className={styles.toolbar}>{toolbar}</div> : null}{children}</div>
  </dialog>;
}
