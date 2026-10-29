import { app, session } from 'electron'
import { DEV_CSP, PRODUCTION_CSP } from '../shared/content-security-policy.ts'
import { isExpectedDocument } from '../shared/ipc-contract.ts'

export function applySessionGuards(devServer: boolean): void {
  const electronSession = session.defaultSession
  const csp = devServer ? DEV_CSP : PRODUCTION_CSP
  electronSession.webRequest.onHeadersReceived((details, callback) => {
    const headers = { ...details.responseHeaders, 'Content-Security-Policy': [csp] }
    callback({ responseHeaders: headers })
  })
  electronSession.setPermissionRequestHandler((_webContents, _permission, callback) => {
    callback(false)
  })
  electronSession.setPermissionCheckHandler(() => false)
}

// Applies to every webContents, including the main window's.
export function installApplicationGuards(expectedDocument: string): void {
  const guardNavigation = (event: { preventDefault(): void }, url: string) => {
    if (!isExpectedDocument(url, expectedDocument)) {
      event.preventDefault()
    }
  }
  app.on('web-contents-created', (_event, contents) => {
    contents.setWindowOpenHandler(() => ({ action: 'deny' }))
    contents.on('will-attach-webview', (event) => {
      event.preventDefault()
    })
    contents.on('will-navigate', guardNavigation)
    contents.on('will-redirect', guardNavigation)
  })
}
