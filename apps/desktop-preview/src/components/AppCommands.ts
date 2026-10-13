import { shortcutForEvent, type ShortcutPreferences } from '../screens/shortcutBindings';
import type { PreviewAction } from '../state/types';
import { canFocus } from '../ui/DialogFrame';

export function runAppCommand(event: KeyboardEvent, host: {
  root: HTMLElement;
  preferences?: ShortcutPreferences;
  child: boolean;
  working: boolean;
  current: () => boolean;
  open: (kind: 'settings' | 'models') => void;
  newChat: () => void;
  dispatch: (action: PreviewAction) => void;
}) {
  const target = event.target;
  if (!host.current() || event.defaultPrevented || !(target instanceof HTMLElement) || !canFocus(target) || !host.root.contains(target) || target.closest('input, textarea, select, [contenteditable]:not([contenteditable="false"]), dialog, [role="menu"], [role="listbox"], [hidden], [inert]') || host.root.querySelector('dialog[open], [role="menu"], [data-shortcuts-blocked]')) return;
  const action = shortcutForEvent(event, host.preferences);
  if (!action || host.child && ['models', 'composer', 'tools', 'stop'].includes(action) || action === 'stop' && !host.working) return;
  // Only the conservative, browser-unreserved key set reaches here. The first activation
  // may await this owner; native editing/browser shortcuts are never canceled or replayed.
  event.preventDefault();
  if (action === 'settings' || action === 'models') host.open(action);
  else if (action === 'newChat') host.newChat();
  else if (action === 'composer') {
    const input = host.root.querySelector<HTMLTextAreaElement>('textarea');
    if (canFocus(input)) input.focus();
  } else if (action === 'tools') {
    const toggle = host.root.querySelector<HTMLButtonElement>('[data-tool-toggle]');
    if (canFocus(toggle)) toggle.click();
  }
  else if (action === 'stop') host.dispatch({ type: 'stopViewedMock' });
  else if (action === 'inspector') host.dispatch({ type: 'toggleInspectorVisibility' });
  else host.dispatch({ type: 'toggleInspector', module: action });
}
