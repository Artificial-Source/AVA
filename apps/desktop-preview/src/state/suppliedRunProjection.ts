import type { ToolChangeView, ToolView } from '../chat/contracts';
import { denseArray, plain, validId, type AssistantMessage, type RunEvent } from './suppliedRun';

function shape(value: unknown, required: string[], optional: string[] = []): value is Record<string, unknown> {
  return plain(value) && required.every((key) => Object.hasOwn(value, key)) && Object.keys(value).every((key) => required.includes(key) || optional.includes(key));
}
function text(value: unknown, max: number): value is string { return typeof value === 'string' && value.length <= max; }
function changes(value: unknown): value is readonly ToolChangeView[] {
  if (!denseArray(value, 64)) return false;
  const ids = new Set(); const files = new Set();
  return value.every((change) => {
    if (!shape(change, ['id', 'filename', 'operation', 'diff'], ['additions', 'deletions']) || !validId(change.id) || ids.has(change.id) || !text(change.filename, 1024) || !change.filename.trim() || files.has(change.filename) || !['created', 'modified', 'deleted'].includes(change.operation as string) || ['additions', 'deletions'].some((key) => Object.hasOwn(change, key) && (!Number.isSafeInteger(change[key]) || (change[key] as number) < 0))) return false;
    ids.add(change.id); files.add(change.filename);
    return shape(change.diff, ['state', 'text', 'truncated']) && change.diff.state === 'text' && text(change.diff.text, 100000) && typeof change.diff.truncated === 'boolean' || shape(change.diff, ['state']) && ['binary', 'unavailable'].includes(change.diff.state as string);
  });
}
function tool(value: unknown): value is ToolView {
  return shape(value, ['id', 'action', 'detail', 'state'], ['resource', 'changes']) && validId(value.id) && text(value.action, 256) && !!value.action.trim() && text(value.detail, 2048) && ['queued', 'running', 'completed', 'failed', 'canceled', 'unavailable'].includes(value.state as string) && (!Object.hasOwn(value, 'resource') || shape(value.resource, ['kind', 'path']) && ['file', 'image'].includes(value.resource.kind as string) && text(value.resource.path, 1024) && !!value.resource.path.trim()) && (!Object.hasOwn(value, 'changes') || changes(value.changes));
}
const envelope = ['chatId', 'runId', 'generation', 'sequence', 'kind'];

// Pure, all-or-nothing projection. No payload object becomes mutable state by alias.
export function projectSuppliedRun(message: AssistantMessage, events: readonly RunEvent[]): AssistantMessage | null {
  let next = message;
  for (const event of events) {
    if (event.kind === 'text') {
      if (!shape(event, [...envelope, 'delta']) || !text(event.delta, 16384) || next.markdown.length + event.delta.length > 100000) return null;
      next = { ...next, markdown: next.markdown + event.delta };
    } else if (event.kind === 'tool') {
      if (!shape(event, [...envelope, 'tool']) || !tool(event.tool)) return null;
      const incoming = event.tool;
      const records = next.activity ?? [];
      const old = records.find((record) => record.id === incoming.id);
      if (!old && records.length === 64 || old && (['completed', 'failed', 'canceled', 'unavailable'].includes(old.state) && incoming.state !== old.state || old.state === 'running' && incoming.state === 'queued')) return null;
      const record: ToolView = { ...incoming, ...(incoming.resource ? { resource: { ...incoming.resource } } : {}), ...(incoming.changes ? { changes: incoming.changes.map((change) => ({ ...change, diff: { ...change.diff } })) } : {}), ...(old?.output ? { output: old.output } : {}) };
      const activity = old ? records.map((entry) => entry.id === record.id ? record : entry) : [...records, record];
      if (activity.reduce((sum, entry) => sum + (entry.changes?.reduce((size, change) => size + (change.diff.state === 'text' ? change.diff.text.length : 0), 0) ?? 0), 0) > 100000) return null;
      next = { ...next, activity };
    } else if (event.kind === 'output') {
      if (!shape(event, [...envelope, 'toolId', 'delta', 'truncated']) || !validId(event.toolId) || !text(event.delta, 16384) || typeof event.truncated !== 'boolean') return null;
      const record = next.activity?.find((entry) => entry.id === event.toolId);
      if (!record || (next.activity!.reduce((sum, entry) => sum + (entry.output?.text.length ?? 0), 0) + event.delta.length > 100000)) return null;
      const output = { text: (record.output?.text ?? '') + event.delta, truncated: event.truncated || record.output?.truncated === true };
      next = { ...next, activity: next.activity!.map((entry) => entry === record ? { ...entry, output } : entry) };
    } else if (event.kind === 'complete') {
      if (!shape(event, envelope) || next.activity?.some((entry) => entry.state === 'running' || entry.state === 'queued')) return null;
    } else if (event.kind === 'error') {
      if (!shape(event, [...envelope, 'error']) || !text(event.error, 2048) || !event.error.trim()) return null;
      next = { ...next, note: event.error };
    }
  }
  return next;
}
