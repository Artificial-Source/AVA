import { fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { ComposerView } from '../contracts';
import { ChatComposer } from './ChatComposer';
import { ContextInfo, type ContextInfoView } from './ContextInfo';

const view: ContextInfoView = {
  sessionTitle: 'Parent session', qualifier: 'Example prompt and local projection; not a provider payload.',
  usage: {
    profile: { id: 'sample', providerId: 'example', providerLabel: 'Example provider', modelId: 'sample', modelLabel: 'Sample', windowTokens: 214000 },
    snapshot: { profileId: 'sample', providerId: 'example', modelId: 'sample', freshness: 'current', activeTokens: 173120 },
  },
  cumulative: { input: 100, output: 20, reasoning: 8, cacheRead: 40, cacheWrite: 0, total: 120, cost: { usd: 0, completeness: 'complete' } },
  systemPrompt: '<script>invented text only</script>',
  messages: [{ id: 'one', role: 'user', origin: 'local', text: '<img src=x onerror=alert(1)>' }, { id: 'two', role: 'assistant', origin: 'example', text: 'A long line: ' + 'x'.repeat(2000) }],
};

function fact(label: string) {
  return screen.getByText(label, { selector: 'dt' }).nextElementSibling;
}

describe('controlled Context presentation', () => {
  it('adds only a decorative exact-ID provider mark without relabeling fictional providers or changing facts', () => {
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
    expect(fact('Window used')).toHaveTextContent('81%');
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
    expect(fact('User / assistant')).toHaveTextContent('1 / 1');
    await user.click(screen.getByRole('button', { name: 'System prompt' }));
    await user.click(screen.getByRole('button', { name: 'Raw messages' }));
    const prompt = screen.getByRole('region', { name: 'Example system prompt' });
    const raw = screen.getByRole('region', { name: 'Local committed messages' });
    expect(prompt.textContent).toBe(view.systemPrompt);
    expect(raw.tagName).toBe('PRE');
    expect(JSON.parse(raw.textContent!)).toEqual(view.messages);
    expect(container.querySelector('script, img, a, textarea, input')).toBeNull();
    expect(screen.queryByRole('button', { name: /export|download|copy|upload/i })).not.toBeInTheDocument();
    raw.focus();
    rerender(<ContextInfo view={{ ...view, messages: [...view.messages, { id: 'three', role: 'user', origin: 'local', text: 'Committed, not a draft' }] }} />);
    expect(screen.getByRole('region', { name: 'Local committed messages' })).toBe(raw);
    expect(raw).toHaveFocus();
    expect(fact('Committed')).toHaveTextContent(/^3$/);
    expect(fact('User / assistant')).toHaveTextContent('2 / 1');
    expect(JSON.parse(raw.textContent!)).toHaveLength(3);
  });

  it.each([['System prompt', 'Example system prompt'], ['Raw messages', 'Local committed messages']])('uses native Enter/Space/Tab and restores the %s trigger when Escape closes focused text', async (title, label) => {
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
    const raw = screen.getByRole('region', { name: 'Local committed messages' });
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

describe('optional controlled composer meter', () => {
  const composer: ComposerView = { title: 'Sample', draft: 'Draft', expanded: false, runState: 'idle', contextOptions: [], selectedContextIds: [], modelOptions: [{ id: 'sample', label: 'Sample' }], modelId: 'sample', modeOptions: [{ id: 'build', label: 'Build' }], modeId: 'build' };
  function callbacks() { return { onDraftChange: vi.fn(), onModelChange: vi.fn(), onModeChange: vi.fn(), onExpandedChange: vi.fn(), onAddReference: vi.fn(), onRemoveReference: vi.fn(), onSend: vi.fn(), onStop: vi.fn(), onOpenContext: vi.fn() }; }

  it('leaves existing consumers without usage unchanged, even if an open callback exists', () => {
    render(<ChatComposer view={composer} {...callbacks()} />);
    expect(screen.queryByRole('button', { name: /Open Context/ })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Send' })).toBeEnabled();
  });

  it('invalidates usage for an old composer selection instead of presenting it as the new model', () => {
    const cb = callbacks();
    const { rerender } = render(<ChatComposer view={{ ...composer, contextUsage: view.usage }} {...cb} />);
    expect(screen.getByRole('button', { name: /Open Context/ })).toHaveTextContent('173.12k (81%)');
    rerender(<ChatComposer view={{ ...composer, modelId: 'other', modelOptions: [{ id: 'other', label: 'Other model' }], contextUsage: view.usage }} {...cb} />);
    const meter = screen.getByRole('button', { name: /Open Context/ });
    expect(meter).toHaveTextContent('Context unknown');
    expect(meter).toHaveAttribute('title', 'Active context estimate: Unknown for selected model');
    expect(meter).not.toHaveTextContent('173.12');
  });

  it.each(['idle', 'running'] as const)('emits only explicit open intents from the native meter while %s, never Send or Stop', async (runState) => {
    const user = userEvent.setup();
    const cb = callbacks();
    render(<ChatComposer view={{ ...composer, runState, contextUsage: view.usage }} {...cb} contextTarget={{ id: 'context-page', open: true }} />);
    const form = screen.getByRole('form', { name: 'Message composer' });
    const meter = within(form).getByRole('button', { name: 'Open Context · 173.12k (81%)' });
    expect(meter).toHaveAttribute('type', 'button');
    expect(meter).toHaveAttribute('aria-controls', 'context-page');
    meter.focus();
    await user.keyboard('{Enter}');
    await user.keyboard(' ');
    expect(cb.onOpenContext).toHaveBeenCalledTimes(2);
    expect(cb.onSend).not.toHaveBeenCalled();
    expect(cb.onStop).not.toHaveBeenCalled();
    expect(screen.getByRole('textbox')).toHaveValue('Draft');
  });
});
