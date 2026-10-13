import type { ToolChangeView } from '../contracts';
import styles from './ToolActivityDetails.module.css';

export function validToolChanges(value: unknown): value is readonly ToolChangeView[] {
  if (!Array.isArray(value)) return false;
  const ids = new Set<string>(); const files = new Set<string>();
  for (const change of value) {
    if (!change || typeof change.id !== 'string' || !change.id.trim() || ids.has(change.id) || typeof change.filename !== 'string' || !change.filename.trim() || files.has(change.filename) || !['created', 'modified', 'deleted'].includes(change.operation)) return false;
    if ([change.additions, change.deletions].some((count) => count !== undefined && (typeof count !== 'number' || !Number.isSafeInteger(count) || count < 0))) return false;
    const diff = change.diff;
    if (!diff || !['text', 'binary', 'unavailable'].includes(diff.state) || diff.state === 'text' && (typeof diff.text !== 'string' || typeof diff.truncated !== 'boolean')) return false;
    ids.add(change.id); files.add(change.filename);
  }
  return true;
}

const operations = { created: 'Created', modified: 'Modified', deleted: 'Deleted' };
export function ToolDiffView({ changes }: { changes: readonly ToolChangeView[] }) {
  if (!validToolChanges(changes)) return <p role="status">File changes unavailable: invalid or duplicate supplied metadata.</p>;
  return <div className={styles.changes}>{changes.map((change) => {
    const text = change.diff.state === 'text' ? change.diff.text.slice(0, 20000).split('\n').slice(0, 200).join('\n') : '';
    return <section key={change.id} className={styles.change} aria-label={`File change: ${change.filename}`}>
      <div className={styles.fileHeading}><span className={styles.filename}>{change.filename}</span><span className={styles.changeFacts}><span>{operations[change.operation]}</span>{change.additions !== undefined ? <span aria-label={`${change.additions} additions`}>+{change.additions}</span> : null}{change.deletions !== undefined ? <span aria-label={`${change.deletions} deletions`}>−{change.deletions}</span> : null}</span></div>
      {change.diff.state === 'text' ? <>
        {text ? <pre className={styles.diff} tabIndex={0} aria-label={`Supplied diff: ${change.filename}`}><code>{text}</code></pre> : <p>No diff text supplied.</p>}
        {change.diff.truncated ? <p>Diff truncated by the supplied view.</p> : null}
        {text.length < change.diff.text.length ? <p>Display limited to 200 lines or 20,000 characters. Remaining supplied diff text omitted.</p> : null}
      </> : <p>{change.diff.state === 'binary' ? 'Binary file · no text diff supplied.' : 'Diff unavailable in the supplied view.'}</p>}
    </section>;
  })}</div>;
}
