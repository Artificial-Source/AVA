import { existsSync, statSync } from 'node:fs'
import { isAbsolute, resolve } from 'node:path'
import type { UserFacingError } from './types.ts'

export type LaunchPaths = {
  binaryPath: string
  binarySource: 'env' | 'default'
  workspace: string
  workspaceSource: 'env' | 'cwd'
}

export type LaunchResolution = { ok: true; value: LaunchPaths } | { ok: false; error: UserFacingError }

function isExecutableFile(path: string): boolean {
  try {
    const stat = statSync(path)
    return stat.isFile() && (stat.mode & 0o111) !== 0
  } catch {
    return false
  }
}

function isDirectory(path: string): boolean {
  try {
    return statSync(path).isDirectory()
  } catch {
    return false
  }
}

export function defaultBinaryPath(packageRoot: string): string {
  return resolve(packageRoot, '../../build/ava')
}

export function resolveLaunchPaths(input: {
  envBinary: string | undefined
  envWorkspace: string | undefined
  packageRoot: string
  cwd: string
  requireExistingBinary?: boolean
}): LaunchResolution {
  let binaryPath: string
  let binarySource: 'env' | 'default'
  if (input.envBinary !== undefined && input.envBinary !== '') {
    if (!isAbsolute(input.envBinary)) {
      return {
        ok: false,
        error: {
          title: 'Invalid AVA_DESKTOP_BINARY',
          message: 'AVA_DESKTOP_BINARY must be an absolute path to an AVA executable.',
          hint: 'Set AVA_DESKTOP_BINARY to the full path of build/ava. See docs/desktop/README.md.'
        }
      }
    }
    binaryPath = input.envBinary
    binarySource = 'env'
  } else {
    binaryPath = defaultBinaryPath(input.packageRoot)
    binarySource = 'default'
  }

  if (input.requireExistingBinary !== false && !existsSync(binaryPath)) {
    return {
      ok: false,
      error: {
        title: 'AVA binary not found',
        message: `No AVA executable at ${binaryPath}.`,
        hint:
          binarySource === 'env'
            ? 'Point AVA_DESKTOP_BINARY at a built Linux ava executable.'
            : 'Build AVA (scripts/build.sh) or set AVA_DESKTOP_BINARY to an absolute executable path. See docs/desktop/README.md.'
      }
    }
  }

  if (input.requireExistingBinary !== false && !isExecutableFile(binaryPath)) {
    return {
      ok: false,
      error: {
        title: 'AVA binary is not executable',
        message: `${binaryPath} exists but is not an executable file.`,
        hint: 'Use the built Linux ava binary from the CMake build tree.'
      }
    }
  }

  let workspace: string
  let workspaceSource: 'env' | 'cwd'
  if (input.envWorkspace !== undefined && input.envWorkspace !== '') {
    if (!isAbsolute(input.envWorkspace)) {
      return {
        ok: false,
        error: {
          title: 'Invalid AVA_DESKTOP_WORKSPACE',
          message: 'AVA_DESKTOP_WORKSPACE must be an absolute existing directory.',
          hint: 'Set AVA_DESKTOP_WORKSPACE to a temporary or project directory. The renderer cannot choose a cwd itself.'
        }
      }
    }
    workspace = input.envWorkspace
    workspaceSource = 'env'
  } else {
    workspace = resolve(input.cwd)
    workspaceSource = 'cwd'
  }

  if (!isDirectory(workspace)) {
    return {
      ok: false,
      error: {
        title: workspaceSource === 'env' ? 'Invalid AVA_DESKTOP_WORKSPACE' : 'Workspace is not a directory',
        message: `${workspace} is not an existing directory.`,
        hint: 'Choose a workspace with the native folder dialog or set AVA_DESKTOP_WORKSPACE to an absolute directory.'
      }
    }
  }

  return {
    ok: true,
    value: { binaryPath, binarySource, workspace, workspaceSource }
  }
}

export function validateWorkspaceDirectory(path: string): UserFacingError | null {
  if (!isAbsolute(path)) {
    return {
      title: 'Invalid workspace',
      message: 'Workspace path must be absolute.',
      hint: 'Use the application folder dialog. Arbitrary renderer paths are rejected.'
    }
  }
  if (!isDirectory(path)) {
    return {
      title: 'Invalid workspace',
      message: `${path} is not an existing directory.`,
      hint: 'Select an existing folder.'
    }
  }
  return null
}
