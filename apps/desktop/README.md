# AVA Desktop (optional)

An optional Electron client that drives a local AVA process over [RPC v1](../../docs/rpc-protocol.md). It is independent of the C++ build: ordinary `cmake` / `scripts/build.sh` work does not need Node.

This package is a **legacy prototype**. It is not the greenfield desktop design baseline. Planning and research live in the [desktop documentation index](../../docs/desktop/README.md).

Linux x64 is AVA's only qualified backend platform. This client can run on other Electron hosts, but it does not make Windows or macOS AVA backends supported.

## Requirements

- Node 24.13.1
- pnpm 10.29.1
- A built Linux AVA executable (default development path: `../../build/ava`)

## Commands

```sh
cd apps/desktop
pnpm install
pnpm typecheck
pnpm test
pnpm build
pnpm dev
pnpm preview
```

`pnpm dev` starts Vite HMR and Electron. The dev server is pinned to `http://127.0.0.1:5173` with `strictPort`, so `pnpm dev` fails if that port is already taken. `pnpm preview` runs the production build.

## Ubuntu sandbox prerequisite

On Ubuntu hosts with `kernel.apparmor_restrict_unprivileged_userns=1`, Electron can fail before app code if its standard sandbox helper is user-owned with mode `0755`. For the **pinned, trusted Electron 44.5.0 installation only**, an administrator must explicitly approve and configure `node_modules/.pnpm/electron@44.5.0/node_modules/electron/dist/chrome-sandbox` with owner **`root:root`** and mode **`4755`** (setuid). This is not automated; the prerequisite must be redone after reinstalling Electron.

Do not bypass the Chromium sandbox or globally disable AppArmor. GUI validation on the current Ubuntu host remains blocked pending administrator approval and configuration; passing Node tests and builds does not establish GUI success.

## Trusted launch environment

Main process only. The renderer cannot choose a binary or arbitrary cwd.

| Variable | Meaning |
| --- | --- |
| `AVA_DESKTOP_BINARY` | Absolute path to an AVA executable. If unset, development default is `../../build/ava` from this package. |
| `AVA_DESKTOP_WORKSPACE` | Absolute existing directory used as AVA's cwd/workspace. If unset, Electron's cwd is used. |

The UI can also pick a workspace through the main-owned native directory dialog. That path is validated in main before the backend is restarted.

## Opt-in RPC smoke

Credential-free, uses `--rpc --offline` and temporary `HOME` / XDG directories:

```sh
AVA_DESKTOP_RPC_SMOKE=1 AVA_DESKTOP_BINARY=/absolute/path/to/ava pnpm test
```

Do not point this at your real home directory.

## Current scope

Implemented (GUI validation blocked as noted above): workspace/backend/model status, session list, new/open while idle, model select while idle, composer with streamed text, collapsed tool summaries, explicit cancel, one-shot Allow/Deny permissions with the backend's read-only diff preview, structured questions, native workspace picker, safe restart while idle.

Not in this client: auth UI, persistent permission grants/rules, arbitrary bash, uploads, plugin/MCP controls, full Markdown/diff editing, remote servers, installers, or packaged distribution.

Missing binary, spawn, auth, and provider errors are shown as errors. They are not turned into a fake successful chat.

## Security

Renderer: `sandbox`, `contextIsolation`, `nodeIntegration: false`, `webSecurity`. Main validates IPC sender (webContents, main frame, exact expected app document URL (fragments allowed)) and payloads. Permission/question replies echo a main-generated display token and bind host-tracked resolver IDs from backend events; the renderer cannot supply RPC correlation. Duplicate or stale display decisions are rejected. Stderr is drained and not content-logged.

Content-Security-Policy lives in `src/shared/content-security-policy.ts`. Main sends it as a response header (this covers the `pnpm dev` server, whose URL main trusts only when electron-vite dev mode started an unpackaged app, and only at the pinned origin in `src/shared/dev-server.ts`); because Electron cannot attach headers to `file://` documents, the production build also embeds it as a `<meta>` tag in `index.html`. Navigation, redirects, new windows, `<webview>`, and permission requests are denied.

Backend lifecycle: AVA must answer the RPC handshake within 30 s or the client reports an error. Starts and restarts (including workspace changes) run one at a time: while one is in progress, Restart is disabled and further requests are rejected, so only one AVA is ever live. On restart or quit, main closes AVA's stdin (AVA cancels active work and exits), then sends `SIGTERM` and finally `SIGKILL` if the process is still alive after 1.5 s each; quit waits for every AVA it started to exit, so AVA never outlives the client.
