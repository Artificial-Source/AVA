// Small synchronous key contract. Action presentation and execution are optional owners.
export const defaultBindings = { settings: 'Mod+Shift+,', newChat: 'Mod+Shift+9', models: 'Mod+Shift+6', composer: 'Mod+Shift+7', tools: 'Mod+Shift+8', stop: null, agents: 'Mod+Shift+1', changes: 'Mod+Shift+2', context: 'Mod+Shift+0', inspector: null } as const;
export type ShortcutAction = keyof typeof defaultBindings;
export type ShortcutPreferences = Readonly<{ enabled: boolean; overrides: Readonly<Partial<Record<ShortcutAction, string | null>>> }>;
export const defaultShortcuts: ShortcutPreferences = { enabled: true, overrides: {} };
export function effectiveBinding(preferences: ShortcutPreferences, id: ShortcutAction) { return Object.hasOwn(preferences.overrides, id) ? preferences.overrides[id] ?? null : defaultBindings[id]; }
// Reserve browser/editor keys, Alt, unshifted tab numbers, and macOS screenshot keys 3–5.
export function safeBinding(binding: string) { return /^Mod\+Shift\+([1267890]|[,.])$/.test(binding); }
export function recordedBinding(event: Pick<KeyboardEvent, 'key' | 'code' | 'ctrlKey' | 'metaKey' | 'altKey' | 'shiftKey' | 'repeat' | 'isComposing' | 'keyCode' | 'getModifierState'>) {
  if (event.repeat || event.isComposing || event.keyCode === 229 || event.getModifierState('AltGraph') || event.altKey || event.ctrlKey === event.metaKey) return null;
  const key = /^Digit[0-9]$/.test(event.code) ? event.code.slice(5) : event.code === 'Comma' ? ',' : event.code === 'Period' ? '.' : event.key.length === 1 ? event.key.toUpperCase() : '';
  return key ? `Mod+${event.shiftKey ? 'Shift+' : ''}${key}` : null;
}
export function shortcutForEvent(event: KeyboardEvent, preferences = defaultShortcuts) {
  if (!preferences.enabled) return null;
  const binding = recordedBinding(event);
  if (!binding || !safeBinding(binding)) return null;
  const matches = (Object.keys(defaultBindings) as ShortcutAction[]).filter((id) => effectiveBinding(preferences, id) === binding);
  return matches.length === 1 ? matches[0] : null;
}
