import { StrictMode, useRef, useState } from 'react';
import { fireEvent, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { installDialogTestSupport } from '../screens/dialogTestSupport';
import { useViewGuard } from '../screens/useViewGuard';
import { DialogFrame } from './DialogFrame';

beforeEach(installDialogTestSupport);
function Host({ back }: { back?: () => void }) {
  const [open, setOpen] = useState(false);
  const fallback = useRef<HTMLButtonElement>(null);
  return <><button ref={fallback}>Fallback</button><button onClick={() => setOpen(true)}>Open</button>{open ? <DialogFrame title="Example dialog" onDismiss={() => setOpen(false)} onBack={back} returnFocus={fallback}><input aria-label="Example input" /></DialogFrame> : null}</>;
}
describe('native dialog lifetime', () => {
  it('keeps accessible naming and description without a visible shared title/subtitle slab', () => {
    render(<DialogFrame title="Task name" description="Accessible scope" onDismiss={vi.fn()}><p>Task-specific body</p></DialogFrame>);
    const dialog = screen.getByRole('dialog', { name: 'Task name' });
    expect(dialog).toHaveAccessibleDescription('Accessible scope');
    expect(dialog.querySelector(':scope > header .srOnly')).toContainElement(screen.getByRole('heading', { name: 'Task name' }));
    expect(dialog.querySelector(':scope > header button')).toBe(screen.getByRole('button', { name: 'Close dialog' }));
    expect(screen.getByText('Task-specific body')).toBeVisible();
  });
  it('opens natively and closes immediately with visible opener restoration', async () => {
    const user = userEvent.setup(); render(<StrictMode><Host /></StrictMode>);
    const opener = screen.getByRole('button', { name: 'Open' }); await user.click(opener);
    expect(screen.getByRole('dialog')).toHaveAttribute('open');
    await user.click(screen.getByRole('button', { name: 'Close dialog' }));
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
    opener.hidden = true; await user.click(screen.getByLabelText('Close dialog')); expect(screen.getByText('Fallback')).toHaveFocus();
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
