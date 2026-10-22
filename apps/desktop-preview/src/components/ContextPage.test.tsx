import { StrictMode } from 'react';
import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { chooseMainModel, installDialogTestSupport } from '../screens/dialogTestSupport';

import { AppShell } from './AppShell';

beforeEach(installDialogTestSupport);

function start() {
  const user = userEvent.setup();
  render(<StrictMode><AppShell /></StrictMode>);
  return user;
}
function rail() { return within(screen.getByRole('navigation', { name: 'Inspectors' })); }
function projects() { return within(screen.getByRole('navigation', { name: 'All project chats' })); }
function page() { return screen.getByRole('region', { name: 'Context information' }); }
function fact(label: string) { return within(page()).getByText(label, { selector: 'dt' }).nextElementSibling; }
function meter() { return screen.getByRole('button', { name: /^Open Context/ }); }

describe('dedicated parent Context page', () => {
  it('keeps visibility first and Agents/Changes ordering, with no shared details row on either module', async () => {
    const user = start();
    const toggle = rail().getByRole('button', { name: 'Hide inspector' });
    expect(rail().getAllByRole('button').map((button) => button.getAttribute('aria-label'))).toEqual(['Hide inspector', 'Agents', 'Changes', 'Context']);
    expect(screen.getByRole('navigation', { name: 'Inspectors' }).firstElementChild).toBe(toggle);
    expect(toggle).toHaveAttribute('title', 'Hide inspector · Agents');
    for (const module of ['Agents', 'Changes']) {
      if (module === 'Changes') await user.click(rail().getByRole('button', { name: module }));
      const panel = screen.getByRole('complementary', { name: module === 'Agents' ? 'Agents inspector' : 'Mock changes' });
      expect(panel.children).toHaveLength(1);
      expect(within(panel).queryByRole('button', { name: 'Context' })).not.toBeInTheDocument();
      expect(within(panel).queryByRole('button', { name: /^(\d+ changes|Child activity)/ })).not.toBeInTheDocument();
      expect(within(panel).queryByText('Wayfinder')).not.toBeInTheDocument();
      expect(within(panel).queryByRole('heading', { name: 'Remember search filters' })).not.toBeInTheDocument();
      expect(within(panel).queryByText('Provider')).not.toBeInTheDocument();
    }
    expect(screen.queryByRole('group', { name: 'Parent chat details' })).not.toBeInTheDocument();
  });

  it('explicitly opens Context from the compact composer meter, including repeated opens and closed-pane memory', async () => {
    const user = start();
    const control = meter();
    expect(control).toHaveTextContent('173.12k (81%)');
    expect(control).not.toHaveAttribute('aria-controls');
    expect(screen.getByRole('form', { name: 'Message composer' })).toContainElement(control);
    control.focus();
    await user.keyboard('{Enter}');
    await screen.findByRole('button', { name: 'Raw messages' });
    expect(control).toHaveFocus();
    expect(control).toHaveAttribute('aria-expanded', 'true');
    expect(control).toHaveAttribute('aria-controls', 'context-inspector');
    await user.keyboard(' ');
    expect(screen.getByRole('complementary', { name: 'Context inspector' })).toBeVisible();
    expect(rail().getByRole('button', { name: 'Context' })).toHaveAttribute('aria-controls', 'context-inspector');
    expect(screen.getByRole('heading', { name: 'Context', level: 2 })).toBeVisible();
    expect(within(page()).queryByRole('heading', { name: 'Remember search filters' })).not.toBeInTheDocument();
    expect(fact('Session')).toHaveTextContent('Remember search filters');
    expect(fact('Provider')).toHaveTextContent('Aster Lab');
    expect(fact('Configured window')).toHaveTextContent('214,000');
    expect(fact('Active estimate')).toHaveTextContent('173,120');
    expect(fact('Window used')).toHaveTextContent('81%');
    expect(fact('Input tokens')).toHaveTextContent('256,400');
    expect(fact('Reported total')).toHaveTextContent('275,320');
    expect(fact('Cost coverage')).toHaveTextContent('Complete');
    expect(page()).toHaveTextContent('not a sent prompt or provider payload');
    expect(screen.queryByRole('article', { name: 'Your local preview message' })).not.toBeInTheDocument();
    await user.click(rail().getByRole('button', { name: 'Context' }));
    expect(control).toHaveAttribute('aria-expanded', 'false');
    expect(control).not.toHaveAttribute('aria-controls');
    await user.click(control);
    expect(page()).toBeVisible();
    const toggle = rail().getByRole('button', { name: 'Hide inspector' });
    toggle.focus();
    await user.keyboard(' ');
    expect(toggle).toHaveFocus();
    expect(toggle).toHaveAttribute('title', 'Show inspector · Context');
    await user.keyboard('{Enter}');
    expect(toggle).toHaveFocus();
    expect(toggle).toHaveAttribute('aria-controls', 'context-inspector');
    expect(page()).toBeVisible();
  });

  it.each(['Close', 'Escape'])('returns immediate focus to the Context rail button on %s and remembers the page', async (action) => {
    const user = start();
    await user.click(meter());
    await screen.findByRole('button', { name: 'Raw messages' });
    const context = rail().getByRole('button', { name: 'Context' });
    if (action === 'Close') await user.click(screen.getByRole('button', { name: 'Close context' }));
    else {
      page().focus();
      fireEvent.keyDown(page(), { key: 'Escape', isComposing: true });
      expect(context).toHaveAttribute('aria-expanded', 'true');
      await user.keyboard('{Escape}');
    }
    expect(context).toHaveFocus();
    expect(context).toHaveAttribute('aria-expanded', 'false');
    expect(context).not.toHaveAttribute('aria-controls');
    expect(screen.queryByRole('complementary', { name: 'Context inspector' })).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Show inspector' }));
    expect(page()).toBeVisible();
  });

  it('closes only focused raw text on first Escape, then closes the pane on the second', async () => {
    const user = start();
    await user.click(meter());
    await screen.findByRole('button', { name: 'Raw messages' });
    const trigger = screen.getByRole('button', { name: 'Raw messages' });
    await user.click(trigger);
    await user.tab();
    expect(screen.getByRole('region', { name: 'Local committed messages' })).toHaveFocus();
    await user.keyboard('{Escape}');
    expect(trigger).toHaveFocus();
    expect(trigger).toHaveAttribute('aria-expanded', 'false');
    expect(page()).toBeVisible();
    await user.keyboard('{Escape}');
    expect(rail().getByRole('button', { name: 'Context' })).toHaveFocus();
    expect(screen.queryByRole('region', { name: 'Context information' })).not.toBeInTheDocument();
  });

  it('changes model to its own capacity and fixed snapshot or unknown, without remounting disclosures or stealing focus', async () => {
    const user = start();
    await user.click(meter());
    await screen.findByRole('button', { name: 'Raw messages' });
    await user.click(screen.getByRole('button', { name: 'Raw messages' }));
    const raw = screen.getByRole('region', { name: 'Local committed messages' });
    const oldPage = page();
    const model = screen.getByRole('button', { name: 'Model for this chat' });
    await chooseMainModel(user, 'aster-mini');
    expect(model).toHaveFocus();
    expect(page()).toBe(oldPage);
    expect(screen.getByRole('region', { name: 'Local committed messages' })).toBe(raw);
    expect(fact('Model')).toHaveTextContent('Aster Mini');
    expect(fact('Configured window')).toHaveTextContent('128,000');
    expect(fact('Active estimate')).toHaveTextContent('48,160');
    expect(meter()).toHaveTextContent('48.16k (38%)');
    expect(fact('Reported total')).toHaveTextContent('275,320');
    await chooseMainModel(user, 'orion');
    expect(fact('Provider')).toHaveTextContent('Orion Lab');
    expect(fact('Model')).toHaveTextContent('Orion');
    expect(fact('Configured window')).toHaveTextContent('96,000');
    expect(fact('Active estimate')).toHaveTextContent('Unknown');
    expect(fact('Window used')).toHaveTextContent('Unknown');
    expect(meter()).toHaveTextContent('Context unknown');
    expect(page()).not.toHaveTextContent('173,120');
    await chooseMainModel(user, 'aster');
    expect(meter()).toHaveTextContent('173.12k (81%)');
    expect(screen.getByRole('region', { name: 'Local committed messages' })).toBe(raw);
  });

  it('keeps typing, reference changes, modes and Send separate from fixed token/cost snapshots while projecting actual committed messages', async () => {
    const user = start();
    await user.click(meter());
    await screen.findByRole('button', { name: 'Raw messages' });
    await user.click(screen.getByRole('button', { name: 'Raw messages' }));
    const raw = screen.getByRole('region', { name: 'Local committed messages' });
    const before = raw.textContent;
    const draft = screen.getByRole('textbox');
    const localText = '<script>plain local message</script>';
    fireEvent.change(draft, { target: { value: localText } });
    await user.type(draft, ' More.');
    expect(draft).toHaveFocus();
    expect(raw.textContent).toBe(before);
    await user.selectOptions(screen.getByRole('combobox', { name: 'Mode for this chat' }), 'plan');
    await user.click(screen.getByRole('button', { name: 'Expand draft' }));
    await user.click(screen.getByRole('button', { name: 'Add example context' }));
    for (const name of ['file SearchPage.tsx', 'image result-layout.png', 'context Navigation notes']) await user.click(screen.getByRole('button', { name }));
    expect(screen.getByRole('list', { name: 'Attached example context' }).children).toHaveLength(3);
    expect(raw.textContent).toBe(before);
    await user.keyboard('{Escape}');
    await user.click(screen.getByRole('button', { name: 'Send' }));
    expect(screen.getByRole('region', { name: 'Local committed messages' })).toBe(raw);
    expect(JSON.parse(raw.textContent!)).toHaveLength(3);
    expect(raw).toHaveTextContent(localText + ' More.');
    expect(raw.querySelector('script')).toBeNull();
    expect(fact('Committed')).toHaveTextContent(/^3$/);
    expect(fact('User / assistant')).toHaveTextContent('2 / 1');
    expect(meter()).toHaveTextContent('173.12k (81%)');
    expect(fact('Cost estimate · USD')).toHaveTextContent('$1.24');
    expect(fact('Reported total')).toHaveTextContent('275,320');
    expect(screen.getByRole('list', { name: 'Attached example context' }).children).toHaveLength(3);
    expect(screen.getByRole('button', { name: 'Collapse draft' })).toBeVisible();
  });

  it('resets old parent disclosures on navigation and New, with unknown usage rather than fabricated zero', async () => {
    const user = start();
    await user.click(meter());
    await screen.findByRole('button', { name: 'Raw messages' });
    await user.click(screen.getByRole('button', { name: 'System prompt' }));
    await user.click(screen.getByRole('button', { name: 'Raw messages' }));
    const oldRaw = screen.getByRole('region', { name: 'Local committed messages' });
    await user.click(projects().getByRole('button', { name: 'Attention Export date range' }));
    expect(fact('Session')).toHaveTextContent('Export date range');
    expect(fact('Model')).toHaveTextContent('Orion');
    expect(fact('Committed')).toHaveTextContent(/^2$/);
    expect(oldRaw).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Raw messages' })).toHaveAttribute('aria-expanded', 'false');
    expect(screen.getByRole('button', { name: 'System prompt' })).toHaveAttribute('aria-expanded', 'false');
    expect(fact('Cost estimate · USD')).toHaveTextContent('Unknown');
    await user.click(screen.getByRole('button', { name: 'New chat' }));
    expect(fact('Session')).toHaveTextContent('New chat 1');
    expect(fact('Committed')).toHaveTextContent(/^0$/);
    expect(fact('User / assistant')).toHaveTextContent('0 / 0');
    expect(fact('Active estimate')).toHaveTextContent('Unknown');
    expect(fact('Reported total')).toHaveTextContent('Unknown');
    expect(meter()).toHaveTextContent('Context unknown');
    await user.click(screen.getByRole('button', { name: 'Raw messages' }));
    expect(screen.getByRole('region', { name: 'Local committed messages' }).textContent).toBe('[]');
    await user.click(projects().getByRole('button', { name: 'Remember search filters' }));
    expect(screen.getByRole('button', { name: 'Raw messages' })).toHaveAttribute('aria-expanded', 'false');
    expect(fact('Committed')).toHaveTextContent(/^2$/);
    expect(meter()).toHaveTextContent('173.12k (81%)');
  });

  it('keeps Context provider/model/counts/raw projection parent-owned during readonly child inspection and Back', async () => {
    const user = start();
    await user.click(screen.getByRole('button', { name: 'Send' }));
    await user.click(screen.getByRole('button', { name: /^Completed Map return paths/ }));
    const title = screen.getByRole('heading', { level: 1 });
    expect(title).toHaveFocus();
    await user.click(rail().getByRole('button', { name: 'Context' }));
    await screen.findByRole('button', { name: 'Raw messages' });
    expect(title).toHaveTextContent('Map return paths');
    expect(screen.getByRole('region', { name: 'Read-only child context' })).toHaveTextContent('Aster Mini');
    expect(fact('Session')).toHaveTextContent('Remember search filters');
    expect(fact('Model')).toHaveTextContent(/^Aster$/);
    expect(fact('Committed')).toHaveTextContent(/^3$/);
    await user.click(screen.getByRole('button', { name: 'Raw messages' }));
    const raw = screen.getByRole('region', { name: 'Local committed messages' });
    expect(JSON.parse(raw.textContent!)).toHaveLength(3);
    expect(raw).not.toHaveTextContent('Outline the paths back');
    expect(screen.queryByRole('form', { name: 'Message composer' })).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: /^(Send|Stop mock work|Steer|Restart|Promote)$/ })).not.toBeInTheDocument();
    expect(screen.getAllByRole('button', { name: 'Back to parent' })).toHaveLength(1);
    await user.click(screen.getByRole('button', { name: 'Back to parent' }));
    expect(title).toHaveFocus();
    expect(screen.getByRole('heading', { level: 1 })).toBe(title);
    expect(screen.getByRole('region', { name: 'Local committed messages' })).toBe(raw);
  });

  it('keeps parent draft/settings/pins/recent/Stop and child selection intact without network or filesystem work', async () => {
    const spies = ['fetch', 'XMLHttpRequest', 'WebSocket', 'FileReader', 'showOpenFilePicker'].map((name) => {
      const spy = vi.fn(); vi.stubGlobal(name, spy); return spy;
    });
    const user = start();
    await user.click(projects().getByRole('button', { name: 'Working Keyboard navigation' }));
    fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Keep parent draft' } });
    await user.selectOptions(screen.getByRole('combobox', { name: 'Mode for this chat' }), 'plan');
    await user.click(screen.getByRole('button', { name: 'Expand draft' }));
    await user.click(screen.getByRole('button', { name: 'Add example context' }));
    await user.click(screen.getByRole('button', { name: 'file SearchPage.tsx' }));
    await user.keyboard('{Escape}');
    await user.click(projects().getByRole('button', { name: 'Pin Keyboard navigation' }));
    const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const before = sidebar.textContent;
    await user.click(meter());
    await screen.findByRole('button', { name: 'Raw messages' });
    expect(fact('Cost coverage')).toHaveTextContent('Partial');
    expect(fact('Cache write')).toHaveTextContent(/^0$/);
    expect(screen.getByRole('button', { name: 'Stop mock work' })).toBeVisible();
    await user.click(meter());
    await screen.findByRole('button', { name: 'Raw messages' });
    expect(projects().getByRole('button', { name: 'Working Keyboard navigation' })).toBeVisible();
    await user.click(rail().getByRole('button', { name: 'Agents' }));
    await user.click(screen.getByRole('button', { name: /^Working Review focus boundaries/ }));
    const transcript = await screen.findByRole('region', { name: /Child conversation:/ });
    for (const name of ['Context', 'Changes', 'Agents', 'Context', 'Hide inspector', 'Show inspector']) await user.click(rail().getByRole('button', { name }));
    expect(screen.getByRole('region', { name: /Child conversation:/ })).toBe(transcript);
    expect(fact('Model')).toHaveTextContent('Aster Mini');
    expect(sidebar.textContent).toBe(before);
    await user.click(screen.getByRole('button', { name: 'Back to parent' }));
    expect(screen.getByRole('textbox')).toHaveValue('Keep parent draft');
    expect(screen.getByRole('combobox', { name: 'Mode for this chat' })).toHaveValue('plan');
    expect(screen.getByRole('button', { name: 'Collapse draft' })).toBeVisible();
    expect(screen.getByRole('list', { name: 'Attached example context' })).toHaveTextContent('SearchPage.tsx');
    expect(screen.getByRole('button', { name: 'Stop mock work' })).toBeVisible();
    expect(screen.queryByRole('article', { name: 'Your local preview message' })).not.toBeInTheDocument();
    for (const spy of spies) expect(spy).not.toHaveBeenCalled();
  });
});
