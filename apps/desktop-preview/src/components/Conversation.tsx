import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { MessageTurn } from '../chat/widgets/MessageTurn';
import { useBottomFollow } from '../chat/useBottomFollow';
import { toolDetailsOpen, type ToolDisplay } from '../chat/toolDisplay';
import { ToolDisplaySelect } from '../chat/widgets/ToolDisplaySelect';
import type { ConversationPreferences } from '../screens/behaviorPreferences';
import type { Chat, Message } from '../state/types';
import { canFocus } from '../ui/DialogFrame';
import { AvaMark } from './AvaMark';
import { ChatStatus } from './ChatStatus';
import { ToolActivityGroup } from '../chat/widgets/ToolActivityGroup';
import styles from './Conversation.module.css';

type Choice = { open: boolean; revision: number };
const ConversationTurn = memo(function ConversationTurn({ message, group, open, intent, display, active, showCompletedOutput, onTools }: {
  message: Message; group: string; open?: boolean; intent: string; display: ToolDisplay; active: boolean; showCompletedOutput?: boolean; onTools: (open: boolean, id?: string) => void;
}) {
  const turn = useMemo(() => ({ ...message, state: message.role === 'assistant' ? message.lifecycle ?? 'committed' : 'committed' as const, author: message.role === 'user' ? 'You' : 'AVA', label: message.role === 'assistant' ? message.lifecycle ? 'AVA supplied run response' : 'AVA example response' : message.origin === 'local' ? 'Your local preview message' : 'Your example message' }), [message]);
  return <MessageTurn turn={turn}
    mark={message.role === 'assistant' ? <AvaMark /> : undefined}
    activity={message.role === 'assistant' && message.activity ? <ToolActivityGroup individual records={message.activity} display={display} lifecycle={message.lifecycle} active={active} showCompletedOutput={showCompletedOutput} open={open} intent={intent} onOpenChange={(value) => onTools(value, group)} /> : undefined} />;
});

export function Conversation({ chat, preferences, active = true, onDisplayChange }: { chat: Chat; preferences?: ConversationPreferences; active?: boolean; onDisplayChange?: (display: ToolDisplay, control: HTMLSelectElement) => void }) {
  const lastMessage = chat.messages.at(-1);
  const supplied = lastMessage?.role === 'assistant' && lastMessage.lifecycle ? lastMessage : null;
  const following = useBottomFollow(supplied, active, Boolean(supplied));
  const transcriptRef = following.ref;
  const display = preferences?.toolDisplay ?? 'compact';
  const groupId = (id: string) => JSON.stringify([chat.id, id]);
  const groups = chat.messages.flatMap((message) => message.role === 'assistant' && message.activity?.length ? [{ id: groupId(message.id), message }] : []);
  const groupIds = [...new Set(groups.map(({ id }) => id))];
  const [overrides, setOverrides] = useState<Record<string, Choice>>({});
  const [preset, setPreset] = useState({ display, revision: 0 });
  // A different preset is fresh intent, not a new turn/group/row identity.
  if (preset.display !== display) { setPreset({ display, revision: preset.revision + 1 }); setOverrides({}); }
  else {
    const retained = Object.fromEntries(Object.entries(overrides).filter(([id]) => groupIds.includes(id)));
    if (Object.keys(retained).length !== Object.keys(overrides).length) setOverrides(retained);
  }
  const revision = useRef(0);
  const latest = useRef({ groupIds, active }); latest.current = { groupIds, active };
  const showTools = useCallback((open: boolean, id?: string) => {
    const current = latest.current;
    if (!current.active || !canFocus(transcriptRef.current) || id !== undefined && !current.groupIds.includes(id)) return;
    const choice = { open, revision: ++revision.current };
    setOverrides((values) => id === undefined ? Object.fromEntries(current.groupIds.map((key) => [key, choice])) : { ...values, [id]: choice });
  }, []);
  const allExpanded = groups.length > 0 && groups.every(({ id, message }) => overrides[id]?.open ?? message.activity!.every((record) => toolDetailsOpen(display, record.state, message.lifecycle)));
  const lastLocalMessageId = lastMessage?.role === 'user' && lastMessage.origin === 'local' ? lastMessage.id : null;

  useEffect(() => {
    if (lastLocalMessageId && active && preferences?.scrollAfterSend !== false && canFocus(transcriptRef.current)) transcriptRef.current.scrollTop = transcriptRef.current.scrollHeight;
  }, [lastLocalMessageId]);

  return <div className={styles.transcriptFrame}>
    {onDisplayChange || groupIds.length ? <div className={styles.toolActions}>
      {onDisplayChange ? <label className={styles.displayLabel}>Chat display<ToolDisplaySelect className={styles.displaySelect} display={display} showCompletedOutput={preferences?.showCompletedOutput} disabled={!active} onChange={onDisplayChange} /></label> : null}
      {groupIds.length ? <><button data-tool-toggle className={styles.toolsToggle} type="button" onClick={() => showTools(!allExpanded)}>{allExpanded ? 'Collapse all tools' : 'Expand all tools'}</button>{!allExpanded ? <button className={styles.toolsToggle} type="button" onClick={() => showTools(false)}>Collapse all tools</button> : null}</> : null}
    </div> : null}
    <div ref={transcriptRef} className={styles.transcript} data-empty={chat.messages.length === 0} role="region" tabIndex={0} aria-label={`Conversation: ${chat.title}`}
      onPointerDown={following.onPointerDown} onBlur={following.onBlur} onScroll={(event) => { if (event.target === event.currentTarget) following.onScroll(); }}>
      <div className={styles.messages}>
        {chat.messages.length === 0 ? <div className={styles.empty} role="img" aria-label="AVA"><AvaMark /></div> : null}
        {chat.messages.map((message) => { const group = groupId(message.id); return <ConversationTurn key={message.id} message={message} group={group} open={overrides[group]?.open} intent={`${group}:${preset.revision}:${overrides[group]?.revision ?? 0}`} display={display} active={active} showCompletedOutput={preferences?.showCompletedOutput} onTools={showTools} />; })}
        {chat.messages.length > 0 && chat.status !== 'idle' ? <div className={styles.fixtureState}><ChatStatus status={chat.status} />{chat.status === 'approval' ? <p>Permission decisions are not part of this preview.</p> : null}</div> : null}
      </div>
    </div>
    {following.away && supplied ? <button ref={following.latestRef} type="button" className={styles.latest} onClick={following.latest}>Latest</button> : null}
  </div>;
}
