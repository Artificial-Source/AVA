import { useLayoutEffect, useRef, useState } from 'react';
import { canFocus } from '../ui/DialogFrame';
import { Button } from '../ui/Button';
import type { BehaviorSettingsProps } from './GeneralSettings';
import { PreferenceRow, SearchField } from './ScreenPrimitives';
import { bindingConflict, bindingLabel, bindingProblem, defaultShortcuts, effectiveBinding, recordedBinding, shortcutActions, type ShortcutAction } from './shortcuts';
import { useViewGuard } from './useViewGuard';
import styles from './BehaviorSettings.module.css';

export function ShortcutsSettings({ preferences, active, allowed, update }: BehaviorSettingsProps) {
  const value = preferences.shortcuts ?? defaultShortcuts;
  const [query, setQuery] = useState('');
  const [recording, setRecording] = useState<ShortcutAction | null>(null);
  const [error, setError] = useState('');
  const composing = useRef(false);
  const recorder = useRef<HTMLDivElement>(null);
  const opener = useRef<HTMLButtonElement>(null);
  const gate = useViewGuard(JSON.stringify([value, query, recording, active]));
  useLayoutEffect(() => { if (recording && active && canFocus(recorder.current)) recorder.current.focus(); }, [recording, active]);
  const matches = shortcutActions.filter((action) => `${action.label} ${action.detail} ${action.group} ${bindingLabel(effectiveBinding(value, action.id))}`.toLowerCase().includes(query.trim().toLowerCase()));
  function valid() { return allowed() && gate.valid(); }
  function cancel() { if (valid()) { setRecording(null); setError(''); if (canFocus(opener.current)) opener.current.focus(); } }
  function bind(id: ShortcutAction, binding: string | null | undefined) {
    if (!valid() || !matches.some((action) => action.id === id)) return;
    if (binding) {
      const problem = bindingProblem(binding); const conflict = bindingConflict(value, id, binding);
      if (problem || conflict) { setError(problem ?? `Already assigned to ${conflict}. Unassign that action first.`); return; }
    }
    const overrides = { ...value.overrides };
    if (binding === undefined) delete overrides[id]; else overrides[id] = binding;
    // Reset must not silently create a conflict with another customized action.
    const next = { ...value, overrides }; const effective = effectiveBinding(next, id);
    const conflict = effective && bindingConflict(next, id, effective);
    if (conflict) { setError(`Default is assigned to ${conflict}. Unassign it first, or reset all shortcuts.`); return; }
    cancel(); update({ shortcuts: next });
  }
  return <div onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }}>
    <p className={styles.intro}>Keys for real local app actions. Ctrl on Windows/Linux, Cmd on macOS. Native editing and General’s Enter behavior are always separate.</p>
    <PreferenceRow title="App shortcuts" description="Active inside the chat workspace, outside editors, menus and dialogs. Never intercept browser chrome."><input type="checkbox" aria-label="Enable app shortcuts" disabled={!active} checked={value.enabled} onChange={(event) => { if (valid()) { update({ shortcuts: { ...value, enabled: event.currentTarget.checked } }); setRecording(null); setError(''); } }} /></PreferenceRow>
    <div className={styles.toolbar}><SearchField label="Search shortcuts" value={query} onChange={(text) => { if (valid()) { setQuery(text); setRecording(null); setError(''); } }} /></div>
    {['Application', 'Conversation', 'Inspector'].map((group) => {
      const actions = matches.filter((action) => action.group === group);
      return actions.length ? <fieldset className={styles.group} disabled={!active} key={group}><legend>{group}</legend><ul className={styles.list}>{actions.map((action) => {
        const binding = effectiveBinding(value, action.id); const custom = Object.hasOwn(value.overrides, action.id);
        return <li className={styles.shortcut} key={action.id}>
          <div><strong>{action.label}</strong><small>{action.detail}</small><small>{custom ? 'Custom' : 'Default'} · Default: {bindingLabel(action.binding)}</small></div>
          <div className={styles.binding}><Button aria-label={`Record shortcut: ${action.label}`} aria-expanded={recording === action.id} onClick={(event) => { if (valid()) { opener.current = event.currentTarget; if (recording === action.id && canFocus(recorder.current)) recorder.current.focus(); setRecording(action.id); setError(''); } }}><kbd>{bindingLabel(binding)}</kbd></Button><Button aria-label={`Unassign ${action.label}`} disabled={!binding} onClick={() => bind(action.id, null)}>Unassign</Button><Button aria-label={`Reset shortcut: ${action.label}`} disabled={!custom} onClick={() => bind(action.id, undefined)}>Reset</Button></div>
          {recording === action.id ? <div className={styles.recording} role="group" aria-label={`Recording ${action.label}`} tabIndex={0} ref={recorder} onKeyDown={(event) => {
            if (event.key === 'Tab') return;
            event.stopPropagation();
            if (!valid() || composing.current || event.nativeEvent.isComposing || event.keyCode === 229 || event.repeat) return;
            if (event.key === 'Escape') { event.preventDefault(); cancel(); return; }
            if (event.target !== event.currentTarget) return;
            if (['Control', 'Meta', 'Shift', 'Alt'].includes(event.key)) return;
            event.preventDefault();
            const candidate = recordedBinding(event.nativeEvent);
            if (!candidate) setError('Use Ctrl/Cmd with another key. AltGraph, typing and modifier-only keys are not app shortcuts.'); else bind(action.id, candidate);
          }}><p>Press a new shortcut. Esc cancels; Tab moves to Cancel. Try Ctrl/Cmd + Shift + period.</p>{error ? <p className={styles.error} role="alert">{error}</p> : null}<Button onClick={cancel}>Cancel recording</Button></div> : null}
        </li>;
      })}</ul></fieldset> : null;
    })}
    {!matches.length ? <p className={styles.hint} role="status">No matching shortcuts. Try “inspector” or “chat”.</p> : null}
    {error && !recording ? <p className={styles.error} role="alert">{error}</p> : null}
    <p className={styles.hint}>Tab, Space, Enter and Escape keep their native jobs. Stop and inspector visibility are unassigned by default. Read-only child views never target a hidden parent composer or run.</p>
    <div className={styles.reset}><p>Restore every default binding and enable app shortcuts.</p><Button disabled={!active || value.enabled && !Object.keys(value.overrides).length} onClick={() => { if (valid()) { update({ shortcuts: defaultShortcuts }); setRecording(null); setError(''); } }}>Reset all shortcuts</Button></div>
  </div>;
}
