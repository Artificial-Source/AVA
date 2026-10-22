#include "sys.h"
#include "tests/support/process_test_support.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/process/environment.h"
#include "ava/process/supervisor.h"
#include "ava/core/AnchorSet.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef AVA_FAKE_PROCESS_CHILD_PATH
#define AVA_FAKE_PROCESS_CHILD_PATH ""
#endif

namespace {

using namespace std::chrono_literals;

#if !defined(_WIN32)

ava::process::OwnerPathV1 application_owner()
{
  auto owner = ava::process::OwnerPathV1::application();
  if (!owner)
    throw std::runtime_error(owner.error().format());
  return std::move(*owner);
}

ava::process::OwnerPathV1 operation_owner(ava::process::OwnerPathV1 const& application)
{
  auto owner = application.operation();
  if (!owner)
    throw std::runtime_error(owner.error().format());
  return std::move(*owner);
}

ava::process::AnchoredWorkingDirectoryV1 anchored_cwd(std::filesystem::path const& cwd)
{
  auto anchors = ava::core::AnchorSet::open({"/"});
  if (!anchors)
    throw std::runtime_error(anchors.error().format());
  auto capability = ava::process::mint_anchored_working_directory(std::move(*anchors), cwd);
  if (!capability)
    throw std::runtime_error(capability.error().format());
  return std::move(*capability);
}

std::vector<ava::process::EnvironmentVariableV1> bash_variables(std::string cwd = "/")
{
  return {{"LANG", "C.UTF-8"},
          {"LC_ALL", "C.UTF-8"},
          {"LC_CTYPE", "C.UTF-8"},
          {"TZ", "UTC"},
          {"USER", "ava-test"},
          {"LOGNAME", "ava-test"},
          {"PWD", std::move(cwd)},
          {"PATH", "/usr/bin:/bin"},
          {"HOME", "/tmp/ava-process-home"},
          {"XDG_CONFIG_HOME", "/tmp/ava-process-xdg-config"},
          {"XDG_CACHE_HOME", "/tmp/ava-process-xdg-cache"},
          {"XDG_DATA_HOME", "/tmp/ava-process-xdg-data"},
          {"XDG_STATE_HOME", "/tmp/ava-process-xdg-state"},
          {"TMPDIR", "/tmp/ava-process-tmp"}};
}

ava::process::SecureAdoptionSpecV1 bash_spec(ava::process::BashContainmentHandshakeV1 containment = ava::process::BashContainmentHandshakeV1::None,
                                             std::vector<std::string> argv = {AVA_FAKE_PROCESS_CHILD_PATH, "normal"}, std::string cwd = "/")
{
  auto environment = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, cwd, bash_variables(cwd));
  if (!environment)
    throw std::runtime_error(environment.error().format());
  auto cwd_capability = anchored_cwd(cwd);
  return {.environment = std::move(*environment),
          .argv = std::move(argv),
          .cwd = std::move(cwd),
          .anchored_cwd = std::move(cwd_capability),
          .bash_containment = containment};
}

ava::process::SecureAdoptionSpecV1 mermaid_spec(std::vector<std::string> argv = {AVA_FAKE_PROCESS_CHILD_PATH, "normal"})
{
  auto environment = ava::process::make_mermaid_environment_v1();
  if (!environment)
    throw std::runtime_error(environment.error().format());
  return {.environment = std::move(*environment),
          .argv = std::move(argv),
          .cwd = "/",
          .anchored_cwd = anchored_cwd("/"),
          .bash_containment = ava::process::BashContainmentHandshakeV1::None};
}

std::optional<ava::process::ExitStatusV1> wait_for(ava::process::Supervisor& supervisor, ava::process::ProcessHandle const& handle)
{
  auto status = supervisor.wait(handle, ava::tests::now_plus_seconds(4s));
  return status ? std::optional(*status) : std::nullopt;
}

bool read_exact_until(int descriptor, void* buffer, std::size_t size, std::chrono::steady_clock::time_point deadline) noexcept
{
  auto* bytes = static_cast<unsigned char*>(buffer);
  std::size_t offset = 0;
  while (offset < size && std::chrono::steady_clock::now() < deadline)
  {
    pollfd item{.fd = descriptor, .events = POLLIN, .revents = 0};
    int result = -1;
    do
      result = ::poll(&item, 1, 50);
    while (result < 0 && errno == EINTR && std::chrono::steady_clock::now() < deadline);
    if (result <= 0)
      continue;
    auto const count = ::read(descriptor, bytes + offset, size - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    offset += static_cast<std::size_t>(count);
  }
  return offset == size;
}

bool read_byte(int descriptor, char expected, std::chrono::steady_clock::time_point deadline) noexcept
{
  while (std::chrono::steady_clock::now() < deadline)
  {
    pollfd item{.fd = descriptor, .events = POLLIN, .revents = 0};
    int const result = ::poll(&item, 1, 50);
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      continue;
    char value = '\0';
    ssize_t count = -1;
    do
      count = ::read(descriptor, &value, 1);
    while (count < 0 && errno == EINTR);
    return count == 1 && value == expected;
  }
  return false;
}

bool read_eof(int descriptor, std::chrono::steady_clock::time_point deadline) noexcept
{
  int flags = ::fcntl(descriptor, F_GETFL);
  if (flags >= 0)
    static_cast<void>(::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK));
  std::array<char, 64> buffer{};
  while (std::chrono::steady_clock::now() < deadline)
  {
    auto const count = ::read(descriptor, buffer.data(), buffer.size());
    if (count == 0)
      return true;
    if (count > 0 || errno == EINTR)
      continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      return false;
    pollfd item{.fd = descriptor, .events = POLLIN, .revents = 0};
    static_cast<void>(::poll(&item, 1, 50));
  }
  return false;
}

bool one_launch_error(ava::process::ProcessSnapshotRecordV1 const& record, ava::process::TerminationReasonV1 reason)
{
  return record.reason == reason && record.exit_kind == ava::process::ExitKindV1::LaunchError && record.cleanup == ava::process::CleanupStateV1::Complete &&
         record.settlement_count == 1;
}

// These test-only atfork globals exist only in the isolated regression subprocess.
sig_atomic_t volatile leader_barrier_armed = 0;
int leader_pid_read_descriptor = -1;
int leader_pid_write_descriptor = -1;
int leader_release_read_descriptor = -1;
int leader_release_write_descriptor = -1;

void disarm_leader_barrier_in_parent() noexcept
{
  leader_barrier_armed = 0;
}

void hold_leader_before_child_setpgid() noexcept
{
  if (leader_barrier_armed == 0)
    return;

  static_cast<void>(::close(leader_pid_read_descriptor));
  static_cast<void>(::close(leader_release_write_descriptor));
  pid_t const process = ::getpid();
  ssize_t count = -1;
  do
    count = ::write(leader_pid_write_descriptor, &process, sizeof(process));
  while (count < 0 && errno == EINTR);
  static_cast<void>(::close(leader_pid_write_descriptor));
  if (count != static_cast<ssize_t>(sizeof(process)))
    _exit(125);

  char release = '\0';
  do
    count = ::read(leader_release_read_descriptor, &release, 1);
  while (count < 0 && errno == EINTR);
  static_cast<void>(::close(leader_release_read_descriptor));
  if (count != 1 || release != 'G')
    _exit(125);
}

struct LeaderGroupRaceReport
{
  std::uint8_t setup = 0;
  std::uint8_t leader_reported = 0;
  std::uint8_t parent_group_created = 0;
  std::uint8_t sentinel_forked = 0;
  std::uint8_t adopted = 0;
  std::uint8_t natural_exit = 0;
  std::array<char, 768> diagnostic{};
};

LeaderGroupRaceReport run_leader_group_race_fixture()
{
  LeaderGroupRaceReport report;
  std::array<int, 2> leader_pid{-1, -1};
  std::array<int, 2> leader_release{-1, -1};
  bool const leader_pid_ready = ::pipe2(leader_pid.data(), O_CLOEXEC) == 0;
  bool const leader_release_ready = leader_pid_ready && ::pipe2(leader_release.data(), O_CLOEXEC) == 0;
  if (!leader_release_ready)
  {
    auto const message = std::string("fixture pipe setup failed: ") + std::to_string(errno);
    std::copy_n(message.data(), std::min(message.size(), report.diagnostic.size() - 1), report.diagnostic.data());
    if (leader_pid_ready)
    {
      static_cast<void>(::close(leader_pid[0]));
      static_cast<void>(::close(leader_pid[1]));
    }
    return report;
  }

  leader_pid_read_descriptor = leader_pid[0];
  leader_pid_write_descriptor = leader_pid[1];
  leader_release_read_descriptor = leader_release[0];
  leader_release_write_descriptor = leader_release[1];
  leader_barrier_armed = 1;
  int const atfork_error = ::pthread_atfork(nullptr, disarm_leader_barrier_in_parent, hold_leader_before_child_setpgid);
  if (atfork_error != 0)
  {
    auto const message = std::string("pthread_atfork setup failed: ") + std::to_string(atfork_error);
    std::copy_n(message.data(), std::min(message.size(), report.diagnostic.size() - 1), report.diagnostic.data());
    static_cast<void>(::close(leader_pid[0]));
    static_cast<void>(::close(leader_pid[1]));
    static_cast<void>(::close(leader_release[0]));
    static_cast<void>(::close(leader_release[1]));
    return report;
  }
  report.setup = 1;

  auto application = application_owner();
  ava::process::Supervisor supervisor;
  int executable = ::open(AVA_FAKE_PROCESS_CHILD_PATH, O_RDONLY | O_CLOEXEC);
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  auto gate = reservation ? supervisor.begin_secure_adoption(std::move(*reservation), bash_spec())
                          : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(reservation.error()));
  auto branch = gate ? gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(gate.error()));
  if (branch && *branch == ava::process::AdoptionForkBranchV1::Child)
  {
    static_cast<void>(::close(leader_pid[0]));
    static_cast<void>(::close(leader_pid[1]));
    static_cast<void>(::close(leader_release[0]));
    static_cast<void>(::close(leader_release[1]));
    gate->child_exec_descriptor(executable);
  }

  leader_barrier_armed = 0;
  static_cast<void>(::close(leader_pid[1]));
  static_cast<void>(::close(leader_release[0]));
  pid_t held_leader = -1;
  report.leader_reported = read_exact_until(leader_pid[0], &held_leader, sizeof(held_leader), ava::tests::now_plus_seconds(2s));
  static_cast<void>(::close(leader_pid[0]));
  pid_t const observed_group = report.leader_reported != 0 ? ::getpgid(held_leader) : -1;
  report.parent_group_created = report.leader_reported != 0 && held_leader > 1 && observed_group == held_leader && observed_group != ::getpgrp();

  auto sentinel = gate ? gate->fork_sentinel() : ava::core::VoidResult(std::unexpected(gate.error()));
  report.sentinel_forked = sentinel.has_value();
  bool const released = write_all_to_descriptor_for_test(leader_release[1], "G", 1);
  static_cast<void>(::close(leader_release[1]));
  if (executable >= 0)
    static_cast<void>(::close(executable));
  auto adopted = gate ? supervisor.adopt(std::move(*gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(gate.error()));
  report.adopted = adopted.has_value();
  auto status = adopted ? wait_for(supervisor, *adopted) : std::nullopt;
  report.natural_exit = status && status->reason == ava::process::TerminationReasonV1::NaturalExit && status->cleanup == ava::process::CleanupStateV1::Complete;

  std::string diagnostic;
  if (!branch)
    diagnostic += "fork_leader: " + branch.error().format();
  if (!sentinel)
    diagnostic += (diagnostic.empty() ? "" : "; ") + std::string("fork_sentinel: ") + sentinel.error().format();
  if (!released)
    diagnostic += (diagnostic.empty() ? "" : "; ") + std::string("leader release pipe failed");
  if (!adopted)
    diagnostic += (diagnostic.empty() ? "" : "; ") + std::string("adopt: ") + adopted.error().format();
  std::copy_n(diagnostic.data(), std::min(diagnostic.size(), report.diagnostic.size() - 1), report.diagnostic.data());
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
  return report;
}

void test_parent_establishes_leader_group_before_sentinel_fork()
{
  std::array<int, 2> result_pipe{-1, -1};
  bool const pipe_ready = ::pipe2(result_pipe.data(), O_CLOEXEC) == 0;
  pid_t const fixture = pipe_ready ? ::fork() : -1;
  if (fixture == 0)
  {
    static_cast<void>(::close(result_pipe[0]));
    auto const report = run_leader_group_race_fixture();
    bool const written = write_all_to_descriptor_for_test(result_pipe[1], &report, sizeof(report));
    static_cast<void>(::close(result_pipe[1]));
    _exit(written ? 0 : 126);
  }
  ava::tests::ScopedChild fixture_owner(fixture);

  LeaderGroupRaceReport report;
  if (pipe_ready)
    static_cast<void>(::close(result_pipe[1]));
  bool const report_received = fixture > 1 && read_exact_until(result_pipe[0], &report, sizeof(report), ava::tests::now_plus_seconds(8s));
  if (pipe_ready)
    static_cast<void>(::close(result_pipe[0]));
  auto const status = fixture > 1 ? fixture_owner.wait_until(ava::tests::now_plus_seconds(10)) : std::nullopt;
  bool const reaped = status.has_value();
  int const fixture_status = status.value_or(0);
  bool const exact = pipe_ready && report_received && reaped && WIFEXITED(fixture_status) && WEXITSTATUS(fixture_status) == 0 && report.setup != 0 &&
                     report.leader_reported != 0 && report.parent_group_created != 0 && report.sentinel_forked != 0 && report.adopted != 0 &&
                     report.natural_exit != 0;
  std::string message = "fork_leader establishes the exact isolated leader group before a held leader can run setpgid, allowing sentinel join and adoption";
  if (!exact)
  {
    message += "; fixture setup=" + std::to_string(report.setup) + ", leader_reported=" + std::to_string(report.leader_reported) +
               ", parent_group_created=" + std::to_string(report.parent_group_created) + ", sentinel_forked=" + std::to_string(report.sentinel_forked) +
               ", adopted=" + std::to_string(report.adopted) + ", natural_exit=" + std::to_string(report.natural_exit);
    if (report.diagnostic.front() != '\0')
      message += "; " + std::string(report.diagnostic.data());
  }
  expect(exact, message);
}

void test_adoption_argv_rejected_before_fork()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  std::vector<std::vector<std::string>> invalid_arguments;
  invalid_arguments.emplace_back();
  invalid_arguments.push_back({""});
  invalid_arguments.push_back({AVA_FAKE_PROCESS_CHILD_PATH, std::string("bad\0argument", 12)});
  invalid_arguments.push_back({AVA_FAKE_PROCESS_CHILD_PATH, std::string(1024 * 1024, 'x')});
  invalid_arguments.emplace_back(257, "x");
  invalid_arguments.back().front() = AVA_FAKE_PROCESS_CHILD_PATH;

  bool rejected = true;
  for (auto& argv : invalid_arguments)
  {
    auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
    auto specification = bash_spec();
    specification.argv = std::move(argv);
    auto gate = reservation ? supervisor.begin_secure_adoption(std::move(*reservation), std::move(specification))
                            : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(reservation.error()));
    rejected = rejected && !gate && gate.error().format().find("argv") != std::string::npos;
  }

  auto snapshot = supervisor.snapshot();
  bool exact = rejected && !snapshot.monitor_started && snapshot.live_records == 0 && snapshot.records.size() == invalid_arguments.size();
  for (auto const& record : snapshot.records)
  {
    exact = exact && record.reason == ava::process::TerminationReasonV1::LaunchFailed && record.exit_kind == ava::process::ExitKindV1::LaunchError &&
            record.cleanup == ava::process::CleanupStateV1::NotRequired && record.settlement_count == 1;
  }
  expect(exact, "secure adoption rejects empty, NUL-bearing, aggregate-oversize, and over-count argv before any child fork");
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
}

void test_eof_child_stages_and_invalid_image()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  auto expect_last_launch_error = [](ava::process::ProcessSnapshotV1 const& snapshot, ava::process::TerminationReasonV1 reason, std::string const& label) {
    if (!ava::tests::require(!snapshot.records.empty(), label + ": terminal record exists"))
      return;
    auto const& record = snapshot.records.back();
    if (ava::tests::require(record.reason.has_value(), label + ": termination reason exists"))
      ava::tests::expect_eq(*record.reason, reason, label + ": exact termination reason");
    ava::tests::expect_eq(record.state, ava::process::ProcessStateV1::Finished, label + ": finished state");
    ava::tests::expect_eq(record.exit_kind, ava::process::ExitKindV1::LaunchError, label + ": exact exit kind");
    ava::tests::expect_eq(record.cleanup, ava::process::CleanupStateV1::Complete, label + ": complete cleanup");
    ava::tests::expect_eq(record.settlement_count, 1U, label + ": exactly one settlement");
  };

  auto eof_reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  auto eof_gate = eof_reservation ? supervisor.begin_secure_adoption(std::move(*eof_reservation), bash_spec())
                                  : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(eof_reservation.error()));
  auto eof_branch = eof_gate ? eof_gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(eof_gate.error()));
  if (eof_branch && *eof_branch == ava::process::AdoptionForkBranchV1::Child)
    _exit(0);
  auto eof_adopted = eof_gate ? supervisor.adopt(std::move(*eof_gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(eof_gate.error()));
  auto const eof_snapshot = supervisor.snapshot();
  ava::tests::expect_all("EOF before exec attempt is a single exact launch failure",
                         {{"forked parent", eof_branch && *eof_branch == ava::process::AdoptionForkBranchV1::Parent},
                          {"adoption rejected", !eof_adopted},
                          {"one record", eof_snapshot.records.size() == 1}});
  expect_last_launch_error(eof_snapshot, ava::process::TerminationReasonV1::LaunchFailed, "EOF before exec attempt");

  std::size_t expected_records = 1;
  for (auto const& [stage, name] : {std::pair{ava::process::AdoptionChildFailureStageV1::Streams, "streams"},
                                    std::pair{ava::process::AdoptionChildFailureStageV1::WorkingDirectory, "working directory"},
                                    std::pair{ava::process::AdoptionChildFailureStageV1::Containment, "containment"},
                                    std::pair{ava::process::AdoptionChildFailureStageV1::DescriptorValidation, "descriptor validation"}})
  {
    auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
    auto gate = reservation ? supervisor.begin_secure_adoption(std::move(*reservation), bash_spec())
                            : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(reservation.error()));
    auto branch = gate ? gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(gate.error()));
    if (branch && *branch == ava::process::AdoptionForkBranchV1::Child)
      gate->child_launch_failed(stage, EACCES);
    auto adopted = gate ? supervisor.adopt(std::move(*gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(gate.error()));
    auto const stage_snapshot = supervisor.snapshot();
    std::string label = "child setup stage: ";
    label += name;
    std::string message = label;
    message += " rejects adoption with exactly one complete launch-error settlement";
    ava::tests::expect_all(message,
                           {{"reservation", reservation.has_value()},
                            {"gate", gate.has_value()},
                            {"parent branch", branch && *branch == ava::process::AdoptionForkBranchV1::Parent},
                            {"adoption rejected", !adopted},
                            {"one additional record", stage_snapshot.records.size() == ++expected_records}});
    expect_last_launch_error(stage_snapshot, ava::process::TerminationReasonV1::LaunchFailed, label);
  }

  auto const root = create_empty_root("process-adoption-invalid-image");
  auto const invalid_path = root / "invalid-image";
  ava::tests::write_test_file(invalid_path, "not an executable image\n", 0700);
  int invalid_descriptor = ::open(invalid_path.c_str(), O_RDONLY | O_CLOEXEC);
  auto invalid_reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  auto invalid_gate = invalid_reservation ? supervisor.begin_secure_adoption(std::move(*invalid_reservation),
                                                                             bash_spec(ava::process::BashContainmentHandshakeV1::None, {invalid_path.string()}))
                                          : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(invalid_reservation.error()));
  auto invalid_branch =
      invalid_gate ? invalid_gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(invalid_gate.error()));
  if (invalid_branch && *invalid_branch == ava::process::AdoptionForkBranchV1::Child)
    invalid_gate->child_exec_descriptor(invalid_descriptor);
  if (invalid_descriptor >= 0)
    static_cast<void>(::close(invalid_descriptor));
  auto invalid_adopted =
      invalid_gate ? supervisor.adopt(std::move(*invalid_gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(invalid_gate.error()));

  auto snapshot = supervisor.snapshot();
  bool const invalid_error_content_free = !invalid_adopted && invalid_adopted.error().format().find(invalid_path.string()) == std::string::npos;
  ava::tests::expect_all("returned exec syscall is content-free ExecFailed, distinct from setup-stage LaunchFailed",
                         {{"six records", snapshot.records.size() == 6},
                          {"invalid image opened", invalid_descriptor >= 0},
                          {"parent branch", invalid_branch && *invalid_branch == ava::process::AdoptionForkBranchV1::Parent},
                          {"content-free rejection", invalid_error_content_free}});
  expect_last_launch_error(snapshot, ava::process::TerminationReasonV1::ExecFailed, "invalid executable image");
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
}

void test_required_containment_exact_environment_sentinel_and_confirmation()
{
  ScopedEnvVar ambient("AVA_PROCESS_AMBIENT_CANARY", "must-not-leak");
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  auto const child_cwd = create_empty_root("process-adoption-cwd-before-containment");
  auto const cwd_marker = child_cwd / "cwd-ready";
  {
    std::ofstream marker(cwd_marker, std::ios::binary | std::ios::trunc);
    marker << "ready\n";
  }
  std::array<int, 2> ready{-1, -1};
  std::array<int, 2> proceed{-1, -1};
  std::array<int, 2> output{-1, -1};
  bool const pipes_ready = ::pipe2(ready.data(), O_CLOEXEC) == 0 && ::pipe2(proceed.data(), O_CLOEXEC) == 0 && ::pipe2(output.data(), O_CLOEXEC) == 0;
  int executable = ::open(AVA_FAKE_PROCESS_CHILD_PATH, O_RDONLY | O_CLOEXEC);

  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  auto gate = reservation
                  ? supervisor.begin_secure_adoption(std::move(*reservation),
                                                     bash_spec(ava::process::BashContainmentHandshakeV1::Required,
                                                               {AVA_FAKE_PROCESS_CHILD_PATH, "adoption-environment", child_cwd.string()}, child_cwd.string()))
                  : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(reservation.error()));
  auto branch = gate ? gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(gate.error()));
  if (branch && *branch == ava::process::AdoptionForkBranchV1::Child)
  {
    int const marker = ::open("cwd-ready", O_RDONLY | O_CLOEXEC);
    if (marker < 0)
      gate->child_launch_failed(ava::process::AdoptionChildFailureStageV1::WorkingDirectory, errno);
    static_cast<void>(::close(marker));
    static_cast<void>(::close(ready[0]));
    static_cast<void>(::close(proceed[1]));
    static_cast<void>(::close(output[0]));
    if (::dup2(output[1], STDOUT_FILENO) < 0 || !write_all_to_descriptor_for_test(ready[1], "R", 1))
      gate->child_launch_failed(ava::process::AdoptionChildFailureStageV1::Streams, errno);
    char release = '\0';
    ssize_t count = -1;
    do
      count = ::read(proceed[0], &release, 1);
    while (count < 0 && errno == EINTR);
    if (count != 1 || release != 'C')
      gate->child_launch_failed(ava::process::AdoptionChildFailureStageV1::Containment, EIO);
    gate->child_bash_containment_applied();
    gate->child_exec_descriptor(executable);
  }
  auto sentinel = gate ? gate->fork_sentinel() : ava::core::VoidResult(std::unexpected(gate.error()));
  static_cast<void>(::close(ready[1]));
  static_cast<void>(::close(proceed[0]));
  static_cast<void>(::close(output[1]));
  if (executable >= 0)
    static_cast<void>(::close(executable));
  int output_flags = ::fcntl(output[0], F_GETFL);
  if (output_flags >= 0)
    static_cast<void>(::fcntl(output[0], F_SETFL, output_flags | O_NONBLOCK));

  std::mutex result_mutex;
  bool adoption_returned = false;
  std::optional<ava::core::Result<ava::process::ProcessHandle>> adopted;
  std::thread adopter([&] {
    auto result = supervisor.adopt(std::move(*gate));
    std::lock_guard lock(result_mutex);
    adopted.emplace(std::move(result));
    adoption_returned = true;
  });
  bool const child_released = pipes_ready && read_byte(ready[0], 'R', ava::tests::now_plus_seconds(2s));
  bool returned_before_confirmation = false;
  {
    std::lock_guard lock(result_mutex);
    returned_before_confirmation = adoption_returned;
  }
  bool const continued = child_released && write_all_to_descriptor_for_test(proceed[1], "C", 1);
  static_cast<void>(::close(proceed[1]));
  std::string environment_output;
  bool const exact_environment = ava::tests::read_until(output[0], environment_output, "CLEAN\n", ava::tests::now_plus_seconds(3s));
  adopter.join();

  auto status = adopted && *adopted ? wait_for(supervisor, **adopted) : std::nullopt;
  bool const exact = pipes_ready && branch && *branch == ava::process::AdoptionForkBranchV1::Parent && sentinel && child_released &&
                     !returned_before_confirmation && continued && exact_environment && adopted && *adopted && status &&
                     status->reason == ava::process::TerminationReasonV1::NaturalExit && status->cleanup == ava::process::CleanupStateV1::Complete;
  std::string message = "required Bash containment, exact environment, sentinel hygiene, and CLOEXEC confirmation complete before adopt returns";
  if (!exact)
  {
    if (!branch)
      message += "; fork_leader: " + branch.error().format();
    if (!sentinel)
      message += "; fork_sentinel: " + sentinel.error().format();
    if (adopted && !*adopted)
      message += "; adopt: " + adopted->error().format();
  }
  expect(exact, message);
  static_cast<void>(::close(ready[0]));
  static_cast<void>(::close(output[0]));
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
}

void test_containment_order_and_non_bash_rejection()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  int executable = ::open(AVA_FAKE_PROCESS_CHILD_PATH, O_RDONLY | O_CLOEXEC);

  auto skipped_reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  auto skipped_gate = skipped_reservation
                          ? supervisor.begin_secure_adoption(std::move(*skipped_reservation), bash_spec(ava::process::BashContainmentHandshakeV1::Required))
                          : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(skipped_reservation.error()));
  auto skipped_branch =
      skipped_gate ? skipped_gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(skipped_gate.error()));
  if (skipped_branch && *skipped_branch == ava::process::AdoptionForkBranchV1::Child)
    skipped_gate->child_exec_descriptor(executable);
  auto skipped =
      skipped_gate ? supervisor.adopt(std::move(*skipped_gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(skipped_gate.error()));

  auto wrong_reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  auto wrong_gate = wrong_reservation ? supervisor.begin_secure_adoption(std::move(*wrong_reservation), bash_spec())
                                      : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(wrong_reservation.error()));
  auto wrong_branch = wrong_gate ? wrong_gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(wrong_gate.error()));
  if (wrong_branch && *wrong_branch == ava::process::AdoptionForkBranchV1::Child)
    wrong_gate->child_bash_containment_applied();
  auto wrong = wrong_gate ? supervisor.adopt(std::move(*wrong_gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(wrong_gate.error()));
  if (executable >= 0)
    static_cast<void>(::close(executable));

  auto mermaid_environment = ava::process::make_mermaid_environment_v1();
  auto non_bash_reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Mermaid);
  auto non_bash =
      non_bash_reservation && mermaid_environment
          ? supervisor.begin_secure_adoption(std::move(*non_bash_reservation), {.environment = std::move(*mermaid_environment),
                                                                                .argv = {AVA_FAKE_PROCESS_CHILD_PATH, "normal"},
                                                                                .cwd = "/",
                                                                                .anchored_cwd = anchored_cwd("/"),
                                                                                .bash_containment = ava::process::BashContainmentHandshakeV1::Required})
          : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "fixture failed")));
  auto snapshot = supervisor.snapshot();
  bool exact = !skipped && !wrong && !non_bash && snapshot.records.size() == 3;
  for (std::size_t index = 0; index < snapshot.records.size(); ++index)
  {
    auto const& record = snapshot.records[index];
    exact = exact && record.reason == ava::process::TerminationReasonV1::LaunchFailed && record.exit_kind == ava::process::ExitKindV1::LaunchError &&
            record.settlement_count == 1 &&
            record.cleanup == (index + 1 == snapshot.records.size() ? ava::process::CleanupStateV1::NotRequired : ava::process::CleanupStateV1::Complete);
  }
  expect(exact && snapshot.live_records == 0,
         "skipped and unexpected containment checkpoints prevent an exec marker, while non-Bash Required rejects before fork");
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
}

void test_cancellation_after_release_before_confirmation()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  std::array<int, 2> reached{-1, -1};
  std::array<int, 2> hold{-1, -1};
  bool const pipes_ready = ::pipe2(reached.data(), O_CLOEXEC) == 0 && ::pipe2(hold.data(), O_CLOEXEC) == 0;
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  auto gate = reservation ? supervisor.begin_secure_adoption(std::move(*reservation), bash_spec())
                          : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(reservation.error()));
  auto branch = gate ? gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(gate.error()));
  if (branch && *branch == ava::process::AdoptionForkBranchV1::Child)
  {
    static_cast<void>(::close(reached[0]));
    static_cast<void>(::close(hold[1]));
    if (!write_all_to_descriptor_for_test(reached[1], "R", 1))
      gate->child_launch_failed(ava::process::AdoptionChildFailureStageV1::Streams, errno);
    char value = '\0';
    while (::read(hold[0], &value, 1) < 0 && errno == EINTR)
    {
    }
    _exit(0);
  }
  static_cast<void>(::close(reached[1]));
  static_cast<void>(::close(hold[0]));
  std::optional<ava::core::Result<ava::process::ProcessHandle>> adopted;
  std::thread adopter([&] { adopted.emplace(supervisor.adopt(std::move(*gate))); });
  bool const after_release = pipes_ready && read_byte(reached[0], 'R', ava::tests::now_plus_seconds(2s));
  auto canceled = supervisor.request_stop(application, ava::process::TerminationReasonV1::Canceled, ava::tests::now_plus_seconds(2s));
  adopter.join();
  static_cast<void>(::close(hold[1]));
  static_cast<void>(::close(reached[0]));

  auto snapshot = supervisor.snapshot();
  bool const exact = snapshot.records.size() == 1 && snapshot.records.front().reason == ava::process::TerminationReasonV1::Canceled &&
                     snapshot.records.front().settlement_count == 1 && snapshot.records.front().cleanup == ava::process::CleanupStateV1::Complete;
  expect(after_release && canceled && canceled->newly_requested == 1 && adopted && !*adopted && exact,
         "cancellation after physical release but before exec confirmation remains the immutable first reason and cleans exactly once");
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
}

ava::core::Result<ava::process::ProcessHandle> launch_mermaid(ava::process::Supervisor& supervisor, ava::process::OwnerPathV1 const& application,
                                                              std::string_view mode)
{
  int executable = ::open(AVA_FAKE_PROCESS_CHILD_PATH, O_RDONLY | O_CLOEXEC);
  int null_output = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Mermaid);
  auto gate = reservation ? supervisor.begin_secure_adoption(std::move(*reservation), mermaid_spec({AVA_FAKE_PROCESS_CHILD_PATH, std::string(mode)}))
                          : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(reservation.error()));
  auto branch = gate ? gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(gate.error()));
  if (branch && *branch == ava::process::AdoptionForkBranchV1::Child)
  {
    if (::dup2(null_output, STDOUT_FILENO) < 0 || ::dup2(null_output, STDERR_FILENO) < 0)
      gate->child_launch_failed(ava::process::AdoptionChildFailureStageV1::Streams, errno);
    gate->child_exec_descriptor(executable);
  }
  if (executable >= 0)
    static_cast<void>(::close(executable));
  if (null_output >= 0)
    static_cast<void>(::close(null_output));
  return gate ? supervisor.adopt(std::move(*gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(gate.error()));
}

void test_mermaid_expected_stop_and_second_stop()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  auto normal = launch_mermaid(supervisor, application, "normal");
  auto normal_status = normal ? wait_for(supervisor, *normal) : std::nullopt;
  auto stopped = launch_mermaid(supervisor, application, "stopped");
  auto stopped_status = stopped ? wait_for(supervisor, *stopped) : std::nullopt;
  ava::tests::expect_all("Mermaid consumes its role-derived initial stop but rejects a post-exec second stop",
                         {{"normal launch", normal.has_value()},
                          {"normal settlement", normal_status.has_value()},
                          {"natural reason", normal_status && normal_status->reason == ava::process::TerminationReasonV1::NaturalExit},
                          {"second-stop launch", stopped.has_value()},
                          {"second-stop settlement", stopped_status.has_value()},
                          {"unsupported suspension", stopped_status && stopped_status->reason == ava::process::TerminationReasonV1::UnsupportedSuspension},
                          {"complete cleanup", stopped_status && stopped_status->cleanup == ava::process::CleanupStateV1::Complete}});
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
}

void test_sentinel_descriptor_hygiene_while_leader_runs()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  std::array<int, 2> control{-1, -1};
  std::array<int, 2> output{-1, -1};
  std::array<int, 2> unrelated{-1, -1};
  bool const pipes_ready = ::pipe2(control.data(), O_CLOEXEC) == 0 && ::pipe2(output.data(), O_CLOEXEC) == 0 && ::pipe2(unrelated.data(), O_CLOEXEC) == 0;
  int executable = ::open(AVA_FAKE_PROCESS_CHILD_PATH, O_RDONLY | O_CLOEXEC);
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  auto gate = reservation ? supervisor.begin_secure_adoption(std::move(*reservation), bash_spec(ava::process::BashContainmentHandshakeV1::None,
                                                                                                {AVA_FAKE_PROCESS_CHILD_PATH, "closed-pipes"}))
                          : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(reservation.error()));
  auto branch = gate ? gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(gate.error()));
  if (branch && *branch == ava::process::AdoptionForkBranchV1::Child)
  {
    if (::dup2(control[0], STDIN_FILENO) < 0 || ::dup2(output[1], STDOUT_FILENO) < 0 || ::dup2(output[1], STDERR_FILENO) < 0)
      gate->child_launch_failed(ava::process::AdoptionChildFailureStageV1::Streams, errno);
    gate->child_exec_descriptor(executable);
  }
  auto sentinel = gate ? gate->fork_sentinel() : ava::core::VoidResult(std::unexpected(gate.error()));
  static_cast<void>(::close(control[0]));
  static_cast<void>(::close(output[1]));
  static_cast<void>(::close(unrelated[1]));
  if (executable >= 0)
    static_cast<void>(::close(executable));
  auto adopted = gate ? supervisor.adopt(std::move(*gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(gate.error()));
  bool const output_eof = read_eof(output[0], ava::tests::now_plus_seconds(2s));
  bool const unrelated_eof = read_eof(unrelated[0], ava::tests::now_plus_seconds(2s));
  bool const released = output_eof && write_all_to_descriptor_for_test(control[1], "X", 1);
  static_cast<void>(::close(control[1]));
  auto status = adopted ? wait_for(supervisor, *adopted) : std::nullopt;
  expect(pipes_ready && sentinel && adopted && output_eof && unrelated_eof && released && status &&
             status->reason == ava::process::TerminationReasonV1::NaturalExit,
         "the Bash-only sentinel retains no output, executable, protocol, or unrelated anchor-like descriptor while the leader runs");
  static_cast<void>(::close(output[0]));
  static_cast<void>(::close(unrelated[0]));
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
}

void test_retained_script_descriptor_and_invalid_keep_sets()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  std::array<int, 2> output{-1, -1};
  bool const pipe_ready = ::pipe2(output.data(), O_CLOEXEC) == 0;
  int executable = ::open(AVA_FAKE_PROCESS_CHILD_PATH, O_RDONLY | O_CLOEXEC);
  int retained = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  int unrelated = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  auto retained_text = std::to_string(retained);
  auto unrelated_text = std::to_string(unrelated);
  std::array<int, 1> keep{retained};
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  auto gate = reservation ? supervisor.begin_secure_adoption(
                                std::move(*reservation), bash_spec(ava::process::BashContainmentHandshakeV1::None,
                                                                   {AVA_FAKE_PROCESS_CHILD_PATH, "check-retained-and-closed", retained_text, unrelated_text}))
                          : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(reservation.error()));
  auto branch = gate ? gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(gate.error()));
  if (branch && *branch == ava::process::AdoptionForkBranchV1::Child)
  {
    if (::dup2(output[1], STDOUT_FILENO) < 0)
      gate->child_launch_failed(ava::process::AdoptionChildFailureStageV1::Streams, errno);
    gate->child_exec_descriptor(executable, keep);
  }
  static_cast<void>(::close(output[1]));
  static_cast<void>(::close(executable));
  static_cast<void>(::close(retained));
  static_cast<void>(::close(unrelated));
  int output_flags = ::fcntl(output[0], F_GETFL);
  if (output_flags >= 0)
    static_cast<void>(::fcntl(output[0], F_SETFL, output_flags | O_NONBLOCK));
  auto adopted = gate ? supervisor.adopt(std::move(*gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(gate.error()));
  std::string descriptor_output;
  bool const descriptor_result = ava::tests::read_until(output[0], descriptor_output, "CLEAN\n", ava::tests::now_plus_seconds(2s));
  auto status = adopted ? wait_for(supervisor, *adopted) : std::nullopt;
  static_cast<void>(::close(output[0]));

  bool invalid_sets_failed = true;
  for (int variant = 0; variant < 3; ++variant)
  {
    int final_descriptor = ::open(AVA_FAKE_PROCESS_CHILD_PATH, O_RDONLY | O_CLOEXEC);
    std::array<int, ava::process::kMaxRetainedScriptDescriptorsV1 + 1> descriptors{};
    for (auto& descriptor : descriptors)
      descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    std::span<int const> invalid;
    if (variant == 0)
    {
      static_cast<void>(::close(descriptors[1]));
      descriptors[1] = descriptors[0];
      invalid = std::span<int const>(descriptors.data(), 2);
    }
    else if (variant == 1)
    {
      static_cast<void>(::close(descriptors[0]));
      descriptors[0] = final_descriptor;
      invalid = std::span<int const>(descriptors.data(), 1);
    }
    else
    {
      invalid = descriptors;
    }
    auto invalid_reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
    auto invalid_gate = invalid_reservation ? supervisor.begin_secure_adoption(std::move(*invalid_reservation), bash_spec())
                                            : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(invalid_reservation.error()));
    auto invalid_branch =
        invalid_gate ? invalid_gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(invalid_gate.error()));
    if (invalid_branch && *invalid_branch == ava::process::AdoptionForkBranchV1::Child)
      invalid_gate->child_exec_descriptor(final_descriptor, invalid);
    auto invalid_adopted =
        invalid_gate ? supervisor.adopt(std::move(*invalid_gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(invalid_gate.error()));
    invalid_sets_failed = invalid_sets_failed && !invalid_adopted;
    static_cast<void>(::close(final_descriptor));
    for (int descriptor : descriptors)
    {
      if (descriptor >= 0 && descriptor != final_descriptor)
        static_cast<void>(::close(descriptor));
    }
  }

  auto snapshot = supervisor.snapshot();
  bool exact_invalid = snapshot.records.size() == 4;
  for (std::size_t index = 1; index < snapshot.records.size(); ++index)
    exact_invalid = exact_invalid && one_launch_error(snapshot.records[index], ava::process::TerminationReasonV1::LaunchFailed);
  expect(pipe_ready && adopted && descriptor_result && status && status->reason == ava::process::TerminationReasonV1::NaturalExit && invalid_sets_failed &&
             exact_invalid,
         "one bounded retained script descriptor survives exec while unrelated descriptors close and duplicate, final, or overflow keep sets fail closed");
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
}

void test_foreign_supervisor_cannot_adopt_gate()
{
  auto application = application_owner();
  ava::process::Supervisor owner;
  ava::process::Supervisor foreign;
  auto reservation = owner.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  if (!ava::tests::require(reservation.has_value(), "foreign adoption fixture reserves owner authority"))
    return;
  auto gate = owner.begin_secure_adoption(std::move(*reservation), bash_spec());
  if (!ava::tests::require(gate.has_value(), "foreign adoption fixture creates its gate"))
    return;
  int const executable = ::open(AVA_FAKE_PROCESS_CHILD_PATH, O_RDONLY | O_CLOEXEC);
  if (!ava::tests::require(executable >= 0, "foreign adoption fixture opens executable"))
    return;
  auto branch = gate->fork_leader();
  if (branch && *branch == ava::process::AdoptionForkBranchV1::Child)
    gate->child_exec_descriptor(executable);
  static_cast<void>(::close(executable));
  if (!ava::tests::require(branch && *branch == ava::process::AdoptionForkBranchV1::Parent, "foreign adoption fixture owns a gated leader"))
    return;
  auto rejected = foreign.adopt(std::move(*gate));
  auto const foreign_snapshot = foreign.snapshot();
  auto adopted = owner.adopt(std::move(*gate));
  auto status = adopted ? wait_for(owner, *adopted) : std::nullopt;
  ava::tests::expect_all(
      "foreign supervisor cannot consume or release another supervisor's exact gated child",
      {{"foreign adoption rejected", !rejected},
       {"foreign authority unchanged", foreign_snapshot.records.empty() && !foreign_snapshot.monitor_started},
       {"owner can still adopt", adopted.has_value()},
       {"owner child exits successfully", status && status->reason == ava::process::TerminationReasonV1::NaturalExit && status->exit_code == 0}});
  expect(owner.shutdown(ava::tests::now_plus_seconds(10)).complete, "foreign adoption fixture cleans up through its owner");
}

#endif

}  // namespace

void run_process_adoption_posix_tests()
{
#if defined(_WIN32)
  ava::tests::request_skip("secure process adoption is compile-time unsupported on Windows");
#else
  test_parent_establishes_leader_group_before_sentinel_fork();
  test_adoption_argv_rejected_before_fork();
  test_foreign_supervisor_cannot_adopt_gate();
  test_eof_child_stages_and_invalid_image();
  test_required_containment_exact_environment_sentinel_and_confirmation();
  test_containment_order_and_non_bash_rejection();
  test_cancellation_after_release_before_confirmation();
  test_mermaid_expected_stop_and_second_stop();
  test_sentinel_descriptor_hygiene_while_leader_runs();
  test_retained_script_descriptor_and_invalid_keep_sets();
#endif
}
