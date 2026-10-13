import type { HostSnapshot } from '../../../shared/types.ts'

type SidebarProps = {
  snapshot: HostSnapshot
  open: boolean
  onClose: () => void
  onNew: () => void
  onOpen: (sessionId: string) => void
}

function sessionLabel(title: string, sessionId: string): string {
  if (title.trim()) {
    return title
  }
  if (sessionId.length <= 18) {
    return sessionId
  }
  return `${sessionId.slice(0, 10)}…${sessionId.slice(-4)}`
}

export function Sidebar({ snapshot, open, onClose, onNew, onOpen }: SidebarProps) {
  const idle = snapshot.connection === 'ready' && !snapshot.busy
  if (!open) {
    return null
  }
  return (
    <aside className="flex h-full w-[min(16.5rem,42vw)] shrink-0 flex-col border-r border-line bg-panel">
      <div className="flex items-center justify-between gap-2 border-b border-line px-3 py-2">
        <h1 className="text-sm font-medium tracking-wide text-ink">Sessions</h1>
        <button type="button" className="px-1.5 py-0.5 text-sm text-muted hover:text-ink" onClick={onClose}>
          Hide
        </button>
      </div>
      <div className="px-3 py-2">
        <button
          type="button"
          className="w-full border border-line bg-canvas px-2 py-1.5 text-left text-sm text-ink enabled:hover:border-accent enabled:hover:bg-accent-soft disabled:text-faint"
          onClick={onNew}
          disabled={!idle}
        >
          New session
        </button>
      </div>
      <nav className="min-h-0 flex-1 overflow-y-auto px-2 pb-3" aria-label="Sessions">
        {snapshot.connection === 'starting' ? (
          <p className="px-1 py-2 text-sm text-muted">Loading sessions…</p>
        ) : snapshot.sessions.length === 0 ? (
          <p className="px-1 py-2 text-sm text-muted">No sessions in this workspace yet.</p>
        ) : (
          <ul className="flex flex-col gap-0.5">
            {snapshot.sessions.map((session) => {
              const current = session.sessionId === snapshot.state?.sessionId
              return (
                <li key={session.sessionId}>
                  <button
                    type="button"
                    className={`w-full rounded-sm px-2 py-1.5 text-left ${
                      current ? 'bg-accent-soft text-ink' : 'text-ink hover:bg-sunken'
                    } disabled:text-faint`}
                    disabled={!idle || current}
                    onClick={() => onOpen(session.sessionId)}
                  >
                    <span className="block truncate text-sm">{sessionLabel(session.title, session.sessionId)}</span>
                    <span className="block truncate text-xs text-faint">
                      {session.lastUpdated || `${session.entryCount} entries`}
                    </span>
                  </button>
                </li>
              )
            })}
          </ul>
        )}
      </nav>
    </aside>
  )
}
