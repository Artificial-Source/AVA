import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'

// A source-level layout guard, not a substitute for the blocked real-window smoke.
test('permission and question dialogs bound and scroll oversized content', () => {
  const source = readFileSync(new URL('../src/renderer/src/components/Dialogs.tsx', import.meta.url), 'utf8')
  const contentClasses = [...source.matchAll(/className="([^"]*fixed[^"]*top-1\/2[^"]*)"/g)]
  assert.equal(contentClasses.length, 2)
  for (const match of contentClasses) {
    const classes = match[1].split(/\s+/)
    assert.ok(classes.includes('max-h-[calc(100vh-2rem)]'), 'modal content must fit within the viewport')
    assert.ok(classes.includes('overflow-y-auto'), 'long commands and questions must scroll to their actions')
  }
})
