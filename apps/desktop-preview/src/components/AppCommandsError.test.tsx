import { fireEvent, render, screen } from '@testing-library/react';
import { expect, it, vi } from 'vitest';
import { AppShell } from './AppShell';
vi.mock('./AppCommands', () => { throw new Error('Owned optional command-load test'); });

it('reports an optional keyboard-owner load failure without dispatching or losing native visible controls', async () => {
  render(<AppShell />); const transcript = screen.getByRole('region', { name: /^Conversation:/ }); transcript.focus();
  fireEvent.keyDown(transcript, { key: '9', code: 'Digit9', ctrlKey: true, shiftKey: true });
  expect(await screen.findByText('Keyboard actions could not load. Use the visible controls or try again.')).toBeInTheDocument();
  expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
  expect(transcript).toHaveFocus(); fireEvent.click(screen.getByRole('button', { name: 'New chat' }));
  expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');
});
