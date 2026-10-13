const CONTROL_OR_META = /[\u0000-\u001f\u007f\s;|&`$<>\\'"*?{}[\]()]/

export function asObject(value: unknown): Record<string, unknown> | null {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    return null
  }
  return value as Record<string, unknown>
}

export function asString(value: unknown): string | null {
  return typeof value === 'string' ? value : null
}

export function asBoolean(value: unknown): boolean | null {
  return typeof value === 'boolean' ? value : null
}

export function asNumber(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}

export function asStringArray(value: unknown): string[] | null {
  if (!Array.isArray(value) || value.some((entry) => typeof entry !== 'string')) {
    return null
  }
  return value
}

export function utf8ByteLength(value: string): number {
  return new TextEncoder().encode(value).length
}

export function isProtocolId(value: unknown, maxBytes = 256): value is string {
  if (typeof value !== 'string' || value.length === 0) {
    return false
  }
  if (utf8ByteLength(value) > maxBytes) {
    return false
  }
  return !CONTROL_OR_META.test(value)
}

export function isNonEmptyString(value: unknown, maxBytes: number): value is string {
  if (typeof value !== 'string' || value.length === 0) {
    return false
  }
  return utf8ByteLength(value) <= maxBytes
}

export function truncateText(value: string, maxChars: number): string {
  if (value.length <= maxChars) {
    return value
  }
  return `${value.slice(0, Math.max(0, maxChars - 1))}…`
}

export function readStringField(object: Record<string, unknown>, key: string): string {
  return asString(object[key]) ?? ''
}

export function errorMessage(error: unknown): string {
  if (error instanceof Error && error.message) {
    return error.message
  }
  return 'Unknown error'
}
