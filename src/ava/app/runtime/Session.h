#pragma once

#include "BasePromptMetadata.h"
#include "ContextSourceMetadata.h"
#include "FreshnessSourceMetadata.h"
#include "OpenContext.h"
#include "PromptOverrides.h"
#include "ReasoningSelection.h"
#include "SessionLifecycleRequest.h"
#include "ava/debug/print_members_on.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime/session_ts.h"
#include "ava/app/session_run_controller.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/subagent_config.h"
#include "ava/mcp/config.h"
#include "ava/config/model_config.h"
#include "ava/config/xdg_paths.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission_rules.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/error.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ava::diagnostics {
class RuntimeDiagnostics;
} // namespace ava::diagnostics

namespace ava::provider {
class ProviderCatalog;
} // namespace ava::provider

namespace ava::app {
class SessionTitleCoordinator;
class SubagentDeliveryManager;
} // namespace ava::app

namespace ava::app::runtime {

struct PromptState;

// Invocation inputs.
//
// Mirrors of OpenContext describing how this session was opened. None of
// these are restored from the store on resume; each is sourced from the
// OpenContext of the current process invocation.
//
struct InvocationInputs
{
  std::filesystem::path workspace_dir;
  std::filesystem::path current_dir;
  // Invocation-requested visibility is retained separately so a replacement can re-resolve primary policy from the original CLI boundary.
  ava::agent::ToolVisibilityOptions requested_tool_visibility = {};
  ava::agent::ToolVisibilityOptions tool_visibility = {};
  // Keep bounded selection intent separately from the currently permitted, provenance-carrying definition.
  std::optional<std::string> requested_primary_agent = std::nullopt;
  std::optional<ava::agent::SubagentDefinition> selected_primary_agent = std::nullopt;
  ava::config::XdgPaths paths;
  bool sessionless;
  bool is_offline_ = false;
  // Additional writable directories beyond workspace_dir (e.g., user-configured paths).
  // These are opened as anchor descriptors at startup and made available to tools via ToolContext::anchor_set.
  std::vector<std::filesystem::path> additional_writable_dirs = {};
  // Resolved once at open time and reused by every runtime history reader.
  ava::session::SessionReadLimits session_read_limits = ava::session::legacy_unbounded_session_read_limits();
  PromptOverrides prompt_overrides = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Resolved prompt state.
//
// The agent mode and assembled prompt material currently in effect for the
// session: base-prompt metadata, contributing context and freshness sources,
// the ordinary system prompt, and its ambient-extension-free runtime variant.
// This whole bundle is recomputed
// whenever the mode or model changes (see apply_prompt_state and
// switch_model) and is otherwise the session's live resolved state.
//
// It is field-compatible with the transient PromptState returned by
// load_runtime_prompt_state, which apply_prompt_state moves into this
// member as a single object rather than copying fields one at a time.
struct ResolvedPromptState
{
  ava::agent::Mode mode = ava::agent::Mode::Build;
  BasePromptMetadata base_prompt;
  std::vector<ContextSourceMetadata> context_sources;
  std::vector<FreshnessSourceMetadata> freshness_sources;
  std::string system_prompt;
  std::string ambient_extension_free_system_prompt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Model selection.
//
// The active model and its derived/user-selected companions: the resolved
// ModelInfo, the latest reasoning selection, and the optional scoped model
// rotation. `model` and `reasoning` are re-derived on resume (model from the
// persisted entries, reasoning from the last assistant turn); all three are
// runtime-mutable: model/reasoning change on /models, scoped_model_cycle on
// /models scope commands.
struct ModelSelection
{
  ava::config::ModelInfo model;
  std::optional<ReasoningSelection> reasoning = std::nullopt;
  std::optional<std::vector<std::string>> scoped_model_cycle = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Trust state.
//
// The project trust decision loaded for this session. Mutated when the user
// adjusts trust (/trust commands); otherwise stable for the session lifetime.
struct TrustState
{
  ProjectTrustState project_trust;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Session resources.
//
// Process-held live infrastructure bound to this process: the cross-process
// lease, pre-opened anchor descriptors, the run controller and append target,
// the bound read authority for detached capsules, and the shared
// application-scoped services (subagent coordinator/delivery, title
// coordinator, diagnostics, MCP config). These cannot round-trip through
// disk; the three app-scoped services are rebound by Session::replace_with
// and mcp_config is set once at ACP session setup.
struct SessionResources
{
  // Persistent runtime owners hold a cross-process lease for the complete session lifetime.
  ava::session::SessionLease lease;
  // One generated session owner, when explicit application process authority
  // was supplied. Copies representing this same runtime session share it.
  std::optional<ava::process::ProcessScopeV1> session_process_scope = std::nullopt;
  // Pre-opened anchor descriptors for all writable directories. Opened once
  // at session creation and shared across all prompts and subagent loops.
  std::shared_ptr<ava::core::AnchorSet> anchor_set = nullptr;
  std::shared_ptr<SessionRunController> run_controller = nullptr;
  // Immutable append target bound into the controller-owned serialized routes.
  // Tests and retained runtime capsules may inspect/copy it, but mutations flow
  // through the controller so append failures remain latched.
  std::shared_ptr<ava::session::SessionAppendTarget> append_target = nullptr;
  // Detached retained capsules bind reads to the exact original leased inode
  // without reacquiring the pathname. Ordinary visible sessions leave this
  // empty and derive an authority from their owned lease.
  std::optional<ava::session::SessionReadAuthority> bound_read_authority = std::nullopt;
  // Shared application process state. Direct coordinator injection remains a
  // compatibility seam; production sessions use the delivery manager.
  std::shared_ptr<ava::agent::SubagentCoordinator> subagent_coordinator = nullptr;
  std::shared_ptr<ava::app::SubagentDeliveryManager> subagent_delivery_manager = nullptr;
  std::shared_ptr<ava::app::SessionTitleCoordinator> session_title_coordinator = nullptr;
  std::shared_ptr<ava::diagnostics::RuntimeDiagnostics> diagnostics = nullptr;
  // Null uses normal global/project discovery; non-null is immutable session-local MCP composition.
  std::shared_ptr<ava::mcp::McpConfig const> mcp_config = nullptr;
  // Exact application-scoped provider catalog pinned for this session's lifetime.
  std::shared_ptr<ava::provider::ProviderCatalog const> provider_catalog = nullptr;

  void swap(SessionResources& other);

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Old aggregate for Session.
// This class holds members that are still initialized by a designated initializer list.
//
// Members are grouped into cohorts by where their values come from and how long
// they live. The grouping is documentation-only today, but it marks the seams
// along which this aggregate is expected to be decomposed into smaller types.
struct Session_aggregate_base
{
  InvocationInputs invocation_inputs_;          // Do NOT use this variable name outside of this file!
  ResolvedPromptState resolved_prompt_state_;   // Do NOT use this variable name outside of this file!
  ModelSelection model_selection_;              // Do NOT use this variable name outside of this file!
  TrustState trust_state_;                      // Do NOT use this variable name outside of this file!
  SessionResources resources_;                  // Do NOT use this variable name outside of this file!

  // ===========================================================================
  // Persistent session identity.
  // `store` is the session's on-disk identity (a durable content handle);
  // `created` records whether this Session was freshly created vs resumed.
  // Resolved model/reasoning live in `model_selection_` above and are
  // projected back out of the persisted entries on resume.
  // ===========================================================================
  ava::session::SessionStore store;
  bool created = false;                         // True when this is a freshly created session rather than a resumed one.

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Hold the mutable application state associated with an open runtime session.
//
// The store may be persistent or ephemeral according to sessionless. Shared background jobs remain valid when the aggregate is moved or replaced.
class Session : protected Session_aggregate_base
{
 public:
  using Session_aggregate_base::created;
  using Session_aggregate_base::store;

 private:
  template <typename, typename>
  friend class threadsafe::Unlocked;

  // Move constructors.
  Session(Session&& session) = default; // `Session&& session` is allowed here (private move-constructor).
  Session(Session_aggregate_base&& base) : Session_aggregate_base(std::move(base)) { }

  // Called from replace_with.
  Session& operator=(Session&& session) = default; // `Session&& session` is allowed here (private move-assignment operator).

  static ava::core::Result<session_ts> construct(OpenContext const& context, SessionLifecycleRequest const& request, ava::session::SessionStore& store,
                                                 ava::session::SessionLease& lease, bool created, bool load_existing_entries, bool append_session_start,
                                                 bool append_initial_session_name, std::shared_ptr<ava::app::SubagentDeliveryManager> delivery_manager,
                                                 std::shared_ptr<ava::app::SessionTitleCoordinator> title_coordinator);

#ifdef CWDEBUG
 public:
  ~Session()
  {
    // This destructs a SubagentDeliveryManager which can join a thread as a result.
    AVA_ASSERT_NO_SESSION_LOCK_HELD("destructing a Session");
  }
#endif

 public:
  // Open a runtime session using stable `context` and the one-shot lifecycle
  // `request`.
  //
  // An empty request creates a persistent session. Returns failure when selectors
  // conflict or the selected session cannot be created, recovered, or opened.
  static ava::core::Result<session_ts> open(OpenContext const& context, SessionLifecycleRequest const& request = {});

  // Open a session using `request` at `workspace_root` and `current_dir`, overriding those locations in `context`.
  static ava::core::Result<session_ts> open_at(OpenContext context, std::filesystem::path const& workspace_root, std::filesystem::path const& current_dir,
                                               SessionLifecycleRequest request = {});

  // Convenience wrapper to create new persistent session at `workspace_root` and `current_dir`, overriding those locations in `context`.
  static ava::core::Result<session_ts> create_at(OpenContext context, std::filesystem::path const& workspace_root, std::filesystem::path const& current_dir)
  {
    return open_at(std::move(context), workspace_root, current_dir);
  }

  // Open a session using `request`, inheriting active state from `current` and frontend-only policy from `base_context`.
  static ava::core::Result<session_ts> open_like(session_ts const& unlocked_session, OpenContext const& base_context, SessionLifecycleRequest request = {});

  // Convenience wrapper to create a persistent session inheriting active state from `current` and frontend-only policy from `base_context`.
  static ava::core::Result<session_ts> create_like(session_ts const& unlocked_session, OpenContext const& base_context)
  {
    return open_like(unlocked_session, base_context);
  }

  // Consume an already-owned persistent store and its lease without reopening by path.
  // Inputs remain intact on failure so callers can roll back by stable identity.
  static ava::core::Result<session_ts> open_owned(OpenContext const& context, ava::session::SessionStore& store, ava::session::SessionLease& lease,
                                                  bool created);

  static ava::core::Result<session_ts> open_requested(session_ts const& unlocked_session, OpenContext const& base_context,
                                                      std::string_view requested_session_id)
  {
    SessionLifecycleRequest request{.requested_session_id = std::string(requested_session_id)};
    return open_like(unlocked_session, base_context, std::move(request));
  }

  // Snapshot detached-session construction state using lease, authority, and manager while inheriting this session's active state and shared resources.
  //
  // The returned aggregate contains no Session object. Callers may therefore move it into the final session_ts after releasing this session's lock
  // without destructing a temporary Session at the boundary.
  Session_aggregate_base create_detached_state(ava::session::SessionLease lease, ava::session::SessionReadAuthority authority,
                                               std::shared_ptr<ava::app::SubagentDeliveryManager> manager) const;

  // Return the AVA-owned filesystem roots that command planning and model
  // ToolContexts are pre-authorized to access.
  //
  // This is the single bounded source for the `ava_authority_roots` field: AVA's
  // config, state, and sessions directories, the active auth file and its legacy
  // credential fallbacks, plus this session's store parent as a fallback for
  // custom/test path layouts. The list is short, stable, and deduplicated; keep
  // it in sync across direct command planning and ToolContext construction.
  std::vector<std::filesystem::path> ava_authority_roots_1() const;

  // Accessors.

  // Base class accessors.
  //
  // The non-const overload is used to replace the whole bundle on mode/model changes;
  // the const overload lets callers copy it out of a const Session without naming the member directly.

  InvocationInputs& invocation_inputs() { return invocation_inputs_; }
  InvocationInputs const& invocation_inputs() const { return invocation_inputs_; }

  ResolvedPromptState& resolve_prompt_state() { return resolved_prompt_state_; }
  ResolvedPromptState const& resolve_prompt_state() const { return resolved_prompt_state_; }

  ModelSelection& model_selection() { return model_selection_; }
  ModelSelection const& model_selection() const { return model_selection_; }

  TrustState& trust_state() { return trust_state_; }
  TrustState const& trust_state() const { return trust_state_; }

  SessionResources& resources() { return resources_; }
  SessionResources const& resources() const { return resources_; }

  // Invocation inputs.
  std::filesystem::path const& workspace_dir() const { return invocation_inputs_.workspace_dir; }
  std::filesystem::path const& current_dir() const noexcept { return invocation_inputs_.current_dir; }
  ava::agent::ToolVisibilityOptions const& tool_visibility() const { return invocation_inputs_.tool_visibility; }
  std::optional<std::string> const& requested_primary_agent() const { return invocation_inputs_.requested_primary_agent; }
  std::optional<ava::agent::SubagentDefinition> const& selected_primary_agent() const { return invocation_inputs_.selected_primary_agent; }
  ava::config::XdgPaths const& paths() const { return invocation_inputs_.paths; }
  bool sessionless() const { return invocation_inputs_.sessionless; }

  // Resolved prompt state.
  ava::agent::Mode mode() const { return resolved_prompt_state_.mode; }
  BasePromptMetadata const& base_prompt() const { return resolved_prompt_state_.base_prompt; }
  std::vector<ContextSourceMetadata> const& context_sources() const { return resolved_prompt_state_.context_sources; }
  std::vector<FreshnessSourceMetadata> const& freshness_sources() const { return resolved_prompt_state_.freshness_sources; }
  std::string const& system_prompt() const { return resolved_prompt_state_.system_prompt; }
  std::string const& ambient_extension_free_system_prompt() const { return resolved_prompt_state_.ambient_extension_free_system_prompt; }
  bool is_offline() const { return invocation_inputs_.is_offline_; }
  std::vector<std::filesystem::path> const& additional_writable_dirs() const { return invocation_inputs_.additional_writable_dirs; }
  // Resolved once at open time and reused by every runtime history reader.
  ava::session::SessionReadLimits const& session_read_limits() const { return invocation_inputs_.session_read_limits; }
  PromptOverrides const& prompt_overrides() const { return invocation_inputs_.prompt_overrides; }

  // Model selection.
  ava::config::ModelInfo const& model() const { return model_selection_.model; }
  std::optional<ReasoningSelection> const& reasoning() const { return model_selection_.reasoning; }
  std::optional<std::vector<std::string>> const& scoped_model_cycle() const { return model_selection_.scoped_model_cycle; }

  // Trust state.
  ProjectTrustState const& project_trust() const { return trust_state_.project_trust; }

  // Session resources.
  ava::session::SessionLease const& lease() const { return resources_.lease; }
  std::optional<ava::process::ProcessScopeV1> const& session_process_scope() const { return resources_.session_process_scope; }
  std::shared_ptr<ava::core::AnchorSet> const& anchor_set() const { return resources_.anchor_set; }
  std::shared_ptr<SessionRunController> const& run_controller() const { return resources_.run_controller; }
  std::shared_ptr<ava::session::SessionAppendTarget> const& append_target() const { return resources_.append_target; }
  std::optional<ava::session::SessionReadAuthority> const& bound_read_authority() const { return resources_.bound_read_authority; }
  std::shared_ptr<ava::agent::SubagentCoordinator> const& subagent_coordinator() const { return resources_.subagent_coordinator; }
  std::shared_ptr<ava::app::SubagentDeliveryManager> const& subagent_delivery_manager() const { return resources_.subagent_delivery_manager; }
  std::shared_ptr<ava::app::SessionTitleCoordinator> const& session_title_coordinator() const { return resources_.session_title_coordinator; }
  std::shared_ptr<ava::diagnostics::RuntimeDiagnostics> const& diagnostics() const { return resources_.diagnostics; }
  std::shared_ptr<ava::mcp::McpConfig const> const& mcp_config() const { return resources_.mcp_config; }
  std::shared_ptr<ava::provider::ProviderCatalog const> const& provider_catalog() const { return resources_.provider_catalog; }

  // Convenience accessor that returns ProviderCatalog::build_builtins_only() if no provider_catalog is set.
  std::shared_ptr<ava::provider::ProviderCatalog const> ensure_provider_catalog() const {
    return resources_.provider_catalog ? resources_.provider_catalog : ava::provider::ProviderCatalog::build_builtins_only();
  }

  // Bind a lifetime-safe history snapshot route to this session's exact lease
  // (or to its shared in-memory state in sessionless mode).
  [[nodiscard]] ava::core::Result<ava::session::SessionReadAuthority> read_authority_1() const
  {
    if (resources_.bound_read_authority)
      return *resources_.bound_read_authority;
    return invocation_inputs_.sessionless
               ? ava::session::SessionReadAuthority::create_ephemeral(store, invocation_inputs_.session_read_limits)
               : ava::session::SessionReadAuthority::create_persistent(store, resources_.lease, invocation_inputs_.session_read_limits);
  }

  // Append through the session owner so writes remain serialized with active runs.
  [[nodiscard]] ava::core::VoidResult append_owned(ava::session::SessionEntry entry)
  {
    if (!resources_.run_controller)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "runtime session controller is unavailable"));
    return resources_.run_controller->append(std::move(entry));
  }

  // Return the stable append route owned by this session, or an empty route when the controller is unavailable.
  [[nodiscard]] ava::agent::SessionAppendSink owner_append_route_1() const
  {
    return resources_.run_controller ? resources_.run_controller->owner_append_route() : ava::agent::SessionAppendSink{};
  }

  [[nodiscard]] ava::agent::SessionAppendBatchSink owner_append_batch_route_1()
  {
    return resources_.run_controller ? resources_.run_controller->owner_append_batch_route() : ava::agent::SessionAppendBatchSink{};
  }

  // Single app-owned source for the persistent permission-rule store bound to a runtime session.
  // Every command permission path (direct app commands, model tool calls, ACP hosts, RPC, print, TUI)
  // resolves the same global and workspace rule files through this helper instead of duplicating path logic.
  [[nodiscard]] ava::permissions::PermissionRuleStore permission_rule_store() const
  {
    return {.global_rules_file = paths().ava_config_dir / "permission-rules.json",
            .workspace_rules_file = workspace_dir() / ".ava" / "permission-rules.json",
            .workspace_dir = workspace_dir(),
            .anchor_set = anchor_set()};
  }

  // Publish callback-free mutable runtime configuration from unlocked_session into an existing retained parent capsule.
  //
  // The wrapper must be unlocked. Returns failure when the parent delivery manager rejects the update; returns success when no manager is attached or
  // the update was applied. No session lock remains held while the manager replaces its capsule.
  [[nodiscard]] static ava::core::VoidResult refresh_parent_configuration(session_ts const& unlocked_session);

  // Replace unlocked_current with unlocked_replacement, rebinding application-scoped services that cannot round-trip through disk.
  //
  // Both wrappers must be distinct and unlocked. This method owns both write-lock scopes and releases them before notifying the delivery manager that
  // the previously visible parent detached; that notification may destroy a retained session or otherwise block. Returns an invalid-argument error
  // only when both parameters identify the same wrapper.
  [[nodiscard]] static ava::core::VoidResult replace_with(session_ts& unlocked_current, session_ts& unlocked_replacement);

  // Recover torn-tail and incomplete-assistant-output suffix entries from the
  // session identified by `source_session_id` so a follow-up mutation
  // (fork/clone/branch summary) reads a clean source.
  //
  // When `source_session_id` names this session, recovery runs against the
  // current store and lease. Otherwise a separate SessionStore is opened and the
  // acquired lease is emplaced into `temporary_source_lease`, which the caller
  // must keep alive for the duration of the mutation. Returns failure when the
  // source store cannot be opened, the lease cannot be acquired, or a recovery
  // stage rejects. Leaves `temporary_source_lease` untouched when recovering this
  // session.
  [[nodiscard]] ava::core::VoidResult recover_source_for_mutation(std::string const& source_session_id,
                                                                  std::optional<ava::session::SessionLease>& temporary_source_lease);

  // Build replacement OpenContext from this session and `base_context`.
  //
  // Runtime context, filesystem policy, resolved read limits, and shared
  // application services are inherited from this session. A stored process
  // scope is reduced to its application root so the replacement derives one
  // fresh session owner. Frontend policy in `base_context`, including model
  // pinning and exact-ID behavior, is retained.
  // An ephemeral session's AnchorSet is not inherited because its temporary
  // spill root cannot authorize a new persistent or ephemeral store.
  [[nodiscard]] OpenContext replacement_open_context(OpenContext const& base_context) const;

  // Append session metadata through the runtime owner's serialized route.
  [[nodiscard]] ava::core::Result<ava::session::SessionMetadataView> append_metadata_1(ava::session::SessionMetadataUpdate update);

  // Append a mode change entry through the runtime owner's serialized route.
  //
  // Records an EntryType::ModeChange entry carrying the given `mode` so the
  // transition survives session resume. Returns failure when the append route
  // rejects the entry (for example, a latched controller).
  [[nodiscard]] ava::core::VoidResult append_mode_change_1(ava::agent::Mode mode);

  // Apply a CLI-supplied initial reasoning level to a freshly opened session.
  //
  // Accepts the raw --thinking level token from SessionLifecycleRequest; an empty or
  // whitespace-only value is rejected. "off" clears the active selection;
  // any other token must resolve to a level the active model supports.
  // Returns failure when the level is empty, unsupported, or the underlying
  // reasoning update rejects the resolved selection.
  [[nodiscard]] ava::core::VoidResult apply_initial_reasoning_level(std::string_view requested_level);

  // Move a freshly assembled PromptState into this already write-locked session's resolved state without refreshing retained parents.
  //
  // `prompt_state` is consumed. This locked mutation primitive always succeeds.
  [[nodiscard]] ava::core::VoidResult apply_prompt_state(PromptState prompt_state);

  // Apply prompt_state to unlocked_session, release its write lock, then synchronously refresh retained parent configuration.
  [[nodiscard]] static ava::core::VoidResult apply_prompt_state_and_refresh(session_ts& unlocked_session, PromptState prompt_state);

  // Atomically publish a trust transition with its permitted primary definition, sticky effective tool visibility, and reconstructed prompt state, then
  // refresh retained parent configuration after releasing the session lock.
  [[nodiscard]] static ava::core::VoidResult apply_trust_prompt_state_and_refresh(session_ts& unlocked_session, ProjectTrustState project_trust,
                                                                                  std::optional<ava::agent::SubagentDefinition> selected_primary_agent,
                                                                                  PromptState prompt_state);

  // Switch the active model to `model`, re-deriving the prompt state for the
  // current mode and clearing any active reasoning selection.
  //
  // Returns false (without writing) when `model` already matches the active
  // provider/model pair. Returns failure when prompt-state loading, the model
  // change append rejects the switch. This locked mutation primitive does not refresh retained parents.
  [[nodiscard]] ava::core::Result<bool> switch_model(ava::config::ModelInfo model);

  // Switch unlocked_session's model, release its write lock, then refresh retained parent configuration when the model changed.
  [[nodiscard]] static ava::core::Result<bool> switch_model_and_refresh(session_ts& unlocked_session, ava::config::ModelInfo model);

  // Set the active reasoning selection, resolving it against the current model.
  //
  // A nullopt `selection` clears reasoning. Non-nullopt selections are trimmed
  // and resolved through the model's supported levels. Returns false (without
  // writing) when the resolved selection equals the active one. Returns failure
  // when the model rejects the level or append rejects the change. This locked mutation primitive does not refresh retained parents.
  [[nodiscard]] ava::core::Result<bool> set_reasoning(std::optional<ReasoningSelection> selection);

  // Set unlocked_session's reasoning, release its write lock, then refresh retained parent configuration when the selection changed.
  [[nodiscard]] static ava::core::Result<bool> set_reasoning_and_refresh(session_ts& unlocked_session, std::optional<ReasoningSelection> selection);

  [[nodiscard]] ava::core::Result<ava::session::SessionMetadataView> load_metadata() const;

  [[nodiscard]] std::string state_result_json_1(bool cancel_requested) const;
  [[nodiscard]] ava::core::Result<std::string> messages_result_json() const;
  [[nodiscard]] ava::core::Result<std::string> list_sessions_result_json_1() const;
  [[nodiscard]] ava::core::Result<std::string> tree_result_json() const;
  [[nodiscard]] ava::core::Result<std::string> list_models_result_json_1() const;
  [[nodiscard]] ava::core::Result<std::string> stats_result_json() const;
  [[nodiscard]] ava::core::Result<std::string> validation_result_json() const;

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(Session_aggregate_base)
};

}  // namespace ava::app::runtime
