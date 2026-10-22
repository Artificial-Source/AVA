import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { AppShell } from '../components/AppShell';
import ScreenReview from './ScreenReview';
import { chooseMainModel, installDialogTestSupport } from './dialogTestSupport';

beforeEach(installDialogTestSupport);
async function appearance(user: ReturnType<typeof userEvent.setup>) {
  await user.click(screen.getByRole('button', { name: 'Settings' }));
  const nav = await screen.findByRole('navigation', { name: 'Settings sections' });
  await user.click(within(nav).getByRole('button', { name: 'Appearance' }));
  return screen.getByRole('region', { name: 'Settings page' });
}
const choices = [
  ['Interface font', 'system', 'ui-font'], ['Conversation text size', 'large', 'prose-size'],
  ['Conversation line spacing', 'relaxed', 'prose-spacing'], ['Reading width', 'narrow', 'reading-width'],
  ['Contrast', 'higher', 'contrast'], ['App motion', 'reduced', 'motion'],
] as const;

describe('actual app appearance ownership', () => {
  it('applies every token at the app root immediately, synchronizes both material controls and retains choices across Back/reopen', async () => {
    const spies = ['fetch', 'XMLHttpRequest', 'WebSocket', 'FileReader', 'open'].map((name) => { const spy = vi.fn(); vi.stubGlobal(name, spy); return spy; });
    const user = userEvent.setup(); render(<AppShell />);
    const app = screen.getByRole('main'); const draft = screen.getByRole('textbox');
    const sidebar = screen.getByRole('complementary', { name: 'Projects and chats' });
    const opaque = screen.getByRole('button', { name: 'Opaque materials' }); const opener = screen.getByRole('button', { name: 'Settings' });
    await user.click(opaque); expect(app).toHaveAttribute('data-material', 'opaque');
    await appearance(user); expect(screen.getByLabelText('Sidebar material')).toHaveValue('opaque');
    fireEvent.change(screen.getByLabelText('Sidebar material'), { target: { value: 'glass' } }); expect(opaque).toHaveAttribute('aria-pressed', 'false');
    fireEvent.click(opaque); expect(app).toHaveAttribute('data-material', 'glass');
    fireEvent.click(screen.getByRole('radio', { name: 'Charcoal' })); expect(app).toHaveAttribute('data-palette', 'charcoal');
    fireEvent.click(screen.getByRole('radio', { name: 'Midnight' })); expect(app).toHaveAttribute('data-palette', 'midnight');
    for (const [label, value, attribute] of choices) { fireEvent.change(screen.getByLabelText(label), { target: { value: value } }); expect(app).toHaveAttribute(`data-${attribute}`, value); }
    fireEvent.change(screen.getByLabelText('Settings-row density'), { target: { value: 'compact' } });
    fireEvent.change(screen.getByLabelText('Sidebar material'), { target: { value: 'opaque' } }); expect(app).toHaveAttribute('data-material', 'opaque');
    await user.click(screen.getByRole('button', { name: 'Back to chat' }));
    expect(opener).toHaveFocus(); expect(screen.getByRole('textbox')).toBe(draft); expect(sidebar).toBeVisible();
    expect(opaque).toHaveAttribute('aria-pressed', 'true');
    for (const [, value, attribute] of choices) expect(app).toHaveAttribute(`data-${attribute}`, value);
    await user.click(opaque); await appearance(user);
    expect(screen.getByLabelText('Sidebar material')).toHaveValue('glass'); expect(screen.getByLabelText('Settings-row density')).toHaveValue('compact');
    expect(screen.getByRole('radio', { name: 'Midnight' })).toBeChecked();
    for (const [label, value] of choices) expect(screen.getByLabelText(label)).toHaveValue(value);
    for (const spy of spies) expect(spy).not.toHaveBeenCalled();
  });
  it('resets appearance only, preserving parent/child/default models, Enter behavior, definitions and parent work', async () => {
    const user = userEvent.setup(); render(<AppShell />); await chooseMainModel(user, 'orion');
    const original = screen.getByRole('region', { name: 'Conversation: Remember search filters' }).textContent;
    fireEvent.change(screen.getByRole('textbox'), { target: { value: 'Preserve appearance-independent work' } });
    fireEvent.change(screen.getByLabelText('Mode for this chat'), { target: { value: 'plan' } });
    await user.click(screen.getByRole('button', { name: 'Expand draft' }));
    await user.click(screen.getByRole('button', { name: 'Add example context' }));
    await user.click(screen.getByRole('button', { name: 'image result-layout.png' })); await user.keyboard('{Escape}');
    const library = within(screen.getByRole('navigation', { name: 'All project chats' }));
    await user.click(library.getByRole('button', { name: 'Pin Remember search filters' })); await user.click(library.getByRole('button', { name: 'Relay' }));
    await user.click(screen.getByRole('button', { name: /^Completed Map return paths/ }));
    const child = await screen.findByRole('region', { name: /Child conversation:/ }); const childText = child.textContent;
    await appearance(user);
    await user.click(screen.getByRole('button', { name: 'General' })); fireEvent.change(screen.getByLabelText('Enter key behavior'), { target: { value: 'newline' } });
    await user.click(screen.getByRole('button', { name: 'Models' })); expect(screen.getByRole('button', { name: 'Choose current model' })).toBeDisabled();
    await user.click(screen.getByRole('button', { name: 'Choose default model' }));
    const dialog = screen.getByRole('dialog'); await user.click(within(dialog).getByRole('radio', { name: /Aster Mini.*128,000/ })); await user.click(within(dialog).getByRole('button', { name: 'Use Aster Mini' }));
    await user.click(screen.getByRole('button', { name: 'Subagents' })); await user.click(screen.getByRole('button', { name: 'Edit Notes reviewer' }));
    fireEvent.change(screen.getByRole('textbox', { name: 'Description' }), { target: { value: 'Keep this local definition' } });
    fireEvent.change(screen.getByLabelText('Model preference'), { target: { value: 'aster-mini' } }); await user.click(screen.getByRole('button', { name: 'Save definition' }));
    await user.click(screen.getByRole('button', { name: 'Appearance' }));
    fireEvent.click(screen.getByRole('radio', { name: 'Charcoal' }));
    for (const [label, value] of choices) fireEvent.change(screen.getByLabelText(label), { target: { value: value } });
    fireEvent.change(screen.getByLabelText('Settings-row density'), { target: { value: 'compact' } }); fireEvent.change(screen.getByLabelText('Sidebar material'), { target: { value: 'opaque' } });
    await user.click(screen.getByRole('button', { name: 'Reset appearance' }));
    const app = screen.getByRole('main'); expect(app).toHaveAttribute('data-palette', 'default'); expect(app).toHaveAttribute('data-material', 'glass'); expect(app).toHaveAttribute('data-motion', 'system');
    expect(screen.getByLabelText('Interface font')).toHaveValue('geist'); expect(screen.getByLabelText('Conversation text size')).toHaveValue('normal'); expect(screen.getByLabelText('Conversation line spacing')).toHaveValue('normal'); expect(screen.getByLabelText('Reading width')).toHaveValue('wide'); expect(screen.getByLabelText('Contrast')).toHaveValue('system'); expect(screen.getByLabelText('Settings-row density')).toHaveValue('comfortable');
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Models' })); expect(screen.getByText('Default: Aster Mini')).toBeVisible(); expect(screen.getByText(/Selected: Orion/)).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'General' })); expect(screen.getByLabelText('Enter key behavior')).toHaveValue('newline');
    await user.click(screen.getByRole('button', { name: 'Subagents' })); expect(screen.getByText('Keep this local definition')).toBeVisible();
    await user.click(screen.getByRole('button', { name: 'Back to chat' }));
    expect(screen.getByRole('region', { name: /Child conversation:/ })).toBe(child); expect(child.textContent).toBe(childText);
    expect(screen.queryByRole('textbox')).not.toBeInTheDocument();
    await user.click(screen.getByRole('button', { name: 'Back to parent' }));
    expect(screen.getByRole('textbox')).toHaveValue('Preserve appearance-independent work'); expect(screen.getByLabelText('Mode for this chat')).toHaveValue('plan');
    expect(screen.getByRole('button', { name: 'Collapse draft' })).toBeVisible(); expect(screen.getByRole('list', { name: 'Attached example context' })).toHaveTextContent('result-layout.png');
    expect(screen.getByRole('region', { name: 'Conversation: Remember search filters' }).textContent).toBe(original);
    expect(library.getByRole('button', { name: 'Relay' })).toHaveAttribute('aria-expanded', 'false'); expect(library.getByRole('button', { name: 'Unpin Remember search filters' })).toBeVisible();
    expect(screen.getByRole('complementary', { name: 'Agents inspector' })).toBeVisible(); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Orion');
    await user.click(screen.getByRole('button', { name: 'New chat' })); expect(screen.getByRole('button', { name: 'Model for this chat' })).toHaveTextContent('Aster Mini');
  });
  it('rejects retained Appearance callbacks after leaving for a nested confirmation and restores all visuals on the existing confirmed reset', async () => {
    const user = userEvent.setup(); render(<AppShell />); await appearance(user);
    fireEvent.click(screen.getByRole('radio', { name: 'Midnight' }));
    for (const [label, value] of choices) fireEvent.change(screen.getByLabelText(label), { target: { value: value } });
    fireEvent.change(screen.getByLabelText('Sidebar material'), { target: { value: 'opaque' } });
    function callback(control: HTMLElement, event: 'onChange' | 'onClick') {
      const key = Object.keys(control).find((key) => key.startsWith('__reactProps'))!;
      return (control as unknown as Record<string, Record<string, (event?: unknown) => void>>)[key][event];
    }
    const material = callback(screen.getByLabelText('Sidebar material'), 'onChange'); const font = callback(screen.getByLabelText('Interface font'), 'onChange'); const reset = callback(screen.getByRole('button', { name: 'Reset appearance' }), 'onClick');
    await user.click(screen.getByRole('button', { name: 'General' }));
    await user.click(screen.getByRole('button', { name: 'Reset all preferences…' }));
    act(() => { material({ currentTarget: { value: 'glass' } }); font({ currentTarget: { value: 'geist' } }); reset(); });
    expect(screen.getByRole('main')).toHaveAttribute('data-material', 'opaque'); expect(screen.getByRole('main')).toHaveAttribute('data-ui-font', 'system'); expect(screen.getByRole('main')).toHaveAttribute('data-palette', 'midnight');
    await user.click(screen.getByRole('button', { name: 'Cancel' }));
    await user.click(screen.getByRole('button', { name: 'Appearance' })); expect(screen.getByLabelText('Sidebar material')).toHaveValue('opaque');
    await user.click(screen.getByRole('button', { name: 'General' }));
    await user.click(screen.getByRole('button', { name: 'Reset all preferences…' })); await user.click(screen.getByRole('button', { name: 'Reset preferences and default' }));
    expect(screen.getByRole('main')).toHaveAttribute('data-palette', 'default'); expect(screen.getByRole('main')).toHaveAttribute('data-material', 'glass'); expect(screen.getByRole('main')).toHaveAttribute('data-motion', 'system');
    await user.click(screen.getByRole('button', { name: 'Appearance' }));
    expect(screen.getByLabelText('Conversation text size')).toHaveValue('normal'); expect(screen.getByLabelText('Reading width')).toHaveValue('wide');
  });
  it('makes manual reduction remove actual sidebar and inspector Presence exits immediately, not just their animations', async () => {
    vi.stubGlobal('matchMedia', vi.fn(() => ({ matches: false, addEventListener: vi.fn(), removeEventListener: vi.fn() })));
    const user = userEvent.setup(); render(<AppShell />); await appearance(user);
    fireEvent.change(screen.getByLabelText('App motion'), { target: { value: 'reduced' } }); await user.click(screen.getByRole('button', { name: 'Back to chat' }));
    const chat = within(screen.getByRole('navigation', { name: 'All project chats' })).getByRole('button', { name: 'Working Keyboard navigation' });
    fireEvent.click(screen.getByRole('button', { name: 'Wayfinder' })); expect(chat).not.toBeInTheDocument();
    const panel = screen.getByRole('complementary', { name: 'Agents inspector' }); fireEvent.click(screen.getByRole('button', { name: 'Hide inspector' })); expect(panel).not.toBeInTheDocument();
    expect(document.querySelector('[data-presence="closing"]')).toBeNull();
  });
});

it('keeps standalone appearance independent, with its opaque baseline and retained preferences on Back', async () => {
  const user = userEvent.setup(); const review = render(<ScreenReview />);
  const main = screen.getByRole('main'); expect(main).toHaveAttribute('data-material', 'opaque');
  await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'Appearance' }));
  fireEvent.click(screen.getByRole('radio', { name: 'Midnight' })); fireEvent.change(screen.getByLabelText('Sidebar material'), { target: { value: 'glass' } });
  fireEvent.change(screen.getByLabelText('App motion'), { target: { value: 'reduced' } });
  expect(main).toHaveAttribute('data-palette', 'midnight'); expect(main).toHaveAttribute('data-material', 'glass'); expect(main).toHaveAttribute('data-motion', 'reduced');
  const sample = screen.getByRole('figure', { name: 'Live appearance preview' });
  expect(sample).toHaveTextContent('Sidebar & reading sample'); expect(sample.querySelector('.material')).not.toBeNull();
  await user.click(screen.getByRole('button', { name: 'Back' })); expect(main).toHaveAttribute('data-material', 'opaque');
  await user.click(screen.getByRole('button', { name: 'Back' })); await user.click(screen.getByRole('button', { name: 'Open settings' }));
  await user.click(within(screen.getByRole('navigation', { name: 'Settings sections' })).getByRole('button', { name: 'Appearance' }));
  expect(screen.getByRole('radio', { name: 'Midnight' })).toBeChecked(); expect(screen.getByLabelText('Sidebar material')).toHaveValue('glass');
  await user.click(screen.getByRole('button', { name: 'Reset appearance' })); expect(main).toHaveAttribute('data-material', 'opaque'); expect(main).toHaveAttribute('data-motion', 'system'); expect(main).toHaveAttribute('data-palette', 'default');
  review.unmount(); render(<AppShell />); expect(screen.getByRole('main')).toHaveAttribute('data-material', 'glass'); expect(screen.getByRole('main')).not.toHaveAttribute('data-palette', 'midnight');
});
