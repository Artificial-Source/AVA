import { Component, lazy, memo, Suspense, useId, useRef, useState, type ReactNode } from 'react';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import { Presence } from '../../ui/Presence';
import { canFocus } from '../../ui/DialogFrame';
import { Status, type StatusValue } from '../../ui/Status';
import type { ToolView } from '../contracts';
import { resourceDisplayName } from '../resourceDisplayName';
import styles from '../../components/ToolActivity.module.css';

const ToolActivityDetails = lazy(() => import('./ToolActivityDetails'));
class DetailsBoundary extends Component<{ children: ReactNode }, { failed: boolean }> {
  state = { failed: false };
  static getDerivedStateFromError() { return { failed: true }; }
  render() { return this.state.failed ? <p className={styles.loading} role="status">Details unavailable. Supplied tool status is retained above.</p> : this.props.children; }
}
const statuses: Record<ToolView['state'], StatusValue | 'queued'> = { queued: 'queued', running: 'working', completed: 'completed', failed: 'failed', canceled: 'stopped', unavailable: 'unavailable' };

const ToolRow = memo(function ToolRow({ record, open, intent, individual, showCompletedOutput }: { record: ToolView; open: boolean; intent: string; individual: boolean; showCompletedOutput?: boolean }) {
  const id = useId();
  const [choice, setChoice] = useState<{ intent: string; open: boolean } | null>(null);
  const expanded = choice?.intent === intent ? choice.open : open;
  const latest = useRef(record); latest.current = record;
  const content = <>{individual ? <CaretDownIcon className={styles.caret} size={12} aria-hidden="true" /> : null}
    <span className={styles.toolLabel}>{record.action}{' '}<span className={styles.resource}>{record.resource ? resourceDisplayName(record.resource) : record.detail}</span></span>{' '}
    {record.state === 'queued' ? <span className={styles.queued}>Queued</span> : <Status status={Object.hasOwn(statuses, record.state) ? statuses[record.state] as StatusValue : 'unavailable'} />}</>;
  return <li data-tool-state={record.state}>
    {individual ? <button type="button" className={styles.row} aria-expanded={expanded} aria-controls={id} onClick={(event) => { if (latest.current === record && canFocus(event.currentTarget)) setChoice({ intent, open: !expanded }); }}>{content}</button> : <div className={styles.row}>{content}</div>}
    <Presence id={id} open={expanded}>{expanded ? <DetailsBoundary><Suspense fallback={<p className={styles.loading}>Loading supplied details…</p>}><ToolActivityDetails record={record} showCompletedOutput={showCompletedOutput} /></Suspense></DetailsBoundary> : null}</Presence>
  </li>;
});

export function ToolActivityGroup({ records, open, intent = 0, individual = false, onOpenChange, showCompletedOutput }: {
  records: readonly ToolView[]; open: boolean; intent?: number; individual?: boolean; showCompletedOutput?: boolean; onOpenChange: (open: boolean) => void;
}) {
  const id = useId();
  const [revision, setRevision] = useState(0);
  const summary = Object.entries(statuses).flatMap(([state, text]) => {
    const count = records.filter((record) => record.state === state).length;
    return count ? [`${count} ${text}`] : [];
  }).join(' · ');
  return <section className={styles.activity} aria-label="Tool activity">
    <button type="button" className={styles.disclosure} aria-expanded={open} aria-controls={id} disabled={!records.length} onClick={(event) => { if (canFocus(event.currentTarget)) { setRevision((value) => value + 1); onOpenChange(!open); } }}>
      <CaretDownIcon className={styles.caret} size={12} aria-hidden="true" />
      <span>{records.length ? `Tool details · ${records.length}` : 'No tool activity'}</span>
      <span className={styles.summary}>{summary}</span>
    </button>
    <ul id={id} className={styles.outcomes} aria-label="Supplied tool activity">{records.map((record) => <ToolRow key={record.id} record={record} open={open} intent={`${intent}:${revision}`} individual={individual} showCompletedOutput={showCompletedOutput} />)}</ul>
  </section>;
}
