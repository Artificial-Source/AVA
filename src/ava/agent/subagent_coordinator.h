#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/agent/background_job_registry.h"
#include "ava/agent/question.h"
#include "ava/agent/subagent_job.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission.h"
#include "ava/core/result.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ava::agent {

// Opaque path-free live inspection types. Complete definitions live in
// subagent_inspector.h so public coordinator fanout never pulls session types.
class SubagentLiveInspectionSource;
struct SubagentInspectorFrame;

// Error context exposed by failed start() calls. Process-local publication is
// atomic, so coordinator failures are always ProvenUnpublished. The uncertain
// value remains reserved for schema-version-1 API compatibility.
enum class SubagentPublicationCommitState
{
  ProvenUnpublished,
  PublicationUncertain,
};

[[nodiscard]] std::string_view to_string(SubagentPublicationCommitState value) noexcept;
[[nodiscard]] SubagentPublicationCommitState subagent_publication_commit_state(ava::core::Error const& error) noexcept;

struct SubagentCoordinatorJobSnapshot
{
  SubagentJobSnapshot job;
  bool timed_out = false;
  bool coordinator_latched = false;
  std::optional<std::string> coordinator_error = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

enum class SubagentWaitMode
{
  Terminal,
  TerminalOrPromotion,
};

// Shared only by one coordinator job and its child loop so steering can cross
// the worker boundary without retaining the coordinator or parent callbacks.
class SubagentSteeringQueue final
{
 public:
  SubagentSteeringQueue() = default;
  [[nodiscard]] static std::shared_ptr<SubagentSteeringQueue> create();
  [[nodiscard]] ava::core::Result<std::vector<std::string>> take();

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  [[nodiscard]] ava::core::VoidResult enqueue(std::string message);
  void close();

  std::mutex mutex_;
  std::deque<std::string> messages_;
  std::size_t queued_bytes_ = 0;
  bool closed_ = false;

  friend class SubagentCoordinator;
};

// Per-job owner-append callback for bounded start/terminal history. Bind the
// parent session's owner_append_route, never a run-scoped append sink.
using SubagentJobHistoryAppend = std::function<ava::core::VoidResult(ava::session::SessionEntry)>;

// Coordinator-owned launch request. Private launch presentation crosses the
// ownership boundary here and never enters BackgroundJobStartOptions/registry.
struct SubagentCoordinatorStartRequest
{
  std::string parent_session_id;
  SubagentJobMode mode = SubagentJobMode::Foreground;
  BackgroundJobStartOptions job;
  SubagentLaunchDisplay launch_display = {};
  std::shared_ptr<SubagentSteeringQueue> steering_queue = nullptr;
  SubagentJobHistoryAppend history_append = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Owns foreground-only frontend callbacks. Promotion reserves this gate before
// changing coordinator state, preventing a new callback from racing promotion.
class SubagentInteractionGate final : public std::enable_shared_from_this<SubagentInteractionGate>
{
 public:
  [[nodiscard]] static std::shared_ptr<SubagentInteractionGate> create(SubagentJobMode mode, ava::permissions::PermissionResolver permission_resolver,
                                                                       QuestionResolver question_resolver);
  [[nodiscard]] ava::permissions::PermissionResolver permission_resolver();
  [[nodiscard]] QuestionResolver question_resolver();
  [[nodiscard]] ava::core::VoidResult prepare_promotion();
  void commit_promotion();
  void abort_promotion();
  void finish();

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  SubagentInteractionGate(SubagentJobMode mode, ava::permissions::PermissionResolver permission_resolver, QuestionResolver question_resolver);

  std::mutex mutex_;
  SubagentJobMode mode_ = SubagentJobMode::Foreground;
  bool promotion_pending_ = false;
  std::size_t outstanding_ = 0;
  ava::permissions::PermissionResolver permission_resolver_ = nullptr;
  QuestionResolver question_resolver_ = nullptr;
};

struct SubagentCoordinatorOptions
{
  BackgroundJobRegistryOptions registry_options = {};
  // Deterministic test seam for process-local identity collision coverage.
  // Production leaves this empty and uses ava::core::make_id.
  std::function<std::string(std::string_view)> id_generator = nullptr;
  // Deterministic test-only seam: after inspect captures the live source and
  // source epoch, and before fingerprint/project work, invoke this hook so a
  // test can complete/freeze the job while the inspector is paused. Production
  // leaves this empty. Must not perform coordinator I/O under JobState locks.
  std::function<void()> inspect_after_source_capture_for_test = nullptr;
  // Deterministic test-only seam: after fingerprint + projection succeed and
  // immediately before the final publish lock, invoke this hook so a test can
  // interleave a concurrent inspect/publish while one inspector holds an older
  // projection. Production leaves this empty. Outside JobState locks.
  std::function<void()> inspect_before_publish_for_test = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Protocol-neutral notification emitted after terminal background state is
// process-locally published. The application sink must be nonblocking.
using SubagentTerminalSink = std::function<void(SubagentCoordinatorJobSnapshot const&)>;

class SubagentCoordinator;

// Move-only process-local barrier for a bounded set of parent session
// identities. It blocks new starts and is granted only when no start is being
// published and no starting/running job exists for those parents. Identities
// remain private and are never included in maintenance errors or snapshots.
class SubagentCoordinatorMaintenanceReservation
{
 public:
  SubagentCoordinatorMaintenanceReservation() = default;
  ~SubagentCoordinatorMaintenanceReservation();
  SubagentCoordinatorMaintenanceReservation(SubagentCoordinatorMaintenanceReservation&& other) noexcept;
  SubagentCoordinatorMaintenanceReservation& operator=(SubagentCoordinatorMaintenanceReservation&& other) noexcept;
  SubagentCoordinatorMaintenanceReservation(SubagentCoordinatorMaintenanceReservation const&) = delete;
  SubagentCoordinatorMaintenanceReservation& operator=(SubagentCoordinatorMaintenanceReservation const&) = delete;

  [[nodiscard]] bool active() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  SubagentCoordinatorMaintenanceReservation(std::shared_ptr<SubagentCoordinator> coordinator, std::vector<std::string> parent_session_ids,
                                            std::uint64_t generation);
  void release() noexcept;

  std::shared_ptr<SubagentCoordinator> coordinator_;
  std::vector<std::string> parent_session_ids_;
  std::uint64_t generation_ = 0;
  friend class SubagentCoordinator;
};

class SubagentCoordinator final : public std::enable_shared_from_this<SubagentCoordinator>
{
 public:
  [[nodiscard]] static ava::core::Result<std::shared_ptr<SubagentCoordinator>> create(SubagentCoordinatorOptions options = {});
  ~SubagentCoordinator();

  SubagentCoordinator(SubagentCoordinator const&) = delete;
  SubagentCoordinator& operator=(SubagentCoordinator const&) = delete;
  SubagentCoordinator(SubagentCoordinator&&) = delete;
  SubagentCoordinator& operator=(SubagentCoordinator&&) = delete;

  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> start(SubagentCoordinatorStartRequest request, BackgroundJobWorker worker,
                                                                        std::shared_ptr<SubagentInteractionGate> interaction_gate = nullptr,
                                                                        std::shared_ptr<SubagentLiveInspectionSource> inspection_source = nullptr);
  // Compatibility surface for callers with no private launch presentation.
  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> start(std::string parent_session_id, SubagentJobMode mode, BackgroundJobStartOptions options,
                                                                        BackgroundJobWorker worker,
                                                                        std::shared_ptr<SubagentInteractionGate> interaction_gate = nullptr,
                                                                        std::shared_ptr<SubagentLiveInspectionSource> inspection_source = nullptr);
  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> start_background(std::string parent_session_id, BackgroundJobStartOptions options,
                                                                                   BackgroundJobWorker worker,
                                                                                   std::shared_ptr<SubagentLiveInspectionSource> inspection_source = nullptr);
  // Nonblocking process-local authority barrier used by the application
  // workspace transaction. Parent identities are accepted as opaque keys and
  // are never surfaced by the resulting errors.
  [[nodiscard]] ava::core::Result<SubagentCoordinatorMaintenanceReservation> reserve_parent_maintenance(std::vector<std::string> parent_session_ids);
  [[nodiscard]] std::vector<SubagentCoordinatorJobSnapshot> list(std::string_view parent_session_id) const;
  [[nodiscard]] ava::core::Result<std::vector<SubagentCoordinatorJobSnapshot>> pending_deliveries(std::string_view parent_session_id);
  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> snapshot(std::string_view parent_session_id, std::string_view job_id);
  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> wait(std::string_view parent_session_id, std::string_view job_id,
                                                                       std::chrono::milliseconds timeout, SubagentWaitMode mode = SubagentWaitMode::Terminal);
  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> result(std::string_view parent_session_id, std::string_view job_id);
  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> cancel(std::string_view parent_session_id, std::string_view job_id);
  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> steer(std::string_view parent_session_id, std::string_view job_id, std::string message);
  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> promote(std::string_view parent_session_id, std::string_view job_id);
  // Owner-bound path-free live inspection. known_generation yields not_modified
  // only when it equals the current published content generation after a fresh
  // fingerprint check (or a stable terminal frame). Never returns raw session
  // path/load errors; refresh failures become path-free frame flags.
  [[nodiscard]] ava::core::Result<std::shared_ptr<SubagentInspectorFrame const>> inspect(std::string_view parent_session_id, std::string_view job_id,
                                                                                         std::optional<std::uint64_t> known_generation = std::nullopt);

  void set_terminal_sink(SubagentTerminalSink sink);
  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> record_delivery_attempt(std::string_view parent_session_id, std::string_view job_id,
                                                                                          std::string attempt_id, std::string prompt_fingerprint);
  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> acknowledge_delivery(std::string_view parent_session_id, std::string_view job_id,
                                                                                       std::string_view attempt_id, std::string committed_turn_id);
  // Settles retry exhaustion without changing the public schema or delivery
  // enum. The latest bounded result remains queryable until normal retention.
  [[nodiscard]] ava::core::Result<SubagentCoordinatorJobSnapshot> exhaust_delivery(std::string_view parent_session_id, std::string_view job_id,
                                                                                   std::string_view attempt_id);

  void shutdown();

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct JobState;
  class StartAdmission final
  {
   public:
    StartAdmission(SubagentCoordinator& coordinator, std::string const& parent_session_id) noexcept
        : coordinator_(coordinator), parent_session_id_(&parent_session_id)
    {
    }
    ~StartAdmission();
    StartAdmission(StartAdmission const&) = delete;
    StartAdmission& operator=(StartAdmission const&) = delete;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

   private:
    SubagentCoordinator& coordinator_;
    std::string const* parent_session_id_;
  };

  explicit SubagentCoordinator(SubagentCoordinatorOptions options);
  [[nodiscard]] std::shared_ptr<JobState> find_owned_locked(std::string_view parent_session_id, std::string_view job_id) const;
  [[nodiscard]] SubagentCoordinatorJobSnapshot public_snapshot_locked(JobState const& state, bool timed_out = false) const;
  [[nodiscard]] BackgroundJobCompletion complete(std::shared_ptr<JobState> const& state, BackgroundJobCompletion completion);
  void publish_terminal_notification(std::shared_ptr<JobState> const& state) noexcept;
  void prune_eligible_locked();
  [[nodiscard]] bool erase_oldest_eligible_locked();

  SubagentCoordinatorOptions options_;
  BackgroundJobRegistry registry_;
  // Protects map/admission/sink state. When both locks are required, acquire
  // this mutex before JobState::mutex; external components are called unlocked.
  mutable std::mutex mutex_;
  std::condition_variable admission_changed_;
  std::size_t active_starts_ = 0;
  std::unordered_map<std::string, std::size_t> active_starts_by_parent_;
  std::unordered_map<std::string, std::uint64_t> maintenance_parents_;
  std::uint64_t next_maintenance_generation_ = 1;
  bool accepting_ = true;
  bool shutdown_complete_ = false;
  std::size_t next_job_sequence_ = 1;
  std::unordered_map<std::string, std::shared_ptr<JobState>> jobs_;
  SubagentTerminalSink terminal_sink_ = nullptr;

  friend class SubagentCoordinatorMaintenanceReservation;
};

}  // namespace ava::agent
