# AVA Desktop (Electron)

Optional, independent Electron + React client. Carlo owns the TUI. Do not change C++ sources, `src/ava/tui`, CMake, CI, or RPC contracts from this tree.

## Boundaries

- Talk to AVA only through public RPC v1 (`docs/rpc-protocol.md`). Never read session JSONL or credentials.
- Main owns process spawn, trusted binary/workspace, the native folder dialog, stdin/stdout, and resolver IDs.
- Renderer is sandboxed: no Node, no raw IPC, no generic RPC. Host-generated request IDs only.
- Linux x64 is AVA's only qualified backend platform. Electron's portability is not Windows/macOS backend support.

## Layout

- `src/shared/`: pure JSONL, RPC, IPC validation, resolvers, transcript reducer. Testable without Electron.
- `src/main/`: window, CSP, spawn, trusted launch env (`AVA_DESKTOP_BINARY`, `AVA_DESKTOP_WORKSPACE`).
- `src/preload/`: bundled CommonJS `contextBridge` only.
- `src/renderer/`: calm React UI.

## Checks

`pnpm typecheck`, `pnpm test`, `pnpm build`. Opt-in RPC smoke: `AVA_DESKTOP_RPC_SMOKE=1 AVA_DESKTOP_BINARY=/abs/path/to/ava pnpm test`.
