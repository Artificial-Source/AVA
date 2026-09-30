# AVA MCP Support

AVA supports a bounded local stdio MCP slice for MVP. MCP servers are treated as untrusted local programs selected by the user: AVA mediates launch/connect/tool/resource operations through permissions and audit entries, but it does not claim OS sandboxing.

## Config

MCP servers are configured explicitly in AVA config files. Global config is trusted by the user but must use absolute executable/script paths or trusted `PATH` command names; workspace-relative executables or script arguments such as `./server`, `.ava/server.js`, or `node_modules/.bin/server` are rejected from global config. Global MCP subprocesses launch from the global config directory, or `/` if that source would be workspace-contained, so interpreter/package-manager commands do not resolve modules from an untrusted workspace by process CWD. Project config is skipped until the workspace is trusted with `/trust project`, and that is the intended place for project-local server code.

```text
$XDG_CONFIG_HOME/ava/mcp.json
$WORKSPACE/.ava/mcp.json
```

Use `/mcp list` and `/mcp inspect <server_id>` to inspect loaded servers. Use `/trust status` to see whether project MCP config is active.

## Supported MVP Surface

- standards-compliant newline-delimited JSON-RPC stdio MCP transport with bounded strict records, exact `2024-11-05` version negotiation, bounded list pagination, and stderr diagnostics;
- `initialize`, `tools/list`, and `tools/call` adapted into AVA model-visible tools named with a bounded `mcp_<server>_<tool>` prefix;
- `resources/list` and `resources/read` adapted into opaque no-argument read-style tools that require `mcp.resource.read` approval before content enters model context;
- `prompts/list` and `prompts/get` exposed through the command registry as `/mcp:<server_id>:<prompt_name>` commands and RPC `list_commands`/`invoke_command`, not as automatic model-visible tools;
- permission/audit coverage for `mcp.server.launch`, `mcp.server.connect`, `mcp.tool.call`, and `mcp.resource.read`;
- headless `--allow-tool mcp` for explicitly approved automation flows.

## Process lifecycle and environment

Each stdio MCP start is a one-shot supervised operation: AVA derives an operation owner from the admitted session/run process scope, reserves an `Mcp` Supervisor record, and launches through common `spawn`. There is no persistent MCP daemon or process cache. `/mcp tools`, prompt listing, and tool/resource calls all start a fresh child and shut it down after the bounded protocol exchange. Missing process authority fails before fork.

Child environment is the closed `ava-mcp-explicit-v1` profile: process-computed `PWD`, at most 64 explicit `env` pairs from MCP config, and the trusted `PATH` (`/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin`) when config omits `PATH`. Nothing is inherited from the host, including `HOME`, provider credentials, proxy/CA variables, and arbitrary `AVA_*` names. Config JSON syntax is unchanged; secrets and user-specific values such as `HOME` or API keys must be listed under the server's explicit `env`. Duplicate names, `PWD` overrides, and non-absolute `PATH` components are rejected by the process environment policy. Supervisor settlement covers the managed group, including a descendant that outlives a naturally exiting leader.

## Safety Rules

- Project MCP config cannot run until project trust is granted.
- Global MCP config cannot point at workspace-relative executable/script paths and does not run with the workspace as process CWD; move project-local server code to project MCP config and trust the workspace.
- Launching and connecting to a server are permissioned operations.
- Tool calls are permissioned even when the MCP schema looks read-only.
- Resources are not silently injected into prompts; the model must request the opaque resource tool and pass the `mcp.resource.read` gate.
- Resource reads are text-only in the MVP slice; blob-only or missing text content fails closed.
- Results, stderr, schemas, and JSON-RPC messages are bounded before they reach AVA state or provider context.

## Deferred

Remote Streamable HTTP MCP, OAuth, subscriptions, sampling callbacks, resource templates, binary/blob resources, server-declared side-effect trust, marketplace discovery, plugin-manifest MCP server contributions, persistent MCP daemon pooling, and OS/container sandbox guarantees are deferred until separate safety and rollout designs exist.

See also `docs/extensions/plugin-system.md`, `docs/plugin-compatibility-policy.md`, `docs/headless-protocol.md`, and `docs/core/configuration.md`.
