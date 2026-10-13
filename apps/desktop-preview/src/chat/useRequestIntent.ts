import { useRef, useState } from 'react';
import type { RequestIdentity } from './contracts';

// A mounted presentation guard, not durable exactly-once delivery. The host owns settlement.
export function useRequestIntent(identity: RequestIdentity) {
  const [sent, setSent] = useState<ReadonlySet<string>>(() => new Set());
  const current = useRef(sent);
  const key = JSON.stringify([identity.id, identity.version]);
  return {
    submitted: sent.has(key),
    claim() {
      if (current.current.has(key)) return false;
      const next = new Set(current.current).add(key);
      current.current = next;
      setSent(next);
      return true;
    },
  };
}
