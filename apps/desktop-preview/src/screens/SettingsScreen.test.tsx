import { useState } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { SettingsSection } from './contracts';
import { SettingsScreen, settingsSections } from './SettingsScreen';
import { defaultPreferences, providers, resourceInspections } from './screenExamples';
import { initialSubagentWorkspace } from './subagentWorkspace';

const callbacks = { onPreferencesChange: vi.fn(), onChooseModel: vi.fn(), onConnect: vi.fn(), onDisconnect: vi.fn(), onReset: vi.fn(), onBack: vi.fn(), onSaveSubagent: vi.fn() };
function Host({ active = true }: { active?: boolean }) {
  const [section, setSection] = useState<SettingsSection>('general'); const [query, setQuery] = useState('');
  return <SettingsScreen {...callbacks} section={section} onSectionChange={setSection} query={query} onQueryChange={setQuery} preferences={defaultPreferences} opaque={false} onOpaqueChange={vi.fn()} currentModel="Aster" defaultModel="Aster" providers={providers} rules={{ state: 'ready', rows: [] }} inspections={resourceInspections} subagents={initialSubagentWorkspace()} active={active} />;
}

describe('sixteen individually rendered settings leaves', () => {
  it('keeps Providers top-level, resources separate and only MCP/LSP in Connections', async () => {
    const user = userEvent.setup(); render(<Host />);
    const nav = screen.getByRole('navigation', { name: 'Settings sections' });
    expect(settingsSections).toHaveLength(16);
    expect(within(nav).getAllByRole('button').map((button) => button.textContent)).toEqual(settingsSections.map((entry) => entry.label));
    expect(within(nav).getByRole('searchbox', { name: 'Search settings' })).toBeInTheDocument();
    expect(within(nav).queryByRole('button', { name: /Reset/ })).not.toBeInTheDocument();
    expect(within(nav).getAllByRole('group')).toHaveLength(2);
    expect(within(within(nav).getByRole('group', { name: 'Resources' })).getAllByRole('button').map((button) => button.textContent)).toEqual(['Subagents', 'Skills', 'Prompts', 'Plugins']);
    expect(within(within(nav).getByRole('group', { name: 'Connections' })).getAllByRole('button').map((button) => button.textContent)).toEqual(['MCP', 'LSP']);
    expect(within(nav).getByRole('button', { name: 'Providers' }).closest('[role=group]')).toBeNull();
    for (const entry of settingsSections) {
      const button = within(nav).getByRole('button', { name: entry.label }); await user.click(button);
      expect(button).toHaveAttribute('aria-current', 'page');
      expect(nav.querySelectorAll('[aria-current=page]')).toHaveLength(1);
      expect(screen.getAllByRole('heading', { level: 2 })).toHaveLength(1); expect(screen.getByRole('heading', { level: 2 })).toHaveTextContent(entry.label);
      if (entry.id !== 'subagents') expect(screen.queryByRole('list', { name: 'Subagent definitions' })).not.toBeInTheDocument();
      if (entry.id !== 'providers') expect(screen.queryByRole('list', { name: 'Provider brands' })).not.toBeInTheDocument();
      if (entry.id !== 'mcp') expect(screen.queryByRole('article', { name: 'Workspace notes' })).not.toBeInTheDocument();
      if (entry.id === 'general') expect(within(screen.getByRole('region', { name: 'General' })).getByRole('button', { name: 'Reset all preferences…' })).toBeVisible();
      else expect(screen.queryByRole('button', { name: 'Reset all preferences…' })).not.toBeInTheDocument();
    }
  });
  it('places the global reset after and separate from Reset General as a keyboard-operable native button, not a link', async () => {
    const user = userEvent.setup(); render(<Host />);
    const body = screen.getByRole('region', { name: 'General' });
    const localReset = within(body).getByRole('button', { name: 'Reset General' });
    const reset = within(body).getByRole('button', { name: 'Reset all preferences…' });
    expect(body.lastElementChild).toContainElement(reset);
    expect(localReset.parentElement).not.toContainElement(reset);
    expect(localReset.compareDocumentPosition(reset) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    expect(reset.tagName).toBe('BUTTON'); expect(reset).toHaveAttribute('type', 'button'); expect(reset).not.toHaveAttribute('href');
    expect(screen.queryByRole('link', { name: 'Reset all preferences…' })).not.toBeInTheDocument();
    expect(localReset).toBeDisabled(); expect(reset).toBeEnabled();
    reset.focus(); await user.keyboard('{Enter}'); expect(callbacks.onReset).toHaveBeenCalledTimes(1);
    await user.keyboard(' '); expect(callbacks.onReset).toHaveBeenCalledTimes(2);
    expect(callbacks.onPreferencesChange).not.toHaveBeenCalled(); expect(callbacks.onBack).not.toHaveBeenCalled();
  });
  it('searches every leaf and keywords with intelligible group hits, without changing the selected body until activation', async () => {
    const user = userEvent.setup(); render(<Host />);
    const query = screen.getByRole('searchbox', { name: 'Search settings' }); const nav = screen.getByRole('navigation', { name: 'Settings sections' });
    for (const entry of settingsSections) {
      fireEvent.change(query, { target: { value: entry.label } }); expect(within(nav).getByRole('button', { name: entry.label })).toBeVisible();
      expect(screen.getByRole('heading', { level: 2, name: 'General' })).toBeVisible();
    }
    fireEvent.change(query, { target: { value: 'resources' } }); expect(within(nav).getByRole('group', { name: 'Resources' })).toBeVisible(); expect(within(nav).queryByRole('group', { name: 'Connections' })).not.toBeInTheDocument();
    fireEvent.change(query, { target: { value: 'stdio' } }); expect(within(nav).getByRole('group', { name: 'Connections' })).toBeVisible();
    await user.click(within(nav).getByRole('button', { name: 'MCP' })); expect(query).toHaveValue(''); expect(screen.getByRole('heading', { level: 2, name: 'MCP' })).toBeVisible();
    fireEvent.change(query, { target: { value: 'not a leaf' } }); expect(screen.getByRole('status')).toHaveTextContent('No matching settings');
    expect(screen.getByRole('heading', { level: 2, name: 'MCP' })).toBeVisible(); expect(within(nav).queryByRole('group')).not.toBeInTheDocument();
    fireEvent.keyDown(query, { key: 'Escape', isComposing: true }); expect(query).toHaveValue('not a leaf');
    fireEvent.keyDown(query, { key: 'Escape' }); expect(query).toHaveValue('');
    fireEvent.keyDown(query, { key: 'Escape' }); expect(within(nav).getByRole('button', { name: 'General' })).toHaveFocus();
    fireEvent.keyDown(document.activeElement!, { key: 'Escape' }); expect(callbacks.onBack).toHaveBeenCalledTimes(1);
  });
  it('finds Appearance through its palette, font, reading, accessibility and material terms', () => {
    render(<Host />); const query = screen.getByRole('searchbox', { name: 'Search settings' });
    for (const value of ['charcoal', 'font', 'prose', 'spacing', 'width', 'contrast', 'glass', 'opaque', 'motion']) {
      fireEvent.change(query, { target: { value } });
      expect(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'Appearance' })).toBeVisible();
      expect(screen.getByRole('heading', { level: 2, name: 'General' })).toBeVisible();
    }
  });
  it('retains page state but rejects navigation, Back and reset callbacks under an overlay', () => {
    const view = render(<Host />); const nav = screen.getByRole('navigation', { name: 'Settings sections' });
    fireEvent.click(within(nav).getByRole('button', { name: 'Subagents' }));
    view.rerender(<Host active={false} />);
    fireEvent.click(within(nav).getByRole('button', { name: 'Providers' })); fireEvent.click(screen.getByRole('button', { name: 'Back' }));
    fireEvent.change(screen.getByRole('searchbox'), { target: { value: 'MCP' } });
    expect(screen.getByRole('heading', { level: 2, name: 'Subagents' })).toBeVisible(); expect(screen.getByRole('searchbox')).toHaveValue('');
    view.rerender(<Host />); fireEvent.click(within(nav).getByRole('button', { name: 'General' }));
    view.rerender(<Host active={false} />);
    fireEvent.click(screen.getByRole('button', { name: 'Reset all preferences…' }));
    fireEvent.click(within(nav).getByRole('button', { name: 'Providers' })); fireEvent.click(screen.getByRole('button', { name: 'Back' }));
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
  it('separates enabled stdio configuration from connection, scope/trust and one-shot status; other inventories remain unknown', async () => {
    const user = userEvent.setup(); render(<Host />); const nav = screen.getByRole('navigation', { name: 'Settings sections' });
    await user.click(within(nav).getByRole('button', { name: 'MCP' }));
    const global = screen.getByRole('article', { name: 'Workspace notes' });
    expect(global).toHaveTextContent('ConfigurationEnabled'); expect(global).toHaveTextContent('ConnectionNot connected'); expect(global).toHaveTextContent('One-shot'); expect(global).toHaveTextContent('Global');
    const project = screen.getByRole('article', { name: 'Project reference' }); expect(project).toHaveTextContent('ConfigurationDisabled'); expect(project).toHaveTextContent('ConnectionUnknown'); expect(project).toHaveTextContent('Required · not evaluated');
    expect(within(global).queryByRole('button')).not.toBeInTheDocument(); expect(within(project).queryByRole('checkbox')).not.toBeInTheDocument();
    for (const leaf of ['Skills', 'Prompts', 'Plugins', 'LSP']) {
      await user.click(within(nav).getByRole('button', { name: leaf }));
      expect(screen.getAllByText('Unknown').length).toBeGreaterThan(0); expect(screen.queryByRole('article')).not.toBeInTheDocument();
      expect(screen.queryByRole('button', { name: /install|launch|connect|restart/i })).not.toBeInTheDocument();
    }
  });
});
