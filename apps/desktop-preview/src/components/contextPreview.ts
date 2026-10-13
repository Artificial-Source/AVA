import type { ContextProfile, ContextSnapshot, ContextUsageView } from '../chat/contextUsage';
import { modelOptions } from '../state/fixtures';
import type { Chat, ModelId } from '../state/types';

// Invented configured profiles, not real provider limits or effective input budgets.
const profiles: Record<ModelId, ContextProfile> = {
  aster: { id: 'aster-example-v1', providerId: 'aster-lab', providerLabel: 'Aster Lab', modelId: 'aster', modelLabel: modelOptions[0].label, windowTokens: 214000 },
  'aster-mini': { id: 'aster-mini-example-v1', providerId: 'aster-lab', providerLabel: 'Aster Lab', modelId: 'aster-mini', modelLabel: modelOptions[1].label, windowTokens: 128000 },
  orion: { id: 'orion-example-v1', providerId: 'orion-lab', providerLabel: 'Orion Lab', modelId: 'orion', modelLabel: modelOptions[2].label, windowTokens: 96000 },
};

// Read-only fictional facts shared by the separate screen-review adapter.
export function previewModelProfile(model: ModelId): Readonly<ContextProfile> {
  return profiles[model];
}

const asterSnapshot: ContextSnapshot = {
  profileId: 'aster-example-v1', providerId: 'aster-lab', modelId: 'aster', freshness: 'current', activeTokens: 173120, capturedAt: '18 Jun 2026, 10:42 UTC',
};
const miniSnapshot: ContextSnapshot = {
  profileId: 'aster-mini-example-v1', providerId: 'aster-lab', modelId: 'aster-mini', freshness: 'current', activeTokens: 48160, capturedAt: '18 Jun 2026, 10:38 UTC',
};
const snapshots: Readonly<Record<string, Partial<Record<ModelId, ContextSnapshot>>>> = {
  'search-filters': { aster: asterSnapshot, 'aster-mini': miniSnapshot },
  'keyboard-navigation': { 'aster-mini': miniSnapshot },
};

export function previewContextUsage(chat: Chat): ContextUsageView {
  return { profile: profiles[chat.model], snapshot: snapshots[chat.id]?.[chat.model] ?? null };
}
