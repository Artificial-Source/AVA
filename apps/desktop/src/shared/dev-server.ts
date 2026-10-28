// The renderer dev server, pinned by electron.vite.config.ts with strictPort: an occupied port
// (for example, another user on the host bound it first) fails `pnpm dev` instead of moving.
export const DEV_SERVER_HOST = '127.0.0.1'
export const DEV_SERVER_PORT = 5173
export const DEV_SERVER_ORIGIN = `http://${DEV_SERVER_HOST}:${DEV_SERVER_PORT}`

export type DevRendererEnv = {
  ELECTRON_RENDERER_URL?: string
  NODE_ENV_ELECTRON_VITE?: string
}

// `electron-vite dev` sets NODE_ENV_ELECTRON_VITE=development and ELECTRON_RENDERER_URL before
// starting Electron. That URL becomes the trusted IPC document, so it is honoured only in that
// mode, never for a packaged app, and only at the exact pinned origin.
export function trustedDevRendererUrl(env: DevRendererEnv, isPackaged: boolean): string | null {
  const url = env.ELECTRON_RENDERER_URL
  if (isPackaged || env.NODE_ENV_ELECTRON_VITE !== 'development' || !url) {
    return null
  }
  try {
    return new URL(url).origin === DEV_SERVER_ORIGIN ? url : null
  } catch {
    return null
  }
}
