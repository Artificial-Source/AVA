import { StrictMode } from 'react';
import { act, fireEvent, render, screen } from '@testing-library/react';
import { expect, it, vi } from 'vitest';
import { AppShell } from './AppShell';
import { prepareAssistantContent } from '../chat/widgets/AssistantContentHost';
import { installDialogTestSupport } from '../screens/dialogTestSupport';

const pending = vi.hoisted(() => {
  let resolve!: () => void; const ready = new Promise<void>((done) => { resolve = done; });
  return { ready, resolve, loads: 0 };
});
vi.mock('./SuppliedRunPreview', async (original) => { pending.loads++; await pending.ready; return original<typeof import('./SuppliedRunPreview')>(); });

it('activation-loads the whole fixture/controller only; a hidden, departed or unmounted activation has no authority after resolution', async () => {
  installDialogTestSupport(); await prepareAssistantContent();
  const view = render(<StrictMode><AppShell /></StrictMode>); expect(pending.loads).toBe(0);
  const start = screen.getByRole('button', { name: 'Run preview' }); const editor = screen.getByRole('textbox');
  await act(async () => { fireEvent.click(start); }); expect(pending.loads).toBe(1); expect(start).toHaveTextContent('Loading preview…');
  await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Settings' })); });
  fireEvent.click(start); expect(start.closest('[hidden][inert]')).not.toBeNull();
  await act(async () => { pending.resolve(); await pending.ready; });
  expect(document.querySelector('[data-turn-state="streaming"]')).toBeNull();
  fireEvent.click(await screen.findByRole('button', { name: 'Back to chat' })); expect(screen.getByRole('textbox')).toBe(editor);
  expect(screen.queryByRole('combobox', { name: 'Preview example' })).not.toBeInTheDocument();
  await act(async () => { fireEvent.click(start); }); expect(await screen.findByRole('button', { name: 'Stop preview' })).toBeVisible();
  view.unmount(); fireEvent.click(start); expect(screen.queryByRole('main')).not.toBeInTheDocument(); expect(pending.loads).toBe(1);
});
