import { Fragment, useRef, useState, type ReactNode } from 'react';
import { createInitialState } from '../state/fixtures';
import { previewContextUsage } from '../components/contextPreview';
import type { ModelId } from '../state/types';
import { AvaMark } from '../components/AvaMark';
import { Button } from '../ui/Button';
import { DialogFrame } from '../ui/DialogFrame';
import { ReducedMotionContext } from '../ui/useReducedMotion';
import appearance from '../styles/appearance.module.css';
import type { ActionView, ConnectIntent, ConnectMethod, ConnectView, FinderView, ModelPickerView, ModelScope, Preferences, RequestReceipt, SettingsSection, WorkspaceState, WorkspaceView } from './contracts';
import { ChatActionDialog, MessageDialog, PermissionDialog, QuestionDialog, RenameDialog, RunDialog, ToolDialog, validRename, validWorkspaceIntent, WorkspaceDialog } from './ChatDialogs';
import { FinderDialog, validFinderIntent } from './FinderDialog';
import { ModelDialog, validModelSelection } from './ModelDialog';
import { ProviderConnectDialog, ProviderDirectory, validConnectIntent } from './ProviderViews';
import { Confirmation } from './ScreenPrimitives';
import { SettingsScreen, settingsSections } from './SettingsScreen';
import { resourceInspections } from './screenExamples';
import { useSubagentWorkspace } from './subagentWorkspace';
import { branchPoints, commands, defaultPreferences, familyIds, inspectionErrorSections, isFamily, isScenario, listState, modelCase, modelChoices, permissionCase, projects, providers, questionCase, reviewFamilies, runCase, sessions, toolCase, workspaceChoices, type ReviewFamily } from './fixtures';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';

type DialogFamily = Exclude<ReviewFamily, 'settings'>;
type ScreenOverlay = { kind: DialogFamily; scenario: string; version: number; providerId: string; method: ConnectMethod | null; scope: ModelScope };
type Overlay = ScreenOverlay | { kind: 'reset'; target: 'preferences' | 'all'; version: number };
const isDirectory = (scenario: string) => scenario === 'directory' || scenario === 'directory-error';
const initialReceipt = 'No local intent recorded.';
const familyDescriptions: Record<ReviewFamily, string> = {
  settings: 'Sixteen settings pages. Preferences and user definition metadata are local; connection and resource inspection is read-only.',
  models: 'One shared selector for current-chat and new-chat defaults, with provider groups and fictional profile facts.',
  providers: 'Real provider identities, with separate fictional sign-in examples. No account, real keys, browser launch or credential storage.',
  sessions: 'Project-filtered example chats. Opening an example only changes the review selection.',
  commands: 'A small registry of known UI actions. Search text is never executed.',
  rename: 'A validated title form, bounded to 256 characters. Only the review title changes.',
  archive: 'Archive is an unwired RPC-v1 intent preview. The original chat is preserved.',
  branch: 'Select an example message as a branch point. No session, Git branch or worktree is created.',
  workspace: 'Workspace choice and host-recovery fixtures. No directory or process authority.',
  message: 'Escaped, read-only message projections. No raw provider payload or private reasoning.',
  tool: 'Supplied tool lifecycle and output detail. Nothing is executed or fetched.',
  permission: 'Explicit advertised decisions only. Dismissal leaves the request unresolved.',
  question: 'Native choices, custom and secret demo answers, with request-lifetime guards.',
  run: 'Error, retry, cancel-requested and terminal states are distinct, manually supplied fixtures.',
};

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

  function nextOverlay(kind: DialogFamily, value: string, providerId?: string, scope: ModelScope = 'current-chat'): ScreenOverlay {
    const id = providerId ?? (value === 'disconnect' ? 'orion-lab' : value === 'externally-managed' ? 'cedar-example' : 'aster-lab');
    const method = value === 'key' || value === 'browser' || value === 'device' ? value : ['waiting', 'success', 'error', 'canceled'].includes(value) ? id === 'aster-lab' ? 'browser' : 'key' : null;
    return { kind, scenario: value, version: ++revision.current, providerId: id, method, scope };
  }
  function open(kind: DialogFamily, value = reviewFamilies[kind].cases[0] as string, providerId?: string, scope?: ModelScope) {
    if (!isScenario(kind, value)) return;
    settingsGate.invalidate();
    setQuery(value === 'no-match' ? 'no matching example' : '');
    setOverlay(nextOverlay(kind, value, providerId, scope));
  }
  function settingsCase(value: string) {
    setSettingsOpen(true);
    const found = settingsSections.find((entry) => entry.label === value);
    setSection(found?.id ?? (value.startsWith('Rules') ? 'permissions' : Object.hasOwn(inspectionErrorSections, value) ? inspectionErrorSections[value] : 'general'));
    setSettingsQuery(value === 'Search / no match' ? 'no matching example' : '');
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
    if (kind !== 'models' || value === 'no-match') setQuery(value === 'no-match' ? 'no matching example' : '');
    const previous = overlay && overlay.kind === kind ? overlay : null;
    setOverlay(nextOverlay(kind, value, kind === 'providers' ? undefined : previous?.providerId, previous?.scope));
  }
  function close() {
    if (!hostGate.valid()) return;
    hostGate.invalidate();
    if (overlay?.kind === 'permission' || overlay?.kind === 'question') setReceipt('Dismissed locally · request unresolved. No approval or answer was sent.');
    else if (overlay?.kind === 'providers') setReceipt('Connection view closed · dummy input cleared. No credential was stored or removed.');
    setOverlay(null);
  }
  function receive(message: string) { setReceipt(`${message} · local review intent only, not backend settlement.`); }
  function showProvider(id: string, disconnect = false) {
    const provider = providers.find((entry) => entry.id === id);
    if (!provider || (disconnect ? provider.source !== 'stored-example' : provider.state === 'external' || !provider.methods.length)) return;
    open('providers', disconnect ? 'disconnect' : 'method', id);
  }
  function reset(target: 'preferences' | 'all') {
    if (!hostGate.claim()) return;
    hostGate.invalidate();
    setPreferences(defaultPreferences); setOpaque(true); setResetVersion((value) => value + 1); setOverlay(null);
    setReceipt('Review preferences reset. No configuration was written.');
    if (target === 'all') {
      definitions.reset();
      setCurrentModel('aster'); setDefaultModel('aster-mini'); setReasoning({ current: 'Low', default: null }); setTitle('Refine search filters'); setSelectedSession('filters'); setWorkspace('atlas');
      setFamily('settings'); setScenario('General'); setSection('general'); setSettingsOpen(true); setQuery(''); setSettingsQuery(''); setReceipt(initialReceipt);
    }
  }
  function requestReset(target: 'preferences' | 'all') { settingsGate.invalidate(); setOverlay({ kind: 'reset', target, version: ++revision.current }); }

  function renderDialog(): ReactNode {
    if (!overlay) return null;
    if (overlay.kind === 'reset') return <DialogFrame title={overlay.target === 'all' ? 'Reset the screen review?' : 'Reset review preferences?'} description="Only local review values are reset. Main-chat state and real configuration are untouched." onDismiss={close} returnFocus={backToChat}><Confirmation label="Reset local examples" onCancel={close} onConfirm={() => reset(overlay.target)}><p className={styles.lead}>This clears {overlay.target === 'all' ? 'the user definitions, selected models, practice title, workspace and preferences' : 'the practice preferences'}. Refresh also starts a fresh review.</p></Confirmation></DialogFrame>;
    const active = overlay;
    const identity = { id: `review-${active.kind}`, version: active.version };
    const controls = <><ScenarioControl family={active.kind} scenario={active.scenario} onChange={(value) => { if (hostGate.valid()) changeCase(active.kind, value); }} onReset={() => {
      if (!hostGate.valid()) return;
      setQuery(active.scenario === 'no-match' ? 'no matching example' : '');
      setCaseResetVersion((version) => version + 1);
      setReceipt(initialReceipt);
      setOverlay({ ...active, version: ++revision.current });
    }} /><p className={styles.receipt} role="status">{receipt}</p></>;
    const chrome = { onDismiss: close, returnFocus: backToChat, toolbar: controls };
    if (active.kind === 'models') {
      const view: ModelPickerView = { ...identity, state: listState(active.scenario), scope: active.scope, currentId: currentModel, defaultId: defaultModel, currentReasoning: reasoning.current, defaultReasoning: reasoning.default, choices: modelCase(active.scenario) };
      return <ModelDialog {...chrome} view={view} query={query} onQueryChange={setQuery} onRetry={() => { if (hostGate.claim()) changeCase('models', 'ready'); }} onSelect={(intent) => {
        if (!validModelSelection(view, intent) || !hostGate.claim()) return;
        if (intent.scope === 'current-chat') { setCurrentModel(intent.modelId); setReasoning({ ...reasoning, current: intent.reasoning }); }
        else { setDefaultModel(intent.modelId); setReasoning({ ...reasoning, default: intent.reasoning }); }
        receive('Model choice'); hostGate.invalidate(); setOverlay(null);
      }} />;
    }
    if (active.kind === 'providers') {
      const provider = providers.find((entry) => entry.id === active.providerId)!;
      const step = active.scenario === 'externally-managed' ? 'method' : active.scenario as ConnectView['step'];
      const view: ConnectView = { ...identity, provider, step, method: active.method };
      function connect(intent: ConnectIntent) {
        if (!validConnectIntent(view, intent) || !hostGate.claim()) return;
        if (intent.action === 'disconnect') { receive('Stored-example disconnect'); hostGate.invalidate(); setOverlay(null); return; }
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
        receive('Connection example step');
      }
      return <ProviderConnectDialog {...chrome} view={view} onIntent={connect} />;
    }
    if (active.kind === 'sessions' || active.kind === 'commands') {
      let items = active.kind === 'sessions' ? sessions.map((entry) => entry.id === 'filters' ? { ...entry, label: title } : entry) : commands;
      if (active.scenario === 'empty' || active.scenario === 'loading') items = [];
      if (active.scenario === 'unknown') items = [{ id: 'unbound', label: 'Unknown example choice', detail: 'No host binding supplied.' }];
      const view: FinderView = { ...identity, kind: active.kind, state: listState(active.scenario), items, projects };
      return <FinderDialog {...chrome} view={view} query={query} onQueryChange={setQuery} onRetry={() => { if (hostGate.claim()) changeCase(active.kind, 'ready'); }} onSelect={(intent) => {
        if (!validFinderIntent(view, intent) || !hostGate.claim()) return;
        const item = view.items.find((entry) => entry.id === intent.itemId)!;
        if (active.kind === 'commands' && item.command) navigate(item.command);
        else { setSelectedSession(item.id); receive('Example session selection'); hostGate.invalidate(); setOverlay(null); }
      }} />;
    }
    const actionView: ActionView = { ...identity, title, state: listState(active.scenario) };
    if (active.kind === 'rename') {
      const view = { ...actionView, state: active.scenario === 'empty' ? 'ready' as const : actionView.state, title: active.scenario === 'empty' ? '' : active.scenario === 'too-long' ? 'A'.repeat(257) : title };
      return <RenameDialog {...chrome} view={view} onRename={(intent) => { if (!validRename(view, intent) || !hostGate.claim()) return; setTitle(intent.title); receive('Example title change'); hostGate.invalidate(); setOverlay(null); }} />;
    }
    if (active.kind === 'archive' || active.kind === 'branch') return <ChatActionDialog {...chrome} action={active.kind} view={actionView} points={active.scenario === 'empty' ? [] : branchPoints} onIntent={(intent) => {
      if (intent.id !== identity.id || intent.version !== identity.version || intent.action !== active.kind || actionView.state !== 'ready' || (intent.action === 'branch' && !branchPoints.some((point) => point.id === intent.pointId)) || !hostGate.claim()) return;
      receive(intent.action === 'archive' ? 'Archive preview · original preserved; RPC v1 unwired' : 'Branch preview · original preserved'); hostGate.invalidate(); setOverlay(null);
    }} />;
    if (active.kind === 'workspace') {
      const view: WorkspaceView = { ...identity, state: active.scenario as WorkspaceState, selectedId: workspace, choices: workspaceChoices };
      return <WorkspaceDialog {...chrome} view={view} onIntent={(intent) => {
        if (!validWorkspaceIntent(view, intent) || !hostGate.claim()) return;
        if (intent.action === 'choose' && intent.workspaceId) setWorkspace(intent.workspaceId);
        const next = intent.action === 'show-ready' ? 'ready' : intent.action === 'show-error' ? 'error' : intent.action === 'cancel' ? 'canceled' : 'opening';
        setOverlay({ ...active, scenario: next, version: ++revision.current }); if (family === 'workspace') setScenario(next); receive('Workspace example step');
      }} />;
    }
    if (active.kind === 'message') return <MessageDialog {...chrome} view={{ role: active.scenario === 'unknown' ? 'unknown' : active.scenario === 'user' ? 'user' : 'assistant', state: active.scenario === 'failed' || active.scenario === 'canceled' ? active.scenario : 'committed example', text: active.scenario === 'empty' ? null : active.scenario === 'user' ? 'Make the search filters easier to scan.' : 'I would group the filters by purpose and preserve keyboard order.\n\n<example>Escaped text, not an executable payload.</example>' }} />;
    if (active.kind === 'tool') return <ToolDialog {...chrome} records={toolCase(active.scenario)} />;
    if (active.kind === 'permission' || active.kind === 'question') {
      const onIntent = (intent: RequestReceipt) => {
        if (intent.id !== identity.id || intent.version !== identity.version || (active.kind === 'permission' ? intent.kind !== 'permission' : !['question-answer', 'question-cancel'].includes(intent.kind)) || !hostGate.claim()) return;
        receive(intent.kind === 'permission' ? 'Explicit permission choice' : intent.kind === 'question-cancel' ? 'Question cancellation · unresolved, not answered' : 'Question answer · custom and secret values excluded');
      };
      return active.kind === 'permission' ? <PermissionDialog {...chrome} request={permissionCase(active.scenario, active.version)} onIntent={onIntent} /> : <QuestionDialog {...chrome} request={questionCase(active.scenario, active.version)} onIntent={onIntent} />;
    }
    return <RunDialog key={active.version} {...chrome} view={runCase(active.scenario)} onRetry={() => { if (hostGate.claim()) receive('Retry request'); }} />;
  }

  const ruleState = scenario.startsWith('Rules / ') ? scenario === 'Rules / denied' ? 'disabled' : listState(scenario.slice(8)) : 'ready';
  const errorSection = Object.hasOwn(inspectionErrorSections, scenario) ? inspectionErrorSections[scenario] : null;
  const inspections = errorSection ? { ...resourceInspections, [errorSection]: { ...resourceInspections[errorSection], state: 'error' as const } } : resourceInspections;
  const modelLabel = (id: string) => modelChoices.find((choice) => choice.id === id)?.label ?? 'Unknown';
  return <ReducedMotionContext.Provider value={preferences.motion === 'reduced'}><main className={`${styles.workspace} ${appearance.scope}`} data-code-wrap={preferences.conversation?.wrapCode ?? false} data-diff-wrap={preferences.conversation?.wrapDiff ?? true} data-palette={preferences.palette} data-ui-font={preferences.uiFont} data-prose-size={preferences.proseSize} data-prose-spacing={preferences.proseSpacing} data-reading-width={preferences.readingWidth} data-contrast={preferences.contrast} data-material={family === 'settings' && settingsOpen && section === 'appearance' && !opaque ? 'glass' : 'opaque'} data-density={preferences.density} data-motion={preferences.motion} aria-label="Screen review workspace">
    <header className={styles.header}><div className={styles.brand}><AvaMark /><span>AVA / Screen review</span></div><div className={styles.inline}><a ref={backToChat} href="?">Back to chat</a><Button onClick={() => requestReset('all')}>Reset review</Button></div></header>
    <div className={styles.layout}><nav className={styles.index} aria-label="Screen families"><h2>Everyday screens</h2>{familyIds.map((id) => <button key={id} className={styles.navButton} aria-current={id === family ? 'page' : undefined} onClick={() => navigate(id)}><span>{reviewFamilies[id].label}</span><small>{reviewFamilies[id].cases.length}</small></button>)}</nav>
      <div className={styles.main} data-settings-open={family === 'settings' && settingsOpen || undefined}><div className={styles.content}>
        <ScenarioControl family={family} scenario={scenario} onChange={(value) => changeCase(family, value)} onReset={() => { setQuery(''); setCaseResetVersion((version) => version + 1); changeCase(family, scenario); }} />
        {preferences.help ? <p className={styles.reviewNote}>Review-only fixtures. No connection, configuration, tool execution or persistence. Scenarios advance only by your actions.</p> : null}
        {!overlay ? <p className={styles.receipt} role="status">{receipt}</p> : null}
        {family === 'settings' && settingsOpen ? <SettingsScreen key={resetVersion} active={!overlay} section={section} onSectionChange={(value) => { setSection(value); setScenario(settingsSections.find((entry) => entry.id === value)!.label); }} query={settingsQuery} onQueryChange={setSettingsQuery} preferences={preferences} onPreferencesChange={(value) => { if (family === 'settings' && settingsOpen && !overlay && settingsGate.valid()) setPreferences(value); }} opaque={opaque} appearanceResetOpaque onOpaqueChange={(value) => { if (family === 'settings' && settingsOpen && !overlay && settingsGate.valid() && typeof value === 'boolean') setOpaque(value); }} currentModel={modelLabel(currentModel)} defaultModel={modelLabel(defaultModel)} onChooseModel={(scope) => open('models', 'ready', undefined, scope)} providers={providers} onConnect={showProvider} onDisconnect={(id) => showProvider(id, true)} onReset={() => requestReset('preferences')} onBack={() => { settingsGate.invalidate(); setSettingsOpen(false); }}
          projects={supplied.projects} contextUsage={previewContextUsage({ ...supplied.chats.find((chat) => chat.id === supplied.activeChatId)!, model: currentModel })}
          rules={{ state: ruleState, rows: [{ title: 'Workspace trust', detail: 'Atlas example · project resource trust is not evaluated by this UI.', status: 'Read-only / unknown' }, { title: 'Persistent rule', detail: 'Example read rule · durable policy scope, not a session grant.', status: 'Inspect only' }, { title: 'Session grant', detail: 'Example process-local read grant · ends with its owning process.', status: 'Session only' }] }}
          inspections={inspections} subagents={definitions.workspace} onSaveSubagent={definitions.save} /> : <>
          <h1 className={styles.pageTitle}>{reviewFamilies[family].label}</h1><p className={styles.lead}>{familyDescriptions[family]}</p>
          {family === 'providers' ? <>{scenario === 'directory-error' ? <p className={styles.notice} role="status">Provider status error example. Last supplied directory retained; no refresh request is running.</p> : null}<ProviderDirectory active={!overlay} providers={providers} onConnect={showProvider} onDisconnect={(id) => showProvider(id, true)} /></> : <section className={styles.stage}><h2>{family === 'settings' ? 'Preferences and scopes' : 'Current review state'}</h2>
            <dl className={styles.facts}><dt>Example chat</dt><dd>{title}</dd><dt>Selected session</dt><dd>{selectedSession === 'filters' ? 'Current example chat' : sessions.find((entry) => entry.id === selectedSession)?.label ?? 'Unknown'}</dd><dt>Current / default model</dt><dd>{modelLabel(currentModel)} / {modelLabel(defaultModel)}</dd></dl>
            <Button tone="primary" onClick={() => family === 'settings' ? setSettingsOpen(true) : open(family, scenario)}>Open {family === 'settings' ? 'settings' : reviewFamilies[family].label.toLocaleLowerCase()}</Button>
          </section>}
        </>}
      </div></div>
    </div>
    <Fragment key={caseResetVersion}>{renderDialog()}</Fragment>
  </main></ReducedMotionContext.Provider>;
}
