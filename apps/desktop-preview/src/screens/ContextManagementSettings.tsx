import { useId, useState } from 'react';
import { WarningCircleIcon } from '@phosphor-icons/react/dist/csr/WarningCircle';
import { useViewGuard } from './useViewGuard';
import { formatTokenCount, type ContextUsageView } from '../chat/contextUsage';
import { Button } from '../ui/Button';
import feedback from '../ui/feedback.module.css';
import { boundedInteger, contextProjection, defaultContextPolicy, type ContextPolicy } from './behaviorPreferences';
import type { BehaviorSettingsProps } from './GeneralSettings';
import { PreferenceRow } from './ScreenPrimitives';
import { anchor } from './settingsIndex';
import styles from './BehaviorSettings.module.css';
import controls from './screens.module.css';

type NumberKey = 'triggerPercent' | 'recentTurns' | 'recentTokens';
type Inputs = Readonly<Record<NumberKey, string>>;
// The measured facts these sentences read; contextUsage supplies them.
type Measured = Readonly<{ activeTokens: number | null; windowTokens: number | null; percentage: number | null; reason: string | null }>;

// Digits with optional thousands separators; anything else is not a whole number.
function parseWhole(text: string) {
  const digits = text.replace(/[\s,]/gu, '');
  return /^\d+$/u.test(digits) ? Number(digits) : Number.NaN;
}
function grouped(value: number) { return Number.isSafeInteger(value) ? value.toLocaleString('en-US') : String(value); }
// The token budget shows thousands separators; the small numbers keep native number fields.
function inputsFor(policy: ContextPolicy): Inputs {
  return { triggerPercent: String(policy.triggerPercent), recentTurns: String(policy.recentTurns), recentTokens: grouped(policy.recentTokens) };
}

// Usage is only ever unknown for a stated reason; name it once instead of a second “Unknown” row.
function usageNow(projection: Measured) {
  if (projection.activeTokens !== null) return `${formatTokenCount(projection.activeTokens)} tokens${projection.percentage !== null ? ` · ${projection.percentage}%` : ''}`;
  if (projection.reason === 'Unknown' || projection.reason === 'Profile mismatch') return 'Not measured yet';
  if (projection.reason === 'Stale snapshot') return 'Out of date';
  if (projection.reason === 'Unknown estimate') return 'Not reported';
  return 'Unknown';
}
// Says exactly which fact is missing, so it never contradicts a fact shown above it.
function unknownOutcome(projection: Measured, usage?: ContextUsageView) {
  if (!usage) return 'There’s no chat to measure, so AVA can’t estimate compaction.';
  const model = usage.profile.modelLabel;
  const gap = projection.reason === 'Stale snapshot' ? 'This chat’s last usage measurement is out of date'
    : projection.reason === 'Unknown estimate' ? 'This chat’s usage wasn’t reported'
      : `This chat’s usage with ${model} hasn’t been measured yet`;
  if (projection.windowTokens === null && projection.activeTokens !== null) return `${model}’s context window isn’t known, so AVA can’t place the compaction point.`;
  if (projection.windowTokens === null) return `${gap} and ${model}’s context window isn’t known, so AVA can’t estimate compaction.`;
  return `${gap}, so AVA can’t tell how close it is to compacting.`;
}

export function ContextManagementSettings({ preferences, active, allowed, update, usage }: BehaviorSettingsProps & { usage?: ContextUsageView }) {
  const value = preferences.contextPolicy ?? defaultContextPolicy;
  const projection = contextProjection(value, usage);
  const id = useId();
  const initial = inputsFor(value);
  const [draft, setDraft] = useState<Readonly<{ source: ContextPolicy; inputs: Inputs }>>({ source: value, inputs: initial });
  if (draft.source !== value) {
    // A value this page just set keeps the text as typed; any other change replaces it.
    const keep = (key: NumberKey) => parseWhole(draft.inputs[key]) === value[key] ? draft.inputs[key] : initial[key];
    setDraft({ source: value, inputs: { triggerPercent: keep('triggerPercent'), recentTurns: keep('recentTurns'), recentTokens: keep('recentTokens') } });
  }
  const gate = useViewGuard(JSON.stringify([draft.inputs, value, active]));
  function valid() { return allowed() && gate.valid(); }
  function change(patch: Partial<ContextPolicy>) { if (valid()) update({ contextPolicy: { ...value, ...patch } }); }
  function number(key: NumberKey, label: string, unit: string, min: number, max: number) {
    const input = draft.inputs[key]; const invalid = !boundedInteger(parseWhole(input), min, max);
    const errorId = `${id}-${key}-error`;
    const text = key === 'recentTokens';
    return <div className={styles.numberField}>
      <span className={controls.unitInput} data-invalid={invalid || undefined}>
        <input type={text ? 'text' : 'number'} inputMode="numeric" autoComplete="off" spellCheck={false} aria-label={label} aria-invalid={invalid || undefined} aria-describedby={invalid ? errorId : undefined} value={input}
          {...(text ? {} : { min, max, step: 1 })}
          onChange={(event) => {
            if (!valid()) return;
            const typed = event.currentTarget.value; const next = parseWhole(typed);
            setDraft({ source: value, inputs: { ...draft.inputs, [key]: typed } });
            if (boundedInteger(next, min, max)) change({ [key]: next });
          }}
          onBlur={() => {
            // Typed text settles into the grouped form once the field is left.
            if (parseWhole(input) === value[key] && input !== initial[key]) setDraft({ source: value, inputs: { ...draft.inputs, [key]: initial[key] } });
          }} />
        <span aria-hidden="true">{unit}</span>
      </span>
      {invalid ? <p id={errorId} className={feedback.fieldError} role="alert"><WarningCircleIcon size={14} aria-hidden="true" /><span>Enter a whole number from {grouped(min)} to {grouped(max)}. The estimate uses the last valid value.</span></p> : null}
    </div>;
  }
  const pending = (['triggerPercent', 'recentTurns', 'recentTokens'] as const).some((key) => parseWhole(draft.inputs[key]) !== value[key]);
  const changed = pending || JSON.stringify(value) !== JSON.stringify(defaultContextPolicy);
  const outcome = !projection.valid ? 'Fix the highlighted value to update this estimate.'
    : !value.autoCompact ? 'Automatic compaction is off.'
      : projection.eligible === null ? unknownOutcome(projection, usage)
        : projection.eligible ? 'At this setting, this chat would be compacted now.' : 'This chat is below the compaction point.';
  return <div>
    <fieldset className={styles.group} disabled={!active}><legend>Compaction</legend>
      <div {...anchor('context.auto')}><PreferenceRow title="Automatic compaction" description="Summarize older turns when the context window fills up."><input type="checkbox" aria-label="Automatic compaction" checked={value.autoCompact} onChange={(event) => change({ autoCompact: event.currentTarget.checked })} /></PreferenceRow></div>
      <div {...anchor('context.trigger')}><PreferenceRow title="Compact at" description="Percentage of the context window, from 1 to 95%. Rounded down to whole tokens.">{number('triggerPercent', 'Compaction trigger percentage', '%', 1, 95)}</PreferenceRow></div>
      <div {...anchor('context.turns')}><PreferenceRow title="Recent turns to keep" description="Turns kept word for word after compaction, from 0 to 20.">{number('recentTurns', 'Recent turns to keep', 'turns', 0, 20)}</PreferenceRow></div>
      <div {...anchor('context.tokens')}><PreferenceRow title="Recent token budget" description="Tokens kept word for word after compaction, from 0 to 100,000.">{number('recentTokens', 'Recent token budget', 'tokens', 0, 100000)}</PreferenceRow></div>
    </fieldset>
    <section className={styles.sample} aria-labelledby={`${id}-projection`}><h3 id={`${id}-projection`}>This chat</h3><dl className={styles.facts}>
      <dt>Model</dt><dd>{usage ? `${usage.profile.modelLabel} · ${usage.profile.providerLabel}` : 'Unknown'}</dd>
      <dt>Context window</dt><dd>{projection.windowTokens !== null ? `${formatTokenCount(projection.windowTokens)} tokens` : 'Unknown'}</dd>
      <dt>In use now</dt><dd>{usageNow(projection)}</dd>
      <dt>Compacts at</dt><dd>{projection.threshold !== null ? `${formatTokenCount(projection.threshold)} tokens · ${value.triggerPercent}%` : projection.valid ? `${value.triggerPercent}% of the context window` : 'Unknown'}</dd>
      <dt>Kept after compaction</dt><dd>{value.recentTurns} recent {value.recentTurns === 1 ? 'turn' : 'turns'} · {formatTokenCount(value.recentTokens)} token budget</dd>
    </dl><p className={styles.outcome} role="status">{outcome}</p></section>
    <div className={styles.reset}><p>Restore the compaction defaults.</p><Button disabled={!active || !changed} onClick={() => { if (changed && valid()) { setDraft({ source: defaultContextPolicy, inputs: inputsFor(defaultContextPolicy) }); update({ contextPolicy: defaultContextPolicy }); } }}>Reset context</Button></div>
  </div>;
}
