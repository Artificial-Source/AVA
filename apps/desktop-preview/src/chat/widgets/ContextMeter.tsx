import { WarningDiamondIcon } from '@phosphor-icons/react/dist/csr/WarningDiamond';
import { contextUsage, formatContextMeter, formatTokenCount, type ContextUsageView } from '../contextUsage';
import styles from './ContextMeter.module.css';

export type ContextTarget = Readonly<{ id: string; open: boolean }>;

// From 80% the meter leads with the share used; from 90% it also takes the Attention look.
const HIGH_USAGE = 80;
const CRITICAL_USAGE = 90;

export function ContextMeter({ view, target, emptyChat = false, onOpen }: { view?: ContextUsageView; target?: ContextTarget; emptyChat?: boolean; onOpen: () => void }) {
  const usage = view ? contextUsage(view) : null;
  const percentage = usage && usage.activeTokens !== null ? usage.percentage : null;
  // Nothing sent yet means nothing to measure; "unknown" is kept for usage that can't be read.
  const unmeasured = emptyChat ? 'empty' : 'unknown';
  const level = percentage === null ? unmeasured : percentage >= CRITICAL_USAGE ? 'critical' : percentage >= HIGH_USAGE ? 'high' : 'normal';
  const text = level === 'empty' ? 'No usage yet' : level === 'unknown' || !view ? 'Context unknown' : level === 'normal' ? formatContextMeter(view) : `${percentage}% used`;
  const detail = usage && percentage !== null
    ? `${formatTokenCount(usage.activeTokens)} of ${formatTokenCount(usage.windowTokens)} tokens in context`
    : level === 'empty' ? 'Usage appears after the first message' : 'Context usage isn’t available for this model';
  return (
    <button
      className={styles.meter}
      type="button"
      data-level={level}
      aria-label={`Open Context · ${text}`}
      aria-expanded={target?.open}
      aria-controls={target?.open ? target.id : undefined}
      title={`${detail}${view && percentage !== null ? ` · ${view.profile.providerLabel} / ${view.profile.modelLabel}` : ''}`}
      onClick={onOpen}
    >
      {level === 'critical' ? <WarningDiamondIcon size={13} aria-hidden="true" /> : null}
      {text}
    </button>
  );
}
