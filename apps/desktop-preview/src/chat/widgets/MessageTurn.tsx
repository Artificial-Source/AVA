import type { ReactNode } from 'react';
import { AssistantContentHost } from './AssistantContentHost';
import type { MessageTurnView } from '../contracts';
import styles from '../../components/Conversation.module.css';

export function MessageTurn({ turn, mark, activity }: { turn: MessageTurnView; mark?: ReactNode; activity?: ReactNode }) {
  const attribution = turn.role === 'assistant' ? turn.attribution : undefined;
  const model = attribution?.modelLabel?.trim();
  const thinking = attribution?.thinkingLabel?.trim();
  const stateLabel = turn.state === 'streaming' ? 'Partial message · receiving' : turn.state === 'failed' ? 'Partial message · failed' : 'Partial message · canceled';
  return (
    <article className={turn.role === 'user' ? styles.userMessage : styles.agentMessage} aria-label={turn.label} data-turn-state={turn.state}>
      <div className={turn.role === 'user' ? styles.userHeading : styles.agentHeading}>{mark}<span>{turn.author}</span>{model || thinking ? <span className={styles.attribution} aria-label="Response attribution">{model ? <span aria-label={`Model: ${model}`}>{model}</span> : null}{thinking ? <span aria-label={`Thinking: ${thinking}`}>{thinking}</span> : null}</span> : null}</div>
      {turn.role === 'user' ? <div className={styles.userBubble}><p>{turn.text}</p></div> : <>
        {activity}
        <AssistantContentHost turn={turn} />
      </>}
      {turn.state !== 'committed' ? <p className={styles.note}>{stateLabel}</p> : null}
    </article>
  );
}
