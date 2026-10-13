import { StrictMode, type MouseEvent } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { Conversation } from './Conversation';
import { createInitialState } from '../state/fixtures';
import type { Chat, Message } from '../state/types';
import { defaultConversation } from '../screens/behaviorPreferences';
import { prepareAssistantContent } from '../chat/widgets/AssistantContentHost';
import ToolActivityDetails from '../chat/widgets/ToolActivityDetails';
import type { ToolView } from '../chat/contracts';
import { useBottomFollow } from '../chat/useBottomFollow';

beforeEach(async () => { await prepareAssistantContent(); });
function assistant(text = 'Partial response'): Extract<Message, { role: 'assistant' }> {
  return { id: 'receiving', role: 'assistant', markdown: text, lifecycle: 'streaming', activity: [{ id: 'read', action: 'Read', detail: 'example.ts', state: 'running', output: { text: 'First line', truncated: false } }, { id: 'check', action: 'Check', detail: 'Supplied check', state: 'queued' }] };
}
function chat(message = assistant()): Chat { return { ...createInitialState().chats[0], messages: [createInitialState().chats[0].messages[0], message], status: 'working' }; }
function geometry(node: HTMLElement, height: () => number, client = 100) { Object.defineProperty(node, 'scrollHeight', { configurable: true, get: height }); Object.defineProperty(node, 'clientHeight', { configurable: true, value: client }); }

describe('stable supplied turn and tool presentation', () => {
  it('retains same-ID turn/group/row/output/disclosure DOM, row choices and scroll across snapshot updates', async () => {
    let message = assistant(); const view = render(<StrictMode><Conversation chat={chat(message)} /></StrictMode>);
    const turn = screen.getByRole('article', { name: 'AVA supplied run response' }); const historical = screen.getByRole('article', { name: 'Your example message' });
    const group = screen.getByRole('button', { name: /^Tool details/ }); const row = screen.getByRole('button', { name: /Read example.ts Working/ });
    fireEvent.click(row); const output = await screen.findByLabelText('Output: Read'); geometry(output, () => 800); output.scrollTop = 60; fireEvent.scroll(output);
    row.focus();
    message = { ...message, markdown: 'Partial response with more text', activity: message.activity!.map((record) => record.id === 'read' ? { ...record, output: { text: 'First line\nSecond line', truncated: false } } : record) };
    view.rerender(<StrictMode><Conversation chat={chat(message)} preferences={{ ...defaultConversation, expandTools: true }} /></StrictMode>);
    expect(screen.getByRole('article', { name: 'AVA supplied run response' })).toBe(turn); expect(screen.getByRole('article', { name: 'Your example message' })).toBe(historical);
    expect(screen.getByRole('button', { name: /^Tool details/ })).toBe(group); expect(screen.getByRole('button', { name: /Read example.ts Working/ })).toBe(row);
    expect(screen.getByLabelText('Output: Read')).toBe(output); expect(output.scrollTop).toBe(60); expect(row).toHaveFocus(); expect(row).toHaveAttribute('aria-expanded', 'true');
    fireEvent.click(row); view.rerender(<StrictMode><Conversation chat={chat({ ...message, markdown: 'More' })} preferences={{ ...defaultConversation, expandTools: true }} /></StrictMode>);
    expect(row).toHaveAttribute('aria-expanded', 'false'); // Row choice beats ambient expand preference.
    fireEvent.click(screen.getByRole('button', { name: 'Collapse all tools' })); expect(row).toHaveAttribute('aria-expanded', 'false');
  });
  it('resets individual row choices on explicit same-false Collapse all intent and explicit group/all expansion', async () => {
    const view = render(<Conversation chat={chat()} />);
    const group = screen.getByRole('button', { name: /^Tool details/ }); const row = screen.getByRole('button', { name: /Read example.ts/ });
    fireEvent.click(row); await screen.findByLabelText('Output: Read'); expect(group).toHaveAttribute('aria-expanded', 'false');
    fireEvent.click(screen.getByRole('button', { name: 'Collapse all tools' })); expect(row).toHaveAttribute('aria-expanded', 'false'); expect(screen.queryByLabelText('Output: Read')).not.toBeInTheDocument();
    fireEvent.click(row); await screen.findByLabelText('Output: Read'); fireEvent.click(screen.getByRole('button', { name: 'Collapse all tools' })); expect(row).toHaveAttribute('aria-expanded', 'false');
    fireEvent.click(group); expect(row).toHaveAttribute('aria-expanded', 'true'); fireEvent.click(row); expect(row).toHaveAttribute('aria-expanded', 'false');
    fireEvent.click(screen.getByRole('button', { name: 'Collapse all tools' })); fireEvent.click(screen.getByRole('button', { name: 'Expand all tools' })); expect(row).toHaveAttribute('aria-expanded', 'true');
    view.rerender(<Conversation chat={chat({ ...assistant(), activity: [] })} />); view.rerender(<Conversation chat={chat()} />);
    expect(screen.getByRole('button', { name: /Read example.ts/ })).toHaveAttribute('aria-expanded', 'false');
  });
  it('bounds output paint separately from supplied truncation, retains failure/change facts with completed output hidden', () => {
    const record: ToolView = { id: 'long', action: 'Inspect', detail: 'Supplied', state: 'failed', output: { text: '<script>bad()</script>\n' + 'x\n'.repeat(500), truncated: true }, changes: [{ id: 'change', filename: 'new.ts', operation: 'created', additions: 0, diff: { state: 'unavailable' } }] };
    const view = render(<ToolActivityDetails record={record} showCompletedOutput={false} />);
    const output = screen.getByLabelText('Output: Inspect'); expect(output.textContent!.split('\n')).toHaveLength(200); expect(output.querySelector('script')).toBeNull();
    expect(screen.getByText('Output truncated by the supplied view.')).toBeVisible(); expect(screen.getByText(/Remaining supplied output text omitted/)).toBeVisible(); expect(screen.getByLabelText('0 additions')).toBeVisible();
    view.rerender(<ToolActivityDetails record={{ ...record, state: 'completed' }} showCompletedOutput={false} />);
    expect(screen.queryByLabelText('Output: Inspect')).not.toBeInTheDocument(); expect(screen.getByLabelText('File change: new.ts')).toBeVisible();
    view.rerender(<ToolActivityDetails record={{ ...record, output: { text: 'x'.repeat(25000), truncated: false } }} />);
    expect(screen.getByLabelText('Output: Inspect').textContent).toHaveLength(20000); expect(screen.queryByText('Output truncated by the supplied view.')).not.toBeInTheDocument();
  });
  it('keeps partial Markdown/fences/Unicode escaped and readable without a dangling surrogate, unsafe HTML or network image nodes', () => {
    const text = '**unclosed\n```ts\n<img src="https://remote.example/x">\n![remote](https://remote.example/x)\n\ud83d';
    const view = render(<Conversation chat={chat(assistant(text))} />); const turn = screen.getByRole('article', { name: 'AVA supplied run response' });
    expect(turn.textContent).toContain('```ts'); expect(turn.textContent).not.toContain('\ud83d'); expect(turn.querySelector('img, script, iframe')).toBeNull();
    view.rerender(<Conversation chat={chat(assistant(text + '\ude80'))} />); expect(turn.textContent).toContain('🚀');
    view.rerender(<Conversation chat={chat({ ...assistant('![remote](https://remote.example/x)\n\n<script>alert(1)</script>\n\n[bad](javascript:alert%281%29)'), lifecycle: 'committed' })} />);
    expect(turn.querySelector('img, script')).toBeNull(); expect(within(turn).getByText('[Image: remote]')).toBeVisible(); expect(within(turn).getByText('bad')).not.toHaveAttribute('href', expect.stringMatching(/^javascript:/));
  });
});

function LatestScope({ version, active = true, capture }: { version: number; active?: boolean; capture: (latest: (event: MouseEvent<HTMLButtonElement>) => void) => void }) {
  const following = useBottomFollow(version, active);
  capture(following.latest);
  return <><div ref={following.ref} role="region" aria-label="Scoped output" tabIndex={0} onScroll={following.onScroll} onPointerDown={following.onPointerDown} onBlur={following.onBlur}><span>Supplied text {version}</span></div>
    {following.away ? <button ref={following.latestRef} type="button" onClick={following.latest}>Latest scope</button> : null}<button type="button">Other control</button></>;
}

function latestContent(kind: 'output' | 'transcript', revision: number) {
  const text = `First supplied text${'\nMore supplied text'.repeat(revision)}`;
  return <>{kind === 'output' ? <ToolActivityDetails record={{ id: 'out', action: 'Read', detail: 'Supplied example', state: 'running', output: { text, truncated: false } }} /> : <Conversation chat={chat({ ...assistant(text), activity: undefined })} />}<textarea aria-label="Editor" defaultValue="Draft selection" /></>;
}

describe('CHAT18-F01 explicit Latest focus ownership', () => {
  it.each(['output', 'transcript'] as const)('hands keyboard %s Latest focus to its named pane before removing the button; arrivals do not refocus', async (kind) => {
    const user = userEvent.setup(); let height = 500;
    const view = render(<StrictMode>{latestContent(kind, 0)}</StrictMode>);
    const pane = kind === 'output' ? screen.getByLabelText('Output: Read') : screen.getByRole('region', { name: /^Conversation:/ });
    geometry(pane, () => height); pane.scrollTop = 80; fireEvent.scroll(pane);
    const latest = screen.getByRole('button', { name: kind === 'output' ? 'Latest output' : 'Latest' });
    pane.focus(); await user.tab(); expect(latest).toHaveFocus();
    const focus = vi.spyOn(pane, 'focus');
    await user.keyboard('{Enter}');
    expect(latest).not.toBeInTheDocument(); expect(pane).toHaveFocus(); expect(pane.scrollTop).toBe(500);
    expect(focus).toHaveBeenCalledExactlyOnceWith({ preventScroll: true }); focus.mockClear();
    height = 700; view.rerender(<StrictMode>{latestContent(kind, 1)}</StrictMode>);
    expect(pane.scrollTop).toBe(700); expect(pane).toHaveFocus(); expect(focus).not.toHaveBeenCalled();
    pane.scrollTop = 80; fireEvent.scroll(pane); height = 900; view.rerender(<StrictMode>{latestContent(kind, 2)}</StrictMode>);
    expect(pane.scrollTop).toBe(80); expect(pane).toHaveFocus(); expect(focus).not.toHaveBeenCalled();
    if (kind === 'output') {
      pane.scrollTop = 800; fireEvent.scroll(pane); height = 1100; view.rerender(<StrictMode>{latestContent(kind, 3)}</StrictMode>);
      expect(pane.scrollTop).toBe(800); // Manual away scrolling revoked the focus-follow exception.
    }
  });
  it('revokes the explicit output-follow intent on blur and manual pointer reading, not just scroll distance', async () => {
    const user = userEvent.setup(); let height = 500;
    const view = render(latestContent('output', 0)); const output = screen.getByLabelText('Output: Read'); geometry(output, () => height);
    output.scrollTop = 40; fireEvent.scroll(output); await user.click(screen.getByRole('button', { name: 'Latest output' })); expect(output).toHaveFocus();
    await user.tab(); expect(screen.getByRole('textbox', { name: 'Editor' })).toHaveFocus();
    await user.tab({ shift: true }); expect(output).toHaveFocus();
    height = 700; view.rerender(latestContent('output', 1)); expect(output.scrollTop).toBe(500);
    await user.click(screen.getByRole('button', { name: 'Latest output' })); expect(output).toHaveFocus(); expect(output.scrollTop).toBe(700);
    fireEvent.pointerDown(output); fireEvent.pointerUp(window); height = 900; view.rerender(latestContent('output', 2));
    expect(output.scrollTop).toBe(700); expect(output).toHaveFocus();
  });
  it('keeps an active output text selection authoritative even after an explicit Latest focus handoff', async () => {
    const user = userEvent.setup(); let height = 500;
    const view = render(latestContent('output', 0)); const output = screen.getByLabelText('Output: Read'); geometry(output, () => height);
    output.scrollTop = 40; fireEvent.scroll(output); await user.click(screen.getByRole('button', { name: 'Latest output' }));
    const text = output.querySelector('code span')!.firstChild!; const range = document.createRange(); range.setStart(text, 0); range.setEnd(text, 5);
    const selection = window.getSelection()!; selection.removeAllRanges(); selection.addRange(range);
    height = 700; view.rerender(latestContent('output', 1));
    expect(output.scrollTop).toBe(500); expect(output).toHaveFocus(); expect(selection.toString()).toBe('First'); selection.removeAllRanges();
    output.scrollTop = 600; fireEvent.scroll(output); height = 900; view.rerender(latestContent('output', 2));
    expect(output.scrollTop).toBe(600); // Selection reading also ends the prior explicit focus-follow intent.
  });
  it.each(['output', 'transcript'] as const)('does not steal editor focus or selection for a synthetic %s Latest click or subsequent growth', (kind) => {
    let height = 500; const view = render(latestContent(kind, 0));
    const pane = kind === 'output' ? screen.getByLabelText('Output: Read') : screen.getByRole('region', { name: /^Conversation:/ });
    geometry(pane, () => height); pane.scrollTop = 40; fireEvent.scroll(pane);
    const editor = screen.getByRole('textbox', { name: 'Editor' }) as HTMLTextAreaElement; editor.focus(); editor.setSelectionRange(2, 7);
    const focus = vi.spyOn(pane, 'focus'); fireEvent.click(screen.getByRole('button', { name: kind === 'output' ? 'Latest output' : 'Latest' }));
    expect(pane.scrollTop).toBe(500); expect(editor).toHaveFocus(); expect(editor.selectionStart).toBe(2); expect(editor.selectionEnd).toBe(7); expect(focus).not.toHaveBeenCalled();
    height = 700; view.rerender(latestContent(kind, 1)); expect(pane.scrollTop).toBe(700); expect(editor).toHaveFocus(); expect(focus).not.toHaveBeenCalled();
  });
  it.each(['hidden', 'inert', 'inactive', 'stale', 'unmounted', 'hidden-control', 'other-control'] as const)('rejects a %s Latest callback without scrolling or handing off focus', (invalid) => {
    let handler!: (event: MouseEvent<HTMLButtonElement>) => void;
    const capture = (latest: typeof handler) => { handler = latest; };
    const view = render(<LatestScope version={0} capture={capture} />);
    const pane = screen.getByRole('region', { name: 'Scoped output' }); geometry(pane, () => 500); pane.scrollTop = 80; fireEvent.scroll(pane);
    const control = screen.getByRole('button', { name: 'Latest scope' }); const stale = handler; control.focus(); const focus = vi.spyOn(pane, 'focus');
    let target = control;
    if (invalid === 'hidden') pane.hidden = true;
    else if (invalid === 'inert') pane.setAttribute('inert', '');
    else if (invalid === 'inactive') view.rerender(<LatestScope version={0} active={false} capture={capture} />);
    else if (invalid === 'stale') view.rerender(<LatestScope version={1} capture={capture} />);
    else if (invalid === 'unmounted') view.unmount();
    else if (invalid === 'hidden-control') control.hidden = true;
    else { target = screen.getByRole('button', { name: 'Other control' }); target.focus(); }
    act(() => stale({ currentTarget: target, defaultPrevented: false } as MouseEvent<HTMLButtonElement>));
    expect(pane.scrollTop).toBe(80); expect(focus).not.toHaveBeenCalled();
  });
});

describe('transcript/output bottom-follow without focus or reading disruption', () => {
  it('follows only from bottom, keeps reading-away offsets, and Latest explicitly resumes with stable editor selection', () => {
    let message = assistant(); let height = 500;
    const view = render(<><Conversation chat={chat(message)} /><textarea aria-label="Editor" defaultValue="Draft selection" /></>);
    const transcript = screen.getByRole('region', { name: /^Conversation:/ }); geometry(transcript, () => height); transcript.scrollTop = 400; fireEvent.scroll(transcript);
    const editor = screen.getByRole('textbox') as HTMLTextAreaElement; editor.focus(); editor.setSelectionRange(2, 7); const focus = vi.spyOn(editor, 'focus');
    function grow() { height += 200; message = { ...message, markdown: message.markdown + '\nMore' }; view.rerender(<><Conversation chat={chat(message)} /><textarea aria-label="Editor" defaultValue="Draft selection" /></>); }
    grow(); expect(transcript.scrollTop).toBe(700); expect(editor).toHaveFocus(); expect(editor.selectionStart).toBe(2); expect(editor.selectionEnd).toBe(7); expect(focus).not.toHaveBeenCalled();
    transcript.scrollTop = 80; fireEvent.scroll(transcript); grow(); expect(transcript.scrollTop).toBe(80);
    const latest = screen.getByRole('button', { name: 'Latest' }); fireEvent.click(latest); expect(transcript.scrollTop).toBe(900); grow(); expect(transcript.scrollTop).toBe(1100); expect(editor).toHaveFocus();
  });
  it('does not disturb active text selection, hidden/inert panes, or scroll because an old Latest callback fires', () => {
    let message = assistant(); let height = 500; const view = render(<Conversation chat={chat(message)} />);
    const transcript = screen.getByRole('region', { name: /^Conversation:/ }); geometry(transcript, () => height); transcript.scrollTop = 400; fireEvent.scroll(transcript);
    const text = screen.getByText('Partial response').firstChild!; const range = document.createRange(); range.setStart(text, 0); range.setEnd(text, 7); const selection = window.getSelection()!; selection.removeAllRanges(); selection.addRange(range);
    height = 700; message = { ...message, markdown: 'Partial response growing' }; view.rerender(<Conversation chat={chat(message)} />);
    expect(transcript.scrollTop).toBe(400); expect(selection.toString()).toBe('Partial'); const latest = screen.getByRole('button', { name: 'Latest' });
    view.rerender(<Conversation chat={chat({ ...message, markdown: message.markdown + '\n**Complete**', lifecycle: 'committed' })} />);
    expect(selection.toString()).toBe('Partial'); expect(screen.getByRole('article', { name: 'AVA supplied run response' })).toHaveAttribute('data-turn-state', 'committed');
    expect(screen.queryByText('Complete', { selector: 'strong' })).not.toBeInTheDocument();
    selection.removeAllRanges(); fireEvent(document, new Event('selectionchange')); expect(screen.getByText('Complete', { selector: 'strong' })).toBeVisible();
    transcript.setAttribute('inert', ''); height = 900; view.rerender(<Conversation chat={chat({ ...message, markdown: 'Hidden growth' })} active={false} />);
    fireEvent.click(latest); expect(transcript.scrollTop).toBe(400); transcript.removeAttribute('inert');
    view.unmount(); fireEvent.click(latest); expect(transcript.scrollTop).toBe(400);
  });
  it('independently follows tool output only from its own bottom and preserves selection/focus/read-away state', () => {
    let record: ToolView = { id: 'out', action: 'Read', detail: 'Example', state: 'running', output: { text: 'first', truncated: false } }; let height = 500;
    const view = render(<ToolActivityDetails record={record} />); const output = screen.getByLabelText('Output: Read'); geometry(output, () => height); output.scrollTop = 400; fireEvent.scroll(output);
    height = 700; record = { ...record, output: { text: 'first\nsecond', truncated: false } }; view.rerender(<ToolActivityDetails record={record} />); expect(screen.getByLabelText('Output: Read')).toBe(output); expect(output.scrollTop).toBe(700);
    output.scrollTop = 30; fireEvent.scroll(output); height = 900; view.rerender(<ToolActivityDetails record={{ ...record, output: { text: 'third', truncated: false } }} />); expect(output.scrollTop).toBe(30);
    fireEvent.click(screen.getByRole('button', { name: 'Latest output' })); expect(output.scrollTop).toBe(900);
    output.focus(); height = 1100; view.rerender(<ToolActivityDetails record={{ ...record, output: { text: 'fourth', truncated: false } }} />); expect(output).toHaveFocus(); expect(output.scrollTop).toBe(900);
  });
  it('handles resize growth without timers and disconnects its observer on StrictMode cleanup', () => {
    const observers: { callback: () => void; disconnect: ReturnType<typeof vi.fn> }[] = [];
    vi.stubGlobal('ResizeObserver', class { disconnect = vi.fn(); observe = vi.fn(); constructor(callback: () => void) { observers.push({ callback, disconnect: this.disconnect }); } });
    const view = render(<StrictMode><Conversation chat={chat()} /></StrictMode>); const transcript = screen.getByRole('region', { name: /^Conversation:/ }); let height = 500; geometry(transcript, () => height); transcript.scrollTop = 400; fireEvent.scroll(transcript);
    height = 800; act(() => observers.at(-1)!.callback()); expect(transcript.scrollTop).toBe(800);
    view.unmount(); expect(observers.every((observer) => observer.disconnect.mock.calls.length > 0)).toBe(true); act(() => observers.at(-1)!.callback()); expect(transcript.scrollTop).toBe(800); vi.unstubAllGlobals();
  });
});
