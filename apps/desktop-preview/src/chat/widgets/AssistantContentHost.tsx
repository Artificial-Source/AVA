import { useEffect, useState } from 'react';
import type { MessageTurnView } from '../contracts';
import type AssistantContent from './AssistantContent';
import { paintText } from '../textPaint';
import styles from '../../components/Conversation.module.css';

type Owner = typeof AssistantContent;
let loaded: Owner | null = null;
let pending: Promise<Owner> | null = null;
export function prepareAssistantContent(): Promise<Owner> {
  return pending ??= import('./AssistantContent').then((module) => { loaded = module.default; return loaded; }, (error: unknown) => { pending = null; throw error; });
}
export function AssistantContentHost({ turn }: { turn: Extract<MessageTurnView, { role: 'assistant' }> }) {
  const [Owner, setOwner] = useState<Owner | null>(() => loaded);
  const [failed, setFailed] = useState(false);
  useEffect(() => {
    let active = true;
    if (!Owner) void prepareAssistantContent().then((component) => { if (active) setOwner(() => component); }, () => { if (active) setFailed(true); });
    return () => { active = false; };
  }, [Owner]);
  if (Owner) return <Owner turn={turn} />;
  // Plain text is the recovery for a chunk that failed to load, never a first paint of raw Markdown.
  if (!failed) return <>
    <div className={styles.pendingText} aria-hidden="true">{paintText(turn.markdown)}</div>
    {turn.code ? <pre className={`${styles.plainCode} ${styles.pendingText}`} aria-hidden="true"><code>{paintText(turn.code.code)}</code></pre> : null}
    {turn.note ? <p className={styles.note}>{turn.note}</p> : null}
    <p className="srOnly">Loading response…</p>
  </>;
  return <>
    <div className={styles.partialText}>{paintText(turn.markdown)}</div>
    {turn.code ? <pre className={styles.plainCode} aria-label={`${turn.code.filename}, plain text`}><code>{paintText(turn.code.code)}</code></pre> : null}
    {turn.note ? <p className={styles.note}>{turn.note}</p> : null}
    <p className={styles.note}>Response formatting unavailable; showing plain text.</p>
    {turn.markdown.length > 20000 || turn.markdown.split('\n').length > 200 || turn.code && (turn.code.code.length > 20000 || turn.code.code.split('\n').length > 200) ? <p className={styles.note}>Display limited to 200 lines or 20,000 characters.</p> : null}
  </>;
}
