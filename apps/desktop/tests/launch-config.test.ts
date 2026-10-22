import assert from 'node:assert/strict'
import { chmodSync, mkdtempSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { test } from 'node:test'
import { resolveLaunchPaths, validateWorkspaceDirectory } from '../src/shared/launch-config.ts'

test('rejects relative binary and workspace env values', () => {
  const workspace = mkdtempSync(join(tmpdir(), 'ava-desktop-ws-'))
  const relativeBinary = resolveLaunchPaths({
    envBinary: 'build/ava',
    envWorkspace: workspace,
    packageRoot: workspace,
    cwd: workspace
  })
  assert.equal(relativeBinary.ok, false)
  const relativeWorkspace = resolveLaunchPaths({
    envBinary: undefined,
    envWorkspace: 'relative-ws',
    packageRoot: workspace,
    cwd: workspace,
    requireExistingBinary: false
  })
  assert.equal(relativeWorkspace.ok, false)
})

test('accepts absolute existing workspace and executable binary', () => {
  const root = mkdtempSync(join(tmpdir(), 'ava-desktop-launch-'))
  const binary = join(root, 'ava')
  writeFileSync(binary, '#!/bin/sh\nexit 0\n', { mode: 0o755 })
  chmodSync(binary, 0o755)
  const resolved = resolveLaunchPaths({
    envBinary: binary,
    envWorkspace: root,
    packageRoot: root,
    cwd: root
  })
  assert.equal(resolved.ok, true)
  if (resolved.ok) {
    assert.equal(resolved.value.binaryPath, binary)
    assert.equal(resolved.value.workspace, root)
    assert.equal(resolved.value.binarySource, 'env')
    assert.equal(resolved.value.workspaceSource, 'env')
  }
})

test('native dialog paths must be absolute existing directories', () => {
  const root = mkdtempSync(join(tmpdir(), 'ava-desktop-dir-'))
  assert.equal(validateWorkspaceDirectory(root), null)
  assert.notEqual(validateWorkspaceDirectory('tmp'), null)
  assert.notEqual(validateWorkspaceDirectory(join(root, 'missing')), null)
})
