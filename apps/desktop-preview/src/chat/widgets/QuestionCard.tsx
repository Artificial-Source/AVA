import { useId, useState } from 'react';
import { Button } from '../../ui/Button';
import { Surface } from '../../ui/Surface';
import type { QuestionAnswer, QuestionIntent, QuestionView, RequestIdentity } from '../contracts';
import { validAnswer, validQuestion } from '../requestValidation';
import { useRequestIntent } from '../useRequestIntent';
import styles from './widgets.module.css';

export function QuestionCard({ request, value, onChange, onAnswer, onCancel }: {
  request: QuestionView;
  value: QuestionAnswer;
  onChange: (value: QuestionAnswer) => void;
  onAnswer?: (intent: QuestionIntent) => void;
  onCancel?: (identity: RequestIdentity) => void;
}) {
  const gate = useRequestIntent(request);
  // Key only request-local presentation state; parent-supplied answers stay controlled.
  return <QuestionForm key={JSON.stringify([request.id, request.version])} request={request} value={value} onChange={onChange} onAnswer={onAnswer} onCancel={onCancel} gate={gate} />;
}

function QuestionForm({ request, value, onChange, onAnswer, onCancel, gate }: Parameters<typeof QuestionCard>[0] & { gate: ReturnType<typeof useRequestIntent> }) {
  const id = useId();
  const [filter, setFilter] = useState('');
  const [invalid, setInvalid] = useState(false);
  const valid = validQuestion(request);
  const enabled = valid && request.state === 'ready' && !gate.submitted;
  const answerValid = validAnswer(request, value);
  const safeValue = Array.isArray(value.selectedIds) && typeof value.customText === 'string';
  const options = valid ? request.options.filter((option) => option.label.toLocaleLowerCase().includes(filter.toLocaleLowerCase())) : [];

  return (
    <Surface aria-labelledby={`${id}-title`}>
      <p className={styles.eyebrow}>Question</p>
      <h3 id={`${id}-title`} className={styles.title}>{request.title}</h3>
      <p className={styles.description}>{request.description}</p>
      <form aria-label={request.title} onSubmit={(event) => {
        event.preventDefault();
        if (!enabled || !onAnswer) return;
        if (!answerValid) { setInvalid(true); return; }
        if (!gate.claim()) return;
        onAnswer({ id: request.id, version: request.version, answer: { selectedIds: [...value.selectedIds], customText: value.customText } });
      }}>
        {valid && safeValue ? <fieldset className={styles.fields} disabled={!enabled}>
          <legend>{request.selection === 'single' ? 'Choose one answer' : 'Choose answers'}{request.required ? ' · required' : ' · optional'}</legend>
          {request.searchable ? <label className={styles.field}>Filter options<input className={styles.input} type="search" value={filter} onChange={(event) => { if (enabled) setFilter(event.currentTarget.value); }} /></label> : null}
          {options.map((option) => <label key={option.id} className={styles.choice}>
            <input type={request.selection === 'single' ? 'radio' : 'checkbox'} name={`${id}-choice`} value={option.id} checked={value.selectedIds.includes(option.id)} onChange={(event) => {
              if (!enabled || !request.options.some((entry) => entry.id === option.id)) return;
              const ids = request.selection === 'single' ? [option.id] : event.currentTarget.checked ? [...new Set([...value.selectedIds, option.id])] : value.selectedIds.filter((entry) => entry !== option.id);
              onChange({ selectedIds: ids, customText: request.selection === 'single' ? '' : value.customText });
              setInvalid(false);
            }} />{option.label}
          </label>)}
          {options.length === 0 && request.options.length > 0 ? <p className={styles.scope}>No matching options. Existing selections are retained.</p> : null}
          {request.custom.kind !== 'none' ? <label className={styles.field}>{request.custom.label}
            <input className={styles.input} type={request.custom.kind === 'secret' ? 'password' : 'text'} autoComplete="off" spellCheck={false} value={value.customText} onChange={(event) => {
              if (!enabled) return;
              onChange({ selectedIds: request.selection === 'single' ? [] : value.selectedIds, customText: event.currentTarget.value });
              setInvalid(false);
            }} />
          </label> : null}
        </fieldset> : null}
        <p className={styles.state} role="status">{!valid || !safeValue ? 'Unsupported or invalid request.' : gate.submitted ? 'Intent submitted · awaiting host state' : request.state}</p>
        {invalid ? <p className={styles.scope} role="alert">Choose a valid answer from the current options.</p> : null}
        {request.state === 'error' ? <p className={styles.description}>{request.error || 'Question unavailable. A revised request is required.'}</p> : null}
        <div className={styles.actions}>
          <Button type="submit" tone="primary" disabled={!enabled || !onAnswer || !safeValue}>Submit answer</Button>
          {onCancel ? <Button disabled={!enabled} onClick={() => {
            if (enabled && gate.claim()) onCancel({ id: request.id, version: request.version });
          }}>Cancel question</Button> : null}
        </div>
      </form>
    </Surface>
  );
}
