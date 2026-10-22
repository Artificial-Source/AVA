import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import { ContextInfo, type ContextInfoView } from './ContextInfo';

const view: ContextInfoView = {
  sessionTitle: 'Parent session',
  usage: {
    profile: { id: 'sample', providerId: 'example', providerLabel: 'Example provider', modelId: 'sample', modelLabel: 'Sample', windowTokens: 214000 },
    snapshot: { profileId: 'sample', providerId: 'example', modelId: 'sample', freshness: 'current', activeTokens: 173120 },
  },
  cumulative: { input: 100, output: 20, reasoning: 8, cacheRead: 40, cacheWrite: 0, total: 120, cost: { usd: 0, completeness: 'complete' } },
  systemPrompt: '<script>invented text only</script>',
  messages: [{ id: 'one', role: 'user', origin: 'local', text: '<img src=x onerror=alert(1)>' }, { id: 'two', role: 'assistant', origin: 'history', text: 'A long line: ' + 'x'.repeat(2000) }],
};

function fact(label: string) {
  return screen.getByText(label, { selector: 'dt' }).nextElementSibling;
}
function summary() {
  return screen.getByRole('region', { name: 'Window usage' });
}

describe('controlled Context presentation', () => {
  it('leads with window usage as words and a meter, then grouped facts', () => {
    const { container, rerender } = render(<ContextInfo view={view} />);
    expect(container.firstElementChild?.firstElementChild).toBe(summary());
    expect(summary()).toHaveTextContent('81% of window used');
    expect(summary()).toHaveTextContent('173,120 of 214,000 tokens');
    const meter = within(summary()).getByRole('meter', { name: 'Context window used' });
    expect(meter).toHaveAttribute('aria-valuenow', '173120');
    expect(meter).toHaveAttribute('aria-valuemax', '214000');
    expect(meter).toHaveAttribute('aria-valuetext', '81% of window used');
    expect(within(summary()).queryByText('Near the limit')).not.toBeInTheDocument();
    expect(screen.getAllByRole('heading', { level: 3 }).map((heading) => heading.textContent)).toEqual(['Window', 'Session usage', 'Messages']);
    // Over the window: the words keep the real share; the bar stops at full.
    rerender(<ContextInfo view={{ ...view, usage: { ...view.usage, snapshot: { ...view.usage.snapshot!, activeTokens: 240000 } } }} />);
    expect(summary()).toHaveTextContent('112% of window used');
    expect(within(summary()).getByText('Near the limit')).toBeVisible();
    expect(within(summary()).getByRole('meter').querySelector('span')).toHaveStyle({ width: '100%' });
    rerender(<ContextInfo view={{ ...view, usage: { ...view.usage, snapshot: null } }} />);
    expect(summary()).toHaveTextContent('Window use unknown');
    expect(summary()).toHaveTextContent('No usage snapshot for this model yet.');
    expect(within(summary()).queryByRole('meter')).not.toBeInTheDocument();
    expect(fact('Snapshot')).toHaveTextContent('Unknown');
  });

  it('adds only a decorative exact-ID provider mark without relabeling providers or changing facts', () => {
    const { rerender } = render(<ContextInfo view={view} />);
    expect(fact('Provider')).toHaveTextContent('Example provider');
    expect(fact('Provider')?.querySelector('[data-provider-mark]')).toHaveAttribute('data-provider-mark', 'neutral');
    for (const [providerId, providerLabel, mark] of [['aster-lab', 'Aster Lab', 'neutral'], ['orion-lab', 'Orion Lab', 'neutral'], ['constructor', 'Custom provider', 'neutral'], ['openai', 'Supplied OpenAI label', 'openai']]) {
      rerender(<ContextInfo view={{ ...view, usage: { ...view.usage, profile: { ...view.usage.profile, providerId, providerLabel } } }} />);
      expect(fact('Provider')).toHaveTextContent(providerLabel);
      expect(fact('Provider')?.querySelector('[data-provider-mark]')).toHaveAttribute('data-provider-mark', mark);
      expect(fact('Provider')?.querySelector('[data-provider-mark]')).toHaveAttribute('aria-hidden', 'true');
      expect(fact('Reported total')).toHaveTextContent(/^120$/);
    }
  });

  it('keeps active context separate from cumulative counters and never adds subsets to the total', () => {
    const { rerender } = render(<ContextInfo view={view} />);
    expect(fact('Active estimate')).toHaveTextContent('173,120');
    expect(fact('Reported total')).toHaveTextContent(/^120$/);
    expect(fact('Cache write')).toHaveTextContent(/^0$/);
    expect(fact('Cost estimate · USD')).toHaveTextContent('$0.00');
    expect(fact('Cost coverage')).toHaveTextContent('Complete');
    rerender(<ContextInfo view={{ ...view, cumulative: { ...view.cumulative, total: undefined, cost: undefined, reasoning: NaN, cacheWrite: -1 } }} />);
    for (const label of ['Reported total', 'Cost estimate · USD', 'Cost coverage', 'Reasoning tokens', 'Cache write']) expect(fact(label)).toHaveTextContent('Unknown');
    expect(fact('Input tokens')).toHaveTextContent(/^100$/);
  });

  it('serializes only supplied messages, derives counts, and escapes prompt and raw text without HTML or link interpretation', async () => {
    const user = userEvent.setup();
    const { container, rerender } = render(<ContextInfo view={view} />);
    expect(fact('Committed')).toHaveTextContent(/^2$/);
    expect(fact('You / AVA')).toHaveTextContent('1 / 1');
    await user.click(screen.getByRole('button', { name: 'System prompt' }));
    await user.click(screen.getByRole('button', { name: 'Raw messages' }));
    const prompt = screen.getByRole('region', { name: 'System prompt text' });
    const raw = screen.getByRole('region', { name: 'Raw message JSON' });
    expect(prompt.textContent).toBe(view.systemPrompt);
    expect(raw.tagName).toBe('PRE');
    expect(JSON.parse(raw.textContent!)).toEqual(view.messages);
    expect(container.querySelector('script, img, a, textarea, input')).toBeNull();
    expect(screen.queryByRole('button', { name: /export|download|copy|upload/i })).not.toBeInTheDocument();
    raw.focus();
    rerender(<ContextInfo view={{ ...view, messages: [...view.messages, { id: 'three', role: 'user', origin: 'local', text: 'Committed, not a draft' }] }} />);
    expect(screen.getByRole('region', { name: 'Raw message JSON' })).toBe(raw);
    expect(raw).toHaveFocus();
    expect(fact('Committed')).toHaveTextContent(/^3$/);
    expect(fact('You / AVA')).toHaveTextContent('2 / 1');
    expect(JSON.parse(raw.textContent!)).toHaveLength(3);
  });

  it.each([['System prompt', 'System prompt text'], ['Raw messages', 'Raw message JSON']])('uses native Enter/Space/Tab and restores the %s trigger when Escape closes focused text', async (title, label) => {
    const user = userEvent.setup();
    render(<ContextInfo view={view} />);
    const trigger = screen.getByRole('button', { name: title });
    trigger.focus();
    await user.keyboard('{Enter}');
    expect(trigger).toHaveFocus();
    const content = screen.getByRole('region', { name: label });
    expect(document.getElementById(trigger.getAttribute('aria-controls')!)).toContainElement(content);
    await user.tab();
    expect(content).toHaveFocus();
    fireEvent.keyDown(content, { key: 'Escape', isComposing: true });
    expect(trigger).toHaveAttribute('aria-expanded', 'true');
    await user.keyboard('{Escape}');
    expect(trigger).toHaveFocus();
    expect(trigger).toHaveAttribute('aria-expanded', 'false');
    expect(trigger).not.toHaveAttribute('aria-controls');
    await user.keyboard(' ');
    await user.tab();
    await user.tab();
    expect(content).not.toHaveFocus();
    expect(trigger).toHaveAttribute('aria-expanded', 'true');
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('does not change disclosure state or focus when the parent is logically inactive', () => {
    const { rerender } = render(<ContextInfo view={view} />);
    const trigger = screen.getByRole('button', { name: 'Raw messages' });
    fireEvent.click(trigger);
    const raw = screen.getByRole('region', { name: 'Raw message JSON' });
    raw.focus();
    const focus = vi.spyOn(trigger, 'focus');
    rerender(<ContextInfo view={view} active={false} />);
    fireEvent.click(trigger);
    fireEvent.keyDown(raw, { key: 'Escape' });
    expect(trigger).toHaveAttribute('aria-expanded', 'true');
    expect(focus).not.toHaveBeenCalled();
    focus.mockRestore();
  });
});
