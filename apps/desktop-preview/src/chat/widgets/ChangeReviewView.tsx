import { FileCodeIcon } from '@phosphor-icons/react/dist/csr/FileCode';
import { Disclosure } from '../../ui/Disclosure';
import type { ChangeReviewState } from '../contracts';
import { useId } from 'react';
import styles from '../../components/ChangeReview.module.css';
import kit from './widgets.module.css';

export function ChangeReviewView({ view, notice, expandedIds, onExpandedChange }: {
  view: ChangeReviewState;
  notice: string;
  expandedIds?: readonly string[];
  onExpandedChange?: (id: string, open: boolean) => void;
}) {
  const id = useId();
  return <div>
    <p className={styles.notice}>{notice}</p>
    {view.state === 'loading' ? <p role="status" className={styles.empty}>Loading change excerpts…</p> : null}
    {view.state === 'empty' || view.state === 'error' || view.state === 'stale' ? <p className={styles.empty} role={view.state === 'error' ? 'alert' : undefined}>{view.message}</p> : null}
    {view.state === 'ready' || view.state === 'stale' ? view.changes.map((change, index) => {
      const content = <><div className={styles.before}><h4>Before</h4><pre><code>{change.before}</code></pre></div><div className={styles.after}><h4>After</h4><pre><code>{change.after}</code></pre></div></>;
      return <section className={styles.change} key={change.id}>
        <h3><FileCodeIcon size={15} aria-hidden="true" />{change.filename}</h3>
        <p>{change.description}</p>
        {onExpandedChange ? <Disclosure id={`${id}-${index}`} open={expandedIds?.includes(change.id) ?? false} onOpenChange={(open) => onExpandedChange(change.id, open)} triggerClassName={kit.excerptToggle} trigger={`Excerpts for ${change.filename}`}>{content}</Disclosure> : content}
      </section>;
    }) : null}
  </div>;
}
