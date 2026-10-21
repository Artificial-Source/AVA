# Security And Sandboxing Guide

AVA is a local coding agent. It can read files, edit files, run selected local
commands, contact model providers, load local resources, and launch extension
processes when you allow those features. AVA's safety model is designed to make
those actions explicit, bounded, auditable, and fail-closed where practical.

**AVA has narrow Linux Landlock+seccomp containment for verified sealed local plans, not a general AVA sandbox.** It applies to Standard path-bearing or mutable commands, Sensitive commands after approval, and some Critical/Raw commands when a verified local plan and mutable-code policy require it. It applies filesystem rules, no-new-privileges, and a seccomp filter in both network modes; network approval permits IP and route-netlink sockets, not AF_UNIX service sockets. It does not sandbox whole AVA, plugins, MCP, LSP, containers, or VMs. Descendants that call `setsid` still inherit Landlock and seccomp, but escape AVA's PGID cleanup and its resource/lifetime control. Run AVA inside a separate container, VM, micro-VM, or policy sandbox when you need isolation from untrusted repositories, generated code, package scripts, or tooling.

## Mental Model

- AVA has an application-level permission model for tool operations.
- Project trust gates project-local resources that can influence prompts,
  commands, subprocesses, plugins, MCP, and LSP.
- Built-in tools add path checks, output bounds, timeouts, process cleanup, and
  audit metadata.
- Plugins, MCP servers, and LSP servers run as child processes with bounded
  protocols, but they are still local programs with the permissions of the AVA
  process unless you run AVA itself in a real sandbox.
- Permission approval is not the same as sandboxing. An approved command or
  plugin may perform effects that AVA cannot inspect after control leaves AVA's
  mediated APIs.

## Permission Model

AVA classifies side effects as operations and resolves each request to
`allow`, `ask`, or `deny` before the tool proceeds. Interactive sessions show a
permission prompt for `ask`; headless modes fail closed unless an explicit
allow flag, session grant, persistent rule, or RPC `permission_reply` resolves
the request. Decisions are recorded in the session audit trail when a session is
active.

Common operations include:

| Area | Operations | Default shape |
| --- | --- | --- |
| Files | `read`, `search`, `edit` | Workspace reads/searches are usually low risk; edits prompt or deny based on mode/path. |
| Shell | `bash` | Sealed and classified before execution; unknown commands ask, and destructive/privileged or raw-shell commands require Critical one-shot approval. |
| Network | `network.fetch`, `network.search` | Explicit approval required. |
| LSP | `lsp.query`, `lsp.server.launch` | Queries are permissioned; server launch asks because it starts a process. |
| Resources | `skill`, `task` | Skill loading requires approval. Task/subagent launch is prompt-free and audited unless an exact persisted task deny matches; only sensitive nested or child actions prompt, and those remain independently permissioned. |
| Plugins | `plugin.execute`, `plugin.tool.call`, `plugin.command.run`, `plugin.event.observe` | Launches and contributed operations require approval. |
| MCP | `mcp.server.launch`, `mcp.server.connect`, `mcp.tool.call`, `mcp.resource.read` | Server process/session/tool/resource actions require approval. |

Persistent permission rules are exact-match allow or deny rules. Deny-only
preflight runs even when built-in policy would auto-allow an operation. Full
allow/deny resolution handles eligible `ask` decisions; a remembered allow
cannot upgrade a built-in hard deny. Matching persistent deny rules win over
matching allow rules, and malformed rule storage fails closed in either phase.
See the [permission gate](../../src/ava/tools/tool_permission.cpp) and
[persistent resolvers](../../src/ava/permissions/permission_rules.cpp).
Normal file tools cannot modify enforceable permission-rule files; manage rules
with `/permissions` or the RPC permission-rule commands.

Useful commands and docs:

- `/permissions audit`, `/permissions diagnose`, and `/permissions list` show
  recent decisions and durable rules. Interactive list/receipt/explain text
  leads with human summaries and display-only ordinals; exact rule ids remain
  required for explain/remove and never yield to ordinals or labels.
- `/diff` and expanded tool cards show file diffs when AVA can compute them
  safely.
- See [USAGE.md](../core/usage.md) for TUI/headless permission flows and
  [headless-protocol.md](../headless-protocol.md) for RPC resolver details.

## Trust-Gated Project Resources

AVA separates ordinary visible context from stronger project-local authority.
`AGENTS.md` and `CLAUDE.md` context files are bounded and can load without a
project trust decision. Resources that can create stronger model influence,
commands, or subprocess authority are skipped until you trust the workspace:

- project prompt commands under `.ava/commands/` or `.ava/command/`
- project skills and custom subagents under `.ava/`, `.agents/`, or `.claude/`
- project plugins under `.ava/plugins/`
- project MCP and LSP config files, `.ava/mcp.json` and `.ava/lsp.json`
- project system prompt files, `.ava/SYSTEM.md` and `.ava/APPEND_SYSTEM.md`

Use `/trust status` to inspect protected resources, `/trust project` to enable
them for this workspace, `/trust deny` to keep them skipped, and `/trust clear`
to remove the saved decision. Trust decisions are stored outside the workspace,
so a repository cannot enable its own executable resources by committing a state
file.

Project trust is an input-loading guard, not a runtime sandbox. Trusting a
project means AVA may load that project's configured resources; tool execution
still goes through the permission model above.

See [CONFIG.md](../core/configuration.md) for the full resource and trust path list.

## Secrets, Protected Paths, And Destructive Commands

AVA treats paths, model output, terminal input, JSON, shell text, and local
config as untrusted. Current hardening includes:

- Secret-looking file reads, edits, and LSP queries are denied with critical
  risk. Examples include SSH/AWS/GnuPG locations, `.env` files other than
  `.env.example`, common credential files, auth files, and path components that
  look like credentials, secrets, or tokens.
- Search results are filtered through the read-file decision per matched file,
  so matches in denied secret paths are skipped instead of injected into model
  context.
- File reads reject non-regular files and enforce size/output bounds. Workspace
  reads are descriptor-anchored: contained relative symlinks can be followed,
  but escaping or absolute symlink targets are refused; the older-kernel
  fallback rejects symlink components.
- Non-secret outside-workspace targets default to high-risk `ask`, not a hard
  deny. `--allow read-only` currently auto-approves otherwise-Ask read/search
  operations even outside the workspace. Matching `--allow-tool read_file`,
  `glob`, `grep`, or `list_directory` flags likewise have no workspace-only
  target condition. Hard secret-path denies and persistent denies still apply.
  See [`headless_policy.cpp`](../../src/ava/app/headless_policy.cpp).
- Plan mode denies source-code edits except planning markdown.
- Normal write/edit/apply-patch tools reject enforceable permission-rule files;
  use the permission management commands instead.
- The sealed command policy classifies destructive/privileged commands such as
  `rm`, `dd`, `mkfs*`, privileged helpers, and destructive Git forms as Critical:
  they require explicit one-shot approval, not an unconditional backend deny.
  Raw-shell syntax, including fork bombs or dangerous redirection, also requires
  Critical one-shot approval. See the [sealed classifier](../../src/ava/command/policy.cpp)
  and [permission decisions](../../src/ava/permissions/permission.cpp).

These checks reduce accidental or model-initiated damage. They do not make a
host account safe to expose to untrusted code. A command, plugin, LSP server, or
MCP server that you approve can still run with the AVA process's OS privileges.

File-tool reads run in AVA itself, outside contained-child Landlock/seccomp
enforcement. `--allow read-only` is read/search approval, **not workspace
confinement**; its outside-workspace policy remains pending, and the current
behaviour above must not be mistaken for denial. Hide unneeded tools with
`--tools`/`--exclude-tools` and use an external sandbox if workspace-only access
is required.

## Bash Tool Safety And Sealed Command Plans

The `/bash` command, `!`/`!!` helpers, and model-visible `bash` tool all use the
sealed command-plan path. A compatibility command that is a simple argv form is
executed directly. Shell syntax is represented as raw shell intent instead of
being mistaken for argv, and remains Critical with one-shot approval only.
Explicit user shell commands are also always Critical even when their text
looks like a standard inspection.

AVA derives a secret-free child environment, preserves the logical cwd spelling
in `PWD`, resolves executables and bounded shebang chains before permission,
binds the approved executable/interpreter inodes, uses the descriptor-bound
workspace cwd, captures bounded output, and cleans up the verified child
process group on timeout/cancel.

Policy details:

- Exact `pwd` and `ls` inspection recipes can be Standard. A path-bearing `ls`
  is contained so a post-check path replacement cannot redirect it outside the
  permitted filesystem view.
- Exact `git status`, `git diff`, and `git log -1` recipes are Standard but
  require containment because repository Git configuration, hooks, filters,
  pagers, or helpers may execute mutable project code.
- Recognized `cmake --build`, `ctest`, `ninja`, `make`, package-manager
  script, pytest, and configured workspace
  script recipes are Standard only under verified development containment.
- Network, installation, publishing, and workspace-mutation families are
  Sensitive and require explicit approval. Destructive/privileged commands,
  inline interpreters, unknown wrappers, and raw shell remain Critical and
  one-shot.
- A persistent Deny is checked before Standard auto-allow. Reusable session or
  workspace Allows exist only for secret-screened, typed recipe identities;
  Critical/raw/unverified commands cannot acquire reusable Allow authority.
- If an executable or shebang interpreter's canonical target is outside the
  roots the containment plan grants, the plan is `Unavailable`. AVA falls back
  to explicit Critical, one-shot approval for uncontained execution instead of
  auto-running. nvm, pipx, and Nix toolchain symlinks can encounter this boundary;
  the plan does not silently grant their arbitrary target directories.

Linux development containment uses Landlock filesystem rules, no-new-privileges,
and a seccomp filter in **both** network modes. With networking denied,
`socket()` and `connect()` return `EPERM`. With networking allowed, `socket()`
permits only `AF_INET`, `AF_INET6`, and `AF_NETLINK` with `NETLINK_ROUTE`;
`AF_UNIX` and other domains remain denied, so local session D-Bus, `docker.sock`,
ssh-agent, and gpg-agent sockets are unreachable by design. Both modes deny
`io_uring_setup()`, but allow private AF_UNIX stream/seqpacket `socketpair()` and
`sendto`/`sendmsg`/`sendmmsg` for IPC such as Python asyncio and Node child-process
pipes; datagram pairs are refused. Inherited-descriptor closure before exec,
not send-syscall blocking, is the defence against inherited sockets.

On Landlock ABI >= 6, contained commands also cannot signal processes or connect
to abstract UNIX sockets outside their Landlock domain. Earlier supported ABIs
retain the filesystem and seccomp policies without this additional scoping.

Executable/interpreter parents normally receive directory read/execute rules.
Parents equal to or containing trusted home (including trailing-slash and
device/inode aliases), or overlapping AVA authority roots, instead grant only
exact-file read/execute access. Startup PATH discovery drops `/` and entries
equal to or containing trusted home, including symlink aliases.

Any failure of the `SECCOMP_GET_ACTION_AVAIL` support probe makes containment
Unavailable and requires Critical, one-shot Ask; there is no `PR_GET_SECCOMP`
fallback. Child installation must still succeed before user code starts.

Remaining containment limitations include:

- Landlock does not filter individual secret files beneath an allowed workspace
  or toolchain root; application file-tool denies do not mediate child reads.
- Below Landlock ABI 6, contained code can signal same-UID processes outside its
  group, including AVA, subject to ordinary OS checks.
- The writable workspace includes an in-workspace `.git`; contained code can
  plant hooks/configuration that affect later Git invocations.
- Descendants that deliberately create a new session still inherit
  Landlock/seccomp but escape verified-PGID cleanup and AVA's resource/lifetime
  control.

Use an external container/VM sandbox for hostile code or stronger isolation.
See [Command containment](containment.md) for the exact contract, implementation
links, and platform fallback.

## Plugin, MCP, And LSP Process Boundaries

AVA keeps extension and server integrations out of the AVA process where
possible, but out-of-process is a containment boundary for AVA stability and
protocol validation, not an OS sandbox.

### Plugins

- Plugins are local executables with a manifest and a versioned JSONL protocol.
- Project plugins are discovered only after project trust and remain disabled
  until explicitly enabled on the local machine.
- Plugin launch and contributed tools, commands, and event hooks are
  permissioned and audited.
- Manifest-declared static prompt and skill files are bounded snapshots read
  beneath a held plugin-directory descriptor. Escaping intermediate symlinks,
  final symlinks, non-regular files, and pathname reopens are rejected.
- AVA bounds plugin stdout protocol records and stderr diagnostics, enforces
  startup/request timeouts, and reports crashes, malformed records, oversized
  records, and unsupported API versions as contained plugin failures.
- The current core-service proxy lets well-behaved plugins ask AVA to perform
  selected read/search/session-status operations through AVA policy. If a plugin
  reads files, runs commands, opens the network, or mutates state directly inside
  its own process, AVA cannot apply built-in file/shell/network policy to that
  internal behavior.

See [plugin-system.md](../extensions/plugin-system.md) and
[plugin-compatibility-policy.md](../plugin-compatibility-policy.md).

### MCP

- MCP servers are explicit local stdio processes configured in global or
  project MCP config.
- Global MCP config cannot point at workspace-relative executables or script
  arguments, and global MCP subprocesses do not launch with the workspace as
  their process CWD.
- Project MCP config is loaded only after `/trust project`.
- Server launch, connection, tool calls, and resource reads are permissioned and
  audited even if an MCP schema looks read-only.
- MCP resources are exposed through opaque read-style tools and require
  `mcp.resource.read` approval before text content enters model context.
- AVA bounds JSON-RPC messages, resource output, stderr diagnostics, and
  request lifetimes.

See [mcp.md](../extensions/mcp.md).

### LSP

Configured LSP servers are also local subprocesses. Global LSP config must use
absolute paths or trusted `PATH` command names; project LSP config is trust
gated. The sole built-in recipe is `clangd`, disabled unless an
owner-controlled global config opts into that exact id. Discovery
accepts only an already-installed executable from fixed system directories or
owner-safe `~/.local/bin`. It rejects workspace/project executables, symlinked
or hardlinked executable files, unsafe ownership/modes/directory chains, and
script wrappers; it never downloads, updates, invokes package/toolchain
managers, or accesses the network. Launch revalidates the sealed descriptor
identity and executes it with `fexecve`. Its permission identity includes clear
exact argv plus a bounded replacement-sensitive fingerprint. The installed-only
`clangd` integration is the sole automatic LSP recipe; every other server
requires explicit configuration and keeps the existing trust
and launch-permission boundary.

Workspace documents, project config, and logical server roots are acquired
through the runtime's shared `AnchorSet`/`AnchorOpen` authority without
canonicalizing their identities. Contained symlink resolution may not leave the
selected writable anchor, and external reads may not enter a writable anchor.
LSP queries and server launch are permissioned, but the language server itself
is still local code once launched. AVA launches it with only the documented LSP
environment allowlist and fixed trusted `PATH`, not the parent process's
provider/cloud/API/token/secret or arbitrary `AVA_*` environment. Capability,
message, notification, diagnostics-cache, document-sync, file, and deadline
bounds remain in force; passive status never launches a server or exposes
paths, argv, fingerprints, config contents, or raw discovery errors.

Cancellation, timeout, and client teardown signal the verified LSP process
group with TERM and then KILL. That covers members which remain in the verified
group; a descendant that calls `setsid` can escape and needs an external OS
sandbox if it must be contained. AVA does not claim cgroup containment.

## No Built-In OS Or Container Sandbox Guarantee

AVA does not currently provide:

- a general whole-AVA, plugin, MCP, or LSP sandbox; the narrow Linux Landlock and
  seccomp path applies only to verified sealed local plans where containment is
  required by command policy
- chroot, user namespace, AppArmor, SELinux, Capsicum, pledge/unveil, cgroup,
  or equivalent general OS policy enforcement
- a bundled container runtime or VM boundary
- per-tool filesystem mounts
- per-tool network namespaces
- a guarantee that plugins, MCP/LSP servers, or arbitrary shell commands outside
  a verified contained plan remain contained, or that a `setsid` descendant stays
  subject to AVA's PGID cleanup or resource/lifetime control
- a guarantee that approved child processes outside the narrow sealed-command
  path cannot read other files available to the AVA process or contact the network
- a guarantee that package scripts, compilers, test runners, language servers,
  plugins, or MCP servers are safe to run

AVA's permission model is still valuable: it makes many effects visible,
auditable, and deniable before AVA starts them. But if your threat model includes
malicious repository code, malicious dependencies, hostile build scripts, or
untrusted extensions, you need OS/container/VM containment in addition to AVA.

## Docker Build Image Is Not A Runtime Sandbox

The [Docker build guide](https://github.com/Artificial-Source/AVA/blob/develop/docs/operations/docker/README.md) documents a Docker image and persistent
container workflow for building and testing AVA. That workflow is a development
convenience, not a complete security sandbox:

- it bind-mounts the repository into the container;
- writes inside a read/write bind mount affect the host;
- mounted config, state, cache, SSH, Git, or provider credential files are
  exposed to processes in the container;
- Docker defaults may still allow network access and other host-mediated
  capabilities unless you restrict them;
- running a container as your host UID reduces root-owned output problems, but
  it does not by itself prevent writes to mounted paths your UID can write.

To use Docker as containment for AVA, create a separate runtime image and run it
with a task-specific policy: minimal mounts, read-only mounts where possible,
ephemeral config/state volumes, least required credentials, non-root user,
restricted network when provider access is not needed, and no host Docker socket
mount unless you intentionally want the container to control the host Docker
daemon.

## Recommended Containment Patterns

Choose containment based on what you are trying to protect. Test the boundary
with canary files and denied network/file probes before relying on it.

### 1. Whole-AVA container for routine isolation

Run the AVA process inside a container with only the workspace copy or bind mount
needed for the task.

- Use a throwaway copy of the repository when you do not want accidental writes
  on the host.
- Use a read-only bind mount for inspect-only work.
- Use a read/write bind mount only when host writes are intended.
- Keep `XDG_CONFIG_HOME` and `XDG_STATE_HOME` inside the container unless you
  intentionally want host AVA auth, settings, sessions, and permission rules
  exposed.
- Pass only the provider credential required for the task, preferably a scoped or
  short-lived key.
- Consider `--network none` only for local/offline workflows; remote model
  providers and web tools need network access.

### 2. VM or micro-VM for stronger local separation

Use a VM/micro-VM when you need a clearer kernel boundary than a normal
container profile provides. Clone or copy the repository into the VM, run AVA
there, then copy reviewed diffs or patches back out. This is slower but avoids
direct write-through to the host unless you configure shared folders.

### 3. Firejail/bubblewrap/nsjail-style policy sandbox

Linux policy sandboxes can restrict visible paths, network access, capabilities,
and process behavior for the whole AVA process. They are useful when you already
operate such tools, but policy details matter: a too-broad home mount or network
allowlist can erase most of the benefit. Treat them as site-specific controls,
not as AVA-provided guarantees.

### 4. Remote or ephemeral development environment

For risky dependency installation or generated code execution, run AVA in a
disposable cloud VM, dev container, remote builder, or CI-like environment. Keep
provider credentials scoped to that environment, review outputs, and destroy the
environment after use.

### 5. Split responsibility when full containment is not available

If you cannot run AVA itself in containment, reduce blast radius:

- work on a disposable branch or copy;
- disable or hide tools you do not need with `--no-tools`, `--tools`, or
  `--exclude-tools`;
- deny project trust until you inspect project resources;
- avoid enabling project plugins/MCP/LSP for unknown repositories;
- run package installs, build scripts, and tests manually in a separate sandbox;
- review diffs before applying or committing.

## High-Level Contrast With Pi

Pi's documented philosophy is that Pi runs locally inside the user's existing
security boundary. Pi does not include a built-in permission system for
filesystem, process, network, or credential access; project trust is an
input-loading guard, and users are expected to containerize or sandbox Pi when
they need stronger boundaries.

AVA intentionally adds more application-level mediation than that model:

- operation-level permissions with `allow`/`ask`/`deny` decisions;
- fail-closed headless behavior unless a resolver, grant, or explicit allow is
  provided;
- persistent exact-match allow and deny rules where built-in policy permits an
  ask;
- audit records with request ids, operations, targets, risks, and resolutions;
- trust gating for project resources that can influence prompts or start local
  code;
- out-of-process plugin and MCP protocols with bounds, diagnostics, and
  permission/audit identities.

The shared high-level lesson is the same: neither AVA nor Pi should be treated as
a substitute for OS/container/VM isolation when running untrusted code. AVA's
permissions reduce and explain local authority; they do not create a hard host
security boundary.

## Before Running Untrusted Work

Use this checklist before opening an unfamiliar repository or enabling new local
code:

1. Run `/trust status`; leave project resources denied until inspected.
2. Start with read/search tools only when exploration is enough.
3. Do not approve plugin, MCP, LSP, shell, network, or task prompts unless you
   understand the target and why it is needed.
4. Keep secrets out of the workspace; prefer environment-scoped or short-lived
   provider credentials inside containers/VMs.
5. Use a container/VM/policy sandbox for dependency installs, build scripts,
   tests, or generated code execution from untrusted sources.
6. Review diffs and session audit entries before copying results back to trusted
   systems.

For engineering changes that add new side effects, use
[development/side-effect-safety-checklist.md](../development/side-effect-safety-checklist.md).
