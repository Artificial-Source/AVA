import { useState } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import { createInitialState } from '../state/fixtures';
import { previewContextUsage } from '../components/contextPreview';
import type { Preferences, SettingsSection } from './contracts';
import { defaultPreferences, providers, resourceInspections } from './screenExamples';
import { initialSubagentWorkspace } from './subagentWorkspace';
import { SettingsScreen } from './SettingsScreen';

function Host({ initial = 'general', active = true, onPreferencesChange }: { initial?: SettingsSection; active?: boolean; onPreferencesChange?: (preferences: Preferences) => void }) {
  const [section, setSection] = useState<SettingsSection>(initial); const [query, setQuery] = useState(''); const [preferences, setPreferences] = useState<Preferences>(defaultPreferences);
  const supplied = createInitialState();
  return <><SettingsScreen section={section} onSectionChange={setSection} query={query} onQueryChange={setQuery} preferences={preferences} onPreferencesChange={(next) => { onPreferencesChange?.(next); setPreferences(next); }} active={active} projects={supplied.projects} contextUsage={previewContextUsage(supplied.chats[0])} opaque={false} onOpaqueChange={() => {}} defaultModel="Aster" providers={providers} rules={{ state: 'ready', rows: [] }} inspections={resourceInspections} subagents={initialSubagentWorkspace()} onChooseModel={() => {}} onConnect={() => {}} onDisconnect={() => {}} onSaveSubagent={() => false} onBack={() => {}} onReset={() => setPreferences(defaultPreferences)} /><pre data-testid="preferences">{JSON.stringify(preferences)}</pre></>;
}
const prefs = () => JSON.parse(screen.getByTestId('preferences').textContent!) as Preferences;
function leaf(label: string) { fireEvent.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: label })); }
function change(label: string, value: string) { fireEvent.change(screen.getByLabelText(label), { target: { value } }); }
const keycap = (action = 'New chat') => screen.getByRole('button', { name: new RegExp(`^${action} shortcut:`) });
function press(target: HTMLElement, init: KeyboardEventInit) { fireEvent.keyDown(target, init); }

describe('independent Settings bodies', () => {
  it('edits General, conversation and notification values with section-only, Appearance-only and global reset ownership', () => {
    render(<Host />); change('Enter key behavior', 'newline'); change('New-chat mode', 'plan'); fireEvent.click(screen.getByLabelText('Name new chats from first message')); change('New-chat inspector', 'closed');
    expect(screen.getByLabelText('New-chat project').querySelectorAll('option')).toHaveLength(4);
    leaf('Conversation behavior'); fireEvent.click(screen.getByLabelText('Wrap code lines')); fireEvent.click(screen.getByLabelText('Show completed-tool output')); expect(screen.getByLabelText('Code wrap sample')).toHaveAttribute('data-wrap', 'true');
    leaf('Notifications & sounds'); fireEvent.click(screen.getByLabelText('Mute all notifications')); expect(prefs().notificationPolicy!.muted).toBe(true);
    leaf('Appearance'); change('Interface font', 'system'); fireEvent.click(screen.getByRole('button', { name: 'Reset appearance' }));
    expect(prefs()).toMatchObject({ general: { mode: 'plan', titleFromFirstMessage: true, inspector: 'closed' }, enter: 'newline', conversation: { wrapCode: true, showCompletedOutput: false }, notificationPolicy: { muted: true } });
    leaf('General'); fireEvent.click(screen.getByRole('button', { name: 'Reset general' })); expect(prefs().enter).toBe('send'); expect(prefs().conversation!.wrapCode).toBe(true);
    leaf('Conversation behavior'); fireEvent.click(screen.getByRole('button', { name: 'Reset conversation' })); expect(prefs().conversation!.wrapCode).toBe(false); expect(prefs().notificationPolicy!.muted).toBe(true);
    leaf('Notifications & sounds'); fireEvent.click(screen.getByRole('button', { name: 'Reset notifications' })); expect(prefs().notificationPolicy!.muted).toBe(false);
    leaf('Shortcuts'); fireEvent.click(screen.getByLabelText('Enable app shortcuts')); leaf('Context management'); change('Compaction trigger percentage', '90');
    leaf('General'); fireEvent.click(screen.getByRole('button', { name: 'Reset all preferences…' })); expect(prefs()).toEqual(defaultPreferences);
  });
  it('keeps every page free of intros, uses checkboxes for on/off settings and sentence-case resets', () => {
    render(<Host />);
    for (const [page, reset] of [['General', 'Reset general'], ['Appearance', 'Reset appearance'], ['Shortcuts', 'Reset all shortcuts'], ['Notifications & sounds', 'Reset notifications'], ['Conversation behavior', 'Reset conversation'], ['Context management', 'Reset context']]) {
      leaf(page); const body = screen.getByRole('region', { name: page });
      expect(within(body).getByRole('button', { name: reset })).toBeVisible();
      expect(body).not.toHaveTextContent(/this visit|not saved|not connected|supplied|example|preview only/i);
    }
    leaf('Conversation behavior'); expect(screen.getByRole('checkbox', { name: 'Show completed-tool output' })).toBeChecked();
    expect(screen.getByRole('combobox', { name: 'Tool details' })).toHaveValue('compact');
  });
  it('shows accurate display defaults and derived Custom, restores output on presets, and keeps no-op/reset/wrap ownership separate', () => {
    const onPreferencesChange = vi.fn(); render(<Host initial="conversation" onPreferencesChange={onPreferencesChange} />);
    const display = screen.getByRole('combobox', { name: 'Tool details' });
    const sample = screen.getByRole('figure', { name: 'Live conversation preview' });
    const openSamples = () => [...sample.querySelectorAll('details')].map((node) => node.open);
    expect(display).toHaveValue('compact'); expect(openSamples()).toEqual([false, false, false]);
    change('Tool details', 'compact'); expect(onPreferencesChange).not.toHaveBeenCalled();
    change('Tool details', 'auto'); expect(openSamples()).toEqual([true, false, true]);
    change('Tool details', 'expanded'); expect(openSamples()).toEqual([true, true, true]);
    fireEvent.click(within(sample).getByText('Running tool')); expect(openSamples()).toEqual([false, true, true]);
    change('Tool details', 'auto'); expect(openSamples()).toEqual([true, false, true]);
    change('Tool details', 'expanded');
    fireEvent.click(screen.getByLabelText('Wrap code lines')); fireEvent.click(screen.getByLabelText('Wrap diff lines')); change('After sending a message', 'keep');
    fireEvent.click(screen.getByLabelText('Show completed-tool output')); expect(display).toHaveValue('custom'); expect(within(display).getByRole('option', { name: 'Custom · output hidden' })).toBeDisabled();
    expect(prefs().conversation!.toolDisplay).toBe('expanded');
    const count = onPreferencesChange.mock.calls.length; change('Tool details', 'custom'); expect(onPreferencesChange).toHaveBeenCalledTimes(count);
    change('Tool details', 'expanded'); expect(display).toHaveValue('expanded'); expect(prefs().conversation).toEqual({ toolDisplay: 'expanded', showCompletedOutput: true, wrapCode: true, wrapDiff: false, scrollAfterSend: false });
    change('Tool details', 'expanded'); expect(onPreferencesChange).toHaveBeenCalledTimes(count + 1);
    change('Tool details', 'compact'); expect(openSamples()).toEqual([false, false, false]);
    fireEvent.click(screen.getByRole('button', { name: 'Reset conversation' })); expect(prefs().conversation).toEqual(defaultPreferences.conversation);
    expect(screen.getByRole('button', { name: 'Reset conversation' })).toBeDisabled();
    change('Tool details', 'auto'); expect(display).toHaveValue('auto'); // No-op/reset never consumes the section gate.
    expect(prefs().general).toEqual(defaultPreferences.general);
  });
  it.each(['hidden', 'inert', 'inactive', 'stale', 'departed'] as const)('rejects %s display controls without forwarding preferences', (invalid) => {
    const onPreferencesChange = vi.fn(); const view = render(<Host initial="conversation" onPreferencesChange={onPreferencesChange} />);
    const control = screen.getByRole('combobox', { name: 'Tool details' }) as HTMLSelectElement;
    const propsKey = Object.keys(control).find((key) => key.startsWith('__reactProps'))!;
    const callback = (control as unknown as Record<string, { onChange: (event: unknown) => void }>)[propsKey].onChange;
    if (invalid === 'hidden') control.hidden = true;
    else if (invalid === 'inert') control.setAttribute('inert', '');
    else if (invalid === 'inactive') view.rerender(<Host initial="conversation" active={false} onPreferencesChange={onPreferencesChange} />);
    else if (invalid === 'stale') fireEvent.click(screen.getByLabelText('Wrap code lines'));
    else leaf('General');
    const count = onPreferencesChange.mock.calls.length; control.value = 'expanded'; act(() => callback({ currentTarget: control }));
    expect(onPreferencesChange).toHaveBeenCalledTimes(count);
  });
  it('records on the keycap itself, cancels with Escape, validates reservations in the error tone and resets or unassigns per row', () => {
    render(<Host initial="shortcuts" />);
    expect(keycap()).toHaveAccessibleName('New chat shortcut: Ctrl+Shift+9'); expect(screen.queryByText(/^Default:/)).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Reset New chat shortcut' })).not.toBeInTheDocument();
    expect(keycap('Open command palette')).toHaveAccessibleName('Open command palette shortcut: Ctrl+K'); expect(keycap('Find a chat')).toHaveAccessibleName('Find a chat shortcut: Ctrl+P');
    fireEvent.click(keycap()); expect(keycap()).toHaveAccessibleName('New chat shortcut: Press keys… Esc cancels');
    press(keycap(), { key: 'l', ctrlKey: true }); expect(screen.getByRole('alert')).toHaveTextContent('Reserved'); expect(keycap()).toHaveAccessibleName(/Press keys/);
    press(keycap(), { key: 'Escape', repeat: true }); expect(keycap()).toHaveAccessibleName(/Press keys/);
    fireEvent.compositionStart(keycap()); press(keycap(), { key: 'Escape' }); expect(keycap()).toHaveAccessibleName(/Press keys/); fireEvent.compositionEnd(keycap());
    keycap().focus(); press(keycap(), { key: 'Escape' }); expect(screen.getByRole('heading', { level: 2, name: 'Shortcuts' })).toBeVisible(); expect(keycap()).toHaveAccessibleName('New chat shortcut: Ctrl+Shift+9'); expect(keycap()).toHaveFocus();
    // Every safe chord has a default owner; free Stop's Ctrl+Shift+. before recording it elsewhere.
    fireEvent.click(screen.getByRole('button', { name: 'Unassign Stop the run' })); expect(prefs().shortcuts!.overrides.stop).toBeNull();
    fireEvent.click(keycap()); press(keycap(), { key: '>', code: 'Period', ctrlKey: true, shiftKey: true });
    expect(prefs().shortcuts!.overrides.newChat).toBe('Mod+Shift+.'); expect(keycap()).toHaveAccessibleName('New chat shortcut: Ctrl+Shift+.');
    expect(keycap().closest('tr')).toHaveTextContent('Default: Ctrl+Shift+9');
    fireEvent.click(screen.getByRole('button', { name: 'Unassign New chat' })); expect(prefs().shortcuts!.overrides.newChat).toBeNull(); expect(keycap()).toHaveFocus();
    expect(screen.queryByRole('button', { name: 'Unassign New chat' })).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Reset New chat shortcut' })); expect(prefs().shortcuts!.overrides.newChat).toBeUndefined(); expect(keycap()).toHaveFocus();
    fireEvent.click(keycap()); fireEvent.blur(keycap()); expect(keycap()).toHaveAccessibleName('New chat shortcut: Ctrl+Shift+9');
  });
  it('shows how to record in plain sight and lists the built-in navigation keys with no recorder', () => {
    render(<Host initial="shortcuts" />);
    const hint = screen.getByText('Select a shortcut, then press the new keys. Esc cancels.');
    expect(hint).not.toHaveClass('srOnly'); expect(keycap()).toHaveAccessibleDescription(hint.textContent!);
    const navigation = screen.getByRole('group', { name: 'Navigation' });
    const keys = within(navigation).getByRole('table', { name: 'Navigation keys' });
    expect(keys).toHaveAccessibleDescription('Always on. These keys can’t be changed.');
    expect(within(keys).getAllByRole('rowheader').map((cell) => cell.querySelector('span')!.textContent)).toEqual(['Move between areas', 'Rename chat', 'Close or go back', 'Send message']);
    expect(Array.from(keys.querySelectorAll('tbody tr'), (row) => Array.from(row.querySelectorAll('kbd'), (key) => key.textContent))).toEqual([['F6', 'Shift+F6'], ['F2'], ['Esc'], ['Ctrl+Enter']]);
    expect(within(navigation).queryByRole('button')).not.toBeInTheDocument();
  });
  it('offers an inline move when a shortcut conflicts, including when a reset default is taken', () => {
    render(<Host initial="shortcuts" />);
    fireEvent.click(keycap()); press(keycap(), { key: '<', code: 'Comma', ctrlKey: true, shiftKey: true });
    expect(screen.getByRole('alert')).toHaveTextContent('Ctrl+Shift+, is already used by Open Settings.'); expect(keycap()).toHaveFocus(); expect(keycap()).toHaveAccessibleDescription(/already used by Open Settings/);
    expect(prefs().shortcuts!.overrides).toEqual({});
    fireEvent.click(screen.getByRole('button', { name: 'Move from Open Settings' }));
    expect(prefs().shortcuts!.overrides).toEqual({ newChat: 'Mod+Shift+,', settings: null }); expect(keycap()).toHaveFocus(); expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Reset Open Settings shortcut' }));
    expect(screen.getByRole('alert')).toHaveTextContent('The default, Ctrl+Shift+,, is used by New chat.'); expect(keycap('Open Settings')).toHaveFocus();
    fireEvent.click(screen.getByRole('button', { name: 'Move from New chat' })); expect(prefs().shortcuts!.overrides).toEqual({ newChat: null });
    fireEvent.click(screen.getByRole('button', { name: 'Reset all shortcuts' })); expect(prefs().shortcuts).toEqual(defaultPreferences.shortcuts);
  });
  it('drops a recording when the page goes inactive instead of keeping a hidden writer', () => {
    const view = render(<Host initial="shortcuts" />); fireEvent.click(keycap()); const recorder = keycap();
    view.rerender(<Host initial="shortcuts" active={false} />);
    press(recorder, { key: '9', code: 'Digit9', ctrlKey: true, shiftKey: true }); expect(prefs().shortcuts!.overrides).toEqual({});
  });
  it('keeps every new page action and reset inert under an overlay, hidden parent or departed DOM', () => {
    const view = render(<Host />); change('New-chat mode', 'plan'); const reset = screen.getByRole('button', { name: 'Reset general' }); const mode = screen.getByLabelText('New-chat mode');
    view.rerender(<Host active={false} />); fireEvent.click(reset); change('New-chat mode', 'build'); expect(prefs().general!.mode).toBe('plan');
    view.rerender(<Host />); const root = screen.getByRole('region', { name: 'Settings' }); root.hidden = true; fireEvent.click(reset); fireEvent.change(mode, { target: { value: 'build' } }); expect(prefs().general!.mode).toBe('plan'); root.hidden = false;
    for (const [section, control] of [['Conversation behavior', 'Wrap code lines'], ['Notifications & sounds', 'Mute all notifications'], ['Context management', 'Automatic compaction'], ['Shortcuts', 'Enable app shortcuts']]) {
      leaf(section); const element = screen.getByLabelText(control); const before = prefs(); view.rerender(<Host active={false} />); fireEvent.click(element); expect(prefs()).toEqual(before); view.rerender(<Host />);
    }
    leaf('General'); fireEvent.click(reset); expect(prefs().general!.mode).toBe('plan'); // disconnected old reset
  });
  it('previews tones from each event row, keeps silence rules and never calls OS notification APIs', () => {
    const Audio = vi.fn(); const notify = vi.fn(); vi.stubGlobal('AudioContext', Audio); vi.stubGlobal('Notification', notify);
    render(<Host initial="notifications" />); expect(Audio).not.toHaveBeenCalled();
    for (const event of ['Task finished', 'Needs attention', 'Task failed']) expect(screen.getByRole('button', { name: `Preview ${event} sound` })).toBeEnabled();
    fireEvent.click(screen.getByRole('button', { name: 'Preview Task finished sound' })); expect(screen.getByText('Sound needs a direct click.')).toBeVisible(); expect(Audio).not.toHaveBeenCalled();
    fireEvent.change(screen.getByLabelText('Task failed sound'), { target: { value: 'none' } }); expect(screen.getByRole('button', { name: 'Preview Task failed sound' })).toBeDisabled();
    fireEvent.click(screen.getByLabelText('Task failed notifications')); expect(prefs().notificationPolicy!.channels.failure).toEqual({ enabled: false, tone: 'none' });
    fireEvent.click(screen.getByLabelText('Mute all notifications')); expect(screen.getByRole('button', { name: 'Preview Task finished sound' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Reset notifications' })); expect(Audio).not.toHaveBeenCalled(); expect(notify).not.toHaveBeenCalled();
    expect(prefs().notificationPolicy).toEqual(defaultPreferences.notificationPolicy);
  });
  it('updates a bounded context estimate, supports empty/invalid editing in the error tone and never changes the measured facts', async () => {
    const user = userEvent.setup(); render(<Host initial="context" />); const projection = screen.getByRole('region', { name: 'This chat' });
    expect(projection).toHaveTextContent('171,200 tokens · 80%'); expect(projection).toHaveTextContent('this chat would be compacted now');
    const percentage = screen.getByLabelText('Compaction trigger percentage'); await user.clear(percentage); expect(percentage).toHaveValue(null);
    expect(screen.getByRole('alert')).toHaveTextContent('last valid'); expect(percentage).toHaveAccessibleDescription(/Enter a whole number from 1 to 95/); expect(projection).toHaveTextContent('171,200');
    await user.type(percentage, '95'); expect(projection).toHaveTextContent('203,300 tokens · 95%'); expect(projection).toHaveTextContent('below the compaction point');
    change('Compaction trigger percentage', '96'); expect(percentage).toHaveAttribute('aria-invalid', 'true'); expect(projection).toHaveTextContent('203,300');
    change('Recent turns to keep', '0'); change('Recent token budget', '100000'); expect(projection).toHaveTextContent('0 recent turns · 100,000 token budget'); expect(projection).toHaveTextContent('173,120 tokens · 81%');
    fireEvent.click(screen.getByRole('button', { name: 'Reset context' })); expect(percentage).toHaveValue(80); expect(screen.queryByRole('alert')).not.toBeInTheDocument(); expect(projection).toHaveTextContent('2 recent turns · 20,000 token budget');
    // Units sit inside the field box; the token budget reads with thousands separators and accepts them as typed.
    const budget = screen.getByLabelText('Recent token budget'); expect(budget).toHaveValue('20,000'); expect(budget.nextElementSibling).toHaveTextContent('tokens'); expect(percentage.nextElementSibling).toHaveTextContent('%');
    change('Recent token budget', '12,500'); expect(projection).toHaveTextContent('12,500 token budget'); change('Recent token budget', '30000'); expect(budget).toHaveValue('30000'); fireEvent.blur(budget); expect(budget).toHaveValue('30,000');
    change('Recent token budget', '1.5'); expect(budget).toHaveAttribute('aria-invalid', 'true'); expect(projection).toHaveTextContent('30,000 token budget');
    expect(screen.queryByRole('button', { name: /Compact now|Save/ })).not.toBeInTheDocument();
  });
  it('keeps accepting edits after an invalid context draft is corrected to the same accepted value without forwarding the no-op', () => {
    const onPreferencesChange = vi.fn(); render(<Host initial="context" onPreferencesChange={onPreferencesChange} />);
    const percentage = screen.getByLabelText('Compaction trigger percentage'); const projection = screen.getByRole('region', { name: 'This chat' });
    expect(percentage).toHaveValue(80); change('Compaction trigger percentage', '90'); expect(projection).toHaveTextContent('192,600 tokens · 90%');
    change('Compaction trigger percentage', '96'); expect(percentage).toHaveAttribute('aria-invalid', 'true'); expect(prefs().contextPolicy!.triggerPercent).toBe(90);
    change('Compaction trigger percentage', '90'); expect(percentage).toHaveValue(90); expect(percentage).not.toHaveAttribute('aria-invalid'); expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    change('Recent turns to keep', '0'); change('Recent token budget', '0');
    expect(screen.getByLabelText('Recent turns to keep')).toHaveValue(0); expect(screen.getByLabelText('Recent token budget')).toHaveValue('0');
    for (const label of ['Recent turns to keep', 'Recent token budget']) expect(screen.getByLabelText(label)).not.toHaveAttribute('aria-invalid');
    expect(prefs().contextPolicy).toEqual({ ...defaultPreferences.contextPolicy, triggerPercent: 90, recentTurns: 0, recentTokens: 0 });
    expect(projection).toHaveTextContent('192,600 tokens · 90%'); expect(projection).toHaveTextContent('0 recent turns · 0 token budget'); expect(projection).toHaveTextContent('173,120 tokens · 81%');
    expect(onPreferencesChange.mock.calls.map(([next]) => next.contextPolicy)).toEqual([
      { ...defaultPreferences.contextPolicy, triggerPercent: 90 },
      { ...defaultPreferences.contextPolicy, triggerPercent: 90, recentTurns: 0 },
      { ...defaultPreferences.contextPolicy, triggerPercent: 90, recentTurns: 0, recentTokens: 0 },
    ]);
  });
  it('resets an invalid draft at default context policy without forwarding a no-op or blocking subsequent edits', () => {
    const onPreferencesChange = vi.fn(); render(<Host initial="context" onPreferencesChange={onPreferencesChange} />);
    const percentage = screen.getByLabelText('Compaction trigger percentage'); const projection = screen.getByRole('region', { name: 'This chat' });
    change('Compaction trigger percentage', '96'); expect(percentage).toHaveAttribute('aria-invalid', 'true'); expect(prefs().contextPolicy).toEqual(defaultPreferences.contextPolicy);
    fireEvent.click(screen.getByRole('button', { name: 'Reset context' })); expect(percentage).toHaveValue(80); expect(percentage).not.toHaveAttribute('aria-invalid'); expect(screen.queryByRole('alert')).not.toBeInTheDocument(); expect(screen.getByRole('button', { name: 'Reset context' })).toBeDisabled();
    change('Recent turns to keep', '0'); change('Recent token budget', '0');
    expect(prefs().contextPolicy).toEqual({ ...defaultPreferences.contextPolicy, recentTurns: 0, recentTokens: 0 });
    expect(projection).toHaveTextContent('171,200 tokens · 80%'); expect(projection).toHaveTextContent('0 recent turns · 0 token budget');
    expect(onPreferencesChange.mock.calls.map(([next]) => next.contextPolicy)).toEqual([
      { ...defaultPreferences.contextPolicy, recentTurns: 0 },
      { ...defaultPreferences.contextPolicy, recentTurns: 0, recentTokens: 0 },
    ]);
  });
  it('rejects a saved old section callback after navigation', () => {
    render(<Host />); const input = screen.getByLabelText('New-chat mode');
    const propsKey = Object.keys(input).find((key) => key.startsWith('__reactProps'))!;
    const callback = (input as unknown as Record<string, { onChange: (event: unknown) => void }>)[propsKey].onChange;
    leaf('Conversation behavior'); act(() => callback({ currentTarget: { value: 'plan' } })); expect(prefs().general!.mode).toBe('build');
  });
});
