import { useEffect, useMemo, useState } from 'react'
import { Composer } from './components/Composer.tsx'
import { Dialogs } from './components/Dialogs.tsx'
import { ErrorNotice } from './components/ErrorNotice.tsx'
import { Sidebar } from './components/Sidebar.tsx'
import { Transcript } from './components/Transcript.tsx'
import { ipcFailure } from './ipc-failure.ts'
import { emptyHostSnapshot, type ActionResult, type HostSnapshot, type UserFacingError } from '../../shared/types.ts'

const initialSnapshot = emptyHostSnapshot('starting', null)

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
  const [snapshot, setSnapshot] = useState<HostSnapshot>(initialSnapshot)
  // Failures main returns without publishing them in the snapshot (guards, invalid workspace).
  const [actionError, setActionError] = useState<UserFacingError | null>(null)
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
    void api.getSnapshot().then(
      (value) => {
        if (cancelled) {
          return
        }
        if ('connection' in value) {
          setSnapshot(value)
        } else {
          setActionError(value.error)
        }
      },
      (error: unknown) => {
        if (!cancelled) {
          setActionError(ipcFailure(error))
        }
      }
    )
    const unsubscribe = api.subscribe(setSnapshot)
    return () => {
      cancelled = true
      unsubscribe()
    }
  }, [])

  function runAction(action: () => Promise<ActionResult>): void {
    setActionError(null)
    void action().then(
      (result) => {
        if (!result.ok) {
          setActionError(result.error)
        }
      },
      (error: unknown) => setActionError(ipcFailure(error))
    )
  }

  const idle = snapshot.connection === 'ready' && !snapshot.busy
  const modelLabel = snapshot.state
    ? `${snapshot.state.provider}/${snapshot.state.model}`
    : 'model unavailable'
  const workspaceLabel = snapshot.state?.workspaceDir || snapshot.workspace || 'no workspace'
  // A dead backend is the root cause of any later action failure, so it wins.
  const error =
    (snapshot.connection === 'error' ? snapshot.connectionError : null) ?? actionError ?? snapshot.lastActionError

  return (
    <div className="flex h-full min-h-0 bg-canvas text-ink">
      <Sidebar
        snapshot={snapshot}
        open={sidebarOpen}
        onClose={() => setSidebarOpen(false)}
        onNew={() => runAction(() => window.avaDesktop.newSession())}
        onOpen={(sessionId) => runAction(() => window.avaDesktop.openSession(sessionId))}
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
                  runAction(() => window.avaDesktop.setModel(model, provider || undefined))
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
            onClick={() => runAction(() => window.avaDesktop.selectWorkspace())}
          >
            Workspace
          </button>
          <button
            type="button"
            className="border border-line px-2 py-1 text-sm text-ink enabled:hover:bg-sunken disabled:text-faint"
            disabled={snapshot.busy || snapshot.connection === 'starting'}
            onClick={() => runAction(() => window.avaDesktop.restart())}
          >
            Restart
          </button>
        </header>
        {error ? <ErrorNotice error={error} className="border-b border-danger-soft px-4 py-2" /> : null}
        <Transcript snapshot={snapshot} />
        <Composer
          disabled={!idle}
          busy={snapshot.busy}
          canceling={snapshot.canceling}
          onSend={(message) => runAction(() => window.avaDesktop.prompt(message))}
          onCancel={() => runAction(() => window.avaDesktop.cancel())}
        />
      </div>
      <Dialogs
        permission={snapshot.pendingPermission}
        question={snapshot.pendingQuestion}
        canceling={snapshot.canceling}
        onCancel={() => runAction(() => window.avaDesktop.cancel())}
        onPermission={(displayToken, decision) => window.avaDesktop.replyPermission(displayToken, decision)}
        onQuestion={(displayToken, reply) => window.avaDesktop.replyQuestion(displayToken, reply)}
      />
    </div>
  )
}
