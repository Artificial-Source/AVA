#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/process/environment.h"
#include "ava/process/environment_test_support.h"
#include "ava/process/supervisor.h"
#include "ava/process/supervisor_test_support.h"
#include "ava/core/AnchorSet.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef AVA_FAKE_PROCESS_CHILD_PATH
#define AVA_FAKE_PROCESS_CHILD_PATH ""
#endif

#if !defined(_WIN32)
namespace {

using namespace std::chrono_literals;
using ava::process::PipeEndpoint;
using ava::process::PipeIoStateV1;

ava::process::OwnerPathV1 application_owner()
{
  auto owner = ava::process::OwnerPathV1::application();
  if (!owner)
    throw std::runtime_error(owner.error().format());
  return std::move(*owner);
}

ava::process::OwnerPathV1 operation_owner(ava::process::OwnerPathV1 const& prefix)
{
  auto owner = prefix.operation();
  if (!owner)
    throw std::runtime_error(owner.error().format());
  return std::move(*owner);
}

ava::process::SpawnSpecV1 fake_spec(std::string mode, ava::process::StreamModeV1 input = ava::process::StreamModeV1::Discard,
                                    ava::process::StreamModeV1 output = ava::process::StreamModeV1::Capture,
                                    ava::process::StreamModeV1 error = ava::process::StreamModeV1::Capture, std::vector<std::string> extra = {})
{
  std::vector<std::string> argv{AVA_FAKE_PROCESS_CHILD_PATH, std::move(mode)};
  argv.insert(argv.end(), std::make_move_iterator(extra.begin()), std::make_move_iterator(extra.end()));
  return {.executable = AVA_FAKE_PROCESS_CHILD_PATH,
          .argv = std::move(argv),
          .environment = {},
          .cwd = "/",
          .stdin_mode = input,
          .stdout_mode = output,
          .stderr_mode = error};
}

std::vector<ava::process::EnvironmentVariableV1> sealed_bash_variables()
{
  return {{"LANG", "C.UTF-8"},
          {"LC_ALL", "C.UTF-8"},
          {"LC_CTYPE", "C.UTF-8"},
          {"TZ", "UTC"},
          {"USER", "ava-test"},
          {"LOGNAME", "ava-test"},
          {"PWD", "/"},
          {"PATH", "/usr/bin:/bin"},
          {"HOME", "/tmp/ava-process-home"},
          {"XDG_CONFIG_HOME", "/tmp/ava-process-xdg-config"},
          {"XDG_CACHE_HOME", "/tmp/ava-process-xdg-cache"},
          {"XDG_DATA_HOME", "/tmp/ava-process-xdg-data"},
          {"XDG_STATE_HOME", "/tmp/ava-process-xdg-state"},
          {"TMPDIR", "/tmp/ava-process-tmp"}};
}

ava::process::ExactEnvironmentV1 environment_for_role(ava::process::ProcessRoleV1 role)
{
  auto make_host = [] {
    auto host = ava::process::testing::EnvironmentTestAccess::make_host({{"PATH", std::string(ava::process::kTrustedEnvironmentPathV1)}});
    if (!host)
      throw std::runtime_error(host.error().format());
    return std::move(*host);
  };

  auto environment = [&]() -> ava::core::Result<ava::process::ExactEnvironmentV1> {
    switch (role)
    {
      case ava::process::ProcessRoleV1::Curl: {
        auto host = make_host();
        return ava::process::make_curl_environment_v1(host);
      }
      case ava::process::ProcessRoleV1::Bash:
        return ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, "/", sealed_bash_variables());
      case ava::process::ProcessRoleV1::Plugin:
        return ava::process::make_plugin_environment_v1("/");
      case ava::process::ProcessRoleV1::Mcp:
        return ava::process::make_mcp_environment_v1("/", {});
      case ava::process::ProcessRoleV1::Lsp: {
        auto host = make_host();
        return ava::process::make_lsp_environment_v1(host, "/");
      }
      case ava::process::ProcessRoleV1::Mermaid:
        return ava::process::make_mermaid_environment_v1();
      case ava::process::ProcessRoleV1::BrowserOpener: {
        auto host = make_host();
        return ava::process::make_browser_desktop_environment_v1(host);
      }
      case ava::process::ProcessRoleV1::ClipboardHelper: {
        auto host = make_host();
        return ava::process::make_clipboard_desktop_environment_v1(host);
      }
      case ava::process::ProcessRoleV1::ExternalEditor: {
        auto host = make_host();
        return ava::process::make_external_editor_environment_v1(host, "/tmp/ava-process-editor-draft");
      }
    }
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "invalid process role fixture"));
  }();
  if (!environment)
    throw std::runtime_error(environment.error().format());
  return std::move(*environment);
}

ava::process::SpawnSpecV1 with_role_environment(ava::process::ProcessRoleV1 role, ava::process::SpawnSpecV1 specification)
{
  specification.environment = environment_for_role(role);
  return specification;
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

ava::process::SecureAdoptionSpecV1 adoption_spec(ava::process::ProcessRoleV1 role, std::string cwd = "/",
                                                 ava::process::BashContainmentHandshakeV1 containment = ava::process::BashContainmentHandshakeV1::None,
                                                 std::vector<std::string> argv = {AVA_FAKE_PROCESS_CHILD_PATH, "normal"})
{
  auto cwd_capability = anchored_cwd(cwd);
  return {.environment = environment_for_role(role),
          .argv = std::move(argv),
          .cwd = std::move(cwd),
          .anchored_cwd = std::move(cwd_capability),
          .bash_containment = containment};
}

ava::core::Result<ava::process::SpawnResultV1> spawn_fake(ava::process::Supervisor& supervisor, ava::process::OwnerPathV1 const& owner,
                                                          ava::process::ProcessRoleV1 role, ava::process::SpawnSpecV1 spec,
                                                          ava::process::LifecyclePolicyV1 policy = {})
{
  auto reservation = supervisor.reserve(owner, role, policy);
  if (!reservation)
    return std::unexpected(std::move(reservation.error()));
  spec.environment = environment_for_role(role);
  return supervisor.spawn(std::move(*reservation), std::move(spec));
}

class SupervisorFallback final
{
 public:
  explicit SupervisorFallback(ava::process::Supervisor& supervisor) : supervisor_(supervisor) { }
  ~SupervisorFallback() { static_cast<void>(supervisor_.shutdown(std::chrono::steady_clock::now() + 2s)); }

 private:
  ava::process::Supervisor& supervisor_;
};

class AfterForkReleaseLatch final
{
 public:
  ~AfterForkReleaseLatch() { release(); }

  void arrive_and_wait()
  {
    std::unique_lock lock(mutex_);
    reached_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_; });
  }

  bool wait_until(std::chrono::steady_clock::time_point deadline)
  {
    std::unique_lock lock(mutex_);
    return changed_.wait_until(lock, deadline, [&] { return reached_; });
  }

  void release() noexcept
  {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool reached_ = false;
  bool released_ = false;
};

class ExactChildFallback final
{
 public:
  ExactChildFallback(pid_t child, int release_descriptor) noexcept : child_(child), release_descriptor_(release_descriptor) { }
  ExactChildFallback(ExactChildFallback const&) = delete;
  ExactChildFallback& operator=(ExactChildFallback const&) = delete;
  ~ExactChildFallback()
  {
    if (release_descriptor_ >= 0)
      static_cast<void>(::close(release_descriptor_));
    if (child_ > 1)
    {
      static_cast<void>(::kill(child_, SIGKILL));
      int status = 0;
      while (::waitpid(child_, &status, 0) < 0 && errno == EINTR)
      {
      }
    }
  }

  void close_release() noexcept
  {
    if (release_descriptor_ >= 0)
      static_cast<void>(::close(release_descriptor_));
    release_descriptor_ = -1;
  }

  void disarm() noexcept
  {
    child_ = -1;
    close_release();
  }

 private:
  pid_t child_ = -1;
  int release_descriptor_ = -1;
};

std::span<std::byte> writable_bytes(std::array<char, 8192>& buffer)
{
  return {reinterpret_cast<std::byte*>(buffer.data()), buffer.size()};
}

bool read_until(PipeEndpoint& endpoint, std::string& received, std::string_view marker, std::chrono::steady_clock::time_point deadline,
                std::size_t bound = 4 * 1024 * 1024)
{
  std::array<char, 8192> buffer{};
  while (received.find(marker) == std::string::npos && received.size() < bound)
  {
    auto result = endpoint.read(writable_bytes(buffer));
    if (!result)
      return false;
    if (result->state == PipeIoStateV1::Progress)
    {
      received.append(buffer.data(), result->bytes);
      continue;
    }
    if (result->state == PipeIoStateV1::EndOfStream)
      return received.find(marker) != std::string::npos;
    auto ready = endpoint.wait_readable(deadline);
    if (!ready || !*ready)
      return false;
  }
  return received.find(marker) != std::string::npos;
}

bool read_to_eof(PipeEndpoint& endpoint, std::string& received, std::chrono::steady_clock::time_point deadline, std::size_t bound = 4 * 1024 * 1024)
{
  std::array<char, 8192> buffer{};
  while (received.size() <= bound)
  {
    auto result = endpoint.read(writable_bytes(buffer));
    if (!result)
      return false;
    if (result->state == PipeIoStateV1::Progress)
    {
      received.append(buffer.data(), result->bytes);
      continue;
    }
    if (result->state == PipeIoStateV1::EndOfStream)
      return true;
    auto ready = endpoint.wait_readable(deadline);
    if (!ready || !*ready)
      return false;
  }
  return false;
}

bool write_text(PipeEndpoint& endpoint, std::string_view text, std::chrono::steady_clock::time_point deadline)
{
  std::size_t offset = 0;
  while (offset < text.size())
  {
    auto source = std::span(reinterpret_cast<std::byte const*>(text.data() + offset), text.size() - offset);
    auto result = endpoint.write(source);
    if (!result)
      return false;
    if (result->state == PipeIoStateV1::Progress)
    {
      offset += result->bytes;
      continue;
    }
    auto ready = endpoint.wait_writable(deadline);
    if (!ready || !*ready)
      return false;
  }
  return true;
}

std::optional<ava::process::ExitStatusV1> wait_for(ava::process::Supervisor& supervisor, ava::process::ProcessHandle const& handle,
                                                   std::chrono::milliseconds timeout = 4s)
{
  auto status = supervisor.wait(handle, std::chrono::steady_clock::now() + timeout);
  if (!status)
    return std::nullopt;
  return *status;
}

void test_committed_stop_observation()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  SupervisorFallback cleanup(supervisor);
  ava::process::Supervisor foreign;
  ava::process::ProcessHandle invalid;
  expect(!supervisor.committed_stop_reason(invalid), "committed-stop observation rejects an invalid handle");

  auto child =
      spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Mcp, fake_spec("ready-gate", ava::process::StreamModeV1::Capture));
  expect(child.has_value(), "committed-stop observation fixture launches");
  if (!child)
    return;
  std::string ready;
  expect(child->standard_output && read_until(*child->standard_output, ready, "READY\n", std::chrono::steady_clock::now() + 2s),
         "committed-stop fixture is active behind its input gate");
  auto active = supervisor.committed_stop_reason(child->handle);
  expect(active && !*active, "an active owned handle has no committed reason");
  expect(!foreign.committed_stop_reason(child->handle), "committed-stop observation rejects a foreign handle");

  // Shared ownership lets the monitor keep the test gate alive until released.
  auto latch = std::make_shared<AfterForkReleaseLatch>();
  ava::process::testing::SupervisorTestAccess::set_after_poll_snapshot_hook(supervisor, [latch] { latch->arrive_and_wait(); });
  bool const reached = latch->wait_until(std::chrono::steady_clock::now() + 2s);
  auto first = supervisor.request_stop(application, ava::process::TerminationReasonV1::OwnerShutdown, std::chrono::steady_clock::now() + 2s);
  auto committed = supervisor.committed_stop_reason(child->handle);
  auto pending = supervisor.try_wait(child->handle);
  auto second = supervisor.request_stop(child->handle, ava::process::TerminationReasonV1::Canceled, std::chrono::steady_clock::now() + 2s);
  auto preserved = supervisor.committed_stop_reason(child->handle);
  expect(reached && first && first->newly_requested == 1 && pending && !*pending && committed && *committed == ava::process::TerminationReasonV1::OwnerShutdown,
         "owner stop is observable before monitor signaling or final settlement");
  expect(second && second->newly_requested == 0 && preserved && *preserved == ava::process::TerminationReasonV1::OwnerShutdown,
         "committed-stop observation preserves the first authoritative reason");
  ava::process::testing::SupervisorTestAccess::clear_after_poll_snapshot_hook(supervisor);
  latch->release();
  auto status = wait_for(supervisor, child->handle);
  auto settled = supervisor.committed_stop_reason(child->handle);
  expect(status && settled && *settled == ava::process::TerminationReasonV1::OwnerShutdown, "the committed first reason remains observable after settlement");
}

void test_exec_gate_normal_nonzero_and_exec_failure()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  SupervisorFallback fallback(supervisor);

  auto gated =
      spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Plugin, fake_spec("ready-gate", ava::process::StreamModeV1::Capture));
  expect(gated && gated->standard_input && gated->standard_output, "generic spawn returns opaque handle and owned capture endpoints after exec commit");
  if (gated && gated->standard_input && gated->standard_output)
  {
    std::string status_text;
    auto const deadline = std::chrono::steady_clock::now() + 3s;
    bool const ready = read_until(*gated->standard_output, status_text, "READY\n", deadline);
    bool const released = write_text(*gated->standard_input, "G", deadline) && read_until(*gated->standard_output, status_text, "RELEASED\n", deadline);
    gated->standard_input->close();
    auto status = wait_for(supervisor, gated->handle);
    auto completed = supervisor.committed_stop_reason(gated->handle);
    expect(completed && *completed == ava::process::TerminationReasonV1::NaturalExit,
           "committed-stop observation exposes natural completion distinctly from interruption");
    expect(ready && released && status && status->reason == ava::process::TerminationReasonV1::NaturalExit &&
               status->kind == ava::process::ExitKindV1::Exited && status->exit_code == 0 && status->cleanup == ava::process::CleanupStateV1::Complete,
           "the child cannot execute before registry commit and a gated normal exit is exactly observed and cleaned");
  }

  auto inherited =
      spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::BrowserOpener,
                 fake_spec("normal", ava::process::StreamModeV1::Inherit, ava::process::StreamModeV1::Inherit, ava::process::StreamModeV1::Inherit),
                 {.execution_deadline = std::chrono::steady_clock::now() + 5s});
  auto inherited_status = inherited ? wait_for(supervisor, inherited->handle) : std::nullopt;
  expect(inherited && !inherited->standard_input && !inherited->standard_output && !inherited->standard_error && inherited_status &&
             inherited_status->exit_code == 0,
         "inherit stream actions create no caller endpoints while retaining supervised lifecycle ownership");

  auto nonzero = spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Mcp, fake_spec("nonzero"));
  auto nonzero_status = nonzero ? wait_for(supervisor, nonzero->handle) : std::nullopt;
  expect(nonzero_status && nonzero_status->reason == ava::process::TerminationReasonV1::NaturalExit &&
             nonzero_status->kind == ava::process::ExitKindV1::Exited && nonzero_status->has_exit_code && nonzero_status->exit_code == 23,
         "exact wait observation preserves a bounded nonzero exit status");

  auto const root = create_empty_root("process-supervisor-exec-failure");
  auto const invalid_image = root / "invalid-image";
  {
    std::ofstream file(invalid_image, std::ios::binary | std::ios::trunc);
    file << "not an executable image\n";
  }
  static_cast<void>(::chmod(invalid_image.c_str(), 0700));
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::ClipboardHelper);
  auto failed_spec =
      with_role_environment(ava::process::ProcessRoleV1::ClipboardHelper,
                            ava::process::SpawnSpecV1{.executable = invalid_image.string(), .argv = {invalid_image.string()}, .environment = {}, .cwd = "/"});
  auto failed = reservation ? supervisor.spawn(std::move(*reservation), std::move(failed_spec))
                            : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(reservation.error()));
  expect(!failed && failed.error().format().find("exec syscall returned") != std::string::npos,
         "the versioned CLOEXEC status channel reports a typed post-attempt exec failure instead of claiming launch success");

  auto const setup_root = create_empty_root("process-supervisor-child-setup-failure");
  auto setup_environment = ava::process::make_plugin_environment_v1(setup_root);
  auto setup_reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Plugin);
  auto setup_specification = fake_spec("normal");
  setup_specification.cwd = setup_root.string();
  if (setup_environment)
    setup_specification.environment = std::move(*setup_environment);
  ava::process::testing::SupervisorTestAccess::fail_next_common_child_working_directory(supervisor);
  auto setup_failed = setup_reservation ? supervisor.spawn(std::move(*setup_reservation), std::move(setup_specification))
                                        : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(setup_reservation.error()));
  expect(setup_environment && !setup_failed && setup_failed.error().format().find("working_directory") != std::string::npos,
         "a typed common child setup failure is classified before any exec attempt");

  auto snapshot = supervisor.snapshot();
  bool saw_exec_failure = false;
  bool saw_setup_failure = false;
  for (auto const& record : snapshot.records)
  {
    saw_exec_failure = saw_exec_failure || (record.reason == ava::process::TerminationReasonV1::ExecFailed &&
                                            record.exit_kind == ava::process::ExitKindV1::LaunchError && record.settlement_count == 1);
    saw_setup_failure = saw_setup_failure || (record.reason == ava::process::TerminationReasonV1::LaunchFailed &&
                                              record.exit_kind == ava::process::ExitKindV1::LaunchError && record.settlement_count == 1);
  }
  expect(saw_exec_failure && saw_setup_failure && snapshot.monitor_started,
         "typed setup and exec failures preserve one launch-error settlement after exact monitor cleanup");
}

void test_matching_common_logical_cwd_executes()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  SupervisorFallback fallback(supervisor);
  auto const logical_cwd = create_empty_root("process-supervisor-common-cwd-match");
  auto environment = ava::process::make_plugin_environment_v1(logical_cwd);
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Plugin);
  auto specification = fake_spec("normal");
  specification.cwd = logical_cwd.string();
  if (environment)
    specification.environment = std::move(*environment);
  auto child = reservation ? supervisor.spawn(std::move(*reservation), std::move(specification))
                           : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(reservation.error()));
  auto status = child ? wait_for(supervisor, child->handle) : std::nullopt;
  auto snapshot = supervisor.snapshot();
  bool const settled_once = snapshot.records.size() == 1 && snapshot.records.front().settlement_count == 1;
  expect(environment && child && status && status->reason == ava::process::TerminationReasonV1::NaturalExit &&
             status->kind == ava::process::ExitKindV1::Exited && status->exit_code == 0 && settled_once && snapshot.monitor_started,
         "a common exact environment executes when SpawnSpec cwd exactly matches its bound logical cwd");
}

void test_natural_exit_does_not_pay_termination_grace()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  SupervisorFallback fallback(supervisor);
  auto const begin = std::chrono::steady_clock::now();
  auto reservation =
      supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::BrowserOpener, {.termination_grace = 5s, .execution_deadline = begin + 8s});
  auto specification = with_role_environment(ava::process::ProcessRoleV1::BrowserOpener, {.executable = "/bin/true",
                                                                                          .argv = {"/bin/true"},
                                                                                          .environment = {},
                                                                                          .cwd = "/",
                                                                                          .stdin_mode = ava::process::StreamModeV1::Discard,
                                                                                          .stdout_mode = ava::process::StreamModeV1::Discard,
                                                                                          .stderr_mode = ava::process::StreamModeV1::Discard});
  auto child = reservation ? supervisor.spawn(std::move(*reservation), std::move(specification))
                           : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(reservation.error()));
  auto status = child ? supervisor.wait(child->handle, begin + 6s) : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(child.error()));
  auto const elapsed = std::chrono::steady_clock::now() - begin;
  expect(child && status && status->reason == ava::process::TerminationReasonV1::NaturalExit && status->kind == ava::process::ExitKindV1::Exited &&
             status->exit_code == 0 && status->cleanup == ava::process::CleanupStateV1::Complete && elapsed < 2500ms,
         "a naturally exited leader with a proven quiet group skips a deliberately long termination grace");
}

void test_exact_environment_signal_reset_and_descriptor_closure()
{
  auto application = application_owner();
  ScopedEnvVar ambient("AVA_PROCESS_AMBIENT_CANARY", "must-not-leak");
  ScopedEnvVar home("HOME", "/must/not/leak");
  ava::process::Supervisor supervisor;
  SupervisorFallback fallback(supervisor);

  auto environment = spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Plugin, fake_spec("environment-clean"));
  std::string environment_status;
  bool environment_clean = environment && environment->standard_output &&
                           read_until(*environment->standard_output, environment_status, "CLEAN\n", std::chrono::steady_clock::now() + 2s);
  auto environment_exit = environment ? wait_for(supervisor, environment->handle) : std::nullopt;
  expect(environment_clean && environment_exit && environment_exit->exit_code == 0, "generic spawn passes the exact caller environment with no ambient merge");

  struct sigaction ignored{};
  struct sigaction previous{};
  ignored.sa_handler = SIG_IGN;
  static_cast<void>(::sigemptyset(&ignored.sa_mask));
  bool const changed_handler = ::sigaction(SIGTERM, &ignored, &previous) == 0;
  sigset_t blocked{};
  sigset_t previous_mask{};
  static_cast<void>(::sigemptyset(&blocked));
  static_cast<void>(::sigaddset(&blocked, SIGUSR1));
  bool const changed_mask = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous_mask) == 0;
  auto signal_state = spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Lsp, fake_spec("signal-state"));
  if (changed_handler)
    static_cast<void>(::sigaction(SIGTERM, &previous, nullptr));
  if (changed_mask)
    static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr));
  std::string signal_text;
  bool const signals_clean = signal_state && signal_state->standard_output &&
                             read_until(*signal_state->standard_output, signal_text, "CLEAN\n", std::chrono::steady_clock::now() + 2s);
  auto signal_exit = signal_state ? wait_for(supervisor, signal_state->handle) : std::nullopt;
  expect(changed_handler && changed_mask && signals_clean && signal_exit && signal_exit->exit_code == 0,
         "the closed child path resets inherited signal dispositions and mask before exec");

  int leaked = ::open("/dev/null", O_RDONLY);
  auto descriptor = spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Curl,
                               fake_spec("check-fd-closed", ava::process::StreamModeV1::Discard, ava::process::StreamModeV1::Capture,
                                         ava::process::StreamModeV1::Capture, {std::to_string(leaked)}));
  std::string descriptor_text;
  bool const descriptor_closed =
      descriptor && descriptor->standard_output && read_until(*descriptor->standard_output, descriptor_text, "CLOSED\n", std::chrono::steady_clock::now() + 2s);
  auto descriptor_exit = descriptor ? wait_for(supervisor, descriptor->handle) : std::nullopt;
  if (leaked >= 0)
    static_cast<void>(::close(leaked));
  expect(descriptor_closed && descriptor_exit && descriptor_exit->exit_code == 0,
         "exec closes an undeclared non-CLOEXEC parent descriptor rather than leaking ambient authority");
}

void test_write_to_closed_reader()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  SupervisorFallback fallback(supervisor);
  auto child =
      spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Mcp, fake_spec("ready-gate", ava::process::StreamModeV1::Capture));
  if (!child || !child->standard_input || !child->standard_output)
  {
    expect(false, "closed-reader fixture returns capture endpoints");
    return;
  }

  auto const deadline = std::chrono::steady_clock::now() + 3s;
  std::string marker;
  bool const ready = read_until(*child->standard_output, marker, "READY\n", deadline);
  bool const released = ready && write_text(*child->standard_input, "G", deadline);
  auto status = released ? wait_for(supervisor, child->handle) : std::nullopt;
  // Settlement proves the fake has closed its reader; the parent's writer
  // remains valid so this exercises peer closure rather than local closure.
  std::string_view const text = "after-exit";
  auto written = child->standard_input->write(std::span(reinterpret_cast<std::byte const*>(text.data()), text.size()));
  expect(status && status->exit_code == 0 && child->standard_input->valid() && written && written->bytes == 0 && written->state == PipeIoStateV1::EndOfStream,
         "neutral pipe writes to a closed peer reader report EndOfStream without a native IO error");
}

void test_closed_pipes_and_flood_draining()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  SupervisorFallback fallback(supervisor);

  auto closed =
      spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Mcp, fake_spec("closed-pipes", ava::process::StreamModeV1::Capture));
  std::string stdout_text;
  std::string stderr_text;
  auto const close_deadline = std::chrono::steady_clock::now() + 3s;
  bool const stdout_closed = closed && closed->standard_output && read_to_eof(*closed->standard_output, stdout_text, close_deadline);
  bool const stderr_closed = closed && closed->standard_error && read_to_eof(*closed->standard_error, stderr_text, close_deadline);
  bool const controlled = closed && closed->standard_input && write_text(*closed->standard_input, "X", close_deadline);
  if (closed && closed->standard_input)
    closed->standard_input->close();
  auto closed_status = closed ? wait_for(supervisor, closed->handle) : std::nullopt;
  expect(stdout_closed && stderr_closed && controlled && closed_status && closed_status->exit_code == 0,
         "parent capture endpoints observe EOF while the child is still alive, proving hidden pipe copies are closed");

  constexpr std::size_t flood_bytes = 1024 * 1024;
  auto flood = spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Plugin,
                          fake_spec("flood", ava::process::StreamModeV1::Discard, ava::process::StreamModeV1::Capture, ava::process::StreamModeV1::Capture,
                                    {std::to_string(flood_bytes)}));
  std::string out;
  std::string err;
  bool out_eof = false;
  bool err_eof = false;
  auto const flood_deadline = std::chrono::steady_clock::now() + 8s;
  std::array<char, 8192> buffer{};
  while (flood && flood->standard_output && flood->standard_error && (!out_eof || !err_eof) && std::chrono::steady_clock::now() < flood_deadline)
  {
    for (auto item : {std::pair{&*flood->standard_output, &out_eof}, std::pair{&*flood->standard_error, &err_eof}})
    {
      if (*item.second)
        continue;
      auto result = item.first->read(writable_bytes(buffer));
      if (!result)
      {
        *item.second = true;
        continue;
      }
      if (result->state == PipeIoStateV1::EndOfStream)
        *item.second = true;
      else if (result->state == PipeIoStateV1::Progress)
        (item.first == &*flood->standard_output ? out : err).append(buffer.data(), result->bytes);
    }
    if (!out_eof && out.size() < flood_bytes)
      static_cast<void>(flood->standard_output->wait_readable(std::min(flood_deadline, std::chrono::steady_clock::now() + 50ms)));
    if (!err_eof && err.size() < flood_bytes)
      static_cast<void>(flood->standard_error->wait_readable(std::min(flood_deadline, std::chrono::steady_clock::now() + 50ms)));
  }
  if (flood)
  {
    static_cast<void>(supervisor.account_output(flood->handle, ava::process::StreamKindV1::StandardOutput, out.size(), false));
    static_cast<void>(supervisor.account_output(flood->handle, ava::process::StreamKindV1::StandardOutput, std::numeric_limits<std::uint64_t>::max(), true));
    static_cast<void>(supervisor.account_output(flood->handle, ava::process::StreamKindV1::StandardError, err.size(), false));
  }
  auto flood_status = flood ? wait_for(supervisor, flood->handle) : std::nullopt;
  auto snapshot = supervisor.snapshot();
  bool accounted = false;
  for (auto const& record : snapshot.records)
    accounted =
        accounted || (record.stdout_bytes == std::numeric_limits<std::uint64_t>::max() && record.stderr_bytes == flood_bytes && record.stdout_truncated);
  expect(out_eof && err_eof && out.size() == flood_bytes && err.size() == flood_bytes && flood_status && flood_status->exit_code == 0 && accounted,
         "caller-owned concurrent draining handles stdout/stderr flood while supervisor stores only saturating aggregate accounting");
}

void test_term_kill_first_reason_and_leader_first_cleanup()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  SupervisorFallback fallback(supervisor);

  auto stubborn = spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::ClipboardHelper,
                             fake_spec("ignore-term", ava::process::StreamModeV1::Capture), {.termination_grace = 75ms, .execution_deadline = std::nullopt});
  std::string ready_text;
  bool const ready =
      stubborn && stubborn->standard_output && read_until(*stubborn->standard_output, ready_text, "READY\n", std::chrono::steady_clock::now() + 2s);
  auto first = stubborn ? supervisor.request_stop(stubborn->handle, ava::process::TerminationReasonV1::Canceled, std::chrono::steady_clock::now() + 2s)
                        : ava::core::Result<ava::process::StopResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "missing child")));
  auto second = stubborn ? supervisor.request_stop(stubborn->handle, ava::process::TerminationReasonV1::OutputLimit, std::chrono::steady_clock::now() + 2s)
                         : ava::core::Result<ava::process::StopResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "missing child")));
  auto stubborn_status = stubborn ? wait_for(supervisor, stubborn->handle) : std::nullopt;
  auto repeated_status = stubborn ? wait_for(supervisor, stubborn->handle, 50ms) : std::nullopt;
  bool stable_status = false;
  if (stubborn_status && repeated_status)
  {
    stable_status = stubborn_status->reason == ava::process::TerminationReasonV1::Canceled && stubborn_status->kind == ava::process::ExitKindV1::Signaled &&
                    stubborn_status->signal_number == SIGKILL && stubborn_status->cleanup == ava::process::CleanupStateV1::Complete &&
                    repeated_status->reason == stubborn_status->reason;
  }
  expect(ready && first && first->newly_requested == 1 && second && second->newly_requested == 0 && stable_status,
         "TERM refusal escalates to verified-group KILL while first reason wins and repeated wait observes one settlement");

  auto stopped = spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::ExternalEditor, fake_spec("stopped"),
                            {.termination_grace = 75ms, .execution_deadline = std::nullopt});
  std::string stopped_text;
  bool const stopped_ready =
      stopped && stopped->standard_output && read_until(*stopped->standard_output, stopped_text, "READY\n", std::chrono::steady_clock::now() + 2s);
  auto stopped_status = stopped ? wait_for(supervisor, stopped->handle) : std::nullopt;
  bool stopped_status_ok = false;
  if (stopped_status)
  {
    stopped_status_ok =
        stopped_status->reason == ava::process::TerminationReasonV1::UnsupportedSuspension && stopped_status->cleanup == ava::process::CleanupStateV1::Complete;
  }
  expect(stopped_ready && stopped_status_ok, "a stopped child is continued and boundedly terminated with unsupported_suspension rather than hanging");

  auto descendant =
      spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Plugin,
                 fake_spec("in-group-descendant", ava::process::StreamModeV1::Capture), {.termination_grace = 75ms, .execution_deadline = std::nullopt});
  std::string descendant_text;
  bool const tree_ready = descendant && descendant->standard_output &&
                          read_until(*descendant->standard_output, descendant_text, "DESCENDANT_READY\n", std::chrono::steady_clock::now() + 2s) &&
                          read_until(*descendant->standard_output, descendant_text, "LEADER_READY\n", std::chrono::steady_clock::now() + 2s);
  auto tree_stop = descendant ? supervisor.request_stop(descendant->handle, ava::process::TerminationReasonV1::Canceled, std::chrono::steady_clock::now() + 2s)
                              : ava::core::Result<ava::process::StopResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "missing child")));
  std::string descendant_tail;
  bool const tree_closed =
      descendant && descendant->standard_output && read_to_eof(*descendant->standard_output, descendant_tail, std::chrono::steady_clock::now() + 3s);
  auto tree_status = descendant ? wait_for(supervisor, descendant->handle) : std::nullopt;
  bool tree_status_ok = false;
  if (tree_status)
  {
    tree_status_ok = tree_status->reason == ava::process::TerminationReasonV1::Canceled && tree_status->signal_number == SIGTERM &&
                     tree_status->cleanup == ava::process::CleanupStateV1::Complete;
  }
  expect(tree_ready && tree_stop && tree_closed && tree_status_ok,
         "cancellation preserves the leader's TERM status while escalating across a TERM-ignoring descendant and closing its inherited status endpoint");

  auto leader_first = spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Plugin, fake_spec("leader-exits-first"),
                                 {.termination_grace = 75ms, .execution_deadline = std::nullopt});
  std::string leader_text;
  bool const descendant_ready = leader_first && leader_first->standard_output &&
                                read_until(*leader_first->standard_output, leader_text, "DESCENDANT_READY\n", std::chrono::steady_clock::now() + 2s);
  bool const leader_reported = leader_first && leader_first->standard_output &&
                               read_until(*leader_first->standard_output, leader_text, "LEADER_EXITING\n", std::chrono::steady_clock::now() + 2s);
  std::string tail;
  bool const all_writers_closed =
      leader_first && leader_first->standard_output && read_to_eof(*leader_first->standard_output, tail, std::chrono::steady_clock::now() + 3s);
  auto leader_status = leader_first ? wait_for(supervisor, leader_first->handle) : std::nullopt;
  expect(descendant_ready && leader_reported && all_writers_closed && leader_status &&
             leader_status->reason == ava::process::TerminationReasonV1::NaturalExit && leader_status->kind == ava::process::ExitKindV1::Exited &&
             leader_status->exit_code == 0 && leader_status->cleanup == ava::process::CleanupStateV1::Complete,
         "natural leader exit retains exact identity, kills a TERM-ignoring same-group descendant, then reaps the leader");
}

void test_cancel_deadline_owner_isolation_and_unrelated_child()
{
  auto application = application_owner();
  auto session_a = application.session();
  auto session_b = application.session();
  expect(session_a && session_b, "concurrent owner-isolation fixture creates independent generated sessions");
  if (!session_a || !session_b)
    return;

  ava::process::Supervisor supervisor;
  SupervisorFallback fallback(supervisor);
  auto child_a =
      spawn_fake(supervisor, operation_owner(*session_a), ava::process::ProcessRoleV1::Mcp, fake_spec("ignore-term", ava::process::StreamModeV1::Capture));
  auto child_b =
      spawn_fake(supervisor, operation_owner(*session_b), ava::process::ProcessRoleV1::Mcp, fake_spec("ignore-term", ava::process::StreamModeV1::Capture));
  std::string ready_a;
  std::string ready_b;
  bool const both_ready = child_a && child_b && child_a->standard_output && child_b->standard_output &&
                          read_until(*child_a->standard_output, ready_a, "READY\n", std::chrono::steady_clock::now() + 2s) &&
                          read_until(*child_b->standard_output, ready_b, "READY\n", std::chrono::steady_clock::now() + 2s);
  auto owner_stop = supervisor.request_stop(*session_a, ava::process::TerminationReasonV1::OwnerShutdown, std::chrono::steady_clock::now() + 2s);
  auto status_a = child_a ? wait_for(supervisor, child_a->handle) : std::nullopt;
  auto premature_b = child_b ? supervisor.wait(child_b->handle, std::chrono::steady_clock::now() + 30ms)
                             : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "missing child")));
  auto deadline_stop = child_b
                           ? supervisor.request_stop(child_b->handle, ava::process::TerminationReasonV1::DeadlineExpired, std::chrono::steady_clock::now() + 2s)
                           : ava::core::Result<ava::process::StopResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "missing child")));
  auto status_b = child_b ? wait_for(supervisor, child_b->handle) : std::nullopt;
  expect(both_ready && owner_stop && owner_stop->matched == 1 && status_a && status_a->reason == ava::process::TerminationReasonV1::OwnerShutdown &&
             !premature_b && deadline_stop && status_b && status_b->reason == ava::process::TerminationReasonV1::DeadlineExpired,
         "owner-prefix shutdown isolates concurrent sessions and a later absolute deadline keeps its own first reason");

  auto output_limited =
      spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Curl, fake_spec("ignore-term", ava::process::StreamModeV1::Capture));
  auto protocol_failed =
      spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Plugin, fake_spec("ignore-term", ava::process::StreamModeV1::Capture));
  std::string output_ready;
  std::string protocol_ready;
  bool const reason_children_ready = output_limited && protocol_failed && output_limited->standard_output && protocol_failed->standard_output &&
                                     read_until(*output_limited->standard_output, output_ready, "READY\n", std::chrono::steady_clock::now() + 2s) &&
                                     read_until(*protocol_failed->standard_output, protocol_ready, "READY\n", std::chrono::steady_clock::now() + 2s);
  auto output_stop =
      output_limited ? supervisor.request_stop(output_limited->handle, ava::process::TerminationReasonV1::OutputLimit, std::chrono::steady_clock::now() + 2s)
                     : ava::core::Result<ava::process::StopResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "missing child")));
  auto protocol_stop =
      protocol_failed
          ? supervisor.request_stop(protocol_failed->handle, ava::process::TerminationReasonV1::ProtocolFailure, std::chrono::steady_clock::now() + 2s)
          : ava::core::Result<ava::process::StopResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "missing child")));
  auto output_status = output_limited ? wait_for(supervisor, output_limited->handle) : std::nullopt;
  auto protocol_status = protocol_failed ? wait_for(supervisor, protocol_failed->handle) : std::nullopt;
  expect(reason_children_ready && output_stop && protocol_stop && output_status && protocol_status &&
             output_status->reason == ava::process::TerminationReasonV1::OutputLimit &&
             protocol_status->reason == ava::process::TerminationReasonV1::ProtocolFailure,
         "output-limit and protocol-failure stops remain distinct first reasons under concurrent monitoring");

  std::array<int, 2> gate{-1, -1};
  bool const pipe_created = ::pipe(gate.data()) == 0;
  pid_t unrelated = pipe_created ? ::fork() : -1;
  if (unrelated == 0)
  {
    static_cast<void>(::close(gate[1]));
    char release = '\0';
    while (::read(gate[0], &release, 1) < 0 && errno == EINTR)
    {
    }
    _exit(37);
  }
  if (pipe_created)
    static_cast<void>(::close(gate[0]));
  ExactChildFallback unrelated_fallback(unrelated, gate[1]);
  auto managed = spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Curl, fake_spec("normal"));
  auto managed_status = managed ? wait_for(supervisor, managed->handle) : std::nullopt;
  bool released_unrelated = unrelated > 1 && write(gate[1], "X", 1) == 1;
  unrelated_fallback.close_release();
  int unrelated_status = 0;
  pid_t const unrelated_waited = unrelated > 1 ? ::waitpid(unrelated, &unrelated_status, 0) : -1;
  if (unrelated_waited == unrelated)
    unrelated_fallback.disarm();
  expect(pipe_created && unrelated > 1 && managed_status && managed_status->exit_code == 0 && released_unrelated && unrelated_waited == unrelated &&
             WIFEXITED(unrelated_status) && WEXITSTATUS(unrelated_status) == 37,
         "exact-PID monitoring never steals or globally waits for an unrelated direct child");
}

void test_registered_spawn_gate_cancellation_never_executes()
{
  {
    auto application = application_owner();
    ava::process::Supervisor supervisor;
    SupervisorFallback fallback(supervisor);
    auto const root = create_empty_root("process-supervisor-after-fork-shutdown");
    auto const marker = root / "executed";
    auto latch = std::make_shared<AfterForkReleaseLatch>();
    ava::process::testing::SupervisorTestAccess::set_after_fork_before_release_hook(supervisor, [latch] { latch->arrive_and_wait(); });

    auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Plugin);
    std::optional<ava::core::Result<ava::process::SpawnResultV1>> launch;
    std::thread launcher;
    if (reservation)
    {
      launcher =
          std::thread([&supervisor, &launch, ticket = std::move(*reservation),
                       specification = with_role_environment(ava::process::ProcessRoleV1::Plugin,
                                                             fake_spec("exec-marker", ava::process::StreamModeV1::Discard, ava::process::StreamModeV1::Discard,
                                                                       ava::process::StreamModeV1::Discard, {marker.string()}))]() mutable {
            launch.emplace(supervisor.spawn(std::move(ticket), std::move(specification)));
          });
    }
    bool const reached = reservation && latch->wait_until(std::chrono::steady_clock::now() + 2s);
    auto shutdown = reached ? supervisor.shutdown(std::chrono::steady_clock::now() + 2s) : ava::process::ShutdownResultV1{.complete = false};
    latch->release();
    if (launcher.joinable())
      launcher.join();
    ava::process::testing::SupervisorTestAccess::clear_after_fork_before_release_hook(supervisor);

    auto snapshot = supervisor.snapshot();
    bool settled_once = snapshot.records.size() == 1 && snapshot.records.front().settlement_count == 1 &&
                        snapshot.records.front().reason == ava::process::TerminationReasonV1::ApplicationShutdown &&
                        snapshot.records.front().cleanup == ava::process::CleanupStateV1::Complete;
    bool const canceled_error = launch && !launch->has_value() && launch->error().format().find("canceled before") != std::string::npos;
    expect(reached && shutdown.complete && canceled_error && settled_once && !std::filesystem::exists(marker),
           "application shutdown ordered before the registered spawn gate commit closes the gate and prevents observable exec");
  }

  {
    auto application = application_owner();
    auto session = application.session();
    expect(session.has_value(), "owner-cancellation gate fixture creates a generated session prefix");
    if (!session)
      return;
    ava::process::Supervisor supervisor;
    SupervisorFallback fallback(supervisor);
    auto const root = create_empty_root("process-supervisor-after-fork-owner-cancel");
    auto const marker = root / "executed";
    auto latch = std::make_shared<AfterForkReleaseLatch>();
    ava::process::testing::SupervisorTestAccess::set_after_fork_before_release_hook(supervisor, [latch] { latch->arrive_and_wait(); });

    auto reservation = supervisor.reserve(operation_owner(*session), ava::process::ProcessRoleV1::Mcp);
    std::optional<ava::core::Result<ava::process::SpawnResultV1>> launch;
    std::thread launcher;
    if (reservation)
    {
      launcher =
          std::thread([&supervisor, &launch, ticket = std::move(*reservation),
                       specification = with_role_environment(ava::process::ProcessRoleV1::Mcp,
                                                             fake_spec("exec-marker", ava::process::StreamModeV1::Discard, ava::process::StreamModeV1::Discard,
                                                                       ava::process::StreamModeV1::Discard, {marker.string()}))]() mutable {
            launch.emplace(supervisor.spawn(std::move(ticket), std::move(specification)));
          });
    }
    bool const reached = reservation && latch->wait_until(std::chrono::steady_clock::now() + 2s);
    auto stopped =
        reached
            ? supervisor.request_stop(*session, ava::process::TerminationReasonV1::Canceled, std::chrono::steady_clock::now() + 2s)
            : ava::core::Result<ava::process::StopResultV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "launch did not reach test gate")));
    latch->release();
    if (launcher.joinable())
      launcher.join();
    ava::process::testing::SupervisorTestAccess::clear_after_fork_before_release_hook(supervisor);
    auto shutdown = supervisor.shutdown(std::chrono::steady_clock::now() + 2s);

    auto snapshot = supervisor.snapshot();
    bool settled_once = snapshot.records.size() == 1 && snapshot.records.front().settlement_count == 1 &&
                        snapshot.records.front().reason == ava::process::TerminationReasonV1::Canceled &&
                        snapshot.records.front().cleanup == ava::process::CleanupStateV1::Complete;
    bool const canceled_error = launch && !launch->has_value() && launch->error().format().find("canceled before") != std::string::npos;
    expect(reached && stopped && stopped->matched == 1 && stopped->newly_requested == 1 && shutdown.complete && canceled_error && settled_once &&
               !std::filesystem::exists(marker),
           "owner-prefix cancellation ordered before the registered spawn gate commit wins once and prevents observable exec");
  }
}

void test_secure_adoption_and_abandoned_ticket_cleanup()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  SupervisorFallback fallback(supervisor);

  int executable = ::open(AVA_FAKE_PROCESS_CHILD_PATH, O_RDONLY | O_CLOEXEC);
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  auto gate = reservation ? supervisor.begin_secure_adoption(std::move(*reservation), adoption_spec(ava::process::ProcessRoleV1::Bash))
                          : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(reservation.error()));
  auto branch = gate ? gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(gate.error()));
  if (branch && *branch == ava::process::AdoptionForkBranchV1::Child)
    gate->child_exec_descriptor(executable);
  if (executable >= 0)
    static_cast<void>(::close(executable));
  auto sentinel = gate ? gate->fork_sentinel() : ava::core::VoidResult(std::unexpected(gate.error()));
  auto adopted = gate ? supervisor.adopt(std::move(*gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(gate.error()));
  auto adopted_status = adopted ? wait_for(supervisor, *adopted) : std::nullopt;
  expect(branch && *branch == ava::process::AdoptionForkBranchV1::Parent && sentinel && adopted && adopted_status &&
             adopted_status->reason == ava::process::TerminationReasonV1::NaturalExit && adopted_status->cleanup == ava::process::CleanupStateV1::Complete,
         "secure adoption accepts only its exact gated leader and optional proven sentinel, then owns both exact waits");

  auto abandoned_reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Mermaid);
  bool parent_branch = false;
  if (abandoned_reservation)
  {
    auto abandoned_gate = supervisor.begin_secure_adoption(std::move(*abandoned_reservation), adoption_spec(ava::process::ProcessRoleV1::Mermaid));
    if (abandoned_gate)
    {
      auto abandoned_branch = abandoned_gate->fork_leader();
      if (abandoned_branch && *abandoned_branch == ava::process::AdoptionForkBranchV1::Child)
        _exit(0);
      parent_branch = abandoned_branch && *abandoned_branch == ava::process::AdoptionForkBranchV1::Parent;
    }
  }
  auto snapshot = supervisor.snapshot();
  bool saw_abandoned = false;
  for (auto const& record : snapshot.records)
    saw_abandoned = saw_abandoned || (record.role == ava::process::ProcessRoleV1::Mermaid && record.reason == ava::process::TerminationReasonV1::LaunchFailed &&
                                      record.cleanup == ava::process::CleanupStateV1::Complete);
  expect(parent_branch && saw_abandoned, "an abandoned post-fork adoption ticket performs bounded exact provisional cleanup without arbitrary PID adoption");
}

void test_destructor_fallback_cleanup()
{
  auto application = application_owner();
  std::optional<PipeEndpoint> output;
  bool ready = false;
  auto const begin = std::chrono::steady_clock::now();
  {
    ava::process::Supervisor supervisor;
    auto child = spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::ClipboardHelper,
                            fake_spec("ignore-term", ava::process::StreamModeV1::Capture), {.termination_grace = 75ms, .execution_deadline = std::nullopt});
    std::string ready_text;
    ready = child && child->standard_output && read_until(*child->standard_output, ready_text, "READY\n", std::chrono::steady_clock::now() + 2s);
    if (child && child->standard_output)
      output.emplace(std::move(*child->standard_output));
  }
  auto const elapsed = std::chrono::steady_clock::now() - begin;
  std::string tail;
  bool const closed = output && read_to_eof(*output, tail, std::chrono::steady_clock::now() + 500ms);
  expect(ready && closed && elapsed < 2100ms,
         "the no-throw Supervisor destructor uses one bounded fallback budget and closes a live child's inherited endpoint");
}

void test_gated_adoption_shutdown_race()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  SupervisorFallback fallback(supervisor);
  auto const root = create_empty_root("process-supervisor-adoption-after-fork-shutdown");
  auto const marker = root / "executed";
  int executable = ::open(AVA_FAKE_PROCESS_CHILD_PATH, O_RDONLY | O_CLOEXEC);
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Mermaid);
  auto gate = reservation
                  ? supervisor.begin_secure_adoption(std::move(*reservation),
                                                     adoption_spec(ava::process::ProcessRoleV1::Mermaid, "/", ava::process::BashContainmentHandshakeV1::None,
                                                                   {AVA_FAKE_PROCESS_CHILD_PATH, "exec-marker", marker.string()}))
                  : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(reservation.error()));
  auto branch = gate ? gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(gate.error()));
  if (branch && *branch == ava::process::AdoptionForkBranchV1::Child)
    gate->child_exec_descriptor(executable);
  if (executable >= 0)
    static_cast<void>(::close(executable));
  expect(branch && *branch == ava::process::AdoptionForkBranchV1::Parent, "Mermaid secure adoption creates its one exact gated leader without a sentinel");
  if (!gate || !branch)
    return;

  auto latch = std::make_shared<AfterForkReleaseLatch>();
  ava::process::testing::SupervisorTestAccess::set_after_fork_before_release_hook(supervisor, [latch] { latch->arrive_and_wait(); });
  std::optional<ava::core::Result<ava::process::ProcessHandle>> adoption;
  auto parent_gate = std::move(*gate);
  std::thread adopter([&supervisor, &adoption, ticket = std::move(parent_gate)]() mutable { adoption.emplace(supervisor.adopt(std::move(ticket))); });
  bool const reached = latch->wait_until(std::chrono::steady_clock::now() + 2s);
  auto shutdown = reached ? supervisor.shutdown(std::chrono::steady_clock::now() + 2s) : ava::process::ShutdownResultV1{.complete = false};
  latch->release();
  adopter.join();
  ava::process::testing::SupervisorTestAccess::clear_after_fork_before_release_hook(supervisor);

  auto snapshot = supervisor.snapshot();
  bool const settled_once = snapshot.live_records == 0 && snapshot.records.size() == 1 && snapshot.records.front().settlement_count == 1 &&
                            snapshot.records.front().reason == ava::process::TerminationReasonV1::ApplicationShutdown &&
                            snapshot.records.front().cleanup == ava::process::CleanupStateV1::Complete;
  bool const canceled_error = adoption && !adoption->has_value() && adoption->error().format().find("canceled before") != std::string::npos;
  expect(reached && shutdown.complete && canceled_error && settled_once && !std::filesystem::exists(marker),
         "shutdown ordered before secure-adoption commit closes both exact-child gates and prevents leader exec");
}

void test_mixed_shared_budget_shutdown()
{
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  SupervisorFallback fallback(supervisor);
  std::vector<ava::process::SpawnResultV1> children;
  children.reserve(8);
  bool all_ready = true;
  for (int index = 0; index < 8; ++index)
  {
    auto child = spawn_fake(supervisor, operation_owner(application), ava::process::ProcessRoleV1::Plugin,
                            fake_spec("ignore-term", ava::process::StreamModeV1::Capture), {.termination_grace = 100ms, .execution_deadline = std::nullopt});
    if (!child || !child->standard_output)
    {
      all_ready = false;
      continue;
    }
    std::string ready;
    all_ready = all_ready && read_until(*child->standard_output, ready, "READY\n", std::chrono::steady_clock::now() + 2s);
    children.push_back(std::move(*child));
  }
  if (!children.empty())
    static_cast<void>(supervisor.request_stop(children.front().handle, ava::process::TerminationReasonV1::Canceled, std::chrono::steady_clock::now() + 1500ms));
  auto const begin = std::chrono::steady_clock::now();
  auto shutdown = supervisor.shutdown(begin + 1500ms);
  auto const elapsed = std::chrono::steady_clock::now() - begin;
  bool statuses_match = true;
  for (std::size_t index = 0; index < children.size(); ++index)
  {
    auto status = supervisor.wait(children[index].handle, std::chrono::steady_clock::now() + 100ms);
    statuses_match = statuses_match && status && status->cleanup == ava::process::CleanupStateV1::Complete &&
                     status->reason == (index == 0 ? ava::process::TerminationReasonV1::Canceled : ava::process::TerminationReasonV1::ApplicationShutdown);
  }
  expect(all_ready && children.size() == 8 && shutdown.complete && shutdown.incomplete_count == 0 && statuses_match && elapsed < 1200ms,
         "mixed cancellation/application shutdown broadcasts TERM and KILL under one shared budget rather than multiplying grace per child");
}

}  // namespace
#endif

void run_process_supervisor_posix_tests()
{
#if defined(_WIN32)
  ava::tests::request_skip("process supervisor POSIX lifecycle backend is compile-time unsupported");
#else
  test_committed_stop_observation();
  test_exec_gate_normal_nonzero_and_exec_failure();
  test_matching_common_logical_cwd_executes();
  test_natural_exit_does_not_pay_termination_grace();
  test_exact_environment_signal_reset_and_descriptor_closure();
  test_write_to_closed_reader();
  test_closed_pipes_and_flood_draining();
  test_term_kill_first_reason_and_leader_first_cleanup();
  test_cancel_deadline_owner_isolation_and_unrelated_child();
  test_registered_spawn_gate_cancellation_never_executes();
  test_secure_adoption_and_abandoned_ticket_cleanup();
  test_destructor_fallback_cleanup();
  test_gated_adoption_shutdown_race();
  test_mixed_shared_budget_shutdown();
#endif
}
