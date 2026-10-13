import { describe, expect, it } from 'vitest';
import { createInitialState } from './fixtures';
import { previewReducer } from './reducer';
import type { PreviewState } from './types';
import type { RunEvent, RunIdentity } from './suppliedRun';
import { suppliedSnapshots } from '../chat/suppliedRunFixture';

const identity: RunIdentity = { chatId: createInitialState().activeChatId, runId: 'opaque-1', generation: 1 };
const event = (sequence: number, kind: RunEvent['kind'] = 'text', fields = {}): RunEvent => ({ ...identity, sequence, kind, ...(kind === 'text' ? { delta: 'a' } : {}), ...fields });
const start = () => previewReducer(createInitialState(), { type: 'startSuppliedRun', identity });
const receive = (state: PreviewState, events: readonly RunEvent[]) => previewReducer(state, { type: 'receiveSuppliedRun', events });
const message = (state: PreviewState) => state.chats.find((chat) => chat.id === identity.chatId)!.messages.at(-1)!;
const tool = { id: 'one', action: 'Read', detail: 'Supplied file', state: 'running' };
function frozen<T>(value: T): T { if (value && typeof value === 'object') { Object.values(value).forEach(frozen); Object.freeze(value); } return value; }

describe('supplied-run core authority and bounded projection', () => {
  it('only starts an explicit selected-parent run with immutable supplied attribution, never from Send or composer facts', () => {
    let state = createInitialState(); const before = state.chats[0];
    state = previewReducer(state, { type: 'setModel', chatId: identity.chatId, model: 'orion' });
    state = previewReducer(state, { type: 'setMode', chatId: identity.chatId, mode: 'plan' });
    state = previewReducer(state, { type: 'sendLocalMessage' });
    expect(message(state)).toMatchObject({ role: 'user', origin: 'local', selectedModel: { id: 'orion' } });
    expect(state.suppliedRun).toBeUndefined();
    const begun = previewReducer(state, { type: 'startSuppliedRun', identity });
    expect(message(begun)).toMatchObject({ role: 'assistant', markdown: '', lifecycle: 'streaming', attribution: { modelLabel: 'Aster', thinkingLabel: 'High' } });
    expect(begun.chats[0].changes).toBe(before.changes); expect(begun.chats[0].model).toBe('orion'); expect(begun.chats[0].mode).toBe('plan');
    expect(previewReducer(begun, { type: 'startSuppliedRun', identity: { ...identity, generation: 2, runId: 'new' } })).toBe(begun);
  });
  it.each([
    { chatId: 'wrong' }, { runId: '__proto__' }, { runId: 'constructor' }, { runId: '' }, { runId: 'a'.repeat(129) }, { generation: NaN }, { generation: Infinity }, { generation: 0 }, { generation: 1.5 }, { generation: 2 }, { extra: true },
  ])('rejects malformed or unauthorised start %j', (fields) => {
    const state = createInitialState(); expect(previewReducer(state, { type: 'startSuppliedRun', identity: { ...identity, ...fields } })).toBe(state);
  });
  it('requires own ordinary identity properties and blocks working/child parents', () => {
    const state = createInitialState(); const inherited = Object.create(identity);
    expect(previewReducer(state, { type: 'startSuppliedRun', identity: inherited })).toBe(state);
    const child = previewReducer(state, { type: 'inspectChild', childId: state.childAgents[0].id });
    expect(previewReducer(child, { type: 'startSuppliedRun', identity })).toBe(child);
    const working = state.chats.find((chat) => chat.status === 'working')!;
    const selected = previewReducer(state, { type: 'viewChat', chatId: working.id });
    expect(previewReducer(selected, { type: 'startSuppliedRun', identity: { ...identity, chatId: working.id } })).toBe(selected);
  });
  it.each([
    { sequence: 0 }, { sequence: 2 }, { sequence: NaN }, { sequence: 1.1 }, { chatId: 'wrong' }, { runId: 'wrong' }, { generation: 2 }, { generation: NaN }, { kind: 'unknown' }, { delta: 12 }, { delta: 'a'.repeat(16385) }, { unexpected: true },
  ])('rejects %j with no partial batch mutation', (fields) => {
    const state = frozen(start()); const before = JSON.stringify(state);
    expect(receive(state, [event(1, 'text', fields)])).toBe(state);
    expect(receive(state, [event(1), event(2, 'text', { ...fields, sequence: 3 })])).toBe(state);
    expect(JSON.stringify(state)).toBe(before);
  });
  it('rejects empty, inherited, accessor and missing-own payloads; accepts exact next sequence only', () => {
    const state = start();
    const accessor = Object.defineProperty([event(1)], '0', { get() { throw new Error('must not read array accessors'); } });
    for (const value of [[], [Object.create(event(1))], [{ ...event(1), get delta() { throw new Error('must not read'); } }], [null], Array(1), accessor, Object.assign([event(1)], { extra: true }), Object.setPrototypeOf([event(1)], null)]) expect(receive(state, value as RunEvent[])).toBe(state);
    const next = receive(state, [event(1)]); expect(next).not.toBe(state);
    expect(receive(next, [event(1)])).toBe(next); expect(receive(next, [event(3)])).toBe(next);
    expect(receive(createInitialState(), [event(1)])).toEqual(createInitialState());
  });
  it('is lossless for 1000 ordered deltas, split UTF-16 pairs, Markdown and fences; unchanged history stays referentially stable', () => {
    const state = frozen(start());
    const deltas = ['**split', ' Markdown**\n```ts\n', 'const icon = "\ud83d', '\ude80";\n```\n', ...Array.from({ length: 1000 }, (_, i) => `${i},`)];
    const next = receive(state, deltas.map((delta, i) => event(i + 1, 'text', { delta })));
    expect(message(next)).toMatchObject({ markdown: deltas.join('') });
    expect(next.chats[0].messages[0]).toBe(state.chats[0].messages[0]); expect(next.chats[1]).toBe(state.chats[1]);
    expect(next.suppliedRun?.sequence).toBe(1004);
  });
  it('caps event count at 4096 and total text at 100000 UTF-16 units without truncating storage', () => {
    const state = start(); expect(receive(state, Array.from({ length: 4097 }, (_, i) => event(i + 1)))).toBe(state);
    const full = receive(state, Array.from({ length: 4095 }, (_, i) => event(i + 1))); const ended = receive(full, [event(4096, 'complete')]);
    expect(ended.suppliedRun).toMatchObject({ state: 'committed', sequence: 4096 }); expect(receive(ended, [event(4097)])).toBe(ended);
    const filled = receive(state, Array.from({ length: 10 }, (_, i) => event(i + 1, 'text', { delta: 'x'.repeat(10000) })));
    expect(message(filled)).toMatchObject({ markdown: 'x'.repeat(100000) }); expect(receive(filled, [event(11)])).toBe(filled);
  });
  it('validates tools, duplicate identities, bounded aggregate output/diffs and copies supplied objects', () => {
    let state = start(); const payload = { ...tool, resource: { kind: 'file', path: 'src/example.ts' }, changes: [{ id: 'change', filename: 'file.ts', operation: 'modified', additions: 0, diff: { state: 'text', text: '+new', truncated: false } }] };
    state = receive(state, [event(1, 'tool', { tool: payload })]); payload.resource.path = 'changed'; payload.changes[0].diff.text = 'changed';
    expect(message(state)).toMatchObject({ activity: [{ resource: { path: 'src/example.ts' }, changes: [{ diff: { text: '+new' }, additions: 0 }] }] });
    for (const bad of [{ ...tool, id: '__proto__' }, { ...tool, state: 'bad' }, { ...tool, action: '' }, { ...tool, resource: { kind: 'file', path: '' } }, { ...tool, resource: Object.create({ kind: 'file', path: 'x' }) }, { ...tool, changes: [{ id: 'c', filename: 'x', operation: 'modified', additions: -1, diff: { state: 'binary' } }] }, { ...tool, changes: Array(65).fill(payload.changes[0]) }, { ...tool, changes: [payload.changes[0], payload.changes[0]] }]) expect(receive(state, [event(2, 'tool', { tool: bad })])).toBe(state);
    expect(receive(state, [event(2, 'output', { toolId: 'missing', delta: 'bad', truncated: false })])).toBe(state);
    const full = receive(state, Array.from({ length: 10 }, (_, i) => event(i + 2, 'output', { toolId: 'one', delta: 'x'.repeat(10000), truncated: i === 0 })));
    expect(message(full)).toMatchObject({ activity: [{ output: { text: 'x'.repeat(100000), truncated: true } }] });
    expect(receive(full, [event(12, 'output', { toolId: 'one', delta: 'x', truncated: false })])).toBe(full);
    expect(receive(state, [event(2, 'output', { toolId: 'one', delta: 'x'.repeat(16385), truncated: false })])).toBe(state);
    expect(receive(state, [event(2, 'tool', { tool: { ...tool, changes: [{ id: 'c', filename: 'x', operation: 'created', diff: { state: 'text', text: 'x'.repeat(100001), truncated: false } }] } })])).toBe(state);
  });
  it('caps 64 tools, retains same-ID activity order/output on updates and disallows tool revival', () => {
    const state = start(); const full = receive(state, Array.from({ length: 64 }, (_, i) => event(i + 1, 'tool', { tool: { ...tool, id: `tool-${i}` } })));
    expect(receive(full, [event(65, 'tool', { tool: { ...tool, id: 'new' } })])).toBe(full);
    const completed = receive(full, [event(65, 'output', { toolId: 'tool-0', delta: 'saved', truncated: false }), event(66, 'tool', { tool: { ...tool, id: 'tool-0', state: 'completed' } })]);
    expect(message(completed)).toMatchObject({ activity: [{ id: 'tool-0', output: { text: 'saved' } }, ...Array.from({ length: 63 }, (_, i) => ({ id: `tool-${i + 1}` }))] });
    expect(receive(completed, [event(67, 'tool', { tool: { ...tool, id: 'tool-0' } })])).toBe(completed);
    expect(receive(completed, [event(67, 'complete')])).toBe(completed);
  });
  it.each(['complete', 'error'] as const)('makes %s terminal, rejects partial-terminal batches/revival, and requires new retry identity', (kind) => {
    const state = receive(start(), [event(1)]); const terminal = event(2, kind, kind === 'error' ? { error: 'Supplied failure' } : {});
    expect(receive(state, [terminal, event(3)])).toBe(state);
    const ended = receive(state, [terminal]); expect(ended.suppliedRun?.state).toBe(kind === 'complete' ? 'committed' : 'failed');
    expect(receive(ended, [event(3)])).toBe(ended);
    expect(previewReducer(ended, { type: 'startSuppliedRun', identity })).toBe(ended);
    expect(previewReducer(ended, { type: 'startSuppliedRun', identity: { ...identity, generation: 2 } })).toBe(ended);
    const retry = previewReducer(ended, { type: 'startSuppliedRun', identity: { ...identity, generation: 2, runId: 'new-run' } });
    expect(retry.suppliedRun?.messageId).not.toBe(ended.suppliedRun?.messageId); expect(receive(retry, [event(3)])).toBe(retry);
  });
  it.each(['stop', 'view', 'new', 'child'])('atomically cancels on %s, retains partials and supplied tool facts, rejects late events', (departure) => {
    const state = receive(start(), [event(1), event(2, 'tool', { tool })]);
    const next = previewReducer(state, departure === 'stop' ? { type: 'stopSuppliedRun', identity } : departure === 'view' ? { type: 'viewChat', chatId: state.chats[1].id } : departure === 'new' ? { type: 'newChat' } : { type: 'inspectChild', childId: state.childAgents[0].id });
    expect(next.suppliedRun?.state).toBe('canceled'); expect(message(next)).toMatchObject({ markdown: 'a', lifecycle: 'canceled', activity: [tool] });
    expect(receive(next, [event(3)])).toBe(next);
    expect(previewReducer(state, { type: 'stopSuppliedRun', identity: { ...identity, runId: 'other' } })).toBe(state);
    expect(previewReducer(state, { type: 'viewChat', chatId: 'missing' })).toBe(state);
  });
  it('keeps 1000 burst text increments and losslessly appends bounded Check output in two stages before completion', () => {
    const snapshots = frozen(suppliedSnapshots(identity, 'burst'));
    const events = snapshots.flat(); const firstBurst = snapshots.at(-3)!; const remainingOutput = snapshots.at(-2)!;
    const burst = [...firstBurst, ...remainingOutput];
    expect(snapshots).toHaveLength(8);
    expect(snapshots.slice(0, -3)).toEqual(suppliedSnapshots(identity, 'completion').slice(0, -1));
    expect(firstBurst.filter((entry) => entry.kind === 'text')).toHaveLength(1000);
    expect(firstBurst.filter((entry) => entry.kind === 'output')).toHaveLength(50);
    expect(remainingOutput).toHaveLength(550);
    expect(remainingOutput.every((entry) => entry.kind === 'output')).toBe(true);
    expect(burst.filter((entry) => entry.kind === 'text')).toHaveLength(1000);
    const output = burst.filter((entry) => entry.kind === 'output');
    expect(output).toHaveLength(600);
    expect(burst).toHaveLength(1600);
    expect(events.length).toBeLessThanOrEqual(4096);
    events.forEach((entry, index) => {
      expect(entry).toMatchObject({ ...identity, sequence: index + 1 });
      if (entry.kind === 'text' || entry.kind === 'output') {
        expect(typeof entry.delta).toBe('string');
        expect((entry.delta as string).length).toBeLessThanOrEqual(16384);
      }
    });
    output.forEach((entry) => {
      expect(entry).toMatchObject({ toolId: 'check', truncated: false });
      expect(entry.delta).toMatch(/^Supplied row \d+: .*\n$/);
    });
    let state = start();
    for (const snapshot of snapshots.slice(0, -3)) state = receive(state, snapshot);
    const runningCheck = { activity: expect.arrayContaining([expect.objectContaining({ id: 'check', state: 'running' })]) };
    expect(message(state)).toMatchObject(runningCheck);
    state = receive(state, firstBurst);
    expect(state.suppliedRun).toMatchObject({ state: 'streaming', sequence: firstBurst.at(-1)!.sequence });
    expect(message(state)).toMatchObject(runningCheck);
    const firstResponse = message(state);
    if (firstResponse.role !== 'assistant') throw new Error('Expected the supplied assistant response');
    const firstOutput = firstResponse.activity!.find((entry) => entry.id === 'check')!.output!;
    expect(firstOutput.truncated).toBe(false);
    expect(firstOutput.text.split('\n').filter(Boolean)).toHaveLength(52);
    expect(firstOutput.text.length).toBeGreaterThan(2000);
    expect(firstOutput.text.length).toBeLessThan(20000);
    expect(firstOutput.text).toBe(snapshots.slice(0, -2).flat().filter((entry) => entry.kind === 'output' && entry.toolId === 'check').map((entry) => entry.delta).join(''));
    state = receive(state, remainingOutput);
    expect(state.suppliedRun).toMatchObject({ state: 'streaming', sequence: remainingOutput.at(-1)!.sequence });
    expect(message(state)).toMatchObject(runningCheck);
    state = receive(state, snapshots.at(-1)!);
    expect(state.suppliedRun).toMatchObject({ state: 'committed', sequence: events.length });
    const response = message(state);
    if (response.role !== 'assistant') throw new Error('Expected the supplied assistant response');
    expect(response.markdown).toBe(events.filter((entry) => entry.kind === 'text').map((entry) => entry.delta).join(''));
    const check = response.activity!.find((entry) => entry.id === 'check')!;
    expect(check).toMatchObject({ state: 'completed', output: { truncated: false } });
    expect(check.output!.text).toBe(events.filter((entry) => entry.kind === 'output' && entry.toolId === 'check').map((entry) => entry.delta).join(''));
    expect(check.output!.text.split('\n').length).toBeGreaterThan(200);
    expect(check.output!.text.length).toBeGreaterThan(20000);
    const storedOutput = response.activity!.reduce((sum, entry) => sum + (entry.output?.text.length ?? 0), 0);
    expect(storedOutput).toBeLessThan(100000);
    expect(storedOutput).toBe(events.filter((entry) => entry.kind === 'output').reduce((sum, entry) => sum + (entry.delta as string).length, 0));
  });
  it.each(['completion', 'failure', 'burst'] as const)('projects the actual finite %s fixture through all core guards', (example) => {
    let state = start();
    for (const events of suppliedSnapshots(identity, example)) { const next = receive(state, events); expect(next).not.toBe(state); state = next; }
    expect(state.suppliedRun?.state).toBe(example === 'failure' ? 'failed' : 'committed');
  });
});
