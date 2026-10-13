import { useState } from 'react';
import { useViewGuard } from './useViewGuard';
import { formatTokenCount, type ContextUsageView } from '../chat/contextUsage';
import { Button } from '../ui/Button';
import { boundedInteger, contextProjection, defaultContextPolicy, type ContextPolicy } from './behaviorPreferences';
import type { BehaviorSettingsProps } from './GeneralSettings';
import { PreferenceRow } from './ScreenPrimitives';
import styles from './BehaviorSettings.module.css';
import controls from './screens.module.css';

export function ContextManagementSettings({ preferences, active, allowed, update, usage }: BehaviorSettingsProps & { usage?: ContextUsageView }) {
  const value = preferences.contextPolicy ?? defaultContextPolicy;
  const projection = contextProjection(value, usage);
  const initial = { triggerPercent: String(value.triggerPercent), recentTurns: String(value.recentTurns), recentTokens: String(value.recentTokens) };
  const [draft, setDraft] = useState({ source: value, inputs: initial });
  if (draft.source !== value) setDraft({ source: value, inputs: initial });
  const gate = useViewGuard(JSON.stringify([draft.inputs, value, active]));
  function valid() { return allowed() && gate.valid(); }
  function change(patch: Partial<ContextPolicy>) { if (valid()) update({ contextPolicy: { ...value, ...patch } }); }
  function number(key: 'triggerPercent' | 'recentTurns' | 'recentTokens', label: string, min: number, max: number) {
    const input = draft.inputs[key]; const invalid = input === '' || !boundedInteger(Number(input), min, max);
    return <div><input className={`${controls.input} ${styles.numbers}`} type="number" aria-label={label} aria-invalid={invalid || undefined} value={input} min={min} max={max} step="1" onChange={(event) => {
      if (!valid()) return;
      const text = event.currentTarget.value; const next = event.currentTarget.valueAsNumber;
      setDraft({ source: value, inputs: { ...draft.inputs, [key]: text } });
      if (text !== '' && boundedInteger(next, min, max)) change({ [key]: next });
    }} />{invalid ? <p className={styles.error} role="alert">Enter a whole number from {min} to {max}. Projection keeps the last valid value.</p> : null}</div>;
  }
  const changed = JSON.stringify(value) !== JSON.stringify(defaultContextPolicy) || JSON.stringify(draft.inputs) !== JSON.stringify(initial);
  return <div>
    <p className={styles.intro}>Policy preview only. Explore a trigger and recent-history budget against supplied estimates. No compaction runs, no context is removed and no configuration is saved.</p>
    <fieldset className={styles.group} disabled={!active}><legend>Compaction policy preview</legend>
      <PreferenceRow title="Automatic compaction" description="Include automatic eligibility in this projection, not in an actual run."><input type="checkbox" aria-label="Preview automatic compaction" checked={value.autoCompact} onChange={(event) => change({ autoCompact: event.currentTarget.checked })} /></PreferenceRow>
      <PreferenceRow title="Trigger percentage" description="1–95% of the configured window. The trigger is rounded down to whole tokens.">{number('triggerPercent', 'Compaction trigger percentage', 1, 95)}</PreferenceRow>
      <PreferenceRow title="Recent turns to retain" description="A proposed recent-history minimum, from 0 to 20 turns.">{number('recentTurns', 'Recent turns to retain', 0, 20)}</PreferenceRow>
      <PreferenceRow title="Recent token budget" description="A proposed retention budget, from 0 to 100,000 tokens. Not a measured remainder.">{number('recentTokens', 'Recent token budget', 0, 100000)}</PreferenceRow>
    </fieldset>
    <section className={styles.sample} aria-label="Context policy projection"><h3>Supplied context · separate from policy</h3><dl className={styles.facts}>
      <dt>Current profile</dt><dd>{usage ? `${usage.profile.modelLabel} · ${usage.profile.providerLabel}` : 'Unknown'}</dd>
      <dt>Configured window</dt><dd>{formatTokenCount(projection.windowTokens)} tokens</dd>
      <dt>Active estimate</dt><dd>{formatTokenCount(projection.activeTokens)}{projection.activeTokens !== null ? ' tokens' : ''}{projection.percentage !== null ? ` · ${projection.percentage}%` : ''}</dd>
      {projection.reason ? <><dt>Estimate status</dt><dd>{projection.reason}</dd></> : null}
      <dt>Preview trigger</dt><dd>{formatTokenCount(projection.threshold)}{projection.threshold !== null ? ` tokens · ${value.triggerPercent}%` : ''}</dd>
      <dt>Proposed retention</dt><dd>{value.recentTurns} recent turns · {formatTokenCount(value.recentTokens)} token budget</dd>
    </dl><div className={styles.testResult} role="status">{!projection.valid ? 'Unknown eligibility · invalid policy.' : !value.autoCompact ? 'Automatic eligibility disabled in this preview.' : projection.eligible === null ? 'Unknown eligibility · a matching current estimate and known window are required.' : projection.eligible ? 'Eligible in this preview · the supplied estimate meets the trigger. Nothing was compacted.' : 'Below the preview trigger. Nothing was compacted.'}</div></section>
    <div className={styles.reset}><p>Restore this policy preview only.</p><Button disabled={!active || !changed} onClick={() => { if (changed && valid()) { setDraft({ source: defaultContextPolicy, inputs: { triggerPercent: '80', recentTurns: '2', recentTokens: '20000' } }); update({ contextPolicy: defaultContextPolicy }); } }}>Reset Context</Button></div>
  </div>;
}
