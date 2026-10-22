import type { ReactNode } from 'react';
import { Status } from '../../ui/Status';
import { AssistantContentHost } from './AssistantContentHost';
import type { MessageTurnView } from '../contracts';
import styles from '../../components/Conversation.module.css';

// One vocabulary with the rest of the app: Responding, Failed, Stopped.
const states = {
  streaming: { status: 'working', label: 'Responding' },
  failed: { status: 'failed', label: 'Failed · reply incomplete' },
  canceled: { status: 'stopped', label: 'Stopped · reply incomplete' },
} as const;

// `notice` replaces the default turn-state line, e.g. a run notice that can retry.
export function MessageTurn({ turn, mark, activity, notice }: { turn: MessageTurnView; mark?: ReactNode; activity?: ReactNode; notice?: ReactNode }) {
  const attribution = turn.role === 'assistant' ? turn.attribution : undefined;
  const model = attribution?.modelLabel?.trim();
  const thinking = attribution?.thinkingLabel?.trim();
  const state = turn.state === 'committed' ? null : states[turn.state];
  return (
    <article className={turn.role === 'user' ? styles.userMessage : styles.agentMessage} aria-label={turn.label} data-turn-state={turn.state}>
      <div className={turn.role === 'user' ? styles.userHeading : styles.agentHeading}>{mark}<span>{turn.author}</span>{model || thinking ? <span className={styles.attribution} aria-label="Response attribution">{model ? <span aria-label={`Model: ${model}`}>{model}</span> : null}{thinking ? <span aria-label={`Thinking: ${thinking}`}>{thinking}</span> : null}</span> : null}</div>
      {turn.role === 'user' ? <div className={styles.userBubble}><p>{turn.text}</p></div> : <>
        {activity}
        <AssistantContentHost turn={turn} />
      </>}
      {notice ? <div className={styles.turnNotice}>{notice}</div> : state ? <p className={styles.turnState}><Status status={state.status} label={state.label} /></p> : null}
    </article>
  );
}
