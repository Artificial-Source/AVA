import { createContext } from 'react';

// Opens Changes focused on one file. Without a provider (catalog, dialogs) diff headers offer no navigation.
export const ChangeNavigationContext = createContext<((path: string) => void) | null>(null);
