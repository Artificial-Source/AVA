#include "sys.h"
#include "ava/process/launch_protocol_posix.h"
#include "ava/process/supervisor_internal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace ava::process {
namespace {

using detail::Clock;
using namespace std::chrono_literals;

bool contains_nul(std::string const& value) noexcept
{
  return value.find('\0') != std::string::npos;
}

#if !defined(_WIN32)

bool prepared_argv_valid(std::vector<std::string> const& storage, std::vector<char*> const& pointers) noexcept
{
  if (storage.empty() || storage.size() > detail::kMaximumLaunchArgumentCount || storage.front().empty() || pointers.size() != storage.size() + 1 ||
      pointers.back() != nullptr)
  {
    return false;
  }
  for (std::size_t index = 0; index < storage.size(); ++index)
  {
    if (contains_nul(storage[index]) || pointers[index] != storage[index].data())
      return false;
  }
  return true;
}

[[noreturn]] void child_fail(int descriptor, detail::LaunchFailureStageV1 stage, int error_number) noexcept
{
  static_cast<void>(detail::child_write_launch_failed(descriptor, stage, error_number));
  _exit(127);
}

detail::LaunchFailureStageV1 adoption_stage(AdoptionChildFailureStageV1 stage) noexcept
{
  switch (stage)
  {
    case AdoptionChildFailureStageV1::Streams:
      return detail::LaunchFailureStageV1::Streams;
    case AdoptionChildFailureStageV1::WorkingDirectory:
      return detail::LaunchFailureStageV1::WorkingDirectory;
    case AdoptionChildFailureStageV1::Containment:
      return detail::LaunchFailureStageV1::Containment;
    case AdoptionChildFailureStageV1::DescriptorValidation:
      return detail::LaunchFailureStageV1::DescriptorValidation;
  }
  return detail::LaunchFailureStageV1::DescriptorValidation;
}

bool set_exact_process_group(pid_t process, pid_t group) noexcept
{
  while (true)
  {
    if (::setpgid(process, group) == 0)
      return true;
    if (errno != EINTR)
      return false;
  }
}

bool descriptor_open(int descriptor) noexcept
{
  if (descriptor <= STDERR_FILENO)
    return false;
  int result = -1;
  do
    result = ::fcntl(descriptor, F_GETFD);
  while (result < 0 && errno == EINTR);
  return result >= 0;
}

bool set_descriptor_cloexec(int descriptor, bool enabled) noexcept
{
  int flags = -1;
  do
    flags = ::fcntl(descriptor, F_GETFD);
  while (flags < 0 && errno == EINTR);
  if (flags < 0)
    return false;
  int result = -1;
  do
    result = ::fcntl(descriptor, F_SETFD, enabled ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC));
  while (result < 0 && errno == EINTR);
  return result == 0;
}

struct ExpectedStopResult
{
  enum class Kind
  {
    Stopped,
    Exited,
    TimedOut,
    Failed,
  };

  Kind kind = Kind::Failed;
  int error_number = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

ExpectedStopResult await_expected_mermaid_stop(pid_t leader, ProcessDeadline deadline) noexcept
{
  while (Clock::now() < deadline)
  {
    siginfo_t stopped{};
    if (detail::waitid_retry(leader, &stopped, WSTOPPED | WNOHANG) != 0)
      return {.kind = ExpectedStopResult::Kind::Failed, .error_number = errno == 0 ? EIO : errno};
    if (stopped.si_pid == leader)
    {
      if (stopped.si_code == CLD_STOPPED && stopped.si_status == SIGSTOP)
        return {.kind = ExpectedStopResult::Kind::Stopped};
      return {.kind = ExpectedStopResult::Kind::Failed, .error_number = EPROTO};
    }

    siginfo_t exited{};
    if (detail::waitid_retry(leader, &exited, WEXITED | WNOHANG | WNOWAIT) != 0)
      return {.kind = ExpectedStopResult::Kind::Failed, .error_number = errno == 0 ? EIO : errno};
    if (exited.si_pid == leader)
      return {.kind = ExpectedStopResult::Kind::Exited};

    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    int const delay = static_cast<int>(std::min(remaining, 5ms).count());
    if (delay > 0)
    {
      int result = -1;
      do
        result = ::poll(nullptr, 0, delay);
      while (result < 0 && errno == EINTR && Clock::now() < deadline);
      if (result < 0)
        return {.kind = ExpectedStopResult::Kind::Failed, .error_number = errno == 0 ? EIO : errno};
    }
  }
  return {.kind = ExpectedStopResult::Kind::TimedOut};
}

ava::core::Error expected_stop_error(ExpectedStopResult const& result)
{
  switch (result.kind)
  {
    case ExpectedStopResult::Kind::Exited:
      return detail::process_error(ava::core::ErrorCategory::Io, "Mermaid leader exited before its required startup stop");
    case ExpectedStopResult::Kind::TimedOut:
      return detail::process_error(ava::core::ErrorCategory::Io, "Mermaid startup stop timed out");
    case ExpectedStopResult::Kind::Failed:
      return detail::io_error("failed to observe the Mermaid startup stop", result.error_number > 0 ? result.error_number : EIO);
    case ExpectedStopResult::Kind::Stopped:
      break;
  }
  return detail::process_error(ava::core::ErrorCategory::Io, "invalid Mermaid startup-stop outcome");
}

#endif

}  // namespace

AdoptionGate::AdoptionGate() noexcept = default;
AdoptionGate::AdoptionGate(std::unique_ptr<Impl> implementation) noexcept : implementation_(std::move(implementation))
{
}
AdoptionGate::AdoptionGate(AdoptionGate&&) noexcept = default;
AdoptionGate& AdoptionGate::operator=(AdoptionGate&& other) noexcept
{
  if (this != &other)
  {
    abandon();
    implementation_ = std::move(other.implementation_);
  }
  return *this;
}
AdoptionGate::~AdoptionGate()
{
  abandon();
}

void AdoptionGate::abandon() noexcept
{
  if (!implementation_)
    return;
  auto& gate = *implementation_;
#if !defined(_WIN32)
  if (gate.child_branch)
  {
    implementation_.reset();
    return;
  }
  if (!gate.registered && (gate.leader > 1 || gate.sentinel > 1))
  {
    auto const cleanup_deadline = detail::provisional_cleanup_deadline(gate.state, gate.record, TerminationReasonV1::LaunchFailed, Clock::now() + 500ms);
    gate.leader_control.write_end.reset();
    gate.containment_control.write_end.reset();
    if (gate.sentinel_control)
      gate.sentinel_control->write_end.reset();
    bool const cleaned = detail::exact_provisional_cleanup(gate.leader, gate.sentinel, cleanup_deadline);
    detail::finish_unregistered(gate.state, gate.record, TerminationReasonV1::LaunchFailed, cleaned ? CleanupStateV1::Complete : CleanupStateV1::Incomplete);
    gate.record = 0;
    implementation_.reset();
    return;
  }
  gate.leader_control.write_end.reset();
  gate.containment_control.write_end.reset();
  if (gate.sentinel_control)
    gate.sentinel_control->write_end.reset();
#endif
  if (!gate.registered)
    detail::finish_unregistered(gate.state, gate.record, TerminationReasonV1::LaunchFailed);
  gate.record = 0;
  implementation_.reset();
}

bool AdoptionGate::valid() const noexcept
{
  return implementation_ && implementation_->state && implementation_->record != 0;
}

ava::core::Result<AdoptionForkBranchV1> AdoptionGate::fork_leader()
{
#if defined(_WIN32)
  return std::unexpected(detail::unsupported_error());
#else
  if (!valid() || implementation_->leader > 1 || implementation_->child_branch)
    return std::unexpected(detail::invalid_error("secure adoption gate cannot fork another leader"));
  auto& gate = *implementation_;
  bool const binding_valid = detail::EnvironmentAccess::matches_secure_adoption(gate.environment, gate.role, gate.cwd) && gate.anchored_cwd.valid() &&
                             detail::ExecutionCapabilityAccess::logical_matches(gate.anchored_cwd, gate.cwd) &&
                             prepared_argv_valid(gate.argv_storage, gate.argv_pointers) && is_valid(gate.bash_containment) &&
                             (gate.role != ProcessRoleV1::Mermaid || gate.cwd == "/") &&
                             (gate.bash_containment != BashContainmentHandshakeV1::Required || gate.role == ProcessRoleV1::Bash);
  if (!binding_valid)
  {
    detail::finish_unregistered(gate.state, gate.record, TerminationReasonV1::LaunchFailed);
    gate.record = 0;
    return std::unexpected(detail::invalid_error("secure-adoption leader requires its retained closed launch binding"));
  }
  if (auto refreshed = detail::ExecutionCapabilityAccess::refresh(gate.anchored_cwd); !refreshed)
  {
    detail::finish_unregistered(gate.state, gate.record, TerminationReasonV1::LaunchFailed);
    gate.record = 0;
    return std::unexpected(std::move(refreshed.error()));
  }

  detail::PreForkDecision initial_check;
  {
    std::lock_guard lock(gate.state->mutex);
    initial_check = detail::check_pre_fork_launch_locked(*gate.state, gate.record, gate.startup_deadline);
  }
  detail::notify_monitor_state(gate.state);
  if (!initial_check.launchable)
  {
    detail::finish_unregistered(gate.state, gate.record, initial_check.reason);
    gate.record = 0;
    return std::unexpected(detail::canceled_launch_error("secure-adoption leader launch", initial_check.reason));
  }

  // Revalidate immutable bindings, then refresh the retained cwd route at the
  // final pre-fork boundary after all allocator-backed preparation.
  if (!detail::EnvironmentAccess::matches_secure_adoption(gate.environment, gate.role, gate.cwd) || !gate.anchored_cwd.valid() ||
      !detail::ExecutionCapabilityAccess::logical_matches(gate.anchored_cwd, gate.cwd) || !prepared_argv_valid(gate.argv_storage, gate.argv_pointers) ||
      !is_valid(gate.bash_containment) || (gate.role == ProcessRoleV1::Mermaid && gate.cwd != "/") ||
      (gate.bash_containment == BashContainmentHandshakeV1::Required && gate.role != ProcessRoleV1::Bash))
  {
    detail::finish_unregistered(gate.state, gate.record, TerminationReasonV1::LaunchFailed);
    gate.record = 0;
    return std::unexpected(detail::invalid_error("secure-adoption leader lost its retained closed launch binding"));
  }
  if (auto refreshed = detail::ExecutionCapabilityAccess::refresh(gate.anchored_cwd); !refreshed)
  {
    detail::finish_unregistered(gate.state, gate.record, TerminationReasonV1::LaunchFailed);
    gate.record = 0;
    return std::unexpected(std::move(refreshed.error()));
  }

  detail::PreForkDecision final_check;
  {
    std::lock_guard lock(gate.state->mutex);
    final_check = detail::check_pre_fork_launch_locked(*gate.state, gate.record, gate.startup_deadline);
  }
  detail::notify_monitor_state(gate.state);
  if (!final_check.launchable)
  {
    detail::finish_unregistered(gate.state, gate.record, final_check.reason);
    gate.record = 0;
    return std::unexpected(detail::canceled_launch_error("secure-adoption leader launch", final_check.reason));
  }

  pid_t const process = ::fork();
  if (process < 0)
  {
    auto error = detail::io_error("failed to fork the secure-adoption leader", errno);
    detail::finish_unregistered(gate.state, gate.record, TerminationReasonV1::LaunchFailed);
    gate.record = 0;
    return std::unexpected(std::move(error));
  }
  if (process == 0)
  {
    gate.child_branch = true;
    gate.leader = -1;
    gate.launch_status.read_end.reset();
    gate.leader_control.write_end.reset();
    gate.containment_control.write_end.reset();
    if (gate.sentinel_status)
    {
      gate.sentinel_status->read_end.reset();
      gate.sentinel_status->write_end.reset();
    }
    if (gate.sentinel_control)
    {
      gate.sentinel_control->read_end.reset();
      gate.sentinel_control->write_end.reset();
    }
    int const status_descriptor = gate.launch_status.write_end.get();
    if (!detail::reset_child_signal_state())
      child_fail(status_descriptor, detail::LaunchFailureStageV1::SignalReset, errno == 0 ? EIO : errno);
    if (::setpgid(0, 0) != 0)
      child_fail(status_descriptor, detail::LaunchFailureStageV1::ProcessGroup, errno);
    if (!detail::child_write_leader_ready(status_descriptor))
      _exit(127);

    char release = '\0';
    auto const release_count = detail::child_read_retry(gate.leader_control.read_end.get(), &release, 1);
    if (release_count != 1 || release != 'G')
      child_fail(status_descriptor, detail::LaunchFailureStageV1::Gate, release_count < 0 && errno > 0 ? errno : EIO);
    gate.leader_control.read_end.reset();
    if (gate.bash_containment == BashContainmentHandshakeV1::None)
      gate.containment_control.read_end.reset();

    if (gate.role == ProcessRoleV1::Mermaid && ::raise(SIGSTOP) != 0)
      child_fail(status_descriptor, detail::LaunchFailureStageV1::Gate, errno == 0 ? EIO : errno);
    int const cwd_descriptor = detail::ExecutionCapabilityAccess::target_descriptor(gate.anchored_cwd);
    if (::fchdir(cwd_descriptor) != 0)
      child_fail(status_descriptor, detail::LaunchFailureStageV1::WorkingDirectory, errno == 0 ? EIO : errno);
    detail::ExecutionCapabilityAccess::child_close_after_fchdir(gate.anchored_cwd);
    gate.cwd_applied = true;
    gate.child_api_ready = true;
    return AdoptionForkBranchV1::Child;
  }
  gate.leader = process;
  // Establish the group before the sentinel fork even when the leader has not run yet.
  bool const leader_group_set = set_exact_process_group(process, process);
  int const leader_group_error = leader_group_set ? 0 : errno;
  pid_t const parent_group = ::getpgrp();
  pid_t const observed_leader_group = ::getpgid(process);
  int const observed_group_error = observed_leader_group >= 0 ? 0 : errno;
  if (!leader_group_set || observed_leader_group != process || parent_group <= 0 || observed_leader_group == parent_group)
  {
    int const error_number = !leader_group_set ? leader_group_error : observed_leader_group < 0 ? observed_group_error : EIO;
    auto error =
        detail::io_error("failed to establish and prove the secure-adoption leader process group before sentinel fork", error_number > 0 ? error_number : EIO);
    abandon();
    return std::unexpected(std::move(error));
  }
  // The forked leader has its own inherited cwd authority. Drop every parent
  // cwd/route copy before a caller can request the optional sentinel.
  gate.anchored_cwd = AnchoredWorkingDirectoryV1{};
  gate.launch_status.write_end.reset();
  gate.leader_control.read_end.reset();
  gate.containment_control.read_end.reset();
  return AdoptionForkBranchV1::Parent;
#endif
}

ava::core::VoidResult AdoptionGate::fork_sentinel()
{
#if defined(_WIN32)
  return std::unexpected(detail::unsupported_error());
#else
  if (!valid() || implementation_->child_branch)
    return std::unexpected(detail::invalid_error("secure adoption sentinel requires one valid parent gate"));
  auto& gate = *implementation_;
  if (gate.role != ProcessRoleV1::Bash || !detail::EnvironmentAccess::matches_secure_adoption(gate.environment, gate.role, gate.cwd))
  {
    abandon();
    return std::unexpected(detail::invalid_error("secure adoption sentinel is unavailable for this closed launch capability"));
  }
  if (gate.leader <= 1 || gate.sentinel > 1 || !gate.sentinel_status || !gate.sentinel_control)
    return std::unexpected(detail::invalid_error("secure adoption sentinel requires exactly one gated parent leader"));
  detail::PreForkDecision pre_fork;
  {
    std::lock_guard lock(gate.state->mutex);
    pre_fork = detail::check_pre_fork_launch_locked(*gate.state, gate.record, gate.startup_deadline);
  }
  detail::notify_monitor_state(gate.state);
  if (!pre_fork.launchable)
    return std::unexpected(detail::canceled_launch_error("secure-adoption sentinel launch", pre_fork.reason));

  pid_t const process = ::fork();
  if (process < 0)
  {
    auto error = detail::io_error("failed to fork the secure-adoption sentinel", errno);
    abandon();
    return std::unexpected(std::move(error));
  }
  if (process == 0)
  {
    gate.child_branch = true;
    int const status_descriptor = gate.sentinel_status->write_end.get();
    int const control_descriptor = gate.sentinel_control->read_end.get();
    std::array<int, 2> const preserved{status_descriptor, control_descriptor};
    detail::close_nonstandard_descriptors_except(preserved, gate.maximum_descriptor);
    static_cast<void>(::close(STDIN_FILENO));
    static_cast<void>(::close(STDOUT_FILENO));
    static_cast<void>(::close(STDERR_FILENO));

    if (!detail::reset_child_signal_state())
      child_fail(status_descriptor, detail::LaunchFailureStageV1::SignalReset, errno == 0 ? EIO : errno);
    if (::setpgid(0, gate.leader) != 0)
      child_fail(status_descriptor, detail::LaunchFailureStageV1::ProcessGroup, errno);
    if (!detail::child_write_leader_ready(status_descriptor))
      _exit(127);
    char release = '\0';
    auto const release_count = detail::child_read_retry(control_descriptor, &release, 1);
    if (release_count != 1 || release != 'G')
      child_fail(status_descriptor, detail::LaunchFailureStageV1::Gate, release_count < 0 && errno > 0 ? errno : EIO);
    static_cast<void>(::close(status_descriptor));
    static_cast<void>(::close(control_descriptor));
    while (true)
      static_cast<void>(::pause());
  }
  gate.sentinel = process;
  gate.sentinel_status->write_end.reset();
  gate.sentinel_control->read_end.reset();
  return {};
#endif
}

[[noreturn]] void AdoptionGate::child_launch_failed(AdoptionChildFailureStageV1 stage, int errno_value) noexcept
{
#if defined(_WIN32)
  static_cast<void>(stage);
  static_cast<void>(errno_value);
  std::_Exit(127);
#else
  if (!implementation_)
    _exit(127);
  auto& gate = *implementation_;
  auto protocol_stage = adoption_stage(stage);
  if (!gate.child_branch || !gate.child_api_ready || !is_valid(stage))
    protocol_stage = detail::LaunchFailureStageV1::DescriptorValidation;
  child_fail(gate.launch_status.write_end.get(), protocol_stage, errno_value > 0 ? errno_value : EIO);
#endif
}

void AdoptionGate::child_bash_containment_applied() noexcept
{
#if defined(_WIN32)
  std::_Exit(127);
#else
  if (!implementation_)
    _exit(127);
  auto& gate = *implementation_;
  int const status_descriptor = gate.launch_status.write_end.get();
  if (!gate.child_branch || !gate.child_api_ready || !gate.cwd_applied || gate.role != ProcessRoleV1::Bash ||
      gate.bash_containment != BashContainmentHandshakeV1::Required || gate.containment_applied)
  {
    child_fail(status_descriptor, detail::LaunchFailureStageV1::DescriptorValidation, EINVAL);
  }
  if (!detail::child_write_bash_containment_applied(status_descriptor))
    _exit(127);
  char continuation = '\0';
  auto const continuation_count = detail::child_read_retry(gate.containment_control.read_end.get(), &continuation, 1);
  if (continuation_count != 1 || continuation != 'C')
    child_fail(status_descriptor, detail::LaunchFailureStageV1::Containment, continuation_count < 0 && errno > 0 ? errno : EIO);
  gate.containment_control.read_end.reset();
  gate.containment_applied = true;
#endif
}

[[noreturn]] void AdoptionGate::child_exec_descriptor(int executable_descriptor, std::span<int const> retained_script_descriptors) noexcept
{
#if defined(_WIN32)
  static_cast<void>(executable_descriptor);
  static_cast<void>(retained_script_descriptors);
  std::_Exit(127);
#else
  if (!implementation_)
    _exit(127);
  auto& gate = *implementation_;
  int const status_descriptor = gate.launch_status.write_end.get();
  auto descriptor_failure = [&]() -> void { child_fail(status_descriptor, detail::LaunchFailureStageV1::DescriptorValidation, EINVAL); };
  if (!gate.child_branch || !gate.child_api_ready)
    descriptor_failure();
  if (!gate.cwd_applied)
    child_fail(status_descriptor, detail::LaunchFailureStageV1::WorkingDirectory, EPROTO);
  if (gate.bash_containment == BashContainmentHandshakeV1::Required && !gate.containment_applied)
    child_fail(status_descriptor, detail::LaunchFailureStageV1::Containment, EPROTO);
  if (retained_script_descriptors.size() > kMaxRetainedScriptDescriptorsV1 || !descriptor_open(executable_descriptor) || gate.argv_pointers.empty() ||
      gate.argv_pointers.back() != nullptr || gate.environment_pointers.empty() || gate.environment_pointers.back() != nullptr)
  {
    descriptor_failure();
  }

  auto hidden_descriptor = [&](int descriptor) noexcept {
    bool hidden = descriptor == status_descriptor || descriptor == gate.leader_control.read_end.get() || descriptor == gate.leader_control.write_end.get() ||
                  descriptor == gate.containment_control.read_end.get() || descriptor == gate.containment_control.write_end.get();
    if (gate.sentinel_status)
      hidden = hidden || descriptor == gate.sentinel_status->read_end.get() || descriptor == gate.sentinel_status->write_end.get();
    if (gate.sentinel_control)
      hidden = hidden || descriptor == gate.sentinel_control->read_end.get() || descriptor == gate.sentinel_control->write_end.get();
    return hidden;
  };
  if (hidden_descriptor(executable_descriptor) || executable_descriptor >= gate.maximum_descriptor)
    descriptor_failure();

  for (std::size_t index = 0; index < retained_script_descriptors.size(); ++index)
  {
    int const descriptor = retained_script_descriptors[index];
    if (!descriptor_open(descriptor) || descriptor >= gate.maximum_descriptor || descriptor == executable_descriptor || hidden_descriptor(descriptor))
      descriptor_failure();
    for (std::size_t previous = 0; previous < index; ++previous)
    {
      if (retained_script_descriptors[previous] == descriptor)
        descriptor_failure();
    }
  }

  if (!set_descriptor_cloexec(executable_descriptor, true))
    descriptor_failure();

  std::array<int, kMaxRetainedScriptDescriptorsV1 + 2> preserved{};
  std::size_t preserved_count = 0;
  preserved[preserved_count++] = status_descriptor;
  preserved[preserved_count++] = executable_descriptor;
  for (int descriptor : retained_script_descriptors)
    preserved[preserved_count++] = descriptor;
  detail::close_nonstandard_descriptors_except(std::span<int const>(preserved.data(), preserved_count), gate.maximum_descriptor);
  for (int descriptor : retained_script_descriptors)
  {
    if (!set_descriptor_cloexec(descriptor, false))
      child_fail(status_descriptor, detail::LaunchFailureStageV1::DescriptorValidation, errno == 0 ? EIO : errno);
  }

  if (!detail::child_write_exec_attempt(status_descriptor))
    _exit(127);
#if defined(__linux__) && defined(SYS_execveat) && defined(AT_EMPTY_PATH)
  static_cast<void>(::syscall(SYS_execveat, executable_descriptor, "", gate.argv_pointers.data(), gate.environment_pointers.data(), AT_EMPTY_PATH));
#else
  static_cast<void>(::fexecve(executable_descriptor, gate.argv_pointers.data(), gate.environment_pointers.data()));
#endif
  int const execute_error = errno == 0 ? EIO : errno;
  static_cast<void>(detail::child_write_exec_failed(status_descriptor, execute_error));
  _exit(127);
#endif
}

ava::core::Result<AdoptionGate> Supervisor::begin_secure_adoption(Reservation&& reservation, SecureAdoptionSpecV1 specification)
{
  auto state = implementation_->state;
  auto consumed = consume_reservation(reservation);
  if (!consumed)
    return std::unexpected(std::move(consumed.error()));
  auto const identity = *consumed;
#if defined(_WIN32)
  static_cast<void>(specification);
  detail::finish_unregistered(state, identity, TerminationReasonV1::LaunchFailed);
  return std::unexpected(detail::unsupported_error());
#else
  auto const role = detail::record_role(state, identity);
  auto reject_invalid = [&](std::string message) -> ava::core::Result<AdoptionGate> {
    detail::finish_unregistered(state, identity, TerminationReasonV1::LaunchFailed);
    return std::unexpected(detail::invalid_error(std::move(message)));
  };
  bool const binding_valid = role && is_valid(specification.bash_containment) && !specification.cwd.empty() && specification.cwd.starts_with('/') &&
                             !contains_nul(specification.cwd) && specification.anchored_cwd.valid() &&
                             detail::ExecutionCapabilityAccess::logical_matches(specification.anchored_cwd, specification.cwd) &&
                             detail::EnvironmentAccess::matches_secure_adoption(specification.environment, *role, specification.cwd) &&
                             (*role != ProcessRoleV1::Mermaid || specification.cwd == "/") &&
                             (specification.bash_containment != BashContainmentHandshakeV1::Required || *role == ProcessRoleV1::Bash);
  if (!binding_valid)
    return reject_invalid("reserved secure adoption requires one matching closed environment, anchored cwd, and containment specification");
  if (auto refreshed = detail::ExecutionCapabilityAccess::refresh(specification.anchored_cwd); !refreshed)
  {
    detail::finish_unregistered(state, identity, TerminationReasonV1::LaunchFailed);
    return std::unexpected(std::move(refreshed.error()));
  }
  if (specification.argv.empty() || specification.argv.size() > detail::kMaximumLaunchArgumentCount)
    return reject_invalid("secure-adoption argv count is outside the supported bound");
  if (specification.argv.front().empty())
    return reject_invalid("secure-adoption argv[0] must be nonempty");

  std::size_t prepared_bytes = specification.cwd.size();
  if (prepared_bytes > detail::kMaximumPreparedLaunchBytes)
    return reject_invalid("secure-adoption cwd exceeds the aggregate byte bound");
  for (auto const& argument : specification.argv)
  {
    if (contains_nul(argument))
      return reject_invalid("secure-adoption argv contains a NUL byte");
    if (argument.size() > detail::kMaximumPreparedLaunchBytes - std::min(prepared_bytes, detail::kMaximumPreparedLaunchBytes))
      return reject_invalid("secure-adoption argv exceeds the aggregate byte bound");
    prepared_bytes += argument.size();
  }
  for (auto const& variable : detail::EnvironmentAccess::variables(specification.environment))
  {
    auto const bytes = variable.name.size() + variable.value.size() + 2;
    if (bytes > detail::kMaximumPreparedLaunchBytes - std::min(prepared_bytes, detail::kMaximumPreparedLaunchBytes))
      return reject_invalid("secure-adoption environment exceeds the aggregate byte bound");
    prepared_bytes += bytes;
  }
  auto const startup_deadline = detail::startup_deadline_for_record(state, identity);
  detail::PreForkDecision initial_check;
  {
    std::lock_guard lock(state->mutex);
    initial_check = detail::check_pre_fork_launch_locked(*state, identity, startup_deadline);
  }
  detail::notify_monitor_state(state);
  if (!initial_check.launchable)
  {
    detail::finish_unregistered(state, identity, initial_check.reason);
    return std::unexpected(detail::canceled_launch_error("secure adoption", initial_check.reason));
  }
  auto launch_status = detail::make_cloexec_pipe();
  auto leader_control = launch_status ? detail::make_cloexec_pipe() : ava::core::Result<detail::Pipe>(std::unexpected(launch_status.error()));
  auto containment_control = leader_control ? detail::make_cloexec_pipe() : ava::core::Result<detail::Pipe>(std::unexpected(leader_control.error()));
  if (!launch_status || !leader_control || !containment_control)
  {
    detail::finish_unregistered(state, identity, TerminationReasonV1::LaunchFailed);
    if (!launch_status)
      return std::unexpected(std::move(launch_status.error()));
    if (!leader_control)
      return std::unexpected(std::move(leader_control.error()));
    return std::unexpected(std::move(containment_control.error()));
  }

  std::optional<detail::Pipe> sentinel_status;
  std::optional<detail::Pipe> sentinel_control;
  if (*role == ProcessRoleV1::Bash)
  {
    auto status = detail::make_cloexec_pipe();
    auto control = status ? detail::make_cloexec_pipe() : ava::core::Result<detail::Pipe>(std::unexpected(status.error()));
    if (!status || !control)
    {
      detail::finish_unregistered(state, identity, TerminationReasonV1::LaunchFailed);
      return std::unexpected(!status ? std::move(status.error()) : std::move(control.error()));
    }
    sentinel_status.emplace(std::move(*status));
    sentinel_control.emplace(std::move(*control));
  }

  std::unique_ptr<AdoptionGate::Impl> gate;
  try
  {
    gate = std::make_unique<AdoptionGate::Impl>();
    gate->handle = std::make_shared<detail::HandleState>();
    gate->state = state;
    gate->record = identity;
    gate->role = *role;
    gate->environment = std::move(specification.environment);
    gate->cwd = std::move(specification.cwd);
    gate->anchored_cwd = std::move(specification.anchored_cwd);
    gate->argv_storage = std::move(specification.argv);
    gate->bash_containment = specification.bash_containment;
    gate->argv_pointers.reserve(gate->argv_storage.size() + 1);
    for (auto& argument : gate->argv_storage)
      gate->argv_pointers.push_back(argument.data());
    gate->argv_pointers.push_back(nullptr);
    auto const& variables = detail::EnvironmentAccess::variables(gate->environment);
    gate->environment_storage.reserve(variables.size());
    for (auto const& variable : variables)
      gate->environment_storage.push_back(variable.name + "=" + variable.value);
    gate->environment_pointers.reserve(gate->environment_storage.size() + 1);
    for (auto& entry : gate->environment_storage)
      gate->environment_pointers.push_back(entry.data());
    gate->environment_pointers.push_back(nullptr);
  }
  catch (std::exception const&)
  {
    detail::finish_unregistered(state, identity, TerminationReasonV1::LaunchFailed);
    return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "failed to prepare secure-adoption capabilities"));
  }
  catch (...)
  {
    detail::finish_unregistered(state, identity, TerminationReasonV1::LaunchFailed);
    return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "failed to prepare secure-adoption capabilities"));
  }

  gate->startup_deadline = startup_deadline;
  gate->launch_status = std::move(*launch_status);
  gate->leader_control = std::move(*leader_control);
  gate->containment_control = std::move(*containment_control);
  gate->sentinel_status = std::move(sentinel_status);
  gate->sentinel_control = std::move(sentinel_control);
  gate->maximum_descriptor = detail::descriptor_limit();

  if (!detail::ExecutionCapabilityAccess::logical_matches(gate->anchored_cwd, gate->cwd))
  {
    detail::finish_unregistered(state, identity, TerminationReasonV1::LaunchFailed);
    return std::unexpected(detail::invalid_error("secure adoption lost its anchored cwd binding"));
  }
  if (auto refreshed = detail::ExecutionCapabilityAccess::refresh(gate->anchored_cwd); !refreshed)
  {
    detail::finish_unregistered(state, identity, TerminationReasonV1::LaunchFailed);
    return std::unexpected(std::move(refreshed.error()));
  }

  detail::PreForkDecision final_check;
  {
    std::lock_guard lock(state->mutex);
    final_check = detail::check_pre_fork_launch_locked(*state, identity, startup_deadline);
  }
  detail::notify_monitor_state(state);
  if (!final_check.launchable)
  {
    detail::finish_unregistered(state, identity, final_check.reason);
    return std::unexpected(detail::canceled_launch_error("secure adoption", final_check.reason));
  }
  return AdoptionGate(std::move(gate));
#endif
}

ava::core::Result<ProcessHandle> Supervisor::adopt(AdoptionGate&& gate)
{
#if defined(_WIN32)
  static_cast<void>(gate);
  return std::unexpected(detail::unsupported_error());
#else
  auto state = implementation_->state;
  if (!gate.valid() || gate.implementation_->state.get() != state.get() || gate.implementation_->child_branch || gate.implementation_->leader <= 1)
    return std::unexpected(detail::invalid_error("secure adoption gate does not contain this supervisor's exact gated leader"));
  // A validated post-fork gate is consumed on every outcome so early framing
  // and group-proof failures cannot defer provisional cleanup to the caller.
  AdoptionGate owned_gate(std::move(gate));
  auto& ticket = *owned_gate.implementation_;
  auto const identity = ticket.record;

  auto const leader_ready = detail::await_launch_leader_ready(ticket.launch_status.read_end.get(), ticket.startup_deadline);
  if (leader_ready.disposition != detail::LaunchProtocolDispositionV1::LeaderReady)
    return std::unexpected(detail::launch_protocol_error(leader_ready, "secure-adoption leader"));

  bool const leader_group_set = set_exact_process_group(ticket.leader, ticket.leader);
  int const leader_group_error = errno;
  pid_t const parent_group = ::getpgrp();
  pid_t const observed_leader_group = ::getpgid(ticket.leader);
  if (!leader_group_set || observed_leader_group != ticket.leader || parent_group <= 0 || observed_leader_group == parent_group)
    return std::unexpected(detail::io_error("failed to prove the secure-adoption leader process group", leader_group_error == 0 ? EIO : leader_group_error));

  if (ticket.sentinel > 1)
  {
    auto const sentinel_ready = detail::await_launch_leader_ready(ticket.sentinel_status->read_end.get(), ticket.startup_deadline);
    if (sentinel_ready.disposition != detail::LaunchProtocolDispositionV1::LeaderReady)
      return std::unexpected(detail::launch_protocol_error(sentinel_ready, "secure-adoption sentinel"));
    bool const sentinel_group_set = set_exact_process_group(ticket.sentinel, ticket.leader);
    int const sentinel_group_error = errno;
    if (!sentinel_group_set || ::getpgid(ticket.sentinel) != ticket.leader)
      return std::unexpected(
          detail::io_error("failed to prove the exact secure-adoption sentinel membership", sentinel_group_error == 0 ? EIO : sentinel_group_error));
  }
  else
  {
    ticket.sentinel_status.reset();
    ticket.sentinel_control.reset();
  }

  if (auto monitor = detail::ensure_monitor_started(state); !monitor)
    return std::unexpected(std::move(monitor.error()));

  auto handle_state = ticket.handle;
  {
    std::lock_guard lock(state->mutex);
    auto found = state->records.find(identity);
    if (found == state->records.end() || found->second->state == ProcessStateV1::Finished)
      return std::unexpected(detail::process_error(ava::core::ErrorCategory::Io, "secure-adoption reservation ended before registry commit"));
    auto& record = *found->second;
    static_cast<void>(detail::commit_due_execution_deadline_locked(record, Clock::now()));
    handle_state->supervisor = state;
    handle_state->record = identity;
    record.handle = handle_state;
    record.registered = true;
    record.group_verified = true;
    record.startup_handshake_complete = false;
    record.leader = ticket.leader;
    record.sentinel = ticket.sentinel;
    record.cleanup = CleanupStateV1::Pending;
    record.state = record.reason ? ProcessStateV1::StopRequested : ProcessStateV1::Launching;
    detail::register_record_members_locked(*state, record, Clock::now());
    ticket.registered = true;
  }
  detail::notify_monitor_state(state);

  bool gate_release_committed = false;
  auto fail_registered = [&](TerminationReasonV1 reason, ava::core::Error error) -> ava::core::Result<ProcessHandle> {
    auto const failure = detail::fail_registered_launch(state, identity, reason);
    ticket.leader_control.write_end.reset();
    ticket.containment_control.write_end.reset();
    ticket.launch_status.read_end.reset();
    if (ticket.sentinel_control)
      ticket.sentinel_control->write_end.reset();
    detail::notify_monitor_state(state);
    detail::await_internal_settlement(handle_state, failure.cleanup_deadline);
    ticket.record = 0;
    if (failure.reason != reason)
    {
      return std::unexpected(gate_release_committed ? detail::startup_stopped_error("secure adoption", failure.reason)
                                                    : detail::canceled_launch_error("secure adoption", failure.reason));
    }
    return std::unexpected(std::move(error));
  };

  if (auto hook = detail::invoke_after_fork_before_release_hook(state); !hook)
    return fail_registered(TerminationReasonV1::LaunchFailed, std::move(hook.error()));

  detail::GateReleaseDecision release_decision;
  {
    std::lock_guard lock(state->mutex);
    // One commit linearizes adoption before either exact child is released.
    release_decision = detail::commit_gate_release_locked(*state, identity, ticket.startup_deadline);
  }
  detail::notify_monitor_state(state);
  if (!release_decision.committed)
  {
    ticket.leader_control.write_end.reset();
    ticket.containment_control.write_end.reset();
    if (ticket.sentinel_control)
      ticket.sentinel_control->write_end.reset();
    detail::await_internal_settlement(handle_state, release_decision.cleanup_deadline);
    ticket.record = 0;
    return std::unexpected(detail::canceled_launch_error("secure adoption", release_decision.reason));
  }
  gate_release_committed = true;

  char const release = 'G';
  bool released = true;
  if (ticket.sentinel_control)
    released = detail::write_without_sigpipe(ticket.sentinel_control->write_end.get(), &release, 1);
  released = detail::write_without_sigpipe(ticket.leader_control.write_end.get(), &release, 1) && released;
  ticket.leader_control.write_end.reset();
  if (ticket.sentinel_control)
    ticket.sentinel_control->write_end.reset();
  if (!released)
  {
    return fail_registered(TerminationReasonV1::LaunchFailed,
                           detail::process_error(ava::core::ErrorCategory::Io, "failed to release a committed secure-adoption gate"));
  }

  if (auto hook = detail::invoke_after_gate_release_hook(state); !hook)
    return fail_registered(TerminationReasonV1::LaunchFailed, std::move(hook.error()));

  if (ticket.role == ProcessRoleV1::Mermaid)
  {
    auto const stop = await_expected_mermaid_stop(ticket.leader, ticket.startup_deadline);
    if (stop.kind != ExpectedStopResult::Kind::Stopped)
      return fail_registered(TerminationReasonV1::LaunchFailed, expected_stop_error(stop));

    std::optional<TerminationReasonV1> stopped_reason;
    {
      std::lock_guard lock(state->mutex);
      auto found = state->records.find(identity);
      if (found == state->records.end())
        stopped_reason = TerminationReasonV1::LaunchFailed;
      else
      {
        static_cast<void>(detail::commit_due_execution_deadline_locked(*found->second, Clock::now()));
        stopped_reason = found->second->reason;
      }
    }
    if (stopped_reason)
      return fail_registered(TerminationReasonV1::LaunchFailed, detail::startup_stopped_error("secure adoption", *stopped_reason));
    if (::kill(-ticket.leader, SIGCONT) != 0)
      return fail_registered(TerminationReasonV1::LaunchFailed, detail::io_error("failed to continue the expected Mermaid startup stop", errno));
  }

  bool const containment_required = ticket.bash_containment == BashContainmentHandshakeV1::Required;
  auto const confirmation = detail::await_launch_exec_confirmation(ticket.launch_status.read_end.get(), ticket.startup_deadline, containment_required,
                                                                   containment_required ? ticket.containment_control.write_end.get() : -1);
  ticket.containment_control.write_end.reset();
  ticket.launch_status.read_end.reset();
  if (confirmation.disposition != detail::LaunchProtocolDispositionV1::ExecConfirmed)
  {
    auto const reason =
        confirmation.disposition == detail::LaunchProtocolDispositionV1::ExecFailed ? TerminationReasonV1::ExecFailed : TerminationReasonV1::LaunchFailed;
    return fail_registered(reason, detail::launch_protocol_error(confirmation, "secure adoption"));
  }

  std::optional<TerminationReasonV1> stopped_reason;
  {
    std::lock_guard lock(state->mutex);
    auto found = state->records.find(identity);
    if (found == state->records.end())
      stopped_reason = TerminationReasonV1::LaunchFailed;
    else
    {
      static_cast<void>(detail::commit_due_execution_deadline_locked(*found->second, Clock::now()));
      if (found->second->state == ProcessStateV1::Finished || found->second->reason)
        stopped_reason = found->second->reason.value_or(TerminationReasonV1::LaunchFailed);
      else
        found->second->startup_handshake_complete = true;
    }
  }
  detail::notify_monitor_state(state);
  if (stopped_reason)
    return fail_registered(TerminationReasonV1::LaunchFailed, detail::startup_stopped_error("secure adoption", *stopped_reason));

  ticket.record = 0;
  return ProcessHandle(std::move(handle_state));
#endif
}

}  // namespace ava::process
