import { act, fireEvent, render, screen } from '@testing-library/react';
import { expect, it, vi } from 'vitest';
import { AppShell } from './AppShell';
import { prepareAssistantContent } from '../chat/widgets/AssistantContentHost';
const failure = vi.hoisted(() => ({ loads: 0 }));
vi.mock('./SuppliedRunPreview', () => { failure.loads++; throw new Error('optional fixture failed to load'); });

it('keeps normal Send, editor and history usable on optional activation failure without adding a run', async () => {
  await prepareAssistantContent(); render(<AppShell />); expect(failure.loads).toBe(0);
  const editor = screen.getByRole('textbox'); const history = screen.getByRole('article', { name: 'AVA response' });
  await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Run preview' })); });
  expect(screen.getByText('Run preview could not load. Try again.')).toBeVisible(); expect(screen.getAllByRole('article', { name: 'AVA response' })).toEqual([history]);
  expect(screen.getByRole('textbox')).toBe(editor); expect(screen.getByRole('article', { name: 'AVA response' })).toBe(history);
  fireEvent.click(screen.getByRole('button', { name: 'Send' })); expect(screen.getAllByRole('article', { name: 'Your message' })).toHaveLength(2);
  expect(screen.getByRole('button', { name: 'Run preview' })).toBeEnabled();
});
