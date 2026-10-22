import { useState } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { ModelId } from '../state/types';
import type { InspectionView, ResourceInspections, SettingsSection } from './contracts';
import type { ProjectTrust } from './ResourceSettings';
import { SettingsScreen, settingsSections } from './SettingsScreen';
import { defaultPreferences, providers, resourceInspections } from './screenExamples';
import { initialSubagentWorkspace } from './subagentWorkspace';

const callbacks = { onPreferencesChange: vi.fn(), onChooseModel: vi.fn(), onConnect: vi.fn(), onDisconnect: vi.fn(), onReset: vi.fn(), onBack: vi.fn(), onSaveSubagent: vi.fn(), onRetryInspection: vi.fn() };
function Host({ active = true, initial = 'general', defaultModelId, rules = { state: 'ready', rows: [] }, inspections = resourceInspections, project }: { active?: boolean; initial?: SettingsSection; defaultModelId?: ModelId; rules?: InspectionView; inspections?: ResourceInspections; project?: ProjectTrust }) {
  const [section, setSection] = useState<SettingsSection>(initial); const [query, setQuery] = useState('');
  return <SettingsScreen {...callbacks} section={section} onSectionChange={setSection} query={query} onQueryChange={setQuery} preferences={defaultPreferences} opaque={false} onOpaqueChange={vi.fn()} defaultModel="Aster" defaultModelId={defaultModelId} providers={providers} rules={rules} inspections={inspections} subagents={initialSubagentWorkspace()} active={active} project={project} />;
}
const nav = () => screen.getByRole('navigation', { name: 'Settings sections' });
const search = () => screen.getByRole('searchbox', { name: 'Search settings' });

describe('eleven grouped Settings sections', () => {
  it('groups App and Agent pages, keeps About apart and replaces the five resource pages with Extensions', async () => {
    const user = userEvent.setup(); render(<Host />);
    expect(settingsSections.map((entry) => entry.id)).toEqual(['general', 'appearance', 'shortcuts', 'notifications', 'models', 'conversation', 'context', 'permissions', 'subagents', 'extensions', 'about']);
    expect(within(nav()).getAllByRole('button').map((button) => button.textContent)).toEqual(settingsSections.map((entry) => entry.label));
    expect(within(within(nav()).getByRole('group', { name: 'App' })).getAllByRole('button').map((button) => button.textContent)).toEqual(['General', 'Appearance', 'Shortcuts', 'Notifications & sounds']);
    expect(within(within(nav()).getByRole('group', { name: 'Agent' })).getAllByRole('button').map((button) => button.textContent)).toEqual(['Models & providers', 'Conversation behavior', 'Context management', 'Permissions & trust', 'Subagents', 'Extensions']);
    expect(within(nav()).getByRole('button', { name: 'About' }).closest('[role=group]')).toBeNull();
    for (const removed of ['Providers', 'Skills', 'Prompts', 'Plugins', 'MCP', 'LSP']) expect(within(nav()).queryByRole('button', { name: removed })).not.toBeInTheDocument();
    for (const entry of settingsSections) {
      const button = within(nav()).getByRole('button', { name: entry.label }); await user.click(button);
      expect(button).toHaveAttribute('aria-current', 'page'); expect(nav().querySelectorAll('[aria-current=page]')).toHaveLength(1);
      expect(within(nav()).getAllByRole('button').filter((item) => item.tabIndex === 0)).toEqual([button]);
      expect(screen.getAllByRole('heading', { level: 2 })).toHaveLength(1); expect(screen.getByRole('heading', { level: 2 })).toHaveTextContent(entry.label);
      expect(screen.getByRole('combobox', { name: 'Section' })).toHaveValue(entry.id);
      if (entry.id === 'general') expect(within(screen.getByRole('region', { name: 'General' })).getByRole('button', { name: 'Reset all preferences…' })).toBeVisible();
      else expect(screen.queryByRole('button', { name: 'Reset all preferences…' })).not.toBeInTheDocument();
    }
  });
  it('makes the nav one Tab stop that arrow keys move through without opening, and opens with Enter', async () => {
    const user = userEvent.setup(); render(<Host />);
    expect(nav()).not.toHaveAttribute('tabindex');
    const general = within(nav()).getByRole('button', { name: 'General' }); general.focus();
    await user.keyboard('{ArrowDown}'); expect(within(nav()).getByRole('button', { name: 'Appearance' })).toHaveFocus(); expect(screen.getByRole('heading', { level: 2 })).toHaveTextContent('General');
    expect(within(nav()).getByRole('button', { name: 'Appearance' })).toHaveAttribute('tabindex', '0'); expect(general).toHaveAttribute('tabindex', '-1');
    await user.keyboard('{End}'); expect(within(nav()).getByRole('button', { name: 'About' })).toHaveFocus();
    await user.keyboard('{ArrowDown}'); expect(general).toHaveFocus();
    await user.keyboard('{ArrowUp}{ArrowUp}'); expect(within(nav()).getByRole('button', { name: 'Extensions' })).toHaveFocus();
    await user.keyboard('{Enter}'); expect(screen.getByRole('heading', { level: 2 })).toHaveTextContent('Extensions');
    await user.keyboard('{Home} '); expect(screen.getByRole('heading', { level: 2 })).toHaveTextContent('General');
  });
  it('opens a section from the compact section select', () => {
    render(<Host />);
    const picker = screen.getByRole('combobox', { name: 'Section' });
    expect(within(picker).getAllByRole('group').map((group) => group.getAttribute('label'))).toEqual(['App', 'Agent']);
    fireEvent.change(picker, { target: { value: 'context' } }); expect(screen.getByRole('heading', { level: 2 })).toHaveTextContent('Context management');
    fireEvent.change(picker, { target: { value: 'not-a-section' } }); expect(screen.getByRole('heading', { level: 2 })).toHaveTextContent('Context management');
  });
  it('searches individual rows with every word, groups them by page and jumps to the chosen row', async () => {
    const user = userEvent.setup(); render(<Host />);
    await user.type(search(), 'api key');
    // Results belong to no page: neither the nav nor the section select keeps marking General.
    expect(nav().querySelector('[aria-current]')).toBeNull(); expect(screen.getByRole('combobox', { name: 'Section' })).toHaveValue('');
    expect(within(screen.getByRole('combobox', { name: 'Section' })).getByRole('option', { name: 'Search results' })).toBeDisabled();
    const results = screen.getByRole('region', { name: 'Search results' });
    expect(within(results).getByRole('group', { name: 'Models & providers' })).toBeVisible();
    await user.click(within(results).getByRole('button', { name: /^Providers/ }));
    expect(search()).toHaveValue(''); expect(screen.getByRole('heading', { level: 2 })).toHaveTextContent('Models & providers');
    expect(within(nav()).getByRole('button', { name: 'Models & providers' })).toHaveAttribute('aria-current', 'page'); expect(screen.getByRole('combobox', { name: 'Section' })).toHaveValue('models');
    expect(within(screen.getByRole('combobox', { name: 'Section' })).queryByRole('option', { name: 'Search results' })).not.toBeInTheDocument();
    expect(document.querySelector('[data-setting="models.providers"]')).toHaveAttribute('data-found', 'true');
    await user.type(search(), 'dark mode');
    await user.click(within(screen.getByRole('region', { name: 'Search results' })).getByRole('button', { name: /^Palette/ }));
    const radio = screen.getByRole('radio', { name: 'Default' }); expect(radio).toHaveFocus(); expect(radio.closest('[data-setting]')).toHaveAttribute('data-found', 'true');
    await user.type(search(), 'wrap diff');
    expect(within(screen.getByRole('region', { name: 'Search results' })).getAllByRole('button').map((button) => button.querySelector('strong')!.textContent)).toEqual(['Wrap diff lines']);
    await user.clear(search()); await user.type(search(), 'contrast{Enter}');
    expect(screen.getByRole('heading', { level: 2 })).toHaveTextContent('Appearance'); expect(screen.getByRole('combobox', { name: 'Contrast' })).toHaveFocus();
    await user.type(search(), 'command palette');
    expect(within(screen.getByRole('region', { name: 'Search results' })).getByRole('group', { name: 'Shortcuts' })).toHaveTextContent('Open command palette');
  });
  it('explains an empty search, clears it with Escape and then leaves Settings', async () => {
    const user = userEvent.setup(); render(<Host />);
    await user.type(search(), 'not a setting'); expect(screen.getByText('No settings match “not a setting”.')).toBeVisible();
    fireEvent.keyDown(search(), { key: 'Escape', isComposing: true }); expect(search()).toHaveValue('not a setting');
    fireEvent.keyDown(search(), { key: 'Escape' }); expect(search()).toHaveValue(''); expect(screen.getByRole('heading', { level: 2 })).toHaveTextContent('General');
    expect(callbacks.onBack).not.toHaveBeenCalled();
    fireEvent.keyDown(search(), { key: 'Escape' }); expect(callbacks.onBack).toHaveBeenCalledTimes(1);
  });
  it('keeps Reset all as a keyboard-operable native button after Reset general, disabled only when nothing differs', async () => {
    const user = userEvent.setup(); const view = render(<Host />);
    const body = screen.getByRole('region', { name: 'General' });
    const localReset = within(body).getByRole('button', { name: 'Reset general' });
    const reset = within(body).getByRole('button', { name: 'Reset all preferences…' });
    expect(body.lastElementChild).toContainElement(reset); expect(localReset.parentElement).not.toContainElement(reset);
    expect(localReset.compareDocumentPosition(reset) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    expect(reset).toHaveAttribute('type', 'button'); expect(localReset).toBeDisabled(); expect(reset).toBeEnabled();
    reset.focus(); await user.keyboard('{Enter}'); expect(callbacks.onReset).toHaveBeenCalledTimes(1);
    await user.keyboard(' '); expect(callbacks.onReset).toHaveBeenCalledTimes(2);
    view.rerender(<Host defaultModelId="aster" />);
    expect(screen.getByRole('button', { name: 'Reset all preferences…' })).toBeDisabled(); expect(screen.getByText('Everything is at its default.')).toBeVisible();
    view.rerender(<Host defaultModelId="orion" />);
    expect(screen.getByRole('button', { name: 'Reset all preferences…' })).toBeEnabled();
    expect(callbacks.onPreferencesChange).not.toHaveBeenCalled(); expect(callbacks.onBack).not.toHaveBeenCalled();
  });
  it('retains page state but rejects navigation, Back, search and reset callbacks under an overlay', () => {
    const view = render(<Host />);
    fireEvent.click(within(nav()).getByRole('button', { name: 'Subagents' }));
    view.rerender(<Host active={false} />);
    fireEvent.click(within(nav()).getByRole('button', { name: 'Extensions' })); fireEvent.click(screen.getByRole('button', { name: 'Back to review' }));
    fireEvent.change(search(), { target: { value: 'MCP' } }); fireEvent.change(screen.getByRole('combobox', { name: 'Section' }), { target: { value: 'about' } });
    expect(screen.getByRole('heading', { level: 2, name: 'Subagents' })).toBeVisible(); expect(search()).toHaveValue('');
    view.rerender(<Host />); fireEvent.click(within(nav()).getByRole('button', { name: 'General' }));
    view.rerender(<Host active={false} />);
    fireEvent.click(screen.getByRole('button', { name: 'Reset all preferences…' }));
    fireEvent.keyDown(search(), { key: 'Escape' });
    expect(screen.getByRole('heading', { level: 2, name: 'General' })).toBeVisible();
    expect(callbacks.onBack).not.toHaveBeenCalled(); expect(callbacks.onReset).not.toHaveBeenCalled();
  });
  it('rejects hidden, inert, stale and unmounted global reset callbacks', () => {
    const view = render(<Host />); const root = screen.getByRole('region', { name: 'Settings' });
    function resetCallback() {
      const button = screen.getByRole('button', { name: 'Reset all preferences…' });
      const key = Object.keys(button).find((key) => key.startsWith('__reactProps'))!;
      return (button as unknown as Record<string, { onClick: () => void }>)[key].onClick;
    }
    const initial = resetCallback();
    root.hidden = true; act(initial); root.hidden = false;
    root.setAttribute('inert', ''); act(initial); root.removeAttribute('inert');
    view.rerender(<Host active={false} />); act(initial); act(resetCallback());
    view.rerender(<Host />); const departed = resetCallback();
    fireEvent.click(screen.getByRole('button', { name: 'Appearance' })); act(departed);
    fireEvent.click(screen.getByRole('button', { name: 'General' })); act(departed);
    const current = resetCallback(); view.unmount(); act(current);
    expect(callbacks.onReset).not.toHaveBeenCalled();
  });
});

describe('Settings content pages', () => {
  it('lists only extensions with data in one table and says plainly what is empty', async () => {
    const user = userEvent.setup();
    const view = render(<Host initial="extensions" />);
    const table = screen.getByRole('table', { name: 'Extensions' });
    expect(within(table).getAllByRole('columnheader').map((cell) => cell.textContent)).toEqual(['Name', 'Type', 'Scope', 'Status']);
    expect(within(table).getAllByRole('rowheader').map((cell) => cell.querySelector('span')!.textContent)).toEqual(['Workspace notes', 'Project reference']);
    expect(within(table).getByRole('row', { name: /Workspace notes/ })).toHaveTextContent('On · not connected');
    expect(within(table).getByRole('row', { name: /Project reference/ })).toHaveTextContent('Off · needs project trust');
    expect(screen.getByText('No skills, prompts, plugins or language servers found.')).toBeVisible();
    expect(screen.queryByText(/Unknown|supplied|example/i)).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: /install|launch|connect|restart/i })).not.toBeInTheDocument();
    view.rerender(<Host initial="extensions" inspections={{ ...resourceInspections, mcp: { state: 'empty', servers: [] } }} />);
    expect(screen.queryByRole('table')).not.toBeInTheDocument();
    expect(screen.getByText('No skills, prompts, plugins, MCP servers or language servers found.')).toBeVisible();
    // “needs project trust” leads to the trust row, which states how to trust the project.
    view.rerender(<Host initial="extensions" project={{ name: 'Wayfinder', trusted: false }} />);
    await user.click(within(screen.getByRole('table', { name: 'Extensions' })).getByRole('button', { name: 'needs project trust' }));
    expect(screen.getByRole('heading', { level: 2 })).toHaveTextContent('Permissions & trust');
    const trust = document.querySelector<HTMLElement>('[data-setting="permissions.trust"]')!;
    expect(trust).toHaveFocus(); expect(trust).toHaveAttribute('data-found', 'true'); expect(trust).toHaveTextContent('To trust it, send /trust project in any Wayfinder chat.');
    view.rerender(<Host initial="extensions" project={{ name: 'Wayfinder', trusted: true }} />); await user.click(within(nav()).getByRole('button', { name: 'Extensions' }));
    expect(within(screen.getByRole('table', { name: 'Extensions' })).getByRole('row', { name: /Project reference/ })).toHaveTextContent(/^Project reference.*Off$/);
    expect(screen.queryByRole('button', { name: 'needs project trust' })).not.toBeInTheDocument();
  });
  it('reports an extension load error with area copy and a Retry for that area', async () => {
    const user = userEvent.setup(); render(<Host initial="extensions" inspections={{ ...resourceInspections, plugins: { state: 'error', rows: [] } }} />);
    expect(screen.getByRole('alert')).toHaveTextContent('Couldn’t load plugins.');
    await user.click(screen.getByRole('button', { name: 'Retry' })); expect(callbacks.onRetryInspection).toHaveBeenCalledExactlyOnceWith('plugins');
    expect(screen.getByRole('table', { name: 'Extensions' })).toBeVisible();
  });
  it('explains permissions without a dead control and gives rule states page copy', async () => {
    const user = userEvent.setup(); const rows = [{ title: 'Run tests', detail: 'Granted for this session only.', status: 'This session' }];
    const view = render(<Host initial="permissions" rules={{ state: 'ready', rows }} />);
    expect(screen.getByText(/What AVA may do without asking/)).toBeVisible(); expect(screen.getByText('This session')).toBeVisible();
    expect(within(screen.getByRole('region', { name: 'Permissions & trust' })).queryByRole('button')).not.toBeInTheDocument();
    // Without a known project the trust row still states the route, never a dead end.
    expect(document.querySelector('[data-setting="permissions.trust"]')).toHaveTextContent('To trust one, send /trust project in one of its chats.');
    view.rerender(<Host initial="permissions" rules={{ state: 'ready', rows }} project={{ name: 'Wayfinder', trusted: false }} />);
    const trust = document.querySelector<HTMLElement>('[data-setting="permissions.trust"]')!;
    expect(within(trust).getByRole('heading', { level: 3, name: 'Project trust' })).toBeVisible(); expect(trust).toHaveTextContent('Not trusted');
    expect(trust).toHaveTextContent('Wayfinder is this chat’s project. Until you trust it, AVA skips its own skills, prompts, subagents, plugins, MCP servers and language servers. To trust it, send /trust project in any Wayfinder chat.');
    expect(within(trust).getByText('/trust project').tagName).toBe('CODE'); expect(screen.getByRole('region', { name: 'Permissions & trust' })).not.toHaveTextContent(/this project/);
    view.rerender(<Host initial="permissions" rules={{ state: 'ready', rows }} project={{ name: 'Atlas', trusted: true }} />);
    expect(document.querySelector('[data-setting="permissions.trust"]')).toHaveTextContent('Trusted'); expect(document.querySelector('[data-setting="permissions.trust"]')).toHaveTextContent('To stop using them, send /trust deny in any Atlas chat.');
    view.rerender(<Host initial="permissions" rules={{ state: 'error', rows }} />);
    expect(screen.getByRole('alert')).toHaveTextContent('Couldn’t load these rules.'); await user.click(screen.getByRole('button', { name: 'Retry' }));
    expect(callbacks.onRetryInspection).toHaveBeenCalledExactlyOnceWith('rules');
    view.rerender(<Host initial="permissions" rules={{ state: 'disabled', rows }} />); expect(screen.getByText('You don’t have access to these rules.')).toBeVisible(); expect(screen.queryByText('This session')).not.toBeInTheDocument();
    view.rerender(<Host initial="permissions" rules={{ state: 'ready', rows: [] }} />); expect(screen.getByText('No saved rules or session grants.')).toBeVisible();
  });
  it('offers the new-chat default model next to providers, with no current-chat control', async () => {
    const user = userEvent.setup(); render(<Host initial="models" />);
    expect(screen.getByText('New chats start with Aster. Existing chats keep their model.')).toBeVisible();
    expect(screen.queryByRole('button', { name: 'Choose current model' })).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Choose default model' })); expect(callbacks.onChooseModel).toHaveBeenCalledExactlyOnceWith('new-chat-default');
    expect(screen.getByRole('heading', { level: 3, name: 'Providers' })).toBeVisible();
  });
  it('keeps About to one honest paragraph and the standalone review toggle', () => {
    render(<Host initial="about" />);
    const body = screen.getByRole('region', { name: 'About' });
    expect(body).toHaveTextContent('Nothing you do here is saved, sent to a model or run on your computer.');
    expect(within(body).getByRole('checkbox', { name: 'Show review notes' })).toBeChecked();
    expect(body).not.toHaveTextContent(/RPC|backend|supplied/);
  });
});
