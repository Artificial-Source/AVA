#include "sys.h"
#include "Session.h"
#include "ava/diagnostics/runtime_diagnostics.h"
#include "ava/app/project_trust.h"
#include "ava/app/reasoning_controls.h"
#include "ava/app/rpc/serialization_detail.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime_json.h"
#include "ava/app/runtime_model.h"
#include "ava/app/runtime_prompt.h"
#include "ava/app/runtime_reasoning.h"
#include "ava/app/session_title_coordinator.h"
#include "ava/app/subagent_delivery_manager.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/config/session_title_config.h"
#include "ava/session/session_branch.h"
#include "ava/session/session_metadata.h"
#include "ava/provider/catalog.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/ids.h"
#include "ava/core/string_utils.h"
#include "ava/core/trusted_home.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "debug.h"

namespace ava::app::runtime {
namespace {

constexpr std::size_t kMaxCommandAuthorityRoots = 8;

// Comma-separated list of reasoning levels acceptable on the CLI for `model`,
// always including "off" first; non-empty, non-"off" supported levels follow.
std::string cli_supported_reasoning_levels(ava::config::ModelInfo const& model)
{
  std::string levels = "off";
  for (auto const& level : ava::config::supported_reasoning_levels(model))
  {
    if (level.empty() || level == "off")
      continue;
    levels += ", ";
    levels += level;
  }
  return levels;
}

// Attach the --thinking option name and the model's supported levels to `error`
// so CLI failures point the user at the levels they may pass.
void add_cli_reasoning_context(ava::core::Error& error, ava::config::ModelInfo const& model)
{
  error.with_context("option", "--thinking");
  error.with_context("supported_levels", cli_supported_reasoning_levels(model));
}

// Compare two optional reasoning selections for field-wise equality so the
// setter can short-circuit a no-op change without writing an entry.
bool same_reasoning_selection(std::optional<ReasoningSelection> const& left, std::optional<ReasoningSelection> const& right)
{
  if (!left || !right)
    return !left && !right;
  return left->level == right->level && left->provider_level == right->provider_level && left->budget_tokens == right->budget_tokens &&
         left->display == right->display;
}

ava::core::Result<std::string> resolve_session_id(std::filesystem::path const& workspace_dir, std::filesystem::path const& root_dir,
                                                  std::string_view requested_id)
{
  auto sessions = ava::session::SessionStore::list_sessions(workspace_dir, root_dir);
  if (!sessions)
    return std::unexpected(sessions.error());

  std::vector<std::string> matches;
  for (auto const& session : *sessions)
  {
    if (session.session_id == requested_id || session.session_id.starts_with(requested_id))
    {
      matches.push_back(session.session_id);
    }
  }
  if (matches.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::NotFound, "session not found");
    error.with_context("session_id", std::string(requested_id));
    return std::unexpected(std::move(error));
  }
  if (matches.size() > 1)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session id prefix is ambiguous");
    error.with_context("session_id", std::string(requested_id));
    error.with_context("matches", std::to_string(matches.size()));
    return std::unexpected(std::move(error));
  }
  return matches.front();
}

ava::core::Result<std::pair<std::filesystem::path, std::filesystem::path>> resolve_runtime_directories(OpenContext const& context)
{
  auto cwd = ava::core::launch_workspace_root();
  if (!cwd)
    return std::unexpected(std::move(cwd.error()));
  auto workspace_dir = context.workspace_dir.empty() ? *cwd : context.workspace_dir;
  auto current_dir = context.current_dir.empty() ? workspace_dir : context.current_dir;
  return std::pair<std::filesystem::path, std::filesystem::path>{std::move(workspace_dir), std::move(current_dir)};
}

ava::core::Result<ava::session::SessionReadAuthority> bind_runtime_read_authority(ava::session::SessionStore const& store,
                                                                                  ava::session::SessionLease const& lease,
                                                                                  ava::session::SessionReadLimits read_limits)
{
  return store.is_ephemeral() ? ava::session::SessionReadAuthority::create_ephemeral(store, read_limits)
                              : ava::session::SessionReadAuthority::create_persistent(store, lease, read_limits);
}

ava::core::VoidResult reconcile_committed_function_calls(std::shared_ptr<ava::session::SessionAppendTarget> const& append_target,
                                                         ava::session::SessionReadLimits limits)
{
  auto read_authority = append_target->read_authority();
  if (!read_authority)
    return std::unexpected(std::move(read_authority.error()));
  return ava::agent::reconcile_unresolved_committed_function_calls(
      *read_authority, [append_target](ava::session::SessionEntry entry) { return append_target->append(entry); }, limits);
}

ava::core::Result<std::shared_ptr<SubagentDeliveryManager>> delivery_manager_for_context(OpenContext const& context)
{
  if (context.subagent_delivery_manager)
    return context.subagent_delivery_manager;
  auto coordinator = context.subagent_coordinator ? ava::core::Result<std::shared_ptr<ava::agent::SubagentCoordinator>>(context.subagent_coordinator)
                                                  : ava::agent::SubagentCoordinator::create();
  if (!coordinator)
    return std::unexpected(std::move(coordinator.error()));
  return SubagentDeliveryManager::create({.coordinator = std::move(*coordinator)});
}

ava::core::Result<std::shared_ptr<SessionTitleCoordinator>> title_coordinator_for_context(OpenContext const& context,
                                                                                          std::shared_ptr<ava::core::AnchorSet> const& anchor_set)
{
  if (context.session_title_coordinator)
    return context.session_title_coordinator;
  auto config = ava::config::load_session_title_config(context.paths, *anchor_set);
  if (!config)
    return std::unexpected(std::move(config.error()));
  return SessionTitleCoordinator::create({.config = std::move(*config)});
}

bool path_contains(std::filesystem::path const& root, std::filesystem::path const& candidate)
{
  auto const relative = candidate.lexically_relative(root);
  auto const text = relative.generic_string();
  return !relative.empty() && relative != ".." && !text.starts_with("../");
}

void append_command_authority_root(std::vector<std::filesystem::path>& roots, std::filesystem::path root)
{
  if (root.empty())
    return;
  root = root.lexically_normal();
  if (std::ranges::any_of(roots, [&root](std::filesystem::path const& existing) { return path_contains(existing, root); }))
    return;
  std::erase_if(roots, [&root](std::filesystem::path const& existing) { return path_contains(root, existing); });
  if (roots.size() < kMaxCommandAuthorityRoots)
    roots.push_back(std::move(root));
}

} // namespace

void SessionResources::swap(SessionResources& other)
{
  lease.swap(other.lease);
  session_process_scope.swap(other.session_process_scope);
  anchor_set.swap(other.anchor_set);
  run_controller.swap(other.run_controller);
  append_target.swap(other.append_target);
  bound_read_authority.swap(other.bound_read_authority);
  subagent_coordinator.swap(other.subagent_coordinator);
  subagent_delivery_manager.swap(other.subagent_delivery_manager);
  session_title_coordinator.swap(other.session_title_coordinator);
  diagnostics.swap(other.diagnostics);
  mcp_config.swap(other.mcp_config);
}

//static
ava::core::Result<session_ts> Session::open_like(session_ts const& unlocked_session, OpenContext const& base_context, SessionLifecycleRequest request)
{
  CRITICAL_AREA_BEGIN_CR(session);
  OpenContext const open_context = session_r->replacement_open_context(base_context);
  auto const workspace_dir = session_r->workspace_dir();
  auto const current_dir = session_r->current_dir();
  CRITICAL_AREA_END_R(session);
  return open_at(std::move(open_context), workspace_dir, current_dir, std::move(request));
}

//static
ava::core::Result<session_ts> Session::open_at(OpenContext context, std::filesystem::path const& workspace_root, std::filesystem::path const& current_dir,
                                               SessionLifecycleRequest request)
{
  context.workspace_dir = workspace_root;
  context.current_dir = current_dir;
  return Session::open(context, request);
}

//static
ava::core::Result<session_ts> Session::open(runtime::OpenContext const& context, runtime::SessionLifecycleRequest const& request)
{
  if (request.requested_session_id && request.continue_last_session)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "use either requested session id or continue, not both"));
  if (request.fork_session_id && (request.requested_session_id || request.continue_last_session))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "use either fork or session resume options, not both"));
  if (request.sessionless && (request.requested_session_id || request.continue_last_session || request.fork_session_id))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "use either no-session or session resume options, not both"));

  auto directories = resolve_runtime_directories(context);
  if (!directories)
    return std::unexpected(std::move(directories.error()));
  auto const& workspace_dir = directories->first;

  auto const session_read_limits = context.session_read_limits.value_or(ava::session::legacy_unbounded_session_read_limits());
  if (request.requested_session_id && context.subagent_delivery_manager)
  {
    bool retained_found = false;
    auto retained = context.subagent_delivery_manager->retained_session(
        *request.requested_session_id, workspace_dir, retained_found,
        {.exact_session_id = context.exact_session_id, .read_limits = session_read_limits, .expected_original_cwd = request.expected_original_cwd});
    if (retained_found)
      return retained;
  }

  bool created = true;
  bool created_from_fork = false;
  bool load_existing_entries = false;
  bool should_append_session_start = true;
  ava::session::SessionLease lease;
  ava::core::Result<ava::session::SessionStore> store = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "session was not initialized"));
  if (request.sessionless)
  {
    store = ava::session::SessionStore::create_ephemeral(workspace_dir);
  }
  else if (request.fork_session_id)
  {
    auto resolved = resolve_session_id(workspace_dir, context.paths.sessions_dir, *request.fork_session_id);
    if (!resolved)
      return std::unexpected(resolved.error());
    auto source = ava::session::SessionStore::open(workspace_dir, *resolved, context.paths.sessions_dir);
    if (!source)
      return std::unexpected(std::move(source.error()));
    auto source_lease = ava::session::SessionLease::acquire(source->session_path());
    if (!source_lease)
      return std::unexpected(std::move(source_lease.error()));
    auto recovered = source->recover_torn_tail(*source_lease, session_read_limits);
    if (!recovered)
      return std::unexpected(std::move(recovered.error()));
    auto staged_recovery = source->recover_incomplete_assistant_output_suffix(*source_lease, session_read_limits);
    if (!staged_recovery)
      return std::unexpected(std::move(staged_recovery.error()));
    auto branch = ava::session::create_session_branch(ava::session::SessionBranchOptions{.workspace_dir = workspace_dir,
                                                                                         .root_dir = context.paths.sessions_dir,
                                                                                         .source_session_id = *resolved,
                                                                                         .branch_from_entry_id = {},
                                                                                         .name = request.initial_session_name,
                                                                                         .labels = std::nullopt,
                                                                                         .read_limits = context.session_read_limits,
                                                                                         .source_lease = &*source_lease,
                                                                                         .mode = ava::session::SessionBranchMode::Fork,
                                                                                         .actor = "cli"});
    if (!branch)
      return std::unexpected(std::move(branch.error()));
    store = std::move(branch->store);
    lease = std::move(branch->lease);
    created_from_fork = true;
    load_existing_entries = true;
    should_append_session_start = false;
  }
  else if (request.requested_session_id)
  {
    if (context.exact_session_id)
    {
      store = ava::session::SessionStore::open(workspace_dir, *request.requested_session_id, context.paths.sessions_dir);
    }
    else
    {
      auto resolved = resolve_session_id(workspace_dir, context.paths.sessions_dir, *request.requested_session_id);
      if (!resolved)
        return std::unexpected(resolved.error());
      store = ava::session::SessionStore::open(workspace_dir, *resolved, context.paths.sessions_dir);
    }
    created = false;
    load_existing_entries = true;
  }
  else if (request.continue_last_session)
  {
    auto sessions = ava::session::SessionStore::list_sessions(workspace_dir, context.paths.sessions_dir);
    if (!sessions)
      return std::unexpected(sessions.error());
    if (!sessions->empty())
    {
      if (context.subagent_delivery_manager)
      {
        bool retained_found = false;
        auto retained = context.subagent_delivery_manager->retained_session(
            sessions->front().session_id, workspace_dir, retained_found,
            {.exact_session_id = true, .read_limits = session_read_limits, .expected_original_cwd = request.expected_original_cwd});
        if (retained_found)
          return retained;
      }
      store = ava::session::SessionStore::open(workspace_dir, sessions->front().session_id, context.paths.sessions_dir);
      created = false;
      load_existing_entries = true;
    }
    else
    {
      store = ava::session::SessionStore::create(workspace_dir, context.paths.sessions_dir);
    }
  }
  else
  {
    store = ava::session::SessionStore::create(workspace_dir, context.paths.sessions_dir);
  }
  if (!store)
    return std::unexpected(store.error());

  if (!created)
  {
    auto acquired = ava::session::SessionLease::acquire(store->session_path());
    if (!acquired)
      return std::unexpected(std::move(acquired.error()));
    lease = std::move(*acquired);
    auto recovered = store->recover_torn_tail(lease, session_read_limits);
    if (!recovered)
      return std::unexpected(std::move(recovered.error()));
    auto staged_recovery = store->recover_incomplete_assistant_output_suffix(lease, session_read_limits);
    if (!staged_recovery)
      return std::unexpected(std::move(staged_recovery.error()));
  }

  auto unlocked_session_result =
      construct(context, request, *store, lease, created, load_existing_entries, created && should_append_session_start,
                request.initial_session_name.has_value() && !request.fork_session_id, context.subagent_delivery_manager, context.session_title_coordinator);
  if (!unlocked_session_result && created_from_fork)
  {
    auto error = std::move(unlocked_session_result.error());
    ava::session::rollback_created_session_with_context(*store, lease, error);
    return std::unexpected(std::move(error));
  }
  return unlocked_session_result;
}

//static
ava::core::Result<session_ts> Session::construct(OpenContext const& context, runtime::SessionLifecycleRequest const& request, ava::session::SessionStore& store,
                                                 ava::session::SessionLease& lease, bool created, bool load_existing_entries, bool should_append_session_start,
                                                 bool append_initial_session_name, std::shared_ptr<SubagentDeliveryManager> delivery_manager,
                                                 std::shared_ptr<SessionTitleCoordinator> title_coordinator)
{
  // This function ends with a move of the newly created Session, after which that moved Session is destructed.
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling Session::construct");

  std::optional<ava::process::ProcessScopeV1> session_process_scope;
  if (context.application_process_scope)
  {
    auto derived = context.application_process_scope->session();
    if (!derived)
      return std::unexpected(std::move(derived.error()));
    session_process_scope = std::move(*derived);
  }

  // Provider catalog is application-scoped authority. Resolve it before any
  // session-file mutation so unsafe providers.json fails closed at startup.
  auto provider_catalog = ava::provider::ensure_provider_catalog(context.provider_catalog, context.paths);
  if (!provider_catalog)
    return std::unexpected(std::move(provider_catalog.error()));

  auto directories = resolve_runtime_directories(context);
  if (!directories)
    return std::unexpected(std::move(directories.error()));
  auto const& workspace_dir = directories->first;
  auto const& current_dir = directories->second;
  auto const session_read_limits = context.session_read_limits.value_or(ava::session::legacy_unbounded_session_read_limits());

  if (context.pin_model_override && !context.default_model_override)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "pinned runtime model override is missing"));

  ava::config::ModelRegistry registry;
  if (context.pin_model_override)
  {
    registry.default_provider_id = context.default_model_override->provider_id;
    registry.default_model_id = context.default_model_override->model_id;
    registry.models = {*context.default_model_override};
    registry.scoped_model_cycle = std::nullopt;
  }
  else
  {
    auto loaded_registry = ava::config::load_model_registry(context.paths);
    if (!loaded_registry)
      return std::unexpected(loaded_registry.error());
    registry = std::move(*loaded_registry);
  }
  auto model = context.default_model_override.value_or(ava::config::select_default_model(registry));
  if (auto valid = (*provider_catalog)->validate_active_model(model); !valid)
    return std::unexpected(std::move(valid.error()));

  std::optional<std::vector<ava::session::SessionEntry>> loaded_entries;
  if (load_existing_entries)
  {
    auto read_authority = bind_runtime_read_authority(store, lease, session_read_limits);
    if (!read_authority)
      return std::unexpected(std::move(read_authority.error()));
    auto entries = read_authority->load();
    if (!entries)
      return std::unexpected(std::move(entries.error()));
    if (!context.pin_model_override)
    {
      if (auto persisted_model = latest_persisted_model(registry, *entries))
        model = std::move(*persisted_model);
    }
    if (auto valid = (*provider_catalog)->validate_active_model(model); !valid)
      return std::unexpected(std::move(valid.error()));
    loaded_entries = std::move(*entries);
    if (request.expected_original_cwd)
    {
      auto summary = read_authority->inspect_bounded(session_read_limits);
      if (!summary)
        return std::unexpected(std::move(summary.error()));
      auto const persisted_cwd = summary->original_cwd.empty() ? workspace_dir : summary->original_cwd;
      if (persisted_cwd != *request.expected_original_cwd)
      {
        auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "requested cwd does not match persisted session cwd");
        error.with_context("persisted_cwd", persisted_cwd.string()).with_context("requested_cwd", request.expected_original_cwd->string());
        return std::unexpected(std::move(error));
      }
    }
  }

  std::optional<ReasoningSelection> reasoning;
  if (loaded_entries)
    reasoning = latest_persisted_reasoning(*loaded_entries, model);

  if (!delivery_manager)
  {
    auto created_manager = delivery_manager_for_context(context);
    if (!created_manager)
      return std::unexpected(std::move(created_manager.error()));
    delivery_manager = std::move(*created_manager);
  }
  // This manager normalizes the workspace key. Holding the move-only
  // reservation across authority resolution, controller registration, and the
  // returned Session publication linearizes construction's trust read against
  // revocation persistence.
  auto reserved_construction = delivery_manager->reserve_workspace_navigation(workspace_dir);
  if (!reserved_construction)
    return std::unexpected(std::move(reserved_construction.error()));
  auto construction_reservation = std::move(*reserved_construction);

  auto project_trust = load_project_trust_state(context.paths, workspace_dir);
  bool const project_trusted = project_resources_trusted(project_trust);
  auto selected_primary_agent = resolve_runtime_primary_agent(
      context.paths, workspace_dir, project_trusted, context.requested_primary_agent,
      context.allow_unavailable_primary_agent ? PrimaryAgentResolutionPolicy::AllowUnavailable : PrimaryAgentResolutionPolicy::RequireAvailable);
  if (!selected_primary_agent)
    return std::unexpected(std::move(selected_primary_agent.error()));
  auto prompt_state = load_runtime_prompt_state(context.paths, model, context.mode, workspace_dir, current_dir, project_trusted, context.prompt_overrides,
                                                *selected_primary_agent);
  if (!prompt_state)
    return std::unexpected(prompt_state.error());
  if (project_trusted)
  {
    if (auto hooked = delivery_manager->run_construction_after_trusted_prompt_resolution_test_hook(); !hooked)
      return std::unexpected(std::move(hooked.error()));
  }

  if (should_append_session_start)
  {
    if (!store.is_ephemeral() && lease.canonical_path().empty())
    {
      auto acquired = ava::session::SessionLease::create_and_acquire(store.session_path());
      if (!acquired)
        return std::unexpected(std::move(acquired.error()));
      lease = std::move(*acquired);
    }
    auto appended =
        store.is_ephemeral()
            ? append_session_start_ephemeral(store, context.mode, model, prompt_state->base_prompt, prompt_state->context_sources.size(), current_dir)
            : append_session_start(store, lease, context.mode, model, prompt_state->base_prompt, prompt_state->context_sources.size(), current_dir);
    if (!appended)
      return std::unexpected(std::move(appended.error()));
  }

  bool const sessionless = store.is_ephemeral();
  auto append_target = sessionless ? ava::session::SessionAppendTarget::create_ephemeral(store, session_read_limits)
                                   : ava::session::SessionAppendTarget::create_persistent(store, lease, session_read_limits);
  if (!append_target)
    return std::unexpected(std::move(append_target.error()));
  // Opening/resuming is the durable recovery boundary: the append target is
  // already authority-checked, so any committed function call left without an
  // exact result is closed without re-executing its tool.
  if (auto reconciled = reconcile_committed_function_calls(*append_target, session_read_limits); !reconciled)
    return std::unexpected(std::move(reconciled.error()));

  if (append_initial_session_name && request.initial_session_name)
  {
    auto read_authority = (*append_target)->read_authority();
    if (!read_authority)
      return std::unexpected(std::move(read_authority.error()));
    auto entries = read_authority->load();
    if (!entries)
      return std::unexpected(std::move(entries.error()));
    auto entry = ava::session::make_session_metadata_entry(ava::session::SessionMetadataUpdate{.name = request.initial_session_name, .actor = "cli"},
                                                           entries->empty() ? std::string{} : entries->back().id);
    if (!entry)
      return std::unexpected(std::move(entry.error()));
    if (auto appended = (*append_target)->append(*entry); !appended)
      return std::unexpected(std::move(appended.error()));
  }

  // Open the AnchorSet once for the session lifetime. The logical workspace,
  // AVA-owned roots, spill directory, and any additional writable directories
  // are opened as descriptors shared by parent delivery and every child loop.
  std::shared_ptr<ava::core::AnchorSet> anchor_set;
  {
    std::vector<std::filesystem::path> anchor_roots;
    anchor_roots.push_back(workspace_dir);
    auto const spill_dir = store.session_path().parent_path() / "spill";
    // Create the spill directory now so it can be opened as an anchor.
    // spill_files.cpp also creates it on first use, but we need it to exist
    // before AnchorSet::open so the anchor descriptor is pre-opened.
    std::error_code spill_error;
    std::filesystem::create_directories(spill_dir, spill_error);
    if (spill_error)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to create the session spill anchor"));
    std::filesystem::permissions(spill_dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, spill_error);
    if (spill_error)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to secure the session spill anchor"));
    anchor_roots.push_back(spill_dir);

    std::error_code config_error;
    std::filesystem::create_directories(context.paths.ava_config_dir, config_error);
    if (config_error)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to create the AVA config anchor"));
    std::filesystem::permissions(context.paths.ava_config_dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, config_error);
    if (config_error)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to secure the AVA config anchor"));
    anchor_roots.push_back(context.paths.ava_config_dir);

    std::error_code state_error;
    std::filesystem::create_directories(context.paths.ava_state_dir, state_error);
    if (state_error)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to create the AVA state anchor"));
    std::filesystem::permissions(context.paths.ava_state_dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, state_error);
    if (state_error)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to secure the AVA state anchor"));
    anchor_roots.push_back(context.paths.ava_state_dir);

    for (auto const& dir : context.additional_writable_dirs) anchor_roots.push_back(dir);
    if (context.anchor_set)
    {
      anchor_set = context.anchor_set;
    }
    else
    {
      auto opened = ava::core::AnchorSet::open(anchor_roots);
      if (!opened)
        return std::unexpected(std::move(opened.error()));
      anchor_set = std::move(*opened);
    }
    auto const config_anchor = anchor_set->find_anchor(context.paths.ava_config_dir);
    auto const state_anchor = anchor_set->find_anchor(context.paths.ava_state_dir);
    if (!anchor_set->find_anchor(workspace_dir) || !anchor_set->find_anchor(spill_dir) || !config_anchor || !config_anchor->relative().empty() ||
        !state_anchor || !state_anchor->relative().empty() ||
        std::ranges::any_of(context.additional_writable_dirs, [&](auto const& directory) { return !anchor_set->find_anchor(directory); }))
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to retain all required runtime anchors"));
  }

  // Resolve the trusted local account once per process and freeze it. The home
  // directory is read from HOME (passwd fallback) here, at startup, so that
  // later command planning reuses the cached value (ava::core::cached_trusted_account)
  // instead of re-reading the sensitive HOME environment variable after the AI
  // or user shell commands may have run. open is invoked for
  // every session (interactive, print, rpc, ACP, and forked/subagent sessions),
  // so the cache check keeps the actual HOME read to the very first call and
  // lets later sessions reuse the frozen result without tripping the freeze
  // assertion.
  if (auto result = ava::core::load_account_once_and_freeze(); !result)
    return std::unexpected(std::move(result.error()));

  if (context.diagnostics)
  {
    if (auto bound = context.diagnostics->bind_anchor_set(anchor_set); !bound)
      return std::unexpected(std::move(bound.error()));
  }

  if (!title_coordinator)
  {
    auto created_coordinator = title_coordinator_for_context(context, anchor_set);
    if (!created_coordinator)
      return std::unexpected(std::move(created_coordinator.error()));
    title_coordinator = std::move(*created_coordinator);
  }

  auto effective_tool_visibility = context.tool_visibility;
  if (*selected_primary_agent && (*selected_primary_agent)->tool_preset == ava::agent::SubagentToolPreset::ReadOnly)
    effective_tool_visibility = ava::agent::narrow_tool_visibility_to_read_only(std::move(effective_tool_visibility));

  InvocationInputs invocation_inputs{.workspace_dir = workspace_dir,
                                     .current_dir = current_dir,
                                     .requested_tool_visibility = context.tool_visibility,
                                     .tool_visibility = std::move(effective_tool_visibility),
                                     .requested_primary_agent = context.requested_primary_agent,
                                     .selected_primary_agent = std::move(*selected_primary_agent),
                                     .paths = context.paths,
                                     .sessionless = sessionless,
                                     .is_offline_ = context.offline,
                                     .additional_writable_dirs = context.additional_writable_dirs,
                                     .session_read_limits = session_read_limits,
                                     .prompt_overrides = context.prompt_overrides};
  ResolvedPromptState resolved_prompt_state{.mode = context.mode,
                                            .base_prompt = std::move(prompt_state->base_prompt),
                                            .context_sources = std::move(prompt_state->context_sources),
                                            .freshness_sources = std::move(prompt_state->freshness_sources),
                                            .system_prompt = std::move(prompt_state->system_prompt),
                                            .ambient_extension_free_system_prompt = std::move(prompt_state->ambient_extension_free_system_prompt)};
  ModelSelection model_selection{.model = std::move(model), .reasoning = std::move(reasoning), .scoped_model_cycle = registry.scoped_model_cycle};
  TrustState trust_state{.project_trust = std::move(project_trust)};
  std::shared_ptr<SessionRunController> run_controller;
  try
  {
    run_controller = std::make_shared<SessionRunController>(*append_target);
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to allocate the runtime session controller"));
  }
  if (auto registered = delivery_manager->register_workspace_controller(workspace_dir, store.session_id(), run_controller); !registered)
    return std::unexpected(std::move(registered.error()));
  SessionResources resources{.lease = std::move(lease),
                             .session_process_scope = std::move(session_process_scope),
                             .anchor_set = std::move(anchor_set),
                             .run_controller = std::move(run_controller),
                             .append_target = std::move(*append_target),
                             .subagent_coordinator = delivery_manager->coordinator(),
                             .subagent_delivery_manager = std::move(delivery_manager),
                             .session_title_coordinator = std::move(title_coordinator),
                             .diagnostics = context.diagnostics,
                             .provider_catalog = std::move(*provider_catalog)};
  Session session({.invocation_inputs_ = std::move(invocation_inputs),
                   .resolved_prompt_state_ = std::move(resolved_prompt_state),
                   .model_selection_ = std::move(model_selection),
                   .trust_state_ = std::move(trust_state),
                   .resources_ = std::move(resources),
                   .store = std::move(store),
                   .created = created});

  if (request.initial_reasoning_level)
  {
    if (auto applied = session.apply_initial_reasoning_level(*request.initial_reasoning_level); !applied)
    {
      auto error = std::move(applied.error());
      store = std::move(session.store);
      lease = std::move(session.resources().lease);
      return std::unexpected(std::move(error));
    }
  }
  if (!sessionless)
    session.subagent_delivery_manager()->attach_parent(session.store.session_id());
  return session;
}

//static
ava::core::Result<session_ts> Session::open_owned(OpenContext const& context, ava::session::SessionStore& store, ava::session::SessionLease& lease,
                                                  bool created)
{
  if (store.is_ephemeral())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "owned runtime session handoff requires a persistent session"));
  if (lease.canonical_path().empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "owned runtime session handoff requires an active lease"));

  auto const session_read_limits = context.session_read_limits.value_or(ava::session::legacy_unbounded_session_read_limits());
  auto recovered = store.recover_torn_tail(lease, session_read_limits);
  if (!recovered)
    return std::unexpected(std::move(recovered.error()));
  auto staged_recovery = store.recover_incomplete_assistant_output_suffix(lease, session_read_limits);
  if (!staged_recovery)
    return std::unexpected(std::move(staged_recovery.error()));
  return construct(context, {}, store, lease, created, true, false, false, context.subagent_delivery_manager, context.session_title_coordinator);
}

// Snapshot all state needed to construct a detached session, using lease, authority, and manager for its independent resources.
//
// No Session is constructed here, so returning this aggregate while the source session is read-locked cannot destruct a temporary Session.
Session_aggregate_base Session::create_detached_state(ava::session::SessionLease lease, ava::session::SessionReadAuthority authority,
                                                      std::shared_ptr<ava::app::SubagentDeliveryManager> manager) const
{
  auto detached_inputs = invocation_inputs();
  detached_inputs.session_read_limits = authority.read_limits();
  SessionResources session_resources{.lease = std::move(lease),
                                     .session_process_scope = session_process_scope(),
                                     .anchor_set = anchor_set(),
                                     .run_controller = run_controller(),
                                     .append_target = append_target(),
                                     .bound_read_authority = std::move(authority),
                                     .subagent_coordinator = subagent_coordinator(),
                                     .subagent_delivery_manager = std::move(manager),
                                     .session_title_coordinator = session_title_coordinator(),
                                     .diagnostics = diagnostics(),
                                     .mcp_config = mcp_config(),
                                     .provider_catalog = provider_catalog()};
  return Session_aggregate_base{.invocation_inputs_ = std::move(detached_inputs),
                                .resolved_prompt_state_ = resolve_prompt_state(),
                                .model_selection_ = model_selection(),
                                .trust_state_ = trust_state(),
                                .resources_ = std::move(session_resources),
                                .store = store,
                                .created = created};
}

//static
ava::core::VoidResult Session::refresh_parent_configuration(session_ts const& unlocked_session)
{
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling Session::refresh_parent_configuration");
  CRITICAL_AREA_BEGIN_CR(session);
  auto manager = session_r->subagent_delivery_manager();
  CRITICAL_AREA_END_R(session);
  return manager ? manager->refresh_parent_configuration(unlocked_session) : ava::core::VoidResult{};
}

//static
ava::core::VoidResult Session::replace_with(session_ts& unlocked_current, session_ts& unlocked_replacement)
{
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling Session::replace_with");

  if (&unlocked_current == &unlocked_replacement)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "cannot replace a session with itself"));

  struct NavigationSnapshot
  {
    std::shared_ptr<SubagentDeliveryManager> manager;
    std::filesystem::path workspace;
    std::shared_ptr<SessionRunController> controller;
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };
  NavigationSnapshot current_navigation;
  NavigationSnapshot replacement_navigation;
  {
    SCOPED_CRITICAL_AREA_R(current_r, unlocked_current);
    current_navigation = {
        .manager = current_r->subagent_delivery_manager(), .workspace = current_r->workspace_dir(), .controller = current_r->run_controller()};
  }
  {
    SCOPED_CRITICAL_AREA_R(replacement_r, unlocked_replacement);
    replacement_navigation = {
        .manager = replacement_r->subagent_delivery_manager(), .workspace = replacement_r->workspace_dir(), .controller = replacement_r->run_controller()};
  }
  std::vector<SubagentDeliveryManager::WorkspaceNavigationReservation> navigation_reservations;
  if (current_navigation.manager)
  {
    auto reserved = current_navigation.manager->reserve_workspace_navigation(current_navigation.workspace);
    if (!reserved)
      return std::unexpected(std::move(reserved.error()));
    navigation_reservations.push_back(std::move(*reserved));
  }
  if (replacement_navigation.manager)
  {
    auto reserved = replacement_navigation.manager->reserve_workspace_navigation(replacement_navigation.workspace);
    if (!reserved)
      return std::unexpected(std::move(reserved.error()));
    navigation_reservations.push_back(std::move(*reserved));
  }
  if (!current_navigation.controller || !replacement_navigation.controller || current_navigation.controller->authority_retired() ||
      replacement_navigation.controller->authority_retired())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "runtime session authority was retired");
    error.with_context("recovery", "reopen the session before navigating");
    return std::unexpected(std::move(error));
  }

  // We can't destroy any resources while holding session locks; therefore keep a store for them outside the critical area.
  SessionResources old_resources;

  CRITICAL_AREA_BEGIN_W(current);

  // Zero-out all resources of current, without destroying any; we just park them in `old_resources`.
  current_w->resources().swap(old_resources);

  CRITICAL_AREA_BEGIN_W(replacement);

  // We might need to call release_detached_parent on this after releasing the locks.
  auto const detached_parent_id = current_w->sessionless() ? std::string{} : current_w->store.session_id();
  bool const leaves_detached_parent = !detached_parent_id.empty() && (replacement_w->sessionless() || replacement_w->store.session_id() != detached_parent_id);
  auto const old_subagent_delivery_manager = old_resources.subagent_delivery_manager;

  // Replace current with replacement.
  *current_w = std::move(*replacement_w);

  // Normal slash navigation does use `current.replacement_open_context`.
  // That function copies the current session’s manager, coordinator, title coordinator, and diagnostics
  // into the replacement context. Those normal replacements therefore generally share identities.
  // However, we need replace_with to work too if current has any of those already set, in which case
  // they should not have been replaced.
  //
  // The correct algorithm here would be, in pseudo code:
  //
  // 1. If current has a manager:
  //      preserve current.manager
  //      coordinator = current.manager->coordinator()
  //
  // 2. Otherwise, if replacement has a manager:
  //      use replacement.manager
  //      coordinator = replacement.manager->coordinator()
  //
  // 3. Otherwise, neither has a manager:
  //      preserve current.coordinator if present;
  //      otherwise use replacement.coordinator.
  //
  // Note that it is impossible for a `manager->coordinator()` to be null, if the `manager` exists.
  // If this fires, old_resources paired a delivery manager with a coordinator not taken from that manager; keep the
  // manager/coordinator pair consistent by taking the coordinator from the manager when the resources are built.
  ASSERT(!old_resources.subagent_delivery_manager ||
         (old_resources.subagent_delivery_manager->coordinator() &&
          old_resources.subagent_delivery_manager->coordinator() == old_resources.subagent_coordinator));
  // If this fires, current_w paired a delivery manager with a coordinator not taken from that manager; keep the
  // manager/coordinator pair consistent by taking the coordinator from the manager when the resources are built.
  ASSERT(!current_w->resources().subagent_delivery_manager ||
         (current_w->resources().subagent_delivery_manager->coordinator() &&
          current_w->resources().subagent_delivery_manager->coordinator() == current_w->resources().subagent_coordinator));

  if (old_resources.subagent_delivery_manager)                  // 1. If current has a manager.
  {
    // Put subagent_delivery_manager and subagent_coordinator back as a pair.
    current_w->resources().subagent_delivery_manager.swap(old_resources.subagent_delivery_manager);
    current_w->resources().subagent_coordinator.swap(old_resources.subagent_coordinator);
  }
  // 2. Otherwise, if replacement has a manager; has already been taken care of by the `*current_w = std::move(*replacement_w);` above.
  else if (!current_w->resources().subagent_delivery_manager)   // 3. Otherwise, neither has a manager:
  {
    if (old_resources.subagent_coordinator)                     // preserve current.coordinator if present.
      current_w->resources().subagent_coordinator.swap(old_resources.subagent_coordinator);
  }

  // Also put back any already existing session_title_coordinator and/or diagnostics.
  if (old_resources.session_title_coordinator)
    current_w->resources().session_title_coordinator.swap(old_resources.session_title_coordinator);
  if (old_resources.diagnostics)
    current_w->resources().diagnostics.swap(old_resources.diagnostics);

  // We expect replacement to always have a subagent_delivery_manager because it
  // should have been constructed with `Session::construct`, that does:
  //   .subagent_coordinator = delivery_manager->coordinator(),
  //   .subagent_delivery_manager = std::move(delivery_manager),
  // after making sure `delivery_manager` is non-null; replace_with is never called
  // on a manager-less Session. And thus
  // if this fires, the replacement Session has no subagent delivery manager; never call replace_with on a manager-less
  // Session — construct replacements with Session::construct, which requires a non-null delivery_manager.
  ASSERT(current_w->resources().subagent_delivery_manager);
  // If this fires, current_w's coordinator was not taken from its delivery manager; keep the manager/coordinator pair
  // consistent by taking the coordinator from the manager.
  ASSERT(current_w->resources().subagent_coordinator == current_w->resources().subagent_delivery_manager->coordinator());

  CRITICAL_AREA_END_W(replacement);
  CRITICAL_AREA_END_W(current);

  // This is an explicit visible-session detach boundary. The delivery manager
  // keeps the exact parent capsule while process-local work still needs it. Both
  // session locks must be released before this potentially blocking boundary.
  if (leaves_detached_parent && old_subagent_delivery_manager)
    old_subagent_delivery_manager->release_detached_parent(detached_parent_id);

  return {};
}

ava::core::VoidResult Session::recover_source_for_mutation(std::string const& source_session_id,
                                                           std::optional<ava::session::SessionLease>& temporary_source_lease)
{
  if (source_session_id == store.session_id())
  {
    auto recovered = store.recover_torn_tail(lease(), session_read_limits());
    if (!recovered)
      return std::unexpected(std::move(recovered.error()));
    auto staged_recovery = store.recover_incomplete_assistant_output_suffix(lease(), session_read_limits());
    if (!staged_recovery)
      return std::unexpected(std::move(staged_recovery.error()));
    return {};
  }

  auto source = ava::session::SessionStore::open(workspace_dir(), source_session_id, paths().sessions_dir);
  if (!source)
    return std::unexpected(std::move(source.error()));
  auto acquired = ava::session::SessionLease::acquire(source->session_path());
  if (!acquired)
    return std::unexpected(std::move(acquired.error()));
  temporary_source_lease.emplace(std::move(*acquired));
  auto recovered = source->recover_torn_tail(*temporary_source_lease, session_read_limits());
  if (!recovered)
    return std::unexpected(std::move(recovered.error()));
  auto staged_recovery = source->recover_incomplete_assistant_output_suffix(*temporary_source_lease, session_read_limits());
  if (!staged_recovery)
    return std::unexpected(std::move(staged_recovery.error()));
  return {};
}

OpenContext Session::replacement_open_context(runtime::OpenContext const& base_context) const
{
  auto context = base_context;
  context.workspace_dir = workspace_dir();
  context.current_dir = current_dir();
  context.mode = mode();
  context.tool_visibility = invocation_inputs().requested_tool_visibility;
  context.requested_primary_agent = requested_primary_agent();
  context.allow_unavailable_primary_agent = true;
  context.paths = paths();
  context.offline = is_offline();
  context.additional_writable_dirs = additional_writable_dirs();
  context.anchor_set = sessionless() ? nullptr : anchor_set();
  context.prompt_overrides = prompt_overrides();
  context.session_read_limits = session_read_limits();
  if (session_process_scope())
    context.application_process_scope = session_process_scope()->application_scope();
  else
    context.application_process_scope.reset();
  context.subagent_coordinator = subagent_coordinator();
  context.subagent_delivery_manager = subagent_delivery_manager();
  context.session_title_coordinator = session_title_coordinator();
  context.diagnostics = diagnostics();
  context.provider_catalog = provider_catalog();
  return context;
}

ava::core::Result<ava::session::SessionMetadataView> Session::append_metadata_1(ava::session::SessionMetadataUpdate update)
{
  auto read_authority = this->read_authority_1();
  if (!read_authority)
    return std::unexpected(std::move(read_authority.error()));
  auto entries = read_authority->load();
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  auto entry = ava::session::make_session_metadata_entry(std::move(update), entries->empty() ? std::string{} : entries->back().id);
  if (!entry)
    return std::unexpected(std::move(entry.error()));
  if (auto appended = append_owned(*entry); !appended)
    return std::unexpected(std::move(appended.error()));
  entries->push_back(std::move(*entry));
  return ava::session::session_metadata_from_entries(store.session_id(), *entries);
}

ava::core::VoidResult Session::append_mode_change_1(ava::agent::Mode mode)
{
  return append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                 .parent_id = "",
                                                 .type = ava::session::EntryType::ModeChange,
                                                 .timestamp = ava::session::now_timestamp(),
                                                 .data_json = "{\"mode\":\"" + ava::agent::to_string(mode) + "\"}"});
}

ava::core::VoidResult Session::apply_initial_reasoning_level(std::string_view requested_level)
{
  auto level = core::trim(requested_level);
  if (level.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "reasoning level is required");
    add_cli_reasoning_context(error, model());
    return std::unexpected(std::move(error));
  }

  std::optional<ReasoningSelection> selection = std::nullopt;
  if (level != "off")
  {
    auto selected = reasoning_selection_for_level(model(), std::move(level));
    if (!selected)
    {
      auto error = std::move(selected.error());
      add_cli_reasoning_context(error, model());
      return std::unexpected(std::move(error));
    }
    selection = std::move(*selected);
  }

  auto changed = set_reasoning(std::move(selection));
  if (!changed)
  {
    auto error = std::move(changed.error());
    add_cli_reasoning_context(error, model());
    return std::unexpected(std::move(error));
  }
  return {};
}

ava::core::VoidResult Session::apply_prompt_state(PromptState prompt_state)
{
  resolve_prompt_state() = ResolvedPromptState{.mode = prompt_state.mode,
                                               .base_prompt = std::move(prompt_state.base_prompt),
                                               .context_sources = std::move(prompt_state.context_sources),
                                               .freshness_sources = std::move(prompt_state.freshness_sources),
                                               .system_prompt = std::move(prompt_state.system_prompt),
                                               .ambient_extension_free_system_prompt = std::move(prompt_state.ambient_extension_free_system_prompt)};
  return {};
}

//static
ava::core::VoidResult Session::apply_prompt_state_and_refresh(session_ts& unlocked_session, PromptState prompt_state)
{
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling Session::apply_prompt_state_and_refresh");
  CRITICAL_AREA_BEGIN_W(session);
  auto applied = session_w->apply_prompt_state(std::move(prompt_state));
  CRITICAL_AREA_END_W(session);
  if (!applied)
    return applied;
  return refresh_parent_configuration(unlocked_session);
}

//static
ava::core::VoidResult Session::apply_trust_prompt_state_and_refresh(session_ts& unlocked_session, ProjectTrustState project_trust,
                                                                    std::optional<ava::agent::AgentDefinition> selected_primary_agent,
                                                                    PromptState prompt_state)
{
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling Session::apply_trust_prompt_state_and_refresh");
  bool const selected_primary_agent_is_non_project =
      selected_primary_agent && (selected_primary_agent->provenance == ava::agent::AgentDefinitionProvenance::Builtin ||
                                 selected_primary_agent->provenance == ava::agent::AgentDefinitionProvenance::Global);
  if (selected_primary_agent && !project_resources_trusted(project_trust) && !selected_primary_agent_is_non_project)
    selected_primary_agent.reset();

  CRITICAL_AREA_BEGIN_W(session);
  auto effective_tool_visibility = session_w->tool_visibility();
  if (selected_primary_agent && selected_primary_agent->tool_preset == ava::agent::SubagentToolPreset::ReadOnly)
    effective_tool_visibility = ava::agent::narrow_tool_visibility_to_read_only(std::move(effective_tool_visibility));
  session_w->trust_state().project_trust = std::move(project_trust);
  session_w->invocation_inputs().tool_visibility = std::move(effective_tool_visibility);
  session_w->invocation_inputs().selected_primary_agent = std::move(selected_primary_agent);
  auto applied = session_w->apply_prompt_state(std::move(prompt_state));
  CRITICAL_AREA_END_W(session);
  if (!applied)
    return applied;
  return refresh_parent_configuration(unlocked_session);
}

ava::core::Result<bool> Session::switch_model(ava::config::ModelInfo model)
{
  if (this->model().provider_id == model.provider_id && this->model().model_id == model.model_id)
    return false;

  auto prompt_state = load_runtime_prompt_state(paths(), model, mode(), workspace_dir(), current_dir(), project_resources_trusted(project_trust()),
                                                prompt_overrides(), selected_primary_agent());
  if (!prompt_state)
    return std::unexpected(std::move(prompt_state.error()));

  auto const previous = this->model();
  auto appended = append_owned(make_model_change_entry(previous, model));
  if (!appended)
    return std::unexpected(std::move(appended.error()));

  model_selection().model = std::move(model);
  resolve_prompt_state() = ResolvedPromptState{.mode = prompt_state->mode,
                                               .base_prompt = std::move(prompt_state->base_prompt),
                                               .context_sources = std::move(prompt_state->context_sources),
                                               .freshness_sources = std::move(prompt_state->freshness_sources),
                                               .system_prompt = std::move(prompt_state->system_prompt),
                                               .ambient_extension_free_system_prompt = std::move(prompt_state->ambient_extension_free_system_prompt)};
  model_selection().reasoning = std::nullopt;
  return true;
}

//static
ava::core::Result<bool> Session::switch_model_and_refresh(session_ts& unlocked_session, ava::config::ModelInfo model)
{
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling Session::switch_model_and_refresh");
  CRITICAL_AREA_BEGIN_W(session);
  auto switched = session_w->switch_model(std::move(model));
  CRITICAL_AREA_END_W(session);
  if (!switched || !*switched)
    return switched;
  if (auto refreshed = refresh_parent_configuration(unlocked_session); !refreshed)
    return std::unexpected(std::move(refreshed.error()));
  return switched;
}

ava::core::Result<bool> Session::set_reasoning(std::optional<ReasoningSelection> selection)
{
  if (selection)
  {
    selection->level = core::trim(selection->level);
    selection->display = core::trim(selection->display);
    auto resolved = resolve_runtime_reasoning_selection(model(), std::move(*selection));
    if (!resolved)
    {
      return std::unexpected(std::move(resolved.error()));
    }
    selection = std::move(*resolved);
  }
  if (same_reasoning_selection(reasoning(), selection))
    return false;

  auto appended = append_owned(make_reasoning_change_entry(model(), selection));
  if (!appended)
    return std::unexpected(std::move(appended.error()));
  model_selection().reasoning = std::move(selection);
  return true;
}

//static
ava::core::Result<bool> Session::set_reasoning_and_refresh(session_ts& unlocked_session, std::optional<ReasoningSelection> selection)
{
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling Session::set_reasoning_and_refresh");
  CRITICAL_AREA_BEGIN_W(session);
  auto changed = session_w->set_reasoning(std::move(selection));
  CRITICAL_AREA_END_W(session);
  if (!changed || !*changed)
    return changed;
  if (auto refreshed = refresh_parent_configuration(unlocked_session); !refreshed)
    return std::unexpected(std::move(refreshed.error()));
  return changed;
}

ava::core::Result<ava::session::SessionMetadataView> Session::load_metadata() const
{
  auto read_authority = this->read_authority_1();
  if (!read_authority)
    return std::unexpected(std::move(read_authority.error()));
  auto entries = read_authority->load();
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  return ava::session::session_metadata_from_entries(store.session_id(), *entries);
}

std::vector<std::filesystem::path> Session::ava_authority_roots_1() const
{
  std::vector<std::filesystem::path> roots;
  roots.reserve(kMaxCommandAuthorityRoots);
  append_command_authority_root(roots, paths().ava_config_dir);
  append_command_authority_root(roots, paths().ava_state_dir);
  append_command_authority_root(roots, paths().sessions_dir);
  append_command_authority_root(roots, paths().auth_file);
  append_command_authority_root(roots, ava::config::legacy_ava_credentials_path());
  append_command_authority_root(roots, ava::config::legacy_compatible_auth_path());
  // Preserve the exact active store parent as a fallback for custom/test path
  // sets whose broader sessions directory is empty or disjoint. This path is
  // derived from the active store, never reconstructed from a session ID.
  append_command_authority_root(roots, store.session_path().parent_path());
  return roots;
}

std::string Session::state_result_json_1(bool cancel_requested) const
{
  return rpc::detail::SessionResultSerializer({}, *this).state_result_json(cancel_requested);
}

ava::core::Result<std::string> Session::messages_result_json() const
{
  return rpc::detail::MessagesResultSerializer({}, *this).run();
}

ava::core::Result<std::string> Session::list_sessions_result_json_1() const
{
  return rpc::detail::SessionResultSerializer({}, *this).list_sessions_result_json();
}

ava::core::Result<std::string> Session::tree_result_json() const
{
  return rpc::detail::SessionResultSerializer({}, *this).session_tree_result_json();
}

ava::core::Result<std::string> Session::list_models_result_json_1() const
{
  return rpc::detail::SessionResultSerializer({}, *this).list_models_result_json();
}

ava::core::Result<std::string> Session::stats_result_json() const
{
  return rpc::detail::SessionResultSerializer({}, *this).session_stats_result_json();
}

ava::core::Result<std::string> Session::validation_result_json() const
{
  return rpc::detail::SessionResultSerializer({}, *this).session_validation_result_json();
}

}  // namespace ava::app::runtime
