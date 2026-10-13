# Desktop Integration Architecture

This document records **narrow integration requirements** for a future optional
Electron desktop client. It is planning, not a shipped UI and not a promise that
a desktop client will ship in the first official host release.

[`rpc-protocol.md`](../rpc-protocol.md) is the authoritative RPC contract. This
page does not add operations, fields, capabilities, or compatibility claims.
Where another description disagrees about wire behavior, the protocol document
wins. Product shape and first-release exclusions remain in
[product principles](../product/principles.md).

## Status

- Desktop remains optional and future. It is outside the first official host
  release.
- This is not an OS-support or distribution document. Linux x64 is AVA's only
  qualified backend platform; Electron starting on another host does not qualify
  a Windows or macOS AVA backend.
- The existing package under `apps/desktop/` is a legacy prototype. It is not
  the greenfield design baseline.

## Backend and transport

Each backend connection talks to a local AVA process through public RPC v1 only.
Start that process with `ava --rpc` (or the equivalent `--mode rpc` /
`--output rpc` forms documented in the protocol). Do not read session JSONL,
credentials, or other backend-private files.

RPC is a local stdio JSONL protocol, not a network server. Follow
[Transport, UTF-8, And JSONL Framing](../rpc-protocol.md#transport-utf-8-and-jsonl-framing):

- stdin carries client-to-AVA request records only;
- stdout carries AVA protocol records only (responses and events);
- stderr is diagnostics; never parse it as RPC.

Clients must correlate by IDs rather than assuming record order. Events and
unrelated responses can interleave on the same stdout stream; see
[Lifecycle, Admission, And Interleaving](../rpc-protocol.md#lifecycle-admission-and-interleaving).

## Process ownership

Three authorities stay distinct:

| Owner | Owns | Must not own |
| --- | --- | --- |
| Electron main | Validated AVA binary path, validated workspace, native folder dialog, child process spawn/lifecycle, and host-generated request IDs | Credentials, session mutation, tool execution, permission policy |
| Sandboxed renderer | Presentation of host-approved state and explicit user intent | Node, generic RPC, raw IPC, choosing a binary/cwd, or supplying RPC correlation/resolver IDs |
| AVA backend | Credentials, sessions, tools, permissions, provider requests, and run settlement | Renderer presentation choices |

The renderer is sandboxed and isolated: no Node, no generic RPC surface, and no
raw IPC. It may send only host-validated, narrowly typed intents. Main generates
client request IDs. Resolver IDs used in `permission_reply` and `question_reply`
are AVA-owned values from backend events, not renderer-supplied correlation; see
[Common Fields, IDs, Correlation, Errors, And Results](../rpc-protocol.md#common-fields-ids-correlation-errors-and-results).

Unknown future event names and additive object fields must be ignored, as the
protocol requires. A client must not infer support for another protocol version
from an event or session version.

## One active run and interleaving

At most one active run exists **per AVA RPC backend process**. Run kinds are
prompt/follow-up, direct bash command, and compaction. Read-only state, catalog, and grant queries documented
as active-safe may interleave; session, history, policy, and model mutations and
a second run return `active_run`. Admission is defined once in
[Lifecycle, Admission, And Interleaving](../rpc-protocol.md#lifecycle-admission-and-interleaving)
and is not restated per command in
[Command Catalog](../rpc-protocol.md#command-catalog).

`steer` and `follow_up` are admitted only for an active prompt/follow-up run.
For a follow-up transition, AVA emits the parent response first and activates
the child immediately before `follow_up_started`. Child runtime events use the
child ID. Desktop UI state must follow those IDs rather than assuming that the
latest stdout record belongs to the latest stdin request.

Parallel independent chats are an [agreed desktop v1 requirement](README.md#agreed-desktop-v1-scope),
not a claim that one RPC process accepts concurrent top-level runs. Backend
lifecycle and concurrent-chat routing remain to be designed and validated.
Switching the viewed chat must not redirect cancel, permission, or question
replies away from their owning backend/run. This note does not select a process
layout or relax session, permission, or admission boundaries.

## Streaming text versus committed settlement

Streaming `message_update` text is in-flight presentation. It is not the settled
result of a run. Prompt success includes `final_text`; the command's exactly-one
response plus terminal runtime events (`done`, `canceled`, or in-run `error`)
settle the run. See [Event Catalog](../rpc-protocol.md#event-catalog): a runtime
`error` event does not replace that response.

Do not treat streamed assistant text as committed session truth, and do not
invent a second transcript authority beside RPC results. Clients should use RPC
results rather than parsing AVA's persisted JSONL.

## Cancel, permission, and question states fail closed

`cancel` sets cooperative cancellation, releases permission and question waits
**fail-closed**, clears queues, emits skipped events, responds to `cancel`, and
emits terminal responses/events as workers observe cancellation. EOF and stdout
write failure perform the same resolver/queue release. Provider requests already
sent are not guaranteed to be aborted or deduplicated.

Pending permission and question UI must fail closed on cancel, disconnect, or
unknown/stale resolver state. Do not default to allow. Reply only through the
documented `permission_reply` and `question_reply` shapes in
[Permission request/reply](../rpc-protocol.md#permission-requestreply) and
[Question request/reply](../rpc-protocol.md#question-requestreply). Wrong types,
duplicates, stale IDs, or invalid combinations do not resolve the pending
request.

Persistent permission rules, session grants, and backend hard denies remain
backend authority. A desktop client must not present a display label, truncated
ID, or local UI row as execution authority.

## Non-goals

This document does not:

- add or preview RPC operations beyond the public v1 catalog;
- claim packaging, installers, signed artifacts, or multi-OS backend support;
- treat GUI tests, Node tests, or a local prototype as evidence that a desktop
  product has shipped;
- replace [product principles](../product/principles.md) or the
  [release-readiness](../product/release-readiness.md) deferral of a production
  desktop client.
