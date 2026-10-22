import { useState, type ComponentProps } from 'react';
import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { installDialogTestSupport } from './dialogTestSupport';
import { HelpDialog } from './HelpDialog';

beforeEach(installDialogTestSupport);
// jsdom has no scrollIntoView; cases that need it define one, and it goes back after each case.
const nativeScroll = Object.getOwnPropertyDescriptor(Element.prototype, 'scrollIntoView');
afterEach(() => {
  vi.restoreAllMocks();
  if (nativeScroll) Object.defineProperty(Element.prototype, 'scrollIntoView', nativeScroll);
  else delete (Element.prototype as Partial<Element>).scrollIntoView;
});

type Props = ComponentProps<typeof HelpDialog>;
const allDone = { providerConnected: true, projectOpened: true, firstMessageSent: true };
function Host(props: Partial<Props>) {
  const [query, setQuery] = useState(props.query ?? '');
  return <HelpDialog onDismiss={() => {}} gettingStarted={allDone} onConnectProvider={() => {}} onOpenProject={() => {}} {...props} query={query} onQueryChange={setQuery} />;
}
const help = () => screen.getByRole('dialog', { name: 'Help' });
const searchbox = () => within(help()).getByRole('searchbox', { name: 'Search help' });
const start = () => within(help()).getByRole('region', { name: 'Getting started' });
const keys = (group: string, action: string) => within(within(help()).getByRole('table', { name: group })).getByRole('rowheader', { name: action }).closest('tr');
const topics = ['Providers and API keys', 'Chats and projects', 'Build and Plan', 'Permissions', 'Changes', 'Subagents', 'Context', 'Tool details', 'Extensions', 'Appearance'];

describe('Help dialog', () => {
  it('starts in the search, above a folded Getting started, folded topics and every shortcut, with no actions it can’t carry out', () => {
    render(<Host />);
    expect(searchbox()).toHaveFocus();
    expect(within(help()).getAllByRole('heading', { level: 3 }).map((heading) => heading.textContent)).toEqual(['Getting started', 'How AVA works', 'Keyboard shortcuts']);
    expect(within(help()).getAllByRole('heading', { level: 4 }).map((heading) => heading.textContent)).toEqual(topics);
    for (const topic of topics) expect(within(help()).getByRole('button', { name: topic })).toHaveAttribute('aria-expanded', 'false');
    expect(within(help()).getAllByRole('table').map((table) => table.querySelector('caption')!.textContent)).toEqual(['General', 'Chat', 'Panels', 'Navigation']);
    expect(keys('Navigation', 'Open Help')).toHaveTextContent('F1');
    expect(keys('General', 'Open command palette')).toHaveTextContent('Ctrl+K');
    expect(within(help()).queryByRole('button', { name: 'Change shortcuts' })).not.toBeInTheDocument();
    expect(help()).not.toHaveTextContent(/preview|example/i);
  });

  it('lists the current keys: remapped, unassigned, and off while navigation keys stay on', () => {
    const view = render(<Host shortcuts={{ enabled: true, overrides: { newChat: 'Mod+Shift+.', stop: null, palette: 'Alt+X' } }} />);
    expect(keys('General', 'New chat')).toHaveTextContent('Ctrl+Shift+.');
    expect(keys('Chat', 'Stop the run')).toHaveTextContent('Unassigned');
    expect(keys('Panels', 'Show or hide the inspector')).toHaveTextContent('Unassigned');
    // A chord the app refuses to run is not presented as a working key.
    expect(keys('General', 'Open command palette')).toHaveTextContent('Unassigned');
    view.unmount();
    render(<Host shortcuts={{ enabled: false, overrides: {} }} />);
    expect(help()).toHaveTextContent('App shortcuts are off. The navigation keys still work.');
    expect(keys('Chat', 'Choose model')).toHaveTextContent('Ctrl+Shift+6');
  });

  it('lists the keys that send for the user’s Enter setting, and only the always-on chord without one', () => {
    const sends = () => Array.from(keys('Navigation', 'Send message')!.querySelectorAll('kbd'), (key) => key.textContent);
    let view = render(<Host enter="send" />);
    expect(sends()).toEqual(['Enter', 'Ctrl+Enter']);
    view.unmount();
    view = render(<Host enter="newline" />);
    expect(sends()).toEqual(['Ctrl+Enter']);
    view.unmount();
    render(<Host />);
    expect(sends()).toEqual(['Ctrl+Enter']);
  });

  it.each([
    ['permissions', () => within(help()).getByRole('button', { name: 'Permissions' }), (node: Element) => node.closest('li')],
    ['shortcuts', () => within(help()).getByRole('heading', { name: 'Keyboard shortcuts' }), (node: Element) => node.closest('section')],
    ['getting-started', () => within(help()).getByRole('heading', { name: 'Getting started' }), (node: Element) => node.closest('section')!.parentElement],
  ] as const)('opens on %s with focus on its heading or disclosure, scrolling only Help’s body so the title stays in view', (topic, target, landing) => {
    const scroll = vi.fn();
    Object.defineProperty(Element.prototype, 'scrollIntoView', { configurable: true, value: scroll });
    // jsdom has no layout: the dialog body scrolls (as DialogFrame's CSS makes it) and sits 100px down; the topic 700px.
    const style = document.createElement('style');
    style.textContent = 'dialog > div > div { overflow-y: auto; }';
    document.head.append(style);
    vi.spyOn(Element.prototype, 'getBoundingClientRect').mockImplementation(function (this: Element) {
      const top = this.matches('dialog > div > div') ? 100 : 700;
      return { top, bottom: top + 40, left: 0, right: 0, width: 0, height: 40, x: 0, y: top, toJSON: () => ({}) } as DOMRect;
    });
    try {
      render(<Host topic={topic} />);
      expect(target()).toHaveFocus();
      const body = help().querySelector<HTMLElement>(':scope > div > div')!;
      expect(body).toContainElement(landing(target()) as HTMLElement);
      expect(body.scrollTop).toBe(600);
      expect(help().scrollTop).toBe(0);
      expect(scroll).not.toHaveBeenCalled();
      if (topic === 'permissions') {
        expect(target()).toHaveAttribute('aria-expanded', 'true');
        expect(help()).toHaveTextContent('Allow once covers only that request');
        expect(within(help()).getByRole('button', { name: 'Context' })).toHaveAttribute('aria-expanded', 'false');
      }
      // Asked for by name, Getting started shows whole even when every step is done.
      if (topic === 'getting-started') expect(within(start()).getAllByRole('listitem')).toHaveLength(3);
    } finally {
      style.remove();
    }
  });

  it('folds a finished Getting started to one line, unfolds it on request, and shows it whole while it matches a search', async () => {
    const user = userEvent.setup();
    const view = render(<Host />);
    expect(start()).toHaveTextContent('You’re set up.');
    expect(within(start()).queryByRole('list')).not.toBeInTheDocument();
    const toggle = within(start()).getByRole('button', { name: 'Show steps' });
    expect(toggle).toHaveAttribute('aria-expanded', 'false');
    await user.click(toggle);
    expect(toggle).toHaveFocus(); expect(toggle).toHaveAccessibleName('Hide steps'); expect(toggle).toHaveAttribute('aria-expanded', 'true');
    expect(toggle).toHaveAttribute('aria-controls', within(start()).getByRole('list').id);
    expect(within(start()).getAllByRole('listitem')).toHaveLength(3);
    expect(within(start()).getAllByText('You’re set up.')).toHaveLength(1);
    await user.click(toggle);
    expect(within(start()).queryByRole('list')).not.toBeInTheDocument();
    await user.type(searchbox(), 'connect');
    expect(within(start()).getAllByRole('listitem')).toHaveLength(3);
    expect(within(start()).queryByRole('button', { name: /steps$/ })).not.toBeInTheDocument();
    view.unmount();
    // Steps left: always the whole checklist, never a toggle.
    render(<Host gettingStarted={{ providerConnected: true, projectOpened: false, firstMessageSent: false }} />);
    expect(within(start()).getAllByRole('listitem')).toHaveLength(3);
    expect(start()).toHaveTextContent('1 of 3 done');
    expect(within(start()).queryByRole('button', { name: /steps$/ })).not.toBeInTheDocument();
  });

  it('filters topics and shortcuts together, opens matching topics, says when nothing matches, and clears before closing', async () => {
    const onDismiss = vi.fn();
    const user = userEvent.setup(); render(<Host onDismiss={onDismiss} />);
    const search = searchbox();
    await user.type(search, 'compaction');
    expect(within(help()).getByRole('button', { name: 'Context' })).toHaveAttribute('aria-expanded', 'true');
    expect(within(help()).queryByRole('button', { name: 'Build and Plan' })).not.toBeInTheDocument();
    expect(within(help()).queryByRole('heading', { name: 'Getting started' })).not.toBeInTheDocument();
    // A topic folded during a search stays folded until the search changes.
    await user.click(within(help()).getByRole('button', { name: 'Context' }));
    expect(within(help()).getByRole('button', { name: 'Context' })).toHaveAttribute('aria-expanded', 'false');
    await user.clear(search); await user.type(search, 'inspector');
    expect(within(help()).getAllByRole('table').map((table) => table.querySelector('caption')!.textContent)).toEqual(['Panels']);
    expect(within(help()).getAllByRole('rowheader').map((cell) => cell.textContent)).toEqual(['Show or hide the inspector']);
    // Typed straight apostrophes find the typographic ones in the copy.
    await user.clear(search); await user.type(search, 'it\'s read only');
    expect(within(help()).getAllByRole('button', { expanded: true }).map((button) => button.textContent)).toEqual(['Subagents']);
    await user.clear(search); await user.type(search, 'zebra crossing');
    expect(within(help()).getByText('No topics or shortcuts match “zebra crossing”.')).toBeVisible();
    expect(within(help()).queryAllByRole('heading', { level: 3 })).toHaveLength(0);
    // Without next steps from the host, the message stands alone.
    expect(within(help()).queryByRole('button', { name: /^(Search Settings|Open Commands)/ })).not.toBeInTheDocument();
    await user.keyboard('{Escape}');
    expect(search).toHaveValue(''); expect(onDismiss).not.toHaveBeenCalled();
    expect(within(help()).getByRole('button', { name: 'Context' })).toHaveAttribute('aria-expanded', 'false');
    await user.keyboard('{Escape}');
    expect(onDismiss).toHaveBeenCalledTimes(1);
  });

  it.each([
    ['api key', 'Providers and API keys'],
    ['theme', 'Appearance'],
    ['dark mode', 'Appearance'],
    ['delete chat', 'Chats and projects'],
    ['mcp', 'Extensions'],
  ])('finds “%s” in %s', async (query, topic) => {
    const user = userEvent.setup(); render(<Host />);
    await user.type(searchbox(), query);
    expect(within(help()).getAllByRole('button', { expanded: true }).map((button) => button.textContent)).toEqual([topic]);
  });

  it('matches whole word starts, so “sign in” finds sign-in help and never an unassigned key', async () => {
    const user = userEvent.setup(); render(<Host shortcuts={{ enabled: true, overrides: { stop: null } }} />);
    expect(keys('Chat', 'Stop the run')).toHaveTextContent('Unassigned');
    await user.type(searchbox(), 'sign in');
    expect(within(help()).getByRole('heading', { name: 'Getting started' })).toBeVisible();
    expect(within(help()).getAllByRole('button', { expanded: true }).map((button) => button.textContent)).toEqual(['Providers and API keys']);
    expect(within(help()).queryByRole('table')).not.toBeInTheDocument();
    await user.clear(searchbox()); await user.type(searchbox(), 'unassigned');
    expect(within(help()).getByText('No topics or shortcuts match “unassigned”.')).toBeVisible();
  });

  it('offers the host’s next steps when nothing matches: Search Settings with the query, and Commands', async () => {
    const onSearchSettings = vi.fn(); const onOpenCommands = vi.fn();
    const user = userEvent.setup(); render(<Host onSearchSettings={onSearchSettings} onOpenCommands={onOpenCommands} />);
    expect(within(help()).queryByRole('button', { name: 'Open Commands' })).not.toBeInTheDocument();
    await user.type(searchbox(), '  zebra crossing ');
    expect(within(help()).getByRole('status')).toHaveTextContent('No topics or shortcuts match “zebra crossing”.');
    await user.click(within(help()).getByRole('button', { name: 'Search Settings for “zebra crossing”' }));
    expect(onSearchSettings).toHaveBeenCalledExactlyOnceWith('zebra crossing');
    await user.click(within(help()).getByRole('button', { name: 'Open Commands' }));
    expect(onOpenCommands).toHaveBeenCalledTimes(1);
    await user.clear(searchbox()); await user.type(searchbox(), 'context');
    expect(within(help()).queryByRole('button', { name: /^Search Settings/ })).not.toBeInTheDocument();
    expect(within(help()).queryByRole('button', { name: 'Open Commands' })).not.toBeInTheDocument();
  });

  it('offers Go to only where the host can open it, and Getting started actions for undone steps', async () => {
    const onGo = vi.fn(); const onConnectProvider = vi.fn(); const onOpenProject = vi.fn();
    const user = userEvent.setup();
    render(<Host onGo={onGo} canGo={(to) => to !== 'sessions'} gettingStarted={{ providerConnected: false, projectOpened: false, firstMessageSent: false }} onConnectProvider={onConnectProvider} onOpenProject={onOpenProject} />);
    await user.click(within(help()).getByRole('button', { name: 'Chats and projects' }));
    expect(within(help()).queryByRole('button', { name: 'Go to Find a chat' })).not.toBeInTheDocument();
    await user.click(within(help()).getByRole('button', { name: 'Changes' }));
    expect(within(help()).queryByRole('button', { name: /^Go to/ })).not.toBeInTheDocument();
    await user.click(within(help()).getByRole('button', { name: 'Permissions' }));
    await user.click(within(help()).getByRole('button', { name: 'Go to Permissions & trust' }));
    await user.click(within(help()).getByRole('button', { name: 'Providers and API keys' }));
    await user.click(within(help()).getByRole('button', { name: 'Go to Models & providers' }));
    await user.click(within(help()).getByRole('button', { name: 'Appearance' }));
    await user.click(within(help()).getByRole('button', { name: 'Go to Appearance' }));
    await user.click(within(help()).getByRole('button', { name: 'Extensions' }));
    await user.click(within(help()).getByRole('button', { name: 'Go to Extensions' }));
    await user.click(within(help()).getByRole('button', { name: 'Change shortcuts' }));
    expect(onGo.mock.calls).toEqual([['permissions'], ['models'], ['appearance'], ['extensions'], ['shortcuts']]);
    await user.click(within(help()).getByRole('button', { name: 'Connect a provider' }));
    await user.click(within(help()).getByRole('button', { name: 'Open a project' }));
    expect(onConnectProvider).toHaveBeenCalledTimes(1); expect(onOpenProject).toHaveBeenCalledTimes(1);
  });
});
