import { useState } from 'react';
import { readFileSync } from 'node:fs';
import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { ConnectView, FinderView, ModelChoice, ModelPickerView, ModelSelection } from './contracts';
import { ChatActionDialog, PermissionDialog, QuestionDialog, RenameDialog, validTitle, validWorkspaceIntent } from './ChatDialogs';
import { installDialogTestSupport } from './dialogTestSupport';
import { FinderDialog, validFinderIntent } from './FinderDialog';
import { modelCase, modelChoices, permissionCase, projects, providers, questionCase, sessions, workspaceChoices } from './fixtures';
import { ModelDialog, validModelCatalog, validModelSelection } from './ModelDialog';
import { ProviderConnectDialog, validConnectIntent } from './ProviderViews';

beforeEach(installDialogTestSupport);
const chrome = { onDismiss: vi.fn() };
const modelView: ModelPickerView = { id: 'models', version: 1, state: 'ready', scope: 'current-chat', currentId: 'aster', defaultId: 'aster-mini', choices: modelChoices };
const modelIntent: ModelSelection = { id: 'models', version: 1, modelId: 'aster', scope: 'current-chat', reasoning: 'Low' };

it('SCREEN11-F01: completed provider examples use white, not Working green', () => {
  const screenCss = readFileSync('src/screens/screens.module.css', 'utf8');
  expect(screenCss).toContain(".status[data-status='connected-example'], .status[data-status='success'] { color: var(--status-completed); }");
  expect(screenCss).toContain(".status[data-status='error'] { color: var(--status-stopped-failed); }");
  expect(screenCss).toContain(".status[data-status='waiting'] { color: var(--status-attention); }");
});

describe('controlled model dialog', () => {
  it('summarizes the selected profile and groups exactly three supplied profiles by family and provider', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn(); render(<ModelDialog {...chrome} view={modelView} query="" onQueryChange={vi.fn()} onSelect={onSelect} onRetry={vi.fn()} />);
    expect(screen.getByRole('dialog', { name: 'Choose a model' })).toHaveAttribute('data-wide', 'true');
    const summary = screen.getByRole('region', { name: 'Selected model' });
    expect(within(summary).getByRole('heading', { name: 'Aster' })).toBeVisible();
    expect(summary).toHaveTextContent('Aster Lab'); expect(summary).toHaveTextContent('This chat');
    expect(summary).toHaveTextContent('214,000 token window'); expect(summary).toHaveTextContent('Images');
    expect(within(summary).getByRole('list', { name: 'Model capabilities' })).toHaveTextContent('Reasoning: Low, Medium, High');
    const aster = screen.getByRole('group', { name: 'Aster family Aster Lab' });
    const orion = screen.getByRole('group', { name: 'Orion family Orion Lab' });
    expect(within(aster).getAllByRole('radio')).toHaveLength(2); expect(within(orion).getAllByRole('radio')).toHaveLength(1);
    expect(within(aster).getByRole('radio', { name: /Aster CURRENT.*214,000/ })).toBeChecked();
    expect(within(aster).getByRole('radio', { name: /Aster Mini DEFAULT.*128,000/ })).toBeEnabled();
    expect(new Set(screen.getAllByRole<HTMLInputElement>('radio').map((radio) => radio.name)).size).toBe(1);
    await user.click(within(orion).getByRole('radio', { name: /Orion.*96,000/ })); expect(onSelect).not.toHaveBeenCalled();
    expect(within(summary).getByRole('heading', { name: 'Orion' })).toBeVisible();
    expect(summary).toHaveTextContent('96,000 token window'); expect(summary).not.toHaveTextContent('Images');
    await user.selectOptions(screen.getByLabelText('Reasoning level'), 'High'); await user.click(screen.getByText('Use Orion'));
    expect(onSelect).toHaveBeenCalledWith({ ...modelIntent, modelId: 'orion', reasoning: 'High' });
  });
  it('offers facts only to a main host with no reasoning setting, emitting null rather than an ignored level', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn(); const view = { ...modelView, reasoningEditable: false };
    render(<ModelDialog {...chrome} description="Changes this local parent only." view={view} query="" onQueryChange={vi.fn()} onSelect={onSelect} onRetry={vi.fn()} />);
    expect(screen.getByRole('dialog')).toHaveAccessibleDescription('Changes this local parent only.');
    expect(screen.queryByLabelText('Reasoning level')).not.toBeInTheDocument();
    expect(screen.getByText('Changes this local parent only.').closest('.srOnly')).not.toBeNull();
    expect(screen.getAllByText('Changes this local parent only.')).toHaveLength(1);
    expect(screen.queryByText(/^Scope:/)).not.toBeInTheDocument();
    expect(screen.queryByText(/^Current:.*Default:/)).not.toBeInTheDocument();
    expect(screen.queryByText(/Reasoning capabilities are facts only|no reasoning setting/)).not.toBeInTheDocument();
    await user.click(screen.getByRole('radio', { name: /Orion.*96,000/ }));
    await user.click(screen.getByText('Use Orion'));
    expect(onSelect).toHaveBeenCalledWith({ ...modelIntent, modelId: 'orion', reasoning: null });
    expect(validModelSelection(view, { ...modelIntent, reasoning: null })).toBe(true);
    expect(validModelSelection(view, modelIntent)).toBe(false);
    expect(validModelSelection(modelView, { ...modelIntent, reasoning: null })).toBe(false);
  });
  it('keeps native radio keyboard navigation across family groups without submitting a draft', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn();
    render(<ModelDialog {...chrome} view={modelView} query="" onQueryChange={vi.fn()} onSelect={onSelect} onRetry={vi.fn()} />);
    const radios = screen.getAllByRole<HTMLInputElement>('radio'); radios[0].focus();
    await user.keyboard('{ArrowRight}'); expect(radios[1]).toHaveFocus(); expect(radios[1]).toBeChecked();
    await user.keyboard('{ArrowRight}'); expect(radios[2]).toHaveFocus(); expect(radios[2]).toBeChecked();
    expect(within(screen.getByRole('region', { name: 'Selected model' })).getByRole('heading', { name: 'Orion' })).toBeVisible();
    expect(onSelect).not.toHaveBeenCalled();
  });
  it('scopes family identities to providers, uses provider-only groups when absent, and searches supplied family labels', async () => {
    const user = userEvent.setup();
    const choices = [
      { ...modelChoices[0], family: { id: 'shared', label: 'Navigation' } },
      { ...modelChoices[1], family: undefined },
      { ...modelChoices[2], family: { id: 'shared', label: 'Writing' } },
    ];
    expect(validModelCatalog({ ...modelView, choices })).toBe(true);
    function Host() {
      const [query, setQuery] = useState('');
      return <ModelDialog {...chrome} view={{ ...modelView, choices }} query={query} onQueryChange={setQuery} onSelect={vi.fn()} onRetry={vi.fn()} />;
    }
    render(<Host />);
    for (const name of ['Navigation family Aster Lab', 'Aster Lab', 'Writing family Orion Lab']) expect(screen.getByRole('group', { name })).toBeVisible();
    const search = screen.getByRole('searchbox', { name: 'Search models' });
    for (const [query, id] of [['navigation', 'aster'], ['Orion Lab', 'orion'], ['Mini', 'aster-mini']]) {
      await user.clear(search); await user.type(search, query);
      expect(screen.getAllByRole('radio')).toHaveLength(1); expect(screen.getByRole('radio')).toHaveAttribute('value', id);
    }
    expect(within(screen.getByRole('region', { name: 'Selected model' })).getByRole('heading', { name: 'Aster' })).toBeVisible();
    await user.clear(search); await user.type(search, 'no matching profile');
    expect(screen.queryByRole('radio')).not.toBeInTheDocument(); expect(screen.getByRole('status')).toHaveTextContent('No matching models');
  });
  it('keeps unknown facts and unbound choices explicit without guessing a family', () => {
    render(<ModelDialog {...chrome} view={{ ...modelView, currentId: 'unbound-example', choices: modelCase('unknown') }} query="" onQueryChange={vi.fn()} onSelect={vi.fn()} onRetry={vi.fn()} />);
    const group = screen.getByRole('group', { name: 'Example provider' });
    expect(group).toHaveTextContent('Window unknown'); expect(group).toHaveTextContent('Capabilities unknown');
    expect(group).toHaveTextContent('No bound model profile'); expect(within(group).getByRole('radio')).toBeDisabled();
    const summary = screen.getByRole('region', { name: 'Selected model' });
    expect(summary).toHaveTextContent('Window unknown'); expect(summary).toHaveTextContent('Capabilities unknown');
    expect(screen.getByRole('button', { name: 'Use Unbound model' })).toBeDisabled();
  });
  it.each([null, [], {}, { id: 'aster' }, { label: 'Aster' }, { id: 4, label: 'Aster' }, { id: ' ', label: 'Aster' }, { id: 'aster', label: ' ' }])('rejects malformed optional family metadata %j', (family) => {
    const view = { ...modelView, choices: [{ ...modelChoices[0], family: family as ModelChoice['family'] }] };
    render(<ModelDialog {...chrome} view={view} query="" onQueryChange={vi.fn()} onSelect={vi.fn()} onRetry={vi.fn()} />);
    expect(validModelCatalog(view)).toBe(false); expect(validModelSelection(view, modelIntent)).toBe(false);
    expect(screen.getByRole('alert')).toHaveTextContent('malformed'); expect(screen.queryByRole('radio')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Use model' })).toBeDisabled();
  });
  it('rejects inconsistent labels for the same supplied provider or provider-scoped family', () => {
    expect(validModelCatalog({ ...modelView, choices: [modelChoices[0], { ...modelChoices[1], family: { id: 'aster', label: 'Different label' } }] })).toBe(false);
    expect(validModelCatalog({ ...modelView, choices: [modelChoices[0], { ...modelChoices[1], providerLabel: 'Different provider label' }] })).toBe(false);
  });
  it('SCREEN11-F02: ignores IME Escape, clears model search first, then dismisses', async () => {
    const user = userEvent.setup(); const onDismiss = vi.fn();
    function Host() {
      const [query, setQuery] = useState('Orion');
      return <ModelDialog onDismiss={onDismiss} view={modelView} query={query} onQueryChange={setQuery} onSelect={vi.fn()} onRetry={vi.fn()} />;
    }
    render(<Host />);
    const search = screen.getByRole('searchbox', { name: 'Search models' }); search.focus();
    fireEvent.keyDown(search, { key: 'Escape', isComposing: true });
    expect(search).toHaveValue('Orion'); expect(onDismiss).not.toHaveBeenCalled();
    await user.keyboard('{Escape}');
    expect(search).toHaveValue(''); expect(search).toHaveFocus(); expect(onDismiss).not.toHaveBeenCalled();
    expect(screen.getByRole('dialog')).toHaveAttribute('open');
    expect(screen.getAllByRole('radio')).toHaveLength(3);
    await user.keyboard('{Escape}'); expect(onDismiss).toHaveBeenCalledTimes(1);
  });
  it('retains the draft across stale/ready views without accepting stale or busy choices', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn(); const props = { ...chrome, query: '', onQueryChange: vi.fn(), onSelect, onRetry: vi.fn() };
    const result = render(<ModelDialog {...props} view={modelView} />); await user.click(screen.getByRole('radio', { name: /Orion.*96,000/ }));
    result.rerender(<ModelDialog {...props} view={{ ...modelView, version: 2, state: 'stale' }} />); expect(screen.getByRole('radio', { name: /Orion.*96,000/ })).toBeChecked(); expect(screen.getByText('Use Orion')).toBeDisabled();
    result.rerender(<ModelDialog {...props} view={{ ...modelView, version: 3 }} />); await user.click(screen.getByText('Use Orion')); expect(onSelect).toHaveBeenCalledTimes(1);
  });
  it.each(['loading', 'empty', 'error', 'stale', 'unavailable', 'busy', 'disabled'] as const)('rejects %s mutation', (state) => {
    expect(validModelSelection({ ...modelView, state }, modelIntent)).toBe(false);
  });
  it('renders malformed supplied catalogs fail-closed rather than dereferencing them', () => {
    const view = { ...modelView, choices: null! };
    render(<ModelDialog {...chrome} view={view} query="" onQueryChange={vi.fn()} onSelect={vi.fn()} onRetry={vi.fn()} />);
    expect(screen.getByRole('alert')).toHaveTextContent('malformed');
    expect(screen.getByText('Use model')).toBeDisabled();
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

describe('provider connection scopes and ephemeral input', () => {
  const view: ConnectView = { id: 'connect', version: 1, provider: providers[0], step: 'key', method: 'key' };
  it('accepts only a masked dummy literal and never emits its value', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); render(<ProviderConnectDialog {...chrome} view={view} onIntent={onIntent} />);
    const input = screen.getByLabelText('Dummy API key'); expect(input).toHaveAttribute('type', 'password');
    await user.type(input, 'not-an-accepted-demo-value'); await user.click(screen.getByText('Continue with dummy key')); expect(onIntent).not.toHaveBeenCalled(); expect(input).toHaveValue(''); expect(screen.getByRole('alert')).toBeInTheDocument();
    await user.type(input, 'AVA-DEMO-KEY'); await user.click(screen.getByText('Continue with dummy key'));
    expect(input).toHaveValue(''); expect(onIntent).toHaveBeenCalledWith({ id: 'connect', version: 1, action: 'submit-demo-key' });
    expect(JSON.stringify(onIntent.mock.calls)).not.toContain('AVA-DEMO-KEY');
  });
  it('clears transient keys on step replacement and unmount, with no automatic settlement', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const result = render(<ProviderConnectDialog {...chrome} view={view} onIntent={onIntent} />);
    await user.type(screen.getByLabelText('Dummy API key'), 'AVA-DEMO-KEY');
    result.rerender(<ProviderConnectDialog {...chrome} view={{ ...view, version: 2, step: 'waiting' }} onIntent={onIntent} />); expect(screen.queryByLabelText('Dummy API key')).not.toBeInTheDocument(); expect(onIntent).not.toHaveBeenCalled();
    result.rerender(<ProviderConnectDialog {...chrome} view={{ ...view, version: 3 }} onIntent={onIntent} />); expect(screen.getByLabelText('Dummy API key')).toHaveValue(''); result.unmount();
    render(<ProviderConnectDialog {...chrome} view={view} onIntent={onIntent} />); expect(screen.getByLabelText('Dummy API key')).toHaveValue('');
  });
  it('renders malformed provider methods without exposing an active form', () => {
    render(<ProviderConnectDialog {...chrome} view={{ ...view, provider: { ...providers[0], methods: null! } }} onIntent={vi.fn()} />);
    expect(screen.getByRole('alert')).toHaveTextContent('Unsupported');
    expect(screen.queryByLabelText('Dummy API key')).not.toBeInTheDocument();
  });
  it('rejects stale, unknown-method, unsupported-step and environment disconnect intents', () => {
    const identity = { id: 'connect', version: 1 };
    expect(validConnectIntent(view, { ...identity, action: 'submit-demo-key', version: 0 })).toBe(false);
    expect(validConnectIntent({ ...view, step: 'method' }, { ...identity, action: 'choose-method', method: 'unknown' as 'key' })).toBe(false);
    expect(validConnectIntent(view, { ...identity, action: 'show-success' })).toBe(false);
    expect(validConnectIntent({ ...view, step: 'disconnect', provider: providers[2] }, { ...identity, action: 'disconnect' })).toBe(false);
    expect(validConnectIntent({ ...view, step: 'disconnect', provider: providers[1] }, { ...identity, action: 'disconnect' })).toBe(true);
  });
  it('ignores IME Escape, then goes Back without closing or emitting input', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const onDismiss = vi.fn(); render(<ProviderConnectDialog onDismiss={onDismiss} view={view} onIntent={onIntent} />);
    await user.type(screen.getByLabelText('Dummy API key'), 'AVA-DEMO-KEY'); fireEvent.keyDown(screen.getByLabelText('Dummy API key'), { key: 'Escape', isComposing: true }); expect(onIntent).not.toHaveBeenCalled();
    fireEvent.keyDown(screen.getByLabelText('Dummy API key'), { key: 'Escape' }); expect(onIntent).toHaveBeenCalledWith({ id: 'connect', version: 1, action: 'back' }); expect(onDismiss).not.toHaveBeenCalled();
  });
});

describe('session, rename, confirmations and request wrappers', () => {
  it('filters projects and refuses unbound or stale finder choices', async () => {
    const user = userEvent.setup(); const onSelect = vi.fn(); const view: FinderView = { id: 'sessions', version: 1, kind: 'sessions', state: 'ready', items: sessions, projects };
    render(<FinderDialog {...chrome} view={view} query="" onQueryChange={vi.fn()} onSelect={onSelect} onRetry={vi.fn()} />);
    await user.selectOptions(screen.getByLabelText('Project filter'), 'notebook'); expect(screen.queryByText('Refine search filters')).not.toBeInTheDocument();
    await user.click(screen.getByText('Organize reading notes')); expect(onSelect).toHaveBeenCalledWith({ id: 'sessions', version: 1, itemId: 'notes' });
    expect(validFinderIntent(view, { id: 'sessions', version: 0, itemId: 'notes' })).toBe(false);
    expect(validFinderIntent(view, { id: 'sessions', version: 1, itemId: 'unavailable' })).toBe(false);
    expect(validFinderIntent({ ...view, kind: 'commands' }, { id: 'sessions', version: 1, itemId: 'notes' })).toBe(false);
  });
  it('rejects unbound, duplicate and stale workspace choices', () => {
    const view = { id: 'workspace', version: 1, state: 'choice' as const, selectedId: 'atlas', choices: workspaceChoices };
    expect(validWorkspaceIntent(view, { id: 'workspace', version: 1, action: 'choose', workspaceId: 'unknown' })).toBe(false);
    expect(validWorkspaceIntent(view, { id: 'workspace', version: 0, action: 'choose', workspaceId: 'atlas' })).toBe(false);
    expect(validWorkspaceIntent({ ...view, choices: [workspaceChoices[0], workspaceChoices[0]] }, { id: 'workspace', version: 1, action: 'choose', workspaceId: 'atlas' })).toBe(false);
    expect(validWorkspaceIntent({ ...view, state: 'opening', selectedId: 'unknown' }, { id: 'workspace', version: 1, action: 'show-ready' })).toBe(false);
  });
  it('validates trimmed nonempty rename and the 256 character bound', async () => {
    const user = userEvent.setup(); const onRename = vi.fn(); render(<RenameDialog {...chrome} view={{ id: 'rename', version: 1, state: 'ready', title: '' }} onRename={onRename} />);
    const input = screen.getByLabelText('Chat title'); expect(input).toHaveAttribute('maxlength', '256'); await user.click(screen.getByText('Rename example')); expect(screen.getByRole('alert')).toHaveTextContent('non-empty'); expect(onRename).not.toHaveBeenCalled();
    await user.type(input, '  Better title  '); await user.click(screen.getByText('Rename example')); expect(onRename).toHaveBeenCalledWith({ id: 'rename', version: 1, title: 'Better title' });
    expect(validTitle('A'.repeat(256))).toBe(true); expect(validTitle('A'.repeat(257))).toBe(false); expect(validTitle('   ')).toBe(false);
  });
  it('cancel never emits archive or branch intent', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const onDismiss = vi.fn(); render(<ChatActionDialog onDismiss={onDismiss} action="archive" view={{ id: 'archive', version: 1, title: 'Original', state: 'ready' }} points={[]} onIntent={onIntent} />);
    expect(screen.getByText(/Archive is not wired in RPC v1/)).toBeInTheDocument(); await user.click(screen.getByText('Cancel')); expect(onDismiss).toHaveBeenCalledTimes(1); expect(onIntent).not.toHaveBeenCalled();
  });
  it('dismissal does not approve; session grant appears only when advertised; double submit is guarded', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const onDismiss = vi.fn(); const result = render(<PermissionDialog onDismiss={onDismiss} request={permissionCase('once', 1)} onIntent={onIntent} />);
    expect(screen.queryByText('Allow for session')).not.toBeInTheDocument(); await user.click(screen.getByText('Dismiss unresolved')); expect(onIntent).not.toHaveBeenCalled(); expect(onDismiss).toHaveBeenCalled();
    result.rerender(<PermissionDialog onDismiss={onDismiss} request={permissionCase('session', 2)} onIntent={onIntent} />);
    await user.dblClick(screen.getByText('Allow for session')); expect(onIntent).toHaveBeenCalledTimes(1); expect(onIntent).toHaveBeenCalledWith({ id: 'review-permission', version: 2, kind: 'permission', decision: 'allow_session' }); expect(screen.queryByText(/Allow always/i)).not.toBeInTheDocument();
  });
  it.each(['pending', 'submitted', 'error', 'stale', 'closed', 'invalid', 'duplicate'])('fails closed for %s requests', (scenario) => {
    const onIntent = vi.fn(); render(<PermissionDialog {...chrome} request={permissionCase(scenario, 1)} onIntent={onIntent} />);
    for (const button of within(screen.getByRole('dialog')).queryAllByRole('button', { name: /Allow once|Allow for session|^Deny$/ })) expect(button).toBeDisabled(); expect(onIntent).not.toHaveBeenCalled();
  });
  it('clears secret drafts when the supplied state goes stale without changing identity', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const request = questionCase('secret', 1);
    const result = render(<QuestionDialog {...chrome} request={request} onIntent={onIntent} />);
    await user.type(screen.getByLabelText('Dummy secret answer'), 'ephemeral-demo');
    result.rerender(<QuestionDialog {...chrome} request={{ ...request, state: 'stale' }} onIntent={onIntent} />);
    expect(screen.getByLabelText('Dummy secret answer')).toHaveValue(''); expect(screen.getByText('Submit answer')).toBeDisabled(); expect(onIntent).not.toHaveBeenCalled();
  });
  it('cancel clears a custom answer and emits cancellation only', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); render(<QuestionDialog {...chrome} request={questionCase('custom', 1)} onIntent={onIntent} />);
    await user.type(screen.getByLabelText('Custom example answer'), 'unrecorded custom text'); await user.click(screen.getByText('Cancel question'));
    expect(screen.getByLabelText('Custom example answer')).toHaveValue(''); expect(onIntent).toHaveBeenCalledWith({ id: 'review-question', version: 1, kind: 'question-cancel' });
  });
  it('clears a masked answer and excludes custom/secret content from receipts', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const result = render(<QuestionDialog {...chrome} request={questionCase('secret', 1)} onIntent={onIntent} />);
    const input = screen.getByLabelText('Dummy secret answer'); expect(input).toHaveAttribute('type', 'password'); await user.type(input, 'dummy-only-value'); await user.click(screen.getByText('Submit answer'));
    expect(input).toHaveValue(''); expect(onIntent).toHaveBeenCalledWith({ id: 'review-question', version: 1, kind: 'question-answer' }); expect(JSON.stringify(onIntent.mock.calls)).not.toContain('dummy-only-value');
    result.rerender(<QuestionDialog {...chrome} request={questionCase('secret', 2)} onIntent={onIntent} />); expect(screen.getByLabelText('Dummy secret answer')).toHaveValue('');
  });
});
