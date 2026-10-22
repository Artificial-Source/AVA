import { useId, type ReactNode } from 'react';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import type { ChangeEntryView } from '../contracts';
import { boundedDiffText, diffLines } from '../toolChanges';
import styles from './DiffView.module.css';

const statusLabels: Record<ChangeEntryView['status'], string> = { modified: 'Modified', added: 'Added', deleted: 'Deleted' };

// The one diff format: path/count header, then unified lines with a +/− gutter and neutral tints.
// Every string is painted as text; nothing here fetches, links or interprets the diff.
// With `onOpenChange` the path becomes the file's disclosure; the header and counts stay visible.
// `wrap="words"` breaks wrapped lines only at spaces, so a token wider than the column scrolls
// sideways instead of splitting; the user's wrap-diff preference still decides whether lines wrap.
export function DiffFile({ entry, heading = false, bounded = false, focusable = false, action, open = true, onOpenChange, wrap = 'anywhere' }: {
  entry: ChangeEntryView;
  heading?: boolean;
  bounded?: boolean;
  focusable?: boolean;
  action?: ReactNode;
  open?: boolean;
  onOpenChange?: (open: boolean) => void;
  wrap?: 'anywhere' | 'words';
}) {
  const id = useId();
  const { path, diff } = entry;
  const collapsible = onOpenChange !== undefined;
  const shown = !collapsible || open;
  const text = shown && diff.state === 'text' ? boundedDiffText(diff.text) : '';
  const lines = diffLines(text);
  // A final newline is not a blank context line.
  if (lines.length > 1 && lines.at(-1)!.text === '') lines.pop();
  const title = collapsible
    ? <button type="button" className={styles.toggle} aria-expanded={shown} aria-controls={`${id}-body`} onClick={() => onOpenChange(!shown)}><CaretDownIcon className={styles.caret} size={12} aria-hidden="true" /><span>{path}</span></button>
    : path;
  const body = shown ? <>
    {entry.description ? <p className={styles.description}>{entry.description}</p> : null}
    {diff.state === 'text' ? <>
      {text ? <pre className={wrap === 'words' ? `${styles.diff} ${styles.words}` : styles.diff} data-bounded={bounded} tabIndex={0} aria-label={`Diff: ${path}`}><code className={styles.lines}>{lines.map((line, index) => line.kind === 'hunk' || line.kind === 'meta'
        ? <span key={index} className={styles.line} data-kind={line.kind}><span className={styles.full}>{line.text}</span></span>
        : <span key={index} className={styles.line} data-kind={line.kind}><span className={styles.marker}>{line.text.slice(0, 1)}</span><span className={styles.text}>{line.text.slice(1)}</span></span>)}</code></pre> : <p className={styles.note}>No line changes to show.</p>}
      {diff.truncated ? <p className={styles.note}>This diff was cut off before the end.</p> : null}
      {text.length < diff.text.length ? <p className={styles.note}>Showing the first 200 lines or 20,000 characters.</p> : null}
    </> : <p className={styles.note}>{diff.state === 'binary' ? 'Binary file. There’s no text diff to show.' : 'No diff available for this file.'}</p>}
  </> : null;
  return <section className={styles.file} aria-label={`File change: ${path}`} data-path={path} tabIndex={focusable ? -1 : undefined}>
    <div className={styles.header}>
      {heading ? <h3 className={styles.path}>{title}</h3> : <span className={styles.path}>{title}</span>}
      <span className={styles.facts}>
        <span>{statusLabels[entry.status]}</span>
        {entry.added !== undefined ? <span className={styles.added} aria-label={`${entry.added} additions`}>+{entry.added}</span> : null}
        {entry.removed !== undefined ? <span aria-label={`${entry.removed} deletions`}>−{entry.removed}</span> : null}
      </span>
      {action ? <span className={styles.action}>{action}</span> : null}
    </div>
    {collapsible ? <div id={`${id}-body`} className={styles.body} hidden={!shown}>{body}</div> : body}
  </section>;
}
