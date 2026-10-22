import { StrictMode } from 'react';
import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent, { type UserEvent } from '@testing-library/user-event';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { AppShell } from '../components/AppShell';
import type { MainScreenRequest } from './contracts';
import { chooseMainModel, installDialogTestSupport } from './dialogTestSupport';
import MainScreens, { type MainScreensProps } from './MainScreens';
import { connectDelays } from './ProviderViews';
import { clearProviderConnections, resetProviderState, setProviderState } from './providerState';
import { defaultPreferences } from './screenExamples';

beforeEach(installDialogTestSupport);
// Provider connections last for the whole visit, so each case starts from the visit's start.
beforeEach(resetProviderState);
// Connect steps settle on product timers. Here sign-ins hold still (no case waits on them)
// and a key check settles on the next tick, so no case sleeps through product delays.
const productDelays = { ...connectDelays };
beforeEach(() => { Object.assign(connectDelays, { browser: 2 ** 31 - 1, device: 2 ** 31 - 1, verify: 0 }); });
afterEach(() => { Object.assign(connectDelays, productDelays); });
const validKey = 'ak-test-5f2c9e81b7d04a63';
function start() { render(<StrictMode><AppShell /></StrictMode>); return userEvent.setup(); }
function projects() { return within(screen.getByRole('navigation', { name: 'All project chats' })); }
function expectHiddenSidebar(sidebar: HTMLElement) {
  expect(sidebar).toBeInTheDocument(); expect(sidebar).not.toBeVisible();
  expect(sidebar).toHaveAttribute('hidden'); expect(sidebar).toHaveAttribute('inert');
  expect(screen.queryByRole('complementary', { name: 'Projects and chats' })).not.toBeInTheDocument();
  expect(screen.queryByRole('region', { name: 'Chat library' })).not.toBeInTheDocument();
  for (const name of ['All project chats', 'Pinned chats', 'Recent chats']) expect(screen.queryByRole('navigation', { name })).not.toBeInTheDocument();
  for (const name of ['New chat', 'Settings']) expect(screen.queryByRole('button', { name })).not.toBeInTheDocument();
}
async function settings(user: UserEvent) {
  await user.click(screen.getByRole('button', { name: 'Settings' }));
  await screen.findByRole('navigation', { name: 'Settings sections' });
  return screen.getByRole('region', { name: 'Settings page' });
}
async function useModel(user: UserEvent, label: 'Aster' | 'Aster Mini' | 'Orion') {
  const dialog = await screen.findByRole('dialog', { name: /^(Model for “.*”|Default model for new chats)$/ });
  const id = { Aster: 'aster', 'Aster Mini': 'aster-mini', Orion: 'orion' }[label];
  await user.click(within(dialog).getAllByRole<HTMLInputElement>('radio').find((input) => input.value === id)!);
  await user.click(within(dialog).getByRole('button', { name: /^(Use for this chat|Set as default)$/ }));
}
const sentMessages = () => screen.queryAllByRole('article', { name: 'Your message' }).length;
async function command(user: UserEvent, chord: 'k' | 'p') {
  const draft = screen.getByRole('textbox'); draft.focus();
  await user.keyboard(`{Control>}${chord}{/Control}`);
  return screen.findByRole('dialog', { name: chord === 'k' ? 'Commands' : 'Find a chat' });
}

describe('main-chat lazy screen integration', () => {
  it.each(['Remember search filters', 'Keyboard navigation Working'])('blocks hidden chat edits, Send and Stop behind Settings for %s', async (name) => {
    const user = start();
    if (name !== 'Remember search filters') await user.click(projects().getByRole('button', { name }));
    const draft = screen.getByRole('textbox') as HTMLTextAreaElement;
    const value = draft.value; const form = screen.getByRole('form', { name: 'Message composer' }); const sent = sentMessages();
    const action = screen.getByRole('button', { name: name.endsWith('Working') ? 'Stop' : 'Send' });
    const page = await settings(user);
    expect(form.closest('[hidden][inert]')).not.toBeNull();
    fireEvent.change(draft, { target: { value: 'Must not replace the hidden draft' } });
    fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true }); fireEvent.submit(form); fireEvent.click(action);
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('textbox')).toBe(draft); expect(draft).toHaveValue(value);
    expect(sentMessages()).toBe(sent);
    expect(projects().getByRole('button', { name })).toHaveAttribute('aria-current', 'page');
  });

  it('makes Enter preferences operational only in the local composer, retaining Shift+Enter and IME behavior', async () => {
    const user = start(); const sent = sentMessages(); const replies = screen.queryAllByRole('article', { name: 'AVA response' }).length;
    const page = await settings(user);
    await user.selectOptions(within(page).getByLabelText('Enter key behavior'), 'newline');
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    const draft = screen.getByRole('textbox') as HTMLTextAreaElement; draft.focus(); draft.setSelectionRange(draft.value.length, draft.value.length);
    await user.keyboard('{Enter}'); expect(sentMessages()).toBe(sent);
    expect(draft).toHaveValue('Add a regression test for returning from a result.\n');
    const reopened = await settings(user);
    await user.selectOptions(within(reopened).getByLabelText('Enter key behavior'), 'send');
    await user.click(within(reopened).getByRole('button', { name: 'Back to chat' }));
    draft.focus(); await user.keyboard('{Shift>}{Enter}{/Shift}');
    fireEvent.compositionStart(draft); fireEvent.keyDown(draft, { key: 'Enter' }); fireEvent.compositionEnd(draft);
    fireEvent.keyDown(draft, { key: 'Enter', keyCode: 229 });
    expect(sentMessages()).toBe(sent);
    await user.keyboard('{Enter}'); expect(sentMessages()).toBe(sent + 1);
    expect(screen.queryAllByRole('article', { name: 'AVA response' })).toHaveLength(replies);
  });
  it('returns only to a visible Settings opener or the visible conversation title fallback', async () => {
    const user = start(); const opener = screen.getByRole('button', { name: 'Settings' });
    const title = screen.getByRole('heading', { level: 1 }); const page = await settings(user);
    opener.hidden = true;
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    expect(title).toHaveFocus(); expect(title.closest('[hidden], [inert]')).toBeNull();
  });

  it('replaces the sidebar and workspace with an eleven-section Settings page, one heading and nav-local search', async () => {
    const user = start();
    const app = screen.getByRole('main', { name: 'AVA UI preview' });
    const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const opener = screen.getByRole('button', { name: 'Settings' });
    expect(sidebar).toContainElement(opener);
    expect(screen.getAllByRole('button', { name: 'Settings' })).toHaveLength(1);
    const title = screen.getByRole('heading', { level: 1 });
    const dialog = await settings(user);
    expectHiddenSidebar(sidebar); expect(sidebar.parentElement).toBe(app);
    expect(app).toHaveAttribute('data-page', 'settings');
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(dialog).toHaveFocus(); expect(opener).toHaveAttribute('aria-current', 'page'); expect(opener).not.toHaveAttribute('aria-haspopup');
    expect(screen.getAllByRole('heading', { level: 1 })).toHaveLength(1);
    expect(within(dialog).getByRole('heading', { level: 1 })).toHaveTextContent('Settings');
    expect(title).not.toBeVisible(); expect(screen.queryByRole('form', { name: 'Message composer' })).not.toBeInTheDocument();
    const nav = within(dialog).getByRole('navigation', { name: 'Settings sections' });
    expect(within(dialog).getByRole('searchbox', { name: 'Search settings' })).toBeVisible();
    const sections = within(nav).getAllByRole('button');
    expect(within(nav).queryByRole('button', { name: /Reset/ })).not.toBeInTheDocument();
    expect(sections.map((button) => button.textContent)).toEqual(['General', 'Appearance', 'Shortcuts', 'Notifications & sounds', 'Models & providers', 'Conversation behavior', 'Context management', 'Permissions & trust', 'Subagents', 'Extensions', 'About']);
    for (const button of sections) {
      await user.click(button);
      expect(button).toHaveAttribute('aria-current', 'page');
      expect(within(dialog).getByRole('heading', { name: button.textContent!, level: 2 })).toBeVisible();
    }
    await user.click(within(dialog).getByRole('button', { name: 'Permissions & trust' }));
    expect(dialog).toHaveTextContent('To trust it, send /trust project in any Wayfinder chat.');
    expect(dialog).not.toHaveTextContent(/example parent|durable policy scope|Atlas/);
    await user.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    expect(opener).toHaveFocus(); expect(opener).toBeVisible(); expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(screen.getByRole('complementary', { name: 'Projects and chats' })).toBe(sidebar);
    expect(sidebar).toBeVisible(); expect(sidebar).not.toHaveAttribute('hidden'); expect(sidebar).not.toHaveAttribute('inert');
    expect(sidebar.parentElement).toBe(app); expect(app).toHaveAttribute('data-page', 'chat');
  });

  it('rejects retained sidebar controls in Settings and restores the same library state on Back', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Pin Keyboard navigation' }));
    await user.click(projects().getByRole('button', { name: /^Relay\b/ }));
    const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const opener = screen.getByRole('button', { name: 'Settings' });
    const title = screen.getByRole('heading', { level: 1 }); const draft = screen.getByRole('textbox');
    const controls = [
      projects().getByRole('button', { name: 'Export date range Attention' }),
      screen.getByRole('button', { name: 'New chat' }), opener,
      projects().getByRole('button', { name: 'Pin Remember search filters' }),
      projects().getByRole('button', { name: 'Unpin Keyboard navigation' }),
      projects().getByRole('button', { name: 'Wayfinder' }), projects().getByRole('button', { name: /^Relay\b/ }),
      screen.getByRole('button', { name: 'Remove Remember search filters from recent chats' }),
    ];
    const page = await settings(user); expectHiddenSidebar(sidebar);
    for (const control of controls) fireEvent.click(control);
    expect(screen.getByRole('region', { name: 'Settings page' })).toBe(page); expectHiddenSidebar(sidebar);
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    expect(opener).toHaveFocus(); expect(opener.closest('[hidden], [inert]')).toBeNull();
    expect(screen.getByRole('complementary', { name: 'Projects and chats' })).toBe(sidebar); expect(sidebar).toBeVisible();
    expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(screen.getByRole('textbox')).toBe(draft); expect(draft).toHaveValue('Add a regression test for returning from a result.');
    expect(projects().getByRole('button', { name: 'Remember search filters' })).toHaveAttribute('aria-current', 'page');
    expect(projects().getByRole('button', { name: 'Wayfinder' })).toHaveAttribute('aria-expanded', 'true');
    expect(projects().getByRole('button', { name: /^Relay\b/ })).toHaveAttribute('aria-expanded', 'false');
    expect(within(screen.getByRole('navigation', { name: 'Pinned chats' })).getAllByRole('listitem')).toHaveLength(1);
    expect(projects().getByRole('button', { name: 'Unpin Keyboard navigation' })).toHaveAttribute('aria-pressed', 'true');
    expect(screen.getByRole('button', { name: 'Remove Remember search filters from recent chats' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');
  });

  it('keeps Settings under one native overlay; Escape clears a query, closes the overlay, then closes Settings from any section', async () => {
    const user = start(); const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const page = await settings(user);
    await user.click(within(page).getByRole('button', { name: 'Models & providers' }));
    const opener = within(page).getByRole('button', { name: 'Choose default model' });
    await user.click(opener);
    const modelDialog = screen.getByRole('dialog', { name: 'Default model for new chats' });
    expect(screen.getAllByRole('dialog')).toHaveLength(1);
    expect(screen.getByRole('region', { name: 'Settings page' })).toBe(page); expectHiddenSidebar(sidebar);
    const search = within(modelDialog).getByRole('searchbox', { name: 'Search models' });
    await user.type(search, 'Orion');
    fireEvent.keyDown(search, { key: 'Escape', isComposing: true }); expect(search).toHaveValue('Orion');
    await user.keyboard('{Escape}'); expect(search).toHaveValue(''); expect(search).toHaveFocus();
    await user.keyboard('{Escape}'); expect(modelDialog).not.toBeInTheDocument(); expect(opener).toHaveFocus();
    expectHiddenSidebar(sidebar);
    expect(within(page).getByRole('button', { name: 'Models & providers' })).toHaveAttribute('aria-current', 'page');
    const settingsSearch = within(page).getByRole('searchbox', { name: 'Search settings' }); await user.type(settingsSearch, 'no matching section');
    fireEvent.keyDown(settingsSearch, { key: 'Escape', keyCode: 229 }); expect(settingsSearch).toHaveValue('no matching section');
    await user.keyboard('{Escape}'); expect(settingsSearch).toHaveValue('');
    // No detour through General: the next Escape leaves Settings.
    await user.keyboard('{Escape}'); expect(page).not.toBeInTheDocument(); expect(screen.getByRole('button', { name: 'Settings' })).toHaveFocus();
    expect(screen.getByRole('complementary', { name: 'Projects and chats' })).toBe(sidebar); expect(sidebar).toBeVisible();
  });

  it('applies only the selected parent profile, preserving drafts, messages, pins, mode, references, expansion, Context and title focus', async () => {
    const user = start();
    const title = screen.getByRole('heading', { level: 1 }); const focus = vi.spyOn(title, 'focus');
    const transcript = screen.getByRole('region', { name: 'Conversation: Remember search filters' }); const text = transcript.textContent;
    const draft = screen.getByRole('textbox'); fireEvent.change(draft, { target: { value: 'Preserved parent draft' } });
    await user.selectOptions(screen.getByRole('combobox', { name: 'Mode for this chat' }), 'plan');
    await user.click(screen.getByRole('button', { name: 'Expand draft' }));
    await user.click(screen.getByRole('button', { name: 'Add context' }));
    await user.click(screen.getByRole('button', { name: 'image result-layout.png' })); await user.keyboard('{Escape}');
    await user.click(projects().getByRole('button', { name: 'Pin Remember search filters' }));
    const sidebarText = screen.getByRole('complementary', { name: 'Projects and chats' }).textContent;
    await user.click(screen.getByRole('button', { name: /^Open Context/ }));
    await user.click(await screen.findByRole('button', { name: 'Raw messages' })); const raw = screen.getByRole('region', { name: 'Raw message JSON' });
    const page = await settings(user);
    const query = within(page).getByRole('searchbox', { name: 'Search settings' });
    await user.type(query, 'does not exist');
    expect(page).toHaveTextContent('No settings match “does not exist”.');
    fireEvent(window, new Event('resize')); expect(query).toHaveFocus();
    await user.keyboard('{Escape}'); expect(query).toHaveValue('');
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('region', { name: 'Raw message JSON' })).toBe(raw);
    await chooseMainModel(user, 'orion');
    expect(screen.getByLabelText('Model: Aster')).toBeVisible();
    expect(screen.getByLabelText('Thinking: High')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveFocus();
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    expect(screen.getByRole('textbox')).toBe(draft); expect(draft).toHaveValue('Preserved parent draft');
    expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan');
    expect(screen.getByRole('button', { name: 'Collapse draft' })).toBeVisible();
    expect(screen.getByRole('list', { name: 'Attached context' })).toHaveTextContent('result-layout.png');
    expect(screen.getByRole('region', { name: 'Raw message JSON' })).toBe(raw);
    expect(screen.getByRole('region', { name: 'Conversation: Remember search filters' })).toBe(transcript); expect(transcript.textContent).toBe(text);
    expect(screen.getByRole('complementary', { name: 'Projects and chats' }).textContent).toBe(sidebarText);
    expect(screen.getByRole('button', { name: /^Open Context/ })).toHaveTextContent('Context unknown');
    expect(focus).not.toHaveBeenCalled();
  });

  it('uses one model browser for this chat and the new-chat default, returning focus to each opener', async () => {
    const user = start(); const opener = screen.getByRole('button', { name: 'Model for this chat' });
    await user.click(opener);
    const dialog = await screen.findByRole('dialog', { name: 'Model for “Remember search filters”' });
    expect(within(dialog).queryByText(/Fictional model profiles|No provider is connected|^Scope:|Reasoning capabilities are facts only/)).not.toBeInTheDocument();
    expect(within(dialog).queryByLabelText('Reasoning level')).not.toBeInTheDocument();
    await user.click(within(dialog).getByRole('radio', { name: /Aster Mini.*128,000/ }));
    await user.keyboard('{Escape}');
    expect(dialog).not.toBeInTheDocument(); expect(opener).toHaveFocus(); expect(opener).toHaveTextContent(/^Aster$/);
    const page = await settings(user); await user.click(within(page).getByRole('button', { name: 'Models & providers' }));
    const defaultOpener = within(page).getByRole('button', { name: 'Choose default model' }); await user.click(defaultOpener);
    expect(screen.getByRole('dialog', { name: 'Default model for new chats' })).toBeVisible();
    await user.keyboard('{Escape}'); expect(defaultOpener).toHaveFocus();
    await user.click(within(page).getByRole('button', { name: 'Back to chat' })); expect(opener).toHaveTextContent(/^Aster$/);
  });

  it('does not mistake a fixed Working status for a busy model request', async () => {
    const user = start(); await user.click(projects().getByRole('button', { name: 'Keyboard navigation Working' }));
    await chooseMainModel(user, 'orion');
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    expect(screen.getByRole('button', { name: 'Stop' })).toBeEnabled();
    expect(projects().getByRole('button', { name: 'Keyboard navigation Working' })).toHaveAttribute('aria-current', 'page');
  });

  it.each(['Aster Mini', 'Orion'] as const)('uses %s only for new chats, including during readonly child inspection, with unknown new usage and cost', async (model) => {
    const user = start(); await user.click(screen.getByRole('button', { name: /^Completed Map return paths/ }));
    const child = await screen.findByRole('region', { name: /Child conversation:/ }); const text = child.textContent;
    const dialog = await settings(user); await user.click(within(dialog).getByRole('button', { name: 'Models & providers' }));
    await user.click(within(dialog).getByRole('button', { name: 'Choose default model' }));
    expect(screen.queryByLabelText('Reasoning level')).not.toBeInTheDocument();
    await useModel(user, model);
    expect(dialog).toHaveTextContent(`New chats start with ${model}.`);
    await user.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('region', { name: /Child conversation:/ })).toBe(child); expect(child.textContent).toBe(text);
    await user.click(screen.getByRole('button', { name: 'Back to parent' }));
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(/^Aster$/);
    expect(screen.getByRole('textbox')).toHaveValue('Add a regression test for returning from a result.');
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(model);
    expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('build');
    expect(screen.getByRole('button', { name: /^Open Context/ })).toHaveTextContent('No usage yet');
    await user.click(screen.getByRole('button', { name: /^Open Context/ }));
    const context = screen.getByRole('region', { name: 'Context information' });
    for (const fact of ['Active estimate', 'Reported total', 'Cost estimate · USD']) expect(within(context).getByText(fact, { selector: 'dt' }).nextElementSibling).toHaveTextContent('Unknown');
  });

  it('retains local preferences across close/reopen and resets only what the confirmation lists, with Undo', async () => {
    const user = start(); await chooseMainModel(user, 'orion');
    let dialog = await settings(user);
    await user.selectOptions(within(dialog).getByLabelText('Enter key behavior'), 'newline');
    expect(within(dialog).getByLabelText('New-chat mode')).toHaveValue('build');
    await user.click(within(dialog).getByRole('button', { name: 'Appearance' }));
    await user.selectOptions(within(dialog).getByLabelText('Settings-row density'), 'compact');
    await user.selectOptions(within(dialog).getByLabelText('App motion'), 'reduced');
    expect(dialog.firstElementChild).toHaveAttribute('data-density', 'compact'); expect(dialog.firstElementChild).toHaveAttribute('data-motion', 'reduced');
    await user.click(within(dialog).getByRole('button', { name: 'Models & providers' }));
    await user.click(within(dialog).getByRole('button', { name: 'Choose default model' })); await useModel(user, 'Aster Mini');
    await user.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    dialog = await settings(user);
    expect(within(dialog).getByRole('button', { name: 'Models & providers' })).toHaveAttribute('aria-current', 'page');
    await user.click(within(dialog).getByRole('button', { name: 'General' }));
    expect(within(dialog).getByLabelText('Enter key behavior')).toHaveValue('newline');
    expect(dialog.firstElementChild).toHaveAttribute('data-density', 'compact'); expect(dialog.firstElementChild).toHaveAttribute('data-motion', 'reduced');
    const reset = within(screen.getByRole('region', { name: 'General' })).getByRole('button', { name: 'Reset all preferences…' }); await user.click(reset);
    const confirm = screen.getByRole('dialog', { name: 'Reset preferences?' });
    const area = (name: string) => within(confirm).getByText(name, { selector: 'dt' }).nextElementSibling;
    expect(area('General')).toHaveTextContent('Enter key');
    expect(area('Appearance')).toHaveTextContent('Row density and motion');
    expect(area('Models & providers')).toHaveTextContent('New chats use Aster Mini');
    expect(within(confirm).queryByText('Shortcuts', { selector: 'dt' })).not.toBeInTheDocument();
    expect(confirm).toHaveTextContent('Chats, drafts, messages');
    await user.click(within(confirm).getByRole('button', { name: 'Cancel' }));
    expect(reset).toHaveFocus(); expect(within(dialog).getByLabelText('Enter key behavior')).toHaveValue('newline');
    await user.click(reset); await user.click(screen.getByRole('button', { name: 'Reset preferences' }));
    const undo = within(dialog).getByRole('button', { name: 'Undo' });
    expect(undo).toHaveFocus(); expect(dialog).toHaveTextContent('Preferences reset');
    expect(within(dialog).getByLabelText('Enter key behavior')).toHaveValue('send'); expect(dialog.firstElementChild).toHaveAttribute('data-density', 'comfortable');
    await user.click(undo);
    expect(within(dialog).queryByRole('button', { name: 'Undo' })).not.toBeInTheDocument(); expect(dialog).toHaveTextContent('Preferences restored.');
    expect(within(dialog).getByLabelText('Enter key behavior')).toHaveValue('newline'); expect(dialog.firstElementChild).toHaveAttribute('data-density', 'compact');
    await user.click(reset); await user.click(screen.getByRole('button', { name: 'Reset preferences' }));
    expect(within(dialog).getByLabelText('Enter key behavior')).toHaveValue('send');
    await user.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    expect(screen.getByRole('textbox')).toHaveValue('Add a regression test for returning from a result.');
    await user.click(screen.getByRole('button', { name: 'New chat' })); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(/^Aster$/);
  });

  it('names custom shortcuts in the reset confirmation and restores them with Undo', async () => {
    const user = start(); const page = await settings(user);
    await user.click(within(page).getByRole('button', { name: 'Shortcuts' }));
    // Every safe chord has a default owner: free Stop's period first, then give it to New chat.
    await user.click(within(page).getByRole('button', { name: 'Unassign Stop the run' }));
    const recorder = within(page).getByRole('button', { name: /^New chat shortcut:/ });
    await user.click(recorder);
    fireEvent.keyDown(recorder, { key: '>', code: 'Period', ctrlKey: true, shiftKey: true });
    expect(within(page).getByRole('button', { name: /^New chat shortcut:/ })).toHaveAccessibleName('New chat shortcut: Ctrl+Shift+.');
    await user.click(within(page).getByRole('button', { name: 'General' }));
    await user.click(within(page).getByRole('button', { name: 'Reset all preferences…' }));
    const confirm = screen.getByRole('dialog', { name: 'Reset preferences?' });
    expect(within(confirm).getByText('Shortcuts', { selector: 'dt' }).nextElementSibling).toHaveTextContent('Custom keys for New chat');
    expect(within(confirm).queryByText('General', { selector: 'dt' })).not.toBeInTheDocument();
    await user.click(within(confirm).getByRole('button', { name: 'Reset preferences' }));
    await user.click(within(page).getByRole('button', { name: 'Undo' }));
    await user.click(within(page).getByRole('button', { name: 'Shortcuts' }));
    expect(within(page).getByRole('button', { name: /^New chat shortcut:/ })).toHaveAccessibleName('New chat shortcut: Ctrl+Shift+.');
  });

  it('opens the palette and the chat finder from the composer and drives both from the keyboard', async () => {
    const user = start();
    const palette = await command(user, 'k');
    const search = within(palette).getByRole('combobox', { name: 'Search commands' });
    expect(search).toHaveFocus();
    const list = within(palette).getByRole('listbox', { name: 'Commands' });
    const option = (name: string) => within(list).getByRole('option', { name });
    expect(search).toHaveAttribute('aria-activedescendant', option('Find a chat').id);
    expect(option('Find a chat')).toHaveAttribute('aria-selected', 'true');
    expect(option('New chat')).toHaveTextContent('Ctrl+Shift+9');
    expect(option('Stop the run')).toHaveAttribute('aria-disabled', 'true');
    await user.keyboard('{ArrowDown}'); expect(search).toHaveAttribute('aria-activedescendant', option('New chat').id);
    await user.keyboard('{End}'); expect(search).toHaveAttribute('aria-activedescendant', option('Show or hide the inspector').id);
    await user.keyboard('{Home}'); expect(search).toHaveAttribute('aria-activedescendant', option('Find a chat').id);
    await user.type(search, 'new chat');
    expect(within(list).getAllByRole('option')[0]).toHaveAccessibleName('New chat');
    await user.keyboard('{Enter}');
    await waitFor(() => expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1'));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();

    const finder = await command(user, 'p');
    const chats = within(finder).getByRole('combobox', { name: 'Search chats' });
    expect(chats).toHaveFocus();
    expect(within(finder).getByRole('option', { name: 'New chat 1' })).toHaveTextContent('Current');
    expect(within(finder).getByRole('group', { name: 'Papertrail' })).toContainElement(within(finder).getByRole('option', { name: 'Export date range' }));
    await user.type(chats, 'export');
    await user.keyboard('{Escape}'); expect(chats).toHaveValue(''); expect(finder).toBeInTheDocument();
    await user.type(chats, 'export'); await user.keyboard('{Enter}');
    await waitFor(() => expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range'));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('renames and archives the current chat from the palette through the shell callbacks, and routes to archived chats', async () => {
    const user = start();
    let palette = await command(user, 'k');
    await user.type(within(palette).getByRole('combobox'), 'rename'); await user.keyboard('{Enter}');
    const rename = await screen.findByRole('dialog', { name: 'Rename chat' });
    const field = within(rename).getByRole('textbox');
    expect(field).toHaveValue('Remember search filters');
    await user.clear(field); await user.type(field, 'Saved filters{Enter}');
    await waitFor(() => expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Saved filters'));
    expect(projects().getByRole('button', { name: 'Saved filters' })).toBeVisible();

    palette = await command(user, 'k');
    await user.type(within(palette).getByRole('combobox'), 'archive'); await user.keyboard('{Enter}');
    const archive = await screen.findByRole('dialog', { name: 'Archive chat?' });
    await user.click(within(archive).getByRole('button', { name: /^Archive/ }));
    await waitFor(() => expect(projects().queryByRole('button', { name: /^Saved filters/ })).not.toBeInTheDocument());
    // The archive guidance opens Find a chat on Archived in one step, in place of the dialog; nothing else is archived.
    const next = screen.getByRole('heading', { level: 1 }).textContent!;
    palette = await command(user, 'k');
    await user.type(within(palette).getByRole('combobox'), 'archive'); await user.keyboard('{Enter}');
    const guidance = await screen.findByRole('dialog', { name: 'Archive chat?' });
    expect(guidance).toHaveTextContent('Nothing is deleted. Find it in Find a chat Ctrl+P → Archived.');
    await user.click(within(guidance).getByRole('button', { name: 'Find a chat, Archived' }));
    let finder = await screen.findByRole('dialog', { name: 'Find a chat' });
    expect(within(finder).getByRole('combobox', { name: 'Search chats' })).toHaveFocus();
    expect(within(finder).getByRole('button', { name: 'Archived' })).toHaveAttribute('aria-pressed', 'true');
    const archived = within(finder).getAllByRole('option');
    expect(archived).toHaveLength(1); expect(archived[0]).toHaveAccessibleName('Saved filters'); expect(archived[0]).toHaveTextContent('Archived');
    await user.keyboard('{Escape}');
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent(next);

    finder = await command(user, 'p');
    expect(within(finder).getByRole('button', { name: 'All' })).toHaveAttribute('aria-pressed', 'true');
    await user.click(within(finder).getByRole('button', { name: 'Archived' }));
    expect(within(finder).getByRole('button', { name: 'Archived' })).toHaveAttribute('aria-pressed', 'true');
    expect(within(finder).getByRole('option', { name: 'Saved filters' })).toHaveTextContent('Archived');
    expect(within(finder).queryByRole('option', { name: 'Export date range' })).not.toBeInTheDocument();
  });

  it('retains user definition edits and creates across Back/reopen and preference reset without changing chat/default models or children', async () => {
    const user = start(); await chooseMainModel(user, 'orion');
    const draft = (screen.getByRole('textbox') as HTMLTextAreaElement).value; const text = screen.getByRole('region', { name: 'Conversation: Remember search filters' }).textContent;
    await user.click(screen.getByRole('button', { name: /^Completed Map return paths/ }));
    const child = await screen.findByRole('region', { name: /Child conversation:/ }); const childText = child.textContent;
    let page = await settings(user); await user.click(within(page).getByRole('button', { name: 'Subagents' }));
    await user.click(within(page).getByRole('button', { name: 'Edit Notes reviewer' }));
    fireEvent.change(screen.getByRole('textbox', { name: 'Description' }), { target: { value: 'Retained local definition.' } });
    await user.selectOptions(screen.getByLabelText('Model preference'), 'aster-mini'); await user.click(screen.getByRole('button', { name: 'Save changes' }));
    await user.click(screen.getByRole('button', { name: 'New subagent' }));
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'New local reviewer' } });
    fireEvent.change(screen.getByRole('textbox', { name: 'Description' }), { target: { value: 'A second user definition.' } });
    await user.click(screen.getByRole('button', { name: 'Create subagent' }));
    await user.click(within(page).getByRole('button', { name: 'Models & providers' })); expect(page).toHaveTextContent('New chats start with Aster.');
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('region', { name: /Child conversation:/ })).toBe(child); expect(child.textContent).toBe(childText);
    await user.click(screen.getByRole('button', { name: 'Back to parent' })); expect(screen.getByRole('textbox')).toHaveValue(draft); expect(screen.getByRole('region', { name: 'Conversation: Remember search filters' }).textContent).toBe(text);
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    page = await settings(user); await user.click(within(page).getByRole('button', { name: 'Subagents' }));
    expect(within(page).getByRole('list', { name: 'Subagent definitions' })).toHaveTextContent('Retained local definition.');
    expect(within(page).getByRole('heading', { name: 'New local reviewer' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' })); expect(screen.getByLabelText('Model preference')).toHaveValue('aster-mini');
    await user.selectOptions(screen.getByLabelText('Model preference'), 'inherit'); await user.click(screen.getByRole('button', { name: 'Save changes' }));
    await user.click(within(page).getByRole('button', { name: 'Appearance' }));
    await user.selectOptions(within(page).getByLabelText('App motion'), 'reduced');
    await user.click(within(page).getByRole('button', { name: 'General' }));
    await user.click(screen.getByRole('button', { name: 'Reset all preferences…' })); await user.click(screen.getByRole('button', { name: 'Reset preferences' }));
    await user.click(within(page).getByRole('button', { name: 'Subagents' }));
    expect(within(page).getByRole('heading', { name: 'New local reviewer' })).toBeVisible();
    await user.click(within(page).getByRole('button', { name: 'Back to chat' })); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    await user.click(screen.getByRole('button', { name: 'New chat' })); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(/^Aster$/);
    page = await settings(user); expect(within(page).getByRole('heading', { name: 'New local reviewer' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' })); expect(screen.getByLabelText('Model preference')).toHaveValue('inherit');
  });

  it('connects and disconnects providers, keeping focus and calling nothing outside the page', async () => {
    const spies = ['fetch', 'XMLHttpRequest', 'WebSocket', 'FileReader', 'showOpenFilePicker', 'open'].map((name) => { const spy = vi.fn(); vi.stubGlobal(name, spy); return spy; });
    // Aster Lab starts connected; this flow connects it from scratch.
    setProviderState('aster-lab', { state: 'available', source: 'none' });
    const user = start(); const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const settingsOpener = screen.getByRole('button', { name: 'Settings' });
    const exportChat = projects().getByRole('button', { name: 'Export date range Attention' });
    const newChat = screen.getByRole('button', { name: 'New chat' });
    const dialog = await settings(user);
    await user.click(within(dialog).getByRole('button', { name: 'Models & providers' }));
    const connect = within(dialog).getByRole('button', { name: 'Connect Aster Lab' }); await user.click(connect);
    let inner = screen.getByRole('dialog', { name: 'Connect Aster Lab' });
    await user.click(within(inner).getByRole('button', { name: /Sign in with a code/ }));
    expect(within(inner).getByRole('button', { name: 'Copy code' })).toBeVisible();
    const ghostFocus = vi.spyOn(connect, 'focus');
    // Neither underlying page callbacks nor retained sidebar controls can leave a nested overlay.
    expectHiddenSidebar(sidebar);
    fireEvent.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    fireEvent.click(exportChat); fireEvent.click(newChat);
    expect(inner).toBeInTheDocument(); expect(screen.getByRole('region', { name: 'Settings page' })).toBe(dialog);
    expectHiddenSidebar(sidebar); expect(ghostFocus).not.toHaveBeenCalled();
    await user.click(within(inner).getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(connect).toHaveFocus();

    await user.click(connect); inner = screen.getByRole('dialog', { name: 'Connect Aster Lab' });
    await user.click(within(inner).getByRole('button', { name: /API key/ }));
    await user.type(within(inner).getByLabelText('API key'), validKey);
    await user.keyboard('{Escape}'); expect(within(inner).queryByRole('button', { name: 'Verify key' })).not.toBeInTheDocument();
    await user.click(within(inner).getByRole('button', { name: /API key/ })); expect(within(inner).getByLabelText('API key')).toHaveValue('');
    // Submitted synchronously, so the check is seen before its next-tick result replaces it.
    await user.type(within(inner).getByLabelText('API key'), validKey); fireEvent.click(within(inner).getByRole('button', { name: 'Verify key' }));
    expect(inner).toHaveTextContent('Verifying key…');
    await user.click(await within(inner).findByRole('button', { name: 'Done' }, { timeout: 2500 }));
    expect(inner).not.toBeInTheDocument();
    // The row moved to Connected; focus lands on its new action instead of the page body.
    expect(within(dialog).getByRole('button', { name: 'Disconnect Aster Lab' })).toHaveFocus();

    await user.click(within(dialog).getByRole('button', { name: 'Disconnect Orion Lab' }));
    inner = screen.getByRole('dialog', { name: 'Disconnect Orion Lab?' });
    await user.click(within(inner).getByRole('button', { name: 'Disconnect' }));
    expect(dialog).toHaveTextContent('Orion Lab disconnected.');
    expect(within(dialog).getByRole('button', { name: 'Connect Orion Lab' })).toHaveFocus();

    ghostFocus.mockClear();
    await user.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    expect(settingsOpener).toHaveFocus(); expect(settingsOpener).toBeVisible(); expect(sidebar).toBeVisible();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    await user.click(projects().getByRole('button', { name: 'Export date range Attention' }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range');
    expect(ghostFocus).not.toHaveBeenCalled();
    for (const spy of spies) expect(spy).not.toHaveBeenCalled();
  });

  it('reviews a failed provider at its cause, then offers its models after a successful retry', async () => {
    setProviderState('aster-lab', { state: 'available', source: 'none' });
    const user = start(); const dialog = await settings(user);
    await user.click(within(dialog).getByRole('button', { name: 'Models & providers' }));
    await user.click(within(dialog).getByRole('button', { name: 'Connect Harbor' }));
    const inner = screen.getByRole('dialog', { name: 'Connect Harbor' });
    expect(within(inner).getByRole('button', { name: 'Try again' })).toBeVisible();
    expect(within(inner).queryByRole('button', { name: /API key/ })).not.toBeInTheDocument();

    await user.click(within(inner).getByRole('button', { name: 'Close' }));
    expect(inner).not.toBeInTheDocument();
    await user.click(within(dialog).getByRole('button', { name: 'Connect Aster Lab' }));
    const aster = screen.getByRole('dialog', { name: 'Connect Aster Lab' });
    await user.click(within(aster).getByRole('button', { name: /API key/ }));
    await user.type(within(aster).getByLabelText('API key'), validKey); await user.click(within(aster).getByRole('button', { name: 'Verify key' }));
    await user.click(await within(aster).findByRole('button', { name: 'Choose a model' }, { timeout: 2500 }));
    const picker = await screen.findByRole('dialog', { name: 'Default model for new chats' });
    expect(within(picker).getByRole('searchbox', { name: 'Search models' })).toHaveValue('Aster Lab');
    expect(within(picker).queryByRole('radio', { name: /Orion/ })).not.toBeInTheDocument();
  });

  it('keeps each provider connection for the whole visit, in the providers dialog and Settings alike', async () => {
    const user = start();
    let page = await settings(user);
    await user.click(within(page).getByRole('button', { name: 'Models & providers' }));
    // The starting chats use Aster and Orion models, so both providers start connected.
    expect(within(page).getByRole('button', { name: 'Disconnect Orion Lab' })).toBeVisible();
    await user.click(within(page).getByRole('button', { name: 'Disconnect Aster Lab' }));
    await user.click(within(screen.getByRole('dialog', { name: 'Disconnect Aster Lab?' })).getByRole('button', { name: 'Disconnect' }));
    expect(within(page).getByRole('button', { name: 'Connect Aster Lab' })).toHaveFocus();
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));

    const palette = await command(user, 'k');
    await user.type(within(palette).getByRole('combobox'), 'connect a provider'); await user.keyboard('{Enter}');
    let providers = await screen.findByRole('dialog', { name: 'Connect a provider' });
    expect(within(providers).queryByRole('button', { name: 'Disconnect Aster Lab' })).not.toBeInTheDocument();
    await user.click(within(providers).getByRole('button', { name: 'Connect Aster Lab' }));
    const connect = screen.getByRole('dialog', { name: 'Connect Aster Lab' });
    await user.click(within(connect).getByRole('button', { name: /API key/ }));
    await user.type(within(connect).getByLabelText('API key'), validKey); await user.click(within(connect).getByRole('button', { name: 'Verify key' }));
    await user.click(await within(connect).findByRole('button', { name: 'Done' }, { timeout: 2500 }));
    providers = await screen.findByRole('dialog', { name: 'Connect a provider' });
    expect(within(providers).getByRole('button', { name: 'Disconnect Aster Lab' })).toBeVisible();
    await user.keyboard('{Escape}'); expect(screen.queryByRole('dialog')).not.toBeInTheDocument();

    page = await settings(user);
    expect(within(page).getByRole('button', { name: 'Models & providers' })).toHaveAttribute('aria-current', 'page');
    expect(within(page).getByRole('button', { name: 'Disconnect Aster Lab' })).toBeVisible();
    expect(within(page).getByRole('button', { name: 'Disconnect Orion Lab' })).toBeVisible();
  });

  it('branches the current chat from the chosen message into a new chat and opens it', async () => {
    const user = start();
    const palette = await command(user, 'k');
    await user.type(within(palette).getByRole('combobox'), 'branch'); await user.keyboard('{Enter}');
    const branch = await screen.findByRole('dialog', { name: 'Branch from a message' });
    expect(branch).not.toHaveTextContent(/preview/i);
    // The seeded request is longer than a row's one-line label; the picked row shows all of it.
    expect(within(branch).getByRole('radio')).toHaveAccessibleName('Message 1 of 1 Keep the search filters when I open a result and go back…');
    expect(within(branch).getByRole('region', { name: 'Message 1 in full' })).toHaveTextContent('Keep the search filters when I open a result and go back. Use the URL so I can share the same view.');
    await user.click(within(branch).getByRole('button', { name: 'Create branch' }));
    await waitFor(() => expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters (branch)'));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(projects().getByRole('button', { name: /^Remember search filters \(branch\)/ })).toHaveAttribute('aria-current', 'page');
    expect(projects().getByRole('button', { name: 'Remember search filters' })).toBeVisible();
  });

  it('finds a chat by words in its messages and shows them in context', async () => {
    const user = start();
    const finder = await command(user, 'p');
    await user.type(within(finder).getByRole('combobox', { name: 'Search chats' }), 'timezone');
    const exportChat = within(finder).getByRole('option', { name: 'Export date range' });
    expect(exportChat.querySelector('mark')).toHaveTextContent(/^timezone$/i);
    expect(exportChat).toHaveAccessibleDescription(/timezone/i);
    expect(within(finder).queryByRole('option', { name: 'Remember search filters' })).not.toBeInTheDocument();
    await user.click(exportChat);
    await waitFor(() => expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range'));
  });

  it('opens Help on Keyboard shortcuts from the palette, with the current keys, and changes them in Settings', async () => {
    const user = start();
    let page = await settings(user);
    await user.click(within(page).getByRole('button', { name: 'Shortcuts' }));
    await user.click(within(page).getByRole('button', { name: 'Unassign New chat' }));
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    const palette = await command(user, 'k');
    const stop = within(palette).getByRole('option', { name: 'Stop the run' });
    expect(stop).toHaveAttribute('aria-disabled', 'true'); expect(stop).toHaveTextContent('Nothing running'); expect(stop).toHaveTextContent('Ctrl+Shift+.');
    await user.type(within(palette).getByRole('combobox'), 'keyboard shortcuts'); await user.keyboard('{Enter}');
    const help = await screen.findByRole('dialog', { name: 'Help' });
    expect(within(help).getByRole('heading', { name: 'Keyboard shortcuts', level: 3 })).toHaveFocus();
    const keys = (group: string, action: string) => within(within(help).getByRole('table', { name: group })).getByRole('rowheader', { name: action }).closest('tr');
    expect(keys('General', 'New chat')).toHaveTextContent('Unassigned');
    expect(keys('Chat', 'Stop the run')).toHaveTextContent('Ctrl+Shift+.');
    expect(keys('Panels', 'Show or hide the inspector')).toHaveTextContent('Unassigned');
    expect(keys('Navigation', 'Open Help')).toHaveTextContent('F1');
    await user.click(within(help).getByRole('button', { name: 'Change shortcuts' }));
    page = await screen.findByRole('region', { name: 'Settings page' });
    expect(within(page).getByRole('button', { name: 'Shortcuts' })).toHaveAttribute('aria-current', 'page');
    expect(within(page).getByRole('heading', { name: 'Shortcuts', level: 2 })).toBeVisible();
  });

  it('opens Help from the palette, filters it as you type, clears then closes on Escape and goes to a topic’s settings', async () => {
    const user = start();
    let palette = await command(user, 'k');
    expect(within(palette).getByRole('option', { name: 'Help' })).toHaveTextContent('F1');
    await user.type(within(palette).getByRole('combobox'), 'help'); await user.keyboard('{Enter}');
    let help = await screen.findByRole('dialog', { name: 'Help' });
    const search = within(help).getByRole('searchbox', { name: 'Search help' });
    expect(search).toHaveFocus();
    expect(help).toHaveTextContent('You’re set up.');
    // Set up, Getting started folds to one line, so the topics come first.
    expect(within(help).getAllByRole('button', { expanded: false }).map((button) => button.textContent)).toEqual(['Show steps', 'Providers and API keys', 'Chats and projects', 'Build and Plan', 'Permissions', 'Changes', 'Subagents', 'Context', 'Tool details', 'Extensions', 'Appearance']);
    await user.type(search, 'allow once');
    expect(within(help).getByRole('button', { name: 'Permissions' })).toHaveAttribute('aria-expanded', 'true');
    expect(within(help).queryByRole('button', { name: 'Build and Plan' })).not.toBeInTheDocument();
    expect(within(help).queryByRole('table')).not.toBeInTheDocument();
    await user.clear(search); await user.type(search, 'zebra');
    expect(within(help).getByText('No topics or shortcuts match “zebra”.')).toBeVisible();
    await user.keyboard('{Escape}'); expect(search).toHaveValue(''); expect(help).toBeInTheDocument();
    await user.keyboard('{Escape}');
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(screen.getByRole('textbox')).toHaveFocus();

    palette = await command(user, 'k');
    await user.type(within(palette).getByRole('combobox'), 'help'); await user.keyboard('{Enter}');
    help = await screen.findByRole('dialog', { name: 'Help' });
    await user.click(within(help).getByRole('button', { name: 'Context' }));
    await user.click(within(help).getByRole('button', { name: 'Go to Context management' }));
    const page = await screen.findByRole('region', { name: 'Settings page' });
    expect(within(page).getByRole('button', { name: 'Context management' })).toHaveAttribute('aria-current', 'page');
  });

  it('starts Getting started from Help: Connect a provider replaces Help with the providers dialog', async () => {
    clearProviderConnections();
    const user = start();
    const palette = await command(user, 'k');
    await user.type(within(palette).getByRole('combobox'), 'help'); await user.keyboard('{Enter}');
    const help = await screen.findByRole('dialog', { name: 'Help' });
    await user.click(within(help).getByRole('button', { name: 'Connect a provider' }));
    const providers = await screen.findByRole('dialog', { name: 'Connect a provider' });
    expect(help).not.toBeInTheDocument();
    expect(within(providers).queryByRole('button', { name: /^Disconnect/ })).not.toBeInTheDocument();
    await user.keyboard('{Escape}');
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(screen.getByRole('textbox')).toHaveFocus();
  });

  it('opens Help on the topic a contextual link names and returns focus to the link', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Export date range Attention' }));
    const [link] = await screen.findAllByRole('button', { name: 'About permissions' });
    await user.click(link);
    const help = await screen.findByRole('dialog', { name: 'Help' });
    const topic = within(help).getByRole('button', { name: 'Permissions' });
    expect(topic).toHaveAttribute('aria-expanded', 'true'); expect(topic).toHaveFocus();
    expect(help).toHaveTextContent('Allow for this session covers this kind of action in this chat until AVA restarts');
    await user.keyboard('{Escape}');
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(link).toHaveFocus();
  });

  it('opens another project straight from the list, and simply closes when the open project is chosen', async () => {
    const user = start();
    let palette = await command(user, 'k');
    await user.type(within(palette).getByRole('combobox'), 'open a project'); await user.keyboard('{Enter}');
    let dialog = await screen.findByRole('dialog', { name: 'Open a project' });
    expect(dialog).not.toHaveTextContent(/workspace/i);
    expect(within(dialog).getByRole('button', { name: /^Wayfinder/ })).toHaveFocus();
    await user.keyboard('{Enter}');
    expect(dialog).not.toBeInTheDocument(); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');

    palette = await command(user, 'k');
    await user.type(within(palette).getByRole('combobox'), 'open a project'); await user.keyboard('{Enter}');
    dialog = await screen.findByRole('dialog', { name: 'Open a project' });
    await user.keyboard('{ArrowDown}'); expect(within(dialog).getByRole('button', { name: /^Papertrail/ })).toHaveFocus();
    await user.keyboard('{Enter}');
    await waitFor(() => expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range'));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });
});

describe('main screens host without a chat', () => {
  function host(request: Partial<MainScreenRequest>, props: Partial<MainScreensProps> = {}) {
    const onDismiss = vi.fn();
    render(<MainScreens request={{ id: 'main-screens', version: 1, kind: 'help', parentChatId: '', inspectedChildId: null, ...request }} parent={{ title: '', model: 'aster' }} defaultModel="aster"
      preferences={defaultPreferences} opaque={false} onOpaqueChange={vi.fn()} section="general" onSectionChange={vi.fn()} onPreferencesChange={vi.fn()} onSelectModel={vi.fn()}
      onDismiss={onDismiss} returnFocus={{ current: null }} chats={[]} projects={[]} onOpenChat={vi.fn()} {...props} />);
    return onDismiss;
  }
  const catalog = [{ id: 'wayfinder', name: 'Wayfinder' }, { id: 'papertrail', name: 'Papertrail' }];

  it('opens Help expanded on the requested topic and goes to its settings page', async () => {
    const onOpenSettingsSection = vi.fn();
    const onDismiss = host({ helpTopic: 'context' }, { onOpenSettingsSection });
    const help = screen.getByRole('dialog', { name: 'Help' });
    const topic = within(help).getByRole('button', { name: 'Context' });
    expect(topic).toHaveAttribute('aria-expanded', 'true'); expect(topic).toHaveFocus();
    expect(within(help).getByRole('button', { name: 'Build and Plan' })).toHaveAttribute('aria-expanded', 'false');
    await userEvent.setup().click(within(help).getByRole('button', { name: 'Go to Context management' }));
    expect(onOpenSettingsSection).toHaveBeenCalledExactlyOnceWith('context'); expect(onDismiss).toHaveBeenCalledTimes(1);
  });

  it('offers projects a new user can open from Getting started and opens the chosen one', async () => {
    const onOpenProject = vi.fn();
    const onDismiss = host({}, { projectsToOpen: catalog, onOpenProject });
    const user = userEvent.setup();
    const help = screen.getByRole('dialog', { name: 'Help' });
    await user.click(within(help).getByRole('button', { name: 'Open a project' }));
    const picker = screen.getByRole('dialog', { name: 'Open a project' });
    expect(within(picker).getByRole('button', { name: /^Wayfinder/ })).toHaveTextContent('Not opened yet');
    expect(within(picker).getByRole('button', { name: /^Wayfinder/ })).toHaveFocus();
    await user.click(within(picker).getByRole('button', { name: /^Papertrail/ }));
    expect(onOpenProject).toHaveBeenCalledExactlyOnceWith('papertrail'); expect(onDismiss).toHaveBeenCalledTimes(1);
  });

  it('holds chat commands until a project is open', () => {
    host({ kind: 'commands' }, { onAppAction: vi.fn(), projectsToOpen: catalog, onOpenProject: vi.fn() });
    const palette = screen.getByRole('dialog', { name: 'Commands' });
    for (const name of ['Choose model', 'Focus composer', 'Toggle Agents', 'Toggle Changes', 'Toggle Context', 'Show or hide the inspector']) expect(within(palette).getByRole('option', { name })).toHaveTextContent('Open a project first');
    expect(within(palette).getByRole('option', { name: 'Open a project' })).not.toHaveAttribute('aria-disabled');
    expect(within(palette).getByRole('option', { name: 'Open a project' })).toHaveAccessibleDescription(/^Switch projects or open another one/);
    expect(within(palette).getByRole('option', { name: 'Help' })).not.toHaveAttribute('aria-disabled');
  });

  it('leads with Done after connecting a provider while Getting started has steps left', async () => {
    clearProviderConnections();
    host({ kind: 'providers' });
    const user = userEvent.setup();
    await user.click(screen.getByRole('button', { name: 'Connect Aster Lab' }));
    const aster = screen.getByRole('dialog', { name: 'Connect Aster Lab' });
    await user.click(within(aster).getByRole('button', { name: /API key/ }));
    await user.type(within(aster).getByLabelText('API key'), validKey); await user.click(within(aster).getByRole('button', { name: 'Verify key' }));
    const done = await within(aster).findByRole('button', { name: 'Done' }, { timeout: 2500 });
    expect(done).toHaveAttribute('data-tone', 'primary');
    expect(within(aster).getByRole('button', { name: 'Choose a model' })).toHaveAttribute('data-tone', 'quiet');
  });

  it('offers Open a project from an empty Find a chat before any project is open', async () => {
    host({ kind: 'sessions' }, { projectsToOpen: catalog, onOpenProject: vi.fn() });
    const finder = screen.getByRole('dialog', { name: 'Find a chat' });
    expect(finder).toHaveTextContent('No chats yet.');
    await userEvent.setup().click(within(finder).getByRole('button', { name: 'Open a project' }));
    expect(finder).not.toBeInTheDocument();
    expect(within(screen.getByRole('dialog', { name: 'Open a project' })).getByRole('button', { name: /^Wayfinder/ })).toHaveTextContent('Not opened yet');
  });

  it('offers New chat from an empty Find a chat when a project is open', async () => {
    const onAppAction = vi.fn();
    const onDismiss = host({ kind: 'sessions' }, { projects: [catalog[0]], onAppAction });
    await userEvent.setup().click(within(screen.getByRole('dialog', { name: 'Find a chat' })).getByRole('button', { name: 'New chat' }));
    expect(onAppAction).toHaveBeenCalledExactlyOnceWith('newChat'); expect(onDismiss).toHaveBeenCalledTimes(1);
  });

  it('offers no first-chat step from Find a chat when the host can open nothing', () => {
    host({ kind: 'sessions' });
    const finder = screen.getByRole('dialog', { name: 'Find a chat' });
    expect(finder).toHaveTextContent('No chats yet.');
    expect(within(finder).queryByRole('button', { name: /^(New chat|Open a project)$/ })).not.toBeInTheDocument();
  });

  it('offers Search Settings, only when Settings would find the words, when Help finds nothing', async () => {
    const onSearchSettings = vi.fn();
    const onDismiss = host({}, { onSearchSettings });
    const user = userEvent.setup();
    const help = screen.getByRole('dialog', { name: 'Help' });
    const search = within(help).getByRole('searchbox', { name: 'Search help' });
    await user.type(search, 'zebra');
    expect(within(help).queryByRole('button', { name: /^Search Settings/ })).not.toBeInTheDocument();
    await user.clear(search); await user.type(search, 'sound');
    expect(within(help).getByText('No topics or shortcuts match “sound”.')).toBeVisible();
    await user.click(within(help).getByRole('button', { name: 'Search Settings for “sound”' }));
    expect(onSearchSettings).toHaveBeenCalledExactlyOnceWith('sound'); expect(onDismiss).toHaveBeenCalledTimes(1);
  });

  it('opens Commands from Help’s no-match next steps', async () => {
    host({});
    const user = userEvent.setup();
    const help = screen.getByRole('dialog', { name: 'Help' });
    await user.type(within(help).getByRole('searchbox', { name: 'Search help' }), 'zebra');
    await user.click(within(help).getByRole('button', { name: 'Open Commands' }));
    expect(help).not.toBeInTheDocument();
    expect(within(screen.getByRole('dialog', { name: 'Commands' })).getByRole('combobox', { name: 'Search commands' })).toHaveValue('');
  });

  it('opens Help over Settings with F1, and Help closes back onto the page, its search or another section', async () => {
    const onSectionChange = vi.fn();
    const onDismiss = host({ kind: 'settings' }, { onSectionChange });
    const user = userEvent.setup();
    const settingsSearch = screen.getByRole('searchbox', { name: 'Search settings' });
    settingsSearch.focus();
    await user.keyboard('{F1}');
    let help = screen.getByRole('dialog', { name: 'Help' });
    const helpSearch = within(help).getByRole('searchbox', { name: 'Search help' });
    expect(helpSearch).toHaveFocus();
    // Inside Help, F1 only keeps the browser's own help away.
    expect(fireEvent.keyDown(helpSearch, { key: 'F1' })).toBe(false);
    expect(screen.getAllByRole('dialog')).toHaveLength(1);
    await user.keyboard('{Escape}');
    expect(help).not.toBeInTheDocument(); expect(settingsSearch).toHaveFocus(); expect(onDismiss).not.toHaveBeenCalled();

    // Over Settings, its own search is the next step; Commands isn't offered.
    await user.keyboard('{F1}');
    help = screen.getByRole('dialog', { name: 'Help' });
    await user.type(within(help).getByRole('searchbox', { name: 'Search help' }), 'sound');
    expect(within(help).queryByRole('button', { name: 'Open Commands' })).not.toBeInTheDocument();
    await user.click(within(help).getByRole('button', { name: 'Search Settings for “sound”' }));
    expect(help).not.toBeInTheDocument();
    expect(settingsSearch).toHaveValue('sound'); expect(settingsSearch).toHaveFocus();
    expect(screen.getByRole('heading', { name: 'Search results', level: 2 })).toBeVisible();

    await user.keyboard('{F1}');
    help = screen.getByRole('dialog', { name: 'Help' });
    await user.click(within(help).getByRole('button', { name: 'Appearance' }));
    await user.click(within(help).getByRole('button', { name: 'Go to Appearance' }));
    expect(help).not.toBeInTheDocument();
    expect(onSectionChange).toHaveBeenCalledExactlyOnceWith('appearance');
    expect(settingsSearch).toHaveValue('');
    expect(screen.getByRole('region', { name: 'General' })).toHaveFocus();
    expect(onDismiss).not.toHaveBeenCalled();
  });
});
