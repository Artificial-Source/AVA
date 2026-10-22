import { useSyncExternalStore } from 'react';
import type { Provider } from './contracts';
import { providers as providerExamples } from './screenExamples';

// Visit-lifetime provider connections. Every surface that lists providers or their models
// (Connect a provider, Settings → Models & providers, the model picker) reads this one
// store, so a connection made in one dialog is still there when another opens.
export type ProviderConnection = Pick<Provider, 'state' | 'source'>;

// The models the starting chats use come from these providers, so they start connected.
const connectedAtStart: Readonly<Record<string, true>> = { 'aster-lab': true, 'orion-lab': true };
function startingList(): readonly Provider[] {
  return providerExamples.map((provider): Provider => Object.hasOwn(connectedAtStart, provider.id) ? { ...provider, state: 'connected-example', source: 'stored-example' } : provider);
}

let current = startingList();
const listeners = new Set<() => void>();
function publish(next: readonly Provider[]) {
  current = next;
  for (const listener of [...listeners]) listener();
}
function subscribe(listener: () => void) {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}

export function getProviderList(): readonly Provider[] { return current; }
export function useProviderList(): readonly Provider[] { return useSyncExternalStore(subscribe, getProviderList, getProviderList); }
export function setProviderState(id: string, connection: ProviderConnection) {
  const target = current.find((provider) => provider.id === id);
  if (!target || target.state === connection.state && target.source === connection.source) return;
  publish(current.map((provider) => provider === target ? { ...provider, state: connection.state, source: connection.source } : provider));
}
// Back to the starting connections. Tests isolate cases with it; the app never calls it.
export function resetProviderState() { publish(startingList()); }
