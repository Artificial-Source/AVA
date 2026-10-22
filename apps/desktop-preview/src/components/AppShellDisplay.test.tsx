import { StrictMode, type ComponentProps } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { AppShell } from './AppShell';
import type { Conversation } from './Conversation';
import * as fixtures from '../state/fixtures';
import * as reducer from '../state/reducer';
import { installDialogTestSupport } from '../screens/dialogTestSupport';
import { prepareAssistantContent } from '../chat/widgets/AssistantContentHost';

const owner = vi.hoisted(() => ({ props: null as ComponentProps<typeof Conversation> | null }));
vi.mock('./Conversation', async (importOriginal) => {
  const actual = await importOriginal<typeof import('./Conversation')>();
  return { Conversation: (props: ComponentProps<typeof Conversation>) => { owner.props = props; return <actual.Conversation {...props} />; } };
});
const frames = new Map<number, FrameRequestCallback>(); let frameId = 0;
beforeEach(async () => {
  installDialogTestSupport(); frames.clear(); frameId = 0;
  await prepareAssistantContent(); await import('./SuppliedRunPreview');
  vi.stubGlobal('requestAnimationFrame', vi.fn((callback: FrameRequestCallback) => { const id = frameId++; frames.set(id, callback); return id; }));
  vi.stubGlobal('cancelAnimationFrame', vi.fn((id: number) => { frames.delete(id); }));
});
afterEach(() => { vi.unstubAllGlobals(); });
function display() { return screen.getByRole('combobox', { name: 'Tool details' }) as HTMLSelectElement; }
function choose(value: string) { fireEvent.change(display(), { target: { value } }); }
function groups() { return screen.getAllByRole('button', { name: /^\d+ tools?\b/ }); }
function library() { return within(screen.getByRole('navigation', { name: 'All project chats' })); }
async function settings() {
  fireEvent.click(screen.getByRole('button', { name: 'Settings' })); const nav = await screen.findByRole('navigation', { name: 'Settings sections' });
  fireEvent.click(within(nav).getByRole('button', { name: 'Conversation behavior' }));
}

describe('current parent-chat display authority', () => {
  it('changes only visit-local presentation: preserves state/history/facts, mounted editor/caret, model/mode and manual choices on no-op delivery', async () => {
    const initial = vi.spyOn(fixtures, 'createInitialState'); const reduce = vi.spyOn(reducer, 'previewReducer');
    render(<StrictMode><AppShell /></StrictMode>); const state = initial.mock.results.at(-1)!.value; const before = JSON.stringify(state);
    const chat = owner.props!.chat; const editor = screen.getByRole('textbox') as HTMLTextAreaElement; const draft = editor.value;
    const context = screen.getByRole('button', { name: /^Open Context/ }).textContent; const model = screen.getByRole('button', { name: 'Model for this chat' }).textContent;
    const article = screen.getByRole('article', { name: 'AVA response' }); const group = groups()[0]; const control = display();
    editor.focus(); editor.setSelectionRange(3, 9); const focus = vi.spyOn(editor, 'focus');
    choose('expanded'); expect(await screen.findAllByLabelText(/^Diff:/)).toHaveLength(2);
    expect(group).toHaveAttribute('aria-expanded', 'true'); fireEvent.click(group); expect(control).toHaveValue('custom');
    act(() => owner.props!.onDisplayChange!('expanded', control)); expect(group).toHaveAttribute('aria-expanded', 'false'); // A no-op delivery keeps the manual choice.
    choose('expanded'); expect(group).toHaveAttribute('aria-expanded', 'true'); expect(control).toHaveValue('expanded'); // Choosing the preset again clears it.
    choose('auto'); expect(screen.getByRole('button', { name: /Pasted image Failed/ })).toHaveAttribute('aria-expanded', 'true');
    expect(owner.props!.chat).toBe(chat); expect(JSON.stringify(state)).toBe(before); expect(reduce).not.toHaveBeenCalled();
    expect(screen.getByRole('article', { name: 'AVA response' })).toBe(article); expect(groups()[0]).toBe(group); expect(display()).toBe(control);
    expect(screen.getByRole('textbox')).toBe(editor); expect(editor).toHaveValue(draft); expect(editor).toHaveFocus(); expect(editor.selectionStart).toBe(3); expect(editor.selectionEnd).toBe(9); expect(focus).not.toHaveBeenCalled();
    expect(screen.getByRole('button', { name: /^Open Context/ })).toHaveTextContent(context!); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(model!); expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('build');
    expect(screen.getAllByRole('article', { name: 'Your message' })).toHaveLength(1); expect(screen.getAllByRole('article', { name: 'AVA response' })).toHaveLength(1);
  });
  it('matches Settings, derives Custom without losing wraps, restores Show, and shows the preset only where tools exist', async () => {
    render(<AppShell />); choose('expanded'); const quick = display();
    await settings(); expect(display()).toHaveValue('expanded');
    fireEvent.click(screen.getByLabelText('Show completed-tool output'));
    fireEvent.click(screen.getByLabelText('Wrap code lines')); fireEvent.click(screen.getByLabelText('Wrap diff lines'));
    fireEvent.change(screen.getByLabelText('After sending a message'), { target: { value: 'keep' } });
    fireEvent.click(screen.getByRole('button', { name: 'Back to chat' })); expect(display()).toBe(quick); expect(quick).toHaveValue('custom'); expect(within(quick).getByRole('option', { name: 'Custom · output hidden' })).toBeDisabled();
    fireEvent.click(groups()[0]); choose('expanded'); expect(groups()[0]).toHaveAttribute('aria-expanded', 'true'); // A preset clears manual overrides and restores Show.
    expect(owner.props!.preferences).toEqual({ toolDisplay: 'expanded', showCompletedOutput: true, wrapCode: true, wrapDiff: false, scrollAfterSend: false });
    fireEvent.click(screen.getByRole('button', { name: /^Completed Map return paths/ })); await screen.findByRole('region', { name: /Child conversation:/ }); expect(screen.queryByRole('combobox', { name: 'Tool details' })).not.toBeInTheDocument();
    expect(screen.getByRole('main')).toHaveAttribute('data-code-wrap', 'true'); fireEvent.click(screen.getByRole('button', { name: 'Back to parent' })); expect(display()).toHaveValue('expanded');
    // Empty chats are logo-only: no tool-details control until the chat has tools; children never get one.
    fireEvent.click(screen.getByRole('button', { name: 'New chat' })); expect(screen.getByRole('region', { name: /^Conversation:/ })).toHaveAttribute('data-empty', 'true');
    expect(screen.queryByRole('combobox', { name: 'Tool details' })).not.toBeInTheDocument();
    expect(screen.getByRole('textbox')).toHaveValue(''); expect(screen.getByRole('button', { name: 'Send' })).toHaveAttribute('aria-disabled', 'true');
    await settings(); expect(display()).toHaveValue('expanded'); choose('auto'); expect(display()).toHaveValue('auto');
    fireEvent.click(screen.getByRole('button', { name: 'Back to chat' })); expect(screen.queryByRole('combobox', { name: 'Tool details' })).not.toBeInTheDocument();
    fireEvent.click(library().getByRole('button', { name: 'Remember search filters' })); expect(display()).toHaveValue('auto');
    await settings(); fireEvent.click(screen.getByRole('button', { name: 'Reset conversation' })); fireEvent.click(screen.getByRole('button', { name: 'Back to chat' }));
    expect(display()).toHaveValue('compact'); expect(screen.getByRole('main')).toHaveAttribute('data-code-wrap', 'false'); expect(screen.getByRole('main')).toHaveAttribute('data-diff-wrap', 'true');
  });
  it.each(['hidden', 'inert', 'overlay', 'settings', 'settings-return', 'child', 'chat', 'new-chat', 'preferences', 'state', 'other-control', 'unmounted'] as const)('rejects a saved %s owner callback independently of the select guard', async (invalid) => {
    const reduce = vi.spyOn(reducer, 'previewReducer'); const view = render(<StrictMode><AppShell /></StrictMode>);
    const control = display(); const callback = owner.props!.onDisplayChange!;
    let target = control;
    if (invalid === 'hidden') control.hidden = true;
    else if (invalid === 'inert') control.setAttribute('inert', '');
    else if (invalid === 'overlay') { fireEvent.click(screen.getByRole('button', { name: 'Model for this chat' })); await screen.findByRole('dialog', { name: /^Model for/ }); }
    else if (invalid === 'settings') await settings();
    else if (invalid === 'settings-return') { await settings(); fireEvent.click(screen.getByRole('button', { name: 'Back to chat' })); }
    else if (invalid === 'child') { fireEvent.click(screen.getByRole('button', { name: /^Completed Map return paths/ })); await screen.findByRole('region', { name: /Child conversation:/ }); }
    else if (invalid === 'chat') fireEvent.click(library().getByRole('button', { name: 'Simplify route config Completed' }));
    else if (invalid === 'new-chat') fireEvent.click(screen.getByRole('button', { name: 'New chat' }));
    else if (invalid === 'preferences') choose('auto');
    else if (invalid === 'state') fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Updated draft owner' } });
    else if (invalid === 'other-control') target = screen.getByRole('combobox', { name: 'Mode for this chat' }) as HTMLSelectElement;
    else view.unmount();
    const props = owner.props; const count = reduce.mock.calls.length;
    act(() => callback('expanded', target)); expect(owner.props).toBe(props); expect(reduce).toHaveBeenCalledTimes(count);
    if (invalid === 'hidden') control.hidden = false; if (invalid === 'inert') control.removeAttribute('inert');
  });
  it('does not cancel, pause, resume, send or Stop the supplied run when selecting a preset; terminal delivery retains truthful tools', async () => {
    const reduce = vi.spyOn(reducer, 'previewReducer'); render(<StrictMode><AppShell /></StrictMode>); choose('auto');
    await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Run preview' })); });
    const queuedFrame = [...frames.values()][0]; const cancel = vi.mocked(cancelAnimationFrame); const cancels = cancel.mock.calls.length; const actions = reduce.mock.calls.length;
    choose('expanded'); expect(reduce).toHaveBeenCalledTimes(actions); expect(cancel).toHaveBeenCalledTimes(cancels); expect([...frames.values()]).toContain(queuedFrame); expect(screen.getByRole('button', { name: 'Stop preview' })).toBeVisible();
    fireEvent.click(screen.getByRole('button', { name: 'Pause preview' }));
    const turn = screen.getAllByRole('article', { name: 'AVA response' }).at(-1)!; const supplied = within(turn);
    fireEvent.click(screen.getByRole('button', { name: 'Next snapshot' }));
    const output = await supplied.findByLabelText('Output: Read'); const row = supplied.getByRole('button', { name: /^Read .* Working/ }); expect(row).toHaveAttribute('aria-expanded', 'true');
    choose('auto'); expect(supplied.getByLabelText('Output: Read')).toBe(output);
    fireEvent.click(screen.getByRole('button', { name: 'Stop preview' })); expect(turn).toHaveAttribute('data-turn-state', 'canceled'); expect(row).toHaveTextContent('Not finished'); expect(row).toHaveAttribute('aria-expanded', 'false');
    choose('expanded'); expect(await supplied.findByLabelText('Output: Read')).toHaveTextContent('export function restoreSearch'); expect(screen.getByRole('button', { name: 'Retry preview' })).toBeVisible();
    const snapshot = owner.props!.chat; const stoppedActions = reduce.mock.calls.length; choose('compact'); choose('auto'); expect(owner.props!.chat).toBe(snapshot); expect(reduce).toHaveBeenCalledTimes(stoppedActions);
  });
});
