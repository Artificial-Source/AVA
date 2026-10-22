import { useEffect, useState, type ReactNode, type SyntheticEvent } from 'react';
import { useReducedMotion } from './useReducedMotion';

// UI-only retention, bounded by --motion-panel-duration. Never drives run state.
export const PRESENCE_EXIT_MS = 200;

interface PresenceProps {
  open: boolean;
  children: ReactNode;
  className?: string;
  id?: string;
}

export function Presence({ open, children, className, id }: PresenceProps) {
  const reducedMotion = useReducedMotion();
  const [retained, setRetained] = useState(open);

  useEffect(() => {
    if (open || reducedMotion) {
      setRetained(open);
      return;
    }
    if (!retained) return;
    const timeout = window.setTimeout(() => setRetained(false), PRESENCE_EXIT_MS);
    return () => window.clearTimeout(timeout);
  }, [open, reducedMotion, retained]);

  if (!open && (reducedMotion || !retained)) return null;

  function guardExit(event: SyntheticEvent) {
    if (open) return;
    event.preventDefault();
    event.stopPropagation();
  }

  return (
    <div
      id={id}
      className={className}
      data-presence={open ? 'open' : 'closing'}
      inert={!open}
      aria-hidden={!open || undefined}
      onClickCapture={guardExit}
      onKeyDownCapture={guardExit}
      onPointerDownCapture={guardExit}
      onSubmitCapture={guardExit}
    >
      {children}
    </div>
  );
}
