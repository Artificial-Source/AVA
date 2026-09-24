#pragma once

#include "ava/process/environment_internal.h"
#include "ava/process/execution_capability_internal.h"
#include "ava/process/supervisor.h"
#include "ava/core/error.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <signal.h>
#include <sys/types.h>
#endif

namespace ava::process::detail {

using Clock = std::chrono::steady_clock;
using AfterForkBeforeReleaseHook = std::function<void()>;

inline constexpr auto kFallbackInitialInterval = std::chrono::milliseconds(10);
inline constexpr auto kFallbackMaximumInterval = std::chrono::seconds(1);
inline constexpr auto kWaitedFallbackMaximumInterval = std::chrono::milliseconds(50);
inline constexpr auto kShortStopProbeInterval = std::chrono::milliseconds(10);
inline constexpr auto kPostKillObservation = std::chrono::milliseconds(20);
inline constexpr auto kDefaultCleanupBudget = std::chrono::seconds(2);
inline constexpr auto kMaximumStartupTimeout = std::chrono::seconds(30);
inline constexpr auto kMaximumBrowserExecutionWindow = std::chrono::seconds(10);
inline constexpr std::size_t kMaximumLaunchArgumentCount = 256;
inline constexpr std::size_t kMaximumPreparedLaunchBytes = 1024 * 1024;

[[nodiscard]] ava::core::Error process_error(ava::core::ErrorCategory category, std::string message);
[[nodiscard]] ava::core::Error unsupported_error();
[[nodiscard]] ava::core::Error invalid_error(std::string message);
[[nodiscard]] ava::core::Error io_error(std::string message, int error_number);
[[nodiscard]] bool requestable_reason(TerminationReasonV1 reason) noexcept;
[[nodiscard]] std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) noexcept;
[[nodiscard]] std::uint64_t elapsed_milliseconds(Clock::time_point begin, Clock::time_point end) noexcept;

struct SupervisorState;

#if !defined(_WIN32)

class UniqueFd final
{
 public:
  explicit UniqueFd(int descriptor = -1) noexcept : descriptor_(descriptor) { }
  UniqueFd(UniqueFd const&) = delete;
  UniqueFd& operator=(UniqueFd const&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : descriptor_(other.release()) { }
  UniqueFd& operator=(UniqueFd&& other) noexcept
  {
    if (this != &other)
      reset(other.release());
    return *this;
  }
  ~UniqueFd() { reset(); }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] int release() noexcept { return std::exchange(descriptor_, -1); }
  void reset(int descriptor = -1) noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  int descriptor_ = -1;
};

struct Pipe
{
  UniqueFd read_end;
  UniqueFd write_end;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

enum class MonitorBackendMode
{
  Automatic,
  ForceFallbackUnavailable,
  ForceFallbackDenied,
};

enum class PidfdFailureClass
{
  None,
  Unavailable,
  Denied,
  Resource,
  Other,
};

struct MonitorTelemetry
{
  std::atomic<std::uint64_t> wake_attempts{0};
  std::atomic<std::uint64_t> wake_drains{0};
  std::atomic<std::uint64_t> wake_coalesces{0};
  std::atomic<std::uint64_t> wake_failures{0};
  std::atomic<std::uint64_t> poll_calls{0};
  std::atomic<std::uint64_t> poll_timeouts{0};
  std::atomic<std::uint64_t> poll_interruptions{0};
  std::atomic<std::uint64_t> poll_peak_entries{0};
  std::atomic<std::uint64_t> pidfd_attempts{0};
  std::atomic<std::uint64_t> pidfd_successes{0};
  std::atomic<std::uint64_t> pidfd_unavailable_failures{0};
  std::atomic<std::uint64_t> pidfd_denied_failures{0};
  std::atomic<std::uint64_t> pidfd_resource_failures{0};
  std::atomic<std::uint64_t> pidfd_other_failures{0};
  std::atomic<std::uint64_t> exact_probes{0};
  std::atomic<std::uint64_t> fallback_probes{0};
  std::atomic<std::uint64_t> short_probes{0};
  std::array<std::atomic<std::uint64_t>, 8> fallback_delay_buckets{};
  std::atomic<std::uint64_t> group_observations{0};
  std::atomic<std::uint64_t> quiet_group_proofs{0};
  std::atomic<std::uint64_t> current_watches{0};
  std::atomic<std::uint64_t> peak_watches{0};
  std::atomic<std::uint64_t> stale_events{0};
  std::atomic<std::uint64_t> pollnval_events{0};
  std::atomic<std::uint64_t> cloexec_checks{0};
  std::atomic<std::uint64_t> cloexec_failures{0};
  std::atomic<std::uint64_t> nonblocking_checks{0};
  std::atomic<std::uint64_t> nonblocking_failures{0};
  std::atomic<std::uint64_t> current_wake_descriptors{0};
  std::atomic<std::uint64_t> peak_wake_descriptors{0};
  std::atomic<std::uint64_t> current_monitor_threads{0};
  std::atomic<std::uint64_t> peak_monitor_threads{0};
  std::atomic<std::uint64_t> monitor_cycles{0};

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PidfdWatch
{
  PidfdWatch(UniqueFd descriptor_value, std::shared_ptr<MonitorTelemetry> telemetry_value) noexcept;
  ~PidfdWatch();

  UniqueFd descriptor;
  std::shared_ptr<MonitorTelemetry> telemetry;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct MonitorWake
{
  MonitorWake(UniqueFd read_descriptor, UniqueFd write_descriptor, bool uses_eventfd, std::shared_ptr<MonitorTelemetry> telemetry_value) noexcept;
  ~MonitorWake();

  [[nodiscard]] int poll_descriptor() const noexcept { return read_end.get(); }
  [[nodiscard]] int signal_descriptor() const noexcept { return eventfd ? read_end.get() : write_end.get(); }
  [[nodiscard]] std::size_t descriptor_count() const noexcept { return eventfd ? 1U : 2U; }

  UniqueFd read_end;
  UniqueFd write_end;
  bool eventfd = false;
  std::shared_ptr<MonitorTelemetry> telemetry;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PidfdOpenResult
{
  std::shared_ptr<PidfdWatch> watch;
  PidfdFailureClass failure = PidfdFailureClass::None;
  bool cache_runtime_unavailable = false;
  bool prompt_exact_probe = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct MemberMonitorState
{
  std::shared_ptr<PidfdWatch> watch;
  std::uint64_t generation = 0;
  std::chrono::milliseconds fallback_interval{kFallbackInitialInterval};
  Clock::time_point next_exact_probe{};
  std::uint64_t probe_count = 0;
  std::uint64_t reset_count = 0;
  bool fallback_enabled = false;
  bool readiness_due = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

#endif

struct PipeEndpointState
{
#if !defined(_WIN32)
  explicit PipeEndpointState(int value, bool can_read, bool can_write) noexcept : descriptor(value), readable(can_read), writable(can_write) { }
  UniqueFd descriptor;
#else
  explicit PipeEndpointState(bool can_read, bool can_write) noexcept : readable(can_read), writable(can_write) { }
#endif
  mutable std::mutex mutex;
  bool readable = false;
  bool writable = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct HandleState
{
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::optional<ExitStatusV1> final_status;
  std::weak_ptr<SupervisorState> supervisor;
  std::uint64_t record = 0;
#if !defined(_WIN32)
  std::optional<Pipe> completion_channel;
  bool completion_signaled = false;
#endif

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct Record
{
  Record(std::uint64_t identity, OwnerPathV1 owner_path, std::string key, std::uint64_t aliased_owner, ProcessRoleV1 process_role,
         LifecyclePolicyV1 lifecycle_policy);

  std::uint64_t id = 0;
  OwnerPathV1 owner;
  std::string owner_key;
  std::uint64_t owner_alias = 0;
  ProcessRoleV1 role = ProcessRoleV1::Curl;
  LifecyclePolicyV1 policy;
  ProcessStateV1 state = ProcessStateV1::Reserved;
  std::optional<TerminationReasonV1> reason;
  CleanupStateV1 cleanup = CleanupStateV1::NotRequired;
  ExitKindV1 exit_kind = ExitKindV1::None;
  Clock::time_point created;
  std::optional<Clock::time_point> finished;
  // Fixed at reservation; execution expiry may only clip cleanup to this cap.
  std::optional<ProcessDeadline> execution_cleanup_horizon;
  std::optional<ProcessDeadline> stop_deadline;
  std::optional<Clock::time_point> kill_due;
  std::optional<Clock::time_point> group_observation_due;
  std::optional<Clock::time_point> short_stop_probe_due;
  std::shared_ptr<HandleState> handle;
  std::uint64_t stdout_bytes = 0;
  std::uint64_t stderr_bytes = 0;
  std::uint32_t settlement_count = 0;
  std::size_t active_waiters = 0;
  int exit_code = 0;
  int signal_number = 0;
  bool stdout_truncated = false;
  bool stderr_truncated = false;
  bool has_exit_code = false;
  bool has_signal_number = false;
  bool registered = false;
  bool group_verified = false;
  bool execution_deadline_processed = false;
  bool release_committed = false;
  bool startup_handshake_complete = false;
  bool included_in_shutdown = false;
  bool term_sent = false;
  bool kill_sent = false;
  bool cleanup_failed = false;
  bool pre_kill_group_observation_started = false;
  bool post_kill_group_observation_started = false;
  std::uint8_t quiet_group_observations = 0;
#if !defined(_WIN32)
  pid_t leader = -1;
  pid_t sentinel = -1;
  MemberMonitorState leader_monitor;
  MemberMonitorState sentinel_monitor;
  bool leader_observed = false;
  bool sentinel_observed = false;
  bool leader_reaped = false;
  bool sentinel_reaped = false;
#endif

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct OwnerAliasEntry
{
  std::uint64_t alias = 0;
  std::size_t references = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct SupervisorState
{
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::map<std::uint64_t, std::unique_ptr<Record>> records;
  std::deque<std::uint64_t> terminal_fifo;
  std::map<std::string, OwnerAliasEntry> owner_aliases;
  std::shared_ptr<AfterForkBeforeReleaseHook> after_fork_before_release_for_test;
  std::shared_ptr<AfterForkBeforeReleaseHook> after_gate_release_for_test;
  std::shared_ptr<AfterForkBeforeReleaseHook> after_completion_channel_create_for_test;
  bool fail_next_common_child_working_directory_for_test = false;
  std::uint64_t next_record = 1;
  std::uint64_t next_owner_alias = 1;
  std::size_t live_records = 0;
  std::size_t settled_records = 0;
  bool accepting = true;
  bool shutting_down = false;
  bool monitor_started = false;
  bool stop_monitor = false;
  bool monitor_joined = false;
#if !defined(_WIN32)
  MonitorBackendMode monitor_backend_mode = MonitorBackendMode::Automatic;
  bool pidfd_runtime_unavailable = false;
  std::uint64_t next_monitor_generation = 1;
  std::uint64_t test_control_generation = 0;
  std::shared_ptr<MonitorTelemetry> monitor_telemetry = std::make_shared<MonitorTelemetry>();
  std::shared_ptr<MonitorWake> monitor_wake;
  std::shared_ptr<AfterForkBeforeReleaseHook> after_poll_snapshot_for_test;
  std::atomic<std::uint64_t> poll_interruptions_for_test{0};
#endif

  // Process lock order is strict:
  // 1. shutdown mutex -> state, with state released before monitor join;
  // 2. monitor lifecycle mutex -> state, never state -> lifecycle;
  // 3. state -> handle only for final-value publication;
  // 4. endpoint mutexes in stable pointer order, never endpoint -> state;
  // 5. poll, wake I/O, and test hooks run without state, handle, or endpoint locks.
  std::mutex monitor_lifecycle_mutex;
  std::thread monitor;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class ActiveWaiterRegistration final
{
 public:
  ActiveWaiterRegistration() noexcept = default;
  ActiveWaiterRegistration(ActiveWaiterRegistration const&) = delete;
  ActiveWaiterRegistration& operator=(ActiveWaiterRegistration const&) = delete;
  ActiveWaiterRegistration(ActiveWaiterRegistration&& other) noexcept;
  ActiveWaiterRegistration& operator=(ActiveWaiterRegistration&& other) noexcept;
  ~ActiveWaiterRegistration();

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  ActiveWaiterRegistration(std::shared_ptr<SupervisorState> state, std::uint64_t record) noexcept;
  void release() noexcept;

  std::shared_ptr<SupervisorState> state_;
  std::uint64_t record_ = 0;

  friend ava::core::Result<ActiveWaiterRegistration> register_active_waiter(std::shared_ptr<SupervisorState> const& state, std::uint64_t record);
};

[[nodiscard]] ava::core::Result<ActiveWaiterRegistration> register_active_waiter(std::shared_ptr<SupervisorState> const& state, std::uint64_t record);
void notify_monitor_state(std::shared_ptr<SupervisorState> const& state) noexcept;
void release_owner_alias_locked(SupervisorState& state, Record const& record);
void prune_terminal_locked(SupervisorState& state);
void await_internal_settlement(std::shared_ptr<HandleState> const& handle, ProcessDeadline deadline) noexcept;
void signal_completion_locked(HandleState& handle) noexcept;
void publish_final_locked(Record& record, ExitStatusV1 status);
void finalize_locked(SupervisorState& state, Record& record, CleanupStateV1 cleanup);
[[nodiscard]] bool commit_reason_locked(Record& record, TerminationReasonV1 reason) noexcept;
[[nodiscard]] bool set_earlier_stop_deadline_locked(Record& record, ProcessDeadline deadline) noexcept;
// The caller holds state.mutex. A due trigger is processed once and clips an
// existing cleanup deadline only to the reservation-time horizon.
[[nodiscard]] bool commit_due_execution_deadline_locked(Record& record, Clock::time_point now) noexcept;
void abandon_reservation(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity) noexcept;
[[nodiscard]] std::optional<ProcessRoleV1> record_role(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity) noexcept;
TerminationReasonV1 finish_unregistered(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity, TerminationReasonV1 fallback,
                                        CleanupStateV1 cleanup = CleanupStateV1::NotRequired) noexcept;
[[nodiscard]] ProcessDeadline startup_deadline_for_record(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity) noexcept;
[[nodiscard]] ava::core::VoidResult invoke_after_fork_before_release_hook(std::shared_ptr<SupervisorState> const& state);
[[nodiscard]] ava::core::VoidResult invoke_after_gate_release_hook(std::shared_ptr<SupervisorState> const& state);

struct PreForkDecision
{
  bool launchable = false;
  TerminationReasonV1 reason = TerminationReasonV1::LaunchFailed;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// The caller holds state.mutex. Every launch check first linearizes a deadline
// already due before inspecting shutdown or startup state.
[[nodiscard]] PreForkDecision check_pre_fork_launch_locked(SupervisorState& state, std::uint64_t identity, ProcessDeadline startup_deadline) noexcept;

struct GateReleaseDecision
{
  bool committed = false;
  TerminationReasonV1 reason = TerminationReasonV1::LaunchFailed;
  ProcessDeadline cleanup_deadline{};

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// The caller holds state.mutex. Committing Running is the launch linearization
// point; physical gate writes happen afterward without the supervisor lock.
[[nodiscard]] GateReleaseDecision commit_gate_release_locked(SupervisorState& state, std::uint64_t identity, ProcessDeadline startup_deadline) noexcept;
[[nodiscard]] ava::core::Error canceled_launch_error(std::string operation, TerminationReasonV1 reason);
[[nodiscard]] ava::core::Error startup_stopped_error(std::string operation, TerminationReasonV1 reason);
[[nodiscard]] GateReleaseDecision fail_registered_launch(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity,
                                                         TerminationReasonV1 fallback) noexcept;
[[nodiscard]] ProcessDeadline provisional_cleanup_deadline(std::shared_ptr<SupervisorState> const& state, std::uint64_t identity, TerminationReasonV1 fallback,
                                                           ProcessDeadline proposed_deadline) noexcept;

#if !defined(_WIN32)

[[nodiscard]] ava::core::Result<int> move_above_standard_descriptors(int descriptor);
[[nodiscard]] ava::core::Result<Pipe> make_cloexec_pipe();
[[nodiscard]] ava::core::VoidResult set_nonblocking(int descriptor);
[[nodiscard]] ava::core::Result<bool> wait_descriptor(int descriptor, short events, ProcessDeadline deadline);
// Write all `size` bytes from `data` to `descriptor`, retrying interrupted writes.
//
// Returns false when a write fails or makes no progress. AVA's process-wide signal policy keeps SIGPIPE ignored and blocked,
// so a closed reader is reported as EPIPE instead of terminating the process.
[[nodiscard]] bool write_all_to_descriptor(int descriptor, void const* data, std::size_t size) noexcept;
[[nodiscard]] pid_t waitpid_retry(pid_t process, int* status, int options) noexcept;
[[nodiscard]] int waitid_retry(pid_t process, siginfo_t* information, int options) noexcept;
[[nodiscard]] bool exact_provisional_cleanup(pid_t leader, pid_t sentinel, ProcessDeadline deadline) noexcept;
[[nodiscard]] ssize_t child_read_retry(int descriptor, void* data, std::size_t size) noexcept;
[[nodiscard]] bool child_write_all(int descriptor, void const* data, std::size_t size) noexcept;
void close_nonstandard_descriptors(int preserved, int maximum) noexcept;
void close_nonstandard_descriptors_except(std::span<int const> preserved, int maximum) noexcept;
[[nodiscard]] int descriptor_limit() noexcept;

void observe_status(Record& record, siginfo_t const& information) noexcept;
[[nodiscard]] bool observe_member(pid_t process, bool& observed, Record& record, bool leader, bool detect_stops = true) noexcept;
[[nodiscard]] bool reap_member(pid_t process, bool observed, bool& reaped, Record& record) noexcept;
[[nodiscard]] bool signal_verified_group(Record& record, int signal_number) noexcept;
[[nodiscard]] std::optional<bool> verified_group_has_live_member(pid_t group) noexcept;
[[nodiscard]] ava::core::Result<std::shared_ptr<MonitorWake>> make_monitor_wake(std::shared_ptr<MonitorTelemetry> const& telemetry);
void signal_monitor_wake(std::shared_ptr<MonitorWake> const& wake) noexcept;
void drain_monitor_wake(std::shared_ptr<MonitorWake> const& wake) noexcept;
[[nodiscard]] PidfdOpenResult open_pidfd_watch(pid_t process, MonitorBackendMode mode, bool runtime_unavailable,
                                               std::shared_ptr<MonitorTelemetry> const& telemetry) noexcept;
void register_record_members_locked(SupervisorState& state, Record& record, Clock::time_point now) noexcept;
void reset_record_probe_schedule_locked(Record& record, Clock::time_point now) noexcept;
void detach_record_watches_locked(Record& record) noexcept;
void begin_stop_locked(Record& record, Clock::time_point now);
void monitor_main(std::shared_ptr<SupervisorState> state) noexcept;
[[nodiscard]] ava::core::VoidResult ensure_monitor_started(std::shared_ptr<SupervisorState> const& state);
void join_monitor(std::shared_ptr<SupervisorState> const& state) noexcept;

#endif

}  // namespace ava::process::detail

namespace ava::process {

struct PipeEndpoint::Impl
{
#if !defined(_WIN32)
  explicit Impl(int value, bool can_read, bool can_write) : state(std::make_shared<detail::PipeEndpointState>(value, can_read, can_write)) { }
#else
  explicit Impl(bool can_read, bool can_write) : state(std::make_shared<detail::PipeEndpointState>(can_read, can_write)) { }
#endif
  std::shared_ptr<detail::PipeEndpointState> state;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct AdoptionGate::Impl
{
  std::shared_ptr<detail::SupervisorState> state;
  std::shared_ptr<detail::HandleState> handle;
  std::uint64_t record = 0;
  ProcessRoleV1 role = ProcessRoleV1::Curl;
  ExactEnvironmentV1 environment;
  std::string cwd;
  AnchoredWorkingDirectoryV1 anchored_cwd;
  std::vector<std::string> argv_storage;
  std::vector<char*> argv_pointers;
  std::vector<std::string> environment_storage;
  std::vector<char*> environment_pointers;
  BashContainmentHandshakeV1 bash_containment = BashContainmentHandshakeV1::None;
  ProcessDeadline startup_deadline{};
  bool child_branch = false;
  bool child_api_ready = false;
  bool cwd_applied = false;
  bool containment_applied = false;
  bool registered = false;
#if !defined(_WIN32)
  detail::Pipe launch_status;
  detail::Pipe leader_control;
  detail::Pipe containment_control;
  std::optional<detail::Pipe> sentinel_status;
  std::optional<detail::Pipe> sentinel_control;
  int maximum_descriptor = 4096;
  pid_t leader = -1;
  pid_t sentinel = -1;
#endif

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct Supervisor::Impl
{
  Impl() : state(std::make_shared<detail::SupervisorState>()) { }

  // Supervisor methods, capabilities, and the one monitor share the state so
  // an RAII capability can always release itself during bounded destruction.
  std::shared_ptr<detail::SupervisorState> state;
  std::mutex shutdown_mutex;
  bool shutdown_called = false;
  ShutdownResultV1 shutdown_result;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::process
