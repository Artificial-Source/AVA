import { describe, expect, it } from 'vitest';
import { isToolDisplay, toolDetailsOpen, type ToolDisplay } from './toolDisplay';
import type { ToolView, TurnState } from './contracts';

const states: ToolView['state'][] = ['queued', 'running', 'completed', 'failed', 'canceled', 'unavailable'];
const lifecycles: (TurnState | undefined)[] = [undefined, 'streaming', 'committed', 'failed', 'canceled'];
describe('pure tool display defaults', () => {
  it.each<ToolDisplay>(['compact', 'auto', 'expanded'])('%s covers every supplied state and explicit/historical lifecycle without changing facts', (display) => {
    for (const state of states) for (const lifecycle of lifecycles) {
      const record = Object.freeze({ id: 'same', action: 'Read', detail: 'example.ts', state });
      const expected = display === 'expanded' || display === 'auto' && (state === 'failed' || state === 'unavailable' || state === 'running' && (lifecycle === undefined || lifecycle === 'streaming'));
      expect(toolDetailsOpen(display, record.state, lifecycle), `${state}/${lifecycle}`).toBe(expected);
      expect(record.state).toBe(state);
    }
  });
  it('accepts only the three stored modes; Custom is derived output visibility, not a mode', () => {
    for (const mode of ['compact', 'auto', 'expanded']) expect(isToolDisplay(mode)).toBe(true);
    for (const mode of ['custom', 'collapsed', '', 'constructor']) expect(isToolDisplay(mode)).toBe(false);
  });
});
