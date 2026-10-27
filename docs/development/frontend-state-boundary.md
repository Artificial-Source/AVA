# Frontend State Boundary

Ordinary backend facts have one canonical owner and registered frontend
projections. Appearance and interaction state belongs to the TUI thread.
This reference describes both that boundary and the precise interactive,
history and service interfaces retained alongside it. It is not a new popup,
history or public C++ API. See the [codebase guide](codebase-guide.md#frontend)
for the frontend module's responsibilities.

## Ownership classes

| Class | Owner and meaning | Boundary |
| --- | --- | --- |
| B | Backend-affecting facts that are or may become visible | The seven reviewed canonical wrappers below, observed through registered projections. A dirty flag invalidates a cache; it does not confer authority. Catalogs and todos remain pending-review transports, not additional wrappers. |
| U | Pure appearance and interaction | TUI-thread-local theme, bindings, layout, focus, selection, previews and editable drafts. No `UnlockedData` or new mutex. Only the submitted string and explicitly captured request parameters cross the submit boundary. |
| D | Deferred interaction, historical content or service | Existing permission/question/plugin exchanges, history pickers, streaming content, subagent/branch operations, on-demand popups and image/editor/Mermaid services. Their existing authority, ordering and synchronization remain necessary. |
| I | Immutable initialization | `app_version`; no change tracking or canonical session copy. |
| A | Action outcome or control | Success/error, correlated status text, quit and committed-session identity reads. An action result does not deliver the ordinary current state. |

The no-configured-primary definition is named `none`; it is not inferred from
model identity. Explicit agent selection resolves an actual configured definition.
The composer label and the next normal request use the same selected-agent
authority; inspector focus and model-picker previews cannot change that authority.
Failed prompt reconstruction remains execution-blocking until recovery, distinct
from a legitimate empty definition-specific prompt.

## Canonical inventory

Alias locations are repository paths; they are deliberately not packaged-doc
links. The first six members belong to `Session_aggregate_base` in
`src/ava/app/runtime/Session.h:65-72`; the seventh belongs to its `store`.
Projection files are under `src/ava/frontend/`, with the named class in matching
`.h`/`.cxx` files.

| Alias declaration | Canonical member and writers | Projection and refresh accessors | Exact copy versus derivation |
| --- | --- | --- | --- |
| `src/ava/app/runtime/AgentSelection.h:30`: `AgentSelection_ts` | `agent_selection_`; `Session::open`, `apply_trust_prompt_state_and_refresh`, `replace_with` | `AgentSelection`: `agent_refresh`, `provenance_refresh`, `tool_preset_refresh` | Exact copies of `selected_primary_agent.name`, `.provenance`, `.tool_preset` only. No prompt body, definition path or fabricated model override. Enum labels are derived. |
| `src/ava/app/runtime/Directories.h:20`: `Directories_ts` | `directories_`; `Session::open` (including `open_at` inputs) and `replace_with` | `Directories`: `workspace_root_refresh`, `current_directory_refresh` | Whole `Directories`, including additional writable directories. Display paths derive from `workspace_dir` and `current_dir` separately; writable-directory-only changes need not change either label. |
| `src/ava/app/runtime/GitBranch.h:18`: `GitBranch_ts` | `git_branch_`; `Session::open`, `refresh_git_branch`, `replace_with` | `GitBranch`: `git_branch_refresh` | Whole canonical `GitBranch`; `name` is the exact observed string and empty means no branch observed. Backend observation retains the startup/action/completion cadence. Rendering does no Git or filesystem work. |
| `src/ava/app/runtime/ModelSelection.h:31`: `ModelSelection_ts` | `model_selection_`; `Session::open`, model/reasoning mutation methods, scoped-model commands, `replace_with` | `ModelSelection`: `model_refresh`, `provider_refresh`, `reasoning_refresh`, `context_window_tokens_refresh` | Whole `ModelSelection::model` (`ModelInfo`) and optional `reasoning`, copied together. Model/reasoning labels are derived; provider and numeric window come from that copy. The optional scoped cycle remains backend authority and is not copied into a new scope projection. |
| `src/ava/app/runtime/ResolvedPromptState.h:24`: `PromptModeAndContextSources_ts` | `resolved_prompt_state_.mode_and_context_sources`; `Session::open`, `apply_prompt_state`, model/trust prompt reconstruction, `replace_with` | `PromptModeAndContextSources`: `mode_refresh`, `context_source_count_refresh` | Exact mode plus derived `context_sources.size()` under one guard. No context vector, assembled prompt, or duplicate backend count. Zero is known empty. |
| `src/ava/session/Stats.h:70`: `Stats_ts` | `session_stats_`; `refresh_session_statistics` in `SessionStatisticsPublication.cpp`, startup and `replace_with` | `SessionStats`: `token_count_refresh`, `contex_percentage_refresh` (current spelling) | Whole `Stats`, including cumulative provider usage and separate active-history/system-prompt estimates. Saturating active total, compact text and percentage are derived caches. Unknown estimates stay unknown. Exact total changes can change percentage without changing compact text; model-window changes also refresh percentage independently. |
| `src/ava/session/session_store.h:97`: `SessionStoreOptions_ts` | `store.options_`; `SessionStore` constructors/copy/move assignment and session replacement | `SessionIdentity`: `session_id_refresh`, `session_path_refresh` | Whole options copy; session ID is exact, path is derived by `session_path_for_options`. Independently consumed ID/path display changes are not the committed-transition control signal. |

`FrontEnd::register_frontend_data` registers the seven final addresses before
typed `FrontEnd::initialize_frontend_state` initializes its frontend-neutral projections. `FrontEnd::data_changed`
routes each registered index. A write guard notifies after unlocking, even for
an identical write. Each projection clears dirty atomically before reading,
retains pending changes independently for its exposed fields, and suppresses
redisplay for equal displayed values. Refresh results must not escape the
frontend's lifetime or be treated as backend write authority.

Changed flags have the implemented single-consumer ownership, not independent
subscriptions for every renderer. In particular `SessionStats` consumes the model
window notification when the two projections are connected. Other model string
refreshes do not consume it. Registered assignment preserves the destination
address/index and mutex; detached construction remains unregistered.

## Seam contract

`src/ava/tui/runtime.h` defines the following complete `TuiRuntimeOptions`
field set. Rows group fields only when their ownership and delivery contract match.
Classes describe payload meaning, not merely the file containing a callback.

| Fields | Class | Contract |
| --- | --- | --- |
| `app_version` | I | Immutable build label. |
| `initial_status` | U/A | Startup diagnostics, not a canonical status register. |
| `initial_transcript` | D | One-time onboarding/content input. |
| `slash_commands`, `slash_catalog_generation`, `file_references`, `workspace_catalog_generation` | B, retained transport | Initial hydration of pending-review catalog domains. |
| `initial_display_settings`, `key_bindings` | U | Local effective settings and bindings. |
| `mermaid_render` | D/U | Retained worker bridge plus local configuration parameters. |
| `read_committed_session_id` | A | Narrow application-owned identity read outside the UI lock, only at serialized boundaries. |
| `read_catalog_delivery`, `read_committed_todos` | B, retained transport | Pulled outside the UI lock like the identity read, then applied at serialized non-quit action/completion boundaries, never during render. |
| `read_startup_overview`, `read_project_trust` | D | Owning filtered popup DTOs, read once per popup open; not ordinary boundary refreshes. |
| `create_active_run_queues`, `on_submit` | D/A | Existing bound queue and submit/content protocol, without ordinary state fields. |
| `on_attach_image`, `on_paste_clipboard_image`, `on_external_editor`, `on_load_image_attachment` | D | Validated service results; optional absence, errors and replacement content remain distinct. |
| `on_toggle_mode` | A | `VoidResult`; display comes from canonical prompt metadata, including the sidebar mode. |
| `on_cycle_reasoning`, `on_cycle_model` | A | `Result<std::string>` feedback, not authoritative labels. |
| `on_reload_key_bindings` | U | `Result<TuiKeyBindings>` for local application. |
| `on_reload_display_settings`, `on_maybe_reload_display_settings`, `on_local_display_settings_action` | U | Local effective results; poll absence means unchanged, presence means applied even when displayed values match, error preserves last-good state. |
| `model_selector_view`, `reasoning_selector_view`, `scoped_model_selector_view`, `session_selector_view` | D | Retained on-demand views; no eager model/scope subscription. |
| `list_subagents`, `inspect_subagent`, `cancel_subagent`, `promote_subagent` | D | Owner-bound hidden IDs, historical inspector frames and narrow control outcomes. |
| `on_open_fork_user_turn_selector`, `on_open_copy_user_turn_selector`, `on_read_user_turn_text` | D | Authorized history picker/exact-text access. |
| `on_session_selector_sort_cycle`, `on_session_selector_named_filter_toggle`, `on_session_selector_path_display_toggle`, `on_session_selector_archived_filter_toggle`, `on_session_selector_label_timestamp_toggle` | U, retained popup path | Existing selector controls and view delivery, not new canonical domains. |
| `on_session_selector_archive`, `on_session_selector_unarchive` | A/D | Validated persistence actions retaining popup results. |
| `on_session_selector_branch_parent`, `on_session_selector_branch_child`, `on_session_selected`, `on_fork_user_turn_selected` | A | `Result<std::string>` feedback; reconcile committed identity separately before retained pulls. Preserve post-commit warnings and error-path synchronization. |
| `on_branch_summary_prepare`, `branch_summary_snapshot`, `on_branch_summary_confirm`, `on_branch_summary_cancel`, `on_branch_summary_refresh_catalog` | D | Existing generation-bound confirmation/status/catalog protocol. |
| `remember_permission_rule` | D | Permission-rule authority/result correlation. |
| `on_settings_selected`, `on_model_selected`, `on_reasoning_selected` | A | `Result<std::string>` feedback; backend validates and publishes, frontend refreshes. Local settings actions use the separate local path. |
| `on_scoped_model_toggled`, `on_scoped_model_enable_all`, `on_scoped_model_clear_all`, `on_scoped_model_toggle_provider`, `on_scoped_model_reorder`, `on_scoped_model_save` | D | Existing scoped-model popup/persistence contracts, including prior-view arguments. |
| `on_before_tui_shutdown` | D | Existing cancel/join barrier while curses is alive. |

`TuiSubmitResult` retains only `quit`, `ordinary_turn_committed`, `output`,
`tool_timeline`, `ordinary_turn_request_ids`, `local_command_result`,
`conversation_output` and `conversation_tool_timeline`. These are outcomes,
attribution and content, not an ordinary-state bundle. The old bulk type,
submit-state member, snapshot-returning actions and apply helpers are removed.

`TuiCatalogDelivery` contains just the two vectors and paired optional
generations. A repeated present generation retains current values, even when
the vector is empty because unchanged data was omitted. A new or absent
generation applies the vector, including a genuinely empty catalog. Pull and
apply form one synchronous step, so constructing a delivery is not separately
mistaken for application. There is no claim of multi-subscriber delivery.
Local completion hints are recomposed on actual source changes.

Todo hydration keeps its former cadence except on quit. Completion hydration
now runs on the TUI thread; this still loads retained history and is not a
canonical todo publication implementation. No catalog/todo wrapper is approved
by this cutover. General history access remains a separate design.

Presentation transitions compare the committed identity, independently of
consumed display flags. Two replacements before one boundary produce one reset
to the final identity. An A-to-B-to-A round trip before an A-bound presentation
boundary produces no reset: the seam has no backend generation. Existing
request/session correlation rejects stale active-run content separately.

## Retained interfaces

These paths are retained by decision. No pending prompt or operation state goes
into `UnlockedData`. A dirty notification neither grants authority nor completes
a waiting request. Partial removal or an unimplemented endpoint requires explicit
agreement on the affected feature and safe user-visible unsupported behavior.
The default is preservation, not a stub, automatic reply or successful empty
history result.

Local selection, focus, layout, previews and drafts remain TUI-thread-owned,
without new locks or `UnlockedData`. Existing synchronization below belongs to
cross-thread protocols and services; it is not a precedent for locking local
preferences. Independently exposed ordinary agent, model, directory and token
facts use their registered projections, even when an interaction also displays
them. Inspector focus is not the active agent receiving the next Enter.

For the retained protocols, coupling to the removed bulk transfer is none except
`on_submit` completion and the fork/session actions whose results become status
strings. The pending-review catalog/todo transport is listed separately because
its payload still needs an approved canonical-type review. Protocol snapshots,
content frames and popup results are not ordinary bulk-state replacements.

The regression column names existing anchors, not fresh test results or proof of
all concurrency interleavings. CTest names come from
`tests/CMakeLists.txt`. Several source files run in
composed suites: permission, selector, command-output, keybinding and TUI Mermaid
coverage is in `ava_tests.tui_composer`; application catalog, history and startup
coverage is in `ava_tests.app_runtime`; event coverage is in
`ava_tests.app_event_bus`; Mermaid coordinator coverage is in
`ava_tests.mermaid_render`. The isolated rendering cases have their own CTest
names. Scenario names refer to
`tests/tui_tmux_scenarios/`, registered as
`ava_tui.tmux_smoke_<scenario>`. Those terminal tests are opt-in; a skip is not a
pass. See the [testing guide](../operations/testing.md) for execution requirements.

| Retained path | Principal files and types | What must be preserved | Existing synchronization | Coupling to bulk-transfer removal | Regression anchors |
| --- | --- | --- | --- | --- | --- |
| Permission and question popups | `src/ava/tui/runtime_prompts_internal.h` and `src/ava/tui/runtime_prompts_internal.cpp`: `RuntimePromptCoordinator`, `PendingPermissionRequest`, `PendingQuestionRequest`. `src/ava/permissions/permission.h`: `PermissionResolver`, `PermissionPrompt`, `PermissionResolutionDecision`. `src/ava/agent/question.h`: `QuestionResolver`, `QuestionPrompt`, `QuestionAnswer`. `src/ava/tui/runtime.h`: `TuiSubmitContext` resolvers and `remember_permission_rule`. | Each invocation owns its prompt and reply. Permission requests have precedence over questions; this is not a single global FIFO. Rendering or clearing a local view does not complete a waiter. Deny is the initial permission choice. Closing acceptance denies queued permissions and interrupts queued questions. Session grants and persistent-rule decisions remain distinct backend authority; persistence failure must not grant Allow. | Coordinator queue mutex, separate permission/question deques, atomic admission flag, per-request mutex/condition variable/result, and a separate audit-sink mutex. A request is claimed under the queue lock and serviced outside it. | None. Keep resolver, waiter, audit, cancellation and rule-action delivery intact. | `ava_tests.tui_composer` (`tests/tui_permission_tests.cpp`); `ava_tests.permission_rules`. Scenarios: `main_permission_flow`, `main_question_flow`. |
| Plugin UI | `src/ava/tui/runtime_plugin_ui.h`: `TuiPluginUiEndpoint`, `TuiPluginUiBinding`, `TuiPluginUiRequest`, `TuiPluginUiReply`. `src/ava/tui/runtime_plugin_ui_internal.cpp`: `RuntimePluginUiCoordinator`. `src/ava/app/tui/interactive_tui.cpp` adapts `TuiSubmitContext::plugin_ui` into an invocation capability. | Preserve plugin/command/invocation binding, hidden request and option IDs, queue generations, live runtime token, deadline, cancellation and late-reply rejection. Status/widgets belong to the same invocation as select/confirm replies. Preserve bounded payloads and rejection when identifying host chrome and controls do not fit. No plugin label is authority. | Shared coordinator-state mutex, bounded queue and per-modal mutex/condition-variable reply. Endpoint lifetime is guarded by a weak runtime token. Completion, cancellation and shutdown invalidate the invocation and clear its surfaces. | None. Status and widget delivery must not be extracted from the invocation protocol into passive state. | `ava_tests.plugin_ui`, `ava_tests.plugin_ui_authority_inventory`. Scenario: `plugin_ui`. |
| Active-run queues, events and completion content | `src/ava/tui/runtime.h`: `TuiActiveRunQueues`, `TuiQueuedFollowUp`, `TuiRestoredQueuedMessage`, `TuiSubmitContext`, `TuiSubmitResult`. `src/ava/app/interactive_run_queue.h`: `InteractiveRunQueue`. `src/ava/event/RuntimeEvent.h`, `src/ava/event/EventEnvelope.h`, `src/ava/tui/runtime_state_internal.h`: `RuntimeEvent`, `EventEnvelope`, `RuntimeEventQueue`. `src/ava/tui/runtime_active_run_internal.cpp` and `src/ava/app/tui/interactive_tui_support.cpp` settle content and queued follow-ups. | Preserve exact session/request/correlation binding, event order, steering/follow-up start/skip/restore/finish behavior, cancellation and subagent-launch notifications. Stop old-session follow-ups after a transition. Restore returns text to the local draft without submitting it. Keep `output`, `tool_timeline`, `local_command_result`, `conversation_output`, `conversation_tool_timeline`, `ordinary_turn_committed` and `ordinary_turn_request_ids`. Local-command events gain no conversation authority without returned ordinary-turn IDs; missing IDs fail closed. | Backend queue mutex; event-queue and event-context mutexes; submit-worker future; UI-lock-protected presentation settlement. Mutable draft editing remains separate from submitted/queued strings. | `on_submit` completion settles through the committed-session transition and then pulls retained catalog/todo data at the serialized boundary. A `quit` completion skips those pulls. Removing ordinary state delivery does not remove content/control fields or redesign the event stream. | `ava_tests.app_event_bus` (`tests/events_tests.cpp`); `ava_tests.tui_composer` (`tests/tui_command_output_tests.cpp`); `ava_tests.tui_composer_rendering.changed_session`, `ava_tests.tui_composer_rendering.session_transition`. Scenarios: `active_run`, `restore_followup`, `streaming_scroll`. |
| History pickers and exact-text readers | `src/ava/tui/runtime.h`: `on_open_fork_user_turn_selector`, `on_open_copy_user_turn_selector`, `on_read_user_turn_text`, `on_fork_user_turn_selected`, session selection/navigation callbacks. `src/ava/app/session_user_turns.h`: `SessionUserTurnList`, `list_session_user_turns`, `read_session_user_turn_text`. `src/ava/app/tui/command_palette_views.cpp`: `user_turn_selector_view`, returning `SelectListView`. | Preserve stable hidden entry/session IDs, newest-first bounded previews, exact public-user-text lookup at action time, and fail-closed missing/non-user/private entry handling. Fork requires a persistent session; copy remains available for ephemeral sessions. No authority switch must remain an error. After a committed fork, catalog-refresh failure remains a visible warning rather than hiding the new authority. Canceling a picker does not mutate canonical selection. | UI-thread picker/filter state. Application helpers briefly acquire Session read access and load through its bound read authority. Mutation and leases stay in the backend fork/open paths. No new history snapshot or synchronization domain is introduced. | Picker/read contracts are unchanged. Fork and session open/parent/child actions return `Result<std::string>` feedback instead of a bulk bundle; the TUI synchronizes committed identity and refreshes at the action boundary, including after errors that may follow mutation. | `ava_tests.app_runtime` (`tests/app_runtime_session_command_tests.cpp`); `ava_tests.tui_composer` (`tests/tui_selector_tests.cpp`). Scenarios: `main_session_mgmt`, `branch_summary`. |
| Subagent workspace | `src/ava/app/tui/subagent_workspace.cpp`, `src/ava/agent/subagent_coordinator.h`, `src/ava/agent/subagent_inspector.h`: `SubagentCoordinator`, `SubagentInspectorFrame`, `SubagentLiveMessage`. `src/ava/tui/runtime_subagent_workspace_internal.cpp`: `RuntimeSubagentWorkspaceController`; `list_subagents`, `inspect_subagent`, `cancel_subagent`, `promote_subagent` in `TuiRuntimeOptions`. | Preserve parent ownership and hidden exact job IDs, optional known generation, bounded path-free committed user/assistant content, terminal/freeze/unavailable/refresh-unavailable flags, polling and narrow cancel/promote outcomes. Published content generation differs from internal source/freeze epochs. Refresh failure may retain prior content and is not successful empty history. Inspector selection does not select the next-prompt agent. | Backend coordinator/job/source synchronization stays backend-owned. Application captures parent identity/coordinator under Session access and uses owner-bound adapters. TUI focus, scroll and polling state are local. | None. Keep `reset_for_session_transition` in the committed-session transition so an old parent's workspace cannot survive replacement. | `ava_tests.subagent_coordinator`, `ava_tests.tui_composer` (`tests/tui_selector_tests.cpp`). Scenario: `subagent_workspace`. |
| Branch summary | `src/ava/app/branch_summary_coordinator.h`: `BranchSummaryCoordinator`, `BranchSummarySnapshot`. `src/ava/app/tui/interactive_tui_support.cpp`: `tui_branch_summary_snapshot`. `src/ava/tui/runtime.h`: `TuiBranchSummarySnapshot`, prepare/snapshot/confirm/cancel/refresh-catalog callbacks. | Preserve read-only preparation, rejection of conflicting operations, separate generation-bound confirmation, deadline starting at confirmation, and nonmutating pre-confirmation cancellation. After confirmation retain lease, revalidation and append authority. TUI delivery remains bounded labels, phases and fixed eligibility/failure/append-commit codes, not source payloads. Keep `refresh_required` after authoritative success/existing observation. | Coordinator mutex/condition variable, cooperative stop source and one joined worker; backend source leases and revalidation. Confirmation/cancellation use operation generation, not display dirtiness. | None. `on_branch_summary_refresh_catalog` returns `Result<SelectListView>`; its refresh integration is not a bulk ordinary-state result. Keep the complete operation snapshot protocol. | `ava_tests.branch_summary_coordinator`, `ava_tests.tui_composer`. Scenario: `branch_summary`. |
| Images, external editor and Mermaid services | `src/ava/tui/runtime.h`: `on_attach_image`, `on_paste_clipboard_image`, `on_load_image_attachment`, `on_external_editor`, `TuiMermaidRenderBridge`, `TuiMermaidRenderRequest`, `TuiMermaidRenderCompletion`. `src/ava/session/attachments.h`: `ImageAttachmentRef`, `LoadedImageAttachment`. `src/ava/app/external_editor.cpp`, `src/ava/app/tui/mermaid_tui_bridge.h`, `src/ava/tui/runtime_mermaid_internal.cpp`, `src/ava/app/mermaid_render_coordinator.h`. | Image import/load keeps backend validation and authorized references; no clipboard image differs from error and valid import. Editor `Result<std::optional<std::string>>` distinguishes replacement, no replacement and error; failure preserves the original draft and restores the terminal. Mermaid enqueue/cancel/drain keeps request identity/configuration epoch, queue-full/rejected outcomes, stale-completion rejection and cancellation/shutdown behavior. | Image actions retain existing Session/store and clipboard-helper ownership. Editor handoff is serialized on the TUI thread. Mermaid bridge mutex and coordinator worker synchronization remain; enqueue uses a try-lock rather than blocking the frontend. `on_before_tui_shutdown` cancels/joins application workers while the terminal is still active. | None. A draft copy passed to the editor is not Session state. Image visibility/width and Mermaid presentation records remain local, not service authority. | `ava_tests.clipboard_image_process`, `ava_tests.clipboard_image_static_contract`, `ava_tests.tui_composer`, `ava_tests.mermaid_render`. Scenarios: `main_paste_scrollback_attach`, `main_editor_input`, `mermaid`. |
| `/models`, `/scoped-models`, `/settings` Workspace trust and `/overview` popups | `src/ava/tui/runtime.h`: `model_selector_view`, `reasoning_selector_view`, `scoped_model_selector_view`, scoped-model actions, `read_project_trust`, `read_startup_overview`. `src/ava/tui/composer.h`: `SelectListView`, `ProjectTrustSnapshot`, `StartupOverviewSnapshot`. `src/ava/app/tui/interactive_tui.cpp` owns application bindings. | Keep on-demand popup list/result contracts, model/reasoning handoff and optional scope semantics, scoped-model toggle/enable/clear/provider/reorder/save flows, and trust decisions. A canceled local candidate/preview must not change canonical selection. Trust remains unwrapped. Overview/trust readers return owning filtered DTOs once per popup open, not ordinary render subscriptions. | UI-thread popup state. Overview and trust readers briefly acquire a Session read guard for bounded in-memory data, without trust-file/resource reload. Backend selection/persistence actions keep their existing ownership. | None in popup delivery. Model/reasoning/settings action feedback is a status string, not replacement ordinary state. Trust changes still refresh application catalog values and reach the palette through the boundary pull. | `ava_tests.app_runtime`, `ava_tests.tui_composer`, `ava_tests.tui_composer_rendering.display_reload_overview`. Scenarios: `main_models_selectors`, `startup_overview`, `main_startup_trust_keybinds`, `nested_settings_preview`. |
| Initial transcript | `src/ava/tui/runtime.h`: `TuiRuntimeOptions::initial_transcript`, a vector of `TranscriptItem` from `src/ava/tui/composer.h`. `src/ava/app/tui/interactive_tui.cpp` supplies onboarding content; `src/ava/tui/runtime_state.cpp` moves it into presentation. | This is a retained initialization/history exception, not a duplicate initial copy of migrated ordinary facts. Preserve item ordering, setup guidance and subsequent session-transition content handling. It does not authorize a new whole-history canonical snapshot. | One-time value transfer before the composer loop; then TUI-owned transcript presentation and existing active-run settlement. No new shared owner or lock. | None. Retain the initialization field independently of completion-result cleanup. | `ava_tests.app_runtime` (`tests/app_runtime_prompt_tests.cpp`); `ava_tests.tui_composer`; `ava_tests.tui_composer_rendering.changed_session`. Scenario: `main_startup_trust_keybinds`. |
| Submitted keybinding compatibility data | `src/ava/tui/runtime.h`: `TuiSubmitContext::key_bindings`; `src/ava/tui/keybindings.h`: `TuiKeyBindings`. `src/ava/tui/runtime_active_run_internal.cpp` captures the copy; `src/ava/app/tui/interactive_tui_support.cpp`: `command_hotkeys_from_key_bindings`. | Keep the immutable per-request copy used by backend command-help labels. The worker must not retain the mutable UI owner. Reloading local bindings changes later requests, not an already captured request. This is compatibility data, not backend-owned keybinding state. | Capture on the TUI thread before worker launch; value capture across the worker boundary. Local reload/application stays on the TUI thread, without a new mutex or `UnlockedData`. | None. Keybinding reload returns `Result<TuiKeyBindings>` and does not refresh backend catalogs. | `ava_tests.tui_composer` (`tests/tui_keybinding_tests.cpp`, `tests/tui_composer_rendering_tests.cpp`). Scenario: `main_startup_trust_keybinds`. |
| Mermaid configuration compatibility parameters | `src/ava/app/tui/tui_display_settings.h`: `MermaidDisplaySettings`. `src/ava/app/tui/mermaid_tui_bridge.h`: `MermaidTuiBridge`, `MermaidTuiPresentationConfiguration`. `src/ava/tui/runtime.h`: `TuiMermaidRenderBridge::config_epoch`/`enabled`, `TuiLocalDisplaySettings::mermaid_config_epoch`/`mermaid_enabled`. | Keep validated helper configuration and epoch/enabled parameters needed by the retained service. Effective reconfiguration advances the epoch and invalidates stale requests/completions. These parameters do not establish a canonical backend appearance/settings domain. Theme, cursor, layout and draft remain local. | Existing display owner applies service configuration; bridge mutex serializes configuration and coordinator access. TUI presentation values remain local. Do not add locks around local preferences. | None. These service parameters remain outside ordinary backend-state projection migration. | `ava_tests.mermaid_render`, `ava_tests.tui_composer`. Scenario: `mermaid`. |
| Pending-review combined catalog/todo transport | `src/ava/app/command_palette.h`: `ApplicationCatalogCoordinator`, `ApplicationCatalogDelivery`. `src/ava/tui/runtime.h`: `TuiCatalogDelivery` and initial catalog/generation fields. `src/ava/tui/TuiFrontEnd.h`: `read_catalog_delivery`, `read_committed_todos`. `src/ava/tui/composer.h`: `SlashCommandItem`, `FileReferenceItem`, `TodoItem`. `src/ava/app/tui/interactive_tui_support.cpp`: `todos_for_session`. | No approved canonical catalog/todo types are introduced. Keep each delivered vector with its generation, suppress replacement/recomposition for identical generations, and rehydrate committed todos at startup and every non-quit action/completion boundary. Live todo events still apply during a run. Keep title-catalog refresh cadence and trust-driven catalog refresh. Git-branch observation remains an app-side action/startup/completion operation, never a render-time filesystem read. | Application catalog mutex serializes values, discovery, Session reads and generations. Todo hydration uses the existing bound Session read authority. TUI pulls and immediately applies at serialized boundaries, never during rendering; committed-identity control is read separately. | This transport moves from action-result bundles to `read_catalog_delivery`/`read_committed_todos` pulls. It remains pending type review, not a renamed bundle for migrated agent/model/directory/token facts. `TuiCatalogDelivery` carries no action status or todos. A `quit` completion performs neither pull. | `ava_tests.app_runtime` (`tests/app_runtime_catalog_tests.cpp`); `ava_tests.tui_composer` (`tests/tui_completion_tests.cpp`, `tests/tui_composer_rendering_tests.cpp`). Scenarios: `main_slash_completions`, `main_startup_trust_keybinds`, `active_run`. |

### Retained dependencies

`src/ava/tui/runtime.h` still needs event, question,
subagent-launch, attachment and permission types for these interfaces. Their
presence does not permit ordinary backend facts to bypass projections.
Application bindings remain in
`src/ava/app/tui/interactive_tui.cpp` and its
supporting app-side adapters; backend permission decisions, Session stores,
leases, plugin invocation authority, agent instructions and assembled prompts do
not become TUI-owned or copied `UnlockedData` domains.

The common projections, their construction, typed `initialize_frontend_state`
and notification routing belong to `src/ava/frontend/FrontEnd.h` / `FrontEnd.cxx`.
The implementation accesses Session during registration and initialization;
projection headers forward-declare FrontEnd and include the small display/result
headers instead of their owning class. `SessionStats.h` includes the canonical
statistics header, not Session. TuiFrontEnd inherits this common behavior without
its own Session access. No replacement index-only binding API is required.

`TuiRuntimeOptions::read_committed_session_id` remains a narrow control read at
serialized action/completion boundaries, independent of consumed display dirty
flags. The application acquires/releases Session access inside the callback; the
TUI invokes it outside the UI lock before applying the committed-session
transition. Observed Git-branch text and a branch-summary confirmation operation
are different contracts. Neither a display refresh nor a local preview grants
permission to perform the latter.

## Guard

`tests/frontend_state_boundary_test.py` is the only source-text test for this
boundary. CTest registers `ava_tests.frontend_state_boundary` (`--source`) and
`ava_tests.frontend_state_boundary_self_test` (`--self-test`), each with a
15-second timeout and bytecode generation disabled. It scans C++ `.h`, `.hpp`,
`.cpp` and `.cxx` files, not historical documentation or its own policy literals.
It strips comments before checking declarations/identifiers.

| Rule | Exact scope and enforcement | One extension point |
| --- | --- | --- |
| Seam | Parse the bodies of `TuiRuntimeOptions` and `TuiSubmitResult` in `src/ava/tui/runtime.h`. Reject unrecognized declarations and non-function members outside the reviewed name sets. Callback return/parameter types cannot name a `*Snapshot` except `TuiBranchSummarySnapshot`, `StartupOverviewSnapshot`, `ProjectTrustSnapshot`; this also rejects composer/sidebar and renamed ordinary snapshot payloads. | `SEAM_FIELDS` for reviewed data fields; `RETAINED_CALLBACK_SNAPSHOTS` only for an explicitly approved retained protocol type. |
| Removed ordinary paths | Reject `TuiRuntimeStateSnapshot`, `state_snapshot`, `apply_runtime_state_snapshot`, `TuiKeyBindingReloadResult`, `refresh_token_status` in `src/ava/tui`, `src/ava/app`, `src/ava/frontend`, `tests`. In `src/ava/tui` additionally reject `refresh_session_statistics`, `compute_session_stats`, `estimate_active_context_tokens`, `read_authority`, `observe_git_branch`, `refresh_git_branch`. | `BANNED_IDENTIFIERS` and `TUI_BACKEND_IDENTIFIERS`, with their existing explicit directory scopes in `check_tree`. |
| Session seam | No file within `src/ava/tui` may use `session_ts`, `SCOPED_CRITICAL_AREA` or `app::runtime::Session`. Common projection initialization lives in `src/ava/frontend/FrontEnd.cxx`. | Any new access requires review of its actual ownership, not a directory exemption. |
| Reviewed canonical domains and lock-free appearance | Exactly the seven path/alias/value triples above under `src/ava/app/runtime` and `src/ava/session`; additional, missing, duplicate, changed-value and non-alias instantiations fail. No `UnlockedData<`, `std::mutex` or `std::shared_mutex` in `src/ava/app/tui/display_reload.*`, `tui_display_settings.*`, `src/ava/tui/theme.*`, `keybindings.*`. | `CANONICAL_WRAPPERS` for an individually approved domain; `UI_ONLY_STEMS` for another local-only owner. |
| No protocol wrappers | No `UnlockedData<` anywhere under `src/ava/tui` or `src/ava/app/tui`. | The two explicit prefixes in `check_tree`; do not exempt a deferred directory to add pending-request state. |

The self-test mutates a valid miniature tree and requires a negative case for
each rule, including every banned identifier and both UI directories. Positive
fixtures retain local composer/sidebar snapshots, the three popup/operation
snapshot types, `SubagentInspectorFrame`, the sanctioned Session seam, backend
statistics computation, and the Mermaid service mutex. The latter is in
`src/ava/app/tui/mermaid_tui_bridge.h`, outside the UI-only stems; permission,
question, plugin and active-run synchronization is likewise not subject to a
blanket mutex ban.

This is a scoped syntactic guard, not a C++ type/alias resolver or a proof that
arbitrary renamed payloads have correct ownership. Review new contracts by
effect and keep behavioral tests below. The separate unchanged
`tests/module_dependency_rules_test.py` enforces include direction with zero
backend exceptions; the plugin and session-read authority inventories remain
independent checks. Adding a domain requires its canonical-type approval,
writer/consumer inventory, behavior test and this document before changing an
allowlist.

## Matrix reconciliation

The following tables close audit 02 sections B/C/D and the reconciliation
matrix sections 4/5/8. Historical proposed removals are not current requirements:
the later decisions retain on-demand trust/overview/model/scope popups, the
other enumerated interactions, and pending-review catalog/todo transports.
“Migrated” means the old ordinary-state transfer is gone; “local” means U
ownership; “retained” is an explicit unchanged boundary, not an unfinished
canonical implementation.

### Behavioral evidence key

All paths below are repository paths. These names identify behavior assertions,
not source-shape assertions and not an unconditional claim that any particular
test invocation ran.

| Key | Tests or terminal scenarios and the behavior they exercise |
| --- | --- |
| Projection | `tests/tui_runtime_state_tests.cpp`: `test_frontend_data_changed_routes_each_canonical` checks initial delivery, isolated canonical writes and once-only display consumption. `tests/app_runtime_model_tests.cpp`: `test_unlocked_data_access_and_notifications` checks unlocked extension notification and registered assignment; `test_frontend_refresh_observes_concurrent_write` gates a canonical writer, checks base routing without extension callbacks, explicitly replays late dirtiness, then checks the next write/refresh. |
| Identity | `tests/tui_composer_rendering_tests.cpp`: `run_tui_composer_rendering_tests_part_4` checks initial/identical/replacement identity, prompt, directory and Git values, same-size context counts, distinct workspace/current paths in the sidebar drawer, and sanitized rendering. `tests/app_runtime_startup_tests.cpp`: `test_app_runtime_open_session_and_context_prompt`, `test_app_runtime_replacement_open_context`, `test_app_runtime_git_branch_observation_and_refresh` exercise backend path/context and observed-branch behavior. |
| Agent | `tests/tui_composer_rendering_tests.cpp`: `test_selected_agent_projection_and_rendering` checks canonical labels, independent metadata flags, no-op writes, replacement and detached initialization. `tests/app_runtime_prompt_tests.cpp`: `test_default_primary_is_canonical_builtin_ava_without_duplicate_prompt`, `test_app_run_prompt_isolates_ambient_extensions`, `test_primary_agent_tool_iteration_limit_is_applied_to_runtime_loop`, `test_project_primary_revocation_removes_authority_without_broadening_tools` check the actual next request's selected instructions/tools, default `none` and trust-driven authority changes. |
| Statistics | `tests/tui_selector_tests.cpp`: `run_tui_selector_tests` checks model/string/numeric flag ordering, compact versus exact totals and model-window-only percentage changes. `tests/app_runtime_startup_tests.cpp`: `test_app_token_status_tracks_compaction_projection` asserts one initial publication, one recomputation for batched mutations and no additional work on unchanged `/exit`; `test_app_statistics_invalidate_partial_append_recovery` and `test_app_statistics_reject_stale_work_across_session_replacement` check mutation/replacement safety. |
| Transition | `tests/tui_composer_rendering_tests.cpp`: `test_changed_session_snapshot_resets_presentation` checks full reset, unchanged identity, two replacements before one boundary and A-to-B-to-A non-reset; `test_active_run_session_transition_discards_prior_session_events` checks old content/event/queue/finish discard and zero catalog/todo pulls on quit. `tests/events_tests.cpp`: `test_interactive_tui_stops_follow_ups_at_session_transition`. |
| Catalog | `tests/tui_composer_rendering_tests.cpp`: `test_local_slash_completion_composition_tracks_current_ui_facts` checks paired delivery, repeated-generation no-op, new-generation empty replacement and local-hint recomposition. `test_reload_submit_routes_backend_targets_and_keeps_local_hot_reload` checks a trust-action completion reaches the palette. `tests/app_runtime_catalog_tests.cpp`: `test_application_catalog_coordinator_publishes_coherent_generation`, `test_application_catalog_cache_reuses_workspace_and_session_indexes`, `test_application_catalog_current_session_incremental_refresh`. |
| Local | `tests/tui_composer_rendering_tests.cpp`: `test_display_settings_reload_poll_outcome_and_preview_staging`, `test_display_settings_reload_rebuilds_open_startup_overview`, `test_local_overview_hint_refresh_is_idempotent_and_bounded`, `test_reload_submit_routes_backend_targets_and_keeps_local_hot_reload`; `tests/app_runtime_command_ui_tests.cpp`: `app_command_dispatcher_ui_part`. Scenarios `nested_settings_preview`, `main_editor_input`, `theme_env`, `theme_persisted` exercise persistence, previews and effective keybinding dispatch. |
| Popup | `tests/tui_selector_tests.cpp`: `run_tui_selector_tests` checks current/default/disabled model and reasoning semantics, branch/subagent views and controls. `tests/app_runtime_command_ui_tests.cpp`: `test_startup_overview_snapshot_bounds_order_redaction`, `test_startup_overview_bounded_lower_bound_counts`. Scenarios `main_models_selectors`, `main_session_mgmt`, `startup_overview`, `main_startup_trust_keybinds` exercise retained interactive flows. |
| Content | `tests/tui_command_output_tests.cpp`: `test_catalog_driven_local_command_transcript_invariant`, `test_unauthorized_command_event_tool_merge`, `test_buffered_runtime_event_completion_settlement`; `tests/tui_runtime_state_tests.cpp`: `test_tui_todowrite_event_state_and_snapshot_hydration`, `test_tui_mixed_runtime_event_queue_preserves_order_and_context`. Scenarios `active_run`, `restore_followup`, `streaming_scroll`. |
| History | `tests/app_runtime_session_command_tests.cpp`: `test_app_session_fork_from_entry_and_user_turns`, `test_app_session_branch_commands`, `test_app_session_new_resume_commands`, `test_app_session_metadata_commands`; scenario `main_session_mgmt`. These retain validated IDs, exact-text/history access and committed navigation, rather than asserting a new history subscription. |
| Initialization | `tests/tui_runtime_state_tests.cpp`: `test_runtime_initialization_preserves_local_and_retained_inputs` checks version, diagnostics, onboarding and canonical mode refresh for composer/sidebar. `tests/app_runtime_prompt_tests.cpp`: `test_app_first_run_auth_onboarding`. |
| Protocol | The per-interface regression anchors in **Retained interfaces**, including permission/question/plugin/subagent/branch/Mermaid and their terminal scenarios. Examples: `test_tui_plugin_ui_coordinator_ordering_limits_and_lifecycle` in `tests/plugin_ui_tests.cpp`, `test_explicit_confirmation_success_and_existing` and `test_shutdown_is_safe_in_every_nonterminal_phase` in `tests/branch_summary_coordinator_tests.cpp`, `test_tui_bridge_epoch_reload_and_callback_lifetime` in `tests/mermaid_render_coordinator_tests.cpp`. |

The gated refresh test proves commit-before-notification ordering, a consumed
no-op notification and a subsequent write observed on the next refresh. It
does not instrument or claim a pause inside the concrete value-copy expression.
The routing test observes public projection changes, not private dirty bits.

### Ordinary inputs: audit 02 B and matrix 4

| Historical fields | Final disposition | Evidence |
| --- | --- | --- |
| `mode`, `session_id`, `session_path`, `workspace` | Migrated B: prompt, identity and distinct directory projections; control identity remains A. | Identity, Transition, Initialization |
| `git_branch` | Migrated B: backend-observed string, no render-time observation. | Identity, Projection |
| `context_source_count` in options, old bundle and submit result | Migrated B: nested prompt metadata vector size, including known zero. | Identity, Projection |
| `initial_status` and action `status` | Local U startup feedback and migrated A action strings; no shared status domain. | Initialization, Local, Transition; `main_models_selectors` |
| `app_version` | Retained I. | Initialization |
| `initial_transcript` | Retained D onboarding/content. | Initialization, Content |
| `slash_commands`, `slash_catalog_generation` | Retained B pending-review delivery plus local U completion hints; no canonical catalog type introduced. | Catalog; `main_slash_completions` |
| `file_references`, `workspace_catalog_generation` | Retained B pending-review delivery; generation distinguishes omitted unchanged from genuinely empty. | Catalog |
| `custom_themes` | Local U discovery, validation, previews and labels. | Local |
| `project_trust` | Retained D on-demand filtered popup data; backend trust authority stays unwrapped. Historical removal proposal superseded. | Popup, Local, Agent |
| `todos` | Retained B history hydration and D live event delivery; no invented todo wrapper. | Content, Transition |
| `show_images`, `image_width_cells`, `cursor` | Local U effective display state. | Local; `main_paste_scrollback_attach` |
| `mermaid_config_epoch`, `mermaid_enabled`, bridge `config_epoch`/`enabled` | Local U preferences and retained D worker-correlation parameters. | Local, Protocol; `mermaid` |
| `startup_overview` | Retained D filtered on-demand input with local U presentation/hints; no eager resource domain. | Popup, Local |
| bridge `enqueue`, `cancel`, `drain` | Retained D service operations. | Protocol |
| `key_bindings` | Local U owner/reload; retained immutable request copy for help compatibility. | Local |

### Callbacks: audit 02 C and matrix 5

This also records callbacks introduced by the cutover. Exact field names and
result contracts are enumerated in **Seam contract** above.

| Callback rows | Final disposition | Evidence |
| --- | --- | --- |
| Former statistics-refresh callback | Migrated B backend publication and frontend observation; callback removed. | Statistics |
| `create_active_run_queues`; `on_submit` | Retained D/A controls and content; ordinary state/count members removed. | Content, Transition |
| `on_attach_image`; `on_paste_clipboard_image`; `on_load_image_attachment`; `on_external_editor` | Retained D validated services; draft remains local U. | Protocol; `main_editor_input`, `main_paste_scrollback_attach` |
| `on_toggle_mode`; `on_cycle_reasoning`; `on_cycle_model` | Migrated A success/error/feedback plus B projections. | Initialization, Statistics; `main_models_selectors` |
| `on_reload_key_bindings`; `on_reload_display_settings`; `on_maybe_reload_display_settings`; `on_local_display_settings_action` | Local U, including explicit applied/unchanged/error semantics. | Local |
| `model_selector_view`; `scoped_model_selector_view`; `reasoning_selector_view`; `session_selector_view` | Retained D interactive lists; committed facts still use B projections. | Popup, Catalog |
| `list_subagents`; `inspect_subagent`; `cancel_subagent`; `promote_subagent` | Retained D ownership/status/control/content protocol, not active-agent authority. | Protocol, Popup; `subagent_workspace` |
| `on_open_fork_user_turn_selector`; `on_open_copy_user_turn_selector`; `on_read_user_turn_text` | Retained D authorized historical retrieval. | History |
| Session-selector sort, named-filter, path-display, archived-filter and label-timestamp callbacks | Local U controls in the retained popup path; no replacement canonical catalog approved. | Popup; `main_session_mgmt` |
| `on_session_selector_archive`; `on_session_selector_unarchive` | Retained A/D persistence action and popup result. | History, Popup; `main_session_mgmt` |
| `on_session_selector_branch_parent`; `on_session_selector_branch_child`; `on_session_selected` | Migrated A feedback and committed transition; D selection remains. | Transition, History |
| `on_branch_summary_prepare`; `branch_summary_snapshot`; `on_branch_summary_confirm`; `on_branch_summary_cancel`; `on_branch_summary_refresh_catalog` | Retained D generation-bound exchange, including refresh request. | Protocol, Popup; `branch_summary` |
| `remember_permission_rule` | Retained D permission authority/result. | Protocol; `main_permission_flow` |
| `on_settings_selected` | Migrated A backend trust actions; appearance/binding actions use local U path. | Local, Catalog; `nested_settings_preview` |
| `on_model_selected`; `on_reasoning_selected` | Migrated A feedback and B canonical model/reasoning. | Statistics, Popup; `main_models_selectors` |
| `on_fork_user_turn_selected` | Migrated A feedback and committed identity; retained D picker and post-commit warning handling. | History, Transition; `main_session_mgmt` |
| Scoped-model toggle, enable-all, clear-all, provider-toggle, reorder and save callbacks | Retained D popup/persistence, including prior-view arguments; no scope projection. | Popup; `main_models_selectors` |
| `on_before_tui_shutdown` | Retained D lifecycle barrier. | Protocol and complete terminal shutdown wave |
| `read_committed_session_id` | Retained A control independent of display flags. | Transition |
| `read_catalog_delivery`; `read_committed_todos` | Retained pending-review B pulls, immediately applied except on quit. | Catalog, Transition |
| `read_startup_overview`; `read_project_trust` | Retained D on-demand popup reads. | Local, Popup |

### Nested results: audit 02 D and matrix 5

| Historical member or nested interface | Final disposition | Evidence |
| --- | --- | --- |
| Submit `quit` | Retained A; no catalog/todo pull on quit. | Transition, Statistics |
| Submit `ordinary_turn_committed`, `ordinary_turn_request_ids` | Retained D/A attribution; absent IDs do not authorize conversation events. | Content |
| Submit `output`, `tool_timeline` | Retained D fallback/streamed content. | Content |
| `local_command_result.output`, `.tool_timeline` | Retained D local-only content, not conversation authority. | Content |
| `conversation_output`, `conversation_tool_timeline` | Retained D separately attributed fallback. | Content |
| Submit context count and bulk-state member | Migrated B count; obsolete bundle removed, retained domains pulled separately. | Identity, Transition, Catalog, Guard |
| Old keybinding reload wrapper and its broad state member | Local U binding reload, no backend publication. | Local, Guard |
| `TuiActiveRunQueues`: `active_request_id`, queue/take/skip/mark/restore/nonblocking-command/finish operations; queued/restored message records | Retained D exact queue order and session/request binding. | Content; `test_interactive_run_queue_runs_follow_up_lifecycle`, `test_interactive_run_queue_restores_latest_pending_message` in `tests/events_tests.cpp` |
| `TuiSubmitContext`: request identity, permission/question resolvers, event sink, cancellation, steering/follow-up operations, launch sink, plugin endpoint, attachments | Retained D request/exchange/content authority. | Content, Protocol |
| `TuiSubmitContext::key_bindings` | Retained immutable U compatibility data captured before worker launch. | Local |
| Mermaid bridge/request/completion identity, source/text, accepted and epoch | Retained D bounded service; local U preference owner. | Protocol |
| `TuiBranchSummarySnapshot`: generation, phase, labels, optional eligibility/failure/append codes, `refresh_required` | Retained D operation state, explicitly not an ordinary-state replacement. | Protocol, Popup |

### Acceptance areas: matrix 8

| Area | Closure evidence and limits |
| --- | --- |
| Reference projection contract | Projection, Identity and Statistics cover initialization, no-op writes, independent pending fields, controlled notification ordering and stable registered assignment/replacement. No multi-subscriber semantics are asserted. |
| Effective agent | Agent covers canonical default/selected/trust state, rendered label and the next provider request's instructions/tool behavior. No agent-switching product feature or model override is invented. |
| Directories | Identity covers distinct root/current values, startup/replacement and sidebar-drawer rendering, with backend path authority unchanged. |
| Local state | Local covers reload/persistence/preview behavior; Guard restricts lock/wrapper ownership in local-only files. Existing cross-thread services remain exceptions. |
| Session transitions | Transition and History cover shared reset, same identity, coalesced replacements, stale completion/follow-up discard and retained navigation. Intermediate unobserved identities do not acquire a new generation. |
| Catalogs | Catalog and Popup cover paired generation application, no-op/empty semantics, cache work avoidance, validated choices and local hints. Canonical catalog/todo types remain pending review; history errors still collapse to the existing empty todo result. |
| Statistics | Statistics covers separate estimates, compact/exact changes, model-window dependency, compaction, stale work rejection and bounded `/exit` publication. Rendering has no statistics-computation callback. |
| Deferred interfaces | Protocol and the retained-interface table preserve exact authority, content and cancellation, verified by focused suites and opt-in terminal scenarios. |
| Boundary | Guard checks the scoped syntactic invariants and unchanged module/authority guards enforce their own contracts. Behavioral assertions, not a blanket `Snapshot` ban, establish equivalence. |
