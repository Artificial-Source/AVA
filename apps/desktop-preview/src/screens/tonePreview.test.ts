import { afterEach, describe, expect, it, vi } from 'vitest';
import { startTonePreview } from './tonePreview';

function audio(state: AudioContextState = 'running') {
  const oscillator = { type: '', frequency: { setValueAtTime: vi.fn() }, connect: vi.fn(), disconnect: vi.fn(), start: vi.fn(), stop: vi.fn(), onended: null as (() => void) | null };
  const gain = { gain: { setValueAtTime: vi.fn(), linearRampToValueAtTime: vi.fn() }, connect: vi.fn(), disconnect: vi.fn() };
  let resume!: () => void; let reject!: () => void;
  const context = { state, currentTime: 10, destination: {}, createOscillator: vi.fn(() => oscillator), createGain: vi.fn(() => gain), resume: vi.fn(() => new Promise<void>((resolve, fail) => { resume = () => { context.state = 'running'; resolve(); }; reject = () => fail(new Error('blocked')); })), close: vi.fn(async () => { context.state = 'closed'; }) };
  const create = vi.fn(function () { return context; }); vi.stubGlobal('AudioContext', create);
  return { oscillator, gain, context, create, resume: () => resume(), reject: () => reject() };
}
afterEach(() => vi.unstubAllGlobals());
describe('explicit bounded Web Audio preview owner (native API fakes, not a browser gesture claim)', () => {
  it.each(['soft', 'chime'] as const)('creates one brief modest-gain %s voice, then disconnects and closes it', (tone) => {
    const a = audio(); const finish = vi.fn(); const cancel = startTonePreview(tone, 30, () => true, finish);
    expect(a.create).toHaveBeenCalledTimes(1); expect(a.context.createOscillator).toHaveBeenCalledTimes(1); expect(a.oscillator.type).toBe('sine');
    expect(a.oscillator.start).toHaveBeenCalledWith(10); expect(a.oscillator.stop).toHaveBeenCalledWith(10.36);
    expect(a.gain.gain.linearRampToValueAtTime).toHaveBeenCalledWith(.012, 10.015); expect(a.gain.gain.linearRampToValueAtTime).toHaveBeenCalledWith(0, 10.34);
    a.oscillator.onended!(); cancel(); expect(finish).toHaveBeenCalledExactlyOnceWith('played'); expect(a.context.close).toHaveBeenCalledTimes(1); expect(a.oscillator.disconnect).toHaveBeenCalledTimes(1); expect(a.gain.disconnect).toHaveBeenCalledTimes(1);
  });
  it.each(['cancel', 'stale', 'closed'] as const)('never starts nodes after a late resume on %s', async (reason) => {
    const a = audio('suspended'); let current = true; const finish = vi.fn(); const cancel = startTonePreview('chime', 100, () => current, finish);
    expect(a.context.resume).toHaveBeenCalledTimes(1); expect(a.context.createOscillator).not.toHaveBeenCalled();
    if (reason === 'cancel') cancel(); else if (reason === 'stale') current = false;
    a.resume(); if (reason === 'closed') a.context.state = 'closed'; await Promise.resolve();
    expect(a.context.createOscillator).not.toHaveBeenCalled(); expect(finish).toHaveBeenCalledExactlyOnceWith('canceled');
    if (reason !== 'closed') expect(a.context.close).toHaveBeenCalledTimes(1);
  });
  it('closes an already playing voice immediately on navigation/cancel', () => {
    const a = audio(); const finish = vi.fn(); const cancel = startTonePreview('soft', 100, () => true, finish); const ended = a.oscillator.onended!;
    cancel(); ended(); expect(finish).toHaveBeenCalledExactlyOnceWith('canceled'); expect(a.context.close).toHaveBeenCalledTimes(1); expect(a.oscillator.onended).toBeNull();
    expect(a.gain.gain.linearRampToValueAtTime.mock.calls[0][0]).toBeLessThanOrEqual(.04);
  });
  it('reports unavailable, constructor errors, rejected resumes and node failures without leaking', async () => {
    vi.stubGlobal('AudioContext', undefined); const finish = vi.fn(); startTonePreview('soft', 30, () => true, finish); expect(finish).toHaveBeenLastCalledWith('unavailable');
    vi.stubGlobal('AudioContext', vi.fn(function () { throw Error('blocked'); })); startTonePreview('soft', 30, () => true, finish); expect(finish).toHaveBeenLastCalledWith('blocked');
    let a = audio('suspended'); startTonePreview('soft', 30, () => true, finish); a.reject(); await Promise.resolve(); expect(finish).toHaveBeenLastCalledWith('blocked'); expect(a.context.close).toHaveBeenCalledTimes(1);
    a = audio(); a.context.createGain.mockImplementation(() => { throw Error('node failure'); }); startTonePreview('soft', 30, () => true, finish); expect(finish).toHaveBeenLastCalledWith('blocked'); expect(a.oscillator.disconnect).toHaveBeenCalled(); expect(a.context.close).toHaveBeenCalledTimes(1);
  });
  it.each([0, -1, 101, NaN, Infinity])('does not allocate for invalid or silent volume %s', (volume) => {
    const a = audio(); const finish = vi.fn(); startTonePreview('soft', volume, () => true, finish); expect(a.create).not.toHaveBeenCalled(); expect(finish).toHaveBeenCalledWith('canceled');
  });
  it('refuses None, unknown tones and stale requests before allocation', () => {
    const a = audio(); const finish = vi.fn(); startTonePreview('none', 30, () => true, finish); startTonePreview('arbitrary' as 'soft', 30, () => true, finish); startTonePreview('soft', 30, () => false, finish); expect(a.create).not.toHaveBeenCalled();
  });
});
