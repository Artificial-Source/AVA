import { StrictMode, type SyntheticEvent } from 'react';
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it } from 'vitest';
import type { ToolView, TurnState } from '../chat/contracts';
import type { ToolDisplay } from '../chat/toolDisplay';
import { defaultConversation } from '../screens/behaviorPreferences';
import { createInitialState } from '../state/fixtures';
import type { Message } from '../state/types';
import { Conversation } from './Conversation';
import { ToolActivity } from './ToolActivity';
import { prepareAssistantContent } from '../chat/widgets/AssistantContentHost';

type Assistant = Extract<Message, { role: 'assistant' }>;
function record(id: string, state: ToolView['state'] = 'running'): ToolView { return Object.freeze({ id, action: `Read ${id}`, detail: `${id}.ts`, state, output: Object.freeze({ text: `First ${id}\nSupplied output`, truncated: false }) }); }
function message(activity = [record('one'), record('two')], lifecycle: TurnState | undefined = 'streaming'): Assistant { return { id: 'same-turn', role: 'assistant', markdown: 'Partial assistant text', lifecycle, activity }; }
function content(turn: Assistant, display: ToolDisplay = 'auto', active = true) {
  return <StrictMode><Conversation chat={{ ...createInitialState().chats[0], messages: [turn] }} preferences={{ ...defaultConversation, toolDisplay: display }} active={active} /><textarea aria-label="Editor" defaultValue="Keep draft selection" /></StrictMode>;
}
function row(id: string) { return screen.getByRole('button', { name: new RegExp(`^Read ${id} `) }); }
function geometry(node: HTMLElement, height: () => number) { Object.defineProperty(node, 'scrollHeight', { configurable: true, get: height }); Object.defineProperty(node, 'clientHeight', { configurable: true, value: 100 }); }
function selectText(output: HTMLElement) {
  const text = output.querySelector('code span')!.firstChild!; const range = document.createRange(); range.setStart(text, 0); range.setEnd(text, 5);
  const selection = window.getSelection()!; selection.removeAllRanges(); selection.addRange(range); return selection;
}
afterEach(() => { window.getSelection()?.removeAllRanges(); });

describe('display presets and stable explicit intent', () => {
  it('preserves the shared standalone disclosure inside its own open tool-detail dialog', async () => {
    render(<dialog open aria-label="Tool detail"><ToolActivity records={[record('dialog')]} /></dialog>);
    const group = screen.getByRole('button', { name: /^\d+ tools?\b/ }); fireEvent.click(group);
    expect(group).toHaveAttribute('aria-expanded', 'true'); expect(await screen.findByLabelText('Output: Read dialog')).toBeVisible();
    fireEvent.click(group); expect(group).toHaveAttribute('aria-expanded', 'false');
  });
  it.each([
    ['compact', [false, false, false, false, false, false], [false, false, false, false, false, false]],
    ['auto', [false, true, false, true, false, true], [false, false, false, true, false, true]],
    ['expanded', [true, true, true, true, true, true], [true, true, true, true, true, true]],
  ] as const)('%s applies per-record defaults while keeping every supplied summary/status through terminal delivery', async (display, initial, terminal) => {
    const states = ['queued', 'running', 'completed', 'failed', 'canceled', 'unavailable'] as const;
    const turn = message(states.map((state) => record(state, state))); const before = JSON.stringify(turn);
    Object.freeze(turn.activity); Object.freeze(turn);
    const view = render(content(turn, display));
    expect(states.map((state) => row(state).getAttribute('aria-expanded'))).toEqual(initial.map(String));
    if (display !== 'compact') await screen.findByLabelText('Output: Read failed');
    for (const lifecycle of ['committed', 'failed', 'canceled'] as const) {
      view.rerender(content({ ...turn, lifecycle }, display));
      expect(states.map((state) => row(state).getAttribute('aria-expanded'))).toEqual(terminal.map(String));
      // Never fabricated as completed; an ended turn shows unstarted/unfinished work as such, in gray.
      expect(row('running')).toHaveTextContent(lifecycle === 'committed' ? 'Working' : 'Not finished');
      expect(row('queued')).toHaveTextContent(lifecycle === 'committed' ? 'Queued' : 'Not run');
      expect(screen.getAllByRole('listitem')).toHaveLength(6);
    }
    expect(JSON.stringify(turn)).toBe(before);
    view.rerender(content({ ...turn, lifecycle: undefined }, display));
    expect(row('running')).toHaveAttribute('aria-expanded', String(display !== 'compact'));
  });
  it('preserves row/group/all choices within a mode; every actual different preset is fresh intent without replacing retained DOM', async () => {
    let turn = message([record('one'), record('two', 'failed')]); const view = render(content(turn));
    const article = screen.getByRole('article'); const group = screen.getByRole('button', { name: /^\d+ tools?\b/ }); const first = row('one'); const second = row('two');
    const failureOutput = await screen.findByLabelText('Output: Read two');
    fireEvent.click(first); expect(first).toHaveAttribute('aria-expanded', 'false');
    turn = { ...turn, markdown: 'Partial assistant text growing', activity: turn.activity!.map((entry) => ({ ...entry, output: { text: entry.output!.text + '\nMore', truncated: false } })) };
    view.rerender(content(turn)); expect(first).toHaveAttribute('aria-expanded', 'false'); expect(screen.getByLabelText('Output: Read two')).toBe(failureOutput);
    view.rerender(content(turn, 'expanded')); expect(first).toHaveAttribute('aria-expanded', 'true'); expect(screen.getByLabelText('Output: Read two')).toBe(failureOutput);
    expect(screen.getByRole('article')).toBe(article); expect(screen.getByRole('button', { name: /^\d+ tools?\b/ })).toBe(group); expect(row('one')).toBe(first); expect(row('two')).toBe(second);
    fireEvent.click(first); view.rerender(content({ ...turn }, 'expanded')); expect(first).toHaveAttribute('aria-expanded', 'false');
    view.rerender(content(turn)); expect(first).toHaveAttribute('aria-expanded', 'true'); // Earlier Auto collapse cannot return.
    fireEvent.click(screen.getByRole('button', { name: 'Collapse all tools' })); expect(first).toHaveAttribute('aria-expanded', 'false'); expect(second).toHaveAttribute('aria-expanded', 'false');
    fireEvent.click(first); fireEvent.click(screen.getByRole('button', { name: 'Expand all tools' })); fireEvent.click(screen.getByRole('button', { name: 'Collapse all tools' })); expect(first).toHaveAttribute('aria-expanded', 'false');
    fireEvent.click(group); expect(first).toHaveAttribute('aria-expanded', 'true'); fireEvent.click(first);
    view.rerender(content({ ...turn, lifecycle: 'committed' })); expect(first).toHaveAttribute('aria-expanded', 'false'); expect(second).toHaveAttribute('aria-expanded', 'true');
    view.rerender(content(turn, 'compact')); expect(first).toHaveAttribute('aria-expanded', 'false'); expect(second).toHaveAttribute('aria-expanded', 'false');
    fireEvent.click(screen.getByRole('button', { name: 'Expand all tools' })); view.rerender(content({ ...turn, lifecycle: 'canceled' }, 'compact')); expect(first).toHaveAttribute('aria-expanded', 'true');
  });
  it('follows supplied tool transitions independently while the assistant is still streaming', async () => {
    const turn = message([record('one', 'queued'), record('two')]); const view = render(content(turn)); const first = row('one');
    expect(first).toHaveAttribute('aria-expanded', 'false');
    view.rerender(content({ ...turn, activity: [record('one'), record('two')] })); await screen.findByLabelText('Output: Read one'); expect(first).toHaveAttribute('aria-expanded', 'true');
    first.focus();
    for (const [state, open] of [['completed', false], ['failed', true], ['unavailable', true], ['canceled', false]] as const) {
      view.rerender(content({ ...turn, activity: [record('one', state), record('two')] }));
      expect(row('one')).toBe(first); expect(first).toHaveAttribute('aria-expanded', String(open)); expect(first).toHaveFocus();
      expect(row('two')).toHaveAttribute('aria-expanded', 'true'); expect(screen.getByRole('article')).toHaveAttribute('data-turn-state', 'streaming');
    }
  });
  it('preserves selected assistant prose across presets and terminal formatting without pinning unrelated tools', async () => {
    await prepareAssistantContent(); const turn = message(); const view = render(content(turn));
    const text = screen.getByText('Partial assistant text').firstChild!; const range = document.createRange(); range.setStart(text, 0); range.setEnd(text, 7);
    const selection = window.getSelection()!; selection.addRange(range); fireEvent(document, new Event('selectionchange'));
    view.rerender(content(turn, 'expanded')); expect(selection.toString()).toBe('Partial');
    view.rerender(content(turn)); expect(selection.toString()).toBe('Partial');
    view.rerender(content({ ...turn, lifecycle: 'committed', markdown: 'Partial assistant text\n\n**Finished**' }));
    expect(selection.toString()).toBe('Partial'); expect(row('one')).toHaveAttribute('aria-expanded', 'false'); expect(screen.queryByText('Finished', { selector: 'strong' })).not.toBeInTheDocument();
    selection.removeAllRanges(); fireEvent(document, new Event('selectionchange')); expect(screen.getByText('Finished', { selector: 'strong' })).toBeVisible();
  });
  it('retains immutable same-ID row choices, but removes them when a row or group departs', async () => {
    let turn = message(); const view = render(content(turn)); await screen.findByLabelText('Output: Read one'); fireEvent.click(row('one'));
    turn = { ...turn, activity: [record('one'), record('two'), record('three')] }; view.rerender(content(turn));
    expect(row('one')).toHaveAttribute('aria-expanded', 'false'); expect(row('three')).toHaveAttribute('aria-expanded', 'true');
    view.rerender(content({ ...turn, activity: [record('two')] })); view.rerender(content(turn)); expect(row('one')).toHaveAttribute('aria-expanded', 'true');
    fireEvent.click(screen.getByRole('button', { name: 'Collapse all tools' })); view.rerender(content({ ...turn, activity: [] })); view.rerender(content(turn)); expect(row('one')).toHaveAttribute('aria-expanded', 'true');
  });
});

describe('Auto reading safety without automatic-scroll pinning', () => {
  it.each(['focus', 'pointer', 'wheel', 'selection', 'pending-selection'] as const)('keeps %s inspection row-scoped through terminal delivery and until an explicit override', async (inspection) => {
    const turn = message(); const view = render(content(turn)); const output = await screen.findByLabelText('Output: Read one'); await screen.findByLabelText('Output: Read two');
    const editor = screen.getByRole('textbox') as HTMLTextAreaElement; editor.focus(); editor.setSelectionRange(2, 6);
    geometry(output, () => 800); output.scrollTop = 50; fireEvent.scroll(output);
    if (inspection === 'focus') act(() => output.focus());
    else if (inspection === 'pointer') { fireEvent.pointerDown(output); fireEvent.pointerUp(window); }
    else if (inspection === 'wheel') { fireEvent.wheel(output, { deltaY: -200 }); expect(output).not.toHaveFocus(); }
    else { selectText(output); if (inspection === 'selection') fireEvent(document, new Event('selectionchange')); }
    const terminal = { ...turn, lifecycle: 'committed' as const, activity: turn.activity!.map((entry) => ({ ...entry, state: 'completed' as const, output: { ...entry.output!, text: entry.output!.text + '\nFinished' } })) };
    view.rerender(content(terminal));
    expect(row('one')).toHaveAttribute('aria-expanded', 'true'); expect(row('two')).toHaveAttribute('aria-expanded', 'false');
    expect(screen.getByLabelText('Output: Read one')).toBe(output); expect(output.scrollTop).toBe(50);
    if (inspection.includes('selection')) expect(window.getSelection()!.toString()).toBe('First');
    if (inspection === 'focus') expect(output).toHaveFocus(); else { expect(editor).toHaveFocus(); expect(editor.selectionStart).toBe(2); expect(editor.selectionEnd).toBe(6); }
    window.getSelection()!.removeAllRanges(); act(() => editor.focus()); fireEvent(document, new Event('selectionchange'));
    view.rerender(content({ ...terminal, markdown: 'Final text' })); expect(row('one')).toHaveAttribute('aria-expanded', 'true');
    fireEvent.click(screen.getByRole('button', { name: 'Expand all tools' })); fireEvent.click(screen.getByRole('button', { name: 'Collapse all tools' })); expect(row('one')).toHaveAttribute('aria-expanded', 'false');
    view.rerender(content(turn)); expect(row('one')).toHaveAttribute('aria-expanded', 'false');
    view.rerender(content(turn, 'expanded')); view.rerender(content(terminal)); expect(row('one')).toHaveAttribute('aria-expanded', 'false');
  });
  it('still folds uninspected output after passive growth and generated scroll events', async () => {
    const turn = message(); let height = 500; const view = render(content(turn)); const output = await screen.findByLabelText('Output: Read one');
    geometry(output, () => height); output.scrollTop = 400; fireEvent.scroll(output);
    height = 700; const growing = { ...turn, activity: turn.activity!.map((entry) => ({ ...entry, output: { ...entry.output!, text: entry.output!.text + '\nGrowing' } })) };
    view.rerender(content(growing)); expect(output.scrollTop).toBe(700); fireEvent.scroll(output);
    view.rerender(content({ ...growing, lifecycle: 'committed' })); expect(row('one')).toHaveAttribute('aria-expanded', 'false'); expect(output).not.toBeInTheDocument();
  });
  it.each(['hidden', 'inert', 'inactive', 'overlay', 'stale', 'other-root', 'departed'] as const)('rejects %s inspection callbacks', async (invalid) => {
    const turn = message(); const view = render(content(turn)); const output = await screen.findByLabelText('Output: Read one');
    const root = output.closest<HTMLElement>('[data-tool-intent]')!;
    const propsKey = Object.keys(root).find((key) => key.startsWith('__reactProps'))!;
    const callback = (root as unknown as Record<string, { onWheelCapture: (event: SyntheticEvent<HTMLDivElement>) => void }>)[propsKey].onWheelCapture;
    let target = root; let dialog: HTMLDialogElement | undefined;
    if (invalid === 'hidden') root.hidden = true;
    else if (invalid === 'inert') root.setAttribute('inert', '');
    else if (invalid === 'inactive') view.rerender(content(turn, 'auto', false));
    else if (invalid === 'overlay') { dialog = document.createElement('dialog'); dialog.open = true; document.body.append(dialog); }
    else if (invalid === 'stale') view.rerender(content({ ...turn, activity: turn.activity!.map((entry) => ({ ...entry })) }));
    else if (invalid === 'other-root') target = screen.getByRole('textbox');
    else view.unmount();
    act(() => callback({ currentTarget: target, target: output } as unknown as SyntheticEvent<HTMLDivElement>));
    root.hidden = false; root.removeAttribute('inert'); dialog?.remove();
    if (invalid === 'departed') { expect(output).not.toBeInTheDocument(); return; }
    view.rerender(content({ ...turn, lifecycle: 'committed' })); expect(row('one')).toHaveAttribute('aria-expanded', 'false');
  });
});
