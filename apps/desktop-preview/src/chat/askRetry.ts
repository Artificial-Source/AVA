import { createContext } from 'react';

// Asks AVA to retry by drafting the request in the composer. Provided only where a failed step has
// no other next step; without a provider (catalog, dialogs, a failed turn that already offers one)
// failed tool rows show none.
export const AskRetryContext = createContext<(() => void) | null>(null);
