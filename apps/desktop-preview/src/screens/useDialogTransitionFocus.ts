import { useLayoutEffect, useRef, useState } from 'react';

function visibleControl(node: HTMLElement, dialog: HTMLDialogElement) {
  if (!node.isConnected || node.matches(':disabled') || node.closest('dialog') !== dialog || node.closest('[hidden], [inert], [aria-hidden="true"]')) return false;
  const box = node.getBoundingClientRect();
  const window = node.ownerDocument.defaultView!;
  if (!box.width || !box.height || box.left < 0 || box.top < 0 || box.right > window.innerWidth || box.bottom > window.innerHeight) return false;
  for (let parent: HTMLElement | null = node; parent; parent = parent.parentElement) {
    const style = window.getComputedStyle(parent);
    if (style.display === 'none' || style.visibility === 'hidden') return false;
    if (parent !== node) {
      const clip = parent.getBoundingClientRect();
      if (/(auto|scroll|hidden|clip)/.test(style.overflowX) && (box.left < clip.left || box.right > clip.right) ||
        /(auto|scroll|hidden|clip)/.test(style.overflowY) && (box.top < clip.top || box.bottom > clip.bottom)) return false;
    }
    if (parent === dialog) break;
  }
  // :modal alone includes underlying Settings. Hit testing also requires this
  // control to be visible in the top native dialog, not behind another modal.
  return node.contains(node.ownerDocument.elementFromPoint(box.left + box.width / 2, box.top + box.height / 2));
}

// Only accepted, explicit UI transitions call capture. Native Tab, initial focus
// and return focus remain DialogFrame/browser-owned; there is no focus watcher.
export function useDialogTransitionFocus(onDismiss: () => void) {
  const content = useRef<HTMLDivElement>(null);
  const active = useRef(false);
  const composing = useRef(false);
  const pending = useRef<{ control: HTMLElement; dialog: HTMLDialogElement } | null>(null);
  const [commit, setCommit] = useState(0);
  useLayoutEffect(() => {
    active.current = true;
    return () => { active.current = false; pending.current = null; };
  }, []);
  useLayoutEffect(() => {
    const previous = pending.current;
    pending.current = null;
    const root = content.current;
    if (!previous || !active.current || composing.current || !root?.isConnected) return;
    const { control, dialog } = previous;
    const document = dialog.ownerDocument;
    if (!dialog.isConnected || !dialog.open || !dialog.matches(':modal') || root.closest('dialog') !== dialog || !document.hasFocus()) return;
    const current = document.activeElement;
    const disabled = control.matches(':disabled');
    // Native focus fixup can lag this effect. Only the captured, now-disabled
    // control is equivalent to body, never a different focus destination.
    if (current !== document.body && (current !== control || !disabled) || control.isConnected && !disabled) return;
    const candidates = [...root.querySelectorAll<HTMLElement>('input:not(:disabled), button:not(:disabled), select:not(:disabled), textarea:not(:disabled)')];
    const close = dialog.querySelector<HTMLElement>(':scope > header button');
    if (close) candidates.push(close);
    candidates.find((node) => visibleControl(node, dialog))?.focus({ preventScroll: true });
  }, [commit]);
  return {
    content: {
      ref: content,
      onCompositionStart: () => { composing.current = true; },
      onCompositionEnd: () => { composing.current = false; },
    },
    capture() {
      const root = content.current;
      const dialog = root?.closest('dialog');
      const control = root?.ownerDocument.activeElement;
      if (!active.current || composing.current || !dialog?.open || !dialog.matches(':modal') || !dialog.ownerDocument.hasFocus() ||
        !(control instanceof HTMLElement) || !root?.contains(control) || control.closest('dialog') !== dialog || control.matches(':disabled')) return;
      pending.current = { control, dialog };
      // PermissionCard/QuestionCard can commit only their own local state. This
      // same-event update guarantees a repair after that DOM commit as well.
      setCommit((value) => value + 1);
    },
    dismiss() { active.current = false; pending.current = null; onDismiss(); },
  };
}
