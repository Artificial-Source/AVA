#pragma once

#include "ava/app/runtime/RunOptions.h"
#include "ava/app/runtime_credentials.h"
#include "ava/app/session_run_controller.h"
#include "ava/agent/subagent_coordinator.h"
#include "ava/session/session_store.h"
#include "ava/core/result.h"
#include "ava/core/thread.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace ava::app {
namespace runtime {
class Session;
} // namespace runtime

struct RetainedSessionOptions
{
  bool exact_session_id = false;
  std::optional<ava::session::SessionReadLimits> read_limits;
  std::optional<std::filesystem::path> expected_original_cwd;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct SubagentDeliveryManagerOptions
{
  std::shared_ptr<ava::agent::SubagentCoordinator> coordinator;
  RuntimeProviderRunBundleFactory provider_bundle_factory = nullptr;
  std::size_t max_queued_deliveries = 64;
  std::size_t max_retained_parents = 64;
  std::size_t max_delivery_attempts = 3;
  std::chrono::milliseconds delivery_deadline = std::chrono::seconds(30);
  std::chrono::milliseconds admission_retry_interval = std::chrono::milliseconds(10);
  // Optional application-owned hook invoked without manager, session, or
  // controller locks after an idle controller is observed but before delivery
  // attempt recording or admission. It must unblock when stop is requested.
  std::function<void(std::stop_token)> admission_preflight = nullptr;
  // Deterministic test-only construction/revocation seams. Production leaves
  // these empty. They run without manager/session/controller locks while the
  // corresponding workspace reservation remains held. Hooks must not issue
  // manager I/O.
  std::function<void()> construction_after_trusted_prompt_resolution_for_test = nullptr;
  std::function<ava::core::VoidResult()> revocation_before_publication_for_test = nullptr;
  std::function<void()> revocation_after_retirement_for_test = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Application-owned automatic summary delivery. It retains only detached,
// callback-free parent runtime capsules and never references frontend/RPC
// state. The coordinator remains protocol-neutral and emits process-local
// terminal notifications into this manager's bounded advisory queue. Workspace
// serialization covers only sessions sharing this manager; separate managers
// and processes remain independent.
class SubagentDeliveryManager final : public std::enable_shared_from_this<SubagentDeliveryManager>
{
 public:
  // Process-local, manager-scoped trust writer serialization. This deliberately
  // makes no singleton or cross-process coordination claim.
  class TrustMutationReservation
  {
   public:
    TrustMutationReservation() = default;
    ~TrustMutationReservation();
    TrustMutationReservation(TrustMutationReservation&& other) noexcept;
    TrustMutationReservation& operator=(TrustMutationReservation&& other) noexcept;
    TrustMutationReservation(TrustMutationReservation const&) = delete;
    TrustMutationReservation& operator=(TrustMutationReservation const&) = delete;
    [[nodiscard]] bool active() const noexcept;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

   private:
    struct Impl;
    explicit TrustMutationReservation(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class SubagentDeliveryManager;
  };

  // Short publication barrier used by Session::replace_with. It prevents a
  // navigation from crossing an authority retirement/publication boundary.
  class WorkspaceNavigationReservation
  {
   public:
    WorkspaceNavigationReservation() = default;
    ~WorkspaceNavigationReservation();
    WorkspaceNavigationReservation(WorkspaceNavigationReservation&& other) noexcept;
    WorkspaceNavigationReservation& operator=(WorkspaceNavigationReservation&& other) noexcept;
    WorkspaceNavigationReservation(WorkspaceNavigationReservation const&) = delete;
    WorkspaceNavigationReservation& operator=(WorkspaceNavigationReservation const&) = delete;
    [[nodiscard]] bool active() const noexcept;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

   private:
    struct Impl;
    explicit WorkspaceNavigationReservation(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class SubagentDeliveryManager;
  };

  // All-controller workspace barrier for an effective untrusted transition.
  // Acquisition is nonblocking and all-or-nothing. Once persistence is marked
  // committed, destruction fail-closes by retiring old/fresh controllers and
  // purging stale delivery state unless commit_after_publication completed.
  class WorkspaceMaintenanceReservation
  {
   public:
    WorkspaceMaintenanceReservation() = default;
    ~WorkspaceMaintenanceReservation();
    WorkspaceMaintenanceReservation(WorkspaceMaintenanceReservation&& other) noexcept;
    WorkspaceMaintenanceReservation& operator=(WorkspaceMaintenanceReservation&& other) noexcept;
    WorkspaceMaintenanceReservation(WorkspaceMaintenanceReservation const&) = delete;
    WorkspaceMaintenanceReservation& operator=(WorkspaceMaintenanceReservation const&) = delete;

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] ava::core::Result<std::shared_ptr<SessionRunController>> prepare_fresh_controller(
        std::shared_ptr<ava::session::SessionAppendTarget> append_target);
    void mark_persistence_committed() noexcept;
    [[nodiscard]] ava::core::VoidResult retire_registered_controllers();
    [[nodiscard]] ava::core::VoidResult run_before_publication_test_hook();
    // Call only after the current Session atomically publishes the prepared
    // fresh controller and fail-closed trust/prompt/tool state.
    [[nodiscard]] ava::core::VoidResult commit_after_publication(std::string_view current_session_id);
    // Idempotent post-persistence failure path.
    void fail_closed() noexcept;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

   private:
    struct Impl;
    explicit WorkspaceMaintenanceReservation(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class SubagentDeliveryManager;
  };

  [[nodiscard]] static ava::core::Result<std::shared_ptr<SubagentDeliveryManager>> create(SubagentDeliveryManagerOptions options);
  ~SubagentDeliveryManager();

  SubagentDeliveryManager(SubagentDeliveryManager const&) = delete;
  SubagentDeliveryManager& operator=(SubagentDeliveryManager const&) = delete;

  [[nodiscard]] std::shared_ptr<ava::agent::SubagentCoordinator> const& coordinator() const noexcept;

  // Register one manager-associated runtime controller under a normalized,
  // path-private workspace key. Entries are weak, deduplicated, and pruned.
  [[nodiscard]] ava::core::VoidResult register_workspace_controller(std::filesystem::path const& workspace_identity, std::string_view session_id,
                                                                    std::shared_ptr<SessionRunController> const& controller);
  [[nodiscard]] ava::core::Result<TrustMutationReservation> reserve_trust_mutation(std::filesystem::path const& workspace_identity);
  [[nodiscard]] ava::core::Result<WorkspaceMaintenanceReservation> reserve_workspace_maintenance(
      TrustMutationReservation const& trust_reservation, std::filesystem::path const& workspace_identity, std::string_view current_session_id,
      std::shared_ptr<SessionRunController> const& current_controller);
  [[nodiscard]] ava::core::Result<WorkspaceNavigationReservation> reserve_workspace_navigation(std::filesystem::path const& workspace_identity);
  [[nodiscard]] ava::core::VoidResult run_construction_after_trusted_prompt_resolution_test_hook();

  // Refreshes the exact parent capsule before each ordinary user prompt.
  // Callback-bearing fields are intentionally not copied.
  using CapsuleGeneration = std::uint64_t;

  [[nodiscard]] ava::core::Result<CapsuleGeneration> refresh_parent(runtime::session_ts const& unlocked_session, runtime::RunOptions const& options);
  // Refresh configuration-only runtime state from unlocked_session while preserving the current detached callback-free policy snapshot.
  //
  // The wrapper must be unlocked. Missing capsules are a no-op; failures duplicate session authority or publish the replacement capsule. The source
  // read lock is released before replacing the capsule, because replacement may destroy a retained Session.
  [[nodiscard]] ava::core::VoidResult refresh_parent_configuration(runtime::session_ts const& unlocked_session);
  void release_parent_if_unused(std::string_view parent_session_id, CapsuleGeneration generation);
  // Marks an explicit runtime attachment, preventing stale detach state from
  // releasing this visible parent's retained capsule.
  void attach_parent(std::string_view parent_session_id);
  // Called only after application navigation detaches the visible parent.
  // Retains any capsule still needed for a live job or delivery.
  void release_detached_parent(std::string_view parent_session_id);

  // Find a retained session matching session_id and workspace_identity, reporting benign absence through found.
  //
  // On success found is true and the result owns the detached session. When no retained session exists, found is false and callers must ignore the
  // sentinel error result. All genuine lookup or attachment errors set found to true and remain available through the result.
  // Validate requested read limits and persisted cwd against the exact retained lease before attachment; validation failure leaves the capsule unchanged.
  [[nodiscard]] ava::core::Result<runtime::session_ts> retained_session(std::string_view session_id, std::filesystem::path const& workspace_identity,
                                                                        bool& found, RetainedSessionOptions const& options = {});

  void shutdown() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct ParentCapsule;
  struct WorkspaceRecord;
  explicit SubagentDeliveryManager(SubagentDeliveryManagerOptions options);
  void start();
  void enqueue(ava::agent::SubagentCoordinatorJobSnapshot const& snapshot) noexcept;
  void worker_loop(std::stop_token stop_token);
  void deliver(ava::agent::SubagentCoordinatorJobSnapshot snapshot, std::shared_ptr<ParentCapsule> const& capsule, std::stop_token stop_token);
  [[nodiscard]] bool parent_needed(std::string_view session_id) const;
  void finish_in_flight_delivery(std::string const& workspace_key) noexcept;

  SubagentDeliveryManagerOptions options_;
  std::shared_ptr<ava::agent::SubagentCoordinator> coordinator_;
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::unordered_map<std::string, std::shared_ptr<ParentCapsule>> parents_;
  std::unordered_set<std::string> detached_parents_;
  std::deque<ava::agent::SubagentCoordinatorJobSnapshot> queue_;
  // All workspace keys and parent mappings are private normalized path data;
  // they are never exposed in snapshots, errors, or debug printing.
  std::unordered_map<std::string, std::shared_ptr<WorkspaceRecord>> workspaces_;
  std::unordered_map<std::string, std::string> session_workspaces_;
  std::unordered_set<std::string> unavailable_deliveries_;
  bool trust_mutation_active_ = false;
  std::uint64_t next_workspace_generation_ = 1;
  bool accepting_ = true;
  CapsuleGeneration next_generation_ = 1;
  ava::core::JoinThread worker_;
};

}  // namespace ava::app
