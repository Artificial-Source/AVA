import { contextBridge, ipcRenderer, type IpcRendererEvent } from 'electron'
import { IPC_CHANNELS, IPC_EVENTS } from '../shared/ipc-contract.ts'
import type { HostSnapshot, QuestionReplyInput } from '../shared/types.ts'

const api = {
  getSnapshot: (): Promise<HostSnapshot> => ipcRenderer.invoke(IPC_CHANNELS.getSnapshot),
  selectWorkspace: (): Promise<unknown> => ipcRenderer.invoke(IPC_CHANNELS.selectWorkspace),
  restart: (): Promise<unknown> => ipcRenderer.invoke(IPC_CHANNELS.restart),
  newSession: (): Promise<unknown> => ipcRenderer.invoke(IPC_CHANNELS.newSession),
  openSession: (sessionId: string): Promise<unknown> =>
    ipcRenderer.invoke(IPC_CHANNELS.openSession, { sessionId }),
  setModel: (model: string, provider?: string): Promise<unknown> =>
    ipcRenderer.invoke(IPC_CHANNELS.setModel, provider ? { model, provider } : { model }),
  prompt: (message: string): Promise<unknown> => ipcRenderer.invoke(IPC_CHANNELS.prompt, { message }),
  cancel: (): Promise<unknown> => ipcRenderer.invoke(IPC_CHANNELS.cancel),
  replyPermission: (displayToken: string, decision: 'allow' | 'deny'): Promise<unknown> =>
    ipcRenderer.invoke(IPC_CHANNELS.replyPermission, { displayToken, decision }),
  replyQuestion: (displayToken: string, reply: QuestionReplyInput): Promise<unknown> =>
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
