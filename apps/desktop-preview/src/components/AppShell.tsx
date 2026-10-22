import { Component, lazy, Suspense, useLayoutEffect, useReducer, useRef, useState, type MouseEvent, type ReactNode } from 'react';
import { ArchiveIcon } from '@phosphor-icons/react/dist/csr/Archive';
import { ArrowCounterClockwiseIcon } from '@phosphor-icons/react/dist/csr/ArrowCounterClockwise';
import { CircleNotchIcon } from '@phosphor-icons/react/dist/csr/CircleNotch';
import { PlayIcon } from '@phosphor-icons/react/dist/csr/Play';
import { StopIcon } from '@phosphor-icons/react/dist/csr/Stop';
import { XIcon } from '@phosphor-icons/react/dist/csr/X';
import { previewContextUsage } from './contextPreview';
import { createInitialState, modelOptions } from '../state/fixtures';
import type { InspectorModule, ModelId, PreviewAction, PreviewState } from '../state/types';
import type { MainScreenRequest, ModelSelection, Preferences, SettingsSection } from '../screens/contracts';
import { appChord, type ShortcutAction } from '../screens/shortcutBindings';
import { canFocus, DialogFrame } from '../ui/DialogFrame';
import { BackButton, IconButton } from '../ui/Button';
import { previewReducer } from '../state/reducer';
import { sameRun } from '../state/suppliedRun';
import { chatChanges } from '../state/changeLedger';
import { ChangeNavigationContext } from '../chat/changeNavigation';
import { prepareAssistantContent } from '../chat/widgets/AssistantContentHost';
import type { RunDriver } from '../chat/suppliedRunDriver';
import type { PreviewExample } from '../chat/suppliedRunFixture';
import type * as SuppliedRunPreviewModule from './SuppliedRunPreview';
import { chatSummaries, inspectedChild, selectedParentChildren } from '../state/selectors';
import { Presence } from '../ui/Presence';
import { ReducedMotionContext } from '../ui/useReducedMotion';
import appearance from '../styles/appearance.module.css';
import { AgentList } from './AgentList';
import { ChatStatus } from './ChatStatus';
import { Composer } from './Composer';
import { Conversation } from './Conversation';
import { defaultConversation } from '../screens/behaviorPreferences';
import { isToolDisplay, type ToolDisplay } from '../chat/toolDisplay';
import { ConversationIdentity } from './ConversationIdentity';
import { Inspector } from './Inspector';
import { InspectorRail } from './InspectorRail';
import { PreviewBanner, PreviewChip } from './PreviewBanner';
import { ProjectSidebar } from './ProjectSidebar';
import banner from './PreviewBanner.module.css';
import styles from './AppShell.module.css';

const MainScreens = lazy(() => import('../screens/MainScreens'));
const DetailInspector = lazy(() => import('./DetailInspector'));
const ChildConversation = lazy(() => import('./ChildConversation'));

// Below 1280px a docked inspector squeezes the transcript: initial load and new chats start with it closed.
// A one-off read, not a placement subscription; CSS alone decides docked versus sheet.
const narrowWindowQuery = '(max-width: 1279.98px)';
function narrowWindow() {
  return typeof window.matchMedia === 'function' && window.matchMedia(narrowWindowQuery).matches;
}
function initialState(): PreviewState {
  const state = createInitialState();
  return narrowWindow() ? { ...state, inspector: null } : state;
}

// Only the optional read-only body is deferred. Identity, Back and model gates stay here.
class ChildContentBoundary extends Component<{ children: ReactNode }, { failed: boolean }> {
  state = { failed: false };
  static getDerivedStateFromError() { return { failed: true }; }
  render() {
    return this.state.failed ? <p className={styles.childNotice} role="alert">Child conversation could not load. Use Back to parent; its draft is unchanged.</p> : this.props.children;
  }
}

type ChangeFocus = Readonly<{ chatId: string; path: string | null; pending: string | null }>;

export function AppShell() {
  const [state, reduce] = useReducer(previewReducer, undefined, initialState);
  const [previewFeature, setPreviewFeature] = useState<typeof SuppliedRunPreviewModule | null>(null);
  const [previewLoading, setPreviewLoading] = useState(false);
  const [previewNotice, setPreviewNotice] = useState('');
  const [previewExample, setPreviewExample] = useState<PreviewExample>('completion');
  const [previewPaused, setPreviewPaused] = useState(false);
  const [bannerOpen, setBannerOpen] = useState(true);
  const [sidebarCollapsed, setSidebarCollapsed] = useState(false);
  // Remembered for the visit, across modules and closing: a wider reading width for diffs and raw context.
  const [inspectorWide, setInspectorWide] = useState(false);
  const [changeFocus, setChangeFocus] = useState<ChangeFocus>({ chatId: '', path: null, pending: null });
  const previewDriver = useRef<RunDriver | null>(null);
  const previewVersion = useRef(0);
  const [screenRequest, setScreenRequest] = useState<MainScreenRequest | null>(null);
  const [preferences, setPreferences] = useState<Preferences>({ density: 'comfortable', motion: 'system', help: true, enter: 'send', notifications: 'off' });
  const [defaultModel, setDefaultModel] = useState<ModelId>('aster');
  const [settingsSection, setSettingsSection] = useState<SettingsSection>('general');
  const settingsWorkspace = useRef<unknown>(undefined);
  const settingsPageRef = useRef<HTMLElement>(null);
  const settingsFocus = useRef<'entry' | 'return' | null>(null);
  const settingsButtonRef = useRef<HTMLButtonElement>(null);
  const screenVersion = useRef(0);
  const mounted = useRef(false);
  const latest = useRef({ state, preferences, request: screenRequest });
  latest.current = { state, preferences, request: screenRequest };
  const composing = useRef(false);
  const commandPending = useRef(false);
  const [commandNotice, setCommandNotice] = useState('');
  const appRef = useRef<HTMLElement>(null);
  const agentsButtonRef = useRef<HTMLButtonElement>(null);
  const changesButtonRef = useRef<HTMLButtonElement>(null);
  const contextButtonRef = useRef<HTMLButtonElement>(null);
  const inspectorPanelRef = useRef<HTMLElement>(null);
  const titleRef = useRef<HTMLHeadingElement>(null);
  const parentChatRef = useRef<HTMLElement>(null);
  const previewToggleRef = useRef<HTMLButtonElement>(null);
  const bannerDismissRef = useRef<HTMLButtonElement>(null);
  const previewChipRef = useRef<HTMLButtonElement>(null);
  const undoArchiveRef = useRef<HTMLButtonElement>(null);
  // One-shot focus intents, each honored after the commit that makes its target visible.
  const focusTitleAfterCommit = useRef(false);
  // Escape from a child view returns to that child's Agents row when it is still on screen.
  const childRowAfterCommit = useRef<string | null>(null);
  const focusComposerAfterCommit = useRef(false);
  const focusArchiveAfterCommit = useRef(false);
  // 'sheet': only a small-window sheet takes focus (rail, meter). 'panel': shortcuts and commands always move focus in.
  const focusInspectorAfterCommit = useRef<'sheet' | 'panel' | null>(null);
  const bannerFocus = useRef<'chip' | 'banner' | null>(null);
  // Last focused banner control: if a commit removes it (Pause/Next end with the run), focus stays in the banner.
  const bannerControl = useRef<HTMLElement | null>(null);
  const pendingAppAction = useRef<ShortcutAction | null>(null);
  const chat = state.chats.find((entry) => entry.id === state.activeChatId)!;
  const children = selectedParentChildren(state);
  const child = inspectedChild(state);
  const changes = chatChanges(chat);
  const activeCount = children.filter((entry) => entry.status === 'working').length;
  const inspectorModule = state.inspector ?? state.lastInspectorModule;
  const settingsOpen = screenRequest?.kind === 'settings';
  const runningPreview = state.suppliedRun?.chatId === chat.id && state.suppliedRun.state === 'streaming';
  const previewEnded = state.suppliedRun?.chatId === chat.id && ['failed', 'canceled'].includes(state.suppliedRun.state);
  const displayOwner = latest.current;
  // A focus request for Changes belongs to one chat and lives only while Changes is open.
  if ((changeFocus.path !== null || changeFocus.pending !== null) && (state.inspector !== 'changes' || changeFocus.chatId !== chat.id)) setChangeFocus({ chatId: '', path: null, pending: null });
  const reviewChangeOwner = useRef<(path: string) => void>(() => {});
  reviewChangeOwner.current = (path) => {
    const current = latest.current;
    if (!mounted.current || current.request || current.state !== state || current.state.inspectedChildId || typeof path !== 'string' || !path) return;
    // Null first, then the path: a second request for the same file still moves focus to it.
    setChangeFocus({ chatId: chat.id, path: null, pending: path });
    dispatch({ type: 'showInspector', module: 'changes' });
  };
  const [reviewChange] = useState(() => (path: string) => reviewChangeOwner.current(path));

  useLayoutEffect(() => {
    // Only explicit page/Inspect/Back/New/Archive intents request focus, after visibility commits.
    if (settingsFocus.current) {
      const target = settingsFocus.current === 'entry' ? settingsPageRef.current : canFocus(settingsButtonRef.current) ? settingsButtonRef.current : titleRef.current;
      settingsFocus.current = null;
      if (canFocus(target)) target.focus({ preventScroll: true });
    }
    if (focusTitleAfterCommit.current) {
      focusTitleAfterCommit.current = false;
      const rowId = childRowAfterCommit.current;
      childRowAfterCommit.current = null;
      const row = rowId === null ? null : Array.from(inspectorPanelRef.current?.querySelectorAll<HTMLElement>('[data-child-id]') ?? []).find((element) => element.dataset.childId === rowId) ?? null;
      (canFocus(row) ? row : titleRef.current)?.focus();
    }
    if (changeFocus.pending !== null) setChangeFocus({ chatId: changeFocus.chatId, path: changeFocus.pending, pending: null });
    const inspectorIntent = focusInspectorAfterCommit.current;
    if (inspectorIntent) {
      focusInspectorAfterCommit.current = null;
      // A rail click leaves a docked panel's focus on the rail; a sheet over the transcript always takes it.
      const panel = inspectorPanelRef.current;
      if (state.inspector && canFocus(panel) && (inspectorIntent === 'panel' || inspectorSheet())) panel.focus({ preventScroll: true });
    }
    if (bannerFocus.current) {
      const target = bannerFocus.current === 'chip' ? previewChipRef.current : canFocus(previewToggleRef.current) ? previewToggleRef.current : bannerDismissRef.current;
      bannerFocus.current = null;
      if (canFocus(target)) target.focus();
    }
    const lost = bannerControl.current;
    if (lost && !lost.isConnected && (document.activeElement === null || document.activeElement === document.body)) {
      bannerControl.current = null;
      if (canFocus(previewToggleRef.current)) previewToggleRef.current.focus();
    }
    // Composer, Archive and palette intents wait until any page or dialog has closed and returned focus.
    if (latest.current.request) return;
    if (focusComposerAfterCommit.current) {
      focusComposerAfterCommit.current = false;
      const input = parentChatRef.current?.querySelector<HTMLTextAreaElement>('textarea') ?? null;
      if (canFocus(input)) input.focus();
    }
    if (focusArchiveAfterCommit.current) {
      focusArchiveAfterCommit.current = false;
      if (canFocus(undoArchiveRef.current)) undoArchiveRef.current.focus();
    }
    const action = pendingAppAction.current;
    const root = appRef.current;
    if (action && root) {
      pendingAppAction.current = null;
      const snapshot = latest.current.state;
      const current = () => mounted.current && !latest.current.request && latest.current.state === snapshot && canFocus(root);
      void import('./AppCommands').then(({ runAppAction }) => { if (current()) runAppAction(action, appHost(root, current)); }, () => { if (current()) setCommandNotice('Keyboard actions could not load. Use the visible controls or try again.'); });
    }
  });

  useLayoutEffect(() => {
    mounted.current = true;
    // Stored replies need the formatting chunk: request it with the first paint, not after each turn mounts.
    if (latest.current.state.chats.some((entry) => entry.messages.some((message) => message.role === 'assistant'))) void prepareAssistantContent().catch(() => undefined);
    return () => { mounted.current = false; ++previewVersion.current; previewDriver.current?.cancel(); previewDriver.current = null; };
  }, []);
  useLayoutEffect(() => {
    if (screenRequest && (screenRequest.parentChatId !== state.activeChatId || screenRequest.inspectedChildId !== state.inspectedChildId)) {
      latest.current.request = null;
      setScreenRequest(null);
    }
  }, [screenRequest, state.activeChatId, state.inspectedChildId]);
  // Opening a chat that is waiting on you lands on the request, not above it.
  useLayoutEffect(() => {
    if (!child) parentChatRef.current?.querySelector<HTMLElement>('[data-permission-pending]')?.scrollIntoView?.({ block: 'nearest' });
  }, [chat.id, child?.id]);

  function cancelPreviewTransport() {
    ++previewVersion.current;
    previewDriver.current?.cancel(); previewDriver.current = null;
    setPreviewLoading(false); setPreviewPaused(false);
  }
  // Any action that leaves, replaces or stops the viewed run also ends its local delivery.
  function cancelFor(action: PreviewAction) {
    const current = latest.current.state;
    const run = current.suppliedRun;
    const leaves = action.type === 'newChat' || action.type === 'branchChat'
      || action.type === 'viewChat' && action.chatId !== current.activeChatId && current.chats.some((entry) => entry.id === action.chatId && !entry.archived)
      || action.type === 'inspectChild' && current.childAgents.some((entry) => entry.id === action.childId && entry.parentChatId === current.activeChatId)
      || action.type === 'archiveChat' && run?.chatId === action.chatId
      || action.type === 'unarchiveChat' && current.archiveNotice?.chatId === action.chatId && current.archiveNotice.activeChatId !== current.activeChatId;
    const stops = action.type === 'stopViewedMock' || action.type === 'stopSuppliedRun' && run && sameRun(run, action.identity);
    if (leaves || stops) cancelPreviewTransport();
  }
  async function startPreview() {
    const captured = latest.current;
    if (!mounted.current || captured.request || captured.state !== state || captured.state.inspectedChildId || chat.status === 'working' || previewLoading) return;
    const version = ++previewVersion.current;
    setPreviewLoading(true); setPreviewNotice('');
    try {
      const [feature] = await Promise.all([import('./SuppliedRunPreview'), prepareAssistantContent()]);
      if (!mounted.current || previewVersion.current !== version || latest.current.state !== captured.state || latest.current.request) return;
      setPreviewFeature(feature);
      const identity = { chatId: chat.id, runId: crypto.randomUUID(), generation: (state.suppliedRun?.generation ?? 0) + 1 };
      const driver = feature.createRunDriver(identity, feature.suppliedSnapshots(identity, previewExample), (events) => {
        const current = latest.current;
        if (mounted.current && previewVersion.current === version && previewDriver.current === driver && !current.request && !current.state.inspectedChildId && current.state.activeChatId === identity.chatId && current.state.suppliedRun?.state === 'streaming' && sameRun(current.state.suppliedRun, identity)) reduce({ type: 'receiveSuppliedRun', events });
      });
      previewDriver.current = driver;
      setPreviewPaused(false);
      reduce({ type: 'startSuppliedRun', identity });
      driver.start();
    } catch {
      if (mounted.current && previewVersion.current === version && !latest.current.request) setPreviewNotice('Run preview could not load. Try again.');
    } finally {
      if (mounted.current && previewVersion.current === version) setPreviewLoading(false);
    }
  }
  function stopPreview() {
    const run = latest.current.state.suppliedRun;
    if (run?.state === 'streaming' && run.chatId === chat.id) dispatch({ type: 'stopSuppliedRun', identity: { chatId: run.chatId, runId: run.runId, generation: run.generation } });
  }

  // A page is not modal, but hidden chat callbacks still have no authority.
  function dispatch(action: PreviewAction) {
    const current = latest.current;
    if (!mounted.current || current.request || current.state !== state) return false;
    cancelFor(action); reduce(action);
    return true;
  }
  function sidebarDispatch(action: PreviewAction) {
    const current = latest.current;
    if (!mounted.current || current.request) return;
    cancelFor(action); reduce(action);
  }
  const railButtons: Record<InspectorModule, typeof agentsButtonRef> = { agents: agentsButtonRef, changes: changesButtonRef, context: contextButtonRef };
  // CSS alone decides docked versus sheet; this reads the outcome only when focus needs it.
  function inspectorSheet() {
    const track = inspectorPanelRef.current?.parentElement;
    return track ? getComputedStyle(track).position === 'absolute' : false;
  }
  // Inspector intents from the rail, keys, meter or palette. Closing a panel that holds focus hands it to its rail button.
  function inspectorDispatch(action: PreviewAction, focus: 'sheet' | 'panel' = 'sheet') {
    const shown = latest.current.state.inspector;
    const opens = action.type === 'toggleInspectorVisibility' ? shown === null : (action.type === 'toggleInspector' || action.type === 'showInspector') && shown !== action.module;
    const closes = shown !== null && (action.type === 'toggleInspectorVisibility' || action.type === 'toggleInspector' && action.module === shown);
    const holdsFocus = closes && document.activeElement instanceof Node && inspectorPanelRef.current?.contains(document.activeElement) === true;
    if (!dispatch(action)) return;
    if (opens) focusInspectorAfterCommit.current = focus;
    else if (holdsFocus && shown) railButtons[shown].current?.focus();
  }
  function openScreen(kind: MainScreenRequest['kind'], targetChatId?: string) {
    const current = latest.current;
    if (!mounted.current || current.request || kind === 'models' && current.state.inspectedChildId) return;
    const target = typeof targetChatId === 'string' && current.state.chats.some((entry) => entry.id === targetChatId) ? targetChatId : undefined;
    cancelPreviewTransport();
    if (current.state.suppliedRun?.state === 'streaming') reduce({ type: 'stopSuppliedRun', identity: current.state.suppliedRun });
    const request: MainScreenRequest = { id: 'main-screens', version: ++screenVersion.current, kind, parentChatId: current.state.activeChatId, inspectedChildId: current.state.inspectedChildId, ...(target ? { targetChatId: target } : {}) };
    current.request = request;
    if (kind === 'settings') settingsFocus.current = 'entry';
    setScreenRequest(request);
  }
  function currentScreen(request: MainScreenRequest) {
    const current = latest.current;
    return mounted.current && current.request === request && current.state.activeChatId === request.parentChatId && current.state.inspectedChildId === request.inspectedChildId;
  }
  function closeScreen(request: MainScreenRequest) {
    if (!currentScreen(request)) return;
    latest.current.request = null;
    if (request.kind === 'settings') settingsFocus.current = 'return';
    setScreenRequest(null);
  }
  function selectModel(request: MainScreenRequest, intent: ModelSelection) {
    if (!currentScreen(request) || intent.id !== request.id || intent.version !== request.version || intent.reasoning !== null) return;
    const model = modelOptions.find((option) => option.id === intent.modelId)?.id;
    if (!model) return;
    if (intent.scope === 'new-chat-default') setDefaultModel(model);
    else if (intent.scope === 'current-chat' && !latest.current.state.inspectedChildId && latest.current.state.chats.some((entry) => entry.id === request.parentChatId)) {
      reduce({ type: 'setModel', chatId: request.parentChatId, model });
    }
  }
  // Screen intents act while their request is current, then the screen closes itself.
  function screenAction(request: MainScreenRequest, action: PreviewAction) {
    if (!currentScreen(request)) return false;
    cancelFor(action); reduce(action);
    return true;
  }
  function openChatFromScreen(request: MainScreenRequest, chatId: string) {
    const target = latest.current.state.chats.find((entry) => entry.id === chatId);
    if (!target || !currentScreen(request)) return;
    // Opening an archived chat from the finder restores it to the sidebar.
    if (target.archived) reduce({ type: 'unarchiveChat', chatId });
    if (screenAction(request, { type: 'viewChat', chatId })) focusComposerAfterCommit.current = true;
  }
  // Settings at a named section, from Settings itself or from a dialog it replaces.
  function openSettingsSection(request: MainScreenRequest, section: SettingsSection) {
    if (!currentScreen(request)) return;
    setSettingsSection(section);
    if (request.kind === 'settings') return;
    latest.current.request = null;
    openScreen('settings');
  }

  function appHost(root: HTMLElement, current: () => boolean) {
    const now = latest.current.state;
    return { root, child: Boolean(now.inspectedChildId), working: now.chats.find((entry) => entry.id === now.activeChatId)?.status === 'working', current, open: openScreen, newChat: () => newChat(), dispatch: (action: PreviewAction) => inspectorDispatch(action, 'panel') };
  }

  function changeDisplay(toolDisplay: ToolDisplay, control: HTMLSelectElement) {
    const current = latest.current;
    if (!mounted.current || current !== displayOwner || current.request || current.state !== state || current.preferences !== preferences || current.state.inspectedChildId || current.state.activeChatId !== chat.id || control !== parentChatRef.current?.querySelector('select[data-tool-display]') || !canFocus(control) || document.hidden || document.querySelector('dialog[open]') || !isToolDisplay(toolDisplay)) return;
    const conversation = preferences.conversation ?? defaultConversation;
    if (conversation.toolDisplay === toolDisplay && conversation.showCompletedOutput) return;
    const next = { ...preferences, conversation: { ...conversation, toolDisplay, showCompletedOutput: true } };
    latest.current.preferences = next;
    setPreferences(next);
  }

  function newChat(projectId?: string) {
    if (!mounted.current || latest.current.request) return;
    focusComposerAfterCommit.current = true;
    sidebarDispatch({ type: 'newChat', model: defaultModel, ...latest.current.preferences.general, ...(typeof projectId === 'string' ? { projectId } : {}), ...(narrowWindow() ? { inspector: 'closed' as const } : {}) });
  }

  function backToParent() {
    if (latest.current.request) return false;
    focusTitleAfterCommit.current = true;
    return dispatch({ type: 'backToParent' });
  }

  function closeInspector() {
    if (!state.inspector || latest.current.request) return;
    dispatch({ type: 'closeInspector' });
    railButtons[state.inspector].current?.focus();
  }

  // A failed step with nothing else to offer: ask AVA again from the message box, never discarding a draft.
  function askRetry() {
    const request = 'Retry the last step.';
    const draft = chat.draft.trim() === '' ? request : chat.draft.includes(request) ? chat.draft : `${chat.draft.trimEnd()}\n${request}`;
    if (!child && dispatch({ type: 'setDraft', chatId: chat.id, draft })) focusComposerAfterCommit.current = true;
  }

  // Keyboard regions for the skip links and F6, in order: sidebar, transcript, message box, open panel.
  function regionTargets(root: HTMLElement): (HTMLElement | null)[] {
    const sidebar = root.querySelector<HTMLElement>(':scope > aside');
    const canvas = parentChatRef.current;
    const sidebarTargets = [sidebar?.querySelector<HTMLElement>('[aria-current="page"]'), sidebar?.querySelector<HTMLElement>('[role="region"]'), sidebar?.querySelector<HTMLElement>('button')];
    return [
      sidebarTargets.find((element) => canFocus(element ?? null)) ?? null,
      canvas?.querySelector<HTMLElement>('[role="region"][tabindex="0"]') ?? null,
      canvas?.querySelector<HTMLElement>('textarea') ?? null,
      state.inspector ? inspectorPanelRef.current : null,
    ];
  }
  function cycleRegion(root: HTMLElement, step: 1 | -1) {
    const targets = regionTargets(root);
    const active = document.activeElement;
    const canvas = parentChatRef.current;
    const at = !(active instanceof Node) ? -1
      : canvas?.querySelector('textarea')?.closest('form')?.contains(active) ? 2
      : canvas?.contains(active) ? 1
      : canvas?.parentElement?.contains(active) ? 3
      : root.querySelector(':scope > aside')?.contains(active) ? 0 : -1;
    const start = at >= 0 ? at : step > 0 ? targets.length - 1 : 0;
    for (let offset = 1; offset <= targets.length; offset += 1) {
      const target = targets[(start + step * offset + targets.length) % targets.length];
      if (canFocus(target)) {
        target.focus();
        return true;
      }
    }
    return false;
  }
  function skipTo(event: MouseEvent<HTMLAnchorElement>, region: 1 | 2) {
    const target = appRef.current ? regionTargets(appRef.current)[region] : null;
    // Without a focusable target the native jump to the canvas still happens.
    if (!canFocus(target)) return;
    event.preventDefault();
    target.focus();
  }

  function undoArchive(chatId: string) {
    if (dispatch({ type: 'unarchiveChat', chatId })) focusTitleAfterCommit.current = true;
  }
  function dismissArchive() {
    if (!dispatch({ type: 'dismissArchiveNotice' })) return;
    focusComposerAfterCommit.current = !child;
    focusTitleAfterCommit.current = Boolean(child);
  }

  const previewLabel = previewLoading ? 'Loading preview…' : runningPreview ? 'Stop preview' : previewEnded ? 'Retry preview' : 'Run preview';
  const workingFixture = chat.status === 'working' && !runningPreview && !previewLoading;
  const previewControls = child ? undefined : <>
    {/* One stable element for Run, Loading, Stop and Retry: keyboard focus never falls to the page. */}
    <button ref={previewToggleRef} className={banner.control} type="button"
      disabled={workingFixture}
      aria-disabled={previewLoading || undefined}
      data-busy={previewLoading || undefined}
      title={workingFixture ? 'Available when this chat isn’t working' : runningPreview ? 'Stop the example reply; the partial reply is kept' : 'Play an example reply in this chat'}
      onClick={(event) => {
        if (!canFocus(event.currentTarget) || previewLoading) return;
        if (runningPreview) stopPreview(); else void startPreview();
      }}>
      {previewLoading ? <CircleNotchIcon size={13} aria-hidden="true" /> : runningPreview ? <StopIcon size={13} aria-hidden="true" /> : previewEnded ? <ArrowCounterClockwiseIcon size={13} aria-hidden="true" /> : <PlayIcon size={13} aria-hidden="true" />}
      {previewLabel}
    </button>
    {previewFeature ? <previewFeature.PreviewControls example={previewExample} running={runningPreview} paused={previewPaused}
      onExample={(value) => { if (mounted.current && !latest.current.request && latest.current.state === state && !runningPreview) setPreviewExample(value); }}
      onPause={() => { if (mounted.current && !latest.current.request && latest.current.state === state && runningPreview) { if (previewPaused) previewDriver.current?.resume(); else previewDriver.current?.pause(); setPreviewPaused(!previewPaused); } }}
      onStep={() => { if (mounted.current && !latest.current.request && latest.current.state === state && runningPreview && previewPaused) previewDriver.current?.step(); }} /> : null}
  </>;
  const notice = state.archiveNotice;

  return (
    <ReducedMotionContext.Provider value={preferences.motion === 'reduced'}>
    <main ref={appRef} className={`${styles.app} ${appearance.scope}`} data-code-wrap={preferences.conversation?.wrapCode ?? false} data-diff-wrap={preferences.conversation?.wrapDiff ?? true} data-palette={preferences.palette} data-ui-font={preferences.uiFont} data-prose-size={preferences.proseSize} data-prose-spacing={preferences.proseSpacing} data-reading-width={preferences.readingWidth} data-contrast={preferences.contrast} data-motion={preferences.motion} data-page={settingsOpen ? 'settings' : 'chat'} data-sidebar={sidebarCollapsed ? 'collapsed' : 'expanded'} data-material={state.opaque ? 'opaque' : 'glass'} aria-label="AVA UI preview" aria-describedby="app-description"
      onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }} onKeyDown={(event) => {
        if (event.key === 'F6' && !event.ctrlKey && !event.metaKey && !event.altKey) {
          // F6 / Shift+F6 step between the sidebar, transcript, message box and open panel; never out of a dialog, menu or picker.
          if (event.defaultPrevented || event.nativeEvent.isComposing || latest.current.request || !mounted.current || !(event.target instanceof Element) || event.target.closest('dialog, [role="menu"], [role="listbox"]') || document.querySelector('dialog[open], [role="menu"]')) return;
          if (cycleRegion(event.currentTarget, event.shiftKey ? -1 : 1)) event.preventDefault();
          return;
        }
        if (event.defaultPrevented || composing.current || event.nativeEvent.isComposing || latest.current.request || !mounted.current || !event.ctrlKey && !event.metaKey || preferences.shortcuts?.enabled === false) return;
        const root = event.currentTarget;
        // Matched synchronously: Mod+K and Mod+P have browser defaults that must not run first.
        const action = appChord(event.nativeEvent, root, preferences.shortcuts);
        if (!action) return;
        event.preventDefault();
        if (commandPending.current) return;
        commandPending.current = true;
        const snapshot = latest.current; const focus = document.activeElement;
        const current = () => latest.current === snapshot && !snapshot.request && !composing.current && canFocus(root) && document.activeElement === focus;
        void import('./AppCommands').then(({ runAppAction }) => {
          if (!current()) return;
          setCommandNotice('');
          runAppAction(action, appHost(root, current));
        }, () => { if (current()) setCommandNotice('Keyboard actions could not load. Use the visible controls or try again.'); }).finally(() => { commandPending.current = false; });
      }}>
      {settingsOpen ? null : <>
        <a className={styles.skipLink} href="#conversation-canvas" onClick={(event) => skipTo(event, 1)}>Skip to conversation</a>
        {child ? null : <a className={styles.skipLink} href="#conversation-canvas" onClick={(event) => skipTo(event, 2)}>Skip to message box</a>}
      </>}
      <p id="app-description" className="srOnly">Browser-only UI preview with invented chats, child agents, models, context and changes. Nothing is saved, sent or run. No backend or model is connected. Run preview plays an example reply in this browser; pausing only pauses that playback.</p>
      <ProjectSidebar state={state} dispatch={sidebarDispatch} settingsActive={settingsOpen} settingsRef={settingsButtonRef} onOpenSettings={() => openScreen('settings')} onNewChat={newChat}
        onFindChat={() => openScreen('sessions')} onOpenCommands={() => openScreen('commands')}
        onChatAction={(chatId, action) => openScreen(action, chatId)} collapsed={sidebarCollapsed} onToggleCollapsed={() => setSidebarCollapsed((value) => !value)} shortcuts={preferences.shortcuts} />
      <div className={styles.workspace} hidden={settingsOpen} inert={settingsOpen} data-inspector={state.inspector ? 'open' : 'closed'} data-inspector-size={inspectorWide ? 'wide' : undefined}>
        <ChangeNavigationContext.Provider value={child ? null : reviewChange}>
        <section ref={parentChatRef} id="conversation-canvas" className={styles.chat} aria-labelledby="conversation-title" onFocus={() => {
          // Small windows: the inspector is a sheet over this column. Focus arriving here by any route (Tab, Shift+Tab,
          // click, F6, skip link, a returned title) dismisses it, so nothing focused ever sits beneath it.
          if (state.inspector && inspectorSheet()) dispatch({ type: 'closeInspector' });
        }} onKeyDown={(event) => {
          // Escape in a child view returns to its parent unless a control already used it.
          if (!child || event.key !== 'Escape' || event.defaultPrevented || event.nativeEvent.isComposing || event.keyCode === 229) return;
          event.preventDefault();
          if (backToParent()) childRowAfterCommit.current = child.id;
        }}>
          {bannerOpen ? (
            <PreviewBanner notice={child ? undefined : previewNotice || undefined} dismissRef={bannerDismissRef} onFocusControl={(control) => { bannerControl.current = control; }} onDismiss={() => { bannerFocus.current = 'chip'; setBannerOpen(false); }}>{previewControls}</PreviewBanner>
          ) : null}
          <ConversationIdentity
            title={child ? child.title : chat.title}
            titleRef={titleRef}
            backDescription={child ? `Back to ${chat.title} · Esc. Its draft and settings are unchanged.` : undefined}
            onBack={child ? () => { backToParent(); } : undefined}
            onChatAction={child ? undefined : (action) => openScreen(action, chat.id)}
            aside={bannerOpen ? undefined : <PreviewChip chipRef={previewChipRef} onOpen={() => { bannerFocus.current = 'banner'; setBannerOpen(true); }} />}
          />
          {notice ? (
            <div className={styles.archived} role="group" aria-label="Archived chat">
              <ArchiveIcon size={14} aria-hidden="true" />
              <p>Archived “{notice.title}”</p>
              <button ref={undoArchiveRef} className={styles.undo} type="button" onClick={() => undoArchive(notice.chatId)}>Undo</button>
              <IconButton variant="quiet" label="Dismiss" title="Dismiss" onClick={dismissArchive}><XIcon size={15} aria-hidden="true" /></IconButton>
            </div>
          ) : null}
          {child ? <ChildContentBoundary key={child.id}><Suspense fallback={<p className={styles.childNotice} role="status">Loading read-only child conversation…</p>}><ChildConversation child={child} /></Suspense></ChildContentBoundary> : (
            <>
              <Conversation key={chat.id} chat={chat} preferences={preferences.conversation} active={!screenRequest} onDisplayChange={changeDisplay} onRetryRun={() => { void startPreview(); }}
                onPermissionDecision={(messageId, decision) => { dispatch({ type: 'resolvePermission', chatId: chat.id, messageId, decision }); }} onAskRetry={askRetry} />
              <Composer chat={chat} enterBehavior={preferences.enter} dispatch={dispatch} shortcuts={preferences.shortcuts} onOpenModel={() => openScreen('models')} contextOpen={state.inspector === 'context'} onOpenContext={() => inspectorDispatch({ type: 'showInspector', module: 'context' })} />
            </>
          )}
        </section>
        </ChangeNavigationContext.Provider>
        <Presence open={state.inspector !== null} className={styles.inspectorTrack}>
          <Inspector
            module={inspectorModule}
            active={state.inspector !== null}
            count={inspectorModule === 'agents' ? children.length : inspectorModule === 'changes' ? changes.length : undefined}
            summary={inspectorModule === 'agents' && activeCount > 0 ? <ChatStatus status="working" label={`${activeCount} working`} /> : undefined}
            panelRef={inspectorPanelRef}
            wide={inspectorWide}
            onToggleWide={() => { if (mounted.current && !latest.current.request) setInspectorWide((value) => !value); }}
            onClose={closeInspector}
          >
            {inspectorModule === 'agents' ? (
              <AgentList children={children} inspectedId={state.inspectedChildId} onInspect={(childId) => {
                if (state.inspector !== 'agents' || latest.current.request) return;
                if (child?.id === childId) {
                  titleRef.current?.focus();
                  return;
                }
                focusTitleAfterCommit.current = true;
                dispatch({ type: 'inspectChild', childId });
              }} />
            ) : <Suspense fallback={<p role="status">Loading {inspectorModule === 'changes' ? 'changes' : 'Context'}…</p>}><DetailInspector module={inspectorModule} chat={chat} active={!screenRequest && state.inspector !== null} focusPath={inspectorModule === 'changes' && changeFocus.chatId === chat.id ? changeFocus.path : null} /></Suspense>}
          </Inspector>
        </Presence>
        <InspectorRail
          selected={state.inspector}
          lastModule={state.lastInspectorModule}
          onToggleVisibility={() => inspectorDispatch({ type: 'toggleInspectorVisibility' })}
          activeCount={activeCount}
          childCount={children.length}
          changeCount={changes.length}
          shortcuts={preferences.shortcuts}
          agentsRef={agentsButtonRef}
          changesRef={changesButtonRef}
          contextRef={contextButtonRef}
          onToggle={(module) => inspectorDispatch({ type: 'toggleInspector', module })}
        />
      </div>
      {screenRequest ? <section ref={settingsPageRef} className={styles.screenHost} aria-label={settingsOpen ? 'Settings page' : undefined} tabIndex={settingsOpen ? -1 : undefined} onKeyDown={(event) => {
        if (settingsOpen && event.target === event.currentTarget && event.key === 'Escape' && currentScreen(screenRequest)) {
          event.preventDefault();
          if (event.nativeEvent.isComposing || event.keyCode === 229) return;
          closeScreen(screenRequest);
        }
      }}>
        <Suspense fallback={settingsOpen ? <div className={styles.settingsLoading}><header><h1>Settings</h1><BackButton onClick={() => closeScreen(screenRequest)}>Back to chat</BackButton></header><p role="status">Loading…</p></div> : <DialogFrame title={screenRequest.kind === 'models' ? 'Loading models' : 'Loading'} onDismiss={() => closeScreen(screenRequest)} returnFocus={settingsButtonRef}><p role="status">Loading…</p></DialogFrame>}>
          <MainScreens projects={state.projects} contextUsage={previewContextUsage(chat)} key={screenRequest.version} request={screenRequest} parent={{ title: chat.title, model: chat.model }} defaultModel={defaultModel} preferences={preferences} opaque={state.opaque} onOpaqueChange={(value) => { if (currentScreen(screenRequest) && settingsOpen && latest.current.state === state) reduce({ type: 'setOpaque', opaque: value }); }} returnFocus={settingsButtonRef}
            workspaceSeed={settingsWorkspace.current} onWorkspaceSnapshot={(snapshot) => { if (currentScreen(screenRequest) && settingsOpen) settingsWorkspace.current = snapshot; }}
            section={settingsSection} onSectionChange={(value) => { if (currentScreen(screenRequest)) setSettingsSection(value); }}
            chats={chatSummaries(state)}
            onOpenChat={(chatId) => openChatFromScreen(screenRequest, chatId)}
            onRenameChat={(chatId, title) => { screenAction(screenRequest, { type: 'renameChat', chatId, title }); }}
            onArchiveChat={(chatId) => { if (screenAction(screenRequest, { type: 'archiveChat', chatId })) focusArchiveAfterCommit.current = true; }}
            onAppAction={(action) => { if (!currentScreen(screenRequest)) return; pendingAppAction.current = action; closeScreen(screenRequest); }}
            onBranchChat={(chatId, messageId) => { if (screenAction(screenRequest, { type: 'branchChat', chatId, messageId })) focusComposerAfterCommit.current = true; }}
            onOpenSettingsSection={(section) => openSettingsSection(screenRequest, section)}
            onDismiss={() => closeScreen(screenRequest)} onPreferencesChange={(value) => { if (currentScreen(screenRequest) && settingsOpen && latest.current.preferences === preferences) { latest.current.preferences = value; setPreferences(value); } }} onSelectModel={(intent) => selectModel(screenRequest, intent)} />
        </Suspense>
      </section> : null}
      <span className="srOnly" role="status" aria-live="polite">{commandNotice || state.announcement}</span>
    </main>
    </ReducedMotionContext.Provider>
  );
}
