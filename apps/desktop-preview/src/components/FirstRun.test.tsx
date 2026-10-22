import { StrictMode } from 'react';
import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent, { type UserEvent } from '@testing-library/user-event';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { installDialogTestSupport } from '../screens/dialogTestSupport';
import { connectDelays } from '../screens/ProviderViews';
import { getProviderList, providerConnected, resetProviderState } from '../screens/providerState';
import { AppShell } from './AppShell';

beforeEach(installDialogTestSupport);
// Provider connections last for the whole visit: every case starts from, and leaves, the seeded ones.
beforeEach(resetProviderState);
afterEach(resetProviderState);
// Sign-ins hold still and a key check settles on the next tick, so no case sleeps through product delays.
const productDelays = { ...connectDelays };
beforeEach(() => { Object.assign(connectDelays, { browser: 2 ** 31 - 1, device: 2 ** 31 - 1, verify: 0 }); });
afterEach(() => { Object.assign(connectDelays, productDelays); });

const validKey = 'ak-test-5f2c9e81b7d04a63';
function start() { render(<StrictMode><AppShell /></StrictMode>); return userEvent.setup(); }
const anyConnected = () => getProviderList().some(providerConnected);
function projects() { return within(screen.getByRole('navigation', { name: 'All project chats' })); }
async function startFresh(user: UserEvent) {
  await user.click(screen.getByRole('button', { name: 'Start fresh' }));
  return screen.getByRole('region', { name: 'Welcome to AVA' });
}
function steps(canvas: HTMLElement) {
  return within(within(canvas).getByRole('region', { name: 'Getting started' })).getAllByRole('listitem');
}
async function closed(dialog: HTMLElement) {
  await waitFor(() => expect(dialog).not.toBeInTheDocument());
}

describe('first run', () => {
  it('starts fresh as a new user and restores the sample data, keeping focus on the one banner control', async () => {
    const user = start();
    expect(anyConnected()).toBe(true);
    const toggle = screen.getByRole('button', { name: 'Start fresh' });
    const canvas = await startFresh(user);
    expect(toggle).toHaveAccessibleName('Restore sample data');
    expect(toggle).toHaveFocus();
    expect(anyConnected()).toBe(false);

    // The Welcome stands in for the conversation, message box and inspector.
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Welcome to AVA');
    expect(canvas).toHaveTextContent('Ask AVA for a change to your code, then review every edit it makes.');
    expect(screen.queryByRole('form', { name: 'Message composer' })).not.toBeInTheDocument();
    expect(screen.queryByRole('textbox')).not.toBeInTheDocument();
    expect(screen.queryByRole('navigation', { name: 'Inspectors' })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Run preview' })).toBeDisabled();
    const items = steps(canvas);
    expect(items).toHaveLength(3);
    for (const item of items) expect(item).toHaveTextContent(/^To do/);
    expect(items[2]).toHaveTextContent('Ask AVA for a change');
    expect(within(items[2]).queryByRole('button')).not.toBeInTheDocument();
    expect(within(canvas).getByRole('status')).toHaveTextContent('0 of 3 done');

    // The sidebar says what goes there.
    expect(projects().getByText('Projects you open appear here.')).toBeVisible();
    expect(screen.queryByRole('navigation', { name: 'Recent chats' })).not.toBeInTheDocument();

    await user.click(toggle);
    expect(toggle).toHaveAccessibleName('Start fresh');
    expect(toggle).toHaveFocus();
    expect(anyConnected()).toBe(true);
    expect(screen.queryByRole('region', { name: 'Welcome to AVA' })).not.toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    expect(screen.getByRole('form', { name: 'Message composer' })).toBeInTheDocument();
    expect(projects().getByRole('button', { name: 'Remember search filters' })).toHaveAttribute('aria-current', 'page');
    expect(screen.getByRole('button', { name: 'Run preview' })).toBeEnabled();
  });

  it('connects a provider and opens a project from the Welcome, landing in that project’s logo-only empty chat', async () => {
    const user = start();
    const canvas = await startFresh(user);
    await user.click(within(canvas).getByRole('button', { name: 'Connect a provider' }));
    const directory = await screen.findByRole('dialog', { name: 'Connect a provider' });
    // A new user has nothing connected, environment credentials included.
    expect(within(directory).queryByRole('list', { name: 'Connected providers' })).not.toBeInTheDocument();
    expect(within(directory).queryByText('cedar')).not.toBeInTheDocument();
    await user.click(within(directory).getByRole('button', { name: 'Connect Aster Lab' }));
    const connect = screen.getByRole('dialog', { name: 'Connect Aster Lab' });
    await user.click(within(connect).getByRole('button', { name: /API key/ }));
    await user.type(within(connect).getByLabelText('API key'), validKey);
    await user.click(within(connect).getByRole('button', { name: 'Verify key' }));
    await user.click(await within(connect).findByRole('button', { name: 'Done' }, { timeout: 2500 }));
    // Done returns to the directory; Escape closes it (the path where focus once fell to the page).
    const back = await screen.findByRole('dialog', { name: 'Connect a provider' });
    expect(back).toContainElement(document.activeElement as HTMLElement);
    await user.keyboard('{Escape}');
    await closed(back);

    const items = steps(canvas);
    expect(items[0]).toHaveTextContent(/^Done/);
    expect(within(canvas).queryByRole('button', { name: 'Connect a provider' })).not.toBeInTheDocument();
    expect(within(canvas).getByRole('status')).toHaveTextContent('1 of 3 done');
    // The finished step's opener is gone: focus lands on the next step's action.
    const open = within(canvas).getByRole('button', { name: 'Open a project' });
    expect(open).toHaveFocus();

    await user.click(open);
    const picker = await screen.findByRole('dialog', { name: 'Open a project' });
    expect(within(picker).getAllByRole('button', { name: /Not opened yet$/ })).toHaveLength(3);
    await user.click(within(picker).getByRole('button', { name: /^Papertrail/ }));
    await closed(picker);

    expect(screen.queryByRole('region', { name: 'Welcome to AVA' })).not.toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');
    const transcript = screen.getByRole('region', { name: 'Conversation: New chat 1' });
    expect(within(transcript).getByRole('img', { name: 'AVA' })).toBeVisible();
    expect(transcript).toHaveTextContent(/^$/);
    expect(screen.getByRole('textbox')).toHaveFocus();
    expect(projects().getByRole('button', { name: 'Papertrail' })).toHaveAttribute('aria-expanded', 'true');
    expect(projects().getByRole('button', { name: 'New chat 1' })).toHaveAttribute('aria-current', 'page');
    expect(screen.queryByText('Projects you open appear here.')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Restore sample data' })).toBeVisible();
    // The default model's provider is the one just connected, so the chat keeps it.
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(/^Aster/);
    expect(screen.getByRole('button', { name: 'Model for this chat' })).not.toHaveTextContent('Not connected');

    // The first request completes the last step, as Help's Getting started shows.
    await user.keyboard('Add a retry button{Enter}');
    expect(screen.getAllByRole('article', { name: 'Your message' })).toHaveLength(1);
    await user.keyboard('{F1}');
    const help = await screen.findByRole('dialog', { name: 'Help' });
    expect(within(help).getByText('You’re set up.')).toBeInTheDocument();
  });

  it('starts a new project’s chat on a connected provider’s model when the default’s provider isn’t connected', async () => {
    const user = start();
    const canvas = await startFresh(user);
    await user.click(within(canvas).getByRole('button', { name: 'Connect a provider' }));
    const directory = await screen.findByRole('dialog', { name: 'Connect a provider' });
    await user.click(within(directory).getByRole('button', { name: 'Connect Orion Lab' }));
    // One sign-in method: the key form comes first.
    const connect = screen.getByRole('dialog', { name: 'Connect Orion Lab' });
    await user.type(within(connect).getByLabelText('API key'), validKey);
    await user.click(within(connect).getByRole('button', { name: 'Verify key' }));
    await user.click(await within(connect).findByRole('button', { name: 'Done' }, { timeout: 2500 }));
    const back = await screen.findByRole('dialog', { name: 'Connect a provider' });
    await user.keyboard('{Escape}');
    await closed(back);
    expect(within(canvas).getByRole('button', { name: 'Open a project' })).toHaveFocus();

    await user.keyboard('{Enter}');
    const picker = await screen.findByRole('dialog', { name: 'Open a project' });
    await user.click(within(picker).getByRole('button', { name: /^Relay/ }));
    await closed(picker);
    const model = screen.getByRole('button', { name: 'Model for this chat' });
    expect(model).toHaveTextContent(/^Orion/);
    expect(model).not.toHaveTextContent('Not connected');
    await user.keyboard('Add a retry button{Enter}');
    expect(screen.getAllByRole('article', { name: 'Your message' })).toHaveLength(1);
    // New chat follows the same rule.
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(/^Orion/);
  });

  it('returns focus to the next step even when nothing held focus as the dialog opened', async () => {
    const user = start();
    await startFresh(user);
    (document.activeElement as HTMLElement | null)?.blur();
    expect(document.activeElement).toBe(document.body);
    // Nothing focused: the dialog has no opener to return to.
    expect(fireEvent.keyDown(screen.getByRole('main', { name: 'AVA UI preview' }), { key: 'F1' })).toBe(false);
    const help = await screen.findByRole('dialog', { name: 'Help' });
    await user.keyboard('{Escape}');
    await closed(help);
    expect(within(screen.getByRole('region', { name: 'Welcome to AVA' })).getByRole('button', { name: 'Connect a provider' })).toHaveFocus();
  });

  it('asks which project to open when New chat has no project to start in', async () => {
    const user = start();
    await startFresh(user);
    const newChat = screen.getByRole('button', { name: 'New chat' });
    await user.click(newChat);
    let picker = await screen.findByRole('dialog', { name: 'Open a project' });
    await user.keyboard('{Escape}');
    await closed(picker);
    expect(newChat).toHaveFocus();
    expect(screen.getByRole('region', { name: 'Welcome to AVA' })).toBeInTheDocument();

    // The empty library opens the same picker.
    await user.click(projects().getByRole('button', { name: 'Open a project' }));
    picker = await screen.findByRole('dialog', { name: 'Open a project' });
    await user.click(within(picker).getByRole('button', { name: /^Relay/ }));
    await closed(picker);
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');

    // With a project open, New chat starts a chat in it straight away.
    await user.click(newChat);
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 2');
    expect(projects().getByRole('button', { name: 'New chat 2' })).toHaveAttribute('aria-current', 'page');
  });

  it('keeps chords, the finder, the palette and Settings safe while nothing is open', async () => {
    const user = start();
    await startFresh(user);
    const find = screen.getByRole('button', { name: 'Find a chat' });
    find.focus();
    // New chat's chord asks for a project too.
    await user.keyboard('{Control>}{Shift>}9{/Shift}{/Control}');
    const picker = await screen.findByRole('dialog', { name: 'Open a project' });
    await user.keyboard('{Escape}');
    await closed(picker);
    expect(find).toHaveFocus();

    await user.keyboard('{Control>}p{/Control}');
    const finder = await screen.findByRole('dialog', { name: 'Find a chat' });
    await user.keyboard('{Escape}');
    await closed(finder);
    await user.keyboard('{Control>}k{/Control}');
    const palette = await screen.findByRole('dialog', { name: 'Commands' });
    await user.keyboard('{Escape}');
    await closed(palette);

    // Chat-bound chords have no target: nothing opens and the Welcome stays.
    await user.keyboard('{Control>}{Shift>}2{/Shift}{/Control}');
    await waitFor(() => expect(screen.queryByRole('complementary', { name: 'Changes inspector' })).not.toBeInTheDocument());
    expect(screen.getByRole('region', { name: 'Welcome to AVA' })).toBeInTheDocument();

    await user.click(screen.getByRole('button', { name: 'Settings' }));
    const page = await screen.findByRole('region', { name: 'Settings page' });
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('region', { name: 'Welcome to AVA' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Run preview' })).toBeDisabled();
  });

  it('keeps the collapsed banner’s way back in the Welcome', async () => {
    const user = start();
    const canvas = await startFresh(user);
    await user.click(within(canvas).getByRole('button', { name: 'Hide preview banner' }));
    const chip = within(canvas).getByRole('button', { name: 'Preview' });
    expect(chip).toHaveFocus();
    await user.keyboard('{Enter}');
    expect(within(canvas).getByRole('button', { name: 'Restore sample data' })).toBeVisible();
  });
});

describe('Help entry points', () => {
  it('opens Help with F1 from the message box and with the footer button, returning focus to each', async () => {
    const user = start();
    const draft = screen.getByRole('textbox');
    draft.focus();
    await user.keyboard('{F1}');
    let help = await screen.findByRole('dialog', { name: 'Help' });
    await user.keyboard('{Escape}');
    await closed(help);
    expect(draft).toHaveFocus();

    const button = screen.getByRole('button', { name: 'Help' });
    await user.click(button);
    help = await screen.findByRole('dialog', { name: 'Help' });
    await user.keyboard('{Escape}');
    await closed(help);
    expect(button).toHaveFocus();
  });

  it('leaves F1 alone during IME composition and with modifiers', async () => {
    start();
    const draft = screen.getByRole('textbox');
    expect(fireEvent.keyDown(draft, { key: 'F1', isComposing: true })).toBe(true);
    expect(fireEvent.keyDown(draft, { key: 'F1', ctrlKey: true })).toBe(true);
    expect(fireEvent.keyDown(draft, { key: 'F1', shiftKey: true })).toBe(true);
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();

    // Claimed only when it opens Help.
    expect(fireEvent.keyDown(draft, { key: 'F1' })).toBe(false);
    expect(await screen.findByRole('dialog', { name: 'Help' })).toBeInTheDocument();
  });

  it('opens Help over Settings with F1 and closes back into the page, but never while a shortcut is being recorded', async () => {
    const user = start();
    await user.click(screen.getByRole('button', { name: 'Settings' }));
    const page = await screen.findByRole('region', { name: 'Settings page' });
    const search = within(page).getByRole('searchbox', { name: 'Search settings' });
    search.focus();
    expect(fireEvent.keyDown(search, { key: 'F1', altKey: true })).toBe(true);
    expect(fireEvent.keyDown(search, { key: 'F1', isComposing: true })).toBe(true);
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();

    await user.keyboard('{F1}');
    const help = await screen.findByRole('dialog', { name: 'Help' });
    expect(screen.getByRole('region', { name: 'Settings page' })).toBe(page);
    // F1 inside Help opens nothing more.
    await user.keyboard('{F1}');
    expect(screen.getAllByRole('dialog')).toHaveLength(1);
    await user.keyboard('{Escape}');
    await closed(help);
    expect(screen.getByRole('region', { name: 'Settings page' })).toBe(page);
    expect(search).toHaveFocus();

    // A shortcut being recorded takes F1 as its own key press.
    await user.click(within(page).getByRole('button', { name: 'Shortcuts' }));
    await user.click(within(page).getByRole('button', { name: /^New chat shortcut:/ }));
    const recorder = within(page).getByRole('button', { name: /^New chat shortcut: Press keys/ });
    fireEvent.keyDown(recorder, { key: 'F1' });
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('opens Help at a topic from a contextual link and returns focus to the link', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Export date range Attention' }));
    const link = screen.getByRole('button', { name: 'About permissions' });
    await user.click(link);
    const help = await screen.findByRole('dialog', { name: 'Help' });
    await waitFor(() => expect(help).toContainElement(document.activeElement as HTMLElement));
    expect(document.activeElement).toHaveTextContent(/permission/i);
    await user.keyboard('{Escape}');
    await closed(help);
    expect(link).toHaveFocus();
  });

  it('opens Help from the Welcome too, with F1, the footer button and the Welcome’s own Help link', async () => {
    const user = start();
    const canvas = await startFresh(user);
    await user.click(screen.getByRole('button', { name: 'Help' }));
    let help = await screen.findByRole('dialog', { name: 'Help' });
    expect(within(help).getByText('0 of 3 done')).toBeInTheDocument();
    await user.keyboard('{Escape}');
    await closed(help);
    expect(screen.getByRole('button', { name: 'Help' })).toHaveFocus();
    await user.keyboard('{F1}');
    help = await screen.findByRole('dialog', { name: 'Help' });
    await user.keyboard('{Escape}');
    await closed(help);

    // One quiet link under the steps, named with its key; Help opens on its search and closes back to it.
    const link = within(canvas).getByRole('button', { name: 'Help F1' });
    expect(link).toHaveAttribute('aria-keyshortcuts', 'F1');
    expect(link).toHaveAttribute('aria-haspopup', 'dialog');
    expect(within(canvas).getByRole('region', { name: 'Getting started' })).not.toContainElement(link);
    expect(within(canvas).getByRole('region', { name: 'Getting started' }).compareDocumentPosition(link) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
    await user.click(link);
    help = await screen.findByRole('dialog', { name: 'Help' });
    expect(within(help).getByRole('searchbox', { name: 'Search help' })).toHaveFocus();
    await user.keyboard('{Escape}');
    await closed(help);
    expect(link).toHaveFocus();
  });
});
