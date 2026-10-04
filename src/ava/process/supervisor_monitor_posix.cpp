#include "sys.h"
#include "ava/process/supervisor_internal.h"
#include "utils/Signals.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#if !defined(_WIN32)
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <dirent.h>
#endif
#endif
#include "debug.h"

namespace ava::process::detail {

#if !defined(_WIN32)
namespace {

constexpr std::size_t kMaximumPollEntries = 1 + 2 * kMaxLiveProcessRecordsV1;

enum class MonitoredMember
{
  Leader,
  Sentinel,
};

struct PollRegistration
{
  std::shared_ptr<MonitorWake> wake;
  std::shared_ptr<PidfdWatch> watch;
  std::uint64_t record_alias = 0;
  std::uint64_t generation = 0;
  MonitoredMember member = MonitoredMember::Leader;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PollBatch
{
  std::array<pollfd, kMaximumPollEntries> descriptors{};
  std::array<PollRegistration, kMaximumPollEntries> registrations{};
  std::size_t count = 0;
  std::optional<Clock::time_point> deadline;
  int error_number = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

#if defined(__linux__)
bool disappearance_error(int error_number) noexcept
{
  return error_number == ENOENT || error_number == ESRCH;
}

bool valid_linux_process_state(char state) noexcept
{
  switch (state)
  {
    case 'R':
    case 'S':
    case 'D':
    case 'Z':
    case 'T':
    case 't':
    case 'X':
    case 'x':
    case 'K':
    case 'W':
    case 'P':
    case 'I':
      return true;
  }
  return false;
}
#endif

void update_peak(std::atomic<std::uint64_t>& peak, std::uint64_t value) noexcept
{
  auto previous = peak.load(std::memory_order_relaxed);
  while (previous < value && !peak.compare_exchange_weak(previous, value, std::memory_order_relaxed))
  {
  }
}

MemberMonitorState& member_monitor(Record& record, MonitoredMember member) noexcept
{
  return member == MonitoredMember::Leader ? record.leader_monitor : record.sentinel_monitor;
}

MemberMonitorState const& member_monitor(Record const& record, MonitoredMember member) noexcept
{
  return member == MonitoredMember::Leader ? record.leader_monitor : record.sentinel_monitor;
}

pid_t member_process(Record const& record, MonitoredMember member) noexcept
{
  return member == MonitoredMember::Leader ? record.leader : record.sentinel;
}

bool& member_observed(Record& record, MonitoredMember member) noexcept
{
  return member == MonitoredMember::Leader ? record.leader_observed : record.sentinel_observed;
}

bool member_observed(Record const& record, MonitoredMember member) noexcept
{
  return member == MonitoredMember::Leader ? record.leader_observed : record.sentinel_observed;
}

bool member_present(Record const& record, MonitoredMember member) noexcept
{
  return member_process(record, member) > 1;
}

void set_nearest(std::optional<Clock::time_point>& nearest, std::optional<Clock::time_point> const& candidate) noexcept
{
  if (candidate && (!nearest || *candidate < *nearest))
    nearest = candidate;
}

void set_nearest(std::optional<Clock::time_point>& nearest, Clock::time_point candidate) noexcept
{
  if (!nearest || candidate < *nearest)
    nearest = candidate;
}

std::size_t fallback_bucket(std::chrono::milliseconds interval) noexcept
{
  constexpr std::array<long long, 8> values{10, 20, 40, 80, 160, 320, 640, 1000};
  for (std::size_t index = 0; index < values.size(); ++index)
  {
    if (interval.count() <= values[index])
      return index;
  }
  return values.size() - 1;
}

std::chrono::milliseconds next_fallback_interval(MemberMonitorState const& member, Record const& record) noexcept
{
  auto maximum = record.active_waiters > 0 ? kWaitedFallbackMaximumInterval : kFallbackMaximumInterval;
  return std::min(maximum, member.fallback_interval * 2);
}

void enable_fallback(MemberMonitorState& member, Clock::time_point now, bool prompt) noexcept
{
  member.watch.reset();
  member.readiness_due = false;
  member.fallback_enabled = true;
  member.fallback_interval = kFallbackInitialInterval;
  member.next_exact_probe = prompt ? now : now + kFallbackInitialInterval;
  ++member.reset_count;
}

void attach_member_locked(SupervisorState& state, Record& record, MonitoredMember kind, Clock::time_point now) noexcept
{
  auto& member = member_monitor(record, kind);
  member = MemberMonitorState{};
  member.generation = state.next_monitor_generation++;
  auto opened = open_pidfd_watch(member_process(record, kind), state.monitor_backend_mode, state.pidfd_runtime_unavailable, state.monitor_telemetry);
  if (opened.cache_runtime_unavailable)
    state.pidfd_runtime_unavailable = true;
  if (opened.watch)
  {
    member.watch = std::move(opened.watch);
    return;
  }
  enable_fallback(member, now, opened.prompt_exact_probe);
}

void detach_member(MemberMonitorState& member) noexcept
{
  member.watch.reset();
  member.readiness_due = false;
  member.fallback_enabled = false;
  member.next_exact_probe = {};
  member.generation = 0;
}

bool short_stop_role(Record const& record) noexcept
{
  return record.role == ProcessRoleV1::ExternalEditor || record.role == ProcessRoleV1::Mermaid;
}

bool short_stop_lane_active(Record const& record) noexcept
{
  return record.startup_handshake_complete && short_stop_role(record) && !record.reason && !record.leader_observed;
}

void count_exact_probe(SupervisorState& state, bool fallback, bool short_probe, MemberMonitorState const& member) noexcept
{
  state.monitor_telemetry->exact_probes.fetch_add(1, std::memory_order_relaxed);
  if (fallback)
  {
    state.monitor_telemetry->fallback_probes.fetch_add(1, std::memory_order_relaxed);
    state.monitor_telemetry->fallback_delay_buckets[fallback_bucket(member.fallback_interval)].fetch_add(1, std::memory_order_relaxed);
  }
  if (short_probe)
    state.monitor_telemetry->short_probes.fetch_add(1, std::memory_order_relaxed);
}

void exact_probe_locked(SupervisorState& state, Record& record, MonitoredMember kind, Clock::time_point now, bool readiness, bool fallback, bool short_probe,
                        bool detect_stops) noexcept
{
  if (!member_present(record, kind) || member_observed(record, kind))
    return;
  auto& member = member_monitor(record, kind);
  count_exact_probe(state, fallback, short_probe, member);
  ++member.probe_count;
  static_cast<void>(observe_member(member_process(record, kind), member_observed(record, kind), record, kind == MonitoredMember::Leader, detect_stops));
  member.readiness_due = false;
  if (member_observed(record, kind))
  {
    member.watch.reset();
    member.fallback_enabled = false;
    return;
  }

  if (readiness)
    enable_fallback(member, now, false);
  else if (fallback)
  {
    member.fallback_interval = next_fallback_interval(member, record);
    member.next_exact_probe = now + member.fallback_interval;
  }
}

void exact_probe_direct_children_locked(SupervisorState& state, Record& record, Clock::time_point now) noexcept
{
  exact_probe_locked(state, record, MonitoredMember::Leader, now, false, false, false, false);
  if (record.sentinel > 1)
    exact_probe_locked(state, record, MonitoredMember::Sentinel, now, false, false, false, false);
}

enum class GroupObservation
{
  Quiet,
  Live,
  Unknown,
};

GroupObservation observe_group_locked(SupervisorState& state, Record& record) noexcept
{
  state.monitor_telemetry->group_observations.fetch_add(1, std::memory_order_relaxed);
  auto const live = verified_group_has_live_member(record.leader);
  if (!live)
  {
    record.cleanup_failed = true;
    record.quiet_group_observations = 0;
    return GroupObservation::Unknown;
  }
  if (*live)
  {
    record.quiet_group_observations = 0;
    return GroupObservation::Live;
  }
  if (record.quiet_group_observations < 2)
  {
    ++record.quiet_group_observations;
    state.monitor_telemetry->quiet_group_proofs.fetch_add(1, std::memory_order_relaxed);
  }
  return GroupObservation::Quiet;
}

bool finalize_after_quiet_locked(SupervisorState& state, Record& record) noexcept
{
  bool const direct_children_observed = record.leader_observed && (record.sentinel <= 1 || record.sentinel_observed);
  if (record.quiet_group_observations < 2 || !direct_children_observed)
    return false;
  bool const leader_reaped = reap_member(record.leader, true, record.leader_reaped, record);
  bool const sentinel_reaped = record.sentinel <= 1 || reap_member(record.sentinel, true, record.sentinel_reaped, record);
  if (leader_reaped && sentinel_reaped)
    finalize_locked(state, record, record.cleanup_failed ? CleanupStateV1::Incomplete : CleanupStateV1::Complete);
  return record.state == ProcessStateV1::Finished;
}

void schedule_group_observation(Record& record, Clock::time_point now) noexcept
{
  record.group_observation_due = now + kPostKillObservation;
}

void process_natural_group_observation_locked(SupervisorState& state, Record& record, Clock::time_point now) noexcept
{
  record.group_observation_due.reset();
  auto const observation = observe_group_locked(state, record);
  if (observation == GroupObservation::Quiet)
  {
    if (!finalize_after_quiet_locked(state, record))
      schedule_group_observation(record, now);
    return;
  }
  begin_stop_locked(record, now);
}

void process_post_kill_group_observation_locked(SupervisorState& state, Record& record, Clock::time_point now) noexcept
{
  record.group_observation_due.reset();
  exact_probe_direct_children_locked(state, record, now);
  auto const observation = observe_group_locked(state, record);
  if (observation == GroupObservation::Quiet && finalize_after_quiet_locked(state, record))
    return;
  schedule_group_observation(record, now);
}

void process_exact_probes_locked(SupervisorState& state, Record& record, Clock::time_point now) noexcept
{
  bool const short_due = record.short_stop_probe_due && now >= *record.short_stop_probe_due && short_stop_lane_active(record);
  for (MonitoredMember const kind : {MonitoredMember::Leader, MonitoredMember::Sentinel})
  {
    if (!member_present(record, kind) || member_observed(record, kind))
      continue;
    auto& member = member_monitor(record, kind);
    bool const readiness = member.readiness_due;
    bool const fallback = member.fallback_enabled && now >= member.next_exact_probe;
    bool const short_probe = short_due && kind == MonitoredMember::Leader;
    if (!readiness && !fallback && !short_probe)
      continue;
    bool detect_stops = short_probe || fallback;
    if (record.role == ProcessRoleV1::Mermaid && !record.startup_handshake_complete)
      detect_stops = false;
    exact_probe_locked(state, record, kind, now, readiness, fallback, short_probe, detect_stops);
  }

  if (short_stop_lane_active(record))
  {
    if (!record.short_stop_probe_due || short_due)
      record.short_stop_probe_due = now + kShortStopProbeInterval;
  }
  else
  {
    record.short_stop_probe_due.reset();
  }
}

void process_record_locked(SupervisorState& state, Record& record, Clock::time_point now) noexcept
{
  if (record.state == ProcessStateV1::Finished)
    return;
  if (!record.registered)
  {
    bool const was_reserved = record.state == ProcessStateV1::Reserved;
    if (commit_due_execution_deadline_locked(record, now) && was_reserved)
      finalize_locked(state, record, CleanupStateV1::NotRequired);
    return;
  }

  // Exact readiness is authoritative and is serviced before a deadline at the
  // same monitor turn can commit its reason. A fallback backend also receives
  // one nonblocking exact observation at the absolute boundary.
  process_exact_probes_locked(state, record, now);
  if (!record.execution_deadline_processed && record.policy.execution_deadline && now >= *record.policy.execution_deadline)
    exact_probe_direct_children_locked(state, record, now);

  if (record.startup_handshake_complete || record.reason)
  {
    if (record.leader_observed)
    {
      static_cast<void>(commit_reason_locked(record, TerminationReasonV1::NaturalExit));
      record.state = ProcessStateV1::Reaping;
      if (!record.stop_deadline)
        static_cast<void>(set_earlier_stop_deadline_locked(record, now + kDefaultCleanupBudget));
    }
    else if (record.sentinel > 1 && record.sentinel_observed && !record.reason)
    {
      static_cast<void>(commit_reason_locked(record, TerminationReasonV1::ProtocolFailure));
    }
  }

  static_cast<void>(commit_due_execution_deadline_locked(record, now));

  if (record.leader_observed && !record.pre_kill_group_observation_started && !record.post_kill_group_observation_started)
  {
    record.pre_kill_group_observation_started = true;
    process_natural_group_observation_locked(state, record, now);
    if (record.state == ProcessStateV1::Finished)
      return;
  }

  if (record.reason && !record.stop_deadline)
    static_cast<void>(set_earlier_stop_deadline_locked(record, now + kDefaultCleanupBudget));
  bool const natural_awaiting_quiet = record.reason == TerminationReasonV1::NaturalExit && !record.term_sent && record.quiet_group_observations > 0;
  if (record.reason && !natural_awaiting_quiet)
    begin_stop_locked(record, now);

  if (record.kill_due && now >= *record.kill_due && !record.kill_sent)
  {
    static_cast<void>(signal_verified_group(record, SIGKILL));
    record.kill_sent = true;
    record.post_kill_group_observation_started = true;
    record.quiet_group_observations = 0;
    schedule_group_observation(record, now);
  }

  if (record.group_observation_due && now >= *record.group_observation_due)
  {
    if (record.post_kill_group_observation_started)
      process_post_kill_group_observation_locked(state, record, now);
    else
    {
      exact_probe_direct_children_locked(state, record, now);
      process_natural_group_observation_locked(state, record, now);
    }
    if (record.state == ProcessStateV1::Finished)
      return;
  }

  if (record.stop_deadline && now >= *record.stop_deadline)
  {
    if (!record.kill_sent)
    {
      static_cast<void>(signal_verified_group(record, SIGKILL));
      record.kill_sent = true;
    }
    exact_probe_direct_children_locked(state, record, now);
    if (record.quiet_group_observations >= 2)
    {
      if (record.leader_observed)
        static_cast<void>(reap_member(record.leader, true, record.leader_reaped, record));
      if (record.sentinel > 1 && record.sentinel_observed)
        static_cast<void>(reap_member(record.sentinel, true, record.sentinel_reaped, record));
    }
    bool const reaped = record.leader_reaped && (record.sentinel <= 1 || record.sentinel_reaped);
    finalize_locked(state, record, reaped && !record.cleanup_failed ? CleanupStateV1::Complete : CleanupStateV1::Incomplete);
  }
}

void degrade_all_watches_locked(SupervisorState& state, Clock::time_point now) noexcept
{
  for (auto& [identity, record_pointer] : state.records)
  {
    static_cast<void>(identity);
    auto& record = *record_pointer;
    if (!record.registered || record.state == ProcessStateV1::Finished)
      continue;
    for (MonitoredMember const kind : {MonitoredMember::Leader, MonitoredMember::Sentinel})
    {
      if (!member_present(record, kind) || member_observed(record, kind))
        continue;
      auto& member = member_monitor(record, kind);
      if (member.watch)
        enable_fallback(member, now, true);
    }
  }
}

void apply_poll_events_locked(SupervisorState& state, PollBatch const& batch, Clock::time_point now) noexcept
{
  if (batch.error_number != 0)
    degrade_all_watches_locked(state, now);
  for (std::size_t index = 1; index < batch.count; ++index)
  {
    short const events = batch.descriptors[index].revents;
    if (events == 0)
      continue;
    auto const& registration = batch.registrations[index];
    auto found = state.records.find(registration.record_alias);
    bool valid = found != state.records.end() && found->second->registered && found->second->state != ProcessStateV1::Finished &&
                 member_present(*found->second, registration.member);
    if (valid)
    {
      auto const& member = member_monitor(*found->second, registration.member);
      valid = member.generation == registration.generation && member.watch.get() == registration.watch.get();
    }
    if (!valid)
    {
      state.monitor_telemetry->stale_events.fetch_add(1, std::memory_order_relaxed);
      if ((events & POLLNVAL) != 0)
        state.monitor_telemetry->pollnval_events.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    auto& member = member_monitor(*found->second, registration.member);
    if ((events & POLLNVAL) != 0)
    {
      state.monitor_telemetry->pollnval_events.fetch_add(1, std::memory_order_relaxed);
      enable_fallback(member, now, true);
      continue;
    }
    if ((events & (POLLIN | POLLHUP | POLLERR)) != 0)
    {
      member.readiness_due = true;
      member.watch.reset();
      member.fallback_enabled = false;
    }
  }
}

std::optional<Clock::time_point> nearest_deadline_locked(SupervisorState const& state) noexcept
{
  std::optional<Clock::time_point> nearest;
  for (auto const& [identity, record_pointer] : state.records)
  {
    static_cast<void>(identity);
    auto const& record = *record_pointer;
    if (record.state == ProcessStateV1::Finished)
      continue;
    if (!record.execution_deadline_processed)
      set_nearest(nearest, record.policy.execution_deadline);
    if (!record.registered)
      continue;
    for (MonitoredMember const kind : {MonitoredMember::Leader, MonitoredMember::Sentinel})
    {
      if (!member_present(record, kind) || member_observed(record, kind))
        continue;
      auto const& member = member_monitor(record, kind);
      if (member.fallback_enabled)
        set_nearest(nearest, member.next_exact_probe);
    }
    set_nearest(nearest, record.short_stop_probe_due);
    if (!record.kill_sent)
      set_nearest(nearest, record.kill_due);
    set_nearest(nearest, record.group_observation_due);
    set_nearest(nearest, record.stop_deadline);
  }
  return nearest;
}

PollBatch build_poll_batch_locked(SupervisorState const& state) noexcept
{
  PollBatch batch;
  batch.deadline = nearest_deadline_locked(state);
  if (!state.monitor_wake)
    return batch;
  batch.descriptors[0] = pollfd{.fd = state.monitor_wake->poll_descriptor(), .events = POLLIN, .revents = 0};
  batch.registrations[0].wake = state.monitor_wake;
  batch.count = 1;

  for (auto const& [identity, record_pointer] : state.records)
  {
    auto const& record = *record_pointer;
    if (!record.registered || record.state == ProcessStateV1::Finished)
      continue;
    for (MonitoredMember const kind : {MonitoredMember::Leader, MonitoredMember::Sentinel})
    {
      auto const& member = member_monitor(record, kind);
      if (!member_present(record, kind) || member_observed(record, kind) || !member.watch || batch.count == batch.descriptors.size())
        continue;
      auto const index = batch.count++;
      batch.descriptors[index] = pollfd{.fd = member.watch->descriptor.get(), .events = POLLIN, .revents = 0};
      batch.registrations[index] =
          PollRegistration{.wake = {}, .watch = member.watch, .record_alias = identity, .generation = member.generation, .member = kind};
    }
  }
  return batch;
}

int poll_timeout(std::optional<Clock::time_point> const& deadline) noexcept
{
  if (!deadline)
    return -1;
  auto const now = Clock::now();
  if (*deadline <= now)
    return 0;
  auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
  if (remaining >= std::chrono::milliseconds(INT_MAX))
    return INT_MAX;
  return std::max(1, static_cast<int>(remaining.count()));
}

void poll_batch(PollBatch& batch, std::shared_ptr<MonitorTelemetry> const& telemetry, std::atomic<std::uint64_t>& interruptions_for_test) noexcept
{
  if (batch.count == 0)
    return;
  update_peak(telemetry->poll_peak_entries, batch.count);
  while (true)
  {
    auto pending_interruption = interruptions_for_test.load(std::memory_order_relaxed);
    while (pending_interruption > 0 && !interruptions_for_test.compare_exchange_weak(pending_interruption, pending_interruption - 1, std::memory_order_relaxed))
    {
    }
    if (pending_interruption > 0)
    {
      telemetry->poll_interruptions.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    telemetry->poll_calls.fetch_add(1, std::memory_order_relaxed);
    int const result = ::poll(batch.descriptors.data(), batch.count, poll_timeout(batch.deadline));
    if (result > 0)
      return;
    if (result == 0)
    {
      telemetry->poll_timeouts.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (errno == EINTR)
    {
      telemetry->poll_interruptions.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    batch.error_number = errno == 0 ? EIO : errno;
    return;
  }
}

void invoke_poll_snapshot_hook(std::shared_ptr<AfterForkBeforeReleaseHook> const& hook) noexcept
{
  if (!hook || !*hook)
    return;
  try
  {
    (*hook)();
  }
  catch (...)
  {
  }
}

}  // namespace

bool observe_member(pid_t process, bool& observed, Record& record, bool leader, bool detect_stops) noexcept
{
  if (process <= 1 || observed)
    return true;
  siginfo_t information{};
  int options = WEXITED | WNOHANG | WNOWAIT;
  if (detect_stops)
    options |= WSTOPPED;
  if (waitid_retry(process, &information, options) != 0)
  {
    if (errno == ECHILD)
    {
      record.cleanup_failed = true;
      observed = true;
      return false;
    }
    record.cleanup_failed = true;
    return false;
  }
  if (information.si_pid != process)
    return true;
  if (information.si_code == CLD_STOPPED)
  {
    static_cast<void>(commit_reason_locked(record, leader ? TerminationReasonV1::UnsupportedSuspension : TerminationReasonV1::ProtocolFailure));
    return true;
  }
  observed = true;
  if (leader)
    observe_status(record, information);
  return true;
}

void observe_status(Record& record, siginfo_t const& information) noexcept
{
  if (record.exit_kind == ExitKindV1::LaunchError)
    return;
  if (information.si_code == CLD_EXITED)
  {
    record.exit_kind = ExitKindV1::Exited;
    record.exit_code = information.si_status;
    record.has_exit_code = true;
  }
  else if (information.si_code == CLD_KILLED || information.si_code == CLD_DUMPED)
  {
    record.exit_kind = ExitKindV1::Signaled;
    record.signal_number = information.si_status;
    record.has_signal_number = true;
  }
  else
  {
    record.cleanup_failed = true;
  }
}

bool reap_member(pid_t process, bool observed, bool& reaped, Record& record) noexcept
{
  if (process <= 1 || reaped)
    return true;
  if (!observed)
    return false;
  int status = 0;
  auto const waited = waitpid_retry(process, &status, WNOHANG);
  if (waited == process)
  {
    reaped = true;
    return true;
  }
  record.cleanup_failed = true;
  if (waited < 0 && errno == ECHILD)
    reaped = true;
  return false;
}

bool signal_verified_group(Record& record, int signal_number) noexcept
{
  if (!record.registered || !record.group_verified || record.leader <= 1 || record.leader_reaped)
  {
    record.cleanup_failed = true;
    return false;
  }
  if (::kill(-record.leader, signal_number) == 0 || errno == ESRCH)
    return true;
  record.cleanup_failed = true;
  return false;
}

std::optional<bool> verified_group_has_live_member(pid_t group) noexcept
{
#if defined(__linux__)
  int const proc_descriptor = ::open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
  if (proc_descriptor < 0)
    return std::nullopt;
  DIR* directory = ::fdopendir(proc_descriptor);
  if (directory == nullptr)
  {
    static_cast<void>(::close(proc_descriptor));
    return std::nullopt;
  }

  bool live = false;
  bool failed = false;
  while (true)
  {
    errno = 0;
    dirent* entry = ::readdir(directory);
    if (entry == nullptr)
    {
      failed = errno != 0;
      break;
    }
    char const* name = entry->d_name;
    if (*name == '\0')
      continue;
    bool numeric = true;
    for (char const* character = name; *character != '\0'; ++character)
      numeric = numeric && *character >= '0' && *character <= '9';
    if (!numeric)
      continue;

    int const process_directory = ::openat(proc_descriptor, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NONBLOCK);
    if (process_directory < 0)
    {
      if (disappearance_error(errno))
        continue;
      failed = true;
      break;
    }
    int const stat_descriptor = ::openat(process_directory, "stat", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    int const stat_open_error = errno;
    static_cast<void>(::close(process_directory));
    if (stat_descriptor < 0)
    {
      if (disappearance_error(stat_open_error))
        continue;
      failed = true;
      break;
    }

    std::array<char, 4096> buffer{};
    ssize_t count = -1;
    do
      count = ::read(stat_descriptor, buffer.data(), buffer.size() - 1);
    while (count < 0 && errno == EINTR);
    int const read_error = errno;
    static_cast<void>(::close(stat_descriptor));
    if (count < 0)
    {
      if (disappearance_error(read_error))
        continue;
      failed = true;
      break;
    }
    if (count == 0)
    {
      failed = true;
      break;
    }

    buffer[static_cast<std::size_t>(count)] = '\0';
    char* command_end = nullptr;
    for (char* current = buffer.data(); current[0] != '\0' && current[1] != '\0'; ++current)
    {
      if (current[0] == ')' && current[1] == ' ')
        command_end = current;
    }
    if (command_end == nullptr)
    {
      failed = true;
      break;
    }
    char process_state = '\0';
    long long parent = 0;
    long long process_group = 0;
    if (std::sscanf(command_end + 2, "%c %lld %lld", &process_state, &parent, &process_group) != 3 || !valid_linux_process_state(process_state))
    {
      failed = true;
      break;
    }
    if (process_group == group && process_state != 'Z' && process_state != 'X' && process_state != 'x')
    {
      live = true;
      break;
    }
  }
  if (::closedir(directory) != 0 && !live)
    failed = true;
  return live ? std::optional<bool>(true) : (failed ? std::nullopt : std::optional<bool>(false));
#else
  static_cast<void>(group);
  return std::nullopt;
#endif
}

void register_record_members_locked(SupervisorState& state, Record& record, Clock::time_point now) noexcept
{
  attach_member_locked(state, record, MonitoredMember::Leader, now);
  if (record.sentinel > 1)
    attach_member_locked(state, record, MonitoredMember::Sentinel, now);
  if (short_stop_lane_active(record))
    record.short_stop_probe_due = now + kShortStopProbeInterval;
}

void reset_record_probe_schedule_locked(Record& record, Clock::time_point now) noexcept
{
  for (MonitoredMember const kind : {MonitoredMember::Leader, MonitoredMember::Sentinel})
  {
    if (!member_present(record, kind) || member_observed(record, kind))
      continue;
    auto& member = member_monitor(record, kind);
    if (member.fallback_enabled)
    {
      member.fallback_interval = kFallbackInitialInterval;
      member.next_exact_probe = now + kFallbackInitialInterval;
      ++member.reset_count;
    }
  }
  if (short_stop_lane_active(record))
    record.short_stop_probe_due = now + kShortStopProbeInterval;
}

void detach_record_watches_locked(Record& record) noexcept
{
  detach_member(record.leader_monitor);
  detach_member(record.sentinel_monitor);
  record.short_stop_probe_due.reset();
  record.group_observation_due.reset();
}

void begin_stop_locked(Record& record, Clock::time_point now)
{
  if (!record.term_sent)
  {
    if (record.reason == TerminationReasonV1::UnsupportedSuspension)
      static_cast<void>(signal_verified_group(record, SIGCONT));
    static_cast<void>(signal_verified_group(record, SIGTERM));
    record.term_sent = true;
    auto due = now + record.policy.termination_grace;
    if (record.stop_deadline && *record.stop_deadline < due)
      due = *record.stop_deadline;
    record.kill_due = due;
  }
  if (record.state != ProcessStateV1::Reaping)
    record.state = ProcessStateV1::StopRequested;
}

void monitor_main(std::shared_ptr<SupervisorState> state) noexcept
{
  Debug(NAMESPACE_DEBUG::init_thread("monitor"));

  auto const current_threads = state->monitor_telemetry->current_monitor_threads.fetch_add(1, std::memory_order_relaxed) + 1;
  update_peak(state->monitor_telemetry->peak_monitor_threads, current_threads);
  struct ThreadCounter final
  {
    std::shared_ptr<MonitorTelemetry> telemetry;
    ~ThreadCounter() { telemetry->current_monitor_threads.fetch_sub(1, std::memory_order_relaxed); }

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  } thread_counter{state->monitor_telemetry};

  PollBatch completed;
  bool have_completed_poll = false;
  while (true)
  {
    if (have_completed_poll && completed.count > 0 && (completed.descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
      drain_monitor_wake(completed.registrations[0].wake);

    PollBatch next;
    std::shared_ptr<AfterForkBeforeReleaseHook> hook;
    bool stop = false;
    {
      std::lock_guard lock(state->mutex);
      auto const now = Clock::now();
      if (have_completed_poll)
        apply_poll_events_locked(*state, completed, now);
      for (auto& [identity, record] : state->records)
      {
        static_cast<void>(identity);
        process_record_locked(*state, *record, now);
      }
      state->monitor_telemetry->monitor_cycles.fetch_add(1, std::memory_order_relaxed);
      stop = state->stop_monitor;
      if (!stop)
      {
        next = build_poll_batch_locked(*state);
        hook = state->after_poll_snapshot_for_test;
      }
    }
    state->changed.notify_all();
    if (stop)
      break;

    completed = std::move(next);
    have_completed_poll = true;
    invoke_poll_snapshot_hook(hook);
    poll_batch(completed, state->monitor_telemetry, state->poll_interruptions_for_test);
  }
  state->changed.notify_all();
}

ava::core::VoidResult ensure_monitor_started(std::shared_ptr<SupervisorState> const& state)
{
  std::lock_guard lifecycle_lock(state->monitor_lifecycle_mutex);
  {
    std::lock_guard lock(state->mutex);
    if (state->stop_monitor || state->monitor_joined)
      return std::unexpected(process_error(ava::core::ErrorCategory::Io, "process supervisor shutdown won the launch race"));
    if (state->monitor.joinable())
      return {};
  }

  auto wake = make_monitor_wake(state->monitor_telemetry);
  if (!wake)
    return std::unexpected(std::move(wake.error()));

  try
  {
    std::lock_guard lock(state->mutex);
    if (state->stop_monitor || state->monitor_joined)
      return std::unexpected(process_error(ava::core::ErrorCategory::Io, "process supervisor shutdown won the launch race"));
    if (state->monitor.joinable())
      return {};
    state->monitor_wake = *wake;
    utils::Signal::BlockGuard scoped_signal_blocks{SIGINT, SIGTERM, SIGHUP};
    Dout(dc::notice, "Creating supervisor monitor thread.");
    state->monitor = std::thread(monitor_main, state);
    state->monitor_started = true;
  }
  catch (std::exception const& error)
  {
    std::lock_guard lock(state->mutex);
    state->monitor_wake.reset();
    return std::unexpected(process_error(ava::core::ErrorCategory::Io, "failed to start the process monitor").with_context("cause", error.what()));
  }
  catch (...)
  {
    std::lock_guard lock(state->mutex);
    state->monitor_wake.reset();
    return std::unexpected(process_error(ava::core::ErrorCategory::Io, "failed to start the process monitor"));
  }
  notify_monitor_state(state);
  return {};
}

void join_monitor(std::shared_ptr<SupervisorState> const& state) noexcept
{
  std::lock_guard lifecycle_lock(state->monitor_lifecycle_mutex);
  if (state->monitor.joinable())
    state->monitor.join();
  std::lock_guard lock(state->mutex);
  state->monitor_wake.reset();
  state->monitor_joined = true;
}

#endif

}  // namespace ava::process::detail
