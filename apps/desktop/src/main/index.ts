import { dirname, join } from 'node:path'
import { fileURLToPath, pathToFileURL } from 'node:url'
import { app, BrowserWindow, dialog, ipcMain, type IpcMainInvokeEvent, type OpenDialogOptions } from 'electron'
import { trustedDevRendererUrl } from '../shared/dev-server.ts'
import { IPC_CHANNELS, IPC_EVENTS, isTrustedSender } from '../shared/ipc-contract.ts'
import type { ActionResult, HostSnapshot } from '../shared/types.ts'
import { AvaBackend } from './backend.ts'
import { applySessionGuards, installApplicationGuards } from './security.ts'

const mainDir = dirname(fileURLToPath(import.meta.url))
const devUrl = trustedDevRendererUrl(process.env, app.isPackaged)
const expectedDocument = devUrl ?? pathToFileURL(join(mainDir, '../renderer/index.html')).href

let mainWindow: BrowserWindow | null = null
let backend: AvaBackend | null = null
let queuedSnapshot: HostSnapshot | null = null
let backendClosing = false
let backendClosed = false

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
      devTools: devUrl !== null
    }
  })
  // A destroyed BrowserWindow throws on property access; forget it as soon as it closes.
  win.on('closed', () => {
    if (mainWindow === win) {
      mainWindow = null
    }
  })
  win.setMenuBarVisibility(false)
  void win.loadURL(expectedDocument)
  return win
}

// Snapshots are whole-state, so a burst of backend events (one stdout chunk can hold many
// stream deltas) collapses into one IPC message carrying the latest snapshot.
function sendSnapshot(snapshot: HostSnapshot): void {
  const flushScheduled = queuedSnapshot !== null
  queuedSnapshot = snapshot
  if (flushScheduled) {
    return
  }
  setImmediate(() => {
    const latest = queuedSnapshot
    queuedSnapshot = null
    const win = mainWindow
    if (latest && win && !win.isDestroyed() && !win.webContents.isDestroyed()) {
      win.webContents.send(IPC_EVENTS.snapshot, latest)
    }
  })
}

async function chooseWorkspace(): Promise<string | null> {
  const options: OpenDialogOptions = { title: 'Choose AVA workspace', properties: ['openDirectory'] }
  const picked = mainWindow ? await dialog.showOpenDialog(mainWindow, options) : await dialog.showOpenDialog(options)
  return picked.canceled ? null : (picked.filePaths[0] ?? null)
}

function registerIpc(): void {
  for (const channel of Object.values(IPC_CHANNELS)) {
    ipcMain.handle(channel, async (event, payload: unknown): Promise<HostSnapshot | ActionResult> => {
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
  applySessionGuards(devUrl !== null)
  registerIpc()
  backend = new AvaBackend({
    packageRoot: app.getAppPath(),
    cwd: process.cwd(),
    env: process.env,
    chooseWorkspace
  })
  mainWindow = createWindow()
  backend.subscribe(sendSnapshot)
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

// Hold the quit until AVA has seen stdin EOF and exited (or been killed), so the backend
// never outlives the client.
app.on('before-quit', (event) => {
  if (!backend || backendClosed) {
    return
  }
  event.preventDefault()
  if (backendClosing) {
    return
  }
  backendClosing = true
  const quit = () => {
    backendClosed = true
    app.quit()
  }
  void backend.stop('application quit').then(quit, quit)
})
