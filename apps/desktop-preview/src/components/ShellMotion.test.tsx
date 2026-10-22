import { StrictMode } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { PRESENCE_EXIT_MS } from '../ui/Presence';
import { AppShell } from './AppShell';
import { Inspector } from './Inspector';

afterEach(() => vi.useRealTimers());

async function openContext() {
  await act(async () => {
    fireEvent.click(screen.getByRole('button', { name: 'Context' }));
    await import('./DetailInspector');
  });
}

describe('shell motion logical boundaries', () => {
  it('closes immediately, restores rail focus, and blocks callbacks from exiting inspector paint', () => {
    vi.useFakeTimers();
    render(<AppShell />);
    const child = screen.getByRole('button', { name: /^Working Review back navigation/ });
    const panel = screen.getByRole('complementary', { name: 'Agents inspector' });
    const title = screen.getByRole('heading', { level: 1 });
    const close = screen.getByRole('button', { name: 'Close agents' });
    child.focus();
    fireEvent.keyDown(child, { key: 'Escape' });
    const railButton = screen.getByRole('button', { name: 'Agents' });
    expect(railButton).toHaveFocus();
    expect(railButton).toHaveAttribute('aria-expanded', 'false');
    expect(railButton).not.toHaveAttribute('aria-controls');
    expect(screen.queryByRole('complementary', { name: 'Agents inspector' })).not.toBeInTheDocument();
    expect(panel.parentElement).toHaveAttribute('inert');
    expect(panel.parentElement).toHaveAttribute('aria-hidden', 'true');
    expect(panel.querySelector('h1, #conversation-title')).toBeNull();
    expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(screen.getByRole('region', { name: 'Remember search filters' })).toContainElement(title);
    expect(document.querySelectorAll('h1')).toHaveLength(1);
    const draft = screen.getByRole('textbox');
    draft.focus();
    fireEvent.click(child);
    fireEvent.click(close);
    fireEvent.keyDown(panel, { key: 'Escape' });
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    expect(draft).toHaveFocus();
    fireEvent.change(draft, { target: { value: 'Typing during the visual exit' } });
    expect(draft).toHaveValue('Typing during the visual exit');
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS));
    expect(panel).not.toBeInTheDocument();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('never lets an old exit hide a reopened or switched module, including across resize events', () => {
    vi.useFakeTimers();
    render(<AppShell />);
    const visibility = screen.getByRole('button', { name: 'Hide inspector' });
    const title = screen.getByRole('heading', { level: 1 });
    const changes = screen.getByRole('button', { name: 'Changes' });
    const agents = screen.getByRole('button', { name: 'Agents' });
    fireEvent.click(visibility);
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS - 10));
    fireEvent.click(changes);
    fireEvent(window, new Event('resize'));
    fireEvent.click(visibility);
    act(() => vi.advanceTimersByTime(20));
    fireEvent.click(agents);
    fireEvent.click(changes);
    fireEvent(window, new Event('resize'));
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS * 2));
    expect(changes).toHaveAttribute('aria-expanded', 'true');
    expect(visibility).toHaveAttribute('title', 'Hide inspector · Changes');
    expect(screen.getAllByRole('complementary', { name: 'Changes inspector' })).toHaveLength(1);
    expect(document.querySelectorAll('#change-review')).toHaveLength(1);
    expect(document.querySelector('#agents-inspector')).toBeNull();
    expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(screen.getByRole('region', { name: 'Remember search filters' })).toContainElement(title);
    expect(document.querySelectorAll('h1')).toHaveLength(1);
    const ids = Array.from(document.querySelectorAll('[id]'), (element) => element.id);
    expect(new Set(ids).size).toBe(ids.length);
    expect(vi.getTimerCount()).toBe(0);
    // Resize is intentionally CSS-only: this tests stale-state safety, not geometry.
  });

  it('makes collapsed project children unreachable during exit and cancels a stale collapse on reopen', () => {
    vi.useFakeTimers();
    render(<AppShell />);
    const projects = within(screen.getByRole('navigation', { name: 'All project chats' }));
    const project = projects.getByRole('button', { name: 'Wayfinder' });
    const chat = projects.getByRole('button', { name: 'Keyboard navigation Working' });
    const pin = projects.getByRole('button', { name: 'Pin Keyboard navigation' });
    const id = project.getAttribute('aria-controls')!;
    chat.focus();
    fireEvent.click(project);
    expect(project).toHaveFocus();
    expect(project).toHaveAttribute('aria-expanded', 'false');
    expect(document.getElementById(id)).toHaveAttribute('inert');
    expect(document.getElementById(id)).toHaveAttribute('aria-hidden', 'true');
    expect(projects.queryByRole('button', { name: 'Keyboard navigation Working' })).not.toBeInTheDocument();
    fireEvent.click(chat);
    fireEvent.click(pin);
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    expect(screen.queryByRole('navigation', { name: 'Pinned chats' })).not.toBeInTheDocument();
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS - 5));
    fireEvent.click(project);
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS));
    expect(projects.getByRole('button', { name: 'Keyboard navigation Working' })).toBe(chat);
    expect(document.querySelectorAll(`#${id}`)).toHaveLength(1);
    expect(document.getElementById(id)).not.toHaveAttribute('inert');
    fireEvent.click(chat);
    expect(screen.getByRole('button', { name: 'Stop' })).toBeEnabled();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('removes panels and project contents immediately with reduced motion and schedules no exits', () => {
    vi.useFakeTimers();
    vi.stubGlobal('matchMedia', vi.fn((query: string) => ({ matches: query === '(prefers-reduced-motion: reduce)', addEventListener: vi.fn(), removeEventListener: vi.fn() })));
    render(<AppShell />);
    const close = screen.getByRole('button', { name: 'Close agents' });
    close.focus();
    fireEvent.click(close);
    expect(screen.getByRole('button', { name: 'Agents' })).toHaveFocus();
    expect(document.querySelector('#agents-inspector')).toBeNull();
    const projects = within(screen.getByRole('navigation', { name: 'All project chats' }));
    const project = projects.getByRole('button', { name: 'Wayfinder' });
    const id = project.getAttribute('aria-controls')!;
    projects.getByRole('button', { name: 'Keyboard navigation Working' }).focus();
    fireEvent.click(project);
    expect(project).toHaveFocus();
    expect(document.getElementById(id)).toBeNull();
    // Native focus queues zero-delay selectionchange in jsdom; no exit may remain.
    act(() => vi.advanceTimersByTime(0));
    expect(vi.getTimerCount()).toBe(0);
  });

  it('does not install document keyboard handlers and cleans every pending nested shell exit on unmount', async () => {
    vi.useFakeTimers();
    const add = vi.spyOn(document, 'addEventListener');
    const { unmount } = render(<StrictMode><AppShell /></StrictMode>);
    await openContext();
    fireEvent.click(screen.getByRole('button', { name: 'Raw messages' }));
    fireEvent.keyDown(screen.getByRole('region', { name: 'Raw message JSON' }), { key: 'Escape' });
    fireEvent.click(screen.getByRole('button', { name: 'Hide inspector' }));
    fireEvent.click(screen.getByRole('button', { name: 'Wayfinder' }));
    expect(vi.getTimerCount()).toBe(3);
    expect(add.mock.calls.filter(([type]) => type === 'keydown' || type === 'pointerdown')).toHaveLength(0);
    unmount();
    expect(vi.getTimerCount()).toBe(0);
    add.mockRestore();
  });

  it('makes raw-message exits inert immediately, restores its visible trigger, and blocks nested Escape from closing the pane', async () => {
    vi.useFakeTimers();
    render(<AppShell />);
    await openContext();
    const trigger = screen.getByRole('button', { name: 'Raw messages' });
    fireEvent.click(trigger);
    const context = screen.getByRole('region', { name: 'Raw message JSON' });
    const presence = context.parentElement!;
    context.focus();
    fireEvent.keyDown(context, { key: 'Escape' });
    expect(trigger).toHaveFocus();
    expect(trigger).toHaveAttribute('aria-expanded', 'false');
    expect(trigger).not.toHaveAttribute('aria-controls');
    expect(presence).toHaveAttribute('inert');
    expect(presence).toHaveAttribute('aria-hidden', 'true');
    expect(screen.queryByRole('region', { name: 'Raw message JSON' })).not.toBeInTheDocument();
    fireEvent.keyDown(context, { key: 'Escape' });
    fireEvent.pointerDown(context);
    fireEvent.click(context);
    expect(screen.getByRole('complementary', { name: 'Context inspector' })).toBeVisible();
    expect(trigger).toHaveFocus();
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS - 1));
    expect(presence).toBeInTheDocument();
    act(() => vi.advanceTimersByTime(1));
    expect(presence).not.toBeInTheDocument();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('closes open raw messages with their parent without ever focusing an inert trigger, then reopens safely', async () => {
    vi.useFakeTimers();
    render(<AppShell />);
    await openContext();
    const trigger = screen.getByRole('button', { name: 'Raw messages' });
    fireEvent.click(trigger);
    const context = screen.getByRole('region', { name: 'Raw message JSON' });
    const panel = screen.getByRole('complementary', { name: 'Context inspector' });
    const close = screen.getByRole('button', { name: 'Close context' });
    context.focus();
    const focus = vi.spyOn(trigger, 'focus');
    fireEvent.click(screen.getByRole('button', { name: 'Close context' }));
    expect(screen.getByRole('button', { name: 'Context' })).toHaveFocus();
    expect(context.closest('[inert]')).toBe(panel.parentElement);
    expect(panel.parentElement).toHaveAttribute('aria-hidden', 'true');
    fireEvent.keyDown(context, { key: 'Escape' });
    fireEvent.click(close);
    fireEvent.click(trigger);
    expect(focus).not.toHaveBeenCalled();
    expect(screen.queryByRole('region', { name: 'Context information' })).not.toBeInTheDocument();
    expect(document.querySelectorAll('#conversation-title')).toHaveLength(1);
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS - 1));
    fireEvent.click(screen.getByRole('button', { name: 'Show inspector' }));
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS));
    expect(screen.getByRole('region', { name: 'Raw message JSON' })).toBe(context);
    expect(context.closest('[inert], [aria-hidden="true"]')).toBeNull();
    expect(trigger).toHaveAttribute('aria-expanded', 'true');
    expect(vi.getTimerCount()).toBe(0);
    focus.mockRestore();
  });

  it('cancels stale raw exits across quick toggles, module switches, parent closing, and resize', async () => {
    vi.useFakeTimers();
    render(<AppShell />);
    await openContext();
    const trigger = screen.getByRole('button', { name: 'Raw messages' });
    fireEvent.click(trigger);
    const raw = screen.getByRole('region', { name: 'Raw message JSON' });
    fireEvent.click(trigger);
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS - 10));
    fireEvent.click(trigger);
    fireEvent.click(screen.getByRole('button', { name: 'Hide inspector' }));
    act(() => vi.advanceTimersByTime(20));
    fireEvent.click(screen.getByRole('button', { name: 'Show inspector' }));
    fireEvent(window, new Event('resize'));
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS * 2));
    expect(screen.getByRole('region', { name: 'Raw message JSON' })).toBe(raw);
    expect(trigger).toHaveAttribute('aria-expanded', 'true');
    fireEvent.click(trigger);
    fireEvent.click(screen.getByRole('button', { name: 'Changes' }));
    fireEvent.click(screen.getByRole('button', { name: 'Context' }));
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS * 2));
    expect(raw).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Raw messages' })).toHaveAttribute('aria-expanded', 'false');
    expect(screen.getByRole('complementary', { name: 'Context inspector' })).toBeVisible();
    const ids = Array.from(document.querySelectorAll('[id]'), (element) => element.id);
    expect(new Set(ids).size).toBe(ids.length);
    expect(vi.getTimerCount()).toBe(0);
    // Resize dispatch checks stale-state safety only; geometry is browser-owned.
  });

  it('reopens remembered Context after full cleanup without stale disclosures or duplicate identity', async () => {
    vi.useFakeTimers();
    render(<AppShell />);
    await openContext();
    const trigger = screen.getByRole('button', { name: 'Raw messages' });
    fireEvent.click(trigger);
    const rawId = trigger.getAttribute('aria-controls')!;
    const previousPage = screen.getByRole('region', { name: 'Context information' });
    const visibility = screen.getByRole('button', { name: 'Hide inspector' });
    visibility.focus();
    fireEvent.click(visibility);
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS));
    expect(previousPage).not.toBeInTheDocument();
    expect(document.getElementById(rawId)).toBeNull();
    expect(screen.getByRole('region', { name: 'Remember search filters' })).toContainElement(screen.getByRole('heading', { level: 1 }));
    fireEvent.click(visibility);
    expect(visibility).toHaveFocus();
    expect(screen.getByRole('complementary', { name: 'Context inspector' })).toBeVisible();
    expect(screen.getByRole('button', { name: 'Raw messages' })).toHaveAttribute('aria-expanded', 'false');
    expect(document.querySelectorAll('#conversation-title')).toHaveLength(1);
    expect(vi.getTimerCount()).toBe(0);
  });

  it('removes raw disclosure and parent panel exits immediately with reduced motion', async () => {
    vi.useFakeTimers();
    vi.stubGlobal('matchMedia', vi.fn((query: string) => ({ matches: query === '(prefers-reduced-motion: reduce)', addEventListener: vi.fn(), removeEventListener: vi.fn() })));
    render(<AppShell />);
    await openContext();
    const trigger = screen.getByRole('button', { name: 'Raw messages' });
    fireEvent.click(trigger);
    const rawId = trigger.getAttribute('aria-controls')!;
    const context = screen.getByRole('region', { name: 'Raw message JSON' });
    context.focus();
    fireEvent.keyDown(context, { key: 'Escape' });
    expect(trigger).toHaveFocus();
    expect(document.getElementById(rawId)).toBeNull();
    fireEvent.click(trigger);
    screen.getByRole('region', { name: 'Raw message JSON' }).focus();
    fireEvent.click(screen.getByRole('button', { name: 'Close context' }));
    expect(document.getElementById(rawId)).toBeNull();
    expect(document.querySelector('#context-inspector')).toBeNull();
    expect(screen.getByRole('button', { name: 'Context' })).toHaveFocus();
    act(() => vi.advanceTimersByTime(0));
    expect(vi.getTimerCount()).toBe(0);
  });

  it('keeps the same title focused outside pane exits and never queues later editing focus under StrictMode', () => {
    vi.useFakeTimers();
    render(<StrictMode><AppShell /></StrictMode>);
    const title = screen.getByRole('heading', { level: 1 });
    const canvas = screen.getByRole('region', { name: 'Remember search filters' });
    const visibility = screen.getByRole('button', { name: 'Hide inspector' });
    title.focus();
    const focus = vi.spyOn(title, 'focus');
    for (const name of ['Hide inspector', 'Show inspector', 'Changes', 'Context', 'Agents', 'Hide inspector']) {
      fireEvent.click(screen.getByRole('button', { name }));
      expect(screen.getByRole('heading', { level: 1 })).toBe(title);
      expect(canvas).toContainElement(title);
      expect(title).toHaveFocus();
      expect(title.closest('[inert], [aria-hidden="true"]')).toBeNull();
      expect(document.querySelectorAll('h1')).toHaveLength(1);
      expect(document.querySelectorAll('#conversation-title')).toHaveLength(1);
    }
    act(() => vi.advanceTimersByTime(PRESENCE_EXIT_MS));
    fireEvent.click(visibility);
    expect(title).toHaveFocus();
    const draft = screen.getByRole('textbox');
    draft.focus();
    fireEvent.change(draft, { target: { value: 'No deferred title-focus request' } });
    expect(draft).toHaveFocus();
    expect(focus).not.toHaveBeenCalled();
    focus.mockRestore();
    // Synthetic clicks preserve pre-existing focus; native click focus is tested separately.
  });

  it('guards inactive local inspector callbacks and replaces the module entry without stacking old modules', () => {
    const onClose = vi.fn();
    const { rerender } = render(<Inspector module="agents" active count={1} onClose={onClose}><button>Child</button></Inspector>);
    const panel = screen.getByRole('complementary');
    const entry = panel.lastElementChild;
    fireEvent.keyDown(panel, { key: 'Escape', isComposing: true });
    expect(onClose).not.toHaveBeenCalled();
    const onToggleWide = vi.fn();
    rerender(<Inspector module="agents" active={false} count={1} onToggleWide={onToggleWide} onClose={onClose}><button>Child</button></Inspector>);
    fireEvent.keyDown(panel, { key: 'Escape' });
    fireEvent.click(screen.getByRole('button', { name: 'Close agents' }));
    fireEvent.click(screen.getByRole('button', { name: 'Widen inspector' }));
    expect(onClose).not.toHaveBeenCalled();
    expect(onToggleWide).not.toHaveBeenCalled();
    rerender(<Inspector module="changes" active count={0} onClose={onClose}>Changes example</Inspector>);
    expect(panel.lastElementChild).not.toBe(entry);
    expect(panel.children).toHaveLength(1);
    expect(screen.queryByRole('button', { name: 'Child' })).not.toBeInTheDocument();
    fireEvent.keyDown(panel, { key: 'Escape' });
    expect(onClose).toHaveBeenCalledTimes(1);
  });
});
