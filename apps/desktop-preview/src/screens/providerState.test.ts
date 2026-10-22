import { afterEach, describe, expect, it } from 'vitest';
import { clearProviderConnections, getProviderList, newChatModel, providerConnected, resetProviderState, setProviderState } from './providerState';

// Provider connections last for the whole visit; each case leaves the visit's start behind it.
afterEach(resetProviderState);

describe('visit provider connections', () => {
  it('starts fresh with nothing connected and environment-managed providers absent, then restores the seeded list', () => {
    const seeded = getProviderList();
    expect(seeded.filter(providerConnected).map((provider) => provider.id)).toEqual(['aster-lab', 'orion-lab', 'cedar']);
    clearProviderConnections();
    const fresh = getProviderList();
    expect(fresh.some(providerConnected)).toBe(false);
    expect(fresh.find((provider) => provider.id === 'cedar')).toBeUndefined();
    expect(fresh.every((provider) => provider.state === 'available' && provider.source === 'none')).toBe(true);
    resetProviderState();
    expect(getProviderList()).toEqual(seeded);
  });

  it('starts new chats on the preferred model while its provider is connected, else on a connected provider’s model', () => {
    expect(newChatModel('aster')).toBe('aster');
    expect(newChatModel('orion')).toBe('orion');
    clearProviderConnections();
    // Nothing connected offers a model: keep the preference, and Send says what to connect.
    expect(newChatModel('aster')).toBe('aster');
    const modelless = getProviderList().find((provider) => provider.id !== 'aster-lab' && provider.id !== 'orion-lab')!;
    setProviderState(modelless.id, { state: 'connected-example', source: 'stored-example' });
    expect(newChatModel('aster')).toBe('aster');
    setProviderState('orion-lab', { state: 'connected-example', source: 'stored-example' });
    expect(newChatModel('aster')).toBe('orion');
    expect(newChatModel('aster-mini')).toBe('orion');
    setProviderState('aster-lab', { state: 'connected-example', source: 'stored-example' });
    expect(newChatModel('aster-mini')).toBe('aster-mini');
  });
});
