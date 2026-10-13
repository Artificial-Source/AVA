// Supplied UI snapshots only. No tokenization, pricing, or transport interpretation.
export type ContextProfile = Readonly<{
  id: string;
  providerId: string;
  providerLabel: string;
  modelId: string;
  modelLabel: string;
  windowTokens: number | null;
}>;

export type ContextSnapshot = Readonly<{
  profileId: string;
  providerId: string;
  modelId: string;
  freshness: 'current' | 'stale';
  activeTokens: number | null;
  capturedAt?: string;
}>;

export type ContextUsageView = Readonly<{
  profile: ContextProfile;
  snapshot: ContextSnapshot | null;
}>;

export type CumulativeUsageView = Readonly<{
  input?: number | null;
  output?: number | null;
  reasoning?: number | null;
  cacheRead?: number | null;
  cacheWrite?: number | null;
  total?: number | null;
  cost?: Readonly<{ usd: number | null; completeness: 'complete' | 'partial' | 'unknown' }>;
  capturedAt?: string;
}>;

export function validTokenCount(value: number | null | undefined): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0;
}

export function contextUsage({ profile, snapshot }: ContextUsageView) {
  const windowTokens = validTokenCount(profile.windowTokens) && profile.windowTokens > 0 ? profile.windowTokens : null;
  const matches = snapshot !== null && snapshot.profileId === profile.id
    && snapshot.providerId === profile.providerId && snapshot.modelId === profile.modelId;
  const reason = !snapshot ? 'Unknown'
    : !matches ? 'Profile mismatch'
      : snapshot.freshness !== 'current' ? 'Stale snapshot'
        : !validTokenCount(snapshot.activeTokens) ? 'Unknown estimate'
          : windowTokens === null ? 'Unknown window' : null;
  const activeTokens = matches && snapshot.freshness === 'current' && validTokenCount(snapshot.activeTokens) ? snapshot.activeTokens : null;
  // Counts may exceed the configured window. Never clamp the displayed percentage.
  const percentage = activeTokens !== null && windowTokens !== null ? Math.round(activeTokens / windowTokens * 100) : null;
  return { windowTokens, activeTokens, percentage, reason };
}

export function formatTokenCount(value: number | null | undefined) {
  return validTokenCount(value) ? value.toLocaleString('en-US') : 'Unknown';
}

export function formatContextMeter(view: ContextUsageView) {
  const { activeTokens, percentage } = contextUsage(view);
  if (activeTokens === null || percentage === null) return 'Context unknown';
  const compact = activeTokens < 1000 ? String(activeTokens) : `${(activeTokens / 1000).toLocaleString('en-US', { maximumFractionDigits: 2 })}k`;
  return `${compact} (${percentage}%)`;
}
