import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it } from 'vitest';
import { installDialogTestSupport } from './dialogTestSupport';
import { familyIds, inspectionErrorSections, reviewFamilies } from './fixtures';
import ScreenReview from './ScreenReview';
import { toolChangeCases } from '../catalog/examples';

beforeEach(installDialogTestSupport);
async function family(user: ReturnType<typeof userEvent.setup>, name: string) {
  await user.click(within(screen.getByRole('navigation', { name: 'Screen families' })).getByRole('button', { name: new RegExp(`^${name}`) }));
}
function scenarioSelect() { return within(screen.queryByRole('dialog') ?? screen.getByRole('main')).getByRole('combobox', { name: 'Review scenario' }); }
const sections = () => within(screen.getByRole('navigation', { name: 'Settings sections' }));
const chatTitle = () => screen.getByText('Chat title', { selector: 'dt' }).nextElementSibling;

describe('complete screen review host', () => {
  it('indexes the eleven settings pages, all 15 families and the expanded case matrix', () => {
    expect(familyIds).toHaveLength(15); expect(reviewFamilies.settings.cases).toHaveLength(23);
    expect(familyIds.reduce((count, id) => count + reviewFamilies[id].cases.length, 0)).toBe(128 + Object.keys(toolChangeCases).length);
    expect(reviewFamilies.settings.cases.slice(0, 11)).toEqual(['General', 'Appearance', 'Shortcuts', 'Notifications & sounds', 'Models & providers', 'Conversation behavior', 'Context management', 'Permissions & trust', 'Subagents', 'Extensions', 'About']);
  });
  it('routes each extension error to Extensions with a working Retry, and keeps every rules state', async () => {
    const user = userEvent.setup(); render(<ScreenReview />);
    const nouns = { skills: 'skills', prompts: 'prompts', plugins: 'plugins', mcp: 'MCP servers', lsp: 'language servers' } as const;
    for (const [scenario, area] of Object.entries(inspectionErrorSections)) {
      await user.selectOptions(scenarioSelect(), scenario);
      expect(screen.getByRole('heading', { level: 2, name: 'Extensions' })).toBeVisible();
      expect(screen.getByRole('alert')).toHaveTextContent(`Couldn’t load ${nouns[area]}.`);
    }
    await user.click(screen.getByRole('button', { name: 'Retry' }));
    expect(scenarioSelect()).toHaveValue('Extensions'); expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    for (const state of ['loading', 'empty', 'error', 'denied', 'stale'] as const) {
      await user.selectOptions(scenarioSelect(), `Rules / ${state}`);
      expect(screen.getByRole('heading', { level: 2, name: 'Permissions & trust' })).toBeVisible();
      // Errors interrupt as alerts; every other state is a polite status.
      const expected = { loading: 'Loading these rules…', empty: 'No saved rules or session grants.', error: 'Couldn’t load these rules.', denied: 'You don’t have access to these rules.', stale: 'These rules may be out of date.' }[state];
      expect(screen.getAllByRole(state === 'error' ? 'alert' : 'status').some((status) => status.textContent?.includes(expected))).toBe(true);
    }
  });
  it('owns an independent subagent workspace, keeps it across Back and preference reset, and clears it only on review reset', async () => {
    const user = userEvent.setup(); const host = render(<ScreenReview />);
    await user.click(sections().getByRole('button', { name: 'Subagents' }));
    await user.click(screen.getByRole('button', { name: 'New subagent' }));
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'Review drafts' } });
    fireEvent.change(screen.getByRole('textbox', { name: 'Description' }), { target: { value: 'Checks release notes before they ship.' } });
    await user.click(screen.getByRole('button', { name: 'Create subagent' }));
    await user.click(sections().getByRole('button', { name: 'General' }));
    await user.click(screen.getByRole('button', { name: 'Reset all preferences…' })); await user.click(screen.getByRole('button', { name: 'Reset preferences' }));
    await user.click(sections().getByRole('button', { name: 'Subagents' }));
    expect(screen.getByRole('heading', { name: 'Review drafts' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Back to review' }));
    await user.click(screen.getByRole('button', { name: 'Open settings' })); await user.click(sections().getByRole('button', { name: 'Subagents' }));
    expect(screen.getByRole('heading', { name: 'Review drafts' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Reset review' })); await user.click(within(screen.getByRole('dialog', { name: 'Reset the review?' })).getByRole('button', { name: 'Reset review' }));
    await user.click(sections().getByRole('button', { name: 'Subagents' }));
    expect(screen.queryByRole('heading', { name: 'Review drafts' })).not.toBeInTheDocument();
    host.unmount(); render(<ScreenReview />);
    await user.click(sections().getByRole('button', { name: 'Subagents' }));
    expect(screen.getAllByRole('button', { name: /^Edit / })).toHaveLength(1);
  });
  for (const id of familyIds) {
    it(`exposes every ${id} scenario with live UI and a way back`, async () => {
      const user = userEvent.setup(); render(<ScreenReview />); await family(user, reviewFamilies[id].label.replace(/[&?]/g, '.'));
      for (const scenario of reviewFamilies[id].cases) {
        // The directory intentionally closes the dialog. Other cases remain selectable inside the native modal.
        await user.selectOptions(scenarioSelect(), scenario);
        expect(scenarioSelect()).toHaveValue(scenario);
        if (screen.queryByRole('dialog')) expect(within(screen.getByRole('dialog')).getByRole('button', { name: 'Close' })).toBeEnabled();
        else expect(screen.getByRole('link', { name: 'Back to chat' })).toHaveAttribute('href', '?');
      }
    });
  }
  // A sweep of every case in eight families. It waits on no timers; cases switch synchronously
  // and the budget covers a loaded machine, since only its length depends on load.
  it('keeps contract-speak and fixture labels out of the chat action and request dialogs', async () => {
    const user = userEvent.setup(); render(<ScreenReview />);
    for (const id of ['rename', 'archive', 'branch', 'workspace', 'message', 'permission', 'question', 'run'] as const) {
      await family(user, reviewFamilies[id].label.replace(/[&?]/g, '.'));
      for (const scenario of reviewFamilies[id].cases) {
        fireEvent.change(scenarioSelect(), { target: { value: scenario } });
        const dialog = screen.getByRole('dialog');
        // The review toolbar (scenario picker and receipt) is harness chrome, not dialog copy.
        const toolbar = within(dialog).getByRole('combobox', { name: 'Review scenario' }).closest('label')!.parentElement!.parentElement!;
        expect(toolbar.querySelector('select')).toHaveValue(scenario);
        expect(dialog.textContent!.replace(toolbar.textContent!, '')).not.toMatch(/\bintent\b|RPC|\bexample\b|fixture|supplied|projection/i);
      }
      await user.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Close' }));
    }
  }, 20_000);
  it('renders all inline tool-diff cases inside the tool details dialog', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await family(user, 'Tool details');
    for (const scenario of Object.keys(toolChangeCases)) {
      await user.selectOptions(scenarioSelect(), scenario);
      const dialog = screen.getByRole('dialog', { name: 'Tool details' });
      expect(await within(dialog).findByLabelText('Output: Update note')).toHaveTextContent('Updated notes/filters.md.');
      if (scenario === 'diff-malformed' || scenario === 'diff-duplicate') expect(within(dialog).getAllByRole('status').some((status) => status.textContent?.includes('File changes unavailable'))).toBe(true);
      else if (scenario === 'diff-missing') expect(within(dialog).queryByRole('region', { name: /^File change:/ })).not.toBeInTheDocument();
      else expect(within(dialog).getByRole('region', { name: /^File change:/ })).toBeVisible();
    }
    await user.click(screen.getByRole('button', { name: 'Done' })); expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('resets a model draft and the finder scope without changing the chosen model', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await family(user, 'Model selector');
    expect(screen.getByRole('dialog', { name: 'Model for “Refine search filters”' })).toBeVisible();
    await user.click(screen.getByRole('radio', { name: /Orion.*96,000/ })); await user.type(screen.getByLabelText('Search models'), 'ori');
    await user.click(within(screen.getByRole('dialog')).getByText('Reset case'));
    expect(screen.getByLabelText('Search models')).toHaveValue(''); expect(screen.getAllByRole<HTMLInputElement>('radio').find((radio) => radio.value === 'aster')).toBeChecked();
    await user.click(screen.getByLabelText('Close')); await family(user, 'Find a chat');
    await user.click(screen.getByRole('button', { name: 'Archived' })); expect(screen.getByRole('button', { name: 'Archived' })).toHaveAttribute('aria-pressed', 'true');
    await user.click(within(screen.getByRole('dialog')).getByText('Reset case')); expect(screen.getByRole('button', { name: 'All' })).toHaveAttribute('aria-pressed', 'true');
  });
  it('changes only review models and restores the settings opener', async () => {
    const user = userEvent.setup(); render(<ScreenReview />);
    await user.click(sections().getByRole('button', { name: 'Models & providers' }));
    const opener = screen.getByRole('button', { name: 'Choose default model' }); await user.click(opener);
    expect(screen.getByRole('dialog', { name: 'Default model for new chats' })).toBeVisible();
    await user.click(screen.getByRole('radio', { name: /Orion.*96,000/ })); await user.selectOptions(screen.getByLabelText('Reasoning level'), 'High'); await user.click(screen.getByRole('button', { name: 'Set as default' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(opener).toHaveFocus(); expect(screen.getByText(/New chats start with Orion/)).toBeInTheDocument();
    await user.click(opener); expect(screen.getByLabelText('Reasoning level')).toHaveValue('High');
  });
  it('walks a provider key through failure and retry, clears the key, and Escape steps back before leaving', async () => {
    const key = 'ak-test-5f2c9e81b7d04a63';
    const user = userEvent.setup(); render(<ScreenReview />); await user.click(sections().getByRole('button', { name: 'Models & providers' }));
    await user.click(screen.getByRole('button', { name: 'Connect Aster Lab' })); await user.click(screen.getByRole('button', { name: /^API key/ }));
    await user.type(screen.getByLabelText('API key'), key); fireEvent.keyDown(screen.getByLabelText('API key'), { key: 'Escape' }); expect(screen.queryByLabelText('API key')).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: /^API key/ })); expect(screen.getByLabelText('API key')).toHaveValue(''); await user.type(screen.getByLabelText('API key'), key); await user.click(screen.getByRole('button', { name: 'Verify key' }));
    expect(screen.getByText('Verifying key…')).toBeInTheDocument(); expect(screen.queryByLabelText('API key')).not.toBeInTheDocument();
    // The review holds each step; its scenario picker supplies the failure, which keeps the key method.
    await user.selectOptions(scenarioSelect(), 'error'); await user.click(screen.getByRole('button', { name: 'Try again' })); expect(screen.getByLabelText('API key')).toHaveValue('');
    await user.click(screen.getByLabelText('Close')); expect(screen.getByRole('heading', { name: 'Settings' })).toBeInTheDocument(); expect(screen.queryByText(key)).not.toBeInTheDocument();
    expect(screen.getByText(/^Last action:|^Closed\./)).toHaveTextContent('Closed. The key field was cleared.');
  });
  it('hands a new connection straight to the model picker', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await family(user, 'Providers . connect');
    await user.selectOptions(scenarioSelect(), 'success');
    await user.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Choose a model' }));
    expect(screen.getByRole('dialog', { name: 'Model for “Refine search filters”' })).toBeVisible();
  });
  it('searches settings, changes working preferences and resets them with confirmation', async () => {
    const user = userEvent.setup(); render(<ScreenReview />);
    await user.type(screen.getByLabelText('Search settings'), 'not a section'); expect(screen.getByText('No settings match “not a section”.')).toBeInTheDocument();
    fireEvent.keyDown(screen.getByLabelText('Search settings'), { key: 'Escape', isComposing: true }); expect(screen.getByLabelText('Search settings')).toHaveValue('not a section');
    fireEvent.compositionStart(screen.getByLabelText('Search settings')); fireEvent.keyDown(screen.getByLabelText('Search settings'), { key: 'Escape' }); expect(screen.getByLabelText('Search settings')).toHaveValue('not a section');
    fireEvent.compositionEnd(screen.getByLabelText('Search settings')); fireEvent.keyDown(screen.getByLabelText('Search settings'), { key: 'Escape' }); expect(screen.getByLabelText('Search settings')).toHaveValue('');
    await user.selectOptions(screen.getByLabelText('Enter key behavior'), 'newline');
    expect(screen.getByLabelText('Enter key behavior')).toHaveValue('newline');
    await user.click(sections().getByRole('button', { name: 'Appearance' }));
    await user.selectOptions(screen.getByLabelText('Settings-row density'), 'compact');
    await user.click(sections().getByRole('button', { name: 'General' }));
    const reset = screen.getByRole('button', { name: 'Reset all preferences…' }); await user.click(reset);
    const confirm = screen.getByRole('dialog', { name: 'Reset preferences?' });
    expect(within(confirm).getByText('General', { selector: 'dt' }).nextElementSibling).toHaveTextContent('Enter key');
    expect(within(confirm).getByText('Appearance', { selector: 'dt' }).nextElementSibling).toHaveTextContent('Row density');
    expect(within(confirm).getByText('Models & providers', { selector: 'dt' }).nextElementSibling).toHaveTextContent('New chats use Aster Mini');
    await user.click(within(confirm).getByRole('button', { name: 'Cancel' })); expect(reset).toHaveFocus();
    expect(screen.getByLabelText('Enter key behavior')).toHaveValue('newline');
    await user.click(sections().getByRole('button', { name: 'Appearance' }));
    expect(screen.getByLabelText('Settings-row density')).toHaveValue('compact');
    await user.click(sections().getByRole('button', { name: 'General' }));
    await user.click(screen.getByRole('button', { name: 'Reset all preferences…' })); await user.click(screen.getByRole('button', { name: 'Reset preferences' }));
    expect(screen.getByLabelText('Enter key behavior')).toHaveValue('send');
    expect(screen.getByRole('button', { name: 'Reset all preferences…' })).toBeDisabled();
    await user.click(sections().getByRole('button', { name: 'Appearance' }));
    expect(screen.getByLabelText('Settings-row density')).toHaveValue('comfortable');
  });
  it('routes known commands, renames, and archives or branches only from their confirm buttons', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await family(user, 'Command palette');
    await user.click(screen.getByRole('option', { name: 'Choose model' })); expect(screen.getByRole('dialog', { name: 'Model for “Refine search filters”' })).toBeInTheDocument();
    await user.click(screen.getByLabelText('Close')); await family(user, 'Rename chat');
    const field = screen.getByLabelText<HTMLInputElement>('Chat title'); expect(field).toHaveFocus(); expect(field.selectionEnd).toBe('Refine search filters'.length);
    await user.clear(field); await user.type(field, 'Reviewed title'); await user.click(screen.getByRole('button', { name: 'Rename' }));
    expect(chatTitle()).toHaveTextContent('Reviewed title'); expect(screen.getByText(/^Last action:/)).toHaveTextContent('chat renamed');
    await family(user, 'Archive chat');
    expect(screen.getByRole('dialog', { name: 'Archive chat?' })).toHaveAccessibleDescription('Reviewed title');
    await user.click(screen.getByRole('button', { name: 'Archive chat' })); expect(screen.getByText(/^Last action:/)).toHaveTextContent('chat archived'); expect(chatTitle()).toHaveTextContent('Reviewed title');
    await family(user, 'Branch from a message');
    expect(screen.getAllByRole<HTMLInputElement>('radio')[0]).toBeChecked(); await user.keyboard('{Enter}'); expect(screen.getByRole('dialog', { name: 'Branch from a message' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Create branch' })); expect(screen.getByText(/^Last action:/)).toHaveTextContent('branch created');
  });
  it('opens Help on its shortcuts from the palette and closes the project picker when the open project is chosen', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await family(user, 'Command palette');
    expect(screen.getByRole('option', { name: 'Help' })).toHaveTextContent('F1');
    await user.click(screen.getByRole('option', { name: 'Keyboard shortcuts' }));
    const help = screen.getByRole('dialog', { name: 'Help' });
    expect(within(help).getByRole('heading', { name: 'Keyboard shortcuts', level: 3 })).toHaveFocus();
    expect(scenarioSelect()).toHaveValue('shortcuts');
    await user.click(within(help).getByRole('button', { name: 'Change shortcuts' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(sections().getByRole('button', { name: 'Shortcuts' })).toHaveAttribute('aria-current', 'page');
    await family(user, 'Open a project');
    await user.click(within(screen.getByRole('list', { name: 'Projects' })).getByRole('button', { name: /^Atlas/ }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(screen.getByText(/^Last action:/)).toHaveTextContent('stayed in Atlas');
  });
  it('shows Help with the current keys, opened on a topic, with no match, and with Getting started to do or done', async () => {
    const user = userEvent.setup(); render(<ScreenReview />);
    await user.click(sections().getByRole('button', { name: 'Shortcuts' }));
    await user.click(screen.getByRole('button', { name: 'Unassign New chat' }));
    await family(user, 'Help');
    let help = screen.getByRole('dialog', { name: 'Help' });
    expect(within(help).getByRole('searchbox', { name: 'Search help' })).toHaveFocus();
    expect(within(within(help).getByRole('table', { name: 'General' })).getByRole('rowheader', { name: 'New chat' }).closest('tr')).toHaveTextContent('Unassigned');
    expect(within(help).getByText('To do')).toBeVisible();
    await user.selectOptions(scenarioSelect(), 'topic');
    help = screen.getByRole('dialog', { name: 'Help' });
    expect(within(help).getByRole('button', { name: 'Permissions' })).toHaveAttribute('aria-expanded', 'true');
    expect(within(help).getByRole('button', { name: 'Permissions' })).toHaveFocus();
    await user.click(within(help).getByRole('button', { name: 'Go to Permissions & trust' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(sections().getByRole('button', { name: 'Permissions & trust' })).toHaveAttribute('aria-current', 'page');
    await family(user, 'Help'); await user.selectOptions(scenarioSelect(), 'no-match');
    help = screen.getByRole('dialog', { name: 'Help' });
    expect(within(help).getByText('No topics or shortcuts match “no matching chat”.')).toBeVisible();
    expect(within(help).queryByRole('heading', { level: 3 })).not.toBeInTheDocument();
    await user.selectOptions(scenarioSelect(), 'all-done');
    expect(within(screen.getByRole('dialog', { name: 'Help' })).getByText('You’re set up.')).toBeVisible();
    await user.selectOptions(scenarioSelect(), 'all-to-do');
    help = screen.getByRole('dialog', { name: 'Help' });
    expect(within(help).getAllByText('To do')).toHaveLength(3);
    await user.click(within(help).getByRole('button', { name: 'Open a project' }));
    expect(screen.getByRole('dialog', { name: 'Open a project' })).toBeVisible();
  });
  it('reloads a question that failed to load from its own Retry', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await family(user, 'Question');
    await user.selectOptions(scenarioSelect(), 'error');
    expect(within(screen.getByRole('dialog')).queryByRole('radio')).not.toBeInTheDocument();
    await user.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Retry' }));
    expect(scenarioSelect()).toHaveValue('single'); expect(within(screen.getByRole('dialog')).getAllByRole('radio')).toHaveLength(3);
    expect(screen.getByText(/^Last action:/)).toHaveTextContent('question reloaded');
  });
  it('resets the whole host without retaining secret values or receipts', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await family(user, 'Question'); await user.selectOptions(scenarioSelect(), 'secret'); await user.type(screen.getByLabelText('Access token'), 'demo-ephemeral');
    await user.click(screen.getByRole('button', { name: 'Answer later' })); expect(screen.getByText(/^Closed\./)).toHaveTextContent('The question is still unanswered.'); expect(screen.queryByText('demo-ephemeral')).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Reset review' })); await user.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Reset review' }));
    expect(screen.getByRole('heading', { name: 'Settings' })).toBeInTheDocument(); expect(screen.getByText('No actions yet.')).toBeInTheDocument();
  });
});
