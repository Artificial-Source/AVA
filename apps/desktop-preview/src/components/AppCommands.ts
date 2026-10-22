import type { MainScreenRequest } from '../screens/contracts';
import { appChord, type ShortcutAction, type ShortcutPreferences } from '../screens/shortcutBindings';
import type { PreviewAction } from '../state/types';
import { canFocus } from '../ui/DialogFrame';

export type AppActionHost = {
  root: HTMLElement;
  child: boolean;
  // No chat is open (after Start fresh, before a project): only actions that need no chat have a target.
  empty?: boolean;
  working: boolean;
  current: () => boolean;
  open: (kind: MainScreenRequest['kind'], targetChatId?: string) => void;
  newChat: () => void;
  dispatch: (action: PreviewAction) => void;
};
// Read-only child inspection never targets the hidden parent's model, draft, tools or run.
const parentOnly: readonly ShortcutAction[] = ['models', 'composer', 'tools', 'stop'];
// With no chat, New chat opens the project picker instead.
const withoutChat: readonly ShortcutAction[] = ['settings', 'sessions', 'palette', 'newChat'];
function allowed(action: ShortcutAction, host: AppActionHost) {
  return host.current() && !(host.child && parentOnly.includes(action)) && !(host.empty && !withoutChat.includes(action)) && (action !== 'stop' || host.working);
}

// One executor for keyboard chords and the command palette. Returns whether the action ran.
export function runAppAction(action: ShortcutAction, host: AppActionHost) {
  if (!allowed(action, host)) return false;
  if (action === 'settings' || action === 'models' || action === 'sessions') host.open(action);
  else if (action === 'palette') host.open('commands');
  else if (action === 'newChat') host.newChat();
  else if (action === 'composer' || action === 'tools') {
    // Focus and expansion stay with the visible owners; nothing here keeps a second copy.
    const target = host.root.querySelector<HTMLElement>(action === 'composer' ? 'textarea' : '[data-tool-toggle]');
    if (!canFocus(target)) return false;
    if (action === 'composer') target.focus(); else target.click();
  }
  else if (action === 'stop') host.dispatch({ type: 'stopViewedMock' });
  else if (action === 'inspector') host.dispatch({ type: 'toggleInspectorVisibility' });
  else host.dispatch({ type: 'toggleInspector', module: action });
  return true;
}

// Keyboard entry: claim the chord, cancel its browser default, then run the shared action.
export function runAppCommand(event: KeyboardEvent, host: AppActionHost & { preferences?: ShortcutPreferences }) {
  const action = host.current() ? appChord(event, host.root, host.preferences) : null;
  if (!action || !allowed(action, host)) return;
  event.preventDefault();
  runAppAction(action, host);
}
