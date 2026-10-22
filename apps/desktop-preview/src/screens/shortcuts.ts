import { defaultBindings, effectiveBinding, safeBinding, type ShortcutAction, type ShortcutPreferences } from './shortcutBindings';
export { defaultShortcuts, effectiveBinding, recordedBinding, shortcutForEvent, type ShortcutAction, type ShortcutPreferences } from './shortcutBindings';
const actions = [
  { id: 'settings', label: 'Open Settings', group: 'Application', detail: 'Open the full Settings page.' },
  { id: 'newChat', label: 'New chat', group: 'Application', detail: 'Use your General and model defaults.' },
  { id: 'models', label: 'Choose model', group: 'Conversation', detail: 'Current parent chat only.' },
  { id: 'composer', label: 'Focus composer', group: 'Conversation', detail: 'Move focus to the parent draft.' },
  { id: 'tools', label: 'Expand / collapse all tools', group: 'Conversation', detail: 'Supplied tool groups in the current parent.' },
  { id: 'stop', label: 'Stop viewed mock', group: 'Conversation', detail: 'Only the viewed Working parent fixture; no real task.' },
  { id: 'agents', label: 'Toggle Agents', group: 'Inspector', detail: 'Show or close Agents.' },
  { id: 'changes', label: 'Toggle Changes', group: 'Inspector', detail: 'Show or close Changes.' },
  { id: 'context', label: 'Toggle Context', group: 'Inspector', detail: 'Show or close Context.' },
  { id: 'inspector', label: 'Show / hide inspector', group: 'Inspector', detail: 'Remember the last inspector tab.' },
] as const;
export const shortcutActions = actions.map((action) => ({ ...action, binding: defaultBindings[action.id] }));
export function bindingLabel(binding: string | null) { return binding?.replace('Mod', 'Ctrl/Cmd').replaceAll('+', ' + ') ?? 'Unassigned'; }
// Deliberately conservative. Browser/OS navigation and native editing stay native.
export function bindingProblem(binding: string) {
  return safeBinding(binding) ? null : 'Reserved for browser, system or native editing. Use Ctrl/Cmd + Shift with 1, 2, 6–9, 0, comma or period. Screenshot keys 3–5 stay reserved.';
}

export function bindingConflict(preferences: ShortcutPreferences, id: ShortcutAction, binding: string) {
  return shortcutActions.find((action) => action.id !== id && effectiveBinding(preferences, action.id) === binding)?.label;
}
