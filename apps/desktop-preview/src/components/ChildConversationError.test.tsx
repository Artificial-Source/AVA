import { fireEvent, render, screen } from '@testing-library/react';
import { expect, it, vi } from 'vitest';
import { AppShell } from './AppShell';

vi.mock('./ChildConversation', async () => { throw new Error('Expected local child-module test failure'); });

it('contains a rejected optional child import without losing identity, readonly scope, Back or parent state', async () => {
  const error = vi.spyOn(console, 'error').mockImplementation(() => {});
  render(<AppShell />);
  const title = screen.getByRole('heading', { level: 1 });
  fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Parent draft survives import failure' } });
  fireEvent.click(screen.getByRole('button', { name: /^Completed Map return paths/ }));
  expect(title).toHaveTextContent('Map return paths'); expect(title).toHaveFocus();
  expect(await screen.findByRole('alert')).toHaveTextContent('Child conversation could not load. Use Back to parent');
  expect(error).toHaveBeenCalled();
  expect(error.mock.calls.flat().some((value) => value instanceof Error)).toBe(true);
  expect(screen.queryByRole('textbox')).not.toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Model for this chat' })).not.toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'Back to parent' }));
  expect(title).toHaveTextContent('Remember search filters'); expect(title).toHaveFocus();
  expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  expect(screen.getByRole('textbox')).toHaveValue('Parent draft survives import failure');
  expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(/^Aster$/);
});
