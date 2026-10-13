import type { RefObject } from 'react';
import { GitDiffIcon } from '@phosphor-icons/react/dist/csr/GitDiff';
import { SidebarSimpleIcon } from '@phosphor-icons/react/dist/csr/SidebarSimple';
import { StackIcon } from '@phosphor-icons/react/dist/csr/Stack';
import { UsersThreeIcon } from '@phosphor-icons/react/dist/csr/UsersThree';
import type { InspectorModule } from '../state/types';
import styles from './InspectorRail.module.css';

interface InspectorRailProps {
  selected: InspectorModule | null;
  lastModule: InspectorModule;
  onToggleVisibility: () => void;
  activeCount: number;
  parentTitle: string;
  agentsRef: RefObject<HTMLButtonElement | null>;
  changesRef: RefObject<HTMLButtonElement | null>;
  contextRef: RefObject<HTMLButtonElement | null>;
  onToggle: (module: InspectorModule) => void;
}

export function InspectorRail({ selected, lastModule, onToggleVisibility, activeCount, parentTitle, agentsRef, changesRef, contextRef, onToggle }: InspectorRailProps) {
  const toggleLabel = selected ? 'Hide inspector' : 'Show inspector';
  const moduleLabel = lastModule === 'agents' ? 'Agents' : lastModule === 'changes' ? 'Changes' : 'Context';

  return (
    <nav className={styles.rail} aria-label="Inspectors">
      <button
        className={styles.visibility}
        type="button"
        aria-label={toggleLabel}
        aria-expanded={selected !== null}
        aria-controls={selected ? (selected === 'agents' ? 'agents-inspector' : selected === 'changes' ? 'change-review' : 'context-inspector') : undefined}
        title={`${toggleLabel} · ${moduleLabel}`}
        onClick={onToggleVisibility}
      ><SidebarSimpleIcon size={17} mirrored aria-hidden="true" /></button>
      <button
        ref={agentsRef}
        className={styles.module}
        type="button"
        aria-label="Agents"
        aria-describedby="agents-count"
        aria-expanded={selected === 'agents'}
        aria-controls={selected === 'agents' ? 'agents-inspector' : undefined}
        title={`Agents · ${activeCount} working for ${parentTitle} · fixed examples`}
        onClick={() => onToggle('agents')}
      >
        <UsersThreeIcon size={19} aria-hidden="true" />
        {activeCount > 0 ? <span className={styles.count} aria-hidden="true">{activeCount}</span> : null}
      </button>
      <span id="agents-count" className="srOnly">{activeCount} working child examples for {parentTitle}. Not live activity.</span>
      <button
        ref={changesRef}
        className={styles.module}
        type="button"
        aria-label="Changes"
        aria-expanded={selected === 'changes'}
        aria-controls={selected === 'changes' ? 'change-review' : undefined}
        title="Changes · read-only examples for the parent chat"
        onClick={() => onToggle('changes')}
      >
        <GitDiffIcon size={19} aria-hidden="true" />
      </button>
      <button
        ref={contextRef}
        className={styles.module}
        type="button"
        aria-label="Context"
        aria-expanded={selected === 'context'}
        aria-controls={selected === 'context' ? 'context-inspector' : undefined}
        title="Context · selected parent profile and local snapshots"
        onClick={() => onToggle('context')}
      >
        <StackIcon size={19} aria-hidden="true" />
      </button>
    </nav>
  );
}
