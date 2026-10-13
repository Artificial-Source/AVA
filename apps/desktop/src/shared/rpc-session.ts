import { JsonlFramer } from './jsonl-framing.ts'
import { asObject, asString, errorMessage, isProtocolId } from './text.ts'
import type { JsonObject, RpcErrorShape } from './types.ts'

export class RpcError extends Error {
  readonly category: string
  readonly code: string
  readonly details: string

  constructor(shape: RpcErrorShape) {
    super(shape.message || 'RPC error')
    this.name = 'RpcError'
    this.category = shape.category
    this.code = shape.code
    this.details = shape.details
  }
}

export type RpcEvent = {
  name: string
  type: string
  eventId: string
  schemaVersion: number | null
  requestId: string
  correlationId: string
  payload: JsonObject
  raw: JsonObject
}

export type RpcSessionOptions = {
  write: (bytes: Uint8Array) => void
  onEvent?: (event: RpcEvent) => void
  onFailure?: (error: Error) => void
  generateId?: (prefix: string) => string
}

type Pending = {
  resolve: (value: JsonObject) => void
  reject: (error: Error) => void
}

const encoder = new TextEncoder()

function parseError(value: unknown): RpcErrorShape {
  const object = asObject(value) ?? {}
  return {
    category: asString(object.category) ?? 'internal',
    code: asString(object.code) ?? 'internal_error',
    message: asString(object.message) ?? 'RPC error',
    details: asString(object.details) ?? ''
  }
}

export function parseRpcEvent(record: JsonObject): RpcEvent | null {
  const name = asString(record.name) ?? asString(record.type)
  if (!name) {
    return null
  }
  const payload = asObject(record.payload) ?? {}
  return {
    name,
    type: asString(record.type) ?? name,
    eventId: asString(record.event_id) ?? '',
    schemaVersion: typeof record.schema_version === 'number' ? record.schema_version : null,
    requestId: asString(record.request_id) ?? '',
    correlationId: asString(record.correlation_id) ?? '',
    payload,
    raw: record
  }
}

export class RpcSession {
  readonly #framer = new JsonlFramer()
  readonly #pending = new Map<string, Pending>()
  readonly #write: (bytes: Uint8Array) => void
  readonly #onEvent?: (event: RpcEvent) => void
  readonly #onFailure?: (error: Error) => void
  readonly #generateId: (prefix: string) => string
  #seq = 0
  #failed: Error | null = null

  constructor(options: RpcSessionOptions) {
    this.#write = options.write
    this.#onEvent = options.onEvent
    this.#onFailure = options.onFailure
    this.#generateId =
      options.generateId ??
      ((prefix: string) => {
        this.#seq += 1
        return `${prefix}-${this.#seq.toString(16)}`
      })
  }

  get pendingSize(): number {
    return this.#pending.size
  }

  get failed(): Error | null {
    return this.#failed
  }

  request(type: string, payload: JsonObject = {}, idPrefix = 'req'): Promise<JsonObject> {
    if (this.#failed) {
      return Promise.reject(this.#failed)
    }
    const id = this.#generateId(idPrefix)
    if (!isProtocolId(id)) {
      return Promise.reject(new Error('generated RPC request id is invalid'))
    }
    if (this.#pending.has(id)) {
      return Promise.reject(new Error('RPC request id is already pending'))
    }
    const record: JsonObject = {
      id,
      type,
      protocol_version: 1,
      ...payload
    }
    const line = encoder.encode(`${JSON.stringify(record)}\n`)
    return new Promise((resolve, reject) => {
      this.#pending.set(id, { resolve, reject })
      try {
        this.#write(line)
      } catch (error) {
        this.#pending.delete(id)
        this.fail(error instanceof Error ? error : new Error(errorMessage(error)))
        reject(this.#failed ?? error)
      }
    })
  }

  handleStdout(chunk: Uint8Array): void {
    if (this.#failed) {
      return
    }
    try {
      for (const record of this.#framer.push(chunk)) {
        this.#handleRecord(record)
      }
    } catch (error) {
      this.fail(error instanceof Error ? error : new Error(errorMessage(error)))
    }
  }

  handleStdoutEnd(): void {
    if (this.#failed) {
      return
    }
    try {
      const last = this.#framer.finish()
      if (last !== undefined) {
        this.#handleRecord(last)
      }
    } catch (error) {
      this.fail(error instanceof Error ? error : new Error(errorMessage(error)))
      return
    }
    this.fail(new Error('AVA RPC stdout closed'))
  }

  fail(error: Error): void {
    if (this.#failed) {
      this.#rejectAll(this.#failed)
      return
    }
    this.#failed = error
    this.#rejectAll(error)
    this.#onFailure?.(error)
  }

  #rejectAll(error: Error): void {
    const pending = [...this.#pending.values()]
    this.#pending.clear()
    for (const waiter of pending) {
      waiter.reject(error)
    }
  }

  #handleRecord(text: string): void {
    let parsed: unknown
    try {
      parsed = JSON.parse(text)
    } catch {
      throw new Error('RPC stdout record is not valid JSON')
    }
    const record = asObject(parsed)
    if (!record) {
      throw new Error('RPC stdout record is not an object')
    }
    if (asString(record.type) === 'response') {
      this.#handleResponse(record)
      return
    }
    const event = parseRpcEvent(record)
    if (event) {
      this.#onEvent?.(event)
    }
  }

  #handleResponse(record: JsonObject): void {
    const id = asString(record.id)
    if (!id) {
      return
    }
    const waiter = this.#pending.get(id)
    if (!waiter) {
      return
    }
    this.#pending.delete(id)
    if (record.success === true) {
      const result = asObject(record.result) ?? {}
      waiter.resolve(result)
      return
    }
    waiter.reject(new RpcError(parseError(record.error)))
  }
}
