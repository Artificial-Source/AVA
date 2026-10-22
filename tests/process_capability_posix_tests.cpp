#include "sys.h"
#include "tests/support/process_test_support.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/process/environment.h"
#include "ava/process/execution_capability.h"
#include "ava/process/execution_capability_test_support.h"
#include "ava/process/supervisor.h"
#include "ava/process/supervisor_test_support.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/AnchorOpen_test_support.h"
#include "ava/core/AnchorSet.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef AVA_FAKE_PROCESS_CHILD_PATH
#define AVA_FAKE_PROCESS_CHILD_PATH ""
#endif

namespace {

using namespace std::chrono_literals;

#if !defined(_WIN32)

using ava::process::AnchoredWorkingDirectoryV1;
using ava::process::PreopenedExecutableV1;

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

std::shared_ptr<ava::core::AnchorSet const> anchors_for(std::filesystem::path const& root)
{
  auto anchors = ava::core::AnchorSet::open({root});
  if (!anchors)
    throw std::runtime_error(anchors.error().format());
  return std::move(*anchors);
}

void copy_executable(std::filesystem::path const& source, std::filesystem::path const& destination, mode_t mode = 0700)
{
  std::error_code error;
  std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error);
  if (error || ::chmod(destination.c_str(), mode) != 0)
    throw std::runtime_error("failed to prepare executable capability fixture");
}

ava::process::ExpectedFileIdentityV1 identity_for(std::filesystem::path const& path)
{
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0)
    throw std::runtime_error("failed to stat capability fixture");
  return {.uid = static_cast<std::uint64_t>(status.st_uid),
          .gid = static_cast<std::uint64_t>(status.st_gid),
          .mode = static_cast<std::uint64_t>(status.st_mode),
          .nlink = static_cast<std::uint64_t>(status.st_nlink),
          .dev = static_cast<std::uint64_t>(status.st_dev),
          .inode = static_cast<std::uint64_t>(status.st_ino),
          .size = static_cast<std::uint64_t>(status.st_size),
          .ctime_sec = static_cast<std::int64_t>(status.st_ctim.tv_sec),
          .ctime_nsec = static_cast<std::int64_t>(status.st_ctim.tv_nsec)};
}

PreopenedExecutableV1 mint_executable(std::shared_ptr<ava::core::AnchorSet const> const& anchors, std::filesystem::path const& path)
{
  auto capability = ava::process::mint_preopened_executable(anchors, path);
  if (!capability)
    throw std::runtime_error(capability.error().format());
  return std::move(*capability);
}

AnchoredWorkingDirectoryV1 mint_cwd(std::shared_ptr<ava::core::AnchorSet const> const& anchors, std::filesystem::path const& path)
{
  auto capability = ava::process::mint_anchored_working_directory(anchors, path);
  if (!capability)
    throw std::runtime_error(capability.error().format());
  return std::move(*capability);
}

std::vector<ava::process::EnvironmentVariableV1> bash_variables(std::string const& cwd)
{
  return {{"LANG", "C.UTF-8"},
          {"LC_ALL", "C.UTF-8"},
          {"LC_CTYPE", "C.UTF-8"},
          {"TZ", "UTC"},
          {"USER", "ava-test"},
          {"LOGNAME", "ava-test"},
          {"PWD", cwd},
          {"PATH", "/usr/bin:/bin"},
          {"HOME", "/tmp/ava-process-home"},
          {"XDG_CONFIG_HOME", "/tmp/ava-process-xdg-config"},
          {"XDG_CACHE_HOME", "/tmp/ava-process-xdg-cache"},
          {"XDG_DATA_HOME", "/tmp/ava-process-xdg-data"},
          {"XDG_STATE_HOME", "/tmp/ava-process-xdg-state"},
          {"TMPDIR", "/tmp/ava-process-tmp"}};
}

ava::process::ExactEnvironmentV1 bash_environment(std::string const& cwd)
{
  auto environment = ava::process::validate_bash_environment_v1(ava::process::kBashEnvironmentProfileIdV1, cwd, bash_variables(cwd));
  if (!environment)
    throw std::runtime_error(environment.error().format());
  return std::move(*environment);
}

ava::process::SpawnSpecV1 plugin_spec(std::filesystem::path const& executable, std::filesystem::path const& cwd, std::string mode,
                                      std::vector<std::string> extra = {})
{
  auto environment = ava::process::make_plugin_environment_v1(cwd);
  if (!environment)
    throw std::runtime_error(environment.error().format());
  std::vector<std::string> argv{executable.string(), std::move(mode)};
  for (auto& argument : extra)
    argv.push_back(std::move(argument));
  return {.executable = executable.string(),
          .argv = std::move(argv),
          .environment = std::move(*environment),
          .cwd = cwd.string(),
          .stdin_mode = ava::process::StreamModeV1::Discard,
          .stdout_mode = ava::process::StreamModeV1::Discard,
          .stderr_mode = ava::process::StreamModeV1::Discard};
}

ava::core::Result<ava::process::SpawnResultV1> spawn_plugin(ava::process::Supervisor& supervisor, ava::process::OwnerPathV1 const& application,
                                                            ava::process::SpawnSpecV1 specification)
{
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Plugin);
  return reservation ? supervisor.spawn(std::move(*reservation), std::move(specification))
                     : ava::core::Result<ava::process::SpawnResultV1>(std::unexpected(std::move(reservation.error())));
}

bool wait_natural(ava::process::Supervisor& supervisor, ava::process::ProcessHandle const& handle)
{
  auto status = supervisor.wait(handle, ava::tests::now_plus_seconds(4s));
  return status && status->reason == ava::process::TerminationReasonV1::NaturalExit && status->kind == ava::process::ExitKindV1::Exited &&
         status->exit_code == 0 && status->cleanup == ava::process::CleanupStateV1::Complete;
}

std::string read_endpoint(ava::process::PipeEndpoint& endpoint)
{
  std::string result;
  std::array<std::byte, 256> buffer{};
  auto const deadline = ava::tests::now_plus_seconds(3s);
  while (std::chrono::steady_clock::now() < deadline)
  {
    auto read = endpoint.read(buffer);
    if (!read)
      break;
    if (read->state == ava::process::PipeIoStateV1::Progress)
    {
      result.append(reinterpret_cast<char const*>(buffer.data()), read->bytes);
      continue;
    }
    if (read->state == ava::process::PipeIoStateV1::EndOfStream)
      break;
    auto ready = endpoint.wait_readable(deadline);
    if (!ready || !*ready)
      break;
  }
  return result;
}

class Latch final
{
 public:
  ~Latch() { release(); }

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

template <typename T>
concept HasFdAccessor = requires(T const& value) { value.fd(); };

template <typename T>
concept HasNativeHandleAccessor = requires(T const& value) { value.native_handle(); };

template <typename T>
concept HasPathAccessor = requires(T const& value) { value.path(); };

template <typename T>
concept HasIdentityAccessor = requires(T const& value) { value.identity(); };

void test_api_shape_paths_and_exact_identity()
{
  static_assert(!std::is_copy_constructible_v<PreopenedExecutableV1> && !std::is_copy_assignable_v<PreopenedExecutableV1>);
  static_assert(std::is_nothrow_move_constructible_v<PreopenedExecutableV1> && std::is_nothrow_move_assignable_v<PreopenedExecutableV1>);
  static_assert(!std::is_copy_constructible_v<AnchoredWorkingDirectoryV1> && !std::is_copy_assignable_v<AnchoredWorkingDirectoryV1>);
  static_assert(std::is_nothrow_move_constructible_v<AnchoredWorkingDirectoryV1> && std::is_nothrow_move_assignable_v<AnchoredWorkingDirectoryV1>);
  static_assert(!HasFdAccessor<PreopenedExecutableV1> && !HasNativeHandleAccessor<PreopenedExecutableV1> && !HasPathAccessor<PreopenedExecutableV1> &&
                !HasIdentityAccessor<PreopenedExecutableV1>);
  static_assert(!HasFdAccessor<AnchoredWorkingDirectoryV1> && !HasNativeHandleAccessor<AnchoredWorkingDirectoryV1> &&
                !HasPathAccessor<AnchoredWorkingDirectoryV1> && !HasIdentityAccessor<AnchoredWorkingDirectoryV1>);

  auto const root = create_empty_root("process-capability-api");
  auto const executable = root / "EXECUTABLE_PATH_CANARY_4fa1";
  copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, executable);
  auto anchors = anchors_for(root);
  PreopenedExecutableV1 empty_executable;
  AnchoredWorkingDirectoryV1 empty_cwd;
  auto exact = identity_for(executable);
  auto capability = ava::process::mint_preopened_executable(anchors, executable, exact);
  auto cwd_capability = ava::process::mint_anchored_working_directory(anchors, root, identity_for(root));
  bool move_shape = capability && cwd_capability;
  if (capability)
  {
    PreopenedExecutableV1 moved(std::move(*capability));
    move_shape = move_shape && moved.valid() && !capability->valid();
  }
  if (cwd_capability)
  {
    AnchoredWorkingDirectoryV1 moved(std::move(*cwd_capability));
    move_shape = move_shape && moved.valid() && !cwd_capability->valid();
  }

  auto relative = ava::process::mint_preopened_executable(anchors, "relative");
  auto nonnormalized = ava::process::mint_preopened_executable(anchors, root / "subdir" / ".." / executable.filename());
  auto duplicate_separator = ava::process::mint_preopened_executable(anchors, std::filesystem::path(root.string() + "//" + executable.filename().string()));
  auto nul = ava::process::mint_preopened_executable(anchors, std::filesystem::path(std::string("/tmp/bad\0path", 13)));
  expect(!empty_executable.valid() && !empty_cwd.valid() && move_shape, "execution capabilities are move-only, default-invalid, and moved-from-invalid");
  expect(!relative && !nonnormalized && !duplicate_separator && !nul, "capability factories reject relative, non-normalized, and NUL-bearing logical paths");

  auto mismatch_rejected = [&](ava::process::ExpectedFileIdentityV1 changed) {
    auto result = ava::process::mint_preopened_executable(anchors, executable, changed);
    return !result && result.error().format().find(executable.string()) == std::string::npos;
  };
  bool every_field = true;
  auto changed = exact;
  ++changed.uid;
  every_field = every_field && mismatch_rejected(changed);
  changed = exact;
  ++changed.gid;
  every_field = every_field && mismatch_rejected(changed);
  changed = exact;
  ++changed.mode;
  every_field = every_field && mismatch_rejected(changed);
  changed = exact;
  ++changed.nlink;
  every_field = every_field && mismatch_rejected(changed);
  changed = exact;
  ++changed.dev;
  every_field = every_field && mismatch_rejected(changed);
  changed = exact;
  ++changed.inode;
  every_field = every_field && mismatch_rejected(changed);
  changed = exact;
  ++changed.size;
  every_field = every_field && mismatch_rejected(changed);
  changed = exact;
  ++changed.ctime_sec;
  every_field = every_field && mismatch_rejected(changed);
  changed = exact;
  ++changed.ctime_nsec;
  every_field = every_field && mismatch_rejected(changed);
  expect(every_field, "the optional expected identity accepts an exact tuple and rejects each field mismatch without echoing identity content");
}

void test_mint_identity_races()
{
  bool all_rejected = true;
  int index = 0;
  for (auto stage :
       {ava::process::testing::CapabilityMintHookStage::AfterBeforeObservation, ava::process::testing::CapabilityMintHookStage::AfterOpenedObservation})
  {
    auto const root = create_empty_root("process-capability-mint-race-" + std::to_string(index++));
    auto const executable = root / "logical-executable";
    auto const retained = root / "retained-a";
    copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, executable);
    auto anchors = anchors_for(root);
    bool fired = false;
    ava::process::testing::ExecutionCapabilityTestAccess::set_mint_hook([&](ava::process::testing::CapabilityMintHookStage observed) {
      if (observed != stage || fired)
        return;
      fired = true;
      std::filesystem::rename(executable, retained);
      copy_executable("/bin/false", executable);
    });
    auto capability = ava::process::mint_preopened_executable(anchors, executable);
    ava::process::testing::ExecutionCapabilityTestAccess::clear_mint_hook();
    all_rejected = all_rejected && fired && !capability && capability.error().format().find(executable.string()) == std::string::npos;
  }
  expect(all_rejected, "replacement after the before or opened observation deterministically fails exact three-way mint identity");
}

void test_executable_policy()
{
  auto const root = create_empty_root("process-capability-policy");
  auto anchors = anchors_for(root);
  auto const owner_writable = root / "owner-writable";
  auto const nonexecute = root / "nonexecute";
  auto const group_writable = root / "group-writable";
  auto const other_writable = root / "other-writable";
  auto const hardlinked = root / "hardlinked";
  auto const hardlink = root / "hardlink-copy";
  auto const symlink = root / "final-symlink";
  auto const directory = root / "working-directory";
  auto const directory_symlink = root / "working-directory-link";
  copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, owner_writable, 0700);
  copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, nonexecute, 0600);
  copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, group_writable, 0720);
  copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, other_writable, 0702);
  copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, hardlinked, 0700);
  std::filesystem::create_directory(directory);
  if (::link(hardlinked.c_str(), hardlink.c_str()) != 0 || ::symlink(owner_writable.filename().c_str(), symlink.c_str()) != 0 ||
      ::symlink(directory.filename().c_str(), directory_symlink.c_str()) != 0)
  {
    throw std::runtime_error("failed to prepare executable link fixtures");
  }

  auto accepted = ava::process::mint_preopened_executable(anchors, owner_writable);
  auto rejected_symlink = ava::process::mint_preopened_executable(anchors, symlink);
  auto rejected_hardlink = ava::process::mint_preopened_executable(anchors, hardlinked);
  auto rejected_directory = ava::process::mint_preopened_executable(anchors, root);
  auto rejected_missing = ava::process::mint_preopened_executable(anchors, root / "missing");
  auto rejected_nonexecute = ava::process::mint_preopened_executable(anchors, nonexecute);
  auto rejected_group = ava::process::mint_preopened_executable(anchors, group_writable);
  auto rejected_other = ava::process::mint_preopened_executable(anchors, other_writable);
  auto accepted_cwd_symlink = ava::process::mint_anchored_working_directory(anchors, directory_symlink);
  auto rejected_cwd_file = ava::process::mint_anchored_working_directory(anchors, owner_writable);
  expect(accepted && !rejected_symlink && !rejected_hardlink && !rejected_directory && !rejected_missing && !rejected_nonexecute && !rejected_group &&
             !rejected_other && accepted_cwd_symlink && !rejected_cwd_file,
         "executable policy rejects unsafe targets while cwd minting preserves a contained final symlink and rejects a nondirectory");
}

bool mutation_rejected_before_fork(std::string const& name, auto mutate)
{
  auto const root = create_empty_root("process-capability-mutation-" + name);
  auto const executable = root / "target";
  auto const marker = root / "marker";
  copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, executable);
  auto anchors = anchors_for(root);
  auto capability = mint_executable(anchors, executable);
  mutate(executable, root);

  auto application = application_owner();
  ava::process::Supervisor supervisor;
  auto specification = plugin_spec(executable, "/", "exec-marker", {marker.string()});
  specification.preopened_executable.emplace(std::move(capability));
  auto result = spawn_plugin(supervisor, application, std::move(specification));
  auto snapshot = supervisor.snapshot();
  bool const exact = !result && !snapshot.monitor_started && snapshot.live_records == 0 && snapshot.records.size() == 1 &&
                     snapshot.records.front().reason == ava::process::TerminationReasonV1::LaunchFailed && !std::filesystem::exists(marker);
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
  return exact;
}

void test_post_mint_mutations_and_pre_spawn_replacement()
{
  bool const hardlink = mutation_rejected_before_fork("hardlink", [](auto const& executable, auto const& root) {
    if (::link(executable.c_str(), (root / "second-link").c_str()) != 0)
      throw std::runtime_error("failed to mutate executable link count");
  });
  bool const mode = mutation_rejected_before_fork("mode", [](auto const& executable, auto const&) {
    if (::chmod(executable.c_str(), 0500) != 0)
      throw std::runtime_error("failed to mutate executable mode");
  });
  bool const setuid = mutation_rejected_before_fork("setuid", [](auto const& executable, auto const&) {
    if (::chmod(executable.c_str(), 04700) != 0)
      throw std::runtime_error("failed to mutate executable set-user-ID bit");
  });
  bool const setgid = mutation_rejected_before_fork("setgid", [](auto const& executable, auto const&) {
    if (::chmod(executable.c_str(), 02700) != 0)
      throw std::runtime_error("failed to mutate executable set-group-ID bit");
  });
  bool const size = mutation_rejected_before_fork("size", [](auto const& executable, auto const&) {
    std::ofstream output(executable, std::ios::binary | std::ios::app);
    output << 'x';
  });
  bool const ctime = mutation_rejected_before_fork("ctime", [](auto const& executable, auto const&) {
    auto const before = identity_for(executable);
    auto const watchdog = ava::tests::now_plus_seconds(10);
    while (std::chrono::steady_clock::now() < watchdog)
    {
      timespec times[2]{{.tv_sec = 0, .tv_nsec = UTIME_OMIT}, {.tv_sec = 100, .tv_nsec = 0}};
      if (::utimensat(AT_FDCWD, executable.c_str(), times, 0) != 0)
        throw std::runtime_error("failed to mutate executable ctime");
      auto const after = identity_for(executable);
      if (after.ctime_sec != before.ctime_sec || after.ctime_nsec != before.ctime_nsec)
        return;
      std::this_thread::yield();
    }
    throw std::runtime_error("filesystem did not expose a ctime transition before the fixture watchdog");
  });

  auto const root = create_empty_root("process-capability-pre-spawn-replacement");
  auto const executable = root / "logical-executable";
  auto const retained = root / "retained-a";
  auto const marker = root / "must-not-execute";
  copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, executable);
  auto anchors = anchors_for(root);
  auto capability = mint_executable(anchors, executable);
  std::filesystem::rename(executable, retained);
  copy_executable("/bin/false", executable);
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  auto specification = plugin_spec(executable, "/", "exec-marker", {marker.string()});
  specification.preopened_executable.emplace(std::move(capability));
  auto replaced = spawn_plugin(supervisor, application, std::move(specification));
  auto snapshot = supervisor.snapshot();
  bool const replacement = !replaced && !snapshot.monitor_started && snapshot.live_records == 0 && !std::filesystem::exists(marker);
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
  ava::tests::expect_all("post-mint identity mutations and pre-spawn replacement fail freshness before fork", {{"hardlink", hardlink},
                                                                                                               {"mode", mode},
                                                                                                               {"set-user-ID", setuid},
                                                                                                               {"set-group-ID", setgid},
                                                                                                               {"size", size},
                                                                                                               {"ctime", ctime},
                                                                                                               {"namespace replacement", replacement}});
}

void test_post_fork_executable_replacement_uses_inherited_a()
{
  auto const root = create_empty_root("process-capability-post-fork-executable");
  auto const executable = root / "logical-executable";
  auto const retained = root / "retained-a";
  auto const marker = root / "a-executed";
  copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, executable);
  auto anchors = anchors_for(root);
  auto capability = mint_executable(anchors, executable);
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  auto latch = std::make_shared<Latch>();
  ava::process::testing::SupervisorTestAccess::set_after_fork_before_release_hook(supervisor, [latch] { latch->arrive_and_wait(); });
  auto specification = plugin_spec(executable, "/", "exec-marker", {marker.string()});
  specification.preopened_executable.emplace(std::move(capability));
  auto reservation = supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Plugin);
  std::optional<ava::core::Result<ava::process::SpawnResultV1>> launched;
  std::thread launcher;
  if (reservation)
  {
    launcher = std::thread([&supervisor, &launched, ticket = std::move(*reservation), specification = std::move(specification)]() mutable {
      launched.emplace(supervisor.spawn(std::move(ticket), std::move(specification)));
    });
  }
  bool const reached = reservation && latch->wait_until(ava::tests::now_plus_seconds(3s));
  if (reached)
  {
    std::filesystem::rename(executable, retained);
    copy_executable("/bin/false", executable);
  }
  latch->release();
  if (launcher.joinable())
    launcher.join();
  ava::process::testing::SupervisorTestAccess::clear_after_fork_before_release_hook(supervisor);
  auto status = launched && *launched
                    ? supervisor.wait((*launched)->handle, ava::tests::now_plus_seconds(10))
                    : ava::core::Result<ava::process::ExitStatusV1>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "launch fixture failed")));
  ava::tests::expect_all("a gate-held child executes inherited executable A after its logical route is replaced by B",
                         {{"reservation", reservation.has_value()},
                          {"gate reached", reached},
                          {"launch returned", launched.has_value()},
                          {"launch succeeded", launched && launched->has_value()},
                          {"status", status.has_value()},
                          {"natural reason", status && status->reason == ava::process::TerminationReasonV1::NaturalExit},
                          {"exited normally", status && status->kind == ava::process::ExitKindV1::Exited},
                          {"exit zero", status && status->has_exit_code && status->exit_code == 0},
                          {"complete cleanup", status && status->cleanup == ava::process::CleanupStateV1::Complete},
                          {"A marker exists", std::filesystem::exists(marker)}});
  if (launched && !*launched)
    expect(false, "inherited executable A launch error: " + launched->error().format());
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
}

void test_cwd_replacement_common_and_secure()
{
  auto const root = create_empty_root("process-capability-cwd-replacement");
  auto const executable = std::filesystem::path(AVA_FAKE_PROCESS_CHILD_PATH);

  auto common_a = root / "common-cwd";
  std::filesystem::create_directory(common_a);
  auto common_anchors = anchors_for(root);
  auto common_capability = mint_cwd(common_anchors, common_a);
  auto common_retained = root / "common-retained-a";
  std::filesystem::rename(common_a, common_retained);
  std::filesystem::create_directory(common_a);
  auto application = application_owner();
  ava::process::Supervisor common_supervisor;
  auto common_specification = plugin_spec(executable, common_a, "exec-marker", {"relative-marker"});
  common_specification.anchored_cwd.emplace(std::move(common_capability));
  auto common = spawn_plugin(common_supervisor, application, std::move(common_specification));
  auto common_snapshot = common_supervisor.snapshot();
  bool const common_rejected = !common && !common_snapshot.monitor_started && !std::filesystem::exists(common_retained / "relative-marker") &&
                               !std::filesystem::exists(common_a / "relative-marker");
  static_cast<void>(common_supervisor.shutdown(ava::tests::now_plus_seconds(2s)));

  auto secure_pre_a = root / "secure-pre-cwd";
  std::filesystem::create_directory(secure_pre_a);
  auto secure_pre_capability = mint_cwd(anchors_for(root), secure_pre_a);
  ava::process::Supervisor secure_pre_supervisor;
  auto pre_reservation = secure_pre_supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  ava::process::SecureAdoptionSpecV1 pre_specification{.environment = bash_environment(secure_pre_a.string()),
                                                       .argv = {executable.string(), "normal"},
                                                       .cwd = secure_pre_a.string(),
                                                       .anchored_cwd = std::move(secure_pre_capability),
                                                       .bash_containment = ava::process::BashContainmentHandshakeV1::None};
  auto pre_gate = pre_reservation ? secure_pre_supervisor.begin_secure_adoption(std::move(*pre_reservation), std::move(pre_specification))
                                  : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(pre_reservation.error()));
  auto secure_pre_retained = root / "secure-pre-retained-a";
  if (pre_gate)
  {
    std::filesystem::rename(secure_pre_a, secure_pre_retained);
    std::filesystem::create_directory(secure_pre_a);
  }
  auto pre_branch = pre_gate ? pre_gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(pre_gate.error()));
  auto pre_snapshot = secure_pre_supervisor.snapshot();
  bool const secure_pre_rejected = pre_gate && !pre_branch && !pre_snapshot.monitor_started && pre_snapshot.live_records == 0;
  static_cast<void>(secure_pre_supervisor.shutdown(ava::tests::now_plus_seconds(2s)));

  auto secure_post_a = root / "secure-post-cwd";
  std::filesystem::create_directory(secure_post_a);
  auto secure_post_capability = mint_cwd(anchors_for(root), secure_post_a);
  ava::process::Supervisor secure_post_supervisor;
  auto post_reservation = secure_post_supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  ava::process::SecureAdoptionSpecV1 post_specification{.environment = bash_environment(secure_post_a.string()),
                                                        .argv = {executable.string(), "exec-marker", "relative-marker"},
                                                        .cwd = secure_post_a.string(),
                                                        .anchored_cwd = std::move(secure_post_capability),
                                                        .bash_containment = ava::process::BashContainmentHandshakeV1::None};
  auto post_gate = post_reservation ? secure_post_supervisor.begin_secure_adoption(std::move(*post_reservation), std::move(post_specification))
                                    : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(post_reservation.error()));
  int executable_descriptor = ::open(executable.c_str(), O_RDONLY | O_CLOEXEC);
  auto post_branch = post_gate ? post_gate->fork_leader() : ava::core::Result<ava::process::AdoptionForkBranchV1>(std::unexpected(post_gate.error()));
  if (post_branch && *post_branch == ava::process::AdoptionForkBranchV1::Child)
    post_gate->child_exec_descriptor(executable_descriptor);
  auto secure_post_retained = root / "secure-post-retained-a";
  if (post_branch && *post_branch == ava::process::AdoptionForkBranchV1::Parent)
  {
    std::filesystem::rename(secure_post_a, secure_post_retained);
    std::filesystem::create_directory(secure_post_a);
  }
  auto adopted =
      post_gate ? secure_post_supervisor.adopt(std::move(*post_gate)) : ava::core::Result<ava::process::ProcessHandle>(std::unexpected(post_gate.error()));
  bool const caller_still_owns_executable = executable_descriptor >= 0 && ::fcntl(executable_descriptor, F_GETFD) >= 0;
  if (executable_descriptor >= 0)
    static_cast<void>(::close(executable_descriptor));
  bool const secure_post = adopted && wait_natural(secure_post_supervisor, *adopted) && caller_still_owns_executable &&
                           std::filesystem::exists(secure_post_retained / "relative-marker") && !std::filesystem::exists(secure_post_a / "relative-marker");
  static_cast<void>(secure_post_supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
  expect(common_rejected && secure_pre_rejected && secure_post,
         "cwd replacement fails before common/secure fork, while a forked secure leader enters retained directory A and leaves B untouched");
}

void test_capability_combinations_and_logical_mismatch()
{
  auto const root = create_empty_root("process-capability-combinations");
  auto const executable = root / "executable";
  auto const cwd = root / "cwd";
  copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, executable);
  std::filesystem::create_directory(cwd);
  auto anchors = anchors_for(root);
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  for (int variant = 0; variant < 4; ++variant)
  {
    auto specification = plugin_spec(executable, cwd, "normal");
    if ((variant & 1) != 0)
      specification.preopened_executable.emplace(mint_executable(anchors, executable));
    if ((variant & 2) != 0)
      specification.anchored_cwd.emplace(mint_cwd(anchors, cwd));
    auto child = spawn_plugin(supervisor, application, std::move(specification));
    bool const natural = child && wait_natural(supervisor, child->handle);
    ava::tests::expect_all("capability combination " + std::to_string(variant) + " launches its fast-exit child",
                           {{"launch", child.has_value()}, {"natural successful settlement", natural}});
    if (!child)
      expect(false, "capability combination " + std::to_string(variant) + ": " + child.error().format());
  }
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));

  ava::process::Supervisor mismatch_supervisor;
  auto executable_mismatch = plugin_spec(executable.string() + "-MISMATCH_CANARY", "/", "normal");
  executable_mismatch.preopened_executable.emplace(mint_executable(anchors, executable));
  auto executable_result = spawn_plugin(mismatch_supervisor, application, std::move(executable_mismatch));
  auto other_cwd = root / "OTHER_CWD_CANARY";
  auto cwd_mismatch = plugin_spec(executable, other_cwd, "normal");
  cwd_mismatch.anchored_cwd.emplace(mint_cwd(anchors, cwd));
  auto cwd_result = spawn_plugin(mismatch_supervisor, application, std::move(cwd_mismatch));
  auto mismatch_snapshot = mismatch_supervisor.snapshot();
  std::string errors;
  if (!executable_result)
    errors += executable_result.error().format();
  if (!cwd_result)
    errors += cwd_result.error().format();
  bool const mismatch = !executable_result && !cwd_result && !mismatch_snapshot.monitor_started && mismatch_snapshot.live_records == 0 &&
                        errors.find(executable.string()) == std::string::npos && errors.find(cwd.string()) == std::string::npos &&
                        errors.find(other_cwd.string()) == std::string::npos;
  static_cast<void>(mismatch_supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
  expect(mismatch, "executable and cwd logical mismatches fail before fork and remain content-free");
}

std::size_t open_nonstandard_descriptor_count()
{
  std::size_t count = 0;
  for (int descriptor = STDERR_FILENO + 1; descriptor < 4096; ++descriptor)
  {
    errno = 0;
    if (::fcntl(descriptor, F_GETFD) >= 0 || errno != EBADF)
      ++count;
  }
  return count;
}

void test_descriptor_hygiene_and_failure_baselines()
{
  auto const root = create_empty_root("process-capability-fd-hygiene");
  auto const executable = root / "executable";
  auto const cwd = root / "cwd";
  copy_executable(AVA_FAKE_PROCESS_CHILD_PATH, executable);
  std::filesystem::create_directory(cwd);
  auto const baseline = open_nonstandard_descriptor_count();
  {
    auto anchors = anchors_for(root);
    auto executable_capability = mint_executable(anchors, executable);
    auto cwd_capability = mint_cwd(anchors, cwd);
    expect(executable_capability.valid() && cwd_capability.valid(), "factory fd baseline fixture mints both capabilities");
  }
  bool const drop_baseline = open_nonstandard_descriptor_count() == baseline;
  {
    auto anchors = anchors_for(root);
    auto expected = identity_for(executable);
    ++expected.inode;
    auto failed = ava::process::mint_preopened_executable(anchors, executable, expected);
    anchors.reset();
    expect(!failed, "factory fd baseline fixture produces an expected-identity failure");
  }
  bool const factory_failure_baseline = open_nonstandard_descriptor_count() == baseline;

  bool child_clean = false;
  {
    auto anchors = anchors_for(root);
    auto specification = plugin_spec(executable, cwd, "check-only-standard-fds");
    specification.stdout_mode = ava::process::StreamModeV1::Capture;
    specification.preopened_executable.emplace(mint_executable(anchors, executable));
    specification.anchored_cwd.emplace(mint_cwd(anchors, cwd));
    anchors.reset();
    auto application = application_owner();
    ava::process::Supervisor supervisor;
    auto child = spawn_plugin(supervisor, application, std::move(specification));
    std::string output;
    if (child && child->standard_output)
      output = read_endpoint(*child->standard_output);
    child_clean = child && wait_natural(supervisor, child->handle) && output.find("CLEAN\n") != std::string::npos;
    static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
  }
  bool const success_baseline = open_nonstandard_descriptor_count() == baseline;

  {
    auto anchors = anchors_for(root);
    auto capability = mint_executable(anchors, executable);
    anchors.reset();
    if (::chmod(executable.c_str(), 0500) != 0)
      throw std::runtime_error("failed to prepare freshness fd baseline");
    auto application = application_owner();
    ava::process::Supervisor supervisor;
    auto specification = plugin_spec(executable, "/", "normal");
    specification.preopened_executable.emplace(std::move(capability));
    auto failed = spawn_plugin(supervisor, application, std::move(specification));
    expect(!failed && !supervisor.snapshot().monitor_started, "freshness fd baseline fixture fails before fork");
    static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
  }
  bool const freshness_failure_baseline = open_nonstandard_descriptor_count() == baseline;
  expect(drop_baseline && factory_failure_baseline && child_clean && success_baseline && freshness_failure_baseline,
         "capability drop/factory/freshness paths restore parent fd baseline and a both-capability child sees only standard descriptors");
}

void test_descriptor_script_and_required_secure_cwd()
{
  auto const root = create_empty_root("process-capability-script");
  auto const script = root / "descriptor-script";
  auto const marker = root / "SCRIPT_FALLBACK_MUST_NOT_RUN";
  ava::tests::write_test_file(script, "#!/bin/sh\nprintf fallback > '" + marker.string() + "'\n", 0700);
  auto anchors = anchors_for(root);
  auto application = application_owner();
  ava::process::Supervisor supervisor;
  auto specification = plugin_spec(script, "/", "unused");
  specification.argv = {script.string()};
  specification.preopened_executable.emplace(mint_executable(anchors, script));
  auto result = spawn_plugin(supervisor, application, std::move(specification));
  auto snapshot = supervisor.snapshot();
  bool const typed_exec_failure = !result && result.error().format().find("exec syscall returned") != std::string::npos && snapshot.records.size() == 1 &&
                                  snapshot.records.front().reason == ava::process::TerminationReasonV1::ExecFailed && !std::filesystem::exists(marker);
  static_cast<void>(supervisor.shutdown(ava::tests::now_plus_seconds(2s)));

  ava::process::Supervisor adoption_supervisor;
  auto reservation = adoption_supervisor.reserve(operation_owner(application), ava::process::ProcessRoleV1::Bash);
  auto missing = reservation
                     ? adoption_supervisor.begin_secure_adoption(std::move(*reservation), {.environment = bash_environment("/"),
                                                                                           .argv = {AVA_FAKE_PROCESS_CHILD_PATH, "normal"},
                                                                                           .cwd = "/",
                                                                                           .bash_containment = ava::process::BashContainmentHandshakeV1::None})
                     : ava::core::Result<ava::process::AdoptionGate>(std::unexpected(reservation.error()));
  auto adoption_snapshot = adoption_supervisor.snapshot();
  bool const missing_rejected = !missing && !adoption_snapshot.monitor_started && adoption_snapshot.live_records == 0;
  static_cast<void>(adoption_supervisor.shutdown(ava::tests::now_plus_seconds(2s)));
  expect(typed_exec_failure && missing_rejected,
         "a CLOEXEC descriptor script is one typed ExecFailed with no pathname fallback, and secure adoption rejects a missing anchored cwd pre-fork");
}

std::string read_all(int descriptor)
{
  std::string result;
  std::array<char, 128> buffer{};
  while (true)
  {
    auto const count = ::read(descriptor, buffer.data(), buffer.size());
    if (count > 0)
      result.append(buffer.data(), static_cast<std::size_t>(count));
    else if (count == 0)
      return result;
    else if (errno != EINTR)
      return result;
  }
}

void test_anchor_open_external_hardening_contract()
{
  auto const root = create_empty_root("process-capability-anchor-open");
  auto const workspace = root / "workspace";
  auto const external = root / "external";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(external);
  auto anchors = anchors_for(workspace);
  auto const contained_target = workspace / "contained-target";
  auto const contained_link = workspace / "contained-link";
  auto const external_target = external / "external-target";
  auto const external_retained = external / "external-retained-a";
  auto const external_link = external / "external-link";
  auto const anchor_alias = external / "anchor-alias";
  ava::tests::write_test_file(contained_target, "contained-a", 0600);
  ava::tests::write_test_file(external_target, "external-a", 0600);
  if (::symlink(contained_target.filename().c_str(), contained_link.c_str()) != 0 ||
      ::symlink(external_target.filename().c_str(), external_link.c_str()) != 0 || ::symlink(contained_target.c_str(), anchor_alias.c_str()) != 0)
  {
    throw std::runtime_error("failed to prepare AnchorOpen symlinks");
  }

  auto contained = ava::core::open_readable(*anchors, contained_link, O_RDONLY | O_CLOEXEC);
  auto contained_nofollow = ava::core::open_readable(*anchors, contained_link, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  auto external_open = ava::core::open_readable(*anchors, external_link, O_RDONLY | O_CLOEXEC);
  auto external_nofollow = ava::core::open_readable(*anchors, external_link, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  auto excluded = ava::core::open_readable(*anchors, anchor_alias, O_RDONLY | O_CLOEXEC);
  auto root_open = ava::core::open_readable(*anchors, "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  bool const initial = contained && read_all(contained->fd()) == "contained-a" && !contained_nofollow && external_open &&
                       read_all(external_open->fd()) == "external-a" && !external_nofollow && !excluded && root_open;

  auto latch = std::make_shared<Latch>();
  ava::core::testing::AnchorOpenTestAccess::set_before_external_reopen_hook([latch] { latch->arrive_and_wait(); });
  std::optional<ava::core::Result<ava::core::AnchorOpen>> raced_open;
  std::thread opener([&] { raced_open.emplace(ava::core::open_readable(*anchors, external_target, O_RDONLY | O_CLOEXEC)); });
  bool const inspected = latch->wait_until(ava::tests::now_plus_seconds(2s));
  if (inspected)
  {
    std::filesystem::rename(external_target, external_retained);
    ava::tests::write_test_file(external_target, "external-b", 0600);
  }
  latch->release();
  opener.join();
  ava::core::testing::AnchorOpenTestAccess::clear_before_external_reopen_hook();
  auto replacement_open = ava::core::open_readable(*anchors, external_target, O_RDONLY | O_CLOEXEC);
  bool const replacement = inspected && raced_open && !*raced_open && replacement_open && read_all(replacement_open->fd()) == "external-b";
  expect(initial && replacement,
         "AnchorOpen preserves contained/external symlinks and root reads, excludes writable-anchor entry, honors logical O_NOFOLLOW, and rejects a "
         "deterministic final replacement identity race");
}

#endif

}  // namespace

void run_process_capability_posix_tests()
{
#if defined(_WIN32)
  ava::tests::request_skip("anchored process execution capabilities are unsupported on Windows");
#else
  test_api_shape_paths_and_exact_identity();
  test_mint_identity_races();
  test_executable_policy();
  test_post_mint_mutations_and_pre_spawn_replacement();
  test_post_fork_executable_replacement_uses_inherited_a();
  test_cwd_replacement_common_and_secure();
  test_capability_combinations_and_logical_mismatch();
  test_descriptor_hygiene_and_failure_baselines();
  test_descriptor_script_and_required_secure_cwd();
  test_anchor_open_external_hardening_contract();
#endif
}
