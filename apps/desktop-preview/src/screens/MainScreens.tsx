import { useRef, useState, type RefObject } from 'react';
import type { ContextUsageView } from '../chat/contextUsage';
import type { ModelId, Project } from '../state/types';
import { DialogFrame } from '../ui/DialogFrame';
import { Button } from '../ui/Button';
import type { ConnectIntent, ConnectView, MainScreenRequest, ModelPickerView, ModelScope, ModelSelection, Preferences, SettingsSection } from './contracts';
import { defaultPreferences, modelChoices, providers, resourceInspections } from './screenExamples';
import { useSubagentWorkspace } from './subagentWorkspace';
import { ModelDialog, validModelSelection } from './ModelDialog';
import { ProviderConnectDialog, validConnectIntent } from './ProviderViews';
import { Confirmation } from './ScreenPrimitives';
import { SettingsScreen } from './SettingsScreen';
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
}
type Overlay = { kind: 'models'; scope: ModelScope; version: number } | { kind: 'provider'; view: ConnectView } | { kind: 'reset'; version: number };
const readOnlyReason = 'Read-only child inspection. Return to the parent before changing its model.';
const modelLabel = (id: ModelId) => modelChoices.find((choice) => choice.id === id)?.label ?? 'Unknown';

// A lazy, local presentation host. No ScreenReview controller, routing, RPC or provider state.
export default function MainScreens({ request, parent, defaultModel, preferences, opaque, onOpaqueChange, section, onSectionChange, onPreferencesChange, onSelectModel, onDismiss, returnFocus, workspaceSeed, onWorkspaceSnapshot, projects, contextUsage }: MainScreensProps) {
  const [settingsQuery, setSettingsQuery] = useState('');
  const [modelQuery, setModelQuery] = useState('');
  const [receipt, setReceipt] = useState('');
  const [overlay, setOverlay] = useState<Overlay | null>(request.kind === 'models' ? { kind: 'models', scope: 'current-chat', version: 1 } : null);
  const revision = useRef(1);
  const composing = useRef(false);
  const rootGate = useViewGuard(JSON.stringify(request));
  const overlayGate = useViewGuard(JSON.stringify(overlay));
  const readOnly = request.inspectedChildId !== null;
  const definitions = useSubagentWorkspace(workspaceSeed, request.kind === 'settings' && section === 'subagents' && !overlay, (snapshot) => { if (rootGate.valid()) onWorkspaceSnapshot?.(snapshot); });

  function dismissRoot() {
    if (!rootGate.valid()) return;
    rootGate.invalidate(); overlayGate.invalidate(); onDismiss();
  }
  function finishOverlay() {
    overlayGate.invalidate();
    if (request.kind === 'models') dismissRoot(); else setOverlay(null);
  }
  function closeOverlay() { if (rootGate.valid() && overlayGate.valid()) finishOverlay(); }
  function open(next: Overlay) {
    if (!rootGate.valid() || overlay || !overlayGate.claim()) return;
    overlayGate.invalidate(); setReceipt(''); setModelQuery(''); setOverlay(next);
  }
  function chooseModel(scope: ModelScope) {
    if (scope === 'current-chat' && readOnly) return;
    open({ kind: 'models', scope, version: ++revision.current });
  }
  function showProvider(id: string, disconnect = false) {
    const provider = providers.find((entry) => entry.id === id);
    if (!provider || (disconnect ? provider.source !== 'stored-example' : provider.state === 'external' || !provider.methods.length)) return;
    open({ kind: 'provider', view: { id: `${request.id}:provider`, version: ++revision.current, provider, step: disconnect ? 'disconnect' : 'method', method: null } });
  }
  function renderOverlay() {
    if (!overlay) return null;
    const chrome = { onDismiss: closeOverlay, returnFocus };
    if (overlay.kind === 'models') {
      const view: ModelPickerView = { id: `${request.id}:models`, version: overlay.version, scope: overlay.scope, state: readOnly && overlay.scope === 'current-chat' ? 'disabled' : 'ready', currentId: parent.model, defaultId: defaultModel, choices: modelChoices, reasoningEditable: false };
      return <ModelDialog {...chrome} view={view} query={modelQuery} onQueryChange={(value) => { if (overlayGate.valid()) setModelQuery(value); }} onRetry={() => {}}
        description={readOnly && overlay.scope === 'current-chat' ? readOnlyReason : overlay.scope === 'current-chat' ? `Choose a model for “${parent.title}”.` : 'Choose the model used by New chat; existing chats stay unchanged.'}
        onSelect={(intent) => {
          if (!rootGate.valid() || !validModelSelection(view, intent) || !overlayGate.claim()) return;
          onSelectModel({ ...intent, id: request.id, version: request.version });
          finishOverlay();
        }} />;
    }
    if (overlay.kind === 'provider') {
      const view = overlay.view;
      function connect(intent: ConnectIntent) {
        if (!rootGate.valid() || !validConnectIntent(view, intent) || !overlayGate.claim()) return;
        if (intent.action === 'disconnect') {
          setReceipt('Disconnect intent recorded locally. No credentials were removed; supplied provider status is unchanged.');
          finishOverlay(); return;
        }
        let { step, method } = view;
        if (intent.action === 'choose-method') { method = intent.method; step = method; }
        else if (intent.action === 'submit-demo-key' || intent.action === 'show-waiting') step = 'waiting';
        else if (intent.action === 'show-success') step = 'success';
        else if (intent.action === 'show-error') step = 'error';
        else if (intent.action === 'retry') step = method ?? 'method';
        else if (intent.action === 'back') step = step === 'waiting' ? method ?? 'method' : 'method';
        overlayGate.invalidate();
        setOverlay({ kind: 'provider', view: { ...view, step, method, version: ++revision.current } });
      }
      return <ProviderConnectDialog {...chrome} view={view} onIntent={connect} />;
    }
    return <DialogFrame {...chrome} title="Reset preferences?" description="Reset only these local preferences and the new-chat default. Existing chats and their models, drafts and messages are preserved.">
      <Confirmation label="Reset preferences and default" onCancel={closeOverlay} onConfirm={() => {
        if (!rootGate.valid() || !overlayGate.claim()) return;
        onPreferencesChange(defaultPreferences);
        onOpaqueChange(false);
        onSelectModel({ id: request.id, version: request.version, scope: 'new-chat-default', modelId: 'aster', reasoning: null });
        setReceipt('Preferences reset. New chats use Aster. Existing chats are unchanged; no configuration was written.');
        finishOverlay();
      }}><h2 className={styles.taskTitle}>Reset local preferences?</h2><p className={styles.lead}>Restore default appearance, glass sidebar, comfortable rows, system motion, Enter to send and Aster for new chats? Existing chats and drafts stay unchanged.</p></Confirmation>
    </DialogFrame>;
  }

  return <div className={styles.mainScreens} data-page={request.kind} data-density={preferences.density} data-motion={preferences.motion} onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }} onKeyDown={(event) => {
    if (event.defaultPrevented || event.key !== 'Escape' || request.kind !== 'settings' || overlay || !rootGate.valid() || event.target instanceof Element && event.target.closest('dialog')) return;
    event.preventDefault(); event.stopPropagation();
    if (composing.current || event.nativeEvent.isComposing || event.keyCode === 229) return;
    if (settingsQuery) setSettingsQuery(''); else if (section !== 'general') onSectionChange('general'); else dismissRoot();
  }}>
    {request.kind === 'settings' ? <div className={styles.settingsPage}>
      <header className={styles.settingsHeader}><h1>Settings</h1><Button onClick={() => { if (rootGate.valid() && !overlay) dismissRoot(); }}>Back to chat</Button></header>
      {receipt ? <p className={styles.receipt} role="status">{receipt}</p> : null}
      <SettingsScreen embedded active={!overlay} section={section} onSectionChange={(value) => { if (rootGate.valid() && !overlay) onSectionChange(value); }} query={settingsQuery} onQueryChange={(value) => { if (rootGate.valid() && !overlay) setSettingsQuery(value); }}
        opaque={opaque} onOpaqueChange={(value) => { if (rootGate.valid() && !overlay && overlayGate.valid()) onOpaqueChange(value); }}
        preferences={preferences} onPreferencesChange={(value) => { if (rootGate.valid() && !overlay && overlayGate.valid()) onPreferencesChange(value); }} currentModel={modelLabel(parent.model)} defaultModel={modelLabel(defaultModel)}
        projects={projects} contextUsage={contextUsage}
        mainChat={{ title: parent.title, modelDisabledReason: readOnly ? readOnlyReason : undefined }} onChooseModel={chooseModel} providers={providers} onConnect={showProvider} onDisconnect={(id) => showProvider(id, true)}
        onReset={() => open({ kind: 'reset', version: ++revision.current })} onBack={() => { if (rootGate.valid() && !overlay) dismissRoot(); }}
        rules={{ state: 'ready', rows: [{ title: 'Workspace trust', detail: `${parent.title} · current example parent; project resource trust is not evaluated by this UI.`, status: 'Read-only / unknown' }, { title: 'Persistent rule', detail: 'Example read rule · durable policy scope, not a session grant.', status: 'Inspect only' }, { title: 'Session grant', detail: 'Example process-local read grant · ends with its owning process.', status: 'Session only' }] }}
        inspections={resourceInspections} subagents={definitions.workspace} onSaveSubagent={(intent) => rootGate.valid() && definitions.save(intent)} />
    </div> : null}
    {renderOverlay()}
  </div>;
}
