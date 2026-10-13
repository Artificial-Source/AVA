import { StrictMode, useState } from 'react';
import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { expect, it, vi } from 'vitest';
import type { ToolView } from '../contracts';
import { ToolActivityGroup } from './ToolActivityGroup';

const detail = vi.hoisted(() => {
  let resolve!: () => void;
  const ready = new Promise<void>((done) => { resolve = done; });
  return { ready, resolve, loads: 0, rendered: [] as string[] };
});
vi.mock('./ToolActivityDetails', async () => {
  detail.loads++;
  await detail.ready;
  return { default: ({ record }: { record: ToolView }) => {
    detail.rendered.push(record.id);
    return <pre aria-label="Loaded tool output" tabIndex={0}>{record.output?.text}</pre>;
  } };
});

it('loads only after explicit expansion, never reopens on late resolution, and keeps closing/unmounted detail inert or absent', async () => {
  const user = userEvent.setup();
  function Host({ id = 'initial' }: { id?: string }) {
    const [open, setOpen] = useState(false);
    return <ToolActivityGroup records={[{ id, action: 'Read note', detail: 'Supplied record', state: 'completed', output: { text: id, truncated: false } }]} open={open} onOpenChange={setOpen} />;
  }
  const view = render(<StrictMode><Host /></StrictMode>);
  expect(detail.loads).toBe(0); expect(screen.queryByLabelText('Loaded tool output')).not.toBeInTheDocument();
  const trigger = screen.getByRole('button', { name: /^Tool details/ }); trigger.focus(); await user.keyboard('{Enter}');
  await waitFor(() => expect(detail.loads).toBe(1)); expect(trigger).toHaveAttribute('aria-expanded', 'true'); expect(trigger).toHaveFocus();
  await user.keyboard(' '); expect(trigger).toHaveAttribute('aria-expanded', 'false');
  const closing = view.container.querySelector('[data-presence="closing"]'); expect(closing).toHaveAttribute('inert'); expect(closing).toHaveAttribute('aria-hidden', 'true');
  expect(closing).toBeEmptyDOMElement();
  view.rerender(<StrictMode><Host id="latest" /></StrictMode>);
  await act(async () => { detail.resolve(); await detail.ready; });
  expect(detail.rendered).toEqual([]); expect(screen.queryByLabelText('Loaded tool output')).not.toBeInTheDocument();
  expect(trigger).toHaveAttribute('aria-expanded', 'false');
  await user.click(trigger); expect(await screen.findByLabelText('Loaded tool output')).toHaveTextContent('latest');
  expect(detail.rendered).not.toContain('initial'); expect(detail.loads).toBe(1); expect(trigger).toHaveFocus();
  const output = screen.getByLabelText('Loaded tool output');
  await user.click(trigger); expect(output).not.toBeInTheDocument(); expect(trigger).toHaveFocus();
  view.unmount(); fireEvent.click(trigger); expect(screen.queryByLabelText('Loaded tool output')).not.toBeInTheDocument();
});
