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
  return <><SettingsScreen section={section} onSectionChange={setSection} query={query} onQueryChange={setQuery} preferences={preferences} onPreferencesChange={(next) => { onPreferencesChange?.(next); setPreferences(next); }} active={active} projects={supplied.projects} contextUsage={previewContextUsage(supplied.chats[0])} opaque={false} onOpaqueChange={() => {}} currentModel="Aster" defaultModel="Aster" providers={providers} rules={{ state: 'ready', rows: [] }} inspections={resourceInspections} subagents={initialSubagentWorkspace()} onChooseModel={() => {}} onConnect={() => {}} onDisconnect={() => {}} onSaveSubagent={() => false} onBack={() => {}} onReset={() => setPreferences(defaultPreferences)} /><pre data-testid="preferences">{JSON.stringify(preferences)}</pre></>;
}
const prefs = () => JSON.parse(screen.getByTestId('preferences').textContent!) as Preferences;
function leaf(label: string) { fireEvent.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: label })); }
function change(label: string, value: string) { fireEvent.change(screen.getByLabelText(label), { target: { value } }); }
function record(label = 'New chat') { fireEvent.click(screen.getByRole('button', { name: `Record shortcut: ${label}` })); return screen.getByRole('group', { name: `Recording ${label}` }); }

describe('five independent Settings bodies', () => {
  it('edits General, conversation and notification values with section-only, Appearance-only and global reset ownership', () => {
    render(<Host />); change('Enter key behavior', 'newline'); change('New-chat mode', 'plan'); fireEvent.click(screen.getByLabelText('Name new chats from first message')); change('New-chat inspector', 'closed');
    expect(screen.getByLabelText('New-chat project').querySelectorAll('option')).toHaveLength(4);
    leaf('Conversation behavior'); fireEvent.click(screen.getByLabelText('Wrap code lines')); change('Completed-tool output', 'hide'); expect(screen.getByLabelText('Code wrap sample')).toHaveAttribute('data-wrap', 'true');
    leaf('Notifications & sounds'); fireEvent.click(screen.getByLabelText('Mute all notifications')); expect(prefs().notificationPolicy!.muted).toBe(true);
    leaf('Appearance'); change('Interface font', 'system'); fireEvent.click(screen.getByRole('button', { name: 'Reset appearance' }));
    expect(prefs()).toMatchObject({ general: { mode: 'plan', titleFromFirstMessage: true, inspector: 'closed' }, enter: 'newline', conversation: { wrapCode: true, showCompletedOutput: false }, notificationPolicy: { muted: true } });
    leaf('General'); fireEvent.click(screen.getByRole('button', { name: 'Reset General' })); expect(prefs().enter).toBe('send'); expect(prefs().conversation!.wrapCode).toBe(true);
    leaf('Conversation behavior'); fireEvent.click(screen.getByRole('button', { name: 'Reset Conversation' })); expect(prefs().conversation!.wrapCode).toBe(false); expect(prefs().notificationPolicy!.muted).toBe(true);
    leaf('Notifications & sounds'); fireEvent.click(screen.getByRole('button', { name: 'Reset Notifications' })); expect(prefs().notificationPolicy!.muted).toBe(false);
    leaf('Shortcuts'); fireEvent.click(screen.getByLabelText('Enable app shortcuts')); leaf('Context management'); change('Compaction trigger percentage', '90');
    leaf('General'); fireEvent.click(screen.getByRole('button', { name: 'Reset all preferences…' })); expect(prefs()).toEqual(defaultPreferences);
  });
  it('shows accurate display defaults and derived Custom, restores output on presets, and keeps no-op/reset/wrap ownership separate', () => {
    const onPreferencesChange = vi.fn(); render(<Host initial="conversation" onPreferencesChange={onPreferencesChange} />);
    const display = screen.getByRole('combobox', { name: 'Chat display' });
    const sample = screen.getByRole('figure', { name: 'Live conversation preview' });
    const openSamples = () => [...sample.querySelectorAll('details')].map((node) => node.open);
    expect(display).toHaveValue('compact'); expect(openSamples()).toEqual([false, false, false]);
    change('Chat display', 'compact'); expect(onPreferencesChange).not.toHaveBeenCalled();
    change('Chat display', 'auto'); expect(openSamples()).toEqual([true, false, true]);
    change('Chat display', 'expanded'); expect(openSamples()).toEqual([true, true, true]);
    fireEvent.click(within(sample).getByText('Running tool · sample details')); expect(openSamples()).toEqual([false, true, true]);
    change('Chat display', 'auto'); expect(openSamples()).toEqual([true, false, true]);
    change('Chat display', 'expanded');
    fireEvent.click(screen.getByLabelText('Wrap code lines')); fireEvent.click(screen.getByLabelText('Wrap diff lines')); change('Scroll after local send', 'keep');
    change('Completed-tool output', 'hide'); expect(display).toHaveValue('custom'); expect(within(display).getByRole('option', { name: 'Custom · output hidden' })).toBeDisabled();
    expect(prefs().conversation!.toolDisplay).toBe('expanded');
    const count = onPreferencesChange.mock.calls.length; change('Chat display', 'custom'); expect(onPreferencesChange).toHaveBeenCalledTimes(count);
    change('Chat display', 'expanded'); expect(display).toHaveValue('expanded'); expect(prefs().conversation).toEqual({ toolDisplay: 'expanded', showCompletedOutput: true, wrapCode: true, wrapDiff: false, scrollAfterSend: false });
    change('Chat display', 'expanded'); expect(onPreferencesChange).toHaveBeenCalledTimes(count + 1);
    change('Chat display', 'compact'); expect(openSamples()).toEqual([false, false, false]);
    fireEvent.click(screen.getByRole('button', { name: 'Reset Conversation' })); expect(prefs().conversation).toEqual(defaultPreferences.conversation);
    expect(screen.getByRole('button', { name: 'Reset Conversation' })).toBeDisabled();
    change('Chat display', 'auto'); expect(display).toHaveValue('auto'); // No-op/reset never consumes the section gate.
    expect(prefs().general).toEqual(defaultPreferences.general);
  });
  it.each(['hidden', 'inert', 'inactive', 'stale', 'departed'] as const)('rejects %s display controls without forwarding preferences', (invalid) => {
    const onPreferencesChange = vi.fn(); const view = render(<Host initial="conversation" onPreferencesChange={onPreferencesChange} />);
    const control = screen.getByRole('combobox', { name: 'Chat display' }) as HTMLSelectElement;
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
  it('records real keys, cancels with focus, validates conflicts/reservations and resets/unassigns per row or all', async () => {
    const user = userEvent.setup(); render(<Host initial="shortcuts" />);
    let capture = record(); expect(capture).toHaveFocus();
    fireEvent.keyDown(capture, { key: '<', code: 'Comma', ctrlKey: true, shiftKey: true }); expect(screen.getByRole('alert')).toHaveTextContent('Already assigned to Open Settings');
    fireEvent.keyDown(capture, { key: 'l', ctrlKey: true }); expect(screen.getByRole('alert')).toHaveTextContent('Reserved');
    fireEvent.keyDown(capture, { key: 'Escape', repeat: true }); expect(capture).toBeInTheDocument();
    fireEvent.compositionStart(capture); fireEvent.keyDown(capture, { key: 'Escape' }); expect(capture).toBeInTheDocument(); fireEvent.compositionEnd(capture);
    fireEvent.keyDown(capture, { key: 'Escape' }); expect(screen.getByRole('heading', { level: 2, name: 'Shortcuts' })).toBeVisible(); expect(screen.getByRole('button', { name: 'Record shortcut: New chat' })).toHaveFocus();
    capture = record(); fireEvent.keyDown(capture, { key: '>', code: 'Period', ctrlKey: true, shiftKey: true }); expect(prefs().shortcuts!.overrides.newChat).toBe('Mod+Shift+.'); expect(screen.queryByRole('group', { name: 'Recording New chat' })).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Unassign New chat' })); expect(prefs().shortcuts!.overrides.newChat).toBeNull();
    fireEvent.click(screen.getByRole('button', { name: 'Reset shortcut: New chat' })); expect(prefs().shortcuts!.overrides.newChat).toBeUndefined();
    capture = record(); await user.tab(); expect(screen.getByRole('button', { name: 'Cancel recording' })).toHaveFocus(); await user.keyboard('{Enter}'); expect(capture).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Unassign Open Settings' })); capture = record('Stop viewed mock'); fireEvent.keyDown(capture, { key: '<', code: 'Comma', ctrlKey: true, shiftKey: true });
    fireEvent.click(screen.getByRole('button', { name: 'Reset shortcut: Open Settings' })); expect(screen.getByRole('alert')).toHaveTextContent('Default is assigned to Stop viewed mock');
    fireEvent.click(screen.getByRole('button', { name: 'Reset all shortcuts' })); expect(prefs().shortcuts).toEqual(defaultPreferences.shortcuts);
  });
  it('searches actions and cancels an invisible recording instead of keeping a hidden writer', () => {
    render(<Host initial="shortcuts" />); const capture = record(); change('Search shortcuts', 'inspector');
    expect(capture).not.toBeInTheDocument(); expect(screen.queryByRole('button', { name: 'Record shortcut: New chat' })).not.toBeInTheDocument();
    fireEvent.keyDown(capture, { key: '9', code: 'Digit9', ctrlKey: true, shiftKey: true }); expect(prefs().shortcuts!.overrides).toEqual({});
    expect(screen.getByRole('button', { name: 'Record shortcut: Toggle Context' })).toBeVisible(); change('Search shortcuts', 'not-an-action'); expect(screen.getByRole('status')).toHaveTextContent('No matching shortcuts');
  });
  it('keeps every new page action and reset inert under an overlay, hidden parent or departed DOM', () => {
    const view = render(<Host />); change('New-chat mode', 'plan'); const reset = screen.getByRole('button', { name: 'Reset General' }); const mode = screen.getByLabelText('New-chat mode');
    view.rerender(<Host active={false} />); fireEvent.click(reset); change('New-chat mode', 'build'); expect(prefs().general!.mode).toBe('plan');
    view.rerender(<Host />); const root = screen.getByRole('region', { name: 'Settings' }); root.hidden = true; fireEvent.click(reset); fireEvent.change(mode, { target: { value: 'build' } }); expect(prefs().general!.mode).toBe('plan'); root.hidden = false;
    for (const [section, control] of [['Conversation behavior', 'Wrap code lines'], ['Notifications & sounds', 'Mute all notifications'], ['Context management', 'Preview automatic compaction'], ['Shortcuts', 'Enable app shortcuts']]) {
      leaf(section); const element = screen.getByLabelText(control); const before = prefs(); view.rerender(<Host active={false} />); fireEvent.click(element); expect(prefs()).toEqual(before); view.rerender(<Host />);
    }
    leaf('General'); fireEvent.click(reset); expect(prefs().general!.mode).toBe('plan'); // disconnected old reset
  });
  it('renders truthful delivery cards, silence rules and untrusted-click audio rejection without OS APIs', () => {
    const Audio = vi.fn(); const notify = vi.fn(); vi.stubGlobal('AudioContext', Audio); vi.stubGlobal('Notification', notify);
    render(<Host initial="notifications" />); expect(Audio).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Test event' })); expect(within(screen.getByRole('region', { name: 'Local notification test' })).getByRole('status')).toHaveTextContent('Delivered preview · Completion'); expect(Audio).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Play sound' })); expect(within(screen.getByRole('region', { name: 'Local notification test' })).getByRole('status')).toHaveTextContent('requires a direct user click'); expect(Audio).not.toHaveBeenCalled();
    change('Simulated app state', 'foreground'); fireEvent.click(screen.getByRole('button', { name: 'Test event' })); expect(within(screen.getByRole('region', { name: 'Local notification test' })).getByRole('status')).toHaveTextContent('Suppressed preview'); expect(screen.getByRole('button', { name: 'Play sound' })).toBeDisabled();
    fireEvent.click(screen.getByLabelText('Background-only notifications')); fireEvent.click(screen.getByRole('button', { name: 'Test event' })); expect(within(screen.getByRole('region', { name: 'Local notification test' })).getByRole('status')).toHaveTextContent('Delivered preview');
    change('Test event', 'failure'); fireEvent.click(screen.getByLabelText('Failure notifications')); fireEvent.click(screen.getByRole('button', { name: 'Test event' })); expect(within(screen.getByRole('region', { name: 'Local notification test' })).getByRole('status')).toHaveTextContent('event channel is off');
    fireEvent.click(screen.getByRole('button', { name: 'Reset Notifications' })); expect(Audio).not.toHaveBeenCalled(); expect(notify).not.toHaveBeenCalled();
  });
  it('updates a bounded context projection, supports empty/invalid editing and never changes the supplied facts', async () => {
    const user = userEvent.setup(); render(<Host initial="context" />); const projection = screen.getByRole('region', { name: 'Context policy projection' });
    expect(projection).toHaveTextContent('171,200 tokens · 80%'); expect(projection).toHaveTextContent('Eligible in this preview');
    const percentage = screen.getByLabelText('Compaction trigger percentage'); await user.clear(percentage); expect(percentage).toHaveValue(null); expect(screen.getByRole('alert')).toHaveTextContent('last valid'); expect(projection).toHaveTextContent('171,200');
    await user.type(percentage, '95'); expect(projection).toHaveTextContent('203,300 tokens · 95%'); expect(projection).toHaveTextContent('Below the preview trigger');
    change('Compaction trigger percentage', '96'); expect(percentage).toHaveAttribute('aria-invalid', 'true'); expect(projection).toHaveTextContent('203,300');
    change('Recent turns to retain', '0'); change('Recent token budget', '100000'); expect(projection).toHaveTextContent('0 recent turns · 100,000 token budget'); expect(projection).toHaveTextContent('173,120 tokens · 81%');
    fireEvent.click(screen.getByRole('button', { name: 'Reset Context' })); expect(percentage).toHaveValue(80); expect(screen.queryByRole('alert')).not.toBeInTheDocument(); expect(projection).toHaveTextContent('2 recent turns · 20,000 token budget');
    expect(screen.queryByRole('button', { name: /Compact now|Save/ })).not.toBeInTheDocument();
  });
  it('keeps accepting edits after an invalid context draft is corrected to the same accepted value without forwarding the no-op', () => {
    const onPreferencesChange = vi.fn(); render(<Host initial="context" onPreferencesChange={onPreferencesChange} />);
    const percentage = screen.getByLabelText('Compaction trigger percentage'); const projection = screen.getByRole('region', { name: 'Context policy projection' });
    expect(percentage).toHaveValue(80); change('Compaction trigger percentage', '90'); expect(projection).toHaveTextContent('192,600 tokens · 90%');
    change('Compaction trigger percentage', '96'); expect(percentage).toHaveAttribute('aria-invalid', 'true'); expect(prefs().contextPolicy!.triggerPercent).toBe(90);
    change('Compaction trigger percentage', '90'); expect(percentage).toHaveValue(90); expect(percentage).not.toHaveAttribute('aria-invalid'); expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    change('Recent turns to retain', '0'); change('Recent token budget', '0');
    for (const label of ['Recent turns to retain', 'Recent token budget']) { expect(screen.getByLabelText(label)).toHaveValue(0); expect(screen.getByLabelText(label)).not.toHaveAttribute('aria-invalid'); }
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
    const percentage = screen.getByLabelText('Compaction trigger percentage'); const projection = screen.getByRole('region', { name: 'Context policy projection' });
    change('Compaction trigger percentage', '96'); expect(percentage).toHaveAttribute('aria-invalid', 'true'); expect(prefs().contextPolicy).toEqual(defaultPreferences.contextPolicy);
    fireEvent.click(screen.getByRole('button', { name: 'Reset Context' })); expect(percentage).toHaveValue(80); expect(percentage).not.toHaveAttribute('aria-invalid'); expect(screen.queryByRole('alert')).not.toBeInTheDocument(); expect(screen.getByRole('button', { name: 'Reset Context' })).toBeDisabled();
    change('Recent turns to retain', '0'); change('Recent token budget', '0');
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
