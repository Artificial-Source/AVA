import { describe, expect, it } from 'vitest';
import { previewContextUsage } from '../components/contextPreview';
import { createInitialState } from '../state/fixtures';
import { contextProjection, defaultContextPolicy, defaultGeneral, defaultConversation, defaultNotifications, notificationDecision, boundedInteger } from './behaviorPreferences';

const usage = previewContextUsage(createInitialState().chats[0]);
describe('visit-local behavior defaults and pure policy projections', () => {
  it('preserves old chat behavior with explicit future-chat and notification defaults', () => {
    expect(defaultGeneral).toEqual({ mode: 'build', projectId: 'current', titleFromFirstMessage: false, inspector: 'remember' });
    expect(defaultConversation).toEqual({ expandTools: false, showCompletedOutput: true, wrapCode: false, wrapDiff: true, scrollAfterSend: true });
    expect(defaultNotifications.muted).toBe(false);
  });
  it('separates eligibility and proposed retention from supplied accounting', () => {
    const before = JSON.stringify(usage); Object.freeze(usage.profile); Object.freeze(usage.snapshot);
    expect(contextProjection(defaultContextPolicy, usage)).toMatchObject({ activeTokens: 173120, windowTokens: 214000, percentage: 81, threshold: 171200, eligible: true });
    expect(contextProjection({ ...defaultContextPolicy, triggerPercent: 95, recentTurns: 0, recentTokens: 0 }, usage)).toMatchObject({ activeTokens: 173120, threshold: 203300, eligible: false });
    expect(contextProjection({ ...defaultContextPolicy, autoCompact: false }, usage).eligible).toBe(false);
    expect(JSON.stringify(usage)).toBe(before);
  });
  it.each([null, { ...usage.snapshot!, freshness: 'stale' as const }, { ...usage.snapshot!, profileId: 'wrong' }, { ...usage.snapshot!, modelId: 'wrong' }, { ...usage.snapshot!, providerId: 'wrong' }, { ...usage.snapshot!, activeTokens: NaN }, { ...usage.snapshot!, activeTokens: -1 }])('keeps absent, stale and invalid snapshots unknown (%j)', (snapshot) => {
    expect(contextProjection(defaultContextPolicy, { ...usage, snapshot })).toMatchObject({ activeTokens: null, eligible: null });
  });
  it('preserves zero, over-window estimates, unknown windows and absent profiles', () => {
    expect(contextProjection(defaultContextPolicy, { ...usage, snapshot: { ...usage.snapshot!, activeTokens: 0 } })).toMatchObject({ activeTokens: 0, percentage: 0, eligible: false });
    expect(contextProjection(defaultContextPolicy, { ...usage, snapshot: { ...usage.snapshot!, activeTokens: 428000 } })).toMatchObject({ percentage: 200, eligible: true });
    expect(contextProjection(defaultContextPolicy, { ...usage, profile: { ...usage.profile, windowTokens: null } })).toMatchObject({ threshold: null, activeTokens: 173120, eligible: null });
    expect(contextProjection(defaultContextPolicy).eligible).toBeNull();
  });
  it.each([{ triggerPercent: 0 }, { triggerPercent: 96 }, { triggerPercent: 4.5 }, { recentTurns: -1 }, { recentTurns: 21 }, { recentTokens: 100001 }, { recentTokens: Infinity }])('rejects out-of-bounds policy without projecting fake values (%j)', (patch) => {
    expect(contextProjection({ ...defaultContextPolicy, ...patch }, usage)).toMatchObject({ eligible: null, threshold: null, valid: false });
  });
  it('uses exact integer floors, including safe large windows', () => {
    for (const windowTokens of [1, 99, 123, 214000, Number.MAX_SAFE_INTEGER]) {
      for (const triggerPercent of [1, 29, 80, 95]) {
        const expected = Number(BigInt(windowTokens) * BigInt(triggerPercent) / 100n);
        expect(contextProjection({ ...defaultContextPolicy, triggerPercent }, { ...usage, profile: { ...usage.profile, windowTokens } }).threshold).toBe(expected);
      }
    }
    expect(boundedInteger(0, 0, 100)).toBe(true); expect(boundedInteger(NaN, 0, 100)).toBe(false);
  });
  it.each(['completion', 'input', 'failure'] as const)('evaluates %s without sending events or playing sound', (event) => {
    expect(notificationDecision(defaultNotifications, event, true)).toMatchObject({ delivered: true, tone: event === 'completion' ? 'soft' : 'chime' });
    expect(notificationDecision(defaultNotifications, event, false)).toMatchObject({ delivered: false, tone: 'none' });
    expect(notificationDecision({ ...defaultNotifications, muted: true }, event, true)).toMatchObject({ delivered: false, reason: 'Master mute is on.' });
    expect(notificationDecision({ ...defaultNotifications, channels: { ...defaultNotifications.channels, [event]: { enabled: false, tone: 'soft' } } }, event, true).delivered).toBe(false);
    expect(notificationDecision({ ...defaultNotifications, backgroundOnly: false }, event, false).delivered).toBe(true);
  });
  it('keeps unknown tone values silent rather than treating them as audio instructions', () => {
    const channels = { ...defaultNotifications.channels, completion: { enabled: true, tone: 'unrecognized' as 'soft' } };
    expect(notificationDecision({ ...defaultNotifications, channels }, 'completion', true)).toMatchObject({ delivered: true, tone: 'none' });
  });
  it.each([0, NaN, Infinity, -1, 101, 2.5])('keeps zero/invalid volume silent (%s)', (volume) => {
    expect(notificationDecision({ ...defaultNotifications, volume }, 'completion', true)).toMatchObject({ delivered: true, tone: 'none' });
  });
});
