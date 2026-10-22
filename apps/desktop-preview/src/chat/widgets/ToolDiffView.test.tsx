import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { ToolChangeView, ToolView } from '../contracts';
import { toolChangeCases } from '../../catalog/examples';
import { ChangeNavigationContext } from '../changeNavigation';
import { validToolChanges } from '../toolChanges';
import { ToolActivityGroup } from './ToolActivityGroup';
import { ToolDiffView } from './ToolDiffView';
import ToolActivityDetails from './ToolActivityDetails';

const change: ToolChangeView = { id: 'file', filename: 'note.txt', operation: 'modified', additions: 1, deletions: 1, diff: { state: 'text', text: '@@ -1 +1 @@\n-old\n+new', truncated: false } };
function lines(diff: HTMLElement) { return Array.from(diff.querySelectorAll<HTMLElement>('[data-kind]'), (line) => [line.dataset.kind, line.textContent]); }

describe('explicit readonly tool change projections', () => {
  it.each([['created', 'Added'], ['modified', 'Modified'], ['deleted', 'Deleted']] as const)('renders a supplied %s file as one unified diff without actions', (operation, label) => {
    render(<ToolDiffView changes={[{ ...change, operation }]} />);
    const file = screen.getByRole('region', { name: 'File change: note.txt' });
    expect(file).toHaveTextContent(label);
    expect(within(file).getByLabelText('1 additions')).toHaveTextContent('+1'); expect(within(file).getByLabelText('1 deletions')).toHaveTextContent('−1');
    expect(lines(screen.getByLabelText('Diff: note.txt'))).toEqual([['hunk', '@@ -1 +1 @@'], ['del', '-old'], ['add', '+new']]);
    expect(within(file).queryByRole('button')).not.toBeInTheDocument(); expect(within(file).queryByRole('link')).not.toBeInTheDocument();
  });
  it('offers Review in Changes only when the app provides navigation, for the exact file path', async () => {
    const user = userEvent.setup(); const review = vi.fn();
    render(<ChangeNavigationContext.Provider value={review}><ToolDiffView changes={[change, { ...change, id: 'other', filename: 'src/other.ts' }]} /></ChangeNavigationContext.Provider>);
    await user.click(screen.getByRole('button', { name: 'Review in Changes: src/other.ts' }));
    expect(review).toHaveBeenCalledExactlyOnceWith('src/other.ts');
  });
  it('distinguishes supplied zero, absent counts, empty text, unavailable and binary without guessing', () => {
    const view = render(<ToolDiffView changes={[{ ...change, additions: 0, deletions: 0, diff: { state: 'text', text: '', truncated: false } }]} />);
    expect(screen.getByLabelText('0 additions')).toHaveTextContent('+0'); expect(screen.getByLabelText('0 deletions')).toHaveTextContent('−0'); expect(screen.getByText('No line changes to show.')).toBeVisible();
    view.rerender(<ToolDiffView changes={[{ ...change, additions: undefined, deletions: undefined }]} />);
    expect(screen.queryByLabelText(/ additions$| deletions$/)).not.toBeInTheDocument();
    for (const state of ['unavailable', 'binary'] as const) {
      view.rerender(<ToolDiffView changes={[{ ...change, additions: undefined, deletions: undefined, diff: { state } }]} />);
      expect(screen.queryByLabelText(/^Diff:/)).not.toBeInTheDocument(); expect(screen.queryByLabelText(/ additions$| deletions$/)).not.toBeInTheDocument();
      expect(screen.getByText(state === 'binary' ? 'Binary file. There’s no text diff to show.' : 'No diff available for this file.')).toBeVisible();
    }
  });
  it.each([NaN, Infinity, -Infinity, -1, 0.5, Number.MAX_SAFE_INTEGER + 1, '1', null])('rejects malformed counts %s rather than inventing zero or hiding ordinary output', (count) => {
    const changes = [{ ...change, additions: count }] as readonly ToolChangeView[];
    expect(validToolChanges(changes)).toBe(false);
    render(<ToolActivityDetails record={{ id: 'tool', action: 'Read', detail: 'Ordinary detail', state: 'completed', output: { text: 'Readable output', truncated: false }, changes }} />);
    expect(screen.getByLabelText('Output: Read')).toHaveTextContent('Readable output');
    expect(screen.getByRole('status')).toHaveTextContent('File changes unavailable'); expect(screen.queryByLabelText(/^Diff:/)).not.toBeInTheDocument();
  });
  it.each([
    null, {}, [null], [undefined], [{ ...change, id: '' }], [{ ...change, filename: ' ' }], [{ ...change, operation: 'rename' }],
    [{ ...change, diff: null }], [{ ...change, diff: { state: 'future' } }], [{ ...change, diff: { state: 'text', text: 4, truncated: false } }],
    [{ ...change, diff: { state: 'text', text: 'Supplied', truncated: undefined } }],
    [change, { ...change, filename: 'another.txt' }], [change, { ...change, id: 'another' }],
  ])('fails the entire malformed/duplicate projection closed: %j', (value) => {
    expect(validToolChanges(value)).toBe(false);
    render(<ToolDiffView changes={value as readonly ToolChangeView[]} />);
    expect(screen.getByRole('status')).toHaveTextContent('File changes unavailable');
    expect(screen.queryByRole('region')).not.toBeInTheDocument();
  });
  it('accepts safe integer optional counts and prototype-like file IDs without mutation', () => {
    const changes = Object.freeze([Object.freeze({ ...change, id: '__proto__', additions: 0, deletions: Number.MAX_SAFE_INTEGER })]);
    const snapshot = JSON.stringify(changes); expect(validToolChanges(changes)).toBe(true);
    render(<ToolDiffView changes={changes} />);
    expect(screen.getByLabelText('0 additions')).toBeVisible(); expect(JSON.stringify(changes)).toBe(snapshot);
  });
  it('bounds painted text honestly and distinguishes display clipping from supplied truncation', () => {
    const text = Array.from({ length: 220 }, (_, index) => `+Line ${index}`).join('\n');
    const view = render(<ToolDiffView changes={[{ ...change, diff: { state: 'text', text, truncated: true } }]} />);
    const diff = screen.getByLabelText('Diff: note.txt');
    expect(lines(diff)).toHaveLength(200); expect(diff).toHaveTextContent('+Line 199'); expect(diff).not.toHaveTextContent('+Line 200');
    expect(screen.getByText('This diff was cut off before the end.')).toBeVisible(); expect(screen.getByText(/Showing the first 200 lines/)).toBeVisible();
    view.rerender(<ToolDiffView changes={[{ ...change, diff: { state: 'text', text: '+' + 'x'.repeat(25000), truncated: false } }]} />);
    expect(screen.getByLabelText('Diff: note.txt').textContent).toHaveLength(20000);
    expect(screen.queryByText('This diff was cut off before the end.')).not.toBeInTheDocument(); expect(screen.getByText(/Showing the first 200 lines/)).toBeVisible();
  });
  it('escapes filenames, paths, unified text and output; never fetches, interprets URLs or creates controls', () => {
    const spies = ['fetch', 'XMLHttpRequest', 'WebSocket', 'FileReader', 'open'].map((name) => { const spy = vi.fn(); vi.stubGlobal(name, spy); return spy; });
    const filename = '<img src=x onerror=alert(1)>.txt'; const text = '-<script>old</script>\n+https://example.invalid/new';
    const { container } = render(<ToolActivityDetails record={{ id: 'tool', action: 'Inspect', detail: 'Pasted page.', state: 'failed', resource: { kind: 'image', path: '/tmp/<script>path</script>.png' }, output: { text: '<img src=x> output', truncated: true }, changes: [{ ...change, filename, diff: { state: 'text', text, truncated: false } }] }} />);
    expect(lines(screen.getByLabelText(`Diff: ${filename}`))).toEqual([['del', '-<script>old</script>'], ['add', '+https://example.invalid/new']]);
    expect(screen.getByText('/tmp/<script>path</script>.png')).toBeVisible(); expect(screen.getByText('Pasted page.')).toBeVisible(); expect(screen.getByText('Output was cut off before the end.')).toBeVisible();
    expect(container.querySelector('script, img, a, button, input')).toBeNull(); spies.forEach((spy) => expect(spy).not.toHaveBeenCalled());
  });
  it('does not infer changes from a successful action name, a resource path, or patch-like output', () => {
    const { container } = render(<ToolActivityDetails record={{ id: 'edit', action: 'Updated file', detail: 'Edited file', state: 'completed', resource: { kind: 'file', path: 'example.ts' }, output: { text: '@@ -1 +1 @@\n-old\n+new', truncated: false } }} />);
    expect(screen.getByLabelText('Output: Updated file')).toBeVisible(); expect(screen.queryByLabelText(/^Diff:/)).not.toBeInTheDocument();
    expect(container.querySelector('[aria-label^="File change"]')).toBeNull();
  });
  it('keeps every lifecycle static and summarizes only states that need a look', () => {
    vi.useFakeTimers();
    try {
      const records = ['queued', 'running', 'completed', 'failed', 'canceled', 'unavailable'].map((state) => ({ id: state, action: `Activity ${state}`, detail: 'Example', state: state as ToolView['state'] }));
      const { container } = render(<ToolActivityGroup records={records} open={false} onOpenChange={() => {}} />);
      const list = within(screen.getByRole('region', { name: 'Tool activity' })).getByRole('list');
      for (const label of ['Queued', 'Working', 'Completed', 'Failed', 'Stopped', 'Unavailable']) expect(within(list).getByText(label)).toBeVisible();
      expect(screen.getByRole('button')).toHaveTextContent('6 tools · 1 failed · 1 running · 1 queued · 1 stopped · 1 unavailable Show details');
      expect(container.querySelector('pre')).toBeNull(); const original = container.textContent;
      act(() => vi.advanceTimersByTime(60000)); expect(container.textContent).toBe(original); expect(vi.getTimerCount()).toBe(0);
    } finally { vi.useRealTimers(); }
  });
  it('shows still-pending tools of a stopped or failed turn as Not run / Not finished without rewriting state', () => {
    const records = (['queued', 'running', 'completed'] as const).map((state) => ({ id: state, action: `Activity ${state}`, detail: 'Example', state }));
    for (const lifecycle of ['canceled', 'failed'] as const) {
      const view = render(<ToolActivityGroup individual records={records} lifecycle={lifecycle} open={false} onOpenChange={() => {}} />);
      expect(screen.getByRole('button', { name: 'Activity queued Example Not run' })).toBeVisible();
      expect(screen.getByRole('button', { name: 'Activity running Example Not finished' })).toBeVisible();
      expect(screen.getByRole('button', { name: /^3 tools · 1 not finished · 1 not run/ })).toBeVisible();
      expect(screen.queryByText('Queued')).not.toBeInTheDocument();
      expect(view.container.querySelector('[data-tool-state="queued"] [data-status="unavailable"]')).not.toBeNull();
      view.unmount();
    }
  });
  it('exposes every catalog/review projection without automatic interaction or mutation', async () => {
    const user = userEvent.setup();
    const view = render(<ToolActivityGroup records={toolChangeCases['diff-modified']} open={false} onOpenChange={() => {}} />);
    for (const [scenario, records] of Object.entries(toolChangeCases)) {
      const snapshot = JSON.stringify(records);
      view.rerender(<ToolActivityGroup records={records} open onOpenChange={() => {}} />);
      expect(await screen.findByLabelText('Output: Update note')).toBeVisible();
      if (scenario === 'diff-malformed' || scenario === 'diff-duplicate') expect(screen.getByRole('status')).toHaveTextContent('File changes unavailable');
      else if (scenario === 'diff-missing') expect(screen.queryByRole('region', { name: /^File change:/ })).not.toBeInTheDocument();
      else expect(screen.getByRole('region', { name: /^File change:/ })).toBeVisible();
      expect(JSON.stringify(records)).toBe(snapshot);
    }
    const trigger = screen.getByRole('button', { name: /^\d+ tools?\b/ }); trigger.focus(); await user.keyboard('{Enter}');
    expect(trigger).toHaveFocus(); // Controlled host does not invent a transition.
    fireEvent.click(trigger); expect(trigger).toHaveAttribute('aria-expanded', 'true');
  });
});
