import { useId, useState } from 'react';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import { Disclosure } from '../../ui/Disclosure';
import { ProviderMark } from '../../ui/ProviderMark';
import { contextUsage, formatTokenCount, type ContextUsageView, type CumulativeUsageView } from '../contextUsage';
import styles from './ContextInfo.module.css';

export type LocalMessageProjection = Readonly<{
  id: string;
  role: 'user' | 'assistant';
  origin: 'example' | 'local';
  text: string;
  code?: Readonly<{ filename: string; text: string }>;
  note?: string;
}>;

export type ContextInfoView = Readonly<{
  sessionTitle: string;
  qualifier: string;
  usage: ContextUsageView;
  cumulative: CumulativeUsageView;
  systemPrompt: string;
  messages: readonly LocalMessageProjection[];
}>;

function ContextDisclosure({ title, label, text, active }: { title: string; label: string; text: string; active: boolean }) {
  const id = useId();
  const [open, setOpen] = useState(false);
  return (
    <div className={styles.disclosure} onKeyDown={(event) => {
      if (active && open && event.key === 'Escape' && !event.nativeEvent.isComposing) {
        event.preventDefault();
        event.stopPropagation();
        setOpen(false);
      }
    }}>
      <Disclosure
        id={id}
        open={open}
        onOpenChange={(next) => { if (active) setOpen(next); }}
        triggerClassName={styles.trigger}
        trigger={<>{title}<CaretDownIcon size={12} aria-hidden="true" /></>}
      >
        <pre className={styles.raw} role="region" aria-label={label} tabIndex={0}>{text}</pre>
      </Disclosure>
    </div>
  );
}

export function ContextInfo({ view, active = true }: { view: ContextInfoView; active?: boolean }) {
  const { profile, snapshot } = view.usage;
  const usage = contextUsage(view.usage);
  const cost = formatUsageCost(view.cumulative.cost);
  const users = view.messages.filter((message) => message.role === 'user').length;
  return (
    <div className={styles.context}>
      <p className={styles.qualifier}>{view.qualifier}</p>
      <dl className={styles.facts}>
        <div><dt>Session</dt><dd>{view.sessionTitle}</dd></div>
        <div><dt>Provider</dt><dd className={styles.provider}><ProviderMark id={profile.providerId} />{profile.providerLabel}</dd></div>
        <div><dt>Model</dt><dd>{profile.modelLabel}</dd></div>
        <div><dt>Configured window</dt><dd>{formatTokenCount(usage.windowTokens)}</dd></div>
        <div><dt>Active estimate</dt><dd>{formatTokenCount(usage.activeTokens)}</dd></div>
        <div><dt>Window used</dt><dd>{usage.percentage === null ? 'Unknown' : `${usage.percentage}%`}</dd></div>
        {usage.reason ? <div><dt>Context snapshot</dt><dd>{usage.reason}</dd></div> : snapshot?.capturedAt ? <div><dt>Context snapshot</dt><dd>{snapshot.capturedAt}</dd></div> : null}
      </dl>
      <h3 className={styles.section}>Cumulative usage</h3>
      <dl className={styles.facts}>
        <div><dt>Input tokens</dt><dd>{formatTokenCount(view.cumulative.input)}</dd></div>
        <div><dt>Output tokens</dt><dd>{formatTokenCount(view.cumulative.output)}</dd></div>
        <div><dt>Reasoning tokens</dt><dd>{formatTokenCount(view.cumulative.reasoning)}</dd></div>
        <div><dt>Cache read</dt><dd>{formatTokenCount(view.cumulative.cacheRead)}</dd></div>
        <div><dt>Cache write</dt><dd>{formatTokenCount(view.cumulative.cacheWrite)}</dd></div>
        <div><dt>Reported total</dt><dd>{formatTokenCount(view.cumulative.total)}</dd></div>
        <div><dt>Cost estimate · USD</dt><dd>{cost.amount}</dd></div>
        <div><dt>Cost coverage</dt><dd>{cost.completeness}</dd></div>
        {view.cumulative.capturedAt ? <div><dt>Usage snapshot</dt><dd>{view.cumulative.capturedAt}</dd></div> : null}
      </dl>
      <h3 className={styles.section}>Local messages</h3>
      <dl className={styles.facts}>
        <div><dt>Committed</dt><dd>{view.messages.length}</dd></div>
        <div><dt>User / assistant</dt><dd>{users} / {view.messages.length - users}</dd></div>
      </dl>
      <div className={styles.views}>
        <ContextDisclosure title="System prompt" label="Example system prompt" text={view.systemPrompt} active={active} />
        <ContextDisclosure title="Raw messages" label="Local committed messages" text={JSON.stringify(view.messages, null, 2)} active={active} />
      </div>
    </div>
  );
}

export function formatUsageCost(cost: CumulativeUsageView['cost']) {
  if (!cost || cost.completeness === 'unknown' || cost.usd === null || !Number.isFinite(cost.usd) || cost.usd < 0) {
    return { amount: 'Unknown', completeness: 'Unknown' };
  }
  return {
    amount: cost.usd.toLocaleString('en-US', { style: 'currency', currency: 'USD', minimumFractionDigits: 2, maximumFractionDigits: 4 }),
    completeness: cost.completeness === 'complete' ? 'Complete' : 'Partial',
  };
}
