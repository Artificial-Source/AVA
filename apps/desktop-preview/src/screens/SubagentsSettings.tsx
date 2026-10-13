import { useId, useLayoutEffect, useRef, useState } from 'react';
import { Button } from '../ui/Button';
import type { SubagentDraft, SubagentIntent, SubagentWorkspace } from './contracts';
import { modelChoices } from './screenExamples';
import { FormField } from './ScreenPrimitives';
import { subagentErrors, subagentLimits } from './subagentWorkspace';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';

const preferenceLabel = (value: SubagentDraft['modelPreference']) => value === 'inherit' ? 'Inherit parent' : modelChoices.find((choice) => choice.id === value)?.label ?? 'Unknown';
type Editor = SubagentIntent;

export function SubagentsSettings({ workspace, active, onSave }: { workspace: SubagentWorkspace; active: boolean; onSave: (intent: SubagentIntent) => boolean }) {
  const [editor, setEditor] = useState<Editor | null>(null);
  const [receipt, setReceipt] = useState('');
  const returnFocus = useRef<HTMLButtonElement | null>(null);
  const focusPending = useRef(false);
  const gate = useViewGuard(JSON.stringify([workspace.version, active, editor]));
  useLayoutEffect(() => {
    if (!editor && focusPending.current) {
      focusPending.current = false;
      if (active && returnFocus.current?.isConnected) returnFocus.current.focus();
    }
  }, [editor, active]);
  function close() { focusPending.current = true; setEditor(null); }
  return <div>
    <div className={styles.resourceHeading}><p className={styles.muted}>Definition metadata · this visit</p><Button disabled={!active || Boolean(editor)} onClick={(event) => {
      if (!active || editor || !gate.valid()) return;
      returnFocus.current = event.currentTarget; setReceipt('');
      setEditor({ version: workspace.version, definitionId: null, revision: null, name: '', description: '', modelPreference: 'inherit' });
    }}>New subagent</Button></div>
    {receipt ? <p className={styles.receipt} role="status">{receipt}</p> : null}
    {editor ? <SubagentEditor key={`${editor.version}:${editor.definitionId}`} initial={editor} workspace={workspace} active={active} onCancel={() => { if (active && gate.valid()) close(); }} onSave={(intent) => {
      if (!active || !gate.valid() || !onSave(intent)) return false;
      setReceipt(`${intent.definitionId === null ? 'Created' : 'Saved'} ${intent.name.trim()}.`); close(); return true;
    }} /> : null}
    <ul className={styles.definitionList} aria-label="Subagent definitions">{workspace.definitions.map((definition) => <li key={definition.id}>
      <div className={styles.resourceHeading}><h3>{definition.name}</h3><span className={styles.tag}>{definition.source === 'user' ? 'User' : definition.source === 'builtin' ? 'Built-in · read-only' : 'Project · read-only'}</span></div>
      <p>{definition.description}</p>
      <div className={styles.resourceHeading}><span className={styles.muted}>Model preference · {preferenceLabel(definition.modelPreference)}</span>{definition.source === 'user' ? <Button disabled={!active || Boolean(editor)} onClick={(event) => {
        if (!active || editor || !gate.valid()) return;
        returnFocus.current = event.currentTarget; setReceipt('');
        setEditor({ version: workspace.version, definitionId: definition.id, revision: definition.revision, name: definition.name, description: definition.description, modelPreference: definition.modelPreference });
      }} aria-label={`Edit ${definition.name}`}>Edit</Button> : null}</div>
    </li>)}</ul>
  </div>;
}

function SubagentEditor({ initial, workspace, active, onSave, onCancel }: { initial: Editor; workspace: SubagentWorkspace; active: boolean; onSave: (intent: SubagentIntent) => boolean; onCancel: () => void }) {
  const [draft, setDraft] = useState<SubagentDraft>(initial);
  const [attempted, setAttempted] = useState(false);
  const [rejected, setRejected] = useState(false);
  const nameRef = useRef<HTMLInputElement>(null);
  const descriptionId = useId();
  const gate = useViewGuard(JSON.stringify([initial, workspace.version, active, draft]));
  const errors = subagentErrors(draft, workspace.definitions, initial.definitionId);
  const current = workspace.version === initial.version && (initial.definitionId === null || workspace.definitions.some((entry) => entry.id === initial.definitionId && entry.revision === initial.revision && entry.source === 'user'));
  useLayoutEffect(() => { nameRef.current?.focus(); }, []);
  function update(patch: Partial<SubagentDraft>) { if (active && current && gate.valid()) setDraft({ ...draft, ...patch }); }
  return <form className={styles.definitionEditor} aria-label={initial.definitionId === null ? 'Create subagent' : 'Edit subagent'} noValidate onSubmit={(event) => {
    event.preventDefault();
    if (!active || !current || !gate.valid()) return;
    setAttempted(true);
    if (Object.values(errors).some(Boolean) || !gate.claim()) return;
    if (!onSave({ ...initial, ...draft })) setRejected(true);
  }}>
    <h3>{initial.definitionId === null ? 'New definition' : `Edit ${initial.name}`}</h3>
    {!current || rejected ? <p role="alert" className={styles.notice}>This definition changed. Cancel and reopen the latest version.</p> : null}
    <fieldset disabled={!active || !current || rejected} className={styles.editorFields}>
      <FormField label="Name" error={attempted ? errors.name : undefined}>{(props) => <input {...props} ref={nameRef} className={styles.input} required maxLength={subagentLimits.name} value={draft.name} onChange={(event) => update({ name: event.currentTarget.value })} />}</FormField>
      <FormField label="Description" error={attempted ? errors.description : undefined}>{(props) => <textarea {...props} className={styles.textarea} required maxLength={subagentLimits.description} value={draft.description} onChange={(event) => update({ description: event.currentTarget.value })} />}</FormField>
      <FormField label="Model preference" error={attempted ? errors.modelPreference : undefined}>{(props) => <select {...props} aria-describedby={descriptionId} className={styles.select} value={draft.modelPreference} onChange={(event) => update({ modelPreference: event.currentTarget.value as SubagentDraft['modelPreference'] })}><option value="inherit">Inherit parent</option>{modelChoices.map((choice) => <option key={choice.id} value={choice.id}>{choice.label}</option>)}</select>}</FormField>
      <p id={descriptionId} className="srOnly">Local preference metadata only, not backend model policy. The backend has no per-definition model field; child runs inherit the parent model. Current chat and new-chat default are unchanged.</p>
    </fieldset>
    <div className={styles.actions}><Button disabled={!active} onClick={() => { if (active && gate.valid()) onCancel(); }}>Cancel edit</Button><Button type="submit" tone="primary" disabled={!active || !current || rejected}>{initial.definitionId === null ? 'Create definition' : 'Save definition'}</Button></div>
  </form>;
}
