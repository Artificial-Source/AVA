import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it } from 'vitest';
import { AppShell } from '../components/AppShell';
import ScreenReview from './ScreenReview';
import { chooseMainModel, installDialogTestSupport } from './dialogTestSupport';

beforeEach(installDialogTestSupport);
async function settings(section = 'General') {
  fireEvent.click(screen.getByRole('button', { name: 'Settings' })); const nav = await screen.findByRole('navigation', { name: 'Settings sections' });
  fireEvent.click(within(nav).getByRole('button', { name: section }));
}
function back() { fireEvent.click(screen.getByRole('button', { name: 'Back to chat' })); }
function change(label: string, value: string) { fireEvent.change(within(screen.queryByRole('region', { name: 'Settings page' }) ?? document.body).getByLabelText(label), { target: { value } }); }
async function key(key: string, digit = false, target = screen.getByRole('region', { name: /^Conversation:/ })) {
  target.focus(); await act(async () => { fireEvent.keyDown(target, { key, code: digit ? `Digit${key}` : key === ',' ? 'Comma' : key === '.' ? 'Period' : '', ctrlKey: true, shiftKey: true }); });
}

describe('Settings controls reaching actual normal app owners', () => {
  it('applies General and model defaults only to future chats, then names only their first local message', async () => {
    const user = userEvent.setup(); render(<AppShell />); const originalTitle = screen.getByRole('heading', { level: 1 }).textContent; const originalDraft = (screen.getByRole('textbox') as HTMLTextAreaElement).value;
    await settings(); change('New-chat mode', 'plan'); const project = screen.getByLabelText('New-chat project').querySelectorAll('option')[3]; change('New-chat project', project.value); fireEvent.click(screen.getByLabelText('Name new chats from first message')); change('New-chat inspector', 'context');
    fireEvent.click(screen.getByRole('button', { name: 'Models' })); fireEvent.click(screen.getByRole('button', { name: 'Choose default model' })); fireEvent.click(screen.getByRole('radio', { name: /Orion.*96,000/ })); fireEvent.click(screen.getByRole('button', { name: 'Use Orion' })); back();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent(originalTitle!); expect(screen.getByRole('textbox')).toHaveValue(originalDraft); expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('build');
    fireEvent.click(screen.getByRole('button', { name: 'New chat' })); expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan'); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    expect(within(screen.getByRole('region', { name: `${project.textContent} project` })).getByRole('button', { name: 'New chat 1' })).toHaveAttribute('aria-current', 'page');
    expect(await screen.findByRole('region', { name: 'Context information' })).toBeVisible();
    // Turn naming off after creation: this chat retains its explicit snapshot.
    await settings(); fireEvent.click(screen.getByLabelText('Name new chats from first message')); back();
    fireEvent.change(screen.getByRole('textbox'), { target: { value: '  A local\n\t title 🪐  ' } }); await user.click(screen.getByRole('button', { name: 'Send' })); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('A local title 🪐');
    fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Second message' } }); fireEvent.click(screen.getByRole('button', { name: 'Send' })); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('A local title 🪐');
    expect(screen.getAllByRole('article', { name: 'Your local preview message' })).toHaveLength(2); expect(screen.queryByRole('article', { name: 'AVA example response' })).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'New chat' })); fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Still numbered' } }); fireEvent.click(screen.getByRole('button', { name: 'Send' })); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 2');
  });
  it('runs actual default and recorded app shortcuts, retaining native editor keys and disabled-shortcut Escape', async () => {
    render(<AppShell />); await key(','); await screen.findByRole('navigation', { name: 'Settings sections' });
    change('New-chat mode', 'plan'); fireEvent.click(screen.getByRole('button', { name: 'Shortcuts' })); fireEvent.click(screen.getByRole('button', { name: 'Record shortcut: New chat' }));
    fireEvent.keyDown(screen.getByRole('group', { name: 'Recording New chat' }), { key: '>', code: 'Period', ctrlKey: true, shiftKey: true }); back();
    const draft = screen.getByRole('textbox'); await key('.', false, draft); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    await key('9', true); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    await key('.'); await waitFor(() => expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1')); expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan');
    await key('7', true); expect(screen.getByRole('textbox')).toHaveFocus();
    await key('6', true); const model = await screen.findByRole('dialog', { name: 'Choose a model' }); fireEvent.keyDown(model, { key: '.', ctrlKey: true, shiftKey: true }); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1'); fireEvent.keyDown(model, { key: 'Escape' });
    await settings('Shortcuts'); fireEvent.click(screen.getByLabelText('Enable app shortcuts')); fireEvent.keyDown(screen.getByLabelText('Search shortcuts'), { key: 'Escape' }); expect(screen.getByRole('heading', { level: 2, name: 'General' })).toBeVisible(); back();
    await key(','); expect(screen.queryByRole('region', { name: 'Settings page' })).not.toBeInTheDocument();
    await settings('Shortcuts'); fireEvent.click(screen.getByRole('button', { name: 'Reset all shortcuts' })); back(); await key('9', true); await waitFor(() => expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 2'));
  });
  it('applies tool/output and wrap preferences to the mounted transcript, preserves failures/diffs, and reaches independent Changes and child surfaces', async () => {
    render(<AppShell />); const transcript = screen.getByRole('region', { name: /^Conversation:/ });
    await settings('Conversation behavior'); change('Chat display', 'expanded'); change('Completed-tool output', 'hide'); fireEvent.click(screen.getByLabelText('Wrap code lines')); fireEvent.click(screen.getByLabelText('Wrap diff lines')); back();
    expect(screen.getByRole('region', { name: /^Conversation:/ })).toBe(transcript); expect(screen.getByRole('button', { name: /^Tool details/ })).toHaveAttribute('aria-expanded', 'true');
    expect(await screen.findAllByLabelText(/^Supplied diff:/)).toHaveLength(2); expect(screen.queryAllByLabelText(/^Output:/)).toHaveLength(0); expect(transcript).toHaveTextContent('Failed'); expect(transcript).toHaveTextContent('No failure cause supplied.');
    expect(screen.getByRole('main')).toHaveAttribute('data-code-wrap', 'true'); expect(screen.getByRole('main')).toHaveAttribute('data-diff-wrap', 'false');
    await key('8', true); expect(screen.getByRole('button', { name: /^Tool details/ })).toHaveAttribute('aria-expanded', 'false'); await key('8', true); expect(screen.getByRole('button', { name: /^Tool details/ })).toHaveAttribute('aria-expanded', 'true');
    await key('2', true); expect(await screen.findByRole('heading', { name: /^Changes/ })).toBeVisible(); expect(screen.getAllByRole('heading', { name: 'Before' }).length).toBeGreaterThan(0);
    fireEvent.click(screen.getByRole('button', { name: 'Agents' })); fireEvent.click(screen.getByRole('button', { name: /^Completed Map return paths/ })); expect(await screen.findByRole('region', { name: /Child conversation:/ })).toBeVisible(); expect(screen.getByRole('main')).toHaveAttribute('data-code-wrap', 'true');
    const title = screen.getByRole('heading', { level: 1 }).textContent; await key('6', true, screen.getByRole('region', { name: /Child conversation:/ })); expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent(title!);
  });
  it('keeps Stop unassigned until explicitly bound, then stops only the viewed Working mock', async () => {
    render(<AppShell />); fireEvent.click(within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Working Keyboard navigation' }));
    const draft = (screen.getByRole('textbox') as HTMLTextAreaElement).value;
    await key('.'); expect(screen.getByRole('button', { name: 'Stop mock work' })).toBeVisible();
    await settings('Shortcuts'); fireEvent.click(screen.getByRole('button', { name: 'Record shortcut: Stop viewed mock' }));
    fireEvent.keyDown(screen.getByRole('group', { name: 'Recording Stop viewed mock' }), { key: '>', code: 'Period', ctrlKey: true, shiftKey: true });
    const hiddenTranscript = document.querySelector<HTMLElement>('[aria-label="Conversation: Keyboard navigation"]')!;
    fireEvent.keyDown(hiddenTranscript, { key: '.', code: 'Period', ctrlKey: true, shiftKey: true }); back(); expect(screen.getByRole('button', { name: 'Stop mock work' })).toBeVisible();
    await key('.'); await waitFor(() => expect(screen.queryByRole('button', { name: 'Stop mock work' })).not.toBeInTheDocument());
    expect(within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Stopped Keyboard navigation' })).toHaveAttribute('aria-current', 'page');
    expect(screen.getByRole('textbox')).toHaveValue(draft);
  });
  it('keeps context policy separate from actual matching estimates, model selection and unknown usage', async () => {
    const user = userEvent.setup(); render(<AppShell />); const meter = screen.getByRole('button', { name: /^Open Context/ }); const estimate = meter.textContent;
    await settings('Context management'); expect(screen.getByRole('region', { name: 'Context policy projection' })).toHaveTextContent('171,200'); change('Compaction trigger percentage', '95'); change('Recent token budget', '0'); back(); expect(meter).toHaveTextContent(estimate!);
    await chooseMainModel(user, 'orion'); await settings('Context management'); expect(screen.getByRole('region', { name: 'Context policy projection' })).toHaveTextContent('Unknown eligibility'); expect(screen.getByLabelText('Recent token budget')).toHaveValue(0); back(); expect(meter).toHaveTextContent('Context unknown');
  });
  it('owns separate normal-app and ScreenReview behavior preferences and resets', async () => {
    const app = render(<AppShell />); await settings(); change('New-chat mode', 'plan'); back(); change('Chat display', 'expanded'); const review = render(<ScreenReview />); const reviewRoot = screen.getByRole('main', { name: 'Screen review workspace' });
    expect(within(reviewRoot).getByLabelText('New-chat mode')).toHaveValue('build'); fireEvent.change(within(reviewRoot).getByLabelText('New-chat mode'), { target: { value: 'plan' } });
    fireEvent.click(within(reviewRoot).getByRole('button', { name: 'Reset General' })); expect(within(reviewRoot).getByLabelText('New-chat mode')).toHaveValue('build');
    fireEvent.click(within(reviewRoot).getByRole('button', { name: 'Conversation behavior' })); const reviewDisplay = within(reviewRoot).getByRole('combobox', { name: 'Chat display' }); expect(reviewDisplay).toHaveValue('compact');
    fireEvent.change(reviewDisplay, { target: { value: 'auto' } }); fireEvent.click(within(reviewRoot).getByRole('button', { name: 'Reset Conversation' })); expect(reviewDisplay).toHaveValue('compact'); review.unmount();
    expect(screen.getByRole('combobox', { name: 'Chat display' })).toHaveValue('expanded');
    fireEvent.click(screen.getByRole('button', { name: 'New chat' })); expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan'); app.unmount();
  });
});
