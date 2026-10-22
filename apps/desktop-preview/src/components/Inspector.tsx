import type { ReactNode, Ref } from 'react';
import { ArrowsInLineHorizontalIcon } from '@phosphor-icons/react/dist/csr/ArrowsInLineHorizontal';
import { ArrowsOutLineHorizontalIcon } from '@phosphor-icons/react/dist/csr/ArrowsOutLineHorizontal';
import { XIcon } from '@phosphor-icons/react/dist/csr/X';
import type { InspectorModule } from '../state/types';
import { IconButton } from '../ui/Button';
import styles from './Inspector.module.css';

interface InspectorProps {
  module: InspectorModule;
  active: boolean;
  count?: number;
  /** Beside the heading: explains the rail badge (e.g. "1 working"). */
  summary?: ReactNode;
  panelRef?: Ref<HTMLElement>;
  /** The wide reading width; the shell owns it so it survives module switches. */
  wide?: boolean;
  onToggleWide?: () => void;
  onClose: () => void;
  children: ReactNode;
}

const titles: Record<InspectorModule, string> = { agents: 'Agents', changes: 'Changes', context: 'Context' };

export function Inspector({ module, active, count, summary, panelRef, wide = false, onToggleWide, onClose, children }: InspectorProps) {
  const title = titles[module];
  return (
    <aside
      ref={panelRef}
      id={module === 'agents' ? 'agents-inspector' : module === 'changes' ? 'change-review' : 'context-inspector'}
      className={styles.inspector}
      aria-label={`${title} inspector`}
      tabIndex={0}
      onKeyDown={(event) => {
        if (active && event.key === 'Escape' && !event.defaultPrevented && !event.nativeEvent.isComposing) {
          event.preventDefault();
          onClose();
        }
      }}
    >
      <div key={module} className={styles.module}>
        <header className={styles.header}>
          <h2>{title}{count !== undefined ? <> <span>{count}</span></> : null}</h2>
          {summary ? <div className={styles.summary}>{summary}</div> : null}
          <div className={styles.tools}>
            {onToggleWide ? (
              // One stable name with a pressed state; the tooltip and glyph name the next width.
              <IconButton className={styles.widen} variant="quiet" label="Widen inspector" aria-pressed={wide} title={wide ? 'Narrow inspector' : 'Widen inspector'} onClick={() => { if (active) onToggleWide(); }}>
                {wide ? <ArrowsInLineHorizontalIcon size={15} aria-hidden="true" /> : <ArrowsOutLineHorizontalIcon size={15} aria-hidden="true" />}
              </IconButton>
            ) : null}
            <IconButton variant="quiet" label={`Close ${module}`} aria-keyshortcuts="Escape" title={`Close ${module} · Esc`} onClick={() => { if (active) onClose(); }}>
              <XIcon size={15} aria-hidden="true" />
            </IconButton>
          </div>
        </header>
        <div className={styles.content} role="region" tabIndex={0} aria-label={module === 'agents' ? 'Child agents' : module === 'changes' ? 'Changes in this chat' : 'Context information'}>
          {children}
        </div>
      </div>
    </aside>
  );
}
