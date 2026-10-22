import { previewModelProfile } from '../components/contextPreview';
import type { ModelId } from '../state/types';
import type { ModelChoice, Preferences, Provider, ResourceInspections, SubagentDefinition } from './contracts';

import { defaultGeneral, defaultConversation, defaultNotifications, defaultContextPolicy } from './behaviorPreferences';
import { defaultShortcuts } from './shortcuts';
import { defaultAppearance } from './AppearanceSettings';

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
  { id: 'project:review-example', revision: 1, source: 'project', name: 'Project reviewer', description: 'Supplied project-managed example · trust not evaluated.', modelPreference: 'inherit' },
  { id: 'user:1', revision: 1, source: 'user', name: 'Notes reviewer', description: 'Summarize open questions in a set of notes.', modelPreference: 'inherit' },
];
export const resourceInspections: ResourceInspections = {
  skills: { state: 'ready', rows: [{ title: 'Skill inventory', detail: 'No skill metadata supplied.', status: 'Unknown' }, { title: 'Project scope', detail: 'Project skills require trust.', status: 'Not evaluated' }] },
  prompts: { state: 'ready', rows: [{ title: 'Prompt inventory', detail: 'No prompt metadata supplied.', status: 'Unknown' }, { title: 'Prompt content', detail: 'Instruction bodies are not inspected here.', status: 'Not supplied' }] },
  plugins: { state: 'ready', rows: [{ title: 'Installed inventory', detail: 'No package inventory supplied.', status: 'Unknown' }, { title: 'Plugin processes', detail: 'No process status supplied.', status: 'Unknown' }] },
  lsp: { state: 'ready', rows: [{ title: 'Language servers', detail: 'No server inventory supplied.', status: 'Unknown' }, { title: 'Server status', detail: 'No server is started or queried by this page.', status: 'Not supplied' }] },
  mcp: { state: 'ready', servers: [
    { id: 'notes-example', name: 'Workspace notes', transport: 'stdio', configEnabled: true, source: 'global', trust: 'not-required', connection: 'not-connected', lifecycle: 'one-shot' },
    { id: 'reference-example', name: 'Project reference', transport: 'stdio', configEnabled: false, source: 'project', trust: 'required', connection: 'unknown', lifecycle: 'one-shot' },
  ] },
};
export const providers: readonly Provider[] = [
  { id: 'aster-lab', label: 'Aster Lab', state: 'available', source: 'none', methods: ['key', 'browser', 'device'], description: 'Fictional provider · Aster and Aster Mini example profiles.' },
  { id: 'orion-lab', label: 'Orion Lab', state: 'connected-example', source: 'stored-example', methods: ['key'], description: 'Connected EXAMPLE · stored dummy credential status, not a real connection.' },
  { id: 'cedar-example', label: 'Cedar Example', state: 'external', source: 'environment-example', methods: [], description: 'Externally managed example. Environment credentials cannot be removed here.' },
  { id: 'harbor-example', label: 'Harbor Example', state: 'error', source: 'none', methods: ['key'], description: 'Example connection error. No credential or catalog details were received.' },
];
