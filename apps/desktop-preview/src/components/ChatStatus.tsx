import { ArrowsClockwiseIcon } from '@phosphor-icons/react/dist/csr/ArrowsClockwise';
import { CheckCircleIcon } from '@phosphor-icons/react/dist/csr/CheckCircle';
import { MinusCircleIcon } from '@phosphor-icons/react/dist/csr/MinusCircle';
import { WarningDiamondIcon } from '@phosphor-icons/react/dist/csr/WarningDiamond';
import { XCircleIcon } from '@phosphor-icons/react/dist/csr/XCircle';
import { StopIcon } from '@phosphor-icons/react/dist/csr/Stop';
import type { StatusValue } from '../ui/Status';
import styles from './ChatStatus.module.css';

const labels = {
  working: 'Working',
  completed: 'Completed',
  approval: 'Attention',
  stopped: 'Stopped',
  failed: 'Failed',
  unavailable: 'Unavailable',
};

export function ChatStatus({ status }: { status: StatusValue }) {
  if (status === 'idle') return null;
  return (
    <span className={styles.status} data-status={status} title="Fixed example status, not live activity">
      <span className={status === 'working' ? `${styles.glyph} ${styles.cycling}` : styles.glyph} aria-hidden="true">
        {status === 'working' ? <ArrowsClockwiseIcon size={12} aria-hidden="true" /> : null}
        {status === 'completed' ? <CheckCircleIcon size={12} aria-hidden="true" /> : null}
        {status === 'approval' ? <WarningDiamondIcon size={12} aria-hidden="true" /> : null}
        {status === 'stopped' ? <StopIcon size={11} aria-hidden="true" /> : null}
        {status === 'failed' ? <XCircleIcon size={12} aria-hidden="true" /> : null}
        {status === 'unavailable' ? <MinusCircleIcon size={12} aria-hidden="true" /> : null}
      </span>
      {labels[status]}
    </span>
  );
}
