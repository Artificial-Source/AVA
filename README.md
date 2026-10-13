# AVA

[![CI](https://github.com/Artificial-Source/AVA/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/Artificial-Source/AVA/actions/workflows/ci.yml)

AVA is a native C++23, terminal-first, local-first coding agent with explicit backend authority for permissions, tools, providers, processes, and append-only sessions. The active default branch is `develop`.

The source reports runtime version `1.0.0`, but **AVA 1.0.0 is not a published release**. The dated audit verdict is **READY AFTER LISTED BLOCKERS**; see [product principles](docs/product/principles.md) and the single current [release-readiness ledger](docs/product/release-readiness.md).

**Choose a path:** [use AVA](docs/core/usage.md), [configure AVA](docs/core/configuration.md), [delegate work](docs/core/subagents.md), [understand model tools](docs/core/tools.md), [troubleshoot](docs/operations/troubleshooting.md), [contribute](CONTRIBUTING.md), or browse the [documentation index](docs/README.md).

## Clone and Build

Clone all submodules before configuring:

```sh
git clone --branch develop --single-branch --recurse-submodules https://github.com/Artificial-Source/AVA.git
cd AVA
```

If an existing clone was made without `--recurse-submodules`, recover it with:

```sh
git submodule update --init --checkout --recursive
```

`./autogen.sh` is an optional maintainer convenience. It initializes missing submodules at AVA's pinned commits, sets the repository's missing `push.recurseSubmodules` safety default, and prints CMake guidance; it does not update dependency branches, configure or build AVA, and is not needed after a recursive clone.

Build-only requirements:

- CMake 3.27+
- C++23 compiler
- Boost development headers and CMake package
- `ncursesw` development headers/library
- Python 3 for the complete test/documentation/package gates
- writable `GITACHE_ROOT` for the canonical debug-enabled presets
- JSON-capable Universal Ctags for debug/libcwd print-member generation
- Git and configuration-time access to required dependency sources

The optional Electron desktop client in `apps/desktop/` is independent of CMake and needs Node.js 24 plus pnpm; it drives a local Linux AVA binary over RPC. See `docs/desktop/README.md`. Linux x64 remains AVA's only qualified backend platform.

The presets are the canonical quick start. Prepare a writable Gitache root first:

```sh
export GITACHE_ROOT="${GITACHE_ROOT:-$HOME/.cache/ava/gitache}"
mkdir -p "$GITACHE_ROOT"
cmake --preset dev
scripts/build.sh
scripts/run-tests.sh
```

Canonical ASan/UBSan build:

```sh
cmake --preset sanitize
scripts/build.sh --build-dir build-sanitize --jobs 2
scripts/run-tests.sh --build-dir build-sanitize --jobs 2
```

Direct CMake configuration is a noncanonical fallback unless it supplies the cache-equivalent `BetaTest`, `EnableDebug=ON`, test, compile-command, and sanitizer values documented in the [development guide](docs/development/contributing.md).

`scripts/build.sh` and `scripts/run-tests.sh` default to the `build` tree and all available logical cores. Pass `--jobs N` (or set `CMAKE_BUILD_PARALLEL_LEVEL` / `CTEST_PARALLEL_LEVEL`) to cap concurrency; build options such as `--target` and CTest filters such as `-R` are forwarded. Both runners share a build-tree lock so builds and tests cannot modify one tree concurrently. Sanitizer examples use two jobs to limit memory pressure.

GitHub Actions runs both the normal and sanitizer test jobs on pushes and pull requests targeting `develop`. Dependabot is enabled for GitHub Actions updates on `develop`.

**For detailed cmake configuration options and build instructions see [CONTRIBUTING](docs/development/contributing.md).**

### Linux host artifact

Create the AVA-only host archive and checksum outside the checkout with:

```sh
scripts/package-linux.sh --output-dir /absolute/path/outside/AVA
```

The script builds Release `ava` plus the fake-provider smoke helper, stages only CMake component `ava`, extracts the archive fresh, verifies its checksum and CLI behavior, and runs the deterministic model smoke. `--binary /absolute/path/to/ava` accepts an existing binary; add `--fake-provider /absolute/path/to/ava_fake_provider_server` to run the model smoke in that mode. Without a fake helper the script prints an explicit skip.

This is a dynamically linked **host** artifact, not a portable Linux bundle. The 2026-08-23 audited x64 candidate requires BMI2, `GLIBC_2.38`, `GLIBCXX_3.4.32`, `CXXABI_1.3.13`, `libncursesw.so.6`, `libtinfo.so.6`, a usable terminfo database, and `curl` on `PATH`. The first-publication target is Linux x64 only; another architecture may publish only after native exact-candidate evidence.

Tested on Ubuntu 24.04.4 x64: GCC 13 BetaTest with Unix Makefiles and GCC 13 Release with Ninja. Clang 18 was environment-blocked; MSVC, Windows, macOS, and AArch64 were not qualified. Multi-config builds are not release-qualified. A dirty tree is never a qualified artifact, and these source results do not qualify future release bytes.

`--require-release-qualified` and `PROVENANCE.json` field `release_qualified:true` prove only the implemented static source/gitlink/license/native-architecture/dynamic-dependency/package gates. Complete candidate qualification requires the [release ledger](docs/product/release-readiness.md) and [publication runbook](docs/operations/publication.md).

## Run

```sh
./build/ava
./build/ava connect openai
./build/ava --mode plan
# After configuring reviewer with mode: primary or mode: all:
./build/ava --agent reviewer "review this change"
./build/ava --continue
./build/ava --resume
./build/ava --session <id-or-prefix>
./build/ava --session-id <id-or-prefix>
./build/ava "summarize this repo"
./build/ava @README.md "summarize this file"
./build/ava --print "summarize this repo"
./build/ava --thinking high "reason about this change"
./build/ava --tools read_file,grep "inspect safely"
./build/ava --tools read,grep,find,ls "inspect with Pi-style names"
./build/ava --rpc
./build/ava --acp
```

Bare prompt text and `--print` both run one prompt and exit. `--agent <name>` selects a configured primary agent for that invocation; see [subagent configuration](docs/core/configuration.md#subagents). Multiple positional prompt tokens are joined with spaces, so `ava summarize this repo` is equivalent to `ava "summarize this repo"`. CLI `@path` text file arguments are expanded through the same bounded permissioned file-reference path used by the TUI, including quoted shell arguments for paths with spaces. Add `--json`, `--output json`, or Pi-compatible `--mode json` to emit runtime events instead of final text only. `--thinking off|<level>` is a Pi-compatible startup alias for AVA's reasoning control; non-`off` values must be declared by the active model metadata, such as `low`, `medium`, `high`, or `xhigh` on the default GPT-5.5 profile. `--tools`, `--exclude-tools`, `--no-builtin-tools`, and `--no-tools` control model-visible tools for TUI, print, and RPC sessions; AVA accepts native names plus Pi aliases such as `read`, `write`, `edit`, `find`, and `ls`, but still advertises one native provider schema per operation. These visibility flags are separate from permission policy. `--allow read-only` auto-approves AVA operations classified as read-only; `--allow-tool <list>` auto-approves the named supported tool families and does not make hidden tools model-visible. `--rpc`, `--output rpc`, or Pi-compatible `--mode rpc` starts AVA's proprietary JSONL stdio RPC v1 endpoint for automation and custom clients; RPC prompt requests can import local image paths through `attachments:["path.png"]` or Pi-style inline uploads through `images:[{"type":"image","data":"...","mimeType":"image/png"}]` for image-capable models. See [`docs/rpc-protocol.md`](docs/rpc-protocol.md) for the normative request, response, event, resolver, and lifecycle contract. `--acp` starts the separate stable ACP v1 JSON-RPC 2.0 stdio endpoint for specifically configured editor/IDE clients; see [`docs/acp.md`](docs/acp.md) for Zed/JetBrains/CodeCompanion setup and exact evidence labels, [`docs/acp-support.json`](docs/acp-support.json) for the machine-checked profile, and the [`docs/interop/evidence`](docs/interop/evidence/README.md) policy. Configuration does not imply execution evidence. ACP does not replace AVA's proprietary JSONL RPC or migrate AVA sessions; AVA RPC is also distinct from generic JSON-RPC 2.0 and Pi RPC reference behavior.

When stdin/stdout are not a terminal and no headless mode is selected, AVA falls back to a line-oriented shell for scripting:

```sh
printf '/glob **/*.cpp\n/quit\n' | ./build/ava --continue
```

## Configuration

AVA follows XDG paths on Linux:

- Config: `$XDG_CONFIG_HOME/ava/`, fallback `~/.config/ava/`
- Auth: `$XDG_CONFIG_HOME/ava/auth.json`, fallback `~/.config/ava/auth.json`
- Sessions: `$XDG_STATE_HOME/ava/sessions/`, fallback `~/.local/state/ava/sessions/`
- Project trust: `$XDG_STATE_HOME/ava/project-trust.json`, fallback `~/.local/state/ava/project-trust.json`

Use `ava connect` to choose a provider and supported login method. Provider credentials can also be configured in `auth.json`; see [configuration and authentication](docs/core/configuration.md#auth) for formats and secret-handling rules. Use `/providers` to inspect provider availability and credential status without revealing secrets, and `/models` to inspect or select configured models. See [provider and model status](docs/core/providers.md) for the current concise matrix rather than treating this README as a provider catalog. To add a custom OpenAI-compatible, OpenAI Responses, or Anthropic Messages endpoint, see the [custom providers guide](docs/core/custom-providers.md) (`providers.json` + matching `models.json` entries; restart required).

The default model is `openai/gpt-5.5`. Override models with `$XDG_CONFIG_HOME/ava/models.json`, prompts with `$XDG_CONFIG_HOME/ava/prompts/<provider>/<family>/<mode>.txt`, replace the selected system prompt with `SYSTEM.md` or `--system-prompt`, or append with `APPEND_SYSTEM.md` or repeated `--append-system-prompt` flags. Global prompt resources live under `$XDG_CONFIG_HOME/ava`; project prompt resources live under `$WORKSPACE/.ava` and require `/trust project`. CLI prompt flags win over prompt resource files for the current process.

## Common interactive commands

This is a curated set of common commands, not an exhaustive catalog. See [the current command reference](docs/core/usage.md#commands) for the complete list. In the TUI, local slash and `!`/`!!` command invocations, output, errors, and tool cards stay out of chat/provider content: rich output uses a scrollable command-output panel and short confirmations use footer status. A dynamic prompt/skill/MCP prompt command may still create a genuine provider conversation turn, but its literal slash invocation remains local.

- `/help`: show commands and hotkeys
- `/hotkeys` or `/keybindings`: show effective TUI hotkeys with human primary labels
- `/keybindings init [--force]`: create or explicitly replace a validated keybindings starter file
- `/keybindings import <path> [--force]`: validate and install a keybindings JSON file
- `/keybindings set <action> <key>[,<key>...]`: validate and edit one keybinding action
- `/keybindings reset <action>`: remove one override so the action inherits built-in defaults
- `/keybindings validate`: validate `keybinds.json` without changing active bindings
- `/theme [dark|light|plain|custom-name|reset]`: show or persist the TUI display theme
- `/reload [all|theme|models|prompts|trust|compaction|keybindings|auth|permissions|lsp|mcp|plugins]`: reload supported runtime config domains in the running TUI and report restart-required domains
- `/mode`: toggle build/plan mode
- `/details [compact|rich|expanded]` or Ctrl+O: select tool-card presentation; Rich is the default and bare `/details`/Ctrl+O toggles Rich and Expanded
- Ctrl+G: open the current composer draft in `$VISUAL` or `$EDITOR`
- Ctrl+V: import a PNG/JPEG/WebP/GIF image from the clipboard as a pending attachment when a supported clipboard helper is available
- `/tool [query]`: toggle Expanded presentation for the latest or matching tool card in the TUI; `/tools` is an alias
- `/diff [query]`: show the latest or matching unified tool diff in the TUI
- `/copy [user|tool|diff|permission] [query]`: copy the latest AVA message, a selected public user turn, safe latest or matching tool-card details, latest or matching unified diff, or explicit permission audit details in the TUI
- `/search [query]`: open the TUI transcript finder over currently rendered message and tool-card items
- `/thinking`: toggle inline thinking block visibility without changing provider reasoning mode; `/thinking details` expands or collapses the latest completed long thinking block
- `/attach <path>`: import a local PNG/JPEG/WebP/GIF image into session-owned attachment storage and send it with the next normal TUI prompt; `/image` is an alias

The TUI theme precedence is `NO_COLOR`, then `AVA_TUI_THEME`, then `display.json` including custom themes under `$XDG_CONFIG_HOME/ava/themes/*.json`, then startup OSC 11 on direct terminals (skipped under tmux), then `COLORFGBG`, then the built-in dark fallback. Built-in light/dark keep the ordinary canvas at the terminal-default background; `/settings` can report source `OSC 11`.
- `/connect`: open provider and login method modals; `/login` is an alias
- `/models [query|provider/model]`: list configured models and capabilities; `/model` is an alias, Ctrl+L opens the TUI model selector, and Ctrl+P cycles to the next configured or scoped enabled model between turns
- `/scoped-models`: open the TUI scoped model-cycle selector to enable, disable, and order the Ctrl+P cycle; Ctrl+S persists the saved cycle in `models.json`
- `/sessions [--archived] [query|id]`, `/sessions rename <id> <name|--clear>`, `/sessions labels <id> <label...|--clear>`, `/sessions archive <id> --confirm`, or `/sessions unarchive <id>`: show the resumable session tree, rename/label sessions, or hide/restore sessions without deleting their JSONL files; `/tree` is an alias for the tree view
- `/fork [name]`: fork the current session at its latest entry and switch to the branch
- `/fork-from [query]`: fork from a selected public user turn in the TUI and switch to the branch (sessionless sessions refuse; `/copy user` still works)
- `/clone [name]`: clone the full current session and switch to the copy
- `/new [name]` (alias `/clear [name]`): start a fresh session and switch to it
- `/resume [id]`: resume/switch to an existing session by exact id or unique prefix; exact `/resume` opens the TUI session selector, where PageUp/PageDown page through rows, Ctrl+S or Ctrl+T cycles recent/name/path sort, Ctrl+N toggles named sessions only, Ctrl+P toggles path display, Ctrl+A shows/hides archived sessions, Ctrl+R restores a rename command, Ctrl+L or Shift+L restores a labels command, Shift+T toggles label update timestamps, and Ctrl+D twice or Ctrl+Backspace twice archives or restores the highlighted session when the selector search is empty
- `/name <name|--clear>`: set or clear the current session display name; `/rename` is an alias
- `/labels <label...|--clear>`: set or clear current session labels; `/label` is an alias
- `/context [query|source]`: list base prompt metadata, system-prompt resources, context files, prompt commands, skills, subagents, plugins, LSP config diagnostics, and freshness status
- `/trust [status|project|deny|clear]`: inspect or change this workspace's project-resource trust decision for commands, skills, subagents, plugins, MCP/LSP config, and system prompt resources
- `/compact [instructions]`: generate and record a provider summary
- `/export [markdown|html|jsonl] [path]`: export this session as Markdown, safe self-contained HTML, or sanitized portable AVA JSONL; `/export <file.html>` writes Pi-style HTML and `/export <file.jsonl>` writes re-importable JSONL through the permissioned file path, omitting provider-private reasoning replay metadata
- `/import <path.jsonl> --confirm`: validate an AVA JSONL session archive, create a new local session, and switch to it; without `--confirm`, AVA only previews the entry count
- `/stats`: show session counts, usage, cost, and resume/export hints; `/status` is an alias
- `/permissions <list|audit|diagnose|explain|add|remove> ...`: inspect session permission audits and manage persistent permission rules; list/receipt/explain lead with human summaries while exact rule ids remain authority; `/permission-rules` and `/perms` are aliases
- `/jobs [show|wait|result|cancel|promote] ...`: exact `/jobs` opens the TUI's searchable, latest-first owner-bound job selector during idle or an active parent run. Enter opens a full-width read-only child workspace containing only committed child User/Assistant messages; Esc returns, Tab/Shift+Tab cycle jobs, C cancels, and P promotes. Titles, status/mode/tool metadata, and duplicate-safe short references stay display-only; session/path/full-ID metadata remains hidden and exact owner-bound job IDs remain control authority. In non-TUI modes bare `/jobs` retains its human-readable list, valid subcommands remain `show`/`wait`/`result`/`cancel`/`promote`, and public model/RPC JSON is unchanged (see [`docs/rpc-protocol.md`](docs/rpc-protocol.md)).
- `/read <path>`: read a file through permissions
- `/write <path> <text>`: write a file through permissions using atomic replacement where practical
- `/glob <pattern>`: list readable matching files
- `/grep <text> [glob]`: literal text search
- `/bash <command>`: run an argv-style permissioned command
- `!<command>` or `!!<command>`: run the same permissioned shell helper directly from the composer; output stays visible/audited and is not injected into provider context unless you paste it into a later prompt
- `/plugins list|inspect|enable|disable|validate|failures|prompts|prompt|skills|skill`: inspect and manage local plugins
- `/plugin run <id> <command> [arguments_json]`: run a plugin command through the permissioned extension path
- `/mcp list|inspect|tools|restart`: inspect configured MCP servers and discover tools
- `/quit`: exit and print a resume command

## Documentation

The [documentation index](docs/README.md) is organized by audience and task.

### Users and operators

- [Usage](docs/core/usage.md), [configuration](docs/core/configuration.md), [custom providers](docs/core/custom-providers.md), [environment variables](docs/core/environment-variables.md), and [provider status](docs/core/providers.md)
- [Subagents and background jobs](docs/core/subagents.md), [built-in model tools](docs/core/tools.md), [LSP](docs/extensions/lsp.md), [terminal setup](docs/operations/terminal-setup.md), and [troubleshooting](docs/operations/troubleshooting.md)
- [Diagnostics and support exports](docs/operations/diagnostics.md), [security/sandboxing](docs/security/sandboxing.md), and [support](SUPPORT.md)

### Automation and extension authors

- [Proprietary AVA RPC v1](docs/rpc-protocol.md), [ACP](docs/acp.md), its [evidence policy](docs/interop/evidence/README.md), and [shared headless behavior](docs/headless-protocol.md)
- [Plugins](docs/extensions/plugin-system.md), [MCP](docs/extensions/mcp.md), and [session format](docs/session-format.md)

### Contributors and maintainers

- [Product principles](docs/product/principles.md), [release readiness](docs/product/release-readiness.md), [Contributing](CONTRIBUTING.md), [development guide](docs/development/contributing.md), [build configuration](docs/operations/build-configuration.md), [testing](docs/operations/testing.md), [architecture](docs/development/architecture.md), [codebase guide](docs/development/codebase-guide.md), and [documentation policy](docs/development/documentation-policy.md)
- [Release checklist](docs/operations/release-checklist.md), [publication runbook design](docs/operations/publication.md), [Code of Conduct](CODE_OF_CONDUCT.md), [Security](SECURITY.md), [Governance](GOVERNANCE.md), and [Changelog](CHANGELOG.md)

Product status and future work live in the [product](docs/product/README.md), [plans](docs/plans/README.md), [roadmap](docs/roadmap/README.md), and [goals](docs/goals/README.md) indexes; historical release-position ledgers live under [versions](docs/versions/README.md). Plans and runtime version numbers are not evidence that a tag, artifact, package, or external release has been published.
