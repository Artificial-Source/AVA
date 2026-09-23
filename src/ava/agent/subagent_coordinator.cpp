#include "sys.h"
#include "ava/agent/job_control.h"
#include "ava/agent/subagent_coordinator.h"
#include "ava/agent/subagent_inspector.h"
#include "ava/agent/subagent_inspector_source.h"
#include "ava/session/session_store.h"
#include "ava/session/subagent_job_history.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <utility>

namespace ava::agent {
namespace {

constexpr std::size_t kMaxIdBytes = 96;
constexpr std::size_t kMaxSummaryBytes = 16U * 1024U;
constexpr std::size_t kMaxErrorBytes = 4U * 1024U;
constexpr std::size_t kMaxStopReasonBytes = 1024;
// Matches the model task description bound; never stores the full prompt.
constexpr std::size_t kMaxDisplayTitleBytes = 256;
constexpr std::size_t kMaxDisplaySubagentTypeBytes = 128;
constexpr std::size_t kMaxAccountingValue = 1024U * 1024U;
constexpr std::size_t kMaxIdentityGenerationAttempts = 8;
constexpr std::size_t kMaxSteeringMessages = 16;
constexpr std::size_t kMaxSteeringMessageBytes = 16U * 1024U;
constexpr std::size_t kMaxSteeringQueueBytes = 64U * 1024U;
constexpr std::string_view kPublicationCommitStateContext = "subagent_publication_commit_state";

ava::core::Error coordinator_maintenance_error(std::string_view conflict)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "subagent workspace maintenance is unavailable");
  error.with_context("maintenance_conflict", std::string(conflict));
  return error;
}

ava::core::Error interaction_unavailable(std::string_view interaction, bool background)
{
  auto error =
      ava::core::Error(ava::core::ErrorCategory::Tool, background ? "background interaction unavailable" : "foreground interaction resolver is unavailable");
  error.with_context("error_code", background ? "background_interaction_unavailable" : "foreground_interaction_unavailable");
  error.with_context("interaction", std::string(interaction));
  return error;
}

ava::core::Error not_found(std::string_view job_id)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::NotFound, "subagent job not found");
  error.with_context("job_id", std::string(job_id));
  return error;
}

bool terminal(SubagentExecutionState state) noexcept
{
  return state == SubagentExecutionState::Completed || state == SubagentExecutionState::Failed || state == SubagentExecutionState::Canceled ||
         state == SubagentExecutionState::Interrupted;
}

bool delivery_pending(SubagentDeliveryState state) noexcept
{
  return state == SubagentDeliveryState::Pending || state == SubagentDeliveryState::Attempting;
}

ava::core::Error& mark_unpublished(ava::core::Error& error)
{
  return error.with_context(std::string(kPublicationCommitStateContext), std::string(to_string(SubagentPublicationCommitState::ProvenUnpublished)));
}

ava::core::Error invalid_transition(std::string_view message, std::string_view job_id)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Tool, std::string(message));
  error.with_context("job_id", std::string(job_id));
  return error;
}

bool valid_identifier(std::string_view value) noexcept
{
  if (value.empty() || value.size() > kMaxIdBytes)
    return false;
  return std::ranges::all_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == ':';
  });
}

ava::core::VoidResult validate_identifier(std::string_view field, std::string_view value, std::string_view job_id)
{
  if (valid_identifier(value))
    return {};
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent job identifier is empty, too long, or contains a forbidden character");
  error.with_context("field", std::string(field)).with_context("max_bytes", std::to_string(kMaxIdBytes));
  if (!job_id.empty())
    error.with_context("job_id", std::string(job_id));
  return std::unexpected(std::move(error));
}

ava::core::Result<std::string> generate_identity_candidate(SubagentCoordinatorOptions const& options, std::string_view prefix)
{
  try
  {
    auto candidate = options.id_generator ? options.id_generator(prefix) : ava::core::make_id(prefix);
    auto const field = std::string(prefix) + "_id";
    if (auto valid = validate_identifier(field, candidate, {}); !valid)
      return std::unexpected(std::move(valid.error()));
    return candidate;
  }
  catch (std::exception const& exception)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "subagent identity generation failed");
    error.with_context("identity_kind", std::string(prefix)).with_context("cause", exception.what());
    return std::unexpected(std::move(error));
  }
  catch (...)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "subagent identity generation failed");
    error.with_context("identity_kind", std::string(prefix)).with_context("cause", "unknown exception");
    return std::unexpected(std::move(error));
  }
}

bool has_forbidden_text_control(std::string_view value) noexcept
{
  return std::ranges::any_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return byte < 0x20U && ch != '\n' && ch != '\r' && ch != '\t';
  });
}

bool truncate_utf8(std::string& value, std::size_t max_bytes)
{
  if (value.size() <= max_bytes)
    return false;
  std::size_t end = max_bytes;
  while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U)
    --end;
  value.resize(end);
  return true;
}

bool normalize_text(std::string& value, std::size_t max_bytes, std::string_view fallback)
{
  bool changed = false;
  if (!ava::core::json::is_valid_utf8(value) || has_forbidden_text_control(value))
  {
    value = fallback;
    changed = true;
  }
  return truncate_utf8(value, max_bytes) || changed;
}

// Process-local interactive display only. Replaces controls, collapses
// interior whitespace, rejects invalid UTF-8, and never retains prompt text.
std::string make_display_field(std::string_view raw, std::size_t max_bytes)
{
  std::string value;
  value.reserve(std::min(raw.size(), max_bytes));
  bool pending_space = false;
  auto flush_space = [&] {
    if (pending_space && !value.empty())
      value.push_back(' ');
    pending_space = false;
  };
  for (char const ch : raw)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20U || byte == 0x7FU || ch == ' ')
    {
      pending_space = !value.empty();
      continue;
    }
    flush_space();
    value.push_back(ch);
    if (value.size() >= max_bytes + 4)
      break;
  }
  if (!ava::core::json::is_valid_utf8(value))
    return {};
  truncate_utf8(value, max_bytes);
  while (!value.empty() && value.back() == ' ')
    value.pop_back();
  return value;
}

BackgroundJobCompletion exception_completion(std::string const& job_id, std::stop_token const& stop_token, std::string cause)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "background job worker threw an exception");
  error.with_context("job_id", job_id).with_context("cause", std::move(cause));
  return {.state = stop_token.stop_requested() ? BackgroundJobState::Canceled : BackgroundJobState::Failed,
          .final_text = {},
          .stop_reason = stop_token.stop_requested() ? "canceled" : "failed",
          .error = std::move(error)};
}

std::uint64_t advance_content_generation(std::uint64_t current) noexcept
{
  auto next = current + 1;
  if (next == 0)
    next = 1;
  return next;
}

std::shared_ptr<SubagentInspectorFrame const> stamp_inspection_frame(std::uint64_t generation, bool terminal, bool freeze_pending, bool unavailable,
                                                                     bool refresh_unavailable, std::vector<SubagentLiveMessage> messages)
{
  auto frame = std::make_shared<SubagentInspectorFrame>();
  frame->generation = generation;
  frame->terminal = terminal;
  frame->freeze_pending = freeze_pending;
  frame->unavailable = unavailable;
  frame->refresh_unavailable = refresh_unavailable;
  frame->messages = std::move(messages);
  return std::shared_ptr<SubagentInspectorFrame const>(std::move(frame));
}

std::shared_ptr<SubagentInspectorFrame const> view_published_frame(std::shared_ptr<SubagentInspectorFrame const> const& published, bool terminal,
                                                                   bool freeze_pending)
{
  if (!published)
    return nullptr;
  if (published->terminal == terminal && published->freeze_pending == freeze_pending && !published->not_modified)
    return published;
  return stamp_inspection_frame(published->generation, terminal, freeze_pending, published->unavailable, published->refresh_unavailable, published->messages);
}

BackgroundJobCompletion normalize_completion(std::string const& job_id, BackgroundJobCompletion completion)
{
  if (completion.state == BackgroundJobState::Completed)
  {
    completion.error = std::nullopt;
    if (completion.stop_reason.empty())
      completion.stop_reason = "completed";
    return completion;
  }
  if (completion.state == BackgroundJobState::Canceled)
  {
    completion.final_text.clear();
    completion.error = std::nullopt;
    if (completion.stop_reason.empty())
      completion.stop_reason = "canceled";
    return completion;
  }
  if (completion.state != BackgroundJobState::Failed)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "background job completed with a non-terminal state");
    error.with_context("job_id", job_id);
    completion = {.state = BackgroundJobState::Failed, .final_text = {}, .stop_reason = "failed", .error = std::move(error)};
  }
  completion.final_text.clear();
  if (completion.stop_reason.empty())
    completion.stop_reason = "failed";
  if (!completion.error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "background job failed without an error");
    error.with_context("job_id", job_id);
    completion.error = std::move(error);
  }
  return completion;
}

ava::session::SubagentJobHistoryMode history_mode(SubagentJobMode mode) noexcept
{
  return mode == SubagentJobMode::Background ? ava::session::SubagentJobHistoryMode::Background : ava::session::SubagentJobHistoryMode::Foreground;
}

ava::session::SubagentJobHistoryExecution history_execution(SubagentExecutionState execution) noexcept
{
  switch (execution)
  {
    case SubagentExecutionState::Starting:
      return ava::session::SubagentJobHistoryExecution::Starting;
    case SubagentExecutionState::Running:
      return ava::session::SubagentJobHistoryExecution::Running;
    case SubagentExecutionState::Completed:
      return ava::session::SubagentJobHistoryExecution::Completed;
    case SubagentExecutionState::Failed:
      return ava::session::SubagentJobHistoryExecution::Failed;
    case SubagentExecutionState::Canceled:
      return ava::session::SubagentJobHistoryExecution::Canceled;
    case SubagentExecutionState::Interrupted:
      return ava::session::SubagentJobHistoryExecution::Interrupted;
  }
  return ava::session::SubagentJobHistoryExecution::Interrupted;
}

ava::session::SubagentJobHistoryRecord history_record_from_snapshot(SubagentJobSnapshot const& snapshot, ava::session::SubagentJobHistoryPhase phase)
{
  ava::session::SubagentJobHistoryRecord record;
  record.phase = phase;
  record.job_id = snapshot.identity.job_id;
  record.task_id = snapshot.identity.task_id;
  record.parent_session_id = snapshot.identity.parent_session_id;
  record.child_session_id = snapshot.identity.child_session_id;
  record.delivery_id = snapshot.identity.delivery_id;
  record.mode = history_mode(snapshot.mode);
  record.execution = history_execution(snapshot.execution);
  record.started_at = snapshot.started_at;
  record.updated_at = snapshot.updated_at;
  if (phase == ava::session::SubagentJobHistoryPhase::Terminal)
  {
    record.terminal_at = snapshot.terminal_at;
    record.summary = snapshot.summary;
    record.summary_truncated = snapshot.summary_truncated;
    record.error = snapshot.error;
    record.error_truncated = snapshot.error_truncated;
    record.error_category = snapshot.error_category;
    record.stop_reason = snapshot.stop_reason;
    record.stop_reason_truncated = snapshot.stop_reason_truncated;
    record.provider_iterations = snapshot.provider_iterations;
    record.tool_calls = snapshot.tool_calls;
    record.tool_iterations = snapshot.tool_iterations;
  }
  return record;
}

ava::core::VoidResult persist_job_history(SubagentJobHistoryAppend const& sink, SubagentJobSnapshot const& snapshot,
                                          ava::session::SubagentJobHistoryPhase phase)
{
  if (!sink)
    return {};
  auto entry = ava::session::make_subagent_job_history_entry(history_record_from_snapshot(snapshot, phase));
  if (!entry)
    return std::unexpected(std::move(entry.error()));
  return sink(std::move(*entry));
}

}  // namespace

std::shared_ptr<SubagentSteeringQueue> SubagentSteeringQueue::create()
{
  return std::make_shared<SubagentSteeringQueue>();
}

ava::core::VoidResult SubagentSteeringQueue::enqueue(std::string message)
{
  if (message.empty() || message.size() > kMaxSteeringMessageBytes || !ava::core::json::is_valid_utf8(message) || has_forbidden_text_control(message))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "steering message is empty, too long, or invalid");
    error.with_context("max_bytes", std::to_string(kMaxSteeringMessageBytes));
    return std::unexpected(std::move(error));
  }
  std::lock_guard lock(mutex_);
  if (closed_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "subagent job no longer accepts steering"));
  if (messages_.size() >= kMaxSteeringMessages || message.size() > kMaxSteeringQueueBytes - std::min(queued_bytes_, kMaxSteeringQueueBytes))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "subagent steering queue is full");
    error.with_context("job_error_code", "steering_queue_full");
    return std::unexpected(std::move(error));
  }
  queued_bytes_ += message.size();
  messages_.push_back(std::move(message));
  return {};
}

ava::core::Result<std::vector<std::string>> SubagentSteeringQueue::take()
{
  std::lock_guard lock(mutex_);
  std::vector<std::string> result;
  result.reserve(messages_.size());
  while (!messages_.empty())
  {
    result.push_back(std::move(messages_.front()));
    messages_.pop_front();
  }
  queued_bytes_ = 0;
  return result;
}

void SubagentSteeringQueue::close()
{
  std::lock_guard lock(mutex_);
  closed_ = true;
  messages_.clear();
  queued_bytes_ = 0;
}

SubagentInteractionGate::SubagentInteractionGate(SubagentJobMode mode, ava::permissions::PermissionResolver permission_resolver,
                                                 QuestionResolver question_resolver)
    : mode_(mode),
      permission_resolver_(mode == SubagentJobMode::Foreground ? std::move(permission_resolver) : nullptr),
      question_resolver_(mode == SubagentJobMode::Foreground ? std::move(question_resolver) : nullptr)
{
}

std::shared_ptr<SubagentInteractionGate> SubagentInteractionGate::create(SubagentJobMode mode, ava::permissions::PermissionResolver permission_resolver,
                                                                         QuestionResolver question_resolver)
{
  return std::shared_ptr<SubagentInteractionGate>(new SubagentInteractionGate(mode, std::move(permission_resolver), std::move(question_resolver)));
}

ava::permissions::PermissionResolver SubagentInteractionGate::permission_resolver()
{
  auto self = shared_from_this();
  return [self = std::move(self)](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    ava::permissions::PermissionResolver resolver;
    {
      std::lock_guard lock(self->mutex_);
      if (self->mode_ != SubagentJobMode::Foreground || self->promotion_pending_)
        return std::unexpected(interaction_unavailable("permission", true));
      if (!self->permission_resolver_)
        return std::unexpected(interaction_unavailable("permission", false));
      ++self->outstanding_;
      resolver = self->permission_resolver_;
    }
    struct Completion final
    {
      std::shared_ptr<SubagentInteractionGate> gate;
      ~Completion()
      {
        std::lock_guard lock(gate->mutex_);
        --gate->outstanding_;
      }
    } completion{self};
    return resolver(prompt);
  };
}

QuestionResolver SubagentInteractionGate::question_resolver()
{
  auto self = shared_from_this();
  return [self = std::move(self)](QuestionPrompt const& prompt) -> ava::core::Result<QuestionAnswer> {
    QuestionResolver resolver;
    {
      std::lock_guard lock(self->mutex_);
      if (self->mode_ != SubagentJobMode::Foreground || self->promotion_pending_)
        return std::unexpected(interaction_unavailable("question", true));
      if (!self->question_resolver_)
        return std::unexpected(interaction_unavailable("question", false));
      ++self->outstanding_;
      resolver = self->question_resolver_;
    }
    struct Completion final
    {
      std::shared_ptr<SubagentInteractionGate> gate;
      ~Completion()
      {
        std::lock_guard lock(gate->mutex_);
        --gate->outstanding_;
      }
    } completion{self};
    return resolver(prompt);
  };
}

ava::core::VoidResult SubagentInteractionGate::prepare_promotion()
{
  std::lock_guard lock(mutex_);
  if (mode_ == SubagentJobMode::Background)
    return {};
  if (outstanding_ != 0)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "cannot promote subagent while a foreground interaction is outstanding");
    error.with_context("error_code", "foreground_interaction_outstanding");
    return std::unexpected(std::move(error));
  }
  promotion_pending_ = true;
  return {};
}

void SubagentInteractionGate::commit_promotion()
{
  std::lock_guard lock(mutex_);
  mode_ = SubagentJobMode::Background;
  promotion_pending_ = false;
  permission_resolver_ = nullptr;
  question_resolver_ = nullptr;
}

void SubagentInteractionGate::abort_promotion()
{
  std::lock_guard lock(mutex_);
  promotion_pending_ = false;
}

void SubagentInteractionGate::finish()
{
  std::lock_guard lock(mutex_);
  mode_ = SubagentJobMode::Background;
  promotion_pending_ = false;
  permission_resolver_ = nullptr;
  question_resolver_ = nullptr;
}

std::string_view to_string(SubagentPublicationCommitState value) noexcept
{
  switch (value)
  {
    case SubagentPublicationCommitState::ProvenUnpublished:
      return "proven_unpublished";
    case SubagentPublicationCommitState::PublicationUncertain:
      return "publication_uncertain";
  }
  return "publication_uncertain";
}

SubagentPublicationCommitState subagent_publication_commit_state(ava::core::Error const& error) noexcept
{
  for (auto const& context : error.context())
    if (context.key == kPublicationCommitStateContext && context.value == to_string(SubagentPublicationCommitState::ProvenUnpublished))
      return SubagentPublicationCommitState::ProvenUnpublished;
  return SubagentPublicationCommitState::PublicationUncertain;
}

struct SubagentCoordinator::JobState
{
  SubagentJobSnapshot snapshot;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::shared_ptr<SubagentInteractionGate> interaction_gate = nullptr;
  std::shared_ptr<SubagentLiveInspectionSource> inspection_source = nullptr;
  std::shared_ptr<SubagentSteeringQueue> steering_queue = nullptr;
  // Latest successfully published path-free frame + the fingerprint it was
  // projected from. Survives freeze_pending so racing inspect never sees a gap.
  std::shared_ptr<SubagentInspectorFrame const> published_inspection = nullptr;
  std::optional<ava::session::SessionContentFingerprint> published_fingerprint = std::nullopt;
  // Source/freeze identity. Bumped on complete/shutdown/eviction so late live
  // projections cannot publish after a newer terminal handoff.
  std::uint64_t source_epoch = 1;
  // Published content generation. Distinct from source_epoch; never zero once
  // any frame has been successfully published for this job.
  std::uint64_t content_generation = 0;
  bool freeze_pending = false;
  SubagentJobHistoryAppend history_append = nullptr;
  bool published = false;
  bool terminal_notification_pending = false;
  bool terminal_notification_emitted = false;
  bool delivery_exhausted = false;
  // False while a background terminal history append is in flight so prune
  // and automatic delivery cannot treat the job as Direct-complete or Pending.
  bool delivery_arm_ready = true;
  std::size_t sequence = 0;
};

SubagentCoordinator::SubagentCoordinator(SubagentCoordinatorOptions options) : options_(std::move(options)), registry_(options_.registry_options)
{
}

ava::core::Result<std::shared_ptr<SubagentCoordinator>> SubagentCoordinator::create(SubagentCoordinatorOptions options)
{
  if (options.registry_options.max_running_jobs == 0 || options.registry_options.max_retained_finished_jobs == 0 ||
      options.registry_options.max_running_jobs > std::numeric_limits<std::size_t>::max() - options.registry_options.max_retained_finished_jobs)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent coordinator running and retained-job limits must be positive and bounded"));
  }
  return std::shared_ptr<SubagentCoordinator>(new SubagentCoordinator(std::move(options)));
}

SubagentCoordinator::~SubagentCoordinator()
{
  shutdown();
}

SubagentCoordinatorMaintenanceReservation::SubagentCoordinatorMaintenanceReservation(std::shared_ptr<SubagentCoordinator> coordinator,
                                                                                     std::vector<std::string> parent_session_ids, std::uint64_t generation)
    : coordinator_(std::move(coordinator)), parent_session_ids_(std::move(parent_session_ids)), generation_(generation)
{
}

SubagentCoordinatorMaintenanceReservation::~SubagentCoordinatorMaintenanceReservation()
{
  release();
}

SubagentCoordinatorMaintenanceReservation::SubagentCoordinatorMaintenanceReservation(SubagentCoordinatorMaintenanceReservation&& other) noexcept
    : coordinator_(std::move(other.coordinator_)), parent_session_ids_(std::move(other.parent_session_ids_)), generation_(std::exchange(other.generation_, 0))
{
}

SubagentCoordinatorMaintenanceReservation& SubagentCoordinatorMaintenanceReservation::operator=(SubagentCoordinatorMaintenanceReservation&& other) noexcept
{
  if (this != &other)
  {
    release();
    coordinator_ = std::move(other.coordinator_);
    parent_session_ids_ = std::move(other.parent_session_ids_);
    generation_ = std::exchange(other.generation_, 0);
  }
  return *this;
}

bool SubagentCoordinatorMaintenanceReservation::active() const noexcept
{
  if (!coordinator_ || generation_ == 0)
    return false;
  std::lock_guard lock(coordinator_->mutex_);
  return std::ranges::all_of(parent_session_ids_, [&](auto const& parent) {
    auto found = coordinator_->maintenance_parents_.find(parent);
    return found != coordinator_->maintenance_parents_.end() && found->second == generation_;
  });
}

void SubagentCoordinatorMaintenanceReservation::release() noexcept
{
  if (!coordinator_ || generation_ == 0)
    return;
  try
  {
    std::lock_guard lock(coordinator_->mutex_);
    for (auto const& parent : parent_session_ids_)
    {
      auto found = coordinator_->maintenance_parents_.find(parent);
      if (found != coordinator_->maintenance_parents_.end() && found->second == generation_)
        coordinator_->maintenance_parents_.erase(found);
    }
    coordinator_->admission_changed_.notify_all();
  }
  catch (...)
  {
  }
  coordinator_.reset();
  parent_session_ids_.clear();
  generation_ = 0;
}

SubagentCoordinator::StartAdmission::~StartAdmission()
{
  std::lock_guard lock(coordinator_.mutex_);
  --coordinator_.active_starts_;
  auto found = coordinator_.active_starts_by_parent_.find(*parent_session_id_);
  if (found != coordinator_.active_starts_by_parent_.end())
  {
    if (--found->second == 0)
      coordinator_.active_starts_by_parent_.erase(found);
  }
  coordinator_.admission_changed_.notify_all();
}

std::shared_ptr<SubagentCoordinator::JobState> SubagentCoordinator::find_owned_locked(std::string_view parent_session_id, std::string_view job_id) const
{
  auto found = jobs_.find(std::string(job_id));
  if (found == jobs_.end())
    return nullptr;
  std::lock_guard state_lock(found->second->mutex);
  if (!found->second->published || found->second->snapshot.identity.parent_session_id != parent_session_id)
    return nullptr;
  return found->second;
}

SubagentCoordinatorJobSnapshot SubagentCoordinator::public_snapshot_locked(JobState const& state, bool timed_out) const
{
  return {.job = state.snapshot, .timed_out = timed_out, .coordinator_latched = false, .coordinator_error = std::nullopt};
}

bool SubagentCoordinator::erase_oldest_eligible_locked()
{
  auto oldest = jobs_.end();
  std::size_t oldest_sequence = 0;
  for (auto candidate = jobs_.begin(); candidate != jobs_.end(); ++candidate)
  {
    std::lock_guard state_lock(candidate->second->mutex);
    auto const& state = *candidate->second;
    bool const eligible = state.published && state.delivery_arm_ready && terminal(state.snapshot.execution) &&
                          (state.snapshot.delivery == SubagentDeliveryState::Direct || state.snapshot.delivery == SubagentDeliveryState::Acknowledged ||
                           state.delivery_exhausted);
    if (!eligible)
      continue;
    if (oldest == jobs_.end() || state.sequence < oldest_sequence)
    {
      oldest = candidate;
      oldest_sequence = state.sequence;
    }
  }
  if (oldest == jobs_.end())
    return false;
  {
    std::lock_guard state_lock(oldest->second->mutex);
    // Eviction invalidates the source epoch so late live/freeze results cannot
    // store. Published frames may remain only while JobState is still held.
    ++oldest->second->source_epoch;
    if (oldest->second->source_epoch == 0)
      oldest->second->source_epoch = 1;
    oldest->second->inspection_source = nullptr;
    oldest->second->freeze_pending = false;
  }
  jobs_.erase(oldest);
  return true;
}

void SubagentCoordinator::prune_eligible_locked()
{
  std::size_t eligible = 0;
  for (auto const& [_, state] : jobs_)
  {
    std::lock_guard state_lock(state->mutex);
    if (state->published && state->delivery_arm_ready && terminal(state->snapshot.execution) &&
        (state->snapshot.delivery == SubagentDeliveryState::Direct || state->snapshot.delivery == SubagentDeliveryState::Acknowledged ||
         state->delivery_exhausted))
      ++eligible;
  }
  while (eligible > options_.registry_options.max_retained_finished_jobs && erase_oldest_eligible_locked())
    --eligible;
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::start(SubagentCoordinatorStartRequest request, BackgroundJobWorker worker,
                                                                             std::shared_ptr<SubagentInteractionGate> interaction_gate,
                                                                             std::shared_ptr<SubagentLiveInspectionSource> inspection_source)
{
  auto parent_session_id = std::move(request.parent_session_id);
  auto const mode = request.mode;
  auto options = std::move(request.job);
  auto launch_display = std::move(request.launch_display);
  auto steering_queue = std::move(request.steering_queue);
  auto history_append = std::move(request.history_append);
  if (!worker)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "background job worker is unavailable");
    mark_unpublished(error);
    return std::unexpected(std::move(error));
  }
  if (auto valid = validate_identifier("parent_session_id", parent_session_id, {}); !valid)
  {
    auto error = std::move(valid.error());
    mark_unpublished(error);
    return std::unexpected(std::move(error));
  }
  if (auto valid = validate_identifier("child_session_id", options.child_session_id, {}); !valid)
  {
    auto error = std::move(valid.error());
    mark_unpublished(error);
    return std::unexpected(std::move(error));
  }
  // Coordinated sources must be persistent and bound to the exact child session
  // identity before any worker launch or process-local publication.
  if (inspection_source)
  {
    if (inspection_source->is_ephemeral() || inspection_source->session_id() != options.child_session_id)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "subagent inspection source does not match the child session");
      error.with_context("child_session_id", options.child_session_id);
      if (!inspection_source->session_id().empty())
        error.with_context("source_session_id", inspection_source->session_id());
      error.with_context("source_ephemeral", inspection_source->is_ephemeral() ? "true" : "false");
      mark_unpublished(error);
      return std::unexpected(std::move(error));
    }
  }

  std::shared_ptr<JobState> state;
  SubagentJobIdentity identity;
  {
    std::lock_guard lock(mutex_);
    if (!accepting_)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "subagent coordinator is shutting down");
      mark_unpublished(error);
      return std::unexpected(std::move(error));
    }
    if (maintenance_parents_.contains(parent_session_id))
    {
      auto error = coordinator_maintenance_error("start_reserved");
      mark_unpublished(error);
      return std::unexpected(std::move(error));
    }
    ++active_starts_by_parent_[parent_session_id];
    ++active_starts_;
  }
  StartAdmission admission(*this, parent_session_id);

  auto require_capacity_locked = [&]() -> ava::core::VoidResult {
    prune_eligible_locked();
    auto const hard_cap = options_.registry_options.max_running_jobs + options_.registry_options.max_retained_finished_jobs;
    while (jobs_.size() >= hard_cap && erase_oldest_eligible_locked())
    {
    }
    if (jobs_.size() < hard_cap)
      return {};
    auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "subagent coordinator state limit reached by active or pending jobs");
    error.with_context("max_running_jobs", std::to_string(options_.registry_options.max_running_jobs))
        .with_context("max_retained_finished_jobs", std::to_string(options_.registry_options.max_retained_finished_jobs));
    return std::unexpected(std::move(error));
  };
  {
    std::lock_guard lock(mutex_);
    if (auto capacity = require_capacity_locked(); !capacity)
    {
      auto error = std::move(capacity.error());
      mark_unpublished(error);
      return std::unexpected(std::move(error));
    }
  }

  for (std::size_t attempt = 0; attempt < kMaxIdentityGenerationAttempts && !state; ++attempt)
  {
    auto job_id = generate_identity_candidate(options_, "job");
    if (!job_id)
    {
      auto error = std::move(job_id.error());
      mark_unpublished(error);
      return std::unexpected(std::move(error));
    }
    auto delivery_id = generate_identity_candidate(options_, "delivery");
    if (!delivery_id)
    {
      auto error = std::move(delivery_id.error());
      mark_unpublished(error);
      return std::unexpected(std::move(error));
    }

    auto candidate = std::make_shared<JobState>();
    auto const now = ava::session::now_timestamp();
    candidate->snapshot = {.schema_version = kSubagentJobContractVersion,
                           .identity = {.job_id = std::move(*job_id),
                                        .task_id = options.child_session_id,
                                        .parent_session_id = parent_session_id,
                                        .child_session_id = options.child_session_id,
                                        .delivery_id = std::move(*delivery_id)},
                           .mode = mode,
                           .execution = SubagentExecutionState::Starting,
                           .delivery = SubagentDeliveryState::Direct,
                           .started_at = now,
                           .updated_at = now,
                           .display_title = make_display_field(options.title, kMaxDisplayTitleBytes),
                           .display_subagent_type = make_display_field(options.subagent_type, kMaxDisplaySubagentTypeBytes),
                           .launch_display = launch_display};
    candidate->interaction_gate = interaction_gate;
    candidate->steering_queue = steering_queue;
    candidate->history_append = history_append;
    // Store the source before registry start/publication so inspect can observe
    // the child as soon as the job becomes visible.
    candidate->inspection_source = inspection_source;

    std::lock_guard lock(mutex_);
    if (auto capacity = require_capacity_locked(); !capacity)
    {
      auto error = std::move(capacity.error());
      mark_unpublished(error);
      return std::unexpected(std::move(error));
    }
    if (jobs_.contains(candidate->snapshot.identity.job_id))
      continue;
    bool delivery_id_exists = false;
    for (auto const& [_, current] : jobs_)
    {
      std::lock_guard state_lock(current->mutex);
      if (current->snapshot.identity.delivery_id == candidate->snapshot.identity.delivery_id)
      {
        delivery_id_exists = true;
        break;
      }
    }
    if (delivery_id_exists)
      continue;
    candidate->sequence = next_job_sequence_;
    auto [_, inserted] = jobs_.emplace(candidate->snapshot.identity.job_id, candidate);
    if (!inserted)
      continue;
    ++next_job_sequence_;
    identity = candidate->snapshot.identity;
    state = std::move(candidate);
  }
  if (!state)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to allocate unique subagent job and delivery identities");
    error.with_context("attempts", std::to_string(kMaxIdentityGenerationAttempts));
    mark_unpublished(error);
    return std::unexpected(std::move(error));
  }

  options.job_id = identity.job_id;
  if (auto persisted = persist_job_history(history_append, state->snapshot, ava::session::SubagentJobHistoryPhase::Start); !persisted)
  {
    {
      std::lock_guard lock(mutex_);
      auto found = jobs_.find(identity.job_id);
      if (found != jobs_.end() && found->second == state)
        jobs_.erase(found);
    }
    auto error = std::move(persisted.error());
    mark_unpublished(error);
    return std::unexpected(std::move(error));
  }
  auto started = registry_.start(std::move(options), [this, state, worker = std::move(worker)](BackgroundJobContext const& context) mutable {
    BackgroundJobCompletion completion;
    try
    {
      completion = worker(context);
    }
    catch (std::exception const& error)
    {
      completion = exception_completion(context.job_id, context.stop_token, error.what());
    }
    catch (...)
    {
      completion = exception_completion(context.job_id, context.stop_token, "unknown exception");
    }
    return complete(state, std::move(completion));
  });
  if (!started)
  {
    {
      std::lock_guard lock(mutex_);
      auto found = jobs_.find(identity.job_id);
      if (found != jobs_.end() && found->second == state)
        jobs_.erase(found);
    }
    auto error = std::move(started.error());
    mark_unpublished(error);
    return std::unexpected(std::move(error));
  }

  SubagentCoordinatorJobSnapshot result;
  {
    std::lock_guard lock(mutex_);
    std::lock_guard state_lock(state->mutex);
    if (state->snapshot.execution == SubagentExecutionState::Starting)
    {
      state->snapshot.execution = SubagentExecutionState::Running;
      state->snapshot.updated_at = ava::session::now_timestamp();
    }
    state->published = true;
    result = public_snapshot_locked(*state);
    state->changed.notify_all();
  }
  {
    std::lock_guard lock(mutex_);
    prune_eligible_locked();
  }
  publish_terminal_notification(state);
  return result;
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::start(std::string parent_session_id, SubagentJobMode mode,
                                                                             BackgroundJobStartOptions options, BackgroundJobWorker worker,
                                                                             std::shared_ptr<SubagentInteractionGate> interaction_gate,
                                                                             std::shared_ptr<SubagentLiveInspectionSource> inspection_source)
{
  return start(SubagentCoordinatorStartRequest{.parent_session_id = std::move(parent_session_id), .mode = mode, .job = std::move(options)}, std::move(worker),
               std::move(interaction_gate), std::move(inspection_source));
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::start_background(std::string parent_session_id, BackgroundJobStartOptions options,
                                                                                        BackgroundJobWorker worker,
                                                                                        std::shared_ptr<SubagentLiveInspectionSource> inspection_source)
{
  return start(std::move(parent_session_id), SubagentJobMode::Background, std::move(options), std::move(worker), nullptr, std::move(inspection_source));
}

ava::core::Result<SubagentCoordinatorMaintenanceReservation> SubagentCoordinator::reserve_parent_maintenance(std::vector<std::string> parent_session_ids)
{
  std::ranges::sort(parent_session_ids);
  auto const unique = std::ranges::unique(parent_session_ids);
  parent_session_ids.erase(unique.begin(), unique.end());

  std::lock_guard lock(mutex_);
  if (!accepting_)
    return std::unexpected(coordinator_maintenance_error("coordinator_unavailable"));
  for (auto const& parent : parent_session_ids)
  {
    if (maintenance_parents_.contains(parent))
      return std::unexpected(coordinator_maintenance_error("already_reserved"));
    if (auto active = active_starts_by_parent_.find(parent); active != active_starts_by_parent_.end() && active->second > 0)
      return std::unexpected(coordinator_maintenance_error("start_in_progress"));
  }
  for (auto const& [_, state] : jobs_)
  {
    std::lock_guard state_lock(state->mutex);
    if (!std::ranges::binary_search(parent_session_ids, state->snapshot.identity.parent_session_id))
      continue;
    if (state->snapshot.execution == SubagentExecutionState::Starting || state->snapshot.execution == SubagentExecutionState::Running)
      return std::unexpected(coordinator_maintenance_error("job_active"));
  }

  auto const generation = next_maintenance_generation_++;
  if (next_maintenance_generation_ == 0)
    next_maintenance_generation_ = 1;
  for (auto const& parent : parent_session_ids)
    maintenance_parents_.emplace(parent, generation);
  return SubagentCoordinatorMaintenanceReservation(shared_from_this(), std::move(parent_session_ids), generation);
}

BackgroundJobCompletion SubagentCoordinator::complete(std::shared_ptr<JobState> const& state, BackgroundJobCompletion completion)
{
  std::string job_id;
  {
    std::lock_guard state_lock(state->mutex);
    job_id = state->snapshot.identity.job_id;
  }
  completion = normalize_completion(job_id, std::move(completion));
  auto const now = ava::session::now_timestamp();
  std::shared_ptr<SubagentLiveInspectionSource> freeze_source;
  std::uint64_t freeze_epoch = 0;
  SubagentJobHistoryAppend history_append;
  SubagentJobSnapshot history_snapshot;
  {
    std::lock_guard state_lock(state->mutex);
    if (terminal(state->snapshot.execution))
      return completion;
    state->snapshot.terminal_at = now;
    state->snapshot.updated_at = now;
    state->snapshot.provider_iterations = std::min(completion.provider_iterations, kMaxAccountingValue);
    state->snapshot.tool_calls = std::min(completion.tool_calls, kMaxAccountingValue);
    state->snapshot.tool_iterations = std::min(completion.tool_iterations, kMaxAccountingValue);
    if (completion.state == BackgroundJobState::Completed)
    {
      state->snapshot.execution = SubagentExecutionState::Completed;
      state->snapshot.summary = std::move(completion.final_text);
      state->snapshot.summary_truncated = normalize_text(*state->snapshot.summary, kMaxSummaryBytes, "subagent output unavailable");
      state->snapshot.stop_reason = std::move(completion.stop_reason);
      state->snapshot.stop_reason_truncated = normalize_text(*state->snapshot.stop_reason, kMaxStopReasonBytes, "completed");
    }
    else if (completion.state == BackgroundJobState::Canceled)
    {
      state->snapshot.execution = SubagentExecutionState::Canceled;
      state->snapshot.stop_reason = std::move(completion.stop_reason);
      state->snapshot.stop_reason_truncated = normalize_text(*state->snapshot.stop_reason, kMaxStopReasonBytes, "canceled");
    }
    else
    {
      state->snapshot.execution = SubagentExecutionState::Failed;
      state->snapshot.error_category = completion.error ? safe_subagent_error_category(*completion.error) : "unknown";
      state->snapshot.error = completion.error ? safe_subagent_error_message(*completion.error) : "subagent job failed";
      state->snapshot.error_truncated = normalize_text(*state->snapshot.error, kMaxErrorBytes, "subagent job failed");
    }
    if (state->snapshot.mode == SubagentJobMode::Background)
      state->delivery_arm_ready = false;
    if (state->steering_queue)
      state->steering_queue->close();
    // Terminal inspection handoff: move the source local, bump source epoch so
    // late live publishes cannot store, preserve the latest successful live
    // frame through freeze_pending, and never reacquire by path.
    freeze_source = std::move(state->inspection_source);
    state->inspection_source = nullptr;
    ++state->source_epoch;
    if (state->source_epoch == 0)
      state->source_epoch = 1;
    freeze_epoch = state->source_epoch;
    state->freeze_pending = static_cast<bool>(freeze_source);
    history_append = state->history_append;
    history_snapshot = state->snapshot;
    state->changed.notify_all();
  }

  if (freeze_source)
  {
    // Bounded final projection outside locks. No I/O or callbacks under locks.
    auto projected = freeze_source->project_messages();
    auto fingerprint = freeze_source->content_fingerprint();
    freeze_source.reset();
    {
      std::lock_guard state_lock(state->mutex);
      if (state->freeze_pending && state->source_epoch == freeze_epoch)
      {
        if (projected)
        {
          state->content_generation = advance_content_generation(state->content_generation);
          state->published_inspection = stamp_inspection_frame(state->content_generation, true, false, false, false, std::move(*projected));
          if (fingerprint)
            state->published_fingerprint = std::move(*fingerprint);
        }
        else if (state->published_inspection)
        {
          // Retain prior messages and mark a truthful path-free final-unavailable flag.
          state->content_generation = advance_content_generation(state->content_generation);
          state->published_inspection = stamp_inspection_frame(state->content_generation, true, false, false, true, state->published_inspection->messages);
        }
        else
        {
          state->content_generation = advance_content_generation(state->content_generation);
          state->published_inspection = make_unavailable_inspection_frame(state->content_generation, true, false);
          state->published_fingerprint = std::nullopt;
        }
        state->freeze_pending = false;
        state->changed.notify_all();
      }
    }
  }

  // History is display-only and must not reorder ahead of the start record.
  // Persist the normalized terminal snapshot outside locks, then arm delivery.
  static_cast<void>(persist_job_history(history_append, history_snapshot, ava::session::SubagentJobHistoryPhase::Terminal));

  {
    std::lock_guard state_lock(state->mutex);
    if (state->snapshot.mode == SubagentJobMode::Background && terminal(state->snapshot.execution) && !state->delivery_arm_ready)
    {
      auto const armed_at = ava::session::now_timestamp();
      state->snapshot.delivery = SubagentDeliveryState::Pending;
      state->snapshot.delivery_pending_at = armed_at;
      state->snapshot.updated_at = armed_at;
      state->terminal_notification_pending = true;
      state->delivery_arm_ready = true;
      state->changed.notify_all();
    }
  }

  // Delay terminal sink until freeze is stable and history append has been attempted
  // so observers never race an empty gap or pre-history automatic delivery.
  publish_terminal_notification(state);

  {
    std::lock_guard lock(mutex_);
    prune_eligible_locked();
  }
  return completion;
}

void SubagentCoordinator::publish_terminal_notification(std::shared_ptr<JobState> const& state) noexcept
{
  try
  {
    SubagentTerminalSink sink;
    SubagentCoordinatorJobSnapshot notification;
    {
      std::lock_guard lock(mutex_);
      std::lock_guard state_lock(state->mutex);
      if (!state->published || !state->terminal_notification_pending || state->terminal_notification_emitted || state->delivery_exhausted || !terminal_sink_)
        return;
      sink = terminal_sink_;
      notification = public_snapshot_locked(*state);
      state->terminal_notification_emitted = true;
    }
    sink(notification);
  }
  catch (...)
  {
  }
}

std::vector<SubagentCoordinatorJobSnapshot> SubagentCoordinator::list(std::string_view parent_session_id) const
{
  std::vector<std::pair<std::size_t, SubagentCoordinatorJobSnapshot>> ordered;
  {
    std::lock_guard lock(mutex_);
    for (auto const& [_, state] : jobs_)
    {
      std::lock_guard state_lock(state->mutex);
      if (state->published && state->snapshot.identity.parent_session_id == parent_session_id)
        ordered.emplace_back(state->sequence, public_snapshot_locked(*state));
    }
  }
  std::ranges::sort(ordered, [](auto const& left, auto const& right) { return left.first < right.first; });
  std::vector<SubagentCoordinatorJobSnapshot> result;
  result.reserve(ordered.size());
  for (auto& [_, snapshot] : ordered)
    result.push_back(std::move(snapshot));
  return result;
}

ava::core::Result<std::vector<SubagentCoordinatorJobSnapshot>> SubagentCoordinator::pending_deliveries(std::string_view parent_session_id)
{
  std::vector<std::pair<std::size_t, SubagentCoordinatorJobSnapshot>> ordered;
  {
    std::lock_guard lock(mutex_);
    for (auto const& [_, state] : jobs_)
    {
      std::lock_guard state_lock(state->mutex);
      if (state->published && state->snapshot.identity.parent_session_id == parent_session_id && delivery_pending(state->snapshot.delivery) &&
          !state->delivery_exhausted)
        ordered.emplace_back(state->sequence, public_snapshot_locked(*state));
    }
  }
  std::ranges::sort(ordered, [](auto const& left, auto const& right) { return left.first < right.first; });
  std::vector<SubagentCoordinatorJobSnapshot> result;
  result.reserve(ordered.size());
  for (auto& [_, snapshot] : ordered)
    result.push_back(std::move(snapshot));
  return result;
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::snapshot(std::string_view parent_session_id, std::string_view job_id)
{
  std::shared_ptr<JobState> state;
  {
    std::lock_guard lock(mutex_);
    state = find_owned_locked(parent_session_id, job_id);
  }
  if (!state)
    return std::unexpected(not_found(job_id));
  std::lock_guard state_lock(state->mutex);
  return public_snapshot_locked(*state);
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::wait(std::string_view parent_session_id, std::string_view job_id,
                                                                            std::chrono::milliseconds timeout, SubagentWaitMode mode)
{
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling SubagentCoordinator::wait");

  std::shared_ptr<JobState> state;
  {
    std::lock_guard lock(mutex_);
    state = find_owned_locked(parent_session_id, job_id);
  }
  if (!state)
    return std::unexpected(not_found(job_id));

  auto const deadline = std::chrono::steady_clock::now() + timeout;
  std::unique_lock state_lock(state->mutex);
  auto const ready = [&] { return terminal(state->snapshot.execution) || (mode == SubagentWaitMode::TerminalOrPromotion && state->snapshot.was_promoted); };
  if (!ready() && !state->changed.wait_for(state_lock, timeout, ready))
    return public_snapshot_locked(*state, true);
  bool const is_terminal = terminal(state->snapshot.execution);
  auto result = public_snapshot_locked(*state);
  state_lock.unlock();

  if (is_terminal)
  {
    auto const now = std::chrono::steady_clock::now();
    auto const remaining = now < deadline ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) : std::chrono::milliseconds(0);
    auto registry_snapshot = registry_.wait_snapshot(job_id, remaining);
    if (registry_snapshot && registry_snapshot->timed_out)
      result.timed_out = true;
    else if (registry_snapshot)
      static_cast<void>(registry_.join_finished());
    // A low-level record may already have been joined and pruned. The
    // coordinator's terminal state remains authoritative in that case.
  }
  return result;
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::result(std::string_view parent_session_id, std::string_view job_id)
{
  auto current = snapshot(parent_session_id, job_id);
  if (!current)
    return std::unexpected(std::move(current.error()));
  if (!terminal(current->job.execution))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "subagent job is not ready");
    error.with_context("job_error_code", "job_not_ready").with_context("job_id", std::string(job_id));
    return std::unexpected(std::move(error));
  }
  return current;
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::cancel(std::string_view parent_session_id, std::string_view job_id)
{
  std::shared_ptr<JobState> state;
  {
    std::lock_guard lock(mutex_);
    state = find_owned_locked(parent_session_id, job_id);
  }
  if (!state)
    return std::unexpected(not_found(job_id));

  bool request_registry_cancel = false;
  {
    std::lock_guard state_lock(state->mutex);
    if (!terminal(state->snapshot.execution) && !state->snapshot.cancel_requested)
    {
      state->snapshot.cancel_requested = true;
      state->snapshot.cancel_requested_at = ava::session::now_timestamp();
      state->snapshot.updated_at = *state->snapshot.cancel_requested_at;
      state->changed.notify_all();
    }
    request_registry_cancel = !terminal(state->snapshot.execution);
  }
  if (request_registry_cancel)
  {
    auto canceled = registry_.cancel(job_id);
    if (!canceled)
    {
      std::lock_guard state_lock(state->mutex);
      if (!terminal(state->snapshot.execution))
        return std::unexpected(std::move(canceled.error()));
    }
  }
  std::lock_guard state_lock(state->mutex);
  return public_snapshot_locked(*state);
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::steer(std::string_view parent_session_id, std::string_view job_id, std::string message)
{
  std::shared_ptr<JobState> state;
  {
    std::lock_guard lock(mutex_);
    state = find_owned_locked(parent_session_id, job_id);
  }
  if (!state)
    return std::unexpected(not_found(job_id));

  std::lock_guard state_lock(state->mutex);
  if (terminal(state->snapshot.execution))
    return std::unexpected(invalid_transition("cannot steer a terminal subagent job", job_id));
  if (state->snapshot.cancel_requested)
    return std::unexpected(invalid_transition("cannot steer a subagent after cancellation was requested", job_id));
  if (state->snapshot.execution != SubagentExecutionState::Running || !state->steering_queue)
    return std::unexpected(invalid_transition("subagent job does not accept steering", job_id));
  if (auto queued = state->steering_queue->enqueue(std::move(message)); !queued)
    return std::unexpected(std::move(queued.error()));
  return public_snapshot_locked(*state);
}

ava::core::Result<std::shared_ptr<SubagentInspectorFrame const>> SubagentCoordinator::inspect(std::string_view parent_session_id, std::string_view job_id,
                                                                                              std::optional<std::uint64_t> known_generation)
{
  std::shared_ptr<JobState> state;
  {
    std::lock_guard lock(mutex_);
    state = find_owned_locked(parent_session_id, job_id);
  }
  if (!state)
    return std::unexpected(not_found(job_id));

  std::shared_ptr<SubagentLiveInspectionSource> source;
  std::shared_ptr<SubagentInspectorFrame const> published;
  std::optional<ava::session::SessionContentFingerprint> published_fingerprint;
  std::uint64_t source_epoch = 0;
  std::uint64_t content_generation = 0;
  bool freeze_pending = false;
  bool is_terminal = false;
  std::function<void()> after_capture_hook;
  std::function<void()> before_publish_hook;
  {
    std::lock_guard state_lock(state->mutex);
    source = state->inspection_source;
    published = state->published_inspection;
    published_fingerprint = state->published_fingerprint;
    source_epoch = state->source_epoch;
    content_generation = state->content_generation;
    freeze_pending = state->freeze_pending;
    is_terminal = terminal(state->snapshot.execution);
    after_capture_hook = options_.inspect_after_source_capture_for_test;
    before_publish_hook = options_.inspect_before_publish_for_test;
  }
  SubagentLiveInspectionSource* const captured_source = source.get();
  // Captured after the post-seam recheck below; used at final publish to reject
  // older projections when a concurrent inspector already advanced generation.
  std::uint64_t captured_content_generation = content_generation;

  auto current_frame_locked = [&]() -> std::shared_ptr<SubagentInspectorFrame const> {
    auto const current_published = state->published_inspection;
    auto const current_terminal = terminal(state->snapshot.execution);
    auto const current_freeze_pending = state->freeze_pending;
    auto const current_generation = state->content_generation;
    if (current_published)
      return view_published_frame(current_published, current_terminal, current_freeze_pending);
    return make_unavailable_inspection_frame(current_generation, current_terminal, current_freeze_pending);
  };

  // Stable terminal publication: no live source and freeze is complete.
  if (!source && is_terminal && !freeze_pending)
  {
    if (published)
    {
      if (known_generation && *known_generation == published->generation)
        return make_not_modified_inspection_frame(published->generation, true, false);
      return view_published_frame(published, true, false);
    }
    return make_unavailable_inspection_frame(content_generation, true, false);
  }

  // Freeze in progress: preserve latest successful live content + terminal metadata.
  if (!source && freeze_pending)
  {
    if (published)
      return view_published_frame(published, true, true);
    return make_unavailable_inspection_frame(content_generation, true, true);
  }

  if (!source)
  {
    // Missing source without terminal freeze (legacy/test jobs or shutdown mid-run).
    if (published)
      return view_published_frame(published, is_terminal, freeze_pending);
    return make_unavailable_inspection_frame(content_generation, is_terminal, freeze_pending);
  }

  // Deterministic race seam: pause after live source/epoch capture before project.
  if (after_capture_hook)
    after_capture_hook();

  // Re-check after the test seam: complete/shutdown may have invalidated the source.
  // Capture content_generation here (post-recheck, pre-fingerprint/project) so the
  // final publish lock can detect concurrent generation advances and refuse to
  // store an older projection under a newer generation number.
  {
    std::lock_guard state_lock(state->mutex);
    if (state->source_epoch != source_epoch || state->inspection_source.get() != captured_source)
      return current_frame_locked();
    is_terminal = terminal(state->snapshot.execution);
    freeze_pending = state->freeze_pending;
    published = state->published_inspection;
    published_fingerprint = state->published_fingerprint;
    content_generation = state->content_generation;
    captured_content_generation = content_generation;
  }

  auto fingerprint = source->content_fingerprint();
  if (!fingerprint)
  {
    // Path-free: never surface raw fingerprint/session errors to inspect callers.
    std::lock_guard state_lock(state->mutex);
    if (state->source_epoch != source_epoch || state->inspection_source.get() != captured_source)
      return current_frame_locked();
    if (state->published_inspection)
      return make_refresh_unavailable_inspection_frame(state->published_inspection->generation, terminal(state->snapshot.execution), state->freeze_pending,
                                                       state->published_inspection->messages);
    return make_refresh_unavailable_inspection_frame(state->content_generation, terminal(state->snapshot.execution), state->freeze_pending);
  }

  if (known_generation && published && *known_generation == published->generation && published_fingerprint && *published_fingerprint == *fingerprint)
  {
    std::lock_guard state_lock(state->mutex);
    if (state->source_epoch != source_epoch || state->inspection_source.get() != captured_source)
      return current_frame_locked();
    if (state->published_inspection && state->published_fingerprint && *state->published_fingerprint == *fingerprint && known_generation &&
        *known_generation == state->published_inspection->generation)
      return make_not_modified_inspection_frame(state->published_inspection->generation, terminal(state->snapshot.execution), state->freeze_pending);
    if (state->published_inspection)
      return view_published_frame(state->published_inspection, terminal(state->snapshot.execution), state->freeze_pending);
    return current_frame_locked();
  }

  auto projected = source->project_messages();
  if (!projected)
  {
    // Corrupt/over-cap/load failures become path-free flags only.
    std::lock_guard state_lock(state->mutex);
    if (state->source_epoch != source_epoch || state->inspection_source.get() != captured_source)
      return current_frame_locked();
    if (state->published_inspection)
      return make_refresh_unavailable_inspection_frame(state->published_inspection->generation, terminal(state->snapshot.execution), state->freeze_pending,
                                                       state->published_inspection->messages);
    return make_refresh_unavailable_inspection_frame(state->content_generation, terminal(state->snapshot.execution), state->freeze_pending);
  }

  // Deterministic race seam: pause after fingerprint+project, before publish.
  // Outside locks; lets a concurrent inspector publish a newer generation first.
  if (before_publish_hook)
    before_publish_hook();

  {
    std::lock_guard state_lock(state->mutex);
    // ARCH-INS-06: discard stale live results after complete/shutdown publication.
    if (state->source_epoch != source_epoch || state->inspection_source.get() != captured_source)
      return current_frame_locked();

    // Another concurrent inspector may already have published this fingerprint.
    // Return the cached current frame without advancing generation.
    if (state->published_fingerprint && *state->published_fingerprint == *fingerprint && state->published_inspection)
      return view_published_frame(state->published_inspection, terminal(state->snapshot.execution), state->freeze_pending);

    // ARCH-INS-09: if content_generation moved since our post-recheck capture,
    // a concurrent inspector already published a newer projection. Discard this
    // older projection rather than advancing generation with stale content.
    // No post-project fingerprint recheck: fingerprint I/O stays outside the
    // lock once; same-fingerprint hits the cache path above, and a generation
    // advance with a different fingerprint is exactly the concurrent-publish
    // case this check covers. A fingerprint/content skew from an append between
    // fingerprint and project cannot regress published content (projection reads
    // at least as new as the fingerprint) and self-heals on the next inspect.
    if (state->content_generation != captured_content_generation)
      return current_frame_locked();

    state->content_generation = advance_content_generation(state->content_generation);
    auto frame =
        stamp_inspection_frame(state->content_generation, terminal(state->snapshot.execution), state->freeze_pending, false, false, std::move(*projected));
    state->published_inspection = frame;
    state->published_fingerprint = std::move(*fingerprint);
    return frame;
  }
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::promote(std::string_view parent_session_id, std::string_view job_id)
{
  std::shared_ptr<JobState> state;
  {
    std::lock_guard lock(mutex_);
    state = find_owned_locked(parent_session_id, job_id);
  }
  if (!state)
    return std::unexpected(not_found(job_id));

  std::shared_ptr<SubagentInteractionGate> gate;
  {
    std::lock_guard state_lock(state->mutex);
    if (terminal(state->snapshot.execution) || state->snapshot.was_promoted)
      return public_snapshot_locked(*state);
    if (state->snapshot.mode != SubagentJobMode::Foreground)
      return std::unexpected(invalid_transition("only a running foreground subagent can be promoted", job_id));
    if (state->snapshot.cancel_requested)
      return std::unexpected(invalid_transition("cannot promote a subagent after cancellation was requested", job_id));
    gate = state->interaction_gate;
  }
  if (gate)
    if (auto prepared = gate->prepare_promotion(); !prepared)
      return std::unexpected(std::move(prepared.error()));

  bool commit_gate = false;
  bool cancellation_won = false;
  SubagentCoordinatorJobSnapshot result;
  {
    std::lock_guard state_lock(state->mutex);
    if (terminal(state->snapshot.execution))
    {
      result = public_snapshot_locked(*state);
    }
    else if (state->snapshot.cancel_requested)
    {
      cancellation_won = true;
    }
    else
    {
      if (!state->snapshot.was_promoted)
      {
        state->snapshot.mode = SubagentJobMode::Background;
        state->snapshot.was_promoted = true;
        state->snapshot.promoted_at = ava::session::now_timestamp();
        state->snapshot.updated_at = *state->snapshot.promoted_at;
        state->changed.notify_all();
      }
      commit_gate = true;
      result = public_snapshot_locked(*state);
    }
  }
  if (gate)
  {
    if (commit_gate)
      gate->commit_promotion();
    else
      gate->abort_promotion();
  }
  if (cancellation_won)
    return std::unexpected(invalid_transition("cannot promote a subagent after cancellation was requested", job_id));
  return result;
}

void SubagentCoordinator::set_terminal_sink(SubagentTerminalSink sink)
{
  std::vector<std::shared_ptr<JobState>> pending;
  {
    std::lock_guard lock(mutex_);
    terminal_sink_ = std::move(sink);
    if (terminal_sink_)
    {
      for (auto const& [_, state] : jobs_)
      {
        std::lock_guard state_lock(state->mutex);
        if (state->published && state->terminal_notification_pending && !state->terminal_notification_emitted && !state->delivery_exhausted)
          pending.push_back(state);
      }
    }
  }
  for (auto const& state : pending)
    publish_terminal_notification(state);
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::record_delivery_attempt(std::string_view parent_session_id, std::string_view job_id,
                                                                                               std::string attempt_id, std::string prompt_fingerprint)
{
  if (auto valid = validate_identifier("attempt_id", attempt_id, job_id); !valid)
    return std::unexpected(std::move(valid.error()));
  if (auto valid = validate_identifier("prompt_fingerprint", prompt_fingerprint, job_id); !valid)
    return std::unexpected(std::move(valid.error()));
  std::shared_ptr<JobState> state;
  {
    std::lock_guard lock(mutex_);
    state = find_owned_locked(parent_session_id, job_id);
  }
  if (!state)
    return std::unexpected(not_found(job_id));

  std::lock_guard state_lock(state->mutex);
  if (!terminal(state->snapshot.execution) || !delivery_pending(state->snapshot.delivery) || state->delivery_exhausted)
    return std::unexpected(invalid_transition("subagent delivery is not pending", job_id));
  if (!state->snapshot.delivery_attempt_history.empty() && state->snapshot.delivery_attempt_history.back().attempt_id == attempt_id)
  {
    if (state->snapshot.delivery_attempt_history.back().prompt_fingerprint != prompt_fingerprint)
      return std::unexpected(invalid_transition("subagent delivery attempt identity was reused with different content", job_id));
    return public_snapshot_locked(*state);
  }
  if (state->snapshot.delivery_attempt_history.size() >= kMaxSubagentDeliveryAttemptHistory)
    return std::unexpected(invalid_transition("subagent delivery attempt history limit reached", job_id));
  auto const now = ava::session::now_timestamp();
  state->snapshot.delivery = SubagentDeliveryState::Attempting;
  ++state->snapshot.delivery_attempts;
  state->snapshot.delivery_attempt_history.push_back(
      {.attempt_id = std::move(attempt_id), .prompt_fingerprint = std::move(prompt_fingerprint), .attempted_at = now});
  state->snapshot.last_delivery_attempt_at = now;
  state->snapshot.updated_at = now;
  state->changed.notify_all();
  return public_snapshot_locked(*state);
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::acknowledge_delivery(std::string_view parent_session_id, std::string_view job_id,
                                                                                            std::string_view attempt_id, std::string committed_turn_id)
{
  if (auto valid = validate_identifier("attempt_id", attempt_id, job_id); !valid)
    return std::unexpected(std::move(valid.error()));
  if (auto valid = validate_identifier("committed_turn_id", committed_turn_id, job_id); !valid)
    return std::unexpected(std::move(valid.error()));
  std::shared_ptr<JobState> state;
  {
    std::lock_guard lock(mutex_);
    state = find_owned_locked(parent_session_id, job_id);
  }
  if (!state)
    return std::unexpected(not_found(job_id));
  {
    std::lock_guard state_lock(state->mutex);
    if (state->snapshot.delivery == SubagentDeliveryState::Acknowledged)
    {
      if (state->snapshot.acknowledged_attempt_id == attempt_id && state->snapshot.committed_turn_id == committed_turn_id)
        return public_snapshot_locked(*state);
      return std::unexpected(invalid_transition("subagent delivery was already acknowledged by another attempt", job_id));
    }
    if (state->delivery_exhausted || state->snapshot.delivery != SubagentDeliveryState::Attempting || state->snapshot.delivery_attempt_history.empty() ||
        state->snapshot.delivery_attempt_history.back().attempt_id != attempt_id)
      return std::unexpected(invalid_transition("subagent delivery acknowledgement does not match the latest attempt", job_id));
    auto const now = ava::session::now_timestamp();
    state->snapshot.delivery = SubagentDeliveryState::Acknowledged;
    state->snapshot.acknowledged_attempt_id = std::string(attempt_id);
    state->snapshot.committed_turn_id = std::move(committed_turn_id);
    state->snapshot.delivery_acknowledged_at = now;
    state->snapshot.updated_at = now;
    state->changed.notify_all();
  }
  SubagentCoordinatorJobSnapshot result;
  {
    std::lock_guard state_lock(state->mutex);
    result = public_snapshot_locked(*state);
  }
  {
    std::lock_guard lock(mutex_);
    prune_eligible_locked();
  }
  return result;
}

ava::core::Result<SubagentCoordinatorJobSnapshot> SubagentCoordinator::exhaust_delivery(std::string_view parent_session_id, std::string_view job_id,
                                                                                        std::string_view attempt_id)
{
  if (auto valid = validate_identifier("attempt_id", attempt_id, job_id); !valid)
    return std::unexpected(std::move(valid.error()));
  std::shared_ptr<JobState> state;
  {
    std::lock_guard lock(mutex_);
    state = find_owned_locked(parent_session_id, job_id);
  }
  if (!state)
    return std::unexpected(not_found(job_id));
  {
    std::lock_guard state_lock(state->mutex);
    if (state->delivery_exhausted)
    {
      if (!state->snapshot.delivery_attempt_history.empty() && state->snapshot.delivery_attempt_history.back().attempt_id == attempt_id)
        return public_snapshot_locked(*state);
      return std::unexpected(invalid_transition("subagent delivery exhaustion does not match the latest attempt", job_id));
    }
    if (state->snapshot.delivery != SubagentDeliveryState::Attempting || state->snapshot.delivery_attempt_history.empty() ||
        state->snapshot.delivery_attempt_history.back().attempt_id != attempt_id)
      return std::unexpected(invalid_transition("subagent delivery exhaustion does not match the latest attempt", job_id));
    state->delivery_exhausted = true;
    state->terminal_notification_pending = false;
    state->changed.notify_all();
  }
  SubagentCoordinatorJobSnapshot result;
  {
    std::lock_guard state_lock(state->mutex);
    result = public_snapshot_locked(*state);
  }
  {
    std::lock_guard lock(mutex_);
    prune_eligible_locked();
  }
  return result;
}

void SubagentCoordinator::shutdown()
{
  AVA_ASSERT_NO_SESSION_LOCK_HELD("calling SubagentCoordinator::shutdown");

  std::vector<std::string> jobs;
  {
    std::unique_lock lock(mutex_);
    if (shutdown_complete_)
      return;
    accepting_ = false;
    terminal_sink_ = nullptr;
    admission_changed_.wait(lock, [this] { return active_starts_ == 0; });
    for (auto const& [job_id, state] : jobs_)
    {
      std::lock_guard state_lock(state->mutex);
      // Invalidate source epoch so late live/freeze results cannot store after
      // shutdown. Published frames remain only while JobState is still held.
      ++state->source_epoch;
      if (state->source_epoch == 0)
        state->source_epoch = 1;
      state->inspection_source = nullptr;
      state->freeze_pending = false;
      if (state->steering_queue)
        state->steering_queue->close();
      if (!terminal(state->snapshot.execution))
      {
        state->snapshot.cancel_requested = true;
        if (!state->snapshot.cancel_requested_at)
          state->snapshot.cancel_requested_at = ava::session::now_timestamp();
        state->snapshot.updated_at = *state->snapshot.cancel_requested_at;
        jobs.push_back(job_id);
      }
      state->changed.notify_all();
    }
  }
  for (auto const& job_id : jobs)
    static_cast<void>(registry_.cancel(job_id));
  registry_.shutdown();
  std::unordered_map<std::string, std::shared_ptr<JobState>> released;
  {
    std::lock_guard lock(mutex_);
    shutdown_complete_ = true;
    for (auto const& [_, state] : jobs_)
    {
      std::lock_guard state_lock(state->mutex);
      ++state->source_epoch;
      if (state->source_epoch == 0)
        state->source_epoch = 1;
      state->inspection_source = nullptr;
      state->freeze_pending = false;
    }
    released.swap(jobs_);
  }
}

}  // namespace ava::agent
