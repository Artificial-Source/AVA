import { StrictMode } from 'react';
import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { chooseMainModel, installDialogTestSupport } from '../screens/dialogTestSupport';

import { createInitialState } from '../state/fixtures';
import { AppShell } from './AppShell';
import { Conversation } from './Conversation';

beforeEach(installDialogTestSupport);

function start() {
  const user = userEvent.setup();
  render(<StrictMode><AppShell /></StrictMode>);
  return user;
}

function projects() {
  return within(screen.getByRole('navigation', { name: 'All project chats' }));
}

function pinned() {
  return within(screen.getByRole('navigation', { name: 'Pinned chats' }));
}

function recent() {
  return within(screen.getByRole('navigation', { name: 'Recent chats' }));
}

describe('monochrome sidebar-only UI preview', () => {
  it('keeps the mock boundary accessible without horizontal navigation, empty pin strips, helpers, or developer footers', () => {
    start();
    const app = screen.getByRole('main', { name: 'AVA UI preview' });
    expect(app).toHaveAccessibleDescription(/Browser-only UI preview.*No backend or model is connected/);
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    expect(screen.queryByRole('navigation', { name: 'Chat working set' })).not.toBeInTheDocument();
    expect(screen.queryByRole('tablist')).not.toBeInTheDocument();
    expect(screen.queryByRole('tab')).not.toBeInTheDocument();
    expect(screen.queryByRole('navigation', { name: 'Pinned chats' })).not.toBeInTheDocument();
    expect(screen.queryByText(/No pinned chats/)).not.toBeInTheDocument();
    for (const text of [
      'PREVIEW', 'Mock chat', 'Example message', 'Example response', 'Local preview',
      'Send adds a local message. No model is called.', 'Ctrl / Cmd + Enter',
      'All projects', 'Local fixtures', 'UI preview · mock data · no backend',
    ]) expect(screen.queryByText(text)).not.toBeInTheDocument();
    expect(screen.queryByLabelText('Preview and material status')).not.toBeInTheDocument();
    expect(app.querySelector(':scope > footer')).toBeNull();
    expect(screen.getByRole('form', { name: 'Message composer' })).toContainElement(screen.getByRole('textbox'));
    expect(screen.getByRole('button', { name: 'Changes' })).toHaveAttribute('aria-expanded', 'false');
    expect(screen.queryByRole('complementary', { name: 'Mock changes' })).not.toBeInTheDocument();
  });

  it('starts canvas, attached module, and rail together without a workspace header or changing the left sidebar', async () => {
    const user = start();
    const app = screen.getByRole('main', { name: 'AVA UI preview' });
    const title = screen.getByRole('heading', { level: 1 });
    const canvas = screen.getByRole('region', { name: 'Remember search filters' });
    const workspace = canvas.parentElement!;
    const rail = screen.getByRole('navigation', { name: 'Inspectors' });
    expect(workspace.parentElement).toBe(app);
    expect(screen.getByRole('complementary', { name: 'Projects and chats' }).parentElement).toBe(app);
    const panel = screen.getByRole('complementary', { name: 'Agents inspector' });
    const inspectorTrack = panel.parentElement!;
    expect(Array.from(workspace.children)).toEqual([canvas, inspectorTrack, rail]);
    expect(workspace.querySelector(':scope > header')).toBeNull();
    expect(canvas.querySelector('header')).toBeNull();
    expect(canvas.firstElementChild).toContainElement(title);
    expect(canvas.firstElementChild).toHaveTextContent(/^Remember search filters$/);
    expect(canvas.firstElementChild?.querySelector('p')).toBeNull();
    expect(title.closest('[inert], [aria-hidden="true"], .srOnly')).toBeNull();
    expect(screen.queryByRole('group', { name: 'Parent chat details' })).not.toBeInTheDocument();
    expect(within(panel).queryByText('Wayfinder')).not.toBeInTheDocument();
    expect(panel.children).toHaveLength(1);
    expect(panel.firstElementChild?.firstElementChild).toContainElement(screen.getByRole('heading', { name: 'Agents 3' }));
    expect(within(panel).queryByRole('button', { name: 'Context' })).not.toBeInTheDocument();
    expect(within(panel).queryByRole('button', { name: '2 changes' })).not.toBeInTheDocument();
    expect(rail.firstElementChild).toBe(screen.getByRole('button', { name: 'Hide inspector' }));
    await user.click(screen.getByRole('button', { name: 'Changes' }));
    expect(inspectorTrack).toContainElement(screen.getByRole('complementary', { name: 'Mock changes' }));
    expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(canvas).toContainElement(title);
    expect(within(panel).queryByRole('heading', { name: 'Remember search filters' })).not.toBeInTheDocument();
    expect(panel.querySelector('header')).not.toHaveTextContent('Remember search filters');
    expect(Array.from(workspace.children)).toEqual([canvas, inspectorTrack, rail]);
    await user.click(screen.getByRole('button', { name: 'Changes' }));
    // Retained exit DOM never contains another current title or sidebar identity.
    expect(Array.from(workspace.children).filter((element) => !element.hasAttribute('inert'))).toEqual([canvas, rail]);
    expect(screen.getAllByRole('heading', { level: 1 })).toEqual([title]);
    expect(canvas).toContainElement(title);
    expect(inspectorTrack.querySelector('h1, #conversation-title')).toBeNull();
    expect(document.querySelectorAll('#conversation-title')).toHaveLength(1);
    await user.click(screen.getByRole('button', { name: 'Show inspector' }));
    expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(canvas).toContainElement(title);
    expect(screen.queryByRole('group', { name: 'Parent chat details' })).not.toBeInTheDocument();
  });

  it('keeps the full project library first and optional contextual shortcuts in the same sidebar scroll flow', async () => {
    const user = start();
    for (const name of ['Wayfinder', 'Papertrail', 'Relay']) {
      expect(projects().getByRole('button', { name })).toHaveAttribute('aria-expanded', 'true');
    }
    for (const name of ['Remember search filters', 'Working Keyboard navigation', 'Completed Simplify route config', 'Attention Export date range', 'Stopped Document the import flow', 'Failed Review retry behavior']) {
      expect(projects().getByRole('button', { name })).toBeVisible();
    }
    expect(recent().getAllByRole('listitem')).toHaveLength(1);
    await user.click(projects().getByRole('button', { name: 'Pin Export date range' }));
    const library = screen.getByRole('region', { name: 'Chat library' });
    const navigation = within(library).getAllByRole('navigation');
    expect(navigation.map((nav) => nav.getAttribute('aria-label'))).toEqual(['All project chats', 'Pinned chats', 'Recent chats']);
    expect(navigation.every((nav) => nav.parentElement === library)).toBe(true);
    expect(screen.getByRole('complementary', { name: 'Projects and chats' })).toContainElement(library);
    expect(pinned().getByRole('button', { name: 'Attention Export date range Papertrail' })).toBeVisible();
    expect(recent().getByRole('button', { name: 'Remember search filters Wayfinder' })).toBeVisible();
    expect(screen.getByRole('main').querySelector('button button')).toBeNull();
  });

  it('collapses and expands project chats with the keyboard', async () => {
    const user = start();
    const project = projects().getByRole('button', { name: 'Wayfinder' });
    project.focus();
    await user.keyboard('{Enter}');
    expect(project).toHaveAttribute('aria-expanded', 'false');
    expect(projects().queryByRole('button', { name: 'Working Keyboard navigation' })).not.toBeInTheDocument();
    await user.keyboard(' ');
    expect(project).toHaveAttribute('aria-expanded', 'true');
    expect(projects().getByRole('button', { name: 'Working Keyboard navigation' })).toBeVisible();
  });

  it('supports sidebar navigation with no shortcuts and never changes a Working fixture on navigation', async () => {
    const user = start();
    await user.click(recent().getByRole('button', { name: 'Remove Remember search filters from recent chats' }));
    expect(screen.queryByRole('navigation', { name: 'Recent chats' })).not.toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    await user.click(projects().getByRole('button', { name: 'Working Keyboard navigation' }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Keyboard navigation');
    expect(screen.getByRole('button', { name: 'Stop mock work' })).toBeVisible();
    await user.click(projects().getByRole('button', { name: 'Remember search filters' }));
    expect(screen.queryByRole('button', { name: 'Stop mock work' })).not.toBeInTheDocument();
    expect(projects().getByRole('button', { name: 'Working Keyboard navigation' })).toBeVisible();
    await user.click(recent().getByRole('button', { name: 'Working Keyboard navigation Wayfinder' }));
    expect(screen.getByRole('button', { name: 'Stop mock work' })).toBeVisible();
    expect(screen.queryByRole('navigation', { name: 'Pinned chats' })).not.toBeInTheDocument();
  });

  it('pins explicitly with pressed states and unpins without hiding, duplicating, or cancelling the viewed chat', async () => {
    const user = start();
    const pin = projects().getByRole('button', { name: 'Pin Keyboard navigation' });
    expect(pin).toHaveAttribute('aria-pressed', 'false');
    pin.focus();
    await user.keyboard('{Enter}');
    expect(pin).toHaveAttribute('aria-pressed', 'true');
    expect(pin).toHaveAccessibleName('Unpin Keyboard navigation');
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    expect(pinned().getAllByRole('listitem')).toHaveLength(1);
    await user.click(pinned().getByRole('button', { name: 'Working Keyboard navigation Wayfinder' }));
    expect(projects().getByRole('button', { name: 'Working Keyboard navigation' })).toHaveAttribute('aria-current', 'page');
    expect(recent().queryByRole('button', { name: 'Working Keyboard navigation Wayfinder' })).not.toBeInTheDocument();
    const unpin = pinned().getByRole('button', { name: 'Unpin Keyboard navigation' });
    unpin.focus();
    await user.keyboard(' ');
    expect(screen.queryByRole('navigation', { name: 'Pinned chats' })).not.toBeInTheDocument();
    expect(screen.getByRole('region', { name: 'Chat library' })).toHaveFocus();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Keyboard navigation');
    expect(screen.getByRole('button', { name: 'Stop mock work' })).toBeVisible();
    expect(recent().getAllByRole('button', { name: 'Working Keyboard navigation Wayfinder' })).toHaveLength(1);
    expect(screen.getAllByRole('article', { name: 'AVA example response' })).toHaveLength(1);
    expect(projects().getAllByRole('listitem')).toHaveLength(6);
  });

  it('orders unique recent shortcuts by actual views, caps them at five, and only removes shortcuts', async () => {
    const user = start();
    for (const name of ['Working Keyboard navigation', 'Completed Simplify route config', 'Attention Export date range', 'Stopped Document the import flow', 'Failed Review retry behavior', 'Attention Export date range']) {
      await user.click(projects().getByRole('button', { name }));
    }
    const rows = recent().getAllByRole('listitem');
    expect(rows).toHaveLength(5);
    const names = ['Attention Export date range Papertrail', 'Failed Review retry behavior Relay', 'Stopped Document the import flow Papertrail', 'Completed Simplify route config Wayfinder', 'Working Keyboard navigation Wayfinder'];
    rows.forEach((row, index) => expect(within(row).getByRole('button', { name: names[index] })).toBeVisible());
    expect(screen.queryByRole('navigation', { name: 'Pinned chats' })).not.toBeInTheDocument();
    await user.click(recent().getByRole('button', { name: 'Remove Export date range from recent chats' }));
    expect(recent().getAllByRole('listitem')).toHaveLength(4);
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range');
    expect(projects().getByRole('button', { name: 'Attention Export date range' })).toBeVisible();
    expect(screen.getByRole('region', { name: 'Chat library' })).toHaveFocus();
  });

  it('retains per-chat drafts and model/mode across projects, pins, recents, and project collapse', async () => {
    const user = start();
    const draft = screen.getByRole('textbox', { name: /Message draft/ });
    await user.clear(draft);
    await user.type(draft, 'My search draft');
    await chooseMainModel(user, 'aster-mini');
    await user.selectOptions(screen.getByRole('combobox', { name: 'Mode for this chat' }), 'plan');
    await user.click(projects().getByRole('button', { name: 'Pin Remember search filters' }));
    await user.click(projects().getByRole('button', { name: 'Wayfinder' }));
    await user.click(projects().getByRole('button', { name: 'Attention Export date range' }));
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveValue('Keep the date range visible in the export summary.');
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    await user.clear(screen.getByRole('textbox', { name: /Message draft/ }));
    await user.type(screen.getByRole('textbox', { name: /Message draft/ }), 'My export draft');
    await user.click(pinned().getByRole('button', { name: 'Remember search filters Wayfinder' }));
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveValue('My search draft');
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Aster Mini');
    expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan');
    await user.click(pinned().getByRole('button', { name: 'Unpin Remember search filters' }));
    await user.click(recent().getByRole('button', { name: 'Attention Export date range Papertrail' }));
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveValue('My export draft');
    await user.click(recent().getByRole('button', { name: 'Remember search filters Wayfinder' }));
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveValue('My search draft');
    expect(projects().getByRole('button', { name: 'Wayfinder' })).toHaveAttribute('aria-expanded', 'false');
  });

  it('can pin from a recent shortcut without selecting or duplicating the chat', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Attention Export date range' }));
    await user.click(recent().getByRole('button', { name: 'Pin Remember search filters' }));
    expect(pinned().getAllByRole('button', { name: 'Remember search filters Wayfinder' })).toHaveLength(1);
    expect(recent().queryByRole('button', { name: 'Remember search filters Wayfinder' })).not.toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range');
    expect(screen.getByRole('region', { name: 'Chat library' })).toHaveFocus();
  });

  it('creates a local chat and Send appends escaped user text without a reply or network call', async () => {
    const fetchSpy = vi.fn();
    const xhrSpy = vi.fn();
    const socketSpy = vi.fn();
    vi.stubGlobal('fetch', fetchSpy);
    vi.stubGlobal('XMLHttpRequest', xhrSpy);
    vi.stubGlobal('WebSocket', socketSpy);
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Attention Export date range' }));
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');
    const transcript = screen.getByRole('region', { name: 'Conversation: New chat 1' });
    expect(transcript).toHaveAttribute('data-empty', 'true');
    const logo = within(transcript).getByRole('img', { name: 'AVA' });
    expect(logo).toBeVisible();
    expect(logo.children).toHaveLength(1);
    expect(logo.firstElementChild).toHaveAttribute('aria-hidden', 'true');
    expect(transcript.querySelectorAll('svg')).toHaveLength(1);
    expect(transcript.textContent).toBe('');
    expect(transcript.querySelector('h2, p, span, button, input')).toBeNull();
    expect(screen.getByRole('form', { name: 'Message composer' })).toBeVisible();
    expect(screen.getByRole('button', { name: 'Open Context · Context unknown' })).toBeVisible();
    expect(screen.getByRole('navigation', { name: 'Inspectors' })).toBeVisible();
    expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
    await user.type(screen.getByRole('textbox', { name: /Message draft/ }), '<script>alert("preview")</script>');
    await user.click(screen.getByRole('button', { name: 'Send' }));
    const message = screen.getByRole('article', { name: 'Your local preview message' });
    expect(screen.getByRole('region', { name: 'Conversation: New chat 1' })).toBe(transcript);
    expect(transcript).toHaveAttribute('data-empty', 'false');
    expect(logo).not.toBeInTheDocument();
    expect(transcript).toContainElement(message);
    expect(message).toHaveTextContent('<script>alert("preview")</script>');
    expect(message.querySelector('script')).toBeNull();
    expect(screen.queryByRole('article', { name: 'AVA example response' })).not.toBeInTheDocument();
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveValue('');
    expect(screen.getByRole('status')).toHaveTextContent('No model was called');
    expect(screen.queryByRole('navigation', { name: 'Pinned chats' })).not.toBeInTheDocument();
    expect(projects().getAllByRole('button', { name: 'New chat 1' })).toHaveLength(1);
    expect(recent().getAllByRole('button', { name: 'New chat 1 Papertrail' })).toHaveLength(1);
    expect(fetchSpy).not.toHaveBeenCalled();
    expect(xhrSpy).not.toHaveBeenCalled();
    expect(socketSpy).not.toHaveBeenCalled();
  });

  it('keeps an empty transcript logo-only and restores normal fixture content when messages exist', () => {
    const chat = createInitialState().chats[1];
    const { rerender } = render(<Conversation chat={{ ...chat, messages: [], draft: 'An unsent draft' }} />);
    const transcript = screen.getByRole('region', { name: `Conversation: ${chat.title}` });
    expect(within(transcript).getByRole('img', { name: 'AVA' })).toBeVisible();
    expect(transcript.textContent).toBe('');
    expect(transcript).toHaveAttribute('data-empty', 'true');
    rerender(<Conversation chat={chat} />);
    expect(transcript).toHaveAttribute('data-empty', 'false');
    expect(within(transcript).queryByRole('img', { name: 'AVA' })).not.toBeInTheDocument();
    expect(within(transcript).getByRole('article', { name: 'Your example message' })).toBeVisible();
    expect(transcript).toHaveTextContent('Working');
    expect(transcript).not.toHaveTextContent('Illustrative activity only');
  });

  it.each(['Control', 'Meta'])('supports local-only %s+Enter without creating an assistant response', async (modifier) => {
    const user = start();
    screen.getByRole('textbox', { name: /Message draft/ }).focus();
    await user.keyboard(`{${modifier}>}{Enter}{/${modifier}}`);
    expect(screen.getByRole('article', { name: 'Your local preview message' })).toBeVisible();
    expect(screen.getAllByRole('article', { name: 'AVA example response' })).toHaveLength(1);
  });

  it('keeps Shift+Enter as a newline and does not submit during composition', async () => {
    const user = start();
    const draft = screen.getByRole('textbox', { name: /Message draft/ });
    draft.focus();
    await user.keyboard('{Shift>}{Enter}{/Shift}');
    fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true, isComposing: true });
    expect(screen.queryByRole('article', { name: 'Your local preview message' })).not.toBeInTheDocument();
  });

  it('uses an explicit mock Stop for only the viewed fixture and does not make approval decisions', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Pin Keyboard navigation' }));
    await user.click(pinned().getByRole('button', { name: 'Working Keyboard navigation Wayfinder' }));
    await user.click(screen.getByRole('button', { name: 'Stop mock work' }));
    expect(projects().getByRole('button', { name: 'Stopped Keyboard navigation' })).toBeVisible();
    expect(pinned().getByRole('button', { name: 'Stopped Keyboard navigation Wayfinder' })).toBeVisible();
    expect(screen.queryByRole('button', { name: 'Stop mock work' })).not.toBeInTheDocument();
    await user.click(projects().getByRole('button', { name: 'Attention Export date range' }));
    expect(projects().getByRole('button', { name: 'Attention Export date range' })).toBeVisible();
    expect(screen.getByText(/Permission decisions are not part of this preview/)).toBeVisible();
    expect(screen.queryByRole('button', { name: /^(Approve|Reject|Allow|Deny)$/ })).not.toBeInTheDocument();
  });

  it('discloses fictional tool details with Enter and Space', async () => {
    const user = start();
    const disclosure = screen.getByRole('button', { name: /^Tool details · 3/ });
    expect(disclosure).toHaveAttribute('aria-expanded', 'false');
    disclosure.focus();
    await user.keyboard('{Enter}');
    expect(disclosure).toHaveAttribute('aria-expanded', 'true');
    expect(await screen.findByText('/tmp/example/pi-clipboard-12345678.png')).toBeVisible();
    await user.keyboard(' ');
    expect(disclosure).toHaveAttribute('aria-expanded', 'false');
    expect(screen.queryByText('/tmp/example/pi-clipboard-12345678.png')).not.toBeInTheDocument();
  });

  it('retains expanded tool/diff details through Settings Back without granting hidden controls authority, then resets on chat remount', async () => {
    const user = start(); const draft = screen.getByRole('textbox'); const draftValue = (draft as HTMLTextAreaElement).value;
    const transcript = screen.getByRole('region', { name: 'Conversation: Remember search filters' });
    const batch = screen.getByRole('button', { name: 'Expand all tools' }); await user.click(batch);
    const diff = await screen.findByLabelText('Supplied diff: search-state.ts'); expect(diff).toBeVisible();
    const group = screen.getByRole('button', { name: /^Tool details/ });
    await user.click(screen.getByRole('button', { name: 'Settings' })); await screen.findByRole('navigation', { name: 'Settings sections' });
    expect(transcript.closest('[hidden][inert]')).not.toBeNull();
    fireEvent.click(batch); fireEvent.click(group); expect(group).toHaveAttribute('aria-expanded', 'true');
    await user.click(screen.getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByLabelText('Supplied diff: search-state.ts')).toBe(diff); expect(screen.getByRole('textbox')).toBe(draft); expect(draft).toHaveValue(draftValue);
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(/^Aster$/);
    expect(screen.getByLabelText('Model: Aster')).toBeVisible(); expect(screen.getByLabelText('Thinking: High')).toBeVisible();
    await chooseMainModel(user, 'orion'); expect(screen.getByLabelText('Supplied diff: search-state.ts')).toBe(diff); expect(screen.getByLabelText('Model: Aster')).toBeVisible();
    await user.click(projects().getByRole('button', { name: 'Working Keyboard navigation' }));
    expect(screen.getByRole('button', { name: /^Tool details/ })).toHaveAttribute('aria-expanded', 'false');
    expect(screen.queryByLabelText(/^Supplied diff:/)).not.toBeInTheDocument();
    await user.click(projects().getByRole('button', { name: 'Remember search filters' }));
    expect(screen.getByRole('button', { name: 'Expand all tools' })).toBeVisible(); expect(screen.getByRole('button', { name: /^Tool details/ })).toHaveAttribute('aria-expanded', 'false');
    expect(screen.getByRole('textbox')).toHaveValue(draftValue); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
  });

  it('opens attached example changes, follows the viewed chat, and returns focus on close', async () => {
    const user = start();
    const toggle = screen.getByRole('button', { name: 'Changes' });
    toggle.focus();
    await user.keyboard('{Enter}');
    const review = screen.getByRole('complementary', { name: 'Mock changes' });
    expect(review).toBeVisible();
    expect(within(review).getByText('Invented excerpts for this chat. No files were accessed or changed.')).toBeVisible();
    expect(within(review).getByRole('heading', { name: 'search-state.ts' })).toBeVisible();
    await user.click(projects().getByRole('button', { name: 'Failed Review retry behavior' }));
    expect(within(review).getByText('No changes in this chat.')).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Close changes' }));
    expect(screen.queryByRole('complementary', { name: 'Mock changes' })).not.toBeInTheDocument();
    expect(toggle).toHaveFocus();
  });

  it('toggles the sidebar opaque control with the keyboard and preserves the draft', async () => {
    const user = start();
    const toggle = screen.getByRole('button', { name: 'Opaque materials' });
    expect(screen.getByRole('complementary', { name: 'Projects and chats' })).toContainElement(toggle);
    toggle.focus();
    await user.keyboard(' ');
    expect(toggle).toHaveAttribute('aria-pressed', 'true');
    expect(screen.getByRole('main', { name: 'AVA UI preview' })).toHaveAttribute('data-material', 'opaque');
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveValue('Add a regression test for returning from a result.');
    await user.keyboard('{Enter}');
    expect(toggle).toHaveAttribute('aria-pressed', 'false');
  });

  it('keeps fixture Markdown raw HTML disabled and the default safe URL handling', () => {
    const chat = createInitialState().chats[0];
    render(<Conversation chat={{ ...chat, messages: [{
      id: 'unsafe-example', role: 'assistant',
      markdown: '<img src="x" onerror="alert(1)">\n\n[Unsafe](javascript:alert%281%29)\n\n[Safe](https://example.com)',
    }] }} />);
    const transcript = screen.getByRole('region', { name: `Conversation: ${chat.title}` });
    expect(transcript.querySelector('img')).toBeNull();
    expect(screen.getByText('Unsafe')).not.toHaveAttribute('href', expect.stringMatching(/^javascript:/));
    expect(screen.getByRole('link', { name: 'Safe' })).toHaveAttribute('href', 'https://example.com');
  });
});
