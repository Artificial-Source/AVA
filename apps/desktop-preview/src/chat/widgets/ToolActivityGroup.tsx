import { Component, Fragment, lazy, memo, Suspense, useEffect, useId, useLayoutEffect, useRef, useState, type ReactNode, type SyntheticEvent } from 'react';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import { Presence } from '../../ui/Presence';
import { canFocus } from '../../ui/DialogFrame';
import { Status, type StatusValue } from '../../ui/Status';
import type { ToolView, TurnState } from '../contracts';
import { toolDetailsOpen, type ToolDisplay } from '../toolDisplay';
import { resourceDisplayName } from '../resourceDisplayName';
import { validToolChanges } from '../toolChanges';
import styles from '../../components/ToolActivity.module.css';

const ToolActivityDetails = lazy(() => import('./ToolActivityDetails'));
class DetailsBoundary extends Component<{ children: ReactNode }, { failed: boolean }> {
  state = { failed: false };
  static getDerivedStateFromError() { return { failed: true }; }
  render() { return this.state.failed ? <p className={styles.loading} role="status">Details couldn’t load. The status above is unchanged.</p> : this.props.children; }
}

type Shown = Readonly<{ status: StatusValue; label?: string; count: string }>;
const shown: Record<ToolView['state'], Shown> = {
  queued: { status: 'queued', count: 'queued' },
  running: { status: 'working', count: 'running' },
  completed: { status: 'completed', count: 'completed' },
  failed: { status: 'failed', count: 'failed' },
  canceled: { status: 'stopped', count: 'stopped' },
  unavailable: { status: 'unavailable', count: 'unavailable' },
};
const notRun: Shown = { status: 'unavailable', label: 'Not run', count: 'not run' };
const notFinished: Shown = { status: 'unavailable', label: 'Not finished', count: 'not finished' };
// Presentation only: once the turn ended stopped or failed, work that never started or never
// finished is not shown as still pending. The supplied record state is never rewritten.
function toolShown(state: ToolView['state'], lifecycle?: TurnState): Shown {
  const ended = lifecycle === 'failed' || lifecycle === 'canceled';
  if (ended && state === 'queued') return notRun;
  if (ended && state === 'running') return notFinished;
  return Object.hasOwn(shown, state) ? shown[state] : shown.unavailable;
}
const summaryOrder: readonly Shown[] = [shown.failed, shown.running, shown.queued, notFinished, notRun, shown.canceled, shown.unavailable];

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
// Keeps a user-toggled disclosure in view once its new content has committed (WCAG 2.4.11).
function useReveal<T extends HTMLElement>() {
  const node = useRef<T>(null);
  const pending = useRef(false);
  useLayoutEffect(() => {
    if (!pending.current) return;
    pending.current = false;
    if (canFocus(node.current)) node.current.scrollIntoView?.({ block: 'nearest' });
  });
  return { node, request: () => { pending.current = true; } };
}
function changeCounts(record: ToolView) {
  if (!record.changes?.length || !validToolChanges(record.changes)) return null;
  let added = 0; let removed = 0; let known = false;
  for (const change of record.changes) {
    if (typeof change.additions === 'number') { added += change.additions; known = true; }
    if (typeof change.deletions === 'number') { removed += change.deletions; known = true; }
  }
  return known ? `+${added} −${removed}` : null;
}

const ToolRow = memo(function ToolRow({ record, open, intent, individual, active, display, lifecycle, showCompletedOutput, onDisclosure }: { record: ToolView; open: boolean; intent: string; individual: boolean; active: boolean; display: ToolDisplay; lifecycle?: TurnState; showCompletedOutput?: boolean; onDisclosure?: () => void }) {
  const id = useId();
  const details = useRef<HTMLDivElement>(null);
  const reveal = useReveal<HTMLLIElement>();
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
  const status = toolShown(record.state, lifecycle);
  const counts = changeCounts(record);
  const content = <>{individual ? <CaretDownIcon className={styles.caret} size={12} aria-hidden="true" /> : null}
    <span className={styles.toolLabel}><span className={styles.action}>{record.action}</span>{' '}<span className={styles.resource}>{record.resource ? resourceDisplayName(record.resource) : record.detail}</span>{' '}<Status status={status.status} label={status.label} /></span>
    {counts ? <>{' '}<span className={styles.counts}>{counts}</span></> : null}</>;
  return <li ref={reveal.node} data-tool-state={record.state}>
    {individual ? <button type="button" className={styles.row} aria-expanded={expanded} aria-controls={id} onClick={(event) => {
      if (latest.current !== snapshot || !visible(event.currentTarget, active)) return;
      onDisclosure?.(); reveal.request(); setChoice({ intent, open: !expanded });
    }}>{content}</button> : <div className={styles.row}>{content}</div>}
    <Presence id={id} open={expanded}>{expanded ? <div ref={details} data-tool-intent={intent} onFocusCapture={(event) => { if (event.currentTarget.contains(document.activeElement)) inspect(event); }} onPointerDownCapture={inspect} onWheelCapture={inspect}>
      <DetailsBoundary><Suspense fallback={<p className={styles.loading}>Loading details…</p>}><ToolActivityDetails record={record} showCompletedOutput={showCompletedOutput} /></Suspense></DetailsBoundary>
    </div> : null}</Presence>
  </li>;
});

export function ToolActivityGroup({ records, open, intent = 0, individual = false, active = true, display = 'compact', lifecycle, onOpenChange, onDisclosure, showCompletedOutput }: {
  records: readonly ToolView[]; open?: boolean; intent?: number | string; individual?: boolean; active?: boolean; display?: ToolDisplay; lifecycle?: TurnState; showCompletedOutput?: boolean; onOpenChange: (open: boolean) => void;
  // Called when the user toggles a row or the group, before the change commits.
  onDisclosure?: () => void;
}) {
  const id = useId();
  const reveal = useReveal<HTMLElement>();
  const [revision, setRevision] = useState(0);
  const expanded = open ?? (records.length > 0 && records.every((record) => toolDetailsOpen(display, record.state, lifecycle)));
  const snapshot = { records, open, intent, active, display, lifecycle };
  const latest = useRef(snapshot); latest.current = snapshot;
  if (!records.length) return <section className={styles.activity} aria-label="Tool activity"><p className={styles.empty}>No tool activity</p></section>;
  const tally = new Map<Shown, number>();
  for (const record of records) { const status = toolShown(record.state, lifecycle); tally.set(status, (tally.get(status) ?? 0) + 1); }
  return <section ref={reveal.node} className={styles.activity} aria-label="Tool activity">
    <button type="button" className={styles.disclosure} aria-expanded={expanded} aria-controls={id} onClick={(event) => {
      if (latest.current !== snapshot || !visible(event.currentTarget, active)) return;
      onDisclosure?.(); reveal.request(); setRevision((value) => value + 1); onOpenChange(!expanded);
    }}>
      <span className={styles.count}>{records.length} {records.length === 1 ? 'tool' : 'tools'}</span>
      {/* Separators stay direct children so the accessible name keeps "3 tools · 1 failed" spacing. */}
      {summaryOrder.flatMap((status) => tally.get(status) ? [<Fragment key={status.count}>{' '}<span className={styles.dot}>·</span>{' '}<Status status={status.status} label={`${tally.get(status)} ${status.count}`} /></Fragment>] : [])}
      {' '}<span className={styles.toggle}>{expanded ? 'Hide details' : 'Show details'}<CaretDownIcon className={styles.caret} size={12} aria-hidden="true" /></span>
    </button>
    <ul id={id} className={styles.outcomes}>{records.map((record) => <ToolRow key={record.id} record={record} open={open ?? toolDetailsOpen(display, record.state, lifecycle)} intent={`${intent}:${revision}`} individual={individual} active={active} display={display} lifecycle={lifecycle} showCompletedOutput={showCompletedOutput} onDisclosure={onDisclosure} />)}</ul>
  </section>;
}
