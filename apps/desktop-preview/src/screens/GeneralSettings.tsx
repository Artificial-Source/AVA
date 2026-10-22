import type { Project } from '../state/types';
import { Button } from '../ui/Button';
import type { Preferences } from './contracts';
import { defaultGeneral, type GeneralPreferences } from './behaviorPreferences';
import { PreferenceRow } from './ScreenPrimitives';
import { anchor } from './settingsIndex';
import styles from './BehaviorSettings.module.css';
import controls from './screens.module.css';

export interface BehaviorSettingsProps {
  preferences: Preferences;
  active: boolean;
  allowed: () => boolean;
  update: (patch: Partial<Preferences>) => void;
}
export function GeneralSettings({ preferences, active, allowed, update, projects = [] }: BehaviorSettingsProps & { projects?: readonly Project[] }) {
  const value = preferences.general ?? defaultGeneral;
  function change(patch: Partial<GeneralPreferences>) { if (allowed()) update({ general: { ...value, ...patch } }); }
  const changed = preferences.enter !== 'send' || JSON.stringify(value) !== JSON.stringify(defaultGeneral);
  return <div>
    <fieldset className={styles.group} disabled={!active}><legend>Input</legend>
      <div {...anchor('general.enter')}><PreferenceRow title="Enter key" description="Shift+Enter always adds a line. Ctrl+Enter or Cmd+Enter always sends."><select className={controls.select} aria-label="Enter key behavior" value={preferences.enter} onChange={(event) => { const enter = event.currentTarget.value; if (allowed() && (enter === 'send' || enter === 'newline')) update({ enter }); }}><option value="send">Send message</option><option value="newline">New line</option></select></PreferenceRow></div>
    </fieldset>
    <fieldset className={styles.group} disabled={!active}><legend>New chats</legend>
      <div {...anchor('general.mode')}><PreferenceRow title="Default mode" description="New chats start in Build or Plan."><select className={controls.select} aria-label="New-chat mode" value={value.mode} onChange={(event) => { const mode = event.currentTarget.value; if (mode === 'build' || mode === 'plan') change({ mode }); }}><option value="build">Build</option><option value="plan">Plan</option></select></PreferenceRow></div>
      <div {...anchor('general.project')}><PreferenceRow title="Project" description="Where new chats are created."><select className={controls.select} aria-label="New-chat project" value={value.projectId} onChange={(event) => { const projectId = event.currentTarget.value; if (projectId === 'current' || projects.some((project) => project.id === projectId)) change({ projectId }); }}><option value="current">Current project</option>{projects.map((project) => <option key={project.id} value={project.id}>{project.name}</option>)}</select></PreferenceRow></div>
      <div {...anchor('general.title')}><PreferenceRow title="Name from first message" description="Title each new chat from your first message."><input type="checkbox" aria-label="Name new chats from first message" checked={value.titleFromFirstMessage} onChange={(event) => change({ titleFromFirstMessage: event.currentTarget.checked })} /></PreferenceRow></div>
    </fieldset>
    <fieldset className={styles.group} disabled={!active}><legend>Layout</legend>
      <div {...anchor('general.inspector')}><PreferenceRow title="Inspector on new chats" description="Remember the current layout, or start with a tab open."><select className={controls.select} aria-label="New-chat inspector" value={value.inspector} onChange={(event) => { const inspector = event.currentTarget.value; if (inspector === 'remember' || inspector === 'closed' || inspector === 'agents' || inspector === 'changes' || inspector === 'context') change({ inspector }); }}><option value="remember">Remember</option><option value="agents">Agents</option><option value="changes">Changes</option><option value="context">Context</option><option value="closed">Closed</option></select></PreferenceRow></div>
    </fieldset>
    <div className={styles.reset}><p>Restore Input, New chats and Layout.</p><Button disabled={!active || !changed} onClick={() => { if (changed && allowed()) update({ enter: 'send', general: defaultGeneral }); }}>Reset general</Button></div>
  </div>;
}
