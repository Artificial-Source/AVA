import { Fragment, useId, useLayoutEffect, useRef, useState } from 'react';
import { WarningCircleIcon } from '@phosphor-icons/react/dist/csr/WarningCircle';
import { canFocus } from '../ui/DialogFrame';
import { Button } from '../ui/Button';
import feedback from '../ui/feedback.module.css';
import type { BehaviorSettingsProps } from './GeneralSettings';
import { PreferenceRow } from './ScreenPrimitives';
import { anchor } from './settingsIndex';
import { bindingLabel, bindingProblem, defaultShortcuts, effectiveBinding, navigationKeys, recordedBinding, shortcutActions, shortcutGroups, type ShortcutAction, type ShortcutPreferences } from './shortcuts';
import { useViewGuard } from './useViewGuard';
import styles from './BehaviorSettings.module.css';
import controls from './screens.module.css';

// `binding` undefined means "back to the default"; the fix also unassigns `from`.
type Problem = Readonly<{ id: ShortcutAction; message: string; fix?: Readonly<{ from: ShortcutAction; fromLabel: string; binding: string | undefined }> }>;
type Overrides = Partial<Record<ShortcutAction, string | null>>;
// An override equal to its default is not a customization.
function normalized(overrides: Overrides): ShortcutPreferences['overrides'] {
  for (const action of shortcutActions) if (Object.hasOwn(overrides, action.id) && overrides[action.id] === action.binding) delete overrides[action.id];
  return overrides;
}

export function ShortcutsSettings({ preferences, active, allowed, update }: BehaviorSettingsProps) {
  const value = preferences.shortcuts ?? defaultShortcuts;
  const [recording, setRecording] = useState<ShortcutAction | null>(null);
  const [problem, setProblem] = useState<Problem | null>(null);
  const composing = useRef(false);
  const keycaps = useRef(new Map<ShortcutAction, HTMLButtonElement>());
  const refocus = useRef<ShortcutAction | null>(null);
  const id = useId();
  const gate = useViewGuard(JSON.stringify([value, recording, active]));
  // Unassign/Reset can remove the focused button; keep focus on the row's keycap instead.
  useLayoutEffect(() => {
    const target = refocus.current ? keycaps.current.get(refocus.current) ?? null : null;
    refocus.current = null;
    if (active && canFocus(target)) target.focus();
  });
  function valid() { return allowed() && gate.valid(); }
  function apply(next: ShortcutPreferences, target: ShortcutAction) {
    refocus.current = target; setRecording(null); setProblem(null); update({ shortcuts: next });
  }
  // A recorded string, null to unassign, or undefined to restore the default.
  function assign(target: ShortcutAction, binding: string | null | undefined) {
    if (!valid()) return;
    const reserved = binding ? bindingProblem(binding) : null;
    if (reserved) { setProblem({ id: target, message: reserved }); return; }
    const overrides: Overrides = { ...value.overrides };
    if (binding === undefined) delete overrides[target]; else overrides[target] = binding;
    const next = { ...value, overrides: normalized(overrides) };
    const effective = effectiveBinding(next, target);
    const taken = effective ? shortcutActions.find((action) => action.id !== target && effectiveBinding(value, action.id) === effective) : undefined;
    if (effective && taken) {
      refocus.current = target; setRecording(null);
      setProblem({ id: target, message: binding === undefined ? `The default, ${bindingLabel(effective)}, is used by ${taken.label}.` : `${bindingLabel(effective)} is already used by ${taken.label}.`, fix: { from: taken.id, fromLabel: taken.label, binding: binding ?? undefined } });
      return;
    }
    apply(next, target);
  }
  function move(target: ShortcutAction, fix: NonNullable<Problem['fix']>) {
    if (!valid()) return;
    const overrides: Overrides = { ...value.overrides, [fix.from]: null };
    if (fix.binding === undefined) delete overrides[target]; else overrides[target] = fix.binding;
    apply({ ...value, overrides: normalized(overrides) }, target);
  }
  return <div onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }}>
    <div {...anchor('shortcuts.enabled')}><PreferenceRow title="App shortcuts" description="Ctrl on Windows and Linux, Cmd on macOS. Typing and text editing keys keep working as usual."><input type="checkbox" aria-label="Enable app shortcuts" disabled={!active} checked={value.enabled} onChange={(event) => { if (valid()) { setRecording(null); setProblem(null); update({ shortcuts: { ...value, enabled: event.currentTarget.checked } }); } }} /></PreferenceRow></div>
    <p id={`${id}-hint`} className={styles.recordHint}>Select a shortcut, then press the new keys. Esc cancels.</p>
    {shortcutGroups.map((group) => {
      const actions = shortcutActions.filter((action) => action.group === group);
      return actions.length ? <fieldset className={styles.group} disabled={!active} key={group}><legend>{group}</legend>
        <table className={styles.shortcutTable} aria-label={`${group} shortcuts`}>
          <thead className="srOnly"><tr><th scope="col">Action</th><th scope="col">Shortcut</th><td /></tr></thead>
          <tbody>{actions.map((action) => {
            const binding = effectiveBinding(value, action.id); const custom = Object.hasOwn(value.overrides, action.id);
            const listening = recording === action.id; const issue = problem?.id === action.id ? problem : null;
            const keyText = listening ? 'Press keys… Esc cancels' : bindingLabel(binding);
            return <Fragment key={action.id}>
              <tr className={styles.shortcutRow} data-recording={listening || undefined} {...anchor(`shortcuts.${action.id}`)}>
                <th scope="row"><span>{action.label}</span><small>{action.detail}</small>{custom ? <small>Default: {bindingLabel(action.binding)}</small> : null}</th>
                <td><button type="button" className={styles.keycap} data-state={listening ? 'recording' : binding ? 'bound' : 'unassigned'}
                  ref={(node) => { if (node) keycaps.current.set(action.id, node); else keycaps.current.delete(action.id); }}
                  aria-label={`${action.label} shortcut: ${keyText}`} aria-describedby={issue ? `${id}-${action.id}-problem ${id}-hint` : `${id}-hint`}
                  onClick={() => { if (valid()) { setRecording(listening ? null : action.id); setProblem(null); } }}
                  onBlur={() => { if (listening) setRecording(null); }}
                  onKeyUp={(event) => { if (listening && event.key === ' ') event.preventDefault(); }}
                  onKeyDown={(event) => {
                    if (!listening || event.key === 'Tab') return;
                    event.preventDefault(); event.stopPropagation();
                    if (!valid() || composing.current || event.nativeEvent.isComposing || event.keyCode === 229 || event.repeat) return;
                    if (event.key === 'Escape') { setRecording(null); setProblem(null); return; }
                    if (['Control', 'Meta', 'Shift', 'Alt', 'AltGraph'].includes(event.key)) return;
                    const candidate = recordedBinding(event.nativeEvent);
                    if (!candidate) setProblem({ id: action.id, message: 'Hold Ctrl or Cmd with another key. Keys you type can’t be shortcuts on their own.' });
                    else assign(action.id, candidate);
                  }}>{binding && !listening ? <kbd>{keyText}</kbd> : keyText}</button></td>
                <td><div className={styles.rowActions}>
                  {binding ? <Button className={styles.rowAction} aria-label={`Unassign ${action.label}`} onClick={() => assign(action.id, null)}>Unassign</Button> : null}
                  {custom ? <Button className={styles.rowAction} aria-label={`Reset ${action.label} shortcut`} onClick={() => assign(action.id, undefined)}>Reset</Button> : null}
                </div></td>
              </tr>
              {issue ? <tr className={styles.problemRow}><td colSpan={3}><div className={styles.problem}>
                <p id={`${id}-${action.id}-problem`} className={feedback.fieldError} role="alert"><WarningCircleIcon size={14} aria-hidden="true" /><span>{issue.message}</span></p>
                {issue.fix ? <Button onClick={() => move(action.id, issue.fix!)}>Move from {issue.fix.fromLabel}</Button> : null}
              </div></td></tr> : null}
            </Fragment>;
          })}</tbody>
        </table>
      </fieldset> : null;
    })}
    <fieldset className={styles.group}><legend>Navigation</legend>
      <p id={`${id}-fixed`} className={controls.hint}>Always on. These keys can’t be changed.</p>
      <table className={styles.shortcutTable} aria-label="Navigation keys" aria-describedby={`${id}-fixed`}>
        <thead className="srOnly"><tr><th scope="col">Action</th><th scope="col">Keys</th></tr></thead>
        <tbody>{navigationKeys().map((entry) => <tr key={entry.id} className={styles.shortcutRow}>
          <th scope="row"><span>{entry.label}</span><small>{entry.detail}</small></th>
          <td>{entry.keys.map((key, index) => <Fragment key={key}>{index ? ' ' : null}<kbd className={styles.fixedKey}>{key}</kbd></Fragment>)}</td>
        </tr>)}</tbody>
      </table>
    </fieldset>
    <div className={styles.reset}><p>Restore every default shortcut and turn app shortcuts on.</p><Button disabled={!active || value.enabled && !Object.keys(value.overrides).length} onClick={() => { if (valid()) { setRecording(null); setProblem(null); update({ shortcuts: defaultShortcuts }); } }}>Reset all shortcuts</Button></div>
  </div>;
}
