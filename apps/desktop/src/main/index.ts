import { dirname, join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { app, BrowserWindow, ipcMain, type IpcMainInvokeEvent } from 'electron'

const mainDir = dirname(fileURLToPath(import.meta.url))
import { IPC_CHANNELS, IPC_EVENTS, isTrustedSender } from '../shared/ipc-contract.ts'
import { AvaBackend } from './backend.ts'
import { applySessionGuards, guardWindow, installApplicationGuards } from './security.ts'

const expectedDocument = process.env.ELECTRON_RENDERER_URL ||
  pathToFileURL(join(mainDir, '../renderer/index.html')).href

let mainWindow: BrowserWindow | null = null
let backend: AvaBackend | null = null

function packageRoot(): string {
  return app.getAppPath()
}

function senderView(event: IpcMainInvokeEvent) {
  const frame = event.senderFrame
  return {
    senderId: event.sender.id,
    expectedSenderId: mainWindow?.webContents.id ?? -1,
    isDestroyed: event.sender.isDestroyed(),
    isMainFrame: frame !== null && frame === event.sender.mainFrame,
    url: frame?.url ?? ''
  }
}

function createWindow(): BrowserWindow {
  const win = new BrowserWindow({
    width: 1100,
    height: 760,
    minWidth: 720,
    minHeight: 520,
    backgroundColor: '#f3efe6',
    autoHideMenuBar: true,
    webPreferences: {
      preload: join(mainDir, '../preload/index.cjs'),
      sandbox: true,
      contextIsolation: true,
      nodeIntegration: false,
      webSecurity: true,
      devTools: Boolean(process.env.ELECTRON_RENDERER_URL)
    }
  })
  guardWindow(win, expectedDocument)
  win.setMenuBarVisibility(false)
  void win.loadURL(expectedDocument)
  return win
}

function registerIpc(): void {
  for (const channel of Object.values(IPC_CHANNELS)) {
    ipcMain.handle(channel, async (event, payload: unknown) => {
      if (!backend || !mainWindow) {
        return {
          ok: false,
          error: { title: 'Unavailable', message: 'Desktop backend is not ready.', hint: '' }
        }
      }
      if (!isTrustedSender(senderView(event), expectedDocument)) {
        return {
          ok: false,
          error: { title: 'Rejected', message: 'IPC sender is not the application main frame.', hint: '' }
        }
      }
      return backend.handle(channel, payload)
    })
  }
}

app.whenReady().then(async () => {
  installApplicationGuards(expectedDocument)
  applySessionGuards()
  registerIpc()
  backend = new AvaBackend({
    packageRoot: packageRoot(),
    cwd: process.cwd(),
    env: process.env
  })
  mainWindow = createWindow()
  backend.subscribe((snapshot) => {
    if (!mainWindow || mainWindow.webContents.isDestroyed()) {
      return
    }
    mainWindow.webContents.send(IPC_EVENTS.snapshot, snapshot)
  })
  await backend.start()

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      mainWindow = createWindow()
    }
  })
})

app.on('window-all-closed', () => {
  app.quit()
})

app.on('before-quit', () => {
  void backend?.stop('application quit')
})
