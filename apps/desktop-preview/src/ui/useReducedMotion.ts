import { createContext, useContext, useSyncExternalStore } from 'react';

export const ReducedMotionContext = createContext(false);

const query = '(prefers-reduced-motion: reduce)';

function subscribe(onChange: () => void) {
  if (typeof window.matchMedia !== 'function') return () => {};
  const media = window.matchMedia(query);
  media.addEventListener('change', onChange);
  return () => media.removeEventListener('change', onChange);
}

function getSnapshot() {
  return typeof window.matchMedia === 'function' && window.matchMedia(query).matches;
}

export function useReducedMotion() {
  const manual = useContext(ReducedMotionContext);
  const system = useSyncExternalStore(subscribe, getSnapshot, () => false);
  return manual || system;
}
