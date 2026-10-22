import type { ChatMode, InspectorModule } from '../state/types';
import type { ToolDisplay } from '../chat/toolDisplay';
import { contextUsage, type ContextUsageView } from '../chat/contextUsage';
import type { Preferences } from './contracts';

// Visit-local browser preferences, never AVA configuration or execution policy.
export type GeneralPreferences = Readonly<{ mode: ChatMode; projectId: string; titleFromFirstMessage: boolean; inspector: 'remember' | 'closed' | InspectorModule }>;
export type ConversationPreferences = Readonly<{ toolDisplay: ToolDisplay; showCompletedOutput: boolean; wrapCode: boolean; wrapDiff: boolean; scrollAfterSend: boolean }>;
export type NotificationEvent = 'completion' | 'input' | 'failure';
export type PreviewTone = 'none' | 'soft' | 'chime';
export type NotificationPreferences = Readonly<{ muted: boolean; backgroundOnly: boolean; volume: number; channels: Readonly<Record<NotificationEvent, Readonly<{ enabled: boolean; tone: PreviewTone }>>> }>;
export type ContextPolicy = Readonly<{ autoCompact: boolean; triggerPercent: number; recentTurns: number; recentTokens: number }>;
export const defaultGeneral: GeneralPreferences = { mode: 'build', projectId: 'current', titleFromFirstMessage: false, inspector: 'remember' };
export const defaultConversation: ConversationPreferences = { toolDisplay: 'compact', showCompletedOutput: true, wrapCode: false, wrapDiff: true, scrollAfterSend: true };
export const defaultNotifications: NotificationPreferences = { muted: false, backgroundOnly: true, volume: 30, channels: { completion: { enabled: true, tone: 'soft' }, input: { enabled: true, tone: 'chime' }, failure: { enabled: true, tone: 'chime' } } };
export const defaultContextPolicy: ContextPolicy = { autoCompact: true, triggerPercent: 80, recentTurns: 2, recentTokens: 20000 };
export type AppearanceKey = 'palette' | 'uiFont' | 'proseSize' | 'proseSpacing' | 'readingWidth' | 'contrast' | 'density' | 'motion';
export const defaultAppearance: Required<Pick<Preferences, AppearanceKey>> = {
  palette: 'default', uiFont: 'geist', proseSize: 'normal', proseSpacing: 'normal', readingWidth: 'wide', contrast: 'system', density: 'comfortable', motion: 'system',
};
export const notificationEvents: readonly { id: NotificationEvent; label: string; description: string }[] = [
  { id: 'completion', label: 'Task finished', description: 'When AVA finishes a task.' },
  { id: 'input', label: 'Needs attention', description: 'When AVA needs your answer or approval.' },
  { id: 'failure', label: 'Task failed', description: 'When a task stops with an error.' },
];
export function boundedInteger(value: number, min: number, max: number) { return Number.isSafeInteger(value) && value >= min && value <= max; }
// The tone an event's preview may play. Mute, zero/invalid volume and unknown tones stay silent.
export function previewTone(preferences: NotificationPreferences, event: NotificationEvent): PreviewTone {
  const tone = preferences.channels[event]?.tone;
  if (preferences.muted || !boundedInteger(preferences.volume, 0, 100) || preferences.volume === 0) return 'none';
  return tone === 'soft' || tone === 'chime' ? tone : 'none';
}
export function contextProjection(policy: ContextPolicy, view?: ContextUsageView) {
  const usage = view ? contextUsage(view) : { activeTokens: null, windowTokens: null, percentage: null, reason: 'Unknown model' };
  const valid = typeof policy.autoCompact === 'boolean' && boundedInteger(policy.triggerPercent, 1, 95) && boundedInteger(policy.recentTurns, 0, 20) && boundedInteger(policy.recentTokens, 0, 100000);
  // Exact floor(window * percent / 100), without overflowing the safe-integer product.
  const threshold = valid && usage.windowTokens !== null ? Math.floor(usage.windowTokens / 100) * policy.triggerPercent + Math.floor(usage.windowTokens % 100 * policy.triggerPercent / 100) : null;
  const eligible = !valid || threshold === null || usage.activeTokens === null ? null : policy.autoCompact && usage.activeTokens >= threshold;
  return { ...usage, threshold, eligible, valid };
}
