import { StrictMode, useState } from 'react';
import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { AppShell } from '../components/AppShell';
import { DialogFrame } from '../ui/DialogFrame';
import type { ConnectView, WorkspaceView } from './contracts';
import { PermissionDialog, QuestionDialog, RunDialog, WorkspaceDialog } from './ChatDialogs';
import { installDialogTestSupport } from './dialogTestSupport';
import { permissionCase, questionCase, runCase, workspaceChoices } from './fixtures';
import { ProviderConnectDialog, connectDelays } from './ProviderViews';
import { resetProviderState, setProviderState } from './providerState';
import { providers } from './screenExamples';
import { useDialogTransitionFocus } from './useDialogTransitionFocus';

let modals: HTMLDialogElement[];
let rectangles: Map<Element, DOMRect>;
function rect(left: number, top: number, width: number, height: number) { return new DOMRect(left, top, width, height); }
function bounds(node: Element): DOMRect {
  const supplied = rectangles.get(node);
  if (supplied) return supplied;
  if (node.matches('input, button, select, textarea')) {
    if (node.closest('header')) return rect(590, 70, 32, 32);
    const controls = [...(node.closest('dialog')?.querySelectorAll('input, button, select, textarea') ?? [])];
    return rect(80, 180 + controls.indexOf(node) * 40, 300, 30);
  }
  return rect(50, 50, 600, 650);
}

beforeEach(() => {
  installDialogTestSupport();
  modals = []; rectangles = new Map();
  vi.spyOn(HTMLDialogElement.prototype, 'showModal').mockImplementation(function (this: HTMLDialogElement) {
    this.setAttribute('open', ''); modals.push(this);
    this.querySelector<HTMLElement>('button:not(:disabled)')?.focus();
  });
  vi.spyOn(HTMLDialogElement.prototype, 'close').mockImplementation(function (this: HTMLDialogElement) {
    this.removeAttribute('open'); modals = modals.filter((modal) => modal !== this);
  });
  const matches = Element.prototype.matches;
  vi.spyOn(Element.prototype, 'matches').mockImplementation(function (this: Element, selector) {
    return selector === ':modal' ? modals.includes(this as HTMLDialogElement) : matches.call(this, selector);
  });
  // jsdom has no top layer/layout; Main's browser checks remain the native-behavior proof.
  vi.spyOn(document, 'hasFocus').mockReturnValue(true);
  vi.spyOn(Element.prototype, 'getBoundingClientRect').mockImplementation(function (this: Element) { return bounds(this); });
  Object.defineProperty(document, 'elementFromPoint', { configurable: true, value: (x: number, y: number) => {
    const modal = modals.at(-1);
    if (!modal) return document.body;
    return [...modal.querySelectorAll('input, button, select, textarea')].find((node) => {
      const box = bounds(node);
      return x >= box.left && x <= box.right && y >= box.top && y <= box.bottom;
    }) ?? modal;
  } });
  // Early-blur coverage. Late native focus-fixup tests restore this shim so jsdom
  // retains the now-disabled active control through the layout effect.
  const setAttribute = Element.prototype.setAttribute;
  vi.spyOn(Element.prototype, 'setAttribute').mockImplementation(function (this: Element, name, value) {
    const active = document.activeElement;
    if (name === 'disabled' && active instanceof HTMLElement && (active === this || this instanceof HTMLFieldSetElement && this.contains(active))) active.blur();
    setAttribute.call(this, name, value);
  });
});
afterEach(() => { Reflect.deleteProperty(document, 'elementFromPoint'); });
// Provider connections last for the whole visit; each case starts from the visit's start.
beforeEach(resetProviderState);
// These cases test focus, not settling: connect steps hold still instead of moving on
// after product delays the test would otherwise race.
const productDelays = { ...connectDelays };
beforeEach(() => { Object.assign(connectDelays, { browser: 2 ** 31 - 1, device: 2 ** 31 - 1, verify: 2 ** 31 - 1 }); });
afterEach(() => { Object.assign(connectDelays, productDelays); });

const connection: ConnectView = { id: 'connect', version: 1, provider: providers[0], step: 'method', method: null };
function ProviderHost() {
  const [view, setView] = useState(connection);
  return <ProviderConnectDialog view={view} onDismiss={vi.fn()} onIntent={(intent) => {
    if (intent.action === 'choose-method') setView({ ...view, version: view.version + 1, step: intent.method, method: intent.method });
  }} />;
}

describe('SCREEN11-F03 explicit same-modal content transitions', () => {
  it('focuses the current key field after its method button unmounts, keeping the same native dialog', async () => {
    const user = userEvent.setup(); render(<StrictMode><ProviderHost /></StrictMode>);
    const dialog = screen.getByRole('dialog');
    await user.click(within(dialog).getByRole('button', { name: /API key/ }));
    expect(screen.getByLabelText('API key')).toHaveFocus();
    expect(screen.getByRole('dialog')).toBe(dialog); expect(dialog).toHaveAttribute('open');
  });
  it('focuses the single opening action after removing workspace choices', async () => {
    function Host() {
      const [view, setView] = useState<WorkspaceView>({ id: 'workspace', version: 1, state: 'choice', selectedId: 'atlas', choices: workspaceChoices });
      return <WorkspaceDialog view={view} onDismiss={vi.fn()} onIntent={(intent) => setView({ ...view, version: 2, state: 'opening', selectedId: intent.workspaceId! })} />;
    }
    const user = userEvent.setup(); render(<StrictMode><Host /></StrictMode>);
    await user.click(screen.getByRole('button', { name: new RegExp(`^${workspaceChoices[0].label}`) }));
    expect(screen.getByRole('button', { name: 'Cancel opening' })).toHaveFocus();
  });
  it('keeps focus on the chosen permission decision while the others lock', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn();
    render(<StrictMode><PermissionDialog request={permissionCase('once', 1)} onIntent={onIntent} onDismiss={vi.fn()} /></StrictMode>);
    const allow = screen.getByRole('button', { name: 'Allow once' });
    await user.click(allow);
    expect(allow).toHaveAttribute('aria-pressed', 'true'); expect(allow).toHaveAttribute('aria-disabled', 'true'); expect(allow).toHaveFocus();
    expect(screen.getByRole('button', { name: 'Deny' })).toBeDisabled();
    expect(onIntent).toHaveBeenCalledTimes(1);
  });
  it('repairs the QuestionBody/QuestionCard commit while clearing and excluding secret drafts', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn();
    render(<StrictMode><QuestionDialog request={questionCase('secret', 1)} onIntent={onIntent} onDismiss={vi.fn()} /></StrictMode>);
    const input = screen.getByLabelText('Access token');
    await user.type(input, 'dummy-only-value'); expect(input).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Submit answer' }));
    expect(input).toHaveValue(''); expect(screen.getByRole('button', { name: 'Submit answer' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Answer later' })).toHaveFocus();
    expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'review-question', version: 1, kind: 'question-answer' });
  });
  it('also repairs native form submission when the secret field, not Submit, owned focus', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn();
    render(<QuestionDialog request={questionCase('secret', 1)} onIntent={onIntent} onDismiss={vi.fn()} />);
    await user.type(screen.getByLabelText('Access token'), 'dummy-only-value');
    await user.keyboard('{Enter}');
    expect(screen.getByLabelText('Access token')).toHaveValue('');
    expect(screen.getByRole('button', { name: 'Answer later' })).toHaveFocus();
    expect(onIntent).toHaveBeenCalledTimes(1);
  });
  it('focuses the current Done action after Retry unmounts', async () => {
    const user = userEvent.setup(); const onRetry = vi.fn();
    render(<StrictMode><RunDialog view={runCase('error')} onRetry={onRetry} onDismiss={vi.fn()} /></StrictMode>);
    await user.click(screen.getByRole('button', { name: 'Retry' }));
    expect(screen.queryByRole('button', { name: 'Retry' })).not.toBeInTheDocument();
    expect(screen.getByRole('status')).toHaveTextContent('Retry requested.');
    expect(screen.getByRole('button', { name: 'Done' })).toHaveFocus();
    expect(onRetry).toHaveBeenCalledTimes(1);
  });
});

describe('late native disabled focus before browser focus fixup', () => {
  beforeEach(() => { vi.mocked(Element.prototype.setAttribute).mockRestore(); });

  it('leaves a chosen permission decision focused when the browser has not yet fixed focus', async () => {
    const user = userEvent.setup(); const onIntent = vi.fn();
    render(<StrictMode><PermissionDialog request={permissionCase('once', 1)} onIntent={onIntent} onDismiss={vi.fn()} /></StrictMode>);
    const allow = screen.getByRole('button', { name: 'Allow once' });
    await user.click(allow);
    expect(allow).toHaveAttribute('aria-disabled', 'true'); expect(allow).toHaveFocus();
    expect(onIntent).toHaveBeenCalledTimes(1);
  });
  it.each(['click', 'Enter'])('repairs SecretQuestion %s submission while the captured control still owns focus', async (submission) => {
    const user = userEvent.setup(); const onIntent = vi.fn();
    render(<StrictMode><QuestionDialog request={questionCase('secret', 1)} onIntent={onIntent} onDismiss={vi.fn()} /></StrictMode>);
    const input = screen.getByLabelText('Access token');
    const submit = screen.getByRole('button', { name: 'Submit answer' });
    await user.type(input, 'dummy-only-value'); expect(input).toHaveFocus();
    if (submission === 'Enter') await user.keyboard('{Enter}');
    else await user.click(submit);
    expect(input).toHaveValue(''); expect(input).toBeDisabled(); expect(submit).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Answer later' })).toHaveFocus();
    expect(onIntent).toHaveBeenCalledExactlyOnceWith({ id: 'review-question', version: 1, kind: 'question-answer' });
  });
});

function FocusProbe({ retain = false, onTransition, onDismiss = () => {} }: { retain?: boolean; onTransition?: (dismiss: () => void) => void; onDismiss?: () => void }) {
  const focus = useDialogTransitionFocus(onDismiss);
  const [changed, setChanged] = useState(false);
  return <DialogFrame title="Focus probe" onDismiss={focus.dismiss}><div {...focus.content}>
    {!changed || retain ? <button onClick={() => { focus.capture(); setChanged(true); onTransition?.(focus.dismiss); }}>Transition</button> : null}
    <button>Current action</button>
  </div></DialogFrame>;
}

describe('transition focus stays local, foreground and commit-bound', () => {
  it('does not move a still-connected current control or a deliberate focus destination', async () => {
    const user = userEvent.setup(); const result = render(<FocusProbe retain />);
    const source = screen.getByRole('button', { name: 'Transition' }); const current = screen.getByRole('button', { name: 'Current action' });
    const focus = vi.spyOn(current, 'focus');
    await user.click(source); expect(source).toHaveFocus(); expect(focus).not.toHaveBeenCalled();
    // Remount just the probe to start a fresh explicit transition.
    result.rerender(<FocusProbe key="next" onTransition={() => screen.getByRole('button', { name: 'Current action' }).focus()} />);
    const next = screen.getByRole('button', { name: 'Current action' }); const nextFocus = vi.spyOn(next, 'focus');
    await user.click(screen.getByRole('button', { name: 'Transition' }));
    expect(next).toHaveFocus(); expect(nextFocus).toHaveBeenCalledTimes(1);
  });
  it('preserves a different deliberate focus destination even when it is now disabled', async () => {
    vi.mocked(Element.prototype.setAttribute).mockRestore();
    const user = userEvent.setup(); render(<FocusProbe onTransition={() => {
      const destination = screen.getByRole('button', { name: 'Current action' });
      destination.focus(); destination.setAttribute('disabled', '');
    }} />);
    const destination = screen.getByRole('button', { name: 'Current action' });
    const closeFocus = vi.spyOn(screen.getByRole('button', { name: 'Close' }), 'focus');
    await user.click(screen.getByRole('button', { name: 'Transition' }));
    expect(destination).toBeDisabled(); expect(destination).toHaveFocus(); expect(closeFocus).not.toHaveBeenCalled();
  });
  it('does not repair body focus if the captured control remains enabled and connected', async () => {
    const user = userEvent.setup(); render(<FocusProbe retain onTransition={() => (document.activeElement as HTMLElement).blur()} />);
    await user.click(screen.getByRole('button', { name: 'Transition' }));
    expect(document.activeElement).toBe(document.body);
  });
  it('does not follow browser-chrome focus, or retain work for a later unrelated commit', async () => {
    const user = userEvent.setup(); const result = render(<FocusProbe onTransition={() => vi.mocked(document.hasFocus).mockReturnValue(false)} />);
    await user.click(screen.getByRole('button', { name: 'Transition' }));
    expect(document.activeElement).toBe(document.body);
    vi.mocked(document.hasFocus).mockReturnValue(true);
    result.rerender(<FocusProbe />); fireEvent(window, new Event('resize'));
    expect(document.activeElement).toBe(document.body);
  });
  it('ignores supplied view/case replacements without an accepted UI transition', () => {
    const result = render(<ProviderConnectDialog view={connection} onIntent={vi.fn()} onDismiss={vi.fn()} />);
    screen.getByRole('button', { name: /API key/ }).focus();
    result.rerender(<ProviderConnectDialog view={{ ...connection, version: 2, step: 'key', method: 'key' }} onIntent={vi.fn()} onDismiss={vi.fn()} />);
    expect(document.activeElement).toBe(document.body);
  });
  it('does not capture an underlying modal while another dialog owns the current control', () => {
    render(<><FocusProbe /><DialogFrame title="Other modal" onDismiss={vi.fn()}><input aria-label="Other field" /></DialogFrame></>);
    const input = screen.getByLabelText('Other field'); input.focus(); const focus = vi.spyOn(input, 'focus');
    fireEvent.click(screen.getByRole('button', { name: 'Transition' }));
    expect(input).toHaveFocus(); expect(focus).not.toHaveBeenCalled();
  });
  it('does not repair an underlying native modal even if the new top modal itself loses focus', async () => {
    let other: HTMLDialogElement;
    const user = userEvent.setup(); render(<><FocusProbe onTransition={() => { other.showModal(); (document.activeElement as HTMLElement).blur(); }} /><dialog data-testid="new-modal"><button>New modal action</button></dialog></>);
    other = screen.getByTestId('new-modal') as HTMLDialogElement;
    await user.click(screen.getByRole('button', { name: 'Transition' }));
    expect(modals).toHaveLength(2); expect(document.activeElement).toBe(document.body);
  });
  it('falls back to the visible header when a current body action is clipped by its scroll container', async () => {
    const user = userEvent.setup(); render(<FocusProbe />);
    const dialog = screen.getByRole('dialog'); const body = dialog.lastElementChild as HTMLElement;
    body.style.overflowY = 'auto'; rectangles.set(body, rect(50, 200, 600, 400));
    const action = screen.getByRole('button', { name: 'Current action' }); rectangles.set(action, rect(80, 150, 300, 30));
    const focus = vi.spyOn(action, 'focus');
    await user.click(screen.getByRole('button', { name: 'Transition' }));
    expect(screen.getByRole('button', { name: 'Close' })).toHaveFocus(); expect(focus).not.toHaveBeenCalled();
  });
  it('does not focus hidden or viewport-clipped current controls', async () => {
    const user = userEvent.setup(); render(<FocusProbe />);
    const action = screen.getByRole('button', { name: 'Current action' }); action.style.visibility = 'hidden';
    rectangles.set(action, rect(80, 900, 300, 30));
    await user.click(screen.getByRole('button', { name: 'Transition' }));
    expect(screen.getByRole('button', { name: 'Close' })).toHaveFocus();
  });
  it('ignores IME composition without leaving a delayed repair', async () => {
    const user = userEvent.setup(); const result = render(<FocusProbe />);
    const source = screen.getByRole('button', { name: 'Transition' });
    fireEvent.compositionStart(source); await user.click(source);
    expect(document.activeElement).toBe(document.body);
    fireEvent.compositionEnd(screen.getByRole('button', { name: 'Current action' })); result.rerender(<FocusProbe />);
    expect(document.activeElement).toBe(document.body);
  });
  it('cancels at logical dismissal even if a host has not yet unmounted its dialog', async () => {
    const user = userEvent.setup(); const onDismiss = vi.fn(); const result = render(<FocusProbe onTransition={(dismiss) => dismiss()} onDismiss={onDismiss} />);
    const focus = vi.spyOn(screen.getByRole('button', { name: 'Current action' }), 'focus');
    await user.click(screen.getByRole('button', { name: 'Transition' }));
    expect(onDismiss).toHaveBeenCalledTimes(1); expect(document.activeElement).toBe(document.body);
    result.rerender(<FocusProbe />); expect(focus).not.toHaveBeenCalled();
  });
  it('does not focus after native close removes the active modal status', async () => {
    const user = userEvent.setup(); render(<FocusProbe onTransition={() => (screen.getByRole('dialog') as HTMLDialogElement).close()} />);
    const focus = vi.spyOn(screen.getByRole('button', { name: 'Current action' }), 'focus');
    await user.click(screen.getByRole('button', { name: 'Transition' }));
    expect(document.activeElement).toBe(document.body); expect(focus).not.toHaveBeenCalled();
  });
  it('unmounts without ghost focus and leaves native return focus intact in StrictMode', async () => {
    function Host() {
      const [open, setOpen] = useState(false);
      return <><button onClick={() => setOpen(true)}>Open probe</button>{open ? <FocusProbe onTransition={() => setOpen(false)} /> : null}</>;
    }
    const user = userEvent.setup(); render(<StrictMode><Host /></StrictMode>);
    const opener = screen.getByRole('button', { name: 'Open probe' }); await user.click(opener);
    const ghost = vi.spyOn(screen.getByRole('button', { name: 'Current action' }), 'focus');
    await user.click(screen.getByRole('button', { name: 'Transition' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(opener).toHaveFocus(); expect(ghost).not.toHaveBeenCalled();
    await user.click(opener); expect(screen.getByRole('dialog')).toHaveAttribute('open');
  });
  it('repairs provider steps above a Settings page and preserves both levels of Escape and return focus', async () => {
    // Aster Lab starts connected; its connect steps need it available.
    setProviderState('aster-lab', { state: 'available', source: 'none' });
    const user = userEvent.setup(); render(<StrictMode><AppShell /></StrictMode>);
    const opener = screen.getByRole('button', { name: 'Settings' }); await user.click(opener);
    await screen.findByRole('navigation', { name: 'Settings sections' });
    const settings = screen.getByRole('region', { name: 'Settings page' });
    await user.click(within(settings).getByRole('button', { name: 'Models & providers' }));
    const connect = within(settings).getByRole('button', { name: 'Connect Aster Lab' }); await user.click(connect);
    const provider = screen.getByRole('dialog', { name: 'Connect Aster Lab' });
    await user.click(within(provider).getByRole('button', { name: /API key/ }));
    expect(screen.getByLabelText('API key')).toHaveFocus();
    await user.keyboard('{Escape}');
    expect(within(provider).getByRole('button', { name: /API key/ })).toHaveFocus();
    await user.click(within(provider).getByRole('button', { name: /Sign in with a code/ }));
    expect(within(provider).getByRole('button', { name: 'Copy code' })).toHaveFocus();
    expect(modals).toEqual([provider]); expect(settings.tagName).not.toBe('DIALOG');
    await user.keyboard('{Escape}'); await user.keyboard('{Escape}');
    expect(provider).not.toBeInTheDocument(); expect(connect).toHaveFocus();
    // Settings Escape clears its search, else returns to the chat; no section hop.
    await user.keyboard('{Escape}');
    expect(screen.queryByRole('region', { name: 'Settings page' })).not.toBeInTheDocument();
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument(); expect(opener).toHaveFocus();
  });
});
