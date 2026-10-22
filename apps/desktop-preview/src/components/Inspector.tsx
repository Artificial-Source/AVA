import type { ReactNode } from 'react';
import { XIcon } from '@phosphor-icons/react/dist/csr/X';
import type { InspectorModule } from '../state/types';
import styles from './Inspector.module.css';

interface InspectorProps {
  module: InspectorModule;
  active: boolean;
  count?: number;
  onClose: () => void;
  children: ReactNode;
}

export function Inspector({ module, active, count, onClose, children }: InspectorProps) {
  const title = module === 'agents' ? 'Agents' : module === 'changes' ? 'Changes' : 'Context';
  return (
    <aside
      id={module === 'agents' ? 'agents-inspector' : module === 'changes' ? 'change-review' : 'context-inspector'}
      className={styles.inspector}
      aria-label={module === 'agents' ? 'Agents inspector' : module === 'changes' ? 'Mock changes' : 'Context inspector'}
      tabIndex={0}
      onKeyDown={(event) => {
        if (active && event.key === 'Escape' && !event.nativeEvent.isComposing) {
          event.preventDefault();
          onClose();
        }
      }}
    >
      <div key={module} className={styles.module}>
        <header className={styles.header}>
          <h2>{title}{count !== undefined ? <> <span>{count}</span></> : null}</h2>
          <button className="iconButton" type="button" aria-label={`Close ${module}`} onClick={() => { if (active) onClose(); }}>
            <XIcon size={15} aria-hidden="true" />
          </button>
        </header>
        <div className={styles.content} role="region" tabIndex={0} aria-label={module === 'agents' ? 'Child agent examples' : module === 'changes' ? 'Example change excerpts' : 'Context information'}>
          {children}
        </div>
      </div>
    </aside>
  );
}
