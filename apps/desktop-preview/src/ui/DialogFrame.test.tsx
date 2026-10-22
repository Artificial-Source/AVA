import { readFileSync } from 'node:fs';
import { StrictMode, useRef, useState } from 'react';
import { fireEvent, render, screen } from '@testing-library/react';
import userEvent, { type UserEvent } from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { installDialogTestSupport } from '../screens/dialogTestSupport';
import { useViewGuard } from '../screens/useViewGuard';
import { DialogFrame } from './DialogFrame';

const css = readFileSync('src/ui/DialogFrame.module.css', 'utf8');
beforeEach(installDialogTestSupport);
function Host({ back }: { back?: () => void }) {
  const [open, setOpen] = useState(false);
  const fallback = useRef<HTMLButtonElement>(null);
  return <><button ref={fallback}>Fallback</button><button onClick={() => setOpen(true)}>Open</button>{open ? <DialogFrame title="Example dialog" onDismiss={() => setOpen(false)} onBack={back} returnFocus={fallback}><input aria-label="Example input" /></DialogFrame> : null}</>;
}
function Preferred() {
  const target = useRef<HTMLButtonElement>(null);
  return <DialogFrame title="Preferred" initialFocus={target} onDismiss={vi.fn()}><input aria-label="Earlier field" /><button ref={target}>Chosen action</button></DialogFrame>;
}
// A message box that opens a dialog from the keyboard, as F1 does in the app.
function MessageBox() {
  const [open, setOpen] = useState(false);
  return <>
    <textarea aria-label="Message" onKeyDown={(event) => { if (event.key === 'F1') setOpen(true); }} />
    <input aria-label="Other field" />
    <button>Elsewhere</button>
    {open ? <DialogFrame title="Help" onDismiss={() => setOpen(false)}><input aria-label="Search help" /></DialogFrame> : null}
  </>;
}
// Frames run only when a case says so.
const frames = new Map<number, FrameRequestCallback>();
let frameId = 1;
function holdFrames() {
  frames.clear();
  vi.stubGlobal('requestAnimationFrame', vi.fn((callback: FrameRequestCallback) => { const id = frameId++; frames.set(id, callback); return id; }));
}
function nextFrame() {
  const pending = [...frames.values()]; frames.clear();
  for (const callback of pending) callback(performance.now());
}
function focusEvents(element: HTMLElement) {
  const events: string[] = [];
  element.addEventListener('blur', () => events.push('blur')); element.addEventListener('focus', () => events.push('focus'));
  return events;
}
async function openAndEscape(user: UserEvent) {
  const message = screen.getByLabelText('Message'); message.focus();
  await user.keyboard('{F1}');
  expect(screen.getByLabelText('Search help')).toHaveFocus();
  await user.keyboard('{Escape}');
  expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(message).toHaveFocus();
  return message;
}
describe('native dialog lifetime', () => {
  it('shows a visible title and subtitle, keeps the description screen-reader-only, and closes from a quiet X', () => {
    render(<DialogFrame title="Task name" subtitle="Current chat" description="Accessible scope" onDismiss={vi.fn()}><p>Task-specific body</p></DialogFrame>);
    const dialog = screen.getByRole('dialog', { name: 'Task name' });
    const heading = screen.getByRole('heading', { level: 2, name: 'Task name' });
    expect(dialog.querySelector(':scope > header')).toContainElement(heading);
    expect(heading.closest('.srOnly')).toBeNull();
    expect(screen.getByText('Current chat').closest('.srOnly')).toBeNull();
    expect(screen.getByText('Accessible scope')).toHaveClass('srOnly');
    expect(dialog).toHaveAccessibleDescription('Current chat Accessible scope');
    const close = screen.getByRole('button', { name: 'Close' });
    expect(dialog.querySelector(':scope > header button')).toBe(close);
    expect(close).toHaveAttribute('data-variant', 'quiet');
    expect(close.querySelector('svg')).toHaveAttribute('aria-hidden', 'true');
    expect(close.textContent).toBe('');
    expect(screen.getByText('Task-specific body')).toBeVisible();
  });
  it('opens natively and closes immediately with visible opener restoration', async () => {
    const user = userEvent.setup(); render(<StrictMode><Host /></StrictMode>);
    const opener = screen.getByRole('button', { name: 'Open' }); await user.click(opener);
    expect(screen.getByRole('dialog')).toHaveAttribute('open');
    expect(screen.getByLabelText('Example input')).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Close' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(opener).toHaveFocus();
    await user.click(opener); expect(screen.getByRole('dialog')).toHaveAttribute('open');
  });
  it('ignores composing Escape, then dismisses without bubbling', async () => {
    const outer = vi.fn(); const user = userEvent.setup(); render(<div onKeyDown={outer}><Host /></div>);
    await user.click(screen.getByText('Open'));
    const input = screen.getByLabelText('Example input');
    fireEvent.keyDown(input, { key: 'Escape', isComposing: true }); expect(screen.getByRole('dialog')).toBeInTheDocument();
    fireEvent.compositionStart(input); fireEvent(screen.getByRole('dialog'), new Event('cancel', { cancelable: true })); expect(screen.getByRole('dialog')).toBeInTheDocument();
    fireEvent.compositionEnd(input); fireEvent.keyDown(input, { key: 'Escape', keyCode: 229 }); expect(screen.getByRole('dialog')).toBeInTheDocument();
    fireEvent.keyDown(input, { key: 'Escape' }); expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(outer).not.toHaveBeenCalled();
  });
  it('handles the inner Back before dismissal and native cancel', async () => {
    const back = vi.fn(); const user = userEvent.setup(); render(<Host back={back} />); await user.click(screen.getByText('Open'));
    fireEvent(screen.getByRole('dialog'), new Event('cancel', { cancelable: true })); expect(back).toHaveBeenCalledTimes(1); expect(screen.getByRole('dialog')).toBeInTheDocument();
  });
  it('keeps the wide base native and ignores a different nested dialog’s Escape and cancel', () => {
    const outer = vi.fn(); const inner = vi.fn();
    render(<DialogFrame wide title="Base settings" onDismiss={outer}><DialogFrame title="Inner model" onDismiss={inner}><input aria-label="Inner search" /></DialogFrame></DialogFrame>);
    const base = screen.getByRole('dialog', { name: 'Base settings' }); const modal = screen.getByRole('dialog', { name: 'Inner model' });
    expect(base).toHaveAttribute('data-wide', 'true'); expect(base).toHaveAttribute('open'); expect(modal).not.toHaveAttribute('data-wide');
    fireEvent.keyDown(screen.getByLabelText('Inner search'), { key: 'Escape' });
    expect(inner).toHaveBeenCalledTimes(1); expect(outer).not.toHaveBeenCalled();
    fireEvent(modal, new Event('cancel', { bubbles: true, cancelable: true })); expect(outer).not.toHaveBeenCalled();
  });
  it('never restores a removed or hidden opener', async () => {
    const user = userEvent.setup(); render(<Host />); const opener = screen.getByText('Open'); await user.click(opener);
    opener.hidden = true; await user.click(screen.getByLabelText('Close')); expect(screen.getByText('Fallback')).toHaveFocus();
  });
  it('invalidates captured callbacks on revision, synchronous close and unmount', () => {
    let gate: ReturnType<typeof useViewGuard>;
    function Probe({ scope }: { scope: string }) { gate = useViewGuard(scope); return null; }
    const result = render(<Probe scope="a" />); const first = gate!; expect(first.valid()).toBe(true);
    result.rerender(<Probe scope="b" />); expect(first.claim()).toBe(false);
    const second = gate!; expect(second.claim()).toBe(true); expect(second.claim()).toBe(false); second.invalidate(); expect(second.valid()).toBe(false);
    result.rerender(<Probe scope="c" />); const last = gate!; result.unmount(); expect(last.claim()).toBe(false);
  });
});

// Chromium can leave a returned text field focused but deaf to typing until it is focused again
// after the dialog leaves the page; jsdom only shows the next-frame settle that fixes it.
describe('focus after close', () => {
  it('focuses the returned text field again on the next frame, once the dialog is gone', async () => {
    holdFrames(); const user = userEvent.setup(); render(<MessageBox />);
    const message = await openAndEscape(user); const events = focusEvents(message);
    expect(frames.size).toBe(1);
    nextFrame();
    expect(events).toEqual(['blur', 'focus']); expect(message).toHaveFocus();
  });
  it('re-seats a text field the host focused after the close, and never moves focus off a control', async () => {
    holdFrames(); const user = userEvent.setup(); render(<MessageBox />);
    await openAndEscape(user);
    const other = screen.getByLabelText('Other field'); other.focus(); const events = focusEvents(other);
    nextFrame();
    expect(events).toEqual(['blur', 'focus']); expect(other).toHaveFocus();
    await openAndEscape(user);
    const elsewhere = screen.getByRole('button', { name: 'Elsewhere' }); elsewhere.focus(); const untouched = focusEvents(elsewhere);
    nextFrame();
    expect(untouched).toEqual([]); expect(elsewhere).toHaveFocus();
  });
  it('returns focus that fell to the page, and leaves a returned button as it is', async () => {
    holdFrames(); const user = userEvent.setup(); const { unmount } = render(<MessageBox />);
    const message = await openAndEscape(user);
    message.blur(); expect(document.activeElement).toBe(document.body);
    nextFrame();
    expect(message).toHaveFocus();
    unmount();
    render(<Host />); const opener = screen.getByRole('button', { name: 'Open' }); await user.click(opener);
    await user.click(screen.getByRole('button', { name: 'Close' })); expect(opener).toHaveFocus();
    const events = focusEvents(opener);
    nextFrame();
    expect(events).toEqual([]); expect(opener).toHaveFocus();
  });
  it('voids the settle when another dialog opens first, leaving the new dialog’s focus alone', async () => {
    holdFrames(); const user = userEvent.setup(); render(<MessageBox />);
    const message = await openAndEscape(user);
    expect(frames.size).toBe(1);
    await user.keyboard('{F1}');
    const search = screen.getByLabelText('Search help'); expect(search).toHaveFocus();
    const events = focusEvents(search); const returned = focusEvents(message);
    nextFrame();
    expect(events).toEqual([]); expect(returned).toEqual([]); expect(search).toHaveFocus();
  });
});

// Native showModal lands on Close (the first control); the frame must move focus to the task.
describe('initial focus order', () => {
  it('focuses the first field, not Close, even after an earlier body button', () => {
    render(<DialogFrame title="Find" onDismiss={vi.fn()}><button>Earlier action</button><input aria-label="Search" /></DialogFrame>);
    expect(screen.getByLabelText('Search')).toHaveFocus();
  });
  it('honors initialFocus over every default', () => {
    render(<Preferred />);
    expect(screen.getByRole('button', { name: 'Chosen action' })).toHaveFocus();
  });
  it('falls back to the first usable body control when there is no field', () => {
    render(<DialogFrame title="Confirm" onDismiss={vi.fn()}><p>Body copy</p><button disabled>Disabled action</button><button hidden>Hidden action</button><button>First action</button><button>Second action</button></DialogFrame>);
    expect(screen.getByRole('button', { name: 'First action' })).toHaveFocus();
  });
  it('focuses Close only when the body has no control', () => {
    render(<DialogFrame title="Loading" onDismiss={vi.fn()}><p>Loading…</p></DialogFrame>);
    expect(screen.getByRole('button', { name: 'Close' })).toHaveFocus();
  });
  it('prefers task content over the review toolbar and treats [data-autofocus] as a field', () => {
    render(<DialogFrame title="Review" toolbar={<select aria-label="Scenario"><option>Ready</option></select>} onDismiss={vi.fn()}><button>Earlier action</button><button data-autofocus>Marked action</button></DialogFrame>);
    expect(screen.getByRole('button', { name: 'Marked action' })).toHaveFocus();
  });
  it('falls back to the toolbar before Close when the task content has no control', () => {
    render(<DialogFrame title="Review" toolbar={<select aria-label="Scenario"><option>Ready</option></select>} onDismiss={vi.fn()}><p>Read-only body</p></DialogFrame>);
    expect(screen.getByLabelText('Scenario')).toHaveFocus();
  });
  it('lands on the checked option of a radio group, as Tab would', () => {
    render(<DialogFrame title="Branch" onDismiss={vi.fn()}><input type="radio" name="point" aria-label="First point" /><input type="radio" name="point" aria-label="Second point" defaultChecked /></DialogFrame>);
    expect(screen.getByLabelText('Second point')).toHaveFocus();
  });
  it('keeps the task order across body and footer: fields, marked actions, then the first control', () => {
    const first = render(<DialogFrame title="Connect" footer={<button data-autofocus>Verify</button>} onDismiss={vi.fn()}><input aria-label="API key" /></DialogFrame>);
    expect(screen.getByLabelText('API key')).toHaveFocus(); expect(screen.getByRole('button', { name: 'Verify' })).not.toHaveFocus(); first.unmount();
    const second = render(<DialogFrame title="Done" footer={<><button>Later</button><button data-autofocus>Choose</button></>} onDismiss={vi.fn()}><button>Body action</button></DialogFrame>);
    expect(screen.getByRole('button', { name: 'Choose' })).toHaveFocus(); second.unmount();
    render(<DialogFrame title="Confirm" footer={<><button>Cancel</button><button>Remove</button></>} onDismiss={vi.fn()}><p>Body copy</p></DialogFrame>);
    expect(screen.getByRole('button', { name: 'Cancel' })).toHaveFocus();
  });
});

describe('layout', () => {
  it('pins the footer under the scrolling body, never over it, inside one content region', () => {
    render(<DialogFrame title="Steps" contentProps={{ id: 'task-region' }} footer={<button>Save</button>} onDismiss={vi.fn()}><p>Long body</p></DialogFrame>);
    const dialog = screen.getByRole('dialog', { name: 'Steps' });
    const region = document.getElementById('task-region')!; const save = screen.getByRole('button', { name: 'Save' }); const body = screen.getByText('Long body').parentElement!;
    expect(region.parentElement).toBe(dialog); expect(region).toContainElement(body); expect(region.lastElementChild).toContainElement(save); expect(body).not.toContainElement(save);
    // Source contract only; real heights need browser validation.
    expect(css).toMatch(/\.content \{ display: flex; flex: 1 1 auto; flex-direction: column; min-height: 0; \}/);
    expect(css).toMatch(/\.body \{ flex: 1 1 auto; min-height: 0; overflow: auto;/);
    expect(css).toMatch(/\.footer \{ flex: none;/);
    expect(css).not.toMatch(/sticky/);
  });
  it('pins the top edge by default so changing content never re-centers it; center is opt-in', () => {
    const { rerender } = render(<DialogFrame title="Commands" onDismiss={vi.fn()}><input aria-label="Search commands" /></DialogFrame>);
    expect(screen.getByRole('dialog')).toHaveAttribute('data-anchor', 'top');
    rerender(<DialogFrame key="center" title="Commands" anchor="center" onDismiss={vi.fn()}><input aria-label="Search commands" /></DialogFrame>);
    expect(screen.getByRole('dialog')).toHaveAttribute('data-anchor', 'center');
    // Source contract only; the pinned position needs browser validation.
    expect(css).toMatch(/\.dialog\[data-anchor='top'\] \{ --dialog-top: clamp\(16px, 30vh - 160px, 12vh\); margin-block: var\(--dialog-top\) auto; max-height: min\(720px, calc\(100dvh - var\(--dialog-top\) - 16px\)\); \}/);
    expect(css).toMatch(/@media \(prefers-reduced-motion: reduce\) \{ \.dialog\[open\] \{ animation: none; \} \}/);
  });
});
