import tailwindcss from '@tailwindcss/vite'
import react from '@vitejs/plugin-react'
import { defineConfig, externalizeDepsPlugin } from 'electron-vite'
import type { Plugin } from 'vite'
import { PRODUCTION_META_CSP } from './src/shared/content-security-policy.ts'
import { DEV_SERVER_HOST, DEV_SERVER_PORT } from './src/shared/dev-server.ts'

// Electron cannot attach CSP response headers to file:// documents, so the built renderer
// carries the production policy as the first <head> element. Dev relies on main's DEV_CSP header.
function productionCspMeta(): Plugin {
  return {
    name: 'ava-production-csp-meta',
    apply: 'build',
    transformIndexHtml: () => [
      {
        tag: 'meta',
        attrs: { 'http-equiv': 'Content-Security-Policy', content: PRODUCTION_META_CSP },
        injectTo: 'head-prepend'
      }
    ]
  }
}

export default defineConfig({
  main: {
    plugins: [externalizeDepsPlugin()]
  },
  preload: {
    build: {
      externalizeDeps: false,
      rollupOptions: {
        output: {
          format: 'cjs',
          entryFileNames: 'index.cjs',
          inlineDynamicImports: true
        }
      }
    }
  },
  renderer: {
    // Main trusts only this exact origin (src/shared/dev-server.ts). strictPort makes an occupied
    // port fail the dev run; otherwise Vite would move ports while electron-vite still points
    // ELECTRON_RENDERER_URL at the configured one, i.e. at whoever holds it.
    server: { host: DEV_SERVER_HOST, port: DEV_SERVER_PORT, strictPort: true },
    plugins: [react(), tailwindcss(), productionCspMeta()]
  }
})
