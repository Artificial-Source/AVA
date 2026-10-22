import { useState } from 'react'
import type { HostSnapshot, TranscriptItem } from '../../../shared/types.ts'

function ItemView({ item }: { item: TranscriptItem }) {
  const [expanded, setExpanded] = useState(false)
  if (item.kind === 'user') {
    return (
      <article className="px-4 py-3">
        <h2 className="mb-1 text-xs font-medium tracking-wide text-faint uppercase">You</h2>
        <p className="whitespace-pre-wrap text-ink">{item.text}</p>
      </article>
    )
  }
  if (item.kind === 'assistant') {
    return (
      <article className="px-4 py-3">
        <h2 className="mb-1 text-xs font-medium tracking-wide text-faint uppercase">
          AVA{item.streaming ? ' · writing' : ''}
        </h2>
        <p className="whitespace-pre-wrap text-ink">{item.text || (item.streaming ? '…' : '')}</p>
      </article>
    )
  }
  if (item.kind === 'tool') {
    const long = item.summary.length > 80
    return (
      <article className="px-4 py-2">
        <button
          type="button"
          className="w-full border border-line bg-sunken/60 px-3 py-2 text-left text-sm text-muted"
          onClick={() => setExpanded((value) => !value)}
          aria-expanded={expanded}
        >
          <span className="text-ink">{item.tool}</span>
          <span className="text-faint"> · {item.status}</span>
          {(expanded || !long) && item.summary ? (
            <span className="mt-1 block whitespace-pre-wrap text-muted">{item.summary}</span>
          ) : item.summary ? (
            <span className="mt-1 block truncate text-muted">{item.summary}</span>
          ) : null}
        </button>
      </article>
    )
  }
  if (item.kind === 'error') {
    return (
      <article className="px-4 py-3">
        <h2 className="mb-1 text-xs font-medium tracking-wide text-danger uppercase">Error</h2>
        <p className="whitespace-pre-wrap text-danger">{item.text}</p>
      </article>
    )
  }
  return (
    <article className="px-4 py-2">
      <p className="text-sm text-muted">{item.text}</p>
    </article>
  )
}

export function Transcript({ snapshot }: { snapshot: HostSnapshot }) {
  if (snapshot.connection === 'starting') {
    return (
      <div className="flex flex-1 flex-col justify-center px-6 text-muted">
        <p>Starting AVA…</p>
        <p className="mt-2 text-sm text-faint">Workspace and model status will appear when the backend is ready.</p>
      </div>
    )
  }
  if (snapshot.connection === 'error' && snapshot.items.length === 0) {
    return (
      <div className="flex flex-1 flex-col justify-center px-6">
        <p className="text-danger">{snapshot.connectionError?.title ?? 'Backend unavailable'}</p>
        <p className="mt-2 max-w-[40rem] whitespace-pre-wrap text-ink">
          {snapshot.connectionError?.message ?? 'AVA did not start.'}
        </p>
        {snapshot.connectionError?.hint ? (
          <p className="mt-2 max-w-[40rem] text-sm text-muted">{snapshot.connectionError.hint}</p>
        ) : null}
      </div>
    )
  }
  if (snapshot.items.length === 0) {
    return (
      <div className="flex flex-1 flex-col justify-center px-6 text-muted">
        <p>No messages in this session.</p>
        <p className="mt-2 max-w-[36rem] text-sm text-faint">
          Send a prompt when the backend is idle. This client talks to a real AVA process; it will not invent a
          successful reply if the binary, workspace, or provider is missing.
        </p>
      </div>
    )
  }
  return (
    <div className="min-h-0 flex-1 overflow-y-auto divide-y divide-line/80">
      {snapshot.items.map((item) => (
        <ItemView key={item.id} item={item} />
      ))}
    </div>
  )
}
