import { ArrowUpRightIcon } from '@phosphor-icons/react/dist/csr/ArrowUpRight';
import { modelOptions } from '../state/fixtures';
import type { ChildAgent } from '../state/types';
import { ChatStatus } from './ChatStatus';
import styles from './AgentList.module.css';

export function AgentList({ children, inspectedId, onInspect }: {
  children: ChildAgent[];
  inspectedId: string | null;
  onInspect: (childId: string) => void;
}) {
  return (
    <div>
      <p className={styles.scope}>Children of this chat <span>Fixed examples · read only</span></p>
      {children.length === 0 ? (
        <div className={styles.empty}>
          <h3>No child agents</h3>
          <p>This parent has no child examples. Nothing is running in the background.</p>
        </div>
      ) : (
        <ul className={styles.list}>
          {children.map((child) => (
            <li key={child.id}>
              <button
                className={styles.child}
                type="button"
                data-child-id={child.id}
                aria-current={inspectedId === child.id ? 'true' : undefined}
                aria-controls="conversation-canvas"
                title={`Inspect ${child.title} · read only`}
                onClick={() => onInspect(child.id)}
              >
                <span className={styles.task}><ChatStatus status={child.status} />{' '}<span className={styles.title}>{child.title}</span></span>{' '}
                <span className={styles.meta}>{modelOptions.find((model) => model.id === child.model)!.label} <span aria-hidden="true">·</span> {child.timing}</span>{' '}
                <span className={styles.open}>View conversation <ArrowUpRightIcon size={12} aria-hidden="true" /></span>
              </button>
            </li>
          ))}
        </ul>
      )}
      <p className={styles.notice}>These examples do not track the parent’s status. No live agent is connected.</p>
    </div>
  );
}
