# Desktop screen inventory and reference study

**2026-10-06 · authorized browser-only mock expansion, not a shipped client.**
The user requested settings, model selection, login/connect and chat dialogs so the
whole everyday screen set can be inspected before refinement. Astra at max reasoning
owns implementation/design; Main owns scope, integration and independent evidence.
The [prototype](react-prototype.md) tracks actual completion. An inventory is not
proof a screen is implemented. No live auth/RPC/provider/host/file/persistence work,
new dependency, third-party source/theme/assets or production qualification is implied.

**Implementation follow-through, 2026-10-07:** the bounded revision-11 pack is now
independently mock-validated: **14 families / 114 manual scenarios** and eight
Settings sections, with shared Settings/model/provider views also reachable from
main chat. The [current prototype evidence](react-prototype.md#revision-11-independent-validation)
records 316 tests, native-browser behavior/layout/focus checks, finite contrast
and preservation. The [canonical scenario index](../../apps/desktop-preview/src/screens/fixtures.ts)
is the implemented scope; this research table does not prove live capabilities.
`?view=screens` and the controlled component catalog stay separate lazy reviews.

## Read-only source comparison

| Reference | Pin and concrete behavior | AVA implication, not adoption |
| --- | --- | --- |
| OpenCode v2 | `740072694de71fae23f35131591e7c32a9713014`: [settings surface](https://github.com/anomalyco/opencode/blob/740072694de71fae23f35131591e7c32a9713014/packages/app/src/settings/surface.tsx), [settings shell](https://github.com/anomalyco/opencode/blob/740072694de71fae23f35131591e7c32a9713014/packages/app/src/settings/shell.tsx), [model selector](https://github.com/anomalyco/opencode/blob/740072694de71fae23f35131591e7c32a9713014/packages/app/src/providers/models/select-dialog.tsx), [provider flow](https://github.com/anomalyco/opencode/blob/740072694de71fae23f35131591e7c32a9713014/packages/app/src/providers/connect/controller.ts) | A full settings surface with shared selectors, provider method/back/error states and contextual Escape is useful. Provider login, server pairing and Console/cloud account are different products, not one generic login. |
| OpenCode original | `2406400f0aeb07b36d0495af4e05aaca49159832`: [settings dialog](https://github.com/anomalyco/opencode/blob/2406400f0aeb07b36d0495af4e05aaca49159832/packages/app/src/components/settings-v2/dialog-settings-v2.tsx) | Earlier modal settings are historical comparison; do not mix old/v2 route claims. |
| Pi | `898ab804050730e9dcefb4443875d5a932aa6a32`: [settings](https://github.com/earendil-works/pi/blob/898ab804050730e9dcefb4443875d5a932aa6a32/packages/coding-agent/src/modes/interactive/components/settings-selector.ts#L434-L829), [model selector](https://github.com/earendil-works/pi/blob/898ab804050730e9dcefb4443875d5a932aa6a32/packages/coding-agent/src/modes/interactive/components/model-selector.ts#L76-L244), [login dialog](https://github.com/earendil-works/pi/blob/898ab804050730e9dcefb4443875d5a932aa6a32/packages/coding-agent/src/modes/interactive/components/login-dialog.ts#L55-L167), [session selector](https://github.com/earendil-works/pi/blob/898ab804050730e9dcefb4443875d5a932aa6a32/packages/coding-agent/src/modes/interactive/components/session-selector.ts#L794-L1039) | Search/current/default models, cached/error states, cancel/editor-focus restoration and separate auth methods supply TUI behavior evidence—not desktop geometry or permission semantics. |
| Zeron | `1c83cda221bd18a3b55ea49e5b25ec2c148955bc`: [providers](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/settings/harnesses.rs#L219-L274), [model picker](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/pickers.rs#L1076-L1518), [login states](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/settings/accounts.rs#L698-L1090), [command palette](https://github.com/zeronsh/zeron/blob/1c83cda221bd18a3b55ea49e5b25ec2c148955bc/crates/ui/src/shell/command_palette.rs#L17-L171) | Providers absorb legacy Accounts; selectors and login can share careful generation/cancel/focus behavior. Do not copy harness architecture, native/private state, auto-allow policy, device/voice or full IDE scope. |

Local pins and actual consumers were inspected without fetch/build/application launch,
auth/config/session reads or retired grok-build. Source presence is not tested runtime
availability/accessibility. The codebases suggest reuse, not identical screen counts,
fonts, themes, component APIs or backend behavior.

## Bounded everyday mock screen set

| Family | Required reviewable screens/states | Ownership for a future real client |
| --- | --- | --- |
| Settings | General, Appearance, Models, Providers, Shortcuts, Permissions & trust, Resources/connections, About; search/no-match, local changed/reset confirmation | UI preferences versus separate backend configuration domains; not universal Save to one file. Current mock is dark-only; Light/System and rebinding cannot be presented as working if unimplemented. |
| Model picker | Search/grouped provider rows; current/default scope, capability/window/reasoning facts; ready/loading/empty/no-match/error/stale/unavailable/busy; explicit selection/cancel | Backend catalog and mutation policy; supplied typed UI choices. Match current fictional Aster/Aster Mini/Orion identities, not a live/discovered catalog. |
| Provider directory/connect | Available/connected/externally managed/error; method choice; masked **dummy** key; browser/device sign-in instructions; waiting/success/error/retry/cancel; disconnect confirmation | Backend credentials/OAuth; host opens browser. No AVA cloud-account signup, real secrets, browser launch, polling, storage or network. Auth success and model availability are separate. |
| Session search/actions | Project filter/search/no-match/loading/error; rename validation/confirmation; archive and branch intent previews with original chat preserved | Real session APIs and leases. Rename/fork/clone are public; archive has no RPC-v1 command. No public delete-session action is invented. These are not Git branches/worktrees. |
| Commands | Searchable known UI actions, empty/loading/disabled; Escape/focus restoration | Public registry/host binds actions. No arbitrary slash/shell execution from preview text. |
| Chat detail/dialogs | Message projection, tool detail/output, run error/retry/cancel-requested/canceled, permission/question variants | Reuse controlled chat widgets. Explicit local intents are not tool execution, settlement or backend replies. Permission session choice only when advertised; no "always" grant. |
| Workspace/recovery | Invented workspace choices, opening/ready/unavailable/offline/error/retry/cancel | Electron main validates real directory/binary and process lifecycle; no file picker/path read/restart in browser mock. |
| Existing baseline | Main chat, logo-only new chat, readonly child inspection, Agents, Changes, Context, composer/model/mode/references, separate component catalog | Preserve identity, parent ownership, sidebar-only pins/recents, neutral geometry and safe rendering. |

A separately reachable lazy review area can expose fixture scenarios without adding
mock scenario controls to normal chat. Settings and model selection should share the
same components when opened from chat. Keep overlays native/testable and preserve
drafts, selected parent, child read-only state and focus; do not build a thick store.
Every screen must have an observable way back and a truthful unsupported/unknown state.

## AVA-specific limits and safety

- Public model/reasoning, session-name/fork/clone and permission/grant/rule APIs are
  described in [RPC v1](../rpc-protocol.md#command-catalog). One active top-level run
  per process limits mutation; independent chats require future host routing.
- There is no AVA cloud account. Built-in OpenAI connect supports browser/device/API
  key; other providers' auth differs. Read [providers](../core/providers.md#auth-modes-and-precedence)
  and [custom providers](../core/custom-providers.md#auth-modes). Do not invent OAuth
  for custom providers or confuse environment-managed credentials with removable ones.
- Settings are separate owner-validated domains; project resources require trust.
  See [configuration](../core/configuration.md#settings-architecture-and-resource-packages),
  [context resources](../core/context-resources.md#project-trust) and
  [process ownership](architecture.md#process-ownership). Plugin/MCP/LSP management
  screens are not permission to install/run packages, remote MCP OAuth or a marketplace.
- System-prompt/current-provider-payload and active-estimate RPC gaps remain described
  in [Context research](sidebar-activity-research.md#context-information-follow-on-2026-10-06).
  Projected local messages are not a raw provider export or private reasoning UI.
- Controlled widgets emit explicit typed UI intents. Dismissal is not approval;
  pending/stale/closed/duplicate request and unmount rules remain fail-closed. Secret
  demo fields are masked, ephemeral, cleared on close and excluded from receipts/logs.
- No fake native process/filesystem authority, full IDE/Git/editor/terminal, sync/cloud,
  microphone, image generation, default worktrees or copying third-party assets.
  Dark/system-light design and production/native/accessibility/legal/packaging work
  remain separately scoped. These exclusions do not prevent complete inspectable mocks
  for the bounded families above.
