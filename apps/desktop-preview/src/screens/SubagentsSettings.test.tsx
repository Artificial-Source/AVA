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

describe('user-owned subagent definition editor', () => {
  it('requires bounded unique names and descriptions, then creates a local model preference with no model dialog', async () => {
    const user = userEvent.setup(); render(<StrictMode><Host /></StrictMode>);
    await user.click(screen.getByRole('button', { name: 'New subagent' }));
    const name = screen.getByRole('textbox', { name: 'Name' }); expect(name).toHaveFocus(); expect(name).toHaveAttribute('maxlength', '64');
    const description = screen.getByRole('textbox', { name: 'Description' }); expect(description).toHaveAttribute('maxlength', '512');
    const preference = screen.getByRole('combobox', { name: 'Model preference' });
    expect(preference).toHaveValue('inherit'); expect(preference).toHaveAccessibleDescription(/backend has no per-definition model field/);
    expect(within(preference).getAllByRole('option').map((option) => option.textContent)).toEqual(['Inherit parent', 'Aster', 'Aster Mini', 'Orion']);
    await user.click(screen.getByRole('button', { name: 'Create definition' })); expect(screen.getAllByRole('alert')).toHaveLength(2);
    await user.type(name, 'GENERAL'); await user.type(description, 'A description.');
    await user.click(screen.getByRole('button', { name: 'Create definition' })); expect(screen.getByRole('alert')).toHaveTextContent('already exists');
    await user.clear(name); await user.type(name, '  Review drafts  '); await user.selectOptions(preference, 'aster-mini');
    await user.dblClick(screen.getByRole('button', { name: 'Create definition' }));
    expect(screen.queryByRole('form')).not.toBeInTheDocument(); expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'New subagent' })).toHaveFocus();
    const list = screen.getByRole('list', { name: 'Subagent definitions' }); expect(within(list).getAllByRole('listitem')).toHaveLength(5);
    const row = screen.getByRole('heading', { name: 'Review drafts' }).closest('li')!;
    expect(row).toHaveTextContent('A description.'); expect(row).toHaveTextContent('Model preference · Aster Mini');
    expect(screen.getByRole('status')).toHaveTextContent('Created Review drafts.');
  });
  it('saves edits, can restore inherit, cancels drafts, and never offers built-in/project edit controls', async () => {
    const user = userEvent.setup(); render(<Host />);
    expect(screen.getAllByRole('button', { name: /^Edit / })).toHaveLength(1);
    for (const label of ['general', 'explore', 'Project reviewer']) {
      const row = screen.getByRole('heading', { name: label }).closest('li')!;
      expect(row).toHaveTextContent('read-only'); expect(within(row).queryByRole('button')).not.toBeInTheDocument();
    }
    const edit = screen.getByRole('button', { name: 'Edit Notes reviewer' }); await user.click(edit);
    await user.clear(screen.getByRole('textbox', { name: 'Description' })); await user.type(screen.getByRole('textbox', { name: 'Description' }), 'Edited local summary.');
    await user.selectOptions(screen.getByLabelText('Model preference'), 'orion'); await user.click(screen.getByRole('button', { name: 'Save definition' }));
    expect(edit).toHaveFocus(); expect(screen.getByRole('status')).toHaveTextContent('Saved Notes reviewer.');
    await user.click(edit); expect(screen.getByRole('textbox', { name: 'Description' })).toHaveValue('Edited local summary.'); expect(screen.getByLabelText('Model preference')).toHaveValue('orion');
    await user.selectOptions(screen.getByLabelText('Model preference'), 'inherit'); await user.click(screen.getByRole('button', { name: 'Save definition' }));
    await user.click(edit); await user.clear(screen.getByRole('textbox', { name: 'Description' })); await user.click(screen.getByRole('button', { name: 'Cancel edit' }));
    await user.click(edit); expect(screen.getByRole('textbox', { name: 'Description' })).toHaveValue('Edited local summary.'); expect(screen.getByLabelText('Model preference')).toHaveValue('inherit');
  });
  it('disables stale/readonly revisions and inactive forms rather than retargeting a draft', async () => {
    const user = userEvent.setup(); const workspace = initialSubagentWorkspace(); const onSave = vi.fn();
    const view = render(<SubagentsSettings workspace={workspace} active onSave={onSave} />);
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' }));
    const form = screen.getByRole('form', { name: 'Edit subagent' });
    view.rerender(<SubagentsSettings workspace={workspace} active={false} onSave={onSave} />);
    fireEvent.submit(form); expect(onSave).not.toHaveBeenCalled(); expect(screen.getByRole('button', { name: 'Save definition' })).toBeDisabled();
    view.rerender(<SubagentsSettings workspace={{ ...workspace, version: 2 }} active onSave={onSave} />);
    expect(screen.getByRole('alert')).toHaveTextContent('changed'); fireEvent.submit(form); expect(onSave).not.toHaveBeenCalled();
    expect(screen.getByRole('button', { name: 'Save definition' })).toBeDisabled();
    await user.click(screen.getByRole('button', { name: 'Cancel edit' }));
    await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' }));
    view.rerender(<SubagentsSettings workspace={{ ...workspace, version: 2, definitions: workspace.definitions.map((entry) => entry.id === 'user:1' ? { ...entry, source: 'project' } : entry) }} active onSave={onSave} />);
    fireEvent.submit(screen.getByRole('form')); expect(onSave).not.toHaveBeenCalled(); expect(screen.getByRole('button', { name: 'Save definition' })).toBeDisabled();
    view.unmount(); act(() => fireEvent.submit(form)); expect(onSave).not.toHaveBeenCalled();
  });
});
