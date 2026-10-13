import { sameRun, type RunEvent, type RunIdentity } from '../state/suppliedRun';

export type RunDriver = ReturnType<typeof createRunDriver>;
// Fixture delivery only. The delay spaces supplied snapshots, not generated tokens or work.
export function createRunDriver(identity: RunIdentity, snapshots: readonly (readonly RunEvent[])[], deliver: (events: readonly RunEvent[]) => void) {
  let closed = false; let paused = false; let index = 0;
  let frame: number | null = null; let timer: ReturnType<typeof setTimeout> | null = null;
  let pending: RunEvent[] = [];
  function clear() {
    if (frame !== null) cancelAnimationFrame(frame);
    if (timer !== null) clearTimeout(timer);
    frame = null; timer = null;
  }
  function cancel() { closed = true; clear(); pending = []; }
  function flush() {
    if (closed) return;
    if (frame !== null) cancelAnimationFrame(frame);
    frame = null;
    const events = pending; pending = [];
    if (events.length) deliver(events);
  }
  function next() {
    if (closed) return;
    const events = snapshots[index++];
    if (!events || events.length + pending.length > 4096 || events.some((event) => !sameRun(identity, event))) { cancel(); return; }
    pending.push(...events);
    if (events.some((event) => event.kind === 'complete' || event.kind === 'error') || index === snapshots.length) {
      // Terminal delivery and revocation are immediate, never a deferred paint decision.
      flush(); cancel(); return;
    }
    if (frame === null) frame = requestAnimationFrame(() => { if (!closed) flush(); });
    if (!paused) timer = setTimeout(() => { timer = null; if (!closed && !paused) next(); }, 1800);
  }
  return {
    start: () => { if (!index && !closed) next(); }, cancel,
    pause: () => { if (!closed) { paused = true; clear(); flush(); } },
    resume: () => { if (!closed && paused) { paused = false; next(); } },
    step: () => { if (!closed && paused) { next(); flush(); } },
  };
}
