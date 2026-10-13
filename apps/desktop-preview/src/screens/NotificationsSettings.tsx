import { useLayoutEffect, useRef, useState } from 'react';
import { Button } from '../ui/Button';
import { boundedInteger, defaultNotifications, notificationDecision, notificationEvents, type NotificationEvent, type NotificationPreferences, type PreviewTone } from './behaviorPreferences';
import type { BehaviorSettingsProps } from './GeneralSettings';
import { PreferenceRow } from './ScreenPrimitives';
import { startTonePreview } from './tonePreview';
import { useViewGuard } from './useViewGuard';
import styles from './BehaviorSettings.module.css';
import controls from './screens.module.css';

export function NotificationsSettings({ preferences, active, allowed, update }: BehaviorSettingsProps) {
  const value = preferences.notificationPolicy ?? defaultNotifications;
  const [event, setEvent] = useState<NotificationEvent>('completion');
  const [background, setBackground] = useState(true);
  const [receipt, setReceipt] = useState('');
  const [sound, setSound] = useState('');
  const [playing, setPlaying] = useState(false);
  const cancel = useRef<(() => void) | null>(null);
  const generation = useRef(0);
  const renderToken = {};
  const latestRender = useRef(renderToken);
  useLayoutEffect(() => { latestRender.current = renderToken; });
  const gate = useViewGuard(JSON.stringify([value, active, event, background]));
  function valid() { return allowed() && gate.valid(); }
  function stop() { ++generation.current; cancel.current?.(); cancel.current = null; }
  useLayoutEffect(() => { setPlaying(false); setSound(''); setReceipt(''); return stop; }, [value, active, event, background, allowed]);
  function change(patch: Partial<NotificationPreferences>) { if (valid()) { stop(); update({ notificationPolicy: { ...value, ...patch } }); } }
  const decision = notificationDecision(value, event, background);
  const changed = JSON.stringify(value) !== JSON.stringify(defaultNotifications);
  return <div>
    <p className={styles.intro}>Choose a local delivery policy and audition a quiet tone. Desktop notifications and live run events are not connected; nothing plays automatically.</p>
    <fieldset className={styles.group} disabled={!active}><legend>Delivery</legend>
      <PreferenceRow title="Mute all notifications" description="Suppress every test event and sound preview."><input type="checkbox" aria-label="Mute all notifications" checked={value.muted} onChange={(e) => change({ muted: e.currentTarget.checked })} /></PreferenceRow>
      <PreferenceRow title="Background only" description="Deliver only when the simulated app is in the background."><input type="checkbox" aria-label="Background-only notifications" checked={value.backgroundOnly} onChange={(e) => change({ backgroundOnly: e.currentTarget.checked })} /></PreferenceRow>
    </fieldset>
    <fieldset className={styles.group} disabled={!active}><legend>Events & sounds</legend>
      {notificationEvents.map((entry) => <PreferenceRow key={entry.id} title={entry.label} description={entry.description}><div className={styles.channelControls}><label><input type="checkbox" aria-label={`${entry.label} notifications`} checked={value.channels[entry.id].enabled} onChange={(e) => change({ channels: { ...value.channels, [entry.id]: { ...value.channels[entry.id], enabled: e.currentTarget.checked } } })} />On</label><select className={controls.select} aria-label={`${entry.label} sound`} value={value.channels[entry.id].tone} onChange={(e) => { const tone = e.currentTarget.value as PreviewTone; if (tone === 'none' || tone === 'soft' || tone === 'chime') change({ channels: { ...value.channels, [entry.id]: { ...value.channels[entry.id], tone } } }); }}><option value="none">None</option><option value="soft">Soft tone</option><option value="chime">Chime</option></select></div></PreferenceRow>)}
      <PreferenceRow title="Preview volume" description="Brief, modest-gain tones. Zero is silent."><div className={styles.volume}><input aria-label="Preview volume" type="range" min="0" max="100" step="1" value={value.volume} onChange={(e) => { const volume = Number(e.currentTarget.value); if (boundedInteger(volume, 0, 100)) change({ volume }); }} /><output>{value.volume}%</output></div></PreferenceRow>
    </fieldset>
    <section className={styles.sample} aria-label="Local notification test"><h3>Local test · simulated delivery</h3><div className={styles.testControls}>
      <label>Event<select className={controls.select} aria-label="Test event" disabled={!active || playing} value={event} onChange={(e) => { const next = e.currentTarget.value; if (valid() && notificationEvents.some((entry) => entry.id === next)) setEvent(next as NotificationEvent); }}>{notificationEvents.map((entry) => <option key={entry.id} value={entry.id}>{entry.label}</option>)}</select></label>
      <label>App state<select className={controls.select} aria-label="Simulated app state" disabled={!active || playing} value={background ? 'background' : 'foreground'} onChange={(e) => { const next = e.currentTarget.value; if (valid() && (next === 'background' || next === 'foreground')) setBackground(next === 'background'); }}><option value="background">Background</option><option value="foreground">Foreground</option></select></label>
      <Button disabled={!active} onClick={() => { if (valid()) setReceipt(decision.delivered ? `Delivered preview · ${notificationEvents.find((entry) => entry.id === event)!.label}. Local test card only; no desktop notification.` : `Suppressed preview · ${decision.reason}`); }}>Test event</Button>
      <Button disabled={!active || playing || !decision.delivered || decision.tone === 'none'} onClick={(e) => {
        if (!valid() || latestRender.current !== renderToken || playing || cancel.current || !decision.delivered || decision.tone === 'none') return;
        if (!e.nativeEvent.isTrusted) { setSound('Sound requires a direct user click.'); return; }
        const ticket = ++generation.current;
        setPlaying(true); setSound('Starting sound preview…');
        let finished = false;
        const cleanup = startTonePreview(decision.tone, value.volume, () => ticket === generation.current && valid(), (result) => {
          finished = true;
          if (ticket !== generation.current || !valid()) return;
          cancel.current = null; setPlaying(false);
          setSound(result === 'played' ? 'Tone preview finished.' : result === 'unavailable' ? 'Web Audio is unavailable in this browser.' : result === 'blocked' ? 'Sound could not play. Browser audio may be blocked; try another direct click.' : 'Tone preview stopped.');
        });
        if (!finished) cancel.current = cleanup;
      }}>Play sound</Button>
      <Button disabled={!active || !playing} onClick={() => { if (valid()) { stop(); setPlaying(false); setSound('Tone preview stopped.'); } }}>Stop sound</Button>
    </div><div className={styles.testResult} role="status">{receipt || 'Test an event to see whether this policy delivers or suppresses it.'}{sound ? <div>{sound}</div> : null}{!decision.delivered || decision.tone === 'none' ? <div>Sound is silent under the current test policy.</div> : null}</div></section>
    <div className={styles.reset}><p>Restore notification preferences. Reset never plays sound.</p><Button disabled={!active || !changed} onClick={() => { if (changed && valid()) { stop(); update({ notificationPolicy: defaultNotifications }); } }}>Reset Notifications</Button></div>
  </div>;
}
