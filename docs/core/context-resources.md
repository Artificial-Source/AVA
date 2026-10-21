# AVA Context And Resource Guide

This guide maps which local files affect AVA's prompt, commands, tools, and
execution resources. It is intentionally task-oriented; use
[`CONFIG.md`](configuration.md) for exact schemas, path fallbacks, and validation
details.

## Mental Model

- **Prompt context** is text assembled into the provider system prompt for the
  current session: the selected base prompt, `SYSTEM.md`/`APPEND_SYSTEM.md`,
  `AGENTS.md`/`CLAUDE.md` files, and summaries of available skills/subagents.
- **Resources** are discoverable files or processes that are exposed as slash
  commands, tools, LSP/MCP integrations, or plugin contributions. Most are not
  injected into the prompt until a user or model explicitly invokes them.
- **Project resources** are workspace-owned resource files such as `.ava/*` and
  project-local plugin/MCP/LSP config. They are skipped until `/trust project`,
  except plain `AGENTS.md`/`CLAUDE.md` instruction files.
- **Execution is still permissioned.** Trusting a project makes project resource
  definitions discoverable; it does not auto-approve shell, plugin, MCP, LSP,
  file, or task operations.

## What Enters The System Prompt

AVA builds the prompt in this order:

1. Built-in prompt or provider/family/mode prompt override from `prompts/...`.
2. `SYSTEM.md`, or `--system-prompt`, replaces that selected base text.
3. `APPEND_SYSTEM.md`, or repeated `--append-system-prompt`, appends extra text.
4. A `--agent` selected primary definition's body is appended as an explicit instruction section.
5. Loaded `AGENTS.md`/`CLAUDE.md` instruction files are appended.
6. Enabled static plugin prompt resources are appended as plugin context sources.
7. Available skills, including enabled static plugin skill resources, and visible
   subagents are summarized so the model can decide whether to call `skill` or
   `task`.

Prompt commands, MCP prompts, plugin commands, dynamic plugin resources, and LSP
config are exposed through their command/tool surfaces; they are not silently
appended as full prompt text.

## `AGENTS.md` / `CLAUDE.md` Instruction Files

- Workspace discovery walks from the workspace root to the current directory.
- In each directory, AVA loads the first file by priority:
  `AGENTS.md`, `AGENTS.MD`, `CLAUDE.md`, then `CLAUDE.MD`.
- The global instruction file defaults to `$XDG_CONFIG_HOME/ava/AGENTS.md` and
  uses the same sibling fallback names when `AGENTS.md` is absent.
- These files must be regular and byte-bounded, and load without project trust
  because they are treated as visible user-authored instructions.
- Reads are anchored beneath the selected context root. On Linux's `openat2`
  path, symlinks that remain within that root may be followed; escapes fail.
  When `openat2` is unavailable or unsupported, the stricter component-walk
  fallback rejects symlink components. The selected root's own path may contain
  symlinks; AVA follows them when opening that trusted anchor.

Use `/context` to see which instruction files were loaded and whether their
contents have changed since the current prompt state was assembled.

## `SYSTEM.md` And `APPEND_SYSTEM.md`

- Global files live in `$XDG_CONFIG_HOME/ava/`.
- Project files live in `$WORKSPACE/.ava/` and require `/trust project`.
- A trusted project file wins over a global file with the same name; an
  untrusted project file is skipped.
- `SYSTEM.md` replaces the selected base prompt. `APPEND_SYSTEM.md` appends to
  it. CLI `--system-prompt` and `--append-system-prompt` are process-local and
  override those discovered files.

After editing these files in a running session, use `/reload prompts` or start a
fresh session before relying on the new text.

## Prompt Commands

Prompt commands are Markdown templates that expand slash commands into a user
prompt message.

- Global directories: `$XDG_CONFIG_HOME/ava/commands/` and `command/`.
- Project directories: `$WORKSPACE/.ava/commands/` and `command/`, loaded only
  after project trust.
- The Markdown path becomes the slash command name, so `review.md` becomes
  `/review`; nested files can create nested command names.
- Frontmatter can provide `description`, `argument-hint`/`argument_hint`/`hint`,
  or `template`. Without `template`, the body is the template.
- Template arguments include `$1`, `$2`, `$@`, `$ARGUMENTS`, `${1:-default}`,
  `${@:N}`, `${@:N:L}`, and `$$`.

Built-in commands are not overridden by prompt commands. Among dynamic prompt
commands, project commands are considered before global commands once project
resources are trusted.

## Skills

Skills are reusable instruction bundles. AVA summarizes available skills in the
system prompt, then loads the full skill content only when the model calls the
`skill` tool or the user invokes `/skill:<name>` or `/<name>` for a skill.

- Global skill roots include `$XDG_CONFIG_HOME/ava/skills/`, `~/.agents/skills/`,
  and `~/.claude/skills/`.
- Project skill roots include `.ava/skills/`, `.agents/skills/`, and
  `.claude/skills/`, gated by project trust.
- Each skill is a `SKILL.md` file, either directly in the root or in a child
  directory. Frontmatter supplies `name` and `description`; the body is the
  loadable instruction content.
- Loading a skill requires `skill` permission, and `/context` tracks skill file
  freshness.

For an ordinary skill, for example `.ava/skills/review-change/SKILL.md`:

```markdown
---
name: review-change
description: Review a focused code change for correctness.
---
Read the changed code and report concrete defects with file references.
```

The `name` defaults to the containing directory name when omitted. It must be
1–64 bytes of lowercase letters, digits, and hyphens, with no leading, trailing,
or doubled hyphens. `description` is required, nonempty after trimming, at most
1024 bytes, and free of control bytes. Each ordinary `SKILL.md` is capped at
64 KiB, including frontmatter.

Global roots are processed in the order listed above, then trusted project roots
in their listed order. A later same-name definition replaces an earlier one, so
trusted project skills override global skills. Plugin-declared skill names use
a separate grammar (up to 96 bytes of letters, digits, `.`, `_`, or `-`); do not
use that grammar for ordinary `SKILL.md` frontmatter.

## Subagents

AVA always provides built-in `general` and read-only `explore` subagents. Custom
subagents are Markdown files with frontmatter and a body prompt. See
[subagents.md](subagents.md) for execution modes, permissions, job controls,
delivery, durability, and limits.

- Global roots include `$XDG_CONFIG_HOME/ava/agents/`, `agent/`, `~/.agents/*`,
  and `~/.claude/*` agent directories.
- Project roots include `.ava/agents/`, `.ava/agent/`, `.agents/*`, and
  `.claude/*`, gated by project trust.
- The schema consists of frontmatter `name`, required `description`, `mode`,
  `tools`, and `hidden`, plus the Markdown body. `mode: subagent` is task-only, `mode: primary`
  is `--agent`-only, and `mode: all` is both.
- Visible task subagents are summarized in the prompt. A task-only body is used
  when `task` dispatches it. A selected primary body is appended to both the
  ordinary and ambient-extension-free system-prompt variants without replacing
  AVA's base, safety, or context material.
- Global definitions load regardless of trust. Trusted project definitions
  override same-name global definitions; untrusted project definitions are not
  selectable.
- `tools: read-only`/`explore` narrows tool visibility and does not grant tool
  permission. Primary narrowing intersects CLI visibility; primary inherit does
  not automatically remove `task`, `job`, or `todowrite`.

## Plugins

Plugins are local out-of-process executables described by `plugin.json`.

- Global plugins live under `$XDG_CONFIG_HOME/ava/plugins/<plugin-id>/`.
- Project plugins live under `.ava/plugins/<plugin-id>/` and require project
  trust before discovery.
- `/plugins install <path>` imports a local plugin directory into the global
  plugin directory; it validates and copies local files only, never starts the
  entrypoint, and leaves the plugin disabled until `/plugins enable`.
- Discovered executable plugins are disabled by default. Enablement is local
  state under `$XDG_STATE_HOME/ava/plugin-enablement.json`.
- Static plugin prompts and skills are read through `/plugins prompt` and
  `/plugins skill`; plugin commands run through `/plugin run` or contributed
  slash commands.
- Enabled static plugin prompts are also appended to runtime context, and
  enabled static plugin skills are listed in `available_skills`. This autoload
  only reads manifest-declared files, never launches plugin entrypoints or the
  dynamic resource protocol, and project plugin resources still require project
  trust.
- Launch, tool, command, and event-hook paths are permissioned and run in fresh
  plugin processes today.

See [`plugin-system.md`](../extensions/plugin-system.md) and
[`plugin-compatibility-policy.md`](../plugin-compatibility-policy.md) for authoring
and compatibility details.

## MCP

MCP support is an explicitly configured, bounded stdio surface.

- Global config: `$XDG_CONFIG_HOME/ava/mcp.json`.
- Project config: `$WORKSPACE/.ava/mcp.json`, loaded only after project trust.
- MCP tools become model-visible tools with AVA permission/audit gates.
- MCP resources require explicit `mcp.resource.read` approval before content
  enters model context.
- MCP prompts are exposed as `/mcp:<server_id>:<prompt_name>` commands and RPC
  commands, not as automatic prompt text.

Use `/mcp list`, `/mcp inspect <server_id>`, and [`mcp.md`](../extensions/mcp.md) for the
supported surface and safety rules.

## LSP

LSP servers are optional and only affect LSP-backed tools.

- Global config: `$XDG_CONFIG_HOME/ava/lsp.json`.
- Project config: `$WORKSPACE/.ava/lsp.json`, loaded only after project trust.
- Tools include `lsp_diagnostics`, `lsp_document_symbols`,
  `lsp_workspace_symbols`, `lsp_definition`, and `lsp_references`.
- Querying requires `lsp.query`; starting a server requires
  `lsp.server.launch`.
- The sole built-in recipe is installed-only `clangd`, disabled by default and
  enabled only by exact `builtin_servers` opt-in in the owner-controlled global
  file. Project config cannot enable it. Discovery is passive and excludes
  workspace-local or hardlinked executables, downloads, package managers, and
  network access. This installed-only `clangd` integration is the sole
  automatic LSP recipe; every other server requires explicit configuration
  behind existing trust and launch policy.
- `/context lsp` reports fixed built-in status without launching a process or
  exposing discovered paths, argv, config contents, fingerprints, or raw
  discovery failures.

See the LSP section of [`CONFIG.md`](configuration.md#lsp-servers) for schema and
global-vs-project launch restrictions.

## Project Trust

Project trust decisions are stored outside the workspace in AVA state. Use:

```text
/trust status
/trust project
/trust deny
/trust clear
```

Trust gates project prompt commands, skills, subagents, plugins, MCP config, LSP
config, and project `SYSTEM.md`/`APPEND_SYSTEM.md`. It does not gate plain
workspace `AGENTS.md`/`CLAUDE.md` instruction files.

An effective denial or unknown decision retires all runtime controllers and
retained delivery capsules for that workspace known to the current application
manager, then publishes a fresh fail-closed controller for the visible session.
A stale session copy must be reopened. This is a **process-local,
manager-scoped** transaction: it is not a singleton, distributed lock, or
cross-process revocation protocol. Another AVA process observes the persisted
trust decision only at its own open/reload boundary.

## Reload And Freshness

- `/context [query]` reports the prompt mode/model, project trust state, loaded
  context files, and freshness for prompt resources, prompt commands, skills,
  discovered plugin manifests, and enabled static plugin resources. Status can
  show `current`, `changed`, `missing`, or `unreadable`.
- `/context` is diagnostic only; it does not update the current system prompt.
- `/reload prompts` rebuilds the current prompt state from prompt files,
  instruction files, skills, and task subagents, and refreshes freshness metadata for
  prompt commands, skills, and plugin resources. It reuses the already resolved
  selected-primary definition rather than changing that agent's identity or policy.
- `/reload trust` reloads project trust and rebuilds prompt state with the new
  project-resource decision in the same transaction. An effective untrusted
  reload retires retained local authority rather than refreshing an old parent
  capsule.
- `/reload mcp`, `/reload lsp`, and `/reload plugins` report restart-required
  domains; running server/plugin process state is not hot-reloaded.
- Theme/display and keybinding behavior has TUI-specific live reload support;
  see [`CONFIG.md`](configuration.md) for those domains.

When in doubt: run `/context`, check `/trust status`, then use the narrowest
`/reload ...` target that matches the edited resource.
