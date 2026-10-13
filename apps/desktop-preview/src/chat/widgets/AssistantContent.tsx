import type { MessageTurnView } from '../contracts';
import { paintText } from '../textPaint';
import { SafeMarkdown } from '../../components/SafeMarkdown';
import { CodeBlock } from '../../components/CodeBlock';
import styles from '../../components/Conversation.module.css';

export default function AssistantContent({ turn }: { turn: Extract<MessageTurnView, { role: 'assistant' }> }) {
  return <><div className={styles.markdown}><SafeMarkdown partial={turn.state === 'streaming'}>{turn.markdown}</SafeMarkdown></div>
    {turn.code ? <><CodeBlock filename={turn.code.filename} code={paintText(turn.code.code)} partial={turn.state === 'streaming'} />{turn.code.code.length > 20000 || turn.code.code.split('\n').length > 200 ? <p className={styles.note}>Code display limited to 200 lines or 20,000 characters.</p> : null}</> : null}
    {turn.note ? <p className={styles.note}>{turn.note}</p> : null}</>;
}
