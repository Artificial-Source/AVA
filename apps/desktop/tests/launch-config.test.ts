import assert from 'node:assert/strict'
import { chmodSync, mkdtempSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { test } from 'node:test'
import { DEV_SERVER_ORIGIN, trustedDevRendererUrl } from '../src/shared/dev-server.ts'
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

test('the dev renderer URL is trusted only from electron-vite dev at the pinned origin', () => {
  const dev = { NODE_ENV_ELECTRON_VITE: 'development' }
  assert.equal(DEV_SERVER_ORIGIN, 'http://127.0.0.1:5173')
  assert.equal(trustedDevRendererUrl({ ...dev, ELECTRON_RENDERER_URL: DEV_SERVER_ORIGIN }, false), DEV_SERVER_ORIGIN)
  assert.equal(trustedDevRendererUrl({ ...dev, ELECTRON_RENDERER_URL: `${DEV_SERVER_ORIGIN}/` }, false), `${DEV_SERVER_ORIGIN}/`)
  assert.equal(trustedDevRendererUrl({ ...dev, ELECTRON_RENDERER_URL: DEV_SERVER_ORIGIN }, true), null, 'never for a packaged app')
  assert.equal(trustedDevRendererUrl({ ELECTRON_RENDERER_URL: DEV_SERVER_ORIGIN }, false), null, 'only in electron-vite dev mode')
  assert.equal(
    trustedDevRendererUrl({ NODE_ENV_ELECTRON_VITE: 'production', ELECTRON_RENDERER_URL: DEV_SERVER_ORIGIN }, false),
    null
  )
  for (const url of [
    'http://localhost:5173', 'http://127.0.0.1:5174', 'https://127.0.0.1:5173', 'http://[::1]:5173',
    'http://127.0.0.2:5173', 'http://127.0.0.1', 'http://evil.example:5173', 'not a url', ''
  ]) {
    assert.equal(trustedDevRendererUrl({ ...dev, ELECTRON_RENDERER_URL: url }, false), null, url)
  }
  assert.equal(trustedDevRendererUrl(dev, false), null)
})
