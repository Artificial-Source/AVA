import type { PreviewTone } from './behaviorPreferences';

export type ToneResult = 'played' | 'canceled' | 'unavailable' | 'blocked';
// Called only from an accepted trusted Play click. One synthesized voice, no assets or subscriptions.
export function startTonePreview(tone: PreviewTone, volume: number, current: () => boolean, finish: (result: ToneResult) => void) {
  let context: AudioContext | undefined;
  let oscillator: OscillatorNode | undefined;
  let gain: GainNode | undefined;
  let ended = false;
  function settle(result: ToneResult) {
    if (ended) return;
    ended = true;
    if (oscillator) { oscillator.onended = null; try { oscillator.stop(); } catch { /* Already stopped. */ } try { oscillator.disconnect(); } catch { /* Native cleanup may already be complete. */ } }
    try { gain?.disconnect(); } catch { /* Closed native graph. */ }
    if (context && context.state !== 'closed') { try { void context.close().catch(() => {}); } catch { /* No resources can be reused. */ } }
    finish(result);
  }
  function play() {
    if (ended) return;
    if (!current() || context?.state === 'closed') { settle('canceled'); return; }
    if (!context || context.state !== 'running') { settle('blocked'); return; }
    try {
      const start = context.currentTime;
      oscillator = context.createOscillator(); gain = context.createGain();
      oscillator.type = 'sine';
      oscillator.frequency.setValueAtTime(tone === 'chime' ? 660 : 440, start);
      if (tone === 'chime') oscillator.frequency.setValueAtTime(880, start + .14);
      gain.gain.setValueAtTime(0, start);
      gain.gain.linearRampToValueAtTime(.04 * volume / 100, start + .015);
      gain.gain.linearRampToValueAtTime(0, start + .34);
      oscillator.connect(gain); gain.connect(context.destination);
      oscillator.onended = () => settle(current() ? 'played' : 'canceled');
      oscillator.start(start); oscillator.stop(start + .36);
    } catch { settle('blocked'); }
  }
  if (!current() || !['soft', 'chime'].includes(tone) || !Number.isFinite(volume) || volume <= 0 || volume > 100) settle('canceled');
  else if (typeof globalThis.AudioContext !== 'function') settle('unavailable');
  else {
    try {
      context = new AudioContext();
      if (context.state === 'suspended') void context.resume().then(play, () => settle('blocked'));
      else play();
    } catch { settle('blocked'); }
  }
  return () => settle('canceled');
}
