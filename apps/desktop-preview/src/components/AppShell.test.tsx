import { StrictMode } from 'react';
import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
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
    expect(screen.queryByRole('complementary', { name: 'Changes inspector' })).not.toBeInTheDocument();
  });

  it('states the mock boundary once, in a dismissible banner that holds Run preview above the title, never above the composer', async () => {
    const user = start();
    const canvas = screen.getByRole('region', { name: 'Remember search filters' });
    const banner = screen.getByRole('region', { name: 'Preview' });
    expect(canvas.firstElementChild).toBe(banner);
    expect(banner).toHaveTextContent('Preview · invented data. Nothing is saved, sent or run.');
    expect(within(banner).getByRole('group', { name: 'Run preview' })).toContainElement(screen.getByRole('button', { name: 'Run preview' }));
    const composer = screen.getByRole('form', { name: 'Message composer' });
    const transcript = screen.getByRole('region', { name: 'Conversation: Remember search filters' });
    // Nothing sits between the transcript and the composer.
    expect(transcript.closest('#conversation-canvas > *')?.nextElementSibling).toContainElement(composer);
    expect(screen.getAllByText(/invented data/)).toHaveLength(1);
    await user.click(within(banner).getByRole('button', { name: 'Hide preview banner' }));
    expect(screen.queryByRole('region', { name: 'Preview' })).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Run preview' })).not.toBeInTheDocument();
    const chip = screen.getByRole('button', { name: 'Preview' });
    expect(chip).toHaveFocus();
    expect(chip).toHaveAttribute('aria-expanded', 'false');
    await user.keyboard('{Enter}');
    expect(screen.getByRole('region', { name: 'Preview' })).toBeVisible();
    expect(screen.getByRole('button', { name: 'Run preview' })).toHaveFocus();
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
    const identity = canvas.children[1];
    expect(identity).toContainElement(title);
    expect(identity).toHaveTextContent(/^Remember search filters$/);
    expect(identity.querySelector('p')).toBeNull();
    expect(within(identity as HTMLElement).getByRole('button', { name: 'Chat actions' })).toHaveAttribute('aria-haspopup', 'menu');
    expect(title.closest('[inert], [aria-hidden="true"], .srOnly')).toBeNull();
    expect(screen.queryByRole('group', { name: 'Parent chat details' })).not.toBeInTheDocument();
    expect(within(panel).queryByText('Wayfinder')).not.toBeInTheDocument();
    expect(panel.children).toHaveLength(1);
    expect(panel.firstElementChild?.firstElementChild).toContainElement(screen.getByRole('heading', { name: 'Agents 3' }));
    // The rail badge and the header agree: one working child of three.
    expect(panel.firstElementChild?.firstElementChild).toHaveTextContent('1 working');
    expect(screen.getByRole('button', { name: 'Agents' })).toHaveAccessibleDescription('1 working');
    expect(within(panel).queryByRole('button', { name: 'Context' })).not.toBeInTheDocument();
    expect(within(panel).queryByRole('button', { name: '2 changes' })).not.toBeInTheDocument();
    expect(rail.firstElementChild).toBe(screen.getByRole('button', { name: 'Hide inspector' }));
    await user.click(screen.getByRole('button', { name: 'Changes' }));
    expect(inspectorTrack).toContainElement(screen.getByRole('complementary', { name: 'Changes inspector' }));
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

  it('starts with the inspector closed in windows narrower than 1280px, for the first view and for new chats', async () => {
    vi.stubGlobal('matchMedia', vi.fn((query: string) => ({ matches: query === '(max-width: 1279.98px)', addEventListener: vi.fn(), removeEventListener: vi.fn() })));
    const user = start();
    expect(screen.queryByRole('complementary', { name: 'Agents inspector' })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Show inspector' })).toHaveAttribute('aria-expanded', 'false');
    await user.click(screen.getByRole('button', { name: 'Agents' }));
    expect(screen.getByRole('complementary', { name: 'Agents inspector' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');
    expect(screen.queryByRole('complementary', { name: 'Agents inspector' })).not.toBeInTheDocument();
  });

  it('keeps the full project library first and optional contextual shortcuts in the same sidebar scroll flow', async () => {
    const user = start();
    for (const name of ['Wayfinder', 'Papertrail', 'Relay']) {
      expect(projects().getByRole('button', { name })).toHaveAttribute('aria-expanded', 'true');
    }
    for (const name of ['Remember search filters', 'Keyboard navigation Working', 'Simplify route config Completed', 'Export date range Attention', 'Document the import flow Stopped', 'Review retry behavior Failed']) {
      expect(projects().getByRole('button', { name })).toBeVisible();
    }
    expect(recent().getAllByRole('listitem')).toHaveLength(1);
    await user.click(projects().getByRole('button', { name: 'Pin Export date range' }));
    const library = screen.getByRole('region', { name: 'Chat library' });
    const navigation = within(library).getAllByRole('navigation');
    expect(navigation.map((nav) => nav.getAttribute('aria-label'))).toEqual(['All project chats', 'Pinned chats', 'Recent chats']);
    expect(navigation.every((nav) => nav.parentElement === library)).toBe(true);
    expect(screen.getByRole('complementary', { name: 'Projects and chats' })).toContainElement(library);
    expect(pinned().getByRole('button', { name: 'Export date range Attention Papertrail' })).toBeVisible();
    expect(recent().getByRole('button', { name: 'Remember search filters Wayfinder' })).toBeVisible();
    expect(screen.getByRole('main').querySelector('button button')).toBeNull();
  });

  it('opens Find a chat and Commands from the sidebar and returns focus to the control that opened them', async () => {
    const user = start();
    const sidebar = within(screen.getByRole('complementary', { name: 'Projects and chats' }));
    const find = sidebar.getByRole('button', { name: 'Find a chat' });
    await user.click(find);
    const finder = await screen.findByRole('dialog', { name: 'Find a chat' });
    expect(within(finder).getByRole('combobox', { name: 'Search chats' })).toHaveFocus();
    await user.keyboard('{Escape}');
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(find).toHaveFocus();
    const commands = sidebar.getByRole('button', { name: 'Commands' });
    await user.click(commands);
    const palette = await screen.findByRole('dialog', { name: 'Commands' });
    expect(within(palette).getByRole('combobox', { name: 'Search commands' })).toHaveFocus();
    await user.keyboard('{Escape}');
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(commands).toHaveFocus();
  });

  it('collapses and expands project chats with the keyboard', async () => {
    const user = start();
    const project = projects().getByRole('button', { name: 'Wayfinder' });
    project.focus();
    await user.keyboard('{Enter}');
    expect(project).toHaveAttribute('aria-expanded', 'false');
    expect(projects().queryByRole('button', { name: 'Keyboard navigation Working' })).not.toBeInTheDocument();
    await user.keyboard(' ');
    expect(project).toHaveAttribute('aria-expanded', 'true');
    expect(projects().getByRole('button', { name: 'Keyboard navigation Working' })).toBeVisible();
  });

  it('supports sidebar navigation with no shortcuts and never changes a Working fixture on navigation', async () => {
    const user = start();
    await user.click(recent().getByRole('button', { name: 'Remove Remember search filters from recent chats' }));
    expect(screen.queryByRole('navigation', { name: 'Recent chats' })).not.toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    await user.click(projects().getByRole('button', { name: 'Keyboard navigation Working' }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Keyboard navigation');
    expect(screen.getByRole('button', { name: 'Stop' })).toBeVisible();
    await user.click(projects().getByRole('button', { name: 'Remember search filters' }));
    expect(screen.queryByRole('button', { name: 'Stop' })).not.toBeInTheDocument();
    expect(projects().getByRole('button', { name: 'Keyboard navigation Working' })).toBeVisible();
    await user.click(recent().getByRole('button', { name: 'Keyboard navigation Working Wayfinder' }));
    expect(screen.getByRole('button', { name: 'Stop' })).toBeVisible();
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
    await user.click(pinned().getByRole('button', { name: 'Keyboard navigation Working Wayfinder' }));
    expect(projects().getByRole('button', { name: 'Keyboard navigation Working' })).toHaveAttribute('aria-current', 'page');
    expect(recent().queryByRole('button', { name: 'Keyboard navigation Working Wayfinder' })).not.toBeInTheDocument();
    const unpin = pinned().getByRole('button', { name: 'Unpin Keyboard navigation' });
    unpin.focus();
    await user.keyboard(' ');
    expect(screen.queryByRole('navigation', { name: 'Pinned chats' })).not.toBeInTheDocument();
    expect(screen.getByRole('region', { name: 'Chat library' })).toHaveFocus();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Keyboard navigation');
    expect(screen.getByRole('button', { name: 'Stop' })).toBeVisible();
    expect(recent().getAllByRole('button', { name: 'Keyboard navigation Working Wayfinder' })).toHaveLength(1);
    expect(screen.getAllByRole('article', { name: 'AVA response' })).toHaveLength(1);
    expect(projects().getAllByRole('listitem')).toHaveLength(6);
  });

  it('orders unique recent shortcuts by actual views, caps them at five, and only removes shortcuts', async () => {
    const user = start();
    for (const name of ['Keyboard navigation Working', 'Simplify route config Completed', 'Export date range Attention', 'Document the import flow Stopped', 'Review retry behavior Failed', 'Export date range Attention']) {
      await user.click(projects().getByRole('button', { name }));
    }
    const rows = recent().getAllByRole('listitem');
    expect(rows).toHaveLength(5);
    const names = ['Export date range Attention Papertrail', 'Review retry behavior Failed Relay', 'Document the import flow Stopped Papertrail', 'Simplify route config Completed Wayfinder', 'Keyboard navigation Working Wayfinder'];
    rows.forEach((row, index) => expect(within(row).getByRole('button', { name: names[index] })).toBeVisible());
    expect(screen.queryByRole('navigation', { name: 'Pinned chats' })).not.toBeInTheDocument();
    await user.click(recent().getByRole('button', { name: 'Remove Export date range from recent chats' }));
    expect(recent().getAllByRole('listitem')).toHaveLength(4);
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range');
    expect(projects().getByRole('button', { name: 'Export date range Attention' })).toBeVisible();
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
    await user.click(projects().getByRole('button', { name: 'Export date range Attention' }));
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveValue('Keep the date range visible in the export summary.');
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    await user.clear(screen.getByRole('textbox', { name: /Message draft/ }));
    await user.type(screen.getByRole('textbox', { name: /Message draft/ }), 'My export draft');
    await user.click(pinned().getByRole('button', { name: 'Remember search filters Wayfinder' }));
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveValue('My search draft');
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Aster Mini');
    expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan');
    await user.click(pinned().getByRole('button', { name: 'Unpin Remember search filters' }));
    await user.click(recent().getByRole('button', { name: 'Export date range Attention Papertrail' }));
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveValue('My export draft');
    await user.click(recent().getByRole('button', { name: 'Remember search filters Wayfinder' }));
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveValue('My search draft');
    expect(projects().getByRole('button', { name: /^Wayfinder/ })).toHaveAttribute('aria-expanded', 'false');
  });

  it('can pin from a recent shortcut without selecting or duplicating the chat', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Export date range Attention' }));
    await user.click(recent().getByRole('button', { name: 'Pin Remember search filters' }));
    expect(pinned().getAllByRole('button', { name: 'Remember search filters Wayfinder' })).toHaveLength(1);
    expect(recent().queryByRole('button', { name: 'Remember search filters Wayfinder' })).not.toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Export date range');
    expect(screen.getByRole('region', { name: 'Chat library' })).toHaveFocus();
  });

  it('creates a local chat with typing focus in its composer, and Send appends escaped user text without a reply or network call', async () => {
    const fetchSpy = vi.fn();
    const xhrSpy = vi.fn();
    const socketSpy = vi.fn();
    vi.stubGlobal('fetch', fetchSpy);
    vi.stubGlobal('XMLHttpRequest', xhrSpy);
    vi.stubGlobal('WebSocket', socketSpy);
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Export date range Attention' }));
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveFocus();
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
    expect(screen.getByRole('button', { name: 'Send' })).toHaveAttribute('aria-disabled', 'true');
    await user.keyboard('<script>alert("preview")</script>');
    await user.click(screen.getByRole('button', { name: 'Send' }));
    const message = screen.getByRole('article', { name: 'Your message' });
    expect(screen.getByRole('region', { name: 'Conversation: New chat 1' })).toBe(transcript);
    expect(transcript).toHaveAttribute('data-empty', 'false');
    expect(logo).not.toBeInTheDocument();
    expect(transcript).toContainElement(message);
    expect(message).toHaveTextContent('<script>alert("preview")</script>');
    expect(message.querySelector('script')).toBeNull();
    expect(screen.queryByRole('article', { name: 'AVA response' })).not.toBeInTheDocument();
    expect(screen.getByRole('textbox', { name: /Message draft/ })).toHaveValue('');
    expect(screen.getByRole('status')).toHaveTextContent('Message added.');
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
    expect(within(transcript).getByRole('article', { name: 'Your message' })).toBeVisible();
    expect(transcript).toHaveTextContent('Working');
    expect(transcript).not.toHaveTextContent('Illustrative activity only');
  });

  it.each(['Control', 'Meta'])('supports local-only %s+Enter without creating an assistant response', async (modifier) => {
    const user = start();
    screen.getByRole('textbox', { name: /Message draft/ }).focus();
    await user.keyboard(`{${modifier}>}{Enter}{/${modifier}}`);
    expect(screen.getAllByRole('article', { name: 'Your message' })).toHaveLength(2);
    expect(screen.getAllByRole('article', { name: 'AVA response' })).toHaveLength(1);
  });

  it('keeps Shift+Enter as a newline and does not submit during composition', async () => {
    const user = start();
    const draft = screen.getByRole('textbox', { name: /Message draft/ });
    draft.focus();
    await user.keyboard('{Shift>}{Enter}{/Shift}');
    fireEvent.keyDown(draft, { key: 'Enter', ctrlKey: true, isComposing: true });
    expect(screen.getAllByRole('article', { name: 'Your message' })).toHaveLength(1);
  });

  it('uses an explicit Stop for only the viewed fixture and keeps permission choices with the request', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Pin Keyboard navigation' }));
    await user.click(pinned().getByRole('button', { name: 'Keyboard navigation Working Wayfinder' }));
    await user.click(screen.getByRole('button', { name: 'Stop' }));
    expect(projects().getByRole('button', { name: 'Keyboard navigation Stopped' })).toBeVisible();
    expect(pinned().getByRole('button', { name: 'Keyboard navigation Stopped Wayfinder' })).toBeVisible();
    expect(screen.queryByRole('button', { name: 'Stop' })).not.toBeInTheDocument();
    await user.click(projects().getByRole('button', { name: 'Export date range Attention' }));
    expect(projects().getByRole('button', { name: 'Export date range Attention' })).toBeVisible();
    expect(screen.queryByRole('button', { name: /^(Approve|Reject)$/ })).not.toBeInTheDocument();
  });

  it('discloses tool details with Enter and Space', async () => {
    const user = start();
    const disclosure = screen.getByRole('button', { name: /^3 tools\b/ });
    expect(disclosure).toHaveAttribute('aria-expanded', 'false');
    disclosure.focus();
    await user.keyboard('{Enter}');
    expect(disclosure).toHaveAttribute('aria-expanded', 'true');
    expect(await screen.findByText('/tmp/pi-clipboard-12345678.png')).toBeVisible();
    await user.keyboard(' ');
    expect(disclosure).toHaveAttribute('aria-expanded', 'false');
    expect(screen.queryByText('/tmp/pi-clipboard-12345678.png')).not.toBeInTheDocument();
  });

  it('retains expanded tool/diff details through Settings Back without granting hidden controls authority, then resets on chat remount', async () => {
    const user = start(); const draft = screen.getByRole('textbox'); const draftValue = (draft as HTMLTextAreaElement).value;
    const transcript = screen.getByRole('region', { name: 'Conversation: Remember search filters' });
    const batch = screen.getByRole('button', { name: 'Expand all tools' }); await user.click(batch);
    const diff = await screen.findByLabelText('Diff: src/search-state.ts'); expect(diff).toBeVisible();
    const group = screen.getByRole('button', { name: /^\d+ tools?\b/ });
    await user.click(screen.getByRole('button', { name: 'Settings' })); await screen.findByRole('navigation', { name: 'Settings sections' });
    expect(transcript.closest('[hidden][inert]')).not.toBeNull();
    fireEvent.click(batch); fireEvent.click(group); expect(group).toHaveAttribute('aria-expanded', 'true');
    await user.click(screen.getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByLabelText('Diff: src/search-state.ts')).toBe(diff); expect(screen.getByRole('textbox')).toBe(draft); expect(draft).toHaveValue(draftValue);
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent(/^Aster$/);
    expect(screen.getByLabelText('Model: Aster')).toBeVisible(); expect(screen.getByLabelText('Thinking: High')).toBeVisible();
    await chooseMainModel(user, 'orion'); expect(screen.getByLabelText('Diff: src/search-state.ts')).toBe(diff); expect(screen.getByLabelText('Model: Aster')).toBeVisible();
    await user.click(projects().getByRole('button', { name: 'Keyboard navigation Working' }));
    expect(screen.getByRole('button', { name: /^\d+ tools?\b/ })).toHaveAttribute('aria-expanded', 'false');
    expect(screen.queryByLabelText(/^Diff:/)).not.toBeInTheDocument();
    await user.click(projects().getByRole('button', { name: 'Remember search filters' }));
    expect(screen.getByRole('button', { name: 'Expand all tools' })).toBeVisible(); expect(screen.getByRole('button', { name: /^\d+ tools?\b/ })).toHaveAttribute('aria-expanded', 'false');
    expect(screen.getByRole('textbox')).toHaveValue(draftValue); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
  });

  it('opens attached changes, follows the viewed chat, and returns focus on close', async () => {
    const user = start();
    const toggle = screen.getByRole('button', { name: 'Changes' });
    toggle.focus();
    await user.keyboard('{Enter}');
    const review = screen.getByRole('complementary', { name: 'Changes inspector' });
    expect(review).toBeVisible();
    expect(review).not.toHaveTextContent(/Invented|No files were accessed/);
    expect(within(review).getByRole('heading', { name: /search-state\.ts/ })).toBeVisible();
    await user.click(projects().getByRole('button', { name: 'Review retry behavior Failed' }));
    expect(within(review).getByText('No changes in this chat.')).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Close changes' }));
    expect(screen.queryByRole('complementary', { name: 'Changes inspector' })).not.toBeInTheDocument();
    expect(toggle).toHaveFocus();
  });

  it('archives the current chat from the title menu, hides it everywhere, and Undo restores it', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Pin Remember search filters' }));
    const trigger = screen.getByRole('button', { name: 'Chat actions' });
    await user.click(trigger);
    const menu = screen.getByRole('menu', { name: 'Chat actions for Remember search filters' });
    expect(within(menu).getAllByRole('menuitem').map((item) => item.dataset.label)).toEqual(['Rename', 'Branch from a message…', 'Archive']);
    // The same F2 hint as the sidebar row menu; F2 on the title or this button renames.
    expect(within(menu).getByRole('menuitem', { name: 'Rename' })).toHaveAttribute('aria-keyshortcuts', 'F2');
    await user.click(within(menu).getByRole('menuitem', { name: 'Archive' }));
    const dialog = await screen.findByRole('dialog', { name: 'Archive chat?' });
    await user.click(within(dialog).getByRole('button', { name: 'Archive chat' }));
    expect(screen.getByRole('heading', { level: 1 })).not.toHaveTextContent('Remember search filters');
    expect(projects().queryByRole('button', { name: 'Remember search filters' })).not.toBeInTheDocument();
    expect(screen.queryByRole('navigation', { name: 'Pinned chats' })).not.toBeInTheDocument();
    const notice = screen.getByRole('group', { name: 'Archived chat' });
    expect(notice).toHaveTextContent('Archived “Remember search filters”');
    const undo = within(notice).getByRole('button', { name: 'Undo' });
    expect(undo).toHaveFocus();
    await user.keyboard('{Enter}');
    expect(screen.queryByRole('group', { name: 'Archived chat' })).not.toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    expect(screen.getByRole('heading', { level: 1 })).toHaveFocus();
    expect(projects().getByRole('button', { name: 'Remember search filters' })).toBeVisible();
    expect(pinned().getByRole('button', { name: 'Remember search filters Wayfinder' })).toBeVisible();
  });

  it('returns from a child view to its parent with Escape and focuses the Agents row that opened it', async () => {
    const user = start();
    const row = within(screen.getByRole('complementary', { name: 'Agents inspector' })).getByRole('button', { name: /^Completed Map return paths/ });
    await user.click(row);
    const transcript = await screen.findByRole('region', { name: /Child conversation:/ });
    expect(screen.queryByRole('button', { name: 'Chat actions' })).not.toBeInTheDocument();
    expect(within(transcript).getAllByText('You').length).toBeGreaterThan(0);
    transcript.focus();
    await user.keyboard('{Escape}');
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    expect(row).toHaveFocus();
    expect(screen.getByRole('form', { name: 'Message composer' })).toBeVisible();
    // Without its row on screen, Escape lands on the parent title.
    await user.click(row);
    await user.click(screen.getByRole('button', { name: 'Hide inspector' }));
    (await screen.findByRole('region', { name: /Child conversation:/ })).focus();
    await user.keyboard('{Escape}');
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    expect(screen.getByRole('heading', { level: 1 })).toHaveFocus();
  });

  it('renames from the title with F2', async () => {
    const user = start();
    screen.getByRole('heading', { level: 1 }).focus();
    await user.keyboard('{F2}');
    expect(await screen.findByRole('dialog', { name: 'Rename chat' })).toBeVisible();
  });

  it('clears Attention everywhere once the inline request is answered, and keeps the receipt across chat switches', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Export date range Attention' }));
    expect(recent().getByRole('button', { name: 'Export date range Attention Papertrail' })).toBeVisible();
    await user.click(within(screen.getByRole('group', { name: 'Your decision' })).getByRole('button', { name: 'Deny' }));
    expect(projects().getByRole('button', { name: 'Export date range' })).toHaveAttribute('aria-current', 'page');
    expect(recent().getByRole('button', { name: 'Export date range Papertrail' })).toBeVisible();
    expect(screen.queryByRole('button', { name: /attention/i })).not.toBeInTheDocument();
    await user.click(projects().getByRole('button', { name: 'Remember search filters' }));
    await user.click(projects().getByRole('button', { name: 'Export date range' }));
    expect(screen.getByText(/You chose Deny/)).toBeVisible();
    expect(screen.queryByText('Needs your decision')).not.toBeInTheDocument();
  });

  it('scrolls a waiting request into view when its chat opens', async () => {
    const scrolled: Element[] = [];
    const pending = () => scrolled.filter((element) => element.hasAttribute('data-permission-pending'));
    Object.defineProperty(Element.prototype, 'scrollIntoView', { configurable: true, value(this: Element) { scrolled.push(this); } });
    try {
      const user = start();
      expect(pending()).toHaveLength(0);
      await user.click(projects().getByRole('button', { name: 'Export date range Attention' }));
      expect(pending()).toHaveLength(1);
      expect(within(pending()[0] as HTMLElement).getByRole('button', { name: 'Deny' })).toBeVisible();
    } finally {
      delete (Element.prototype as Partial<Element>).scrollIntoView;
    }
  });

  it('branches from a message into a new chat in the same project that opens with typing focus', async () => {
    const user = start();
    await user.click(screen.getByRole('button', { name: 'Chat actions' }));
    await user.click(within(screen.getByRole('menu')).getByRole('menuitem', { name: 'Branch from a message…' }));
    const dialog = await screen.findByRole('dialog', { name: 'Branch from a message' });
    expect(within(dialog).getByRole('radio')).toBeChecked();
    expect(dialog).toHaveTextContent('Keep the search filters when I open a result and go back…');
    await user.click(within(dialog).getByRole('button', { name: 'Create branch' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters (branch)');
    expect(screen.getByRole('textbox')).toHaveFocus();
    expect(screen.getByRole('textbox')).toHaveValue('');
    const transcript = screen.getByRole('region', { name: 'Conversation: Remember search filters (branch)' });
    expect(within(transcript).getAllByRole('article', { name: 'Your message' })).toHaveLength(1);
    expect(within(transcript).queryByRole('article', { name: 'AVA response' })).not.toBeInTheDocument();
    expect(projects().getByRole('button', { name: 'Remember search filters (branch)' })).toHaveAttribute('aria-current', 'page');
    expect(recent().getByRole('button', { name: 'Remember search filters (branch) Wayfinder' })).toBeVisible();
    expect(projects().getByRole('button', { name: 'Remember search filters' })).toBeVisible();
  });

  it('asks AVA to retry a failed chat from the message box without discarding a draft', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Review retry behavior Failed' }));
    await user.click(await screen.findByRole('button', { name: 'Ask AVA to retry' }));
    const box = screen.getByRole('textbox');
    expect(box).toHaveValue('Retry the last step.');
    expect(box).toHaveFocus();
    fireEvent.change(box, { target: { value: 'Check the backoff cap too.' } });
    await user.click(screen.getByRole('button', { name: 'Ask AVA to retry' }));
    expect(box).toHaveValue('Check the backoff cap too.\nRetry the last step.');
    expect(box).toHaveFocus();
  });

  it('starts keyboard travel at skip links and steps between regions with F6 and Shift+F6', async () => {
    const user = start();
    await user.tab();
    const skip = screen.getByRole('link', { name: 'Skip to conversation' });
    expect(skip).toHaveFocus();
    await user.keyboard('{Enter}');
    const transcript = screen.getByRole('region', { name: 'Conversation: Remember search filters' });
    expect(transcript).toHaveFocus();
    skip.focus();
    await user.tab();
    expect(screen.getByRole('link', { name: 'Skip to message box' })).toHaveFocus();
    await user.keyboard('{Enter}');
    const box = screen.getByRole('textbox');
    expect(box).toHaveFocus();
    await user.keyboard('{F6}');
    expect(screen.getByRole('complementary', { name: 'Agents inspector' })).toHaveFocus();
    await user.keyboard('{F6}');
    expect(projects().getByRole('button', { name: 'Remember search filters' })).toHaveFocus();
    await user.keyboard('{F6}');
    expect(transcript).toHaveFocus();
    await user.keyboard('{F6}');
    expect(box).toHaveFocus();
    await user.keyboard('{Shift>}{F6}{/Shift}');
    expect(transcript).toHaveFocus();
    // A closed panel drops out of the cycle.
    await user.click(screen.getByRole('button', { name: 'Hide inspector' }));
    box.focus();
    await user.keyboard('{F6}');
    expect(projects().getByRole('button', { name: 'Remember search filters' })).toHaveFocus();
  });

  it('moves focus into a panel its shortcut opens, docked too, and back to the rail when the shortcut closes it', async () => {
    const user = start();
    screen.getByRole('textbox').focus();
    await user.keyboard('{Control>}{Shift>}2{/Shift}{/Control}');
    await waitFor(() => expect(screen.getByRole('complementary', { name: 'Changes inspector' })).toHaveFocus());
    await user.keyboard('{Control>}{Shift>}2{/Shift}{/Control}');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Changes' })).toHaveFocus());
    expect(screen.queryByRole('complementary', { name: 'Changes inspector' })).not.toBeInTheDocument();
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
