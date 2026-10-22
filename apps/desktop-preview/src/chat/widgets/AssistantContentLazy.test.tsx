import { StrictMode } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { expect, it, vi } from 'vitest';
import { MessageTurn } from './MessageTurn';
import { AppShell } from '../../components/AppShell';

const pending = vi.hoisted(() => {
  let resolve!: () => void; const ready = new Promise<void>((done) => { resolve = done; });
  return { ready, resolve, loads: 0 };
});
vi.mock('./AssistantContent', async (original) => { pending.loads++; await pending.ready; return original<typeof import('./AssistantContent')>(); });

it('keeps user/turn identity, attribution, tools and Stop eager; bounded escaped fallback uses latest identity and a departed preview cannot start on late load', async () => {
  const turn = { id: 'one', label: 'Response', author: 'AVA', role: 'assistant' as const, state: 'streaming' as const, markdown: '<img src="https://remote/x">\n' + 'x'.repeat(30000), attribution: { modelLabel: 'Aster' } };
  const view = render(<StrictMode><MessageTurn turn={{ id: 'user', label: 'User', author: 'You', role: 'user', state: 'committed', text: 'Only user' }} /></StrictMode>);
  expect(pending.loads).toBe(0);
  view.rerender(<StrictMode><MessageTurn turn={turn} /></StrictMode>);
  const article = screen.getByRole('article', { name: 'Response' }); expect(within(article).getByLabelText('Model: Aster')).toBeVisible(); expect(article.querySelector('img')).toBeNull();
  expect(article.textContent!.length).toBeLessThan(20500); expect(within(article).getByText('Loading response…')).toBeInTheDocument();
  // Raw Markdown only reserves space while formatting loads; it is never painted or announced.
  expect(article.querySelector('[aria-hidden="true"]')).toHaveTextContent('<img src="https://remote/x">');
  view.rerender(<StrictMode><MessageTurn turn={{ ...turn, id: 'new', markdown: 'Latest supplied **response**', state: 'committed' }} /></StrictMode>);
  expect(article).toHaveTextContent('Latest supplied **response**');
  const app = render(<StrictMode><AppShell /></StrictMode>);
  fireEvent.click(screen.getByRole('button', { name: 'Run preview' })); expect(screen.getByRole('button', { name: 'Loading preview…' })).toHaveAttribute('aria-disabled', 'true');
  expect(screen.getAllByRole('article', { name: 'AVA response' })).toHaveLength(1); // Only the stored reply; no run turn yet.
  expect(screen.getByRole('button', { name: /^\d+ tools?\b/ })).toBeEnabled();
  fireEvent.click(screen.getByRole('button', { name: 'New chat' }));
  await act(async () => { pending.resolve(); await pending.ready; });
  expect(await screen.findByText('response', { selector: 'strong' })).toBeVisible(); expect(article).toHaveTextContent('Latest supplied response'); expect(article.textContent).not.toContain('https://remote');
  expect(screen.queryByRole('article', { name: 'AVA response' })).not.toBeInTheDocument(); expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');
  await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Run preview' })); });
  expect(screen.getByRole('button', { name: 'Stop preview' })).toBeVisible(); expect(screen.queryByText('Loading response…')).not.toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'Stop preview' })); app.unmount(); view.unmount(); expect(pending.loads).toBe(1);
});
