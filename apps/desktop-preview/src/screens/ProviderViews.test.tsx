import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { DialogFrame } from '../ui/DialogFrame';
import { providerIdentity } from '../ui/providerIdentity';
import type { ConnectView, Provider } from './contracts';
import { installDialogTestSupport } from './dialogTestSupport';
import { ProviderConnectDialog, ProviderDirectory, connectDelays, connectStart, validConnectIntent, validProvider } from './ProviderViews';
import { providers } from './screenExamples';

beforeEach(installDialogTestSupport);
const chrome = { onDismiss: vi.fn() };
const byId = (id: string) => providers.find((provider) => provider.id === id)!;
const statusOf = (text: string) => screen.getByText(text).closest('[data-status]');
const connectable = (provider: Provider) => (provider.state === 'available' || provider.state === 'error') && provider.methods.length > 0;

describe('provider list', () => {
  it('shows every provider as one row with its status and at most one working action', async () => {
    const spies = ['fetch', 'XMLHttpRequest', 'WebSocket', 'FileReader', 'showOpenFilePicker', 'open'].map((name) => { const spy = vi.fn(); vi.stubGlobal(name, spy); return spy; });
    const onConnect = vi.fn(); const onDisconnect = vi.fn(); const user = userEvent.setup();
    render(<ProviderDirectory providers={providers} onConnect={onConnect} onDisconnect={onDisconnect} />);
    expect(screen.queryByRole('button', { name: /Connection examples/ })).not.toBeInTheDocument();
    const connected = screen.getByRole('list', { name: 'Connected providers' });
    const available = screen.getByRole('list', { name: 'Available providers' });
    expect(within(connected).getAllByRole('listitem').length + within(available).getAllByRole('listitem').length).toBe(providers.length);
    for (const row of [...within(connected).getAllByRole('listitem'), ...within(available).getAllByRole('listitem')]) expect(within(row).queryAllByRole('button').length).toBeLessThanOrEqual(1);
    for (const provider of providers) {
      const row = screen.getByText(provider.id).closest('li')!;
      expect(row).toHaveTextContent(provider.label);
      expect(within(provider.state === 'connected-example' || provider.state === 'external' ? connected : available).getByText(provider.id)).toBe(screen.getByText(provider.id));
      expect(row.querySelector('[data-provider-mark]')).toHaveAttribute('data-provider-mark', providerIdentity(provider.id) ? provider.id : 'neutral');
    }
    expect(within(screen.getByText('orion-lab').closest('li')!).getByText('Connected').closest('[data-status]')).toHaveAttribute('data-status', 'completed');
    const cedar = screen.getByText('cedar').closest('li')!;
    expect(within(cedar).getByText('Managed externally').closest('[data-status]')).toHaveAttribute('data-status', 'completed');
    expect(within(cedar).queryByRole('button')).not.toBeInTheDocument();
    const harbor = screen.getByText('harbor').closest('li')!;
    expect(within(harbor).getByText('Error').closest('[data-status]')).toHaveAttribute('data-status', 'failed');
    expect(within(harbor).getByText('The API key expired.')).toBeVisible();
    expect(within(harbor).getByRole('button', { name: 'Connect Harbor' })).toHaveAccessibleDescription('The API key expired.');
    expect(within(screen.getByText('aster-lab').closest('li')!).getByText('Not connected')).toBeVisible();
    await user.click(screen.getByRole('button', { name: `Disconnect ${byId('orion-lab').label}` })); expect(onDisconnect).toHaveBeenCalledExactlyOnceWith('orion-lab');
    const targets = providers.filter(connectable);
    for (const provider of targets) { await user.click(screen.getByRole('button', { name: `Connect ${provider.label}` })); expect(onConnect).toHaveBeenLastCalledWith(provider.id); }
    expect(onConnect).toHaveBeenCalledTimes(targets.length); expect(targets.map((provider) => provider.id)).toEqual(expect.arrayContaining(['aster-lab', 'harbor']));
    for (const spy of spies) expect(spy).not.toHaveBeenCalled();
  });
  it('rejects retained controls while inactive and accepts them again once active', () => {
    const onConnect = vi.fn(); const props = { providers, onConnect, onDisconnect: vi.fn() };
    const view = render(<ProviderDirectory {...props} />);
    const button = screen.getByRole('button', { name: 'Connect Aster Lab' });
    view.rerender(<ProviderDirectory {...props} active={false} />); fireEvent.click(button); expect(onConnect).not.toHaveBeenCalled();
    view.rerender(<ProviderDirectory {...props} />); fireEvent.click(button); expect(onConnect).toHaveBeenCalledExactlyOnceWith('aster-lab');
  });
  it('opens on the search, never on an action, including when nothing can be connected', () => {
    const first = render(<DialogFrame wide title="Connect a provider" onDismiss={vi.fn()}><ProviderDirectory providers={providers} onConnect={vi.fn()} onDisconnect={vi.fn()} /></DialogFrame>);
    expect(screen.getByRole('searchbox', { name: 'Search providers' })).toHaveFocus();
    expect(screen.getAllByRole('button').filter((button) => button.hasAttribute('data-autofocus'))).toHaveLength(0);
    first.unmount();
    render(<DialogFrame wide title="Connect a provider" onDismiss={vi.fn()}><ProviderDirectory providers={providers.filter((provider) => !connectable(provider))} onConnect={vi.fn()} onDisconnect={vi.fn()} /></DialogFrame>);
    expect(screen.getByRole('searchbox', { name: 'Search providers' })).toHaveFocus();
    expect(screen.getByRole('button', { name: 'Disconnect Orion Lab' })).not.toHaveFocus();
  });
  it('narrows by name or ID word by word, keeps the groups, says when nothing matches, and Escape clears before closing', async () => {
    const user = userEvent.setup(); const onDismiss = vi.fn(); const onConnect = vi.fn();
    render(<DialogFrame wide title="Connect a provider" onDismiss={onDismiss}><ProviderDirectory providers={providers} onConnect={onConnect} onDisconnect={vi.fn()} /></DialogFrame>);
    const search = screen.getByRole('searchbox', { name: 'Search providers' });
    const rows = (title: string) => within(screen.getByRole('list', { name: `${title} providers` })).getAllByRole('listitem');
    const only = (title: string, label: string) => { expect(rows(title)).toHaveLength(1); expect(rows(title)[0]).toHaveTextContent(label); };
    await user.type(search, 'LAB');
    only('Connected', 'Orion Lab'); only('Available', 'Aster Lab');
    await user.clear(search); await user.type(search, 'open ai');
    expect(screen.queryByRole('list', { name: 'Connected providers' })).not.toBeInTheDocument(); only('Available', 'OpenAI');
    await user.click(screen.getByRole('button', { name: 'Connect OpenAI' })); expect(onConnect).toHaveBeenCalledExactlyOnceWith('openai');
    await user.clear(search); await user.type(search, 'zai-coding');
    only('Available', 'Z.ai Coding CN');
    await user.clear(search); await user.type(search, 'nothing like this');
    expect(screen.getByText('No providers match “nothing like this”.')).toHaveAttribute('role', 'status');
    expect(screen.queryByRole('list')).not.toBeInTheDocument(); expect(screen.queryByRole('button', { name: /^(Connect|Disconnect) / })).not.toBeInTheDocument();
    fireEvent.keyDown(search, { key: 'Escape', isComposing: true }); expect(search).toHaveValue('nothing like this');
    fireEvent.compositionStart(search); fireEvent.keyDown(search, { key: 'Escape' }); expect(search).toHaveValue('nothing like this'); fireEvent.compositionEnd(search);
    await user.keyboard('{Escape}'); expect(search).toHaveValue(''); expect(search).toHaveFocus(); expect(onDismiss).not.toHaveBeenCalled();
    expect(screen.getAllByRole('listitem')).toHaveLength(providers.length);
    await user.keyboard('{Escape}'); expect(onDismiss).toHaveBeenCalledTimes(1);
  });
  it('lets Escape reach the page once the search is empty', async () => {
    const user = userEvent.setup(); const onPageKey = vi.fn();
    render(<div onKeyDown={(event) => onPageKey(event.key)}><ProviderDirectory providers={providers} onConnect={vi.fn()} onDisconnect={vi.fn()} /></div>);
    const search = screen.getByRole('searchbox', { name: 'Search providers' });
    await user.type(search, 'orion'); onPageKey.mockClear();
    await user.keyboard('{Escape}'); expect(search).toHaveValue(''); expect(onPageKey).not.toHaveBeenCalled();
    await user.keyboard('{Escape}'); expect(onPageKey).toHaveBeenCalledExactlyOnceWith('Escape');
  });
  it('reflects a visit-local connect and disconnect, and refuses an unknown directory', () => {
    const next = providers.map((provider) => provider.id === 'aster-lab' ? { ...provider, state: 'connected-example' as const, source: 'stored-example' as const }
      : provider.id === 'orion-lab' ? { ...provider, state: 'available' as const, source: 'none' as const } : provider);
    const view = render(<ProviderDirectory providers={next} onConnect={vi.fn()} onDisconnect={vi.fn()} />);
    expect(within(screen.getByRole('list', { name: 'Connected providers' })).getByRole('button', { name: 'Disconnect Aster Lab' })).toBeVisible();
    expect(within(screen.getByRole('list', { name: 'Available providers' })).getByRole('button', { name: 'Connect Orion Lab' })).toBeVisible();
    view.rerender(<ProviderDirectory providers={[...providers, { ...providers[0], id: 'openai-compatible' }]} onConnect={vi.fn()} onDisconnect={vi.fn()} />);
    expect(screen.getByRole('alert')).toHaveTextContent('can’t be shown'); expect(screen.queryByRole('button')).not.toBeInTheDocument();
  });
});

describe('provider authority', () => {
  it('does not grant connection authority to unknown or relabeled IDs', () => {
    for (const id of ['openai-compatible', 'grok', 'constructor', '__proto__', 'unknown']) {
      const provider = { ...providers[0], id };
      expect(validProvider(provider)).toBe(false);
      expect(validConnectIntent({ id: 'demo', version: 1, provider, step: 'method', method: null }, { id: 'demo', version: 1, action: 'choose-method', method: 'key' })).toBe(false);
    }
    for (const id of ['openai', 'anthropic', 'zai-coding-cn']) expect(validProvider({ ...providers[0], id })).toBe(false);
    expect(validProvider({ ...providers[0], label: 'OpenAI' })).toBe(false);
    expect(validProvider({ ...providers[0], methods: ['key'] })).toBe(false);
  });
  it('allows only coherent visit-local state; environment credentials stay external', () => {
    const aster = byId('aster-lab'); const orion = byId('orion-lab'); const cedar = byId('cedar');
    expect(validProvider({ ...aster, state: 'connected-example', source: 'stored-example' })).toBe(true);
    expect(validProvider({ ...orion, state: 'available', source: 'none' })).toBe(true);
    expect(validProvider({ ...aster, state: 'connected-example', source: 'none' })).toBe(false);
    expect(validProvider({ ...aster, state: 'available', source: 'stored-example' })).toBe(false);
    expect(validProvider({ ...aster, state: 'external', source: 'environment-example' })).toBe(false);
    expect(validProvider({ ...cedar, source: 'stored-example', state: 'connected-example' })).toBe(false);
  });
  it.each(providers)('preserves the advertised methods and credential scope for $id', (provider) => {
    const view: ConnectView = { id: 'demo', version: 1, provider, step: 'method', method: null };
    expect(validProvider(provider)).toBe(true);
    for (const method of ['key', 'browser', 'device'] as const) expect(validConnectIntent(view, { id: view.id, version: 1, action: 'choose-method', method })).toBe(provider.methods.includes(method));
    expect(validConnectIntent({ ...view, step: 'disconnect' }, { id: view.id, version: 1, action: 'disconnect' })).toBe(provider.source === 'stored-example');
    const { container } = render(<ProviderConnectDialog view={view} onIntent={vi.fn()} onDismiss={vi.fn()} />);
    expect(container.ownerDocument.querySelector('dialog [data-provider-mark]')).toHaveAttribute('data-provider-mark', providerIdentity(provider.id) ? provider.id : 'neutral');
  });
  it('rejects stale, unknown-method, unsupported-step and environment disconnect intents; choose-model only after success', () => {
    const view: ConnectView = { id: 'connect', version: 1, provider: providers[0], step: 'key', method: 'key' };
    const identity = { id: 'connect', version: 1 };
    expect(validConnectIntent(view, { ...identity, action: 'submit-demo-key', version: 0 })).toBe(false);
    expect(validConnectIntent({ ...view, step: 'method' }, { ...identity, action: 'choose-method', method: 'unknown' as 'key' })).toBe(false);
    expect(validConnectIntent(view, { ...identity, action: 'show-success' })).toBe(false);
    expect(validConnectIntent({ ...view, step: 'disconnect', provider: byId('cedar') }, { ...identity, action: 'disconnect' })).toBe(false);
    expect(validConnectIntent({ ...view, step: 'disconnect', provider: byId('orion-lab') }, { ...identity, action: 'disconnect' })).toBe(true);
    expect(validConnectIntent({ ...view, step: 'success' }, { ...identity, action: 'choose-model' })).toBe(true);
    expect(validConnectIntent({ ...view, step: 'waiting' }, { ...identity, action: 'choose-model' })).toBe(false);
  });
});

describe('connect flow', () => {
  const keyView: ConnectView = { id: 'connect', version: 1, provider: byId('aster-lab'), step: 'key', method: 'key' };
  const at = (step: ConnectView['step'], method: ConnectView['method'], provider = byId('aster-lab')): ConnectView => ({ id: 'connect', version: 1, provider, step, method });

  it('accepts a key that could be real, clears a rejected one with a named cause, and never emits it', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); render(<ProviderConnectDialog {...chrome} view={keyView} onIntent={onIntent} />);
    const dialog = screen.getByRole('dialog', { name: 'Connect Aster Lab' });
    expect(dialog).toHaveAccessibleDescription('Use a key from your Aster Lab account.');
    const input = screen.getByLabelText('API key'); expect(input).toHaveAttribute('type', 'password'); expect(input).toHaveFocus();
    const guide = 'Create a key in your Aster Lab account’s API settings. The key needs access to the models you want to use, and usage is billed by Aster Lab.';
    expect(screen.getByText(guide)).toBeVisible(); expect(dialog).not.toHaveTextContent(/https?:|www\./);
    expect(input).toHaveAccessibleDescription(`AVA checks the key with Aster Lab before saving it. ${guide}`);
    expect(dialog).not.toHaveTextContent(/preview|demo|Mark as/i);
    // The submit sits in the footer, outside the form, and still submits it.
    const verify = screen.getByRole('button', { name: 'Verify key' }); expect(input.closest('form')).not.toContainElement(verify); expect(verify).toHaveAttribute('form', input.closest('form')!.id);
    await user.click(verify); expect(screen.getByRole('alert')).toHaveTextContent('Enter an API key.');
    await user.type(input, 'short-key'); await user.keyboard('{Enter}');
    expect(onIntent).not.toHaveBeenCalled(); expect(input).toHaveValue(''); expect(input).toHaveAttribute('aria-invalid', 'true');
    expect(screen.getByRole('alert')).toHaveTextContent('That’s too short to be an API key, so it was cleared for safety. Paste the whole key from your Aster Lab account.');
    await user.type(input, 'ak test 5f2c9e81b7d04a63'); await user.click(verify);
    expect(onIntent).not.toHaveBeenCalled(); expect(input).toHaveValue('');
    expect(screen.getByRole('alert')).toHaveTextContent('API keys don’t contain spaces, so this one was cleared for safety. Paste the key again.');
    await user.type(input, '  ak-test-5f2c9e81b7d04a63 '); await user.click(verify);
    expect(input).toHaveValue(''); expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'connect', version: 1, action: 'submit-demo-key' });
    expect(JSON.stringify(onIntent.mock.calls)).not.toContain('5f2c9e81b7d04a63');
  });
  it('clears transient keys on step replacement and unmount', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const result = render(<ProviderConnectDialog {...chrome} view={keyView} onIntent={onIntent} />);
    await user.type(screen.getByLabelText('API key'), 'ak-test-5f2c9e81b7d04a63');
    result.rerender(<ProviderConnectDialog {...chrome} view={{ ...keyView, version: 2, step: 'waiting' }} onIntent={onIntent} />); expect(screen.queryByLabelText('API key')).not.toBeInTheDocument(); expect(onIntent).not.toHaveBeenCalled();
    result.rerender(<ProviderConnectDialog {...chrome} view={{ ...keyView, version: 3 }} onIntent={onIntent} />); expect(screen.getByLabelText('API key')).toHaveValue(''); result.unmount();
    render(<ProviderConnectDialog {...chrome} view={keyView} onIntent={onIntent} />); expect(screen.getByLabelText('API key')).toHaveValue('');
  });
  it('settles sign-ins and key checks by itself after a short wait, once per step, unless held or closed', () => {
    vi.useFakeTimers();
    try {
      const onIntent = vi.fn(); const settle = (ms: number) => act(() => { vi.advanceTimersByTime(ms); });
      const result = render(<ProviderConnectDialog {...chrome} view={at('browser', 'browser')} onIntent={onIntent} />);
      settle(connectDelays.browser - 1); expect(onIntent).not.toHaveBeenCalled();
      settle(1); expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'connect', version: 1, action: 'show-waiting' });
      settle(connectDelays.device); expect(onIntent).toHaveBeenCalledTimes(1);
      result.rerender(<ProviderConnectDialog {...chrome} view={{ ...at('waiting', 'browser'), version: 2 }} onIntent={onIntent} />);
      settle(connectDelays.verify); expect(onIntent).toHaveBeenLastCalledWith({ id: 'connect', version: 2, action: 'show-success' });
      result.rerender(<ProviderConnectDialog {...chrome} view={{ ...at('device', 'device'), version: 3 }} onIntent={onIntent} />);
      settle(connectDelays.device); expect(onIntent).toHaveBeenLastCalledWith({ id: 'connect', version: 3, action: 'show-waiting' });
      for (const [version, step] of [[4, 'success'], [5, 'error'], [6, 'canceled']] as const) {
        result.rerender(<ProviderConnectDialog {...chrome} view={{ ...at(step, 'key'), version }} onIntent={onIntent} />); settle(60_000);
      }
      result.rerender(<ProviderConnectDialog {...chrome} autoAdvance={false} view={{ ...at('waiting', 'key'), version: 7 }} onIntent={onIntent} />); settle(60_000);
      expect(onIntent).toHaveBeenCalledTimes(3);
      result.rerender(<ProviderConnectDialog {...chrome} view={{ ...at('waiting', 'key'), version: 8 }} onIntent={onIntent} />); result.unmount(); settle(60_000);
      expect(onIntent).toHaveBeenCalledTimes(3);
    } finally { vi.useRealTimers(); }
  });
  it('renders malformed provider methods without exposing an active form', () => {
    render(<ProviderConnectDialog {...chrome} view={{ ...keyView, provider: { ...providers[0], methods: null! } }} onIntent={vi.fn()} />);
    expect(screen.getByRole('alert')).toHaveTextContent('This connection can’t be shown');
    expect(screen.queryByLabelText('API key')).not.toBeInTheDocument();
  });
  it('ignores IME Escape, then goes Back without closing or emitting input', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const onDismiss = vi.fn(); render(<ProviderConnectDialog onDismiss={onDismiss} view={keyView} onIntent={onIntent} />);
    await user.type(screen.getByLabelText('API key'), 'ak-test-5f2c9e81b7d04a63'); fireEvent.keyDown(screen.getByLabelText('API key'), { key: 'Escape', isComposing: true }); expect(onIntent).not.toHaveBeenCalled();
    fireEvent.keyDown(screen.getByLabelText('API key'), { key: 'Escape' }); expect(onIntent).toHaveBeenCalledWith({ id: 'connect', version: 1, action: 'back' }); expect(onDismiss).not.toHaveBeenCalled();
  });
  it('opens a provider with one sign-in method on that method, where Back leaves rather than showing a one-item list', async () => {
    const openai = byId('openai'); const harbor = byId('harbor');
    expect(connectStart(openai)).toEqual({ step: 'key', method: 'key' });
    expect(connectStart(byId('aster-lab'))).toEqual({ step: 'method', method: null });
    expect(connectStart(harbor)).toEqual({ step: 'error', method: 'key' });
    const back = { id: 'connect', version: 1, action: 'back' } as const;
    for (const step of ['key', 'error', 'canceled'] as const) expect(validConnectIntent(at(step, 'key', openai), back)).toBe(false);
    expect(validConnectIntent(at('waiting', 'key', openai), back)).toBe(true);
    expect(validConnectIntent(at('error', 'key'), back)).toBe(true);

    const user = userEvent.setup(); const onIntent = vi.fn(); const onDismiss = vi.fn();
    const result = render(<ProviderConnectDialog onDismiss={onDismiss} autoAdvance={false} view={at('key', 'key', openai)} onIntent={onIntent} />);
    expect(screen.getByRole('dialog', { name: 'Connect OpenAI' })).toHaveAccessibleDescription('Use a key from your OpenAI account.');
    expect(screen.queryByRole('list', { name: 'Sign-in methods' })).not.toBeInTheDocument();
    expect(screen.getByLabelText('API key')).toHaveFocus(); expect(screen.queryByRole('button', { name: 'Back' })).not.toBeInTheDocument();
    await user.keyboard('{Escape}'); expect(onDismiss).toHaveBeenCalledTimes(1); expect(onIntent).not.toHaveBeenCalled();
    // Waiting still steps back to the key form.
    result.unmount(); render(<ProviderConnectDialog onDismiss={onDismiss} autoAdvance={false} view={at('waiting', 'key', openai)} onIntent={onIntent} />);
    await user.click(screen.getByRole('button', { name: 'Back' })); expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'connect', version: 1, action: 'back' });
  });
  it('offers each method as one choice with Cancel in the footer and no Back on the first step', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); render(<ProviderConnectDialog {...chrome} view={at('method', null)} onIntent={onIntent} />);
    const dialog = screen.getByRole('dialog', { name: 'Connect Aster Lab' });
    const list = within(dialog).getByRole('list', { name: 'Sign-in methods' });
    expect(list).toHaveTextContent('API keyPaste a key from your Aster Lab account.');
    const cancel = within(dialog).getByRole('button', { name: 'Cancel' }); expect(cancel).toBeVisible(); expect(list.parentElement).not.toContainElement(cancel);
    expect(within(dialog).queryByRole('button', { name: 'Back' })).not.toBeInTheDocument();
    expect(statusOf('Not connected')).toHaveAttribute('data-status', 'unavailable');
    expect(dialog).not.toHaveTextContent(/example|supplied|rehearsal|fictional/i);
    await user.click(within(dialog).getByRole('button', { name: /Sign in with a code/ })); expect(onIntent).toHaveBeenCalledWith({ id: 'connect', version: 1, action: 'choose-method', method: 'device' });
  });
  it('marks only the steps that need the user as Attention, verification as Working, and offers no outcome controls', () => {
    const onIntent = vi.fn();
    const result = render(<ProviderConnectDialog {...chrome} view={at('browser', 'browser')} onIntent={onIntent} />);
    expect(statusOf('Waiting for you')).toHaveAttribute('data-status', 'approval');
    expect(screen.getByText('Finish signing in to Aster Lab in your browser. AVA continues on its own when you’re done.')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Cancel' })).toHaveFocus();
    expect(screen.queryByRole('button', { name: /signed in|Mark as/ })).not.toBeInTheDocument();
    result.rerender(<ProviderConnectDialog {...chrome} view={{ ...at('waiting', 'key'), version: 2 }} onIntent={onIntent} />);
    expect(statusOf('Verifying key…')).toHaveAttribute('data-status', 'working'); expect(screen.queryByText('Waiting for you')).not.toBeInTheDocument();
    expect(screen.getByText('Checking the key with Aster Lab.')).toBeVisible();
    expect(screen.queryByRole('group', { name: /outcome/i })).not.toBeInTheDocument(); expect(screen.getByRole('dialog')).not.toHaveTextContent(/preview|Mark as/i);
    expect(screen.getByRole('button', { name: 'Back' })).toBeVisible(); expect(screen.getByRole('button', { name: 'Cancel' })).toBeVisible();
    expect(onIntent).not.toHaveBeenCalled();
  });
  it('shows a device code with Copy, a plain instruction and its expiry', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); render(<ProviderConnectDialog {...chrome} view={at('device', 'device')} onIntent={onIntent} />);
    expect(screen.getByText('KQRT-4829')).toBeVisible(); expect(screen.getByText('The code expires in 15 minutes.')).toBeVisible();
    expect(screen.getByText(/open Aster Lab’s sign-in page and enter this code\. AVA continues on its own once it’s entered\./)).toBeVisible();
    expect(screen.getByRole('button', { name: 'Copy code' })).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Copy code' }));
    expect(await screen.findByRole('button', { name: 'Copied' })).toBeVisible(); expect(await navigator.clipboard.readText()).toBe('KQRT-4829');
    expect(screen.queryByRole('button', { name: /entered the code/ })).not.toBeInTheDocument(); expect(onIntent).not.toHaveBeenCalled();
    for (const button of screen.getAllByRole('button')) expect(button).toBeEnabled();
  });
  it('ends a successful connection with Choose a model, never Cancel', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const onDismiss = vi.fn(); render(<ProviderConnectDialog onDismiss={onDismiss} view={at('success', 'key')} onIntent={onIntent} />);
    expect(statusOf('Connected')).toHaveAttribute('data-status', 'completed'); expect(screen.getByText('Aster Lab is connected. Aster models are ready to use.')).toBeVisible();
    expect(screen.queryByRole('button', { name: 'Cancel' })).not.toBeInTheDocument(); expect(screen.queryByRole('button', { name: 'Back' })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Choose a model' })).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Choose a model' })); expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'connect', version: 1, action: 'choose-model' });
  });
  it('leads with Done while Getting started has steps left, keeping Choose a model beside it', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const onDismiss = vi.fn();
    render(<ProviderConnectDialog onDismiss={onDismiss} view={at('success', 'key')} onIntent={onIntent} settingUp />);
    const done = screen.getByRole('button', { name: 'Done' });
    expect(done).toHaveFocus(); expect(done).toHaveAttribute('data-tone', 'primary');
    expect(screen.getByRole('button', { name: 'Choose a model' })).toHaveAttribute('data-tone', 'quiet');
    // The primary action sits last, at the bar's end.
    expect(done.compareDocumentPosition(screen.getByRole('button', { name: 'Choose a model' })) & Node.DOCUMENT_POSITION_PRECEDING).toBeTruthy();
    await user.click(screen.getByRole('button', { name: 'Choose a model' })); expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'connect', version: 1, action: 'choose-model' });
  });
  it('names the cause of a failed connection and offers Try again', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const harbor = byId('harbor');
    const result = render(<ProviderConnectDialog {...chrome} view={at('error', 'key', harbor)} onIntent={onIntent} />);
    expect(statusOf('Key not accepted')).toHaveAttribute('data-status', 'failed');
    expect(screen.getByText('Harbor couldn’t connect. The API key expired. Enter a new key to reconnect.')).toBeVisible();
    expect(screen.getByRole('button', { name: 'Try again' })).toHaveFocus(); expect(screen.queryByRole('button', { name: 'Back' })).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Try again' })); expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'connect', version: 1, action: 'retry' });
    const failedAster = { ...byId('aster-lab'), state: 'error' as const, description: 'The sign-in expired.' };
    result.rerender(<ProviderConnectDialog {...chrome} view={{ ...at('method', null, failedAster), version: 2 }} onIntent={onIntent} />);
    expect(screen.getByText('The last connection to Aster Lab failed. The sign-in expired. Connect again to restore it.')).toBeVisible();
    result.rerender(<ProviderConnectDialog {...chrome} view={{ ...at('error', 'key'), version: 3 }} onIntent={onIntent} />);
    expect(screen.getByText('Aster Lab didn’t accept the API key. Check the key and try again.')).toBeVisible();
  });
  it('states what a disconnect changes, opens on Cancel, and uses the destructive tone', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn(); const onDismiss = vi.fn();
    const view = render(<ProviderConnectDialog onDismiss={onDismiss} view={at('disconnect', null, byId('orion-lab'))} onIntent={onIntent} />);
    expect(screen.getByRole('dialog', { name: 'Disconnect Orion Lab?' })).toHaveTextContent('Chats using Orion models can’t send until you reconnect.');
    expect(screen.getByRole('button', { name: 'Cancel' })).toHaveFocus();
    const disconnect = screen.getByRole('button', { name: 'Disconnect' }); expect(disconnect).toHaveAttribute('data-tone', 'destructive');
    await user.click(screen.getByRole('button', { name: 'Cancel' })); expect(onDismiss).toHaveBeenCalledTimes(1); expect(onIntent).not.toHaveBeenCalled();
    view.unmount();
    render(<ProviderConnectDialog onDismiss={vi.fn()} view={at('disconnect', null, byId('orion-lab'))} onIntent={onIntent} />);
    await user.click(screen.getByRole('button', { name: 'Disconnect' })); expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'connect', version: 1, action: 'disconnect' });
  });
  it('shows an externally managed provider as a status, not a disabled action', () => {
    render(<ProviderConnectDialog {...chrome} view={at('method', null, byId('cedar'))} onIntent={vi.fn()} />);
    expect(statusOf('Managed externally')).toHaveAttribute('data-status', 'completed');
    for (const button of within(screen.getByRole('dialog')).getAllByRole('button')) expect(button).toBeEnabled();
    expect(screen.queryByRole('button', { name: /Connect/ })).not.toBeInTheDocument();
  });
});
