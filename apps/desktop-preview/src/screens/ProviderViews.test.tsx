import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { ConnectView } from './contracts';
import { installDialogTestSupport } from './dialogTestSupport';
import { ProviderConnectDialog, ProviderDirectory, validConnectIntent, validProvider } from './ProviderViews';
import { providers } from './screenExamples';

beforeEach(installDialogTestSupport);
describe('read-only brand catalog and isolated connection examples', () => {
  it('shows 15 real identity cards without auth/state claims and initially hides all four manual demos', async () => {
    const spies = ['fetch', 'XMLHttpRequest', 'WebSocket', 'FileReader', 'showOpenFilePicker', 'open'].map((name) => { const spy = vi.fn(); vi.stubGlobal(name, spy); return spy; });
    const onConnect = vi.fn(); const onDisconnect = vi.fn(); const user = userEvent.setup();
    render(<ProviderDirectory providers={providers} onConnect={onConnect} onDisconnect={onDisconnect} />);
    const brands = screen.getByRole('list', { name: 'Provider brands' });
    expect(within(brands).getAllByRole('listitem')).toHaveLength(15);
    expect(within(brands).queryByRole('button')).not.toBeInTheDocument(); expect(within(brands).queryByRole('link')).not.toBeInTheDocument();
    expect(brands).not.toHaveTextContent(/connected|configured|key|OAuth|available|Aster|Orion/i);
    for (const provider of providers) expect(screen.queryByRole('heading', { name: provider.label })).not.toBeInTheDocument();
    const disclosure = screen.getByRole('button', { name: 'Connection examples' }); expect(disclosure).toHaveAttribute('aria-expanded', 'false');
    await user.click(disclosure);
    for (const provider of providers) expect(screen.getByRole('heading', { name: provider.label })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Connect example' })); expect(onConnect).toHaveBeenLastCalledWith('aster-lab');
    await user.click(screen.getByRole('button', { name: 'Disconnect example' })); expect(onDisconnect).toHaveBeenLastCalledWith('orion-lab');
    const external = screen.getByRole('button', { name: 'Managed externally' }); expect(external).toBeDisabled(); fireEvent.click(external);
    await user.click(screen.getByRole('button', { name: 'Review connection' })); expect(onConnect).toHaveBeenLastCalledWith('harbor-example');
    expect(onConnect).toHaveBeenCalledTimes(2); expect(onDisconnect).toHaveBeenCalledTimes(1);
    for (const spy of spies) expect(spy).not.toHaveBeenCalled();
  });
  it('does not grant demo authority to real, unknown or relabeled IDs', () => {
    for (const id of ['openai', 'anthropic', 'zai', 'zai-coding-cn', 'xai', 'constructor', 'unknown']) {
      const provider = { ...providers[0], id };
      expect(validProvider(provider)).toBe(false);
      expect(validConnectIntent({ id: 'demo', version: 1, provider, step: 'method', method: null }, { id: 'demo', version: 1, action: 'choose-method', method: 'key' })).toBe(false);
    }
    expect(validProvider({ ...providers[0], label: 'OpenAI' })).toBe(false);
    expect(validProvider({ ...providers[2], source: 'stored-example', state: 'connected-example' })).toBe(false);
  });
  it.each(providers)('preserves the advertised methods and credential scope for $id', (provider) => {
    const view: ConnectView = { id: 'demo', version: 1, provider, step: 'method', method: null };
    expect(validProvider(provider)).toBe(true);
    for (const method of ['key', 'browser', 'device'] as const) expect(validConnectIntent(view, { id: view.id, version: 1, action: 'choose-method', method })).toBe(provider.methods.includes(method));
    expect(validConnectIntent({ ...view, step: 'disconnect' }, { id: view.id, version: 1, action: 'disconnect' })).toBe(provider.source === 'stored-example');
    const { container } = render(<ProviderConnectDialog view={view} onIntent={vi.fn()} onDismiss={vi.fn()} />);
    expect(container.querySelector('[data-provider-mark]')).toHaveAttribute('data-provider-mark', 'neutral');
  });
  it('rejects retained controls while inactive or after the example disclosure closes', async () => {
    const user = userEvent.setup(); const onConnect = vi.fn(); const props = { providers, onConnect, onDisconnect: vi.fn() };
    const view = render(<ProviderDirectory {...props} />);
    await user.click(screen.getByRole('button', { name: 'Connection examples' }));
    const button = screen.getByRole('button', { name: 'Connect example' });
    view.rerender(<ProviderDirectory {...props} active={false} />); fireEvent.click(button); expect(onConnect).not.toHaveBeenCalled();
    view.rerender(<ProviderDirectory {...props} />);
    await user.click(screen.getByRole('button', { name: 'Connection examples' })); fireEvent.click(button); expect(onConnect).not.toHaveBeenCalled();
  });
});
