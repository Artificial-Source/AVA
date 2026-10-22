import { useState } from 'react';
import type { SubagentDefinition, SubagentDraft, SubagentIntent, SubagentWorkspace } from './contracts';
import { subagentDefinitions } from './screenExamples';
import { useViewGuard } from './useViewGuard';

export const subagentLimits = { name: 64, description: 512 } as const;
export function subagentErrors(draft: SubagentDraft, definitions: readonly SubagentDefinition[], editingId: string | null) {
  const name = typeof draft.name === 'string' ? draft.name.trim() : '';
  return {
    name: !name || draft.name.length > subagentLimits.name ? 'Enter a name of 1–64 characters.' : definitions.some((entry) => entry.id !== editingId && typeof entry.name === 'string' && entry.name.toLocaleLowerCase() === name.toLocaleLowerCase()) ? 'A definition with this name already exists.' : '',
    description: typeof draft.description !== 'string' || !draft.description.trim() || draft.description.length > subagentLimits.description ? 'Enter a description of 1–512 characters.' : '',
    modelPreference: !['inherit', 'aster', 'aster-mini', 'orion'].includes(draft.modelPreference) ? 'Choose a supplied model preference.' : '',
  };
}
const positive = (value: unknown): value is number => Number.isSafeInteger(value) && (value as number) > 0 && (value as number) < Number.MAX_SAFE_INTEGER;
function validWorkspace(value: unknown): value is SubagentWorkspace {
  if (!value || typeof value !== 'object') return false;
  const view = value as SubagentWorkspace;
  if (!positive(view.version) || !positive(view.nextUserId) || !Array.isArray(view.definitions)) return false;
  const ids = new Set<string>();
  for (const entry of view.definitions) {
    if (!entry || typeof entry.id !== 'string' || ids.has(entry.id) || !positive(entry.revision) || Object.values(subagentErrors(entry, view.definitions.filter(Boolean), entry.id)).some(Boolean)) return false;
    ids.add(entry.id);
    if (entry.source === 'user') {
      const number = Number(entry.id.slice(5));
      if (entry.id !== `user:${number}` || !positive(number) || number >= view.nextUserId) return false;
    } else {
      const original = subagentDefinitions.find((item) => item.id === entry.id && item.source !== 'user');
      if (!original || Object.keys(original).some((key) => original[key as keyof SubagentDefinition] !== entry[key as keyof SubagentDefinition])) return false;
    }
  }
  return subagentDefinitions.filter((entry) => entry.source !== 'user').every((entry) => ids.has(entry.id));
}
function copyDefinition({ id, revision, source, name, description, modelPreference }: SubagentDefinition): SubagentDefinition {
  return { id, revision, source, name, description, modelPreference };
}
export function initialSubagentWorkspace(seed?: unknown): SubagentWorkspace {
  // The AppShell retains this opaque snapshot only. Validation/defaults stay in the lazy owner.
  return validWorkspace(seed) ? { version: seed.version, nextUserId: seed.nextUserId, definitions: seed.definitions.map(copyDefinition) } : { version: 1, nextUserId: 2, definitions: subagentDefinitions.map(copyDefinition) };
}
export function applySubagentIntent(workspace: SubagentWorkspace, intent: SubagentIntent): SubagentWorkspace | null {
  if (!validWorkspace(workspace) || intent.version !== workspace.version || Object.values(subagentErrors(intent, workspace.definitions, intent.definitionId)).some(Boolean)) return null;
  const draft: SubagentDraft = { name: intent.name.trim(), description: intent.description.trim(), modelPreference: intent.modelPreference };
  if (intent.definitionId === null) {
    if (intent.revision !== null) return null;
    return { version: workspace.version + 1, nextUserId: workspace.nextUserId + 1, definitions: [...workspace.definitions, { ...draft, id: `user:${workspace.nextUserId}`, revision: 1, source: 'user' }] };
  }
  const definition = workspace.definitions.find((entry) => entry.id === intent.definitionId);
  if (!definition || definition.source !== 'user' || definition.revision !== intent.revision) return null;
  return { ...workspace, version: workspace.version + 1, definitions: workspace.definitions.map((entry) => entry === definition ? { ...entry, ...draft, revision: entry.revision + 1 } : entry) };
}

export function useSubagentWorkspace(seed: unknown, active: boolean, onSnapshot?: (snapshot: unknown) => void) {
  const [workspace, setWorkspace] = useState(() => initialSubagentWorkspace(seed));
  const gate = useViewGuard(JSON.stringify([workspace.version, active]));
  return {
    workspace,
    save(intent: SubagentIntent) {
      if (!active || !gate.valid()) return false;
      const next = applySubagentIntent(workspace, intent);
      if (!next || !gate.claim()) return false;
      setWorkspace(next); onSnapshot?.(next);
      return true;
    },
    // Only the standalone review's explicit Reset all confirmation uses this.
    reset() {
      if (!gate.claim()) return;
      gate.invalidate(); setWorkspace({ ...initialSubagentWorkspace(), version: workspace.version + 1 });
    },
  };
}
