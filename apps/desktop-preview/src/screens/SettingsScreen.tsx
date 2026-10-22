import { useId, useLayoutEffect, useRef, useState, type KeyboardEvent } from 'react';
import type { ModelId, Project } from '../state/types';
import type { ContextUsageView } from '../chat/contextUsage';
import { canFocus } from '../ui/DialogFrame';
import { AvaMark } from '../components/AvaMark';
import { BackButton, Button } from '../ui/Button';
import type { InspectionView, ModelScope, Preferences, Provider, ResourceInspections, SettingsSection, SubagentWorkspace } from './contracts';
import { GeneralSettings } from './GeneralSettings';
import { ShortcutsSettings } from './ShortcutsSettings';
import { ConversationSettings } from './ConversationSettings';
import { NotificationsSettings } from './NotificationsSettings';
import { ContextManagementSettings } from './ContextManagementSettings';
import { ExtensionsTable, ProjectTrustRow, RulesList, type InspectionArea, type ProjectTrust } from './ResourceSettings';
import { SubagentsSettings } from './SubagentsSettings';
import { AppearanceSettings, defaultAppearance } from './AppearanceSettings';
import { ProviderDirectory } from './ProviderViews';
import { changedPreferenceAreas } from './preferenceDiff';
import { ListNotice, PreferenceRow, SearchField } from './ScreenPrimitives';
import { anchor, matchSettings, type SettingRow } from './settingsIndex';
import type { SubagentChange } from './subagentWorkspace';
import { useViewGuard } from './useViewGuard';
import styles from './screens.module.css';

export const settingsSections: readonly Readonly<{ id: SettingsSection; label: string; group: 'App' | 'Agent' | 'About' }>[] = [
  { id: 'general', label: 'General', group: 'App' },
  { id: 'appearance', label: 'Appearance', group: 'App' },
  { id: 'shortcuts', label: 'Shortcuts', group: 'App' },
  { id: 'notifications', label: 'Notifications & sounds', group: 'App' },
  { id: 'models', label: 'Models & providers', group: 'Agent' },
  { id: 'conversation', label: 'Conversation behavior', group: 'Agent' },
  { id: 'context', label: 'Context management', group: 'Agent' },
  { id: 'permissions', label: 'Permissions & trust', group: 'Agent' },
  { id: 'subagents', label: 'Subagents', group: 'Agent' },
  { id: 'extensions', label: 'Extensions', group: 'Agent' },
  { id: 'about', label: 'About', group: 'About' },
];
const navGroups = ['App', 'Agent'] as const;
const navKeys = ['ArrowDown', 'ArrowUp', 'Home', 'End'];
const focusable = 'input:not([type="hidden"]), select, textarea, button';

export function SettingsScreen({ section, onSectionChange, query, onQueryChange, preferences, onPreferencesChange, opaque, onOpaqueChange, appearanceResetOpaque = false, defaultModel, defaultModelId, onChooseModel, providers, onConnect, onDisconnect, rules, inspections, onRetryInspection, subagents, onSaveSubagent, onReset, onBack, embedded = false, active = true, projects, project, contextUsage }: {
  section: SettingsSection; onSectionChange: (section: SettingsSection) => void; query: string; onQueryChange: (value: string) => void;
  preferences: Preferences; onPreferencesChange: (preferences: Preferences) => void;
  // Label shown in Models & providers; the id lets Reset all tell whether anything differs.
  defaultModel: string; defaultModelId?: ModelId;
  onChooseModel: (scope: ModelScope) => void; providers: readonly Provider[]; onConnect: (id: string) => void; onDisconnect: (id: string) => void;
  rules: InspectionView; inspections: ResourceInspections; onRetryInspection?: (area: InspectionArea) => void;
  subagents: SubagentWorkspace; onSaveSubagent: (change: SubagentChange) => boolean; onReset: () => void; onBack: () => void;
  opaque: boolean; onOpaqueChange: (opaque: boolean) => void; appearanceResetOpaque?: boolean;
  projects?: readonly Project[]; contextUsage?: ContextUsageView;
  // The project of the chat Settings was opened from; Permissions & trust names it.
  project?: ProjectTrust;
  embedded?: boolean; active?: boolean;
}) {
  const gate = useViewGuard(JSON.stringify([section, preferences, opaque, query, active]));
  const root = useRef<HTMLElement>(null);
  const layout = useRef<HTMLDivElement>(null);
  const nav = useRef<HTMLElement>(null);
  const body = useRef<HTMLDivElement>(null);
  const composing = useRef(false);
  const id = useId();
  // The nav is one Tab stop: the current page, or the item arrow keys last reached.
  const [roving, setRoving] = useState<SettingsSection | null>(null);
  const [found, setFound] = useState<Readonly<{ id: SettingRow['id'] }> | null>(null);
  useLayoutEffect(() => {
    // Only section changes restart content. The narrow stack restarts with its section row.
    body.current!.scrollTop = 0;
    layout.current!.scrollTop = 0;
    const list = nav.current; const current = list?.querySelector<HTMLElement>('[aria-current="page"]');
    if (!list || !current) return;
    const item = current.getBoundingClientRect(); const box = list.getBoundingClientRect();
    if (item.top < box.top || item.bottom > box.bottom) current.scrollIntoView?.({ block: 'nearest' });
  }, [section]);
  // A chosen search result: scroll to its row, focus its control and highlight it briefly.
  useLayoutEffect(() => {
    const page = body.current;
    if (!found || !page) return;
    const target = page.querySelector<HTMLElement>(`[data-setting="${found.id}"]`);
    if (!target) { if (canFocus(page)) page.focus(); return; }
    target.scrollIntoView?.({ block: 'center' });
    const control = target.querySelector<HTMLElement>('input[type="radio"]:checked') ?? target.querySelector<HTMLElement>(focusable);
    if (canFocus(control)) control.focus({ preventScroll: true });
    else { target.tabIndex = -1; target.focus({ preventScroll: true }); }
    target.dataset.found = 'true';
    const timer = window.setTimeout(() => { delete target.dataset.found; }, 2400);
    return () => { window.clearTimeout(timer); delete target.dataset.found; };
  }, [found]);
  function allowed() { return active && gate.valid() && canFocus(root.current); }
  function update(patch: Partial<Preferences>) {
    const next = { ...preferences, ...patch };
    // A no-op must not consume the claim for the unchanged preferences scope.
    if (allowed() && JSON.stringify(next) !== JSON.stringify(preferences) && gate.claim()) onPreferencesChange(next);
  }
  function open(next: SettingsSection) {
    if (!allowed()) return;
    onSectionChange(next); if (query) onQueryChange('');
  }
  // A search result, or a link to another page's row: open its page, then scroll to, focus and highlight the row.
  function choose(row: Pick<SettingRow, 'id' | 'section'>) {
    if (!allowed()) return;
    setFound({ id: row.id }); onQueryChange('');
    if (row.section !== section) onSectionChange(row.section);
  }
  function moveNavFocus(event: KeyboardEvent<HTMLElement>) {
    if (!navKeys.includes(event.key)) return;
    const items = [...event.currentTarget.querySelectorAll<HTMLButtonElement>('button[data-section]')];
    const index = items.findIndex((item) => item === document.activeElement);
    if (index < 0) return;
    event.preventDefault();
    const next = event.key === 'Home' ? 0 : event.key === 'End' ? items.length - 1 : (index + (event.key === 'ArrowDown' ? 1 : -1) + items.length) % items.length;
    items[next].focus();
  }
  const behavior = { preferences, active, allowed, update };
  const searching = query.trim().length > 0;
  const results = searching ? matchSettings(query, settingsSections) : [];
  const resultCount = results.reduce((count, group) => count + group.rows.length, 0);
  const resettable = defaultModelId === undefined || changedPreferenceAreas(preferences, opaque !== appearanceResetOpaque, defaultModelId).length > 0;
  const tabStop = roving ?? section;
  // While results show, no page is current: the nav and section select stop marking the last page.
  const navItem = (entry: (typeof settingsSections)[number]) => <button key={entry.id} type="button" data-section={entry.id} className={styles.navButton} aria-current={!searching && entry.id === section ? 'page' : undefined}
    tabIndex={entry.id === tabStop ? 0 : -1} onFocus={() => setRoving(entry.id)} onClick={() => open(entry.id)}>{entry.label}</button>;
  return <section ref={root} className={styles.settingsScreen} aria-label="Settings" onCompositionStart={() => { composing.current = true; }} onCompositionEnd={() => { composing.current = false; }} onKeyDown={(event) => {
    if (event.key !== 'Escape' || event.target instanceof Element && event.target.closest('dialog') !== event.currentTarget.closest('dialog')) return;
    event.preventDefault(); event.stopPropagation();
    if (composing.current || event.nativeEvent.isComposing || event.keyCode === 229 || !allowed()) return;
    if (query) onQueryChange(''); else onBack();
  }}>
    {/* Standalone only in the screen review, where Back returns to the review; the app supplies its own header. */}
    {!embedded ? <header className={styles.settingsHeader}><h1>Settings</h1><BackButton onClick={() => { if (allowed()) onBack(); }}>Back to review</BackButton></header> : null}
    <div ref={layout} className={styles.settingsLayout}>
      <div className={styles.settingsSidebar}>
        <div className={styles.sectionPicker}><label htmlFor={`${id}-section`}>Section</label><select id={`${id}-section`} className={styles.select} value={searching ? '' : section} onChange={(event) => {
          const next = settingsSections.find((entry) => entry.id === event.currentTarget.value);
          if (next) open(next.id);
        }}>
          {searching ? <option value="" disabled>Search results</option> : null}
          {navGroups.map((group) => <optgroup key={group} label={group}>{settingsSections.filter((entry) => entry.group === group).map((entry) => <option key={entry.id} value={entry.id}>{entry.label}</option>)}</optgroup>)}
          {settingsSections.filter((entry) => entry.group === 'About').map((entry) => <option key={entry.id} value={entry.id}>{entry.label}</option>)}
        </select></div>
        <div className={styles.settingsSearch} onKeyDown={(event) => {
          // Enter opens the first result, like choosing it.
          if (event.key !== 'Enter' || composing.current || event.nativeEvent.isComposing || event.keyCode === 229 || !results.length) return;
          event.preventDefault(); choose(results[0].rows[0]);
        }}><SearchField label="Search settings" value={query} onChange={(value) => { if (allowed()) onQueryChange(value); }} /></div>
        <nav ref={nav} className={styles.settingsNav} aria-label="Settings sections" onKeyDown={moveNavFocus} onBlur={(event) => { if (!event.currentTarget.contains(event.relatedTarget as Node | null)) setRoving(null); }}>
          {navGroups.map((group) => <div key={group} className={styles.settingsNavGroup} role="group" aria-labelledby={`${id}-${group}`}>
            <p id={`${id}-${group}`} className={styles.navGroupLabel}>{group}</p>
            {settingsSections.filter((entry) => entry.group === group).map(navItem)}
          </div>)}
          <div className={styles.settingsNavAbout}>{settingsSections.filter((entry) => entry.group === 'About').map(navItem)}</div>
        </nav>
      </div>
      <div ref={body} className={styles.settingsBody} role="region" aria-labelledby={`${id}-heading`} tabIndex={0}>
        <h2 id={`${id}-heading`}>{searching ? 'Search results' : settingsSections.find((entry) => entry.id === section)?.label ?? 'Unsupported settings section'}</h2>
        <p className="srOnly" role="status">{searching && resultCount ? `${resultCount} ${resultCount === 1 ? 'setting' : 'settings'} found` : ''}</p>
        {searching ? results.length ? results.map(({ section: page, rows }) => <div key={page.id} className={styles.resultGroup} role="group" aria-labelledby={`${id}-results-${page.id}`}>
          <h3 id={`${id}-results-${page.id}`}>{page.label}</h3>
          <ul className={styles.list}>{rows.map((row) => <li key={row.id}><button type="button" className={styles.result} onClick={() => choose(row)}><span><strong>{row.title}</strong><small>{row.description}</small></span></button></li>)}</ul>
        </div>) : <ListNotice state="empty" message={`No settings match “${query.trim()}”.`} /> : <>
          {section === 'general' ? <><GeneralSettings {...behavior} projects={projects} /><div className={styles.settingsReset} {...anchor('general.resetAll')}><button type="button" disabled={!resettable} onClick={() => { if (allowed() && resettable) onReset(); }}>Reset all preferences…</button>{resettable ? null : <span>Everything is at its default.</span>}</div></> : null}
          {section === 'appearance' ? <AppearanceSettings preferences={preferences} opaque={opaque} active={active} resetOpaque={appearanceResetOpaque}
            onChange={(value) => { if (allowed()) onPreferencesChange(value); }}
            onOpaqueChange={(value) => { if (allowed()) onOpaqueChange(value); }}
            onReset={() => { if (allowed()) { onPreferencesChange({ ...preferences, ...defaultAppearance }); onOpaqueChange(appearanceResetOpaque); } }} /> : null}
          {section === 'shortcuts' ? <ShortcutsSettings {...behavior} /> : null}
          {section === 'notifications' ? <NotificationsSettings {...behavior} /> : null}
          {section === 'models' ? <>
            <div {...anchor('models.default')}><PreferenceRow title="Default model" description={`New chats start with ${defaultModel}. Existing chats keep their model.`}><Button onClick={() => { if (allowed()) onChooseModel('new-chat-default'); }}>Choose default model</Button></PreferenceRow></div>
            <div className={styles.settingsSubsection} {...anchor('models.providers')}><h3>Providers</h3><ProviderDirectory active={active} providers={providers} onConnect={(provider) => { if (allowed()) onConnect(provider); }} onDisconnect={(provider) => { if (allowed()) onDisconnect(provider); }} /></div>
          </> : null}
          {section === 'conversation' ? <ConversationSettings {...behavior} /> : null}
          {section === 'context' ? <ContextManagementSettings {...behavior} usage={contextUsage} /> : null}
          {section === 'permissions' ? <><ProjectTrustRow project={project} />
            <div className={styles.settingsSubsection}><h3>Rules and grants</h3><p className={styles.pageLead}>What AVA may do without asking. Saved rules apply to every session; session grants end when the session closes. You can review them here, not change them.</p>
              <RulesList view={rules} onRetry={onRetryInspection ? () => { if (allowed()) onRetryInspection('rules'); } : undefined} /></div></> : null}
          {section === 'subagents' ? <SubagentsSettings workspace={subagents} active={active} onSave={(change) => allowed() && onSaveSubagent(change)} /> : null}
          {section === 'extensions' ? <ExtensionsTable inspections={inspections} projectTrusted={project?.trusted ?? false} onOpenTrust={() => choose({ id: 'permissions.trust', section: 'permissions' })} onRetry={onRetryInspection ? (area) => { if (allowed()) onRetryInspection(area); } : undefined} /> : null}
          {section === 'about' ? <div className={styles.about} {...anchor('about.preview')}>
            <div className={styles.aboutIdentity}><AvaMark /><div><strong>AVA</strong><span>Desktop preview</span></div></div>
            <p>This preview runs in your browser with invented chats, models and activity. Nothing you do here is saved, sent to a model or run on your computer.</p>
            {embedded ? <p className={styles.muted}><a href="?view=screens">Open the screen review</a> to see every screen and state.</p>
              : <PreferenceRow title="Review notes" description="Show explanatory notes in this screen review."><input type="checkbox" aria-label="Show review notes" checked={preferences.help} onChange={(event) => update({ help: event.currentTarget.checked })} /></PreferenceRow>}
          </div> : null}
        </>}
      </div>
    </div>
  </section>;
}
