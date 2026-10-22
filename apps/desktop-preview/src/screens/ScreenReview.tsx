import { Fragment, useRef, useState, type ReactNode } from 'react';
import { createInitialState } from '../state/fixtures';
import { previewContextUsage } from '../components/contextPreview';
import type { ModelId } from '../state/types';
import { AvaMark } from '../components/AvaMark';
import { BackLink, Button } from '../ui/Button';
import { DialogFrame } from '../ui/DialogFrame';
import { ReducedMotionContext } from '../ui/useReducedMotion';
import appearance from '../styles/appearance.module.css';
import type { ActionView, ConnectIntent, ConnectMethod, ConnectView, FinderView, ModelPickerView, ModelScope, Preferences, RequestReceipt, SettingsSection, WorkspaceState, WorkspaceView } from './contracts';
import { ChatActionDialog, MessageDialog, PermissionDialog, QuestionDialog, RenameDialog, RunDialog, ToolDialog, validRename, validWorkspaceIntent, WorkspaceDialog } from './ChatDialogs';
import { FinderDialog, validFinderIntent } from './FinderDialog';
import { ModelDialog, validModelSelection } from './ModelDialog';
import { ProviderConnectDialog, ProviderDirectory, connectStart, validConnectIntent } from './ProviderViews';
import { changedPreferenceAreas } from './preferenceDiff';
import { Confirmation, ListNotice } from './ScreenPrimitives';
import { SettingsScreen, settingsSections } from './SettingsScreen';
import { resourceInspections } from './screenExamples';
import { useSubagentWorkspace } from './subagentWorkspace';
import { branchPoints, commandItems, defaultPreferences, familyIds, inspectionErrorSections, isFamily, isScenario, listState, modelCase, modelChoices, permissionCase, projects, providers, questionCase, reviewFamilies, runCase, sessions, toolCase, workspaceChoices, type ReviewFamily } from './fixtures';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';

type DialogFamily = Exclude<ReviewFamily, 'settings'>;
type ScreenOverlay = { kind: DialogFamily; scenario: string; version: number; providerId: string; method: ConnectMethod | null; scope: ModelScope };
type Overlay = ScreenOverlay | { kind: 'reset'; target: 'preferences' | 'all'; version: number };
const isDirectory = (scenario: string) => scenario === 'directory' || scenario === 'directory-error';
const initialReceipt = 'No actions yet.';
const noMatch = 'no matching chat';
const familyDescriptions: Record<ReviewFamily, string> = {
  settings: 'Every settings page, plus search, changed preferences and the loading, empty, error and access states of rules and extensions.',
  models: 'One picker for this chat’s model and the default for new chats, grouped by provider.',
  providers: 'The provider list and each way to connect: API key, browser sign-in and a device code, through success, failure and disconnect.',
  sessions: 'Search chat titles and messages across projects, with active and archived chats kept apart.',
  commands: 'Every action in one searchable list, with its keyboard shortcut.',
  rename: 'A single title field, up to 256 characters.',
  archive: 'What archiving does before you confirm it, and what happens while the chat is still running.',
  branch: 'Pick one of your messages to start a new chat from. The original chat stays as it is.',
  workspace: 'Pick a project, then follow it through opening, ready, offline, failure and cancel. Picking the project already open just closes.',
  message: 'One message on its own: your words, AVA’s reply, or a reply that stopped or failed.',
  tool: 'One tool call: its status, output and file changes.',
  permission: 'What AVA wants to do, where and why, and what each answer allows.',
  question: 'Questions AVA asks mid-run: single or multiple choice, your own answer, or a hidden value.',
  run: 'Errors, retries and stops, each shown as its own state.',
};
const decisions = { allow: 'allowed once', allow_session: 'allowed for this session', deny: 'denied' } as const;

function ScenarioControl({ family, scenario, onChange, onReset }: { family: ReviewFamily; scenario: string; onChange: (value: string) => void; onReset: () => void }) {
  return <div className={styles.reviewBar}><label>Review scenario<select className={styles.select} aria-label="Review scenario" value={scenario} onChange={(event) => { if (isScenario(family, event.currentTarget.value)) onChange(event.currentTarget.value); }}>{reviewFamilies[family].cases.map((value) => <option key={value}>{value}</option>)}</select></label><Button onClick={onReset}>Reset case</Button></div>;
}

export default function ScreenReview() {
  const [supplied] = useState(createInitialState);
  const [family, setFamily] = useState<ReviewFamily>('settings');
  const [scenario, setScenario] = useState<string>('General');
  const [overlay, setOverlay] = useState<Overlay | null>(null);
  const [preferences, setPreferences] = useState<Preferences>(defaultPreferences);
  const [opaque, setOpaque] = useState(true);
  const [section, setSection] = useState<SettingsSection>('general');
  const [settingsOpen, setSettingsOpen] = useState(true);
  const [query, setQuery] = useState('');
  const [settingsQuery, setSettingsQuery] = useState('');
  const [currentModel, setCurrentModel] = useState<ModelId>('aster');
  const [defaultModel, setDefaultModel] = useState<ModelId>('aster-mini');
  const [reasoning, setReasoning] = useState<{ current: string | null; default: string | null }>({ current: 'Low', default: null });
  const [title, setTitle] = useState('Refine search filters');
  const [selectedSession, setSelectedSession] = useState('filters');
  const [workspace, setWorkspace] = useState('atlas');
  const [receipt, setReceipt] = useState(initialReceipt);
  const [resetVersion, setResetVersion] = useState(0);
  const [caseResetVersion, setCaseResetVersion] = useState(0);
  const revision = useRef(0);
  const backToChat = useRef<HTMLAnchorElement>(null);
  const hostGate = useViewGuard(JSON.stringify(overlay));
  const settingsGate = useViewGuard(JSON.stringify([family, settingsOpen, section, overlay, preferences, opaque]));
  const definitions = useSubagentWorkspace(undefined, family === 'settings' && settingsOpen && section === 'subagents' && !overlay);

  function nextOverlay(kind: DialogFamily, value: string, providerId?: string, scope: ModelScope = 'current-chat', previous?: ScreenOverlay | null): ScreenOverlay {
    const id = providerId ?? (value === 'disconnect' ? 'orion-lab' : value === 'externally-managed' ? 'cedar' : 'aster-lab');
    // A result step keeps the method the same provider was just using: a failed key returns to the key form.
    const kept = previous?.kind === kind && previous.providerId === id ? previous.method : null;
    const method = value === 'key' || value === 'browser' || value === 'device' ? value : ['waiting', 'success', 'error', 'canceled'].includes(value) ? kept ?? (id === 'aster-lab' ? 'browser' : 'key') : null;
    return { kind, scenario: value, version: ++revision.current, providerId: id, method, scope };
  }
  function open(kind: DialogFamily, value = reviewFamilies[kind].cases[0] as string, providerId?: string, scope?: ModelScope) {
    if (!isScenario(kind, value)) return;
    settingsGate.invalidate();
    setQuery(value === 'no-match' ? noMatch : '');
    setOverlay(nextOverlay(kind, value, providerId, scope));
  }
  function settingsCase(value: string) {
    setSettingsOpen(true);
    const found = settingsSections.find((entry) => entry.label === value);
    setSection(found?.id ?? (value.startsWith('Rules') ? 'permissions' : Object.hasOwn(inspectionErrorSections, value) ? 'extensions' : 'general'));
    setSettingsQuery(value === 'Search / no match' ? 'no matching setting' : '');
    if (value === 'Changed preferences') setPreferences({ ...defaultPreferences, density: 'compact', notifications: 'attention', enter: 'newline' });
  }
  function navigate(value: string, selectedCase?: string) {
    if (!isFamily(value)) return;
    const nextCase = selectedCase && isScenario(value, selectedCase) ? selectedCase : reviewFamilies[value].cases[0];
    hostGate.invalidate(); settingsGate.invalidate();
    setFamily(value); setScenario(nextCase); setReceipt(initialReceipt);
    if (value === 'settings') { setOverlay(null); settingsCase(nextCase); }
    else if (value === 'providers' && isDirectory(nextCase)) setOverlay(null);
    else open(value, nextCase);
  }
  function changeCase(kind: ReviewFamily, value: string) {
    if (!isScenario(kind, value)) return;
    if (family === kind) setScenario(value);
    setReceipt(initialReceipt);
    if (kind === 'settings') { setOverlay(null); settingsCase(value); setResetVersion((version) => version + 1); return; }
    if (kind === 'providers' && isDirectory(value)) { setOverlay(null); return; }
    // Preserve a typed model query across catalog state changes, except the explicit no-match case.
    if (kind !== 'models' || value === 'no-match') setQuery(value === 'no-match' ? noMatch : '');
    const previous = overlay && overlay.kind === kind ? overlay : null;
    setOverlay(nextOverlay(kind, value, kind === 'providers' ? undefined : previous?.providerId, previous?.scope, previous));
  }
  function close() {
    if (!hostGate.valid()) return;
    hostGate.invalidate();
    if (overlay?.kind === 'permission') setReceipt('Closed. The request is still waiting for a decision.');
    else if (overlay?.kind === 'question') setReceipt('Closed. The question is still unanswered.');
    else if (overlay?.kind === 'providers') setReceipt('Closed. The key field was cleared.');
    setOverlay(null);
  }
  function receive(message: string) { setReceipt(`Last action: ${message}.`); }
  function finish(message: string) { receive(message); hostGate.invalidate(); setOverlay(null); }
  function showProvider(id: string, disconnect = false) {
    const provider = providers.find((entry) => entry.id === id);
    if (!provider || (disconnect ? provider.source !== 'stored-example' : provider.state === 'external' || !provider.methods.length)) return;
    open('providers', disconnect ? 'disconnect' : connectStart(provider).step, id);
  }
  // Matches the app: Reset all preferences also returns new chats to Aster.
  function reset(target: 'preferences' | 'all') {
    if (!hostGate.claim()) return;
    hostGate.invalidate();
    setPreferences(defaultPreferences); setOpaque(true); setDefaultModel('aster'); setReasoning({ ...reasoning, default: null }); setResetVersion((value) => value + 1); setOverlay(null);
    setReceipt('Last action: preferences reset.');
    if (target === 'all') {
      definitions.reset();
      setCurrentModel('aster'); setDefaultModel('aster-mini'); setReasoning({ current: 'Low', default: null }); setTitle('Refine search filters'); setSelectedSession('filters'); setWorkspace('atlas');
      setFamily('settings'); setScenario('General'); setSection('general'); setSettingsOpen(true); setQuery(''); setSettingsQuery(''); setReceipt(initialReceipt);
    }
  }
  function requestReset(target: 'preferences' | 'all') { settingsGate.invalidate(); setOverlay({ kind: 'reset', target, version: ++revision.current }); }

  function renderDialog(): ReactNode {
    if (!overlay) return null;
    if (overlay.kind === 'reset') {
      const all = overlay.target === 'all';
      const areas = all ? [] : changedPreferenceAreas(preferences, !opaque, defaultModel);
      return <DialogFrame title={all ? 'Reset the review?' : 'Reset preferences?'} onDismiss={close} returnFocus={backToChat}>
        <Confirmation label={all ? 'Reset review' : 'Reset preferences'} onCancel={close} onConfirm={() => reset(overlay.target)}>
          {areas.length ? <dl className={styles.facts}>{areas.map((entry) => <Fragment key={entry.area}><dt>{entry.area}</dt><dd>{entry.detail}</dd></Fragment>)}</dl> : null}
          <p className={styles.lead}>{all ? 'Clears your subagents, model choices, chat title, project and preferences, and returns to Settings.' : 'These go back to their defaults. Subagents, this chat’s model and the chat title stay as they are.'}</p>
        </Confirmation>
      </DialogFrame>;
    }
    const active = overlay;
    const identity = { id: `review-${active.kind}`, version: active.version };
    const controls = <><ScenarioControl family={active.kind} scenario={active.scenario} onChange={(value) => { if (hostGate.valid()) changeCase(active.kind, value); }} onReset={() => {
      if (!hostGate.valid()) return;
      setQuery(active.scenario === 'no-match' ? noMatch : '');
      setCaseResetVersion((version) => version + 1);
      setReceipt(initialReceipt);
      setOverlay({ ...active, version: ++revision.current });
    }} /><p className={styles.receipt} role="status">{receipt}</p></>;
    const chrome = { onDismiss: close, returnFocus: backToChat, toolbar: controls };
    if (active.kind === 'models') {
      const view: ModelPickerView = { ...identity, state: listState(active.scenario), scope: active.scope, currentId: currentModel, defaultId: defaultModel, currentReasoning: reasoning.current, defaultReasoning: reasoning.default, choices: modelCase(active.scenario) };
      return <ModelDialog {...chrome} view={view} chatTitle={title} query={query} onQueryChange={setQuery} onRetry={() => { if (hostGate.claim()) changeCase('models', 'ready'); }} providers={providers}
        onConnectProvider={(providerId) => { if (!hostGate.claim()) return; const provider = providerId ? providers.find((entry) => entry.id === providerId) : undefined; if (provider) open('providers', connectStart(provider).step, provider.id); else navigate('providers'); }}
        onSelect={(intent) => {
          if (!validModelSelection(view, intent) || !hostGate.claim()) return;
          if (intent.scope === 'current-chat') { setCurrentModel(intent.modelId); setReasoning({ ...reasoning, current: intent.reasoning }); }
          else { setDefaultModel(intent.modelId); setReasoning({ ...reasoning, default: intent.reasoning }); }
          finish(intent.scope === 'current-chat' ? 'model changed for this chat' : 'default model changed');
        }} />;
    }
    if (active.kind === 'providers') {
      const provider = providers.find((entry) => entry.id === active.providerId)!;
      const step = active.scenario === 'externally-managed' ? 'method' : active.scenario as ConnectView['step'];
      const view: ConnectView = { ...identity, provider, step, method: active.method };
      function connect(intent: ConnectIntent) {
        if (!validConnectIntent(view, intent) || !hostGate.claim()) return;
        if (intent.action === 'disconnect') { finish(`${provider.label} disconnected`); return; }
        // A finished connection hands straight over to the model picker.
        if (intent.action === 'choose-model') { receive(`${provider.label} connected`); open('models', 'ready'); return; }
        let next = active.scenario;
        let method = active.method;
        if (intent.action === 'choose-method') { method = intent.method; next = method; }
        else if (intent.action === 'submit-demo-key' || intent.action === 'show-waiting') next = 'waiting';
        else if (intent.action === 'show-success') next = 'success';
        else if (intent.action === 'show-error') next = 'error';
        else if (intent.action === 'retry') next = method ?? 'method';
        else if (intent.action === 'back') next = active.scenario === 'waiting' ? method ?? 'method' : 'method';
        setOverlay({ ...active, scenario: next, method, version: ++revision.current });
        if (family === 'providers') setScenario(next);
      }
      // Review cases hold each step still; the scenario picker drives outcomes, not timers.
      return <ProviderConnectDialog {...chrome} view={view} onIntent={connect} autoAdvance={false} />;
    }
    if (active.kind === 'sessions' || active.kind === 'commands') {
      let items = active.kind === 'sessions' ? sessions.map((entry) => entry.id === 'filters' ? { ...entry, label: title } : entry) : commandItems(preferences.shortcuts);
      if (active.scenario === 'empty' || active.scenario === 'loading') items = [];
      if (active.scenario === 'unknown') items = [{ id: 'unbound', label: 'Unrecognized item', detail: 'AVA doesn’t know how to open this.' }];
      const view: FinderView = { ...identity, kind: active.kind, state: listState(active.scenario), items, projects };
      return <FinderDialog {...chrome} view={view} query={query} onQueryChange={setQuery} onRetry={() => { if (hostGate.claim()) changeCase(active.kind, 'ready'); }} onSelect={(intent) => {
        if (!validFinderIntent(view, intent) || !hostGate.claim()) return;
        const item = view.items.find((entry) => entry.id === intent.itemId)!;
        if (active.kind === 'sessions') { setSelectedSession(item.id); finish(`opened “${item.label}”`); }
        else if (item.command === 'shortcuts') navigate('settings', 'Shortcuts');
        else if (item.command && isFamily(item.command)) navigate(item.command);
        else finish(`ran “${item.label.replace(/…$/, '')}”`);
      }} />;
    }
    const actionView: ActionView = { ...identity, title, state: listState(active.scenario) };
    if (active.kind === 'rename') {
      const view = { ...actionView, state: active.scenario === 'empty' ? 'ready' as const : actionView.state, title: active.scenario === 'empty' ? '' : active.scenario === 'too-long' ? 'A'.repeat(257) : title };
      return <RenameDialog {...chrome} view={view} onRename={(intent) => { if (!validRename(view, intent) || !hostGate.claim()) return; setTitle(intent.title); finish('chat renamed'); }} />;
    }
    if (active.kind === 'archive' || active.kind === 'branch') return <ChatActionDialog {...chrome} action={active.kind} view={actionView} points={active.scenario === 'empty' ? [] : branchPoints}
      onGoToChat={() => { if (hostGate.claim()) finish('opened the chat to stop its run'); }}
      onIntent={(intent) => {
        if (intent.id !== identity.id || intent.version !== identity.version || intent.action !== active.kind || actionView.state !== 'ready' || (intent.action === 'branch' && !branchPoints.some((point) => point.id === intent.pointId)) || !hostGate.claim()) return;
        finish(intent.action === 'archive' ? 'chat archived' : 'branch created');
      }} />;
    if (active.kind === 'workspace') {
      const view: WorkspaceView = { ...identity, state: active.scenario as WorkspaceState, selectedId: workspace, choices: workspaceChoices };
      return <WorkspaceDialog {...chrome} view={view} onIntent={(intent) => {
        if (!validWorkspaceIntent(view, intent) || !hostGate.claim()) return;
        const current = workspaceChoices.find((choice) => choice.id === workspace)?.label ?? 'the project';
        if (intent.action === 'use') { finish(`opened ${current}`); return; }
        // The project already open needs no confirmation: choosing it just closes.
        if (intent.action === 'choose' && intent.workspaceId === workspace) { finish(`stayed in ${current}`); return; }
        if (intent.action === 'choose' && intent.workspaceId) setWorkspace(intent.workspaceId);
        const next: WorkspaceState = intent.action === 'change' ? 'choice' : intent.action === 'cancel' ? 'canceled' : 'opening';
        setOverlay({ ...active, scenario: next, version: ++revision.current }); if (family === 'workspace') setScenario(next);
      }} />;
    }
    if (active.kind === 'message') return <MessageDialog {...chrome} view={{ role: active.scenario === 'unknown' ? 'unknown' : active.scenario === 'user' ? 'user' : 'assistant', state: active.scenario === 'failed' || active.scenario === 'canceled' ? active.scenario : 'committed', text: active.scenario === 'empty' ? null : active.scenario === 'user' ? 'Make the search filters easier to scan.' : 'I’d group the filters by purpose and keep the keyboard order.\n\n<b>Markup</b> in a message stays plain text.' }} />;
    if (active.kind === 'tool') return <ToolDialog {...chrome} records={toolCase(active.scenario)} />;
    if (active.kind === 'permission' || active.kind === 'question') {
      const onIntent = (intent: RequestReceipt) => {
        if (intent.id !== identity.id || intent.version !== identity.version || (active.kind === 'permission' ? intent.kind !== 'permission' : !['question-answer', 'question-cancel'].includes(intent.kind)) || !hostGate.claim()) return;
        receive(intent.kind === 'permission' ? intent.decision ? decisions[intent.decision] : 'decision sent' : intent.kind === 'question-cancel' ? 'question declined' : 'question answered');
      };
      if (active.kind === 'permission') return <PermissionDialog {...chrome} request={permissionCase(active.scenario, active.version)} onIntent={onIntent} />;
      // A question that failed to load offers Retry, which loads it again as a fresh, answerable question.
      return <QuestionDialog {...chrome} request={questionCase(active.scenario, active.version)} onIntent={onIntent}
        onRetry={active.scenario === 'error' ? () => { if (!hostGate.claim()) return; changeCase('question', 'single'); receive('question reloaded'); } : undefined} />;
    }
    return <RunDialog key={active.version} {...chrome} view={runCase(active.scenario)} onRetry={() => { if (hostGate.claim()) receive('retry requested'); }} />;
  }

  const ruleState = scenario.startsWith('Rules / ') ? scenario === 'Rules / denied' ? 'disabled' : listState(scenario.slice(8)) : 'ready';
  const errorSection = Object.hasOwn(inspectionErrorSections, scenario) ? inspectionErrorSections[scenario] : null;
  const inspections = errorSection ? { ...resourceInspections, [errorSection]: { ...resourceInspections[errorSection], state: 'error' as const } } : resourceInspections;
  const modelLabel = (id: string) => modelChoices.find((choice) => choice.id === id)?.label ?? 'Unknown';
  return <ReducedMotionContext.Provider value={preferences.motion === 'reduced'}><main className={`${styles.workspace} ${appearance.scope}`} data-code-wrap={preferences.conversation?.wrapCode ?? false} data-diff-wrap={preferences.conversation?.wrapDiff ?? true} data-palette={preferences.palette} data-ui-font={preferences.uiFont} data-prose-size={preferences.proseSize} data-prose-spacing={preferences.proseSpacing} data-reading-width={preferences.readingWidth} data-contrast={preferences.contrast} data-material={family === 'settings' && settingsOpen && section === 'appearance' && !opaque ? 'glass' : 'opaque'} data-density={preferences.density} data-motion={preferences.motion} aria-label="Screen review workspace">
    <header className={styles.header}><div className={styles.brand}><AvaMark /><span>AVA / Screen review</span></div><div className={styles.inline}><BackLink ref={backToChat} href="?">Back to chat</BackLink><Button onClick={() => requestReset('all')}>Reset review</Button></div></header>
    <div className={styles.layout}><nav className={styles.index} aria-label="Screen families"><h2>Screens</h2>{familyIds.map((id) => <button key={id} className={styles.navButton} aria-current={id === family ? 'page' : undefined} onClick={() => navigate(id)}><span>{reviewFamilies[id].label}</span><small>{reviewFamilies[id].cases.length}</small></button>)}</nav>
      <div className={styles.main} data-settings-open={family === 'settings' && settingsOpen || undefined}><div className={styles.content}>
        <ScenarioControl family={family} scenario={scenario} onChange={(value) => changeCase(family, value)} onReset={() => { setQuery(''); setCaseResetVersion((version) => version + 1); changeCase(family, scenario); }} />
        {preferences.help ? <p className={styles.reviewNote}>Review only: every screen here uses local examples, and cases change only when you pick them. Nothing is saved, sent or run.</p> : null}
        {!overlay ? <p className={styles.receipt} role="status">{receipt}</p> : null}
        {family === 'settings' && settingsOpen ? <SettingsScreen key={resetVersion} active={!overlay} section={section} onSectionChange={(value) => { setSection(value); setScenario(settingsSections.find((entry) => entry.id === value)!.label); }} query={settingsQuery} onQueryChange={setSettingsQuery} preferences={preferences} onPreferencesChange={(value) => { if (family === 'settings' && settingsOpen && !overlay && settingsGate.valid()) setPreferences(value); }} opaque={opaque} appearanceResetOpaque onOpaqueChange={(value) => { if (family === 'settings' && settingsOpen && !overlay && settingsGate.valid() && typeof value === 'boolean') setOpaque(value); }} defaultModel={modelLabel(defaultModel)} defaultModelId={defaultModel} onChooseModel={(scope) => open('models', 'ready', undefined, scope)} providers={providers} onConnect={showProvider} onDisconnect={(id) => showProvider(id, true)} onReset={() => requestReset('preferences')} onBack={() => { settingsGate.invalidate(); setSettingsOpen(false); }}
          onRetryInspection={() => { if (settingsGate.valid() && (scenario.startsWith('Rules / ') || errorSection)) changeCase('settings', errorSection ? 'Extensions' : 'Permissions & trust'); }}
          projects={supplied.projects} contextUsage={previewContextUsage({ ...supplied.chats.find((chat) => chat.id === supplied.activeChatId)!, model: currentModel })} project={{ name: 'Atlas', trusted: true }}
          rules={{ state: ruleState, rows: [{ title: 'Read files in notes/', detail: 'Saved rule · applies to every chat in Atlas', status: 'Always allow' }, { title: 'Run npm test', detail: 'This session only · ends when AVA restarts', status: 'Session' }] }}
          inspections={inspections} subagents={definitions.workspace} onSaveSubagent={definitions.save} /> : <>
          <h1 className={styles.pageTitle}>{reviewFamilies[family].label}</h1><p className={styles.lead}>{familyDescriptions[family]}</p>
          {family === 'providers' ? <>{scenario === 'directory-error' ? <ListNotice state="error" message="Couldn’t refresh provider status. Showing the last known list." onRetry={() => changeCase('providers', 'directory')} /> : null}<ProviderDirectory active={!overlay} providers={providers} onConnect={showProvider} onDisconnect={(id) => showProvider(id, true)} /></> : <section className={styles.stage}><h2>{family === 'settings' ? 'Settings closed' : 'Current state'}</h2>
            <dl className={styles.facts}><dt>Chat title</dt><dd>{title}</dd><dt>Open chat</dt><dd>{selectedSession === 'filters' ? title : sessions.find((entry) => entry.id === selectedSession)?.label ?? 'Unknown'}</dd><dt>This chat / new chats</dt><dd>{modelLabel(currentModel)} / {modelLabel(defaultModel)}</dd><dt>Project</dt><dd>{workspaceChoices.find((choice) => choice.id === workspace)?.label ?? 'Unknown'}</dd></dl>
            <Button tone="primary" onClick={() => family === 'settings' ? setSettingsOpen(true) : open(family, scenario)}>{family === 'settings' ? 'Open settings' : 'Show dialog'}</Button>
          </section>}
        </>}
      </div></div>
    </div>
    <Fragment key={caseResetVersion}>{renderDialog()}</Fragment>
  </main></ReducedMotionContext.Provider>;
}
