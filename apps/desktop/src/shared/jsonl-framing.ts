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

// Splits a byte stream into LF-delimited UTF-8 records. Each byte is scanned once and copied
// at most twice (once into the partial-record list, once into the joined record), so a large
// record arriving in many small chunks costs O(record) rather than O(record²).
export class JsonlFramer {
  readonly maxBytes: number
  readonly #decoder = new TextDecoder('utf-8', { fatal: true, ignoreBOM: false })
  // Bytes of the current unterminated record. Invariant: no chunk here contains LF.
  #partial: Uint8Array[] = []
  #partialBytes = 0

  constructor(maxBytes = DEFAULT_MAX_FRAME_BYTES) {
    this.maxBytes = maxBytes
  }

  push(chunk: Uint8Array): string[] {
    const records: string[] = []
    let start = 0
    while (start < chunk.length) {
      const delimiter = chunk.indexOf(LF, start)
      if (delimiter < 0) {
        this.#appendPartial(chunk.subarray(start))
        break
      }
      const record = this.#takeRecord(chunk.subarray(start, delimiter))
      start = delimiter + 1
      if (record !== undefined) {
        records.push(record)
      }
    }
    return records
  }

  finish(): string | undefined {
    if (this.#partialBytes === 0) {
      return undefined
    }
    return this.#takeRecord(new Uint8Array(0))
  }

  #appendPartial(bytes: Uint8Array): void {
    if (this.#partialBytes + bytes.length > this.maxBytes) {
      this.#clearPartial()
      throw new FramingError('max_frame', `RPC frame exceeded ${this.maxBytes} bytes without a newline`)
    }
    // Copy: the caller owns `bytes` and may reuse its buffer once push() returns.
    this.#partial.push(new Uint8Array(bytes))
    this.#partialBytes += bytes.length
  }

  // Joins the buffered partial record with `tail` (the bytes before LF or EOF) and decodes it.
  #takeRecord(tail: Uint8Array): string | undefined {
    const size = this.#partialBytes + tail.length
    const parts = this.#partial
    this.#clearPartial()
    if (size > this.maxBytes) {
      throw new FramingError('max_frame', `RPC frame exceeded ${this.maxBytes} bytes`)
    }
    let raw = tail
    if (parts.length > 0) {
      raw = new Uint8Array(size)
      let offset = 0
      for (const part of parts) {
        raw.set(part, offset)
        offset += part.length
      }
      raw.set(tail, offset)
    }
    const record = stripTrailingCr(raw)
    return record.length === 0 ? undefined : decodeRecord(record, this.#decoder)
  }

  #clearPartial(): void {
    this.#partial = []
    this.#partialBytes = 0
  }
}
