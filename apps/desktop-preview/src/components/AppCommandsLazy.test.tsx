import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { beforeEach, expect, it, vi } from 'vitest';
import { installDialogTestSupport } from '../screens/dialogTestSupport';
import { AppShell } from './AppShell';

const pending = vi.hoisted(() => { let resolve!: () => void; const ready = new Promise<void>((done) => { resolve = done; }); return { ready, resolve, loads: 0, calls: 0 }; });
vi.mock('./AppCommands', async (original) => {
  pending.loads++; await pending.ready; const actual = await original<typeof import('./AppCommands')>();
  return {
    runAppCommand: (...args: Parameters<typeof actual.runAppCommand>) => { pending.calls++; actual.runAppCommand(...args); },
    runAppAction: (...args: Parameters<typeof actual.runAppAction>) => { pending.calls++; return actual.runAppAction(...args); },
  };
});
beforeEach(installDialogTestSupport);

it('loads the whole keyboard owner only on activation, rejects pending repeats and discards an intent after target replacement', async () => {
  render(<AppShell />); expect(pending.loads).toBe(0);
  const transcript = screen.getByRole('region', { name: /^Conversation:/ }); transcript.focus();
  fireEvent.keyDown(transcript, { key: '9', code: 'Digit9', ctrlKey: true, shiftKey: true });
  await waitFor(() => expect(pending.loads).toBe(1));
  fireEvent.keyDown(transcript, { key: '9', code: 'Digit9', ctrlKey: true, shiftKey: true });
  expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
  fireEvent.click(within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Export date range Attention' }));
  await act(async () => { pending.resolve(); await import('./AppCommands'); });
  expect(pending.calls).toBe(0);
  expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range'); expect(screen.queryByText('New chat 1')).not.toBeInTheDocument();
  const current = screen.getByRole('region', { name: /^Conversation:/ }); current.focus(); fireEvent.keyDown(current, { key: '9', code: 'Digit9', ctrlKey: true, shiftKey: true });
  await waitFor(() => expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1'));
  expect(screen.queryByText('New chat 2')).not.toBeInTheDocument();
});
