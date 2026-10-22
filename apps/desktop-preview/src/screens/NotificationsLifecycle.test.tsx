import { useRef, useState } from 'react';
import { act, fireEvent, render, screen } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { canFocus } from '../ui/DialogFrame';
import type { Preferences } from './contracts';
import { defaultPreferences } from './screenExamples';
import { NotificationsSettings } from './NotificationsSettings';

function Harness({ active = true, revision = 0 }: { active?: boolean; revision?: number }) {
  const [preferences, setPreferences] = useState<Preferences>(defaultPreferences); const root = useRef<HTMLDivElement>(null);
  return <div ref={root} data-revision={revision}><NotificationsSettings preferences={preferences} active={active} allowed={() => active && canFocus(root.current)} update={(patch) => setPreferences({ ...preferences, ...patch })} /></div>;
}
const preview = (event = 'Task finished') => screen.getByRole('button', { name: `Preview ${event} sound` });
// Exercises controller acceptance and cleanup with fake native APIs. This deliberately
// invokes the trusted-branch seam, and does NOT establish a trusted browser gesture.
function acceptedPlay(button = preview()) {
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
  it('previews each event from its own row, allows one voice at a time, stops it and never lets Reset autoplay', async () => {
    const a = nativeAudio(); render(<Harness />);
    const play = acceptedPlay(); act(() => { play(); play(); }); expect(a.Audio).toHaveBeenCalledTimes(1);
    expect(screen.getByRole('button', { name: 'Stop Task finished sound' })).toBeEnabled();
    expect(preview('Needs attention')).toBeDisabled(); expect(preview('Task failed')).toBeDisabled();
    act(acceptedPlay(preview('Needs attention'))); expect(a.Audio).toHaveBeenCalledTimes(1);
    await act(async () => a.voices[0].resume()); expect(a.voices[0].createOscillator).toHaveBeenCalledTimes(1);
    fireEvent.click(screen.getByRole('button', { name: 'Stop Task finished sound' })); expect(a.voices[0].close).toHaveBeenCalledTimes(1); expect(preview()).toBeEnabled(); expect(preview('Task failed')).toBeEnabled();
    fireEvent.click(screen.getByLabelText('Mute all notifications')); expect(preview()).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Reset notifications' })); expect(a.Audio).toHaveBeenCalledTimes(1); expect(preview()).toBeEnabled();
  });
  it('keeps silent tones and untrusted clicks from allocating audio', () => {
    const a = nativeAudio(); render(<Harness />);
    fireEvent.change(screen.getByLabelText('Task failed sound'), { target: { value: 'none' } }); expect(preview('Task failed')).toBeDisabled();
    fireEvent.click(preview()); expect(screen.getByRole('status')).toHaveTextContent('Sound needs a direct click.'); expect(a.Audio).not.toHaveBeenCalled();
    fireEvent.change(screen.getByLabelText('Notification volume'), { target: { value: '0' } }); expect(preview()).toBeDisabled(); expect(preview('Needs attention')).toBeDisabled();
  });
  it.each(['unmount', 'inactive', 'revision', 'mute', 'volume', 'hidden'] as const)('cancels late resume after %s without allocating a voice or reviving UI', async (change) => {
    const a = nativeAudio(); const view = render(<Harness />); const play = acceptedPlay(); act(play); expect(a.voices[0].createOscillator).not.toHaveBeenCalled();
    if (change === 'unmount') view.unmount();
    else if (change === 'inactive') view.rerender(<Harness active={false} />);
    else if (change === 'revision') view.rerender(<Harness revision={1} />);
    else if (change === 'mute') fireEvent.click(screen.getByLabelText('Mute all notifications'));
    else if (change === 'volume') fireEvent.change(screen.getByLabelText('Notification volume'), { target: { value: '0' } });
    else view.container.hidden = true;
    await act(async () => a.voices[0].resume()); act(play);
    expect(a.Audio).toHaveBeenCalledTimes(1); expect(a.voices[0].createOscillator).not.toHaveBeenCalled(); expect(a.voices[0].close).toHaveBeenCalledTimes(1);
    expect(view.container.querySelector('[role=status]')?.textContent ?? '').toBe('');
  });
  it('reports missing native API and blocked playback in the error tone, but no result after a stale callback', () => {
    vi.stubGlobal('AudioContext', undefined); const view = render(<Harness />); const oldPlay = acceptedPlay(); act(oldPlay);
    expect(screen.getByRole('status')).toHaveTextContent('This browser can’t play sounds.'); expect(screen.getByRole('status')).toHaveAttribute('data-failed', 'true');
    vi.stubGlobal('AudioContext', vi.fn(function () { throw Error('blocked'); })); act(acceptedPlay());
    expect(screen.getByRole('status')).toHaveTextContent('Sound couldn’t play');
    view.rerender(<Harness active={false} />); act(oldPlay); expect(view.container).not.toHaveTextContent('Sound couldn’t play');
  });
});
