import * as Dialog from '@radix-ui/react-dialog'
import { useReducedMotion } from 'motion/react'
import { useRef, useState } from 'react'
import { createQuestionFormState, selectQuestionOption, serializeQuestionForm, setQuestionCustom } from '../../../shared/question-form.ts'
import type { PermissionPromptView, QuestionPromptView, QuestionReplyInput } from '../../../shared/types.ts'

type DialogsProps = {
  permission: PermissionPromptView | null
  question: QuestionPromptView | null
  canceling: boolean
  onCancel: () => void
  onPermission: (displayToken: string, decision: 'allow' | 'deny') => Promise<unknown>
  onQuestion: (displayToken: string, reply: QuestionReplyInput) => Promise<unknown>
}

export function Dialogs({ permission, question, canceling, onCancel, onPermission, onQuestion }: DialogsProps) {
  const reduceMotion = useReducedMotion()
  const overlayClass = reduceMotion
    ? 'fixed inset-0 z-20 bg-ink/30'
    : 'fixed inset-0 z-20 bg-ink/30 data-[state=open]:animate-[fade_120ms_ease-out]'

  return (
    <>
      <Dialog.Root open={permission !== null}>
        <Dialog.Portal>
          <Dialog.Overlay className={overlayClass} />
          <Dialog.Content onEscapeKeyDown={(event) => event.preventDefault()} onPointerDownOutside={(event) => event.preventDefault()} className="fixed top-1/2 left-1/2 z-30 max-h-[calc(100vh-2rem)] w-[min(32rem,calc(100vw-2rem))] -translate-x-1/2 -translate-y-1/2 overflow-y-auto border border-line bg-panel p-5 shadow-[0_12px_40px_-24px_rgba(42,39,35,0.5)]">
            <Dialog.Title className="text-base font-medium text-ink">Permission required</Dialog.Title>
            <Dialog.Description className="mt-2 text-sm text-muted">
              AVA is waiting for an explicit Allow or Deny. This client does not remember grants.
            </Dialog.Description>
            {permission ? (
              <dl className="mt-4 grid grid-cols-[auto_1fr] gap-x-3 gap-y-1 text-sm">
                <Field label="Operation" value={permission.operation} />
                <Field label="Tool" value={permission.toolName} />
                <Field label="Target" value={permission.targetPath} />
                <Field label="Command" value={permission.command} />
                <Field label="Reason" value={permission.reason} />
                <Field label="Risk" value={permission.risk} />
                <Field label="Mode" value={permission.mode} />
              </dl>
            ) : null}
            {permission ? (
              <PermissionActions key={permission.displayToken} permission={permission} canceling={canceling} onCancel={onCancel} onReply={onPermission} />
            ) : null}
          </Dialog.Content>
        </Dialog.Portal>
      </Dialog.Root>

      <Dialog.Root open={question !== null && permission === null}>
        <Dialog.Portal>
          <Dialog.Overlay className={overlayClass} />
          <Dialog.Content onEscapeKeyDown={(event) => event.preventDefault()} onPointerDownOutside={(event) => event.preventDefault()} className="fixed top-1/2 left-1/2 z-30 max-h-[calc(100vh-2rem)] w-[min(32rem,calc(100vw-2rem))] -translate-x-1/2 -translate-y-1/2 overflow-y-auto border border-line bg-panel p-5 shadow-[0_12px_40px_-24px_rgba(42,39,35,0.5)]">
            {question ? (
              <QuestionForm key={question.displayToken} question={question} canceling={canceling} onCancel={onCancel} onSubmit={onQuestion} />
            ) : null}
          </Dialog.Content>
        </Dialog.Portal>
      </Dialog.Root>
    </>
  )
}

function Field({ label, value }: { label: string; value: string }) {
  if (!value) {
    return null
  }
  return (
    <>
      <dt className="text-faint">{label}</dt>
      <dd className="break-all text-ink">{value}</dd>
    </>
  )
}

function CancelRun({ canceling, onCancel }: { canceling: boolean; onCancel: () => void }) {
  return (
    <button type="button" disabled={canceling} onClick={onCancel} className="border border-line px-3 py-1.5 text-sm text-ink enabled:hover:bg-sunken disabled:text-faint">
      {canceling ? 'Canceling…' : 'Cancel run'}
    </button>
  )
}

function PermissionActions({ permission, canceling, onCancel, onReply }: {
  permission: PermissionPromptView
  canceling: boolean
  onCancel: () => void
  onReply: DialogsProps['onPermission']
}) {
  const [pending, setPending] = useState(false)
  const inFlight = useRef(false)

  async function reply(decision: 'allow' | 'deny'): Promise<void> {
    if (inFlight.current || canceling) {
      return
    }
    inFlight.current = true
    setPending(true)
    try {
      await onReply(permission.displayToken, decision)
    } finally {
      inFlight.current = false
      setPending(false)
    }
  }

  return (
    <div className="mt-5 flex justify-end gap-2">
      <CancelRun canceling={canceling} onCancel={onCancel} />
      <button type="button" disabled={pending || canceling} className="border border-line px-3 py-1.5 text-sm text-ink enabled:hover:bg-sunken disabled:text-faint" onClick={() => { void reply('deny') }}>Deny</button>
      <button type="button" disabled={pending || canceling} className="border border-accent bg-accent px-3 py-1.5 text-sm text-panel enabled:hover:opacity-90 disabled:opacity-50" onClick={() => { void reply('allow') }}>Allow</button>
    </div>
  )
}

function QuestionForm({ question, canceling, onCancel, onSubmit }: {
  question: QuestionPromptView
  canceling: boolean
  onCancel: () => void
  onSubmit: DialogsProps['onQuestion']
}) {
  const [state, setState] = useState(() => createQuestionFormState(question.displayToken))
  const [pending, setPending] = useState(false)
  const inFlight = useRef(false)
  const reply = serializeQuestionForm(state, question)

  async function submit(): Promise<void> {
    if (!reply || inFlight.current || canceling) {
      return
    }
    inFlight.current = true
    setPending(true)
    try {
      await onSubmit(question.displayToken, reply)
    } finally {
      inFlight.current = false
      setPending(false)
    }
  }

  return (
    <form onSubmit={(event) => { event.preventDefault(); void submit() }}>
      <Dialog.Title className="text-base font-medium text-ink">{question.header || 'Question'}</Dialog.Title>
      <Dialog.Description className="mt-2 whitespace-pre-wrap text-sm text-ink">{question.question}</Dialog.Description>
      {question.options.length > 0 ? (
        <fieldset disabled={pending || canceling} className="mt-4 flex flex-col gap-2">
          <legend className="sr-only">{question.multiple ? 'Choose one or more options' : 'Choose one option'}</legend>
          {question.options.map((option) => (
            <label key={option.value} className="flex items-start gap-2 text-sm text-ink">
              <input
                type={question.multiple ? 'checkbox' : 'radio'}
                name="ava-question"
                value={option.value}
                checked={state.selected.includes(option.value)}
                onChange={() => setState((current) => selectQuestionOption(current, question, option.value))}
                className="mt-1"
              />
              <span>{option.label}</span>
            </label>
          ))}
        </fieldset>
      ) : null}
      {question.allowCustom ? (
        <div className="mt-4 flex flex-col gap-1.5">
          <label htmlFor="ava-question-custom" className="text-sm text-muted">
            {question.secret ? 'Secret answer' : 'Custom answer'}
          </label>
          <input
            id="ava-question-custom"
            className="border border-line bg-canvas px-3 py-2 text-ink"
            type={question.secret ? 'password' : 'text'}
            autoComplete="off"
            disabled={pending || canceling}
            value={state.custom}
            onChange={(event) => setState((current) => setQuestionCustom(current, question, event.target.value))}
          />
        </div>
      ) : null}
      <div className="mt-5 flex justify-end gap-2">
        <CancelRun canceling={canceling} onCancel={onCancel} />
        <button
          type="submit"
          className="border border-accent bg-accent px-3 py-1.5 text-sm text-panel enabled:hover:opacity-90 disabled:border-line disabled:bg-sunken disabled:text-faint"
          disabled={!reply || pending || canceling}
        >
          Reply
        </button>
      </div>
    </form>
  )
}
