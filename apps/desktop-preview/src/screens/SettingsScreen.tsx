import { useId, useLayoutEffect, useRef } from 'react';
import type { Project } from '../state/types';
import type { ContextUsageView } from '../chat/contextUsage';
import { GeneralSettings } from './GeneralSettings';
import { ShortcutsSettings } from './ShortcutsSettings';
import { ConversationSettings } from './ConversationSettings';
import { NotificationsSettings } from './NotificationsSettings';
import { ContextManagementSettings } from './ContextManagementSettings';
import { canFocus } from '../ui/DialogFrame';
import { AvaMark } from '../components/AvaMark';
import { Button } from '../ui/Button';
import type { InspectionView, ModelScope, Preferences, Provider, ResourceInspections, SettingsSection, SubagentIntent, SubagentWorkspace } from './contracts';
import { McpSettings, ResourceSettings } from './ResourceSettings';
import { SubagentsSettings } from './SubagentsSettings';
import { AppearanceSettings, defaultAppearance } from './AppearanceSettings';
import { ProviderDirectory } from './ProviderViews';
import { PreferenceRow, SearchField } from './ScreenPrimitives';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';

export const settingsSections: readonly { id: SettingsSection; label: string; keywords: string; group?: 'Resources' | 'Connections' }[] = [
  { id: 'general', label: 'General', keywords: 'help send enter input preferences new chat mode project naming title inspector layout' },
  { id: 'appearance', label: 'Appearance', keywords: 'dark light system theme palette charcoal midnight font typography prose text size line spacing reading width contrast glass opaque sidebar material density motion reset' },
  { id: 'models', label: 'Models', keywords: 'reasoning current default context window' },
  { id: 'providers', label: 'Providers', keywords: 'connect login credentials key browser device' },
  { id: 'shortcuts', label: 'Shortcuts', keywords: 'keyboard bindings rebind' },
  { id: 'conversation', label: 'Conversation behavior', keywords: 'reading tool output details disclosure wrap code diff scroll send' },
  { id: 'notifications', label: 'Notifications & sounds', keywords: 'completion failure input required volume mute tone test background delivery' },
  { id: 'context', label: 'Context management', keywords: 'compaction policy preview trigger retention tokens window estimate' },
  { id: 'permissions', label: 'Permissions & trust', keywords: 'rules grants scope policy project session' },
  { id: 'subagents', label: 'Subagents', group: 'Resources', keywords: 'resources definitions description model preference inherit user built-in project' },
  { id: 'skills', label: 'Skills', group: 'Resources', keywords: 'resources inventory scope trust' },
  { id: 'prompts', label: 'Prompts', group: 'Resources', keywords: 'resources instructions inventory' },
  { id: 'plugins', label: 'Plugins', group: 'Resources', keywords: 'resources packages inventory' },
  { id: 'mcp', label: 'MCP', group: 'Connections', keywords: 'connections stdio configuration enabled server source trust one-shot' },
  { id: 'lsp', label: 'LSP', group: 'Connections', keywords: 'connections language servers status' },
  { id: 'about', label: 'About', keywords: 'version preview limits AVA' },
];

export function SettingsScreen({ section, onSectionChange, query, onQueryChange, preferences, onPreferencesChange, opaque, onOpaqueChange, appearanceResetOpaque = false, currentModel, defaultModel, onChooseModel, providers, onConnect, onDisconnect, rules, inspections, subagents, onSaveSubagent, onReset, onBack, embedded = false, active = true, mainChat, projects, contextUsage }: {
  section: SettingsSection; onSectionChange: (section: SettingsSection) => void; query: string; onQueryChange: (value: string) => void;
  preferences: Preferences; onPreferencesChange: (preferences: Preferences) => void; currentModel: string; defaultModel: string;
  onChooseModel: (scope: ModelScope) => void; providers: readonly Provider[]; onConnect: (id: string) => void; onDisconnect: (id: string) => void;
  rules: InspectionView; inspections: ResourceInspections; subagents: SubagentWorkspace; onSaveSubagent: (intent: SubagentIntent) => boolean; onReset: () => void; onBack: () => void;
  opaque: boolean; onOpaqueChange: (opaque: boolean) => void; appearanceResetOpaque?: boolean;
  projects?: readonly Project[]; contextUsage?: ContextUsageView;
  embedded?: boolean; active?: boolean; mainChat?: Readonly<{ title: string; modelDisabledReason?: string }>;
}) {
  const gate = useViewGuard(JSON.stringify([section, preferences, opaque, query, active]));
  const root = useRef<HTMLElement>(null);
  const layout = useRef<HTMLDivElement>(null);
  const body = useRef<HTMLDivElement>(null);
  const headingId = useId();
  useLayoutEffect(() => {
    // Only section changes restart content. The narrow stack keeps navigation first.
    body.current!.scrollTop = 0;
    layout.current!.scrollTop = 0;
  }, [section]);
  const composing = useRef(false);
  const generalButton = useRef<HTMLButtonElement>(null);
  const search = query.trim().toLocaleLowerCase();
  const matches = settingsSections.filter((entry) => `${entry.label} ${entry.keywords}`.toLocaleLowerCase().includes(search));
  function back() {
    if (!allowed()) return;
    if (query) onQueryChange(''); else if (section !== 'general') { onSectionChange('general'); generalButton.current?.focus(); } else onBack();
  }
  function allowed() { return active && gate.valid() && canFocus(root.current); }
  function update(patch: Partial<Preferences>) {
    const next = { ...preferences, ...patch };
    // A no-op must not consume the claim for the unchanged preferences scope.
    if (allowed() && JSON.stringify(next) !== JSON.stringify(preferences) && gate.claim()) onPreferencesChange(next);
  }
  const behavior = { preferences, active, allowed, update };
  return <section ref={root} className={styles.settingsScreen} aria-label="Settings" onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }} onKeyDown={(event) => {
    if (event.key !== 'Escape' || event.target instanceof Element && event.target.closest('dialog') !== event.currentTarget.closest('dialog')) return;
    event.preventDefault(); event.stopPropagation();
    if (!composing.current && !event.nativeEvent.isComposing && event.keyCode !== 229) back();
  }}>
    {!embedded ? <header className={styles.settingsHeader}><h1>Settings</h1><Button onClick={back}>Back</Button></header> : null}
    <div ref={layout} className={styles.settingsLayout}>
      <nav className={styles.settingsNav} aria-label="Settings sections" tabIndex={0}>
        <div className={styles.settingsSearch}><SearchField label="Search settings" value={query} onChange={(value) => { if (allowed()) onQueryChange(value); }} /></div>
        {[undefined, 'Resources', 'Connections', 'about'].map((group) => {
          const entries = matches.filter((entry) => group === 'about' ? entry.id === 'about' : entry.id !== 'about' && entry.group === group);
          if (!entries.length) return null;
          return <div key={group ?? 'primary'} className={styles.settingsNavGroup} role={group === 'Resources' || group === 'Connections' ? 'group' : undefined} aria-label={group === 'Resources' || group === 'Connections' ? group : undefined}>
            {group === 'Resources' || group === 'Connections' ? <h3>{group}</h3> : null}
            {entries.map((entry) => <button key={entry.id} ref={entry.id === 'general' ? generalButton : undefined} className={styles.navButton} aria-current={entry.id === section ? 'page' : undefined} onClick={() => { if (allowed()) { onSectionChange(entry.id); onQueryChange(''); } }}>{entry.label}</button>)}
          </div>;
        })}
        {search && !matches.length ? <p className={styles.hint} role="status">No matching settings. Try “model”, “theme” or “permissions”.</p> : null}
        {query ? <Button onClick={() => { if (allowed()) onQueryChange(''); }}>Clear search</Button> : null}
      </nav>
      <div ref={body} className={styles.settingsBody} role="region" aria-labelledby={headingId} tabIndex={0}><h2 id={headingId}>{settingsSections.find((entry) => entry.id === section)?.label ?? 'Unsupported settings section'}</h2>
        {section === 'general' ? <><GeneralSettings {...behavior} projects={projects} review={!mainChat} /><div className={styles.settingsReset}><button type="button" onClick={() => { if (allowed()) onReset(); }}>Reset all preferences…</button></div></> : null}
        {section === 'appearance' ? <AppearanceSettings preferences={preferences} opaque={opaque} active={active} resetOpaque={appearanceResetOpaque}
          onChange={(value) => { if (allowed()) onPreferencesChange(value); }}
          onOpaqueChange={(value) => { if (allowed()) onOpaqueChange(value); }}
          onReset={() => { if (allowed()) { onPreferencesChange({ ...preferences, ...defaultAppearance }); onOpaqueChange(appearanceResetOpaque); } }} /> : null}
        {section === 'models' ? <><PreferenceRow title={mainChat ? 'Current chat' : 'Current example chat'} description={mainChat ? `${mainChat.title} · Selected: ${currentModel}${mainChat.modelDisabledReason ? `. ${mainChat.modelDisabledReason}` : ''}` : `Selected: ${currentModel}`}><Button disabled={Boolean(mainChat?.modelDisabledReason)} onClick={() => { if (allowed() && !mainChat?.modelDisabledReason) onChooseModel('current-chat'); }}>Choose current model</Button></PreferenceRow><PreferenceRow title="New chats" description={`Default: ${defaultModel}`}><Button onClick={() => { if (allowed()) onChooseModel('new-chat-default'); }}>Choose default model</Button></PreferenceRow></> : null}
        {section === 'providers' ? <ProviderDirectory active={active} providers={providers} onConnect={(id) => { if (allowed()) onConnect(id); }} onDisconnect={(id) => { if (allowed()) onDisconnect(id); }} /> : null}
        {section === 'shortcuts' ? <ShortcutsSettings {...behavior} /> : null}
        {section === 'conversation' ? <ConversationSettings {...behavior} /> : null}
        {section === 'notifications' ? <NotificationsSettings {...behavior} /> : null}
        {section === 'context' ? <ContextManagementSettings {...behavior} usage={contextUsage} /> : null}
        {section === 'permissions' ? <><p className={styles.muted}>Read-only example policy inspection. Persistent rules and process-local session grants have different scopes. No approval, trust write or “always” grant.</p>{rules.state === 'disabled' ? <p className={styles.notice} role="status">Access denied in this policy-inspection example. No trust or grant is changed.</p> : <ResourceSettings view={rules} />}<PreferenceRow title="Change trust or rules" description="Effective policy belongs to AVA, not this presentation layer."><Button disabled>Read-only scope</Button></PreferenceRow></> : null}
        {section === 'subagents' ? <SubagentsSettings workspace={subagents} active={active} onSave={(intent) => allowed() && onSaveSubagent(intent)} /> : null}
        {section === 'skills' || section === 'prompts' || section === 'plugins' || section === 'lsp' ? <ResourceSettings view={inspections[section]} /> : null}
        {section === 'mcp' ? <McpSettings view={inspections.mcp} /> : null}
        {section === 'about' ? <><div className={styles.inline}><AvaMark /><span>AVA · browser preview</span></div><PreferenceRow title="Local by design" description="Invented chats, model profiles, activity and resource examples. Local edits last for this visit only; no model is invoked." /><PreferenceRow title="Not connected" description="No backend, provider, auth, credential, file or persistent state access. This preview does not report an installed backend version or live connection." /><PreferenceRow title="Subagent model preferences" description="Local definition metadata, not backend policy. The backend has no per-definition model field or definition-write RPC; child runs inherit the parent model. Preferences never change a chat, a running child or the new-chat default." />{!mainChat ? <PreferenceRow title="Screen review notes" description="Show explanatory notes in this standalone screen review."><input type="checkbox" aria-label="Show review help" checked={preferences.help} onChange={(event) => update({ help: event.currentTarget.checked })} /></PreferenceRow> : null}{embedded ? <p className={styles.muted}><a href="?view=screens">Screens review</a> · Optional examples of every screen and state.</p> : null}</> : null}
      </div>
    </div>
  </section>;
}
