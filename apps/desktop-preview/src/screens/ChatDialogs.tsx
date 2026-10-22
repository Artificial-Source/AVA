import { useId, useRef, useState, type KeyboardEvent } from 'react';
import { CaretRightIcon } from '@phosphor-icons/react/dist/csr/CaretRight';
import { EyeSlashIcon } from '@phosphor-icons/react/dist/csr/EyeSlash';
import { FolderSimpleIcon } from '@phosphor-icons/react/dist/csr/FolderSimple';
import { MagnifyingGlassIcon } from '@phosphor-icons/react/dist/csr/MagnifyingGlass';
import type { PermissionView, QuestionAnswer, QuestionView, RunView, ToolView } from '../chat/contracts';
import { validAnswer, validChoices, validIdentity, validPermission, validQuestion } from '../chat/requestValidation';
import { PermissionCard } from '../chat/widgets/PermissionCard';
import { QuestionCard } from '../chat/widgets/QuestionCard';
import { RunNotice } from '../chat/widgets/RunNotice';
import { ToolActivityGroup } from '../chat/widgets/ToolActivityGroup';
import { ChatStatus } from '../components/ChatStatus';
import { Button } from '../ui/Button';
import { canFocus, DialogFrame, type DialogChrome } from '../ui/DialogFrame';
import type { StatusValue } from '../ui/Status';
import type { ActionView, BranchPoint, ChatActionIntent, ListState, RenameIntent, RequestReceipt, WorkspaceIntent, WorkspaceState, WorkspaceView } from './contracts';
import { FormField, ListNotice } from './ScreenPrimitives';
import { useDialogTransitionFocus } from './useDialogTransitionFocus';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';
import css from './ChatDialogs.module.css';

type Notices = Partial<Record<ListState, string>>;

const titleLimit = 256;
// The counter appears only when it can change what the user types next.
const counterFrom = titleLimit - 40;
export function validTitle(value: string) { return typeof value === 'string' && value.trim().length > 0 && value.length <= titleLimit; }
export function validRename(view: ActionView, intent: RenameIntent) {
  return validIdentity(view) && view.state === 'ready' && view.id === intent.id && view.version === intent.version && validTitle(intent.title);
}
const renameNotices: Notices = {
  loading: 'Loading the chat title…',
  busy: 'You can rename this chat after its run finishes or is stopped.',
  stale: 'This chat changed after you opened Rename. Close and reopen it to edit the latest title.',
  error: 'Couldn’t load this chat’s title. Close and try again.',
  unavailable: 'This chat can’t be renamed.',
};
// Top-anchored, so a validation message grows the dialog downward instead of re-centring it.
export function RenameDialog({ view, onRename, ...chrome }: DialogChrome & { view: ActionView; onRename: (intent: RenameIntent) => void }) {
  return <DialogFrame {...chrome} anchor="top" title="Rename chat"><RenameForm key={`${view.id}:${view.version}`} view={view} onRename={onRename} onDismiss={chrome.onDismiss} /></DialogFrame>;
}
function RenameForm({ view, onRename, onDismiss }: { view: ActionView; onRename: (intent: RenameIntent) => void; onDismiss: () => void }) {
  const [title, setTitle] = useState(view.title);
  const [attempted, setAttempted] = useState(false);
  const selected = useRef(false);
  const gate = useViewGuard(JSON.stringify(view));
  const enabled = validIdentity(view) && view.state === 'ready';
  const over = title.length - titleLimit;
  const error = over > 0 ? `Titles can be up to ${titleLimit} characters. Remove ${over} ${over === 1 ? 'character' : 'characters'}.`
    : attempted && !title.trim() ? 'Enter a title for this chat.' : undefined;
  const hint = !error && title.length >= counterFrom ? `${-over} ${over === -1 ? 'character' : 'characters'} left` : undefined;
  // The first focus selects the whole title, so typing replaces it.
  return <form noValidate onSubmit={(event) => { event.preventDefault(); setAttempted(true); const intent = { id: view.id, version: view.version, title: title.trim() }; if (validTitle(title) && validRename(view, intent) && gate.claim()) onRename(intent); }}>
    <ListNotice state={view.state} message={renameNotices[view.state]} />
    <FormField label="Chat title" hint={hint} error={error}>{(props) => <input {...props} className={styles.input} value={title} maxLength={titleLimit} autoComplete="off" disabled={!enabled}
      onFocus={(event) => { if (!selected.current) { selected.current = true; event.currentTarget.select(); } }}
      onChange={(event) => { if (gate.valid() && enabled) setTitle(event.currentTarget.value); }} />}</FormField>
    <div className={styles.actions}><Button onClick={onDismiss}>Cancel</Button><Button type="submit" tone="primary" disabled={!enabled}>Rename</Button></div>
  </form>;
}

type ChatActionProps = DialogChrome & { view: ActionView; onIntent: (intent: ChatActionIntent) => void };
// Archive only: `onFindArchived` opens Find a chat on Archived (without it the guidance stays plain
// text); `findShortcut` is Find a chat's key label while one is assigned.
type ArchiveRoutes = { onGoToChat?: () => void; onFindArchived?: () => void; findShortcut?: string };
export function ChatActionDialog({ action, points, onGoToChat, onFindArchived, findShortcut, ...props }: ChatActionProps & ArchiveRoutes & { action: 'archive' | 'branch'; points: readonly BranchPoint[] }) {
  return action === 'archive' ? <ArchiveDialog {...props} onGoToChat={onGoToChat} onFindArchived={onFindArchived} findShortcut={findShortcut} /> : <BranchDialog {...props} points={points} />;
}

const archiveNotices: Notices = {
  busy: 'This chat is still running. Stop the run first, then archive it.',
  stale: 'This chat changed after you opened this. Close and try again.',
  error: 'Couldn’t load this chat. Close and try again.',
  unavailable: 'This chat can’t be archived.',
};
function ArchiveDialog({ view, onIntent, onGoToChat, onFindArchived, findShortcut, ...chrome }: ChatActionProps & ArchiveRoutes) {
  const gate = useViewGuard(JSON.stringify(['archive', view]));
  const confirm = useRef<HTMLButtonElement>(null);
  const goToChat = useRef<HTMLButtonElement>(null);
  const enabled = validIdentity(view) && view.state === 'ready';
  const busy = view.state === 'busy' && Boolean(onGoToChat);
  // Archive is reversible, so the confirm takes focus; a running chat offers the way to Stop instead.
  return <DialogFrame {...chrome} title="Archive chat?" subtitle={view.title.trim() || undefined} initialFocus={busy ? goToChat : confirm}>
    <form onSubmit={(event) => { event.preventDefault(); if (enabled && gate.claim()) onIntent({ id: view.id, version: view.version, action: 'archive' }); }}>
      <ListNotice state={view.state} message={archiveNotices[view.state]} />
      {['ready', 'busy', 'stale'].includes(view.state) ? <ul className={css.consequences}>
        <li><EyeSlashIcon size={16} aria-hidden="true" /><span>Removed from Projects, Pinned and Recent.</span></li>
        <li><MagnifyingGlassIcon size={16} aria-hidden="true" /><span>Nothing is deleted. Find it in {onFindArchived
          ? <button type="button" className={styles.inlineLink} aria-label="Find a chat, Archived" onClick={() => { if (onFindArchived && gate.claim()) onFindArchived(); }}>Find a chat</button>
          : <strong>Find a chat</strong>}{findShortcut ? <> <kbd className={css.key}>{findShortcut}</kbd></> : null} → <strong>Archived</strong>.</span></li>
      </ul> : null}
      <div className={styles.actions}>
        <Button onClick={chrome.onDismiss}>Cancel</Button>
        {busy ? <Button ref={goToChat} onClick={() => { if (onGoToChat && gate.claim()) onGoToChat(); }}>Go to chat</Button> : null}
        <Button ref={confirm} type="submit" tone="primary" disabled={!enabled}>Archive chat</Button>
      </div>
    </form>
  </DialogFrame>;
}

const branchNotices: Notices = {
  empty: 'This chat has no messages to branch from yet.',
  busy: 'You can branch this chat after its run finishes or is stopped.',
  stale: 'This chat changed after you opened this. Close and try again.',
  error: 'Couldn’t load this chat’s messages. Close and try again.',
  unavailable: 'This chat can’t be branched.',
};
// Points arrive oldest first. The newest is the likeliest branch, so it is listed first and preselected.
// Rows name their message in one line; the picked row opens the whole message (bounded, scrolling on
// its own) above what the branch keeps. A radio's name stays that one line; the keep note describes it.
function BranchDialog({ view, points, onIntent, ...chrome }: ChatActionProps & { points: readonly BranchPoint[] }) {
  const id = useId();
  const validPoints = validChoices(points);
  const total = validPoints ? points.length : 0;
  const [point, setPoint] = useState(() => validPoints ? points.at(-1)?.id ?? '' : '');
  const gate = useViewGuard(JSON.stringify(['branch', view, points]));
  const ready = validIdentity(view) && view.state === 'ready';
  const enabled = ready && validPoints && points.some((entry) => entry.id === point);
  const state: ListState = view.state === 'ready' && validPoints && total === 0 ? 'empty' : view.state;
  const listed = validPoints && total > 0 && ['ready', 'busy', 'stale', 'disabled'].includes(view.state);
  // Enter on a radio must not submit; only the Create branch button does.
  return <DialogFrame {...chrome} title="Branch from a message" subtitle={view.title.trim() || undefined}>
    <form onSubmit={(event) => { event.preventDefault(); if (enabled && gate.claim()) onIntent({ id: view.id, version: view.version, action: 'branch', pointId: point }); }}>
      {!validPoints ? <ListNotice state="error" message="These messages can’t be shown, so a branch can’t be created." /> : <ListNotice state={state} message={branchNotices[state]} />}
      {listed ? <>
        <p className={css.lead}>Starts a new chat from the message you pick. The original chat stays as it is.</p>
        <fieldset className={css.points} disabled={!ready} onKeyDown={(event) => { if (event.key === 'Enter' && event.target instanceof HTMLInputElement && event.target.type === 'radio') event.preventDefault(); }}>
          <legend>Your messages</legend>
          {points.map((entry, index) => ({ entry, position: index + 1, later: total - index - 1 })).reverse().map(({ entry, position, later }) => {
            const picked = point === entry.id;
            // Only a message the one-line label shortened has more to show.
            const whole = picked && typeof entry.text === 'string' ? entry.text.trim() : '';
            const full = whole && whole.replace(/\s+/gu, ' ') !== entry.label.trim() ? whole : null;
            const row = `${id}-point-${position}`;
            return <label key={entry.id} className={css.point}>
              <input type="radio" name="branch-point" value={entry.id} checked={picked} aria-labelledby={`${row}-meta ${row}-label`} aria-describedby={`${row}-keeps`}
                onChange={() => { if (gate.valid() && points.some((value) => value.id === entry.id)) setPoint(entry.id); }} />
              <span className={css.pointBody}>
                <span id={`${row}-meta`} className={css.pointMeta}>Message {position} of {total}</span>
                <span id={`${row}-label`} className={full ? 'srOnly' : css.pointText}>{entry.label}</span>
                {full ? <span className={css.pointFull} role="region" aria-label={`Message ${position} in full`} tabIndex={0}>{full}</span> : null}
                <span id={`${row}-keeps`} className={css.pointKeeps}>{later === 0 ? 'Keeps the whole chat up to this message.' : `Keeps the chat up to this message. ${later === 1 ? 'The 1 later message stays' : `The ${later} later messages stay`} in the original.`}</span>
              </span>
            </label>;
          })}
        </fieldset>
      </> : null}
      <div className={styles.actions}><Button onClick={chrome.onDismiss}>Cancel</Button><Button type="submit" tone="primary" disabled={!enabled}>Create branch</Button></div>
    </form>
  </DialogFrame>;
}

export function validWorkspaceIntent(view: WorkspaceView, intent: WorkspaceIntent) {
  if (!validIdentity(view) || !validChoices(view.choices) || intent.id !== view.id || intent.version !== view.version) return false;
  const selected = view.choices.some((choice) => choice.id === view.selectedId);
  if (intent.action === 'choose') return view.state === 'choice' && view.choices.some((choice) => choice.id === intent.workspaceId);
  if (intent.action === 'use') return view.state === 'ready' && selected;
  if (intent.action === 'retry') return ['offline', 'error', 'canceled'].includes(view.state) && selected;
  if (intent.action === 'change') return ['ready', 'offline', 'error', 'canceled', 'unavailable'].includes(view.state);
  return intent.action === 'cancel' && view.state === 'opening';
}
// User-facing copy says “project”, the sidebar's word for the same thing.
const workspaceStates: Readonly<Record<Exclude<WorkspaceState, 'choice'>, { status: StatusValue; label: string; cause?: string }>> = {
  opening: { status: 'working', label: 'Opening' },
  ready: { status: 'completed', label: 'Ready' },
  offline: { status: 'unavailable', label: 'Offline', cause: 'Can’t reach this project. Check your connection, then retry.' },
  error: { status: 'failed', label: 'Couldn’t open', cause: 'Something went wrong while opening it. Retry, or choose another project.' },
  unavailable: { status: 'unavailable', label: 'Unavailable', cause: 'This project can’t be opened anymore. Choose another one.' },
  canceled: { status: 'stopped', label: 'Canceled', cause: 'Opening was canceled. Nothing changed.' },
};
// Arrow keys, Home and End move between rows as in Find a chat; Tab still reaches every row.
function moveBetweenRows(event: KeyboardEvent<HTMLUListElement>) {
  if (event.nativeEvent.isComposing || event.keyCode === 229 || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
  const rows = Array.from(event.currentTarget.querySelectorAll<HTMLButtonElement>('button')).filter((row) => canFocus(row));
  const index = rows.findIndex((row) => row === event.target);
  if (index < 0) return;
  const last = rows.length - 1;
  const next = event.key === 'ArrowDown' ? (index >= last ? 0 : index + 1) : event.key === 'ArrowUp' ? (index <= 0 ? last : index - 1)
    : event.key === 'Home' ? 0 : event.key === 'End' ? last : null;
  if (next === null) return;
  event.preventDefault();
  rows[next].focus();
}
export function WorkspaceDialog({ view, onIntent, ...chrome }: DialogChrome & { view: WorkspaceView; onIntent: (intent: WorkspaceIntent) => void }) {
  const gate = useViewGuard(JSON.stringify(view));
  const focus = useDialogTransitionFocus(chrome.onDismiss);
  const valid = validIdentity(view) && validChoices(view.choices);
  const selected = valid ? view.choices.find((choice) => choice.id === view.selectedId) : undefined;
  const known = Object.hasOwn(workspaceStates, view.state) ? workspaceStates[view.state as keyof typeof workspaceStates] : null;
  function emit(action: WorkspaceIntent['action'], workspaceId?: string) {
    const intent = { id: view.id, version: view.version, action, workspaceId };
    if (validWorkspaceIntent(view, intent) && gate.claim()) { focus.capture(); onIntent(intent); }
  }
  const change = <Button onClick={() => emit('change')}>Choose another project</Button>;
  // Top-anchored: moving between the list and a project's state never re-centres the dialog.
  // The list opens on the current project, so Enter keeps it and arrows reach the others.
  return <DialogFrame {...chrome} anchor="top" onDismiss={focus.dismiss} title="Open a project">
    <div {...focus.content}>
      {!valid ? <ListNotice state="error" message="These projects can’t be shown." />
        : view.state === 'choice' ? view.choices.length ? <ul className={css.workspaces} aria-label="Projects" onKeyDown={moveBetweenRows}>{view.choices.map((choice) => <li key={choice.id}>
          <button type="button" className={css.workspaceChoice} aria-current={choice.id === view.selectedId ? 'true' : undefined} data-autofocus={choice.id === view.selectedId || undefined} onClick={() => emit('choose', choice.id)}>
            <FolderSimpleIcon size={18} aria-hidden="true" />
            <span className={css.workspaceText}><strong>{choice.label}</strong><small>{choice.detail}</small></span>
            {choice.id === view.selectedId ? <span className={css.current}>Current</span> : null}
            <CaretRightIcon size={14} aria-hidden="true" />
          </button>
        </li>)}</ul> : <ListNotice state="empty" message="No projects yet." />
          : known ? <div role={view.state === 'error' ? 'alert' : 'status'}>
            <div className={css.workspaceState}>
              <FolderSimpleIcon size={18} aria-hidden="true" />
              <span className={css.workspaceText}><strong>{selected?.label ?? 'No project selected'}</strong>{selected?.detail ? <small>{selected.detail}</small> : null}</span>
              <ChatStatus status={known.status} label={known.label} />
            </div>
            {known.cause ? <p className={css.cause}>{known.cause}</p> : null}
          </div> : <ListNotice state="error" message="This project’s state can’t be shown." />}
      <div className={styles.actions}>
        {!valid || view.state === 'choice' || !known ? <Button onClick={focus.dismiss}>Cancel</Button> : null}
        {valid && view.state === 'opening' ? <Button onClick={() => emit('cancel')}>Cancel opening</Button> : null}
        {valid && view.state === 'ready' ? <>{change}<Button tone="primary" disabled={!selected} onClick={() => emit('use')}>Use {selected?.label ?? 'project'}</Button></> : null}
        {valid && (view.state === 'offline' || view.state === 'error') ? <>{change}<Button tone="primary" disabled={!selected} onClick={() => emit('retry')}>Retry</Button></> : null}
        {valid && view.state === 'canceled' ? <>{change}<Button tone="primary" disabled={!selected} onClick={() => emit('retry')}>Open {selected?.label ?? 'project'}</Button></> : null}
        {valid && view.state === 'unavailable' ? <Button tone="primary" onClick={() => emit('change')}>Choose another project</Button> : null}
      </div>
    </div>
  </DialogFrame>;
}

export type MessageProjection = Readonly<{ role: 'user' | 'assistant' | 'unknown'; state: string; text: string | null }>;
const senders: Readonly<Record<MessageProjection['role'], string>> = { user: 'Your message', assistant: 'Reply from AVA', unknown: 'Unknown sender' };
export function MessageDialog({ view, ...chrome }: DialogChrome & { view: MessageProjection }) {
  const role = Object.hasOwn(senders, view.role) ? view.role : 'unknown';
  const readable = typeof view.text === 'string' && view.text.length > 0 && role !== 'unknown';
  const outcome = view.state === 'failed' ? { status: 'failed' as const, label: 'Failed', note: 'This reply didn’t finish. Only the text that arrived is shown.' }
    : view.state === 'canceled' ? { status: 'stopped' as const, label: 'Stopped', note: 'This reply was stopped. Only the text that arrived is shown.' } : null;
  return <DialogFrame {...chrome} title="Message details" subtitle={senders[role]}>
    {outcome ? <p className={css.messageState}><ChatStatus status={outcome.status} label={outcome.label} /><span>{outcome.note}</span></p> : null}
    {readable ? <p className={css.messageText}>{view.text}</p>
      : <ListNotice state={role === 'unknown' ? 'unavailable' : 'empty'} message={role === 'unknown' ? 'This message can’t be shown.' : 'This message has no text.'} />}
    <div className={styles.actions}><Button onClick={chrome.onDismiss}>Done</Button></div>
  </DialogFrame>;
}
export function ToolDialog({ records, ...chrome }: DialogChrome & { records: readonly ToolView[] }) {
  const [open, setOpen] = useState(true);
  return <DialogFrame {...chrome} title="Tool details">
    <ToolActivityGroup records={records} open={open} onOpenChange={setOpen} />
    <div className={styles.actions}><Button onClick={chrome.onDismiss}>Done</Button></div>
  </DialogFrame>;
}

// Request dialogs: the request's own title is the visible dialog title and the card
// drops its heading. Initial focus lands on the decision: Deny (the card marks it
// data-autofocus) for permissions, the first answer for questions — never Close.
export function PermissionDialog({ request, onIntent, ...chrome }: DialogChrome & { request: PermissionView; onIntent: (intent: RequestReceipt) => void }) {
  const gate = useViewGuard(JSON.stringify(request));
  const focus = useDialogTransitionFocus(chrome.onDismiss);
  return <DialogFrame {...chrome} onDismiss={focus.dismiss} title={typeof request.title === 'string' && request.title.trim() ? request.title : 'Permission request'} description="Closing leaves this request waiting for your decision.">
    <div {...focus.content}>
      <PermissionCard titled={false} request={request} onDecide={(intent) => {
        if (validPermission(request) && request.state === 'ready' && intent.id === request.id && intent.version === request.version && request.decisions.includes(intent.decision) && gate.claim()) { focus.capture(); onIntent({ ...intent, kind: 'permission' }); }
      }} />
      <div className={styles.actions}><Button onClick={focus.dismiss}>Decide later</Button></div>
    </div>
  </DialogFrame>;
}
// `onRetry` reloads a question that failed to load; without it the error state offers no Retry.
export function QuestionDialog({ request, onIntent, onRetry, ...chrome }: DialogChrome & { request: QuestionView; onIntent: (intent: RequestReceipt) => void; onRetry?: () => void }) {
  const focus = useDialogTransitionFocus(chrome.onDismiss);
  return <DialogFrame {...chrome} onDismiss={focus.dismiss} title={typeof request.title === 'string' && request.title.trim() ? request.title : 'Question'} description="Closing leaves this question unanswered.">
    <div {...focus.content}>
      <QuestionBody key={JSON.stringify([request.id, request.version, request.state, request.custom])} request={request} onIntent={(intent) => { focus.capture(); onIntent(intent); }}
        onRetry={onRetry ? () => { focus.capture(); onRetry(); } : undefined} />
      <div className={styles.actions}><Button onClick={focus.dismiss}>Answer later</Button></div>
    </div>
  </DialogFrame>;
}
function QuestionBody({ request, onIntent, onRetry }: { request: QuestionView; onIntent: (intent: RequestReceipt) => void; onRetry?: () => void }) {
  const [value, setValue] = useState<QuestionAnswer>({ selectedIds: [], customText: '' });
  const gate = useViewGuard(JSON.stringify(request));
  return <QuestionCard titled={false} request={request} value={value} onChange={(next) => { if (gate.valid() && request.state === 'ready' && validQuestion(request)) setValue(next); }} onAnswer={(intent) => {
    if (!validAnswer(request, intent.answer) || request.state !== 'ready' || intent.id !== request.id || intent.version !== request.version || !gate.claim()) return;
    setValue({ selectedIds: [], customText: '' });
    onIntent({ id: request.id, version: request.version, kind: 'question-answer' });
  }} onCancel={(identity) => {
    if (!validQuestion(request) || request.state !== 'ready' || identity.id !== request.id || identity.version !== request.version || !gate.claim()) return;
    setValue({ selectedIds: [], customText: '' });
    onIntent({ ...identity, kind: 'question-cancel' });
  }} onRetry={onRetry ? (identity) => {
    if (request.state !== 'error' || identity.id !== request.id || identity.version !== request.version || !gate.claim()) return;
    onRetry();
  } : undefined} />;
}
export function RunDialog({ view, onRetry, ...chrome }: DialogChrome & { view: RunView; onRetry: () => void }) {
  const gate = useViewGuard(JSON.stringify(view));
  const focus = useDialogTransitionFocus(chrome.onDismiss);
  const [submitted, setSubmitted] = useState(false);
  return <DialogFrame {...chrome} onDismiss={focus.dismiss} title="Run status">
    <div {...focus.content}>
      <RunNotice view={view} onRetry={view.state === 'error' && !submitted ? () => { if (gate.claim()) { focus.capture(); setSubmitted(true); onRetry(); } } : undefined} />
      {submitted ? <p role="status" className={css.receipt}>Retry requested.</p> : null}
      <div className={styles.actions}><Button onClick={focus.dismiss}>Done</Button></div>
    </div>
  </DialogFrame>;
}
