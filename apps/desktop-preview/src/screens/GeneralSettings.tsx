import type { Project } from '../state/types';
import { Button } from '../ui/Button';
import type { Preferences } from './contracts';
import { defaultGeneral, type GeneralPreferences } from './behaviorPreferences';
import { PreferenceRow } from './ScreenPrimitives';
import styles from './BehaviorSettings.module.css';
import controls from './screens.module.css';

export interface BehaviorSettingsProps {
  preferences: Preferences;
  active: boolean;
  allowed: () => boolean;
  update: (patch: Partial<Preferences>) => void;
}
export function GeneralSettings({ preferences, active, allowed, update, projects = [], review = false }: BehaviorSettingsProps & { projects?: readonly Project[]; review?: boolean }) {
  const value = preferences.general ?? defaultGeneral;
  function change(patch: Partial<GeneralPreferences>) { if (allowed()) update({ general: { ...value, ...patch } }); }
  const changed = preferences.enter !== 'send' || JSON.stringify(value) !== JSON.stringify(defaultGeneral);
  return <div>
    <p className={styles.intro}>Small defaults for your everyday flow. Changes stay in this visit.{review ? ' This independent review does not control the main chat.' : ' New-chat preferences leave existing chats unchanged.'}</p>
    <fieldset className={styles.group} disabled={!active}><legend>Input</legend>
      <PreferenceRow title="Enter key" description="Shift+Enter adds a line. Ctrl/Cmd+Enter sends. Composition stays native."><select className={controls.select} aria-label="Enter key behavior" value={preferences.enter} onChange={(event) => { const enter = event.currentTarget.value; if (allowed() && (enter === 'send' || enter === 'newline')) update({ enter }); }}><option value="send">Send message</option><option value="newline">New line</option></select></PreferenceRow>
    </fieldset>
    <fieldset className={styles.group} disabled={!active}><legend>New chats</legend>
      <PreferenceRow title="Default mode" description="Start future chats in Build or Plan."><select className={controls.select} aria-label="New-chat mode" value={value.mode} onChange={(event) => { const mode = event.currentTarget.value; if (mode === 'build' || mode === 'plan') change({ mode }); }}><option value="build">Build</option><option value="plan">Plan</option></select></PreferenceRow>
      <PreferenceRow title="Project" description="Use the viewed chat’s project, or a supplied project."><select className={controls.select} aria-label="New-chat project" value={value.projectId} onChange={(event) => { const projectId = event.currentTarget.value; if (projectId === 'current' || projects.some((project) => project.id === projectId)) change({ projectId }); }}><option value="current">Current project</option>{projects.map((project) => <option key={project.id} value={project.id}>{project.name}</option>)}</select></PreferenceRow>
      <PreferenceRow title="Name from first message" description="Use the first local message as a short title, once. No model or title service."><input type="checkbox" aria-label="Name new chats from first message" checked={value.titleFromFirstMessage} onChange={(event) => change({ titleFromFirstMessage: event.currentTarget.checked })} /></PreferenceRow>
    </fieldset>
    <fieldset className={styles.group} disabled={!active}><legend>Layout</legend>
      <PreferenceRow title="Inspector on new chats" description="Remember the current layout, or start with a particular tab."><select className={controls.select} aria-label="New-chat inspector" value={value.inspector} onChange={(event) => { const inspector = event.currentTarget.value; if (inspector === 'remember' || inspector === 'closed' || inspector === 'agents' || inspector === 'changes' || inspector === 'context') change({ inspector }); }}><option value="remember">Remember</option><option value="agents">Agents</option><option value="changes">Changes</option><option value="context">Context</option><option value="closed">Closed</option></select></PreferenceRow>
    </fieldset>
    <div className={styles.reset}><p>Restore Input, New chats and Layout only.</p><Button disabled={!active || !changed} onClick={() => { if (changed && allowed()) update({ enter: 'send', general: defaultGeneral }); }}>Reset General</Button></div>
  </div>;
}
