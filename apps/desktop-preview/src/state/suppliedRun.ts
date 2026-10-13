import type { TurnState } from '../chat/contracts';
import type { Message, PreviewState } from './types';
import { projectSuppliedRun } from './suppliedRunProjection';

export type RunIdentity = Readonly<{ chatId: string; runId: string; generation: number }>;
export type SuppliedRun = RunIdentity & Readonly<{ messageId: string; sequence: number; state: TurnState }>;
export type RunEvent = RunIdentity & Readonly<{ sequence: number; kind: 'text' | 'tool' | 'output' | 'complete' | 'error'; [key: string]: unknown }>;
export type AssistantMessage = Extract<Message, { role: 'assistant' }>;

export function sameRun(a: RunIdentity, b: RunIdentity) {
  return a.chatId === b.chatId && a.runId === b.runId && a.generation === b.generation;
}
export function plain(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && Object.getPrototypeOf(value) === Object.prototype && Reflect.ownKeys(value).every((key) => typeof key === 'string' && 'value' in Object.getOwnPropertyDescriptor(value, key)!);
}
export function denseArray(value: unknown, max: number): value is unknown[] {
  if (!Array.isArray(value) || Object.getPrototypeOf(value) !== Array.prototype || value.length > max || Reflect.ownKeys(value).length !== value.length + 1) return false;
  for (let i = 0; i < value.length; i++) {
    const descriptor = Object.getOwnPropertyDescriptor(value, i);
    if (!descriptor || !Object.hasOwn(descriptor, 'value')) return false;
  }
  return true;
}
export function validId(value: unknown): value is string {
  return typeof value === 'string' && /^[a-zA-Z0-9_-]{1,128}$/.test(value) && !['__proto__', 'prototype', 'constructor'].includes(value);
}
export function replaceRunTurn(state: PreviewState, run: SuppliedRun, message: AssistantMessage): PreviewState {
  return { ...state, suppliedRun: run, chats: state.chats.map((chat) => chat.id !== run.chatId ? chat : { ...chat, status: run.state === 'streaming' ? 'working' : run.state === 'committed' ? 'completed' : run.state === 'failed' ? 'failed' : 'stopped', messages: chat.messages.map((entry) => entry.id === run.messageId ? message : entry) }) };
}
export function stopSuppliedRun(state: PreviewState): PreviewState {
  const run = state.suppliedRun;
  if (!run || run.state !== 'streaming') return state;
  const message = state.chats.find((chat) => chat.id === run.chatId)?.messages.find((entry) => entry.id === run.messageId);
  return message?.role === 'assistant' ? { ...replaceRunTurn(state, { ...run, state: 'canceled' }, { ...message, lifecycle: 'canceled' }), announcement: 'Run preview stopped. Partial response and supplied tool facts retained.' } : state;
}

export function receiveSuppliedRun(state: PreviewState, events: readonly RunEvent[]): PreviewState {
  const run = state.suppliedRun;
  if (!run || run.state !== 'streaming' || state.inspectedChildId || state.activeChatId !== run.chatId || !denseArray(events, 4096 - run.sequence) || !events.length) return state;
  for (let i = 0; i < events.length; i++) {
    const event = events[i];
    if (!plain(event) || !['chatId', 'runId', 'generation', 'sequence', 'kind'].every((key) => Object.hasOwn(event, key)) || !sameRun(run, event as RunEvent) || event.sequence !== run.sequence + i + 1 || !['text', 'tool', 'output', 'complete', 'error'].includes(event.kind as string) || i < events.length - 1 && (event.kind === 'complete' || event.kind === 'error')) return state;
  }
  const message = state.chats.find((chat) => chat.id === run.chatId)?.messages.find((entry) => entry.id === run.messageId);
  if (message?.role !== 'assistant') return state;
  const next = projectSuppliedRun(message, events);
  if (!next) return state;
  const kind = events.at(-1)!.kind;
  const phase = kind === 'complete' ? 'committed' : kind === 'error' ? 'failed' : 'streaming';
  return { ...replaceRunTurn(state, { ...run, sequence: run.sequence + events.length, state: phase }, { ...next, lifecycle: phase }), announcement: kind === 'complete' ? 'Run preview complete.' : kind === 'error' ? 'Run preview failed. Partial response retained.' : state.announcement };
}
