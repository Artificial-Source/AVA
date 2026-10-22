import { afterEach, describe, expect, it, vi } from 'vitest';
import { appChord, bindingConflict, bindingLabel, bindingProblem, defaultShortcuts, effectiveBinding, recordedBinding, shortcutActions, shortcutForEvent, shortcutGroups, shortcutHint } from './shortcuts';

function key(init: KeyboardEventInit) { return new KeyboardEvent('keydown', init); }
afterEach(() => { vi.unstubAllGlobals(); document.body.replaceChildren(); });
describe('browser-only shortcut contracts', () => {
  it('has one safe unique default per assigned action, stable ids and no Enter authority', () => {
    const bound = shortcutActions.filter((action) => action.binding);
    expect(new Set(bound.map((action) => action.binding)).size).toBe(bound.length);
    for (const action of bound) expect(bindingProblem(action.binding!)).toBeNull();
    expect(effectiveBinding(defaultShortcuts, 'stop')).toBe('Mod+Shift+.');
    expect(effectiveBinding(defaultShortcuts, 'inspector')).toBeNull();
    expect(shortcutActions.map((action) => action.id).sort()).toEqual(['agents', 'changes', 'composer', 'context', 'inspector', 'models', 'newChat', 'palette', 'sessions', 'settings', 'stop', 'tools']);
    expect(shortcutActions.every((action) => shortcutGroups.includes(action.group))).toBe(true);
    expect(shortcutActions.some((action) => /terminal|send|mock|example/i.test(`${action.label} ${action.detail}`))).toBe(false);
    expect(effectiveBinding(defaultShortcuts, 'palette')).toBe('Mod+K');
    expect(effectiveBinding(defaultShortcuts, 'sessions')).toBe('Mod+P');
  });
  it('canonicalizes Ctrl/Cmd and shifted digit key values, not native typing', () => {
    expect(recordedBinding(key({ key: '!', code: 'Digit1', ctrlKey: true, shiftKey: true }))).toBe('Mod+Shift+1');
    expect(shortcutForEvent(key({ key: '!', code: 'Digit1', metaKey: true, shiftKey: true }))).toBe('agents');
    expect(shortcutForEvent(key({ key: ',', ctrlKey: true, shiftKey: true }))).toBe('settings');
    expect(shortcutForEvent(key({ key: 'k', code: 'KeyK', ctrlKey: true }))).toBe('palette');
    expect(shortcutForEvent(key({ key: 'p', code: 'KeyP', ctrlKey: true }))).toBe('sessions');
  });
  it('uses only the platform modifier for the plain palette and finder chords', () => {
    expect(shortcutForEvent(key({ key: 'k', code: 'KeyK', metaKey: true }))).toBeNull();
    vi.stubGlobal('navigator', { platform: 'MacIntel' });
    // Ctrl+K and Ctrl+P edit text on macOS; only Cmd claims them.
    expect(shortcutForEvent(key({ key: 'k', code: 'KeyK', ctrlKey: true }))).toBeNull();
    expect(shortcutForEvent(key({ key: 'k', code: 'KeyK', metaKey: true }))).toBe('palette');
    expect(shortcutHint(undefined, 'newChat')).toEqual({ label: '⌘⇧9', aria: 'Meta+Shift+9' });
    expect(bindingLabel('Mod+P')).toBe('⌘P');
  });
  it('describes the effective binding for menus and chips, or nothing when unassigned or disabled', () => {
    expect(shortcutHint(undefined, 'newChat')).toEqual({ label: 'Ctrl+Shift+9', aria: 'Control+Shift+9' });
    expect(shortcutHint(defaultShortcuts, 'palette')).toEqual({ label: 'Ctrl+K', aria: 'Control+K' });
    expect(shortcutHint(defaultShortcuts, 'stop')).toEqual({ label: 'Ctrl+Shift+.', aria: 'Control+Shift+.' });
    expect(shortcutHint({ enabled: true, overrides: { stop: null } }, 'stop')).toBeNull();
    expect(shortcutHint({ enabled: true, overrides: { inspector: 'Mod+Shift+.', stop: null } }, 'inspector')?.label).toBe('Ctrl+Shift+.');
    expect(shortcutHint({ enabled: true, overrides: { newChat: null } }, 'newChat')).toBeNull();
    expect(shortcutHint({ enabled: false, overrides: {} }, 'newChat')).toBeNull();
    expect(bindingLabel(null)).toBe('Unassigned');
  });
  it.each(['Enter', 'Shift+Enter', 'Mod+Enter', 'Escape', 'Space', 'Tab', 'A', 'F5', 'Alt+F4', 'Mod+L', 'Mod+Shift+T', 'Mod+Shift+I', 'Mod+Shift+K', 'Mod+C', 'Mod+1', 'Mod+Shift+P', 'Mod+,', 'Mod+Shift+3', 'Mod+Shift+4', 'Mod+Shift+5', 'Mod+A', 'Mod+Z'])('reserves editing, system and browser binding %s', (binding) => {
    expect(bindingProblem(binding)).toMatch(/Reserved/);
  });
  it.each([{ repeat: true }, { isComposing: true }, { keyCode: 229 }, { altKey: true }, { ctrlKey: true, metaKey: true }, { ctrlKey: false }])('ignores an unsafe event %j', (patch) => {
    expect(shortcutForEvent(key({ key: ',', ctrlKey: true, shiftKey: true, ...patch }))).toBeNull();
    expect(shortcutForEvent(key({ key: 'k', code: 'KeyK', ctrlKey: true, ...patch }))).toBeNull();
  });
  it('rejects AltGraph, modifiers, conflicts and malformed duplicate effective bindings', () => {
    const event = key({ key: ',', ctrlKey: true, shiftKey: true }); Object.defineProperty(event, 'getModifierState', { value: () => true });
    expect(recordedBinding(event)).toBeNull();
    expect(recordedBinding(key({ key: 'Control', ctrlKey: true }))).toBeNull();
    expect(bindingConflict(defaultShortcuts, 'stop', 'Mod+Shift+,')).toBe('Open Settings');
    expect(bindingConflict(defaultShortcuts, 'stop', 'Mod+K')).toBe('Open command palette');
    expect(shortcutForEvent(key({ key: ',', ctrlKey: true, shiftKey: true }), { enabled: true, overrides: { stop: 'Mod+Shift+,' } })).toBeNull();
  });
  it('honors custom bindings, explicit unassignment and global disable', () => {
    const prefs = { enabled: true, overrides: { settings: null, stop: 'Mod+Shift+.' } } as const;
    expect(shortcutForEvent(key({ key: '>', code: 'Period', ctrlKey: true, shiftKey: true }))).toBe('stop');
    expect(shortcutForEvent(key({ key: '>', code: 'Period', ctrlKey: true, shiftKey: true }), { enabled: true, overrides: { inspector: 'Mod+Shift+.' } })).toBeNull();
    expect(effectiveBinding(prefs, 'settings')).toBeNull();
    expect(shortcutForEvent(key({ key: ',', ctrlKey: true, shiftKey: true }), prefs)).toBeNull();
    expect(shortcutForEvent(key({ key: '>', code: 'Period', ctrlKey: true, shiftKey: true }), prefs)).toBe('stop');
    expect(shortcutForEvent(key({ key: ',', metaKey: true, shiftKey: true }), { ...defaultShortcuts, enabled: false })).toBeNull();
    expect(shortcutForEvent(key({ key: 'k', code: 'KeyK', ctrlKey: true }), { enabled: true, overrides: { palette: null } })).toBeNull();
  });
  it('claims chords from the composer and fields but never from dialogs, menus or pickers', () => {
    const root = document.createElement('main'); const draft = document.createElement('textarea'); const dialog = document.createElement('dialog'); const inner = document.createElement('input');
    dialog.append(inner); root.append(draft, dialog); document.body.append(root);
    const press = (target: HTMLElement) => { const event = key({ key: 'k', code: 'KeyK', ctrlKey: true, bubbles: true, cancelable: true }); target.dispatchEvent(event); return appChord(event, root); };
    expect(press(draft)).toBe('palette');
    expect(press(inner)).toBeNull();
    const menu = document.createElement('div'); menu.setAttribute('role', 'menu'); root.append(menu);
    expect(press(draft)).toBeNull();
  });
});
