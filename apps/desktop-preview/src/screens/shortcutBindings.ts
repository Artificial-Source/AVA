import { canFocus } from '../ui/DialogFrame';

// Small synchronous key contract. Action presentation and execution are optional owners.
export const defaultBindings = { settings: 'Mod+Shift+,', newChat: 'Mod+Shift+9', models: 'Mod+Shift+6', composer: 'Mod+Shift+7', tools: 'Mod+Shift+8', stop: 'Mod+Shift+.', agents: 'Mod+Shift+1', changes: 'Mod+Shift+2', context: 'Mod+Shift+0', inspector: null, palette: 'Mod+K', sessions: 'Mod+P' } as const;
export type ShortcutAction = keyof typeof defaultBindings;
export type ShortcutPreferences = Readonly<{ enabled: boolean; overrides: Readonly<Partial<Record<ShortcutAction, string | null>>> }>;
export const defaultShortcuts: ShortcutPreferences = { enabled: true, overrides: {} };
export function effectiveBinding(preferences: ShortcutPreferences, id: ShortcutAction) { return Object.hasOwn(preferences.overrides, id) ? preferences.overrides[id] ?? null : defaultBindings[id]; }
// Reserve browser/editor keys, Alt, unshifted tab numbers, and macOS screenshot keys 3–5.
// Unshifted Mod+K and Mod+P are the only plain chords: palette and chat finder.
export function safeBinding(binding: string) { return /^Mod\+(Shift\+([1267890]|[,.])|[KP])$/.test(binding); }
export function applePlatform() {
  const nav = globalThis.navigator as (Navigator & { userAgentData?: { platform?: string } }) | undefined;
  return /mac|iphone|ipad|ipod/i.test(nav?.userAgentData?.platform || nav?.platform || '');
}
export function recordedBinding(event: Pick<KeyboardEvent, 'key' | 'code' | 'ctrlKey' | 'metaKey' | 'altKey' | 'shiftKey' | 'repeat' | 'isComposing' | 'keyCode' | 'getModifierState'>) {
  if (event.repeat || event.isComposing || event.keyCode === 229 || event.getModifierState('AltGraph') || event.altKey || event.ctrlKey === event.metaKey) return null;
  const key = /^Digit[0-9]$/.test(event.code) ? event.code.slice(5) : event.code === 'Comma' ? ',' : event.code === 'Period' ? '.' : event.key.length === 1 ? event.key.toUpperCase() : '';
  return key ? `Mod+${event.shiftKey ? 'Shift+' : ''}${key}` : null;
}
export function shortcutForEvent(event: KeyboardEvent, preferences = defaultShortcuts) {
  if (!preferences.enabled) return null;
  const binding = recordedBinding(event);
  if (!binding || !safeBinding(binding)) return null;
  // Ctrl+K and Ctrl+P edit text on macOS; plain chords need the platform's own modifier.
  if (!binding.startsWith('Mod+Shift+') && !(applePlatform() ? event.metaKey : event.ctrlKey)) return null;
  const matches = (Object.keys(defaultBindings) as ShortcutAction[]).filter((id) => effectiveBinding(preferences, id) === binding);
  return matches.length === 1 ? matches[0] : null;
}

const blocked = 'dialog, [role="menu"], [role="listbox"], [hidden], [inert]';
// Synchronous claim for the app root: chords fire from the chat, the composer and other
// fields, never from dialogs, menus or pickers. Callers prevent the default before any await.
export function appChord(event: KeyboardEvent, root: HTMLElement, preferences?: ShortcutPreferences) {
  const target = event.target;
  if (event.defaultPrevented || !(target instanceof HTMLElement) || !canFocus(target) || !root.contains(target) || target.closest(blocked) || root.querySelector('dialog[open], [role="menu"], [data-shortcuts-blocked]')) return null;
  return shortcutForEvent(event, preferences);
}
