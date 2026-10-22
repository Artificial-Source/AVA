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

describe('complete screen review host', () => {
  it('indexes 16 settings leaves, all 14 families and the expanded case matrix', () => {
    expect(familyIds).toHaveLength(14); expect(reviewFamilies.settings.cases).toHaveLength(28);
    expect(familyIds.reduce((count, id) => count + reviewFamilies[id].cases.length, 0)).toBe(126 + Object.keys(toolChangeCases).length);
    expect(reviewFamilies.settings.cases.slice(0, 16)).toEqual(['General', 'Appearance', 'Models', 'Providers', 'Shortcuts', 'Conversation behavior', 'Notifications & sounds', 'Context management', 'Permissions & trust', 'Subagents', 'Skills', 'Prompts', 'Plugins', 'MCP', 'LSP', 'About']);
  });
  it('routes each inspection error to its specific leaf and retains every policy-state fixture', async () => {
    const user = userEvent.setup(); render(<ScreenReview />);
    for (const [scenario, section] of Object.entries(inspectionErrorSections)) {
      await user.selectOptions(scenarioSelect(), scenario);
      expect(screen.getByRole('heading', { level: 2, name: section === 'mcp' || section === 'lsp' ? section.toUpperCase() : section[0].toUpperCase() + section.slice(1) })).toBeVisible();
      expect(screen.getByText('The example could not be loaded. A revised view is required.')).toBeVisible();
      expect(screen.queryByRole('article', { name: 'Workspace notes' })).not.toBeInTheDocument();
    }
    for (const state of ['loading', 'empty', 'error', 'denied', 'stale']) {
      await user.selectOptions(scenarioSelect(), `Rules / ${state}`);
      expect(screen.getByRole('heading', { level: 2, name: 'Permissions & trust' })).toBeVisible();
      expect(screen.getAllByRole('status').some((status) => status.textContent?.includes(state === 'denied' ? 'Access denied' : state === 'empty' ? 'No entries' : state === 'error' ? 'could not be loaded' : state === 'loading' ? 'Loading example' : 'stale'))).toBe(true);
    }
  });
  it('owns an independent definition workspace, retains it across Back/preference reset and clears it only on explicit review reset', async () => {
    const user = userEvent.setup(); const host = render(<ScreenReview />);
    await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'Subagents' }));
    await user.click(screen.getByRole('button', { name: 'New subagent' }));
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'Review-only definition' } });
    fireEvent.change(screen.getByRole('textbox', { name: 'Description' }), { target: { value: 'Independent local metadata.' } });
    await user.click(screen.getByRole('button', { name: 'Create definition' }));
    await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'General' }));
    await user.click(screen.getByRole('button', { name: 'Reset all preferences…' })); await user.click(screen.getByRole('button', { name: 'Reset local examples' }));
    await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'Subagents' }));
    expect(screen.getByRole('heading', { name: 'Review-only definition' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Back' })); await user.click(screen.getByRole('button', { name: 'Back' }));
    await user.click(screen.getByRole('button', { name: 'Open settings' })); await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'Subagents' }));
    expect(screen.getByRole('heading', { name: 'Review-only definition' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Reset review' })); await user.click(screen.getByRole('button', { name: 'Reset local examples' }));
    await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'Subagents' }));
    expect(screen.queryByRole('heading', { name: 'Review-only definition' })).not.toBeInTheDocument();
    host.unmount(); render(<ScreenReview />);
    await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'Subagents' }));
    expect(screen.getAllByRole('button', { name: /^Edit / })).toHaveLength(1);
  });
  for (const id of familyIds) {
    it(`exposes every ${id} scenario with live UI and a way back`, async () => {
      const user = userEvent.setup(); render(<ScreenReview />); await family(user, reviewFamilies[id].label.replace(/[&?]/g, '.'));
      for (const scenario of reviewFamilies[id].cases) {
        // The directory intentionally closes the dialog. Other cases remain selectable inside the native modal.
        await user.selectOptions(scenarioSelect(), scenario);
        expect(scenarioSelect()).toHaveValue(scenario);
        if (screen.queryByRole('dialog')) expect(within(screen.getByRole('dialog')).getByRole('button', { name: 'Close dialog' })).toBeEnabled();
        else expect(screen.getByRole('link', { name: 'Back to chat' })).toHaveAttribute('href', '?');
        expect(screen.queryByText('Loading local screen examples…')).not.toBeInTheDocument();
      }
    });
  }
  it('renders all added inline tool-diff cases inside the existing read-only output dialog', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await family(user, 'Tool output');
    for (const scenario of Object.keys(toolChangeCases)) {
      await user.selectOptions(scenarioSelect(), scenario);
      const dialog = screen.getByRole('dialog', { name: 'Tool output detail' });
      expect(await within(dialog).findByLabelText('Output: Update note')).toHaveTextContent('Supplied tool output remains readable.');
      expect(dialog).toHaveAccessibleDescription(/No private provider payload or reasoning/);
      if (scenario === 'diff-malformed' || scenario === 'diff-duplicate') expect(within(dialog).getAllByRole('status').some((status) => status.textContent?.includes('File changes unavailable'))).toBe(true);
      else if (scenario === 'diff-missing') expect(within(dialog).queryByRole('region', { name: /^File change:/ })).not.toBeInTheDocument();
      else expect(within(dialog).getByRole('region', { name: /^File change:/ })).toBeVisible();
    }
    await user.click(screen.getByRole('button', { name: 'Close detail' })); expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('resets a model draft and finder project filter without changing the chosen host model', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await family(user, 'Model selector');
    await user.click(screen.getByRole('radio', { name: /Orion.*96,000/ })); await user.type(screen.getByLabelText('Search models'), 'ori');
    await user.click(within(screen.getByRole('dialog')).getByText('Reset case'));
    expect(screen.getByLabelText('Search models')).toHaveValue(''); expect(screen.getAllByRole<HTMLInputElement>('radio').find((radio) => radio.value === 'aster')).toBeChecked();
    await user.click(screen.getByLabelText('Close dialog')); await family(user, 'Session finder'); await user.selectOptions(screen.getByLabelText('Project filter'), 'notebook');
    await user.click(within(screen.getByRole('dialog')).getByText('Reset case')); expect(screen.getByLabelText('Project filter')).toHaveValue('all');
  });
  it('changes only review models, distinguishes defaults, and restores the settings opener', async () => {
    const user = userEvent.setup(); render(<ScreenReview />);
    await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByText('Models'));
    const opener = screen.getByText('Choose default model'); await user.click(opener);
    expect(screen.getByText('New chat default')).toBeInTheDocument();
    await user.click(screen.getByRole('radio', { name: /Orion.*96,000/ })); await user.selectOptions(screen.getByLabelText('Reasoning level'), 'High'); await user.click(screen.getByText('Use Orion'));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(opener).toHaveFocus(); expect(screen.getByText(/Default: Orion/)).toBeInTheDocument(); expect(screen.getByText('Selected: Aster')).toBeInTheDocument();
    await user.click(opener); expect(screen.getByLabelText('Reasoning level')).toHaveValue('High');
  });
  it('rehearses provider methods and manual outcomes, clears secrets, contextual Escape leaves settings open', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByText('Providers'));
    await user.click(screen.getByRole('button', { name: 'Connection examples' }));
    await user.click(screen.getAllByText('Connect example')[0]); await user.click(screen.getByText('Dummy API key'));
    await user.type(screen.getByLabelText('Dummy API key'), 'AVA-DEMO-KEY'); fireEvent.keyDown(screen.getByLabelText('Dummy API key'), { key: 'Escape' }); expect(screen.queryByLabelText('Dummy API key')).not.toBeInTheDocument();
    await user.click(screen.getByText('Dummy API key')); expect(screen.getByLabelText('Dummy API key')).toHaveValue(''); await user.type(screen.getByLabelText('Dummy API key'), 'AVA-DEMO-KEY'); await user.click(screen.getByText('Continue with dummy key'));
    expect(screen.getByText('Waiting · supplied example')).toBeInTheDocument(); expect(screen.queryByLabelText('Dummy API key')).not.toBeInTheDocument();
    await user.click(screen.getByText('Show error example')); await user.click(screen.getByText('Retry example')); expect(screen.getByLabelText('Dummy API key')).toHaveValue('');
    await user.click(screen.getByLabelText('Close dialog')); expect(screen.getByRole('heading', { name: 'Settings' })).toBeInTheDocument(); expect(screen.queryByText('AVA-DEMO-KEY')).not.toBeInTheDocument();
  });
  it('searches settings, changes working preferences and resets them with confirmation', async () => {
    const user = userEvent.setup(); render(<ScreenReview />);
    await user.type(screen.getByLabelText('Search settings'), 'not a section'); expect(screen.getByText(/No matching settings/)).toBeInTheDocument();
    fireEvent.keyDown(screen.getByLabelText('Search settings'), { key: 'Escape', isComposing: true }); expect(screen.getByLabelText('Search settings')).toHaveValue('not a section');
    fireEvent.compositionStart(screen.getByLabelText('Search settings')); fireEvent.keyDown(screen.getByLabelText('Search settings'), { key: 'Escape' }); expect(screen.getByLabelText('Search settings')).toHaveValue('not a section');
    fireEvent.compositionEnd(screen.getByLabelText('Search settings')); fireEvent.keyDown(screen.getByLabelText('Search settings'), { key: 'Escape' }); expect(screen.getByLabelText('Search settings')).toHaveValue('');
    await user.selectOptions(screen.getByLabelText('Enter key behavior'), 'newline');
    expect(screen.getByLabelText('Enter key behavior')).toHaveValue('newline');
    await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByText('Appearance'));
    await user.selectOptions(screen.getByLabelText('Settings-row density'), 'compact');
    await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'General' }));
    const reset = screen.getByRole('button', { name: 'Reset all preferences…' }); await user.click(reset);
    const confirm = screen.getByRole('dialog', { name: 'Reset review preferences?' });
    expect(confirm).toHaveAccessibleDescription('Only local review values are reset. Main-chat state and real configuration are untouched.');
    await user.click(within(confirm).getByRole('button', { name: 'Cancel' })); expect(reset).toHaveFocus();
    expect(screen.getByLabelText('Enter key behavior')).toHaveValue('newline');
    await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'Appearance' }));
    expect(screen.getByLabelText('Settings-row density')).toHaveValue('compact');
    await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'General' }));
    await user.click(screen.getByRole('button', { name: 'Reset all preferences…' })); await user.click(screen.getByRole('button', { name: 'Reset local examples' }));
    expect(screen.getByLabelText('Enter key behavior')).toHaveValue('send');
    await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'Appearance' }));
    expect(screen.getByLabelText('Settings-row density')).toHaveValue('comfortable');
  });
  it('routes known commands, validates rename and keeps originals after archive/branch intent', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await family(user, 'Command palette'); await user.click(screen.getByText('Choose model')); expect(screen.getByRole('dialog', { name: 'Choose a model' })).toBeInTheDocument();
    await user.click(screen.getByLabelText('Close dialog')); await family(user, 'Rename chat'); await user.clear(screen.getByLabelText('Chat title')); await user.type(screen.getByLabelText('Chat title'), 'Reviewed title'); await user.click(screen.getByText('Rename example'));
    expect(screen.getByText('Reviewed title')).toBeInTheDocument(); await family(user, 'Archive preview'); await user.click(screen.getByText('Record archive intent')); expect(screen.getByText('Reviewed title')).toBeInTheDocument(); expect(screen.getByRole('status')).toHaveTextContent('original preserved');
    await family(user, 'Branch preview'); await user.click(screen.getByText('Record branch intent')); expect(screen.getByText('Reviewed title')).toBeInTheDocument();
  });
  it('resets the whole host without retaining secret values or mutation receipts', async () => {
    const user = userEvent.setup(); render(<ScreenReview />); await family(user, 'Question'); await user.selectOptions(scenarioSelect(), 'secret'); await user.type(screen.getByLabelText('Dummy secret answer'), 'demo-ephemeral');
    await user.click(screen.getByText('Dismiss unresolved')); expect(screen.getByRole('status')).toHaveTextContent('unresolved'); expect(screen.queryByText('demo-ephemeral')).not.toBeInTheDocument();
    await user.click(screen.getByText('Reset review')); await user.click(screen.getByText('Reset local examples')); expect(screen.getByRole('heading', { name: 'Settings' })).toBeInTheDocument(); expect(screen.getByRole('status')).toHaveTextContent('No local intent');
  });
});
