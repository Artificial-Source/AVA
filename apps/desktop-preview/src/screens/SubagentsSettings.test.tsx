import { StrictMode } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import { SubagentsSettings } from './SubagentsSettings';
import { initialSubagentWorkspace, useSubagentWorkspace } from './subagentWorkspace';

function Host({ active = true }: { active?: boolean }) {
  const definitions = useSubagentWorkspace(undefined, active);
  return <SubagentsSettings workspace={definitions.workspace} active={active} onSave={definitions.save} />;
}
const row = (name: string) => screen.getByRole('heading', { name }).closest('li')!;

describe('user-owned subagent definition editor', () => {
  it('validates in the error tone, focuses the first invalid field, then creates a subagent with a visible model caveat', async () => {
    const user = userEvent.setup(); render(<StrictMode><Host /></StrictMode>);
    await user.click(screen.getByRole('button', { name: 'New subagent' }));
    const name = screen.getByRole('textbox', { name: 'Name' }); expect(name).toHaveFocus(); expect(name).toHaveAttribute('maxlength', '64');
    const description = screen.getByRole('textbox', { name: 'Description' }); expect(description).toHaveAttribute('maxlength', '512');
    const preference = screen.getByRole('combobox', { name: 'Model preference' });
    expect(preference).toHaveValue('inherit'); expect(preference).toHaveAccessibleDescription('Saved with this subagent. Subagents currently run on the parent chat’s model.');
    expect(screen.getByText('Saved with this subagent. Subagents currently run on the parent chat’s model.')).toBeVisible();
    expect(within(preference).getAllByRole('option').map((option) => option.textContent)).toEqual(['Same as parent chat', 'Aster', 'Aster Mini', 'Orion']);
    await user.click(screen.getByRole('button', { name: 'Create subagent' })); expect(screen.getAllByRole('alert')).toHaveLength(2);
    expect(name).toHaveFocus(); expect(name).toHaveAttribute('aria-invalid', 'true');
    await user.type(name, 'GENERAL'); await user.click(screen.getByRole('button', { name: 'Create subagent' }));
    expect(name).toHaveFocus(); expect(name).toHaveAccessibleDescription(/already exists/);
    await user.clear(name); await user.type(name, '  Review drafts  '); await user.click(screen.getByRole('button', { name: 'Create subagent' }));
    expect(description).toHaveFocus(); expect(description).toHaveAttribute('aria-invalid', 'true');
    await user.type(description, 'A description.'); await user.selectOptions(preference, 'aster-mini');
    await user.dblClick(screen.getByRole('button', { name: 'Create subagent' }));
    expect(screen.queryByRole('form')).not.toBeInTheDocument(); expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'New subagent' })).toHaveFocus();
    const list = screen.getByRole('list', { name: 'Subagent definitions' }); expect(within(list).getAllByRole('listitem')).toHaveLength(5);
    expect(row('Review drafts')).toHaveTextContent('A description.'); expect(row('Review drafts')).toHaveTextContent('Model preference · Aster Mini');
    expect(screen.getByRole('status')).toHaveTextContent('Created Review drafts.');
  });
  it('edits inline at the item, returns focus to its Edit button, cancels with Escape and never offers built-in/project controls', async () => {
    const user = userEvent.setup(); render(<Host />);
    expect(screen.getAllByRole('button', { name: /^Edit / })).toHaveLength(1); expect(screen.getAllByRole('button', { name: /^Delete / })).toHaveLength(1);
    for (const label of ['general', 'explore', 'Project reviewer']) { expect(row(label)).toHaveTextContent('read-only'); expect(within(row(label)).queryByRole('button')).not.toBeInTheDocument(); }
    const item = row('Notes reviewer');
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' }));
    const form = screen.getByRole('form', { name: 'Edit Notes reviewer' }); expect(item).toContainElement(form);
    await user.clear(screen.getByRole('textbox', { name: 'Description' })); await user.type(screen.getByRole('textbox', { name: 'Description' }), 'Edited local summary.');
    await user.selectOptions(screen.getByLabelText('Model preference'), 'orion'); await user.click(screen.getByRole('button', { name: 'Save changes' }));
    expect(screen.getByRole('button', { name: 'Edit Notes reviewer' })).toHaveFocus(); expect(screen.getByRole('status')).toHaveTextContent('Saved Notes reviewer.');
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' })); expect(screen.getByRole('textbox', { name: 'Description' })).toHaveValue('Edited local summary.'); expect(screen.getByLabelText('Model preference')).toHaveValue('orion');
    await user.selectOptions(screen.getByLabelText('Model preference'), 'inherit'); await user.click(screen.getByRole('button', { name: 'Save changes' }));
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' })); await user.clear(screen.getByRole('textbox', { name: 'Description' })); await user.keyboard('{Escape}');
    expect(screen.queryByRole('form')).not.toBeInTheDocument(); expect(screen.getByRole('button', { name: 'Edit Notes reviewer' })).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' })); expect(screen.getByRole('textbox', { name: 'Description' })).toHaveValue('Edited local summary.'); expect(screen.getByLabelText('Model preference')).toHaveValue('inherit');
    await user.click(screen.getByRole('button', { name: 'Cancel' })); expect(screen.getByRole('button', { name: 'Edit Notes reviewer' })).toHaveFocus();
  });
  it('deletes a user subagent only after an inline confirmation', async () => {
    const user = userEvent.setup(); render(<Host />);
    await user.click(screen.getByRole('button', { name: 'Delete Notes reviewer' }));
    const confirm = screen.getByRole('group', { name: 'Delete Notes reviewer?' }); expect(within(confirm).getByRole('button', { name: 'Cancel' })).toHaveFocus();
    await user.keyboard('{Escape}'); expect(confirm).not.toBeInTheDocument(); expect(screen.getByRole('button', { name: 'Delete Notes reviewer' })).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Delete Notes reviewer' })); await user.click(screen.getByRole('button', { name: 'Delete subagent' }));
    expect(screen.queryByRole('heading', { name: 'Notes reviewer' })).not.toBeInTheDocument(); expect(screen.getByRole('status')).toHaveTextContent('Deleted Notes reviewer.');
    expect(screen.getByRole('button', { name: 'New subagent' })).toHaveFocus(); expect(within(screen.getByRole('list', { name: 'Subagent definitions' })).getAllByRole('listitem')).toHaveLength(3);
  });
  it('disables stale/readonly revisions and inactive forms rather than retargeting a draft', async () => {
    const user = userEvent.setup(); const workspace = initialSubagentWorkspace(); const onSave = vi.fn();
    const view = render(<SubagentsSettings workspace={workspace} active onSave={onSave} />);
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' }));
    const form = screen.getByRole('form', { name: 'Edit Notes reviewer' });
    view.rerender(<SubagentsSettings workspace={workspace} active={false} onSave={onSave} />);
    fireEvent.submit(form); expect(onSave).not.toHaveBeenCalled(); expect(screen.getByRole('button', { name: 'Save changes' })).toBeDisabled();
    view.rerender(<SubagentsSettings workspace={{ ...workspace, version: 2 }} active onSave={onSave} />);
    expect(screen.getByRole('alert')).toHaveTextContent('changed'); fireEvent.submit(form); expect(onSave).not.toHaveBeenCalled();
    expect(screen.getByRole('button', { name: 'Save changes' })).toBeDisabled();
    await user.click(screen.getByRole('button', { name: 'Cancel' }));
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' }));
    view.rerender(<SubagentsSettings workspace={{ ...workspace, version: 2, definitions: workspace.definitions.map((entry) => entry.id === 'user:1' ? { ...entry, source: 'project' } : entry) }} active onSave={onSave} />);
    fireEvent.submit(screen.getByRole('form')); expect(onSave).not.toHaveBeenCalled(); expect(screen.getByRole('button', { name: 'Save changes' })).toBeDisabled();
    view.unmount(); act(() => fireEvent.submit(form)); expect(onSave).not.toHaveBeenCalled();
  });
  it('reports a delete that lost a race and never removes it', async () => {
    const user = userEvent.setup(); const onSave = vi.fn(() => false);
    render(<SubagentsSettings workspace={initialSubagentWorkspace()} active onSave={onSave} />);
    await user.click(screen.getByRole('button', { name: 'Delete Notes reviewer' })); await user.click(screen.getByRole('button', { name: 'Delete subagent' }));
    expect(onSave).toHaveBeenCalledExactlyOnceWith({ remove: true, version: 1, definitionId: 'user:1', revision: 1 });
    expect(screen.getByRole('alert')).toHaveTextContent('This subagent changed.'); expect(screen.getByRole('button', { name: 'Delete subagent' })).toBeDisabled();
    expect(screen.getByRole('heading', { name: 'Notes reviewer' })).toBeVisible();
  });
});
