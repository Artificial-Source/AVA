import { useLayoutEffect, useRef, useState } from 'react';
import { PlayIcon } from '@phosphor-icons/react/dist/csr/Play';
import { StopIcon } from '@phosphor-icons/react/dist/csr/Stop';
import { Button, IconButton } from '../ui/Button';
import { boundedInteger, defaultNotifications, notificationEvents, previewTone, type NotificationEvent, type NotificationPreferences } from './behaviorPreferences';
import type { BehaviorSettingsProps } from './GeneralSettings';
import { PreferenceRow } from './ScreenPrimitives';
import { anchor } from './settingsIndex';
import { startTonePreview } from './tonePreview';
import { useViewGuard } from './useViewGuard';
import styles from './BehaviorSettings.module.css';
import controls from './screens.module.css';

export function NotificationsSettings({ preferences, active, allowed, update }: BehaviorSettingsProps) {
  const value = preferences.notificationPolicy ?? defaultNotifications;
  const [playing, setPlaying] = useState<NotificationEvent | null>(null);
  const [sound, setSound] = useState<Readonly<{ text: string; failed: boolean }> | null>(null);
  const cancel = useRef<(() => void) | null>(null);
  const generation = useRef(0);
  const renderToken = {};
  const latestRender = useRef(renderToken);
  useLayoutEffect(() => { latestRender.current = renderToken; });
  const gate = useViewGuard(JSON.stringify([value, active]));
  function valid() { return allowed() && gate.valid(); }
  function stop() { ++generation.current; cancel.current?.(); cancel.current = null; }
  // Any policy, visibility or owner change ends the voice and its message.
  useLayoutEffect(() => { setPlaying(null); setSound(null); return stop; }, [value, active, allowed]);
  function change(patch: Partial<NotificationPreferences>) { if (valid()) { stop(); update({ notificationPolicy: { ...value, ...patch } }); } }
  function channel(event: NotificationEvent, patch: Partial<NotificationPreferences['channels'][NotificationEvent]>) { change({ channels: { ...value.channels, [event]: { ...value.channels[event], ...patch } } }); }
  // One voice at a time, started only by a trusted click on that event's own preview.
  function preview(event: NotificationEvent, trusted: boolean) {
    const tone = previewTone(value, event);
    if (!valid() || latestRender.current !== renderToken || playing || cancel.current || tone === 'none') return;
    if (!trusted) { setSound({ text: 'Sound needs a direct click.', failed: true }); return; }
    const ticket = ++generation.current;
    setPlaying(event); setSound(null);
    let finished = false;
    const cleanup = startTonePreview(tone, value.volume, () => ticket === generation.current && valid(), (result) => {
      finished = true;
      if (ticket !== generation.current || !valid()) return;
      cancel.current = null; setPlaying(null);
      setSound(result === 'unavailable' ? { text: 'This browser can’t play sounds.', failed: true } : result === 'blocked' ? { text: 'Sound couldn’t play. The browser may be blocking audio; click the preview again.', failed: true } : null);
    });
    if (!finished) cancel.current = cleanup;
  }
  const changed = JSON.stringify(value) !== JSON.stringify(defaultNotifications);
  return <div>
    <fieldset className={styles.group} disabled={!active}><legend>Delivery</legend>
      <div {...anchor('notifications.mute')}><PreferenceRow title="Mute all notifications" description="Silence every notification and sound."><input type="checkbox" aria-label="Mute all notifications" checked={value.muted} onChange={(e) => change({ muted: e.currentTarget.checked })} /></PreferenceRow></div>
      <div {...anchor('notifications.background')}><PreferenceRow title="Background only" description="Notify only while AVA isn’t the active window."><input type="checkbox" aria-label="Background-only notifications" checked={value.backgroundOnly} onChange={(e) => change({ backgroundOnly: e.currentTarget.checked })} /></PreferenceRow></div>
    </fieldset>
    <fieldset className={styles.group} disabled={!active}><legend>Events & sounds</legend>
      {notificationEvents.map((entry) => {
        const tone = previewTone(value, entry.id); const current = playing === entry.id;
        return <div key={entry.id} {...anchor(`notifications.${entry.id}`)}><PreferenceRow title={entry.label} description={entry.description}><div className={styles.channelControls}>
          <label><input type="checkbox" aria-label={`${entry.label} notifications`} checked={value.channels[entry.id].enabled} onChange={(e) => channel(entry.id, { enabled: e.currentTarget.checked })} />On</label>
          <select className={controls.select} aria-label={`${entry.label} sound`} value={value.channels[entry.id].tone} onChange={(e) => { const next = e.currentTarget.value; if (next === 'none' || next === 'soft' || next === 'chime') channel(entry.id, { tone: next }); }}><option value="none">None</option><option value="soft">Soft tone</option><option value="chime">Chime</option></select>
          {current ? <IconButton variant="quiet" label={`Stop ${entry.label} sound`} onClick={() => { if (valid()) { stop(); setPlaying(null); } }}><StopIcon size={15} aria-hidden="true" /></IconButton>
            : <IconButton variant="quiet" label={`Preview ${entry.label} sound`} disabled={!active || tone === 'none' || playing !== null} onClick={(e) => preview(entry.id, e.nativeEvent.isTrusted)}><PlayIcon size={15} aria-hidden="true" /></IconButton>}
        </div></PreferenceRow></div>;
      })}
      <div {...anchor('notifications.volume')}><PreferenceRow title="Volume" description="Loudness of notification sounds. Zero is silent."><div className={styles.volume}><input aria-label="Notification volume" aria-valuetext={`${value.volume}%`} type="range" min="0" max="100" step="1" value={value.volume} onChange={(e) => { const volume = Number(e.currentTarget.value); if (boundedInteger(volume, 0, 100)) change({ volume }); }} /><span aria-hidden="true">{value.volume}%</span></div></PreferenceRow></div>
      <p className={styles.soundStatus} data-failed={sound?.failed || undefined} role="status">{sound?.text}</p>
    </fieldset>
    <div className={styles.reset}><p>Restore delivery, events and volume. Resetting never plays a sound.</p><Button disabled={!active || !changed} onClick={() => { if (changed && valid()) { stop(); update({ notificationPolicy: defaultNotifications }); } }}>Reset notifications</Button></div>
  </div>;
}
