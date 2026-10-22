import { Fragment, useLayoutEffect, useRef, useState, type RefObject } from 'react';
import type { ContextUsageView } from '../chat/contextUsage';
import type { ModelId, Project } from '../state/types';
import { canFocus, DialogFrame } from '../ui/DialogFrame';
import { BackButton, Button } from '../ui/Button';
import { ChatActionDialog, RenameDialog, validRename, validWorkspaceIntent, WorkspaceDialog } from './ChatDialogs';
import type { ActionView, ChatActionIntent, ChatSummary, ConnectIntent, ConnectView, FinderItem, FinderScope, FinderView, MainScreenRequest, ModelPickerView, ModelScope, ModelSelection, Preferences, SettingsSection, UICommand, WorkspaceIntent, WorkspaceView } from './contracts';
import { FinderDialog, validFinderIntent } from './FinderDialog';
import { ModelDialog, validModelSelection } from './ModelDialog';
import { changedPreferenceAreas } from './preferenceDiff';
import { connectStart, ProviderConnectDialog, ProviderDirectory, validConnectIntent } from './ProviderViews';
import { setProviderState, useProviderList } from './providerState';
import { defaultPreferences, modelChoices, resourceInspections } from './screenExamples';
import { Confirmation } from './ScreenPrimitives';
import { SettingsScreen } from './SettingsScreen';
import { shortcutActions, shortcutHint, type ShortcutAction } from './shortcuts';
import { useSubagentWorkspace } from './subagentWorkspace';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';

export interface MainScreensProps {
  request: MainScreenRequest;
  projects?: readonly Project[];
  contextUsage?: ContextUsageView;
  parent: Readonly<{ title: string; model: ModelId }>;
  defaultModel: ModelId;
  preferences: Preferences;
  opaque: boolean;
  onOpaqueChange: (opaque: boolean) => void;
  section: SettingsSection;
  onSectionChange: (section: SettingsSection) => void;
  onPreferencesChange: (preferences: Preferences) => void;
  onSelectModel: (intent: ModelSelection) => void;
  onDismiss: () => void;
  returnFocus: RefObject<HTMLElement | null>;
  workspaceSeed?: unknown;
  onWorkspaceSnapshot?: (snapshot: unknown) => void;
  // Every chat, archived included; `current` marks the active parent chat.
  chats?: readonly ChatSummary[];
  // Terminal callbacks run while this request is current; MainScreens then calls onDismiss.
  onOpenChat?: (chatId: string) => void;
  onRenameChat?: (chatId: string, title: string) => void;
  onArchiveChat?: (chatId: string) => void;
  // Starts a new chat from the chat's messages up to and including `messageId`, and opens it.
  onBranchChat?: (chatId: string, messageId: string) => void;
  onAppAction?: (action: ShortcutAction) => void;
  onOpenSettingsSection?: (section: SettingsSection) => void;
}
type ChatTask = 'rename' | 'archive' | 'branch';
type Overlay =
  | { kind: 'models'; scope: ModelScope; version: number }
  | { kind: 'provider'; view: ConnectView; back: boolean }
  | { kind: 'providers' | 'reset' | 'commands' | 'workspace'; version: number }
  // Find a chat, optionally opened on one filter (Archived, from the archive dialog).
  | { kind: 'sessions'; version: number; scope?: FinderScope }
  | { kind: ChatTask; chatId: string; version: number };
type Receipt = Readonly<{ text: string; undo?: Readonly<{ preferences: Preferences; opaque: boolean; defaultModel: ModelId; reset: Preferences }> }>;

const readOnlyReason = 'You’re viewing a subagent. Go back to the parent chat to change its model.';
// Run by the shell after this screen closes; everything else opens inside MainScreens.
const hostCommands: Partial<Record<UICommand, true>> = { settings: true, newChat: true, composer: true, tools: true, stop: true, agents: true, changes: true, context: true, inspector: true };
const parentCommands: Partial<Record<UICommand, true>> = { models: true, composer: true, tools: true, stop: true };
const paletteOrder: readonly UICommand[] = ['sessions', 'newChat', 'workspace', 'settings', 'shortcuts', 'providers', 'models', 'composer', 'tools', 'stop', 'rename', 'branch', 'archive', 'agents', 'changes', 'context', 'inspector'];
const paletteOnly: Readonly<Record<'providers' | 'workspace' | 'shortcuts' | ChatTask, Readonly<{ label: string; detail: string; group: string }>>> = {
  providers: { label: 'Connect a provider', detail: 'Sign in to a model provider', group: 'General' },
  workspace: { label: 'Open a project', detail: 'Go to a chat in another project', group: 'General' },
  shortcuts: { label: 'Keyboard shortcuts', detail: 'See and change every shortcut', group: 'General' },
  rename: { label: 'Rename chat', detail: '', group: 'Chat' },
  branch: { label: 'Branch from a message', detail: 'Start a new chat from an earlier message', group: 'Chat' },
  archive: { label: 'Archive chat', detail: '', group: 'Chat' },
};

function initialOverlay(request: MainScreenRequest): Overlay | null {
  const chatId = request.targetChatId ?? request.parentChatId;
  if (request.kind === 'models') return { kind: 'models', scope: 'current-chat', version: 1 };
  if (request.kind === 'providers' || request.kind === 'commands' || request.kind === 'sessions' || request.kind === 'workspace') return { kind: request.kind, version: 1 };
  if (request.kind === 'rename' || request.kind === 'archive' || request.kind === 'branch') return { kind: request.kind, chatId, version: 1 };
  return null;
}

// A lazy, local presentation host. No ScreenReview controller, routing or RPC. Provider
// connections live in the visit-wide store, so they outlast this host.
export default function MainScreens({ request, parent, defaultModel, preferences, opaque, onOpaqueChange, section, onSectionChange, onPreferencesChange, onSelectModel, onDismiss, returnFocus, workspaceSeed, onWorkspaceSnapshot, projects, contextUsage, chats, onOpenChat, onRenameChat, onArchiveChat, onBranchChat, onAppAction, onOpenSettingsSection }: MainScreensProps) {
  const [settingsQuery, setSettingsQuery] = useState('');
  const [query, setQuery] = useState('');
  const [receipt, setReceipt] = useState<Receipt | null>(null);
  const providerList = useProviderList();
  const [overlay, setOverlay] = useState<Overlay | null>(() => initialOverlay(request));
  // Read once when the screen opens over the chat: the palette only offers a tools toggle that exists.
  const [toolsAvailable] = useState(() => canFocus(document.querySelector<HTMLElement>('[data-tool-toggle]')));
  const revision = useRef(1);
  const composing = useRef(false);
  // Some closes remove their own opener: reset disables it, Undo removes itself and a connected
  // provider swaps Connect for Disconnect. Focus then lands on what replaced the opener.
  const landing = useRef<{ kind: 'receipt' } | { kind: 'provider'; id: string } | null>(null);
  const root = useRef<HTMLDivElement>(null);
  const receiptBox = useRef<HTMLDivElement>(null);
  const undoButton = useRef<HTMLButtonElement>(null);
  const rootGate = useViewGuard(JSON.stringify(request));
  const overlayGate = useViewGuard(JSON.stringify(overlay));
  const readOnly = request.inspectedChildId !== null;
  const settings = request.kind === 'settings';
  const parentChat = chats?.find((chat) => chat.id === request.parentChatId);
  const definitions = useSubagentWorkspace(workspaceSeed, settings && section === 'subagents' && !overlay, (snapshot) => { if (rootGate.valid()) onWorkspaceSnapshot?.(snapshot); });
  const undo = receipt?.undo && preferences === receipt.undo.reset && !opaque && defaultModel === 'aster' ? receipt.undo : null;
  useLayoutEffect(() => {
    const next = landing.current;
    if (!next) return;
    landing.current = null;
    if (next.kind === 'receipt') {
      const target = canFocus(undoButton.current) ? undoButton.current : receiptBox.current;
      if (canFocus(target)) target.focus({ preventScroll: true });
      return;
    }
    if (document.activeElement instanceof HTMLElement && document.activeElement !== document.body && canFocus(document.activeElement)) return;
    const row = Array.from(root.current?.querySelectorAll<HTMLElement>('[data-provider-action]') ?? []).find((node) => node.dataset.providerAction === next.id) ?? null;
    if (canFocus(row)) row.focus({ preventScroll: true });
  });

  function dismissRoot() {
    if (!rootGate.valid()) return;
    rootGate.invalidate(); overlayGate.invalidate(); onDismiss();
  }
  // Dialog requests close with their last overlay; on the Settings page only the overlay closes.
  function finishOverlay() {
    overlayGate.invalidate();
    if (overlay?.kind === 'provider' && overlay.back) switchTo({ kind: 'providers', version: ++revision.current });
    else if (settings) {
      if (overlay?.kind === 'provider') landing.current = { kind: 'provider', id: overlay.view.provider.id };
      setOverlay(null);
    } else dismissRoot();
  }
  function closeOverlay() { if (rootGate.valid() && overlayGate.valid()) finishOverlay(); }
  // Callers have already claimed the current overlay.
  function switchTo(next: Overlay) {
    overlayGate.invalidate(); setQuery(''); setOverlay(next);
  }
  function open(next: Overlay) {
    if (!rootGate.valid() || overlay || !overlayGate.claim()) return;
    setReceipt(null); switchTo(next);
  }
  function chooseModel(scope: ModelScope) {
    if (scope === 'current-chat' && readOnly) return;
    open({ kind: 'models', scope, version: ++revision.current });
  }
  function providerOverlay(id: string, disconnect: boolean, back: boolean): Overlay | null {
    const provider = providerList.find((entry) => entry.id === id);
    if (!provider || (disconnect ? provider.state !== 'connected-example' || provider.source !== 'stored-example' : provider.state === 'external' || !provider.methods.length)) return null;
    const start = disconnect ? { step: 'disconnect' as const, method: null } : connectStart(provider);
    return { kind: 'provider', back, view: { id: `${request.id}:provider`, version: ++revision.current, provider, ...start } };
  }
  function showProvider(id: string, disconnect = false) {
    const next = providerOverlay(id, disconnect, false);
    if (next) open(next);
  }
  function runCommand(command: UICommand) {
    if (hostCommands[command]) {
      onAppAction?.(command as ShortcutAction);
      dismissRoot();
    } else if (command === 'shortcuts') {
      onOpenSettingsSection?.('shortcuts');
      dismissRoot();
    } else if (command === 'models') switchTo({ kind: 'models', scope: 'current-chat', version: ++revision.current });
    else if (command === 'sessions' || command === 'providers' || command === 'workspace') switchTo({ kind: command, version: ++revision.current });
    else if (command === 'rename' || command === 'archive' || command === 'branch') switchTo({ kind: command, chatId: request.parentChatId, version: ++revision.current });
  }

  function paletteItems(): FinderItem[] {
    const offered = (command: UICommand) => hostCommands[command] ? Boolean(onAppAction)
      : command === 'sessions' || command === 'workspace' ? Boolean(chats && onOpenChat)
      : command === 'shortcuts' ? Boolean(onOpenSettingsSection)
      : command === 'rename' ? Boolean(parentChat && onRenameChat)
      : command === 'archive' ? Boolean(parentChat && onArchiveChat)
      : command === 'branch' ? Boolean(parentChat && onBranchChat) : true;
    const reason = (command: UICommand) => readOnly && parentCommands[command] ? 'Parent chat only'
      : command === 'stop' && parentChat?.status !== 'working' ? 'Nothing running'
      : command === 'tools' && !toolsAvailable ? 'No tool activity'
      : command === 'archive' && parentChat?.archived ? 'Already archived'
      : command === 'branch' && !parentChat?.branchPoints.length ? 'No messages yet' : undefined;
    return paletteOrder.filter(offered).map((command) => {
      const action = shortcutActions.find((entry) => entry.id === command);
      const copy = action ?? paletteOnly[command as keyof typeof paletteOnly];
      const detail = command === 'rename' || command === 'archive' ? parentChat?.title ?? '' : copy.detail;
      return { id: command, command, label: copy.label, detail, group: copy.group, shortcut: action ? shortcutHint(preferences.shortcuts, action.id)?.label : undefined, disabledReason: reason(command) };
    });
  }
  function chatProjects() {
    const list = (projects ?? []).map((project) => ({ id: project.id, label: project.name }));
    for (const chat of chats ?? []) if (!list.some((project) => project.id === chat.projectId)) list.push({ id: chat.projectId, label: chat.projectLabel });
    return list;
  }

  function renderOverlay() {
    if (!overlay) return null;
    const chrome = { onDismiss: closeOverlay, returnFocus };
    const key = overlay.kind === 'provider' ? overlay.kind : `${overlay.kind}:${overlay.version}`;
    if (overlay.kind === 'models') {
      const blocked = readOnly && overlay.scope === 'current-chat';
      const view: ModelPickerView = { id: `${request.id}:models`, version: overlay.version, scope: overlay.scope, state: blocked ? 'disabled' : 'ready', currentId: parent.model, defaultId: defaultModel, choices: modelChoices, reasoningEditable: false };
      return <ModelDialog key={key} {...chrome} view={view} chatTitle={parent.title} query={query} onQueryChange={(value) => { if (overlayGate.valid()) setQuery(value); }} onRetry={() => {}}
        description={blocked ? readOnlyReason : undefined}
        onConnectProvider={(providerId) => {
          if (!rootGate.valid() || !overlayGate.valid()) return;
          const next = providerId ? providerOverlay(providerId, false, false) : { kind: 'providers' as const, version: ++revision.current };
          if (next && overlayGate.claim()) switchTo(next);
        }}
        onSelect={(intent) => {
          if (!rootGate.valid() || !validModelSelection(view, intent) || !overlayGate.claim()) return;
          onSelectModel({ ...intent, id: request.id, version: request.version });
          finishOverlay();
        }} />;
    }
    if (overlay.kind === 'provider') {
      const { view, back } = overlay;
      function connect(intent: ConnectIntent) {
        if (!rootGate.valid() || !validConnectIntent(view, intent) || !overlayGate.claim()) return;
        const { provider } = view;
        if (intent.action === 'disconnect') {
          setProviderState(provider.id, { state: 'available', source: 'none' });
          setReceipt({ text: `${provider.label} disconnected.` });
          finishOverlay(); return;
        }
        if (intent.action === 'choose-model') {
          // Open the picker on this provider's models when it has any.
          const scope: ModelScope = settings || readOnly ? 'new-chat-default' : 'current-chat';
          switchTo({ kind: 'models', scope, version: ++revision.current });
          if (modelChoices.some((choice) => choice.providerId === provider.id)) setQuery(provider.label);
          return;
        }
        let { step, method } = view;
        let next = provider;
        if (intent.action === 'choose-method') { method = intent.method; step = method; }
        else if (intent.action === 'submit-demo-key' || intent.action === 'show-waiting') step = 'waiting';
        else if (intent.action === 'show-success') { step = 'success'; next = { ...provider, state: 'connected-example', source: 'stored-example' }; setProviderState(provider.id, next); }
        else if (intent.action === 'show-error') step = 'error';
        else if (intent.action === 'retry') step = method ?? 'method';
        else if (intent.action === 'back') step = step === 'waiting' ? method ?? 'method' : 'method';
        overlayGate.invalidate();
        setOverlay({ kind: 'provider', back, view: { ...view, provider: next, step, method, version: ++revision.current } });
      }
      return <ProviderConnectDialog key={key} {...chrome} view={view} onIntent={connect} />;
    }
    if (overlay.kind === 'providers') {
      const pick = (id: string, disconnect: boolean) => {
        if (!rootGate.valid() || !overlayGate.valid()) return;
        const next = providerOverlay(id, disconnect, true);
        if (next && overlayGate.claim()) switchTo(next);
      };
      return <DialogFrame key={key} {...chrome} wide title="Connect a provider" description="Connect a provider to use its models.">
        <ProviderDirectory active providers={providerList} onConnect={(id) => pick(id, false)} onDisconnect={(id) => pick(id, true)} />
      </DialogFrame>;
    }
    if (overlay.kind === 'commands' || overlay.kind === 'sessions') {
      const view: FinderView = overlay.kind === 'commands'
        ? { id: `${request.id}:commands`, version: overlay.version, kind: 'commands', state: 'ready', items: paletteItems(), projects: [] }
        : { id: `${request.id}:sessions`, version: overlay.version, kind: 'sessions', state: chats ? 'ready' : 'unavailable', projects: chatProjects(),
          items: (chats ?? []).map((chat) => ({ id: chat.id, label: chat.title, detail: '', projectId: chat.projectId, status: chat.status, archived: chat.archived, current: chat.current, searchText: chat.searchText })) };
      return <FinderDialog key={key} {...chrome} view={view} initialScope={overlay.kind === 'sessions' ? overlay.scope : undefined} query={query} onQueryChange={(value) => { if (overlayGate.valid()) setQuery(value); }} onRetry={() => {}} onSelect={(intent) => {
        if (!rootGate.valid() || !validFinderIntent(view, intent) || !overlayGate.claim()) return;
        const item = view.items.find((entry) => entry.id === intent.itemId)!;
        if (view.kind === 'commands') { runCommand(item.command!); return; }
        if (!item.current || item.archived) onOpenChat?.(item.id);
        dismissRoot();
      }} />;
    }
    if (overlay.kind === 'rename' || overlay.kind === 'archive' || overlay.kind === 'branch') {
      const { chatId, kind: task } = overlay;
      const chat = chats?.find((entry) => entry.id === chatId);
      const handled = task === 'rename' ? onRenameChat : task === 'archive' ? onArchiveChat : onBranchChat;
      const state = !chat || !handled ? 'unavailable' : task === 'archive' && chat.status === 'working' ? 'busy' : task === 'branch' && !chat.branchPoints.length ? 'empty' : 'ready';
      const view: ActionView = { id: `${request.id}:${task}:${chatId}`, version: overlay.version, state, title: chat?.title ?? '' };
      if (task === 'rename') return <RenameDialog key={key} {...chrome} view={view} onRename={(intent) => {
        if (!rootGate.valid() || !validRename(view, intent) || !overlayGate.claim()) return;
        onRenameChat?.(chatId, intent.title);
        finishOverlay();
      }} />;
      const points = chat?.branchPoints ?? [];
      // Archived chats live in Find a chat, so the archive dialog can open it on Archived in its place.
      return <ChatActionDialog key={key} {...chrome} action={task} view={view} points={points}
        onGoToChat={chat && onOpenChat ? () => { if (rootGate.valid() && overlayGate.claim()) { onOpenChat(chatId); dismissRoot(); } } : undefined}
        onFindArchived={chats && onOpenChat ? () => { if (rootGate.valid() && overlayGate.claim()) switchTo({ kind: 'sessions', scope: 'archived', version: ++revision.current }); } : undefined}
        findShortcut={shortcutHint(preferences.shortcuts, 'sessions')?.label}
        onIntent={(intent: ChatActionIntent) => {
          const point = points.find((entry) => entry.id === intent.pointId);
          if (!rootGate.valid() || state !== 'ready' || intent.id !== view.id || intent.version !== view.version || intent.action !== task || intent.action === 'branch' && !point || !overlayGate.claim()) return;
          if (intent.action === 'archive') onArchiveChat?.(chatId);
          else onBranchChat?.(chatId, point!.id);
          finishOverlay();
        }} />;
    }
    if (overlay.kind === 'workspace') {
      const list = chats ?? [];
      const currentProject = parentChat?.projectId ?? '';
      const choices = chats && onOpenChat ? chatProjects().filter((project) => list.some((chat) => chat.projectId === project.id)).map((project) => {
        const active = list.filter((chat) => chat.projectId === project.id && !chat.archived).length;
        return { id: project.id, label: project.label, detail: active ? `${active} ${active === 1 ? 'chat' : 'chats'}` : 'Only archived chats' };
      }) : [];
      const view: WorkspaceView = { id: `${request.id}:workspace`, version: overlay.version, state: 'choice', selectedId: currentProject, choices };
      return <WorkspaceDialog key={key} {...chrome} view={view} onIntent={(intent: WorkspaceIntent) => {
        if (!rootGate.valid() || !validWorkspaceIntent(view, intent) || !overlayGate.claim()) return;
        // Choosing opens the project straight away, on its latest active chat (else its latest
        // archived one). The project already open needs nothing, so choosing it only closes.
        if (intent.workspaceId !== currentProject) {
          const inProject = list.filter((chat) => chat.projectId === intent.workspaceId);
          const target = inProject.find((chat) => !chat.archived) ?? inProject[0];
          if (target) onOpenChat?.(target.id);
        }
        finishOverlay();
      }} />;
    }
    const areas = changedPreferenceAreas(preferences, opaque, defaultModel);
    return <DialogFrame key={key} {...chrome} title="Reset preferences?" subtitle={areas.length ? 'These go back to their defaults.' : 'Everything already uses its default.'}>
      <Confirmation tone="destructive" label="Reset preferences" disabled={!areas.length} onCancel={closeOverlay} onConfirm={() => {
        if (!rootGate.valid() || !areas.length || !overlayGate.claim()) return;
        const previous = { preferences, opaque, defaultModel, reset: defaultPreferences };
        onPreferencesChange(defaultPreferences);
        onOpaqueChange(false);
        onSelectModel({ id: request.id, version: request.version, scope: 'new-chat-default', modelId: 'aster', reasoning: null });
        setReceipt({ text: 'Preferences reset.', undo: previous });
        landing.current = { kind: 'receipt' };
        finishOverlay();
      }}>
        {areas.length ? <dl className={styles.facts}>{areas.map((entry) => <Fragment key={entry.area}><dt>{entry.area}</dt><dd>{entry.detail}</dd></Fragment>)}</dl> : null}
        <p className={styles.muted}>Chats, drafts, messages, subagents and provider connections stay as they are.</p>
      </Confirmation>
    </DialogFrame>;
  }

  return <div ref={root} className={styles.mainScreens} data-page={request.kind} data-density={preferences.density} data-motion={preferences.motion} onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }} onKeyDown={(event) => {
    if (event.defaultPrevented || event.key !== 'Escape' || !settings || overlay || !rootGate.valid() || event.target instanceof Element && event.target.closest('dialog')) return;
    event.preventDefault(); event.stopPropagation();
    if (composing.current || event.nativeEvent.isComposing || event.keyCode === 229) return;
    if (settingsQuery) setSettingsQuery(''); else dismissRoot();
  }}>
    {settings ? <div className={styles.settingsPage}>
      <header className={styles.settingsHeader}><h1>Settings</h1><BackButton onClick={() => { if (rootGate.valid() && !overlay) dismissRoot(); }}>Back to chat</BackButton></header>
      {receipt ? <div ref={receiptBox} className={`${styles.receipt} ${styles.inline}`} tabIndex={-1}><span role="status">{undo ? 'Preferences reset' : receipt.text}</span>{undo ? <Button ref={undoButton} onClick={() => {
        if (!rootGate.valid() || overlay) return;
        onPreferencesChange(undo.preferences);
        onOpaqueChange(undo.opaque);
        if (undo.defaultModel !== 'aster') onSelectModel({ id: request.id, version: request.version, scope: 'new-chat-default', modelId: undo.defaultModel, reasoning: null });
        setReceipt({ text: 'Preferences restored.' });
        landing.current = { kind: 'receipt' };
      }}>Undo</Button> : null}</div> : null}
      <SettingsScreen embedded active={!overlay} section={section} onSectionChange={(value) => { if (rootGate.valid() && !overlay) onSectionChange(value); }} query={settingsQuery} onQueryChange={(value) => { if (rootGate.valid() && !overlay) setSettingsQuery(value); }}
        opaque={opaque} onOpaqueChange={(value) => { if (rootGate.valid() && !overlay && overlayGate.valid()) onOpaqueChange(value); }}
        preferences={preferences} onPreferencesChange={(value) => { if (rootGate.valid() && !overlay && overlayGate.valid()) onPreferencesChange(value); }}
        defaultModel={modelChoices.find((choice) => choice.id === defaultModel)?.label ?? 'Unknown'} defaultModelId={defaultModel}
        projects={projects} contextUsage={contextUsage} project={parentChat ? { name: parentChat.projectLabel, trusted: false } : undefined}
        onChooseModel={chooseModel} providers={providerList} onConnect={showProvider} onDisconnect={(id) => showProvider(id, true)}
        onReset={() => open({ kind: 'reset', version: ++revision.current })} onBack={() => { if (rootGate.valid() && !overlay) dismissRoot(); }}
        rules={{ state: 'ready', rows: [
          { title: parentChat ? `Read files in ${parentChat.projectLabel}` : 'Read project files', detail: 'Saved rule · applies to every session.', status: 'Allowed' },
          { title: 'Run tests', detail: 'Granted for this session only.', status: 'This session' },
        ] }}
        inspections={resourceInspections} subagents={definitions.workspace} onSaveSubagent={(intent) => rootGate.valid() && definitions.save(intent)} />
    </div> : null}
    {renderOverlay()}
  </div>;
}
