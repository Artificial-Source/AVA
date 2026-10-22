import { contextUsage, formatContextMeter, formatTokenCount, type ContextUsageView } from '../contextUsage';
import styles from './ContextMeter.module.css';

export type ContextTarget = Readonly<{ id: string; open: boolean }>;

export function ContextMeter({ view, target, onOpen }: { view?: ContextUsageView; target?: ContextTarget; onOpen: () => void }) {
  const usage = view ? contextUsage(view) : null;
  const text = view ? formatContextMeter(view) : 'Context unknown';
  const detail = usage ? usage.reason ?? `${formatTokenCount(usage.activeTokens)} of ${formatTokenCount(usage.windowTokens)} tokens` : 'Unknown for selected model';
  return (
    <button
      className={styles.meter}
      type="button"
      aria-label={`Open Context · ${text}`}
      aria-expanded={target?.open}
      aria-controls={target?.open ? target.id : undefined}
      title={`Active context estimate: ${detail}${view ? ` · ${view.profile.providerLabel} / ${view.profile.modelLabel}` : ''}`}
      onClick={onOpen}
    >{text}</button>
  );
}
