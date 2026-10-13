import { useState } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { ChangeReviewState, ComposerView, MessageTurnView, ToolView } from '../contracts';
import { MessageTurn } from './MessageTurn';
import { ToolActivityGroup } from './ToolActivityGroup';
import { ChangeReviewView } from './ChangeReviewView';
import { RunNotice } from './RunNotice';
import { ChatComposer } from './ChatComposer';

const composer: ComposerView = { title: 'Controlled draft', draft: 'Draft', expanded: false, runState: 'idle', contextOptions: [{ id: 'note', label: 'Notes', kind: 'context' }], selectedContextIds: [], modelOptions: [{ id: 'sample', label: 'Sample' }], modelId: 'sample', modeOptions: [{ id: 'build', label: 'Build' }, { id: 'plan', label: 'Plan' }], modeId: 'build' };
function callbacks() { return { onDraftChange: vi.fn(), onModelChange: vi.fn(), onModeChange: vi.fn(), onExpandedChange: vi.fn(), onAddReference: vi.fn(), onRemoveReference: vi.fn(), onSend: vi.fn(), onStop: vi.fn() }; }

describe('controlled turn/tool/change/run presentation', () => {
  it('shows only supplied model and thinking facts, with no guessed attribution for missing or blank values', () => {
    const turn: MessageTurnView = { id: 'facts', role: 'assistant', author: 'AVA', label: 'Supplied response', state: 'committed', markdown: 'Example', attribution: { modelLabel: 'GPT6.1Sol', thinkingLabel: 'xHigh' } };
    const { rerender } = render(<MessageTurn turn={Object.freeze(turn)} />);
    expect(screen.getByLabelText('Model: GPT6.1Sol')).toBeVisible();
    expect(screen.getByLabelText('Thinking: xHigh')).toBeVisible();
    rerender(<MessageTurn turn={{ ...turn, attribution: { modelLabel: 'Aster' } }} />);
    expect(screen.getByLabelText('Model: Aster')).toBeVisible();
    expect(screen.queryByLabelText(/Thinking:/)).not.toBeInTheDocument();
    for (const attribution of [undefined, { modelLabel: ' ', thinkingLabel: '' }]) {
      rerender(<MessageTurn turn={{ ...turn, attribution }} />);
      expect(screen.queryByLabelText('Response attribution')).not.toBeInTheDocument();
      expect(screen.getByRole('article')).toHaveTextContent('AVA');
    }
  });

  it('keeps generated image labels separate from failure and raw details opt-in and escaped', async () => {
    const user = userEvent.setup();
    const path = '/tmp/invented/pi-clipboard-12345678-1234-1234-1234-123456789abc.png';
    const records: readonly ToolView[] = [{ id: 'image', action: 'Inspect image', detail: 'No cause supplied.', state: 'failed', resource: { kind: 'image', path }, output: { text: '<script>raw /tmp/example</script>', truncated: false } }];
    function Host() { const [open, setOpen] = useState(false); return <ToolActivityGroup records={records} open={open} onOpenChange={setOpen} />; }
    render(<Host />);
    const outcomes = screen.getByRole('list', { name: 'Supplied tool activity' });
    expect(outcomes).toHaveTextContent('Inspect image Pasted image Failed');
    expect(outcomes).not.toHaveTextContent(path); expect(screen.queryByText(path)).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: /^Tool details · 1/ }));
    expect(await screen.findByText(path)).toBeVisible();
    expect(screen.getByText('<script>raw /tmp/example</script>')).toBeVisible();
    expect(screen.getByRole('region', { name: 'Tool activity' }).querySelector('script')).toBeNull();
  });
  it.each(['streaming', 'committed', 'canceled', 'failed'] as const)('renders a supplied %s turn without changing it over time', (state) => {
    vi.useFakeTimers();
    try {
      const turn: MessageTurnView = { id: 'turn', label: 'Assistant example', author: 'AVA', role: 'assistant', markdown: state === 'streaming' ? '' : 'Supplied **text**.', state };
      const { container } = render(<MessageTurn turn={turn} />);
      expect(screen.getByRole('article')).toHaveAttribute('data-turn-state', state);
      const original = container.textContent;
      act(() => vi.advanceTimersByTime(60_000));
      expect(container.textContent).toBe(original);
      expect(vi.getTimerCount()).toBe(0);
      if (state !== 'committed') expect(screen.getByRole('article')).toHaveTextContent('Partial message');
    } finally { vi.useRealTimers(); }
  });

  it('escapes user text and assistant raw HTML/URLs while keeping stable accessible labels', () => {
    const { container, rerender } = render(<MessageTurn turn={{ id: 'user', role: 'user', author: 'You', label: 'User example', state: 'committed', text: '<script>example</script>' }} />);
    expect(screen.getByRole('article', { name: 'User example' })).toHaveTextContent('<script>example</script>');
    expect(container.querySelector('script')).toBeNull();
    rerender(<MessageTurn turn={{ id: 'assistant', role: 'assistant', author: 'AVA', label: 'Assistant example', state: 'committed', markdown: '<img src=x onerror=alert(1)>\n\n[Unsafe](javascript:alert%281%29)\n\n[Safe](https://example.com)', note: '<script>plain note</script>' }} />);
    expect(container.querySelector('img, script')).toBeNull();
    expect(screen.getByText('Unsafe')).not.toHaveAttribute('href', expect.stringMatching(/^javascript:/));
    expect(screen.getByRole('link', { name: 'Safe' })).toHaveAttribute('href', 'https://example.com');
    expect(screen.getByText('<script>plain note</script>')).toBeInTheDocument();
  });

  it('uses controlled disclosure state, stable tool order, escaped/truncated output, and all supplied lifecycle labels', async () => {
    const user = userEvent.setup();
    const records: readonly ToolView[] = ['queued', 'running', 'completed', 'failed', 'canceled'].map((state) => ({ id: state, state: state as ToolView['state'], action: `Example ${state}`, detail: 'Detail', output: state === 'failed' ? { text: '<script>tail</script>', truncated: true } : undefined }));
    const onOpenChange = vi.fn();
    const { rerender } = render(<ToolActivityGroup records={records} open={false} onOpenChange={onOpenChange} />);
    const outcomes = screen.getByRole('list', { name: 'Supplied tool activity' });
    for (const label of ['Queued', 'Working', 'Completed', 'Failed', 'Stopped']) expect(within(outcomes).getByText(label)).toBeVisible();
    records.forEach((record, index) => expect(outcomes.children[index]).toHaveAttribute('data-tool-state', record.state));
    const trigger = screen.getByRole('button'); trigger.focus(); await user.keyboard('{Enter}');
    expect(onOpenChange).toHaveBeenCalledWith(true);
    expect(screen.queryByText('<script>tail</script>')).not.toBeInTheDocument();
    rerender(<ToolActivityGroup records={records} open onOpenChange={onOpenChange} />);
    await screen.findByText('<script>tail</script>');
    const details = screen.getByRole('region', { name: 'Tool activity' });
    const items = within(details).getAllByRole('listitem');
    expect(items).toHaveLength(5);
    records.forEach((record, index) => expect(items[index]).toHaveTextContent(record.action));
    expect(screen.getByText('Output truncated by the supplied view.')).toBeInTheDocument();
    expect(screen.getByText('<script>tail</script>')).toBeInTheDocument();
    expect(details.querySelector('script')).toBeNull();
    rerender(<ToolActivityGroup records={records} open={false} onOpenChange={onOpenChange} />);
    expect(screen.queryByText('<script>tail</script>')).not.toBeInTheDocument();
  });

  it.each(['loading', 'empty', 'error', 'ready', 'stale'] as const)('presents %s changes without mutation controls', (state) => {
    const change = { id: 'file', filename: '<example>.ts', description: 'Supplied excerpt', before: '<script>old</script>', after: '<img>new</img>' };
    const view: ChangeReviewState = state === 'ready' ? { state, changes: [change] } : state === 'stale' ? { state, changes: [change], message: 'Last supplied content is stale.' } : state === 'loading' ? { state } : { state, message: 'No current excerpts.' };
    const { container } = render(<ChangeReviewView view={view} notice="Read-only example." />);
    if (state === 'ready' || state === 'stale') {
      expect(screen.getByRole('heading', { name: '<example>.ts' })).toBeInTheDocument();
      expect(screen.getByText('<script>old</script>')).toBeInTheDocument();
    }
    if (state === 'loading') expect(screen.getByRole('status')).toHaveTextContent('Loading');
    if (state === 'error') expect(screen.getByRole('alert')).toHaveTextContent('No current excerpts');
    expect(container.querySelector('script, img')).toBeNull();
    expect(screen.queryByRole('button')).not.toBeInTheDocument();
  });

  it('can disclose file excerpts through a supplied callback without owning expanded state', () => {
    const onExpandedChange = vi.fn();
    render(<ChangeReviewView view={{ state: 'ready', changes: [{ id: 'file', filename: 'sample.ts', description: 'Sample', before: 'a', after: 'b' }] }} notice="Example" expandedIds={[]} onExpandedChange={onExpandedChange} />);
    fireEvent.click(screen.getByRole('button', { name: 'Excerpts for sample.ts' }));
    expect(onExpandedChange).toHaveBeenCalledWith('file', true);
    expect(screen.queryByRole('heading', { name: 'Before' })).not.toBeInTheDocument();
  });

  it('keeps retry capability explicit, unknown errors generic, and supplied retry/cancel/terminal states static', () => {
    vi.useFakeTimers();
    try {
      const retry = vi.fn();
      const { rerender } = render(<RunNotice view={{ state: 'error', code: '__proto__' }} />);
      expect(screen.getByText(/No further error details/)).toBeInTheDocument();
      expect(screen.queryByRole('button')).not.toBeInTheDocument();
      rerender(<RunNotice view={{ state: 'error', code: 'unavailable' }} onRetry={retry} />);
      expect(retry).not.toHaveBeenCalled();
      fireEvent.click(screen.getByRole('button', { name: 'Request retry' }));
      expect(retry).toHaveBeenCalledTimes(1);
      rerender(<RunNotice view={{ state: 'retrying', attempt: 3, cause: '<script>supplied cause</script>' }} onRetry={retry} />);
      expect(screen.getByText(/Attempt 3/)).toHaveTextContent('<script>supplied cause</script>');
      act(() => vi.advanceTimersByTime(60_000));
      expect(screen.getByText(/Attempt 3/)).toBeInTheDocument();
      for (const state of ['cancel-requested', 'canceled', 'completed'] as const) {
        rerender(<RunNotice view={{ state }} onRetry={retry} />);
        expect(screen.queryByRole('button')).not.toBeInTheDocument();
      }
      expect(vi.getTimerCount()).toBe(0);
      expect(retry).toHaveBeenCalledTimes(1);
    } finally { vi.useRealTimers(); }
  });
});

describe('controlled composer without preview-state imports', () => {
  it('connects the optional Enter preference without overriding Shift, IME, keyCode 229 or explicit modifier sends', () => {
    const cb = callbacks();
    const { rerender } = render(<ChatComposer view={composer} {...cb} enterBehavior="send" />);
    const draft = screen.getByRole('textbox');
    for (const event of [{ shiftKey: true }, { altKey: true }, { isComposing: true }, { keyCode: 229 }]) fireEvent.keyDown(draft, { key: 'Enter', ...event });
    fireEvent.compositionStart(draft); fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true }); fireEvent.compositionEnd(draft);
    expect(cb.onSend).not.toHaveBeenCalled();
    fireEvent.keyDown(draft, { key: 'Enter' }); expect(cb.onSend).toHaveBeenCalledTimes(1);
    rerender(<ChatComposer view={composer} {...cb} enterBehavior="newline" />);
    fireEvent.keyDown(draft, { key: 'Enter' }); expect(cb.onSend).toHaveBeenCalledTimes(1);
    fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true }); fireEvent.keyDown(draft, { key: 'Enter', metaKey: true });
    expect(cb.onSend).toHaveBeenCalledTimes(3);
  });
  it('optionally emits model-open only, while the catalog fallback remains a native controlled select', async () => {
    const user = userEvent.setup(); const cb = callbacks(); const onOpenModel = vi.fn();
    const { rerender } = render(<ChatComposer idPrefix="" view={composer} {...cb} onOpenModel={onOpenModel} />);
    const draft = screen.getByRole('textbox');
    const button = screen.getByRole('button', { name: 'Model for this chat' });
    expect(button).toHaveAttribute('id', 'model-choice'); expect(button).toHaveAttribute('type', 'button'); expect(button).toHaveAttribute('aria-haspopup', 'dialog');
    expect(button).toHaveTextContent(composer.modelOptions[0].label);
    expect(screen.queryByRole('combobox', { name: 'Model for this chat' })).not.toBeInTheDocument();
    button.focus(); await user.keyboard('{Enter}'); await user.keyboard(' ');
    expect(onOpenModel).toHaveBeenCalledTimes(2);
    expect(cb.onModelChange).not.toHaveBeenCalled(); expect(cb.onSend).not.toHaveBeenCalled(); expect(cb.onStop).not.toHaveBeenCalled();
    expect(cb.onModeChange).not.toHaveBeenCalled(); expect(screen.getByRole('textbox')).toBe(draft);
    rerender(<ChatComposer idPrefix="" view={composer} {...cb} />);
    const select = screen.getByRole('combobox', { name: 'Model for this chat' });
    expect(select).toHaveAttribute('id', 'model-choice');
    expect(select).toHaveValue(composer.modelId);
    fireEvent.change(select, { target: { value: composer.modelOptions[0].id } });
    expect(cb.onModelChange).toHaveBeenCalledWith(composer.modelOptions[0].id);
    expect(onOpenModel).toHaveBeenCalledTimes(2);
  });
  it('emits changes without owning draft/model/mode/expansion and retains ordinary focus', () => {
    const cb = callbacks();
    const { rerender } = render(<ChatComposer view={composer} {...cb} />);
    const draft = screen.getByRole('textbox'); draft.focus();
    fireEvent.change(draft, { target: { value: 'Edited draft' } });
    expect(cb.onDraftChange).toHaveBeenCalledWith('Edited draft');
    expect(draft).toHaveValue('Draft'); expect(draft).toHaveFocus();
    fireEvent.change(screen.getByRole('combobox', { name: 'Mode for this chat' }), { target: { value: 'plan' } });
    expect(cb.onModeChange).toHaveBeenCalledWith('plan');
    expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('build');
    fireEvent.click(screen.getByRole('button', { name: 'Expand draft' }));
    expect(cb.onExpandedChange).toHaveBeenCalledWith(true);
    rerender(<ChatComposer view={{ ...composer, draft: 'Host update', expanded: true }} {...cb} />);
    expect(screen.getByRole('textbox')).toBe(draft); expect(draft).toHaveValue('Host update');
    fireEvent.keyDown(draft, { key: 'Escape' });
    expect(cb.onExpandedChange).toHaveBeenLastCalledWith(false);
    expect(screen.getByRole('button', { name: 'Collapse draft' })).toHaveFocus();
  });

  it('guards empty/running/unadvertised Send and Stop, and ignores IME/plain Enter', () => {
    const cb = callbacks();
    const { rerender } = render(<ChatComposer view={composer} {...cb} />);
    const draft = screen.getByRole('textbox');
    fireEvent.keyDown(draft, { key: 'Enter' });
    fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true, isComposing: true });
    expect(cb.onSend).not.toHaveBeenCalled();
    fireEvent.keyDown(draft, { key: 'Enter', metaKey: true });
    expect(cb.onSend).toHaveBeenCalledTimes(1);
    rerender(<ChatComposer view={{ ...composer, draft: '  ' }} {...cb} />);
    fireEvent.submit(screen.getByRole('form')); expect(cb.onSend).toHaveBeenCalledTimes(1);
    rerender(<ChatComposer view={{ ...composer, runState: 'running' }} {...cb} />);
    fireEvent.submit(screen.getByRole('form')); fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true });
    expect(cb.onSend).toHaveBeenCalledTimes(1);
    fireEvent.click(screen.getByRole('button', { name: 'Request stop' })); expect(cb.onStop).toHaveBeenCalledTimes(1);
    rerender(<ChatComposer view={{ ...composer, runState: 'running' }} {...cb} onStop={undefined} />);
    expect(screen.getByRole('button', { name: 'Request stop' })).toBeDisabled();
    rerender(<ChatComposer view={composer} {...cb} onSend={undefined} />);
    expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
  });

  it('filters unknown/duplicate references and invalid model/mode events before callbacks', () => {
    const cb = callbacks();
    const { rerender } = render(<ChatComposer view={{ ...composer, selectedContextIds: ['unknown', 'note', 'note'] }} {...cb} />);
    expect(within(screen.getByRole('list', { name: 'Attached context' })).getAllByRole('listitem')).toHaveLength(1);
    fireEvent.change(screen.getByRole('combobox', { name: 'Model for this chat' }), { target: { value: 'unknown' } });
    fireEvent.change(screen.getByRole('combobox', { name: 'Mode for this chat' }), { target: { value: 'unknown' } });
    expect(cb.onModelChange).not.toHaveBeenCalled(); expect(cb.onModeChange).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Remove Notes context' }));
    expect(cb.onRemoveReference).toHaveBeenCalledExactlyOnceWith('note');
    expect(screen.getByRole('button', { name: 'Add context' })).toHaveFocus();
    rerender(<ChatComposer view={{ ...composer, modelId: 'missing', modeId: 'missing' }} {...cb} />);
    expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
    expect(screen.getByRole('combobox', { name: 'Model for this chat' })).toHaveValue('');
  });

  it('uses unique IDs, native context/escape/blur behavior, and cleans up on unmount', async () => {
    const user = userEvent.setup(); const cb = callbacks();
    function Example() {
      const [view, setView] = useState(composer);
      return <ChatComposer view={view} {...cb} onAddReference={(id) => setView({ ...view, selectedContextIds: [id] })} />;
    }
    const { unmount } = render(<><Example /><ChatComposer view={{ ...composer, title: 'Second' }} {...cb} /></>);
    const forms = screen.getAllByRole('form'); const first = within(forms[0]);
    const add = first.getByRole('button', { name: 'Add context' });
    add.focus(); await user.keyboard('{Enter}');
    const option = first.getByRole('button', { name: 'context Notes' });
    expect(option).toHaveFocus(); await user.keyboard(' ');
    expect(option).toHaveAttribute('aria-pressed', 'true');
    await user.keyboard('{Escape}'); expect(add).toHaveFocus();
    expect(first.queryByRole('region', { name: 'Context references' })).not.toBeInTheDocument();
    await user.click(add); await user.click(within(forms[1]).getByRole('textbox'));
    expect(first.queryByRole('region', { name: 'Context references' })).not.toBeInTheDocument();
    const ids = Array.from(document.querySelectorAll('[id]'), (entry) => entry.id);
    expect(new Set(ids).size).toBe(ids.length);
    unmount(); fireEvent.click(option);
    expect(cb.onSend).not.toHaveBeenCalled();
  });
});
