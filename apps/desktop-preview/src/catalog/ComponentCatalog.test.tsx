import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import ComponentCatalog from './ComponentCatalog';
import { toolChangeCases } from './examples';

describe('browser-only component catalog', () => {
  it('mounts useful fixture families without shell navigation or execution/network actions', () => {
    const fetch = vi.fn(); const socket = vi.fn(); const reader = vi.fn();
    vi.stubGlobal('fetch', fetch); vi.stubGlobal('WebSocket', socket); vi.stubGlobal('FileReader', reader);
    render(<ComponentCatalog />);
    expect(screen.getByRole('main', { name: 'Frontend component catalog' })).toHaveAttribute('data-material', 'opaque');
    for (const name of ['Foundations', 'Turns', 'Tools', 'Permissions', 'Questions', 'Run notices', 'Composer', 'Changes']) expect(screen.getByRole('heading', { name })).toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'Back to chat' })).toHaveAttribute('href', '?');
    expect(screen.getByRole('button', { name: 'Unavailable action' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Record foundation intent' }));
    expect(screen.getByText(/Icon button: local fixture intent only/)).toBeInTheDocument();
    expect(screen.queryByRole('navigation', { name: 'All project chats' })).not.toBeInTheDocument();
    expect(screen.getByText(/Nothing is connected, saved, granted/)).toBeInTheDocument();
    expect(document.querySelector('input[type="file"], img')).toBeNull();
    expect(fetch).not.toHaveBeenCalled(); expect(socket).not.toHaveBeenCalled(); expect(reader).not.toHaveBeenCalled();
  });

  it('keeps tool bodies opt-in and exposes every supplied inline diff case without per-file actions', async () => {
    const user = userEvent.setup(); render(<ComponentCatalog />);
    const tools = within(screen.getByRole('region', { name: 'Tools' }));
    expect(tools.getByRole('button', { name: /^Tool details/ })).toHaveAttribute('aria-expanded', 'false');
    expect(tools.queryByLabelText(/^Output:/)).not.toBeInTheDocument();
    const select = tools.getByRole('combobox', { name: 'Tool example' }); expect(within(select).getAllByRole('option')).toHaveLength(13);
    for (const scenario of Object.keys(toolChangeCases)) {
      await user.selectOptions(select, scenario);
      const trigger = tools.getByRole('button', { name: /^Tool details/ }); expect(trigger).toHaveAttribute('aria-expanded', 'false');
      await user.click(trigger); expect(await tools.findByLabelText('Output: Update note')).toBeVisible();
      if (scenario === 'diff-malformed' || scenario === 'diff-duplicate') expect(tools.getByRole('status')).toHaveTextContent('File changes unavailable');
      else if (scenario === 'diff-missing') expect(tools.queryByRole('region', { name: /^File change:/ })).not.toBeInTheDocument();
      else expect(tools.getByRole('region', { name: /^File change:/ })).toBeVisible();
      expect(tools.getAllByRole('button')).toHaveLength(1);
    }
  });

  it('records only local receipts, blocks repeated requests, revises versions explicitly, and resets values with focus retained', async () => {
    const user = userEvent.setup();
    render(<ComponentCatalog />);
    const permission = screen.getByRole('button', { name: 'Allow once' });
    await user.click(permission);
    expect(permission).toBeDisabled();
    expect(screen.getByText(/Permission choice: local fixture intent only/)).toHaveTextContent('No policy changed, resolver settled, or command executed');
    await user.click(screen.getByRole('button', { name: 'Revise permission fixture' }));
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeEnabled();
    await user.selectOptions(screen.getByRole('combobox', { name: 'Question example' }), 'secret');
    const secret = screen.getByLabelText('Secret example answer');
    expect(secret).toHaveAttribute('type', 'password');
    await user.type(secret, 'invented-secret');
    await user.click(screen.getByRole('button', { name: 'Submit answer' }));
    expect(screen.getByText(/Question answer: local fixture intent only/)).toBeInTheDocument();
    expect(document.body.textContent).not.toContain('invented-secret');
    fireEvent.change(screen.getByRole('textbox', { name: 'Message draft for Catalog draft' }), { target: { value: 'Changed local draft' } });
    const reset = screen.getByRole('button', { name: 'Reset examples' });
    await user.click(reset);
    expect(reset).toHaveFocus();
    expect(screen.getByText('No local intent received.')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeEnabled();
    expect(screen.getByRole('textbox', { name: 'Message draft for Catalog draft' })).toHaveValue('Keep the next answer concise.');
    expect(screen.queryByLabelText('Secret example answer')).not.toBeInTheDocument();
  });

  it('exposes lifecycle/resource variants without pretending retry or Stop settled a run', async () => {
    const user = userEvent.setup();
    render(<ComponentCatalog />);
    await user.selectOptions(screen.getByRole('combobox', { name: 'Turn example' }), 'canceled');
    expect(screen.getByRole('article', { name: 'Canceled assistant example' })).toHaveTextContent('Partial message · canceled');
    await user.selectOptions(screen.getByRole('combobox', { name: 'Permission example' }), 'session');
    expect(screen.getByRole('button', { name: 'Allow for session' })).toBeEnabled();
    await user.selectOptions(screen.getByRole('combobox', { name: 'Permission example' }), 'stale');
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeDisabled();
    await user.click(screen.getByRole('button', { name: 'Request retry' }));
    expect(screen.getByText(/Retry request: local fixture intent only/)).toBeInTheDocument();
    expect(screen.getByRole('combobox', { name: 'Run example' })).toHaveValue('error');
    await user.selectOptions(screen.getByRole('combobox', { name: 'Composer example' }), 'running');
    await user.click(screen.getByRole('button', { name: 'Request stop' }));
    expect(screen.getByRole('combobox', { name: 'Composer example' })).toHaveValue('running');
    expect(screen.getByText(/Composer stop: local fixture intent only/)).toBeInTheDocument();
    const changeSection = screen.getByRole('region', { name: 'Changes' });
    await user.selectOptions(screen.getByRole('combobox', { name: 'Changes example' }), 'loading');
    expect(within(changeSection).getByRole('status')).toHaveTextContent('Loading change excerpts');
    await user.selectOptions(screen.getByRole('combobox', { name: 'Changes example' }), 'stale');
    expect(within(changeSection).getByText(/Last supplied excerpts · stale/)).toBeInTheDocument();
    const ids = Array.from(document.querySelectorAll('[id]'), (entry) => entry.id);
    expect(new Set(ids).size).toBe(ids.length);
  });
});
