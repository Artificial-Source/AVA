#include "sys.h"
#include "tests/support/syscall_errno_injection.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/command/command.h"
#include "ava/containment/containment.h"
#include "ava/agent/mode.h"
#include "ava/tools/bash_tool.h"
#include "ava/permissions/permission.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/trusted_home.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <grp.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <pwd.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace command = ava::command;
namespace containment = ava::containment;
namespace perms = ava::permissions;

bool landlock_available()
{
  return containment::probe_landlock_abi_version() >= containment::kRequiredLandlockAbiVersion;
}

bool python3_available()
{
  return std::filesystem::exists("/usr/bin/python3");
}

std::optional<gid_t> private_primary_group_for_test()
{
  std::array<char, 16 * 1024> password_storage{};
  passwd account{};
  passwd* resolved_account = nullptr;
  if (::getpwuid_r(::geteuid(), &account, password_storage.data(), password_storage.size(), &resolved_account) != 0 || !resolved_account ||
      !resolved_account->pw_name || resolved_account->pw_name[0] == '\0')
  {
    return std::nullopt;
  }

  std::array<char, 16 * 1024> group_storage{};
  group primary_group{};
  group* resolved_group = nullptr;
  if (::getgrgid_r(resolved_account->pw_gid, &primary_group, group_storage.data(), group_storage.size(), &resolved_group) != 0 || !resolved_group ||
      !resolved_group->gr_name || std::string_view(resolved_group->gr_name) != resolved_account->pw_name)
  {
    return std::nullopt;
  }
  for (auto const* const* member = resolved_group->gr_mem; member && *member; ++member)
  {
    if (std::string_view(*member) != resolved_account->pw_name)
      return std::nullopt;
  }
  return resolved_group->gr_gid;
}

struct ContainmentFixture
{
  std::filesystem::path root;
  std::filesystem::path workspace;
  std::filesystem::path bin;
  std::filesystem::path build_dir;
  std::filesystem::path secret_file;
  std::filesystem::path ava_authority_root;
  std::filesystem::path spill;
  std::shared_ptr<ava::core::AnchorSet> anchors;

  explicit ContainmentFixture(std::string_view name, mode_t workspace_mode = S_IRWXU)
      : root(temp_root() / ("containment-" + std::string(name))),
        workspace(root / "workspace"),
        bin(root / "bin"),
        build_dir(workspace / "build"),
        secret_file(root / "secret" / "test-secret"),
        ava_authority_root(root / "ava-authority"),
        spill(root / "spill")
  {
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
    std::filesystem::create_directories(workspace);
    std::filesystem::create_directories(bin);
    std::filesystem::create_directories(build_dir);
    std::filesystem::create_directories(secret_file.parent_path());
    std::filesystem::create_directories(ava_authority_root);
    std::filesystem::create_directories(spill);
    ::chmod(temp_root().c_str(), S_IRWXU);
    ::chmod(root.c_str(), S_IRWXU);
    ::chmod(workspace.c_str(), workspace_mode);
    ::chmod(bin.c_str(), S_IRWXU);
    ::chmod(build_dir.c_str(), S_IRWXU);
    ::chmod(secret_file.parent_path().c_str(), S_IRWXU);
    ::chmod(ava_authority_root.c_str(), S_IRWXU);
    ::chmod(spill.c_str(), S_IRWXU);
    {
      std::ofstream f(secret_file, std::ios::binary | std::ios::trunc);
      f << "secret-data\n";
    }
    ::chmod(secret_file.c_str(), S_IRUSR | S_IWUSR);
    auto opened = ava::core::AnchorSet::open({workspace, spill});
    if (!opened)
      throw std::runtime_error("failed to open containment test AnchorSet");
    anchors = *opened;
  }

  bool write_executable(std::string_view name, std::string_view body)
  {
    auto path = bin / std::string(name);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << body;
    f.close();
    return ::chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0;
  }

  ava::tools::ToolContext make_context()
  {
    return ava::tools::ToolContext{
        .workspace_dir = workspace, .spill_dir = spill, .mode = ava::agent::Mode::Build, .anchor_set = anchors, .ava_authority_roots = {ava_authority_root}};
  }

  ava::tools::ToolContext make_allow_context()
  {
    return ava::tools::ToolContext{.workspace_dir = workspace,
                                   .spill_dir = spill,
                                   .mode = ava::agent::Mode::Build,
                                   .permission_resolver = [](perms::PermissionPrompt const&) -> ava::core::Result<perms::PermissionResolutionDecision> {
                                     return perms::PermissionResolution::Allow;
                                   },
                                   .anchor_set = anchors,
                                   .ava_authority_roots = {ava_authority_root}};
  }
};

command::CommandBuildOptions containment_options_for_test(ContainmentFixture const& fix)
{
  auto const home = fix.root / "trusted-home";
  std::filesystem::create_directories(home);
  ::chmod(home.c_str(), S_IRWXU);
  for (auto const* name : {"home", "config", "cache", "data", "state", "tmp"})
  {
    auto const directory = fix.spill / name;
    std::filesystem::create_directory(directory);
    ::chmod(directory.c_str(), S_IRWXU);
  }
  return command::CommandBuildOptions{.workspace = fix.workspace,
                                      .anchor_set = fix.anchors,
                                      .trusted_home = home,
                                      .discover_host_user_tools = false,
                                      .startup_path = "/usr/bin:/bin",
                                      .shell = "/bin/sh",
                                      .ava_authority_roots = {fix.ava_authority_root},
                                      .environment = command::CommandEnvironmentOptions{.profile_id = "containment-test-environment",
                                                                                        .user = "test-user",
                                                                                        .logname = "test-user",
                                                                                        .home = fix.spill / "home",
                                                                                        .xdg_config_home = fix.spill / "config",
                                                                                        .xdg_cache_home = fix.spill / "cache",
                                                                                        .xdg_data_home = fix.spill / "data",
                                                                                        .xdg_state_home = fix.spill / "state",
                                                                                        .tmpdir = fix.spill / "tmp"},
                                      .workspace_script_recipes = {},
                                      .limits = {}};
}

// ---------------------------------------------------------------------------
// Unit-level tests: these run without Landlock and are never skipped.
// ---------------------------------------------------------------------------

void test_containment_summary_redacts_paths()
{
  ContainmentFixture fix("summary");
  auto plan = containment::DevelopmentContainmentPlan{};
  plan.availability = containment::ContainmentAvailability::Available;
  plan.profile_id = "ava-landlock-seccomp-v1";
  plan.network_allowed = false;
  plan.network_filter_planned = true;
  plan.filesystem_scope.workspace_writable = true;
  auto summary = containment::containment_summary(plan);
  expect(summary.find("available") != std::string::npos, "containment summary shows availability");
  expect(summary.find("\"network_allowed\":false") != std::string::npos, "containment summary shows network denied");
  expect(summary.find("\"network_filter\":\"planned\"") != std::string::npos, "containment summary says planned before fork");
  expect(summary.find(fix.workspace.string()) == std::string::npos, "containment summary does not leak workspace paths");
  expect(summary.find(fix.secret_file.string()) == std::string::npos, "containment summary does not leak secret paths");
}

void test_containment_fd_closure_keeps_bound_executable()
{
  std::array<int, 2> report{-1, -1};
  bool const report_pipe_created = ::pipe2(report.data(), O_CLOEXEC) == 0;
  int const handshake_fd = report_pipe_created ? ::open("/dev/null", O_RDONLY | O_CLOEXEC) : -1;
  int const executable_fd = report_pipe_created ? ::open("/dev/null", O_RDONLY | O_CLOEXEC) : -1;
  int const inherited_fd = report_pipe_created ? ::open("/dev/null", O_RDONLY | O_CLOEXEC) : -1;
  std::array<int, 4> child_state{-1, -1, -1, -1};
  pid_t const child = report_pipe_created && handshake_fd >= 0 && executable_fd >= 0 && inherited_fd >= 0 ? ::fork() : -1;
  if (child == 0)
  {
    static_cast<void>(::close(report[0]));
    containment::close_inherited_fds_except(report[1], handshake_fd, executable_fd);
    child_state = {::fcntl(report[1], F_GETFD), ::fcntl(handshake_fd, F_GETFD), ::fcntl(executable_fd, F_GETFD), ::fcntl(inherited_fd, F_GETFD)};
    bool const reported = write_all_to_descriptor_for_test(report[1], child_state.data(), sizeof(child_state));
    static_cast<void>(::close(report[1]));
    _exit(reported ? 0 : 2);
  }
  if (report[1] >= 0)
    static_cast<void>(::close(report[1]));
  if (handshake_fd >= 0)
    static_cast<void>(::close(handshake_fd));
  if (executable_fd >= 0)
    static_cast<void>(::close(executable_fd));
  if (inherited_fd >= 0)
    static_cast<void>(::close(inherited_fd));

  std::array<int, 4> observed{-1, -1, -1, -1};
  bool const report_read = report_pipe_created && read_exact_from_descriptor_for_test(report[0], observed.data(), sizeof(observed));
  if (report[0] >= 0)
    static_cast<void>(::close(report[0]));
  int status = 0;
  if (child > 0)
    static_cast<void>(::waitpid(child, &status, 0));
  expect(report_read, "containment fd closure report transfers to the parent");
  expect(report_pipe_created && child > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 && observed[0] >= 0 && observed[1] >= 0 && observed[2] >= 0 &&
             observed[3] == -1,
         "containment fd closure preserves handshake and approved-executable descriptors while closing inherited descriptors");
}

void test_unavailable_probe_forces_ask()
{
  ContainmentFixture fix("unavailable-ask");
  fix.write_executable("cmake", "#!/bin/sh\nexit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  int prompts = 0;
  bool critical_risk = false;
  ava::tools::ToolContext ctx{
      .workspace_dir = fix.workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [&prompts, &critical_risk](perms::PermissionPrompt const& p) -> ava::core::Result<perms::PermissionResolutionDecision> {
        ++prompts;
        if (p.command_metadata && !p.command_metadata->containment_available)
        {
          critical_risk = (p.risk == perms::PermissionRisk::Critical);
          return perms::PermissionResolution::Allow;
        }
        return perms::PermissionResolution::Deny;
      }};
  ctx.spill_dir = fix.spill;
  ctx.anchor_set = fix.anchors;
  auto result =
      ava::tools::run_bash(ctx, "cmake --build " + fix.build_dir.generic_string(), ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  if (landlock_available())
  {
    expect(result && result->exit_code == 0, "when Landlock is available, standard mutable command auto-allows without resolver");
  }
  else
  {
    // Unavailable containment must downgrade to Critical risk Ask, not
    // ordinary High risk Ask. After explicit one-shot approval it runs
    // uncontained, but metadata must state unavailable/uncontained.
    expect(result && prompts == 1, "when containment is unavailable, standard mutable command downgrades to Ask");
    expect(critical_risk, "unavailable containment downgrades to Critical risk, not ordinary High");
    if (result)
    {
      expect(!result->containment_applied, "unavailable containment reports not applied in result");
    }
  }
}

void test_sensitive_network_enabled_remains_ask()
{
  ContainmentFixture fix("sensitive-network");
  fix.write_executable("curl", "#!/bin/sh\necho curl-ran\nexit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  int prompts = 0;
  bool network_allowed_in_metadata = false;
  ava::tools::ToolContext ctx{.workspace_dir = fix.workspace,
                              .mode = ava::agent::Mode::Build,
                              .permission_resolver = [&prompts, &network_allowed_in_metadata](
                                                         perms::PermissionPrompt const& p) -> ava::core::Result<perms::PermissionResolutionDecision> {
                                ++prompts;
                                if (p.command_metadata)
                                {
                                  network_allowed_in_metadata = p.command_metadata->containment_network_allowed;
                                }
                                return perms::PermissionResolution::Allow;
                              }};
  ctx.spill_dir = fix.spill;
  ctx.anchor_set = fix.anchors;
  auto result = ava::tools::run_bash(ctx, "curl https://example.com", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && prompts == 1, "sensitive network-enabled command remains Ask");
  expect(network_allowed_in_metadata, "sensitive network-enabled command metadata states network permitted after approval");
}

void test_critical_raw_remains_ask_no_containment()
{
  ContainmentFixture fix("critical-raw");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  int prompts = 0;
  bool containment_not_required = false;
  ava::tools::ToolContext ctx{
      .workspace_dir = fix.workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [&prompts, &containment_not_required](perms::PermissionPrompt const& p) -> ava::core::Result<perms::PermissionResolutionDecision> {
        ++prompts;
        if (p.command_metadata)
        {
          containment_not_required = p.command_metadata->containment_status == perms::CommandContainmentStatus::NotRequired ||
                                     p.command_metadata->containment_status == perms::CommandContainmentStatus::Unavailable;
        }
        return perms::PermissionResolution::Deny;
      }};
  ctx.spill_dir = fix.spill;
  ctx.anchor_set = fix.anchors;
  auto result = ava::tools::run_bash(ctx, "printf raw; true", ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(!result && prompts == 1 && containment_not_required, "critical/raw command remains Ask once with no containment claim in metadata");
}

void test_home_as_workspace_rejected()
{
  // Reject a workspace that equals or is an ancestor of the trusted home.
  // Ordinary projects nested under the trusted home are allowed.
  //
  // The boundary rule operates on CommandBuildOptions::trusted_home. Obtain that
  // path from ava::core::cached_trusted_account() -- the process-wide trusted
  // home, and the single place HOME is read -- rather than reading $HOME here.
  // To exercise the rule we need a directory whose ancestor chain is owner-safe
  // so it passes the sealing ancestor-safety validation; /tmp-based temp space
  // does not qualify (its ancestors are world-writable). Create a per-process
  // scratch directory under the trusted home to stand in for it; when the
  // trusted home is mounted read-only (some CI containers) fall back to a
  // scratch directory under the build tree, which is writable and owner-safe.
  // Because the stand-in is always a directory we create, cleanup is uniform and
  // never touches a real home directory.
  std::error_code ec;
  std::filesystem::path trusted_home;
  auto const boundary_name = "ava-test-home-boundary-" + std::to_string(::getpid());

  {
    auto candidate = ava::core::cached_trusted_account().home / boundary_name;
    std::filesystem::create_directories(candidate, ec);
    if (!ec)
      trusted_home = std::move(candidate);
  }
#ifdef AVA_TESTS_BINARY_DIR
  // The trusted home may be mounted read-only (some CI containers); fall back to
  // a scratch directory under the build tree, which is writable and whose
  // ancestor chain is owner-safe.
  if (trusted_home.empty())
  {
    auto candidate = std::filesystem::path(AVA_TESTS_BINARY_DIR) / boundary_name;
    std::filesystem::create_directories(candidate, ec);
    if (!ec)
      trusted_home = std::move(candidate);
  }
#endif
  if (trusted_home.empty())
  {
    ava::tests::request_skip("no writable trusted-home stand-in with a safe ancestor chain available");
    return;
  }
  // create_directories applies the process umask to 0777. The trusted-home
  // validator intentionally rejects group-writable roots, so make this fixture
  // deterministic even when the invoking shell uses umask 0002.
  if (::chmod(trusted_home.c_str(), S_IRWXU) != 0)
  {
    std::filesystem::remove_all(trusted_home, ec);
    ava::tests::request_skip("cannot secure the trusted-home stand-in to mode 0700");
    return;
  }

  auto project_under_home = trusted_home / "ava-test-project-boundary";
  std::filesystem::create_directories(project_under_home, ec);
  ::chmod(project_under_home.c_str(), S_IRWXU);
  // Synthetic roots must be disjoint from workspace and trusted_home; use
  // temp_root() so they never overlap.
  auto synth_base = temp_root() / "ava-boundary-synth";
  std::filesystem::create_directories(synth_base, ec);
  ::chmod(synth_base.c_str(), S_IRWXU);
  for (auto const* d : {"home", "cfg", "cache", "data", "state", "tmp"})
  {
    auto p = synth_base / d;
    std::filesystem::create_directories(p, ec);
    ::chmod(p.c_str(), S_IRWXU);
  }
  command::CommandLimits limits;
  auto home_anchors = ava::core::AnchorSet::open({trusted_home, synth_base});
  expect(home_anchors.has_value(), "home boundary test opens its shared AnchorSet");
  if (!home_anchors)
    return;
  command::CommandBuildOptions opts{.workspace = trusted_home,
                                    .anchor_set = *home_anchors,
                                    .trusted_home = trusted_home,
                                    .startup_path = "/usr/bin:/bin",
                                    .shell = "/bin/sh",
                                    .ava_authority_roots = {},
                                    .environment = command::CommandEnvironmentOptions{.profile_id = "test",
                                                                                      .user = "test",
                                                                                      .logname = "test",
                                                                                      .home = synth_base / "home",
                                                                                      .xdg_config_home = synth_base / "cfg",
                                                                                      .xdg_cache_home = synth_base / "cache",
                                                                                      .xdg_data_home = synth_base / "data",
                                                                                      .xdg_state_home = synth_base / "state",
                                                                                      .tmpdir = synth_base / "tmp"},
                                    .workspace_script_recipes = {},
                                    .limits = limits};
  auto intent = command::CommandIntent::compatibility("ls", limits);
  auto plan = command::seal_command_plan(*intent, opts);
  expect(!plan && plan.error().category() == ava::core::ErrorCategory::PermissionDenied &&
             plan.error().message() == "command workspace must not equal or contain the trusted real home directory",
         "sealing rejects workspace specifically at the trusted-home boundary");

  // Positive: a project nested under the trusted home is allowed.
  auto project_anchors = ava::core::AnchorSet::open({project_under_home, synth_base});
  expect(project_anchors.has_value(), "nested project boundary test opens its shared AnchorSet");
  if (!project_anchors)
    return;
  opts.workspace = project_under_home;
  opts.anchor_set = *project_anchors;
  auto nested_intent = command::CommandIntent::compatibility("ls", limits);
  auto nested_plan = command::seal_command_plan(*nested_intent, opts);
  expect(nested_plan.has_value(), "sealing allows ordinary projects nested under home");

  std::filesystem::remove_all(trusted_home, ec);
  std::filesystem::remove_all(synth_base, ec);
}

void test_authority_overlap_rejected()
{
  ContainmentFixture fix("authority-overlap");
  // Reject workspace that overlaps with an AVA authority root.
  command::CommandLimits limits;
  auto opts = containment_options_for_test(fix);
  auto const& synth_root = fix.spill;
  auto anchors = ava::core::AnchorSet::open({fix.ava_authority_root, synth_root});
  expect(anchors.has_value(), "authority overlap test opens its shared AnchorSet");
  if (!anchors)
    return;
  opts.workspace = fix.ava_authority_root;
  opts.anchor_set = *anchors;
  opts.discover_host_user_tools = true;
  opts.environment.profile_id = "test";
  opts.environment.user = "test";
  opts.environment.logname = "test";
  opts.limits = limits;
  auto intent = command::CommandIntent::compatibility("ls", limits);
  auto plan = command::seal_command_plan(*intent, opts);
  expect(!plan, "sealing rejects workspace that overlaps with an AVA authority root");
}

// ---------------------------------------------------------------------------
// Feature probe and Landlock-dependent tests.
// ---------------------------------------------------------------------------

void test_feature_probe()
{
  auto const abi = containment::probe_landlock_abi_version();
  auto const seccomp_ok = containment::seccomp_network_filter_supported();
  if (abi >= containment::kRequiredLandlockAbiVersion && seccomp_ok)
  {
    expect(abi >= 3, "Landlock ABI version meets the minimum requirement for truncate/refer mediation");
    expect(seccomp_ok, "seccomp network filter is supported on this architecture");
  }
  else
  {
    ava::tests::request_skip("kernel lacks required Landlock ABI or seccomp support");
  }
}

void test_replaced_external_path_rule_fails_child_application()
{
  if (!landlock_available())
  {
    ava::tests::request_skip("Landlock ABI insufficient");
    return;
  }

  ContainmentFixture fix("replaced-external-path-rule");
  expect(fix.write_executable("cmake", "#!/bin/sh\nexit 0\n"), "replacement-race fixture creates a known contained command");

  auto options = containment_options_for_test(fix);
  options.startup_path = fix.bin.string();
  auto const& environment = options.environment;
  auto anchors = ava::core::AnchorSet::open({fix.workspace, environment.home, environment.xdg_config_home, environment.xdg_cache_home,
                                             environment.xdg_data_home, environment.xdg_state_home, environment.tmpdir});
  expect(anchors.has_value() && !(*anchors)->contains_lexical(fix.bin), "replacement-race PATH directory is sealed as an external path");
  if (!anchors)
    return;

  options.anchor_set = *anchors;
  auto intent = command::CommandIntent::structured({"cmake", "--build", "build"});
  auto preparation = intent ? command::prepare_command(*intent, options)
                            : ava::core::Result<command::CommandPreparation>{
                                  std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "no contained command intent"))};
  expect(preparation.has_value(), "replacement-race command preparation succeeds");
  if (!preparation)
    return;

  auto const path_entry = std::ranges::find_if(preparation->plan().path_entries(),
                                               [&](command::CommandPathEntry const& entry) { return entry.directory == fix.bin.lexically_normal(); });
  auto plan = containment::prepare_development_containment(*preparation, false);
  auto const target_rule = std::ranges::find_if(
      plan.filesystem_rules, [&](containment::ContainmentFilesystemRule const& rule) { return rule.logical_path == fix.bin.lexically_normal(); });
  expect(path_entry != preparation->plan().path_entries().end() && plan.availability == containment::ContainmentAvailability::Available &&
             target_rule != plan.filesystem_rules.end() && target_rule->identity_bound,
         "containment plan includes an identity-bound rule for the external sealed PATH directory");
  if (path_entry == preparation->plan().path_entries().end() || plan.availability != containment::ContainmentAvailability::Available ||
      target_rule == plan.filesystem_rules.end() || !target_rule->identity_bound)
    return;

  auto const approved_directory = fix.root / "approved-path-original";
  std::error_code replacement_error;
  std::filesystem::rename(fix.bin, approved_directory, replacement_error);
  bool const approved_directory_moved = !replacement_error;
  if (approved_directory_moved)
  {
    std::filesystem::create_directory(fix.bin, replacement_error);
    if (!replacement_error && ::chmod(fix.bin.c_str(), S_IRWXU) != 0)
      replacement_error = std::error_code(errno, std::generic_category());
  }
  if (replacement_error)
  {
    if (approved_directory_moved)
    {
      std::error_code cleanup_error;
      std::filesystem::remove_all(fix.bin, cleanup_error);
      cleanup_error.clear();
      std::filesystem::rename(approved_directory, fix.bin, cleanup_error);
    }
    expect(false, "replacement-race fixture replaces the approved PATH directory");
    return;
  }

  pid_t const child = ::fork();
  if (child == 0)
  {
    auto const applied = containment::apply_containment_in_child(plan);
    _exit(applied ? 91 : 90);
  }

  std::optional<int> child_status;
  if (child > 0)
  {
    auto const deadline = ava::tests::now_plus_seconds(2);
    child_status = ava::tests::wait_child_until(child, deadline);
  }

  std::error_code restore_error;
  std::filesystem::remove_all(fix.bin, restore_error);
  bool const replacement_removed = !restore_error;
  restore_error.clear();
  std::filesystem::rename(approved_directory, fix.bin, restore_error);
  bool const restored = !restore_error;

  expect(child_status && WIFEXITED(*child_status) && WEXITSTATUS(*child_status) == 90,
         "child containment application rejects a replaced external PATH rule before accepting or executing the command");
  expect(replacement_removed && restored, "replacement-race fixture restores the approved PATH directory");
}

void test_contained_command_writes_workspace()
{
  if (!landlock_available())
  {
    ava::tests::request_skip("Landlock ABI insufficient");
    return;
  }
  ContainmentFixture fix("writes-workspace");
  fix.write_executable("cmake", "#!/bin/sh\necho cmake-ran > " + (fix.build_dir / "marker.txt").string() + "\nexit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0, "contained standard project command succeeds under development containment");
  expect(std::filesystem::exists(fix.build_dir / "marker.txt"), "contained command writes to workspace build directory");
}

void test_contained_command_writes_private_primary_group_workspace()
{
  if (!landlock_available())
  {
    ava::tests::request_skip("Landlock ABI insufficient");
    return;
  }
  auto const private_group = private_primary_group_for_test();
  if (!private_group)
    return;

  ContainmentFixture fix("writes-private-primary-group", S_IRWXU | S_IRWXG);
  if (::chown(fix.workspace.c_str(), ::geteuid(), *private_group) != 0)
  {
    expect(false, "private-primary-group containment fixture changes workspace group");
    return;
  }
  fix.write_executable("cmake", "#!/bin/sh\necho cmake-ran-private-group > " + (fix.build_dir / "marker.txt").string() + "\nexit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && std::filesystem::exists(fix.build_dir / "marker.txt"),
         "contained standard project command succeeds under a private-primary-group 0770 workspace");
  if (result)
  {
    expect(result->containment_applied, "private-primary-group workspace containment is applied and reported in result");
  }
}

void test_contained_command_writes_0755_workspace()
{
  if (!landlock_available())
  {
    ava::tests::request_skip("Landlock ABI insufficient");
    return;
  }
  // A writable workspace may be current-user-owned 0755 (group read/execute)
  // but must still pass containment validation.
  ContainmentFixture fix("writes-0755", S_IRWXU | S_IRGRP | S_IXGRP);
  fix.write_executable("cmake", "#!/bin/sh\necho cmake-ran-0755 > " + (fix.build_dir / "marker.txt").string() + "\nexit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0, "contained standard project command succeeds under 0755 workspace");
  expect(std::filesystem::exists(fix.build_dir / "marker.txt"), "contained command writes to 0755 workspace build directory");
  if (result)
  {
    expect(result->containment_applied, "0755 workspace containment is applied and reported in result");
  }
}

void test_contained_command_reads_system_libs()
{
  if (!landlock_available())
  {
    ava::tests::request_skip("Landlock ABI insufficient");
    return;
  }
  ContainmentFixture fix("reads-system");
  fix.write_executable("cmake", "#!/bin/sh\ncat /etc/passwd > /dev/null 2>&1 && echo SYS_READ_OK || echo SYS_READ_FAIL\nexit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && result->output.find("SYS_READ_OK") != std::string::npos,
         "contained command can read system files under allowed system roots");
}

void test_contained_command_cannot_read_secret()
{
  if (!landlock_available())
  {
    ava::tests::request_skip("Landlock ABI insufficient");
    return;
  }
  ContainmentFixture fix("cannot-read-secret");
  fix.write_executable("cmake", "#!/bin/sh\ncat " + fix.secret_file.string() + " > /dev/null 2>&1 && echo SECRET_READ_BAD || echo SECRET_BLOCKED\nexit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && result->output.find("SECRET_BLOCKED") != std::string::npos,
         "contained command cannot read a test secret file outside the workspace");
}

void test_contained_command_cannot_write_ava_authority()
{
  if (!landlock_available())
  {
    ava::tests::request_skip("Landlock ABI insufficient");
    return;
  }
  ContainmentFixture fix("cannot-write-authority");
  auto const authority_marker = fix.ava_authority_root / "marker.txt";
  fix.write_executable("cmake", "#!/bin/sh\necho bad > " + authority_marker.string() + " 2>/dev/null && echo AUTH_WRITE_BAD || echo AUTH_BLOCKED\nexit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && result->output.find("AUTH_BLOCKED") != std::string::npos && !std::filesystem::exists(authority_marker),
         "contained command cannot write to AVA authority test root");
}

void test_contained_command_cannot_open_outbound_network()
{
  if (!landlock_available() || !python3_available())
  {
    ava::tests::request_skip("Landlock ABI or python3 insufficient");
    return;
  }
  ContainmentFixture fix("network-denied");
  fix.write_executable("cmake",
                       "#!/bin/sh\n"
                       "python3 -c 'import socket; s=socket.socket(socket.AF_INET,socket.SOCK_STREAM)' 2>&1\n"
                       "if [ $? -ne 0 ]; then echo NETWORK_BLOCKED; else echo NETWORK_OK; fi\n"
                       "exit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && result->output.find("NETWORK_BLOCKED") != std::string::npos,
         "contained command with network denied cannot create outbound IP socket");
}

void test_contained_command_cannot_open_unix_socket()
{
  if (!landlock_available() || !python3_available())
  {
    ava::tests::request_skip("Landlock ABI or python3 insufficient");
    return;
  }
  // Network-denied profile must block ALL socket creation, including
  // AF_UNIX/abstract sockets, not just AF_INET/AF_INET6.
  ContainmentFixture fix("unix-denied");
  fix.write_executable("cmake",
                       "#!/bin/sh\n"
                       "python3 -c 'import socket; s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)' 2>&1\n"
                       "if [ $? -ne 0 ]; then echo UNIX_BLOCKED; else echo UNIX_OK; fi\n"
                       "exit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && result->output.find("UNIX_BLOCKED") != std::string::npos,
         "contained command with network denied cannot create AF_UNIX socket");
}

void test_contained_command_cannot_connect_abstract()
{
  if (!landlock_available() || !python3_available())
  {
    ava::tests::request_skip("Landlock ABI or python3 insufficient");
    return;
  }
  // The seccomp filter blocks connect() so even an abstract socket connect
  // cannot succeed.
  ContainmentFixture fix("abstract-denied");
  fix.write_executable("cmake",
                       "#!/bin/sh\n"
                       "python3 -c 'import socket; s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect(\"\\x00ava-test-abstract\")' 2>&1\n"
                       "if [ $? -ne 0 ]; then echo CONNECT_BLOCKED; else echo CONNECT_OK; fi\n"
                       "exit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && result->output.find("CONNECT_BLOCKED") != std::string::npos,
         "contained command with network denied cannot connect to abstract socket");
}

void test_contained_command_cannot_escape_via_symlink()
{
  if (!landlock_available())
  {
    ava::tests::request_skip("Landlock ABI insufficient");
    return;
  }
  ContainmentFixture fix("symlink-escape");
  auto const link = fix.workspace / "escape-link";
  std::error_code ec;
  std::filesystem::create_directory_symlink(fix.secret_file.parent_path(), link, ec);
  fix.write_executable("cmake",
                       "#!/bin/sh\ncat " + link.string() + "/test-secret > /dev/null 2>&1 && echo SYMLINK_ESCAPE_BAD || echo SYMLINK_BLOCKED\nexit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && result->output.find("SYMLINK_BLOCKED") != std::string::npos,
         "contained command cannot escape workspace via symlink to external directory");
}

void test_standard_contained_no_resolver_executes()
{
  if (!landlock_available())
  {
    ava::tests::request_skip("Landlock ABI insufficient");
    return;
  }
  ContainmentFixture fix("no-resolver");
  fix.write_executable("cmake", "#!/bin/sh\necho auto-ran > " + (fix.build_dir / "auto.txt").string() + "\nexit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && std::filesystem::exists(fix.build_dir / "auto.txt"),
         "standard contained command executes automatically without a resolver");
  if (result)
  {
    expect(result->containment_applied, "contained command reports containment applied in result");
    expect(!result->containment_profile_id.empty(), "contained command reports containment profile in result");
    expect(result->containment_network_mode == "denied", "contained command reports network denied mode in result");
  }
}

void test_contained_command_cancel()
{
  if (!landlock_available())
  {
    ava::tests::request_skip("Landlock ABI insufficient");
    return;
  }
  ContainmentFixture fix("cancel");
  fix.write_executable("cmake", "#!/bin/sh\nwhile true; do sleep 0.1; done\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  int cancel_checks = 0;
  ava::tools::ToolContext ctx{.workspace_dir = fix.workspace,
                              .mode = ava::agent::Mode::Build,
                              .permission_resolver = [](perms::PermissionPrompt const&) -> ava::core::Result<perms::PermissionResolutionDecision> {
                                return perms::PermissionResolution::Allow;
                              },
                              .cancel_requested =
                                  [&cancel_checks] {
                                    ++cancel_checks;
                                    return cancel_checks >= 3;
                                  }};
  ctx.spill_dir = fix.spill;
  ctx.anchor_set = fix.anchors;
  auto result =
      ava::tools::run_bash(ctx, "cmake --build " + fix.build_dir.generic_string(), ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->canceled, "contained descriptor-executed command observes cancellation and reports a canceled process result");
}

void test_contained_direct_file_recipe()
{
  if (!landlock_available())
  {
    ava::tests::request_skip("Landlock ABI insufficient");
    return;
  }
  // Regular-file Landlock rules must use only file-valid rights (READ_FILE
  // and EXECUTE), never READ_DIR. A recipe with a file path argument must
  // not fail rule installation due to an invalid READ_DIR on a file.
  // Using cmake --build <file> classifies as CmakeBuild with the file as a
  // recipe path argument. Since the file is within the writable workspace,
  // the redundant file rule is omitted, and the file is accessible through
  // the workspace writable rule.
  ContainmentFixture fix("direct-file");
  auto const test_file = fix.workspace / "test_input.txt";
  {
    std::ofstream f(test_file, std::ios::binary | std::ios::trunc);
    f << "test-content\n";
  }
  ::chmod(test_file.c_str(), S_IRUSR | S_IWUSR);
  fix.write_executable("cmake", "#!/bin/sh\ncat " + test_file.string() + " > /dev/null 2>&1 && echo FILE_READ_OK || echo FILE_READ_FAIL\nexit 0\n");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + test_file.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0, "contained command with direct-file recipe argument installs containment and executes");
  if (result)
  {
    expect(result->containment_applied, "direct-file recipe containment is applied");
  }
}

void test_network_allowed_containment_restricts_socket_domains()
{
#if defined(__linux__)
  if (!landlock_available() || !containment::seccomp_network_filter_supported() || !python3_available())
  {
    ava::tests::request_skip("Landlock, seccomp, or python3 unavailable");
    return;
  }
  ContainmentFixture fix("allowed-socket-domains");
  auto const socket_path = (fix.root / "service.sock").string();
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(address.sun_path))
  {
    expect(false, "pathname UNIX socket fixture fits sockaddr_un");
    return;
  }
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  int const listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  bool const listening = listener >= 0 && ::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && ::listen(listener, 1) == 0;
  expect(listening, "network-allowed fixture listens on a pathname UNIX socket outside allowed roots");
  if (!listening)
  {
    if (listener >= 0)
      ::close(listener);
    return;
  }
  expect(fix.write_executable("curl", R"PY(#!/bin/sh
exec /usr/bin/python3 - "$1" <<'PYTHON'
import ctypes
import errno
import socket
import sys

def denied(operation):
    try:
        operation()
    except OSError as error:
        assert error.errno == errno.EPERM, error
    else:
        raise AssertionError("socket operation unexpectedly permitted")

denied(lambda: socket.socket(socket.AF_UNIX, socket.SOCK_STREAM))
def connect_service():
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.connect(sys.argv[1])
denied(connect_service)
denied(lambda: socket.socket(socket.AF_PACKET, socket.SOCK_RAW))
denied(lambda: socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 15))
with socket.socket(socket.AF_INET, socket.SOCK_STREAM):
    pass
with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, socket.NETLINK_ROUTE):
    pass
assert socket.getaddrinfo("localhost", 80, socket.AF_UNSPEC, socket.SOCK_STREAM, 0, socket.AI_ADDRCONFIG)
for kind in (socket.SOCK_STREAM, socket.SOCK_SEQPACKET):
    left, right = socket.socketpair(socket.AF_UNIX, kind | socket.SOCK_CLOEXEC | socket.SOCK_NONBLOCK)
    with left, right:
        assert left.send(b"pair") == 4
        assert right.recv(4) == b"pair"
denied(lambda: socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM))
libc = ctypes.CDLL(None, use_errno=True)
assert libc.syscall(425, 0, None) == -1 and ctypes.get_errno() == errno.EPERM
print("ALLOWED_NETWORK_BOUNDARY_OK")
PYTHON
)PY"),
         "network-allowed fixture creates its socket-domain probe");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_allow_context(), "curl " + socket_path, ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  ::close(listener);
  expect(result && result->exit_code == 0 && result->containment_applied && result->containment_network_mode == "allowed" &&
             result->output.find("ALLOWED_NETWORK_BOUNDARY_OK") != std::string::npos,
         "network-allowed containment denies UNIX service access and other domains while permitting IP, route queries, localhost and private pairs");
#else
  ava::tests::request_skip("Linux containment unavailable");
#endif
}

void test_network_denied_containment_allows_private_socketpairs()
{
#if defined(__linux__)
  if (!landlock_available() || !containment::seccomp_network_filter_supported() || !python3_available())
  {
    ava::tests::request_skip("Landlock, seccomp, or python3 unavailable");
    return;
  }
  ContainmentFixture fix("denied-private-pairs");
  expect(fix.write_executable("cmake", R"PY(#!/bin/sh
exec /usr/bin/python3 - <<'PYTHON'
import asyncio
import ctypes
import errno
import socket

asyncio.run(asyncio.sleep(0))
for kind in (socket.SOCK_STREAM, socket.SOCK_SEQPACKET):
    left, right = socket.socketpair(socket.AF_UNIX, kind | socket.SOCK_CLOEXEC | socket.SOCK_NONBLOCK)
    with left, right:
        assert left.send(b"stream") == 6
        assert right.recv(6) == b"stream"
        assert left.sendmsg([b"message"]) == 7
        assert right.recv(7) == b"message"
        try:
            left.connect("/ava-containment-must-not-connect")
        except OSError as error:
            assert error.errno == errno.EPERM, error
        else:
            raise AssertionError("connect unexpectedly permitted")
for operation in (
    lambda: socket.socket(socket.AF_INET, socket.SOCK_STREAM),
    lambda: socket.socket(socket.AF_UNIX, socket.SOCK_STREAM),
    lambda: socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM),
):
    try:
        operation()
    except OSError as error:
        assert error.errno == errno.EPERM, error
    else:
        raise AssertionError("network operation unexpectedly permitted")
libc = ctypes.CDLL(None, use_errno=True)
assert libc.syscall(425, 0, None) == -1 and ctypes.get_errno() == errno.EPERM
assert libc.sendmmsg(-1, None, 1, 0) == -1 and ctypes.get_errno() == errno.EBADF
print("PRIVATE_PAIRS_OK")
PYTHON
)PY"),
         "network-denied fixture creates its asyncio and socketpair probe");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && result->containment_applied && result->containment_network_mode == "denied" &&
             result->output.find("PRIVATE_PAIRS_OK") != std::string::npos,
         "network-denied containment permits asyncio and stream/seqpacket sends but rejects IP sockets, UNIX sockets and datagram pairs");
#else
  ava::tests::request_skip("Linux containment unavailable");
#endif
}

void test_containment_rejects_external_runtime_symlink_targets()
{
#if defined(__linux__)
  if (!landlock_available() || !containment::seccomp_network_filter_supported() || !std::filesystem::exists("/bin/sh"))
  {
    ava::tests::request_skip("Landlock, seccomp, or shell interpreter unavailable");
    return;
  }
  for (bool const interpreter_link : {false, true})
  {
    ContainmentFixture fix(interpreter_link ? "external-interpreter-target" : "external-executable-target");
    auto const runtime_root = fix.root / "runtime";
    auto options = containment_options_for_test(fix);
    options.startup_path = fix.bin.string() + ":/usr/bin:/bin";
    std::filesystem::create_directories(runtime_root);
    ::chmod(runtime_root.c_str(), S_IRWXU);
    auto const target = runtime_root / "tool";
    {
      std::ofstream file(target);
      file << "#!/bin/sh\nexit 0\n";
    }
    expect(::chmod(target.c_str(), S_IRWXU) == 0, "external runtime target is executable");
    std::filesystem::create_symlink(target, fix.bin / (interpreter_link ? "interpreter" : "cmake"));
    if (interpreter_link)
      expect(fix.write_executable("cmake", "#!" + (fix.bin / "interpreter").string() + "\nexit 0\n"),
             "runtime fixture creates a script with a symlinked interpreter");

    auto anchors = ava::core::AnchorSet::open({fix.workspace, fix.spill});
    expect(anchors.has_value(), "external-runtime fixture opens its containment anchors");
    if (!anchors)
      return;
    options.anchor_set = *anchors;
    auto intent = command::CommandIntent::structured({"cmake", "--build", "build"});
    expect(intent.has_value(), "external-runtime command intent is valid");
    if (!intent)
      return;
    auto preparation = command::prepare_command(*intent, options);
    expect(preparation.has_value(), "external-runtime symlink command seals successfully before containment admission");
    if (!preparation)
      return;
    for (bool const network_allowed : {false, true})
    {
      auto const plan = containment::prepare_development_containment(*preparation, network_allowed);
      auto const expected_reason = interpreter_link ? "canonical interpreter target" : "canonical executable target";
      expect(plan.availability == containment::ContainmentAvailability::Unavailable && plan.unavailable_reason.find(expected_reason) != std::string::npos &&
                 plan.unavailable_reason.find("outside containment roots") != std::string::npos,
             "external canonical executable/interpreter target downgrades containment instead of failing after auto-approval");
    }

    ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
    int prompts = 0;
    bool unavailable_prompt = false;
    auto context = fix.make_context();
    context.permission_resolver = [&](perms::PermissionPrompt const& prompt) -> ava::core::Result<perms::PermissionResolutionDecision> {
      ++prompts;
      unavailable_prompt = prompt.risk == perms::PermissionRisk::Critical && prompt.command_metadata && !prompt.command_metadata->containment_available;
      return perms::PermissionResolution::Deny;
    };
    auto const result =
        ava::tools::run_bash(context, "cmake --build " + fix.build_dir.generic_string(), ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
    expect(!result && prompts == 1 && unavailable_prompt, "unsupported runtime targets require Ask instead of automatic execution");

    // A sibling target within the requested directory remains supported.
    std::filesystem::rename(target, fix.bin / "tool");
    auto const link = fix.bin / (interpreter_link ? "interpreter" : "cmake");
    std::filesystem::remove(link);
    std::filesystem::create_symlink(fix.bin / "tool", link);
    preparation = command::prepare_command(*intent, options);
    expect(preparation.has_value(), "same-directory runtime symlink command seals successfully");
    if (!preparation)
      return;
    for (bool const network_allowed : {false, true})
    {
      auto const plan = containment::prepare_development_containment(*preparation, network_allowed);
      expect(plan.availability == containment::ContainmentAvailability::Available && plan.network_filter_planned,
             "supported runtime targets retain containment with a required seccomp filter in both network modes");
    }
  }
#else
  ava::tests::request_skip("Linux containment unavailable");
#endif
}

void test_containment_accepts_symlinked_workspace_roots()
{
#if defined(__linux__)
  if (!landlock_available() || !containment::seccomp_network_filter_supported() || !std::filesystem::exists("/bin/sh"))
  {
    ava::tests::request_skip("Landlock, seccomp, or shell interpreter unavailable");
    return;
  }
  ContainmentFixture fix("logical-workspace-root");
  auto const physical_workspace = fix.workspace;
  auto const logical_workspace = fix.root / "logical-workspace";
  std::filesystem::rename(fix.bin, physical_workspace / "bin");
  std::filesystem::create_directory_symlink(physical_workspace, logical_workspace);
  fix.workspace = logical_workspace;
  fix.bin = logical_workspace / "bin";
  fix.build_dir = logical_workspace / "build";
  auto anchors = ava::core::AnchorSet::open({fix.workspace, fix.spill});
  expect(anchors.has_value(), "logical-workspace fixture opens its symlinked workspace anchor");
  if (!anchors)
    return;
  fix.anchors = *anchors;
  expect(fix.write_executable("cmake", "#!/bin/sh\nexit 0\n"), "logical-workspace fixture creates a contained script");
  ScopedEnvVar const path_guard("PATH", fix.bin.string() + ":/usr/bin:/bin");
  auto result = ava::tools::run_bash(fix.make_context(), "cmake --build " + fix.build_dir.generic_string(),
                                     ava::tools::BashOptions{.timeout = std::chrono::milliseconds(5000)});
  expect(result && result->exit_code == 0 && result->containment_applied,
         "canonical targets beneath a symlinked workspace root retain automatic contained execution");
#else
  ava::tests::request_skip("Linux containment unavailable");
#endif
}

bool path_covers_for_test(std::filesystem::path const& parent, std::filesystem::path const& child)
{
  auto const physical_parent = std::filesystem::canonical(parent);
  auto const physical_child = std::filesystem::canonical(child);
  return std::mismatch(physical_parent.begin(), physical_parent.end(), physical_child.begin(), physical_child.end()).first == physical_parent.end();
}

void test_containment_limits_protected_executable_parents()
{
  if (!landlock_available() || !containment::seccomp_network_filter_supported() || !std::filesystem::exists("/bin/sh"))
  {
    ava::tests::request_skip("Landlock, seccomp, or shell interpreter unavailable");
    return;
  }
  for (auto const location : {"home", "home-trailing", "home-alias", "home-parent", "authority-parent", "ordinary"})
  {
    for (bool const interpreter : {false, true})
    {
      ContainmentFixture fix(std::string("exact-file-") + location + (interpreter ? "-interpreter" : "-executable"));
      auto options = containment_options_for_test(fix);
      auto directory = fix.bin;
      if (std::string_view(location) == "home")
        directory = options.trusted_home;
      else if (std::string_view(location) == "home-trailing")
      {
        directory = options.trusted_home;
        options.trusted_home /= "";
      }
      else if (std::string_view(location) == "home-alias")
      {
        directory = fix.root / "home-alias";
        std::filesystem::create_directory_symlink(options.trusted_home, directory);
      }
      else if (std::string_view(location) == "home-parent")
      {
        directory = fix.root / "home-parent";
        std::filesystem::rename(options.trusted_home, directory);
        options.trusted_home = directory / "home";
        std::filesystem::create_directory(options.trusted_home);
        ::chmod(options.trusted_home.c_str(), S_IRWXU);
      }
      else if (std::string_view(location) == "authority-parent")
      {
        directory = fix.root / "authority-parent";
        std::filesystem::create_directory(directory);
        ::chmod(directory.c_str(), S_IRWXU);
        std::filesystem::rename(fix.ava_authority_root, directory / "ava");
        options.ava_authority_roots = {directory / "ava"};
      }
      auto const executable = directory / "cmake";
      {
        std::ofstream file(executable);
        file << "#!/bin/sh\nexit 0\n";
      }
      expect(::chmod(executable.c_str(), S_IRWXU) == 0, "protected-parent executable is owner-executable");
      auto command_path = executable;
      if (interpreter)
      {
        command_path = fix.workspace / "build-script";
        std::ofstream file(command_path);
        file << "#!" << executable.string() << "\nexit 0\n";
        file.close();
        expect(::chmod(command_path.c_str(), S_IRWXU) == 0, "protected-parent shebang script is executable");
      }
      auto intent = command::CommandIntent::structured({command_path.string()});
      expect(intent.has_value(), "protected-parent command intent is valid");
      if (!intent)
        return;
      auto preparation = command::prepare_command(*intent, options);
      expect(preparation.has_value(), "protected-parent executable or interpreter seals successfully");
      if (!preparation)
        return;
      if (!interpreter && std::string_view(location) == "home")
        expect(preparation->plan().resolved_executable()->origin == command::ExecutableOrigin::User, "home executable has user origin");
      auto const plan = containment::prepare_development_containment(*preparation, false);
      expect(plan.availability == containment::ContainmentAvailability::Available, "protected-parent commands retain available containment");
      bool const ordinary = std::string_view(location) == "ordinary";
      auto const rule =
          std::ranges::find_if(plan.filesystem_rules, [&](auto const& candidate) { return candidate.logical_path == (ordinary ? directory : executable); });
      auto const expected_mask = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_EXECUTE | (ordinary ? LANDLOCK_ACCESS_FS_READ_DIR : 0);
      expect(rule != plan.filesystem_rules.end() && rule->access_mask == expected_mask && rule->identity_bound,
             "protected parents get exact file rights while ordinary tool directories keep sibling read access");
      for (auto const& candidate : plan.filesystem_rules)
      {
        if (!S_ISDIR(static_cast<mode_t>(candidate.mode)))
          continue;
        expect(!path_covers_for_test(candidate.logical_path, options.trusted_home), "no directory rule covers the trusted home");
        for (auto const& authority : options.ava_authority_roots)
          expect(!path_covers_for_test(candidate.logical_path, authority) && !path_covers_for_test(authority, candidate.logical_path),
                 "no directory rule overlaps AVA authority");
      }
    }
  }
}

void test_startup_path_excludes_trusted_home_and_root()
{
  if (!std::filesystem::exists("/bin/sh"))
  {
    ava::tests::request_skip("shell interpreter unavailable");
    return;
  }
  for (auto const spelling : {"home", "path-trailing", "home-trailing", "home-alias"})
  {
    ContainmentFixture fix(std::string("home-startup-path-") + spelling);
    auto options = containment_options_for_test(fix);
    // No authority roots: home exclusion must not depend on their location.
    options.ava_authority_roots.clear();
    auto entry = options.trusted_home;
    if (std::string_view(spelling) == "path-trailing")
      entry /= "";
    else if (std::string_view(spelling) == "home-trailing")
      options.trusted_home /= "";
    else if (std::string_view(spelling) == "home-alias")
    {
      entry = fix.root / "home-alias";
      std::filesystem::create_directory_symlink(options.trusted_home, entry);
    }
    options.startup_path = entry.string() + ":" + fix.root.string() + ":/:" + fix.bin.string() + ":/usr/bin:/bin";
    expect(fix.write_executable("cmake", "#!/bin/sh\nexit 0\n"), "startup PATH fixture creates its ordinary tool");
    auto intent = command::CommandIntent::structured({"cmake", "--build", "build"});
    expect(intent.has_value(), "startup PATH command intent is valid");
    if (!intent)
      return;
    auto preparation = command::prepare_command(*intent, options);
    expect(preparation.has_value(), "unsafe broad startup PATH entries are omitted without rejecting the command");
    if (!preparation)
      return;
    auto const& entries = preparation->plan().path_entries();
    expect(std::ranges::none_of(entries, [&](auto const& candidate) { return path_covers_for_test(candidate.directory, options.trusted_home); }),
           "startup PATH excludes the trusted home, its ancestors, and aliases regardless of trailing slashes");
    expect(std::ranges::any_of(entries, [&](auto const& candidate) { return candidate.directory == fix.bin; }),
           "ordinary startup tool directories are preserved");
    if (landlock_available() && containment::seccomp_network_filter_supported())
    {
      auto const plan = containment::prepare_development_containment(*preparation, false);
      expect(plan.availability == containment::ContainmentAvailability::Available, "filtered startup PATH retains available containment");
      expect(std::ranges::none_of(plan.filesystem_rules, [&](auto const& rule) { return path_covers_for_test(rule.logical_path, options.trusted_home); }),
             "startup PATH cannot add a Landlock rule covering the real home");
    }
  }
}

void test_failed_seccomp_action_probe_makes_containment_unavailable()
{
  if (!landlock_available() || !containment::seccomp_network_filter_supported() || !std::filesystem::exists("/bin/sh"))
  {
    ava::tests::request_skip("Landlock, seccomp, or shell interpreter unavailable");
    return;
  }
  ContainmentFixture fix("denied-seccomp-probe");
  auto options = containment_options_for_test(fix);
  expect(fix.write_executable("cmake", "#!/bin/sh\nexit 0\n"), "seccomp probe fixture creates its executable");
  auto intent = command::CommandIntent::structured({(fix.bin / "cmake").string(), "--build", "build"});
  expect(intent.has_value(), "seccomp probe command intent is valid");
  if (!intent)
    return;
  auto preparation = command::prepare_command(*intent, options);
  expect(preparation.has_value(), "seccomp probe command seals before installing the outer filter");
  if (!preparation)
    return;
  for (int const probe_error : {EINVAL, EPERM})
  {
    auto const status = ava::tests::run_in_child_with_syscall_errno(
        SYS_seccomp, ava::tests::SyscallArgumentFilter{.index = 0, .value = SECCOMP_GET_ACTION_AVAIL}, probe_error, [&] {
          std::uint32_t const action = SECCOMP_RET_ERRNO;
          if (::syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0u, &action) != -1 || errno != probe_error)
            return 1;
          if (containment::seccomp_network_filter_supported())
            return 2;
          for (bool const network_allowed : {false, true})
          {
            auto const plan = containment::prepare_development_containment(*preparation, network_allowed);
            if (plan.availability != containment::ContainmentAvailability::Unavailable || plan.network_filter_planned ||
                plan.unavailable_reason.find("seccomp") == std::string::npos)
              return 3;
          }
          return 0;
        });
    ava::tests::expect_child_success(status, "EINVAL and EPERM action-probe failures make containment unavailable in both network modes",
                                     "outer seccomp test filter cannot be installed");
    if (ava::tests::skip_requested())
      return;
  }
}

std::optional<containment::DevelopmentContainmentPlan> scoped_containment_plan_for_test(ContainmentFixture const& fix)
{
  if (containment::probe_landlock_abi_version() < 6 || !containment::seccomp_network_filter_supported() || !std::filesystem::exists("/bin/sh"))
  {
    ava::tests::request_skip("Landlock ABI 6, seccomp, or shell interpreter unavailable");
    return std::nullopt;
  }
  auto options = containment_options_for_test(fix);
  auto intent = command::CommandIntent::structured({"/bin/sh"});
  expect(intent.has_value(), "scope test command intent is valid");
  if (!intent)
    return std::nullopt;
  auto preparation = command::prepare_command(*intent, options);
  expect(preparation.has_value(), "scope test command seals successfully");
  if (!preparation)
    return std::nullopt;
  auto plan = containment::prepare_development_containment(*preparation, true);
  expect(plan.availability == containment::ContainmentAvailability::Available, "scope test containment is available");
  if (plan.availability != containment::ContainmentAvailability::Available)
    return std::nullopt;
  return plan;
}

void test_landlock_scopes_parent_signals_and_abstract_sockets()
{
  ContainmentFixture fix("landlock-ipc-scope");
  auto plan = scoped_containment_plan_for_test(fix);
  if (!plan)
    return;

  int const listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  int const connector = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  auto const name = "ava-landlock-scope-" + std::to_string(::getpid());
  std::memcpy(address.sun_path + 1, name.data(), name.size());
  auto const address_size = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
  bool const listening =
      listener >= 0 && connector >= 0 && ::bind(listener, reinterpret_cast<sockaddr*>(&address), address_size) == 0 && ::listen(listener, 1) == 0;
  if (!listening)
  {
    if (listener >= 0)
      ::close(listener);
    if (connector >= 0)
      ::close(connector);
    ava::tests::request_skip("abstract UNIX sockets unavailable");
    return;
  }
  auto const child = ::fork();
  if (child == 0)
  {
    ::close(listener);
    // Signal zero performs the real permission check without risking the
    // harness if scoping regresses. Own-process signaling must remain allowed.
    auto const parent = ::getppid();
    if (::kill(parent, 0) != 0)
      ::_exit(1);
    if (!containment::apply_containment_in_child(*plan))
      ::_exit(2);
    if (::kill(::getpid(), 0) != 0 || ::kill(parent, 0) != -1 || errno != EPERM)
      ::_exit(3);
    // The connector predates containment so seccomp's socket() denial cannot
    // hide a missing Landlock abstract-socket scope; connect is permitted here.
    if (::connect(connector, reinterpret_cast<sockaddr*>(&address), address_size) != -1 || errno != EPERM)
      ::_exit(4);
    ::_exit(0);
  }
  ::close(connector);
  auto const status = ava::tests::wait_child_until(child, ava::tests::now_plus_seconds(5));
  ::close(listener);
  expect(status && WIFEXITED(*status) && WEXITSTATUS(*status) == 0, "ABI-6 containment denies parent signals and abstract connections outside its domain");
}

void test_landlock_scopes_broadcast_signals()
{
  ContainmentFixture fix("landlock-broadcast-scope");
  auto plan = scoped_containment_plan_for_test(fix);
  if (!plan)
    return;
  auto const child = ::fork();
  if (child == 0)
  {
    if (::setpgid(0, 0) != 0)
      ::_exit(1);
    // A real broadcast is safe only in an isolated PID namespace. Unprivileged
    // hosts may require a user namespace first; no UID/GID mappings are needed.
    if (::unshare(CLONE_NEWPID) != 0 && ::unshare(CLONE_NEWUSER | CLONE_NEWPID) != 0)
      ::_exit(77);
    // User namespaces change numeric UID/GID presentation, not the pinned
    // files. Translate only those identity fields before child application.
    for (auto& rule : plan->filesystem_rules)
    {
      struct stat status{};
      if (::stat(rule.logical_path.c_str(), &status) != 0)
        ::_exit(2);
      rule.owner = static_cast<std::uint64_t>(status.st_uid);
      rule.group = static_cast<std::uint64_t>(status.st_gid);
    }
    auto const init = ::fork();
    if (init < 0)
      ::_exit(3);
    if (init == 0)
    {
      // PID 1 is excluded by kill(-1), so use a second process as the victim.
      auto const victim = ::fork();
      if (victim < 0)
        ::_exit(4);
      if (victim == 0)
      {
        sigset_t signals;
        ::sigemptyset(&signals);
        ::sigaddset(&signals, SIGUSR1);
        if (::sigprocmask(SIG_BLOCK, &signals, nullptr) != 0)
          ::_exit(5);
        // Prove the isolated victim observes a real signal before testing denial.
        timespec const immediate{};
        if (::raise(SIGUSR1) != 0 || ::sigtimedwait(&signals, nullptr, &immediate) != SIGUSR1)
          ::_exit(6);
        auto const sender = ::fork();
        if (sender < 0)
          ::_exit(7);
        if (sender == 0)
        {
          if (!containment::apply_containment_in_child(*plan))
            ::_exit(8);
          // Linux may return success even if every target was denied. Verify
          // delivery at the victim instead of asserting an aggregate errno.
          static_cast<void>(::kill(-1, SIGUSR1));
          ::_exit(0);
        }
        auto const status = ava::tests::wait_child_until(sender, ava::tests::now_plus_seconds(5));
        sigset_t pending;
        bool const blocked = ::sigpending(&pending) == 0 && ::sigismember(&pending, SIGUSR1) == 0;
        ::_exit(status && WIFEXITED(*status) && WEXITSTATUS(*status) == 0 && blocked ? 0 : 9);
      }
      auto const status = ava::tests::wait_child_until(victim, ava::tests::now_plus_seconds(5));
      ::_exit(status && WIFEXITED(*status) ? WEXITSTATUS(*status) : 10);
    }
    auto const status = ava::tests::wait_child_until(init, ava::tests::now_plus_seconds(5));
    ::_exit(status && WIFEXITED(*status) ? WEXITSTATUS(*status) : 11);
  }
  if (child > 0)
    static_cast<void>(::setpgid(child, child));
  auto const status = ava::tests::wait_child_until(child, ava::tests::now_plus_seconds(5), true);
  ava::tests::expect_child_success(status ? std::expected<int, int>(*status) : std::unexpected(errno),
                                   "ABI-6 containment prevents kill(-1) from reaching an outside-domain victim",
                                   "PID/user namespaces unavailable for safe broadcast-signal testing");
}

}  // namespace

void run_containment_tests()
{
  // Unit-level tests run first and are never skipped, ensuring
  // unavailable-policy coverage even when Landlock is absent.
  test_containment_summary_redacts_paths();
  test_containment_fd_closure_keeps_bound_executable();
  test_unavailable_probe_forces_ask();
  test_sensitive_network_enabled_remains_ask();
  test_critical_raw_remains_ask_no_containment();
  test_home_as_workspace_rejected();
  if (ava::tests::skip_requested())
    return;
  test_authority_overlap_rejected();
  if (ava::tests::skip_requested())
    return;
  test_startup_path_excludes_trusted_home_and_root();
  if (ava::tests::skip_requested())
    return;

  // Feature probe: skip remaining Landlock-dependent tests if unavailable.
  test_feature_probe();
  if (ava::tests::skip_requested())
    return;

  test_replaced_external_path_rule_fails_child_application();
  test_contained_command_writes_workspace();
  test_contained_command_writes_private_primary_group_workspace();
  test_contained_command_writes_0755_workspace();
  test_contained_command_reads_system_libs();
  test_contained_command_cannot_read_secret();
  test_contained_command_cannot_write_ava_authority();
  test_contained_command_cannot_open_outbound_network();
  test_contained_command_cannot_open_unix_socket();
  test_contained_command_cannot_connect_abstract();
  test_contained_command_cannot_escape_via_symlink();
  test_standard_contained_no_resolver_executes();
  test_contained_command_cancel();
  test_contained_direct_file_recipe();
  test_network_allowed_containment_restricts_socket_domains();
  test_network_denied_containment_allows_private_socketpairs();
  test_containment_rejects_external_runtime_symlink_targets();
  test_containment_accepts_symlinked_workspace_roots();
  test_containment_limits_protected_executable_parents();
  test_failed_seccomp_action_probe_makes_containment_unavailable();
  test_landlock_scopes_parent_signals_and_abstract_sockets();
  test_landlock_scopes_broadcast_signals();
}
