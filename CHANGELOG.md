# Changelog

All notable changes to AVA are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
A version here does not mean that a tag or public release exists.

## [Unreleased]

The runtime version is `0.32.0`. This is not a stable release. The terminal
frontend is still in active development. A stable 1.0 release will follow once
it is complete. No release is scheduled yet.

### Changed

- Restore the runtime version to `0.32.0`. The May backend version bump did
  not mark a stable product release. Keep the release tooling and its recorded
  evidence available for future releases.
- CI Release builds no longer restore the SDK job's compiler cache by
  mistake, which had forced an almost full rebuild. Local ccache use is now
  documented.
- Agent startup reports tool-registry composition errors instead of silently
  using empty tools. Context-dependent metadata and schema APIs return explicit
  errors, and returned metadata owns its strings.
- C++ builds enable warnings for non-virtual destructors, hidden virtual methods,
  fallthrough, formatting, and sign comparisons. GCC also checks duplicate
  conditions.
- Desktop CI installs locked dependencies with lifecycle scripts disabled, then
  runs type checks and tests without downloading or running Electron.
- Minimum-host smoke checks run the resolved container image and retain its
  identity, available image digests, and installed package versions.

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

- LSP launch admission reserves capacity before running permission/startup callbacks
  outside the cache lock; reentrant callbacks cannot launch the same client twice.
  ACP session-list cursors commit only after bounded serialization succeeds, and
  direct ACP JSON-value APIs enforce the wire codec's limits. RPC reusable grants
  are capped at 256 entries and 1 MiB of accounted retained data; rejected grants
  remain pending for revocation, clearing, or a one-shot reply. Plugin installation
  rejects a source directory replaced after inspection, even with the same plugin ID.
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
  FIFOs. Attachment import, replay, and branch copies stay inside the
  session folder even if a folder is swapped for a symlink while AVA works.
  Trace and support files have bounded retention and safer cleanup.
- Contained commands cannot reach host UNIX services. On Landlock ABI 6 or
  newer, they also cannot signal or use abstract sockets outside their domain.
  Unsafe executable paths need explicit approval rather than broader grants.
- Plugin installation uses no-replace publication and checks directory swaps.
  Provider, child-process, cancellation, and terminal regressions have focused
  tests. Release qualification tests and retains the same package bytes rather
  than rebuilding them for publication.
- Session recovery rejects renamed files and extra hard links. Session creation
  changes permissions through the opened directory rather than a pathname that
  could have been replaced.
- Session loading and catalog listing report filesystem and read errors instead
  of returning empty results. Compatibility reads reject symlinks and special
  files, and in-memory histories enforce serialized byte limits.
- Session copies and recovery invalidate pending append state before changing
  history, preventing stale writers from duplicating or restoring output.
  Throwing write callbacks report uncertain writes and require recovery.
- Attachments are fully written and synced before becoming visible, including
  on filesystems without unnamed temporary files. Publication never overwrites
  an existing attachment, and failed temporary-file cleanup is reported.
- Session catalogs sort from stable timestamp snapshots with deterministic
  ties. Title generation no longer deadlocks shutdown or writes metadata after
  scheduling has stopped.
- Process waits honor the full deadline when polling rounds a timeout down.
  Launch and containment errors retain the original operation and cause.
- Contained commands prepare access rules before child startup and reject
  changed file identities without reopening paths in the child. Child launches
  close unrelated descriptors even after the open-file limit has been lowered.
- Process supervision retains ownership of unreaped children after bounded
  cleanup or monitor shutdown, waiting only for those exact children and leaving
  unrelated processes to their owners.
- Browser launch reports setup and execution failures without leaking unrelated
  open files. Pipes and temporary descriptors in audited launch and I/O paths
  are non-inheritable from creation.
- Read-only subagents cannot widen their parent's allowed tools, including when
  tool lists do not overlap, use aliases, or request exact tool composition.
- Edits and previews require the same approved filesystem access as reads for
  external files. Growing files cannot bypass the read-size limit.
- Permission rules reject malformed Unicode and tolerate interrupted reads.
  Permission-list filters handle extra whitespace, and secret-file restrictions
  cover Google Cloud configuration files regardless of their names.
- Context freshness checks and external-editor results use bounded, validated
  regular-file reads. Trust read failures remain errors, and context folder
  names beginning with two dots no longer trigger false traversal errors.
- OAuth token and device responses reject malformed JSON and invalid or
  overflowing expiry values. Refresh timing handles timestamp boundaries.
- OAuth exchanges, refreshes, device authorization, and device polling no longer
  follow redirects that could forward credential-bearing request bodies.
- Credential storage rejects malformed JSON, unsafe lock files, and non-regular
  files without hanging or changing unrelated permissions. Interrupted I/O is
  retried, and directory-sync failures are reported even after publication.
- Audited content-bearing debug types redact HTTP messages, tool arguments,
  assistant output, and other sensitive contents. Numeric and boolean secrets
  no longer become public trace metadata.
- Anthropic streams keep correct token totals across partial usage updates and
  ignore content after completion or error. OpenAI-compatible streams reject
  invalid tool-call indices rather than merging unrelated calls.
- HTTP status codes and JSON integer fields are parsed strictly without
  overflow. Negative retry caps no longer produce huge countdowns, and provider
  URLs reject malformed IP literals.
- Background-job cancellation can reenter safely, shutdown cannot admit late
  workers, and failed startup does not leave a running record or consume capacity.
- Parallel tools and foreground subagents preserve the admitted tool-execution
  cancellation checks instead of replacing them with provider cancellation.
- Tool and question executor exceptions become structured failures in both
  sequential and parallel execution rather than escaping dispatch.
- Event callbacks can safely add subscribers during delivery. New subscribers
  start with the next event without invalidating the callback currently running.
- ACP cancellation and saturated queues no longer deadlock shutdown. Invalid
  UTF-8 client-tool requests return errors before sending, and invalid tool
  location metadata is omitted without dropping the update.
- RPC input handles exception-enabled streams without losing final records or
  cancellation, and rejects invalid input descriptors instead of waiting forever.
- LSP rejects malformed or oversized headers and invalid response JSON, answers
  server requests with string IDs, and checks exact child ownership before
  signaling or declaring shutdown complete.
- MCP accepts records exactly at the configured payload limit and does not
  reject a valid record because later records arrive in the same read.
- Plugin manifests reject duplicate keys, embedded NUL bytes in launch arguments,
  and a contributions field that is not an object.
- Rejected overlapping plugin installations leave the source tree untouched.
  Plugin removal stays bound to the selected direct-child directory and cannot
  follow a substituted symlink outside it.
- Desktop permission dialogs show proposed edits and mark truncated previews.
  Invalid or oversized question replies keep the dialog open, with reply
  failures displayed inside the dialog.
- Desktop action and IPC failures are shown instead of silently dropped.
  Child-stream errors and updates during window destruction no longer crash
  the main process.
- Desktop startup times out after 30 seconds instead of waiting forever. Idle
  cancellation stays idle, and a failed catalog refresh cannot turn a completed
  prompt into a failure.
- Desktop shutdown waits for AVA to exit and escalates to termination and forced
  kill when needed. Overlapping restarts and quitting during restart cannot
  leave an extra backend running.
- Desktop workspace changes do not mutate the launch environment and recheck
  that AVA is idle after the folder picker closes.
- Packaged desktop builds include a content security policy. Only the pinned
  local development server may receive the desktop bridge, and navigation
  redirects are guarded.
- Desktop transcripts follow new output unless the reader has scrolled up.
  Tool summaries preserve Unicode and offer expansion only when truncated.
  Streaming avoids repeated partial-record copies and coalesces snapshot delivery.
- TSan CI installs its debug-build prerequisites. Third-party notices include
  the adopted color-difference implementation's attribution, and the desktop
  setup link points to the runnable prototype.
- Live model and coding smoke launchers fail early on setup errors and terminate
  their process groups on cancellation, including surviving descendants, before
  cleaning up private evidence.

### Supported platform

- The current Linux x64 package target requires **x86-64-v2**
  (Nehalem-class or newer). BMI2 and AVX are not required.
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

See the [release ledger](docs/product/release-readiness.md) for the current
release plan and the
[publication runbook](docs/operations/publication.md) for release policy.

## Historical release-position documentation

Historical capability and release-position details live in
[`docs/versions/`](docs/versions/README.md). They record repository history and
plans, not proof that a release was published.
