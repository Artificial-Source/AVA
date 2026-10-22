import { afterEach, describe, expect, it, vi } from 'vitest';
import { runAppAction, runAppCommand } from './AppCommands';
import { defaultShortcuts, type ShortcutPreferences } from '../screens/shortcutBindings';

function setup() {
  const root = document.createElement('main'); const target = document.createElement('button'); root.append(target); document.body.append(root);
  const host = { root, current: () => true, child: false, working: false, preferences: defaultShortcuts as ShortcutPreferences, open: vi.fn(), newChat: vi.fn(), dispatch: vi.fn() };
  function press(init: KeyboardEventInit = { key: ',', ctrlKey: true, shiftKey: true }, node: HTMLElement = target) {
    const event = new KeyboardEvent('keydown', { bubbles: true, cancelable: true, ...init }); node.dispatchEvent(event); runAppCommand(event, host); return event;
  }
  return { root, target, host, press };
}
afterEach(() => document.body.replaceChildren());
describe('complete root-scoped application command owner', () => {
  it('opens Settings, models, the palette, the chat finder and future chats and performs real inspector commands', () => {
    const { host, press } = setup(); expect(press().defaultPrevented).toBe(true); expect(host.open).toHaveBeenLastCalledWith('settings');
    for (const [digit, module] of [['1', 'agents'], ['2', 'changes'], ['0', 'context']]) { press({ key: digit, code: `Digit${digit}`, ctrlKey: true, shiftKey: true }); expect(host.dispatch).toHaveBeenLastCalledWith({ type: 'toggleInspector', module }); }
    press({ key: '6', ctrlKey: true, shiftKey: true }); expect(host.open).toHaveBeenLastCalledWith('models');
    press({ key: 'k', code: 'KeyK', ctrlKey: true }); expect(host.open).toHaveBeenLastCalledWith('commands');
    press({ key: 'p', code: 'KeyP', ctrlKey: true }); expect(host.open).toHaveBeenLastCalledWith('sessions');
    press({ key: '9', ctrlKey: true, shiftKey: true }); expect(host.newChat).toHaveBeenCalledTimes(1);
    // Every safe chord has a default owner; moving one to the inspector frees it from Stop first.
    host.preferences = { ...defaultShortcuts, overrides: { inspector: 'Mod+Shift+.', stop: null } }; press({ key: '.', metaKey: true, shiftKey: true }); expect(host.dispatch).toHaveBeenLastCalledWith({ type: 'toggleInspectorVisibility' });
  });
  it('fires app chords from the composer and other fields, leaving native editing keys alone', () => {
    const { root, host, press } = setup(); const draft = document.createElement('textarea'); const field = document.createElement('input'); root.append(draft, field);
    draft.focus(); expect(press({ key: 'k', code: 'KeyK', ctrlKey: true }, draft).defaultPrevented).toBe(true); expect(host.open).toHaveBeenLastCalledWith('commands');
    press({ key: 'p', code: 'KeyP', ctrlKey: true }, field); expect(host.open).toHaveBeenLastCalledWith('sessions');
    press({ key: '9', code: 'Digit9', ctrlKey: true, shiftKey: true }, draft); expect(host.newChat).toHaveBeenCalledTimes(1);
    host.open.mockClear();
    for (const init of [{ key: 'a', ctrlKey: true }, { key: 'z', ctrlKey: true }, { key: 'c', ctrlKey: true }, { key: 'Enter', ctrlKey: true }, { key: 'k', code: 'KeyK', ctrlKey: true, shiftKey: true }]) {
      expect(press(init, draft).defaultPrevented).toBe(false);
    }
    expect(host.open).not.toHaveBeenCalled(); expect(host.dispatch).not.toHaveBeenCalled();
  });
  it('focuses only the visible draft and activates the owning all-tools button without a second expansion authority', () => {
    const { root, press } = setup(); const draft = document.createElement('textarea'); const tools = document.createElement('button'); tools.dataset.toolToggle = ''; tools.onclick = vi.fn(); root.append(draft, tools);
    press({ key: '7', ctrlKey: true, shiftKey: true }); expect(document.activeElement).toBe(draft);
    press({ key: '8', ctrlKey: true, shiftKey: true }); expect(tools.onclick).toHaveBeenCalledTimes(1);
    tools.hidden = true; press({ key: '8', ctrlKey: true, shiftKey: true }); expect(tools.onclick).toHaveBeenCalledTimes(1);
  });
  it('requires a Working parent for Stop and rejects every parent-only command from child inspection', () => {
    // Stop's default is Ctrl/Cmd+Shift+period.
    const { host, press } = setup();
    press({ key: '.', ctrlKey: true, shiftKey: true }); expect(host.dispatch).not.toHaveBeenCalled(); host.working = true; press({ key: '.', ctrlKey: true, shiftKey: true }); expect(host.dispatch).toHaveBeenCalledExactlyOnceWith({ type: 'stopViewedMock' });
    host.child = true; host.dispatch.mockClear(); for (const digit of ['6', '7', '8', '.']) press({ key: digit, ctrlKey: true, shiftKey: true }); expect(host.dispatch).not.toHaveBeenCalled(); expect(host.open).not.toHaveBeenCalled();
    press({ key: 'p', code: 'KeyP', ctrlKey: true }); expect(host.open).toHaveBeenLastCalledWith('sessions');
  });
  it('shares one executor with the palette, holding the same child, Working and staleness checks', () => {
    const { host } = setup();
    expect(runAppAction('palette', host)).toBe(true); expect(host.open).toHaveBeenLastCalledWith('commands');
    expect(runAppAction('stop', host)).toBe(false);
    expect(runAppAction('composer', host)).toBe(false);
    host.child = true; expect(runAppAction('models', host)).toBe(false); expect(runAppAction('agents', host)).toBe(true);
    host.current = () => false; expect(runAppAction('settings', host)).toBe(false);
    expect(host.open).toHaveBeenCalledTimes(1); expect(host.dispatch).toHaveBeenCalledExactlyOnceWith({ type: 'toggleInspector', module: 'agents' });
  });
  it.each(['dialog', 'menu', 'listbox', 'hidden', 'inert', 'disconnected', 'outside', 'prevented', 'stale'])('ignores %s keyboard context', (context) => {
    const { host, root, target, press } = setup(); let node: HTMLElement = target;
    if (context === 'dialog') { node = document.createElement('input'); const dialog = document.createElement('dialog'); dialog.setAttribute('open', ''); dialog.append(node); root.append(dialog); }
    if (context === 'menu' || context === 'listbox') target.setAttribute('role', context);
    if (context === 'hidden' || context === 'inert') target.setAttribute(context, '');
    if (context === 'disconnected') root.remove(); if (context === 'outside') { document.body.append(target); }
    if (context === 'stale') host.current = () => false;
    if (context === 'prevented') target.addEventListener('keydown', (event) => event.preventDefault());
    press(undefined, node); expect(host.open).not.toHaveBeenCalled(); expect(host.newChat).not.toHaveBeenCalled(); expect(host.dispatch).not.toHaveBeenCalled();
  });
  it.each(['dialog', 'menu', 'context-picker'])('rejects shortcuts while an app %s is open elsewhere in the root', (kind) => {
    const { root, host, press } = setup(); const blocker = document.createElement(kind === 'dialog' ? 'dialog' : 'div');
    if (kind === 'dialog') blocker.setAttribute('open', ''); else if (kind === 'menu') blocker.setAttribute('role', 'menu'); else blocker.dataset.shortcutsBlocked = '';
    root.append(blocker); press(); expect(host.open).not.toHaveBeenCalled();
  });
});
