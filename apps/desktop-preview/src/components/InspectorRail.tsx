import type { RefObject } from 'react';
import { GitDiffIcon } from '@phosphor-icons/react/dist/csr/GitDiff';
import { SidebarSimpleIcon } from '@phosphor-icons/react/dist/csr/SidebarSimple';
import { StackIcon } from '@phosphor-icons/react/dist/csr/Stack';
import { UsersThreeIcon } from '@phosphor-icons/react/dist/csr/UsersThree';
import { shortcutHint, type ShortcutPreferences } from '../screens/shortcuts';
import type { InspectorModule } from '../state/types';
import styles from './InspectorRail.module.css';

interface InspectorRailProps {
  selected: InspectorModule | null;
  lastModule: InspectorModule;
  onToggleVisibility: () => void;
  activeCount: number;
  childCount: number;
  changeCount: number;
  shortcuts?: ShortcutPreferences;
  agentsRef: RefObject<HTMLButtonElement | null>;
  changesRef: RefObject<HTMLButtonElement | null>;
  contextRef: RefObject<HTMLButtonElement | null>;
  onToggle: (module: InspectorModule) => void;
}

const panelIds: Record<InspectorModule, string> = { agents: 'agents-inspector', changes: 'change-review', context: 'context-inspector' };
const moduleLabels: Record<InspectorModule, string> = { agents: 'Agents', changes: 'Changes', context: 'Context' };

export function InspectorRail({ selected, lastModule, onToggleVisibility, activeCount, childCount, changeCount, shortcuts, agentsRef, changesRef, contextRef, onToggle }: InspectorRailProps) {
  const toggleLabel = selected ? 'Hide inspector' : 'Show inspector';
  // Tooltip facts first, then the key: "Agents · 1 working · Ctrl+Shift+1".
  const tip = (parts: readonly string[], action: 'inspector' | InspectorModule) => {
    const hint = shortcutHint(shortcuts, action);
    return { title: [...parts, ...(hint ? [hint.label] : [])].join(' · '), 'aria-keyshortcuts': hint?.aria };
  };
  const agentsFact = activeCount > 0 ? `${activeCount} working` : childCount > 0 ? `${childCount} ${childCount === 1 ? 'agent' : 'agents'}` : 'No agents';

  return (
    <nav className={styles.rail} aria-label="Inspectors">
      <button
        className={styles.visibility}
        type="button"
        aria-label={toggleLabel}
        aria-expanded={selected !== null}
        aria-controls={selected ? panelIds[selected] : undefined}
        {...tip([toggleLabel, moduleLabels[lastModule]], 'inspector')}
        onClick={onToggleVisibility}
      ><SidebarSimpleIcon size={17} mirrored aria-hidden="true" /></button>
      <button
        ref={agentsRef}
        className={styles.module}
        type="button"
        aria-label="Agents"
        aria-describedby="agents-count"
        aria-expanded={selected === 'agents'}
        aria-controls={selected === 'agents' ? panelIds.agents : undefined}
        {...tip(['Agents', agentsFact], 'agents')}
        onClick={() => onToggle('agents')}
      >
        <UsersThreeIcon size={19} aria-hidden="true" />
        {activeCount > 0 ? <span className={styles.count} aria-hidden="true">{activeCount}</span> : null}
      </button>
      <span id="agents-count" className="srOnly">{activeCount} working</span>
      <button
        ref={changesRef}
        className={styles.module}
        type="button"
        aria-label="Changes"
        aria-expanded={selected === 'changes'}
        aria-controls={selected === 'changes' ? panelIds.changes : undefined}
        {...tip(['Changes', changeCount === 0 ? 'No changes' : `${changeCount} ${changeCount === 1 ? 'file' : 'files'}`], 'changes')}
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
        aria-controls={selected === 'context' ? panelIds.context : undefined}
        {...tip(['Context', 'Window usage and prompt'], 'context')}
        onClick={() => onToggle('context')}
      >
        <StackIcon size={19} aria-hidden="true" />
      </button>
    </nav>
  );
}
