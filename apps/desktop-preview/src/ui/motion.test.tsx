import { readFileSync } from 'node:fs';
import { StrictMode, useState } from 'react';
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { Disclosure } from './Disclosure';
import { Presence, PRESENCE_EXIT_MS } from './Presence';
import { ReducedMotionContext, useReducedMotion } from './useReducedMotion';

const tokens = readFileSync('src/styles/tokens.css', 'utf8');

function motionPreference(initial: boolean) {
  const listeners = new Set<() => void>();
  const media = {
    matches: initial,
    addEventListener: vi.fn((_type: string, listener: () => void) => listeners.add(listener)),
    removeEventListener: vi.fn((_type: string, listener: () => void) => listeners.delete(listener)),
  };
  vi.stubGlobal('matchMedia', vi.fn(() => media));
  return {
    media,
    listeners,
    change(matches: boolean) {
      act(() => {
        media.matches = matches;
        listeners.forEach((listener) => listener());
      });
    },
  };
}

afterEach(() => vi.useRealTimers());

describe('UI-only presence', () => {
  it('retains a logically closed subtree inert and hidden only for the panel transition', () => {
    vi.useFakeTimers();
    const click = vi.fn();
    const key = vi.fn();
    const { rerender, container } = render(<Presence open><button onClick={click} onKeyDown={key}>Example</button></Presence>);
    const button = screen.getByRole('button', { name: 'Example' });
    fireEvent.click(button);
    expect(click).toHaveBeenCalledTimes(1);
    rerender(<Presence open={false}><button onClick={click} onKeyDown={key}>Example</button></Presence>);
    expect(container.firstElementChild).toHaveAttribute('data-presence', 'closing');
    expect(container.firstElementChild).toHaveAttribute('inert');
    expect(container.firstElementChild).toHaveAttribute('aria-hidden', 'true');
    expect(screen.queryByRole('button', { name: 'Example' })).not.toBeInTheDocument();
    expect(button).toBeInTheDocument();
    fireEvent.click(button);
    fireEvent.keyDown(button, { key: 'Enter' });
    expect(click).toHaveBeenCalledTimes(1);
    expect(key).not.toHaveBeenCalled();
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS - 1));
    expect(button).toBeInTheDocument();
    act(() => vi.advanceTimersByTime(1));
    expect(container).toBeEmptyDOMElement();
    expect(vi.getTimerCount()).toBe(0);
    expect(tokens).toContain(`--motion-panel-duration: ${PRESENCE_EXIT_MS}ms;`);
  });

  it('never schedules an exit for initial absence or a stable open subtree', () => {
    vi.useFakeTimers();
    const { rerender, container } = render(<Presence open={false}><button>Example</button></Presence>);
    expect(container).toBeEmptyDOMElement();
    expect(vi.getTimerCount()).toBe(0);
    rerender(<Presence open><button>Example</button></Presence>);
    expect(screen.getByRole('button', { name: 'Example' })).toBeEnabled();
    expect(container.firstElementChild).not.toHaveAttribute('inert');
    expect(container.firstElementChild).not.toHaveAttribute('aria-hidden');
    expect(vi.getTimerCount()).toBe(0);
  });

  it('cancels an old exit on rapid reopen, preserves controls, and bounds the next exit independently', () => {
    vi.useFakeTimers();
    const content = <input aria-label="Draft" defaultValue="Keep this" />;
    const { rerender } = render(<Presence open>{content}</Presence>);
    const draft = screen.getByRole('textbox');
    draft.focus();
    fireEvent.change(draft, { target: { value: 'Still editable' } });
    rerender(<Presence open={false}>{content}</Presence>);
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS - 10));
    rerender(<Presence open>{content}</Presence>);
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS));
    expect(screen.getByRole('textbox')).toBe(draft);
    expect(draft).toHaveValue('Still editable');
    expect(vi.getTimerCount()).toBe(0);
    rerender(<Presence open={false}>{content}</Presence>);
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS - 1));
    expect(draft).toBeInTheDocument();
    act(() => vi.advanceTimersByTime(1));
    expect(draft).not.toBeInTheDocument();
  });

  it('unmounts immediately for reduced motion, including a preference change during exit', () => {
    vi.useFakeTimers();
    const preference = motionPreference(false);
    const { rerender, container } = render(<Presence open><button>Example</button></Presence>);
    rerender(<Presence open={false}><button>Example</button></Presence>);
    expect(vi.getTimerCount()).toBe(1);
    preference.change(true);
    expect(container).toBeEmptyDOMElement();
    expect(vi.getTimerCount()).toBe(0);
    rerender(<Presence open><button>Example</button></Presence>);
    rerender(<Presence open={false}><button>Example</button></Presence>);
    expect(container).toBeEmptyDOMElement();
    expect(vi.getTimerCount()).toBe(0);
    preference.change(false);
    expect(container).toBeEmptyDOMElement();
  });

  it('cleans up its pending exit and media listener on unmount under StrictMode', () => {
    vi.useFakeTimers();
    const preference = motionPreference(false);
    const { rerender, unmount } = render(<StrictMode><Presence open><button>Example</button></Presence></StrictMode>);
    expect(preference.listeners.size).toBe(1);
    rerender(<StrictMode><Presence open={false}><button>Example</button></Presence></StrictMode>);
    expect(vi.getTimerCount()).toBe(1);
    unmount();
    expect(vi.getTimerCount()).toBe(0);
    expect(preference.listeners.size).toBe(0);
    expect(preference.media.removeEventListener).toHaveBeenCalledTimes(preference.media.addEventListener.mock.calls.length);
  });
});

describe('controlled disclosure and reduced-motion preference', () => {
  it('keeps ordinary controls usable while open and hides interactions immediately on close', () => {
    vi.useFakeTimers();
    const action = vi.fn();
    function Example() {
      const [open, setOpen] = useState(true);
      return <Disclosure id="example-disclosure" open={open} onOpenChange={setOpen} trigger="Project"><button onClick={action}>Chat</button></Disclosure>;
    }
    render(<Example />);
    const trigger = screen.getByRole('button', { name: 'Project' });
    const chat = screen.getByRole('button', { name: 'Chat' });
    chat.focus();
    fireEvent.click(chat);
    expect(chat).toHaveFocus();
    expect(action).toHaveBeenCalledTimes(1);
    fireEvent.click(trigger);
    expect(trigger).toHaveFocus();
    expect(trigger).toHaveAttribute('aria-expanded', 'false');
    expect(trigger).not.toHaveAttribute('aria-controls');
    expect(chat.closest('[inert]')).toHaveAttribute('aria-hidden', 'true');
    fireEvent.click(chat);
    expect(action).toHaveBeenCalledTimes(1);
    fireEvent.click(trigger);
    expect(trigger).toHaveAttribute('aria-expanded', 'true');
    expect(trigger).toHaveAttribute('aria-controls', 'example-disclosure');
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS));
    expect(screen.getByRole('button', { name: 'Chat' })).toBe(chat);
    expect(document.querySelectorAll('#example-disclosure')).toHaveLength(1);
  });

  it.each([false, true])('restores focus on controlled collapse with reduced motion %s without stealing outside focus', (reduced) => {
    vi.useFakeTimers();
    motionPreference(reduced);
    const onOpenChange = vi.fn();
    function Example({ open }: { open: boolean }) {
      return <><Disclosure id="project-content" open={open} onOpenChange={onOpenChange} trigger="Project"><button>Chat</button></Disclosure><button>Outside</button></>;
    }
    const { rerender } = render(<Example open />);
    const trigger = screen.getByRole('button', { name: 'Project' });
    screen.getByRole('button', { name: 'Chat' }).focus();
    rerender(<Example open={false} />);
    expect(trigger).toHaveFocus();
    expect(screen.queryByRole('button', { name: 'Chat' })).not.toBeInTheDocument();
    rerender(<Example open />);
    const outside = screen.getByRole('button', { name: 'Outside' });
    outside.focus();
    rerender(<Example open={false} />);
    expect(outside).toHaveFocus();
    expect(onOpenChange).not.toHaveBeenCalled();
    // Flush jsdom's zero-delay selectionchange events from native focus, not exits.
    act(() => vi.advanceTimersByTime(0));
    if (reduced) expect(vi.getTimerCount()).toBe(0);
  });

  it('reads and observes the media preference rather than viewport geometry', () => {
    const preference = motionPreference(true);
    function Example() {
      return <output>{useReducedMotion() ? 'Reduced' : 'Normal'}</output>;
    }
    const { unmount } = render(<Example />);
    expect(screen.getByText('Reduced')).toBeInTheDocument();
    expect(window.matchMedia).toHaveBeenCalledWith('(prefers-reduced-motion: reduce)');
    preference.change(false);
    expect(screen.getByText('Normal')).toBeInTheDocument();
    unmount();
    expect(preference.listeners.size).toBe(0);
  });
});


describe('scoped manual motion reduction', () => {
  it('removes pending Presence exits immediately and never revives closed content on returning to system motion', () => {
    vi.useFakeTimers(); motionPreference(false);
    function Example({ open, reduced }: { open: boolean; reduced: boolean }) {
      return <ReducedMotionContext.Provider value={reduced}><Presence open={open}><button>Scoped content</button></Presence></ReducedMotionContext.Provider>;
    }
    const view = render(<Example open reduced={false} />);
    view.rerender(<Example open={false} reduced={false} />); expect(vi.getTimerCount()).toBe(1);
    view.rerender(<Example open={false} reduced />); expect(view.container).toBeEmptyDOMElement(); expect(vi.getTimerCount()).toBe(0);
    view.rerender(<Example open={false} reduced={false} />); expect(view.container).toBeEmptyDOMElement();
    view.rerender(<Example open reduced />); view.rerender(<Example open={false} reduced />);
    expect(view.container).toBeEmptyDOMElement(); expect(vi.getTimerCount()).toBe(0);
  });
  it('ORs manual and system reduction without leaking a local choice into another scope', () => {
    const preference = motionPreference(false);
    function Value({ name }: { name: string }) { return <output aria-label={name}>{useReducedMotion() ? 'Reduced' : 'Normal'}</output>; }
    const view = render(<><ReducedMotionContext.Provider value><Value name="Manual" /></ReducedMotionContext.Provider><ReducedMotionContext.Provider value={false}><Value name="System" /></ReducedMotionContext.Provider></>);
    expect(screen.getByLabelText('Manual')).toHaveTextContent('Reduced'); expect(screen.getByLabelText('System')).toHaveTextContent('Normal');
    preference.change(true); expect(screen.getByLabelText('System')).toHaveTextContent('Reduced');
    preference.change(false); expect(screen.getByLabelText('Manual')).toHaveTextContent('Reduced'); expect(screen.getByLabelText('System')).toHaveTextContent('Normal');
    view.unmount(); expect(preference.listeners.size).toBe(0);
  });
});
