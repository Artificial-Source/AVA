import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { PermissionDecision, PermissionIntent, RunView, TurnState } from '../chat/contracts';
import { AskRetryContext } from '../chat/askRetry';
import { MessageTurn } from '../chat/widgets/MessageTurn';
import { useBottomFollow } from '../chat/useBottomFollow';
import { toolDetailsOpen, type ToolDisplay } from '../chat/toolDisplay';
import { validPermission } from '../chat/requestValidation';
import { ToolDisplaySelect } from '../chat/widgets/ToolDisplaySelect';
import { PermissionCard } from '../chat/widgets/PermissionCard';
import { RunNotice } from '../chat/widgets/RunNotice';
import type { ConversationPreferences } from '../screens/behaviorPreferences';
import type { Chat, Message } from '../state/types';
import { canFocus } from '../ui/DialogFrame';
import { AvaMark } from './AvaMark';
import { ChatStatus } from './ChatStatus';
import { ToolActivityGroup } from '../chat/widgets/ToolActivityGroup';
import styles from './Conversation.module.css';

type Choice = { open: boolean; revision: number };

const chatRuns: Partial<Record<Chat['status'], RunView>> = { working: { state: 'running' }, completed: { state: 'completed' }, stopped: { state: 'canceled' }, failed: { state: 'error', code: 'run_failed' } };

const ConversationTurn = memo(function ConversationTurn({ message, group, open, intent, display, active, showCompletedOutput, toolLifecycle, onTools, onDisclosure, onRetry, onAskRetry, onPermissionDecision }: {
  message: Message; group: string; open?: boolean; intent: string; display: ToolDisplay; active: boolean; showCompletedOutput?: boolean; toolLifecycle?: TurnState;
  onTools: (open: boolean, id?: string) => void; onDisclosure: () => void; onRetry?: () => void; onAskRetry?: () => void;
  onPermissionDecision?: (messageId: string, intent: PermissionIntent) => void;
}) {
  // A failed run's cause moves into its run notice, so it is not printed twice.
  const turn = useMemo(() => message.role === 'assistant'
    ? { ...message, note: message.lifecycle === 'failed' ? undefined : message.note, state: message.lifecycle ?? 'committed', author: 'AVA', label: 'AVA response' }
    : { ...message, state: 'committed' as const, author: 'You', label: 'Your message' }, [message]);
  const assistant = message.role === 'assistant' ? message : null;
  // A failed turn's next step lives in its run notice; its failed tools don't repeat it. The
  // provider is always present so a changing value never remounts the tool rows.
  const tools = assistant?.activity?.length ? <AskRetryContext.Provider value={toolLifecycle === 'failed' ? null : onAskRetry ?? null}>
    <ToolActivityGroup individual records={assistant.activity} display={display} lifecycle={toolLifecycle} active={active} showCompletedOutput={showCompletedOutput} open={open} intent={intent} onOpenChange={(value) => onTools(value, group)} onDisclosure={onDisclosure} />
  </AskRetryContext.Provider> : null;
  // The blocked step, expanded in place where the run is waiting. The receipt comes from the
  // stored decision, so it is the same after switching chats.
  const request = assistant?.permission;
  const decided = assistant?.permissionDecision;
  const pending = request !== undefined && decided === undefined && request.state === 'ready' && validPermission(request);
  const permission = request ? <div className={styles.permission} data-permission-pending={pending || undefined}>
    <PermissionCard request={request} decided={decided} onDecide={onPermissionDecision ? (intent) => onPermissionDecision(message.id, intent) : undefined} />
  </div> : null;
  return <MessageTurn turn={turn}
    mark={assistant ? <AvaMark /> : undefined}
    activity={tools || permission ? <>{tools}{permission}</> : undefined}
    notice={assistant?.lifecycle === 'failed' ? <RunNotice view={{ state: 'error', code: 'run_failed', message: assistant.note }} onRetry={onRetry} onAskRetry={onAskRetry} /> : undefined} />;
});

export function Conversation({ chat, preferences, active = true, onDisplayChange, onRetryRun, onAskRetry, onPermissionDecision }: {
  chat: Chat; preferences?: ConversationPreferences; active?: boolean;
  onDisplayChange?: (display: ToolDisplay, control: HTMLSelectElement) => void; onRetryRun?: () => void;
  // Drafts a retry request in the composer: the next step for a failure that has no Retry.
  onAskRetry?: () => void;
  onPermissionDecision?: (messageId: string, decision: PermissionDecision) => void;
}) {
  const lastMessage = chat.messages.at(-1);
  const supplied = lastMessage?.role === 'assistant' && lastMessage.lifecycle ? lastMessage : null;
  const following = useBottomFollow(supplied, active, Boolean(supplied));
  const transcriptRef = following.ref;
  const hold = following.hold;
  const display = preferences?.toolDisplay ?? 'compact';
  // A stopped or failed chat ends its last turn too; its unfinished tools are presented as such.
  const lastAssistant = chat.messages.findLast((message) => message.role === 'assistant');
  const ended: TurnState | undefined = chat.status === 'stopped' ? 'canceled' : chat.status === 'failed' ? 'failed' : undefined;
  const toolLifecycle = (message: Message) => message.role === 'assistant' ? message.lifecycle ?? (message === lastAssistant ? ended : undefined) : undefined;
  const groupId = (id: string) => JSON.stringify([chat.id, id]);
  const groups = chat.messages.flatMap((message) => message.role === 'assistant' && message.activity?.length ? [{ id: groupId(message.id), message }] : []);
  const groupIds = [...new Set(groups.map(({ id }) => id))];
  const [overrides, setOverrides] = useState<Record<string, Choice>>({});
  const [preset, setPreset] = useState({ display, revision: 0 });
  // A different preset is fresh intent, not a new turn/group/row identity.
  if (preset.display !== display) { hold(); setPreset({ display, revision: preset.revision + 1 }); setOverrides({}); }
  else {
    const retained = Object.fromEntries(Object.entries(overrides).filter(([id]) => groupIds.includes(id)));
    if (Object.keys(retained).length !== Object.keys(overrides).length) setOverrides(retained);
  }
  // The preset control names what is showing: once an override shows something other than the
  // preset would for any of its rows, it says Custom.
  const customized = groups.some(({ id, message }) => overrides[id] !== undefined && !message.activity!.every((record) => toolDetailsOpen(display, record.state, toolLifecycle(message)) === overrides[id].open));
  const revision = useRef(0);
  const latest = useRef({ groupIds, active, customized, chat, onDisplayChange, onAskRetry, onPermissionDecision });
  latest.current = { groupIds, active, customized, chat, onDisplayChange, onAskRetry, onPermissionDecision };
  const showTools = useCallback((open: boolean, id?: string) => {
    const current = latest.current;
    if (!current.active || !canFocus(transcriptRef.current) || id !== undefined && !current.groupIds.includes(id)) return;
    hold();
    const choice = { open, revision: ++revision.current };
    setOverrides((values) => id === undefined ? Object.fromEntries(current.groupIds.map((key) => [key, choice])) : { ...values, [id]: choice });
  }, []);
  // Choosing a preset, even the current one, clears every override so the tools match it again.
  const changeDisplay = useCallback((next: ToolDisplay, control: HTMLSelectElement) => {
    const current = latest.current;
    if (!current.active || !current.onDisplayChange) return;
    if (current.customized) { hold(); setOverrides({}); setPreset((value) => ({ display: value.display, revision: value.revision + 1 })); }
    current.onDisplayChange(next, control);
  }, []);
  const askRetry = useCallback(() => {
    const current = latest.current;
    if (current.active && canFocus(transcriptRef.current)) current.onAskRetry?.();
  }, []);
  // Only a decision for the request this message still shows, unanswered, reaches the host.
  const decidePermission = useCallback((messageId: string, intent: PermissionIntent) => {
    const current = latest.current;
    if (!current.active || !canFocus(transcriptRef.current) || !current.onPermissionDecision) return;
    const message = current.chat.messages.find((entry) => entry.id === messageId);
    const request = message?.role === 'assistant' && message.permissionDecision === undefined ? message.permission : undefined;
    if (!request || !validPermission(request) || request.state !== 'ready' || intent.id !== request.id || intent.version !== request.version || !request.decisions.includes(intent.decision)) return;
    current.onPermissionDecision(messageId, intent.decision);
  }, []);
  const allExpanded = groups.length > 0 && groups.every(({ id, message }) => overrides[id]?.open ?? message.activity!.every((record) => toolDetailsOpen(display, record.state, toolLifecycle(message))));
  const lastLocalMessageId = lastMessage?.role === 'user' && lastMessage.origin === 'local' ? lastMessage.id : null;
  const liveTurn = lastMessage?.role === 'assistant' && lastMessage.lifecycle !== undefined && lastMessage.lifecycle !== 'committed';
  const asksPermission = chat.messages.some((message) => message.role === 'assistant' && message.permission);
  const chatRun = Object.hasOwn(chatRuns, chat.status) ? chatRuns[chat.status] : undefined;
  // While the run is still going, AVA may recover a failed step itself; asking comes after.
  const runningTurn = chat.status === 'working' || lastAssistant?.role === 'assistant' && lastAssistant.lifecycle === 'streaming' ? lastAssistant : undefined;

  useEffect(() => {
    if (lastLocalMessageId && active && preferences?.scrollAfterSend !== false && canFocus(transcriptRef.current)) transcriptRef.current.scrollTop = transcriptRef.current.scrollHeight;
  }, [lastLocalMessageId]);

  return <div className={styles.transcriptFrame}>
    {groupIds.length ? <div className={styles.toolActions}>
      {onDisplayChange ? <label className={styles.displayLabel}>Tool details<ToolDisplaySelect className={styles.displaySelect} display={display} showCompletedOutput={preferences?.showCompletedOutput} custom={customized} disabled={!active} onChange={changeDisplay} /></label> : null}
      <button data-tool-toggle className={styles.toolsToggle} type="button" onClick={() => showTools(!allExpanded)}>{allExpanded ? 'Collapse all tools' : 'Expand all tools'}</button>
    </div> : null}
    <div ref={transcriptRef} className={styles.transcript} data-empty={chat.messages.length === 0} role="region" tabIndex={0} aria-label={`Conversation: ${chat.title}`}
      onPointerDown={following.onPointerDown} onWheel={following.onWheel} onKeyDown={following.onKeyDown} onBlur={following.onBlur} onScroll={(event) => { if (event.target === event.currentTarget) following.onScroll(); }}>
      <div className={styles.messages}>
        {chat.messages.length === 0 ? <div className={styles.empty} role="img" aria-label="AVA"><AvaMark /></div> : null}
        {chat.messages.map((message) => { const group = groupId(message.id); return <ConversationTurn key={message.id} message={message} group={group} open={overrides[group]?.open} intent={`${group}:${preset.revision}:${overrides[group]?.revision ?? 0}`} display={display} active={active} showCompletedOutput={preferences?.showCompletedOutput} toolLifecycle={toolLifecycle(message)} onTools={showTools} onDisclosure={hold}
          onRetry={message === lastMessage && message.role === 'assistant' && message.lifecycle === 'failed' ? onRetryRun : undefined}
          onAskRetry={onAskRetry && message !== runningTurn ? askRetry : undefined} onPermissionDecision={onPermissionDecision ? decidePermission : undefined} />; })}
        {chat.messages.length > 0 && !liveTurn && chat.status !== 'idle' && !(chat.status === 'approval' && asksPermission) ? <div className={styles.runState}>
          {chatRun ? <RunNotice view={chatRun} onAskRetry={onAskRetry ? askRetry : undefined} /> : <ChatStatus status={chat.status} label={chat.status === 'approval' ? 'Needs your decision' : undefined} />}
        </div> : null}
      </div>
    </div>
    {following.away && supplied ? <button ref={following.latestRef} type="button" className={styles.latest} onClick={following.latest}>Latest</button> : null}
  </div>;
}
