import { StrictMode } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { AppShell } from './AppShell';
import { installDialogTestSupport } from '../screens/dialogTestSupport';
import { prepareAssistantContent } from '../chat/widgets/AssistantContentHost';

const frames = new Map<number, FrameRequestCallback>(); let frameId = 0;
beforeEach(async () => {
  installDialogTestSupport(); frames.clear(); frameId = 0;
  await prepareAssistantContent(); await import('./SuppliedRunPreview');
  vi.stubGlobal('requestAnimationFrame', vi.fn((callback: FrameRequestCallback) => { const id = frameId++; frames.set(id, callback); return id; }));
  vi.stubGlobal('cancelAnimationFrame', vi.fn((id: number) => { frames.delete(id); }));
});
afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });
async function begin() {
  await act(async () => { fireEvent.click(screen.getByRole('button', { name: /^(Run|Retry) preview$/ })); });
  expect(screen.getByRole('button', { name: 'Stop preview' })).toBeVisible();
}
async function paused() { await begin(); fireEvent.click(screen.getByRole('button', { name: 'Pause preview' })); }
function step() { fireEvent.click(screen.getByRole('button', { name: 'Next snapshot' })); }
function run() { return screen.getAllByRole('article', { name: 'AVA response' }).at(-1)!; }

describe('explicit normal-chat supplied run integration', () => {
  it('keeps Send user-only, fixed response facts/context, mounted editor/history and draft selection while the preview progresses', async () => {
    const fetch = vi.fn(); const socket = vi.fn(); vi.stubGlobal('fetch', fetch); vi.stubGlobal('WebSocket', socket);
    render(<StrictMode><AppShell /></StrictMode>);
    const draft = screen.getByRole('textbox') as HTMLTextAreaElement;
    const historical = screen.getByRole('article', { name: 'AVA response' }); const historicalBody = historical.querySelector('p');
    fireEvent.change(draft, { target: { value: 'Local-only message' } }); fireEvent.click(screen.getByRole('button', { name: 'Send' }));
    expect(screen.getAllByRole('article', { name: 'AVA response' })).toEqual([historical]);
    fireEvent.change(draft, { target: { value: 'Keep editing this draft' } });
    const context = screen.getByRole('button', { name: /^Open Context/ }).textContent;
    await paused();
    expect(run()).toHaveAttribute('data-turn-state', 'streaming');
    expect(within(run()).getByLabelText('Model: Aster')).toBeVisible(); expect(within(run()).getByLabelText('Thinking: High')).toBeVisible();
    expect(draft).toHaveValue('Keep editing this draft'); expect(screen.getByRole('textbox')).toBe(draft);
    draft.focus(); draft.setSelectionRange(5, 12); const focus = vi.spyOn(draft, 'focus');
    const next = screen.getByRole('button', { name: 'Next snapshot' }); fireEvent.click(next);
    expect(document.activeElement).toBe(draft); expect(draft.selectionStart).toBe(5); expect(draft.selectionEnd).toBe(12); expect(focus).not.toHaveBeenCalled();
    expect(screen.getAllByRole('article', { name: 'AVA response' })[0]).toBe(historical); expect(historical.querySelector('p')).toBe(historicalBody);
    for (let i = 0; i < 4; i++) step();
    expect(run()).toHaveAttribute('data-turn-state', 'committed'); expect(run()).toHaveTextContent('Saved filters now survive a return from a result.');
    expect(screen.queryByRole('button', { name: 'Stop preview' })).not.toBeInTheDocument(); expect(frames.size).toBe(0);
    expect(screen.getByRole('button', { name: /^Open Context/ })).toHaveTextContent(context!);
    expect(fetch).not.toHaveBeenCalled(); expect(socket).not.toHaveBeenCalled();
  });
  it('Stop retains accepted partial text/tools, revokes even a captured frame, and Retry creates a distinct turn', async () => {
    render(<StrictMode><AppShell /></StrictMode>); await begin();
    const firstFrame = [...frames.values()][0]; act(() => firstFrame(0)); frames.clear();
    const partial = run(); const text = partial.textContent;
    fireEvent.click(screen.getByRole('button', { name: 'Stop preview' }));
    expect(partial).toHaveAttribute('data-turn-state', 'canceled'); expect(partial.textContent).toContain('I’ll trace how search filters');
    const stoppedText = partial.textContent; act(() => firstFrame(0)); expect(partial.textContent).toBe(stoppedText);
    expect(partial.textContent).not.toBe(text); expect(within(partial).getAllByRole('listitem')).toHaveLength(3);
    await paused(); expect(run()).not.toBe(partial); expect(screen.getAllByRole('article', { name: 'AVA response' })).toHaveLength(3);
    expect(partial).toHaveAttribute('data-turn-state', 'canceled'); expect(frames.size).toBe(0);
  });
  it('plays real local failure and retry transitions; burst delivery remains finite', async () => {
    render(<AppShell />); await paused(); fireEvent.click(screen.getByRole('button', { name: 'Stop preview' }));
    fireEvent.change(screen.getByRole('combobox', { name: 'Preview example' }), { target: { value: 'failure' } });
    await paused(); for (let i = 0; i < 5; i++) step();
    expect(run()).toHaveAttribute('data-turn-state', 'failed'); expect(run()).toHaveTextContent('The navigation test couldn’t read a browser-history result');
    expect(within(run()).getByRole('button', { name: /Check.*Failed/ })).toBeVisible();
    fireEvent.change(screen.getByRole('combobox', { name: 'Preview example' }), { target: { value: 'burst' } });
    await paused(); for (let i = 0; i < 7; i++) step();
    expect(run()).toHaveAttribute('data-turn-state', 'committed'); expect(run()).toHaveTextContent('Replaying the test across the full result list:'); expect(frames.size).toBe(0);
  });
  it.each(['Settings', 'models', 'chat', 'new', 'child'])('cancels atomically on %s departure; old callbacks cannot write or restart hidden history', async (destination) => {
    render(<AppShell />); const draft = screen.getByRole('textbox') as HTMLTextAreaElement; const draftText = draft.value;
    await begin(); const late = [...frames.values()][0]; act(() => late(0)); frames.clear();
    const partial = run();
    if (destination === 'Settings') await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Settings' })); });
    else if (destination === 'models') await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Model for this chat' })); });
    else if (destination === 'new') fireEvent.click(screen.getByRole('button', { name: 'New chat' }));
    else if (destination === 'child') await act(async () => { fireEvent.click(screen.getByRole('button', { name: /^Working Review back navigation/ })); });
    else fireEvent.click(within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Simplify route config Completed' }));
    const after = partial.textContent; act(() => late(0)); expect(partial.textContent).toBe(after); expect(frames.size).toBe(0);
    if (destination === 'Settings') {
      expect(partial.closest('[hidden][inert]')).not.toBeNull();
      const pause = document.querySelector('button[title="Pause the preview reply"]'); if (pause) fireEvent.click(pause);
      await act(async () => { fireEvent.click(await screen.findByRole('button', { name: 'Back to chat' })); });
    } else if (destination === 'models') await act(async () => { fireEvent.click(await screen.findByRole('button', { name: 'Close' })); });
    else if (destination === 'child') fireEvent.click(screen.getByRole('button', { name: 'Back to parent' }));
    else fireEvent.click(within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Remember search filters Stopped' }));
    expect(run()).toHaveAttribute('data-turn-state', 'canceled'); expect(screen.getByRole('textbox')).toHaveValue(draftText);
    expect(screen.getByRole('button', { name: 'Retry preview' })).toBeEnabled();
  });
  it('cleans timer/frame work under StrictMode unmount, including old queued callbacks', async () => {
    // Leave React's immediate scheduling real; assert the fixture's timeout/frame resources.
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] }); const view = render(<StrictMode><AppShell /></StrictMode>); await begin();
    const late = [...frames.values()][0]; expect(vi.getTimerCount()).toBe(1);
    view.unmount(); expect(vi.getTimerCount()).toBe(0); expect(frames.size).toBe(0);
    act(() => { late(0); vi.runAllTimers(); }); expect(screen.queryByRole('main')).not.toBeInTheDocument();
  });
});
