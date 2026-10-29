// Renderer Content-Security-Policy, defined once. Main attaches it as a response header, which
// covers the dev server. Electron cannot attach headers to file:// documents, so the production
// build also embeds PRODUCTION_META_CSP in index.html (see electron.vite.config.ts).

import { DEV_SERVER_HOST, DEV_SERVER_PORT } from './dev-server.ts'

const PRODUCTION_DIRECTIVES = [
  "default-src 'self'",
  "script-src 'self'",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' data:",
  "font-src 'none'",
  "connect-src 'self'",
  "object-src 'none'",
  "base-uri 'self'",
  "form-action 'none'",
  "frame-ancestors 'none'"
]

export const PRODUCTION_CSP = PRODUCTION_DIRECTIVES.join('; ')

// Browsers ignore frame-ancestors in a <meta> policy and log a warning, so the embedded copy omits it.
export const PRODUCTION_META_CSP = PRODUCTION_DIRECTIVES
  .filter((directive) => !directive.startsWith('frame-ancestors'))
  .join('; ')

// Vite HMR needs inline/eval scripts and its websocket on the pinned dev origin; never used for
// a built renderer.
export const DEV_CSP = [
  "default-src 'self'",
  "script-src 'self' 'unsafe-eval' 'unsafe-inline'",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' data:",
  "font-src 'none'",
  `connect-src 'self' ws://${DEV_SERVER_HOST}:${DEV_SERVER_PORT}`,
  "object-src 'none'",
  "base-uri 'self'",
  "form-action 'none'",
  "frame-ancestors 'none'"
].join('; ')
