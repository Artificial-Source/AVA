import { useEffect, useMemo, useState } from 'react'
import { Composer } from './components/Composer.tsx'
import { Dialogs } from './components/Dialogs.tsx'
import { Sidebar } from './components/Sidebar.tsx'
import { Transcript } from './components/Transcript.tsx'
import type { HostSnapshot } from '../../shared/types.ts'

const emptySnapshot: HostSnapshot = {
  connection: 'starting',
  connectionError: null,
  lastActionError: null,
  protocol: null,
  state: null,
  binaryPath: '',
  workspace: '',
  sessions: [],
  models: [],
  items: [],
  busy: false,
  canceling: false,
  pendingPermission: null,
  pendingQuestion: null
}

function statusLabel(snapshot: HostSnapshot): string {
  if (snapshot.connection === 'starting') {
    return 'starting'
  }
  if (snapshot.connection === 'ready') {
    return snapshot.busy ? (snapshot.canceling ? 'canceling' : 'running') : 'idle'
  }
  if (snapshot.connection === 'stopped') {
    return 'stopped'
  }
  return 'error'
}

export function App() {
  const [snapshot, setSnapshot] = useState<HostSnapshot>(emptySnapshot)
  const [sidebarOpen, setSidebarOpen] = useState(true)
  const selectedModel = useMemo(() => {
    const selected = snapshot.models.find((model) => model.selected)
    if (selected) {
      return `${selected.provider}::${selected.model}`
    }
    if (snapshot.state) {
      return `${snapshot.state.provider}::${snapshot.state.model}`
    }
    return ''
  }, [snapshot.models, snapshot.state])

  useEffect(() => {
    let cancelled = false
    const api = window.avaDesktop
    void api.getSnapshot().then((value) => {
      if (!cancelled) {
        setSnapshot(value)
      }
    })
    const unsubscribe = api.subscribe(setSnapshot)
    return () => {
      cancelled = true
      unsubscribe()
    }
  }, [])

  const idle = snapshot.connection === 'ready' && !snapshot.busy
  const modelLabel = snapshot.state
    ? `${snapshot.state.provider}/${snapshot.state.model}`
    : 'model unavailable'
  const workspaceLabel = snapshot.state?.workspaceDir || snapshot.workspace || 'no workspace'
  const error = snapshot.lastActionError ?? (snapshot.connection === 'error' ? snapshot.connectionError : null)

  return (
    <div className="flex h-full min-h-0 bg-canvas text-ink">
      <Sidebar
        snapshot={snapshot}
        open={sidebarOpen}
        onClose={() => setSidebarOpen(false)}
        onNew={() => {
          void window.avaDesktop.newSession()
        }}
        onOpen={(sessionId) => {
          void window.avaDesktop.openSession(sessionId)
        }}
      />
      <div className="flex min-w-0 flex-1 flex-col">
        <header className="flex flex-wrap items-center gap-x-3 gap-y-2 border-b border-line bg-panel px-3 py-2">
          {sidebarOpen ? null : (
            <button
              type="button"
              className="border border-line px-2 py-1 text-sm text-ink hover:bg-sunken"
              onClick={() => setSidebarOpen(true)}
            >
              Sessions
            </button>
          )}
          <p className="min-w-0 flex-1 truncate text-sm text-muted">
            <span className="text-ink">{statusLabel(snapshot)}</span>
            <span className="text-faint"> · </span>
            <span title={workspaceLabel}>{workspaceLabel}</span>
            <span className="text-faint"> · </span>
            <span title={modelLabel}>{modelLabel}</span>
            {snapshot.protocol ? (
              <>
                <span className="text-faint"> · </span>
                <span>RPC {snapshot.protocol.protocolVersion}</span>
              </>
            ) : null}
          </p>
          <label className="flex items-center gap-2 text-sm text-muted">
            <span>Model</span>
            <select
              className="max-w-[16rem] border border-line bg-canvas px-2 py-1 text-ink disabled:text-faint"
              value={selectedModel}
              disabled={!idle || snapshot.models.length === 0}
              onChange={(event) => {
                const [provider, ...rest] = event.target.value.split('::')
                const model = rest.join('::')
                if (model) {
                  void window.avaDesktop.setModel(model, provider || undefined)
                }
              }}
            >
              {snapshot.models.length === 0 ? <option value="">No models</option> : null}
              {snapshot.models.map((model) => (
                <option
                  key={`${model.provider}::${model.model}`}
                  value={`${model.provider}::${model.model}`}
                  disabled={!model.selectable}
                >
                  {model.displayName || `${model.provider}/${model.model}`}
                </option>
              ))}
            </select>
          </label>
          <button
            type="button"
            className="border border-line px-2 py-1 text-sm text-ink enabled:hover:bg-sunken disabled:text-faint"
            disabled={!idle}
            onClick={() => {
              void window.avaDesktop.selectWorkspace()
            }}
          >
            Workspace
          </button>
          <button
            type="button"
            className="border border-line px-2 py-1 text-sm text-ink enabled:hover:bg-sunken disabled:text-faint"
            disabled={snapshot.busy || snapshot.connection === 'starting'}
            onClick={() => {
              void window.avaDesktop.restart()
            }}
          >
            Restart
          </button>
        </header>
        {error ? (
          <div className="border-b border-danger-soft bg-danger-soft px-4 py-2 text-sm text-danger" role="alert">
            <p className="font-medium">{error.title}</p>
            <p className="whitespace-pre-wrap text-ink">{error.message}</p>
            {error.hint ? <p className="mt-1 text-muted">{error.hint}</p> : null}
          </div>
        ) : null}
        <Transcript snapshot={snapshot} />
        <Composer
          disabled={!idle}
          busy={snapshot.busy}
          canceling={snapshot.canceling}
          onSend={(message) => {
            void window.avaDesktop.prompt(message)
          }}
          onCancel={() => {
            void window.avaDesktop.cancel()
          }}
        />
      </div>
      <Dialogs
        permission={snapshot.pendingPermission}
        question={snapshot.pendingQuestion}
        canceling={snapshot.canceling}
        onCancel={() => { void window.avaDesktop.cancel() }}
        onPermission={(displayToken, decision) => window.avaDesktop.replyPermission(displayToken, decision)}
        onQuestion={(displayToken, reply) => window.avaDesktop.replyQuestion(displayToken, reply)}
      />
    </div>
  )
}
