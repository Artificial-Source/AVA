import { useState } from 'react';
import type { PermissionView, QuestionAnswer, QuestionView, RunView, ToolView } from '../chat/contracts';
import { validAnswer, validChoices, validIdentity, validPermission, validQuestion } from '../chat/requestValidation';
import { PermissionCard } from '../chat/widgets/PermissionCard';
import { QuestionCard } from '../chat/widgets/QuestionCard';
import { RunNotice } from '../chat/widgets/RunNotice';
import { ToolActivityGroup } from '../chat/widgets/ToolActivityGroup';
import { Button } from '../ui/Button';
import { DialogFrame, type DialogChrome } from '../ui/DialogFrame';
import type { ActionView, BranchPoint, ChatActionIntent, RenameIntent, RequestReceipt, WorkspaceIntent, WorkspaceView } from './contracts';
import { Confirmation, FormField, ListNotice } from './ScreenPrimitives';
import { useDialogTransitionFocus } from './useDialogTransitionFocus';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';

export function validTitle(value: string) { return typeof value === 'string' && value.trim().length > 0 && value.length <= 256; }
export function validRename(view: ActionView, intent: RenameIntent) {
  return validIdentity(view) && view.state === 'ready' && view.id === intent.id && view.version === intent.version && validTitle(intent.title);
}
export function RenameDialog({ view, onRename, ...chrome }: DialogChrome & { view: ActionView; onRename: (intent: RenameIntent) => void }) {
  return <DialogFrame {...chrome} title="Rename chat" description="Change the example title in this review only. The main chat and backend session remain unchanged."><RenameForm key={`${view.id}:${view.version}`} view={view} onRename={onRename} onDismiss={chrome.onDismiss} /></DialogFrame>;
}
function RenameForm({ view, onRename, onDismiss }: { view: ActionView; onRename: (intent: RenameIntent) => void; onDismiss: () => void }) {
  const [title, setTitle] = useState(view.title);
  const [attempted, setAttempted] = useState(false);
  const gate = useViewGuard(JSON.stringify(view));
  const enabled = validIdentity(view) && view.state === 'ready';
  const error = attempted || title.length > 256 ? !title.trim() ? 'Enter a non-empty title.' : title.length > 256 ? 'Use at most 256 characters.' : undefined : undefined;
  return <form onSubmit={(event) => { event.preventDefault(); setAttempted(true); const intent = { id: view.id, version: view.version, title: title.trim() }; if (validTitle(title) && validRename(view, intent) && gate.claim()) onRename(intent); }}>
    <ListNotice state={view.state} />
    <FormField label="Chat title" hint={`${title.length} / 256 characters. Leading and trailing whitespace is removed.`} error={error}>{(props) => <input {...props} className={styles.input} value={title} maxLength={256} autoComplete="off" disabled={!enabled} onChange={(event) => { if (gate.valid() && enabled) setTitle(event.currentTarget.value); }} />}</FormField>
    <div className={styles.actions}><Button onClick={onDismiss}>Cancel</Button><Button type="submit" tone="primary" disabled={!enabled}>Rename example</Button></div>
  </form>;
}
export function ChatActionDialog({ action, view, points, onIntent, ...chrome }: DialogChrome & { action: 'archive' | 'branch'; view: ActionView; points: readonly BranchPoint[]; onIntent: (intent: ChatActionIntent) => void }) {
  const [point, setPoint] = useState(points[0]?.id ?? '');
  const gate = useViewGuard(JSON.stringify([action, view, points]));
  const validPoints = validChoices(points);
  const enabled = validIdentity(view) && view.state === 'ready' && (action === 'archive' || validPoints && points.some((entry) => entry.id === point));
  return <DialogFrame {...chrome} title={action === 'archive' ? 'Archive intent preview' : 'Branch from a message'} description="Local intent only. The original chat is preserved; no session mutation is performed.">
    <ListNotice state={view.state} />
    <Confirmation label={action === 'archive' ? 'Record archive intent' : 'Record branch intent'} disabled={!enabled} onCancel={chrome.onDismiss} onConfirm={() => {
      if (enabled && gate.claim()) onIntent({ id: view.id, version: view.version, action, ...(action === 'branch' ? { pointId: point } : {}) });
    }}>
      <h2 className={styles.taskTitle}>{action === 'archive' ? 'Archive chat?' : 'Branch from a message'}</h2>
      <p className={styles.lead}>{view.title}</p>
      {action === 'archive' ? <p className={styles.muted}>Archive is not wired in RPC v1. This preview does not delete, hide or archive the original chat. No delete-session action is offered.</p> : <><p className={styles.muted}>Choose a prior user message as an example branch point. This is a conversation branch, not a Git branch or worktree.</p><fieldset className={styles.group}><legend>Branch point</legend>{validPoints ? points.map((entry) => <label key={entry.id} className={styles.choice}><input type="radio" name="branch-point" value={entry.id} checked={point === entry.id} disabled={view.state !== 'ready'} onChange={() => { if (gate.valid() && points.some((value) => value.id === entry.id)) setPoint(entry.id); }} /><span>{entry.label}</span></label>) : <p role="alert">Invalid branch points. No choice can be submitted.</p>}</fieldset></>}
    </Confirmation>
  </DialogFrame>;
}

export function validWorkspaceIntent(view: WorkspaceView, intent: WorkspaceIntent) {
  if (!validIdentity(view) || !validChoices(view.choices) || intent.id !== view.id || intent.version !== view.version) return false;
  if (intent.action === 'choose') return view.state === 'choice' && view.choices.some((choice) => choice.id === intent.workspaceId);
  const selected = view.choices.some((choice) => choice.id === view.selectedId);
  if (intent.action === 'retry') return ['offline', 'error', 'canceled'].includes(view.state) && selected;
  return view.state === 'opening' && (intent.action === 'cancel' || selected && ['show-ready', 'show-error'].includes(intent.action));
}
export function WorkspaceDialog({ view, onIntent, ...chrome }: DialogChrome & { view: WorkspaceView; onIntent: (intent: WorkspaceIntent) => void }) {
  const gate = useViewGuard(JSON.stringify(view));
  const focus = useDialogTransitionFocus(chrome.onDismiss);
  const valid = validIdentity(view) && validChoices(view.choices);
  const selected = valid ? view.choices.find((choice) => choice.id === view.selectedId) : undefined;
  function emit(action: WorkspaceIntent['action'], workspaceId?: string) {
    const intent = { id: view.id, version: view.version, action, workspaceId };
    if (validWorkspaceIntent(view, intent) && gate.claim()) { focus.capture(); onIntent(intent); }
  }
  return <DialogFrame {...chrome} onDismiss={focus.dismiss} title="Workspace & recovery" description="Host-owned workspace choices represented by local fixtures. No directory picker, path access or process restart.">
    <h2 className={styles.taskTitle}>Workspace</h2><p className={styles.muted}>Supplied choices only. No directory access or process restart.</p>
    <div {...focus.content}>{view.state === 'choice' && valid ? <ul className={styles.list}>{view.choices.map((choice) => <li key={choice.id}><button className={styles.result} onClick={() => emit('choose', choice.id)}><span><strong>{choice.label}</strong><small>{choice.detail}</small></span><span aria-hidden="true">→</span></button></li>)}</ul> : <section><h3>{view.state === 'opening' ? 'Opening · example' : view.state === 'ready' ? 'Workspace ready · example' : view.state === 'offline' ? 'Host offline · example' : view.state === 'error' ? 'Workspace error · example' : view.state === 'canceled' ? 'Workspace choice canceled' : 'Workspace unavailable'}</h3><p className={styles.muted}>{selected?.label ?? 'No known workspace selected'}. These are supplied status fixtures, not observations of a running host.</p></section>}
    {view.state === 'opening' ? <div className={styles.inline}><Button disabled={!selected} onClick={() => emit('show-ready')}>Show ready example</Button><Button disabled={!selected} onClick={() => emit('show-error')}>Show error example</Button><Button onClick={() => emit('cancel')}>Cancel example opening</Button></div> : null}
    {['offline', 'error', 'canceled'].includes(view.state) ? <Button disabled={!selected} onClick={() => emit('retry')}>Retry example</Button> : null}
    <div className={styles.preference}><div><h3>Choose another folder</h3><p>Unavailable: trusted native directory selection belongs to the host.</p></div><Button disabled>Folder picker unavailable</Button></div>
    <div className={styles.actions}><Button onClick={focus.dismiss}>Cancel</Button></div></div>
  </DialogFrame>;
}

export type MessageProjection = Readonly<{ role: 'user' | 'assistant' | 'unknown'; state: string; text: string | null }>;
export function MessageDialog({ view, ...chrome }: DialogChrome & { view: MessageProjection }) {
  return <DialogFrame {...chrome} title="Message projection" description="Read-only invented message text. Not a sent provider payload, system prompt or private reasoning."><dl className={styles.facts}><dt>Role</dt><dd>{view.role}</dd><dt>Supplied state</dt><dd>{view.state}</dd><dt>Source</dt><dd>Local example projection</dd></dl>{view.text !== null && view.role !== 'unknown' ? <pre className={styles.projection}>{view.text}</pre> : <p className={styles.notice}>No supported message text supplied.</p>}<div className={styles.actions}><Button onClick={chrome.onDismiss}>Close detail</Button></div></DialogFrame>;
}
export function ToolDialog({ records, ...chrome }: DialogChrome & { records: readonly ToolView[] }) {
  const [open, setOpen] = useState(true);
  return <DialogFrame {...chrome} title="Tool output detail" description="Supplied, escaped output only. No tools ran, files changed or output was fetched. No private provider payload or reasoning is available."><ToolActivityGroup records={records} open={open} onOpenChange={setOpen} /><div className={styles.actions}><Button onClick={chrome.onDismiss}>Close detail</Button></div></DialogFrame>;
}

export function PermissionDialog({ request, onIntent, ...chrome }: DialogChrome & { request: PermissionView; onIntent: (intent: RequestReceipt) => void }) {
  const gate = useViewGuard(JSON.stringify(request));
  const focus = useDialogTransitionFocus(chrome.onDismiss);
  return <DialogFrame {...chrome} onDismiss={focus.dismiss} title="Permission" description="Dismissal leaves the request unresolved. Only explicit advertised choices emit local intent."><div {...focus.content}><p className={styles.muted}>Dismissal leaves this request unresolved. Only the explicit choices below record local intent.</p><PermissionCard request={request} onDecide={(intent) => {
    if (validPermission(request) && request.state === 'ready' && intent.id === request.id && intent.version === request.version && request.decisions.includes(intent.decision) && gate.claim()) { focus.capture(); onIntent({ ...intent, kind: 'permission' }); }
  }} /><div className={styles.actions}><Button onClick={focus.dismiss}>Dismiss unresolved</Button></div></div></DialogFrame>;
}
export function QuestionDialog({ request, onIntent, ...chrome }: DialogChrome & { request: QuestionView; onIntent: (intent: RequestReceipt) => void }) {
  const focus = useDialogTransitionFocus(chrome.onDismiss);
  return <DialogFrame {...chrome} onDismiss={focus.dismiss} title="Question" description="Answers are ephemeral. Neither custom text nor secret values are included in review receipts."><div {...focus.content}><p className={styles.muted}>Answers are ephemeral. Custom and secret values are excluded from receipts.</p><QuestionBody key={JSON.stringify([request.id, request.version, request.state, request.custom])} request={request} onIntent={(intent) => { focus.capture(); onIntent(intent); }} /><div className={styles.actions}><Button onClick={focus.dismiss}>Dismiss unresolved</Button></div></div></DialogFrame>;
}
function QuestionBody({ request, onIntent }: { request: QuestionView; onIntent: (intent: RequestReceipt) => void }) {
  const [value, setValue] = useState<QuestionAnswer>({ selectedIds: [], customText: '' });
  const gate = useViewGuard(JSON.stringify(request));
  return <QuestionCard request={request} value={value} onChange={(next) => { if (gate.valid() && request.state === 'ready' && validQuestion(request)) setValue(next); }} onAnswer={(intent) => {
    if (!validAnswer(request, intent.answer) || request.state !== 'ready' || intent.id !== request.id || intent.version !== request.version || !gate.claim()) return;
    setValue({ selectedIds: [], customText: '' });
    onIntent({ id: request.id, version: request.version, kind: 'question-answer' });
  }} onCancel={(identity) => {
    if (!validQuestion(request) || request.state !== 'ready' || identity.id !== request.id || identity.version !== request.version || !gate.claim()) return;
    setValue({ selectedIds: [], customText: '' });
    onIntent({ ...identity, kind: 'question-cancel' });
  }} />;
}
export function RunDialog({ view, onRetry, ...chrome }: DialogChrome & { view: RunView; onRetry: () => void }) {
  const gate = useViewGuard(JSON.stringify(view));
  const focus = useDialogTransitionFocus(chrome.onDismiss);
  const [submitted, setSubmitted] = useState(false);
  return <DialogFrame {...chrome} onDismiss={focus.dismiss} title="Run status" description="Supplied run state. Retry intent does not schedule execution; a cancellation request is not a canceled result."><div {...focus.content}><RunNotice view={view} onRetry={view.state === 'error' && !submitted ? () => { if (gate.claim()) { focus.capture(); setSubmitted(true); onRetry(); } } : undefined} />{submitted ? <p role="status" className={styles.muted}>Retry intent recorded locally. Awaiting a supplied state; no retry was executed.</p> : null}<div className={styles.actions}><Button onClick={focus.dismiss}>Close notice</Button></div></div></DialogFrame>;
}
