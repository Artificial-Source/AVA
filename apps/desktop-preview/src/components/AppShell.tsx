import { Component, lazy, Suspense, useLayoutEffect, useReducer, useRef, useState, type ReactNode } from 'react';
import { previewContextUsage } from './contextPreview';
import { createInitialState, modelOptions } from '../state/fixtures';
import type { ModelId, PreviewAction } from '../state/types';
import type { MainScreenRequest, ModelSelection, Preferences, SettingsSection } from '../screens/contracts';
import { canFocus, DialogFrame } from '../ui/DialogFrame';
import { previewReducer } from '../state/reducer';
import { sameRun } from '../state/suppliedRun';
import { prepareAssistantContent } from '../chat/widgets/AssistantContentHost';
import type { RunDriver } from '../chat/suppliedRunDriver';
import type { PreviewExample } from '../chat/suppliedRunFixture';
import previewStyles from './SuppliedRunPreview.module.css';
import { inspectedChild, selectedParentChildren } from '../state/selectors';
import { Presence } from '../ui/Presence';
import { ReducedMotionContext } from '../ui/useReducedMotion';
import appearance from '../styles/appearance.module.css';
import { AgentList } from './AgentList';
import { Composer } from './Composer';
import { Conversation } from './Conversation';
import { ConversationIdentity } from './ConversationIdentity';
import { Inspector } from './Inspector';
import { InspectorRail } from './InspectorRail';
import { ProjectSidebar } from './ProjectSidebar';
import styles from './AppShell.module.css';

const MainScreens = lazy(() => import('../screens/MainScreens'));
const DetailInspector = lazy(() => import('./DetailInspector'));
const ChildConversation = lazy(() => import('./ChildConversation'));

// Only the optional read-only body is deferred. Identity, Back and model gates stay here.
class ChildContentBoundary extends Component<{ children: ReactNode }, { failed: boolean }> {
  state = { failed: false };
  static getDerivedStateFromError() { return { failed: true }; }
  render() {
    return this.state.failed ? <p className={styles.childNotice} role="alert">Child conversation could not load. Use Back to parent; its draft is unchanged.</p> : this.props.children;
  }
}

export function AppShell() {
  const [state, reduce] = useReducer(previewReducer, undefined, createInitialState);
  const [previewFeature, setPreviewFeature] = useState<typeof import('./SuppliedRunPreview') | null>(null);
  const [previewLoading, setPreviewLoading] = useState(false);
  const [previewNotice, setPreviewNotice] = useState('');
  const [previewExample, setPreviewExample] = useState<PreviewExample>('completion');
  const [previewPaused, setPreviewPaused] = useState(false);
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
  const agentsButtonRef = useRef<HTMLButtonElement>(null);
  const changesButtonRef = useRef<HTMLButtonElement>(null);
  const contextButtonRef = useRef<HTMLButtonElement>(null);
  const titleRef = useRef<HTMLHeadingElement>(null);
  const focusTitleAfterCommit = useRef(false);
  const chat = state.chats.find((entry) => entry.id === state.activeChatId)!;
  const children = selectedParentChildren(state);
  const child = inspectedChild(state);
  const activeCount = children.filter((entry) => entry.status === 'working').length;
  const inspectorModule = state.inspector ?? state.lastInspectorModule;
  const settingsOpen = screenRequest?.kind === 'settings';
  const runningPreview = state.suppliedRun?.chatId === chat.id && state.suppliedRun.state === 'streaming';

  useLayoutEffect(() => {
    // Only explicit page/Inspect/Back intents request focus, after visibility commits.
    if (settingsFocus.current) {
      const target = settingsFocus.current === 'entry' ? settingsPageRef.current : canFocus(settingsButtonRef.current) ? settingsButtonRef.current : titleRef.current;
      settingsFocus.current = null;
      if (canFocus(target)) target.focus({ preventScroll: true });
    }
    if (focusTitleAfterCommit.current) {
      focusTitleAfterCommit.current = false;
      titleRef.current?.focus();
    }
  });

  useLayoutEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; ++previewVersion.current; previewDriver.current?.cancel(); previewDriver.current = null; };
  }, []);
  useLayoutEffect(() => {
    if (screenRequest && (screenRequest.parentChatId !== state.activeChatId || screenRequest.inspectedChildId !== state.inspectedChildId)) {
      latest.current.request = null;
      setScreenRequest(null);
    }
  }, [screenRequest, state.activeChatId, state.inspectedChildId]);

  function cancelPreviewTransport() {
    ++previewVersion.current;
    previewDriver.current?.cancel(); previewDriver.current = null;
    setPreviewLoading(false); setPreviewPaused(false);
  }
  function cancelFor(action: PreviewAction) {
    const current = latest.current.state;
    if (action.type === 'newChat' || action.type === 'viewChat' && action.chatId !== current.activeChatId && current.chats.some((entry) => entry.id === action.chatId) || action.type === 'inspectChild' && current.childAgents.some((entry) => entry.id === action.childId && entry.parentChatId === current.activeChatId) || action.type === 'stopViewedMock' || action.type === 'stopSuppliedRun' && current.suppliedRun && sameRun(current.suppliedRun, action.identity)) cancelPreviewTransport();
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

  // A page is not modal, but hidden chat callbacks still have no authority.
  function dispatch(action: PreviewAction) {
    const current = latest.current;
    if (mounted.current && !current.request && current.state === state) { cancelFor(action); reduce(action); }
  }
  function sidebarDispatch(action: PreviewAction) {
    const current = latest.current;
    if (!mounted.current || current.request) return;
    cancelFor(action); reduce(action);
  }
  function openScreen(kind: MainScreenRequest['kind']) {
    const current = latest.current;
    if (!mounted.current || current.request || kind === 'models' && current.state.inspectedChildId) return;
    cancelPreviewTransport();
    if (current.state.suppliedRun?.state === 'streaming') reduce({ type: 'stopSuppliedRun', identity: current.state.suppliedRun });
    const request: MainScreenRequest = { id: 'main-screens', version: ++screenVersion.current, kind, parentChatId: current.state.activeChatId, inspectedChildId: current.state.inspectedChildId };
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

  function newChat() {
    sidebarDispatch({ type: 'newChat', model: defaultModel, ...latest.current.preferences.general });
  }

  function closeInspector() {
    if (!state.inspector || latest.current.request) return;
    dispatch({ type: 'closeInspector' });
    (state.inspector === 'agents' ? agentsButtonRef : state.inspector === 'changes' ? changesButtonRef : contextButtonRef).current?.focus();
  }

  return (
    <ReducedMotionContext.Provider value={preferences.motion === 'reduced'}>
    <main className={`${styles.app} ${appearance.scope}`} data-code-wrap={preferences.conversation?.wrapCode ?? false} data-diff-wrap={preferences.conversation?.wrapDiff ?? true} data-palette={preferences.palette} data-ui-font={preferences.uiFont} data-prose-size={preferences.proseSize} data-prose-spacing={preferences.proseSpacing} data-reading-width={preferences.readingWidth} data-contrast={preferences.contrast} data-motion={preferences.motion} data-page={settingsOpen ? 'settings' : 'chat'} data-material={state.opaque ? 'opaque' : 'glass'} aria-label="AVA UI preview" aria-describedby="app-description"
      onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }} onKeyDown={(event) => {
        if (event.defaultPrevented || composing.current || latest.current.request || commandPending.current || !mounted.current || !event.ctrlKey && !event.metaKey || preferences.shortcuts?.enabled === false) return;
        const root = event.currentTarget; const key = event.nativeEvent;
        commandPending.current = true;
        const snapshot = latest.current; const focus = document.activeElement;
        const current = () => latest.current === snapshot && !snapshot.request && !composing.current && canFocus(root) && document.activeElement === focus;
        void import('./AppCommands').then(({ runAppCommand }) => {
          if (!current()) return;
          setCommandNotice('');
          runAppCommand(key, { root, preferences: preferences.shortcuts, child: Boolean(child), working: chat.status === 'working', current, open: openScreen, newChat, dispatch });
        }, () => { if (current()) setCommandNotice('Keyboard actions could not load. Use the visible controls or try again.'); }).finally(() => { commandPending.current = false; });
      }}>
      <p id="app-description" className="srOnly">Browser-only UI preview with invented chats, child conversations, models, context, activity, and changes. Nothing is saved. No backend or model is connected. Fixed examples are not live activity. Run preview explicitly plays supplied incremental text and tool snapshots in this browser; pausing affects only preview delivery.</p>
      <ProjectSidebar state={state} dispatch={sidebarDispatch} settingsActive={settingsOpen} settingsRef={settingsButtonRef} onOpenSettings={() => openScreen('settings')} onNewChat={newChat} />
      <div className={styles.workspace} hidden={settingsOpen} inert={settingsOpen} data-inspector={state.inspector ? 'open' : 'closed'}>
        <section id="conversation-canvas" className={styles.chat} aria-labelledby="conversation-title">
          <ConversationIdentity
            title={child ? child.title : chat.title}
            titleRef={titleRef}
            backDescription={child ? `Return to parent: ${chat.title}. Restore the parent’s draft and settings.` : undefined}
            onBack={child ? () => {
              if (latest.current.request) return;
              focusTitleAfterCommit.current = true;
              dispatch({ type: 'backToParent' });
            } : undefined}
          />
          {child ? <ChildContentBoundary key={child.id}><Suspense fallback={<p className={styles.childNotice} role="status">Loading read-only child conversation…</p>}><ChildConversation child={child} /></Suspense></ChildContentBoundary> : (
            <>
              <Conversation key={chat.id} chat={chat} preferences={preferences.conversation} active={!screenRequest} />
              <div className={previewStyles.controls}>
                <button type="button" disabled={chat.status === 'working' || previewLoading} onClick={(event) => { if (canFocus(event.currentTarget)) void startPreview(); }}>{previewLoading ? 'Loading preview…' : state.suppliedRun?.chatId === chat.id && ['failed', 'canceled'].includes(state.suppliedRun.state) ? 'Retry preview' : 'Run preview'}</button>
                {previewFeature ? <previewFeature.PreviewControls example={previewExample} running={runningPreview} paused={previewPaused}
                  onExample={(value) => { if (mounted.current && !latest.current.request && latest.current.state === state && !runningPreview) setPreviewExample(value); }}
                  onPause={() => { if (mounted.current && !latest.current.request && latest.current.state === state && runningPreview) { if (previewPaused) previewDriver.current?.resume(); else previewDriver.current?.pause(); setPreviewPaused(!previewPaused); } }}
                  onStep={() => { if (mounted.current && !latest.current.request && latest.current.state === state && runningPreview && previewPaused) previewDriver.current?.step(); }} /> : null}
                {previewNotice ? <p role="status">{previewNotice}</p> : null}
              </div>
              <Composer chat={chat} enterBehavior={preferences.enter} dispatch={dispatch} onOpenModel={() => openScreen('models')} contextOpen={state.inspector === 'context'} onOpenContext={() => dispatch({ type: 'showInspector', module: 'context' })} />
            </>
          )}
        </section>
        <Presence open={state.inspector !== null} className={styles.inspectorTrack}>
          <Inspector
            module={inspectorModule}
            active={state.inspector !== null}
            count={inspectorModule === 'agents' ? children.length : inspectorModule === 'changes' ? chat.changes.length : undefined}
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
            ) : <Suspense fallback={<p role="status">Loading {inspectorModule === 'changes' ? 'changes' : 'Context'}…</p>}><DetailInspector module={inspectorModule} chat={chat} active={!screenRequest && state.inspector !== null} /></Suspense>}
          </Inspector>
        </Presence>
        <InspectorRail
          selected={state.inspector}
          lastModule={state.lastInspectorModule}
          onToggleVisibility={() => dispatch({ type: 'toggleInspectorVisibility' })}
          activeCount={activeCount}
          parentTitle={chat.title}
          agentsRef={agentsButtonRef}
          changesRef={changesButtonRef}
          contextRef={contextButtonRef}
          onToggle={(module) => dispatch({ type: 'toggleInspector', module })}
        />
      </div>
      {screenRequest ? <section ref={settingsPageRef} className={styles.screenHost} aria-label={settingsOpen ? 'Settings page' : undefined} tabIndex={settingsOpen ? -1 : undefined} onKeyDown={(event) => {
        if (settingsOpen && event.target === event.currentTarget && event.key === 'Escape' && currentScreen(screenRequest)) {
          event.preventDefault();
          if (event.nativeEvent.isComposing || event.keyCode === 229) return;
          if (settingsSection !== 'general') setSettingsSection('general'); else closeScreen(screenRequest);
        }
      }}>
        <Suspense fallback={settingsOpen ? <div className={styles.settingsLoading}><h1>Settings</h1><p role="status">Loading…</p><button type="button" onClick={() => closeScreen(screenRequest)}>Back to chat</button></div> : <DialogFrame title="Loading models" description="Local controls. No provider connected." onDismiss={() => closeScreen(screenRequest)} returnFocus={settingsButtonRef}><p role="status">Loading…</p></DialogFrame>}>
          <MainScreens projects={state.projects} contextUsage={previewContextUsage(chat)} key={screenRequest.version} request={screenRequest} parent={{ title: chat.title, model: chat.model }} defaultModel={defaultModel} preferences={preferences} opaque={state.opaque} onOpaqueChange={(value) => { if (currentScreen(screenRequest) && settingsOpen && latest.current.state === state) reduce({ type: 'setOpaque', opaque: value }); }} returnFocus={settingsButtonRef}
            workspaceSeed={settingsWorkspace.current} onWorkspaceSnapshot={(snapshot) => { if (currentScreen(screenRequest) && settingsOpen) settingsWorkspace.current = snapshot; }}
            section={settingsSection} onSectionChange={(value) => { if (currentScreen(screenRequest)) setSettingsSection(value); }}
            onDismiss={() => closeScreen(screenRequest)} onPreferencesChange={(value) => { if (currentScreen(screenRequest) && settingsOpen && latest.current.preferences === preferences) { latest.current.preferences = value; setPreferences(value); } }} onSelectModel={(intent) => selectModel(screenRequest, intent)} />
        </Suspense>
      </section> : null}
      <span className="srOnly" role="status" aria-live="polite">{commandNotice || state.announcement}</span>
    </main>
    </ReducedMotionContext.Provider>
  );
}
