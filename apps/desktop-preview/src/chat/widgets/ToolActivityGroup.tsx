import { Component, lazy, memo, Suspense, useEffect, useId, useRef, useState, type ReactNode, type SyntheticEvent } from 'react';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import { Presence } from '../../ui/Presence';
import { canFocus } from '../../ui/DialogFrame';
import { Status, type StatusValue } from '../../ui/Status';
import type { ToolView, TurnState } from '../contracts';
import { toolDetailsOpen, type ToolDisplay } from '../toolDisplay';
import { resourceDisplayName } from '../resourceDisplayName';
import styles from '../../components/ToolActivity.module.css';

const ToolActivityDetails = lazy(() => import('./ToolActivityDetails'));
class DetailsBoundary extends Component<{ children: ReactNode }, { failed: boolean }> {
  state = { failed: false };
  static getDerivedStateFromError() { return { failed: true }; }
  render() { return this.state.failed ? <p className={styles.loading} role="status">Details unavailable. Supplied tool status is retained above.</p> : this.props.children; }
}
const statuses: Record<ToolView['state'], StatusValue | 'queued'> = { queued: 'queued', running: 'working', completed: 'completed', failed: 'failed', canceled: 'stopped', unavailable: 'unavailable' };
function visible(node: HTMLElement | null, active: boolean): node is HTMLElement {
  const dialog = document.querySelector('dialog[open]');
  return active && !document.hidden && canFocus(node) && (!dialog || dialog.contains(node));
}
function selected(node: HTMLElement) {
  const selection = window.getSelection();
  if (!selection || selection.isCollapsed) return false;
  for (let index = 0; index < selection.rangeCount; index++) if (selection.getRangeAt(index).intersectsNode(node)) return true;
  return false;
}

const ToolRow = memo(function ToolRow({ record, open, intent, individual, active, display, showCompletedOutput }: { record: ToolView; open: boolean; intent: string; individual: boolean; active: boolean; display: ToolDisplay; showCompletedOutput?: boolean }) {
  const id = useId();
  const details = useRef<HTMLDivElement>(null);
  const [choice, setChoice] = useState<{ intent: string; open: boolean } | null>(null);
  let expanded = choice?.intent === intent ? choice.open : open;
  // Native selection can precede selectionchange delivery. Preserve the current
  // detail before Auto folds it, but never veto a new row/group/all/preset intent.
  const node = details.current;
  if (!expanded && choice?.intent !== intent && individual && display === 'auto' && node?.dataset.toolIntent === intent && visible(node, active) && (node.contains(document.activeElement) || selected(node))) {
    expanded = true;
    setChoice({ intent, open: true });
  }
  const snapshot = { record, intent, active, expanded };
  const latest = useRef(snapshot); latest.current = snapshot;
  function inspect(event?: SyntheticEvent<HTMLDivElement>) {
    const root = details.current;
    if (latest.current !== snapshot || !individual || !expanded || !visible(root, active) || event && (event.currentTarget !== root || !(event.target instanceof Node) || !root.contains(event.target))) return;
    setChoice((value) => value?.intent === intent && value.open ? value : { intent, open: true });
  }
  useEffect(() => {
    if (!individual || display !== 'auto' || !expanded || !active) return;
    const selectionChanged = () => { if (details.current && selected(details.current)) inspect(); };
    document.addEventListener('selectionchange', selectionChanged);
    return () => document.removeEventListener('selectionchange', selectionChanged);
  });
  const content = <>{individual ? <CaretDownIcon className={styles.caret} size={12} aria-hidden="true" /> : null}
    <span className={styles.toolLabel}>{record.action}{' '}<span className={styles.resource}>{record.resource ? resourceDisplayName(record.resource) : record.detail}</span></span>{' '}
    {record.state === 'queued' ? <span className={styles.queued}>Queued</span> : <Status status={Object.hasOwn(statuses, record.state) ? statuses[record.state] as StatusValue : 'unavailable'} />}</>;
  return <li data-tool-state={record.state}>
    {individual ? <button type="button" className={styles.row} aria-expanded={expanded} aria-controls={id} onClick={(event) => { if (latest.current === snapshot && visible(event.currentTarget, active)) setChoice({ intent, open: !expanded }); }}>{content}</button> : <div className={styles.row}>{content}</div>}
    <Presence id={id} open={expanded}>{expanded ? <div ref={details} data-tool-intent={intent} onFocusCapture={(event) => { if (event.currentTarget.contains(document.activeElement)) inspect(event); }} onPointerDownCapture={inspect} onWheelCapture={inspect}>
      <DetailsBoundary><Suspense fallback={<p className={styles.loading}>Loading supplied details…</p>}><ToolActivityDetails record={record} showCompletedOutput={showCompletedOutput} /></Suspense></DetailsBoundary>
    </div> : null}</Presence>
  </li>;
});

export function ToolActivityGroup({ records, open, intent = 0, individual = false, active = true, display = 'compact', lifecycle, onOpenChange, showCompletedOutput }: {
  records: readonly ToolView[]; open?: boolean; intent?: number | string; individual?: boolean; active?: boolean; display?: ToolDisplay; lifecycle?: TurnState; showCompletedOutput?: boolean; onOpenChange: (open: boolean) => void;
}) {
  const id = useId();
  const [revision, setRevision] = useState(0);
  const expanded = open ?? (records.length > 0 && records.every((record) => toolDetailsOpen(display, record.state, lifecycle)));
  const snapshot = { records, open, intent, active, display, lifecycle };
  const latest = useRef(snapshot); latest.current = snapshot;
  const summary = Object.entries(statuses).flatMap(([state, text]) => {
    const count = records.filter((record) => record.state === state).length;
    return count ? [`${count} ${text}`] : [];
  }).join(' · ');
  return <section className={styles.activity} aria-label="Tool activity">
    <button type="button" className={styles.disclosure} aria-expanded={expanded} aria-controls={id} disabled={!records.length} onClick={(event) => { if (latest.current === snapshot && visible(event.currentTarget, active)) { setRevision((value) => value + 1); onOpenChange(!expanded); } }}>
      <CaretDownIcon className={styles.caret} size={12} aria-hidden="true" />
      <span>{records.length ? `Tool details · ${records.length}` : 'No tool activity'}</span>
      <span className={styles.summary}>{summary}</span>
    </button>
    <ul id={id} className={styles.outcomes} aria-label="Supplied tool activity">{records.map((record) => <ToolRow key={record.id} record={record} open={open ?? toolDetailsOpen(display, record.state, lifecycle)} intent={`${intent}:${revision}`} individual={individual} active={active} display={display} showCompletedOutput={showCompletedOutput} />)}</ul>
  </section>;
}
