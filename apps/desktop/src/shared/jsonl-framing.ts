const LF = 0x0a
const CR = 0x0d
export const DEFAULT_MAX_FRAME_BYTES = 32 * 1024 * 1024

export class FramingError extends Error {
  readonly code: 'utf8' | 'max_frame'

  constructor(code: 'utf8' | 'max_frame', message: string) {
    super(message)
    this.name = 'FramingError'
    this.code = code
  }
}

function concatBytes(left: Uint8Array, right: Uint8Array): Uint8Array {
  const next = new Uint8Array(left.length + right.length)
  next.set(left)
  next.set(right, left.length)
  return next
}

function decodeRecord(bytes: Uint8Array, decoder: TextDecoder): string {
  try {
    return decoder.decode(bytes)
  } catch {
    throw new FramingError('utf8', 'RPC record is not valid UTF-8')
  }
}

function stripTrailingCr(bytes: Uint8Array): Uint8Array {
  if (bytes.length > 0 && bytes[bytes.length - 1] === CR) {
    return bytes.subarray(0, bytes.length - 1)
  }
  return bytes
}

export class JsonlFramer {
  #buffer: Uint8Array = new Uint8Array(0)
  readonly maxBytes: number
  readonly #decoder = new TextDecoder('utf-8', { fatal: true, ignoreBOM: false })

  constructor(maxBytes = DEFAULT_MAX_FRAME_BYTES) {
    this.maxBytes = maxBytes
  }

  get bufferedBytes(): number {
    return this.#buffer.length
  }

  push(chunk: Uint8Array): string[] {
    if (chunk.length === 0) {
      return []
    }
    this.#buffer = concatBytes(this.#buffer, chunk)
    if (this.#buffer.length > this.maxBytes) {
      const hasLf = this.#buffer.indexOf(LF) >= 0
      if (!hasLf) {
        this.#buffer = new Uint8Array(0)
        throw new FramingError('max_frame', `RPC frame exceeded ${this.maxBytes} bytes without a newline`)
      }
    }

    const records: string[] = []
    while (this.#buffer.length > 0) {
      const delimiter = this.#buffer.indexOf(LF)
      if (delimiter < 0) {
        if (this.#buffer.length > this.maxBytes) {
          this.#buffer = new Uint8Array(0)
          throw new FramingError('max_frame', `RPC frame exceeded ${this.maxBytes} bytes without a newline`)
        }
        break
      }
      const raw = this.#buffer.subarray(0, delimiter)
      this.#buffer = this.#buffer.subarray(delimiter + 1)
      if (raw.length > this.maxBytes) {
        throw new FramingError('max_frame', `RPC frame exceeded ${this.maxBytes} bytes`)
      }
      const record = stripTrailingCr(raw)
      if (record.length === 0) {
        continue
      }
      records.push(decodeRecord(record, this.#decoder))
    }
    return records
  }

  finish(): string | undefined {
    if (this.#buffer.length === 0) {
      return undefined
    }
    if (this.#buffer.length > this.maxBytes) {
      this.#buffer = new Uint8Array(0)
      throw new FramingError('max_frame', `RPC frame exceeded ${this.maxBytes} bytes at EOF`)
    }
    const record = stripTrailingCr(this.#buffer)
    this.#buffer = new Uint8Array(0)
    if (record.length === 0) {
      return undefined
    }
    return decodeRecord(record, this.#decoder)
  }
}
