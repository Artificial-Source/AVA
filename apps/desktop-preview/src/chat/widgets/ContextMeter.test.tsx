import { render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { ContextUsageView } from '../contextUsage';
import type { ComposerView } from '../contracts';
import { ChatComposer } from './ChatComposer';
import { ContextMeter } from './ContextMeter';

const usage: ContextUsageView = {
  profile: { id: 'sample', providerId: 'example', providerLabel: 'Example provider', modelId: 'sample', modelLabel: 'Sample', windowTokens: 214000 },
  snapshot: { profileId: 'sample', providerId: 'example', modelId: 'sample', freshness: 'current', activeTokens: 173120 },
};
const composer: ComposerView = { title: 'Sample', draft: 'Draft', expanded: false, runState: 'idle', contextOptions: [], selectedContextIds: [], modelOptions: [{ id: 'sample', label: 'Sample' }], modelId: 'sample', modeOptions: [{ id: 'build', label: 'Build' }], modeId: 'build' };
function callbacks() { return { onDraftChange: vi.fn(), onModelChange: vi.fn(), onModeChange: vi.fn(), onExpandedChange: vi.fn(), onAddReference: vi.fn(), onRemoveReference: vi.fn(), onSend: vi.fn(), onStop: vi.fn(), onOpenContext: vi.fn() }; }
function withTokens(activeTokens: number): ContextUsageView { return { ...usage, snapshot: { ...usage.snapshot!, activeTokens } }; }

describe('composer context meter', () => {
  it('leaves existing consumers without usage unchanged, even if an open callback exists', () => {
    render(<ChatComposer view={composer} {...callbacks()} />);
    expect(screen.queryByRole('button', { name: /Open Context/ })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Send' })).toBeEnabled();
  });

  it('shows tokens below 80%, leads with the share used from 80%, and adds the Attention look only from 90%', () => {
    const { rerender } = render(<ContextMeter view={withTokens(139100)} onOpen={vi.fn()} />);
    const meter = screen.getByRole('button', { name: 'Open Context · 139.1k (65%)' });
    expect(meter).toHaveAttribute('data-level', 'normal');
    rerender(<ContextMeter view={usage} onOpen={vi.fn()} />);
    expect(meter).toHaveAccessibleName('Open Context · 81% used');
    expect(meter).toHaveTextContent(/^81% used$/);
    expect(meter).toHaveAttribute('data-level', 'high');
    expect(meter.querySelector('svg')).toBeNull();
    rerender(<ContextMeter view={withTokens(196880)} onOpen={vi.fn()} />);
    expect(meter).toHaveAccessibleName('Open Context · 92% used');
    expect(meter).toHaveAttribute('data-level', 'critical');
    // Attention is never color alone: the warning glyph rides along, hidden from the name.
    expect(meter.querySelector('svg')).toHaveAttribute('aria-hidden', 'true');
    expect(meter).toHaveAttribute('title', '196,880 of 214,000 tokens in context · Example provider / Sample');
  });

  it('invalidates usage for an old composer selection instead of presenting it as the new model', () => {
    const cb = callbacks();
    const { rerender } = render(<ChatComposer view={{ ...composer, contextUsage: usage }} {...cb} />);
    expect(screen.getByRole('button', { name: /Open Context/ })).toHaveTextContent('81% used');
    rerender(<ChatComposer view={{ ...composer, modelId: 'other', modelOptions: [{ id: 'other', label: 'Other model' }], contextUsage: usage }} {...cb} />);
    const meter = screen.getByRole('button', { name: /Open Context/ });
    expect(meter).toHaveTextContent('Context unknown');
    expect(meter).toHaveAttribute('data-level', 'unknown');
    expect(meter).toHaveAttribute('title', 'Context usage isn’t available for this model');
    expect(meter).not.toHaveTextContent('81');
  });

  it.each(['idle', 'running'] as const)('emits only explicit open requests from the native meter while %s, never Send or Stop', async (runState) => {
    const user = userEvent.setup();
    const cb = callbacks();
    render(<ChatComposer view={{ ...composer, runState, contextUsage: usage }} {...cb} contextTarget={{ id: 'context-page', open: true }} />);
    const form = screen.getByRole('form', { name: 'Message composer' });
    const meter = within(form).getByRole('button', { name: 'Open Context · 81% used' });
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
