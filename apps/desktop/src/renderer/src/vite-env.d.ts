/// <reference types="vite/client" />

import type { AvaDesktopApi } from '../../preload/index.ts'

declare global {
  interface Window {
    avaDesktop: AvaDesktopApi
  }
}

export {}
