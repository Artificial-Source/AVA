import { asBoolean, asObject, asString, readStringField } from './text.ts'
import type { PermissionPromptView, QuestionOption, QuestionPromptView, QuestionReplyInput } from './types.ts'
import type { RpcEvent } from './rpc-session.ts'

export type BoundPermission = {
  resolverRequestId: string
  correlationId: string
  view: PermissionPromptView
}

export type BoundQuestion = {
  resolverRequestId: string
  correlationId: string
  view: QuestionPromptView
}

function optionFromUnknown(value: unknown): QuestionOption | null {
  const object = asObject(value)
  if (!object) {
    return null
  }
  const optionValue = asString(object.value)
  if (!optionValue) {
    return null
  }
  return { value: optionValue, label: asString(object.label) ?? optionValue }
}

export function permissionViewFromEvent(event: RpcEvent, displayToken: string): BoundPermission | null {
  const resolverRequestId = asString(event.payload.resolver_request_id)
  const correlationId = event.correlationId
  if (!resolverRequestId || !correlationId) {
    return null
  }
  return {
    resolverRequestId,
    correlationId,
    view: {
      displayToken,
      operation: readStringField(event.payload, 'operation'),
      mode: readStringField(event.payload, 'mode'),
      targetPath: readStringField(event.payload, 'target_path'),
      command: readStringField(event.payload, 'command'),
      toolName: readStringField(event.payload, 'tool_name'),
      reason: readStringField(event.payload, 'reason'),
      risk: readStringField(event.payload, 'risk'),
      diffPreview: readStringField(event.payload, 'diff_preview'),
      diffTruncated: asBoolean(event.payload.diff_truncated) === true
    }
  }
}

export function questionViewFromEvent(event: RpcEvent, displayToken: string): BoundQuestion | null {
  const resolverRequestId = asString(event.payload.resolver_request_id)
  const correlationId = event.correlationId
  if (!resolverRequestId || !correlationId) {
    return null
  }
  const options = Array.isArray(event.payload.options)
    ? event.payload.options.map(optionFromUnknown).filter((option): option is QuestionOption => option !== null)
    : []
  return {
    resolverRequestId,
    correlationId,
    view: {
      displayToken,
      header: readStringField(event.payload, 'header'),
      question: readStringField(event.payload, 'question'),
      options,
      multiple: asBoolean(event.payload.multiple) === true,
      allowCustom: asBoolean(event.payload.allow_custom) === true,
      secret: asBoolean(event.payload.secret) === true
    }
  }
}

export class ResolverStore {
  #permissions: BoundPermission[] = []
  #questions: BoundQuestion[] = []
  #inFlight = new Set<BoundPermission | BoundQuestion>()
  readonly #generateDisplayToken: () => string

  constructor(generateDisplayToken: () => string) {
    this.#generateDisplayToken = generateDisplayToken
  }

  get pendingPermission(): PermissionPromptView | null {
    return this.#permissions[0]?.view ?? null
  }

  get pendingQuestion(): QuestionPromptView | null {
    return this.#questions[0]?.view ?? null
  }

  get permissionCount(): number {
    return this.#permissions.length
  }

  bindEvent(event: RpcEvent): void {
    if (event.name === 'permission_requested') {
      const bound = permissionViewFromEvent(event, this.#generateDisplayToken())
      if (bound) {
        this.#permissions.push(bound)
      }
      return
    }
    if (event.name === 'question_requested') {
      const bound = questionViewFromEvent(event, this.#generateDisplayToken())
      if (bound) {
        this.#questions.push(bound)
      }
    }
  }

  beginPermission(displayToken: string): BoundPermission | null {
    return this.#begin(this.#permissions, displayToken)
  }

  beginQuestion(displayToken: string): BoundQuestion | null {
    return this.#begin(this.#questions, displayToken)
  }

  // Peeking never marks a display in flight, so a locally rejected reply can be retried.
  peekQuestion(displayToken: string): BoundQuestion | null {
    const bound = this.#questions[0]
    return bound && bound.view.displayToken === displayToken ? bound : null
  }

  finishPermission(bound: BoundPermission): boolean {
    return this.#removeMatched(bound, this.#permissions)
  }

  finishQuestion(bound: BoundQuestion): boolean {
    return this.#removeMatched(bound, this.#questions)
  }

  // An accepted reply and a rejected one both retire the matched display; only the caller
  // differs, because a rejected reply also fails the run closed.
  retirePermission(bound: BoundPermission): boolean {
    return this.#removeMatched(bound, this.#permissions)
  }

  retireQuestion(bound: BoundQuestion): boolean {
    return this.#removeMatched(bound, this.#questions)
  }

  // Only the head of a queue is displayed, so only it can be answered, and only once.
  #begin<T extends BoundPermission | BoundQuestion>(queue: T[], displayToken: string): T | null {
    const bound = queue[0]
    if (!bound || bound.view.displayToken !== displayToken || this.#inFlight.has(bound)) {
      return null
    }
    this.#inFlight.add(bound)
    return bound
  }

  #removeMatched(bound: BoundPermission | BoundQuestion, queue: Array<BoundPermission | BoundQuestion>): boolean {
    if (!this.#inFlight.delete(bound) || queue[0] !== bound) {
      return false
    }
    queue.shift()
    return true
  }

  clear(): void {
    // Clearing a run/session invalidates even replies still awaiting transport.
    this.#inFlight.clear()
    this.#permissions = []
    this.#questions = []
  }
}

export function questionReplyFields(reply: QuestionReplyInput): Record<string, unknown> {
  const fields: Record<string, unknown> = {}
  if (reply.answer !== undefined) {
    fields.answer = reply.answer
  }
  if (reply.selected !== undefined) {
    fields.selected = reply.selected
  }
  if (reply.selectedOptions !== undefined) {
    fields.selected_options = reply.selectedOptions
  }
  return fields
}
