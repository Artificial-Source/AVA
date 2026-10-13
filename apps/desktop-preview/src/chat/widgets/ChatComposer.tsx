import { useId, useRef, useState, type ReactNode, type FormEvent, type KeyboardEvent } from 'react';
import { ArrowUpIcon } from '@phosphor-icons/react/dist/csr/ArrowUp';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import { ArrowsInSimpleIcon } from '@phosphor-icons/react/dist/csr/ArrowsInSimple';
import { ArrowsOutSimpleIcon } from '@phosphor-icons/react/dist/csr/ArrowsOutSimple';
import { CheckIcon } from '@phosphor-icons/react/dist/csr/Check';
import { PlusIcon } from '@phosphor-icons/react/dist/csr/Plus';
import { XIcon } from '@phosphor-icons/react/dist/csr/X';
import { StopIcon } from '@phosphor-icons/react/dist/csr/Stop';
import type { ComposerView } from '../contracts';
import { validChoices } from '../requestValidation';
import { ContextMeter, type ContextTarget } from './ContextMeter';
import styles from '../../components/Composer.module.css';
import modelButtonStyles from './ModelChoiceButton.module.css';

interface ComposerCopy {
  context: string;
  contextDescription: string;
  addContext: string;
  attachedContext: string;
  contextTitle: string;
  referencePrefix: string;
  modelTitle: string;
  sendTitle: string;
  stop: string;
}

interface ChatComposerProps {
  view: ComposerView;
  idPrefix?: string;
  modelMark?: ReactNode;
  copy?: Partial<ComposerCopy>;
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
  enterBehavior?: 'send' | 'newline';
}

const defaultCopy: ComposerCopy = {
  context: 'Context references', contextDescription: 'References supplied by the host. This control does not open or upload files.',
  addContext: 'Add context', attachedContext: 'Attached context', contextTitle: 'Choose supplied context references',
  referencePrefix: 'Supplied', modelTitle: 'Available models', sendTitle: 'Send message · Ctrl/Cmd+Enter', stop: 'Request stop',
};

export function ChatComposer({ view, idPrefix, modelMark, copy: suppliedCopy, onDraftChange, onModelChange, onOpenModel, onModeChange, onExpandedChange, onAddReference, onRemoveReference, onSend, onStop, onOpenContext, contextTarget, enterBehavior = 'newline' }: ChatComposerProps) {
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
  const canSend = !working && Boolean(onSend) && Boolean(view.draft.trim()) && validModel && validMode;
  const [contextOpen, setContextOpen] = useState(false);
  const composing = useRef(false);
  const addRef = useRef<HTMLButtonElement>(null);
  const expandRef = useRef<HTMLButtonElement>(null);

  function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (canSend) onSend?.();
  }

  function handleKeyDown(event: KeyboardEvent<HTMLTextAreaElement>) {
    if (event.key === 'Enter' && !event.repeat && !event.shiftKey && !event.altKey && (enterBehavior === 'send' || event.ctrlKey || event.metaKey) && !composing.current && !event.nativeEvent.isComposing && event.keyCode !== 229 && canSend) {
      event.preventDefault();
      event.currentTarget.form?.requestSubmit();
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
            <p>{copy.contextDescription}</p>
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
                  <span title={`${copy.referencePrefix} ${reference.kind} reference`}>{reference.label}</span>
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
        <div className={styles.controls}>
          <div className={styles.tools}>
            <button
              ref={addRef}
              className="iconButton"
              type="button"
              aria-label={copy.addContext}
              aria-expanded={contextOpen}
              aria-controls={contextOpen ? id('fixture-context') : undefined}
              title={copy.contextTitle}
              onClick={() => setContextOpen(!contextOpen)}
            ><PlusIcon size={17} aria-hidden="true" /></button>
            <button
              ref={expandRef}
              className="iconButton"
              type="button"
              aria-label={view.expanded ? 'Collapse draft' : 'Expand draft'}
              aria-expanded={view.expanded}
              aria-controls={id('message-draft')}
              title={view.expanded ? 'Collapse draft · Escape' : 'Expand draft within the conversation'}
              onClick={() => onExpandedChange(!view.expanded)}
            >{view.expanded ? <ArrowsInSimpleIcon size={16} aria-hidden="true" /> : <ArrowsOutSimpleIcon size={16} aria-hidden="true" />}</button>
          </div>
          <div className={styles.modelPicker}>
            <>{modelMark ?? <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" aria-hidden="true"><path d="M8 2 14 8 8 14 2 8Z" /></svg>}</>
            <label className="srOnly" htmlFor={id('model-choice')}>Model for this chat</label>
            {onOpenModel ? <button
              className={modelButtonStyles.button}
              id={id('model-choice')}
              type="button"
              title={copy.modelTitle}
              aria-haspopup="dialog"
              onClick={onOpenModel}
            >{modelOptions.find((option) => option.id === view.modelId)?.label ?? 'Select model'}</button> : <select
              id={id('model-choice')}
              title={copy.modelTitle}
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
          <span className={styles.separator} aria-hidden="true" />
          <div className={styles.modePicker}>
            <label className="srOnly" htmlFor={id('mode-choice')}>Mode for this chat</label>
            <select
              id={id('mode-choice')}
              value={validMode ? view.modeId : ''}
              onChange={(event) => {
                const mode = event.currentTarget.value;
                if (modeOptions.some((option) => option.id === mode)) onModeChange(mode);
              }}
            >
              {!validMode ? <option value="" disabled>Select mode</option> : null}
              {modeOptions.map((mode) => <option key={mode.id} value={mode.id}>{mode.label}</option>)}
            </select>
            <CaretDownIcon size={11} className={styles.selectCaret} aria-hidden="true" />
          </div>
          {view.contextUsage && onOpenContext ? <ContextMeter view={validModel && view.contextUsage.profile.modelId === view.modelId ? view.contextUsage : undefined} target={contextTarget} onOpen={onOpenContext} /> : null}
          {working ? (
            <button key="stop" className={styles.stop} type="button" disabled={!onStop} onClick={(event) => { event.preventDefault(); if (working) onStop?.(); }}>
              <StopIcon size={13} aria-hidden="true" />
              {copy.stop}
            </button>
          ) : (
            <button key="send" className={styles.send} type="submit" disabled={!canSend} aria-keyshortcuts={enterBehavior === 'send' ? 'Enter Control+Enter Meta+Enter' : 'Control+Enter Meta+Enter'} title={copy.sendTitle}>
              Send
              <ArrowUpIcon size={15} aria-hidden="true" />
            </button>
          )}
        </div>
      </form>
    </footer>
  );
}
