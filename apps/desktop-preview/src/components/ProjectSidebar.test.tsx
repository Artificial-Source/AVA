import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import { createInitialState } from '../state/fixtures';
import type { PreviewState } from '../state/types';
import { ProjectSidebar } from './ProjectSidebar';

function nav(name: string) { return within(screen.getByRole('navigation', { name })); }
function sidebar(state: PreviewState = createInitialState(), props: Partial<Parameters<typeof ProjectSidebar>[0]> = {}) {
  const onChatAction = vi.fn();
  const onNewChat = vi.fn();
  const view = render(<ProjectSidebar state={state} dispatch={vi.fn()} onChatAction={onChatAction} onNewChat={onNewChat} {...props} />);
  return { ...view, onChatAction, onNewChat };
}

describe('project sidebar', () => {
  it('keeps titles first with the status word on the meta line, and marks the open chat once', () => {
    const { rerender } = sidebar();
    const row = nav('All project chats').getByRole('button', { name: 'Keyboard navigation Working' });
    expect(row.firstElementChild).toHaveTextContent(/^Keyboard navigation$/);
    expect(row.firstElementChild?.querySelector('[data-status="working"]')).toHaveAttribute('aria-hidden', 'true');
    expect(nav('All project chats').getByRole('button', { name: 'Remember search filters' })).toHaveAttribute('aria-current', 'page');
    expect(nav('Recent chats').getByRole('button', { name: 'Remember search filters Wayfinder' })).not.toHaveAttribute('aria-current');
    expect(screen.getAllByRole('button', { current: 'page' })).toHaveLength(1);

    rerender(<ProjectSidebar state={{ ...createInitialState(), collapsedProjectIds: ['wayfinder'] }} dispatch={vi.fn()} />);
    expect(nav('Recent chats').getByRole('button', { name: 'Remember search filters Wayfinder' })).toHaveAttribute('aria-current', 'page');
    expect(screen.getAllByRole('button', { current: 'page' })).toHaveLength(1);
  });

  it('rolls up unsettled work on collapsed project headings only', () => {
    sidebar({ ...createInitialState(), collapsedProjectIds: ['papertrail', 'relay'] });
    const projects = nav('All project chats');
    expect(projects.getByRole('button', { name: 'Wayfinder' })).toHaveAttribute('aria-expanded', 'true');
    const papertrail = projects.getByRole('button', { name: 'Papertrail, 1 needs attention' });
    expect(papertrail.querySelector('[aria-hidden="true"] [data-status="approval"]')).toHaveTextContent(/^1$/);
    expect(papertrail.querySelector('[data-status="stopped"]')).toBeNull();
    expect(projects.getByRole('button', { name: 'Relay, 1 failed' })).toHaveAttribute('aria-expanded', 'false');
  });

  it('opens one chat-actions menu from the row button, right-click and F2, with menu keyboard semantics and focus return', async () => {
    const user = userEvent.setup();
    const { onChatAction } = sidebar();
    const projects = nav('All project chats');
    const trigger = projects.getByRole('button', { name: 'Chat actions for Keyboard navigation' });

    await user.click(trigger);
    const menu = screen.getByRole('menu', { name: 'Chat actions for Keyboard navigation' });
    expect(trigger).toHaveAttribute('aria-expanded', 'true');
    expect(trigger).toHaveAttribute('aria-controls', menu.id);
    expect(within(menu).getByRole('menuitem', { name: 'Rename' })).toHaveFocus();
    expect(within(menu).getByRole('menuitem', { name: 'Rename' })).toHaveAttribute('aria-keyshortcuts', 'F2');
    await user.keyboard('{ArrowDown}');
    expect(within(menu).getByRole('menuitem', { name: 'Branch from a message…' })).toHaveFocus();
    await user.keyboard('{ArrowUp}{ArrowUp}');
    expect(within(menu).getByRole('menuitem', { name: 'Archive' })).toHaveFocus();
    await user.keyboard('{Escape}');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(trigger).toHaveFocus();
    expect(onChatAction).not.toHaveBeenCalled();

    await user.keyboard('{Enter}');
    await user.keyboard('a');
    expect(screen.getByRole('menuitem', { name: 'Archive' })).toHaveFocus();
    await user.keyboard('{Enter}');
    expect(onChatAction).toHaveBeenLastCalledWith('keyboard-navigation', 'archive');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    // The archived row is about to leave; focus waits on the stable library.
    expect(screen.getByRole('region', { name: 'Chat library' })).toHaveFocus();

    const row = projects.getByRole('button', { name: 'Export date range Attention' });
    fireEvent.contextMenu(row);
    await user.click(within(screen.getByRole('menu', { name: 'Chat actions for Export date range' })).getByRole('menuitem', { name: 'Rename' }));
    expect(onChatAction).toHaveBeenLastCalledWith('export-range', 'rename');
    expect(row).toHaveFocus();

    fireEvent.contextMenu(row);
    expect(screen.getByRole('menu')).toBeInTheDocument();
    fireEvent.pointerDown(document.body);
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();

    projects.getByRole('button', { name: 'Simplify route config Completed' }).focus();
    await user.keyboard('{F2}');
    expect(onChatAction).toHaveBeenLastCalledWith('route-config', 'rename');
    expect(onChatAction).toHaveBeenCalledTimes(3);
  });

  it('hides archived chats and starts chats in the chosen project', async () => {
    const user = userEvent.setup();
    const state = createInitialState();
    const { onNewChat } = sidebar({ ...state, chats: state.chats.map((chat) => chat.id === 'keyboard-navigation' ? { ...chat, archived: true } : chat) });
    expect(screen.queryByRole('button', { name: /^Keyboard navigation/ })).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'New chat in Papertrail' }));
    expect(onNewChat).toHaveBeenLastCalledWith('papertrail');
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(onNewChat).toHaveBeenLastCalledWith(undefined);
  });

  it('collapses to a rail that keeps New chat, Settings and the same toggle element', async () => {
    const user = userEvent.setup();
    const onToggleCollapsed = vi.fn();
    const { rerender } = sidebar(undefined, { onToggleCollapsed, onOpenSettings: vi.fn() });
    expect(screen.queryByRole('button', { name: 'Opaque materials' })).not.toBeInTheDocument();
    const toggle = screen.getByRole('button', { name: 'Collapse sidebar' });
    await user.click(toggle);
    expect(onToggleCollapsed).toHaveBeenCalledTimes(1);
    rerender(<ProjectSidebar state={createInitialState()} dispatch={vi.fn()} collapsed onToggleCollapsed={onToggleCollapsed} onOpenSettings={vi.fn()} />);
    expect(screen.getByRole('button', { name: 'Expand sidebar' })).toBe(toggle);
    expect(toggle).toHaveFocus();
    expect(screen.queryByRole('region', { name: 'Chat library' })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'New chat' })).toBeVisible();
    expect(screen.getByRole('button', { name: 'Settings' })).toBeVisible();
  });

  it('offers Find a chat and Commands under New chat with their keys, and keeps both in the collapsed rail', async () => {
    const user = userEvent.setup();
    const onFindChat = vi.fn();
    const onOpenCommands = vi.fn();
    const { rerender } = sidebar(undefined, { onFindChat, onOpenCommands });
    const find = screen.getByRole('button', { name: 'Find a chat' });
    const commands = screen.getByRole('button', { name: 'Commands' });
    // One actions group beside New chat, not a separate bar.
    expect(screen.getByRole('button', { name: 'New chat' }).parentElement).toContainElement(find);
    expect(find.parentElement).toContainElement(commands);
    expect(find).toHaveAttribute('title', 'Find a chat · Ctrl+P');
    expect(find).toHaveAttribute('aria-keyshortcuts', 'Control+P');
    expect(find).toHaveTextContent(/^Find a chatCtrl\+P$/);
    expect(commands).toHaveAttribute('title', 'Commands · Ctrl+K');
    expect(commands).toHaveAttribute('aria-keyshortcuts', 'Control+K');
    await user.click(find);
    expect(onFindChat).toHaveBeenCalledTimes(1);
    await user.click(commands);
    expect(onOpenCommands).toHaveBeenCalledTimes(1);

    rerender(<ProjectSidebar state={createInitialState()} dispatch={vi.fn()} collapsed onFindChat={onFindChat} onOpenCommands={onOpenCommands} shortcuts={{ enabled: false, overrides: {} }} />);
    expect(screen.getByRole('button', { name: 'Find a chat' })).toBe(find);
    expect(screen.getByRole('button', { name: 'Commands' })).toBe(commands);
    expect(find).toBeVisible();
    expect(commands).toBeVisible();
    // Shortcuts off: no key is advertised anywhere.
    expect(find).toHaveAttribute('title', 'Find a chat');
    expect(find).not.toHaveAttribute('aria-keyshortcuts');
    expect(find).not.toHaveTextContent('Ctrl');
    expect(commands).toHaveAttribute('title', 'Commands');
  });
});
