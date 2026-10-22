import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import ComponentCatalog from './ComponentCatalog';
import { toolChangeCases } from './examples';

describe('browser-only component catalog', () => {
  it('mounts every component family with plain titles and no shell navigation or execution/network actions', () => {
    const fetch = vi.fn(); const socket = vi.fn(); const reader = vi.fn();
    vi.stubGlobal('fetch', fetch); vi.stubGlobal('WebSocket', socket); vi.stubGlobal('FileReader', reader);
    render(<ComponentCatalog />);
    expect(screen.getByRole('main', { name: 'Frontend component catalog' })).toHaveAttribute('data-material', 'opaque');
    const index = within(screen.getByRole('navigation', { name: 'Component sections' }));
    for (const name of ['Foundations', 'Turns', 'Tools', 'Permissions', 'Questions', 'Run notices', 'Composer', 'Changes']) {
      expect(screen.getByRole('heading', { level: 2, name })).toBeInTheDocument();
      expect(index.getByRole('link', { name })).toHaveAttribute('href', `#${screen.getByRole('region', { name }).id}`);
    }
    expect(screen.queryByText(/^0\d$/)).not.toBeInTheDocument();
    expect(screen.getByRole('link', { name: 'Back to chat' })).toHaveAttribute('href', '?');
    expect(screen.getByRole('button', { name: 'Unavailable action' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Add item' }));
    expect(screen.getByText('Icon button received. Answers aren’t recorded.')).toBeInTheDocument();
    expect(screen.queryByRole('navigation', { name: 'All project chats' })).not.toBeInTheDocument();
    expect(screen.getByText(/nothing is connected, saved, granted or run/)).toBeInTheDocument();
    const chrome = [screen.getByRole('heading', { level: 1 }).parentElement!, ...document.querySelectorAll('section > header')].map((node) => node.textContent).join(' ');
    expect(chrome).not.toMatch(/RPC|\bintent\b|fixture|supplied|invented/i);
    expect(document.querySelector('input[type="file"], img')).toBeNull();
    expect(fetch).not.toHaveBeenCalled(); expect(socket).not.toHaveBeenCalled(); expect(reader).not.toHaveBeenCalled();
  });

  it('keeps tool bodies opt-in and exposes every inline diff case without per-file actions', async () => {
    const user = userEvent.setup(); render(<ComponentCatalog />);
    const tools = within(screen.getByRole('region', { name: 'Tools' }));
    expect(tools.getByRole('button', { name: /^\d+ tools?\b/ })).toHaveAttribute('aria-expanded', 'false');
    expect(tools.queryByLabelText(/^Output:/)).not.toBeInTheDocument();
    const select = tools.getByRole('combobox', { name: 'Tool example' }); expect(within(select).getAllByRole('option')).toHaveLength(13);
    for (const scenario of Object.keys(toolChangeCases)) {
      await user.selectOptions(select, scenario);
      const trigger = tools.getByRole('button', { name: /^\d+ tools?\b/ }); expect(trigger).toHaveAttribute('aria-expanded', 'false');
      await user.click(trigger); expect(await tools.findByLabelText('Output: Update note')).toBeVisible();
      if (scenario === 'diff-malformed' || scenario === 'diff-duplicate') expect(tools.getByRole('status')).toHaveTextContent('File changes unavailable');
      else if (scenario === 'diff-missing') expect(tools.queryByRole('region', { name: /^File change:/ })).not.toBeInTheDocument();
      else expect(tools.getByRole('region', { name: /^File change:/ })).toBeVisible();
      expect(tools.getAllByRole('button')).toHaveLength(1);
    }
  });

  it('records only local receipts, blocks repeated requests, issues new versions explicitly, and resets values with focus retained', async () => {
    const user = userEvent.setup();
    render(<ComponentCatalog />);
    const permission = screen.getByRole('button', { name: 'Allow once' });
    await user.click(permission);
    expect(permission).toHaveAttribute('aria-pressed', 'true'); expect(permission).toHaveAttribute('aria-disabled', 'true');
    expect(screen.getByRole('button', { name: 'Deny' })).toBeDisabled();
    expect(screen.getByText('Permission choice received. Answers aren’t recorded.')).toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'New request' }));
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeEnabled(); expect(screen.getByRole('button', { name: 'Allow once' })).not.toHaveAttribute('aria-pressed', 'true');
    await user.selectOptions(screen.getByRole('combobox', { name: 'Question example' }), 'secret');
    const secret = screen.getByLabelText('Access token');
    expect(secret).toHaveAttribute('type', 'password');
    await user.type(secret, 'invented-secret');
    await user.click(screen.getByRole('button', { name: 'Submit answer' }));
    expect(screen.getByText('Question answer received. Answers aren’t recorded.')).toBeInTheDocument();
    expect(document.body.textContent).not.toContain('invented-secret');
    fireEvent.change(screen.getByRole('textbox', { name: 'Message draft for Catalog draft' }), { target: { value: 'Changed local draft' } });
    const reset = screen.getByRole('button', { name: 'Reset examples' });
    await user.click(reset);
    expect(reset).toHaveFocus();
    expect(screen.getByText('Nothing received yet.')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeEnabled();
    expect(screen.getByRole('textbox', { name: 'Message draft for Catalog draft' })).toHaveValue('Keep the next answer concise.');
    expect(screen.queryByLabelText('Access token')).not.toBeInTheDocument();
  });

  it('exposes lifecycle and resource variants without pretending retry or Stop settled a run', async () => {
    const user = userEvent.setup();
    render(<ComponentCatalog />);
    await user.selectOptions(screen.getByRole('combobox', { name: 'Turn example' }), 'canceled');
    expect(screen.getByRole('article', { name: 'Stopped AVA response' })).toHaveTextContent('Stopped · reply incomplete');
    await user.selectOptions(screen.getByRole('combobox', { name: 'Permission example' }), 'session');
    expect(screen.getByRole('button', { name: 'Allow for this session' })).toBeEnabled();
    await user.selectOptions(screen.getByRole('combobox', { name: 'Permission example' }), 'stale');
    expect(screen.getByRole('button', { name: 'Allow once' })).toBeDisabled();
    await user.click(within(screen.getByRole('region', { name: 'Run notices' })).getByRole('button', { name: 'Retry' }));
    expect(screen.getByText('Retry received. Answers aren’t recorded.')).toBeInTheDocument();
    expect(screen.getByRole('combobox', { name: 'Run example' })).toHaveValue('error');
    await user.selectOptions(screen.getByRole('combobox', { name: 'Composer example' }), 'running');
    await user.click(screen.getByRole('button', { name: 'Request stop' }));
    expect(screen.getByRole('combobox', { name: 'Composer example' })).toHaveValue('running');
    expect(screen.getByText('Composer stop received. Answers aren’t recorded.')).toBeInTheDocument();
    const changeSection = within(screen.getByRole('region', { name: 'Changes' }));
    expect(changeSection.getByRole('region', { name: 'File change: src/layout.ts' })).toBeVisible();
    await user.selectOptions(changeSection.getByRole('combobox', { name: 'Changes example' }), 'loading');
    expect(changeSection.getByRole('status')).toHaveTextContent('Loading changes…');
    await user.selectOptions(changeSection.getByRole('combobox', { name: 'Changes example' }), 'stale');
    expect(changeSection.getByText('These changes may be out of date.')).toBeInTheDocument();
    const ids = Array.from(document.querySelectorAll('[id]'), (entry) => entry.id);
    expect(new Set(ids).size).toBe(ids.length);
  });
});
