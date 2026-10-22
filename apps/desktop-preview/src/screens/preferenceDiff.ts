import type { ModelId } from '../state/types';
import { defaultContextPolicy, defaultConversation, defaultGeneral, defaultNotifications, notificationEvents } from './behaviorPreferences';
import type { Preferences } from './contracts';
import { defaultPreferences, modelChoices } from './screenExamples';
import { defaultShortcuts, shortcutActions } from './shortcuts';

export type PreferenceArea = Readonly<{ area: string; detail: string }>;
const join = new Intl.ListFormat('en', { style: 'long', type: 'conjunction' });

// Labels of the fields that differ from their defaults; a missing value counts as its default.
function differing<T extends object>(current: T | undefined, base: T, labels: Readonly<Partial<Record<keyof T, string>>>) {
  return (Object.keys(labels) as (keyof T)[]).filter((key) => JSON.stringify(current?.[key] ?? base[key]) !== JSON.stringify(base[key])).map((key) => labels[key]!);
}

// What “Reset all” would actually change, by Settings section, against the defaults
// (default preferences, glass sidebar, Aster for new chats). Empty when nothing differs.
export function changedPreferenceAreas(current: Preferences, opaque: boolean, defaultModel: ModelId): readonly PreferenceArea[] {
  const notifications = current.notificationPolicy ?? defaultNotifications;
  const shortcuts = current.shortcuts ?? defaultShortcuts;
  const customKeys = shortcutActions.filter((action) => Object.hasOwn(shortcuts.overrides, action.id)).map((action) => action.label);
  const areas: [string, string[]][] = [
    ['General', [
      ...differing(current, defaultPreferences, { enter: 'Enter key', help: 'review notes' }),
      ...differing(current.general, defaultGeneral, { mode: 'new-chat mode', projectId: 'new-chat project', titleFromFirstMessage: 'chat naming', inspector: 'inspector for new chats' }),
    ]],
    ['Appearance', [
      ...differing(current, defaultPreferences, { palette: 'palette', uiFont: 'interface font', proseSize: 'text size', proseSpacing: 'line spacing', readingWidth: 'reading width', contrast: 'contrast', density: 'row density', motion: 'motion' }),
      ...opaque ? ['sidebar material'] : [],
    ]],
    ['Shortcuts', [
      ...shortcuts.enabled ? [] : ['app shortcuts are off'],
      ...customKeys.length ? [`custom keys for ${join.format(customKeys)}`] : [],
    ]],
    ['Notifications & sounds', [
      ...differing(current, defaultPreferences, { notifications: 'attention alerts' }),
      ...differing(notifications, defaultNotifications, { muted: 'mute', backgroundOnly: 'background only', volume: 'volume' }),
      ...notificationEvents.filter((event) => JSON.stringify(notifications.channels?.[event.id]) !== JSON.stringify(defaultNotifications.channels[event.id])).map((event) => `${event.label.toLocaleLowerCase()} alerts`),
    ]],
    ['Models & providers', defaultModel === 'aster' ? [] : [`new chats use ${modelChoices.find((choice) => choice.id === defaultModel)?.label ?? defaultModel}`]],
    ['Conversation behavior', differing(current.conversation, defaultConversation, { toolDisplay: 'tool details', showCompletedOutput: 'completed output', wrapCode: 'code wrapping', wrapDiff: 'diff wrapping', scrollAfterSend: 'scroll after sending' })],
    ['Context management', differing(current.contextPolicy, defaultContextPolicy, { autoCompact: 'automatic compaction', triggerPercent: 'compaction trigger', recentTurns: 'recent turns kept', recentTokens: 'recent tokens kept' })],
  ];
  return areas.filter(([, details]) => details.length).map(([area, details]) => {
    const detail = join.format(details);
    return { area, detail: detail.charAt(0).toLocaleUpperCase() + detail.slice(1) };
  });
}
