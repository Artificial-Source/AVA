import { readFileSync } from 'node:fs';
import { StrictMode, useState } from 'react';
import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it } from 'vitest';
import { AppShell } from '../components/AppShell';
import type { Preferences, SettingsSection } from './contracts';
import { installDialogTestSupport } from './dialogTestSupport';
import { SettingsScreen, settingsSections } from './SettingsScreen';
import ScreenReview from './ScreenReview';
import { defaultPreferences, providers, resourceInspections } from './screenExamples';
import { initialSubagentWorkspace } from './subagentWorkspace';
import styles from './screens.module.css';

beforeEach(installDialogTestSupport);
function Host({ active = true }: { active?: boolean }) {
  const [section, setSection] = useState<SettingsSection>('general');
  const [query, setQuery] = useState('');
  const [preferences, setPreferences] = useState<Preferences>(defaultPreferences);
  return <SettingsScreen section={section} onSectionChange={setSection} query={query} onQueryChange={setQuery} preferences={preferences} onPreferencesChange={setPreferences} active={active}
    opaque={false} onOpaqueChange={() => {}} currentModel="Aster" defaultModel="Aster" providers={providers} rules={{ state: 'ready', rows: [] }} inspections={resourceInspections} subagents={initialSubagentWorkspace()}
    onChooseModel={() => {}} onConnect={() => {}} onDisconnect={() => {}} onSaveSubagent={() => false} onReset={() => {}} onBack={() => {}} />;
}
function sections() { return screen.getByRole('navigation', { name: 'Settings sections' }); }

// DOM scroll offsets exercise the reset seam, not jsdom layout or native wheel behavior.
describe('bounded Settings scroll ownership', () => {
  it('scopes scrollbar suppression to navigation without changing either native overflow seam', () => {
    const css = readFileSync('src/screens/screens.module.css', 'utf8');
    const rules = [...css.matchAll(/([^{}]+)\{([^{}]*)\}/g)];
    expect(rules.filter(([, , declarations]) => /scrollbar-width\s*:/.test(declarations)).map(([, selector, declarations]) => [selector.trim(), declarations.match(/scrollbar-width\s*:\s*([^;]+)/)?.[1]])).toEqual([['.settingsNav', 'none']]);
    expect(rules.filter(([, selector]) => selector.includes('::-webkit-scrollbar')).map(([, selector, declarations]) => [selector.trim(), declarations.trim()])).toEqual([['.settingsNav::-webkit-scrollbar', 'display: none;']]);
    expect(css).toMatch(/\.settingsNav, \.settingsBody \{[^}]*overflow: auto;[^}]*overscroll-behavior: contain;/);
    expect(css).toMatch(/@media \(max-width: 1100px\) \{ \.settingsLayout \{[^}]*overflow: auto;[^}]*overscroll-behavior: contain;[^}]*\} \.settingsNav, \.settingsBody \{ overflow: visible;/);
  });

  it('exposes named keyboard seams with the header outside the scrolling layout', async () => {
    const user = userEvent.setup(); render(<Host />);
    const nav = sections(); const body = screen.getByRole('region', { name: 'General' });
    expect(nav).toHaveAttribute('tabindex', '0'); expect(body).toHaveAttribute('tabindex', '0');
    expect(body).toHaveAccessibleName('General'); expect(body).toContainElement(screen.getByRole('heading', { level: 2, name: 'General' }));
    expect(body.parentElement).toBe(nav.parentElement); expect(body.parentElement).toHaveClass(styles.settingsLayout);
    expect(body.parentElement).not.toContainElement(screen.getByRole('button', { name: 'Back' }));
    screen.getByRole('button', { name: 'Back' }).focus(); await user.tab(); expect(nav).toHaveFocus();
    await user.tab(); expect(screen.getByRole('searchbox', { name: 'Search settings' })).toHaveFocus();
    expect(within(nav).getAllByRole('button').at(-1)).toHaveAccessibleName('About');
    within(nav).getByRole('button', { name: 'About' }).focus(); await user.tab(); expect(body).toHaveFocus();
    await user.tab(); expect(screen.getByLabelText('Enter key behavior')).toHaveFocus();
    await user.selectOptions(screen.getByLabelText('New-chat mode'), 'plan');
    const localReset = within(body).getByRole('button', { name: 'Reset General' }); localReset.focus();
    await user.tab(); expect(within(body).getByRole('button', { name: 'Reset all preferences…' })).toHaveFocus();
    await user.tab({ shift: true }); expect(localReset).toHaveFocus();
  });

  it('resets only the content and stacked scroll seam on new sections, preserving nav identity, offset and clicked-control focus', async () => {
    const user = userEvent.setup(); render(<StrictMode><Host /></StrictMode>);
    const nav = sections(); const body = screen.getByRole('region', { name: 'General' }); const stack = body.parentElement!;
    for (const entry of settingsSections.slice(1)) {
      nav.scrollTop = 180; body.scrollTop = 700; stack.scrollTop = 320;
      const button = within(nav).getByRole('button', { name: entry.label }); await user.click(button);
      expect(sections()).toBe(nav); expect(nav.scrollTop).toBe(180); expect(button).toHaveFocus();
      expect(screen.getByRole('region', { name: entry.label })).toBe(body); expect(body.scrollTop).toBe(0);
      // The narrow stack restarts with navigation/search, rather than scrolling them out of view.
      expect(stack.scrollTop).toBe(0);
      body.scrollTop = 90; stack.scrollTop = 40; await user.click(button);
      expect(body.scrollTop).toBe(90); expect(stack.scrollTop).toBe(40); expect(button).toHaveFocus();
    }
  });

  it('does not reset on preference edits, page reset, query edits or inactive/active transitions', async () => {
    const user = userEvent.setup(); const view = render(<Host />);
    const nav = sections(); const body = screen.getByRole('region', { name: 'General' }); const stack = body.parentElement!;
    nav.scrollTop = 150; body.scrollTop = 440; stack.scrollTop = 270;
    const assertOffsets = () => { expect(nav.scrollTop).toBe(150); expect(body.scrollTop).toBe(440); expect(stack.scrollTop).toBe(270); };
    const mode = screen.getByLabelText('New-chat mode'); await user.selectOptions(mode, 'plan'); expect(mode).toHaveFocus(); assertOffsets();
    await user.click(screen.getByRole('button', { name: 'Reset General' })); expect(mode).toHaveValue('build'); assertOffsets();
    const query = screen.getByRole('searchbox', { name: 'Search settings' }); await user.type(query, 'context'); expect(query).toHaveFocus(); assertOffsets();
    await user.clear(query); assertOffsets();
    view.rerender(<Host active={false} />); assertOffsets();
    fireEvent.click(within(nav).getByRole('button', { name: 'Shortcuts' })); expect(screen.getByRole('region', { name: 'General' })).toBe(body); assertOffsets();
    view.rerender(<Host />); expect(query).toHaveFocus(); assertOffsets();
  });

  it('retains the selected search result control and restarts content on activation', async () => {
    const user = userEvent.setup(); render(<Host />);
    const nav = sections(); const body = screen.getByRole('region', { name: 'General' });
    await user.type(screen.getByRole('searchbox', { name: 'Search settings' }), 'stdio'); body.scrollTop = 200;
    const mcp = within(nav).getByRole('button', { name: 'MCP' }); await user.click(mcp);
    expect(within(nav).getByRole('button', { name: 'MCP' })).toBe(mcp); expect(mcp).toHaveFocus();
    expect(screen.getByRole('searchbox', { name: 'Search settings' })).toHaveValue('');
    expect(screen.getByRole('region', { name: 'MCP' })).toBe(body); expect(body.scrollTop).toBe(0);
  });

  it('bounds only normal Settings, keeps the header separate and preserves both panes across native overlay close', async () => {
    const user = userEvent.setup(); render(<AppShell />);
    await user.click(screen.getByRole('button', { name: 'Settings' })); const nav = await screen.findByRole('navigation', { name: 'Settings sections' });
    const page = screen.getByRole('region', { name: 'Settings page' }); const host = page.firstElementChild!;
    expect(host).toHaveAttribute('data-page', 'settings');
    const back = within(page).getByRole('button', { name: 'Back to chat' });
    await user.click(within(nav).getByRole('button', { name: 'Models' }));
    const body = screen.getByRole('region', { name: 'Models' }); const stack = body.parentElement!;
    expect(stack).not.toContainElement(back);
    nav.scrollTop = 110; body.scrollTop = 160; stack.scrollTop = 70;
    const opener = within(body).getByRole('button', { name: 'Choose current model' }); await user.click(opener);
    await user.click(within(screen.getByRole('dialog', { name: 'Choose a model' })).getByRole('button', { name: 'Cancel' }));
    expect(opener).toHaveFocus(); expect(sections()).toBe(nav); expect(screen.getByRole('region', { name: 'Models' })).toBe(body);
    expect(nav.scrollTop).toBe(110); expect(body.scrollTop).toBe(160); expect(stack.scrollTop).toBe(70);
    await user.click(back); await user.click(screen.getByRole('button', { name: 'Model for this chat' }));
    const dialog = await screen.findByRole('dialog', { name: 'Choose a model' });
    expect(dialog.closest(`.${styles.mainScreens}`)).toHaveAttribute('data-page', 'models');
    expect(document.querySelector(`.${styles.settingsPage}`)).toBeNull(); expect(document.querySelector(`.${styles.settingsScreen}`)).toBeNull();
  });

  it('bounds standalone review only while Settings is open, preserving pane identity and offsets on modal close', async () => {
    const user = userEvent.setup(); const view = render(<ScreenReview />);
    const main = view.container.querySelector(`.${styles.main}`)!;
    expect(main).toHaveAttribute('data-settings-open', 'true');
    const nav = sections(); await user.click(within(nav).getByRole('button', { name: 'Models' }));
    const body = screen.getByRole('region', { name: 'Models' }); const stack = body.parentElement!;
    expect(stack).not.toContainElement(screen.getByRole('button', { name: 'Back' }));
    nav.scrollTop = 100; body.scrollTop = 120; stack.scrollTop = 80;
    const opener = within(body).getByRole('button', { name: 'Choose default model' }); await user.click(opener);
    expect(main).toHaveAttribute('data-settings-open', 'true');
    await user.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Cancel' }));
    expect(opener).toHaveFocus(); expect(sections()).toBe(nav); expect(screen.getByRole('region', { name: 'Models' })).toBe(body);
    expect(nav.scrollTop).toBe(100); expect(body.scrollTop).toBe(120); expect(stack.scrollTop).toBe(80);
    await user.click(screen.getByRole('button', { name: 'Back' })); await user.click(screen.getByRole('button', { name: 'Back' }));
    expect(main).not.toHaveAttribute('data-settings-open'); expect(screen.queryByRole('navigation', { name: 'Settings sections' })).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Open settings' })); expect(main).toHaveAttribute('data-settings-open', 'true');
    await user.click(within(screen.getByRole('navigation', { name: 'Screen families' })).getByRole('button', { name: /^Providers/ }));
    expect(main).not.toHaveAttribute('data-settings-open'); expect(view.container.querySelector(`.${styles.settingsScreen}`)).toBeNull();
    expect(screen.getByRole('list', { name: 'Provider brands' })).toBeInTheDocument();
  });
});
