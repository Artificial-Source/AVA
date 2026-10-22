import { useId, useRef, useState, type ReactNode, type FormEvent, type KeyboardEvent } from 'react';
import { ArrowUpIcon } from '@phosphor-icons/react/dist/csr/ArrowUp';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import { ArrowsInSimpleIcon } from '@phosphor-icons/react/dist/csr/ArrowsInSimple';
import { ArrowsOutSimpleIcon } from '@phosphor-icons/react/dist/csr/ArrowsOutSimple';
import { CheckIcon } from '@phosphor-icons/react/dist/csr/Check';
import { PlusIcon } from '@phosphor-icons/react/dist/csr/Plus';
import { PlugsIcon } from '@phosphor-icons/react/dist/csr/Plugs';
import { XIcon } from '@phosphor-icons/react/dist/csr/X';
import { StopIcon } from '@phosphor-icons/react/dist/csr/Stop';
import { IconButton } from '../../ui/Button';
import type { ComposerView } from '../contracts';
import { validChoices } from '../requestValidation';
import { ContextMeter, type ContextTarget } from './ContextMeter';
import styles from '../../components/Composer.module.css';
import modelButtonStyles from './ModelChoiceButton.module.css';

interface ComposerCopy {
  context: string;
  /** Optional helper line under the picker heading; empty hides it. */
  contextDescription: string;
  addContext: string;
  attachedContext: string;
  contextTitle: string;
  modelTitle: string;
  sendTitle: string;
  stop: string;
  /** Tooltip for Stop; defaults to the Stop label. */
  stopTitle: string;
}

interface ChatComposerProps {
  view: ComposerView;
  idPrefix?: string;
  modelMark?: ReactNode;
  copy?: Partial<ComposerCopy>;
  /** Short explanation per mode id, exposed as the mode control's description. */
  modeDescriptions?: Readonly<Record<string, string>>;
  /** aria-keyshortcuts values for app-level chords that reach these controls. */
  keyShortcuts?: Readonly<{ model?: string; stop?: string }>;
  /** Set while the selected model can't run (its provider isn't connected). The model control
   * shows `status` and is described by `detail`; Send stays reachable but unavailable, with
   * `sendReason` as its tooltip and accessible description. While Send is blocked, `message`
   * shows in the composer beside one way forward: choosing a model. */
  modelUnavailable?: Readonly<{ status: string; detail: string; sendReason: string; message: string }>;
  onDraftChange: (draft: string) => void;
  onModelChange: (id: string) => void;
  onOpenModel?: () => void;
  onModeChange: (id: string) => void;
  onExpandedChange: (expanded: boolean) => void;
  onAddReference: (id: string) => void;
  onRemoveReference: (id: string) => void;
  onSend?: () => void;
  onStop?: () => void;
  onOpenContext?: () => void;
  contextTarget?: ContextTarget;
  /** The chat has no messages yet: unmeasured context reads as no usage rather than unknown. */
  emptyChat?: boolean;
  enterBehavior?: 'send' | 'newline';
}

type ActionMode = 'send' | 'stop';

const defaultCopy: ComposerCopy = {
  context: 'Context references', contextDescription: '',
  addContext: 'Add context', attachedContext: 'Attached context', contextTitle: 'Add context',
  modelTitle: 'Choose model', sendTitle: 'Send message · Ctrl/Cmd+Enter', stop: 'Request stop', stopTitle: '',
};

export function ChatComposer({ view, idPrefix, modelMark, copy: suppliedCopy, modeDescriptions, keyShortcuts, modelUnavailable, onDraftChange, onModelChange, onOpenModel, onModeChange, onExpandedChange, onAddReference, onRemoveReference, onSend, onStop, onOpenContext, contextTarget, emptyChat = false, enterBehavior = 'newline' }: ChatComposerProps) {
  const generatedId = useId();
  const prefix = idPrefix ?? generatedId;
  const id = (name: string) => prefix ? `${prefix}-${name}` : name;
  const copy = { ...defaultCopy, ...suppliedCopy };
  const contextReferences = validChoices(view.contextOptions) ? view.contextOptions : [];
  const contextIds = [...new Set(view.selectedContextIds)].filter((id) => contextReferences.some((entry) => entry.id === id));
  const modelOptions = validChoices(view.modelOptions) ? view.modelOptions : [];
  const modeOptions = validChoices(view.modeOptions) ? view.modeOptions : [];
  const validModel = modelOptions.some((option) => option.id === view.modelId);
  const validMode = modeOptions.some((option) => option.id === view.modeId);
  const working = view.runState === 'running';
  const canSend = !working && Boolean(onSend) && Boolean(view.draft.trim()) && validModel && validMode && !modelUnavailable;
  const actionMode: ActionMode = working ? 'stop' : 'send';
  // Native disabled only when the action cannot exist here; an empty draft stays focusable (aria-disabled).
  const actionUnavailable = working ? !onStop : !onSend || !validModel || !validMode;
  // Only Send is blocked by the model; a run in progress still offers Stop.
  const blocked = !working && modelUnavailable ? modelUnavailable : undefined;
  const sendReason = blocked?.sendReason ?? '';
  const modelStatus = modelUnavailable ? <span className={modelButtonStyles.status} aria-hidden="true"><PlugsIcon size={12} aria-hidden="true" />{modelUnavailable.status}</span> : null;
  const modelTitle = modelUnavailable ? `${modelUnavailable.detail} ${copy.modelTitle}` : copy.modelTitle;
  const modeDescription = validMode ? modeDescriptions?.[view.modeId] : undefined;
  const modeSummary = modeDescriptions ? modeOptions.flatMap((mode) => modeDescriptions[mode.id] ? [`${mode.label}: ${modeDescriptions[mode.id]}`] : []).join('\n') : '';
  const [contextOpen, setContextOpen] = useState(false);
  const composing = useRef(false);
  const addRef = useRef<HTMLButtonElement>(null);
  const expandRef = useRef<HTMLButtonElement>(null);
  // Send and Stop share one button so keyboard focus survives the switch. These refs keep a press,
  // or a double-click, that began as one action from completing as the other.
  const pressedAs = useRef<ActionMode | null>(null);
  const clickSequenceAs = useRef<ActionMode | null>(null);
  const modelButtonRef = useRef<HTMLButtonElement>(null);
  const modelSelectRef = useRef<HTMLSelectElement>(null);
  // Each blocked send attempt remounts the status text: a fresh node re-announces it and replays its cue.
  const [restated, setRestated] = useState(0);
  // A block that clears and later returns starts quiet again.
  if (!blocked && restated !== 0) setRestated(0);

  function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (canSend) onSend?.();
  }

  function handleKeyDown(event: KeyboardEvent<HTMLTextAreaElement>) {
    const sendKey = event.key === 'Enter' && !event.shiftKey && !event.altKey && (enterBehavior === 'send' || event.ctrlKey || event.metaKey) && !composing.current && !event.nativeEvent.isComposing && event.keyCode !== 229;
    if (!sendKey) return;
    // Blocked by the model: the send key never sends (held or not) and the draft stays as typed;
    // a fresh press restates why. Shift+Enter still adds a line.
    if (blocked) {
      event.preventDefault();
      if (!event.repeat) setRestated((count) => count + 1);
    } else if (!event.repeat && canSend) {
      event.preventDefault();
      event.currentTarget.form?.requestSubmit();
    }
  }

  function chooseModel() {
    if (onOpenModel) {
      // Open from the model control itself, so the picker hands focus back there once this line is gone.
      modelButtonRef.current?.focus();
      onOpenModel();
      return;
    }
    const select = modelSelectRef.current;
    if (!select) return;
    select.focus();
    try {
      select.showPicker();
    } catch {
      // Not every engine opens a select's list on request; the focused select is still the way forward.
    }
  }

  return (
    <footer className={styles.canvas}>
      <form
        className={styles.composer}
        data-expanded={view.expanded}
        onSubmit={submit}
        aria-label="Message composer"
        onCompositionStart={() => { composing.current = true; }}
        onCompositionEnd={() => { composing.current = false; }}
        onBlur={(event) => {
          if (!event.currentTarget.contains(event.relatedTarget)) setContextOpen(false);
        }}
        onKeyDown={(event) => {
          if (event.key !== 'Escape' || composing.current || event.nativeEvent.isComposing || event.keyCode === 229) return;
          if (contextOpen) {
            event.preventDefault();
            setContextOpen(false);
            addRef.current?.focus();
          } else if (view.expanded) {
            event.preventDefault();
            onExpandedChange(!view.expanded);
            expandRef.current?.focus();
          }
        }}
      >
        {contextOpen ? (
          <div id={id('fixture-context')} className={styles.contextPicker} data-shortcuts-blocked role="region" aria-label={copy.context}>
            <h2>{copy.context}</h2>
            {copy.contextDescription ? <p>{copy.contextDescription}</p> : null}
            <div className={styles.contextOptions}>
              {contextReferences.map((reference, index) => {
                const attached = contextIds.includes(reference.id);
                return (
                  <button
                    key={reference.id}
                    type="button"
                    autoFocus={index === 0}
                    aria-pressed={attached}
                    onClick={() => {
                      if (contextOpen && contextReferences.some((entry) => entry.id === reference.id)) (attached ? onRemoveReference : onAddReference)(reference.id);
                    }}
                  >
                    <span><span className={styles.kind}>{reference.kind}</span>{' '}{reference.label}</span>
                    {attached ? <CheckIcon size={14} aria-hidden="true" /> : <PlusIcon size={14} aria-hidden="true" />}
                  </button>
                );
              })}
            </div>
          </div>
        ) : null}
        {contextIds.length > 0 ? (
          <ul className={styles.chips} aria-label={copy.attachedContext}>
            {contextIds.map((id) => {
              const reference = contextReferences.find((entry) => entry.id === id)!;
              return (
                <li key={id} data-context-id={id}>
                  <span title={`${reference.label} (${reference.kind})`}>{reference.label}</span>
                  <button type="button" aria-label={`Remove ${reference.label} context`} onClick={() => {
                    if (contextIds.includes(id)) onRemoveReference(id);
                    addRef.current?.focus();
                  }}><XIcon size={12} aria-hidden="true" /></button>
                </li>
              );
            })}
          </ul>
        ) : null}
        <label className="srOnly" htmlFor={id('message-draft')}>Message draft for {view.title}</label>
        <textarea
          id={id('message-draft')}
          className={styles.input}
          rows={2}
          spellCheck={false}
          value={view.draft}
          placeholder="Describe a task, or ask a question…"
          onChange={(event) => onDraftChange(event.currentTarget.value)}
          onKeyDown={handleKeyDown}
        />
        {blocked ? (
          <p className={styles.blocked}>
            <PlugsIcon size={14} aria-hidden="true" />
            <span>
              <span role="status" aria-atomic="true"><span key={restated} data-restated={restated > 0 || undefined}>{blocked.message}</span></span>
              {' '}
              <button type="button" className={styles.blockedAction} onClick={chooseModel}>Choose a model</button>
            </span>
          </p>
        ) : null}
        <div className={styles.controls}>
          <div className={styles.tools}>
            <IconButton
              ref={addRef}
              variant="quiet"
              label={copy.addContext}
              aria-expanded={contextOpen}
              aria-controls={contextOpen ? id('fixture-context') : undefined}
              title={copy.contextTitle}
              onClick={() => setContextOpen(!contextOpen)}
            ><PlusIcon size={17} aria-hidden="true" /></IconButton>
            <IconButton
              ref={expandRef}
              variant="quiet"
              label={view.expanded ? 'Collapse draft' : 'Expand draft'}
              aria-expanded={view.expanded}
              aria-controls={id('message-draft')}
              title={view.expanded ? 'Collapse draft · Escape' : 'Expand draft'}
              onClick={() => onExpandedChange(!view.expanded)}
            >{view.expanded ? <ArrowsInSimpleIcon size={16} aria-hidden="true" /> : <ArrowsOutSimpleIcon size={16} aria-hidden="true" />}</IconButton>
          </div>
          <div className={styles.modelPicker}>
            <>{modelMark ?? <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" aria-hidden="true"><path d="M8 2 14 8 8 14 2 8Z" /></svg>}</>
            <label className="srOnly" htmlFor={id('model-choice')}>Model for this chat</label>
            {onOpenModel ? <button
              ref={modelButtonRef}
              className={modelButtonStyles.button}
              id={id('model-choice')}
              type="button"
              title={modelTitle}
              aria-haspopup="dialog"
              aria-keyshortcuts={keyShortcuts?.model}
              aria-describedby={modelUnavailable ? id('model-unavailable') : undefined}
              data-unavailable={modelUnavailable ? true : undefined}
              onClick={onOpenModel}
            ><span className={modelButtonStyles.label}>{modelOptions.find((option) => option.id === view.modelId)?.label ?? 'Select model'}</span>{modelStatus}</button> : <select
              ref={modelSelectRef}
              id={id('model-choice')}
              title={modelTitle}
              aria-describedby={modelUnavailable ? id('model-unavailable') : undefined}
              value={validModel ? view.modelId : ''}
              onChange={(event) => {
                const model = modelOptions.find((option) => option.id === event.currentTarget.value);
                if (model) onModelChange(model.id);
              }}
            >
              {!validModel ? <option value="" disabled>Select model</option> : null}
              {modelOptions.map((model) => <option key={model.id} value={model.id}>{model.label}</option>)}
            </select>}
            <CaretDownIcon size={11} className={styles.selectCaret} aria-hidden="true" />
          </div>
          {onOpenModel ? null : modelStatus}
          {modelUnavailable ? <span id={id('model-unavailable')} className="srOnly">{modelUnavailable.detail}</span> : null}
          <span className={styles.separator} aria-hidden="true" />
          <div className={styles.modePicker}>
            <label className="srOnly" htmlFor={id('mode-choice')}>Mode for this chat</label>
            <select
              id={id('mode-choice')}
              value={validMode ? view.modeId : ''}
              title={modeSummary || undefined}
              aria-describedby={modeDescription ? id('mode-description') : undefined}
              onChange={(event) => {
                const mode = event.currentTarget.value;
                if (modeOptions.some((option) => option.id === mode)) onModeChange(mode);
              }}
            >
              {!validMode ? <option value="" disabled>Select mode</option> : null}
              {modeOptions.map((mode) => <option key={mode.id} value={mode.id}>{mode.label}</option>)}
            </select>
            {modeDescription ? <span id={id('mode-description')} className="srOnly">{modeDescription}</span> : null}
            <CaretDownIcon size={11} className={styles.selectCaret} aria-hidden="true" />
          </div>
          {view.contextUsage && onOpenContext ? <ContextMeter view={validModel && view.contextUsage.profile.modelId === view.modelId ? view.contextUsage : undefined} target={contextTarget} emptyChat={emptyChat} onOpen={onOpenContext} /> : null}
          <button
            className={styles.action}
            data-mode={actionMode}
            type={working ? 'button' : 'submit'}
            disabled={actionUnavailable}
            aria-disabled={!actionUnavailable && !working && !canSend ? true : undefined}
            aria-keyshortcuts={working ? keyShortcuts?.stop : enterBehavior === 'send' ? 'Enter Control+Enter Meta+Enter' : 'Control+Enter Meta+Enter'}
            title={working ? copy.stopTitle || copy.stop : sendReason || copy.sendTitle}
            aria-describedby={sendReason ? id('send-reason') : undefined}
            onPointerDown={(event) => { if (event.button === 0) pressedAs.current = actionMode; }}
            onKeyDown={(event) => {
              if (event.key !== 'Enter' && event.key !== ' ') return;
              // A held key must not repeat Stop into Send (or Send into Stop).
              if (event.repeat) event.preventDefault();
              else pressedAs.current = actionMode;
            }}
            onBlur={() => { pressedAs.current = null; }}
            onClick={(event) => {
              const pressed = pressedAs.current;
              pressedAs.current = null;
              const switched = pressed !== null && pressed !== actionMode || event.detail > 1 && clickSequenceAs.current !== actionMode;
              if (event.detail <= 1) clickSequenceAs.current = actionMode;
              // Cancel the native submit before any synchronous run-state change can retarget it.
              if (switched || working || !canSend) event.preventDefault();
              if (!switched && working) onStop?.();
              else if (!switched && blocked) setRestated((count) => count + 1);
            }}
          >
            {working ? <StopIcon size={13} aria-hidden="true" /> : null}
            <span>{working ? copy.stop : 'Send'}</span>
            {working ? null : <ArrowUpIcon size={15} aria-hidden="true" />}
          </button>
          {sendReason ? <span id={id('send-reason')} className="srOnly">{sendReason}</span> : null}
        </div>
      </form>
    </footer>
  );
}
