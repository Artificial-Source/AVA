import { useLayoutEffect, useRef } from 'react';

// Presentation lifetime only: reject old closures after a view revision, close or unmount.
// This is not a backend request ID, durable receipt or exactly-once delivery guarantee.
export function useViewGuard(scope: string) {
  const current = useRef({ scope, active: false, claimed: false });
  if (current.current.scope !== scope) current.current = { scope, active: false, claimed: false };
  const token = current.current;
  useLayoutEffect(() => {
    token.active = true;
    return () => { token.active = false; };
  }, [token]);
  return {
    valid: () => token.active && current.current === token,
    invalidate: () => { token.active = false; },
    claim: () => {
      if (!token.active || current.current !== token || token.claimed) return false;
      token.claimed = true;
      return true;
    },
  };
}
