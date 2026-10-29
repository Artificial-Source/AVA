import { useState, type KeyboardEvent } from 'react'

type ComposerProps = {
  disabled: boolean
  busy: boolean
  canceling: boolean
  onSend: (message: string) => void
  onCancel: () => void
}

export function Composer({ disabled, busy, canceling, onSend, onCancel }: ComposerProps) {
  const [draft, setDraft] = useState('')

  function send(): void {
    const message = draft.trim()
    if (!message || disabled) {
      return
    }
    setDraft('')
    onSend(message)
  }

  function onKeyDown(event: KeyboardEvent<HTMLTextAreaElement>): void {
    if (event.nativeEvent.isComposing || event.key === 'Process' || event.keyCode === 229) {
      return
    }
    if (event.key === 'Enter' && !event.shiftKey) {
      event.preventDefault()
      send()
    }
  }

  return (
    <form
      className="border-t border-line bg-panel px-4 py-3"
      onSubmit={(event) => {
        event.preventDefault()
        send()
      }}
    >
      <label htmlFor="ava-composer" className="mb-1.5 block text-xs tracking-wide text-faint uppercase">
        Message
      </label>
      <textarea
        id="ava-composer"
        className="block min-h-[5.5rem] w-full resize-y border border-line bg-canvas px-3 py-2 text-ink placeholder:text-faint disabled:bg-sunken"
        placeholder={busy ? 'Wait for the current run, or cancel it.' : 'Write a prompt. Enter sends, Shift+Enter inserts a newline.'}
        value={draft}
        onChange={(event) => setDraft(event.target.value)}
        onKeyDown={onKeyDown}
        disabled={disabled && !busy}
      />
      <div className="mt-2 flex flex-wrap items-center gap-2">
        <button
          type="submit"
          className="border border-accent bg-accent px-3 py-1.5 text-sm text-panel enabled:hover:opacity-90 enabled:active:scale-[0.99] disabled:border-line disabled:bg-sunken disabled:text-faint"
          disabled={disabled || !draft.trim()}
        >
          Send
        </button>
        <button
          type="button"
          className="border border-line px-3 py-1.5 text-sm text-ink enabled:hover:bg-sunken disabled:text-faint"
          disabled={!busy || canceling}
          onClick={onCancel}
        >
          {canceling ? 'Canceling…' : 'Cancel'}
        </button>
        <span className="text-xs text-faint">Enter to send</span>
      </div>
    </form>
  )
}
