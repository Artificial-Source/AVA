import { describe, expect, it } from 'vitest';
import { formatUsageCost } from './widgets/ContextInfo';
import { contextUsage, formatContextMeter, formatTokenCount, type ContextUsageView } from './contextUsage';

const view: ContextUsageView = {
  profile: { id: 'profile', providerId: 'provider', providerLabel: 'Example provider', modelId: 'model', modelLabel: 'Example model', windowTokens: 214000 },
  snapshot: { profileId: 'profile', providerId: 'provider', modelId: 'model', freshness: 'current', activeTokens: 173120 },
};

describe('supplied active-context math', () => {
  it('formats the supplied estimate against the configured window, without an output reserve', () => {
    expect(contextUsage(view)).toMatchObject({ activeTokens: 173120, windowTokens: 214000, percentage: 81, reason: null });
    expect(formatContextMeter(view)).toBe('173.12k (81%)');
    expect(formatTokenCount(173120)).toBe('173,120');
  });

  it.each([[0, '0 (0%)'], [100, '100 (0%)'], [214000, '214k (100%)'], [256800, '256.8k (120%)']] as const)('preserves supplied %s tokens including zero and over-window percentages', (activeTokens, expected) => {
    expect(formatContextMeter({ ...view, snapshot: { ...view.snapshot!, activeTokens } })).toBe(expected);
  });

  it.each([null, undefined, -1, 0.5, NaN, Infinity, -Infinity, Number.MAX_SAFE_INTEGER + 1])('rejects invalid or missing token counts: %s', (value) => {
    const invalid = { ...view, snapshot: { ...view.snapshot!, activeTokens: value ?? null } };
    expect(contextUsage(invalid).activeTokens).toBeNull();
    expect(contextUsage(invalid).percentage).toBeNull();
    expect(formatContextMeter(invalid)).toBe('Context unknown');
    expect(formatTokenCount(value)).toBe('Unknown');
  });

  it.each([null, 0, -1, 0.5, NaN, Infinity, Number.MAX_SAFE_INTEGER + 1])('requires a known positive safe-integer window: %s', (windowTokens) => {
    const invalid = { ...view, profile: { ...view.profile, windowTokens } };
    expect(contextUsage(invalid).windowTokens).toBeNull();
    expect(contextUsage(invalid).percentage).toBeNull();
    expect(formatContextMeter(invalid)).toBe('Context unknown');
  });

  it('accepts the largest safe count without emitting nonfinite numbers', () => {
    const largest = { ...view, profile: { ...view.profile, windowTokens: Number.MAX_SAFE_INTEGER }, snapshot: { ...view.snapshot!, activeTokens: Number.MAX_SAFE_INTEGER } };
    expect(contextUsage(largest).percentage).toBe(100);
  });

  it.each(['profileId', 'providerId', 'modelId'] as const)('invalidates a mismatched snapshot %s instead of rescaling it', (field) => {
    const mismatch = { ...view, snapshot: { ...view.snapshot!, [field]: 'old-selection' } };
    expect(contextUsage(mismatch)).toMatchObject({ activeTokens: null, percentage: null, reason: 'Profile mismatch' });
    expect(formatContextMeter(mismatch)).toBe('Context unknown');
  });

  it('does not turn absent or stale measurements into zero, including after changing capacity', () => {
    expect(contextUsage({ ...view, snapshot: null })).toMatchObject({ activeTokens: null, percentage: null, reason: 'Unknown' });
    const stale = { ...view, profile: { ...view.profile, windowTokens: 128000 }, snapshot: { ...view.snapshot!, freshness: 'stale' as const } };
    expect(contextUsage(stale)).toMatchObject({ activeTokens: null, percentage: null, reason: 'Stale snapshot' });
    expect(formatContextMeter(stale)).toBe('Context unknown');
  });
});

describe('independent cumulative fields', () => {
  it('never fills an absent total and retains a supplied zero', () => {
    expect(formatTokenCount(undefined)).toBe('Unknown');
    expect(formatTokenCount(0)).toBe('0');
  });

  it('distinguishes complete, partial, unknown, missing and actual zero costs', () => {
    expect(formatUsageCost({ usd: 1.24, completeness: 'complete' })).toEqual({ amount: '$1.24', completeness: 'Complete' });
    expect(formatUsageCost({ usd: 0.18, completeness: 'partial' })).toEqual({ amount: '$0.18', completeness: 'Partial' });
    expect(formatUsageCost({ usd: 0, completeness: 'complete' })).toEqual({ amount: '$0.00', completeness: 'Complete' });
    for (const usd of [null, NaN, Infinity, -1]) expect(formatUsageCost({ usd, completeness: 'complete' })).toEqual({ amount: 'Unknown', completeness: 'Unknown' });
    expect(formatUsageCost({ usd: 0, completeness: 'unknown' })).toEqual({ amount: 'Unknown', completeness: 'Unknown' });
    expect(formatUsageCost(undefined)).toEqual({ amount: 'Unknown', completeness: 'Unknown' });
  });
});
