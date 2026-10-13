import { StrictMode } from 'react';
import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { AppShell } from '../components/AppShell';
import { chooseMainModel, installDialogTestSupport } from './dialogTestSupport';

beforeEach(installDialogTestSupport);
function start() { render(<StrictMode><AppShell /></StrictMode>); return userEvent.setup(); }
function projects() { return within(screen.getByRole('navigation', { name: 'All project chats' })); }
function expectHiddenSidebar(sidebar: HTMLElement) {
  expect(sidebar).toBeInTheDocument(); expect(sidebar).not.toBeVisible();
  expect(sidebar).toHaveAttribute('hidden'); expect(sidebar).toHaveAttribute('inert');
  expect(screen.queryByRole('complementary', { name: 'Projects and chats' })).not.toBeInTheDocument();
  expect(screen.queryByRole('region', { name: 'Chat library' })).not.toBeInTheDocument();
  for (const name of ['All project chats', 'Pinned chats', 'Recent chats']) expect(screen.queryByRole('navigation', { name })).not.toBeInTheDocument();
  for (const name of ['New chat', 'Settings', 'Opaque materials']) expect(screen.queryByRole('button', { name })).not.toBeInTheDocument();
}
async function settings(user: ReturnType<typeof userEvent.setup>) {
  await user.click(screen.getByRole('button', { name: 'Settings' }));
  await screen.findByRole('navigation', { name: 'Settings sections' });
  return screen.getByRole('region', { name: 'Settings page' });
}
async function useModel(user: ReturnType<typeof userEvent.setup>, label: 'Aster' | 'Aster Mini' | 'Orion') {
  const dialog = await screen.findByRole('dialog', { name: 'Choose a model' });
  const id = { Aster: 'aster', 'Aster Mini': 'aster-mini', Orion: 'orion' }[label];
  await user.click(within(dialog).getAllByRole<HTMLInputElement>('radio').find((input) => input.value === id)!);
  await user.click(within(dialog).getByRole('button', { name: `Use ${label}` }));
}

describe('main-chat lazy screen integration', () => {
  it.each(['Remember search filters', 'Working Keyboard navigation'])('blocks hidden chat edits, Send and Stop behind Settings for %s', async (name) => {
    const user = start();
    if (name !== 'Remember search filters') await user.click(projects().getByRole('button', { name }));
    const draft = screen.getByRole('textbox') as HTMLTextAreaElement;
    const value = draft.value; const form = screen.getByRole('form', { name: 'Message composer' });
    const action = screen.getByRole('button', { name: name.startsWith('Working') ? 'Stop mock work' : 'Send' });
    const page = await settings(user);
    expect(form.closest('[hidden][inert]')).not.toBeNull();
    fireEvent.change(draft, { target: { value: 'Must not replace the hidden draft' } });
    fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true }); fireEvent.submit(form); fireEvent.click(action);
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('textbox')).toBe(draft); expect(draft).toHaveValue(value);
    expect(screen.queryByRole('article', { name: 'Your local preview message' })).not.toBeInTheDocument();
    expect(projects().getByRole('button', { name })).toHaveAttribute('aria-current', 'page');
  });

  it('makes Enter preferences operational only in the local composer, retaining Shift+Enter and IME behavior', async () => {
    const user = start(); const page = await settings(user);
    await user.selectOptions(within(page).getByLabelText('Enter key behavior'), 'newline');
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    const draft = screen.getByRole('textbox') as HTMLTextAreaElement; draft.focus(); draft.setSelectionRange(draft.value.length, draft.value.length);
    await user.keyboard('{Enter}'); expect(screen.queryByRole('article', { name: 'Your local preview message' })).not.toBeInTheDocument();
    expect(draft).toHaveValue('Add a regression test for returning from a result.\n');
    const reopened = await settings(user);
    await user.selectOptions(within(reopened).getByLabelText('Enter key behavior'), 'send');
    await user.click(within(reopened).getByRole('button', { name: 'Back to chat' }));
    draft.focus(); await user.keyboard('{Shift>}{Enter}{/Shift}');
    fireEvent.compositionStart(draft); fireEvent.keyDown(draft, { key: 'Enter' }); fireEvent.compositionEnd(draft);
    fireEvent.keyDown(draft, { key: 'Enter', keyCode: 229 });
    expect(screen.queryByRole('article', { name: 'Your local preview message' })).not.toBeInTheDocument();
    await user.keyboard('{Enter}'); expect(screen.getByRole('article', { name: 'Your local preview message' })).toBeVisible();
    expect(screen.getAllByRole('article', { name: 'AVA example response' })).toHaveLength(1);
  });
  it('returns only to a visible Settings opener or the visible conversation title fallback', async () => {
    const user = start(); const opener = screen.getByRole('button', { name: 'Settings' });
    const title = screen.getByRole('heading', { level: 1 }); const page = await settings(user);
    opener.hidden = true;
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    expect(title).toHaveFocus(); expect(title.closest('[hidden], [inert]')).toBeNull();
  });

  it('replaces the sidebar and workspace with a sixteen-section Settings page, one heading and nav-local search', async () => {
    const user = start();
    const app = screen.getByRole('main', { name: 'AVA UI preview' });
    const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const opener = screen.getByRole('button', { name: 'Settings' });
    expect(sidebar).toContainElement(opener);
    expect(screen.getAllByRole('button', { name: 'Settings' })).toHaveLength(1);
    expect(screen.queryByRole('link', { name: 'Screens review' })).not.toBeInTheDocument();
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
    expect(nav).toContainElement(within(dialog).getByRole('searchbox', { name: 'Search settings' }));
    const sections = within(nav).getAllByRole('button');
    expect(within(nav).queryByRole('button', { name: /Reset/ })).not.toBeInTheDocument();
    expect(sections).toHaveLength(16);
    for (const button of sections) {
      await user.click(button);
      expect(button).toHaveAttribute('aria-current', 'page');
      expect(within(dialog).getByRole('heading', { name: button.textContent!, level: 2 })).toBeVisible();
    }
    expect(within(dialog).getByRole('link', { name: 'Screens review' })).toHaveAttribute('href', '?view=screens');
    expect(dialog).toHaveTextContent('Local edits last for this visit only; no model is invoked.');
    await user.click(within(dialog).getByRole('button', { name: 'Permissions & trust' }));
    expect(dialog).toHaveTextContent('Remember search filters · current example parent');
    expect(dialog).not.toHaveTextContent('Atlas');
    expect(within(dialog).getByRole('button', { name: 'Read-only scope' })).toBeDisabled();
    await user.click(within(dialog).getByRole('button', { name: 'MCP' }));
    expect(within(dialog).getByRole('article', { name: 'Workspace notes' })).toHaveTextContent('Not connected');
    await user.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    expect(opener).toHaveFocus(); expect(opener).toBeVisible(); expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(screen.getByRole('complementary', { name: 'Projects and chats' })).toBe(sidebar);
    expect(sidebar).toBeVisible(); expect(sidebar).not.toHaveAttribute('hidden'); expect(sidebar).not.toHaveAttribute('inert');
    expect(sidebar.parentElement).toBe(app); expect(app).toHaveAttribute('data-page', 'chat');
  });

  it('rejects retained sidebar controls in Settings and restores the same library state on Back', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Pin Keyboard navigation' }));
    await user.click(projects().getByRole('button', { name: 'Relay' }));
    const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const opener = screen.getByRole('button', { name: 'Settings' });
    const title = screen.getByRole('heading', { level: 1 }); const draft = screen.getByRole('textbox');
    const controls = [
      projects().getByRole('button', { name: 'Attention Export date range' }),
      screen.getByRole('button', { name: 'New chat' }), opener,
      projects().getByRole('button', { name: 'Pin Remember search filters' }),
      projects().getByRole('button', { name: 'Unpin Keyboard navigation' }),
      projects().getByRole('button', { name: 'Wayfinder' }), projects().getByRole('button', { name: 'Relay' }),
      screen.getByRole('button', { name: 'Remove Remember search filters from recent chats' }),
      screen.getByRole('button', { name: 'Opaque materials' }),
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
    expect(projects().getByRole('button', { name: 'Relay' })).toHaveAttribute('aria-expanded', 'false');
    expect(within(screen.getByRole('navigation', { name: 'Pinned chats' })).getAllByRole('listitem')).toHaveLength(1);
    expect(projects().getByRole('button', { name: 'Unpin Keyboard navigation' })).toHaveAttribute('aria-pressed', 'true');
    expect(screen.getByRole('button', { name: 'Remove Remember search filters from recent chats' })).toBeVisible();
    expect(screen.getByRole('button', { name: 'Opaque materials' })).toHaveAttribute('aria-pressed', 'false');
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');
  });

  it('keeps the Settings page under a single native overlay; Escape clears inner query, closes it, then handles page context', async () => {
    const user = start(); const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const dialog = await settings(user);
    await user.click(within(dialog).getByRole('button', { name: 'Models' }));
    const opener = within(dialog).getByRole('button', { name: 'Choose current model' });
    await user.click(opener);
    const modelDialog = screen.getByRole('dialog', { name: 'Choose a model' });
    expect(screen.getAllByRole('dialog')).toHaveLength(1);
    expect(screen.getByRole('region', { name: 'Settings page' })).toBe(dialog); expectHiddenSidebar(sidebar);
    const search = within(modelDialog).getByRole('searchbox');
    await user.type(search, 'Orion');
    fireEvent.keyDown(search, { key: 'Escape', isComposing: true }); expect(search).toHaveValue('Orion');
    await user.keyboard('{Escape}'); expect(search).toHaveValue(''); expect(search).toHaveFocus();
    await user.keyboard('{Escape}'); expect(modelDialog).not.toBeInTheDocument(); expect(opener).toHaveFocus();
    expectHiddenSidebar(sidebar);
    expect(within(dialog).getByRole('button', { name: 'Models' })).toHaveAttribute('aria-current', 'page');
    await user.keyboard('{Escape}');
    expect(within(dialog).getByRole('button', { name: 'General' })).toHaveAttribute('aria-current', 'page');
    const settingsSearch = within(dialog).getByRole('searchbox'); await user.type(settingsSearch, 'no matching section');
    fireEvent.keyDown(settingsSearch, { key: 'Escape', keyCode: 229 }); expect(settingsSearch).toHaveValue('no matching section');
    await user.keyboard('{Escape}'); expect(settingsSearch).toHaveValue('');
    await user.keyboard('{Escape}'); expect(dialog).not.toBeInTheDocument(); expect(screen.getByRole('button', { name: 'Settings' })).toHaveFocus();
    expect(screen.getByRole('complementary', { name: 'Projects and chats' })).toBe(sidebar); expect(sidebar).toBeVisible();
  });

  it('applies only the selected parent profile, preserving drafts, messages, pins, mode, references, expansion, Context and title focus', async () => {
    const user = start();
    const title = screen.getByRole('heading', { level: 1 }); const focus = vi.spyOn(title, 'focus');
    const transcript = screen.getByRole('region', { name: 'Conversation: Remember search filters' }); const text = transcript.textContent;
    const draft = screen.getByRole('textbox'); fireEvent.change(draft, { target: { value: 'Preserved parent draft' } });
    await user.selectOptions(screen.getByRole('combobox', { name: 'Mode for this chat' }), 'plan');
    await user.click(screen.getByRole('button', { name: 'Expand draft' }));
    await user.click(screen.getByRole('button', { name: 'Add example context' }));
    await user.click(screen.getByRole('button', { name: 'image result-layout.png' })); await user.keyboard('{Escape}');
    await user.click(projects().getByRole('button', { name: 'Pin Remember search filters' }));
    const sidebarText = screen.getByRole('complementary', { name: 'Projects and chats' }).textContent;
    await user.click(screen.getByRole('button', { name: /^Open Context/ }));
    await user.click(await screen.findByRole('button', { name: 'Raw messages' })); const raw = screen.getByRole('region', { name: 'Local committed messages' });
    const page = await settings(user);
    const query = within(page).getByRole('searchbox');
    await user.type(query, 'does not exist');
    expect(within(page).getByRole('status')).toHaveTextContent('No matching settings');
    fireEvent(window, new Event('resize')); expect(query).toHaveFocus();
    await user.click(within(page).getByRole('button', { name: 'Clear search' }));
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('region', { name: 'Local committed messages' })).toBe(raw);
    await chooseMainModel(user, 'orion');
    expect(screen.getByLabelText('Model: Aster')).toBeVisible();
    expect(screen.getByLabelText('Thinking: High')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveFocus();
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    expect(screen.getByRole('textbox')).toBe(draft); expect(draft).toHaveValue('Preserved parent draft');
    expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan');
    expect(screen.getByRole('button', { name: 'Collapse draft' })).toBeVisible();
    expect(screen.getByRole('list', { name: 'Attached example context' })).toHaveTextContent('result-layout.png');
    expect(screen.getByRole('region', { name: 'Local committed messages' })).toBe(raw);
    expect(screen.getByRole('region', { name: 'Conversation: Remember search filters' })).toBe(transcript); expect(transcript.textContent).toBe(text);
    expect(screen.getByRole('complementary', { name: 'Projects and chats' }).textContent).toBe(sidebarText);
    expect(screen.getByRole('button', { name: /^Open Context/ })).toHaveTextContent('Context unknown');
    expect(focus).not.toHaveBeenCalled();
  });

  it('uses a concise model browser in chat and Settings without visible catalog caveats or scope paragraphs', async () => {
    const user = start(); const opener = screen.getByRole('button', { name: 'Model for this chat' });
    await user.click(opener);
    const dialog = await screen.findByRole('dialog', { name: 'Choose a model' });
    expect(dialog).toHaveAccessibleDescription(/Remember search filters/);
    expect(within(dialog).getByRole('region', { name: 'Selected model' })).toHaveTextContent('This chat');
    expect(within(dialog).getByRole('group', { name: 'Aster family Aster Lab' })).toBeVisible();
    expect(within(dialog).queryByText(/Fictional model profiles|No provider is connected|^Scope:|Reasoning capabilities are facts only/)).not.toBeInTheDocument();
    expect(within(dialog).queryByLabelText('Reasoning level')).not.toBeInTheDocument();
    await user.click(within(dialog).getByRole('radio', { name: /Aster Mini.*128,000/ }));
    await user.click(within(dialog).getByRole('button', { name: 'Cancel' }));
    expect(opener).toHaveFocus(); expect(opener).toHaveTextContent(/^Aster$/);
    const page = await settings(user); await user.click(within(page).getByRole('button', { name: 'Models' }));
    expect(page).toHaveTextContent('Remember search filters · Selected: Aster'); expect(page).toHaveTextContent('Default: Aster');
    expect(within(page).queryByText(/The shared picker|token accounting|billing|Changes only this chat|Used by New chat/)).not.toBeInTheDocument();
    const defaultOpener = within(page).getByRole('button', { name: 'Choose default model' }); await user.click(defaultOpener);
    const defaultDialog = screen.getByRole('dialog', { name: 'Choose a model' });
    expect(defaultDialog).toHaveAccessibleDescription(/existing chats stay unchanged/);
    expect(within(defaultDialog).getByRole('region', { name: 'Selected model' })).toHaveTextContent('New chat default');
    await user.click(within(defaultDialog).getByRole('button', { name: 'Cancel' })); expect(defaultOpener).toHaveFocus();
    await user.click(within(page).getByRole('button', { name: 'Back to chat' })); expect(opener).toHaveTextContent(/^Aster$/);
  });

  it('does not mistake a fixed Working status for a busy model request', async () => {
    const user = start(); await user.click(projects().getByRole('button', { name: 'Working Keyboard navigation' }));
    await chooseMainModel(user, 'orion');
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    expect(screen.getByRole('button', { name: 'Stop mock work' })).toBeEnabled();
    expect(projects().getByRole('button', { name: 'Working Keyboard navigation' })).toHaveAttribute('aria-current', 'page');
  });

  it.each(['Aster Mini', 'Orion'] as const)('uses %s only for new chats, including during readonly child inspection, with unknown new usage and cost', async (model) => {
    const user = start(); await user.click(screen.getByRole('button', { name: /^Completed Map return paths/ }));
    const child = await screen.findByRole('region', { name: /Child conversation:/ }); const text = child.textContent;
    const dialog = await settings(user); await user.click(within(dialog).getByRole('button', { name: 'Models' }));
    expect(within(dialog).getByRole('button', { name: 'Choose current model' })).toBeDisabled();
    expect(dialog).toHaveTextContent('Read-only child inspection');
    await user.click(within(dialog).getByRole('button', { name: 'Choose default model' }));
    expect(screen.queryByLabelText('Reasoning level')).not.toBeInTheDocument();
    expect(screen.getByRole('dialog', { name: 'Choose a model' })).toHaveAccessibleDescription(/existing chats stay unchanged/);
    await useModel(user, model);
    expect(dialog).toHaveTextContent(`Default: ${model}`); expect(dialog).toHaveTextContent('Selected: Aster');
    await user.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('region', { name: /Child conversation:/ })).toBe(child); expect(child.textContent).toBe(text);
    await user.click(screen.getByRole('button', { name: 'Back to parent' }));
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(/^Aster$/);
    expect(screen.getByRole('textbox')).toHaveValue('Add a regression test for returning from a result.');
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(model);
    expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('build');
    expect(screen.getByRole('button', { name: /^Open Context/ })).toHaveTextContent('Context unknown');
    await user.click(screen.getByRole('button', { name: /^Open Context/ }));
    const context = screen.getByRole('region', { name: 'Context information' });
    for (const fact of ['Active estimate', 'Reported total', 'Cost estimate · USD']) expect(within(context).getByText(fact, { selector: 'dt' }).nextElementSibling).toHaveTextContent('Unknown');
  });

  it('retains local preferences across close/reopen and resets preferences/default only after confirmation', async () => {
    const user = start(); await chooseMainModel(user, 'orion');
    let dialog = await settings(user);
    await user.selectOptions(within(dialog).getByLabelText('Enter key behavior'), 'newline');
    expect(within(dialog).getByLabelText('New-chat mode')).toHaveValue('build');
    expect(within(dialog).queryByRole('textbox', { name: 'Practice input' })).not.toBeInTheDocument();
    await user.click(within(dialog).getByRole('button', { name: 'Appearance' }));
    await user.selectOptions(within(dialog).getByLabelText('Settings-row density'), 'compact');
    await user.selectOptions(within(dialog).getByLabelText('App motion'), 'reduced');
    expect(dialog).toHaveTextContent('Light and system theme switching are not available.');
    expect(dialog.firstElementChild).toHaveAttribute('data-density', 'compact'); expect(dialog.firstElementChild).toHaveAttribute('data-motion', 'reduced');
    await user.click(within(dialog).getByRole('button', { name: 'Models' }));
    await user.click(within(dialog).getByRole('button', { name: 'Choose default model' })); await useModel(user, 'Aster Mini');
    await user.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    dialog = await settings(user);
    expect(within(dialog).getByRole('button', { name: 'Models' })).toHaveAttribute('aria-current', 'page');
    await user.click(within(dialog).getByRole('button', { name: 'General' }));
    expect(within(dialog).getByLabelText('Enter key behavior')).toHaveValue('newline');
    expect(dialog.firstElementChild).toHaveAttribute('data-density', 'compact'); expect(dialog.firstElementChild).toHaveAttribute('data-motion', 'reduced');
    const reset = within(screen.getByRole('region', { name: 'General' })).getByRole('button', { name: 'Reset all preferences…' }); await user.click(reset);
    const confirm = screen.getByRole('dialog', { name: 'Reset preferences?' });
    expect(confirm).toHaveTextContent('Existing chats'); await user.click(within(confirm).getByRole('button', { name: 'Cancel' }));
    expect(reset).toHaveFocus(); expect(within(dialog).getByLabelText('Enter key behavior')).toHaveValue('newline');
    await user.click(reset); await user.click(screen.getByRole('button', { name: 'Reset preferences and default' }));
    expect(reset).toHaveFocus(); expect(within(dialog).getByLabelText('Enter key behavior')).toHaveValue('send');
    await user.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    expect(screen.getByRole('textbox')).toHaveValue('Add a regression test for returning from a result.');
    await user.click(screen.getByRole('button', { name: 'New chat' })); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(/^Aster$/);
  });

  it('retains user definition edits and creates across Back/reopen and preference reset without changing chat/default models or children', async () => {
    const user = start(); await chooseMainModel(user, 'orion');
    const draft = (screen.getByRole('textbox') as HTMLTextAreaElement).value; const text = screen.getByRole('region', { name: 'Conversation: Remember search filters' }).textContent;
    await user.click(screen.getByRole('button', { name: /^Completed Map return paths/ }));
    const child = await screen.findByRole('region', { name: /Child conversation:/ }); const childText = child.textContent;
    let page = await settings(user); await user.click(within(page).getByRole('button', { name: 'Subagents' }));
    await user.click(within(page).getByRole('button', { name: 'Edit Notes reviewer' }));
    fireEvent.change(screen.getByRole('textbox', { name: 'Description' }), { target: { value: 'Retained local definition.' } });
    await user.selectOptions(screen.getByLabelText('Model preference'), 'aster-mini'); await user.click(screen.getByRole('button', { name: 'Save definition' }));
    await user.click(screen.getByRole('button', { name: 'New subagent' }));
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'New local reviewer' } });
    fireEvent.change(screen.getByRole('textbox', { name: 'Description' }), { target: { value: 'A second user definition.' } });
    await user.click(screen.getByRole('button', { name: 'Create definition' }));
    await user.click(within(page).getByRole('button', { name: 'Models' })); expect(page).toHaveTextContent('Selected: Orion'); expect(page).toHaveTextContent('Default: Aster');
    await user.click(within(page).getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('region', { name: /Child conversation:/ })).toBe(child); expect(child.textContent).toBe(childText);
    await user.click(screen.getByRole('button', { name: 'Back to parent' })); expect(screen.getByRole('textbox')).toHaveValue(draft); expect(screen.getByRole('region', { name: 'Conversation: Remember search filters' }).textContent).toBe(text);
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    page = await settings(user); await user.click(within(page).getByRole('button', { name: 'Subagents' }));
    expect(within(page).getByRole('list', { name: 'Subagent definitions' })).toHaveTextContent('Retained local definition.');
    expect(within(page).getByRole('heading', { name: 'New local reviewer' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' })); expect(screen.getByLabelText('Model preference')).toHaveValue('aster-mini');
    await user.selectOptions(screen.getByLabelText('Model preference'), 'inherit'); await user.click(screen.getByRole('button', { name: 'Save definition' }));
    await user.click(within(page).getByRole('button', { name: 'General' }));
    await user.click(screen.getByRole('button', { name: 'Reset all preferences…' })); await user.click(screen.getByRole('button', { name: 'Reset preferences and default' }));
    await user.click(within(page).getByRole('button', { name: 'Subagents' }));
    expect(within(page).getByRole('heading', { name: 'New local reviewer' })).toBeVisible();
    await user.click(within(page).getByRole('button', { name: 'Back to chat' })); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    await user.click(screen.getByRole('button', { name: 'New chat' })); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(/^Aster$/);
    page = await settings(user); expect(within(page).getByRole('heading', { name: 'New local reviewer' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' })); expect(screen.getByLabelText('Model preference')).toHaveValue('inherit');
  });

  it('reuses only manual dummy provider steps; closes inner then base without losing focus or retaining keys', async () => {
    const spies = ['fetch', 'XMLHttpRequest', 'WebSocket', 'FileReader', 'showOpenFilePicker', 'open'].map((name) => { const spy = vi.fn(); vi.stubGlobal(name, spy); return spy; });
    const user = start(); const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const settingsOpener = screen.getByRole('button', { name: 'Settings' });
    const exportChat = projects().getByRole('button', { name: 'Attention Export date range' });
    const newChat = screen.getByRole('button', { name: 'New chat' });
    const dialog = await settings(user);
    await user.click(within(dialog).getByRole('button', { name: 'Providers' }));
    expect(within(dialog).queryByRole('button', { name: 'Connect example' })).not.toBeInTheDocument();
    await user.click(within(dialog).getByRole('button', { name: 'Connection examples' }));
    const connect = within(dialog).getAllByRole('button', { name: 'Connect example' })[0]; await user.click(connect);
    let inner = screen.getByRole('dialog', { name: 'Connect Aster Lab' });
    await user.click(within(inner).getByRole('button', { name: /Dummy API key/ }));
    await user.type(screen.getByLabelText('Dummy API key'), 'AVA-DEMO-KEY');
    await user.keyboard('{Escape}'); expect(inner).not.toHaveTextContent('Continue with dummy key');
    await user.click(within(inner).getByRole('button', { name: /Dummy API key/ })); expect(screen.getByLabelText('Dummy API key')).toHaveValue('');
    await user.type(screen.getByLabelText('Dummy API key'), 'AVA-DEMO-KEY'); await user.click(screen.getByRole('button', { name: 'Continue with dummy key' }));
    expect(inner).toHaveTextContent('Waiting · supplied example');
    await user.click(within(inner).getByRole('button', { name: 'Show success example' })); await user.click(within(inner).getByRole('button', { name: 'Done' }));
    expect(connect).toHaveFocus(); expect(dialog).toHaveTextContent('Available example');
    await user.click(connect); inner = screen.getByRole('dialog', { name: 'Connect Aster Lab' });
    await user.click(within(inner).getByRole('button', { name: /Device code example/ })); expect(inner).toHaveTextContent('AVA-DEMO-2048');
    const ghostFocus = vi.spyOn(connect, 'focus');
    // Neither underlying page callbacks nor retained sidebar controls can leave a nested overlay.
    expectHiddenSidebar(sidebar);
    fireEvent.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    fireEvent.click(exportChat); fireEvent.click(newChat);
    expect(inner).toBeInTheDocument(); expect(screen.getByRole('region', { name: 'Settings page' })).toBe(dialog);
    expectHiddenSidebar(sidebar); expect(ghostFocus).not.toHaveBeenCalled();
    await user.click(within(inner).getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(connect).toHaveFocus();
    ghostFocus.mockClear();
    await user.click(within(dialog).getByRole('button', { name: 'Back to chat' }));
    expect(settingsOpener).toHaveFocus(); expect(settingsOpener).toBeVisible(); expect(sidebar).toBeVisible();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    await user.click(projects().getByRole('button', { name: 'Attention Export date range' }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range');
    expect(ghostFocus).not.toHaveBeenCalled();
    for (const spy of spies) expect(spy).not.toHaveBeenCalled();
  });
});
