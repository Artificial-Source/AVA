import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createRunDriver } from './suppliedRunDriver';
import { createInitialState } from '../state/fixtures';
import { previewReducer } from '../state/reducer';
import type { RunEvent } from '../state/suppliedRun';

const identity = { chatId: createInitialState().activeChatId, runId: 'driver-run', generation: 1 };
const event = (sequence: number, delta = 'x'): RunEvent => ({ ...identity, sequence, kind: 'text', delta });
const frames = new Map<number, FrameRequestCallback>(); let nextFrame = 0;
beforeEach(() => {
  vi.useFakeTimers(); nextFrame = 0; frames.clear();
  vi.stubGlobal('requestAnimationFrame', vi.fn((callback: FrameRequestCallback) => { const id = nextFrame++; frames.set(id, callback); return id; }));
  vi.stubGlobal('cancelAnimationFrame', vi.fn((id: number) => { frames.delete(id); }));
});
afterEach(() => { vi.useRealTimers(); vi.unstubAllGlobals(); });
function frame() { const callbacks = [...frames.values()]; frames.clear(); callbacks.forEach((callback) => callback(0)); }

describe('finite supplied snapshot transport', () => {
  it('coalesces 1000+ increments into one frame without dropping order/text and delivers terminal immediately', () => {
    let state = previewReducer(createInitialState(), { type: 'startSuppliedRun', identity });
    const deltas = ['\ud83d', '\ude80', ...Array.from({ length: 1000 }, (_, i) => `${i},`)];
    const receive = vi.fn((events: readonly RunEvent[]) => { state = previewReducer(state, { type: 'receiveSuppliedRun', events }); });
    const terminal: RunEvent = { ...identity, sequence: deltas.length + 1, kind: 'complete' };
    const driver = createRunDriver(identity, [deltas.map((delta, i) => event(i + 1, delta)), [terminal]], receive);
    driver.start(); driver.start(); expect(receive).not.toHaveBeenCalled(); expect(frames.size).toBe(1); expect(vi.getTimerCount()).toBe(1);
    frame(); expect(receive).toHaveBeenCalledTimes(1); expect(state.suppliedRun?.sequence).toBe(1002);
    expect(state.chats[0].messages.at(-1)).toMatchObject({ markdown: deltas.join('') });
    vi.advanceTimersByTime(1800); expect(receive).toHaveBeenCalledTimes(2); expect(state.suppliedRun?.state).toBe('committed');
    expect(frames.size).toBe(0); expect(vi.getTimerCount()).toBe(0); driver.cancel(); driver.resume(); driver.step(); vi.runAllTimers(); expect(receive).toHaveBeenCalledTimes(2);
  });
  it('flushes ordered pending text with a terminal snapshot before any queued animation frame', () => {
    const receive = vi.fn(); const driver = createRunDriver(identity, [[event(1)], [{ ...identity, sequence: 2, kind: 'error', error: 'fixture failure' }]], receive);
    driver.start(); const late = [...frames.values()][0]; vi.advanceTimersByTime(1800);
    expect(receive).toHaveBeenCalledExactlyOnceWith([event(1), { ...identity, sequence: 2, kind: 'error', error: 'fixture failure' }]);
    late(0); expect(receive).toHaveBeenCalledTimes(1); expect(frames.size).toBe(0); expect(vi.getTimerCount()).toBe(0);
  });
  it('cancels frame ID zero, timers and queued callbacks on Stop and never resurrects', () => {
    const receive = vi.fn(); const driver = createRunDriver(identity, [[event(1)], [event(2)]], receive);
    driver.start(); const late = [...frames.values()][0]; driver.cancel(); driver.cancel();
    expect(cancelAnimationFrame).toHaveBeenCalledWith(0); expect(vi.getTimerCount()).toBe(0); expect(frames.size).toBe(0);
    late(0); driver.start(); driver.step(); driver.resume(); vi.runAllTimers(); expect(receive).not.toHaveBeenCalled();
  });
  it('pauses fixture delivery, retains already queued data, steps without timers and resumes finite playback', () => {
    const receive = vi.fn(); const driver = createRunDriver(identity, [[event(1)], [event(2)], [event(3)], [{ ...identity, sequence: 4, kind: 'complete' }]], receive);
    driver.start(); driver.pause(); expect(receive).toHaveBeenCalledExactlyOnceWith([event(1)]); expect(frames.size).toBe(0); expect(vi.getTimerCount()).toBe(0);
    vi.advanceTimersByTime(10000); expect(receive).toHaveBeenCalledTimes(1);
    driver.step(); expect(receive).toHaveBeenLastCalledWith([event(2)]); expect(vi.getTimerCount()).toBe(0);
    driver.resume(); frame(); expect(receive).toHaveBeenLastCalledWith([event(3)]);
    vi.advanceTimersByTime(1800); expect(receive).toHaveBeenCalledTimes(4); expect(vi.getTimerCount()).toBe(0);
  });
  it('never delivers a wrong-run or overbound snapshot', () => {
    const receive = vi.fn();
    for (const events of [[{ ...event(1), runId: 'wrong' }], Array.from({ length: 4097 }, (_, i) => event(i + 1))]) {
      const driver = createRunDriver(identity, [events], receive); driver.start(); frame(); expect(frames.size).toBe(0); expect(vi.getTimerCount()).toBe(0);
    }
    expect(receive).not.toHaveBeenCalled();
  });
});
