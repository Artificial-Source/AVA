import { useId } from 'react';
import { TextAaIcon } from '@phosphor-icons/react/dist/csr/TextAa';
import { Button } from '../ui/Button';
import type { Preferences } from './contracts';
import { PreferenceRow } from './ScreenPrimitives';
import { useViewGuard } from './useViewGuard';
import styles from './AppearanceSettings.module.css';
import controls from './screens.module.css';

type AppearanceKey = 'palette' | 'uiFont' | 'proseSize' | 'proseSpacing' | 'readingWidth' | 'contrast' | 'density' | 'motion';
export const defaultAppearance: Required<Pick<Preferences, AppearanceKey>> = {
  palette: 'default', uiFont: 'geist', proseSize: 'normal', proseSpacing: 'normal', readingWidth: 'wide', contrast: 'system', density: 'comfortable', motion: 'system',
};
const options = {
  palette: [['default', 'Default'], ['charcoal', 'Charcoal'], ['midnight', 'Midnight']],
  uiFont: [['geist', 'Geist · local'], ['system', 'System sans']],
  proseSize: [['small', 'Small · 14 px'], ['normal', 'Default · 15 px'], ['large', 'Large · 17 px']],
  proseSpacing: [['compact', 'Compact · 1.5'], ['normal', 'Default · 1.7'], ['relaxed', 'Relaxed · 1.9']],
  readingWidth: [['narrow', 'Narrow · 620 px'], ['balanced', 'Balanced · 700 px'], ['wide', 'Default · 780 px']],
  contrast: [['system', 'Follow system'], ['higher', 'Higher contrast']],
  density: [['comfortable', 'Comfortable'], ['compact', 'Compact']],
  motion: [['system', 'Follow system'], ['reduced', 'Reduce motion']],
} as const;

export interface AppearanceSettingsProps {
  preferences: Preferences;
  opaque: boolean;
  active: boolean;
  resetOpaque?: boolean;
  onChange: (preferences: Preferences) => void;
  onOpaqueChange: (opaque: boolean) => void;
  onReset: () => void;
}

export function AppearanceSettings({ preferences, opaque, active, resetOpaque = false, onChange, onOpaqueChange, onReset }: AppearanceSettingsProps) {
  const id = useId();
  const gate = useViewGuard(JSON.stringify([preferences, opaque, active]));
  function change(key: AppearanceKey, value: string) {
    if (!active || !options[key].some(([candidate]) => candidate === value) || value === (preferences[key] ?? defaultAppearance[key]) || !gate.claim()) return;
    onChange({ ...preferences, [key]: value });
  }
  function select(key: Exclude<AppearanceKey, 'palette'>, label: string) {
    return <select className={controls.select} aria-label={label} disabled={!active} value={preferences[key] ?? defaultAppearance[key]} onChange={(event) => change(key, event.currentTarget.value)}>{options[key].map(([value, text]) => <option key={value} value={value}>{text}</option>)}</select>;
  }
  const changed = opaque !== resetOpaque || (Object.keys(defaultAppearance) as AppearanceKey[]).some((key) => (preferences[key] ?? defaultAppearance[key]) !== defaultAppearance[key]);
  return <div className={styles.appearance}>
    <p className={styles.intro}>Make AVA comfortable to read. Changes apply immediately for this visit.</p>
    <fieldset className={styles.group} disabled={!active}>
      <legend>Colors</legend>
      <div className={styles.palettes} role="radiogroup" aria-label="Dark palette">
        {options.palette.map(([value, label]) => <label key={value} className={styles.palette}>
          <span className={styles.swatch} data-palette={value} aria-hidden="true"><span /><span /><span /></span>
          <span><input type="radio" name={`${id}-palette`} value={value} checked={(preferences.palette ?? defaultAppearance.palette) === value} onChange={(event) => change('palette', event.currentTarget.value)} />{label}</span>
        </label>)}
      </div>
      <p className={styles.hint}>Neutral dark shades. Light and system theme switching are not available.</p>
      <PreferenceRow title="Contrast" description="Stronger text and separators. System higher contrast always wins.">{select('contrast', 'Contrast')}</PreferenceRow>
    </fieldset>
    <fieldset className={styles.group} disabled={!active}>
      <legend>Reading</legend>
      <PreferenceRow title="Interface font" description="Local fonts only. Code keeps its monospace font.">{select('uiFont', 'Interface font')}</PreferenceRow>
      <PreferenceRow title="Conversation text" description="Prose in parent and read-only child conversations, not fixed-size UI labels.">{select('proseSize', 'Conversation text size')}</PreferenceRow>
      <PreferenceRow title="Line spacing" description="Space between lines of conversation prose.">{select('proseSpacing', 'Conversation line spacing')}</PreferenceRow>
      <PreferenceRow title="Reading width" description="Maximum width for the conversation, title and composer.">{select('readingWidth', 'Reading width')}</PreferenceRow>
    </fieldset>
    <figure className={styles.sample} aria-label="Live appearance preview">
      <figcaption><TextAaIcon size={16} aria-hidden="true" /><span>Live preview</span><span>Sidebar & reading sample</span></figcaption>
      <div className={styles.sampleBody}>
        <div className={`${styles.sampleSidebar} material`} aria-hidden="true"><span>Chats</span><span>Notes</span></div>
        <div className={styles.sampleCanvas}><div className={styles.sampleText}><strong>A little room to think.</strong><p>Keep the conversation easy to follow, with clear labels and a comfortable line length.</p><code>example.ts</code></div></div>
      </div>
    </figure>
    <fieldset className={styles.group} disabled={!active}>
      <legend>Surfaces & motion</legend>
      <PreferenceRow title="Sidebar material" description="Shared with the sidebar’s Opaque materials control. System accessibility settings can make glass opaque."><select className={controls.select} aria-label="Sidebar material" value={opaque ? 'opaque' : 'glass'} onChange={(event) => {
        const value = event.currentTarget.value;
        if (active && (value === 'opaque' || value === 'glass') && (value === 'opaque') !== opaque && gate.claim()) onOpaqueChange(value === 'opaque');
      }}><option value="glass">Glass</option><option value="opaque">Opaque</option></select></PreferenceRow>
      <PreferenceRow title="Motion" description="Reduce movement across chat, sidebar, inspectors and dialogs. System reduced motion always wins.">{select('motion', 'App motion')}</PreferenceRow>
      <PreferenceRow title="Settings-row density" description="Spacing between Settings rows only; conversation spacing is separate.">{select('density', 'Settings-row density')}</PreferenceRow>
    </fieldset>
    <div className={styles.reset}><p>Restore appearance only. Keep chat behavior, models and definitions.</p><Button disabled={!active || !changed} onClick={() => { if (active && changed && gate.claim()) onReset(); }}>Reset appearance</Button></div>
  </div>;
}
