import { createContext, useContext } from 'react';
import type { HelpTopicId } from '../screens/contracts';

// Opens Help at a topic. The app shell provides it; isolated widgets, the component catalog and
// the screen-review harness have none, so contextual help links render nothing there instead of
// a control that does nothing.
export const HelpContext = createContext<((topic: HelpTopicId) => void) | null>(null);

export function useOpenHelp(): ((topic: HelpTopicId) => void) | null {
  return useContext(HelpContext);
}
