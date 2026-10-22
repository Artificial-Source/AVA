import { useId, useState } from 'react';
import { CaretDownIcon } from '@phosphor-icons/react/dist/csr/CaretDown';
import { ChatStatus } from '../../components/ChatStatus';
import { Disclosure } from '../../ui/Disclosure';
import { ProviderMark } from '../../ui/ProviderMark';
import { contextUsage, formatTokenCount, type ContextUsageView, type CumulativeUsageView } from '../contextUsage';
import styles from './ContextInfo.module.css';

export type LocalMessageProjection = Readonly<{
  id: string;
  role: 'user' | 'assistant';
  origin: 'history' | 'local';
  text: string;
  code?: Readonly<{ filename: string; text: string }>;
  note?: string;
}>;

export type ContextInfoView = Readonly<{
  sessionTitle: string;
  usage: ContextUsageView;
  cumulative: CumulativeUsageView;
  systemPrompt: string;
  messages: readonly LocalMessageProjection[];
}>;

// At this share of the window, the next turns may trigger compaction: the user should know.
const NEAR_LIMIT_PERCENT = 90;

const unknownReasons: Readonly<Record<string, string>> = {
  Unknown: 'No usage snapshot for this model yet.',
  'Profile mismatch': 'The last snapshot was taken with a different model.',
  'Stale snapshot': 'The last snapshot is out of date.',
  'Unknown estimate': 'The last snapshot has no token estimate.',
  'Unknown window': 'This model’s window size is unknown.',
};

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
  const known = usage.percentage !== null && usage.activeTokens !== null && usage.windowTokens !== null;
  const near = known && usage.percentage! >= NEAR_LIMIT_PERCENT;
  const lead = known ? `${usage.percentage}% of window used` : 'Window use unknown';
  return (
    <div className={styles.context}>
      <section className={styles.summary} aria-label="Window usage">
        <div className={styles.lead}>
          <p>{lead}</p>
          {near ? <ChatStatus status="approval" label="Near the limit" /> : null}
        </div>
        {known ? (
          <div className={styles.meter} role="meter" aria-label="Context window used" aria-valuemin={0} aria-valuemax={usage.windowTokens!} aria-valuenow={Math.min(usage.activeTokens!, usage.windowTokens!)} aria-valuetext={lead} data-level={near ? 'near' : 'normal'}>
            {/* Counts may exceed the window; the bar stops at full, the words do not. */}
            <span style={{ width: `${Math.min(usage.percentage!, 100)}%` }} />
          </div>
        ) : <div className={styles.meter} aria-hidden="true" />}
        <p className={styles.detail}>{known ? `${formatTokenCount(usage.activeTokens)} of ${formatTokenCount(usage.windowTokens)} tokens` : unknownReasons[usage.reason ?? 'Unknown'] ?? unknownReasons.Unknown}</p>
      </section>

      <h3 className={styles.section}>Window</h3>
      <dl className={styles.facts}>
        <div><dt>Chat</dt><dd>{view.sessionTitle}</dd></div>
        <div><dt>Model</dt><dd>{profile.modelLabel}</dd></div>
        <div><dt>Provider</dt><dd className={styles.provider}><ProviderMark id={profile.providerId} />{profile.providerLabel}</dd></div>
        <div><dt>Configured window</dt><dd>{formatTokenCount(usage.windowTokens)}</dd></div>
        <div><dt>Active estimate</dt><dd>{formatTokenCount(usage.activeTokens)}</dd></div>
        {usage.reason ? <div><dt>Snapshot</dt><dd>{usage.reason}</dd></div> : snapshot?.capturedAt ? <div><dt>Snapshot</dt><dd>{snapshot.capturedAt}</dd></div> : null}
      </dl>

      <h3 className={styles.section}>Session usage</h3>
      {view.cumulative.capturedAt ? <p className={styles.caption}>As of {view.cumulative.capturedAt}</p> : null}
      <dl className={styles.facts}>
        <div><dt>Input tokens</dt><dd>{formatTokenCount(view.cumulative.input)}</dd></div>
        <div><dt>Output tokens</dt><dd>{formatTokenCount(view.cumulative.output)}</dd></div>
        <div><dt>Reasoning tokens</dt><dd>{formatTokenCount(view.cumulative.reasoning)}</dd></div>
        <div><dt>Cache read</dt><dd>{formatTokenCount(view.cumulative.cacheRead)}</dd></div>
        <div><dt>Cache write</dt><dd>{formatTokenCount(view.cumulative.cacheWrite)}</dd></div>
        <div className={styles.total}><dt>Reported total</dt><dd>{formatTokenCount(view.cumulative.total)}</dd></div>
        <div><dt>Cost estimate · USD</dt><dd>{cost.amount}</dd></div>
        <div><dt>Cost coverage</dt><dd>{cost.completeness}</dd></div>
      </dl>

      <h3 className={styles.section}>Messages</h3>
      <dl className={styles.facts}>
        <div><dt>Committed</dt><dd>{view.messages.length}</dd></div>
        <div><dt>You / AVA</dt><dd>{users} / {view.messages.length - users}</dd></div>
      </dl>
      <div className={styles.views}>
        <ContextDisclosure title="System prompt" label="System prompt text" text={view.systemPrompt} active={active} />
        <ContextDisclosure title="Raw messages" label="Raw message JSON" text={JSON.stringify(view.messages, null, 2)} active={active} />
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
