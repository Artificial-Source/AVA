import { useLayoutEffect, useRef, useState } from 'react';
import { WarningCircleIcon } from '@phosphor-icons/react/dist/csr/WarningCircle';
import { canFocus } from '../ui/DialogFrame';
import { Button } from '../ui/Button';
import feedback from '../ui/feedback.module.css';
import type { SubagentDraft, SubagentIntent, SubagentWorkspace } from './contracts';
import { modelChoices } from './screenExamples';
import { FormField } from './ScreenPrimitives';
import { anchor } from './settingsIndex';
import { subagentErrors, subagentLimits, type SubagentChange } from './subagentWorkspace';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';

const preferenceLabel = (value: SubagentDraft['modelPreference']) => value === 'inherit' ? 'Same as parent chat' : modelChoices.find((choice) => choice.id === value)?.label ?? 'Unknown';
const sourceLabel = { user: 'Custom', builtin: 'Built-in · read-only', project: 'Project · read-only' } as const;
type Editor = SubagentIntent;

export function SubagentsSettings({ workspace, active, onSave }: { workspace: SubagentWorkspace; active: boolean; onSave: (change: SubagentChange) => boolean }) {
  const [editor, setEditor] = useState<Editor | null>(null);
  const [deleting, setDeleting] = useState<Readonly<{ id: string; failed: boolean }> | null>(null);
  const [receipt, setReceipt] = useState('');
  // Edit buttons unmount while their row edits inline, so focus returns by trigger key (`edit:` or `delete:` + id).
  const returnTo = useRef<string | null>(null);
  const focusPending = useRef(false);
  const newButton = useRef<HTMLButtonElement>(null);
  const triggers = useRef(new Map<string, HTMLButtonElement>());
  const gate = useViewGuard(JSON.stringify([workspace.version, active, editor, deleting]));
  const busy = Boolean(editor || deleting);
  useLayoutEffect(() => {
    if (busy || !focusPending.current) return;
    focusPending.current = false;
    const target = (returnTo.current && triggers.current.get(returnTo.current)) || newButton.current;
    if (active && canFocus(target)) target.focus();
  }, [busy, active]);
  function close() { focusPending.current = true; setEditor(null); setDeleting(null); }
  function open(next: Editor) {
    if (!active || busy || !gate.valid()) return;
    returnTo.current = next.definitionId === null ? null : `edit:${next.definitionId}`; setReceipt(''); setEditor(next);
  }
  function remove(id: string) {
    const definition = workspace.definitions.find((entry) => entry.id === id);
    if (!active || !gate.valid() || !definition || definition.source !== 'user') return;
    if (!onSave({ remove: true, version: workspace.version, definitionId: definition.id, revision: definition.revision })) { setDeleting({ id, failed: true }); return; }
    returnTo.current = null; setReceipt(`Deleted ${definition.name}.`); close();
  }
  const editorFor = (definitionId: string | null) => editor && editor.definitionId === definitionId ? <SubagentEditor key={`${editor.version}:${editor.definitionId}`} initial={editor} workspace={workspace} active={active} onCancel={() => { if (active && gate.valid()) close(); }} onSave={(intent) => {
    if (!active || !gate.valid() || !onSave(intent)) return false;
    setReceipt(`${intent.definitionId === null ? 'Created' : 'Saved'} ${intent.name.trim()}.`); close(); return true;
  }} /> : null;
  return <div {...anchor('subagents.list')}>
    <div className={styles.subagentsHeader}><p className={styles.muted}>Subagents take on delegated work in a chat. Built-in and project subagents can’t be changed here.</p><Button ref={newButton} disabled={!active || busy} onClick={() => open({ version: workspace.version, definitionId: null, revision: null, name: '', description: '', modelPreference: 'inherit' })}>New subagent</Button></div>
    <p className={styles.receipt} role="status">{receipt}</p>
    {editorFor(null)}
    <ul className={styles.definitionList} aria-label="Subagent definitions">{workspace.definitions.map((definition) => <li key={definition.id}>
      {editorFor(definition.id) ?? <>
        <div className={styles.resourceHeading}><h3>{definition.name}</h3><span className={styles.tag}>{sourceLabel[definition.source]}</span></div>
        <p>{definition.description}</p>
        <div className={styles.resourceHeading}><span className={styles.muted}>Model preference · {preferenceLabel(definition.modelPreference)}</span>{definition.source === 'user' ? <div className={styles.inline}>
          <Button ref={(node) => { if (node) triggers.current.set(`edit:${definition.id}`, node); else triggers.current.delete(`edit:${definition.id}`); }} aria-label={`Edit ${definition.name}`} disabled={!active || busy}
            onClick={() => open({ version: workspace.version, definitionId: definition.id, revision: definition.revision, name: definition.name, description: definition.description, modelPreference: definition.modelPreference })}>Edit</Button>
          <Button ref={(node) => { if (node) triggers.current.set(`delete:${definition.id}`, node); else triggers.current.delete(`delete:${definition.id}`); }} tone="destructive" aria-label={`Delete ${definition.name}`} disabled={!active || busy} onClick={() => { if (active && !busy && gate.valid()) { returnTo.current = `delete:${definition.id}`; setReceipt(''); setDeleting({ id: definition.id, failed: false }); } }}>Delete</Button>
        </div> : null}</div>
        {deleting?.id === definition.id ? <DeleteConfirmation name={definition.name} failed={deleting.failed} active={active} onCancel={() => { if (gate.valid()) close(); }} onConfirm={() => remove(definition.id)} /> : null}
      </>}
    </li>)}</ul>
  </div>;
}

function DeleteConfirmation({ name, failed, active, onCancel, onConfirm }: { name: string; failed: boolean; active: boolean; onCancel: () => void; onConfirm: () => void }) {
  const cancel = useRef<HTMLButtonElement>(null);
  useLayoutEffect(() => { if (canFocus(cancel.current)) cancel.current.focus(); }, []);
  return <div className={styles.deleteConfirmation} role="group" aria-label={`Delete ${name}?`} onKeyDown={(event) => {
    if (event.key !== 'Escape' || event.nativeEvent.isComposing || event.keyCode === 229) return;
    event.preventDefault(); event.stopPropagation(); onCancel();
  }}>
    <p>Delete {name}? This can’t be undone.</p>
    {failed ? <p className={feedback.fieldError} role="alert"><WarningCircleIcon size={14} aria-hidden="true" /><span>This subagent changed. Cancel and try again.</span></p> : null}
    <div className={styles.inline}><Button ref={cancel} disabled={!active} onClick={onCancel}>Cancel</Button><Button tone="destructive" disabled={!active || failed} onClick={onConfirm}>Delete subagent</Button></div>
  </div>;
}

function SubagentEditor({ initial, workspace, active, onSave, onCancel }: { initial: Editor; workspace: SubagentWorkspace; active: boolean; onSave: (intent: SubagentIntent) => boolean; onCancel: () => void }) {
  const [draft, setDraft] = useState<SubagentDraft>(initial);
  const [attempted, setAttempted] = useState(false);
  const [rejected, setRejected] = useState(false);
  const nameRef = useRef<HTMLInputElement>(null);
  const descriptionRef = useRef<HTMLTextAreaElement>(null);
  const modelRef = useRef<HTMLSelectElement>(null);
  const gate = useViewGuard(JSON.stringify([initial, workspace.version, active, draft]));
  const errors = subagentErrors(draft, workspace.definitions, initial.definitionId);
  const current = workspace.version === initial.version && (initial.definitionId === null || workspace.definitions.some((entry) => entry.id === initial.definitionId && entry.revision === initial.revision && entry.source === 'user'));
  useLayoutEffect(() => { nameRef.current?.focus(); }, []);
  function update(patch: Partial<SubagentDraft>) { if (active && current && gate.valid()) setDraft({ ...draft, ...patch }); }
  return <form className={styles.definitionEditor} aria-label={initial.definitionId === null ? 'Create subagent' : `Edit ${initial.name}`} noValidate onKeyDown={(event) => {
    if (event.key !== 'Escape' || event.nativeEvent.isComposing || event.keyCode === 229) return;
    event.preventDefault(); event.stopPropagation();
    if (active && gate.valid()) onCancel();
  }} onSubmit={(event) => {
    event.preventDefault();
    if (!active || !current || !gate.valid()) return;
    setAttempted(true);
    // Errors use the danger tone; focus lands on the first field that needs fixing.
    const invalid = errors.name ? nameRef.current : errors.description ? descriptionRef.current : errors.modelPreference ? modelRef.current : null;
    if (invalid) { invalid.focus(); return; }
    if (!gate.claim()) return;
    if (!onSave({ ...initial, ...draft })) setRejected(true);
  }}>
    {initial.definitionId === null ? <h3>New subagent</h3> : null}
    {!current || rejected ? <p role="alert" className={`${feedback.fieldError} ${styles.editorAlert}`}><WarningCircleIcon size={14} aria-hidden="true" /><span>This subagent changed while you were editing. Cancel and open it again.</span></p> : null}
    <fieldset disabled={!active || !current || rejected} className={styles.editorFields}>
      <FormField label="Name" error={attempted ? errors.name : undefined}>{(props) => <input {...props} ref={nameRef} className={styles.input} required maxLength={subagentLimits.name} value={draft.name} onChange={(event) => update({ name: event.currentTarget.value })} />}</FormField>
      <FormField label="Description" error={attempted ? errors.description : undefined}>{(props) => <textarea {...props} ref={descriptionRef} className={styles.textarea} required maxLength={subagentLimits.description} value={draft.description} onChange={(event) => update({ description: event.currentTarget.value })} />}</FormField>
      <FormField label="Model preference" hint="Saved with this subagent. Subagents currently run on the parent chat’s model." error={attempted ? errors.modelPreference : undefined}>{(props) => <select {...props} ref={modelRef} className={styles.select} value={draft.modelPreference} onChange={(event) => update({ modelPreference: event.currentTarget.value as SubagentDraft['modelPreference'] })}><option value="inherit">Same as parent chat</option>{modelChoices.map((choice) => <option key={choice.id} value={choice.id}>{choice.label}</option>)}</select>}</FormField>
    </fieldset>
    <div className={styles.actions}><Button disabled={!active} onClick={() => { if (active && gate.valid()) onCancel(); }}>Cancel</Button><Button type="submit" tone="primary" disabled={!active || !current || rejected}>{initial.definitionId === null ? 'Create subagent' : 'Save changes'}</Button></div>
  </form>;
}
