import { contextBridge, ipcRenderer, type IpcRendererEvent } from 'electron'
import { IPC_CHANNELS, IPC_EVENTS } from '../shared/ipc-contract.ts'
import type {
  ActionFailure,
  ActionResult,
  HostSnapshot,
  PermissionDecision,
  QuestionReplyInput
} from '../shared/types.ts'

// Result types mirror what main returns for each channel (src/main/index.ts, backend.ts handle()).
const api = {
  getSnapshot: (): Promise<HostSnapshot | ActionFailure> => ipcRenderer.invoke(IPC_CHANNELS.getSnapshot),
  selectWorkspace: (): Promise<ActionResult> => ipcRenderer.invoke(IPC_CHANNELS.selectWorkspace),
  restart: (): Promise<ActionResult> => ipcRenderer.invoke(IPC_CHANNELS.restart),
  newSession: (): Promise<ActionResult> => ipcRenderer.invoke(IPC_CHANNELS.newSession),
  openSession: (sessionId: string): Promise<ActionResult> =>
    ipcRenderer.invoke(IPC_CHANNELS.openSession, { sessionId }),
  setModel: (model: string, provider?: string): Promise<ActionResult> =>
    ipcRenderer.invoke(IPC_CHANNELS.setModel, provider ? { model, provider } : { model }),
  prompt: (message: string): Promise<ActionResult> => ipcRenderer.invoke(IPC_CHANNELS.prompt, { message }),
  cancel: (): Promise<ActionResult> => ipcRenderer.invoke(IPC_CHANNELS.cancel),
  replyPermission: (displayToken: string, decision: PermissionDecision): Promise<ActionResult> =>
    ipcRenderer.invoke(IPC_CHANNELS.replyPermission, { displayToken, decision }),
  replyQuestion: (displayToken: string, reply: QuestionReplyInput): Promise<ActionResult> =>
    ipcRenderer.invoke(IPC_CHANNELS.replyQuestion, { ...reply, displayToken }),
  subscribe: (listener: (snapshot: HostSnapshot) => void): (() => void) => {
    const wrapped = (_event: IpcRendererEvent, snapshot: HostSnapshot) => {
      listener(snapshot)
    }
    ipcRenderer.on(IPC_EVENTS.snapshot, wrapped)
    return () => {
      ipcRenderer.removeListener(IPC_EVENTS.snapshot, wrapped)
    }
  }
}

contextBridge.exposeInMainWorld('avaDesktop', api)

export type AvaDesktopApi = typeof api
