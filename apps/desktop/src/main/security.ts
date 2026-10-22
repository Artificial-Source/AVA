import { app, session, type BrowserWindow } from 'electron'
import { isExpectedDocument } from '../shared/ipc-contract.ts'

const DEV_CSP = [
  "default-src 'self'",
  "script-src 'self' 'unsafe-eval' 'unsafe-inline' http://localhost:* http://127.0.0.1:*",
  "style-src 'self' 'unsafe-inline'",
  "img-src 'self' data:",
  "font-src 'none'",
  "connect-src 'self' ws://localhost:* ws://127.0.0.1:* http://localhost:* http://127.0.0.1:*",
  "object-src 'none'",
  "base-uri 'self'",
  "form-action 'none'",
  "frame-ancestors 'none'"
].join('; ')

const PRODUCTION_CSP = [
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
].join('; ')

export function isDevSession(): boolean {
  return Boolean(process.env.ELECTRON_RENDERER_URL)
}

export function applySessionGuards(): void {
  const electronSession = session.defaultSession
  const csp = isDevSession() ? DEV_CSP : PRODUCTION_CSP
  electronSession.webRequest.onHeadersReceived((details, callback) => {
    const headers = { ...details.responseHeaders, 'Content-Security-Policy': [csp] }
    callback({ responseHeaders: headers })
  })
  electronSession.setPermissionRequestHandler((_webContents, _permission, callback) => {
    callback(false)
  })
  electronSession.setPermissionCheckHandler(() => false)
}

export function guardWindow(win: BrowserWindow, expectedDocument: string): void {
  win.webContents.setWindowOpenHandler(() => ({ action: 'deny' }))
  win.webContents.on('will-navigate', (event, url) => {
    if (!isExpectedDocument(url, expectedDocument)) {
      event.preventDefault()
    }
  })
  win.webContents.on('will-redirect', (event, url) => {
    if (!isExpectedDocument(url, expectedDocument)) {
      event.preventDefault()
    }
  })
}

export function installApplicationGuards(expectedDocument: string): void {
  app.on('web-contents-created', (_event, contents) => {
    contents.setWindowOpenHandler(() => ({ action: 'deny' }))
    contents.on('will-attach-webview', (event) => {
      event.preventDefault()
    })
    contents.on('will-navigate', (event, url) => {
      if (!isExpectedDocument(url, expectedDocument)) {
        event.preventDefault()
      }
    })
  })
}
