import { StrictMode, type ReactNode } from 'react';
import { act, renderHook } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import type { SubagentIntent } from './contracts';
import { applySubagentIntent, applySubagentRemoval, initialSubagentWorkspace, subagentErrors, useSubagentWorkspace } from './subagentWorkspace';

const create: SubagentIntent = { version: 1, definitionId: null, revision: null, name: '  Release notes  ', description: '  Review the public release summary.  ', modelPreference: 'orion' };
const edit: SubagentIntent = { ...create, definitionId: 'user:1', revision: 1 };

describe('local user-definition metadata ownership', () => {
  it('creates and edits only bounded metadata with stable IDs and monotonic revisions, without changing the input', () => {
    const initial = initialSubagentWorkspace(); const before = JSON.stringify(initial);
    const created = applySubagentIntent(initial, create)!;
    expect(created.version).toBe(2); expect(created.nextUserId).toBe(3);
    expect(created.definitions.at(-1)).toEqual({ id: 'user:2', source: 'user', revision: 1, name: 'Release notes', description: 'Review the public release summary.', modelPreference: 'orion' });
    expect(applySubagentIntent(created, create)).toBeNull();
    const updated = applySubagentIntent(created, { ...create, version: 2, definitionId: 'user:2', revision: 1, name: 'Release review', modelPreference: 'inherit' })!;
    expect(updated.definitions.at(-1)).toMatchObject({ id: 'user:2', revision: 2, modelPreference: 'inherit' });
    expect(updated.nextUserId).toBe(3); expect(updated.definitions).toHaveLength(5);
    expect(JSON.stringify(initial)).toBe(before);
    expect(initialSubagentWorkspace(updated)).toEqual(updated);
    expect(initialSubagentWorkspace(updated)).not.toBe(updated);
    expect(initialSubagentWorkspace(updated).definitions[0]).not.toBe(updated.definitions[0]);
  });
  it.each([
    { name: '' }, { name: '   ' }, { name: 'A'.repeat(65) }, { description: '' }, { description: '  ' }, { description: 'x'.repeat(513) },
    { name: 'GENERAL' }, { name: 'Notes reviewer' }, { modelPreference: 'openai' }, { modelPreference: '__proto__' },
    { definitionId: 'builtin:general', revision: 1 }, { definitionId: 'builtin:explore', revision: 1 }, { definitionId: 'project:review-example', revision: 1 },
    { definitionId: 'user:unknown', revision: 1 }, { definitionId: 'user:1', revision: 0 }, { version: 0 }, { revision: 1 },
  ])('rejects malformed, duplicate, stale or read-only mutation %j', (patch) => {
    expect(applySubagentIntent(initialSubagentWorkspace(), { ...create, ...patch } as SubagentIntent)).toBeNull();
  });
  it('accepts exact bounds and own-name edits, excludes extra fields, and never stores a model policy', () => {
    const initial = initialSubagentWorkspace();
    const updated = applySubagentIntent(initial, { ...edit, name: 'n'.repeat(64), description: 'd'.repeat(512), modelPreference: 'inherit', model: 'orion', path: '/not-adopted', systemPrompt: 'not adopted', source: 'builtin' } as SubagentIntent)!;
    expect(updated.definitions.at(-1)).toEqual({ id: 'user:1', source: 'user', revision: 2, name: 'n'.repeat(64), description: 'd'.repeat(512), modelPreference: 'inherit' });
    expect(subagentErrors({ ...edit, name: 'Notes reviewer' }, initial.definitions, 'user:1').name).toBe('');
  });
  it('fails closed on malformed snapshots and never seeds altered built-in/project metadata', () => {
    const initial = initialSubagentWorkspace();
    for (const seed of [null, [], {}, { ...initial, version: -1 }, { ...initial, nextUserId: 1 }, { ...initial, definitions: [null] }, { ...initial, definitions: [{ id: 'user:1' }] }, { ...initial, definitions: initial.definitions.slice(1) }, { ...initial, definitions: [...initial.definitions, initial.definitions[3]] }, { ...initial, definitions: initial.definitions.map((entry) => ({ ...entry, modelPreference: 'orion' })) }]) {
      expect(initialSubagentWorkspace(seed)).toEqual(initial);
    }
  });
  it('removes only a current user definition, keeps ids monotonic and refuses built-in, project, stale or forged removals', () => {
    const initial = initialSubagentWorkspace();
    for (const forged of [{ definitionId: 'builtin:general', revision: 1 }, { definitionId: 'project:review-example', revision: 1 }, { definitionId: 'user:1', revision: 2 }, { definitionId: 'user:9', revision: 1 }, { version: 2 }, { remove: false }]) {
      expect(applySubagentRemoval(initial, { remove: true, version: 1, definitionId: 'user:1', revision: 1, ...forged } as Parameters<typeof applySubagentRemoval>[1])).toBeNull();
    }
    const removed = applySubagentRemoval(initial, { remove: true, version: 1, definitionId: 'user:1', revision: 1 })!;
    expect(removed.version).toBe(2); expect(removed.nextUserId).toBe(initial.nextUserId); expect(removed.definitions.map((entry) => entry.id)).not.toContain('user:1');
    expect(initialSubagentWorkspace(removed)).toEqual(removed);
    expect(applySubagentIntent(removed, { ...create, version: 2 })!.definitions.at(-1)!.id).toBe('user:2');
    const host = renderHook(() => useSubagentWorkspace(undefined, true));
    act(() => expect(host.result.current.save({ remove: true, version: 1, definitionId: 'user:1', revision: 1 })).toBe(true));
    expect(host.result.current.workspace.definitions.some((entry) => entry.id === 'user:1')).toBe(false);
  });
  it('guards duplicate, latest, inactive, closed and unmounted callbacks; reopening uses only an explicit opaque seed', () => {
    const nextEdit = { ...edit, version: 2, name: 'Updated notes' };
    const onSnapshot = vi.fn(); const wrapper = ({ children }: { children: ReactNode }) => <StrictMode>{children}</StrictMode>;
    const host = renderHook(({ active }) => useSubagentWorkspace(undefined, active, onSnapshot), { initialProps: { active: true }, wrapper });
    const first = host.result.current.save;
    act(() => { expect(first(create)).toBe(true); expect(first(create)).toBe(false); });
    expect(onSnapshot).toHaveBeenCalledTimes(1);
    const latest = host.result.current.save;
    act(() => { expect(first(nextEdit)).toBe(false); });
    host.rerender({ active: false });
    act(() => { expect(latest(nextEdit)).toBe(false); expect(host.result.current.save(nextEdit)).toBe(false); });
    host.rerender({ active: true });
    act(() => { expect(latest(nextEdit)).toBe(false); });
    const reopened = host.result.current.save; const snapshot = host.result.current.workspace;
    host.unmount(); act(() => expect(reopened(nextEdit)).toBe(false));
    expect(onSnapshot).toHaveBeenCalledTimes(1);
    const next = renderHook(() => useSubagentWorkspace(snapshot, true));
    expect(next.result.current.workspace).toEqual(snapshot);
    act(() => expect(next.result.current.save({ ...edit, version: 2, name: 'Updated notes' })).toBe(true));
    expect(next.result.current.workspace.version).toBe(3);
  });
  it('keeps independently mounted workspaces separate and explicit reset invalidates prior callbacks', () => {
    const one = renderHook(() => useSubagentWorkspace(undefined, true)); const two = renderHook(() => useSubagentWorkspace(undefined, true));
    act(() => { one.result.current.save(create); });
    expect(two.result.current.workspace).toEqual(initialSubagentWorkspace());
    const old = one.result.current.save; const oldReset = one.result.current.reset;
    act(() => { oldReset(); oldReset(); });
    expect(one.result.current.workspace.definitions).toEqual(initialSubagentWorkspace().definitions);
    act(() => { expect(old({ ...edit, version: 2 })).toBe(false); oldReset(); });
    expect(one.result.current.workspace.version).toBe(3);
  });
});
