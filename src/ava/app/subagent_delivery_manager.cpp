#include "sys.h"
#ifdef CWDEBUG
#include "ava/debug/debug_ostream_operators.h"
#endif
#include "ava/http/transport.h"
#include "ava/app/headless_policy.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime/session_ts.h"
#include "ava/app/subagent_delivery_manager.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission_rules.h"
#include "ava/core/fingerprint.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/path.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include "debug.h"

#ifdef CWDEBUG
#include <utils/at_scope_end.h>
#endif

namespace ava::app {
namespace {

constexpr std::string_view kDeliveryMarkerPrefix = "[AVA_SUBAGENT_DELIVERY_V1 delivery_id=";

class BoundedDeliveryTransport final : public ava::http::Transport
{
 public:
  BoundedDeliveryTransport(std::unique_ptr<ava::http::Transport> inner, std::stop_token stop_token, std::chrono::steady_clock::time_point deadline)
      : inner_(std::move(inner)), stop_token_(stop_token), deadline_(deadline)
  {
  }

  ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override { return send(request, nullptr); }

  ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request, CancelCallback cancel_requested) override
  {
    auto bounded = bounded_request(request);
    return inner_->send(bounded, [this, cancel_requested] { return canceled() || (cancel_requested && cancel_requested()); });
  }

  bool supports_streaming() const noexcept override { return inner_->supports_streaming(); }

  ava::core::Result<ava::http::HttpResponse> send_streaming(ava::http::HttpRequest const& request, BodyChunkSink on_body_chunk,
                                                            CancelCallback cancel_requested = nullptr) override
  {
    auto bounded = bounded_request(request);
    return inner_->send_streaming(bounded, std::move(on_body_chunk),
                                  [this, cancel_requested] { return canceled() || (cancel_requested && cancel_requested()); });
  }

 private:
  ava::http::HttpRequest bounded_request(ava::http::HttpRequest request) const
  {
    auto const now = std::chrono::steady_clock::now();
    auto const remaining = now < deadline_ ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now).count() : 0;
    auto const bounded_ms = static_cast<int>(std::clamp<long long>(remaining, 1, std::numeric_limits<int>::max()));
    request.timeout_ms = request.timeout_ms > 0 ? std::min(request.timeout_ms, bounded_ms) : bounded_ms;
    return request;
  }

  bool canceled() const noexcept { return stop_token_.stop_requested() || std::chrono::steady_clock::now() >= deadline_; }

  std::unique_ptr<ava::http::Transport> inner_;
  std::stop_token stop_token_;
  std::chrono::steady_clock::time_point deadline_;
};

bool delivery_pending(ava::agent::SubagentJobSnapshot const& job)
{
  return job.delivery == ava::agent::SubagentDeliveryState::Pending || job.delivery == ava::agent::SubagentDeliveryState::Attempting;
}

bool execution_live(ava::agent::SubagentExecutionState state)
{
  return state == ava::agent::SubagentExecutionState::Starting || state == ava::agent::SubagentExecutionState::Running;
}

std::filesystem::path normalized_workspace_identity(std::filesystem::path const& workspace)
{
  return ava::core::normalized_absolute_path(workspace);
}

std::string normalized_workspace_key(std::filesystem::path const& workspace)
{
  return normalized_workspace_identity(workspace).generic_string();
}

ava::core::Error workspace_transaction_error(std::string_view conflict)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "workspace authority transaction is unavailable");
  error.with_context("workspace_conflict", std::string(conflict));
  return error;
}

ava::core::Error reopen_required_error()
{
  auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "retained runtime authority is no longer current");
  error.with_context("recovery", "reopen the session");
  return error;
}

ava::core::Error retained_owner_not_found(std::string_view session_id)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::NotFound, "retained session not found for this workspace");
  error.with_context("session_id", std::string(session_id));
  return error;
}

void clear_secret(std::string& secret) noexcept
{
  std::fill(secret.begin(), secret.end(), '\0');
  secret.clear();
}

runtime::RunOptions detached_run_options(runtime::RunOptions const& source)
{
  runtime::RunOptions result;
  result.access_token = source.access_token;
  result.credential_type = source.credential_type;
  result.openai_oauth = source.openai_oauth;
  result.openai_account_id = source.openai_account_id;
  result.stream = source.stream;
  result.enable_transport_retries = source.enable_transport_retries;
  result.exact_builtin_tool_names = std::vector<std::string>{};
  result.isolate_ambient_extensions = true;
  result.disable_session_mcp = true;
  result.require_descriptor_secure_workspace = source.require_descriptor_secure_workspace;
  result.announce_execution_after_permission = source.announce_execution_after_permission;
  result.redact_permission_audit_arguments = source.redact_permission_audit_arguments;
  result.require_explicit_file_permissions = source.require_explicit_file_permissions;
  result.exact_file_access = nullptr;
  result.command_executor = nullptr;
  result.offline = source.offline;
  result.expand_prompt_file_references = false;
  return result;
}

std::string delivery_marker(ava::agent::SubagentJobIdentity const& identity)
{
  return std::string(kDeliveryMarkerPrefix) + identity.delivery_id + "]";
}

std::string delivery_prompt(ava::agent::SubagentJobSnapshot const& job)
{
  std::string prompt = delivery_marker(job.identity);
  prompt += "\nAutomatic subagent completion delivery. Integrate this result into the parent conversation exactly once. ";
  prompt += "If this delivery marker already appears earlier, do not duplicate work; briefly acknowledge the existing integration.\n";
  prompt += "delivery_id: " + job.identity.delivery_id;
  prompt += "\njob_id: " + job.identity.job_id;
  prompt += "\ntask_id: " + job.identity.task_id;
  prompt += "\nchild_session_id: " + job.identity.child_session_id;
  prompt += "\nstate: " + std::string(ava::agent::to_string(job.execution));
  prompt += "\nsummary_truncated: " + std::string(job.summary_truncated ? "true" : "false");
  prompt += "\nerror_truncated: " + std::string(job.error_truncated ? "true" : "false");
  prompt += "\nstop_reason_truncated: " + std::string(job.stop_reason_truncated ? "true" : "false");
  prompt += "\nprovider_iterations: " + std::to_string(job.provider_iterations);
  prompt += "\ntool_calls: " + std::to_string(job.tool_calls);
  prompt += "\ntool_iterations: " + std::to_string(job.tool_iterations);
  if (job.execution == ava::agent::SubagentExecutionState::Completed)
    prompt += "\ncompleted_summary:\n" + job.summary.value_or("(completed without a summary)");
  else
    prompt += "\nstatus: The child did not complete successfully. Refer to child session " + job.identity.child_session_id +
              " for the canonical explicit result; no raw provider or tool error is included in this delivery.";
  return prompt;
}

std::string prompt_fingerprint(std::string_view prompt)
{
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << ava::core::content_fingerprint(prompt);
  return out.str();
}

std::optional<std::string> committed_delivery_turn(std::vector<ava::session::SessionEntry> const& entries, std::string_view delivery_id,
                                                   std::string_view fingerprint)
{
  for (std::size_t index = 0; index < entries.size(); ++index)
  {
    auto const& entry = entries[index];
    if (entry.type != ava::session::EntryType::UserMessage)
      continue;
    auto provenance = ava::session::parse_synthetic_delivery_provenance(entry);
    if (!provenance || !*provenance || (*provenance)->source != ava::session::kSyntheticSubagentDeliverySource || (*provenance)->delivery_id != delivery_id ||
        (*provenance)->prompt_fingerprint != fingerprint)
      continue;
    for (std::size_t following = index + 1; following < entries.size(); ++following)
    {
      if (entries[following].type == ava::session::EntryType::UserMessage && !ava::session::is_internal_replay_user_message(entries[following]))
        break;
      if (entries[following].type == ava::session::EntryType::AssistantTurnCommit)
      {
        auto commit = ava::session::parse_assistant_turn_commit(entries[following]);
        bool has_function_call = false;
        if (commit)
        {
          for (std::size_t item_index = index + 1; item_index < following; ++item_index)
          {
            if (entries[item_index].type != ava::session::EntryType::AssistantOutputItem)
              continue;
            auto item = ava::session::parse_assistant_output_item(entries[item_index]);
            if (item && item->assistant_turn_id == commit->assistant_turn_id && item->kind == ava::session::AssistantOutputItemKind::FunctionCall)
            {
              has_function_call = true;
              break;
            }
          }
        }
        if (commit && commit->finish_reason != "tool_calls" && !has_function_call)
          return entries[following].id;
      }
    }
  }
  return std::nullopt;
}

}  // namespace

struct SubagentDeliveryManager::ParentCapsule
{
  ParentCapsule(runtime::Session_aggregate_base&& detached_state, runtime::RunOptions options_in, CapsuleGeneration generation_in, std::string workspace_key_in)
      : unlocked_session(std::move(detached_state)), run_options(std::move(options_in)), generation(generation_in), workspace_key(std::move(workspace_key_in))
  {
  }
  // Background delivery retains only this detached, independently leased copy; every access below is scoped through rat/wat guards.
  runtime::session_ts unlocked_session;
  runtime::RunOptions run_options;
  CapsuleGeneration generation = 0;
  std::string workspace_key;

  ~ParentCapsule() { clear_secret(run_options.access_token); }
};

struct SubagentDeliveryManager::WorkspaceRecord
{
  struct ControllerRegistration
  {
    std::weak_ptr<SessionRunController> controller;
    std::string session_id;
  };

  std::vector<ControllerRegistration> controllers;
  bool maintenance_reserved = false;
  std::uint64_t maintenance_generation = 0;
  std::size_t navigation_reservations = 0;
  std::size_t in_flight_deliveries = 0;
};

struct SubagentDeliveryManager::TrustMutationReservation::Impl
{
  std::shared_ptr<SubagentDeliveryManager> manager;
  std::string workspace_key;
};

struct SubagentDeliveryManager::WorkspaceNavigationReservation::Impl
{
  std::shared_ptr<SubagentDeliveryManager> manager;
  std::string workspace_key;
};

struct SubagentDeliveryManager::WorkspaceMaintenanceReservation::Impl
{
  std::shared_ptr<SubagentDeliveryManager> manager;
  std::string workspace_key;
  std::uint64_t generation = 0;
  std::vector<std::string> parent_session_ids;
  std::vector<std::shared_ptr<SessionRunController>> controllers;
  std::vector<SessionMaintenanceReservation> controller_reservations;
  std::optional<ava::agent::SubagentCoordinatorMaintenanceReservation> coordinator_reservation;
  std::shared_ptr<SessionRunController> fresh_controller;
  std::optional<SessionMaintenanceReservation> fresh_reservation;
  bool persistence_committed = false;
  bool manager_state_committed = false;
};

SubagentDeliveryManager::SubagentDeliveryManager(SubagentDeliveryManagerOptions options) : options_(std::move(options)), coordinator_(options_.coordinator)
{
  if (!options_.provider_bundle_factory)
    options_.provider_bundle_factory = create_runtime_provider_run_bundle;
}

ava::core::Result<std::shared_ptr<SubagentDeliveryManager>> SubagentDeliveryManager::create(SubagentDeliveryManagerOptions options)
{
  if (!options.coordinator)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent delivery manager requires a coordinator"));
  if (options.max_queued_deliveries == 0 || options.max_retained_parents == 0 || options.max_delivery_attempts < 2 ||
      options.max_delivery_attempts > ava::agent::kMaxSubagentDeliveryAttemptHistory || options.delivery_deadline.count() <= 0 ||
      options.admission_retry_interval.count() <= 0)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument,
                                            "subagent delivery manager limits must be positive with between two and 64 delivery attempts"));
  auto manager = std::shared_ptr<SubagentDeliveryManager>(new SubagentDeliveryManager(std::move(options)));
  manager->start();
  return manager;
}

void SubagentDeliveryManager::start()
{
  std::weak_ptr<SubagentDeliveryManager> weak = shared_from_this();
  coordinator_->set_terminal_sink([weak](ava::agent::SubagentCoordinatorJobSnapshot const& snapshot) {
    if (auto manager = weak.lock())
      manager->enqueue(snapshot);
  });
  worker_ = ava::core::JoinThread::create("SA_delivery", [this](std::stop_token stop_token) { worker_loop(stop_token); });
}

SubagentDeliveryManager::~SubagentDeliveryManager()
{
  shutdown();
}

std::shared_ptr<ava::agent::SubagentCoordinator> const& SubagentDeliveryManager::coordinator() const noexcept
{
  return coordinator_;
}

SubagentDeliveryManager::TrustMutationReservation::TrustMutationReservation(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
}

SubagentDeliveryManager::TrustMutationReservation::~TrustMutationReservation()
{
  if (!impl_)
    return;
  try
  {
    std::lock_guard lock(impl_->manager->mutex_);
    impl_->manager->trust_mutation_active_ = false;
    impl_->manager->changed_.notify_all();
  }
  catch (...)
  {
  }
}

SubagentDeliveryManager::TrustMutationReservation::TrustMutationReservation(TrustMutationReservation&& other) noexcept = default;
SubagentDeliveryManager::TrustMutationReservation& SubagentDeliveryManager::TrustMutationReservation::operator=(TrustMutationReservation&& other) noexcept
{
  if (this != &other)
  {
    TrustMutationReservation replacement(std::move(other));
    impl_.swap(replacement.impl_);
  }
  return *this;
}

bool SubagentDeliveryManager::TrustMutationReservation::active() const noexcept
{
  if (!impl_)
    return false;
  std::lock_guard lock(impl_->manager->mutex_);
  return impl_->manager->trust_mutation_active_;
}

SubagentDeliveryManager::WorkspaceNavigationReservation::WorkspaceNavigationReservation(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
}

SubagentDeliveryManager::WorkspaceNavigationReservation::~WorkspaceNavigationReservation()
{
  if (!impl_)
    return;
  try
  {
    std::lock_guard lock(impl_->manager->mutex_);
    auto found = impl_->manager->workspaces_.find(impl_->workspace_key);
    if (found != impl_->manager->workspaces_.end() && found->second->navigation_reservations > 0)
      --found->second->navigation_reservations;
    impl_->manager->changed_.notify_all();
  }
  catch (...)
  {
  }
}

SubagentDeliveryManager::WorkspaceNavigationReservation::WorkspaceNavigationReservation(WorkspaceNavigationReservation&& other) noexcept = default;
SubagentDeliveryManager::WorkspaceNavigationReservation& SubagentDeliveryManager::WorkspaceNavigationReservation::operator=(
    WorkspaceNavigationReservation&& other) noexcept
{
  if (this != &other)
  {
    WorkspaceNavigationReservation replacement(std::move(other));
    impl_.swap(replacement.impl_);
  }
  return *this;
}

bool SubagentDeliveryManager::WorkspaceNavigationReservation::active() const noexcept
{
  if (!impl_)
    return false;
  std::lock_guard lock(impl_->manager->mutex_);
  auto found = impl_->manager->workspaces_.find(impl_->workspace_key);
  return found != impl_->manager->workspaces_.end() && found->second->navigation_reservations > 0;
}

SubagentDeliveryManager::WorkspaceMaintenanceReservation::WorkspaceMaintenanceReservation(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
}

SubagentDeliveryManager::WorkspaceMaintenanceReservation::~WorkspaceMaintenanceReservation()
{
  if (!impl_)
    return;
  if (impl_->persistence_committed && !impl_->manager_state_committed)
    fail_closed();

  // Fresh authority is released only after commit/fail-closed purged every
  // stale capsule and queue entry for this workspace.
  impl_->fresh_reservation.reset();
  impl_->controller_reservations.clear();
  impl_->coordinator_reservation.reset();
  try
  {
    std::lock_guard lock(impl_->manager->mutex_);
    auto found = impl_->manager->workspaces_.find(impl_->workspace_key);
    if (found != impl_->manager->workspaces_.end() && found->second->maintenance_reserved && found->second->maintenance_generation == impl_->generation)
      found->second->maintenance_reserved = false;
    impl_->manager->changed_.notify_all();
  }
  catch (...)
  {
  }
}

SubagentDeliveryManager::WorkspaceMaintenanceReservation::WorkspaceMaintenanceReservation(WorkspaceMaintenanceReservation&& other) noexcept = default;
SubagentDeliveryManager::WorkspaceMaintenanceReservation& SubagentDeliveryManager::WorkspaceMaintenanceReservation::operator=(
    WorkspaceMaintenanceReservation&& other) noexcept
{
  if (this != &other)
  {
    WorkspaceMaintenanceReservation replacement(std::move(other));
    impl_.swap(replacement.impl_);
  }
  return *this;
}

bool SubagentDeliveryManager::WorkspaceMaintenanceReservation::active() const noexcept
{
  if (!impl_)
    return false;
  std::lock_guard lock(impl_->manager->mutex_);
  auto found = impl_->manager->workspaces_.find(impl_->workspace_key);
  return found != impl_->manager->workspaces_.end() && found->second->maintenance_reserved && found->second->maintenance_generation == impl_->generation;
}

ava::core::Result<std::shared_ptr<SessionRunController>> SubagentDeliveryManager::WorkspaceMaintenanceReservation::prepare_fresh_controller(
    std::shared_ptr<ava::session::SessionAppendTarget> append_target)
{
  if (!impl_ || !active() || !append_target)
    return std::unexpected(workspace_transaction_error("fresh_controller_unavailable"));
  if (impl_->fresh_controller)
    return std::unexpected(workspace_transaction_error("fresh_controller_already_prepared"));
  try
  {
    auto controller = std::make_shared<SessionRunController>(std::move(append_target));
    auto reserved = controller->reserve_maintenance();
    if (!reserved)
      return std::unexpected(workspace_transaction_error("fresh_controller_reservation_failed"));
    impl_->fresh_controller = controller;
    impl_->fresh_reservation.emplace(std::move(*reserved));
    return controller;
  }
  catch (...)
  {
    return std::unexpected(workspace_transaction_error("fresh_controller_allocation_failed"));
  }
}

void SubagentDeliveryManager::WorkspaceMaintenanceReservation::mark_persistence_committed() noexcept
{
  if (impl_)
    impl_->persistence_committed = true;
}

ava::core::VoidResult SubagentDeliveryManager::WorkspaceMaintenanceReservation::retire_registered_controllers()
{
  if (!impl_ || !impl_->persistence_committed)
    return std::unexpected(workspace_transaction_error("persistence_not_committed"));
  bool failed = false;
  for (std::size_t index = 0; index < impl_->controllers.size(); ++index)
  {
    if (index >= impl_->controller_reservations.size() || !impl_->controllers[index]->retire_authority(impl_->controller_reservations[index]))
      failed = true;
  }
  if (failed)
    return std::unexpected(workspace_transaction_error("controller_retirement_failed"));
  if (impl_->manager->options_.revocation_after_retirement_for_test)
  {
    try
    {
      impl_->manager->options_.revocation_after_retirement_for_test();
    }
    catch (...)
    {
      return std::unexpected(workspace_transaction_error("retirement_hook_failed"));
    }
  }
  return {};
}

ava::core::VoidResult SubagentDeliveryManager::WorkspaceMaintenanceReservation::run_before_publication_test_hook()
{
  if (!impl_ || !impl_->persistence_committed)
    return std::unexpected(workspace_transaction_error("persistence_not_committed"));
  if (!impl_->manager->options_.revocation_before_publication_for_test)
    return {};
  try
  {
    auto result = impl_->manager->options_.revocation_before_publication_for_test();
    if (!result)
      return std::unexpected(workspace_transaction_error("publication_rejected"));
    return {};
  }
  catch (...)
  {
    return std::unexpected(workspace_transaction_error("publication_rejected"));
  }
}

ava::core::VoidResult SubagentDeliveryManager::run_construction_after_trusted_prompt_resolution_test_hook()
{
  if (!options_.construction_after_trusted_prompt_resolution_for_test)
    return {};
  try
  {
    options_.construction_after_trusted_prompt_resolution_for_test();
    return {};
  }
  catch (...)
  {
    return std::unexpected(workspace_transaction_error("construction_hook_failed"));
  }
}

ava::core::VoidResult SubagentDeliveryManager::register_workspace_controller(std::filesystem::path const& workspace_identity, std::string_view session_id,
                                                                             std::shared_ptr<SessionRunController> const& controller)
{
  if (!controller || controller->authority_retired())
    return std::unexpected(reopen_required_error());
  auto const workspace_key = normalized_workspace_key(workspace_identity);
  std::lock_guard lock(mutex_);
  if (!accepting_)
    return std::unexpected(workspace_transaction_error("manager_unavailable"));
  auto& record = workspaces_[workspace_key];
  if (!record)
    record = std::make_shared<WorkspaceRecord>();
  if (record->maintenance_reserved)
    return std::unexpected(workspace_transaction_error("maintenance_reserved"));
  std::erase_if(record->controllers, [](auto const& registration) { return registration.controller.expired(); });
  if (!session_id.empty())
  {
    auto existing = session_workspaces_.find(std::string(session_id));
    if (existing != session_workspaces_.end() && existing->second != workspace_key)
      return std::unexpected(workspace_transaction_error("session_workspace_mismatch"));
  }
  auto duplicate = std::ranges::any_of(record->controllers, [&](auto const& registration) {
    auto retained = registration.controller.lock();
    return retained == controller && registration.session_id == session_id;
  });
  if (!duplicate)
    record->controllers.push_back({.controller = controller, .session_id = std::string(session_id)});
  if (!session_id.empty())
    session_workspaces_.insert_or_assign(std::string(session_id), workspace_key);
  return {};
}

ava::core::Result<SubagentDeliveryManager::TrustMutationReservation> SubagentDeliveryManager::reserve_trust_mutation(
    std::filesystem::path const& workspace_identity)
{
  auto const workspace_key = normalized_workspace_key(workspace_identity);
  std::lock_guard lock(mutex_);
  if (!accepting_)
    return std::unexpected(workspace_transaction_error("manager_unavailable"));
  if (trust_mutation_active_)
    return std::unexpected(workspace_transaction_error("trust_mutation_active"));
  trust_mutation_active_ = true;
  auto impl = std::make_unique<TrustMutationReservation::Impl>();
  impl->manager = shared_from_this();
  impl->workspace_key = workspace_key;
  return TrustMutationReservation(std::move(impl));
}

ava::core::Result<SubagentDeliveryManager::WorkspaceNavigationReservation> SubagentDeliveryManager::reserve_workspace_navigation(
    std::filesystem::path const& workspace_identity)
{
  auto const workspace_key = normalized_workspace_key(workspace_identity);
  std::lock_guard lock(mutex_);
  if (!accepting_)
    return std::unexpected(workspace_transaction_error("manager_unavailable"));
  auto& record = workspaces_[workspace_key];
  if (!record)
    record = std::make_shared<WorkspaceRecord>();
  if (record->maintenance_reserved)
    return std::unexpected(workspace_transaction_error("maintenance_reserved"));
  ++record->navigation_reservations;
  auto impl = std::make_unique<WorkspaceNavigationReservation::Impl>();
  impl->manager = shared_from_this();
  impl->workspace_key = workspace_key;
  return WorkspaceNavigationReservation(std::move(impl));
}

ava::core::Result<SubagentDeliveryManager::WorkspaceMaintenanceReservation> SubagentDeliveryManager::reserve_workspace_maintenance(
    TrustMutationReservation const& trust_reservation, std::filesystem::path const& workspace_identity, std::string_view current_session_id,
    std::shared_ptr<SessionRunController> const& current_controller)
{
  auto const workspace_key = normalized_workspace_key(workspace_identity);
  if (!trust_reservation.impl_ || trust_reservation.impl_->manager.get() != this || trust_reservation.impl_->workspace_key != workspace_key ||
      !current_controller || current_controller->authority_retired())
    return std::unexpected(workspace_transaction_error("trust_reservation_mismatch"));

  auto impl = std::make_unique<WorkspaceMaintenanceReservation::Impl>();
  impl->manager = shared_from_this();
  impl->workspace_key = workspace_key;
  {
    std::lock_guard lock(mutex_);
    if (!accepting_ || !trust_mutation_active_)
      return std::unexpected(workspace_transaction_error("manager_unavailable"));
    auto& record = workspaces_[workspace_key];
    if (!record)
      record = std::make_shared<WorkspaceRecord>();
    std::erase_if(record->controllers, [](auto const& registration) { return registration.controller.expired(); });
    auto current_registered = std::ranges::any_of(record->controllers, [&](auto const& registration) {
      return registration.controller.lock() == current_controller && registration.session_id == current_session_id;
    });
    if (!current_registered)
      record->controllers.push_back({.controller = current_controller, .session_id = std::string(current_session_id)});
    if (record->maintenance_reserved || record->navigation_reservations > 0 || record->in_flight_deliveries > 0)
      return std::unexpected(workspace_transaction_error(record->in_flight_deliveries > 0 ? "delivery_in_flight" : "workspace_busy"));

    record->maintenance_reserved = true;
    record->maintenance_generation = next_workspace_generation_++;
    if (next_workspace_generation_ == 0)
      next_workspace_generation_ = 1;
    impl->generation = record->maintenance_generation;
    for (auto const& registration : record->controllers)
    {
      auto controller = registration.controller.lock();
      if (!controller)
        continue;
      if (!std::ranges::contains(impl->controllers, controller))
        impl->controllers.push_back(std::move(controller));
      if (!registration.session_id.empty() && !std::ranges::contains(impl->parent_session_ids, registration.session_id))
        impl->parent_session_ids.push_back(registration.session_id);
    }
    for (auto const& [parent_id, capsule] : parents_)
      if (capsule->workspace_key == workspace_key && !std::ranges::contains(impl->parent_session_ids, parent_id))
        impl->parent_session_ids.push_back(parent_id);
    for (auto const& queued : queue_)
    {
      auto mapped = session_workspaces_.find(queued.job.identity.parent_session_id);
      if (mapped != session_workspaces_.end() && mapped->second == workspace_key &&
          !std::ranges::contains(impl->parent_session_ids, queued.job.identity.parent_session_id))
        impl->parent_session_ids.push_back(queued.job.identity.parent_session_id);
    }
  }

  WorkspaceMaintenanceReservation reservation(std::move(impl));
  auto coordinator_reservation = coordinator_->reserve_parent_maintenance(reservation.impl_->parent_session_ids);
  if (!coordinator_reservation)
    return std::unexpected(std::move(coordinator_reservation.error()));
  reservation.impl_->coordinator_reservation.emplace(std::move(*coordinator_reservation));
  reservation.impl_->controller_reservations.reserve(reservation.impl_->controllers.size());
  for (auto const& controller : reservation.impl_->controllers)
  {
    auto controller_reservation = controller->reserve_maintenance();
    if (!controller_reservation)
      return std::unexpected(std::move(controller_reservation.error()));
    reservation.impl_->controller_reservations.push_back(std::move(*controller_reservation));
  }
  return ava::core::Result<WorkspaceMaintenanceReservation>(std::in_place, std::move(reservation));
}

ava::core::VoidResult SubagentDeliveryManager::WorkspaceMaintenanceReservation::commit_after_publication(std::string_view current_session_id)
{
  if (!impl_ || !impl_->persistence_committed || !impl_->fresh_controller || !impl_->fresh_reservation || !impl_->fresh_reservation->active())
    return std::unexpected(workspace_transaction_error("fresh_authority_unavailable"));

  std::vector<ava::agent::SubagentCoordinatorJobSnapshot> pending;
  for (auto const& parent : impl_->parent_session_ids)
  {
    auto parent_pending = impl_->manager->coordinator_->pending_deliveries(parent);
    if (parent_pending)
      pending.insert(pending.end(), parent_pending->begin(), parent_pending->end());
  }

  std::unordered_map<std::string, std::shared_ptr<ParentCapsule>> released;
  try
  {
    std::lock_guard lock(impl_->manager->mutex_);
    auto record_it = impl_->manager->workspaces_.find(impl_->workspace_key);
    if (record_it == impl_->manager->workspaces_.end() || !record_it->second->maintenance_reserved ||
        record_it->second->maintenance_generation != impl_->generation)
      return std::unexpected(workspace_transaction_error("maintenance_lost"));

    for (auto const& snapshot : pending)
      impl_->manager->unavailable_deliveries_.insert(snapshot.job.identity.delivery_id);
    std::erase_if(impl_->manager->queue_, [&](auto const& queued) {
      auto mapped = impl_->manager->session_workspaces_.find(queued.job.identity.parent_session_id);
      bool const matches = mapped != impl_->manager->session_workspaces_.end() && mapped->second == impl_->workspace_key;
      if (matches)
        impl_->manager->unavailable_deliveries_.insert(queued.job.identity.delivery_id);
      return matches;
    });
    for (auto parent = impl_->manager->parents_.begin(); parent != impl_->manager->parents_.end();)
    {
      if (parent->second->workspace_key != impl_->workspace_key)
      {
        ++parent;
        continue;
      }
      released.emplace(parent->first, std::move(parent->second));
      impl_->manager->detached_parents_.erase(parent->first);
      parent = impl_->manager->parents_.erase(parent);
    }
    for (auto const& parent : impl_->parent_session_ids)
    {
      impl_->manager->detached_parents_.erase(parent);
      impl_->manager->session_workspaces_.erase(parent);
    }
    record_it->second->controllers.clear();
    record_it->second->controllers.push_back({.controller = impl_->fresh_controller, .session_id = std::string(current_session_id)});
    if (!current_session_id.empty())
      impl_->manager->session_workspaces_.insert_or_assign(std::string(current_session_id), impl_->workspace_key);
    impl_->manager_state_committed = true;
    impl_->manager->changed_.notify_all();
  }
  catch (...)
  {
    return std::unexpected(workspace_transaction_error("manager_publication_failed"));
  }

  // Destroy purged capsules (and their detached leases) outside the manager
  // lock before the freshly published controller can accept work.
  released.clear();
  impl_->fresh_reservation.reset();
  return {};
}

void SubagentDeliveryManager::WorkspaceMaintenanceReservation::fail_closed() noexcept
{
  if (!impl_ || impl_->manager_state_committed)
    return;
  try
  {
    for (std::size_t index = 0; index < impl_->controllers.size() && index < impl_->controller_reservations.size(); ++index)
      static_cast<void>(impl_->controllers[index]->retire_authority(impl_->controller_reservations[index]));
    if (impl_->fresh_controller && impl_->fresh_reservation)
      static_cast<void>(impl_->fresh_controller->retire_authority(*impl_->fresh_reservation));

    std::vector<ava::agent::SubagentCoordinatorJobSnapshot> pending;
    for (auto const& parent : impl_->parent_session_ids)
    {
      auto parent_pending = impl_->manager->coordinator_->pending_deliveries(parent);
      if (parent_pending)
        pending.insert(pending.end(), parent_pending->begin(), parent_pending->end());
    }

    std::unordered_map<std::string, std::shared_ptr<ParentCapsule>> released;
    {
      std::lock_guard lock(impl_->manager->mutex_);
      for (auto const& snapshot : pending)
        impl_->manager->unavailable_deliveries_.insert(snapshot.job.identity.delivery_id);
      std::erase_if(impl_->manager->queue_, [&](auto const& queued) {
        auto mapped = impl_->manager->session_workspaces_.find(queued.job.identity.parent_session_id);
        bool const matches = mapped != impl_->manager->session_workspaces_.end() && mapped->second == impl_->workspace_key;
        if (matches)
          impl_->manager->unavailable_deliveries_.insert(queued.job.identity.delivery_id);
        return matches;
      });
      for (auto parent = impl_->manager->parents_.begin(); parent != impl_->manager->parents_.end();)
      {
        if (parent->second->workspace_key != impl_->workspace_key)
        {
          ++parent;
          continue;
        }
        released.emplace(parent->first, std::move(parent->second));
        impl_->manager->detached_parents_.erase(parent->first);
        parent = impl_->manager->parents_.erase(parent);
      }
      for (auto const& parent : impl_->parent_session_ids)
      {
        impl_->manager->detached_parents_.erase(parent);
        impl_->manager->session_workspaces_.erase(parent);
      }
      auto record = impl_->manager->workspaces_.find(impl_->workspace_key);
      if (record != impl_->manager->workspaces_.end())
        record->second->controllers.clear();
      impl_->manager_state_committed = true;
      impl_->manager->changed_.notify_all();
    }
    released.clear();
    impl_->fresh_reservation.reset();
  }
  catch (...)
  {
    // Every controller retirement happens before fallible container cleanup.
    // Even an allocation failure therefore leaves all captured authority
    // permanently unrunnable and reopen-required.
  }
}

ava::core::Result<SubagentDeliveryManager::CapsuleGeneration> SubagentDeliveryManager::refresh_parent(runtime::session_ts const& unlocked_session,
                                                                                                      runtime::RunOptions const& options)
{
  DoutEntering(dc::notice, "SubagentDeliveryManager::refresh_parent()");
#ifdef CWDEBUG
  auto&& f = at_scope_end([] { Dout(dc::notice, "Leaving SubagentDeliveryManager::refresh_parent()"); });
#endif

  std::filesystem::path reserved_workspace;
  {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    reserved_workspace = session_r->workspace_dir();
  }
  auto navigation = reserve_workspace_navigation(reserved_workspace);
  if (!navigation)
    return std::unexpected(std::move(navigation.error()));

  CRITICAL_AREA_BEGIN_CR(session);

  if (normalized_workspace_identity(session_r->workspace_dir()) != normalized_workspace_identity(reserved_workspace))
    return std::unexpected(workspace_transaction_error("session_changed"));
  if (!session_r->run_controller())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "cannot retain a parent without a run controller"));
  auto authority = session_r->read_authority_1();
  if (!authority)
    return std::unexpected(std::move(authority.error()));
  ava::session::SessionLease lease;
  if (!session_r->sessionless())
  {
    auto duplicated = session_r->lease().duplicate();
    if (!duplicated)
      return std::unexpected(std::move(duplicated.error()));
    lease = std::move(*duplicated);
  }
  auto detached_state = session_r->create_detached_state(std::move(lease), *authority, nullptr);

  auto safe_options = detached_run_options(options);
  auto const session_id = session_r->store.session_id();
  auto const workspace_dir = session_r->workspace_dir();
  auto const controller = session_r->run_controller();

  CRITICAL_AREA_END_R(session);

  if (auto registered = register_workspace_controller(workspace_dir, session_id, controller); !registered)
    return std::unexpected(std::move(registered.error()));
  auto const workspace_key = normalized_workspace_key(workspace_dir);
  CapsuleGeneration published_generation = 0;
  {
    std::lock_guard lock(mutex_);
    if (!accepting_)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent delivery manager is shutting down"));
    auto workspace = workspaces_.find(workspace_key);
    if (workspace == workspaces_.end() || workspace->second->maintenance_reserved)
      return std::unexpected(workspace_transaction_error("maintenance_reserved"));
    auto found = parents_.find(session_id);
    if (found == parents_.end() && parents_.size() >= options_.max_retained_parents)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "automatic subagent delivery parent retention limit reached"));
    published_generation = next_generation_++;
    if (next_generation_ == 0)
      next_generation_ = 1;
    auto refreshed = std::make_shared<ParentCapsule>(std::move(detached_state), std::move(safe_options), published_generation, workspace_key);
    if (found == parents_.end())
      parents_.emplace(session_id, std::move(refreshed));
    else
    {
      // Capsules are immutable after publication. A delivery already holding
      // the previous generation completes safely while this fresh generation
      // becomes visible to later work.
      found->second = std::move(refreshed);
    }
  }
  auto pending = coordinator_->pending_deliveries(session_id);
  if (pending)
    for (auto const& delivery : *pending)
      enqueue(delivery);
  changed_.notify_all();
  return published_generation;
}

bool SubagentDeliveryManager::parent_needed(std::string_view session_id) const
{
  auto jobs = coordinator_->list(session_id);
  if (std::ranges::any_of(jobs, [](auto const& snapshot) { return execution_live(snapshot.job.execution); }))
    return true;
  auto pending = coordinator_->pending_deliveries(session_id);
  return pending && !pending->empty();
}

ava::core::VoidResult SubagentDeliveryManager::refresh_parent_configuration(runtime::session_ts const& unlocked_session)
{
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling SubagentDeliveryManager::refresh_parent_configuration");

  std::filesystem::path reserved_workspace;
  {
    SCOPED_CRITICAL_AREA_CR(session_r, unlocked_session);
    reserved_workspace = session_r->workspace_dir();
  }
  auto navigation = reserve_workspace_navigation(reserved_workspace);
  if (!navigation)
    return std::unexpected(std::move(navigation.error()));

  CRITICAL_AREA_BEGIN_CR(session);
  if (normalized_workspace_identity(session_r->workspace_dir()) != normalized_workspace_identity(reserved_workspace))
    return std::unexpected(workspace_transaction_error("session_changed"));
  auto const session_id = session_r->store.session_id();
  auto const workspace_key = normalized_workspace_key(session_r->workspace_dir());
  CRITICAL_AREA_END_R(session);

  std::shared_ptr<ParentCapsule> retained;
  {
    std::lock_guard lock(mutex_);
    auto found = parents_.find(session_id);
    if (found == parents_.end())
      return {};
    auto workspace = workspaces_.find(workspace_key);
    if (workspace == workspaces_.end() || workspace->second->maintenance_reserved)
      return std::unexpected(workspace_transaction_error("maintenance_reserved"));
    retained = found->second;
  }

  CRITICAL_AREA_CONTINUE_R(session);
  auto authority = session_r->read_authority_1();
  if (!authority)
    return std::unexpected(std::move(authority.error()));
  ava::session::SessionLease lease;
  if (!session_r->sessionless())
  {
    auto duplicated = session_r->lease().duplicate();
    if (!duplicated)
      return std::unexpected(std::move(duplicated.error()));
    lease = std::move(*duplicated);
  }
  auto detached_state = session_r->create_detached_state(std::move(lease), *authority, nullptr);
  auto retained_options = retained->run_options;
  CRITICAL_AREA_END_R(session);

  {
    std::lock_guard lock(mutex_);
    auto found = parents_.find(session_id);
    if (!accepting_ || found == parents_.end() || found->second != retained)
    {
      clear_secret(retained_options.access_token);
      return {};
    }
    auto const generation = next_generation_++;
    if (next_generation_ == 0)
      next_generation_ = 1;
    auto workspace = workspaces_.find(workspace_key);
    if (workspace == workspaces_.end() || workspace->second->maintenance_reserved)
    {
      clear_secret(retained_options.access_token);
      return std::unexpected(workspace_transaction_error("maintenance_reserved"));
    }
    found->second = std::make_shared<ParentCapsule>(std::move(detached_state), std::move(retained_options), generation, workspace_key);
  }
  changed_.notify_all();
  return {};
}

void SubagentDeliveryManager::release_parent_if_unused(std::string_view parent_session_id, CapsuleGeneration generation)
{
  // This function potentially destroys a ParentCapsule object, which owns a session_ts,
  // and we are not allowed to hold a session lock while destroying a Session instance.
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling SubagentDeliveryManager::release_parent_if_unused");

  auto capsule_active = [](std::shared_ptr<ParentCapsule> const& capsule) {
    SCOPED_CRITICAL_AREA_R(session_r, capsule->unlocked_session);
    return session_r->run_controller() && session_r->run_controller()->snapshot().active;
  };
  std::shared_ptr<ParentCapsule> candidate;
  {
    std::lock_guard lock(mutex_);
    auto found = parents_.find(std::string(parent_session_id));
    if (found == parents_.end() || found->second->generation != generation || capsule_active(found->second))
      return;
    candidate = found->second;
  }
  if (parent_needed(parent_session_id))
    return;
  {
    std::lock_guard lock(mutex_);
    auto found = parents_.find(std::string(parent_session_id));
    if (found == parents_.end() || found->second != candidate || found->second->generation != generation || capsule_active(found->second))
      return;
    parents_.erase(found);
    detached_parents_.erase(std::string(parent_session_id));
  }
}

void SubagentDeliveryManager::attach_parent(std::string_view parent_session_id)
{
  std::lock_guard lock(mutex_);
  detached_parents_.erase(std::string(parent_session_id));
}

void SubagentDeliveryManager::release_detached_parent(std::string_view parent_session_id)
{
  // Might call release_parent_if_unused.
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling SubagentDeliveryManager::release_detached_parent");

  std::optional<CapsuleGeneration> generation;
  {
    std::lock_guard lock(mutex_);
    if (auto found = parents_.find(std::string(parent_session_id)); found != parents_.end())
    {
      detached_parents_.insert(std::string(parent_session_id));
      generation = found->second->generation;
    }
  }
  if (generation)
    release_parent_if_unused(parent_session_id, *generation);
}

ava::core::Result<runtime::session_ts> SubagentDeliveryManager::retained_session(std::string_view session_id, std::filesystem::path const& workspace_identity,
                                                                                 bool& found, RetainedSessionOptions const& options)
{
  found = false;
  auto navigation = reserve_workspace_navigation(workspace_identity);
  if (!navigation)
  {
    found = true;
    return std::unexpected(std::move(navigation.error()));
  }

  std::shared_ptr<ParentCapsule> capsule;
  auto const expected_workspace = normalized_workspace_identity(workspace_identity);
  auto const expected_workspace_key = normalized_workspace_key(workspace_identity);
  bool saw_foreign_owner = false;
  {
    std::lock_guard lock(mutex_);
    if (!accepting_)
      return std::unexpected(retained_owner_not_found(session_id));
    auto retained = parents_.find(std::string(session_id));
    if (retained != parents_.end())
    {
      found = true;
      if (retained->second->workspace_key != expected_workspace_key)
        return std::unexpected(retained_owner_not_found(session_id));
      capsule = retained->second;
    }
    else if (!options.exact_session_id)
    {
      for (auto const& [retained_id, retained] : parents_)
      {
        if (!retained_id.starts_with(session_id))
          continue;
        if (retained->workspace_key != expected_workspace_key)
        {
          saw_foreign_owner = true;
          continue;
        }
        if (capsule)
        {
          auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session id prefix is ambiguous among retained parents");
          error.with_context("session_id", std::string(session_id));
          return std::unexpected(std::move(error));
        }
        capsule = retained;
        found = true;
      }
    }
    if (!capsule)
    {
      if (saw_foreign_owner)
      {
        found = true;
        return std::unexpected(retained_owner_not_found(session_id));
      }
      return std::unexpected(retained_owner_not_found(session_id));
    }
  }

  ava::config::XdgPaths paths;
  ProjectTrustState retained_trust;
  std::shared_ptr<SessionRunController> controller;
  std::filesystem::path retained_workspace;
  {
    SCOPED_CRITICAL_AREA_R(session_r, capsule->unlocked_session);
    paths = session_r->paths();
    retained_trust = session_r->project_trust();
    controller = session_r->run_controller();
    retained_workspace = session_r->workspace_dir();
  }
  auto const disk_trust = load_project_trust_state(paths, retained_workspace);
  bool const trust_is_current = disk_trust.decision == retained_trust.decision && disk_trust.matched_path == retained_trust.matched_path;
  if (!controller || controller->authority_retired() || !trust_is_current || normalized_workspace_identity(retained_workspace) != expected_workspace)
  {
    std::shared_ptr<ParentCapsule> released;
    {
      std::lock_guard lock(mutex_);
      auto retained = std::ranges::find_if(parents_, [&](auto const& entry) { return entry.second == capsule; });
      if (retained != parents_.end())
      {
        released = std::move(retained->second);
        detached_parents_.erase(retained->first);
        parents_.erase(retained);
      }
    }
    found = false;
    return std::unexpected(retained_owner_not_found(session_id));
  }

  std::optional<ava::session::SessionReadAuthority> bound_authority;
  auto detached_state_result = [&]() -> ava::core::Result<runtime::Session_aggregate_base> {
    SCOPED_CRITICAL_AREA_R(session_r, capsule->unlocked_session);
    ava::session::SessionLease lease;
    if (!session_r->sessionless())
    {
      auto duplicated = session_r->lease().duplicate();
      if (!duplicated)
        return std::unexpected(std::move(duplicated.error()));
      lease = std::move(*duplicated);
    }
    auto const limits = options.read_limits.value_or(session_r->session_read_limits());
    auto authority = session_r->sessionless() ? ava::session::SessionReadAuthority::create_ephemeral(session_r->store, limits)
                                              : ava::session::SessionReadAuthority::create_persistent(session_r->store, lease, limits);
    if (!authority)
      return std::unexpected(std::move(authority.error()));
    bound_authority = *authority;
    return session_r->create_detached_state(std::move(lease), *authority, shared_from_this());
  }();
  if (!detached_state_result)
    return std::unexpected(std::move(detached_state_result.error()));
  // Read only through the newly bound exact-lease authority, outside Session
  // and manager locks. Failed validation must not attach or alter the capsule.
  auto const& authority = *bound_authority;
  auto entries = authority.load();
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  if (options.expected_original_cwd)
  {
    auto summary = authority.inspect_bounded(authority.read_limits());
    if (!summary)
      return std::unexpected(std::move(summary.error()));
    auto const persisted_cwd = summary->original_cwd.empty() ? workspace_identity : summary->original_cwd;
    if (persisted_cwd != *options.expected_original_cwd)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "requested cwd does not match persisted session cwd");
      error.with_context("persisted_cwd", persisted_cwd.string()).with_context("requested_cwd", options.expected_original_cwd->string());
      return std::unexpected(std::move(error));
    }
  }
  detached_state_result->created = false;
  auto const attached_id = detached_state_result->store.session_id();

  attach_parent(attached_id);

  return ava::core::Result<runtime::session_ts>(std::in_place, std::move(*detached_state_result));
}

void SubagentDeliveryManager::enqueue(ava::agent::SubagentCoordinatorJobSnapshot const& snapshot) noexcept
{
  try
  {
    std::lock_guard lock(mutex_);
    if (!accepting_ || !delivery_pending(snapshot.job) || unavailable_deliveries_.contains(snapshot.job.identity.delivery_id))
      return;
    auto duplicate = std::ranges::any_of(queue_, [&](auto const& queued) { return queued.job.identity.delivery_id == snapshot.job.identity.delivery_id; });
    if (!duplicate)
    {
      if (queue_.size() >= options_.max_queued_deliveries && parents_.contains(snapshot.job.identity.parent_session_id))
      {
        auto unavailable = std::ranges::find_if(queue_, [&](auto const& queued) { return !parents_.contains(queued.job.identity.parent_session_id); });
        if (unavailable != queue_.end())
          queue_.erase(unavailable);
      }
      if (queue_.size() < options_.max_queued_deliveries)
        queue_.push_back(snapshot);
    }
    changed_.notify_all();
  }
  catch (...)
  {
  }
}

void SubagentDeliveryManager::worker_loop(std::stop_token stop_token)
{
  while (!stop_token.stop_requested())
  {
    ava::agent::SubagentCoordinatorJobSnapshot snapshot;
    std::shared_ptr<ParentCapsule> capsule;
    std::string in_flight_workspace_key;
    {
      std::unique_lock lock(mutex_);
      auto selectable = [&](auto const& queued) {
        if (unavailable_deliveries_.contains(queued.job.identity.delivery_id))
          return false;
        auto parent = parents_.find(queued.job.identity.parent_session_id);
        if (parent == parents_.end())
          return false;
        auto workspace = workspaces_.find(parent->second->workspace_key);
        return workspace != workspaces_.end() && !workspace->second->maintenance_reserved;
      };
      changed_.wait(lock, stop_token, [&] { return !accepting_ || std::ranges::any_of(queue_, selectable); });
      if (!accepting_ || stop_token.stop_requested())
        break;
      auto found = std::ranges::find_if(queue_, selectable);
      if (found == queue_.end())
        continue;
      snapshot = *found;
      capsule = parents_.at(snapshot.job.identity.parent_session_id);
      in_flight_workspace_key = capsule->workspace_key;
      auto workspace = workspaces_.find(in_flight_workspace_key);
      if (workspace == workspaces_.end() || workspace->second->maintenance_reserved)
        continue;
      ++workspace->second->in_flight_deliveries;
      queue_.erase(found);
    }
    try
    {
      deliver(std::move(snapshot), capsule, stop_token);
    }
    catch (...)
    {
    }
    finish_in_flight_delivery(in_flight_workspace_key);

    // A full bounded queue may have omitted a notification. Once one slot is
    // consumed, rediscover durable pending work from retained parents rather
    // than relying on a lossy callback edge.
    std::vector<std::string> parents;
    {
      std::lock_guard lock(mutex_);
      parents.reserve(parents_.size());
      for (auto const& [parent, _] : parents_)
        parents.push_back(parent);
    }
    for (auto const& parent : parents)
    {
      auto pending = coordinator_->pending_deliveries(parent);
      if (!pending)
        continue;
      for (auto const& delivery : *pending)
        enqueue(delivery);
    }
  }
}

void SubagentDeliveryManager::finish_in_flight_delivery(std::string const& workspace_key) noexcept
{
  try
  {
    std::lock_guard lock(mutex_);
    auto workspace = workspaces_.find(workspace_key);
    if (workspace != workspaces_.end() && workspace->second->in_flight_deliveries > 0)
      --workspace->second->in_flight_deliveries;
    changed_.notify_all();
  }
  catch (...)
  {
  }
}

void SubagentDeliveryManager::deliver(ava::agent::SubagentCoordinatorJobSnapshot snapshot, std::shared_ptr<ParentCapsule> const& capsule,
                                      std::stop_token stop_token)
{
  // Might call release_parent_if_unused.
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling SubagentDeliveryManager::deliver");

  auto capsule_controller = [](std::shared_ptr<ParentCapsule> const& retained) {
    SCOPED_CRITICAL_AREA_R(session_r, retained->unlocked_session);
    return session_r->run_controller();
  };

  auto selected_capsule = capsule;
  auto current = coordinator_->snapshot(snapshot.job.identity.parent_session_id, snapshot.job.identity.job_id);
  if (current)
    snapshot = *current;
  if (!delivery_pending(snapshot.job))
    return;
  auto prompt = delivery_prompt(snapshot.job);
  auto const fingerprint = prompt_fingerprint(prompt);

  auto entries = [&]() -> ava::core::Result<ava::session::SessionReadAuthority> {
    SCOPED_CRITICAL_AREA_R(session_r, selected_capsule->unlocked_session);
    return session_r->read_authority_1();
  }();
  if (!entries)
    return;
  auto history = entries->load();
  if (!history)
    return;
  if (auto committed = committed_delivery_turn(*history, snapshot.job.identity.delivery_id,
                                               snapshot.job.delivery_attempt_history.empty()
                                                   ? std::string_view{}
                                                   : std::string_view(snapshot.job.delivery_attempt_history.back().prompt_fingerprint)))
  {
    if (!snapshot.job.delivery_attempt_history.empty())
    {
      auto acknowledged = coordinator_->acknowledge_delivery(snapshot.job.identity.parent_session_id, snapshot.job.identity.job_id,
                                                             snapshot.job.delivery_attempt_history.back().attempt_id, *committed);
      if (!acknowledged)
        enqueue(snapshot);
    }
    release_parent_if_unused(snapshot.job.identity.parent_session_id, selected_capsule->generation);
    return;
  }
  if (snapshot.job.delivery_attempts >= options_.max_delivery_attempts)
  {
    if (!snapshot.job.delivery_attempt_history.empty())
      static_cast<void>(coordinator_->exhaust_delivery(snapshot.job.identity.parent_session_id, snapshot.job.identity.job_id,
                                                       snapshot.job.delivery_attempt_history.back().attempt_id));
    release_parent_if_unused(snapshot.job.identity.parent_session_id, selected_capsule->generation);
    return;
  }

  auto const attempt_id = ava::core::make_id("delivery_attempt");
  auto const delivery_run_id = ava::core::make_id("automatic_delivery");
  auto const deadline = std::chrono::steady_clock::now() + options_.delivery_deadline;
  auto selected_controller = capsule_controller(selected_capsule);
  if (!selected_controller)
    return;

  // Do not consume an attempt or append the synthetic user message while an
  // ordinary parent turn is active. Once idle is observed, record the attempt
  // before admission; if a user races and wins, this same stable attempt waits
  // and retries admission without consuming another attempt.
  while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline)
  {
    auto admission = selected_controller->inspect_admission(RunRequest{.request_id = delivery_run_id});
    if (admission == AdmissionDisposition::Admit)
      break;
    if (admission == AdmissionDisposition::RejectClosing || admission == AdmissionDisposition::RejectPersistenceFailure ||
        admission == AdmissionDisposition::RejectRetiredAuthority)
      return;
    std::this_thread::sleep_for(options_.admission_retry_interval);
  }
  if (stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
  {
    if (!stop_token.stop_requested())
      enqueue(snapshot);
    return;
  }

  if (options_.admission_preflight)
    options_.admission_preflight(stop_token);
  if (stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline)
  {
    if (!stop_token.stop_requested())
      enqueue(snapshot);
    return;
  }

  {
    std::lock_guard lock(mutex_);
    auto refreshed = parents_.find(snapshot.job.identity.parent_session_id);
    if (refreshed == parents_.end())
      return;
    selected_capsule = refreshed->second;
  }
  selected_controller = capsule_controller(selected_capsule);
  if (!selected_controller)
    return;

  auto attempted = coordinator_->record_delivery_attempt(snapshot.job.identity.parent_session_id, snapshot.job.identity.job_id, attempt_id, fingerprint);
  if (!attempted)
    return;

  std::optional<ActiveRunGuard> guard;
  while (!stop_token.stop_requested() && std::chrono::steady_clock::now() < deadline)
  {
    auto admitted = selected_controller->admit(RunRequest{.request_id = delivery_run_id});
    if (admitted)
    {
      guard.emplace(std::move(*admitted));
      break;
    }
    auto admission = selected_controller->inspect_admission(RunRequest{.request_id = delivery_run_id});
    if (admission == AdmissionDisposition::RejectClosing || admission == AdmissionDisposition::RejectPersistenceFailure ||
        admission == AdmissionDisposition::RejectRetiredAuthority)
      return;
    std::this_thread::sleep_for(options_.admission_retry_interval);
  }
  if (!guard)
  {
    if (!stop_token.stop_requested())
      enqueue(*attempted);
    return;
  }

  std::shared_ptr<ParentCapsule> refreshed_capsule;
  {
    std::lock_guard lock(mutex_);
    auto refreshed = parents_.find(snapshot.job.identity.parent_session_id);
    if (refreshed == parents_.end())
      return;
    refreshed_capsule = refreshed->second;
  }
  if (capsule_controller(refreshed_capsule) != selected_controller)
    return;
  selected_capsule = std::move(refreshed_capsule);

  auto run_options = selected_capsule->run_options;
  run_options.request_id = delivery_run_id;
  run_options.event_sink = nullptr;
  run_options.question_resolver = nullptr;
  run_options.take_steering_messages = nullptr;
  run_options.image_attachments.clear();
  run_options.synthetic_subagent_delivery = true;
  run_options.synthetic_user_message_provenance =
      ava::session::SyntheticDeliveryProvenance{.delivery_id = snapshot.job.identity.delivery_id, .prompt_fingerprint = fingerprint};
  // Automatic delivery always resolves credentials at execution time. A
  // retained token/account binding may have rotated while the parent was
  // inactive and must never be treated as current authority.
  clear_secret(run_options.access_token);
  run_options.credential_type = "bearer";
  run_options.openai_oauth = false;
  run_options.openai_account_id.clear();
  run_options.cancel_requested = [stop_token, deadline] { return stop_token.stop_requested() || std::chrono::steady_clock::now() >= deadline; };

  auto bundle = [&]() -> ava::core::Result<RuntimeProviderRunBundle> {
    run_options.permission_resolver = ava::permissions::build_persistent_permission_rule_resolver(
        runtime::session_ts::rat(selected_capsule->unlocked_session)->permission_rule_store(), build_headless_permission_resolver({}));
    return options_.provider_bundle_factory(selected_capsule->unlocked_session, std::move(run_options), "automatic subagent delivery");
  }();
  if (!bundle || !bundle->provider || !bundle->transport)
  {
    if (!stop_token.stop_requested())
      enqueue(*attempted);
    return;
  }
  BoundedDeliveryTransport bounded_transport(std::move(bundle->transport), stop_token, deadline);
  auto result = run_admitted_prompt(selected_capsule->unlocked_session, prompt, *bundle->provider, bounded_transport, bundle->options, std::move(*guard));
  if (!result || !result->committed_turn_id)
  {
    if (!stop_token.stop_requested())
      enqueue(*attempted);
    return;
  }
  auto acknowledged =
      coordinator_->acknowledge_delivery(snapshot.job.identity.parent_session_id, snapshot.job.identity.job_id, attempt_id, *result->committed_turn_id);
  if (!acknowledged)
    enqueue(*attempted);
  release_parent_if_unused(snapshot.job.identity.parent_session_id, selected_capsule->generation);
}

void SubagentDeliveryManager::shutdown() noexcept
{
  // Potentially joins a thread.
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling SubagentDeliveryManager::shutdown");

  try
  {
    std::vector<std::shared_ptr<SessionRunController>> controllers;
    {
      std::lock_guard lock(mutex_);
      if (!accepting_ && !worker_.joinable())
        return;
      accepting_ = false;
      queue_.clear();
      controllers.reserve(parents_.size());
      for (auto const& [_, capsule] : parents_)
      {
        SCOPED_CRITICAL_AREA_R(session_r, capsule->unlocked_session);
        if (session_r->run_controller())
          controllers.push_back(session_r->run_controller());
      }
    }
    // Stop callbacks may reenter application code. Never invoke them while the
    // manager mutex protects capsule/queue teardown.
    for (auto const& controller : controllers)
      static_cast<void>(controller->request_stop(StopReason::UserCanceled));
    coordinator_->set_terminal_sink(nullptr);
    changed_.notify_all();
    if (worker_.joinable())
    {
      worker_.request_stop();
      worker_.join();
    }
    coordinator_->shutdown();
    std::unordered_map<std::string, std::shared_ptr<ParentCapsule>> released;
    {
      std::lock_guard lock(mutex_);
      released.swap(parents_);
    }
  }
  catch (...)
  {
  }
}

}  // namespace ava::app
