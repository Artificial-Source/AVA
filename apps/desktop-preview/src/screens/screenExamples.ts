import { previewModelProfile } from '../components/contextPreview';
import type { ModelId } from '../state/types';
import { providerIdentities } from '../ui/providerIdentity';
import type { ModelChoice, Preferences, Provider, ResourceInspections, SubagentDefinition } from './contracts';

import { defaultAppearance, defaultGeneral, defaultConversation, defaultNotifications, defaultContextPolicy } from './behaviorPreferences';
import { defaultShortcuts } from './shortcuts';

// Shared local screen examples, separate from the standalone scenario index.
export const defaultPreferences: Preferences = { ...defaultAppearance, help: true, enter: 'send', notifications: 'off', general: defaultGeneral, conversation: defaultConversation, notificationPolicy: defaultNotifications, contextPolicy: defaultContextPolicy, shortcuts: defaultShortcuts };
const modelIds: readonly ModelId[] = ['aster', 'aster-mini', 'orion'];
const modelFamilies = {
  aster: { id: 'aster', label: 'Aster' },
  'aster-mini': { id: 'aster', label: 'Aster' },
  orion: { id: 'orion', label: 'Orion' },
} as const;
export const modelChoices: readonly ModelChoice[] = modelIds.map((id) => {
  const profile = previewModelProfile(id);
  return { id, modelId: id, label: profile.modelLabel, providerId: profile.providerId, providerLabel: profile.providerLabel, family: modelFamilies[id], windowTokens: profile.windowTokens ?? undefined,
    capabilities: id === 'orion' ? ['Text', 'Tools', 'Streaming'] : ['Text', 'Images', 'Tools', 'Streaming'], reasoning: id === 'aster-mini' ? [] : ['Low', 'Medium', 'High'] };
});
export const subagentDefinitions: readonly SubagentDefinition[] = [
  { id: 'builtin:general', revision: 1, source: 'builtin', name: 'general', description: 'General-purpose delegated work.', modelPreference: 'inherit' },
  { id: 'builtin:explore', revision: 1, source: 'builtin', name: 'explore', description: 'Read-only codebase exploration.', modelPreference: 'inherit' },
  { id: 'project:review-example', revision: 1, source: 'project', name: 'Project reviewer', description: 'Reviews changes against this project’s conventions.', modelPreference: 'inherit' },
  { id: 'user:1', revision: 1, source: 'user', name: 'Notes reviewer', description: 'Summarize open questions in a set of notes.', modelPreference: 'inherit' },
];
// Only MCP has entries; the other inventories are honestly empty.
export const resourceInspections: ResourceInspections = {
  skills: { state: 'empty', rows: [] },
  prompts: { state: 'empty', rows: [] },
  plugins: { state: 'empty', rows: [] },
  lsp: { state: 'empty', rows: [] },
  mcp: { state: 'ready', servers: [
    { id: 'notes-example', name: 'Workspace notes', transport: 'stdio', configEnabled: true, source: 'global', trust: 'not-required', connection: 'not-connected', lifecycle: 'one-shot' },
    { id: 'reference-example', name: 'Project reference', transport: 'stdio', configEnabled: false, source: 'project', trust: 'required', connection: 'unknown', lifecycle: 'one-shot' },
  ] },
};
export const providers: readonly Provider[] = [
  { id: 'aster-lab', label: 'Aster Lab', state: 'available', source: 'none', methods: ['key', 'browser', 'device'], description: 'Aster and Aster Mini models.' },
  { id: 'orion-lab', label: 'Orion Lab', state: 'connected-example', source: 'stored-example', methods: ['key'], description: 'Orion models.' },
  { id: 'cedar', label: 'Cedar', state: 'external', source: 'environment-example', methods: [], description: 'Credentials come from your environment.' },
  { id: 'harbor', label: 'Harbor', state: 'error', source: 'none', methods: ['key'], description: 'The API key expired.' },
  ...Object.entries(providerIdentities).map(([id, { label }]): Provider => ({ id, label, state: 'available', source: 'none', methods: ['key'], description: `${label} models.` })),
];
