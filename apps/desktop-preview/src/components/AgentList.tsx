import { ArrowRightIcon } from '@phosphor-icons/react/dist/csr/ArrowRight';
import { modelOptions } from '../state/fixtures';
import type { ChildAgent } from '../state/types';
import { ChatStatus } from './ChatStatus';
import styles from './AgentList.module.css';

export function AgentList({ children, inspectedId, onInspect }: {
  children: ChildAgent[];
  inspectedId: string | null;
  onInspect: (childId: string) => void;
}) {
  if (children.length === 0) {
    return (
      <div className={styles.empty}>
        <h3>No child agents</h3>
        <p>Agents that AVA starts for this chat appear here.</p>
      </div>
    );
  }
  return (
    <ul className={styles.list} aria-label="Child agents of this chat">
      {children.map((child) => (
        <li key={child.id}>
          <button
            className={styles.child}
            type="button"
            data-child-id={child.id}
            aria-current={inspectedId === child.id ? 'true' : undefined}
            aria-controls="conversation-canvas"
            title={`View ${child.title}`}
            onClick={() => onInspect(child.id)}
          >
            <span className={styles.task}><ChatStatus status={child.status} />{' '}<span className={styles.title}>{child.title}</span></span>{' '}
            <span className={styles.meta}>{modelOptions.find((model) => model.id === child.model)!.label} <span aria-hidden="true">·</span> {child.timing}</span>{' '}
            <span className={styles.open}>View conversation <ArrowRightIcon size={12} aria-hidden="true" /></span>
          </button>
        </li>
      ))}
    </ul>
  );
}
