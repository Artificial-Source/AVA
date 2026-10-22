import { useRef, useState } from 'react';
import { act, fireEvent, render, screen, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { canFocus } from '../ui/DialogFrame';
import type { Preferences } from './contracts';
import { defaultPreferences } from './screenExamples';
import { NotificationsSettings } from './NotificationsSettings';

function Harness({ active = true, revision = 0 }: { active?: boolean; revision?: number }) {
  const [preferences, setPreferences] = useState<Preferences>(defaultPreferences); const root = useRef<HTMLDivElement>(null);
  return <div ref={root} data-revision={revision}><NotificationsSettings preferences={preferences} active={active} allowed={() => active && canFocus(root.current)} update={(patch) => setPreferences({ ...preferences, ...patch })} /></div>;
}
// Exercises controller acceptance and cleanup with fake native APIs. This deliberately
// invokes the trusted-branch seam, and does NOT establish a trusted browser gesture.
function acceptedPlay(button = screen.getByRole('button', { name: 'Play sound' })) {
  const key = Object.keys(button).find((name) => name.startsWith('__reactProps'))!;
  const handler = (button as unknown as Record<string, { onClick: (event: unknown) => void }>)[key].onClick;
  return () => handler({ nativeEvent: { isTrusted: true } });
}
function nativeAudio() {
  const voices: { state: string; close: ReturnType<typeof vi.fn>; oscillator: { onended: (() => void) | null }; createOscillator: ReturnType<typeof vi.fn>; resume: () => void }[] = [];
  const Audio = vi.fn(function () {
    let resolve!: () => void;
    const oscillator = { frequency: { setValueAtTime() {} }, connect() {}, disconnect() {}, start() {}, stop() {}, onended: null as (() => void) | null };
    const context = { state: 'suspended', currentTime: 0, destination: {}, resume: () => new Promise<void>((done) => { resolve = done; }), close: vi.fn(async () => { context.state = 'closed'; }), createOscillator: vi.fn(() => oscillator), createGain: () => ({ gain: { setValueAtTime() {}, linearRampToValueAtTime() {} }, connect() {}, disconnect() {} }) };
    voices.push({ get state() { return context.state; }, close: context.close, oscillator, createOscillator: context.createOscillator, resume: () => { if (context.state !== 'closed') context.state = 'running'; resolve(); } }); return context;
  }); vi.stubGlobal('AudioContext', Audio); return { Audio, voices };
}
afterEach(() => vi.unstubAllGlobals());
describe('notification page lifecycle authority', () => {
  it('allows only one pending/playing voice, stops it and never lets Reset or Test event autoplay', async () => {
    const a = nativeAudio(); render(<Harness />);
    fireEvent.click(screen.getByRole('button', { name: 'Test event' })); expect(a.Audio).not.toHaveBeenCalled();
    const play = acceptedPlay(); act(() => { play(); play(); }); expect(a.Audio).toHaveBeenCalledTimes(1); expect(screen.getByRole('button', { name: 'Play sound' })).toBeDisabled();
    await act(async () => a.voices[0].resume()); expect(a.voices[0].createOscillator).toHaveBeenCalledTimes(1);
    fireEvent.click(screen.getByRole('button', { name: 'Stop sound' })); expect(a.voices[0].close).toHaveBeenCalledTimes(1); expect(screen.getByRole('button', { name: 'Play sound' })).toBeEnabled();
    fireEvent.click(screen.getByLabelText('Mute all notifications')); fireEvent.click(screen.getByRole('button', { name: 'Reset Notifications' })); expect(a.Audio).toHaveBeenCalledTimes(1);
  });
  it.each(['unmount', 'inactive', 'revision', 'mute', 'volume', 'hidden'] as const)('cancels late resume after %s without allocating a voice or reviving UI', async (change) => {
    const a = nativeAudio(); const view = render(<Harness />); const play = acceptedPlay(); act(play); expect(a.voices[0].createOscillator).not.toHaveBeenCalled();
    if (change === 'unmount') view.unmount();
    else if (change === 'inactive') view.rerender(<Harness active={false} />);
    else if (change === 'revision') view.rerender(<Harness revision={1} />);
    else if (change === 'mute') fireEvent.click(screen.getByLabelText('Mute all notifications'));
    else if (change === 'volume') fireEvent.change(screen.getByLabelText('Preview volume'), { target: { value: '0' } });
    else view.container.hidden = true;
    await act(async () => a.voices[0].resume()); act(play);
    expect(a.Audio).toHaveBeenCalledTimes(1); expect(a.voices[0].createOscillator).not.toHaveBeenCalled(); expect(a.voices[0].close).toHaveBeenCalledTimes(1);
    expect(view.container).not.toHaveTextContent('Tone preview finished.');
  });
  it('reports missing native API and blocked playback, but no result after a stale callback', () => {
    vi.stubGlobal('AudioContext', undefined); const view = render(<Harness />); const oldPlay = acceptedPlay(); act(oldPlay);
    expect(within(screen.getByRole('region', { name: 'Local notification test' })).getByRole('status')).toHaveTextContent('Web Audio is unavailable');
    vi.stubGlobal('AudioContext', vi.fn(function () { throw Error('blocked'); })); act(acceptedPlay());
    expect(within(screen.getByRole('region', { name: 'Local notification test' })).getByRole('status')).toHaveTextContent('Sound could not play');
    view.rerender(<Harness active={false} />); act(oldPlay); expect(view.container).not.toHaveTextContent('Sound could not play');
  });
});
