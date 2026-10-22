import { useLayoutEffect, useRef, useState } from 'react';
import type { ChangeEntryView, ChangeReviewState } from '../contracts';
import { canFocus } from '../../ui/DialogFrame';
import { LearnMoreLink } from '../../components/LearnMoreLink';
import { ListNotice } from '../../screens/ScreenPrimitives';
import { DiffFile } from './DiffView';
import styles from '../../components/ChangeReview.module.css';

function totals(changes: readonly ChangeEntryView[]) {
  let added = 0; let removed = 0; let known = false;
  for (const change of changes) {
    if (change.added !== undefined) { added += change.added; known = true; }
    if (change.removed !== undefined) { removed += change.removed; known = true; }
  }
  return known ? { added, removed } : null;
}
// The file name leads; its folder follows, muted, and is what gets cut when the column is narrow.
function splitPath(path: string) {
  const slash = path.lastIndexOf('/');
  return slash <= 0 || slash === path.length - 1 ? { name: path, folder: '' } : { name: path.slice(slash + 1), folder: path.slice(0, slash) };
}

// Read-only inventory in the same diff format as the transcript: totals, an index that jumps to
// each file, then every file with its own disclosure. `focusPath` is an explicit "Review in
// Changes" request: open that file, bring it into view and move focus to it once. The help link
// shares the totals row, or follows the notice when there is nothing listed.
export function ChangeReviewView({ view, focusPath }: { view: ChangeReviewState; focusPath?: string | null }) {
  const root = useRef<HTMLDivElement>(null);
  const [collapsed, setCollapsed] = useState<ReadonlySet<string>>(() => new Set());
  const listed = view.state === 'ready' || view.state === 'stale';
  function setOpen(path: string, open: boolean) {
    setCollapsed((value) => {
      if (value.has(path) !== open) return value;
      const next = new Set(value);
      if (open) next.delete(path); else next.add(path);
      return next;
    });
  }
  function reveal(path: string) {
    const target = Array.from(root.current?.querySelectorAll<HTMLElement>('[data-path]') ?? []).find((node) => node.dataset.path === path) ?? null;
    if (!canFocus(target)) return;
    // The header is always rendered, so focus lands now; the body opens below it on the next commit.
    setOpen(path, true);
    target.scrollIntoView?.({ block: 'start' });
    target.focus({ preventScroll: true });
  }
  useLayoutEffect(() => {
    if (focusPath && listed) reveal(focusPath);
  }, [focusPath, listed]);
  const changes = listed ? view.changes : [];
  const sum = totals(changes);
  const files = `${changes.length} ${changes.length === 1 ? 'file' : 'files'}`;
  const help = <LearnMoreLink topic="changes" className={styles.help}>About changes</LearnMoreLink>;
  return <div ref={root} className={styles.review}>
    {view.state === 'loading' ? <ListNotice state="loading" message="Loading changes…" /> : null}
    {view.state === 'empty' ? <ListNotice state="empty" message={view.message} /> : null}
    {view.state === 'error' ? <ListNotice state="error" message={view.message} /> : null}
    {view.state === 'stale' ? <ListNotice state="stale" message={view.message} /> : null}
    {listed && changes.length ? <>
      <div className={styles.head}>
        <p className={styles.totals}>
          <span className="srOnly">{sum ? `${files} changed, ${sum.added} ${sum.added === 1 ? 'line' : 'lines'} added, ${sum.removed} removed` : `${files} changed`}</span>
          <span className={styles.figures} aria-hidden="true">{files}{sum ? <><span className={styles.dot}>·</span><span className={styles.added}>+{sum.added}</span><span>−{sum.removed}</span></> : null}</span>
        </p>
        {help}
      </div>
      {changes.length > 1 ? <nav aria-label="Changed files"><ul className={styles.index}>{changes.map((change) => {
        const { name, folder } = splitPath(change.path);
        const counts = change.added !== undefined || change.removed !== undefined ? `+${change.added ?? 0} −${change.removed ?? 0}` : '';
        return <li key={change.id}><button type="button" className={styles.jump} aria-label={counts ? `${change.path}, ${change.added ?? 0} added, ${change.removed ?? 0} removed` : change.path} onClick={() => reveal(change.path)}>
          <span className={styles.jumpPath}><span className={styles.jumpName}>{name}</span>{folder ? <span className={styles.jumpFolder}>{folder}</span> : null}</span>
          {counts ? <span className={styles.jumpCounts}>{counts}</span> : null}
        </button></li>;
      })}</ul></nav> : null}
      <ul className={styles.list}>{changes.map((change) => <li key={change.id} className={styles.entry}>
        <DiffFile entry={change} heading focusable wrap="words" open={!collapsed.has(change.path)} onOpenChange={(open) => setOpen(change.path, open)} />
      </li>)}</ul>
    </> : help}
  </div>;
}
