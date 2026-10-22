import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, expect, it, vi } from 'vitest';
import { installDialogTestSupport } from '../screens/dialogTestSupport';
import { AppShell } from './AppShell';

const lazy = vi.hoisted(() => {
  let resolve!: () => void;
  const ready = new Promise<void>((done) => { resolve = done; });
  return { ready, resolve, loads: 0, reviewLoads: 0 };
});
vi.mock('./DetailInspector', async (original) => {
  lazy.loads++;
  await lazy.ready;
  return original<typeof import('./DetailInspector')>();
});
vi.mock('../screens/ScreenReview', () => { lazy.reviewLoads++; return { default: () => null }; });
vi.mock('../catalog/ComponentCatalog', () => { lazy.reviewLoads++; return { default: () => null }; });
beforeEach(installDialogTestSupport);

it('loads optional inspector content on demand, using the latest visible scope without late focus or stale content', async () => {
  const user = userEvent.setup(); render(<AppShell />);
  expect(lazy.loads).toBe(0); expect(lazy.reviewLoads).toBe(0);
  const title = screen.getByRole('heading', { level: 1 }); const focus = vi.spyOn(title, 'focus');
  await user.click(screen.getByRole('button', { name: /^Open Context/ }));
  expect(lazy.loads).toBe(1);
  expect(within(screen.getByRole('complementary', { name: 'Context inspector' })).getByRole('status')).toHaveTextContent('Loading Context');
  await user.click(screen.getByRole('button', { name: 'Close context' }));
  expect(screen.queryByRole('complementary', { name: 'Context inspector' })).not.toBeInTheDocument();
  expect(screen.getByRole('button', { name: 'Context' })).toHaveFocus();
  await user.click(screen.getByRole('button', { name: 'Changes' }));
  await user.click(within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Attention Export date range' }));
  await user.click(screen.getByRole('button', { name: 'Settings' }));
  const search = await screen.findByRole('searchbox', { name: 'Search settings' });
  await user.type(search, 'model');
  await act(async () => { lazy.resolve(); await lazy.ready; });
  expect(search).toHaveFocus(); expect(lazy.reviewLoads).toBe(0); expect(focus).not.toHaveBeenCalled();
  expect(screen.queryByRole('complementary', { name: 'Mock changes' })).not.toBeInTheDocument();
  await user.click(screen.getByRole('button', { name: 'Back to chat' }));
  const changes = screen.getByRole('complementary', { name: 'Mock changes' });
  expect(await within(changes).findByText('No changes in this chat.')).toBeVisible();
  expect(changes).not.toHaveTextContent('search-state.ts');
  await user.click(screen.getByRole('button', { name: 'Context' }));
  await user.click(screen.getByRole('button', { name: 'Raw messages' }));
  const raw = screen.getByRole('region', { name: 'Local committed messages' });
  expect(raw).toHaveTextContent('export');
  const draft = screen.getByRole('textbox'); draft.focus(); fireEvent.change(draft, { target: { value: 'Keep this draft' } });
  expect(draft).toHaveFocus(); expect(screen.getByRole('region', { name: 'Local committed messages' })).toBe(raw);
  expect(lazy.loads).toBe(1); expect(focus).not.toHaveBeenCalled();
});
