import { useState, type ComponentProps } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { ModelChoice, ModelPickerView, ModelSelection } from './contracts';
import { installDialogTestSupport } from './dialogTestSupport';
import { modelCase, modelChoices, providers } from './fixtures';
import { ModelDialog, forgetRecentModels, validModelCatalog, validModelSelection } from './ModelDialog';
import { resetProviderState, setProviderState } from './providerState';

beforeEach(() => { installDialogTestSupport(); forgetRecentModels(); resetProviderState(); });
const chrome = { onDismiss: vi.fn() };
const modelView: ModelPickerView = { id: 'models', version: 1, state: 'ready', scope: 'current-chat', currentId: 'aster', defaultId: 'aster-mini', choices: modelChoices };
const modelIntent: ModelSelection = { id: 'models', version: 1, modelId: 'aster', scope: 'current-chat', reasoning: 'Low' };
const summary = () => document.querySelector<HTMLElement>('[data-model-summary]')!;
const primary = () => screen.getByRole('button', { name: /^(Use for this chat|Set as default)$/ });

type Props = Partial<ComponentProps<typeof ModelDialog>>;
function Host({ initialQuery = '', ...props }: Props & { initialQuery?: string }) {
  const [query, setQuery] = useState(initialQuery);
  return <ModelDialog {...chrome} view={modelView} onSelect={vi.fn()} onRetry={vi.fn()} {...props} query={query} onQueryChange={setQuery} />;
}

describe('model picker', () => {
  it('names the scope, groups three profiles by family and provider, and commits the selected model with its reasoning', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn();
    render(<ModelDialog {...chrome} chatTitle="Refine search filters" view={modelView} query="" onQueryChange={vi.fn()} onSelect={onSelect} onRetry={vi.fn()} />);
    const dialog = screen.getByRole('dialog', { name: 'Model for “Refine search filters”' });
    expect(dialog).toHaveAttribute('data-wide', 'true'); expect(dialog).toHaveAccessibleDescription('Only this chat changes.');
    expect(screen.getByRole('searchbox', { name: 'Search models' })).toHaveFocus();
    const aster = screen.getByRole('group', { name: 'Aster family Aster Lab' });
    const orion = screen.getByRole('group', { name: 'Orion family Orion Lab' });
    expect(within(aster).getAllByRole('radio')).toHaveLength(2); expect(within(orion).getAllByRole('radio')).toHaveLength(1);
    expect(within(aster).getByRole('radio', { name: /Aster This chat.*214,000/ })).toBeChecked();
    expect(within(aster).getByRole('radio', { name: /Aster Mini New chat default.*128,000/ })).toBeEnabled();
    expect(new Set(screen.getAllByRole<HTMLInputElement>('radio').map((radio) => radio.name)).size).toBe(1);
    expect(summary()).toHaveTextContent('Aster · Aster Lab · 214,000 token window');
    await user.click(within(orion).getByRole('radio', { name: /Orion.*96,000/ })); expect(onSelect).not.toHaveBeenCalled();
    expect(summary()).toHaveTextContent('Orion · Orion Lab · 96,000 token window');
    await user.selectOptions(screen.getByLabelText('Reasoning level'), 'High'); await user.click(primary());
    expect(primary()).toHaveTextContent('Use for this chat');
    expect(onSelect).toHaveBeenCalledWith({ ...modelIntent, modelId: 'orion', reasoning: 'High' });
    expect(dialog).not.toHaveTextContent(/supplied|example|CURRENT|DEFAULT/);
  });
  it('names the default scope and starts on the new-chat default', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn();
    render(<ModelDialog {...chrome} view={{ ...modelView, scope: 'new-chat-default' }} query="" onQueryChange={vi.fn()} onSelect={onSelect} onRetry={vi.fn()} />);
    expect(screen.getByRole('dialog', { name: 'Default model for new chats' })).toHaveAccessibleDescription('Existing chats keep their model.');
    expect(screen.getByRole('radio', { name: /Aster Mini New chat default/ })).toBeChecked();
    await user.click(screen.getByRole('button', { name: 'Set as default' }));
    expect(onSelect).toHaveBeenCalledWith({ ...modelIntent, modelId: 'aster-mini', scope: 'new-chat-default', reasoning: null });
  });
  it('keyboard: typing “ori” highlights Orion and Enter selects it', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn(); render(<Host onSelect={onSelect} />);
    const search = screen.getByRole('searchbox', { name: 'Search models' }); expect(search).toHaveFocus();
    await user.keyboard('ori');
    expect(screen.getAllByRole('radio')).toHaveLength(1); expect(screen.getByRole('radio', { name: /Orion/ })).toBeChecked();
    expect(summary()).toHaveTextContent('Orion'); expect(primary()).toBeEnabled();
    await user.keyboard('{Enter}');
    expect(onSelect).toHaveBeenCalledExactlyOnceWith({ ...modelIntent, modelId: 'orion' });
  });
  it('moves the selection with arrow keys from the search field, wrapping, without leaving it or committing', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn(); render(<Host onSelect={onSelect} />);
    const search = screen.getByRole('searchbox', { name: 'Search models' });
    const checked = () => screen.getAllByRole<HTMLInputElement>('radio').find((radio) => radio.checked)?.value;
    await user.keyboard('{ArrowDown}'); expect(checked()).toBe('aster-mini');
    await user.keyboard('{ArrowDown}'); expect(checked()).toBe('orion');
    await user.keyboard('{ArrowDown}'); expect(checked()).toBe('aster');
    await user.keyboard('{ArrowUp}'); expect(checked()).toBe('orion');
    expect(search).toHaveFocus(); expect(onSelect).not.toHaveBeenCalled();
  });
  it('keeps native radio keyboard navigation across family groups without committing', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn();
    render(<ModelDialog {...chrome} view={modelView} query="" onQueryChange={vi.fn()} onSelect={onSelect} onRetry={vi.fn()} />);
    const radios = screen.getAllByRole<HTMLInputElement>('radio'); radios[0].focus();
    await user.keyboard('{ArrowRight}'); expect(radios[1]).toHaveFocus(); expect(radios[1]).toBeChecked();
    await user.keyboard('{ArrowRight}'); expect(radios[2]).toHaveFocus(); expect(radios[2]).toBeChecked();
    expect(summary()).toHaveTextContent('Orion'); expect(onSelect).not.toHaveBeenCalled();
  });
  it('never offers a model the filter hides; no match disables the primary and offers Clear search', async () => {
    const user = userEvent.setup(); const props = { ...chrome, view: modelView, onQueryChange: vi.fn(), onSelect: vi.fn(), onRetry: vi.fn() };
    const result = render(<ModelDialog {...props} query="" />);
    expect(primary()).toBeEnabled();
    result.rerender(<ModelDialog {...props} query="orion" />);
    expect(screen.queryByRole('radio', { name: /Aster/ })).not.toBeInTheDocument();
    expect(primary()).toBeDisabled(); expect(summary()).toHaveTextContent('Nothing selected');
    result.unmount();
    render(<Host />);
    await user.keyboard('zzz');
    expect(screen.queryByRole('radio')).not.toBeInTheDocument(); expect(screen.getByRole('status')).toHaveTextContent('No models match “zzz”.');
    expect(primary()).toBeDisabled();
    await user.click(screen.getByRole('button', { name: 'Clear search' }));
    expect(screen.getByRole('searchbox', { name: 'Search models' })).toHaveValue(''); expect(screen.getByRole('searchbox', { name: 'Search models' })).toHaveFocus();
    expect(screen.getAllByRole('radio')).toHaveLength(3);
  });
  it('starts on the first match when opened with a query', () => {
    render(<Host initialQuery="Orion Lab" />);
    expect(screen.getByRole('radio', { name: /Orion/ })).toBeChecked(); expect(primary()).toBeEnabled();
  });
  it('lists this visit’s picks under Recent when browsing, not while searching', async () => {
    const user = userEvent.setup();
    const first = render(<Host />);
    await user.keyboard('ori{Enter}'); first.unmount();
    render(<Host />);
    const recent = screen.getByRole('group', { name: 'Recent' });
    expect(within(recent).getByRole('radio', { name: /Orion/ })).toBeInTheDocument();
    expect(screen.queryByRole('group', { name: 'Orion family Orion Lab' })).not.toBeInTheDocument();
    expect(screen.getAllByRole('radio')).toHaveLength(3);
    await user.keyboard('a');
    expect(screen.queryByRole('group', { name: 'Recent' })).not.toBeInTheDocument();
    expect(screen.getByRole('group', { name: 'Orion family Orion Lab' })).toBeInTheDocument();
  });
  it('dims unavailable models with their reason and offers to connect their provider', async () => {
    const user = userEvent.setup(); const onConnectProvider = vi.fn();
    render(<ModelDialog {...chrome} view={{ ...modelView, choices: modelCase('unavailable') }} query="" onQueryChange={vi.fn()} onSelect={vi.fn()} onRetry={vi.fn()} onConnectProvider={onConnectProvider} />);
    for (const radio of screen.getAllByRole('radio')) { expect(radio).toBeDisabled(); expect(radio.closest('[data-disabled]')).toHaveAttribute('data-disabled', 'true'); }
    expect(primary()).toBeDisabled();
    expect(screen.getAllByRole('button', { name: 'Connect Aster Lab' })).toHaveLength(2);
    await user.click(screen.getByRole('button', { name: 'Connect Orion Lab' })); expect(onConnectProvider).toHaveBeenLastCalledWith('orion-lab');
    await user.click(screen.getByRole('button', { name: 'Connect a provider…' }));
    expect(onConnectProvider).toHaveBeenCalledTimes(2); expect(onConnectProvider).toHaveBeenLastCalledWith();
  });
  it('keeps the actions in the dialog footer, outside the scrolling list, and still submits the list', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn(); render(<Host onSelect={onSelect} />);
    const list = screen.getByRole('group', { name: 'Aster family Aster Lab' }).closest('form')!;
    expect(list).not.toContainElement(primary()); expect(primary()).toHaveAttribute('form', list.id);
    expect(list).not.toContainElement(summary());
    await user.click(primary()); expect(onSelect).toHaveBeenCalledExactlyOnceWith(modelIntent);
  });
  it('marks models of a provider that isn’t connected as unavailable, with a way to connect it, and follows the connection live', async () => {
    const user = userEvent.setup(); const onConnectProvider = vi.fn();
    setProviderState('orion-lab', { state: 'available', source: 'none' });
    render(<Host onConnectProvider={onConnectProvider} />);
    const orion = screen.getByRole('radio', { name: /Orion/ });
    expect(orion).toBeDisabled(); expect(orion.closest('[data-disabled]')).toHaveAttribute('data-disabled', 'true');
    expect(screen.getByText('Orion Lab isn’t connected.')).toBeVisible();
    expect(screen.getByRole('radio', { name: /Aster This chat/ })).toBeEnabled(); expect(primary()).toBeEnabled();
    await user.keyboard('ori'); expect(primary()).toBeDisabled();
    await user.click(screen.getByRole('button', { name: 'Connect Orion Lab' })); expect(onConnectProvider).toHaveBeenCalledExactlyOnceWith('orion-lab');
    act(() => setProviderState('orion-lab', { state: 'connected-example', source: 'stored-example' }));
    expect(screen.getByRole('radio', { name: /Orion/ })).toBeEnabled(); expect(screen.queryByRole('button', { name: 'Connect Orion Lab' })).not.toBeInTheDocument();
  });
  it('reads a host-supplied provider list instead of this visit’s connections', () => {
    // The example list starts with Aster Lab not connected and Orion Lab connected.
    render(<Host providers={providers} onConnectProvider={vi.fn()} />);
    for (const radio of screen.getAllByRole('radio', { name: /Aster/ })) expect(radio).toBeDisabled();
    expect(screen.getAllByRole('button', { name: 'Connect Aster Lab' })).toHaveLength(2);
    expect(screen.getByRole('radio', { name: /Orion/ })).toBeEnabled(); expect(primary()).toBeDisabled();
    expect(summary()).toHaveTextContent('Aster · Aster Lab · 214,000 token window');
  });
  it('keeps unknown facts and unbound choices explicit, without a provider link', () => {
    render(<ModelDialog {...chrome} view={{ ...modelView, currentId: 'unbound-example', choices: modelCase('unknown') }} query="" onQueryChange={vi.fn()} onSelect={vi.fn()} onRetry={vi.fn()} onConnectProvider={vi.fn()} />);
    const group = screen.getByRole('group', { name: 'Self-hosted' });
    expect(group).toHaveTextContent('Window unknown'); expect(group).toHaveTextContent('Capabilities unknown');
    expect(within(group).getByRole('radio')).toBeDisabled(); expect(within(group).queryByRole('button')).not.toBeInTheDocument();
    expect(summary()).toHaveTextContent('Unlisted model · Self-hosted · Window unknown');
    expect(primary()).toBeDisabled();
  });
  it('keeps the current model visible while models load', () => {
    render(<ModelDialog {...chrome} view={{ ...modelView, state: 'loading', choices: modelCase('loading') }} query="" onQueryChange={vi.fn()} onSelect={vi.fn()} onRetry={vi.fn()} />);
    expect(screen.getByRole('status')).toHaveTextContent('Loading models…');
    expect(summary()).toHaveTextContent('Aster · Aster Lab · In use'); expect(primary()).toBeDisabled();
  });
  it('shows the host’s reason when choosing is turned off', () => {
    render(<ModelDialog {...chrome} description="Return to the parent chat to change its model." view={{ ...modelView, state: 'disabled' }} query="" onQueryChange={vi.fn()} onSelect={vi.fn()} onRetry={vi.fn()} />);
    expect(screen.getByRole('status')).toHaveTextContent('Return to the parent chat to change its model.'); expect(primary()).toBeDisabled();
  });
  it('emits null reasoning when the host does not edit reasoning, and shows no reasoning control', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn(); const view = { ...modelView, reasoningEditable: false };
    render(<ModelDialog {...chrome} description="Changes this local parent only." view={view} query="" onQueryChange={vi.fn()} onSelect={onSelect} onRetry={vi.fn()} />);
    expect(screen.getByRole('dialog')).toHaveAccessibleDescription(/Changes this local parent only\./);
    expect(screen.queryByLabelText('Reasoning level')).not.toBeInTheDocument();
    expect(screen.getByText('Changes this local parent only.').closest('.srOnly')).not.toBeNull();
    expect(screen.getAllByText('Changes this local parent only.')).toHaveLength(1);
    await user.click(screen.getByRole('radio', { name: /Orion.*96,000/ })); await user.click(primary());
    expect(onSelect).toHaveBeenCalledWith({ ...modelIntent, modelId: 'orion', reasoning: null });
    expect(validModelSelection(view, { ...modelIntent, reasoning: null })).toBe(true);
    expect(validModelSelection(view, modelIntent)).toBe(false);
    expect(validModelSelection(modelView, { ...modelIntent, reasoning: null })).toBe(false);
  });
  it('scopes family identities to providers, uses provider-only groups when absent, and searches family labels', async () => {
    const user = userEvent.setup();
    const choices = [
      { ...modelChoices[0], family: { id: 'shared', label: 'Navigation' } },
      { ...modelChoices[1], family: undefined },
      { ...modelChoices[2], family: { id: 'shared', label: 'Writing' } },
    ];
    expect(validModelCatalog({ ...modelView, choices })).toBe(true);
    render(<Host view={{ ...modelView, choices }} />);
    for (const name of ['Navigation family Aster Lab', 'Aster Lab', 'Writing family Orion Lab']) expect(screen.getByRole('group', { name })).toBeVisible();
    const search = screen.getByRole('searchbox', { name: 'Search models' });
    for (const [query, id] of [['navigation', 'aster'], ['Orion Lab', 'orion'], ['Mini', 'aster-mini']]) {
      await user.clear(search); await user.type(search, query);
      expect(screen.getAllByRole('radio')).toHaveLength(1); expect(screen.getByRole('radio')).toHaveAttribute('value', id); expect(screen.getByRole('radio')).toBeChecked();
    }
    await user.clear(search); await user.type(search, 'no matching profile');
    expect(screen.queryByRole('radio')).not.toBeInTheDocument(); expect(screen.getByRole('status')).toHaveTextContent('No models match');
  });
  it.each([null, [], {}, { id: 'aster' }, { label: 'Aster' }, { id: 4, label: 'Aster' }, { id: ' ', label: 'Aster' }, { id: 'aster', label: ' ' }])('rejects malformed optional family metadata %j', (family) => {
    const view = { ...modelView, choices: [{ ...modelChoices[0], family: family as ModelChoice['family'] }] };
    render(<ModelDialog {...chrome} view={view} query="" onQueryChange={vi.fn()} onSelect={vi.fn()} onRetry={vi.fn()} />);
    expect(validModelCatalog(view)).toBe(false); expect(validModelSelection(view, modelIntent)).toBe(false);
    expect(screen.getByRole('alert')).toHaveTextContent('This model list can’t be shown'); expect(screen.queryByRole('radio')).not.toBeInTheDocument();
    expect(primary()).toBeDisabled();
  });
  it('rejects inconsistent labels for the same provider or provider-scoped family', () => {
    expect(validModelCatalog({ ...modelView, choices: [modelChoices[0], { ...modelChoices[1], family: { id: 'aster', label: 'Different label' } }] })).toBe(false);
    expect(validModelCatalog({ ...modelView, choices: [modelChoices[0], { ...modelChoices[1], providerLabel: 'Different provider label' }] })).toBe(false);
  });
  it('SCREEN11-F02: ignores IME Escape and Enter, clears model search first, then dismisses', async () => {
    const user = userEvent.setup(); const onDismiss = vi.fn(); const onSelect = vi.fn();
    render(<Host onDismiss={onDismiss} onSelect={onSelect} initialQuery="Orion" />);
    const search = screen.getByRole('searchbox', { name: 'Search models' }); search.focus();
    fireEvent.keyDown(search, { key: 'Escape', isComposing: true });
    fireEvent.keyDown(search, { key: 'Enter', isComposing: true });
    expect(search).toHaveValue('Orion'); expect(onDismiss).not.toHaveBeenCalled(); expect(onSelect).not.toHaveBeenCalled();
    await user.keyboard('{Escape}');
    expect(search).toHaveValue(''); expect(search).toHaveFocus(); expect(onDismiss).not.toHaveBeenCalled();
    expect(screen.getByRole('dialog')).toHaveAttribute('open');
    expect(screen.getAllByRole('radio')).toHaveLength(3);
    await user.keyboard('{Escape}'); expect(onDismiss).toHaveBeenCalledTimes(1);
  });
  it('retains the selection across stale/ready views without accepting stale or busy choices', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn(); const props = { ...chrome, query: '', onQueryChange: vi.fn(), onSelect, onRetry: vi.fn() };
    const result = render(<ModelDialog {...props} view={modelView} />); await user.click(screen.getByRole('radio', { name: /Orion.*96,000/ }));
    result.rerender(<ModelDialog {...props} view={{ ...modelView, version: 2, state: 'stale' }} />); expect(screen.getByRole('radio', { name: /Orion.*96,000/ })).toBeChecked(); expect(primary()).toBeDisabled();
    result.rerender(<ModelDialog {...props} view={{ ...modelView, version: 3 }} />); await user.click(primary()); await user.click(primary()); expect(onSelect).toHaveBeenCalledTimes(1);
  });
  it.each(['loading', 'empty', 'error', 'stale', 'unavailable', 'busy', 'disabled'] as const)('rejects %s mutation', (state) => {
    expect(validModelSelection({ ...modelView, state }, modelIntent)).toBe(false);
  });
  it('renders malformed catalogs fail-closed rather than dereferencing them', () => {
    const view = { ...modelView, choices: null! };
    render(<ModelDialog {...chrome} view={view} query="" onQueryChange={vi.fn()} onSelect={vi.fn()} onRetry={vi.fn()} />);
    expect(screen.getByRole('alert')).toHaveTextContent('This model list can’t be shown');
    expect(primary()).toBeDisabled();
    expect(validModelSelection(view, modelIntent)).toBe(false);
  });
  it('rejects unknown, malformed, duplicate, stale-scope and unknown reasoning intents', () => {
    expect(validModelSelection(modelView, { ...modelIntent, modelId: 'unknown' as ModelSelection['modelId'] })).toBe(false);
    expect(validModelSelection(modelView, { ...modelIntent, version: 0 })).toBe(false);
    expect(validModelSelection(modelView, { ...modelIntent, scope: 'new-chat-default' })).toBe(false);
    expect(validModelSelection(modelView, { ...modelIntent, reasoning: 'Unbound' })).toBe(false);
    expect(validModelCatalog({ ...modelView, choices: [...modelChoices, modelChoices[0]] })).toBe(false);
    expect(validModelCatalog({ ...modelView, choices: [{ ...modelChoices[0], reasoning: null! }] })).toBe(false);
    expect(validModelCatalog({ ...modelView, choices: null! })).toBe(false);
  });
});
