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
function scope() { return within(screen.queryByRole('region', { name: 'Settings page' }) ?? document.body); }
function change(label: string, value: string) { fireEvent.change(scope().getByLabelText(label), { target: { value } }); }
function record(action: string, init: KeyboardEventInit) {
  const keycap = scope().getByRole('button', { name: new RegExp(`^${action} shortcut:`) });
  fireEvent.click(keycap); fireEvent.keyDown(keycap, init);
}
async function key(key: string, digit = false, target = screen.getByRole('region', { name: /^Conversation:/ })) {
  target.focus(); await act(async () => { fireEvent.keyDown(target, { key, code: digit ? `Digit${key}` : key === ',' ? 'Comma' : key === '.' ? 'Period' : '', ctrlKey: true, shiftKey: true }); });
}

describe('Settings controls reaching actual normal app owners', () => {
  it('applies General and model defaults only to future chats, then names only their first local message', async () => {
    const user = userEvent.setup(); render(<AppShell />); const originalTitle = screen.getByRole('heading', { level: 1 }).textContent; const originalDraft = (screen.getByRole('textbox') as HTMLTextAreaElement).value;
    await settings(); change('New-chat mode', 'plan'); const project = scope().getByLabelText('New-chat project').querySelectorAll('option')[3]; change('New-chat project', project.value); fireEvent.click(scope().getByLabelText('Name new chats from first message')); change('New-chat inspector', 'context');
    fireEvent.click(screen.getByRole('button', { name: 'Models & providers' })); fireEvent.click(screen.getByRole('button', { name: 'Choose default model' })); fireEvent.click(screen.getByRole('radio', { name: /Orion.*96,000/ })); fireEvent.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Set as default' })); back();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent(originalTitle!); expect(screen.getByRole('textbox')).toHaveValue(originalDraft); expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('build');
    fireEvent.click(screen.getByRole('button', { name: 'New chat' })); expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan'); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    expect(within(screen.getByRole('region', { name: `${project.textContent} project` })).getByRole('button', { name: 'New chat 1' })).toHaveAttribute('aria-current', 'page');
    expect(await screen.findByRole('region', { name: 'Context information' })).toBeVisible();
    // Turn naming off after creation: this chat retains its explicit snapshot.
    await settings(); fireEvent.click(scope().getByLabelText('Name new chats from first message')); back();
    fireEvent.change(screen.getByRole('textbox'), { target: { value: '  A local\n\t title 🪐  ' } }); await user.click(screen.getByRole('button', { name: 'Send' })); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('A local title 🪐');
    fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Second message' } }); fireEvent.click(screen.getByRole('button', { name: 'Send' })); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('A local title 🪐');
    expect(screen.getAllByRole('article', { name: 'Your message' })).toHaveLength(2); expect(screen.queryByRole('article', { name: 'AVA response' })).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'New chat' })); fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Still numbered' } }); fireEvent.click(screen.getByRole('button', { name: 'Send' })); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 2');
  });
  it('runs default and recorded app shortcuts, including from the composer, and closes Settings with Escape', async () => {
    render(<AppShell />); await key(','); await screen.findByRole('navigation', { name: 'Settings sections' });
    change('New-chat mode', 'plan'); fireEvent.click(screen.getByRole('button', { name: 'Shortcuts' }));
    // Every safe chord has a default owner: free Stop's Ctrl+Shift+. before giving it to New chat.
    fireEvent.click(scope().getByRole('button', { name: 'Unassign Stop the run' })); record('New chat', { key: '>', code: 'Period', ctrlKey: true, shiftKey: true }); back();
    await key('9', true); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    await key('.', false, screen.getByRole('textbox')); await waitFor(() => expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1')); expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan');
    await key('7', true); expect(screen.getByRole('textbox')).toHaveFocus();
    await key('6', true); const model = await screen.findByRole('dialog'); fireEvent.keyDown(model, { key: '.', ctrlKey: true, shiftKey: true }); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1'); fireEvent.keyDown(model, { key: 'Escape' });
    await settings('Shortcuts'); const toggle = scope().getByLabelText('Enable app shortcuts'); fireEvent.click(toggle);
    fireEvent.keyDown(toggle, { key: 'Escape' }); expect(screen.queryByRole('region', { name: 'Settings page' })).not.toBeInTheDocument();
    await key(','); expect(screen.queryByRole('region', { name: 'Settings page' })).not.toBeInTheDocument();
    await settings('Shortcuts'); fireEvent.click(screen.getByRole('button', { name: 'Reset all shortcuts' })); back(); await key('9', true); await waitFor(() => expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 2'));
  });
  it('applies tool/output and wrap preferences to the mounted transcript, preserves failures/diffs, and reaches independent Changes and child surfaces', async () => {
    render(<AppShell />); const transcript = screen.getByRole('region', { name: /^Conversation:/ });
    await settings('Conversation behavior'); change('Tool details', 'expanded'); fireEvent.click(scope().getByLabelText('Show completed-tool output')); fireEvent.click(scope().getByLabelText('Wrap code lines')); fireEvent.click(scope().getByLabelText('Wrap diff lines')); back();
    const group = () => screen.getAllByRole('button', { name: /^\d+ tools?\b/ })[0];
    expect(screen.getByRole('region', { name: /^Conversation:/ })).toBe(transcript); expect(group()).toHaveAttribute('aria-expanded', 'true');
    expect((await screen.findAllByLabelText(/^Diff:/)).length).toBeGreaterThan(0); expect(screen.queryAllByLabelText(/^Output:/)).toHaveLength(0); expect(transcript).toHaveTextContent('Failed');
    expect(screen.getByRole('main')).toHaveAttribute('data-code-wrap', 'true'); expect(screen.getByRole('main')).toHaveAttribute('data-diff-wrap', 'false');
    await key('8', true); expect(group()).toHaveAttribute('aria-expanded', 'false'); await key('8', true); expect(group()).toHaveAttribute('aria-expanded', 'true');
    await key('2', true); expect(await screen.findByRole('heading', { name: /^Changes/ })).toBeVisible();
    fireEvent.click(screen.getByRole('button', { name: 'Agents' })); fireEvent.click(screen.getByRole('button', { name: /^Completed Map return paths/ })); expect(await screen.findByRole('region', { name: /Child conversation:/ })).toBeVisible(); expect(screen.getByRole('main')).toHaveAttribute('data-code-wrap', 'true');
    const title = screen.getByRole('heading', { level: 1 }).textContent; await key('6', true, screen.getByRole('region', { name: /Child conversation:/ })); expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent(title!);
  });
  it('stops only the viewed Working chat with the default Stop chord, never from hidden Settings, and honors unassignment', async () => {
    render(<AppShell />); fireEvent.click(within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Keyboard navigation Working' }));
    const draft = (screen.getByRole('textbox') as HTMLTextAreaElement).value;
    await settings('Shortcuts'); fireEvent.click(scope().getByRole('button', { name: 'Unassign Stop the run' })); back();
    await key('.'); expect(screen.getByRole('button', { name: 'Stop' })).toBeVisible();
    await settings('Shortcuts'); fireEvent.click(scope().getByRole('button', { name: 'Reset Stop the run shortcut' }));
    const hiddenTranscript = document.querySelector<HTMLElement>('[aria-label="Conversation: Keyboard navigation"]')!;
    fireEvent.keyDown(hiddenTranscript, { key: '.', code: 'Period', ctrlKey: true, shiftKey: true }); back(); expect(screen.getByRole('button', { name: 'Stop' })).toBeVisible();
    await key('.'); await waitFor(() => expect(screen.queryByRole('button', { name: 'Stop' })).not.toBeInTheDocument());
    expect(within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Keyboard navigation Stopped' })).toHaveAttribute('aria-current', 'page');
    expect(screen.getByRole('textbox')).toHaveValue(draft);
  });
  it('keeps context policy separate from actual matching estimates, model selection and unknown usage', async () => {
    const user = userEvent.setup(); render(<AppShell />); const meter = screen.getByRole('button', { name: /^Open Context/ }); const estimate = meter.textContent;
    await settings('Context management'); expect(screen.getByRole('region', { name: 'This chat' })).toHaveTextContent('171,200'); change('Compaction trigger percentage', '95'); change('Recent token budget', '0'); back(); expect(meter).toHaveTextContent(estimate!);
    await chooseMainModel(user, 'orion'); await settings('Context management'); const projection = screen.getByRole('region', { name: 'This chat' });
    // A known window and unknown usage: the page names only the missing fact, never the known window.
    expect(projection).toHaveTextContent('Context window96,000 tokens'); expect(projection).toHaveTextContent('In use nowNot measured yet'); expect(projection).not.toHaveTextContent('Usage');
    expect(projection).toHaveTextContent('This chat’s usage with Orion hasn’t been measured yet, so AVA can’t tell how close it is to compacting.'); expect(projection).not.toHaveTextContent(/needs .*context window/);
    expect(scope().getByLabelText('Recent token budget')).toHaveValue('0'); back(); expect(meter).toHaveTextContent('Context unknown');
  });
  it('owns separate normal-app and ScreenReview behavior preferences and resets', async () => {
    const app = render(<AppShell />); await settings(); change('New-chat mode', 'plan'); back(); change('Tool details', 'expanded'); const review = render(<ScreenReview />); const reviewRoot = screen.getByRole('main', { name: 'Screen review workspace' });
    expect(within(reviewRoot).getByLabelText('New-chat mode')).toHaveValue('build'); fireEvent.change(within(reviewRoot).getByLabelText('New-chat mode'), { target: { value: 'plan' } });
    fireEvent.click(within(reviewRoot).getByRole('button', { name: 'Reset general' })); expect(within(reviewRoot).getByLabelText('New-chat mode')).toHaveValue('build');
    fireEvent.click(within(reviewRoot).getByRole('button', { name: 'Conversation behavior' })); const reviewDisplay = within(reviewRoot).getByRole('combobox', { name: 'Tool details' }); expect(reviewDisplay).toHaveValue('compact');
    fireEvent.change(reviewDisplay, { target: { value: 'auto' } }); fireEvent.click(within(reviewRoot).getByRole('button', { name: 'Reset conversation' })); expect(reviewDisplay).toHaveValue('compact'); review.unmount();
    expect(screen.getByRole('combobox', { name: 'Tool details' })).toHaveValue('expanded');
    fireEvent.click(screen.getByRole('button', { name: 'New chat' })); expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan'); app.unmount();
  });
});
