import { StrictMode } from 'react';
import { act, fireEvent, render, screen } from '@testing-library/react';
import { expect, it, vi } from 'vitest';
import type { ToolView } from '../contracts';
import { ToolActivityGroup } from './ToolActivityGroup';
const pending = vi.hoisted(() => {
  let resolve!: () => void; const ready = new Promise<void>((done) => { resolve = done; });
  return { ready, resolve, loads: 0, rendered: [] as string[] };
});
vi.mock('./ToolActivityDetails', async () => {
  pending.loads++; await pending.ready;
  return { default: ({ record }: { record: ToolView }) => { pending.rendered.push(record.output!.text); return <pre aria-label="Row output">{record.output!.text}</pre>; } };
});
it('retains native row identity across updates and obeys explicit same-false collapse before late output resolves', async () => {
  const record: ToolView = { id: 'same', action: 'Read', detail: 'file.ts', state: 'running', output: { text: 'old snapshot', truncated: false } };
  const view = render(<StrictMode><ToolActivityGroup individual records={[record]} open={false} onOpenChange={() => {}} /></StrictMode>);
  const row = screen.getByRole('button', { name: 'Read file.ts Working' }); expect(pending.loads).toBe(0);
  await act(async () => { fireEvent.click(row); }); expect(pending.loads).toBe(1); expect(row).toHaveAttribute('aria-expanded', 'true');
  view.rerender(<StrictMode><ToolActivityGroup individual records={[{ ...record, output: { text: 'latest snapshot', truncated: false } }]} open={false} intent={1} onOpenChange={() => {}} /></StrictMode>);
  expect(row).toHaveAttribute('aria-expanded', 'false');
  await act(async () => { pending.resolve(); await pending.ready; }); expect(pending.rendered).toEqual([]);
  fireEvent.click(row); expect(await screen.findByLabelText('Row output')).toHaveTextContent('latest snapshot'); expect(pending.rendered).not.toContain('old snapshot');
  expect(screen.getByRole('button', { name: 'Read file.ts Working' })).toBe(row);
  view.container.setAttribute('inert', ''); fireEvent.click(row); expect(row).toHaveAttribute('aria-expanded', 'true'); view.unmount(); fireEvent.click(row); expect(screen.queryByLabelText('Row output')).not.toBeInTheDocument();
});
