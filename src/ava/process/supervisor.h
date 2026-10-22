#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/process/environment.h"
#include "ava/process/execution_capability.h"
#include "ava/process/owner.h"
#include "ava/process/types.h"
#include "ava/core/result.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ava::process {

namespace detail {
struct HandleState;
struct PipeEndpointState;
struct SupervisorState;
}  // namespace detail

namespace testing {
class SupervisorTestAccess;
}  // namespace testing

class Supervisor;

// A borrowed readiness capability. It keeps no endpoint or native resource
// alive and can be constructed only by PipeEndpoint::watch.
class PipeWatchV1
{
 public:
  PipeWatchV1(PipeWatchV1 const&) noexcept = default;
  PipeWatchV1& operator=(PipeWatchV1 const&) noexcept = default;
  PipeWatchV1(PipeWatchV1&&) noexcept = default;
  PipeWatchV1& operator=(PipeWatchV1&&) noexcept = default;
  ~PipeWatchV1() = default;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  PipeWatchV1(std::weak_ptr<detail::PipeEndpointState> endpoint, PipeInterestV1 interest, std::uint32_t token) noexcept;

  std::weak_ptr<detail::PipeEndpointState> endpoint_;
  PipeInterestV1 interest_ = PipeInterestV1::Readable;
  std::uint32_t token_ = 0;

  friend class PipeEndpoint;
  friend class Supervisor;
};

struct SpawnSpecV1
{
  std::string executable;
  std::vector<std::string> argv;
  ExactEnvironmentV1 environment;
  std::string cwd;
  // Independent optional descriptor authorities. When present, each private
  // logical spelling must exactly match its corresponding string above.
  std::optional<PreopenedExecutableV1> preopened_executable = std::nullopt;
  std::optional<AnchoredWorkingDirectoryV1> anchored_cwd = std::nullopt;
  StreamModeV1 stdin_mode = StreamModeV1::Discard;
  StreamModeV1 stdout_mode = StreamModeV1::Capture;
  StreamModeV1 stderr_mode = StreamModeV1::Capture;
  // Optional parent-only, content-free cancellation observation. The common
  // spawn path checks it without retaining it in process state.
  std::function<bool()> cancel_requested = nullptr;

  // This type contains executable, argv, exact-environment authority, and cwd
  // content. A default-constructed environment is an invalid pre-fork state.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct SecureAdoptionSpecV1
{
  ExactEnvironmentV1 environment;
  std::vector<std::string> argv;
  std::string cwd;
  // Secure adoption has no pathname-open cwd seam; this capability is required.
  AnchoredWorkingDirectoryV1 anchored_cwd{};
  BashContainmentHandshakeV1 bash_containment = BashContainmentHandshakeV1::None;

  // Exact environment, argv, and cwd content are intentionally excluded from
  // debug output. The containment selector is a closed role-bound policy value.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Native-handle-neutral ownership wrapper for one nonblocking parent pipe end.
class PipeEndpoint
{
 public:
  PipeEndpoint() noexcept;
  PipeEndpoint(PipeEndpoint const&) = delete;
  PipeEndpoint& operator=(PipeEndpoint const&) = delete;
  PipeEndpoint(PipeEndpoint&&) noexcept;
  PipeEndpoint& operator=(PipeEndpoint&&) noexcept;
  ~PipeEndpoint();

  [[nodiscard]] bool valid() const noexcept;
  void close() noexcept;
  [[nodiscard]] ava::core::Result<PipeIoResultV1> read(std::span<std::byte> destination);
  [[nodiscard]] ava::core::Result<PipeIoResultV1> write(std::span<std::byte const> source);
  [[nodiscard]] ava::core::Result<bool> wait_readable(ProcessDeadline deadline) const;
  [[nodiscard]] ava::core::Result<bool> wait_writable(ProcessDeadline deadline) const;
  [[nodiscard]] ava::core::Result<PipeWatchV1> watch(PipeInterestV1 interest, std::uint32_t token) const;

  // Watches are borrowed. An endpoint watched by wait_for_activity must stay
  // alive and must not be moved or closed concurrently with that call.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct Impl;
  explicit PipeEndpoint(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> implementation_;

  friend class Supervisor;
};

class Reservation
{
 public:
  Reservation() noexcept;
  Reservation(Reservation const&) = delete;
  Reservation& operator=(Reservation const&) = delete;
  Reservation(Reservation&&) noexcept;
  Reservation& operator=(Reservation&&) noexcept;
  ~Reservation();

  [[nodiscard]] bool valid() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  Reservation(std::shared_ptr<detail::SupervisorState> state, std::uint64_t record) noexcept;
  void abandon() noexcept;

  // Shared ownership keeps the reservation's release route valid until its
  // owning Supervisor has completed destruction.
  std::shared_ptr<detail::SupervisorState> state_;
  std::uint64_t record_ = 0;

  friend class Supervisor;
};

class ProcessHandle
{
 public:
  ProcessHandle() noexcept;
  ProcessHandle(ProcessHandle const&) = delete;
  ProcessHandle& operator=(ProcessHandle const&) = delete;
  ProcessHandle(ProcessHandle&&) noexcept;
  ProcessHandle& operator=(ProcessHandle&&) noexcept;
  ~ProcessHandle();

  [[nodiscard]] bool valid() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  explicit ProcessHandle(std::shared_ptr<detail::HandleState> state) noexcept;

  // The monitor and caller share only this bounded, PID-free final-value cell.
  std::shared_ptr<detail::HandleState> state_;

  friend class Supervisor;
};

struct SpawnResultV1
{
  ProcessHandle handle;
  std::optional<PipeEndpoint> standard_input;
  std::optional<PipeEndpoint> standard_output;
  std::optional<PipeEndpoint> standard_error;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class AdoptionGate
{
 public:
  AdoptionGate() noexcept;
  AdoptionGate(AdoptionGate const&) = delete;
  AdoptionGate& operator=(AdoptionGate const&) = delete;
  AdoptionGate(AdoptionGate&&) noexcept;
  AdoptionGate& operator=(AdoptionGate&&) noexcept;
  ~AdoptionGate();

  [[nodiscard]] bool valid() const noexcept;

  // In the child branch this call has already reset signals, established a
  // private group, acknowledged the parent, waited for registration, and
  // entered the preopened cwd. It must then execute only its reviewed
  // async-signal-safe child path, ending with exec or _exit rather than
  // returning to application code.
  [[nodiscard]] ava::core::Result<AdoptionForkBranchV1> fork_leader();

  // Adds the one optional AVA-owned Bash sentinel as an exact direct child.
  // Other secure roles fail closed before a sentinel fork. The sentinel joins
  // the gated leader group and remains blocked until adoption.
  [[nodiscard]] ava::core::VoidResult fork_sentinel();

  // These methods are available only on the child copy returned by
  // fork_leader. They use only argv, environment, and control state prepared
  // before fork and never return to application code after a failed launch or
  // exec attempt.
  [[noreturn]] void child_launch_failed(AdoptionChildFailureStageV1 stage, int errno_value) noexcept;
  void child_bash_containment_applied() noexcept;
  [[noreturn]] void child_exec_descriptor(int executable_descriptor, std::span<int const> retained_script_descriptors = {}) noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct Impl;
  explicit AdoptionGate(std::unique_ptr<Impl> implementation) noexcept;
  void abandon() noexcept;

  std::unique_ptr<Impl> implementation_;

  friend class Supervisor;
};

class Supervisor
{
 public:
  Supervisor();
  Supervisor(Supervisor const&) = delete;
  Supervisor& operator=(Supervisor const&) = delete;
  Supervisor(Supervisor&&) = delete;
  Supervisor& operator=(Supervisor&&) = delete;
  ~Supervisor() noexcept;

  [[nodiscard]] ava::core::Result<Reservation> reserve(OwnerPathV1 const& owner, ProcessRoleV1 role, LifecyclePolicyV1 policy = {});
  [[nodiscard]] ava::core::Result<SpawnResultV1> spawn(Reservation&& reservation, SpawnSpecV1 specification);

  [[nodiscard]] ava::core::Result<AdoptionGate> begin_secure_adoption(Reservation&& reservation, SecureAdoptionSpecV1 specification);
  [[nodiscard]] ava::core::Result<ProcessHandle> adopt(AdoptionGate&& gate);

  void stop_accepting() noexcept;
  [[nodiscard]] ava::core::Result<StopResultV1> request_stop(ProcessHandle const& handle, TerminationReasonV1 reason, ProcessDeadline deadline);
  [[nodiscard]] ava::core::Result<StopResultV1> request_stop(OwnerPathV1 const& owner_prefix, TerminationReasonV1 reason, ProcessDeadline deadline);
  // Read-only observation of the first committed reason for an owned handle,
  // including before settlement. No reason means no committed stop/termination;
  // NaturalExit is a completion, not an interruption. Does not observe native
  // process state, advance deadlines, signal, wait, or reap. Invalid, foreign,
  // or no-longer-retained handles fail explicitly.
  [[nodiscard]] ava::core::Result<std::optional<TerminationReasonV1>> committed_stop_reason(ProcessHandle const& handle) const;
  [[nodiscard]] ava::core::Result<ExitStatusV1> wait(ProcessHandle const& handle, ProcessDeadline deadline) const;
  [[nodiscard]] ava::core::Result<std::optional<ExitStatusV1>> try_wait(ProcessHandle const& handle) const;
  [[nodiscard]] ava::core::Result<ProcessActivityV1> wait_for_activity(ProcessHandle const& handle, std::span<PipeWatchV1 const> watches,
                                                                       ProcessDeadline deadline) const;
  [[nodiscard]] ava::core::VoidResult account_output(ProcessHandle const& handle, StreamKindV1 stream, std::uint64_t bytes, bool truncated);

  [[nodiscard]] ProcessSnapshotV1 snapshot() const;
  [[nodiscard]] ShutdownResultV1 shutdown(ProcessDeadline deadline) noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  [[nodiscard]] ava::core::Result<std::uint64_t> consume_reservation(Reservation& reservation);

  struct Impl;
  std::unique_ptr<Impl> implementation_;

  friend class testing::SupervisorTestAccess;
};

}  // namespace ava::process
