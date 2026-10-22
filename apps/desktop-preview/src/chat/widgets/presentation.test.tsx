import { useState } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { ChangeEntryView, ChangeReviewState, ComposerView, MessageTurnView, ToolView } from '../contracts';
import { HelpContext } from '../../components/helpContext';
import { MessageTurn } from './MessageTurn';
import { ToolActivityGroup } from './ToolActivityGroup';
import { ChangeReviewView } from './ChangeReviewView';
import { RunNotice } from './RunNotice';
import { ChatComposer } from './ChatComposer';

const composer: ComposerView = { title: 'Controlled draft', draft: 'Draft', expanded: false, runState: 'idle', contextOptions: [{ id: 'note', label: 'Notes', kind: 'context' }], selectedContextIds: [], modelOptions: [{ id: 'sample', label: 'Sample' }], modelId: 'sample', modeOptions: [{ id: 'build', label: 'Build' }, { id: 'plan', label: 'Plan' }], modeId: 'build' };
function callbacks() { return { onDraftChange: vi.fn(), onModelChange: vi.fn(), onModeChange: vi.fn(), onExpandedChange: vi.fn(), onAddReference: vi.fn(), onRemoveReference: vi.fn(), onSend: vi.fn(), onStop: vi.fn() }; }
const unavailable = { status: 'Not connected', detail: 'Sample Lab isn’t connected.', sendReason: 'Connect Sample Lab to send with Sample', message: 'Sample Lab isn’t connected, so this chat can’t send.' };

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
    const outcomes = within(screen.getByRole('region', { name: 'Tool activity' })).getByRole('list');
    expect(outcomes).toHaveTextContent('Inspect image Pasted image Failed');
    expect(outcomes).not.toHaveTextContent(path); expect(screen.queryByText(path)).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: /^1 tool\b/ }));
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
      if (state !== 'committed') expect(screen.getByRole('article')).toHaveTextContent({ streaming: 'Responding', failed: 'Failed · reply incomplete', canceled: 'Stopped · reply incomplete' }[state]);
      expect(screen.getByRole('article')).not.toHaveTextContent(/Partial message|canceled/);
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
    const outcomes = within(screen.getByRole('region', { name: 'Tool activity' })).getByRole('list');
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
    expect(screen.getByText('Output was cut off before the end.')).toBeInTheDocument();
    expect(screen.getByText('<script>tail</script>')).toBeInTheDocument();
    expect(details.querySelector('script')).toBeNull();
    rerender(<ToolActivityGroup records={records} open={false} onOpenChange={onOpenChange} />);
    expect(screen.queryByText('<script>tail</script>')).not.toBeInTheDocument();
  });

  it.each(['loading', 'empty', 'error', 'ready', 'stale'] as const)('presents %s changes in the shared diff format with no control but the file disclosure', (state) => {
    const change: ChangeEntryView = { id: 'file', path: '<example>.ts', status: 'modified', added: 1, removed: 1, description: 'Narrower reading width', diff: { state: 'text', truncated: false, text: '@@ -1 +1 @@\n-<script>old</script>\n+<img>new</img>' } };
    const view: ChangeReviewState = state === 'ready' ? { state, changes: [change] } : state === 'stale' ? { state, changes: [change], message: 'These changes may be out of date.' } : state === 'loading' ? { state } : { state, message: 'Couldn’t load changes.' };
    const { container } = render(<ChangeReviewView view={view} />);
    if (state === 'ready' || state === 'stale') {
      expect(screen.getByRole('heading', { name: '<example>.ts' })).toBeInTheDocument();
      expect(screen.getByLabelText('Diff: <example>.ts')).toHaveTextContent('-<script>old</script>+<img>new</img>');
      expect(screen.getByLabelText('1 additions')).toHaveTextContent('+1'); expect(screen.getByText('Narrower reading width')).toBeVisible();
      expect(screen.getByText('1 file changed, 1 line added, 1 removed')).toBeInTheDocument();
      expect(screen.queryByRole('navigation', { name: 'Changed files' })).not.toBeInTheDocument(); // One file needs no index.
    }
    if (state === 'loading') expect(screen.getByRole('status')).toHaveTextContent('Loading');
    if (state === 'error') expect(screen.getByRole('alert')).toHaveTextContent('Couldn’t load changes');
    expect(container.querySelector('script, img')).toBeNull();
    expect(screen.queryAllByRole('button').map((button) => button.getAttribute('aria-expanded'))).toEqual(state === 'ready' || state === 'stale' ? ['true'] : []);
  });

  it('focuses the requested file once per request without stealing focus on later updates', () => {
    const entry = (path: string): ChangeEntryView => ({ id: path, path, status: 'modified', diff: { state: 'text', truncated: false, text: `+${path}` } });
    const view: ChangeReviewState = { state: 'ready', changes: [entry('a.ts'), entry('src/b.ts')] };
    const { rerender } = render(<><ChangeReviewView view={view} focusPath="src/b.ts" /><button type="button">Elsewhere</button></>);
    expect(screen.getByRole('region', { name: 'File change: src/b.ts' })).toHaveFocus();
    screen.getByRole('button', { name: 'Elsewhere' }).focus();
    rerender(<><ChangeReviewView view={{ ...view, changes: [...view.changes] }} focusPath="src/b.ts" /><button type="button">Elsewhere</button></>);
    expect(screen.getByRole('button', { name: 'Elsewhere' })).toHaveFocus();
    rerender(<><ChangeReviewView view={view} focusPath="a.ts" /><button type="button">Elsewhere</button></>);
    expect(screen.getByRole('region', { name: 'File change: a.ts' })).toHaveFocus();
    rerender(<><ChangeReviewView view={view} focusPath="missing.ts" /><button type="button">Elsewhere</button></>);
    expect(screen.getByRole('region', { name: 'File change: a.ts' })).toHaveFocus();
  });

  it('totals the files, jumps to each from a compact index and collapses files one at a time', async () => {
    const user = userEvent.setup();
    const entry = (path: string, added?: number, removed?: number): ChangeEntryView => ({ id: path, path, status: 'modified', added, removed, diff: { state: 'text', truncated: false, text: `@@ -1 +1 @@\n-old ${path}\n+new ${path}` } });
    const view: ChangeReviewState = { state: 'ready', changes: [entry('src/search-state.ts', 2, 1), entry('README.md', 1, 3), entry('docs/notes.txt')] };
    const { rerender } = render(<ChangeReviewView view={view} />);
    expect(screen.getByText('3 files changed, 3 lines added, 4 removed')).toBeInTheDocument();
    const index = screen.getByRole('navigation', { name: 'Changed files' });
    expect(within(index).getAllByRole('button').map((button) => button.textContent)).toEqual(['search-state.tssrc+2 −1', 'README.md+1 −3', 'notes.txtdocs']);
    const file = screen.getByRole('region', { name: 'File change: src/search-state.ts' });
    const toggle = within(file).getByRole('button', { name: 'src/search-state.ts' });
    await user.click(toggle);
    expect(toggle).toHaveAttribute('aria-expanded', 'false'); expect(screen.queryByLabelText('Diff: src/search-state.ts')).not.toBeInTheDocument();
    expect(within(file).getByLabelText('2 additions')).toBeVisible(); expect(screen.getByLabelText('Diff: README.md')).toBeVisible();
    // Jumping to a collapsed file opens it and moves focus to it.
    await user.click(within(index).getByRole('button', { name: 'src/search-state.ts, 2 added, 1 removed' }));
    expect(file).toHaveFocus(); expect(toggle).toHaveAttribute('aria-expanded', 'true'); expect(screen.getByLabelText('Diff: src/search-state.ts')).toBeVisible();
    await user.click(toggle); rerender(<ChangeReviewView view={view} focusPath="src/search-state.ts" />);
    expect(file).toHaveFocus(); expect(toggle).toHaveAttribute('aria-expanded', 'true');
    rerender(<ChangeReviewView view={{ state: 'ready', changes: [view.changes[0]] }} />);
    expect(screen.queryByRole('navigation', { name: 'Changed files' })).not.toBeInTheDocument(); expect(screen.getByText('1 file changed, 2 lines added, 1 removed')).toBeInTheDocument();
  });

  it('puts About changes in the totals row, or after the notice when nothing is listed, only where Help exists', async () => {
    const user = userEvent.setup(); const openHelp = vi.fn();
    const change: ChangeEntryView = { id: 'file', path: 'src/a.ts', status: 'modified', added: 2, removed: 1, diff: { state: 'text', truncated: false, text: '+a' } };
    const ready: ChangeReviewState = { state: 'ready', changes: [change] };
    const bare = render(<ChangeReviewView view={ready} />);
    expect(screen.queryByRole('button', { name: 'About changes' })).not.toBeInTheDocument();
    bare.unmount();
    const { rerender } = render(<HelpContext.Provider value={openHelp}><ChangeReviewView view={ready} /></HelpContext.Provider>);
    const link = screen.getByRole('button', { name: 'About changes' });
    expect(link.parentElement).toContainElement(screen.getByText('1 file changed, 2 lines added, 1 removed'));
    await user.click(link); expect(openHelp).toHaveBeenCalledExactlyOnceWith('changes');
    rerender(<HelpContext.Provider value={openHelp}><ChangeReviewView view={{ state: 'empty', message: 'No changes in this chat.' }} /></HelpContext.Provider>);
    expect(screen.getByRole('status')).toHaveTextContent('No changes in this chat.');
    expect(screen.getAllByRole('button').map((button) => button.textContent)).toEqual(['About changes']);
    await user.click(screen.getByRole('button', { name: 'About changes' })); expect(openHelp).toHaveBeenLastCalledWith('changes');
  });

  it('keeps retry capability explicit, unknown errors generic, and supplied retry/cancel/terminal states static', () => {
    vi.useFakeTimers();
    try {
      const retry = vi.fn();
      const { rerender } = render(<RunNotice view={{ state: 'error', code: '__proto__' }} />);
      expect(screen.getByRole('region', { name: 'Run status' })).toHaveTextContent('Failed');
      expect(screen.getByText('AVA didn’t report why.')).toBeInTheDocument();
      expect(screen.queryByRole('button')).not.toBeInTheDocument();
      rerender(<RunNotice view={{ state: 'error', code: 'unavailable' }} onRetry={retry} />);
      expect(retry).not.toHaveBeenCalled(); expect(screen.queryByText('AVA didn’t report why.')).not.toBeInTheDocument();
      fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
      expect(retry).toHaveBeenCalledTimes(1);
      rerender(<RunNotice view={{ state: 'error', code: 'run_failed', message: '<script>reported cause</script>' }} onRetry={retry} />);
      expect(screen.getByText('<script>reported cause</script>')).toBeInTheDocument();
      rerender(<RunNotice view={{ state: 'retrying', attempt: 3, cause: '<script>supplied cause</script>' }} onRetry={retry} />);
      expect(screen.getByRole('region', { name: 'Run status' })).toHaveTextContent('Attempt 3'); expect(screen.getByText('<script>supplied cause</script>')).toBeInTheDocument();
      act(() => vi.advanceTimersByTime(60_000));
      expect(screen.getByText(/Attempt 3/)).toBeInTheDocument();
      for (const state of ['running', 'cancel-requested', 'canceled', 'completed'] as const) {
        rerender(<RunNotice view={{ state }} onRetry={retry} />);
        expect(screen.queryByRole('button')).not.toBeInTheDocument();
      }
      expect(vi.getTimerCount()).toBe(0);
      expect(retry).toHaveBeenCalledTimes(1);
    } finally { vi.useRealTimers(); }
  });

  it('offers Ask AVA to retry on the cause line only for a failure the caller can’t retry', () => {
    const ask = vi.fn(); const retry = vi.fn();
    const { rerender } = render(<RunNotice view={{ state: 'error', code: 'run_failed' }} onAskRetry={ask} />);
    fireEvent.click(within(screen.getByText('AVA didn’t report why.')).getByRole('button', { name: 'Ask AVA to retry' })); expect(ask).toHaveBeenCalledTimes(1);
    rerender(<RunNotice view={{ state: 'error', code: 'unavailable' }} onAskRetry={ask} />);
    expect(screen.getByRole('button', { name: 'Ask AVA to retry' })).toBeVisible();
    rerender(<RunNotice view={{ state: 'error', code: 'run_failed' }} onRetry={retry} onAskRetry={ask} />);
    expect(screen.getAllByRole('button').map((button) => button.textContent)).toEqual(['Retry']);
    for (const state of ['running', 'cancel-requested', 'canceled', 'completed'] as const) {
      rerender(<RunNotice view={{ state }} onAskRetry={ask} />);
      expect(screen.queryByRole('button')).not.toBeInTheDocument();
    }
    expect(ask).toHaveBeenCalledTimes(1); expect(retry).not.toHaveBeenCalled();
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

  it('blocks Send with a stated reason while the model is unavailable, keeps the model control usable, and recovers', async () => {
    const user = userEvent.setup(); const cb = callbacks(); const onOpenModel = vi.fn();
    const { rerender } = render(<ChatComposer view={composer} {...cb} onOpenModel={onOpenModel} modelUnavailable={unavailable} />);
    const send = screen.getByRole('button', { name: 'Send' }); const draft = screen.getByRole('textbox');
    // Reachable and explained rather than silently inert.
    expect(send).toBeEnabled(); expect(send).toHaveAttribute('aria-disabled', 'true');
    expect(send).toHaveAccessibleDescription('Connect Sample Lab to send with Sample'); expect(send).toHaveAttribute('title', 'Connect Sample Lab to send with Sample');
    expect(screen.getByRole('status')).toHaveTextContent(/^Sample Lab isn’t connected, so this chat can’t send\.$/); expect(screen.getByRole('status')).toBeVisible();
    fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true }); fireEvent.submit(screen.getByRole('form')); await user.click(send);
    expect(cb.onSend).not.toHaveBeenCalled();
    const model = screen.getByRole('button', { name: 'Model for this chat' });
    expect(model).toHaveTextContent('SampleNot connected'); expect(model).toHaveAccessibleDescription('Sample Lab isn’t connected.');
    expect(model).toHaveAttribute('title', 'Sample Lab isn’t connected. Choose model');
    await user.click(model); expect(onOpenModel).toHaveBeenCalledTimes(1);
    // A run in progress still offers Stop, with no Send reason and no blocked line.
    rerender(<ChatComposer view={{ ...composer, runState: 'running' }} {...cb} onOpenModel={onOpenModel} modelUnavailable={unavailable} />);
    expect(screen.getByRole('button', { name: 'Request stop' })).not.toHaveAttribute('aria-describedby');
    expect(screen.queryByRole('status')).not.toBeInTheDocument(); expect(screen.queryByRole('button', { name: 'Choose a model' })).not.toBeInTheDocument();
    rerender(<ChatComposer view={composer} {...cb} onOpenModel={onOpenModel} />);
    expect(send).not.toHaveAttribute('aria-disabled'); expect(send).not.toHaveAttribute('aria-describedby'); expect(send).toHaveAttribute('title', 'Send message · Ctrl/Cmd+Enter');
    expect(model).not.toHaveTextContent('Not connected'); expect(model).not.toHaveAttribute('aria-describedby');
    expect(screen.queryByRole('status')).not.toBeInTheDocument(); expect(screen.queryByRole('button', { name: 'Choose a model' })).not.toBeInTheDocument();
    fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true }); expect(cb.onSend).toHaveBeenCalledTimes(1);
    // The catalog select fallback states it too.
    rerender(<ChatComposer view={composer} {...cb} modelUnavailable={unavailable} />);
    expect(screen.getByRole('combobox', { name: 'Model for this chat' })).toHaveAccessibleDescription('Sample Lab isn’t connected.');
    expect(screen.getByText('Not connected')).toBeInTheDocument(); expect(screen.getByRole('button', { name: 'Send' })).toHaveAttribute('aria-disabled', 'true');
    expect(screen.getByRole('status')).toHaveTextContent('Sample Lab isn’t connected, so this chat can’t send.');
  });

  it('offers one way forward while blocked: Choose a model opens the picker from the model control, or focuses the fallback select', async () => {
    const user = userEvent.setup(); const cb = callbacks();
    // The picker opens with focus already on the model control, so it hands focus back there even after this line is gone.
    const onOpenModel = vi.fn(() => { expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveFocus(); });
    const { rerender } = render(<ChatComposer view={composer} {...cb} onOpenModel={onOpenModel} modelUnavailable={unavailable} />);
    const choose = screen.getByRole('button', { name: 'Choose a model' });
    expect(choose).toHaveAttribute('type', 'button');
    await user.click(choose);
    expect(onOpenModel).toHaveBeenCalledTimes(1); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveFocus();
    expect(cb.onSend).not.toHaveBeenCalled(); expect(cb.onModelChange).not.toHaveBeenCalled();
    // Keyboard reaches it too.
    choose.focus(); await user.keyboard('{Enter}'); expect(onOpenModel).toHaveBeenCalledTimes(2);
    rerender(<ChatComposer view={composer} {...cb} modelUnavailable={unavailable} />);
    await user.click(screen.getByRole('button', { name: 'Choose a model' }));
    expect(screen.getByRole('combobox', { name: 'Model for this chat' })).toHaveFocus();
    expect(cb.onModelChange).not.toHaveBeenCalled(); expect(cb.onSend).not.toHaveBeenCalled();
  });

  it('restates the blocked line on each fresh send attempt, never on a held key, and starts quiet when a block returns', async () => {
    const user = userEvent.setup(); const cb = callbacks();
    const { rerender } = render(<ChatComposer view={composer} {...cb} enterBehavior="send" modelUnavailable={unavailable} />);
    const status = screen.getByRole('status'); const draft = screen.getByRole('textbox');
    const text = () => status.firstElementChild!;
    expect(status).toHaveAttribute('aria-atomic', 'true');
    const first = text(); expect(first).not.toHaveAttribute('data-restated');
    // A fresh text node inside the same polite region re-announces the same words.
    fireEvent.keyDown(draft, { key: 'Enter' });
    const second = text(); expect(second).not.toBe(first); expect(second).toHaveAttribute('data-restated', 'true');
    expect(screen.getByRole('status')).toBe(status); expect(status).toHaveTextContent(/^Sample Lab isn’t connected, so this chat can’t send\.$/);
    fireEvent.keyDown(draft, { key: 'Enter', repeat: true }); expect(text()).toBe(second);
    fireEvent.keyDown(draft, { key: 'Enter', shiftKey: true }); expect(text()).toBe(second);
    fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true }); const third = text(); expect(third).not.toBe(second);
    await user.click(screen.getByRole('button', { name: 'Send' })); expect(text()).not.toBe(third);
    expect(cb.onSend).not.toHaveBeenCalled(); expect(cb.onDraftChange).not.toHaveBeenCalled();
    rerender(<ChatComposer view={composer} {...cb} enterBehavior="send" />);
    expect(screen.queryByRole('status')).not.toBeInTheDocument();
    rerender(<ChatComposer view={composer} {...cb} enterBehavior="send" modelUnavailable={unavailable} />);
    expect(screen.getByRole('status').firstElementChild).not.toHaveAttribute('data-restated');
  });

  it('makes the send key inert while blocked, held or not, without touching the draft; Shift+Enter and IME Enter still type', async () => {
    const user = userEvent.setup(); const cb = callbacks();
    const { rerender } = render(<ChatComposer view={composer} {...cb} enterBehavior="send" modelUnavailable={unavailable} />);
    const draft = screen.getByRole('textbox');
    // fireEvent returns false when the default (inserting a newline) was prevented.
    expect(fireEvent.keyDown(draft, { key: 'Enter' })).toBe(false);
    expect(fireEvent.keyDown(draft, { key: 'Enter', repeat: true })).toBe(false);
    expect(fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true })).toBe(false);
    expect(fireEvent.keyDown(draft, { key: 'Enter', shiftKey: true })).toBe(true);
    expect(fireEvent.keyDown(draft, { key: 'Enter', isComposing: true })).toBe(true);
    expect(fireEvent.keyDown(draft, { key: 'Enter', keyCode: 229 })).toBe(true);
    await user.click(draft); await user.keyboard('{Enter}');
    expect(cb.onDraftChange).not.toHaveBeenCalled(); expect(cb.onSend).not.toHaveBeenCalled();
    await user.keyboard('{Shift>}{Enter}{/Shift}');
    expect(cb.onDraftChange).toHaveBeenCalledTimes(1); expect(cb.onDraftChange).toHaveBeenLastCalledWith(expect.stringContaining('\n'));
    // Newline mode: plain Enter types as always; the modifier chord is the inert one.
    rerender(<ChatComposer view={composer} {...cb} enterBehavior="newline" modelUnavailable={unavailable} />);
    expect(fireEvent.keyDown(draft, { key: 'Enter' })).toBe(true);
    expect(fireEvent.keyDown(draft, { key: 'Enter', metaKey: true })).toBe(false);
    // Unblocked, nothing changes: a held send key still neither sends again nor is swallowed.
    rerender(<ChatComposer view={composer} {...cb} enterBehavior="send" />);
    expect(fireEvent.keyDown(draft, { key: 'Enter', repeat: true })).toBe(true); expect(cb.onSend).not.toHaveBeenCalled();
    expect(fireEvent.keyDown(draft, { key: 'Enter' })).toBe(false); expect(cb.onSend).toHaveBeenCalledTimes(1);
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
