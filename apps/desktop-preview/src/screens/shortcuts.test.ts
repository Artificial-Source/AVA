import { describe, expect, it } from 'vitest';
import { bindingConflict, bindingProblem, defaultShortcuts, effectiveBinding, recordedBinding, shortcutActions, shortcutForEvent } from './shortcuts';

function key(init: KeyboardEventInit) { return new KeyboardEvent('keydown', init); }
describe('browser-only shortcut contracts', () => {
  it('has one safe unique default per assigned action and no Enter authority', () => {
    const bound = shortcutActions.filter((action) => action.binding);
    expect(new Set(bound.map((action) => action.binding)).size).toBe(bound.length);
    for (const action of bound) expect(bindingProblem(action.binding!)).toBeNull();
    expect(effectiveBinding(defaultShortcuts, 'stop')).toBeNull();
    expect(shortcutActions).toHaveLength(10);
    expect(shortcutActions.some((action) => /terminal|palette|session|send/i.test(action.label))).toBe(false);
  });
  it('canonicalizes Ctrl/Cmd and shifted digit key values, not native typing', () => {
    expect(recordedBinding(key({ key: '!', code: 'Digit1', ctrlKey: true, shiftKey: true }))).toBe('Mod+Shift+1');
    expect(shortcutForEvent(key({ key: '!', code: 'Digit1', metaKey: true, shiftKey: true }))).toBe('agents');
    expect(shortcutForEvent(key({ key: ',', ctrlKey: true, shiftKey: true }))).toBe('settings');
  });
  it.each(['Enter', 'Shift+Enter', 'Mod+Enter', 'Escape', 'Space', 'Tab', 'A', 'F5', 'Alt+F4', 'Mod+L', 'Mod+Shift+T', 'Mod+Shift+I', 'Mod+Shift+K', 'Mod+C', 'Mod+1', 'Mod+Shift+P', 'Mod+,', 'Mod+Shift+3', 'Mod+Shift+4', 'Mod+Shift+5'])('reserves editing, system and browser binding %s', (binding) => {
    expect(bindingProblem(binding)).toMatch(/Reserved/);
  });
  it.each([{ repeat: true }, { isComposing: true }, { keyCode: 229 }, { altKey: true }, { ctrlKey: true, metaKey: true }, { ctrlKey: false }])('ignores an unsafe event %j', (patch) => {
    expect(shortcutForEvent(key({ key: ',', ctrlKey: true, shiftKey: true, ...patch }))).toBeNull();
  });
  it('rejects AltGraph, modifiers, conflicts and malformed duplicate effective bindings', () => {
    const event = key({ key: ',', ctrlKey: true, shiftKey: true }); Object.defineProperty(event, 'getModifierState', { value: () => true });
    expect(recordedBinding(event)).toBeNull();
    expect(recordedBinding(key({ key: 'Control', ctrlKey: true }))).toBeNull();
    expect(bindingConflict(defaultShortcuts, 'stop', 'Mod+Shift+,')).toBe('Open Settings');
    expect(shortcutForEvent(key({ key: ',', ctrlKey: true, shiftKey: true }), { enabled: true, overrides: { stop: 'Mod+Shift+,' } })).toBeNull();
  });
  it('honors custom bindings, explicit unassignment and global disable', () => {
    const prefs = { enabled: true, overrides: { settings: null, stop: 'Mod+Shift+.' } } as const;
    expect(effectiveBinding(prefs, 'settings')).toBeNull();
    expect(shortcutForEvent(key({ key: ',', ctrlKey: true, shiftKey: true }), prefs)).toBeNull();
    expect(shortcutForEvent(key({ key: '>', code: 'Period', ctrlKey: true, shiftKey: true }), prefs)).toBe('stop');
    expect(shortcutForEvent(key({ key: ',', metaKey: true, shiftKey: true }), { ...defaultShortcuts, enabled: false })).toBeNull();
  });
});
