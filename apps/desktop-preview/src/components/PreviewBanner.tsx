import type { ReactNode, Ref } from 'react';
import { FlaskIcon } from '@phosphor-icons/react/dist/csr/Flask';
import { WarningCircleIcon } from '@phosphor-icons/react/dist/csr/WarningCircle';
import { XIcon } from '@phosphor-icons/react/dist/csr/X';
import { IconButton } from '../ui/Button';
import styles from './PreviewBanner.module.css';

export const previewDisclosure = 'Preview · invented data. Nothing is saved, sent or run.';

// The one place the mock boundary is stated. Run preview controls ride along; never below the transcript.
// `onFocusControl` reports the focused control, or null once focus leaves for another element.
export function PreviewBanner({ children, notice, dismissRef, onFocusControl, onDismiss }: {
  children?: ReactNode;
  notice?: string;
  dismissRef?: Ref<HTMLButtonElement>;
  onFocusControl?: (control: HTMLElement | null) => void;
  onDismiss: () => void;
}) {
  return (
    <section id="preview-banner" className={styles.banner} aria-label="Preview"
      onFocus={(event) => onFocusControl?.(event.target instanceof HTMLElement ? event.target : null)}
      onBlur={(event) => { if (event.relatedTarget instanceof Node && !event.currentTarget.contains(event.relatedTarget)) onFocusControl?.(null); }}>
      <div className={styles.inner}>
        <p className={styles.disclosure}>
          <FlaskIcon size={14} aria-hidden="true" />
          <span><strong>Preview</strong> · invented data. Nothing is saved, sent or run.</span>
        </p>
        {children ? <div className={styles.controls} role="group" aria-label="Run preview">{children}</div> : null}
        {notice ? <p className={styles.notice} role="status"><WarningCircleIcon size={13} aria-hidden="true" />{notice}</p> : null}
        <IconButton ref={dismissRef} className={styles.dismiss} variant="quiet" label="Hide preview banner" title="Hide preview banner" onClick={onDismiss}>
          <XIcon size={15} aria-hidden="true" />
        </IconButton>
      </div>
    </section>
  );
}

// The collapsed banner: a quiet, keyboard-reachable way back.
export function PreviewChip({ chipRef, onOpen }: { chipRef?: Ref<HTMLButtonElement>; onOpen: () => void }) {
  return (
    <button ref={chipRef} className={styles.chip} type="button" aria-expanded="false" title={`Show preview banner · ${previewDisclosure}`} onClick={onOpen}>
      <FlaskIcon size={13} aria-hidden="true" />Preview
    </button>
  );
}
