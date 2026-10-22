import { useSyncExternalStore } from 'react';
import type { ModelId } from '../state/types';
import type { Provider } from './contracts';
import { modelChoices, providers as providerExamples } from './screenExamples';

// Visit-lifetime provider connections. Every surface that lists providers or their models
// (Connect a provider, Settings → Models & providers, the model picker) reads this one
// store, so a connection made in one dialog is still there when another opens.
export type ProviderConnection = Pick<Provider, 'state' | 'source'>;

// The models the starting chats use come from these providers, so they start connected.
const connectedAtStart: Readonly<Record<string, true>> = { 'aster-lab': true, 'orion-lab': true };
function startingList(): readonly Provider[] {
  return providerExamples.map((provider): Provider => Object.hasOwn(connectedAtStart, provider.id) ? { ...provider, state: 'connected-example', source: 'stored-example' } : provider);
}
// What counts as connected everywhere: a stored sign-in, or credentials from the environment.
export function providerConnected(provider: Provider): boolean {
  return provider.state === 'connected-example' || provider.state === 'external';
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
function anyConnected() { return current.some(providerConnected); }
export function useAnyProviderConnected(): boolean { return useSyncExternalStore(subscribe, anyConnected, anyConnected); }
// The model a new chat starts on: the preferred one while its provider is connected, else the first
// catalog model a connected provider offers, else still the preferred one (Send then says what to connect).
export function newChatModel(preferred: ModelId, providers: readonly Provider[] = current): ModelId {
  const connected = new Set(providers.filter(providerConnected).map((provider) => provider.id));
  const usable = modelChoices.filter((choice) => choice.modelId !== undefined && connected.has(choice.providerId));
  return usable.some((choice) => choice.modelId === preferred) ? preferred : usable[0]?.modelId ?? preferred;
}
export function setProviderState(id: string, connection: ProviderConnection) {
  const target = current.find((provider) => provider.id === id);
  if (!target || target.state === connection.state && target.source === connection.source) return;
  publish(current.map((provider) => provider === target ? { ...provider, state: connection.state, source: connection.source } : provider));
}
// Start fresh: the visit continues as a new user. Nothing is connected: no stored sign-ins and no
// environment credentials, and environment-managed providers can't change state, so they are absent.
export function clearProviderConnections() {
  publish(providerExamples.flatMap((provider): Provider[] => provider.state === 'external' || provider.source === 'environment-example' ? [] : [{ ...provider, state: 'available', source: 'none' }]));
}
// Back to the starting connections: Restore sample data, and test isolation.
export function resetProviderState() { publish(startingList()); }
