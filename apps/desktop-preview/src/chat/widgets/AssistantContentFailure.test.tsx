import { act, fireEvent, render, screen } from '@testing-library/react';
import { expect, it, vi } from 'vitest';
import { AppShell } from '../../components/AppShell';
vi.mock('./AssistantContent', () => { throw new Error('fixture chunk unavailable'); });

it('preserves escaped readable history and eager controls when rich content fails, and never starts a run before its content owner is ready', async () => {
  const view = render(<AppShell />); const draft = screen.getByRole('textbox');
  expect(await screen.findByText('Response formatting unavailable; showing plain text.')).toBeVisible();
  expect(screen.getByRole('article', { name: 'AVA example response' })).toHaveTextContent('Browser back/forward behavior still needs a manual check.');
  expect(screen.getByRole('button', { name: /^Tool details/ })).toBeEnabled();
  await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Run preview' })); });
  expect(await screen.findByText('Run preview could not load. Try again.')).toBeVisible();
  expect(screen.queryByRole('button', { name: 'Stop preview' })).not.toBeInTheDocument(); expect(screen.queryByRole('article', { name: 'AVA supplied run response' })).not.toBeInTheDocument();
  expect(screen.getByRole('textbox')).toBe(draft); expect(screen.getByRole('button', { name: 'Run preview' })).toBeEnabled(); view.unmount();
});
