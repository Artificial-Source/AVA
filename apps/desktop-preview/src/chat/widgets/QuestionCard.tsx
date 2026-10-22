import { useId, useRef, useState } from 'react';
import { WarningCircleIcon } from '@phosphor-icons/react/dist/csr/WarningCircle';
import { Button } from '../../ui/Button';
import { Status } from '../../ui/Status';
import { Surface } from '../../ui/Surface';
import feedback from '../../ui/feedback.module.css';
import type { QuestionAnswer, QuestionIntent, QuestionView, RequestIdentity } from '../contracts';
import { validAnswer, validQuestion } from '../requestValidation';
import { useRequestIntent } from '../useRequestIntent';
import styles from './widgets.module.css';

type QuestionCardProps = {
  request: QuestionView;
  value: QuestionAnswer;
  onChange: (value: QuestionAnswer) => void;
  onAnswer?: (intent: QuestionIntent) => void;
  onCancel?: (identity: RequestIdentity) => void;
  // Asks the host to load the question again after it failed; at most once per version.
  onRetry?: (identity: RequestIdentity) => void;
  titled?: boolean;
};
type RequestGate = Readonly<{ submitted: boolean; claim: () => boolean }>;

export function QuestionCard({ request, value, onChange, onAnswer, onCancel, onRetry, titled = true }: QuestionCardProps) {
  const gate = useRequestIntent(request);
  // Key only request-local presentation state; parent-supplied answers stay controlled.
  return <QuestionForm key={JSON.stringify([request.id, request.version])} request={request} value={value} onChange={onChange} onAnswer={onAnswer} onCancel={onCancel} onRetry={onRetry} titled={titled} gate={gate} />;
}

function stateMessage(request: QuestionView, usable: boolean, sent: boolean) {
  if (!usable) return 'This question can’t be answered here. It’s incomplete or unsupported.';
  // A reported failure is newer than any answer this card sent for the same version.
  if (request.state === 'error') return request.error || 'This question couldn’t be completed.';
  if (sent) return 'Answer sent.';
  if (request.state === 'pending') return 'Not ready for an answer yet.';
  if (request.state === 'submitted') return 'Your answer was sent.';
  if (request.state === 'stale') return 'Replaced by a newer question.';
  if (request.state === 'closed') return 'Closed by AVA.';
  return '';
}

function QuestionForm({ request, value, onChange, onAnswer, onCancel, onRetry, titled, gate }: QuestionCardProps & { titled: boolean; gate: RequestGate }) {
  const id = useId();
  const customInput = useRef<HTMLInputElement>(null);
  const [filter, setFilter] = useState('');
  const [problem, setProblem] = useState<string | null>(null);
  // Choosing "Other…" is visible state even before anything is typed.
  const [otherChosen, setOtherChosen] = useState(false);
  // One retry per version (this form is keyed by version), separate from the answer gate.
  const [retried, setRetried] = useState(false);
  const retrying = useRef(false);
  const valid = validQuestion(request);
  const enabled = valid && request.state === 'ready' && !gate.submitted;
  const answerValid = validAnswer(request, value);
  const safeValue = Array.isArray(value.selectedIds) && typeof value.customText === 'string';
  const single = request.selection === 'single';
  const custom = valid && request.custom.kind !== 'none' ? request.custom : null;
  const hasOptions = valid && request.options.length > 0;
  const other = custom !== null && hasOptions;
  const otherChecked = other && safeValue && (single ? value.selectedIds.length === 0 && (otherChosen || value.customText.length > 0) : otherChosen || value.customText.length > 0);
  const options = valid ? request.options.filter((option) => option.label.toLocaleLowerCase().includes(filter.toLocaleLowerCase())) : [];
  const status = stateMessage(request, valid && safeValue, gate.submitted);
  // A question that failed has nothing to answer: its choices and actions give way to the cause.
  const failed = valid && safeValue && request.state === 'error';

  return (
    <Surface className={styles.request} aria-labelledby={titled ? `${id}-title` : undefined} aria-label={titled ? undefined : request.title}>
      {titled ? <h3 id={`${id}-title`} className={styles.title}>{request.title}</h3> : null}
      <p className={styles.description}>{request.description}</p>
      {failed ? <div className={styles.requestFailure}>
        <p className={styles.runLine} role="status">{retried ? <span className={styles.failureMessage}>Retry requested.</span> : <><Status status="failed" /><span className={styles.failureMessage}>{status}</span></>}</p>
        {/* Stays mounted (aria-disabled, not disabled) after a retry so keyboard focus isn't dropped. */}
        {onRetry ? <Button aria-disabled={retried || undefined} onClick={() => {
          if (retrying.current) return;
          retrying.current = true; setRetried(true);
          onRetry({ id: request.id, version: request.version });
        }}>Retry</Button> : null}
      </div> : <form aria-label={request.title} noValidate onSubmit={(event) => {
        event.preventDefault();
        if (!enabled || !onAnswer) return;
        if (other && otherChecked && !value.customText.trim()) { setProblem('Type your answer, or choose one of the options.'); customInput.current?.focus(); return; }
        if (!answerValid) { setProblem(request.required && safeValue && value.selectedIds.length === 0 && !value.customText.trim() ? (hasOptions ? 'Choose an answer to continue.' : 'Type an answer to continue.') : 'Choose one of the current options.'); return; }
        if (!gate.claim()) return;
        onAnswer({ id: request.id, version: request.version, answer: { selectedIds: [...value.selectedIds], customText: value.customText } });
      }}>
        {valid && safeValue ? <fieldset className={styles.fields} disabled={!enabled} aria-labelledby={hasOptions ? undefined : titled ? `${id}-title` : undefined}>
          {hasOptions ? <legend>{single ? 'Choose one' : 'Choose any that apply'}{request.required ? '' : ' · optional'}</legend> : null}
          {request.searchable && hasOptions ? <label className={styles.field}>Filter options<input className={styles.input} type="search" value={filter} onChange={(event) => { if (enabled) setFilter(event.currentTarget.value); }} /></label> : null}
          {options.map((option) => <label key={option.id} className={styles.option}>
            <input type={single ? 'radio' : 'checkbox'} name={`${id}-choice`} value={option.id} checked={value.selectedIds.includes(option.id)} onChange={(event) => {
              if (!enabled || !request.options.some((entry) => entry.id === option.id)) return;
              const ids = single ? [option.id] : event.currentTarget.checked ? [...new Set([...value.selectedIds, option.id])] : value.selectedIds.filter((entry) => entry !== option.id);
              if (single) setOtherChosen(false);
              onChange({ selectedIds: ids, customText: single ? '' : value.customText });
              setProblem(null);
            }} />{option.label}
          </label>)}
          {options.length === 0 && hasOptions ? <p className={styles.hint}>No options match. Your earlier choices still count.</p> : null}
          {other ? <label className={styles.option}>
            <input type={single ? 'radio' : 'checkbox'} name={`${id}-choice`} value="" checked={otherChecked} onChange={(event) => {
              if (!enabled) return;
              const checked = single || event.currentTarget.checked;
              setOtherChosen(checked);
              onChange({ selectedIds: single ? [] : value.selectedIds, customText: checked ? value.customText : '' });
              setProblem(null);
              if (checked) customInput.current?.focus();
            }} />Other…
          </label> : null}
          {custom ? <label className={other ? `${styles.field} ${styles.otherField}` : styles.field}>{custom.label}
            <input ref={customInput} className={`${styles.input} ${feedback.invalidControl}`} type={custom.kind === 'secret' ? 'password' : 'text'} autoComplete="off" spellCheck={false} aria-invalid={problem !== null && otherChecked || undefined} value={value.customText} onChange={(event) => {
              if (!enabled) return;
              // Typing is choosing "Other…": the radio shows it, so no earlier choice clears silently.
              setOtherChosen(true);
              onChange({ selectedIds: single ? [] : value.selectedIds, customText: event.currentTarget.value });
              setProblem(null);
            }} />
          </label> : null}
        </fieldset> : null}
        {problem ? <p className={feedback.fieldError} role="alert"><WarningCircleIcon size={14} aria-hidden="true" /><span>{problem}</span></p> : null}
        <p className={styles.outcome} role="status">{status}</p>
        <div className={styles.actions}>
          <Button type="submit" tone="primary" disabled={!enabled || !onAnswer || !safeValue}>Submit answer</Button>
          {onCancel ? <Button disabled={!enabled} onClick={() => {
            if (enabled && gate.claim()) onCancel({ id: request.id, version: request.version });
          }}>Cancel question</Button> : null}
        </div>
      </form>}
    </Surface>
  );
}
