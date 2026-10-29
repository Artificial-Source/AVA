import type { UserFacingError } from '../../../shared/types.ts'

export function ErrorNotice({ error, className }: { error: UserFacingError; className: string }) {
  return (
    <div className={`bg-danger-soft text-sm text-danger ${className}`} role="alert">
      <p className="font-medium">{error.title}</p>
      <p className="whitespace-pre-wrap text-ink">{error.message}</p>
      {error.hint ? <p className="mt-1 text-muted">{error.hint}</p> : null}
    </div>
  )
}
