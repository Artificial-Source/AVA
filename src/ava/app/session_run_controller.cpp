#include "sys.h"
#include "ava/observability/run_observer.h"
#include "ava/app/session_run_controller.h"
#include "ava/core/error.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>
#include "debug.h"

namespace ava::app {
namespace {

ava::core::Error transition_error(RunPhase from, RunPhase to)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "invalid run phase transition");
  error.with_context("from", std::string(to_string(from)));
  error.with_context("to", std::string(to_string(to)));
  return error;
}

ava::core::Error inactive_error()
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session has no active run");
}

ava::core::Error stop_requested_error(StopReason reason)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, reason == StopReason::UserCanceled ? "agent loop canceled" : "run stop requested",
                                reason == StopReason::UserCanceled ? ava::core::ErrorCode::Canceled : ava::core::ErrorCode::Unspecified);
  error.with_context("stop_reason", std::string(to_string(reason)));
  return error;
}

ava::core::Error stale_error()
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "stale run route");
}

ava::core::Error queue_limit_error(std::string_view queue, std::size_t limit)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session run queue limit exceeded");
  error.with_context("queue", std::string(queue));
  error.with_context("limit", std::to_string(limit));
  return error;
}

ava::core::Error compaction_pending_error()
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session already has a pending compaction append");
  error.with_context("queue", "compaction");
  error.with_context("recovery", "wait for the pending compaction append to resolve, then retry with a fresh authoritative snapshot");
  return error;
}

ava::core::Error reentrant_append_error()
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "reentrant session append is not allowed");
}

ava::core::Error maintenance_reservation_error(std::string_view reason)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "runtime session is reserved for exclusive maintenance");
  error.with_context("maintenance_conflict", std::string(reason));
  return error;
}

ava::core::Error retired_authority_error()
{
  auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "runtime session authority was retired");
  error.with_context("recovery", "reopen the session before running or writing");
  return error;
}

bool legal_transition(RunPhase from, RunPhase to)
{
  if (from == to)
    return true;
  switch (from)
  {
    case RunPhase::Admitted:
      return to == RunPhase::BuildingContext || to == RunPhase::Canceling || to == RunPhase::Failed;
    case RunPhase::BuildingContext:
      return to == RunPhase::AwaitingProvider || to == RunPhase::Compacting || to == RunPhase::Canceling || to == RunPhase::Failed;
    case RunPhase::AwaitingProvider:
      return to == RunPhase::PersistingAssistant || to == RunPhase::PreparingTools || to == RunPhase::Compacting || to == RunPhase::Completing ||
             to == RunPhase::Canceling || to == RunPhase::Failed;
    case RunPhase::PersistingAssistant:
      return to == RunPhase::PreparingTools || to == RunPhase::AwaitingProvider || to == RunPhase::Completing || to == RunPhase::Canceling ||
             to == RunPhase::Failed;
    case RunPhase::PreparingTools:
      return to == RunPhase::ExecutingTools || to == RunPhase::Canceling || to == RunPhase::Failed;
    case RunPhase::ExecutingTools:
      return to == RunPhase::SettlingTools || to == RunPhase::Canceling || to == RunPhase::Failed;
    case RunPhase::SettlingTools:
      return to == RunPhase::AwaitingProvider || to == RunPhase::Completing || to == RunPhase::Canceling || to == RunPhase::Failed;
    case RunPhase::Compacting:
      return to == RunPhase::BuildingContext || to == RunPhase::AwaitingProvider || to == RunPhase::Canceling || to == RunPhase::Failed;
    case RunPhase::Completing:
    case RunPhase::Canceling:
    case RunPhase::Failed:
      return false;
  }
  AI_NEVER_REACHED
}

struct ActiveControllerFrame
{
  void const* state = nullptr;
  ActiveControllerFrame* previous = nullptr;
};

thread_local ActiveControllerFrame* active_controller_frame = nullptr;

[[nodiscard]] bool controller_active_on_this_thread(void const* state) noexcept
{
  for (auto frame = active_controller_frame; frame; frame = frame->previous)
    if (frame->state == state)
      return true;
  return false;
}

class ActiveControllerMarker final
{
 public:
  explicit ActiveControllerMarker(void const* state) noexcept : frame_{.state = state, .previous = active_controller_frame}
  {
    active_controller_frame = &frame_;
  }
  ~ActiveControllerMarker() { active_controller_frame = frame_.previous; }
  ActiveControllerMarker(ActiveControllerMarker const&) = delete;
  ActiveControllerMarker& operator=(ActiveControllerMarker const&) = delete;

 private:
  ActiveControllerFrame frame_;
};

ava::core::Error reentrant_reset_error()
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "cannot reset persistence from a session append observer");
}

std::shared_ptr<ava::core::Error const> make_append_exception_fallback()
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Io, "session append threw an unexpected exception");
  error.with_context("append_commit_state", "partial_or_unknown");
  error.with_context("recovery", "run explicit session persistence recovery before any later append");
  return std::make_shared<ava::core::Error>(std::move(error));
}

std::shared_ptr<ava::core::Error const> make_conditional_rejection_fallback()
{
  return std::make_shared<ava::core::Error>(ava::core::ErrorCategory::Unknown, "conditional branch summary was rejected before append");
}

ava::core::Result<std::size_t> entry_byte_count(ava::session::SessionEntry const& entry)
{
  std::size_t bytes = 32;
  for (auto const size : {entry.id.size(), entry.parent_id.size(), entry.timestamp.size(), entry.data_json.size()})
  {
    if (size > std::numeric_limits<std::size_t>::max() - bytes)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session append byte accounting overflow"));
    bytes += size;
  }
  return bytes;
}

ava::core::Result<std::size_t> batch_entry_byte_count(std::vector<ava::session::SessionEntry> const& entries)
{
  if (entries.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session append batch must not be empty"));
  std::size_t bytes = 0;
  for (auto const& entry : entries)
  {
    auto entry_bytes = entry_byte_count(entry);
    if (!entry_bytes)
      return std::unexpected(std::move(entry_bytes.error()));
    if (*entry_bytes > std::numeric_limits<std::size_t>::max() - bytes)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session append batch byte accounting overflow"));
    bytes += *entry_bytes;
  }
  return bytes;
}

}  // namespace

struct ActiveRunGuard::State
{
  struct AppendItem
  {
    enum class Completion
    {
      Pending,
      Succeeded,
      Rejected,
      PersistenceFailed,
      Closing,
    };

    std::vector<ava::session::SessionEntry> entries;
    std::vector<ava::session::SessionEntry> expected;
    std::size_t bytes = 0;
    SessionAppendRequestKind kind = SessionAppendRequestKind::Ordinary;
    ava::session::SessionCancelCallback cancel_requested = nullptr;
    std::optional<ava::session::SessionConditionalAppendResult> branch_summary_result = std::nullopt;
    std::optional<ava::session::SessionCompactionAppendResult> compaction_result = std::nullopt;
    Completion completion = Completion::Pending;
    std::shared_ptr<ava::core::Error const> failure;
  };

  mutable std::mutex mutex;
  std::mutex append_mutex;
  std::condition_variable appends_drained;
  std::condition_variable outcome_changed;
  std::stop_source stop_source;
  bool active = false;
  bool maintenance_reserved = false;
  bool authority_retired = false;
  bool closing = false;
  bool shutting_down = false;
  std::uint64_t generation = 0;
  std::uint64_t maintenance_generation = 0;
  std::string run_id;
  RunPhase phase = RunPhase::Admitted;
  std::optional<StopReason> requested_stop;
  std::optional<RunOutcome> outcome;
  // Retain terminal outcomes by correlation so an A waiter cannot observe a
  // newly admitted B and lose A's result before it wakes.
  std::unordered_map<std::string, RunOutcome> outcomes;
  std::deque<std::string> outcome_order;
  std::shared_ptr<ava::core::Error const> persistence_failure;
  std::uint64_t persistence_failure_generation = 0;
  std::shared_ptr<ava::core::Error const> append_exception_fallback = make_append_exception_fallback();
  std::shared_ptr<ava::core::Error const> conditional_rejection_fallback = make_conditional_rejection_fallback();
  std::shared_ptr<ava::session::SessionAppendTarget> append_target;
  std::deque<RunCommand> commands;
  std::deque<std::shared_ptr<AppendItem>> appends;
  std::size_t append_bytes = 0;
};

SessionRunController::SessionRunController(std::shared_ptr<ava::session::SessionAppendTarget> append_target) : state_(std::make_shared<ActiveRunGuard::State>())
{
  state_->append_target = std::move(append_target);
}

SessionRunController::~SessionRunController()
{
  shutdown();
}

void SessionRunController::shutdown() noexcept
{
  auto state = state_;
  if (!state)
    return;
  try
  {
    auto const observer_callback = ava::observability::in_run_observer_callback();
    auto const active_controller = controller_active_on_this_thread(state.get());
    std::stop_source source;
    {
      std::lock_guard lock(state->mutex);
      if (!state->shutting_down)
      {
        state->shutting_down = true;
        state->closing = true;
        state->requested_stop = StopReason::UserCanceled;
        source = state->stop_source;
      }
    }
    // Stop callbacks can reenter controller APIs and therefore run without the
    // state mutex. An active driver owns finalization after its callback returns.
    static_cast<void>(source.request_stop());
    state->outcome_changed.notify_all();
    if (active_controller)
      return;

    std::unique_lock driver_lock(state->append_mutex, std::defer_lock);
    if (observer_callback)
    {
      // A user observer may own emit_mutex while another thread's append driver
      // owns append_mutex and waits to emit. Never complete that ABBA edge.
      if (!driver_lock.try_lock())
        return;
    }
    else
    {
      // Ordinary cross-thread teardown remains synchronous: the current driver
      // finishes first, then this call terminally drains every accepted ticket.
      driver_lock.lock();
    }
    ActiveControllerMarker finalizer_marker(state.get());

    std::shared_ptr<ava::session::SessionAppendTarget> released_target;
    {
      std::lock_guard lock(state->mutex);
      released_target = std::move(state->append_target);
      while (!state->appends.empty())
      {
        auto pending = state->appends.front();
        state->appends.pop_front();
        state->append_bytes -= pending->bytes;
        pending->completion = ActiveRunGuard::State::AppendItem::Completion::Closing;
      }
    }
    state->appends_drained.notify_all();
    released_target.reset();
  }
  catch (...)
  {
    // Destructors must not throw. Never revoke the append target here: without
    // append_mutex that could close its lease while an append is in flight.
    try
    {
      std::lock_guard lock(state->mutex);
      state->shutting_down = true;
      state->closing = true;
    }
    catch (...)
    {
    }
  }
}

AdmissionDisposition SessionRunController::inspect_admission(RunRequest const& request) const
{
  std::lock_guard lock(state_->mutex);
  if (state_->authority_retired)
    return AdmissionDisposition::RejectRetiredAuthority;
  if (state_->shutting_down || !state_->append_target)
    return AdmissionDisposition::RejectClosing;
  if (state_->persistence_failure)
    return AdmissionDisposition::RejectPersistenceFailure;
  if (state_->maintenance_reserved)
    return AdmissionDisposition::RejectMaintenanceReservation;
  if (!state_->active)
    return AdmissionDisposition::Admit;
  return request.request_id == state_->run_id ? AdmissionDisposition::JoinExistingOutcome : AdmissionDisposition::RejectDifferentRequest;
}

ava::core::Result<ActiveRunGuard> SessionRunController::admit(RunRequest request)
{
  if (request.request_id.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "run request id is required"));
  std::lock_guard lock(state_->mutex);
  if (state_->authority_retired)
    return std::unexpected(retired_authority_error());
  if (state_->shutting_down || !state_->append_target)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "runtime session is closing"));
  if (state_->persistence_failure)
    return std::unexpected(*state_->persistence_failure);
  if (state_->maintenance_reserved)
    return std::unexpected(maintenance_reservation_error("run_admission"));
  if (state_->active)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session already has an active run");
    error.with_context("active_run_id", state_->run_id);
    error.with_context("admission", request.request_id == state_->run_id ? "join_existing_outcome" : "rejected_different_prompt");
    return std::unexpected(std::move(error));
  }
  state_->stop_source = std::stop_source{};
  state_->active = true;
  state_->closing = false;
  ++state_->generation;
  state_->run_id = std::move(request.request_id);
  state_->phase = RunPhase::Admitted;
  state_->requested_stop.reset();
  state_->outcome.reset();
  state_->commands.clear();
  return ActiveRunGuard(state_, state_->generation);
}

ava::core::Result<SessionMaintenanceReservation> SessionRunController::reserve_maintenance()
{
  auto state = state_;
  if (!state)
    return std::unexpected(maintenance_reservation_error("controller_unavailable"));

  // Never wait out an accepted append and then mistake the resulting idle
  // state for an atomic reservation opportunity.
  std::unique_lock append_lock(state->append_mutex, std::try_to_lock);
  if (!append_lock.owns_lock())
    return std::unexpected(maintenance_reservation_error("append_queued_or_in_flight"));

  std::lock_guard state_lock(state->mutex);
  if (state->authority_retired)
    return std::unexpected(retired_authority_error());
  if (state->shutting_down || !state->append_target)
    return std::unexpected(maintenance_reservation_error("closing"));
  if (state->persistence_failure)
    return std::unexpected(*state->persistence_failure);
  if (state->active)
    return std::unexpected(maintenance_reservation_error("active_run"));
  if (state->maintenance_reserved)
    return std::unexpected(maintenance_reservation_error("already_reserved"));
  if (!state->appends.empty())
    return std::unexpected(maintenance_reservation_error("append_queued_or_in_flight"));

  state->maintenance_reserved = true;
  ++state->maintenance_generation;
  return SessionMaintenanceReservation(state, state->maintenance_generation);
}

ava::core::VoidResult SessionRunController::retire_authority(SessionMaintenanceReservation const& reservation)
{
  auto state = state_;
  if (!state || reservation.state_ != state)
    return std::unexpected(maintenance_reservation_error("reservation_mismatch"));
  std::shared_ptr<ava::session::SessionAppendTarget> released_target;
  {
    std::lock_guard lock(state->mutex);
    if (!state->maintenance_reserved || state->maintenance_generation != reservation.generation_)
      return std::unexpected(maintenance_reservation_error("reservation_unavailable"));
    if (state->active || !state->appends.empty())
      return std::unexpected(maintenance_reservation_error("work_active"));
    state->authority_retired = true;
    state->closing = true;
    state->commands.clear();
    released_target = std::move(state->append_target);
  }
  // The fresh controller already duplicated this shared target before old
  // authority retirement. Releasing the old reference here prevents stale
  // route copies from retaining a session lease while remaining permanently
  // unable to append.
  released_target.reset();
  return {};
}

bool SessionRunController::authority_retired() const noexcept
{
  auto state = state_;
  if (!state)
    return true;
  std::lock_guard lock(state->mutex);
  return state->authority_retired;
}

ava::core::Result<RunOutcome> SessionRunController::wait_outcome(std::string_view correlation_id)
{
  std::unique_lock lock(state_->mutex);
  if (correlation_id.empty())
    return std::unexpected(stale_error());
  if (auto completed = state_->outcomes.find(std::string(correlation_id)); completed != state_->outcomes.end())
    return completed->second;
  if (!state_->active || correlation_id != state_->run_id)
    return std::unexpected(stale_error());
  state_->outcome_changed.wait(lock, [&] { return state_->outcomes.contains(std::string(correlation_id)); });
  auto completed = state_->outcomes.find(std::string(correlation_id));
  if (completed == state_->outcomes.end())
    return std::unexpected(stale_error());
  return completed->second;
}

ava::core::VoidResult SessionRunController::wake(RunCommand command)
{
  if (command.message.size() > kMaxSessionRunCommandBytes)
    return std::unexpected(queue_limit_error("commands", kMaxSessionRunCommandBytes));
  std::lock_guard lock(state_->mutex);
  if (state_->authority_retired)
    return std::unexpected(retired_authority_error());
  if (!state_->active || state_->closing)
    return std::unexpected(inactive_error());
  if (!command.correlation_id.empty() && command.correlation_id != state_->run_id)
    return std::unexpected(stale_error());
  if (state_->commands.size() >= kMaxSessionRunCommands)
    return std::unexpected(queue_limit_error("commands", kMaxSessionRunCommands));
  if (command.correlation_id.empty())
    command.correlation_id = state_->run_id;
  state_->commands.push_back(std::move(command));
  return {};
}

ava::core::Result<std::deque<RunCommand>> SessionRunController::take_commands(std::string_view correlation_id, RunCommand::Kind kind)
{
  std::lock_guard lock(state_->mutex);
  if (state_->authority_retired)
    return std::unexpected(retired_authority_error());
  if (!state_->active)
    return std::unexpected(inactive_error());
  std::deque<RunCommand> selected, remaining;
  while (!state_->commands.empty())
  {
    auto command = std::move(state_->commands.front());
    state_->commands.pop_front();
    if ((correlation_id.empty() || command.correlation_id == correlation_id) && command.kind == kind)
      selected.push_back(std::move(command));
    else
      remaining.push_back(std::move(command));
  }
  state_->commands = std::move(remaining);
  return selected;
}

ava::core::Result<bool> SessionRunController::request_stop(StopReason reason)
{
  std::stop_source source;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->authority_retired)
      return std::unexpected(retired_authority_error());
    if (!state_->active || state_->closing || state_->phase == RunPhase::Completing || state_->requested_stop)
      return false;
    state_->requested_stop = reason;
    source = state_->stop_source;
  }
  // Stop callbacks may reenter the controller, so they must run lock-free.
  static_cast<void>(source.request_stop());
  return true;
}

RunSnapshot SessionRunController::snapshot() const
{
  std::lock_guard lock(state_->mutex);
  return {.active = state_->active,
          .maintenance_reserved = state_->maintenance_reserved,
          .authority_retired = state_->authority_retired,
          .run_id = state_->run_id,
          .phase = state_->phase,
          .stop_requested = state_->stop_source.stop_requested(),
          .queued_commands = state_->commands.size(),
          .queued_appends = state_->appends.size(),
          .queued_append_bytes = state_->append_bytes,
          .outcome = state_->outcome};
}

ava::core::VoidResult SessionRunController::append_for_generation(std::shared_ptr<ActiveRunGuard::State> const& state, std::uint64_t generation,
                                                                  std::vector<ava::session::SessionEntry> entries, bool owner_route,
                                                                  SessionAppendRequestKind kind, std::vector<ava::session::SessionEntry> expected,
                                                                  ava::session::SessionConditionalAppendResult* branch_summary_result,
                                                                  ava::session::SessionCompactionAppendResult* compaction_result,
                                                                  ava::session::SessionCancelCallback cancel_requested)
{
  if (!state)
    return std::unexpected(inactive_error());
  if (controller_active_on_this_thread(state.get()))
    return std::unexpected(reentrant_append_error());

  // Observer callbacks must never wait behind a driver that may itself be
  // waiting for the callback's emit mutex. They may drive an idle controller.
  std::unique_lock driver_lock(state->append_mutex, std::defer_lock);
  if (ava::observability::in_run_observer_callback() && !driver_lock.try_lock())
    return std::unexpected(reentrant_append_error());

  auto bytes = batch_entry_byte_count(entries);
  if (!bytes)
    return std::unexpected(std::move(bytes.error()));

  // A compaction expected snapshot is CAS comparison data, not append payload,
  // so it is never folded into queue byte accounting; only the appended entry
  // bytes below count against kMaxSessionAppendQueueBytes.
  auto item = std::make_shared<ActiveRunGuard::State::AppendItem>();
  item->bytes = *bytes;
  item->entries = std::move(entries);
  item->expected = std::move(expected);
  item->kind = kind;
  item->cancel_requested = std::move(cancel_requested);
  {
    std::lock_guard lock(state->mutex);
    if (state->authority_retired)
      return std::unexpected(retired_authority_error());
    if (state->persistence_failure)
      return std::unexpected(*state->persistence_failure);
    if (state->maintenance_reserved)
      return std::unexpected(maintenance_reservation_error("owner_append_admission"));
    // The stable owner route is valid between runs as well as during one. Only
    // shutdown and an explicitly latched persistence failure revoke it.
    if (state->shutting_down || !state->append_target)
      return std::unexpected(inactive_error());
    if (!owner_route && (!state->active || state->closing || state->generation != generation))
      return std::unexpected(stale_error());
    if (state->appends.size() >= kMaxSessionAppendQueueEntries)
      return std::unexpected(queue_limit_error("appends", kMaxSessionAppendQueueEntries));
    if (item->bytes > kMaxSessionAppendQueueBytes || state->append_bytes > kMaxSessionAppendQueueBytes - item->bytes)
      return std::unexpected(queue_limit_error("append_bytes", kMaxSessionAppendQueueBytes));
    // Bound compaction snapshot memory amplification with one dedicated lane:
    // queued tickets remain Pending until their terminal erase on every queue
    // exit, so queue membership is the exact pending/in-flight signal. This
    // admission rejection never latches persistence or stops an active run.
    if (kind == SessionAppendRequestKind::Compaction &&
        std::ranges::any_of(state->appends, [](auto const& queued) { return queued->kind == SessionAppendRequestKind::Compaction; }))
      return std::unexpected(compaction_pending_error());
    state->appends.push_back(item);
    state->append_bytes += item->bytes;
  }

  auto drain_locked = [&](ActiveRunGuard::State::AppendItem::Completion completion, std::shared_ptr<ava::core::Error const> const& failure = {}) {
    while (!state->appends.empty())
    {
      auto pending = state->appends.front();
      state->appends.pop_front();
      state->append_bytes -= pending->bytes;
      pending->failure = failure;
      pending->completion = completion;
    }
  };

  if (!driver_lock.owns_lock())
    driver_lock.lock();
  ActiveControllerMarker driver_marker(state.get());

  std::shared_ptr<ava::session::SessionAppendTarget> released_target;
  while (true)
  {
    std::shared_ptr<ActiveRunGuard::State::AppendItem> head;
    std::shared_ptr<ava::session::SessionAppendTarget> target;
    std::shared_ptr<ava::core::Error const> fallback;
    std::shared_ptr<ava::core::Error const> rejection_fallback;
    {
      std::lock_guard lock(state->mutex);
      if (item->completion != ActiveRunGuard::State::AppendItem::Completion::Pending)
        break;
      if (state->shutting_down || !state->append_target)
      {
        drain_locked(ActiveRunGuard::State::AppendItem::Completion::Closing);
        released_target = std::move(state->append_target);
      }
      else if (state->persistence_failure)
      {
        drain_locked(ActiveRunGuard::State::AppendItem::Completion::PersistenceFailed, state->persistence_failure);
      }
      else
      {
        head = state->appends.front();
        target = state->append_target;
        fallback = state->append_exception_fallback;
        rejection_fallback = state->conditional_rejection_fallback;
      }
    }
    state->appends_drained.notify_all();
    released_target.reset();
    if (!head)
      continue;

    // Build one immutable failure before mutating queue state. Every ticket
    // affected by this append then shares the exact same terminal error and
    // append_commit_state, even if error-detail allocation itself fails.
    std::shared_ptr<ava::core::Error const> failure;
    std::shared_ptr<ava::core::Error const> rejection;
    try
    {
      ava::core::VoidResult persisted;
      if (head->kind == SessionAppendRequestKind::BranchSummary)
      {
        auto conditional = target->append_branch_summary_if_absent_classified(head->entries.front(), head->cancel_requested);
        if (conditional.completion == ava::session::SessionAppendTarget::ConditionalAppendCompletion::Succeeded)
        {
          if (!conditional.result)
            throw std::logic_error("successful conditional append has no result");
          head->branch_summary_result = std::move(*conditional.result);
        }
        else if (conditional.completion == ava::session::SessionAppendTarget::ConditionalAppendCompletion::RejectedBeforeAppend)
        {
          try
          {
            rejection = conditional.error ? std::make_shared<ava::core::Error>(std::move(*conditional.error)) : rejection_fallback;
          }
          catch (...)
          {
            rejection = rejection_fallback;
          }
        }
        else
        {
          persisted = conditional.error ? ava::core::VoidResult(std::unexpected(std::move(*conditional.error)))
                                        : ava::core::VoidResult(std::unexpected(ava::core::Error(
                                              ava::core::ErrorCategory::Unknown, "conditional branch summary append failed without a stable error")));
        }
      }
      else if (head->kind == SessionAppendRequestKind::Compaction)
      {
        auto conditional = target->append_compaction_if_snapshot_matches_classified(head->entries.front(), head->expected, head->cancel_requested);
        if (conditional.completion == ava::session::SessionAppendTarget::ConditionalAppendCompletion::Succeeded)
        {
          if (!conditional.result)
            throw std::logic_error("successful conditional compaction append has no result");
          head->compaction_result = *conditional.result;
        }
        else if (conditional.completion == ava::session::SessionAppendTarget::ConditionalAppendCompletion::RejectedBeforeAppend)
        {
          try
          {
            rejection = conditional.error ? std::make_shared<ava::core::Error>(std::move(*conditional.error)) : rejection_fallback;
          }
          catch (...)
          {
            rejection = rejection_fallback;
          }
        }
        else
        {
          persisted = conditional.error ? ava::core::VoidResult(std::unexpected(std::move(*conditional.error)))
                                        : ava::core::VoidResult(std::unexpected(ava::core::Error(
                                              ava::core::ErrorCategory::Unknown, "conditional compaction append failed without a stable error")));
        }
      }
      else
      {
        persisted = head->entries.size() == 1 ? target->append(head->entries.front()) : target->append_batch(std::move(head->entries));
      }
      if (!persisted)
      {
        try
        {
          failure = std::make_shared<ava::core::Error>(std::move(persisted.error()));
        }
        catch (...)
        {
          failure = fallback;
        }
      }
    }
    catch (std::exception const& exception)
    {
      try
      {
        auto error = ava::core::Error(ava::core::ErrorCategory::Io, "session append threw an unexpected exception");
        error.with_context("cause", exception.what());
        error.with_context("append_commit_state", "partial_or_unknown");
        error.with_context("recovery", "run explicit session persistence recovery before any later append");
        failure = std::make_shared<ava::core::Error>(std::move(error));
      }
      catch (...)
      {
        failure = fallback;
      }
    }
    catch (...)
    {
      failure = fallback;
    }
    target.reset();

    std::stop_source source;
    bool request_stop = false;
    {
      std::lock_guard lock(state->mutex);
      // append_mutex makes this the only queue consumer. Preserve exact byte
      // accounting even if an invariant is violated, then fail closed.
      auto found = std::ranges::find(state->appends, head);
      if (found != state->appends.end())
      {
        state->append_bytes -= head->bytes;
        state->appends.erase(found);
      }
      else if (!failure && !rejection)
      {
        failure = fallback;
      }

      if (failure)
      {
        head->failure = failure;
        head->completion = ActiveRunGuard::State::AppendItem::Completion::PersistenceFailed;
        state->persistence_failure = failure;
        ++state->persistence_failure_generation;
        if (state->active)
        {
          state->requested_stop = StopReason::PersistenceError;
          source = state->stop_source;
          request_stop = true;
        }
        drain_locked(ActiveRunGuard::State::AppendItem::Completion::PersistenceFailed, failure);
      }
      else if (rejection)
      {
        head->failure = rejection;
        head->completion = ActiveRunGuard::State::AppendItem::Completion::Rejected;
      }
      else
      {
        head->completion = ActiveRunGuard::State::AppendItem::Completion::Succeeded;
      }
      if (state->shutting_down)
      {
        if (!state->persistence_failure)
          drain_locked(ActiveRunGuard::State::AppendItem::Completion::Closing);
        released_target = std::move(state->append_target);
      }
    }
    state->appends_drained.notify_all();
    if (request_stop)
      static_cast<void>(source.request_stop());

    // A stop callback or SessionStore observer can request shutdown after the
    // first state update. Recheck while still serialized and release authority
    // only after target->append and all of its callbacks have returned.
    {
      std::lock_guard lock(state->mutex);
      if (state->shutting_down)
      {
        if (!state->persistence_failure)
          drain_locked(ActiveRunGuard::State::AppendItem::Completion::Closing);
        released_target = std::move(state->append_target);
      }
    }
    state->appends_drained.notify_all();
    released_target.reset();
  }

  ActiveRunGuard::State::AppendItem::Completion completion;
  std::shared_ptr<ava::core::Error const> failure;
  {
    std::lock_guard lock(state->mutex);
    // An observer-side shutdown can lose its try-lock race to any append_mutex
    // holder, including a caller whose ticket was already terminal. Every
    // holder therefore owns this final deferred-shutdown check.
    if (state->shutting_down)
    {
      if (!state->persistence_failure)
        drain_locked(ActiveRunGuard::State::AppendItem::Completion::Closing);
      released_target = std::move(state->append_target);
    }
    completion = item->completion;
    failure = item->failure;
  }
  state->appends_drained.notify_all();

  // Destroy any released target while append_mutex still serializes authority
  // handoff. Then recheck under state->mutex and unlock append_mutex while that
  // state lock is held. An observer shutdown either publishes shutting_down
  // before a recheck or acquires append_mutex after the atomic unlock; an
  // ordinary shutdown cannot return before the duplicated lease is closed.
  while (true)
  {
    released_target.reset();
    std::lock_guard lock(state->mutex);
    if (state->shutting_down)
    {
      if (!state->persistence_failure)
        drain_locked(ActiveRunGuard::State::AppendItem::Completion::Closing);
      released_target = std::move(state->append_target);
    }
    if (!released_target)
    {
      driver_lock.unlock();
      break;
    }
  }
  state->appends_drained.notify_all();

  if (completion == ActiveRunGuard::State::AppendItem::Completion::PersistenceFailed)
    return std::unexpected(failure ? *failure : *state->append_exception_fallback);
  if (completion == ActiveRunGuard::State::AppendItem::Completion::Rejected)
    return std::unexpected(failure ? *failure : *state->conditional_rejection_fallback);
  if (completion == ActiveRunGuard::State::AppendItem::Completion::Closing)
    return std::unexpected(inactive_error());
  if (completion != ActiveRunGuard::State::AppendItem::Completion::Succeeded)
    return std::unexpected(*state->append_exception_fallback);
  if (branch_summary_result != nullptr)
  {
    if (!item->branch_summary_result)
      return std::unexpected(*state->append_exception_fallback);
    *branch_summary_result = std::move(*item->branch_summary_result);
  }
  if (compaction_result != nullptr)
  {
    if (!item->compaction_result)
      return std::unexpected(*state->append_exception_fallback);
    *compaction_result = *item->compaction_result;
  }
  return {};
}

ava::core::VoidResult SessionRunController::append(ava::session::SessionEntry entry)
{
  return append_for_generation(state_, 0, {std::move(entry)}, true, SessionAppendRequestKind::Ordinary);
}

ava::core::Result<ava::session::SessionConditionalAppendResult> SessionRunController::append_branch_summary_if_absent(
    ava::session::SessionEntry entry, ava::session::SessionCancelCallback cancel_requested)
{
  ava::session::SessionConditionalAppendResult result;
  if (auto appended = append_for_generation(state_, 0, {std::move(entry)}, true, SessionAppendRequestKind::BranchSummary, {}, &result, nullptr,
                                            std::move(cancel_requested));
      !appended)
  {
    return std::unexpected(std::move(appended.error()));
  }
  return result;
}

ava::core::Result<ava::session::SessionCompactionAppendResult> SessionRunController::append_compaction_if_snapshot_matches(
    ava::session::SessionEntry entry, std::vector<ava::session::SessionEntry> expected, ava::session::SessionCancelCallback cancel_requested)
{
  ava::session::SessionCompactionAppendResult result = ava::session::SessionCompactionAppendResult::SnapshotMismatch;
  if (auto appended = append_for_generation(state_, 0, {std::move(entry)}, true, SessionAppendRequestKind::Compaction, std::move(expected), nullptr, &result,
                                            std::move(cancel_requested));
      !appended)
  {
    return std::unexpected(std::move(appended.error()));
  }
  return result;
}

ava::core::VoidResult SessionRunController::append_batch(std::vector<ava::session::SessionEntry> entries)
{
  return append_for_generation(state_, 0, std::move(entries), true, SessionAppendRequestKind::Ordinary);
}

ava::agent::SessionAppendSink SessionRunController::owner_append_route() const
{
  auto state = state_;
  return [state = std::move(state)](ava::session::SessionEntry entry) {
    return append_for_generation(state, 0, {std::move(entry)}, true, SessionAppendRequestKind::Ordinary);
  };
}

ava::agent::SessionAppendBatchSink SessionRunController::owner_append_batch_route() const
{
  auto state = state_;
  return [state = std::move(state)](std::vector<ava::session::SessionEntry> entries) {
    return append_for_generation(state, 0, std::move(entries), true, SessionAppendRequestKind::Ordinary);
  };
}

ava::core::VoidResult SessionRunController::reset_persistence_failure()
{
  auto state = state_;
  if (ava::observability::in_run_observer_callback() || controller_active_on_this_thread(state.get()))
    return std::unexpected(reentrant_reset_error());

  std::unique_lock driver_lock(state->append_mutex);
  ActiveControllerMarker recovery_marker(state.get());
  std::shared_ptr<ava::session::SessionAppendTarget> target;
  std::uint64_t failure_generation = 0;
  ava::core::VoidResult result;
  {
    std::lock_guard lock(state->mutex);
    if (state->authority_retired)
      result = std::unexpected(retired_authority_error());
    else if (state->active || state->maintenance_reserved || !state->appends.empty())
      result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "cannot recover append failure during active run or maintenance"));
    else if (state->shutting_down || !state->append_target)
      result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "cannot recover persistence for a closing runtime session"));
    else if (!state->persistence_failure)
      result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session has no latched persistence failure to recover"));
    else
    {
      target = state->append_target;
      failure_generation = state->persistence_failure_generation;
    }
  }

  if (target)
  {
    // Recovery can scan, quarantine, sync, and truncate. Never hold the state
    // mutex across it; append_mutex alone excludes append drivers and serializes
    // shutdown's authority release.
    auto recovered = target->recover();
    if (!recovered)
    {
      result = std::unexpected(std::move(recovered.error()));
    }
    else
    {
      std::lock_guard lock(state->mutex);
      if (state->active || state->maintenance_reserved || !state->appends.empty() || state->shutting_down || state->append_target != target ||
          !state->persistence_failure || state->persistence_failure_generation != failure_generation)
      {
        result = std::unexpected(
            ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "persistence recovery completed after runtime session authority changed"));
      }
      else
      {
        state->persistence_failure.reset();
      }
    }
  }

  // Observer-side shutdown cannot wait for a concurrent recovery. The recovery
  // holder therefore performs the same terminal drain and authority release on
  // every exit, including failed recovery and precondition paths.
  std::shared_ptr<ava::session::SessionAppendTarget> released_target;
  {
    std::lock_guard lock(state->mutex);
    if (state->shutting_down)
    {
      while (!state->appends.empty())
      {
        auto pending = state->appends.front();
        state->appends.pop_front();
        state->append_bytes -= pending->bytes;
        pending->completion = ActiveRunGuard::State::AppendItem::Completion::Closing;
      }
      released_target = std::move(state->append_target);
    }
  }
  state->appends_drained.notify_all();
  released_target.reset();
  return result;
}

SessionMaintenanceReservation::SessionMaintenanceReservation(std::shared_ptr<ActiveRunGuard::State> state, std::uint64_t generation)
    : state_(std::move(state)), generation_(generation)
{
}

SessionMaintenanceReservation::~SessionMaintenanceReservation()
{
  release();
}

SessionMaintenanceReservation::SessionMaintenanceReservation(SessionMaintenanceReservation&& other) noexcept
    : state_(std::move(other.state_)), generation_(std::exchange(other.generation_, 0))
{
}

SessionMaintenanceReservation& SessionMaintenanceReservation::operator=(SessionMaintenanceReservation&& other) noexcept
{
  if (this != &other)
  {
    release();
    state_ = std::move(other.state_);
    generation_ = std::exchange(other.generation_, 0);
  }
  return *this;
}

bool SessionMaintenanceReservation::active() const noexcept
{
  if (!state_)
    return false;
  std::lock_guard lock(state_->mutex);
  return state_->maintenance_reserved && state_->maintenance_generation == generation_;
}

void SessionMaintenanceReservation::release() noexcept
{
  if (!state_)
    return;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->maintenance_reserved && state_->maintenance_generation == generation_)
      state_->maintenance_reserved = false;
  }
  state_.reset();
  generation_ = 0;
}

ActiveRunGuard::ActiveRunGuard(std::shared_ptr<State> state, std::uint64_t generation) : state_(std::move(state)), generation_(generation)
{
}
ActiveRunGuard::~ActiveRunGuard()
{
  release();
}
ActiveRunGuard::ActiveRunGuard(ActiveRunGuard&& other) noexcept : state_(std::move(other.state_)), generation_(std::exchange(other.generation_, 0))
{
}
ActiveRunGuard& ActiveRunGuard::operator=(ActiveRunGuard&& other) noexcept
{
  if (this != &other)
  {
    release();
    state_ = std::move(other.state_);
    generation_ = std::exchange(other.generation_, 0);
  }
  return *this;
}
std::stop_token ActiveRunGuard::stop_token() const noexcept
{
  return state_ ? state_->stop_source.get_token() : std::stop_token{};
}
bool ActiveRunGuard::stop_requested() const noexcept
{
  return state_ && state_->stop_source.stop_requested();
}

ava::core::VoidResult ActiveRunGuard::transition(RunPhase next)
{
  if (!state_)
    return std::unexpected(inactive_error());
  std::lock_guard lock(state_->mutex);
  if (!state_->active || state_->generation != generation_)
    return std::unexpected(stale_error());
  if (next == RunPhase::Completing && state_->requested_stop)
    return std::unexpected(stop_requested_error(*state_->requested_stop));
  if (!legal_transition(state_->phase, next))
    return std::unexpected(transition_error(state_->phase, next));
  state_->phase = next;
  return {};
}

ava::core::Result<RunOutcome> ActiveRunGuard::complete(RunOutcome outcome)
{
  if (!state_)
    return std::unexpected(inactive_error());
  std::unique_lock lock(state_->mutex);
  if (!state_->active || state_->generation != generation_ || state_->outcome)
    return std::unexpected(stale_error());
  if (outcome.run_id.empty())
    outcome.run_id = state_->run_id;
  if (outcome.run_id != state_->run_id)
    return std::unexpected(stale_error());
  if (state_->requested_stop && state_->phase != RunPhase::Completing)
  {
    outcome.reason = *state_->requested_stop;
    outcome.error.reset();
  }
  if (outcome.reason == StopReason::Completed)
  {
    if (state_->phase != RunPhase::Completing)
      return std::unexpected(transition_error(state_->phase, RunPhase::Completing));
  }
  else if (outcome.reason == StopReason::UserCanceled || outcome.reason == StopReason::Deadline)
    state_->phase = RunPhase::Canceling;
  else
    state_->phase = RunPhase::Failed;
  state_->closing = true;
  state_->appends_drained.wait(lock, [&] { return state_->appends.empty(); });
  if (state_->persistence_failure)
  {
    outcome.reason = StopReason::PersistenceError;
    outcome.error = *state_->persistence_failure;
    state_->phase = RunPhase::Failed;
  }
  state_->outcome = outcome;
  if (state_->outcomes.emplace(outcome.run_id, outcome).second)
    state_->outcome_order.push_back(outcome.run_id);
  else
    state_->outcomes.insert_or_assign(outcome.run_id, outcome);
  while (state_->outcome_order.size() > kMaxRetainedRunOutcomes)
  {
    state_->outcomes.erase(state_->outcome_order.front());
    state_->outcome_order.pop_front();
  }
  state_->active = false;
  state_->commands.clear();
  lock.unlock();
  state_->outcome_changed.notify_all();
  state_.reset();
  generation_ = 0;
  return outcome;
}

bool ActiveRunGuard::active() const noexcept
{
  if (!state_)
    return false;
  std::lock_guard lock(state_->mutex);
  return state_->active && state_->generation == generation_;
}

ava::agent::SessionAppendSink ActiveRunGuard::append_route() const
{
  auto state = state_;
  auto generation = generation_;
  return [state = std::move(state), generation](ava::session::SessionEntry entry) {
    return SessionRunController::append_for_generation(state, generation, {std::move(entry)}, false, SessionAppendRequestKind::Ordinary);
  };
}

ava::agent::SessionAppendBatchSink ActiveRunGuard::append_batch_route() const
{
  auto state = state_;
  auto generation = generation_;
  return [state = std::move(state), generation](std::vector<ava::session::SessionEntry> entries) {
    return SessionRunController::append_for_generation(state, generation, std::move(entries), false, SessionAppendRequestKind::Ordinary);
  };
}

ava::session::SessionCompactionAppendSink ActiveRunGuard::compaction_append_route() const
{
  auto state = state_;
  auto generation = generation_;
  return [state = std::move(state), generation](ava::session::SessionEntry entry, std::vector<ava::session::SessionEntry> expected,
                                                ava::session::SessionCancelCallback cancel_requested) {
    ava::session::SessionCompactionAppendResult result = ava::session::SessionCompactionAppendResult::SnapshotMismatch;
    if (auto appended = SessionRunController::append_for_generation(state, generation, {std::move(entry)}, false, SessionAppendRequestKind::Compaction,
                                                                    std::move(expected), nullptr, &result, std::move(cancel_requested));
        !appended)
    {
      return ava::core::Result<ava::session::SessionCompactionAppendResult>(std::unexpected(std::move(appended.error())));
    }
    return ava::core::Result<ava::session::SessionCompactionAppendResult>(result);
  };
}

void ActiveRunGuard::release() noexcept
{
  if (!state_)
    return;
  std::unique_lock lock(state_->mutex);
  if (state_->active && state_->generation == generation_)
  {
    RunOutcome outcome{.run_id = state_->run_id,
                       .reason = state_->stop_source.stop_requested() ? state_->requested_stop.value_or(StopReason::UserCanceled) : StopReason::ProviderError};
    if (outcome.reason == StopReason::ProviderError)
      outcome.error.emplace(ava::core::Error(ava::core::ErrorCategory::Unknown, "active run guard released before terminal outcome"));
    state_->phase = outcome.reason == StopReason::UserCanceled || outcome.reason == StopReason::Deadline ? RunPhase::Canceling : RunPhase::Failed;
    // Do not permit a generation route to enqueue after terminal release.
    // Waiting releases the state mutex, so the append driver can finish.
    state_->closing = true;
    state_->appends_drained.wait(lock, [&] { return state_->appends.empty(); });
    if (state_->persistence_failure)
    {
      outcome.reason = StopReason::PersistenceError;
      outcome.error = *state_->persistence_failure;
      state_->phase = RunPhase::Failed;
    }
    state_->outcome = outcome;
    if (state_->outcomes.emplace(outcome.run_id, outcome).second)
      state_->outcome_order.push_back(outcome.run_id);
    else
      state_->outcomes.insert_or_assign(outcome.run_id, outcome);
    while (state_->outcome_order.size() > kMaxRetainedRunOutcomes)
    {
      state_->outcomes.erase(state_->outcome_order.front());
      state_->outcome_order.pop_front();
    }
    state_->active = false;
    state_->commands.clear();
  }
  lock.unlock();
  state_->outcome_changed.notify_all();
  state_.reset();
  generation_ = 0;
}

std::string_view to_string(RunPhase phase)
{
  constexpr std::string_view names[] = {"admitted",        "building_context", "awaiting_provider", "persisting_assistant", "preparing_tools",
                                        "executing_tools", "settling_tools",   "compacting",        "completing",           "canceling",
                                        "failed"};
  return names[static_cast<std::size_t>(phase)];
}
std::string_view to_string(StopReason reason)
{
  constexpr std::string_view names[] = {"completed",   "user_canceled",  "deadline",   "max_turns",        "max_tool_calls",
                                        "no_progress", "provider_error", "tool_error", "persistence_error"};
  return names[static_cast<std::size_t>(reason)];
}

std::string_view to_string(AdmissionDisposition disposition)
{
  constexpr std::string_view names[] = {"admit",
                                        "join_existing_outcome",
                                        "rejected_different_prompt",
                                        "rejected_maintenance_reservation",
                                        "rejected_closing",
                                        "rejected_persistence_failure",
                                        "rejected_retired_authority"};
  return names[static_cast<std::size_t>(disposition)];
}

}  // namespace ava::app
