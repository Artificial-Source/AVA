import { applePlatform, defaultBindings, defaultShortcuts, effectiveBinding, safeBinding, type ShortcutAction, type ShortcutPreferences } from './shortcutBindings';
export { appChord, defaultShortcuts, effectiveBinding, recordedBinding, shortcutForEvent, type ShortcutAction, type ShortcutPreferences } from './shortcutBindings';

export const shortcutGroups = ['General', 'Chat', 'Panels'] as const;
export type ShortcutGroup = (typeof shortcutGroups)[number];
// Ids are stable: Settings → Shortcuts stores overrides by id.
const actions: readonly Readonly<{ id: ShortcutAction; label: string; group: ShortcutGroup; detail: string }>[] = [
  { id: 'palette', label: 'Open command palette', group: 'General', detail: 'Search every action' },
  { id: 'sessions', label: 'Find a chat', group: 'General', detail: 'Search chats in every project' },
  { id: 'newChat', label: 'New chat', group: 'General', detail: 'Start a chat with your defaults' },
  { id: 'settings', label: 'Open Settings', group: 'General', detail: 'Preferences, models and shortcuts' },
  { id: 'models', label: 'Choose model', group: 'Chat', detail: 'Pick the model for this chat' },
  { id: 'composer', label: 'Focus composer', group: 'Chat', detail: 'Jump to the message box' },
  { id: 'tools', label: 'Toggle tool details', group: 'Chat', detail: 'Expand or collapse all tool activity' },
  { id: 'stop', label: 'Stop the run', group: 'Chat', detail: 'Stop this chat while it’s working' },
  { id: 'agents', label: 'Toggle Agents', group: 'Panels', detail: 'Show or hide subagents' },
  { id: 'changes', label: 'Toggle Changes', group: 'Panels', detail: 'Show or hide file changes' },
  { id: 'context', label: 'Toggle Context', group: 'Panels', detail: 'Show or hide context usage' },
  { id: 'inspector', label: 'Show or hide the inspector', group: 'Panels', detail: 'Reopen the last panel you used' },
];
export const shortcutActions = actions.map((action) => ({ ...action, binding: defaultBindings[action.id] as string | null }));

// Platform-native chord text: “⌘⇧9” on Apple platforms, “Ctrl+Shift+9” elsewhere.
export function bindingLabel(binding: string | null) {
  if (!binding) return 'Unassigned';
  const apple = applePlatform();
  const parts = binding.split('+').slice(1).map((key) => key === 'Shift' ? apple ? '⇧' : 'Shift' : key);
  return apple ? `⌘${parts.join('')}` : ['Ctrl', ...parts].join('+');
}
export function shortcutHint(preferences: ShortcutPreferences | undefined, action: ShortcutAction): Readonly<{ label: string; aria: string }> | null {
  const value = preferences ?? defaultShortcuts;
  const binding = value.enabled ? effectiveBinding(value, action) : null;
  // aria-keyshortcuts names the platform's real modifier key in place of “Mod”.
  return binding && safeBinding(binding) ? { label: bindingLabel(binding), aria: binding.replace('Mod', applePlatform() ? 'Meta' : 'Control') } : null;
}
// Deliberately conservative. Browser/OS navigation and native editing stay native.
export function bindingProblem(binding: string) {
  const mod = applePlatform() ? 'Cmd' : 'Ctrl';
  return safeBinding(binding) ? null : `Reserved by the browser, system or text editing. Use ${mod}+Shift with 1, 2, 6–9, 0, comma or period, or ${mod}+K or ${mod}+P. Screenshot keys 3–5 stay reserved.`;
}

export function bindingConflict(preferences: ShortcutPreferences, id: ShortcutAction, binding: string) {
  return shortcutActions.find((action) => action.id !== id && effectiveBinding(preferences, action.id) === binding)?.label;
}
