# AVA Plugin And MCP Compatibility Policy

This policy defines the compatibility contract for AVA's local plugin and MCP foundation as it stabilizes for 1.0. It applies to the `ava.plugin.v1` manifest/protocol surface, plugin-contributed tools/commands/static resources/event hooks, and AVA's stdio MCP adapter.

The current compiled runtime version is still managed separately from this release-position work. Do not infer a runtime version bump from this document.

## Stable v1 Contract

AVA treats these surfaces as the supported v1 extension contract:

- Plugin manifests with `schema_version: 1` and `api_version: "ava.plugin.v1"`.
- Out-of-process plugin JSONL records for `initialize`, `tool.call`, `command.call`, `event.observe`, and the additive host-rendered `ui.status`, `ui.widget`, `ui.select`, `ui.confirm`, and `ui.action` contract.
- Static plugin contributions: `tools`, `commands`, `prompts`, `skills`, and `event_hooks`.
- Plugin discovery and local enablement state for global and project plugin directories.
- Provider-facing tool schema shape for enabled plugin tools and enabled MCP tools.
- Permission categories and audit data for plugin launch/calls/events, including the high-risk `plugin.ui.present` preflight, and MCP launch/connect/tool calls.
- Stdio MCP `initialize`, `tools/list`, `tools/call`, `prompts/list`, and `prompts/get` through AVA-owned registry and command paths.

The contract is intentionally narrow. Plugin code stays out-of-process, project plugins remain disabled until locally enabled, MCP servers are not trusted as safe by default, and side effects still require AVA permission checks when they go through AVA-owned operations.

## Compatible Changes

The following changes are compatible with `ava.plugin.v1` when they preserve existing behavior:

- Adding optional manifest fields that older AVA builds can ignore safely.
- Adding optional fields to plugin protocol requests or responses when existing required fields keep the same meaning.
- Adding new contribution types only when old contribution arrays continue to parse the same way and unsupported contributions fail closed.
- Adding new permission/audit metadata fields without renaming or removing existing fields.
- Accepting a broader JSON-schema subset for tool inputs while preserving already accepted schemas.
- Adding diagnostics, command output details, or runtime events that do not change success/failure semantics.
- Adding new MCP commands or transports behind explicit names and permissions while preserving current stdio tool/prompt behavior.

Compatible additions should be covered by focused tests and, when they affect stable serialized shapes, by small deterministic golden fixtures.

### Additive Host-Rendered UI Boundary

The bounded host-rendered UI slice approved by the [historical Pi-inspired TUI feature expansion record](https://github.com/Artificial-Source/AVA/blob/develop/docs/history/tui-pi-feature-expansion.md) is now an additive `ava.plugin.v1` contract. Old manifests and every existing record retain their meaning. A new manifest opts into the exact independent capabilities `ui.status`, `ui.widget`, `ui.select`, and/or `ui.confirm`; there is no wildcard capability and none is implied by `commands`.

The new requests have exact required fields: `ui.status` has `id`, `type`, and `text`; `ui.widget` has `id`, `type`, non-empty `title`, and 1–8 `lines`; `ui.select` has `id`, `type`, non-empty `title`, `description`, and 1–32 `choices`, whose objects have required unique `id` and non-empty `label` plus optional `description`; `ui.confirm` has `id`, `type`, non-empty `title`, and `description`. Unknown and duplicate fields are invalid. The host's exact `ui.action` has matching `id`, `type`, and `action`; status/widget require `ack`, select permits `select` plus a declared `option_id` or `cancel`, and confirm permits `confirm` or `cancel`. `option_id` exists only for `select`.

The fixed limits are: 65,536 raw bytes and 128 JSON levels per record; 64 unique-id plugin-to-host UI requests per invocation; one status, two widgets, and eight select/confirm records; 96-byte ASCII-grammar request/choice ids; 256 decoded UTF-8 bytes for every text component; 2,048 aggregate title/line bytes per widget; 8,192 aggregate bytes per modal payload; and a single absolute invocation deadline of at most 120 seconds. A widget has at most eight lines and a select at most 32 choices. Host geometry caps modals at 160 display cells by 22 rows and docks at 12 rows, subject to a smaller terminal/composer budget. Text must be well-formed shortest-form UTF-8 with valid Unicode scalars. AVA rejects U+0000–U+001F, U+007F–U+009F, U+061C, U+200E–U+200F, U+2028–U+2029, U+202A–U+202E, and U+2066–U+206F in raw or escaped form, containing ESC/OSC/C1/control and bidi attacks before rendering.

Every command context remains default-null. AVA mints opaque interaction authority only for the exact canonical direct foreground interactive-TUI `/plugin run` command, bound to full command/plugin/invocation identity, the live runtime, and the absolute deadline. RPC, ACP, print, non-TTY/headless, model/tool, hook, background, queued follow-up, synthetic, and plugin-to-plugin routes cannot acquire it. A UI-capable eligible command receives `plugin.execute` and `plugin.command.run` checks first, then a high-risk default-Ask `plugin.ui.present` preflight before child launch even if it never emits UI. Denial, cancellation, expiry, disablement, or malformed/failed exact enablement reads start no child.

While active, a fail-closed predicate re-reads the exact enablement file/workspace/plugin/scope tuple at a finite 25 ms cadence and drives both runner and presenter cancellation. Snapshots must be no-follow, nonblocking regular files no larger than 1 MiB; missing, non-regular, oversized, malformed, and failed reads revoke. External atomic disable therefore terminates output, status/widget, or blocking-modal work promptly and latches revocation; re-enable affects only a later invocation. Same-process `/plugins disable` cannot race a foreground command because that command owns interactive dispatch, so in-flight revocation requires another process/writer.

The host owns all presentation and confirmation. It renders the complete canonical plugin id and command on separate fixed lines, never hashes or ellipsizes identity, and shares display-cell/row geometry between coordinator, renderer, and hit testing. If complete identity plus `Enter` where applicable, `Esc cancel`, `Ctrl+C stop`, and `120s max` cannot fit, the host returns `cancel` before publishing any surface; status/widget then fail because only `ack` is valid. Plugin body text may truncate. Confirm always defaults to Cancel. Close/poll races are revalidated after queue transfer, and cancellation, expiry, disable, child exit, protocol failure, shutdown, and command completion terminate/reap and close idempotently.

UI content is ephemeral: it is excluded from sessions, RPC/ACP responses and events, exports, provider/model input, and plugin diagnostics. Failures do not echo raw UI, raw records, enablement paths, or underlying enablement errors. This contract grants no arbitrary terminal bytes, markup, renderer/native code, geometry/styles/themes, key capture, editor, browser, auth/permission impersonation, file or secret chooser, clipboard, forms, or free-form/secret input. Marketplace delivery and those richer surfaces remain deferred. The [plugin-system contract](extensions/plugin-system.md#host-rendered-foreground-tui-ui) is the full authoring reference. Evidence currently covers deterministic protocol/coordinator/composer behavior and a credential-free tmux smoke, not an untested real-terminal matrix.

## Breaking Changes

These changes are breaking and require an explicit versioned transition rather than silent modification of `ava.plugin.v1`:

- Changing required manifest fields, plugin id/name rules, contribution names, or entrypoint semantics.
- Renaming, removing, or changing the meaning of existing JSONL record types or required fields.
- Changing provider-facing plugin/MCP tool names for existing contributions.
- Tightening accepted schemas or resource paths in a way that rejects previously valid safe plugins without a security rationale and migration note.
- Removing permission prompts, weakening default-deny behavior, or changing audit field meanings.
- Treating MCP server-declared read-only or safety metadata as sufficient to bypass AVA permissions.
- Injecting MCP resources into model context outside the explicit read-style `mcp.resource.read` tool path.

Security containment fixes may intentionally reject unsafe behavior that previously slipped through validation. Those changes still need tests, release notes, and clear diagnostics.

## Deprecation Expectations

When a non-security change needs to replace a supported v1 behavior:

1. Add a compatibility-preserving path first.
2. Document the old and new behavior in extension docs and version-position docs.
3. Emit actionable diagnostics where practical.
4. Keep the old path through at least one documented stabilization/release window.
5. Remove or hard-error only after a follow-up plan explicitly owns the breaking transition.

## JSON And Schema Rules

- JSON object key order is not a public plugin/MCP contract, but AVA's checked-in golden fixtures intentionally lock representative emitted key order so accidental serializer churn is visible.
- Unknown top-level manifest fields are ignored unless a schema-controlled contribution object says otherwise.
- Required fields must remain required with the same type and meaning for `ava.plugin.v1`.
- New optional fields must have conservative defaults and must not grant authority by being present.
- Tool input schemas are preserved as JSON objects and adapted into provider-compatible `parameters` objects. Unsupported schemas should disable the affected contribution with diagnostics rather than crash AVA.
- Serialized audit records should remain machine-readable JSON and preserve existing field names for permission request id, operation, mode, tool name, action, reason, risk, command/path, resolution, source, and reason.

## Diagnostics And Golden Coverage

Contract changes should prefer small, deterministic tests over broad snapshots:

- Keep fixtures under `tests/golden/ava-080/` for this stabilization slice.
- Compare stable JSON shapes after removing insignificant whitespace. These fixtures intentionally do not reorder object keys because they guard AVA's current hand-built serializers, not generic JSON semantic equivalence.
- Avoid generated IDs, temp paths, timestamps, process IDs, and environment-dependent paths in golden files. When volatile values are the behavior under test, assert structural linkage directly.
- Use local fake plugins/MCP servers only; provider live-smokes and network credentials are release-validation work outside this contract policy.

At minimum, representative golden coverage should include plugin tool schemas, MCP tool schemas, and permission audit JSON shape. Focused behavioral tests should cover permission-denial audits and contained plugin/MCP failure paths.

## MCP Boundary And Resource Decision

AVA's current 1.0 MCP boundary is stdio server configuration plus `initialize`, `tools/list`, `tools/call`, bounded-paginated `resources/list`, text-only `resources/read`, `prompts/list`, and `prompts/get`. Launching, connecting, calling tools, and reading resources are permissioned and audited AVA operations.

`resources/list` and `resources/read` are exposed only through bounded no-argument model tools with opaque URI-hash tool names, generic provider-facing schema metadata, `mcp.resource.read` permission prompts, JSON-RPC message size limits, cancellation, diagnostics, and tests. Server-controlled resource names, URIs, MIME types, and descriptions are not sent to provider tool schemas before read approval. MCP resources must not be silently injected into prompt context as part of this contract-hardening slice. Blob/binary resource surfacing, resource templates, subscriptions, and remote transports remain future work.
