import type { ToolView, TurnState } from './contracts';

export type ToolDisplay = 'compact' | 'auto' | 'expanded';

export function isToolDisplay(value: string): value is ToolDisplay {
  return value === 'compact' || value === 'auto' || value === 'expanded';
}

// Presentation only: an explicitly terminal turn can fold a running snapshot,
// but must never replace its supplied tool status (including after Stop).
export function toolDetailsOpen(display: ToolDisplay, state: ToolView['state'], lifecycle?: TurnState) {
  if (display === 'expanded') return true;
  if (display === 'compact') return false;
  return state === 'failed' || state === 'unavailable' || state === 'running' && (lifecycle === undefined || lifecycle === 'streaming');
}
