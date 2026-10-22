import { useContext } from 'react';
import { ArrowRightIcon } from '@phosphor-icons/react/dist/csr/ArrowRight';
import type { ToolChangeView } from '../contracts';
import { ChangeNavigationContext } from '../changeNavigation';
import { validToolChanges } from '../toolChanges';
import { DiffFile } from './DiffView';
import styles from './ToolActivityDetails.module.css';

const statuses = { created: 'added', modified: 'modified', deleted: 'deleted' } as const;

export function ToolDiffView({ changes }: { changes: readonly ToolChangeView[] }) {
  const review = useContext(ChangeNavigationContext);
  if (!validToolChanges(changes)) return <p role="status">File changes unavailable. The change details were incomplete.</p>;
  return <div className={styles.changes}>{changes.map((change) => <DiffFile key={change.id} bounded
    entry={{ id: change.id, path: change.filename, status: statuses[change.operation], added: change.additions, removed: change.deletions, diff: change.diff }}
    action={review ? <button type="button" className={styles.review} aria-label={`Review in Changes: ${change.filename}`} onClick={() => review(change.filename)}>Review in Changes<ArrowRightIcon size={12} aria-hidden="true" /></button> : undefined} />)}</div>;
}
