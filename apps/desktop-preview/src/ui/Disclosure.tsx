import { useLayoutEffect, useRef, type ReactNode } from 'react';
import { Presence } from './Presence';
import styles from './Disclosure.module.css';

interface DisclosureProps {
  id: string;
  open: boolean;
  onOpenChange: (open: boolean) => void;
  trigger: ReactNode;
  triggerClassName?: string;
  /** Optional control placed between the trigger and the content, e.g. a heading-row action. */
  accessory?: ReactNode;
  children: ReactNode;
}

export function Disclosure({ id, open, onOpenChange, trigger, triggerClassName, accessory, children }: DisclosureProps) {
  const triggerRef = useRef<HTMLButtonElement>(null);
  const contentFocused = useRef(false);

  useLayoutEffect(() => {
    if (!open && contentFocused.current) {
      triggerRef.current?.focus();
      contentFocused.current = false;
    }
  }, [open]);

  return (
    <>
      <button
        ref={triggerRef}
        className={triggerClassName}
        type="button"
        aria-expanded={open}
        aria-controls={open ? id : undefined}
        onClick={() => onOpenChange(!open)}
      >{trigger}</button>
      {accessory}
      <div
        className={styles.track}
        data-open={open}
        onFocusCapture={() => { contentFocused.current = true; }}
        onBlurCapture={(event) => { contentFocused.current = event.currentTarget.contains(event.relatedTarget); }}
      >
        <Presence id={id} open={open} className={styles.content}>{children}</Presence>
      </div>
    </>
  );
}
