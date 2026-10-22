import { StrictMode } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { chooseMainModel, installDialogTestSupport } from '../screens/dialogTestSupport';

import { createInitialState } from '../state/fixtures';
import { AppShell } from './AppShell';
import ChildConversation from './ChildConversation';

beforeEach(installDialogTestSupport);

function start() {
  const user = userEvent.setup();
  render(<StrictMode><AppShell /></StrictMode>);
  return user;
}

function projects() {
  return within(screen.getByRole('navigation', { name: 'All project chats' }));
}

function agents() {
  return within(screen.getByRole('complementary', { name: 'Agents inspector' }));
}

function childButton(title: string) {
  return agents().getByRole('button', { name: new RegExp(title) });
}

describe('attached inspector and read-only child view', () => {
  it('defaults to Agents, derives working-first rows, and keeps exactly three functional rail modules', async () => {
    const user = start();
    const rail = screen.getByRole('navigation', { name: 'Inspectors' });
    const toggle = within(rail).getByRole('button', { name: 'Agents' });
    expect(within(rail).getAllByRole('button')).toEqual([
      within(rail).getByRole('button', { name: 'Hide inspector' }),
      toggle,
      within(rail).getByRole('button', { name: 'Changes' }),
      within(rail).getByRole('button', { name: 'Context' }),
    ]);
    expect(toggle).toHaveAttribute('aria-expanded', 'true');
    expect(toggle).toHaveAttribute('aria-controls', 'agents-inspector');
    expect(toggle).toHaveAccessibleDescription('1 working');
    expect(toggle).toHaveAttribute('title', 'Agents · 1 working · Ctrl+Shift+1');
    expect(toggle).toHaveAttribute('aria-keyshortcuts');
    expect(agents().getAllByRole('listitem').map((item) => item.querySelector('button')?.dataset.childId)).toEqual(['search-return', 'search-outline', 'search-edge-cases']);
    expect(childButton('Review back navigation')).toHaveAccessibleName(/^Working Review back navigation Aster/);
    expect(childButton('Map return paths')).toHaveAccessibleName(/^Completed Map return paths Aster Mini/);
    expect(projects().getByRole('button', { name: 'Remember search filters' }).querySelector('[data-status]')).toBeNull();
    expect(screen.queryByRole('button', { name: 'Stop' })).not.toBeInTheDocument();
    toggle.focus();
    await user.keyboard(' ');
    expect(screen.queryByRole('complementary', { name: 'Agents inspector' })).not.toBeInTheDocument();
    expect(toggle).toHaveAttribute('aria-expanded', 'false');
    expect(toggle).not.toHaveAttribute('aria-controls');
    expect(toggle).toHaveAccessibleDescription('1 working');
    await user.keyboard('{Enter}');
    expect(toggle).toHaveAttribute('aria-expanded', 'true');
  });

  it('switches one panel at a time and returns focus to its rail button on Close or Escape', async () => {
    const user = start();
    const agentToggle = screen.getByRole('button', { name: 'Agents' });
    const changesToggle = screen.getByRole('button', { name: 'Changes' });
    await user.click(changesToggle);
    expect(screen.queryByRole('complementary', { name: 'Agents inspector' })).not.toBeInTheDocument();
    expect(screen.getByRole('complementary', { name: 'Changes inspector' })).toBeVisible();
    expect(changesToggle).toHaveAttribute('aria-controls', 'change-review');
    expect(agentToggle).toHaveAttribute('aria-expanded', 'false');
    await user.click(changesToggle);
    expect(screen.queryByRole('complementary', { name: 'Changes inspector' })).not.toBeInTheDocument();
    await user.click(agentToggle);
    await user.click(screen.getByRole('button', { name: 'Close agents' }));
    expect(agentToggle).toHaveFocus();
    await user.keyboard('{Enter}');
    childButton('Review back navigation').focus();
    await user.keyboard('{Escape}');
    expect(agentToggle).toHaveFocus();
    expect(agentToggle).toHaveAttribute('aria-expanded', 'false');
    await user.click(changesToggle);
    screen.getByRole('region', { name: 'Changes in this chat' }).focus();
    await user.keyboard('{Escape}');
    expect(changesToggle).toHaveFocus();
    expect(changesToggle).toHaveAttribute('aria-expanded', 'false');
  });

  it('treats a small-window sheet as transient: it takes focus, and focus arriving in the conversation it covers closes it', async () => {
    // jsdom has no layout: report the inspector track as the absolutely placed sheet CSS makes it at 1100px and below.
    const computed = globalThis.getComputedStyle.bind(globalThis);
    const placement = vi.spyOn(globalThis, 'getComputedStyle').mockImplementation((element: Element, pseudo?: string | null) => {
      const style = computed(element, pseudo);
      if (element.querySelector(':scope > #agents-inspector, :scope > #change-review, :scope > #context-inspector')) Object.defineProperty(style, 'position', { configurable: true, value: 'absolute' });
      return style;
    });
    const user = start();
    const changesToggle = screen.getByRole('button', { name: 'Changes' });
    await user.click(changesToggle);
    expect(screen.getByRole('complementary', { name: 'Changes inspector' })).toHaveFocus();
    // Shift+Tab leaves the sheet for Send, the control it covered: the sheet closes and focus stays on Send.
    await user.tab({ shift: true });
    expect(screen.getByRole('button', { name: 'Send' })).toHaveFocus();
    expect(screen.queryByRole('complementary', { name: 'Changes inspector' })).not.toBeInTheDocument();
    expect(changesToggle).toHaveAttribute('aria-expanded', 'false');

    const agentToggle = screen.getByRole('button', { name: 'Agents' });
    await user.click(agentToggle);
    expect(screen.getByRole('complementary', { name: 'Agents inspector' })).toHaveFocus();
    // The sidebar and rail sit beside the sheet, never beneath it.
    act(() => screen.getByRole('button', { name: 'Find a chat' }).focus());
    expect(screen.getByRole('complementary', { name: 'Agents inspector' })).toBeVisible();
    const draft = screen.getByRole('textbox');
    await user.click(draft);
    expect(draft).toHaveFocus();
    expect(screen.queryByRole('complementary', { name: 'Agents inspector' })).not.toBeInTheDocument();
    expect(agentToggle).toHaveAttribute('aria-expanded', 'false');

    // Escape still closes the sheet and returns focus to its rail button.
    await user.click(agentToggle);
    expect(screen.getByRole('complementary', { name: 'Agents inspector' })).toHaveFocus();
    await user.keyboard('{Escape}');
    expect(agentToggle).toHaveFocus();
    expect(agentToggle).toHaveAttribute('aria-expanded', 'false');

    // A docked panel sits beside the transcript: focus there leaves it open.
    placement.mockRestore();
    await user.click(agentToggle);
    expect(agentToggle).toHaveFocus();
    await user.click(draft);
    expect(draft).toHaveFocus();
    expect(screen.getByRole('complementary', { name: 'Agents inspector' })).toBeVisible();
  });

  it('widens the inspector for reading and keeps that width across modules and closing for the visit', async () => {
    const user = start();
    const workspace = screen.getByRole('navigation', { name: 'Inspectors' }).parentElement!;
    const widen = () => within(screen.getByRole('complementary', { name: /inspector$/ })).getByRole('button', { name: 'Widen inspector' });
    expect(widen()).toHaveAttribute('aria-pressed', 'false');
    expect(widen()).toHaveAttribute('title', 'Widen inspector');
    expect(workspace).not.toHaveAttribute('data-inspector-size');
    await user.click(widen());
    expect(widen()).toHaveFocus();
    expect(widen()).toHaveAttribute('aria-pressed', 'true');
    expect(widen()).toHaveAttribute('title', 'Narrow inspector');
    expect(widen()).toHaveAccessibleDescription('Narrow inspector');
    expect(workspace).toHaveAttribute('data-inspector-size', 'wide');
    const changesToggle = screen.getByRole('button', { name: 'Changes' });
    await user.click(changesToggle);
    expect(within(screen.getByRole('complementary', { name: 'Changes inspector' })).getByRole('button', { name: 'Widen inspector' })).toHaveAttribute('aria-pressed', 'true');
    await user.click(changesToggle);
    expect(screen.queryByRole('complementary', { name: 'Changes inspector' })).not.toBeInTheDocument();
    await user.click(changesToggle);
    expect(widen()).toHaveAttribute('aria-pressed', 'true');
    expect(workspace).toHaveAttribute('data-inspector-size', 'wide');
    await user.click(widen());
    expect(widen()).toHaveAttribute('aria-pressed', 'false');
    expect(widen()).toHaveAttribute('title', 'Widen inspector');
    expect(workspace).not.toHaveAttribute('data-inspector-size');
  });

  it('shows a truthful empty roster and zero selected-parent count even with working examples elsewhere', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Simplify route config Completed' }));
    expect(agents().getByRole('heading', { name: 'No child agents' })).toBeVisible();
    expect(agents().queryByRole('list')).not.toBeInTheDocument();
    const toggle = screen.getByRole('button', { name: 'Agents' });
    expect(toggle).toHaveAccessibleDescription('0 working');
    expect(toggle.querySelector('span')).toBeNull();
    await user.click(toggle);
    expect(toggle).toHaveAccessibleDescription('0 working');
    await user.click(projects().getByRole('button', { name: 'Remember search filters' }));
    expect(toggle).toHaveAccessibleDescription('1 working');
    expect(toggle).toHaveAttribute('aria-expanded', 'false');
  });

  it('inspects child conversations without a composer or new top-level identity and restores the parent settings', async () => {
    const user = start();
    const draftText = 'Keep this parent draft\nAnd this second line.';
    fireEvent.change(screen.getByRole('textbox'), { target: { value: draftText } });
    await chooseMainModel(user, 'orion');
    await user.selectOptions(screen.getByRole('combobox', { name: 'Mode for this chat' }), 'plan');
    await user.click(screen.getByRole('button', { name: 'Expand draft' }));
    await user.click(screen.getByRole('button', { name: 'Add context' }));
    await user.click(screen.getByRole('button', { name: 'image result-layout.png' }));
    await user.keyboard('{Escape}');
    const recent = screen.getByRole('navigation', { name: 'Recent chats' });
    const originalRecent = recent.textContent;
    const row = childButton('Review back navigation');
    row.focus();
    await user.keyboard('{Enter}');
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Review back navigation');
    expect(screen.getByRole('heading', { level: 1 })).toHaveFocus();
    expect(row).toHaveAttribute('aria-current', 'true');
    expect(screen.queryByText('Child of Remember search filters · Wayfinder')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Back to parent' })).toHaveAccessibleDescription('Back to Remember search filters · Esc. Its draft and settings are unchanged.');
    expect(await screen.findByRole('group', { name: 'Child agent' })).toHaveTextContent('Aster');
    expect(screen.getByRole('group', { name: 'Child agent' })).toHaveTextContent('Read only');
    expect(screen.getAllByRole('button', { name: 'Back to parent' })).toHaveLength(1);
    expect(screen.getByRole('article', { name: 'Your message' })).toBeVisible();
    expect(screen.getByRole('article', { name: 'Child agent response' })).toHaveTextContent('suggested manual check, not a completed one');
    expect(screen.getByRole('status')).toHaveTextContent('read only');
    expect(screen.queryByRole('form', { name: 'Message composer' })).not.toBeInTheDocument();
    expect(screen.queryByRole('textbox')).not.toBeInTheDocument();
    expect(screen.queryByRole('combobox')).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: /^(Send|Stop|Steer|Restart|Promote)$/i })).not.toBeInTheDocument();
    expect(projects().getAllByRole('listitem')).toHaveLength(6);
    expect(projects().getByRole('button', { name: 'Remember search filters' })).toHaveAttribute('aria-current', 'page');
    expect(screen.queryByRole('navigation', { name: 'Pinned chats' })).not.toBeInTheDocument();
    expect(recent.textContent).toBe(originalRecent);
    await user.click(screen.getByRole('button', { name: 'Close agents' }));
    expect(screen.getByRole('region', { name: /Child conversation:/ })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Changes' }));
    expect(within(screen.getByRole('complementary', { name: 'Changes inspector' })).getByRole('heading', { name: /search-state\.ts/ })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Back to parent' }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveFocus();
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('Remember search filters');
    expect(screen.getByRole('textbox')).toHaveValue(draftText);
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan');
    expect(screen.getByRole('button', { name: 'Collapse draft' })).toHaveAttribute('aria-expanded', 'true');
    expect(screen.getByRole('list', { name: 'Attached context' })).toHaveTextContent('result-layout.png');
    expect(recent.textContent).toBe(originalRecent);
  });

  it.each(['open', 'closed'])('keeps one visible child identity and Back with the pane %s and focuses the committed return title', async (pane) => {
    const user = start();
    const title = screen.getByRole('heading', { level: 1 });
    const row = childButton('Review back navigation');
    row.focus();
    await user.keyboard('{Enter}');
    const canvas = screen.getByRole('region', { name: 'Review back navigation' });
    expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(canvas).toContainElement(title);
    expect(title).toHaveAttribute('id', 'conversation-title');
    expect(title).toHaveFocus();
    expect(title.closest('[inert], [aria-hidden="true"], .srOnly')).toBeNull();
    expect(canvas.children[1]).toHaveTextContent(/^Review back navigationBack to parent$/);
    expect(canvas.children[1].querySelector('p')).toBeNull();
    expect(screen.queryByRole('group', { name: 'Parent chat details' })).not.toBeInTheDocument();
    // Re-inspecting the same reducer identity must still focus the visible title.
    await user.click(childButton('Review back navigation'));
    expect(title).toHaveFocus();
    if (pane === 'closed') await user.click(screen.getByRole('button', { name: 'Hide inspector' }));
    else await user.click(screen.getByRole('button', { name: 'Changes' }));
    expect(screen.getAllByRole('heading', { level: 1 })).toEqual([title]);
    expect(document.querySelectorAll('#conversation-title')).toHaveLength(1);
    const back = within(canvas).getByRole('button', { name: 'Back to parent' });
    expect(screen.getAllByRole('button', { name: 'Back to parent' })).toEqual([back]);
    expect(back).toBeVisible();
    expect(back).toHaveAttribute('title', 'Back to Remember search filters · Esc. Its draft and settings are unchanged.');
    expect(back).toHaveAccessibleDescription(back.title);
    expect(screen.queryByRole('form', { name: 'Message composer' })).not.toBeInTheDocument();
    back.focus();
    await user.keyboard('{Enter}');
    const parentTitle = screen.getByRole('heading', { level: 1 });
    expect(parentTitle).toBe(title);
    expect(parentTitle).toHaveTextContent('Remember search filters');
    expect(parentTitle).toHaveFocus();
    expect(parentTitle.closest('[inert], [aria-hidden="true"], .srOnly')).toBeNull();
    expect(canvas).toHaveAccessibleName('Remember search filters');
    expect(document.querySelectorAll('#conversation-title')).toHaveLength(1);
    expect(screen.queryByRole('button', { name: 'Back to parent' })).not.toBeInTheDocument();
    expect(canvas).toContainElement(parentTitle);
    expect(canvas.children[1]).toHaveTextContent(/^Remember search filters$/);
  });

  it('keeps one stable canvas title across resize and pane modes without a placement subscription or deferred focus', async () => {
    const matchMedia = vi.fn(() => ({ matches: false, addEventListener: vi.fn(), removeEventListener: vi.fn() }));
    vi.stubGlobal('matchMedia', matchMedia);
    const user = start();
    const canvas = screen.getByRole('region', { name: 'Remember search filters' });
    const title = screen.getByRole('heading', { level: 1 });
    title.focus();
    const focus = vi.spyOn(title, 'focus');
    for (const width of [850, 1440, 2560, 3440, 850]) {
      vi.stubGlobal('innerWidth', width);
      fireEvent(window, new Event('resize'));
      expect(screen.getByRole('heading', { level: 1 })).toBe(title);
      expect(title).toHaveFocus();
      expect(canvas).toContainElement(title);
      expect(title.closest('[inert], [aria-hidden="true"], .srOnly')).toBeNull();
      expect(document.querySelectorAll('#conversation-title')).toHaveLength(1);
    }
    const draft = screen.getByRole('textbox');
    await user.type(draft, ' Still editing.');
    expect(draft).toHaveFocus();
    for (const name of ['Hide inspector', 'Show inspector', 'Changes', 'Context', 'Agents']) {
      const button = screen.getByRole('button', { name });
      await user.click(button);
      expect(button).toHaveFocus();
      expect(screen.getByRole('heading', { level: 1 })).toBe(title);
      expect(canvas).toContainElement(title);
      expect(document.querySelectorAll('h1')).toHaveLength(1);
      expect(document.querySelectorAll('#conversation-title')).toHaveLength(1);
    }
    await user.click(draft);
    for (const width of [1440, 850, 3440]) {
      vi.stubGlobal('innerWidth', width);
      fireEvent(window, new Event('resize'));
      expect(draft).toHaveFocus();
      expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    }
    expect(focus).not.toHaveBeenCalled();
    await user.click(childButton('Map return paths'));
    expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(title).toHaveTextContent('Map return paths');
    expect(title).toHaveFocus();
    fireEvent(window, new Event('resize'));
    expect(title).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Back to parent' }));
    expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(canvas).toContainElement(title);
    expect(title).toHaveTextContent('Remember search filters');
    expect(title).toHaveFocus();
    expect(focus).toHaveBeenCalledTimes(2);
    await user.type(screen.getByRole('textbox'), ' After returning.');
    expect(screen.getByRole('textbox')).toHaveFocus();
    expect(focus).toHaveBeenCalledTimes(2);
    expect(matchMedia).not.toHaveBeenCalledWith('(max-width: 1100px)');
    focus.mockRestore();
    // DOM identity/focus only; resize geometry and focus paint are browser-owned.
  });

  it('does not focus a title during ordinary Send, settings or navigation, and New moves typing focus to the composer', async () => {
    const user = start();
    const title = screen.getByRole('heading', { level: 1 });
    const focus = vi.spyOn(title, 'focus');
    const draft = screen.getByRole('textbox');
    await user.type(draft, ' Keep normal editing focus.');
    expect(draft).toHaveFocus();
    await chooseMainModel(user, 'orion');
    expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveFocus();
    const mode = screen.getByRole('combobox', { name: 'Mode for this chat' });
    await user.selectOptions(mode, 'plan');
    expect(mode).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Send' }));
    expect(focus).not.toHaveBeenCalled();
    expect(title).not.toHaveFocus();
    const navigate = projects().getByRole('button', { name: 'Export date range Attention' });
    await user.click(navigate);
    expect(navigate).toHaveFocus();
    const newChat = screen.getByRole('button', { name: 'New chat' });
    await user.click(newChat);
    expect(screen.getByRole('textbox')).toHaveFocus();
    expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');
    expect(screen.getByRole('heading', { level: 1 })).not.toHaveFocus();
    expect(focus).not.toHaveBeenCalled();
    focus.mockRestore();
  });

  it('never retargets a working parent Stop to a child and leaves child liveness independent after returning', async () => {
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Keyboard navigation Working' }));
    const sent = screen.queryAllByRole('article', { name: 'Your message' }).length;
    await user.type(screen.getByRole('textbox'), 'A future parent message');
    await user.keyboard('{Control>}{Enter}{/Control}');
    expect(screen.queryAllByRole('article', { name: 'Your message' })).toHaveLength(sent);
    await user.click(childButton('Review focus boundaries'));
    expect(screen.queryByRole('button', { name: 'Stop' })).not.toBeInTheDocument();
    await user.keyboard('{Control>}{Enter}{/Control}');
    expect(projects().getByRole('button', { name: 'Keyboard navigation Working' })).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Back to parent' }));
    expect(screen.getByRole('textbox')).toHaveValue('A future parent message');
    await user.click(screen.getByRole('button', { name: 'Stop' }));
    expect(projects().getByRole('button', { name: 'Keyboard navigation Stopped' })).toBeVisible();
    expect(childButton('Review focus boundaries')).toHaveAccessibleName(/^Working/);
    expect(screen.getByRole('button', { name: 'Agents' })).toHaveAccessibleDescription('1 working');
  });

  it('clears child inspection when the same parent, another sidebar chat, or New is selected', async () => {
    const user = start();
    for (const name of ['Remember search filters', 'Export date range Attention']) {
      await user.click(childButton('Review back navigation'));
      await user.click(projects().getByRole('button', { name }));
      expect(screen.queryByRole('region', { name: /Child conversation:/ })).not.toBeInTheDocument();
      expect(screen.getByRole('textbox')).toBeVisible();
      await user.click(projects().getByRole('button', { name: 'Remember search filters' }));
    }
    await user.click(childButton('Review back navigation'));
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(screen.getByRole('heading', { level: 1 })).toHaveTextContent('New chat 1');
    expect(screen.getByRole('textbox')).toHaveValue('');
    expect(agents().getByRole('heading', { name: 'No child agents' })).toBeVisible();
    expect(screen.queryByRole('button', { name: 'Back to parent' })).not.toBeInTheDocument();
  });

  it('distinguishes completed, failed, and unavailable children without tool streams, progress, or invented success', async () => {
    const user = start();
    await user.click(childButton('Map return paths'));
    let transcript = await screen.findByRole('region', { name: /Child conversation:/ });
    expect(within(transcript).getByText('Completed')).toBeVisible();
    await user.click(childButton('Check empty queries'));
    transcript = screen.getByRole('region', { name: /Child conversation:/ });
    expect(within(transcript).getByText('Failed')).toBeVisible();
    expect(screen.queryByRole('progressbar')).not.toBeInTheDocument();
    await user.click(projects().getByRole('button', { name: 'Export date range Attention' }));
    await user.click(childButton('Review summary wording'));
    transcript = screen.getByRole('region', { name: /Child conversation:/ });
    expect(within(transcript).getByText('Unavailable')).toBeVisible();
    expect(transcript).toHaveTextContent('No conversation to display.');
    expect(within(transcript).queryByRole('article')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Back to parent' })).toBeVisible();
  });

  it('gives each parent status a distinct static glyph and word without truncating the accessible title, including shortcuts', async () => {
    const user = start();
    const fixtures = [
      ['Working', 'Keyboard navigation'], ['Completed', 'Simplify route config'],
      ['Attention', 'Export date range'], ['Stopped', 'Document the import flow'], ['Failed', 'Review retry behavior'],
    ];
    const glyphs = new Set<string>();
    for (const [status, title] of fixtures) {
      const row = projects().getByRole('button', { name: `${title} ${status}` });
      expect(row).toHaveTextContent(status);
      glyphs.add(row.querySelector('[data-status] path')!.getAttribute('d')!);
    }
    expect(glyphs.size).toBe(5);
    await user.click(projects().getByRole('button', { name: 'Simplify route config Completed' }));
    await user.click(projects().getByRole('button', { name: 'Pin Simplify route config' }));
    const pin = within(screen.getByRole('navigation', { name: 'Pinned chats' })).getByRole('button', { name: 'Simplify route config Completed Wayfinder' });
    expect(pin.querySelector('[data-status]')).not.toBeNull();
    await user.click(projects().getByRole('button', { name: 'Remember search filters' }));
    await user.click(pin);
    expect(projects().getByRole('button', { name: 'Simplify route config Completed' })).toHaveAttribute('aria-current', 'page');
    const transcript = screen.getByRole('region', { name: 'Conversation: Simplify route config' });
    expect(within(transcript).getByText('Completed')).toBeVisible();
    expect(transcript).not.toHaveTextContent('viewing it does not clear the label');
  });

  it('uses escaped text and the same safe Markdown boundary for child conversations', () => {
    const state = createInitialState();
    render(<ChildConversation child={{ ...state.childAgents[0], messages: [
      { id: 'child-user', role: 'user', text: '<script>example</script>' },
      { id: 'child-assistant', role: 'assistant', markdown: '<img src="x" onerror="alert(1)">\n\n[Unsafe](javascript:alert%281%29)\n\n[Safe](https://example.com)' },
    ] }} />);
    const transcript = screen.getByRole('region', { name: /Child conversation:/ });
    expect(transcript.querySelector('script, img')).toBeNull();
    expect(screen.getByRole('article', { name: 'Your message' })).toHaveTextContent('<script>example</script>');
    expect(screen.getByText('Unsafe')).not.toHaveAttribute('href', expect.stringMatching(/^javascript:/));
    expect(screen.getByRole('link', { name: 'Safe' })).toHaveAttribute('href', 'https://example.com');
  });
});

describe('local composer context and presentation', () => {
  it('opens an ordinary keyboard disclosure, attaches only listed references, and restores focus on Escape/removal', async () => {
    const fileReader = vi.fn();
    const filePicker = vi.fn();
    vi.stubGlobal('FileReader', fileReader);
    vi.stubGlobal('showOpenFilePicker', filePicker);
    const user = start();
    const add = screen.getByRole('button', { name: 'Add context' });
    add.focus();
    await user.keyboard('{Enter}');
    expect(add).toHaveAttribute('aria-expanded', 'true');
    expect(add).toHaveAttribute('aria-controls', 'fixture-context');
    const file = screen.getByRole('button', { name: 'file SearchPage.tsx' });
    expect(file).toHaveFocus();
    expect(screen.getByRole('region', { name: 'Add context' })).toBeVisible();
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(document.querySelector('input[type="file"]')).toBeNull();
    await user.keyboard(' ');
    expect(file).toHaveAttribute('aria-pressed', 'true');
    expect(screen.getByRole('status')).toHaveTextContent('Context added to this chat.');
    expect(screen.getByRole('list', { name: 'Attached context' }).children).toHaveLength(1);
    await user.keyboard('{Enter}');
    expect(file).toHaveAttribute('aria-pressed', 'false');
    expect(screen.queryByRole('list', { name: 'Attached context' })).not.toBeInTheDocument();
    await user.keyboard('{Enter}');
    await user.tab();
    expect(screen.getByRole('button', { name: 'image result-layout.png' })).toHaveFocus();
    await user.keyboard('{Enter}');
    await user.tab();
    expect(screen.getByRole('button', { name: 'context Navigation notes' })).toHaveFocus();
    await user.keyboard(' ');
    expect(screen.getByRole('list', { name: 'Attached context' }).children).toHaveLength(3);
    await user.keyboard('{Escape}');
    expect(add).toHaveFocus();
    expect(add).toHaveAttribute('aria-expanded', 'false');
    expect(screen.queryByRole('region', { name: 'Add context' })).not.toBeInTheDocument();
    screen.getByRole('button', { name: 'Remove SearchPage.tsx context' }).focus();
    await user.keyboard('{Enter}');
    expect(add).toHaveFocus();
    expect(screen.getByRole('list', { name: 'Attached context' }).children).toHaveLength(2);
    expect(screen.getByRole('textbox')).toHaveValue(createInitialState().chats[0].draft);
    expect(fileReader).not.toHaveBeenCalled();
    expect(filePicker).not.toHaveBeenCalled();
  });

  it('isolates context per parent, closes disclosure when leaving the composer, and keeps attachment identities on return', async () => {
    const user = start();
    await user.click(screen.getByRole('button', { name: 'Add context' }));
    await user.click(screen.getByRole('button', { name: 'file SearchPage.tsx' }));
    await user.click(projects().getByRole('button', { name: 'Export date range Attention' }));
    expect(screen.getByRole('button', { name: 'Add context' })).toHaveAttribute('aria-expanded', 'false');
    expect(screen.queryByRole('list', { name: 'Attached context' })).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Add context' }));
    await user.click(screen.getByRole('button', { name: 'context Navigation notes' }));
    await user.click(projects().getByRole('button', { name: 'Remember search filters' }));
    const chips = screen.getByRole('list', { name: 'Attached context' });
    expect(chips).toHaveTextContent('SearchPage.tsx');
    expect(chips).not.toHaveTextContent('Navigation notes');
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(screen.queryByRole('list', { name: 'Attached context' })).not.toBeInTheDocument();
  });

  it('expands inline with Enter/Space, retains the same textarea and text, and collapses with Escape without sending', async () => {
    const user = start();
    const draft = screen.getByRole('textbox');
    const text = 'A longer draft.\nKeep these lines.\nAnd the caret-friendly input.';
    fireEvent.change(draft, { target: { value: text } });
    const expand = screen.getByRole('button', { name: 'Expand draft' });
    expand.focus();
    await user.keyboard('{Enter}');
    expect(expand).toHaveAttribute('aria-expanded', 'true');
    expect(expand).toHaveAccessibleName('Collapse draft');
    expect(expand).toHaveFocus();
    expect(screen.getByRole('textbox')).toBe(draft);
    expect(draft).toHaveValue(text);
    expect(screen.getByRole('form', { name: 'Message composer' })).toHaveAttribute('data-expanded', 'true');
    await user.keyboard(' ');
    expect(expand).toHaveAttribute('aria-expanded', 'false');
    await user.keyboard(' ');
    draft.focus();
    await user.keyboard('{Escape}');
    expect(expand).toHaveFocus();
    expect(expand).toHaveAttribute('aria-expanded', 'false');
    expect(screen.getByRole('textbox')).toBe(draft);
    expect(draft).toHaveValue(text);
    expect(screen.getAllByRole('article', { name: 'Your message' })).toHaveLength(1);
  });

  it('opens the shared native model dialog from a labeled button beside unique first-party currentColor marks', async () => {
    const user = start();
    const model = screen.getByRole('button', { name: 'Model for this chat' });
    const draft = screen.getByRole('textbox');
    const marks = new Set<string>();
    expect(model).toHaveAttribute('id', 'model-choice');
    expect(model).toHaveAttribute('aria-haspopup', 'dialog');
    for (const value of ['aster', 'aster-mini', 'orion'] as const) {
      await chooseMainModel(user, value);
      expect(model).toHaveFocus();
      const mark = model.parentElement!.querySelector('[data-model-mark]')!;
      expect(mark).toHaveAttribute('data-model-mark', value);
      expect(mark).toHaveAttribute('stroke', 'currentColor');
      expect(mark).toHaveAttribute('aria-hidden', 'true');
      expect(mark).toBe(model.parentElement!.firstElementChild);
      expect(mark.querySelector('image, foreignObject, use')).toBeNull();
      marks.add(mark.innerHTML);
      expect(screen.getByRole('textbox')).toBe(draft);
    }
    expect(marks.size).toBe(3);
    expect(screen.queryByRole('combobox', { name: 'Model for this chat' })).not.toBeInTheDocument();
    expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('build');
    expect(document.querySelector('img')).toBeNull();
  });
});
