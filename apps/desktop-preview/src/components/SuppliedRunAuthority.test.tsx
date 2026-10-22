import { StrictMode } from 'react';
import { act, fireEvent, render, screen } from '@testing-library/react';
import { expect, it, vi } from 'vitest';
import type { RunEvent, RunIdentity } from '../state/suppliedRun';
import { prepareAssistantContent } from '../chat/widgets/AssistantContentHost';
import { AppShell } from './AppShell';
import { installDialogTestSupport } from '../screens/dialogTestSupport';

const captures = vi.hoisted(() => [] as { identity: RunIdentity; deliver: (events: readonly RunEvent[]) => void }[]);
vi.mock('./SuppliedRunPreview', async (original) => {
  const actual = await original<typeof import('./SuppliedRunPreview')>();
  return { ...actual, createRunDriver: (identity: RunIdentity, _snapshots: unknown, deliver: (events: readonly RunEvent[]) => void) => {
    captures.push({ identity, deliver }); return { start() {}, cancel() {}, pause() {}, resume() {}, step() {} };
  } };
});

it('root lifetime/current scope and core order independently reject direct late callbacks even when a fixture driver fails to revoke them', async () => {
  installDialogTestSupport(); await prepareAssistantContent(); await import('./SuppliedRunPreview');
  const view = render(<StrictMode><AppShell /></StrictMode>);
  await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Run preview' })); });
  const first = captures[0]; const event = (sequence: number, delta: string): RunEvent => ({ ...first.identity, sequence, kind: 'text', delta });
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Draft changed while delivery is pending' } });
  act(() => first.deliver([event(1, 'Accepted despite unrelated draft edit.')]));
  const old = screen.getAllByRole('article', { name: 'AVA response' }).at(-1)!; expect(old).toHaveTextContent('Accepted despite unrelated draft edit.');
  act(() => { first.deliver([event(1, 'duplicate')]); first.deliver([event(3, 'gap')]); first.deliver([{ ...event(2, 'wrong chat'), chatId: 'other' }]); });
  expect(old.textContent).not.toMatch(/duplicate|gap|wrong chat/);
  await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Settings' })); });
  const stopped = old.textContent; act(() => first.deliver([event(2, 'hidden write')])); expect(old.textContent).toBe(stopped); expect(old.closest('[hidden][inert]')).not.toBeNull();
  fireEvent.click(await screen.findByRole('button', { name: 'Back to chat' }));
  await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Retry preview' })); });
  const second = captures[1]; expect(second.identity.runId).not.toBe(first.identity.runId); expect(second.identity.generation).toBe(first.identity.generation + 1);
  act(() => { first.deliver([event(2, 'stale generation')]); second.deliver([{ ...second.identity, sequence: 1, kind: 'text', delta: 'New identity only.' }]); });
  expect(old.textContent).toBe(stopped); expect(screen.getAllByRole('article', { name: 'AVA response' })[2]).toHaveTextContent('New identity only.');
  fireEvent.click(screen.getByRole('button', { name: 'Stop preview' }));
  const newTurn = screen.getAllByRole('article', { name: 'AVA response' })[2]; const text = newTurn.textContent;
  act(() => second.deliver([{ ...second.identity, sequence: 2, kind: 'complete' }])); expect(newTurn.textContent).toBe(text); expect(newTurn).toHaveAttribute('data-turn-state', 'canceled');
  view.unmount(); act(() => second.deliver([{ ...second.identity, sequence: 2, kind: 'text', delta: 'unmounted' }])); expect(screen.queryByRole('main')).not.toBeInTheDocument();
});
