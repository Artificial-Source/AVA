import { errorMessage } from '../../shared/text.ts'
import type { UserFacingError } from '../../shared/types.ts'

// ipcRenderer.invoke rejects only when a main-process handler throws, which is a desktop bug,
// not a backend error; surface it instead of leaving an unhandled rejection.
export function ipcFailure(error: unknown): UserFacingError {
  return {
    title: 'Desktop request failed',
    message: errorMessage(error),
    hint: 'Restart the desktop client. If this keeps happening, report the steps that led here.'
  }
}
