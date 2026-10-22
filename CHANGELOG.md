# Changelog

All notable changes to AVA are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and version identifiers here do not imply that a tag, artifact, package, or external release has been published.

## [Unreleased]

### Added

- Community documentation and GitHub contribution templates.
- Configurable primary agent definitions and invocation-local `--agent` selection; see [subagent configuration](docs/core/configuration.md#subagents).
- Completed TUI parity documentation for Mermaid fence projection, transcript selection/navigation/copy affordances, cursor control and bounded Kitty/`modifyOtherKeys` negotiation, prompt stash, and the 23-scenario isolated tmux suite. These surfaces retain explicit accessibility, renderer, probing, and session-tree non-goals.
- Current architecture, codebase, build-configuration, environment-variable, LSP, built-in-tool, subagent/background-job, troubleshooting, and documentation-policy references.
- Offline source Markdown relative-target validation with focused CTest coverage.
- Added the additive `ava.plugin.v1` host-rendered plugin UI contract: exact `ui.status`, `ui.widget`, `ui.select`, and `ui.confirm` manifest capabilities and bounded JSONL records/actions; high-risk `plugin.ui.present` preflight before child launch; exact direct foreground interactive-TUI authority only; complete nontruncated plugin/command attribution with fail-closed display-cell fitting; host confirmation defaulting to Cancel; a 120-second absolute deadline; finite external-disable revocation; and deterministic child/presenter cleanup. UI authority remains unavailable to RPC, ACP, print, headless/non-TTY, model/tool, hook, background, queued, synthetic, and plugin-to-plugin paths. UI text is strict UTF-8 with control/C1/bidi rejection and is ephemeral—excluded from sessions, responses/events, exports, provider context, and diagnostics. Arbitrary markup/native renderers, browser/auth/file/secret/form access, and marketplace delivery remain deferred; coverage is deterministic plus credential-free tmux rather than an untested real-terminal claim.

### Changed

- Removed the plain-text `--line-shell` frontend and automatic non-TTY interactive fallback. Interactive AVA now requires a terminal on both stdin and stdout; use `--print` or `--rpc` for non-interactive runs.
- Removed the local first-run setup wizard, its `/setup` command, onboarding marker state, and wizard-only credential readiness probe. Fresh TUI startup now opens the normal composer directly; ordinary disconnected `/connect` guidance remains.
- Reorganized first-party documentation into explicit core, interfaces, extensions, operations, development, security, product, plans, roadmap, and history categories while preserving fixed normative contract and evidence paths.
- Added one task-ordered human documentation spine, concise category indexes, nested documentation-maintenance instructions, and a root `llms.txt` map for automated readers.
- Refreshed build, testing, Docker, release, support, and Linux host-artifact documentation; packaged end-user references now include context resources, providers, security, subagents, terminal setup, themes/keybindings, thinking modes, environment, LSP, tools, and troubleshooting guides.
- Model file tools now refuse escaping and absolute symlinks beneath the workspace root in every mode; permitted outside-workspace access must name the real path.
- `glob` and `grep` results include a nonzero `skipped_paths` field for paths they could not inspect, independently of result truncation.
- Narrowed executable/interpreter parent grants to exact-file read/execute when a parent covers trusted home or overlaps AVA authority; startup PATH discovery omits `/` and trusted-home ancestors/aliases (PROC-3, SEC-6).
- Added signal and abstract UNIX socket scoping for contained commands on Landlock ABI 6 and newer; older supported ABIs retain their previous scope (PROC-7).
- Aggregated private trace stream deltas into per-segment `delta_count`/byte summaries, capped at 1,000 of 10,000 records to reserve lifecycle capacity; summary refusals are separate from event loss (CORE-2).
- Bounded managed trace retention to 32 files / 100 MiB with a 10 MiB active-trace reservation, and support exports to 32 files / 1 MiB; unlocked orphan temporaries older than 24 hours are swept while unsafe/shared siblings are left untouched (CORE-3).
- Plugin installation now requires atomic `renameat2(RENAME_NOREPLACE)` publication. Unsupported kernels/filesystems (`EINVAL` or `ENOSYS`) produce an actionable error instead of falling back to plain rename.

### Fixed

- Prevented Gemini API keys from being forwarded to redirect targets by disabling HTTP redirects (PROV-1).
- Kept generated Gemini tool-call IDs unique across streamed response records (PROV-3).
- Restored session torn-tail recovery for unprivileged users on older kernels with a `/proc/self/fd` descriptor-link fallback (SESS-1).
- Synced session appends before reporting them committed and synced new session files and parent directories with `fdatasync`/`fsync` (SESS-2).
- Prevented contained network-allowed commands from reaching host AF_UNIX services such as D-Bus and `docker.sock` (PROC-1).
- Restored private socket pairs needed by Python asyncio and Node child-process pipes under network-denied containment (PROC-2).
- Requested explicit one-shot approval when a toolchain's real executable or interpreter lies outside containment roots, instead of failing after launch (PROC-6).
- Compacted multi-line plugin arguments into single JSONL records without changing string contents (EXT-1).
- Closed plugin-install directory-swap races with descriptor-relative copying and cleanup, using atomic no-replace publication (EXT-3).
- Bounded configured LSP providers to eight live clients, evicting idle clients in least-recently-used order and returning a retryable capacity error when all are busy (EXT-7).
- Applied descriptor-anchored workspace safety to model file tools in every mode, preventing symlink redirection outside the workspace (TOOL-1).
- Prevented secure file writes from inheriting a replaced symlink's `0777` mode and prevented file reads from hanging on FIFOs (TOOL-7/8).
- Kept searches running past unreadable folders and kept `grep` running past files over 10 MiB, reporting omitted paths instead of aborting the search (TOOL-13/14).
- Prevented executable/interpreter parent-directory rules from exposing trusted home or overlapping AVA authority, including trailing-slash and device/inode aliases (PROC-3).
- Prevented contained commands from signaling processes or connecting to abstract UNIX sockets outside their Landlock domain on ABI 6 and newer (PROC-7).
- Downgraded containment to Critical, one-shot Ask on every failed `SECCOMP_GET_ACTION_AVAIL` probe instead of assuming support through `PR_GET_SECCOMP` (PROC-12).
- Dropped startup PATH entries that are `/`, equal trusted home, or contain it, including trailing-slash and symlink aliases (SEC-6).
- Made model-facing job waits observe parent cancellation in approximately 50 ms slices without stopping the background job (AGENT-6).
- Reported provider-native subagent cancellation as canceled and provider-error outcomes as failed with a descriptive provider error (AGENT-7).
- Surfaced plugin model-tool name collisions through plugin diagnostics and `/plugins failures`, with bounded plugin/MCP collision logging in debug builds, while keeping the later contribution disabled (EXT-10).
- Preserved private trace lifecycle capacity under delta-heavy streams and counted intentional summary-budget refusals as `writer_health.delta_summaries_refused`, not `events_dropped` (CORE-2).
- Pruned old private trace/support artifacts under count/byte limits without deleting active files or unsafe/shared siblings (CORE-3).
- Retried short and interrupted trace writes, attempted rollback of partial records on failure, and closed failed writers before further records could extend a broken line (CORE-9).
- Kept MCP/plugin failure attribution working beyond 20,000 completed calls and refused alias overflow rather than merging unrelated identities (CORE-11).
- Recovered malformed trace counters through a bounded `trace-counters-v1.corrupt` quarantine and counted consecutive identical last failures in `occurrences` (CORE-12).
- Contained allocation/parsing failures in native reasoning validation and drained diagnostic writers before best-effort shutdown bookkeeping (CORE-16).
- Classified loss-obscured missing trace boundaries as incomplete coverage rather than lifecycle errors, with saturating loss accounting (CORE-17).
- Routed Gemini thoughts to reasoning and privately replayed `thoughtSignature` on matching follow-up parts; replay metadata over 64 KiB or 1,024 parts is dropped without failing the response (PROV-4).
- Read image attachments through one no-follow, non-blocking descriptor snapshot so FIFO/symlink swaps are rejected instead of hanging or redirecting reads (SESS-8).
- Refused permission-rule additions that would exceed the 1 MiB file cap while preserving the existing rules file (PERM-7).
- Kept each active-run joiner bound to its own outcome after historical-cache eviction and preferred the active generation when a request id is reused (RUN-8).

## Historical release-position documentation

Historical capability and release-position detail is maintained in [`docs/versions/`](docs/versions/README.md). These ledgers document repository history and planning context; consult them rather than treating this changelog as evidence of a published release.
