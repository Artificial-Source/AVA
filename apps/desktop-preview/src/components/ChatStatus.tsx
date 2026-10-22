import { ArrowsClockwiseIcon } from '@phosphor-icons/react/dist/csr/ArrowsClockwise';
import { CheckCircleIcon } from '@phosphor-icons/react/dist/csr/CheckCircle';
import { ClockIcon } from '@phosphor-icons/react/dist/csr/Clock';
import { MinusCircleIcon } from '@phosphor-icons/react/dist/csr/MinusCircle';
import { WarningDiamondIcon } from '@phosphor-icons/react/dist/csr/WarningDiamond';
import { XCircleIcon } from '@phosphor-icons/react/dist/csr/XCircle';
import { StopIcon } from '@phosphor-icons/react/dist/csr/Stop';
import type { StatusValue } from '../ui/Status';
import styles from './ChatStatus.module.css';

const labels: Record<Exclude<StatusValue, 'idle'>, string> = {
  queued: 'Queued',
  working: 'Working',
  completed: 'Completed',
  approval: 'Attention',
  stopped: 'Stopped',
  failed: 'Failed',
  unavailable: 'Unavailable',
};

// `label` replaces the visible word only; color and glyph stay tied to `status`.
// `part` splits the pair across layouts (glyph on a title line, word on a meta line);
// the caller must render the 'label' part somewhere so the status stays readable.
export function ChatStatus({ status, label, part }: { status: StatusValue; label?: string; part?: 'glyph' | 'label' }) {
  if (status === 'idle') return null;
  return (
    <span className={styles.status} data-status={status} aria-hidden={part === 'glyph' || undefined}>
      {part === 'label' ? null : (
        <span className={status === 'working' ? `${styles.glyph} ${styles.cycling}` : styles.glyph} aria-hidden="true">
          {status === 'queued' ? <ClockIcon size={12} aria-hidden="true" /> : null}
          {status === 'working' ? <ArrowsClockwiseIcon size={12} aria-hidden="true" /> : null}
          {status === 'completed' ? <CheckCircleIcon size={12} aria-hidden="true" /> : null}
          {status === 'approval' ? <WarningDiamondIcon size={12} aria-hidden="true" /> : null}
          {status === 'stopped' ? <StopIcon size={11} aria-hidden="true" /> : null}
          {status === 'failed' ? <XCircleIcon size={12} aria-hidden="true" /> : null}
          {status === 'unavailable' ? <MinusCircleIcon size={12} aria-hidden="true" /> : null}
        </span>
      )}
      {part === 'glyph' ? null : label || labels[status]}
    </span>
  );
}
