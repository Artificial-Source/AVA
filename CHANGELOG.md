# Changelog

All notable changes to AVA are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
A version here does not mean that a tag or public release exists.

## [Unreleased]

## [1.0.0] - UNRELEASED

These notes are ready for owner review. AVA 1.0.0 has not been published.

### Added

- A native terminal coding assistant with a multiline editor, prompt history,
  file completion, model and session pickers, themes, and keybindings. The
  terminal shows Markdown, code, diffs, tool activity, and permission requests.
  You can select transcript text, copy answers, and expand long thinking blocks.
- Print mode for scripts, a versioned local JSONL RPC interface, and a separate
  ACP endpoint for editor clients. Interactive mode needs a terminal on both
  input and output. Use `--print` or `--rpc` for non-interactive runs.
- OpenAI, Anthropic, Gemini, and OpenAI-compatible provider support. You can add
  providers and models through local configuration. OpenAI supports browser and
  device OAuth as well as API keys. Model switching keeps visible conversation
  history and removes provider-private data that the new model cannot use.
- Local sessions with resume, names, labels, usage and cost estimates, branches,
  forks, clones, compaction, image attachments, and local export/import.
- File reading, writing, editing, patching, search, shell, and web tools.
  Project and global `AGENTS.md` files, prompts, and skills supply local context.
  Configured language servers can provide diagnostics, symbols, definitions,
  and references. The optional automatic recipe uses an already installed
  `clangd`; AVA does not download language servers.
- Configurable primary agents, task subagents, and background jobs. Foreground
  children can ask for permission. Background actions that need a new approval
  fail closed. Print and RPC users can opt into bounded parallel read/search
  tools; other ordinary tool calls run sequentially.
- Local out-of-process plugins and stdio MCP servers. Plugin tools and commands
  use permission checks. Direct foreground TUI plugin commands can show bounded,
  attributed status, widgets, choices, and confirmation dialogs.
- Persistent allow/ask/deny rules, permission audits, and command containment.
  Project trust does not grant permission to run tools. Offline mode disables
  provider calls, but network tools still follow their normal permission rules.
- User guides for configuration, providers, terminal setup, tools, extensions,
  troubleshooting, and local automation. Linux packages include the runtime
  guides, license notices, and build provenance.

### Fixed

- Persistent deny rules take precedence across equivalent paths. Malformed
  protected rule files fail closed. File tools reject workspace symlinks that
  escape the workspace and do not hang when a file is replaced by a FIFO.
- Tool calls cut short by a provider token limit never run, even if their
  partial arguments look valid. Gemini requests do not follow redirects that
  could expose API keys, and streamed tool-call IDs stay distinct.
- Every provider family (OpenAI, OpenAI-compatible, Anthropic, and Gemini)
  now applies the same size limits to streamed and buffered replies. An
  oversized reply fails with an error before any tool runs.
- Session writes are synced before AVA reports success. Torn final records can
  recover on supported older kernels. Attachment reads reject symlinks and
  FIFOs. Trace and support files have bounded retention and safer cleanup.
- Contained commands cannot reach host UNIX services. On Landlock ABI 6 or
  newer, they also cannot signal or use abstract sockets outside their domain.
  Unsafe executable paths need explicit approval rather than broader grants.
- Plugin installation uses no-replace publication and checks directory swaps.
  Provider, child-process, cancellation, and terminal regressions have focused
  tests. Release qualification tests and retains the same package bytes rather
  than rebuilding them for publication.

### Supported platform

- The first official artifact is **Linux x64 only**. The CPU must support
  **x86-64-v2** (Nehalem-class or newer). BMI2 and AVX are not required.
- The minimum qualified host is Ubuntu 24.04 with glibc 2.39. The host needs
  libstdc++ providing `GLIBCXX_3.4.32` and `CXXABI_1.3.13`, ncursesw/tinfo ABI 6
  (6.4 or newer), a usable terminfo database, and `curl` 8.5 or newer on `PATH`.
- The inspected binary needs no symbols newer than `GLIBC_2.38`,
  `GLIBCXX_3.4.32`, or `CXXABI_1.3.13`. Its loader is
  `/lib64/ld-linux-x86-64.so.2`. It uses the host's `libc.so.6`, `libm.so.6`,
  `libgcc_s.so.1`, `libstdc++.so.6`, `libncursesw.so.6`, and `libtinfo.so.6`.
  These symbol limits do not qualify older distributions. Python and CMake are
  not required to run AVA.

### Known limitations

- Windows, macOS, AArch64, and other distribution artifacts are not qualified.
  CPU-floor evidence uses a Nehalem CPU profile, not physical Nehalem hardware.
- Provider support does not mean every model or endpoint has passed a live
  test. OpenAI and Kimi-for-coding have recorded live prompt evidence; several
  other providers have offline tests or need working credentials for live tests.
  Cost estimates can be incomplete when pricing or usage data is missing.
- Command containment is not an OS or container sandbox guarantee. Review tool
  requests and local plugin/MCP code before granting access.
- Background jobs and pending result delivery last only for the current AVA
  process. Child session files persist, but jobs do not resume after restart.
- MCP is local stdio only. LSP uses saved files, not unsaved editor buffers.
  Cloud sharing, remote plugin installation, a package marketplace, and broader
  task graphs are not included. Local JSONL export does not bundle attachment
  bytes; keep the session and attachment files when making backups.
- Terminal tests are not screen-reader certification. LaTeX, arbitrary plugin
  renderers, and broad physical-terminal image validation are not included.
- AVA has no telemetry, automatic updates, remote catalog refresh, package-manager
  publication, detached signatures, SBOM, or artifact attestations. SHA-256
  checksums detect changed downloads; they are not independent proof of origin.

See the [release ledger](docs/product/release-readiness.md) for the remaining
approval and exact-candidate GitHub gates, and the
[publication runbook](docs/operations/publication.md) for release policy.

## Historical release-position documentation

Historical capability and release-position details live in
[`docs/versions/`](docs/versions/README.md). They record repository history and
plans, not proof that a release was published.
