# Language-server protocol support

AVA can expose configured local LSP servers to the model as permission-controlled tools. It is optional: without valid configuration, LSP tools are unavailable. This is the current runtime contract; general AVA configuration and permissions remain in [CONFIG.md](../core/configuration.md#lsp-servers) and [CONFIG.md](../core/configuration.md#permission-rules), and command-containment limits are in [security/containment.md](../security/containment.md).

## Configuration files and schema

AVA reads, in order:

```text
$XDG_CONFIG_HOME/ava/lsp.json
<workspace>/.ava/lsp.json
```

The global file loads before the project file. Missing files are harmless; a malformed present file disables configured LSP for that context. Explicit servers from both files share an eight-server total and ids must be unique. A project file is considered only after the workspace is trusted; it is executable project content, not ordinary passive configuration.

Schema version is exactly `1`. `servers` is an explicit array (it may be empty); each item is:

```json
{
  "id": "typescript",
  "argv": ["/absolute/path/to/typescript-language-server", "--stdio"],
  "file_extensions": [".ts", ".tsx"],
  "language_id": "typescript",
  "timeout_ms": 3000,
  "startup_timeout_ms": 5000
}
```

`id` is 1–64 bytes of letters, digits, `_`, `-`, or `.`. `argv` is required, has 1–32 nonempty arguments, and each argument is at most 4096 bytes with no control byte. `file_extensions` is optional; an empty/missing array matches every file, otherwise entries are `.` plus letters/digits/`_`/`-`/`+` and at most 32 bytes. `language_id` defaults to `plaintext`, is at most 64 bytes, and permits letters/digits/`_`/`-`/`+`/`#`. `timeout_ms` defaults to 3000 and `startup_timeout_ms` defaults to `timeout_ms`; each is an integer from 100 through 30000 milliseconds. Config files are capped at 64 KiB.

Global server commands must be absolute paths or trusted-PATH command names; they may not contain workspace-relative executable or script arguments such as `./server`, `.ava/server.js`, or `node_modules/.bin/server`. Those are permitted only in trusted project config. A global server starts with the global config directory as cwd, unless it is workspace-contained, in which case `/` is used; the LSP workspace root remains the current workspace. A project server starts in the trusted workspace.

## Built-in `clangd`

No server is enabled by default. The only built-in recipe is `clangd`, and only the owner-controlled global file may opt in:

```json
{"version": 1, "servers": [], "builtin_servers": ["clangd"]}
```

`builtin_servers` is absent/empty to disable it, accepts only one unique exact `clangd` id, and is rejected in project config. A global config which uses it must be current-user-owned, single-link, and not group/other writable. A configured explicit server with id `clangd` takes precedence over the built-in recipe.

The recipe executes `clangd --background-index` for C/C++/Objective-C extensions `.c`, `.cc`, `.cpp`, `.cxx`, `.h`, `.hh`, `.hpp`, `.hxx`, `.m`, and `.mm`, with language id `cpp`. Discovery examines `/usr/local/bin`, `/usr/bin`, `/bin`, then `$HOME/.local/bin`. It rejects workspace-local candidates, symlinks, scripts, hardlinks, group/world-writable files or directories, unsafe ownership, non-ELF files, and identities that change before launch. A built-in executable is descriptor/metadata identity-bound when permission is requested and when it launches.

For a clangd file query, AVA picks the nearest ancestor within the workspace containing `compile_commands.json`, `compile_flags.txt`, or `.clangd`, searching at most 64 levels; otherwise it uses the workspace root. Clients are cached by server id and selected root.

## Selection, permissions, and execution

For file operations, the first configured server whose extension list matches is used. Workspace-symbol queries visit all configured servers. Servers are loaded and inspected lazily: inspection does not launch a process or prompt, and launch happens only when an LSP tool needs it.

Each configured provider keeps at most eight live clients across all server/root pairs. Before launching another, AVA evicts the least recently used idle client and finishes its process teardown. If all eight clients are busy, the query returns a capacity error that can be retried after an active query completes; active clients are not evicted.

Launching a server requires the `lsp.server.launch` permission operation. Every tool query requires `lsp.query`; normal AVA permission policy, project trust, headless policy, and stored rules apply. `lsp.server.launch` and `lsp.query` may be addressed by the permission-rule interfaces described in [CONFIG.md](../core/configuration.md#permission-rules). A denial or unavailable configuration does not silently fall back to an arbitrary executable.

The subprocess has stdin/stdout LSP pipes, closes inherited non-standard descriptors, uses a fixed executable search PATH (`/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`), and gets a narrow environment only: `HOME`, `USER`, `LOGNAME`, `TMPDIR`, `TMP`, `TEMP`, `LANG`, `LANGUAGE`, `LC_ALL`, other `LC_*`, `XDG_CONFIG_HOME`, `XDG_CACHE_HOME`, `XDG_DATA_HOME`, `XDG_STATE_HOME`, `TERM`, `COLORTERM`, `PATH`, and its logical `PWD`. Provider credentials/base URLs, cloud/token/secret variables, every `AVA_*` variable, compiler/runtime homes, and all other ambient values are intentionally not inherited.

## File acquisition, deadlines, and cancellation

Workspace documents are acquired through descriptor-relative anchored reads, not by trusting a path after validation. Reads reject unsafe final objects (including FIFO and symlink replacement), enforce workspace scope where required, check size before allocation, and read one validated descriptor snapshot. A document is limited to 512 KiB and AVA keeps at most 64 open documents.

Startup has its configured startup timeout and requests their configured request timeout. Definition/reference use one absolute request deadline across file acquisition, `didOpen`/document synchronization, and the request/response exchange rather than resetting the budget per phase. Cancellation is checked during startup, I/O, and requests; cancellation is reported as cancellation rather than being converted to a timeout. On timeout, cancellation, crash, or teardown, AVA terminates the verified LSP process group with TERM, then KILL after a 50 ms grace period. As with other process groups, descendants that escape with `setsid` are outside PGID teardown; see [security/containment.md](../security/containment.md#verified-scope-and-limitations).

LSP framing bounds are 64 KiB headers and 4 MiB messages. Server diagnostics caches retain at most 64 documents, 2048 diagnostics, 16 KiB per diagnostic message/code, and 2 MiB total cached diagnostic data. Invalid protocol messages/capabilities and out-of-workspace published diagnostics are rejected rather than reflected unchecked.

## Model tools and result conventions

| Tool | Required fields | Result |
| --- | --- | --- |
| `lsp_diagnostics` | `path` | Diagnostics with severity, message, zero-based line/column, and code. |
| `lsp_document_symbols` | `path` | Symbol names, kinds, ranges, containers, and paths. |
| `lsp_workspace_symbols` | `query` | Symbols across configured servers. |
| `lsp_definition` | `path`, `line`, `column` | Definition locations/ranges. |
| `lsp_references` | `path`, `line`, `column` | Reference locations/ranges. |

Paths are workspace-relative model inputs, limited to 4096 bytes. Workspace-symbol queries are limited to 1024 bytes. Lines and columns are nonnegative **zero-based LSP positions**. Location/symbol paths are normalized to workspace-relative form when possible. Each tool response is bounded to 200 entries and 64 KiB JSON and includes `truncated` plus the original total when AVA truncates it. Server error detail is reduced to safe context; server command lines, raw protocol payloads, and unrelated local paths are not exposed.

## Diagnostics and testing

`ava doctor`/configuration inspection can report whether each config exists, loaded, byte count, configured-server count, parse errors, and built-in `clangd` status (`disabled`, `available`, `not-found`, or `unsafe`) without launching it. Use that before granting a launch permission.

Focused coverage is in `tests/lsp_tests.cpp` with `ava_fake_lsp_server`: schema/trust checks, safe config/file acquisition, environment filtering, process cleanup, cancellation/deadlines, framing/cache limits, tool serialization, root selection, and built-in identity checks. The optional real-clangd smoke is gated by `AVA_LSP_REAL_CLANGD_SMOKE`; it is not required for ordinary tests. Run configured-tree tests through `scripts/run-tests.sh --build-dir build -R lsp` as described in [TESTING.md](../operations/testing.md).
