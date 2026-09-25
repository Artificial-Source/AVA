#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/command/command.h"
#include "ava/command/discovery.h"
#include "ava/permissions/permission.h"
#include "ava/core/AnchorSet.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

namespace command = ava::command;

static_assert(!std::default_initializable<command::CommandEnvironment>);
static_assert(!std::default_initializable<command::CommandPlan>);
static_assert(!std::default_initializable<command::CommandPreparation>);
static_assert(std::equality_comparable<command::CommandEnvironment>);
static_assert(std::equality_comparable<command::CommandPreparation>);

struct CommandFixture
{
  std::filesystem::path root;
  std::filesystem::path workspace;
  std::filesystem::path trusted_home;
  std::filesystem::path synthetic_home;
  std::filesystem::path synthetic_xdg_config_home;
  std::filesystem::path synthetic_xdg_cache_home;
  std::filesystem::path synthetic_xdg_data_home;
  std::filesystem::path synthetic_xdg_state_home;
  std::filesystem::path synthetic_tmpdir;
  std::filesystem::path bin;
  std::shared_ptr<ava::core::AnchorSet> anchors;

  explicit CommandFixture(std::string_view name)
      : root(create_empty_root("command-foundation-" + std::string(name))),
        workspace(root / "workspace"),
        trusted_home(root / "trusted-home"),
        synthetic_home(root / "synthetic-child-home"),
        synthetic_xdg_config_home(root / "synthetic-xdg-config"),
        synthetic_xdg_cache_home(root / "synthetic-xdg-cache"),
        synthetic_xdg_data_home(root / "synthetic-xdg-data"),
        synthetic_xdg_state_home(root / "synthetic-xdg-state"),
        synthetic_tmpdir(root / "synthetic-tmp"),
        bin(root / "bin")
  {
    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
    // The shared test harness root may be group writable. Command planning
    // deliberately validates ancestors, so create and secure this fixture's
    // enclosing namespace before sealing any path under it.
    std::filesystem::create_directories(root.parent_path());
    ::chmod(root.parent_path().c_str(), S_IRWXU);
    for (auto const& directory : {workspace, trusted_home, synthetic_home, synthetic_xdg_config_home, synthetic_xdg_cache_home, synthetic_xdg_data_home,
                                  synthetic_xdg_state_home, synthetic_tmpdir, bin, workspace / "build", workspace / "tests", workspace / "nested"})
    {
      std::filesystem::create_directories(directory);
      ::chmod(directory.c_str(), S_IRWXU);
    }
    ::chmod(root.c_str(), S_IRWXU);
    executable_in(bin, "shell");
    anchors = open_anchors();
  }

  std::shared_ptr<ava::core::AnchorSet> open_anchors(std::filesystem::path workspace_root = {}, std::vector<std::filesystem::path> additional = {}) const
  {
    if (workspace_root.empty())
      workspace_root = workspace;
    std::vector<std::filesystem::path> roots{workspace_root,          synthetic_home,           synthetic_xdg_config_home, synthetic_xdg_cache_home,
                                             synthetic_xdg_data_home, synthetic_xdg_state_home, synthetic_tmpdir};
    roots.insert(roots.end(), additional.begin(), additional.end());
    auto opened = ava::core::AnchorSet::open(roots);
    if (!opened)
      throw std::runtime_error("failed to open command test AnchorSet");
    return *opened;
  }

  void executable(std::string_view name, std::string_view content = "#!/bin/sh\nexit 0\n") const { executable_in(bin, name, content); }

  void executable_in(std::filesystem::path const& directory, std::string_view name, std::string_view content = "#!/bin/sh\nexit 0\n") const
  {
    std::filesystem::create_directories(directory);
    ::chmod(directory.c_str(), S_IRWXU);
    auto const path = directory / name;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    output.close();
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
  }

  command::CommandBuildOptions options(std::string profile_id = "command-test") const
  {
    return command::CommandBuildOptions{.workspace = workspace,
                                        .anchor_set = anchors,
                                        .trusted_home = trusted_home,
                                        .startup_path = bin.string(),
                                        .shell = bin / "shell",
                                        .ava_authority_roots = {},
                                        .environment = command::CommandEnvironmentOptions{.profile_id = std::move(profile_id),
                                                                                          .user = "ava-test-user",
                                                                                          .logname = "ava-test-login",
                                                                                          .home = synthetic_home,
                                                                                          .xdg_config_home = synthetic_xdg_config_home,
                                                                                          .xdg_cache_home = synthetic_xdg_cache_home,
                                                                                          .xdg_data_home = synthetic_xdg_data_home,
                                                                                          .xdg_state_home = synthetic_xdg_state_home,
                                                                                          .tmpdir = synthetic_tmpdir},
                                        .workspace_script_recipes = {},
                                        .limits = {}};
  }
};

std::optional<std::string> environment_value(command::CommandEnvironment const& environment, std::string_view key)
{
  auto const found = std::ranges::find_if(environment.entries(), [key](command::EnvironmentVariable const& entry) { return entry.key == key; });
  if (found == environment.entries().end())
    return std::nullopt;
  return found->value;
}

bool is_standard_recipe(ava::core::Result<command::CommandPlan> const& plan)
{
  return plan && plan->classification().level == command::CommandLevel::Standard && plan->classification().recipe.has_value();
}

std::optional<std::pair<std::string, gid_t>> current_account_for_test()
{
  std::array<char, 16 * 1024> storage{};
  passwd record{};
  passwd* resolved = nullptr;
  if (::getpwuid_r(::geteuid(), &record, storage.data(), storage.size(), &resolved) != 0 || !resolved || !resolved->pw_name || resolved->pw_name[0] == '\0')
    return std::nullopt;
  return std::pair{std::string(resolved->pw_name), resolved->pw_gid};
}

std::optional<gid_t> private_primary_group_for_test()
{
  auto const account = current_account_for_test();
  if (!account)
    return std::nullopt;
  std::array<char, 16 * 1024> storage{};
  group record{};
  group* resolved = nullptr;
  if (::getgrgid_r(account->second, &record, storage.data(), storage.size(), &resolved) != 0 || !resolved || !resolved->gr_name ||
      std::string_view(resolved->gr_name) != account->first)
  {
    return std::nullopt;
  }
  for (auto const* const* member = resolved->gr_mem; member && *member; ++member)
  {
    if (std::string_view(*member) != account->first)
      return std::nullopt;
  }
  return resolved->gr_gid;
}

std::optional<gid_t> shared_supplementary_group_for_test()
{
  auto const account = current_account_for_test();
  if (!account)
    return std::nullopt;
  int const count = ::getgroups(0, nullptr);
  if (count <= 0)
    return std::nullopt;
  std::vector<gid_t> groups(static_cast<std::size_t>(count));
  if (::getgroups(count, groups.data()) != count)
    return std::nullopt;
  for (gid_t const candidate : groups)
  {
    if (candidate == account->second)
      continue;
    std::array<char, 16 * 1024> storage{};
    group record{};
    group* resolved = nullptr;
    if (::getgrgid_r(candidate, &record, storage.data(), storage.size(), &resolved) != 0 || !resolved)
      continue;
    for (auto const* const* member = resolved->gr_mem; member && *member; ++member)
    {
      if (std::string_view(*member) != account->first)
        return candidate;
    }
  }
  return std::nullopt;
}

bool set_group_and_mode(std::filesystem::path const& path, gid_t group_id, mode_t mode)
{
  return ::chown(path.c_str(), ::geteuid(), group_id) == 0 && ::chmod(path.c_str(), mode) == 0;
}

void test_compatibility_parser_is_lossless_or_raw_shell()
{
  auto quoted_backslash = command::CommandIntent::compatibility("external-tool 'a\\b'");
  auto ordinary_double_quote = command::CommandIntent::compatibility("external-tool \"a\\q\"");
  auto expanded_double_quote = command::CommandIntent::compatibility("external-tool \"$HOME\"");
  auto backtick = command::CommandIntent::compatibility("external-tool `id`");
  auto assignment = command::CommandIntent::compatibility("NAME=value external-tool safe");
  auto substitution = command::CommandIntent::compatibility("external-tool $(id)");
  auto unmatched_quote = command::CommandIntent::compatibility("external-tool 'unterminated");
  auto unmatched_escape = command::CommandIntent::compatibility("external-tool trailing\\");

  expect(quoted_backslash && quoted_backslash->lane() == command::CommandIntentLane::Compatibility &&
             quoted_backslash->argv() == std::vector<std::string>({"external-tool", "a\\b"}) && ordinary_double_quote &&
             ordinary_double_quote->lane() == command::CommandIntentLane::Compatibility &&
             ordinary_double_quote->argv() == std::vector<std::string>({"external-tool", "a\\q"}),
         "compatibility parsing preserves literal single-quote backslashes and supported double-quote escapes exactly");
  expect(expanded_double_quote && expanded_double_quote->lane() == command::CommandIntentLane::RawShell && backtick &&
             backtick->lane() == command::CommandIntentLane::RawShell && assignment && assignment->lane() == command::CommandIntentLane::RawShell &&
             substitution && substitution->lane() == command::CommandIntentLane::RawShell && !unmatched_quote && !unmatched_escape,
         "compatibility parsing routes expansion, leading assignment, unsupported shell constructs, and non-lossless syntax to critical raw-shell handling");
}

void test_compatibility_shell_words_are_critical_raw_shell()
{
  CommandFixture fixture("compatibility-shell-words");
  std::vector<std::string> const shell_words{"cd workspace", "chdir workspace",   "command ls",   "exec tool", "time tool",  "enable -n echo",
                                             "coproc tool",  "export NAME=value", "unset NAME",   "set -e",    "read value", "source script",
                                             ". script",     "eval tool",         "if condition", "for item"};
  bool all_raw_critical = true;
  for (auto const& text : shell_words)
  {
    auto intent = command::CommandIntent::compatibility(text);
    auto plan = intent
                    ? command::seal_command_plan(*intent, fixture.options())
                    : ava::core::Result<command::CommandPlan>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no compatibility intent"))};
    all_raw_critical = all_raw_critical && intent && intent->lane() == command::CommandIntentLane::RawShell && plan &&
                       plan->execution_domain() == command::CommandExecutionDomain::RawShell &&
                       plan->classification().level == command::CommandLevel::Critical && plan->classification().family == command::CommandFamily::RawShell;
  }
  expect(all_raw_critical,
         "compatibility shell builtins, special builtins, and reserved command-position words always use critical raw-shell plans instead of argv lookup");
}

void test_intent_bounds_and_path_bounds_do_not_underflow()
{
  command::CommandLimits request_limit;
  request_limit.max_request_bytes = 4;
  auto too_large = command::CommandIntent::structured({"aa", "bbb"}, std::nullopt, request_limit);
  command::CommandLimits exact_limit;
  exact_limit.max_request_bytes = 4;
  auto exact = command::CommandIntent::structured({"aa", "bb"}, std::nullopt, exact_limit);

  CommandFixture fixture("bounds");
  auto const path_root = fixture.root / ("path-limit-" + std::string(128, 'p'));
  fixture.executable_in(path_root, "pwd");
  auto intent = command::CommandIntent::structured({"pwd"});
  auto fits = fixture.options();
  fits.startup_path = path_root.string();
  fits.limits.max_path_bytes = path_root.string().size();
  auto does_not_fit = fits;
  does_not_fit.limits.max_path_bytes = path_root.string().size() - 1;
  auto fitting_plan = command::seal_command_plan(*intent, fits);
  auto oversized_path_plan = command::seal_command_plan(*intent, does_not_fit);

  expect(!too_large && exact && fitting_plan && !oversized_path_plan,
         "bounded argv and PATH accumulation reject overflows without unsigned-underflow bypasses while accepting exact boundaries");
}

void test_private_primary_group_directories_are_accepted()
{
  auto const private_group = private_primary_group_for_test();
  if (!private_group)
    return;

  CommandFixture fixture("private-primary-group");
  fixture.executable("pwd");
  bool const configured = set_group_and_mode(fixture.root, *private_group, S_IRWXU | S_IRGRP | S_IWGRP | S_IXGRP) &&
                          set_group_and_mode(fixture.workspace, *private_group, S_IRWXU | S_IRWXG) &&
                          set_group_and_mode(fixture.workspace / "nested", *private_group, S_IRWXU | S_IRGRP | S_IWGRP | S_IXGRP) &&
                          set_group_and_mode(fixture.bin, *private_group, S_IRWXU | S_IRGRP | S_IWGRP | S_IXGRP);
  expect(configured, "private-primary-group fixture changes directory group and modes");
  if (!configured)
    return;

  auto const workspace_0770 = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), fixture.options());
  auto const cwd_0775 = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}, "nested"), fixture.options());
  ::chmod(fixture.workspace.c_str(), S_IRWXU | S_IRGRP | S_IWGRP | S_IXGRP);
  auto const prepared_0775 = command::prepare_command(*command::CommandIntent::structured({"pwd"}), fixture.options());
  auto const fresh = prepared_0775 ? command::plan_is_fresh(prepared_0775->plan())
                                   : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no private-group plan"))};

  expect(workspace_0770 && cwd_0775 && prepared_0775 && fresh && *fresh,
         "owner/private-primary-group 0770 and 0775 workspace, cwd, PATH, and ancestor directories seal a normal pwd plan");
}

void test_world_writable_directories_are_rejected()
{
  CommandFixture final_fixture("world-writable-final");
  final_fixture.executable("pwd");
  ::chmod(final_fixture.workspace.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
  auto const unsafe_final = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), final_fixture.options());

  CommandFixture ancestor_fixture("world-writable-ancestor");
  ancestor_fixture.executable("pwd");
  ::chmod(ancestor_fixture.root.c_str(), S_IRWXU | S_IRWXG | S_IRWXO);
  auto const unsafe_ancestor = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), ancestor_fixture.options());

  expect(!unsafe_final && !unsafe_ancestor, "world-writable final workspace and captured ancestors are rejected");
}

void test_shared_supplementary_group_directories_are_rejected()
{
  auto const shared_group = shared_supplementary_group_for_test();
  if (!shared_group)
    return;

  CommandFixture fixture("shared-supplementary-group");
  fixture.executable("pwd");
  bool const configured =
      set_group_and_mode(fixture.workspace, *shared_group, S_IRWXU | S_IRWXG) && set_group_and_mode(fixture.root, *shared_group, S_IRWXU | S_IRWXG);
  expect(configured, "shared supplementary-group fixture changes directory group and modes");
  if (!configured)
    return;

  auto const plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), fixture.options());
  expect(!plan, "writable shared supplementary-group workspace and captured ancestor are rejected");
}

void test_group_changes_invalidate_sealed_plans()
{
  auto const shared_group = shared_supplementary_group_for_test();
  if (!shared_group)
    return;

  CommandFixture fixture("group-freshness");
  fixture.executable("pwd");
  ::chmod(fixture.workspace.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
  auto const before = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), fixture.options());
  bool const changed = ::chown(fixture.workspace.c_str(), ::geteuid(), *shared_group) == 0;
  expect(changed, "group-freshness fixture changes workspace group");
  if (!changed)
    return;
  auto const stale = before ? command::plan_is_fresh(*before)
                            : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no group-freshness plan"))};
  auto const after = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), fixture.options());

  expect(before && stale && !*stale && after && before->fingerprint() != after->fingerprint(),
         "workspace group metadata changes invalidate freshness and fingerprint identity");
}

void test_known_symlink_alias_keeps_invoked_family()
{
  CommandFixture fixture("symlink-alias");
  fixture.executable("npm-cli.js");
  std::filesystem::create_symlink("npm-cli.js", fixture.bin / "npm");

  auto const bare = command::seal_command_plan(*command::CommandIntent::structured({"npm", "run", "test"}), fixture.options());
  auto const absolute = command::seal_command_plan(*command::CommandIntent::structured({(fixture.bin / "npm").string(), "run", "test"}), fixture.options());
  ava::permissions::CommandContainmentInfo const contained{.available = true, .profile_id = "ava-development-v1", .network_allowed = false};
  auto const metadata = bare ? ava::permissions::command_permission_metadata(*bare, contained) : ava::permissions::CommandPermissionMetadata{};
  auto const permission = ava::permissions::decide(metadata);

  expect(bare && absolute && bare->classification().level == command::CommandLevel::Standard && bare->classification().recipe &&
             bare->classification().recipe->recipe == command::CommandRecipe::PackageManagerRunScript && absolute->classification().recipe &&
             absolute->classification().recipe->recipe == command::CommandRecipe::PackageManagerRunScript && bare->resolved_executable() &&
             bare->resolved_executable()->executable.canonical_path.filename() == "npm-cli.js" &&
             bare->classification().recipe->canonical_argv == absolute->classification().recipe->canonical_argv &&
             metadata.resolved_executable == fixture.bin / "npm" && permission.action == ava::permissions::PermissionAction::Allow,
         "a known symlinked executable alias classifies by the invoked family while descriptor identity and recipe argv bind its physical target, public "
         "metadata preserves the logical alias, and verified containment permits the recipe");
}

void test_workspace_spoofs_are_not_inspection_recipes()
{
  CommandFixture fixture("origin-spoof");
  fixture.executable("ls");
  auto const venv_bin = fixture.workspace / ".venv" / "bin";
  fixture.executable_in(venv_bin, "ls");
  fixture.executable_in(venv_bin, "cmake");
  ::chmod((fixture.workspace / ".venv").c_str(), S_IRWXU);
  std::filesystem::create_directories(fixture.workspace / "build");

  auto options = fixture.options("origin-profile");
  options.startup_path = venv_bin.string() + ":" + fixture.bin.string();
  auto bare_ls = command::seal_command_plan(*command::CommandIntent::structured({"ls"}), options);
  auto absolute_ls = command::seal_command_plan(*command::CommandIntent::structured({(venv_bin / "ls").string()}), options);
  auto workspace_cmake = command::seal_command_plan(*command::CommandIntent::structured({"cmake", "--build", "build"}), options);
  options.startup_path = fixture.bin.string();
  auto trusted_ls = command::seal_command_plan(*command::CommandIntent::structured({"ls"}), options);

  expect(bare_ls && absolute_ls && bare_ls->resolved_executable() && absolute_ls->resolved_executable() &&
             bare_ls->resolved_executable()->origin == command::ExecutableOrigin::Workspace &&
             absolute_ls->resolved_executable()->origin == command::ExecutableOrigin::Workspace &&
             bare_ls->classification().level == command::CommandLevel::Critical && absolute_ls->classification().level == command::CommandLevel::Critical &&
             bare_ls->classification().capabilities.executes_mutable_project_code && bare_ls->classification().capabilities.requires_containment &&
             absolute_ls->classification().capabilities.executes_mutable_project_code && absolute_ls->classification().capabilities.requires_containment &&
             workspace_cmake && workspace_cmake->classification().level == command::CommandLevel::Standard &&
             workspace_cmake->classification().capabilities.executes_mutable_project_code &&
             workspace_cmake->classification().capabilities.requires_containment && trusted_ls && trusted_ls->resolved_executable() &&
             trusted_ls->resolved_executable()->origin == command::ExecutableOrigin::User &&
             trusted_ls->classification().level == command::CommandLevel::Critical && trusted_ls->classification().capabilities.requires_containment,
         "workspace and user-owned same-name spoof executables never gain uncontained inspection status, while project-code recipes retain containment");
}

void test_recipe_paths_are_logical_sealed_and_fresh()
{
  CommandFixture fixture("recipe-paths");
  for (auto const name : {"cmake", "ctest", "pytest"}) fixture.executable(name);
  auto const outside = fixture.root / "outside";
  std::filesystem::create_directories(outside);
  ::chmod(outside.c_str(), S_IRWXU);

  struct RecipeCase
  {
    std::string label;
    std::string executable;
    std::vector<std::string> prefix;
  };
  std::vector<RecipeCase> const cases{{"ls", "/usr/bin/ls", {}}, {"cmake", "cmake", {"--build"}}, {"ctest", "ctest", {"--test-dir"}}, {"pytest", "pytest", {}}};
  bool all_escaped_downgraded = true;
  bool all_replacements_stale = true;
  bool all_paths_bound = true;
  for (auto const& recipe : cases)
  {
    auto const target = fixture.workspace / (recipe.label + "-target");
    auto const escape = fixture.workspace / (recipe.label + "-escape");
    std::filesystem::create_directories(target);
    ::chmod(target.c_str(), S_IRWXU);
    std::filesystem::create_directory_symlink(outside, escape);

    std::vector<std::string> escaped_argv{recipe.executable};
    escaped_argv.insert(escaped_argv.end(), recipe.prefix.begin(), recipe.prefix.end());
    escaped_argv.push_back(escape.filename().string());
    auto escaped = command::seal_command_plan(*command::CommandIntent::structured(std::move(escaped_argv)), fixture.options());
    all_escaped_downgraded = all_escaped_downgraded && escaped && escaped->classification().level == command::CommandLevel::Critical;

    std::vector<std::string> valid_argv{recipe.executable};
    valid_argv.insert(valid_argv.end(), recipe.prefix.begin(), recipe.prefix.end());
    valid_argv.push_back(target.filename().string());
    auto plan = command::seal_command_plan(*command::CommandIntent::structured(std::move(valid_argv)), fixture.options());
    auto before =
        plan ? command::plan_is_fresh(*plan) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};
    all_paths_bound = all_paths_bound && is_standard_recipe(plan) && plan->classification().recipe->path_arguments.size() == 1 &&
                      plan->classification().recipe->path_arguments.front().canonical_path == target.lexically_normal() &&
                      plan->classification().recipe->canonical_argv.back() == target.lexically_normal().string() && before && *before;

    std::filesystem::remove_all(target);
    std::filesystem::create_directories(target);
    ::chmod(target.c_str(), S_IRWXU);
    auto after =
        plan ? command::plan_is_fresh(*plan) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};
    all_replacements_stale = all_replacements_stale && after && !*after;
  }
  expect(all_escaped_downgraded && all_paths_bound && all_replacements_stale,
         "ls, build, ctest, and pytest path arguments reject symlink escapes, bind logical descriptor identities, and detect replacement before execution");
}

void test_sealed_freshness_and_symlink_provenance()
{
  CommandFixture fixture("freshness");
  fixture.executable("fresh");
  auto fresh = command::seal_command_plan(*command::CommandIntent::structured({"fresh"}), fixture.options());
  auto before =
      fresh ? command::plan_is_fresh(*fresh) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};
  {
    std::ofstream append(fixture.bin / "fresh", std::ios::binary | std::ios::app);
    append << "# changed\n";
  }
  auto after =
      fresh ? command::plan_is_fresh(*fresh) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  fixture.executable("gone");
  auto gone = command::seal_command_plan(*command::CommandIntent::structured({"gone"}), fixture.options());
  std::filesystem::remove(fixture.bin / "gone");
  auto missing =
      gone ? command::plan_is_fresh(*gone) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  fixture.executable("target-one");
  fixture.executable("target-two");
  std::filesystem::create_symlink(fixture.bin / "target-one", fixture.bin / "linked");
  auto linked = command::seal_command_plan(*command::CommandIntent::structured({"linked"}), fixture.options());
  auto linked_before =
      linked ? command::plan_is_fresh(*linked) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};
  std::filesystem::remove(fixture.bin / "linked");
  std::filesystem::create_symlink(fixture.bin / "target-two", fixture.bin / "linked");
  auto linked_after =
      linked ? command::plan_is_fresh(*linked) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  expect(fresh && before && *before && after && !*after && gone && !missing && linked && linked->resolved_executable() &&
             linked->resolved_executable()->executable.requested_path_is_symlink && linked_before && *linked_before && linked_after && !*linked_after,
         "freshness checks executable metadata and symlink provenance, returning an error for inaccessible identities rather than silently treating them as "
         "stale");
}

void test_workspace_cwd_path_and_interpreter_freshness()
{
  CommandFixture fixture("all-freshness");
  fixture.executable("pwd");
  auto workspace_plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), fixture.options());
  ::chmod(fixture.workspace.c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
  auto workspace_changed = workspace_plan ? command::plan_is_fresh(*workspace_plan)
                                          : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  CommandFixture cwd_fixture("cwd-freshness");
  cwd_fixture.executable("pwd");
  auto cwd_plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}, "nested"), cwd_fixture.options());
  ::chmod((cwd_fixture.workspace / "nested").c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
  auto cwd_changed =
      cwd_plan ? command::plan_is_fresh(*cwd_plan) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  CommandFixture path_fixture("path-freshness");
  path_fixture.executable("pwd");
  auto path_plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), path_fixture.options());
  ::chmod(path_fixture.bin.c_str(), S_IRUSR | S_IWUSR | S_IXUSR);
  auto path_changed =
      path_plan ? command::plan_is_fresh(*path_plan) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  CommandFixture interpreter_fixture("interpreter-freshness");
  interpreter_fixture.executable("interpreter");
  interpreter_fixture.executable("script", "#!" + (interpreter_fixture.bin / "interpreter").string() + "\nexit 0\n");
  auto script_plan = command::seal_command_plan(*command::CommandIntent::structured({"script"}), interpreter_fixture.options());
  {
    std::ofstream append(interpreter_fixture.bin / "interpreter", std::ios::binary | std::ios::app);
    append << "# changed\n";
  }
  auto interpreter_changed = script_plan ? command::plan_is_fresh(*script_plan)
                                         : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  expect(workspace_changed && !*workspace_changed && cwd_changed && !*cwd_changed && path_changed && !*path_changed && script_plan &&
             script_plan->resolved_executable()->shebang_interpreters.size() == 2 && interpreter_changed && !*interpreter_changed,
         "workspace, cwd, PATH entries, executables, and every shebang interpreter participate in freshness validation");
}

void test_raw_shell_binds_configured_shell_and_accepts_env_shebang()
{
  CommandFixture fixture("raw-shell");
  auto raw = command::CommandIntent::compatibility("echo safe; whoami");
  auto raw_plan = command::seal_command_plan(*raw, fixture.options());
  auto default_shell_options = fixture.options();
  default_shell_options.shell = "/bin/sh";
  auto default_shell_plan = command::seal_command_plan(*raw, default_shell_options);
  auto invalid_shell_options = fixture.options();
  invalid_shell_options.shell = "shell";
  auto invalid_shell_plan = command::seal_command_plan(*raw, invalid_shell_options);

  // Real env-python/node-like fixtures: a script using #!/usr/bin/env <name>
  // resolves the interpreter through the sealed PATH and binds the chain.
  fixture.executable("python3", "printf 'python3-ran\\n'\n");
  fixture.executable("node", "printf 'node-ran\\n'\n");
  fixture.executable("env-python", "#!/usr/bin/env python3\nprintf 'env-python-ran\\n'\n");
  fixture.executable("env-node", "#!/usr/bin/env node\nprintf 'env-node-ran\\n'\n");
  // An interpreter-argument shebang (Linux one-argument behavior).
  fixture.executable("interpreter", "#!/bin/sh\nprintf 'interpreter-ran\\n'\n");
  fixture.executable("arg-script", "#!" + (fixture.bin / "interpreter").string() + " -u\nprintf 'arg-script-ran\\n'\n");
  // An env shebang with an unresolvable name: plan succeeds with a partial
  // (unresolved) chain, producing a one-shot critical prompt.
  fixture.executable("env-missing", "#!/usr/bin/env nonexistent-lang\nprintf 'env-missing\\n'\n");

  auto env_python = command::seal_command_plan(*command::CommandIntent::structured({"env-python"}), fixture.options());
  auto env_node = command::seal_command_plan(*command::CommandIntent::structured({"env-node"}), fixture.options());
  auto arg_script = command::seal_command_plan(*command::CommandIntent::structured({"arg-script"}), fixture.options());
  auto env_missing = command::seal_command_plan(*command::CommandIntent::structured({"env-missing"}), fixture.options());

  CommandFixture shadow_fixture("env-shadow");
  auto const unsafe_bin = shadow_fixture.root / "unsafe-bin";
  auto const safe_bin = shadow_fixture.root / "safe-bin";
  shadow_fixture.executable_in(unsafe_bin, "python3", "#!/bin/sh\nprintf unsafe\\n\n");
  shadow_fixture.executable_in(safe_bin, "python3", "#!/bin/sh\nprintf safe\\n\n");
  ::chmod((unsafe_bin / "python3").c_str(), S_IRWXU | S_IRWXG);
  shadow_fixture.executable("env-shadow", "#!/usr/bin/env python3\nprintf script\\n\n");
  shadow_fixture.executable("env-crlf", "#!/usr/bin/env python3\r\nprintf script\\n\n");
  shadow_fixture.executable("long-shebang", "#!/bin/sh " + std::string(300, 'x') + "\nprintf script\\n\n");
  auto shadow_options = shadow_fixture.options();
  shadow_options.startup_path = shadow_fixture.bin.string() + ":" + unsafe_bin.string() + ":" + safe_bin.string();
  auto env_shadow = command::seal_command_plan(*command::CommandIntent::structured({"env-shadow"}), shadow_options);
  auto env_crlf = command::seal_command_plan(*command::CommandIntent::structured({"env-crlf"}), shadow_options);
  auto long_shebang = command::seal_command_plan(*command::CommandIntent::structured({"long-shebang"}), shadow_options);

  expect(raw_plan && raw_plan->execution_domain() == command::CommandExecutionDomain::RawShell && raw_plan->resolved_executable() &&
             raw_plan->resolved_executable()->executable.canonical_path == std::filesystem::canonical(fixture.bin / "shell") &&
             raw_plan->classification().level == command::CommandLevel::Critical && default_shell_plan && default_shell_plan->resolved_executable() &&
             default_shell_plan->resolved_executable()->executable.canonical_path == std::filesystem::canonical("/bin/sh") && !invalid_shell_plan &&
             env_python && env_python->resolved_executable()->shebang_interpreters.size() == 2 &&
             env_python->resolved_executable()->shebang_interpreters[0].interpreter.canonical_path == std::filesystem::canonical("/usr/bin/env") &&
             env_python->resolved_executable()->shebang_interpreters[0].argument == "python3" &&
             env_python->resolved_executable()->shebang_interpreters[1].interpreter.canonical_path == std::filesystem::canonical(fixture.bin / "python3") &&
             env_python->resolved_executable()->shebang_fully_resolved && env_node && env_node->resolved_executable()->shebang_fully_resolved && arg_script &&
             arg_script->resolved_executable()->shebang_interpreters.size() >= 1 &&
             arg_script->resolved_executable()->shebang_interpreters[0].argument == "-u" && arg_script->resolved_executable()->shebang_fully_resolved &&
             env_missing && !env_missing->resolved_executable()->shebang_fully_resolved &&
             env_missing->classification().level == command::CommandLevel::Critical && env_shadow &&
             !env_shadow->resolved_executable()->shebang_fully_resolved && env_shadow->classification().level == command::CommandLevel::Critical && env_crlf &&
             !env_crlf->resolved_executable()->shebang_fully_resolved && env_crlf->classification().level == command::CommandLevel::Critical && !long_shebang,
         "raw-shell plans bind the configured shell; env shebangs resolve only the interpreter the kernel will select, "
         "CRLF remains part of the kernel argument, and overlong Linux shebangs fail sealed planning");
}

void test_environment_is_synthetic_and_digest_bound()
{
  CommandFixture fixture("environment");
  auto const user_bin = fixture.trusted_home / ".local" / "bin";
  fixture.executable_in(user_bin, "user-tool");
  ::chmod(user_bin.parent_path().c_str(), S_IRWXU);
  auto prepared = command::prepare_command(*command::CommandIntent::structured({"user-tool"}), fixture.options("environment-profile"));
  auto changed_environment_options = fixture.options("environment-profile");
  changed_environment_options.environment.tmpdir = fixture.root / "different-synthetic-tmp";
  std::filesystem::create_directories(changed_environment_options.environment.tmpdir);
  ::chmod(changed_environment_options.environment.tmpdir.c_str(), S_IRWXU);
  changed_environment_options.anchor_set = fixture.open_anchors({}, {changed_environment_options.environment.tmpdir});
  auto changed_environment = command::seal_command_plan(*command::CommandIntent::structured({"user-tool"}), changed_environment_options);
  auto overlapping_root_options = fixture.options("environment-profile");
  overlapping_root_options.environment.home = fixture.trusted_home;
  auto overlapping_root = command::seal_command_plan(*command::CommandIntent::structured({"user-tool"}), overlapping_root_options);

  auto home = prepared ? environment_value(prepared->environment(), "HOME") : std::nullopt;
  auto pwd = prepared ? environment_value(prepared->environment(), "PWD") : std::nullopt;
  auto path = prepared ? environment_value(prepared->environment(), "PATH") : std::nullopt;
  expect(prepared && prepared->plan().resolved_executable() && prepared->plan().resolved_executable()->origin == command::ExecutableOrigin::User && home &&
             *home == fixture.synthetic_home.string() && *home != fixture.trusted_home.string() && pwd && *pwd == fixture.workspace.string() && path &&
             path->find(user_bin.string()) != std::string::npos && prepared->plan().environment_digest().starts_with("sha256:ava-command-environment-v3:") &&
             prepared->plan().fingerprint().starts_with("sha256:ava-command-plan-v5:") &&
             prepared->plan().display_json().find(fixture.synthetic_home.string()) == std::string::npos && changed_environment &&
             changed_environment->environment_digest() != prepared->plan().environment_digest() &&
             changed_environment->fingerprint() != prepared->plan().fingerprint() && !overlapping_root,
         "trusted host user-tool discovery is separate from synthetic child roots, PWD retains the logical cwd spelling, and a versioned SHA-256 "
         "environment digest binds exact environment content to plans");
}

void test_delegated_context_omits_automatic_host_user_tools()
{
  CommandFixture fixture("delegated-host-user-tools");
  fixture.executable("pwd");
  auto const user_bin = fixture.trusted_home / ".local" / "bin";
  auto const other_user_bin = fixture.trusted_home / ".tools" / "bin";
  auto const workspace_venv = fixture.workspace / ".venv" / "bin";
  auto const workspace_node_modules = fixture.workspace / "node_modules" / ".bin";
  for (auto const& directory : {user_bin, other_user_bin, workspace_venv, workspace_node_modules})
  {
    std::filesystem::create_directories(directory);
    ::chmod(directory.c_str(), S_IRWXU);
  }
  ::chmod(user_bin.parent_path().c_str(), S_IRWXU);
  ::chmod(other_user_bin.parent_path().c_str(), S_IRWXU);

  auto const intent = command::CommandIntent::structured({"pwd"});
  auto local = command::prepare_command(*intent, fixture.options("local-host-user-tools"));
  auto delegated_options = fixture.options("delegated-host-user-tools");
  delegated_options.discover_host_user_tools = false;
  auto delegated = command::prepare_command(*intent, delegated_options);
  auto const has_path = [](command::CommandPlan const& plan, std::filesystem::path const& directory, command::PathProvenance provenance) {
    return std::ranges::any_of(plan.path_entries(), [&directory, provenance](command::CommandPathEntry const& entry) {
      return entry.directory == directory && entry.provenance == provenance;
    });
  };
  auto const has_directory = [](command::CommandPlan const& plan, std::filesystem::path const& directory) {
    return std::ranges::any_of(plan.path_entries(), [&directory](command::CommandPathEntry const& entry) { return entry.directory == directory; });
  };

  expect(local && delegated && has_path(local->plan(), user_bin, command::PathProvenance::UserLocal) && !has_directory(local->plan(), other_user_bin) &&
             has_path(local->plan(), workspace_venv, command::PathProvenance::WorkspaceVenv) &&
             has_path(local->plan(), workspace_node_modules, command::PathProvenance::WorkspaceNodeModules) &&
             !has_path(delegated->plan(), user_bin, command::PathProvenance::UserLocal) &&
             has_path(delegated->plan(), fixture.bin, command::PathProvenance::StartupPath) &&
             has_path(delegated->plan(), workspace_venv, command::PathProvenance::WorkspaceVenv) &&
             has_path(delegated->plan(), workspace_node_modules, command::PathProvenance::WorkspaceNodeModules),
         "automatic host discovery includes only the safe trusted-home .local/bin path, delegated contexts omit it, and startup and workspace PATH entries "
         "remain available in both contexts");
}

void test_logical_workspace_and_cwd_spelling_is_preserved()
{
  CommandFixture fixture("logical-workspace");
  auto const logical_workspace = fixture.root / "workspace-link";
  std::filesystem::create_directory_symlink(fixture.workspace.filename(), logical_workspace);

  auto options = fixture.options("logical-workspace-profile");
  options.workspace = logical_workspace;
  options.anchor_set = fixture.open_anchors(logical_workspace);
  auto intent = command::CommandIntent::structured({"shell"}, logical_workspace / "nested");
  auto prepared = intent ? command::prepare_command(*intent, options)
                         : ava::core::Result<command::CommandPreparation>{
                               std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "no logical command intent"))};
  auto pwd = prepared ? environment_value(prepared->environment(), "PWD") : std::nullopt;

  expect(prepared && prepared->plan().workspace() == logical_workspace && prepared->plan().cwd() == logical_workspace / "nested" && pwd &&
             *pwd == (logical_workspace / "nested").string(),
         "sealed command workspace, cwd, and PWD preserve the launch-time logical symlink spelling without filesystem canonicalization");
}

void test_only_direct_trusted_home_entry_churn_stays_fresh_for_user_tools()
{
  CommandFixture fixture("trusted-home-entry-churn");
  auto const user_bin = fixture.trusted_home / ".local" / "bin";
  fixture.executable_in(user_bin, "user-tool");
  ::chmod(user_bin.parent_path().c_str(), S_IRWXU);
  auto options = fixture.options("trusted-home-churn-profile");
  options.startup_path = user_bin.string() + ":" + fixture.bin.string();

  auto plan = command::seal_command_plan(*command::CommandIntent::structured({"user-tool"}), options);
  auto const fingerprint = plan ? plan->fingerprint() : std::string{};
  auto const startup_user_path = plan ? std::ranges::find_if(plan->path_entries(),
                                                             [&user_bin](command::CommandPathEntry const& entry) {
                                                               return entry.directory == user_bin && entry.provenance == command::PathProvenance::StartupPath;
                                                             })
                                      : std::vector<command::CommandPathEntry>::const_iterator{};

  auto const direct_entry = fixture.trusted_home / "unrelated-direct-entry";
  {
    std::ofstream output(direct_entry);
  }
  auto direct_created = plan ? command::plan_is_fresh(*plan)
                             : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no trusted-home plan"))};
  std::filesystem::remove(direct_entry);
  auto direct_removed = plan ? command::plan_is_fresh(*plan)
                             : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no trusted-home plan"))};

  auto const local_entry = user_bin.parent_path() / "unrelated-user-tool-entry";
  {
    std::ofstream output(local_entry);
  }
  auto descendant_created =
      plan ? command::plan_is_fresh(*plan) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no user-tool plan"))};
  std::filesystem::remove(local_entry);
  auto descendant_removed =
      plan ? command::plan_is_fresh(*plan) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no user-tool plan"))};

  bool private_group_descendant_churn_staled = true;
  if (auto const private_group = private_primary_group_for_test())
  {
    CommandFixture private_fixture("private-group-user-tool-ancestor-churn");
    auto const private_bin = private_fixture.trusted_home / ".local" / "bin";
    private_fixture.executable_in(private_bin, "private-tool");
    auto const private_local = private_bin.parent_path();
    auto private_options = private_fixture.options();
    private_options.startup_path = private_bin.string() + ":" + private_fixture.bin.string();
    bool const configured = set_group_and_mode(private_local, *private_group, S_IRWXU | S_IRWXG);
    auto private_plan = configured ? command::seal_command_plan(*command::CommandIntent::structured({"private-tool"}), private_options)
                                   : ava::core::Result<command::CommandPlan>{
                                         std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "private-group fixture setup failed"))};
    {
      std::ofstream output(private_local / "ctime-only-churn");
    }
    auto private_stale = private_plan
                             ? command::plan_is_fresh(*private_plan)
                             : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no private-group user-tool plan"))};
    private_group_descendant_churn_staled = configured && private_plan && private_stale && !*private_stale;
  }

  expect(plan && plan->resolved_executable() && plan->resolved_executable()->origin == command::ExecutableOrigin::User &&
             startup_user_path != plan->path_entries().end() && direct_created && *direct_created && direct_removed && *direct_removed && descendant_created &&
             !*descendant_created && descendant_removed && !*descendant_removed && private_group_descendant_churn_staled && plan->fingerprint() == fingerprint,
         "only direct trusted-home entry churn preserves a sealed user executable and startup PATH duplicate; .local and available "
         "private-primary-group descendant churn stale the plan");
}

void test_user_tool_scope_keeps_final_and_symlink_identity_strict()
{
  CommandFixture executable_fixture("user-tool-final-executable");
  auto const executable_bin = executable_fixture.trusted_home / ".local" / "bin";
  executable_fixture.executable_in(executable_bin, "user-tool");
  auto executable_options = executable_fixture.options();
  executable_options.startup_path = executable_bin.string() + ":" + executable_fixture.bin.string();
  auto executable_plan = command::seal_command_plan(*command::CommandIntent::structured({"user-tool"}), executable_options);
  {
    std::ofstream output(executable_bin / "user-tool", std::ios::app);
    output << "# changed\n";
  }
  auto executable_changed =
      executable_plan ? command::plan_is_fresh(*executable_plan)
                      : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no final user executable plan"))};

  CommandFixture path_fixture("user-tool-final-path");
  path_fixture.executable("pwd");
  auto const path_bin = path_fixture.trusted_home / ".local" / "bin";
  std::filesystem::create_directories(path_bin);
  ::chmod(path_bin.c_str(), S_IRWXU);
  auto path_plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), path_fixture.options());
  auto const outside_path = path_plan
                                ? std::ranges::find_if(path_plan->path_entries(),
                                                       [&path_fixture](command::CommandPathEntry const& entry) { return entry.directory == path_fixture.bin; })
                                : std::vector<command::CommandPathEntry>::const_iterator{};
  auto outside_scope_misuse =
      path_plan && outside_path != path_plan->path_entries().end()
          ? command::detail::user_tool_path_metadata_is_fresh(outside_path->metadata, path_plan->trusted_home_metadata(), path_plan->anchor_set())
          : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no outside PATH metadata"))};
  ::chmod(path_bin.c_str(), S_IRWXU | S_IRGRP | S_IXGRP);
  auto path_changed = path_plan ? command::plan_is_fresh(*path_plan)
                                : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no final user PATH plan"))};

  CommandFixture symlink_fixture("user-tool-symlink-ancestor");
  auto const target_one = symlink_fixture.trusted_home / "target-one";
  auto const target_two = symlink_fixture.trusted_home / "target-two";
  symlink_fixture.executable_in(target_one, "linked-tool");
  symlink_fixture.executable_in(target_two, "linked-tool");
  auto const linked_bin = symlink_fixture.trusted_home / "linked-bin";
  std::filesystem::create_directory_symlink(target_one.filename(), linked_bin);
  auto symlink_options = symlink_fixture.options();
  symlink_options.startup_path = linked_bin.string() + ":" + symlink_fixture.bin.string();
  auto symlink_plan = command::seal_command_plan(*command::CommandIntent::structured({"linked-tool"}), symlink_options);
  std::filesystem::remove(linked_bin);
  std::filesystem::create_directory_symlink(target_two.filename(), linked_bin);
  auto symlink_changed = symlink_plan
                             ? command::plan_is_fresh(*symlink_plan)
                             : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no user-tool symlink plan"))};

  expect(executable_changed && !*executable_changed && outside_scope_misuse && !*outside_scope_misuse && path_changed && !*path_changed && symlink_changed &&
             !*symlink_changed,
         "user-tool freshness keeps final executable and PATH metadata plus every symlink ancestor exact and fails closed when used for an outside path");
}

void test_trusted_home_stable_identity_changes_stale()
{
  CommandFixture mode_fixture("trusted-home-mode-freshness");
  mode_fixture.executable("pwd");
  auto mode_plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), mode_fixture.options());
  ::chmod(mode_fixture.trusted_home.c_str(), S_IRWXU | S_IXGRP);
  auto mode_changed = mode_plan ? command::plan_is_fresh(*mode_plan)
                                : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no trusted-home mode plan"))};

  CommandFixture replacement_fixture("trusted-home-replacement-freshness");
  replacement_fixture.executable("pwd");
  auto replacement_plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), replacement_fixture.options());
  auto moved_home = replacement_fixture.trusted_home;
  moved_home += ".moved";
  std::filesystem::rename(replacement_fixture.trusted_home, moved_home);
  std::filesystem::create_directories(replacement_fixture.trusted_home);
  ::chmod(replacement_fixture.trusted_home.c_str(), S_IRWXU);
  auto replaced = replacement_plan
                      ? command::plan_is_fresh(*replacement_plan)
                      : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no trusted-home replacement plan"))};

  bool available_owner_or_group_change_staled = true;
  if (auto const shared_group = shared_supplementary_group_for_test())
  {
    CommandFixture group_fixture("trusted-home-group-freshness");
    group_fixture.executable("pwd");
    auto group_plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), group_fixture.options());
    bool const changed = ::chown(group_fixture.trusted_home.c_str(), ::geteuid(), *shared_group) == 0;
    auto group_changed = group_plan
                             ? command::plan_is_fresh(*group_plan)
                             : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no trusted-home group plan"))};
    available_owner_or_group_change_staled = changed && group_changed && !*group_changed;
  }
  else if (::geteuid() == 0)
  {
    CommandFixture owner_fixture("trusted-home-owner-freshness");
    owner_fixture.executable("pwd");
    auto owner_plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), owner_fixture.options());
    bool const changed = ::chown(owner_fixture.trusted_home.c_str(), 1, ::getegid()) == 0;
    auto owner_changed = owner_plan
                             ? command::plan_is_fresh(*owner_plan)
                             : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no trusted-home owner plan"))};
    available_owner_or_group_change_staled = changed && owner_changed && !*owner_changed;
  }

  expect(mode_changed && !*mode_changed && replaced && !*replaced && available_owner_or_group_change_staled,
         "trusted-home mode, replacement, and available owner/group identity changes stale sealed plans");
}

void test_synthetic_environment_roots_are_sealed_and_fresh()
{
  CommandFixture fixture("synthetic-environment-roots");
  fixture.executable("pwd");
  auto intent = command::CommandIntent::structured({"pwd"});
  auto authority_root = fixture.root / "ava-authority";
  std::filesystem::create_directories(authority_root);
  ::chmod(authority_root.c_str(), S_IRWXU);
  // Declare the foreign target first so its destructor runs after sealed_tmpdir.
  // The test later replaces sealed_tmpdir with a symlink to outside; teardown
  // must unlink that symlink and never follow it into the separately owned dir.
  auto outside = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-command-sealed-outside-");
  auto sealed_tmpdir = ScopedTestDirectory::create_unique(std::filesystem::temp_directory_path(), "ava-command-sealed-synthetic-");
  ::chmod(sealed_tmpdir.path().c_str(), S_IRWXU);
  ::chmod(outside.path().c_str(), S_IRWXU);

  auto loose_mode_options = fixture.options();
  ::chmod(loose_mode_options.environment.xdg_cache_home.c_str(), S_IRWXU | S_IRGRP);
  auto loose_mode = command::seal_command_plan(*intent, loose_mode_options);
  ::chmod(loose_mode_options.environment.xdg_cache_home.c_str(), S_IRWXU);

  auto sealed_options = fixture.options();
  sealed_options.environment.tmpdir = sealed_tmpdir.path();
  sealed_options.anchor_set = fixture.open_anchors({}, {sealed_tmpdir.path()});
  auto plan = command::seal_command_plan(*intent, sealed_options);
  auto before = plan ? command::plan_is_fresh(*plan) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  auto missing_options = fixture.options();
  missing_options.environment.tmpdir = fixture.root / "missing-synthetic-tmp";
  auto missing = command::seal_command_plan(*intent, missing_options);
  auto authority_overlap_options = fixture.options();
  authority_overlap_options.ava_authority_roots = {authority_root};
  authority_overlap_options.environment.tmpdir = authority_root;
  auto authority_overlap = command::seal_command_plan(*intent, authority_overlap_options);
  auto missing_authority_overlap_options = fixture.options();
  missing_authority_overlap_options.ava_authority_roots = {fixture.workspace / "future-ava-authority"};
  auto missing_authority_overlap = command::seal_command_plan(*intent, missing_authority_overlap_options);
  auto path_authority_options = fixture.options();
  path_authority_options.ava_authority_roots = {fixture.bin};
  auto path_authority_overlap = command::seal_command_plan(*intent, path_authority_options);
  auto const executable_authority = fixture.root / "executable-authority";
  fixture.executable_in(executable_authority, "authority-tool");
  auto executable_authority_options = fixture.options();
  executable_authority_options.startup_path = "/usr/bin:/bin";
  executable_authority_options.shell = "/bin/sh";
  executable_authority_options.ava_authority_roots = {executable_authority};
  auto executable_authority_overlap =
      command::seal_command_plan(*command::CommandIntent::structured({(executable_authority / "authority-tool").string()}), executable_authority_options);
  auto const credential_authority = fixture.root / "credentials.json";
  {
    std::ofstream output(credential_authority, std::ios::binary | std::ios::trunc);
    output << "secret";
  }
  ::chmod(credential_authority.c_str(), S_IRUSR | S_IWUSR);
  auto credential_authority_options = fixture.options();
  credential_authority_options.ava_authority_roots = {credential_authority};
  auto credential_authority_plan = command::seal_command_plan(*intent, credential_authority_options);
  {
    std::ofstream output(credential_authority, std::ios::binary | std::ios::app);
    output << "-changed";
  }
  auto credential_authority_stale = credential_authority_plan
                                        ? command::plan_is_fresh(*credential_authority_plan)
                                        : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};
  auto const future_authority = fixture.root / "future-ava-authority";
  auto missing_disjoint_authority_options = fixture.options();
  missing_disjoint_authority_options.ava_authority_roots = {future_authority};
  auto missing_disjoint_authority = command::seal_command_plan(*intent, missing_disjoint_authority_options);
  auto other_authority_options = fixture.options();
  other_authority_options.ava_authority_roots = {fixture.root / "other-future-ava-authority"};
  auto other_authority = command::seal_command_plan(*intent, other_authority_options);
  std::filesystem::create_directories(future_authority);
  ::chmod(future_authority.c_str(), S_IRWXU);
  auto created_authority_stale = missing_disjoint_authority
                                     ? command::plan_is_fresh(*missing_disjoint_authority)
                                     : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  std::filesystem::remove(sealed_tmpdir.path());
  std::filesystem::create_directory_symlink(outside.path(), sealed_tmpdir.path());
  auto replaced =
      plan ? command::plan_is_fresh(*plan) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  expect(plan && before && *before && !missing && !loose_mode && !authority_overlap && !missing_authority_overlap && !path_authority_overlap &&
             !executable_authority_overlap && credential_authority_plan && credential_authority_stale && !*credential_authority_stale &&
             missing_disjoint_authority && other_authority && missing_disjoint_authority->fingerprint() != other_authority->fingerprint() &&
             created_authority_stale && !*created_authority_stale && replaced && !*replaced,
         "synthetic HOME/XDG/TMP roots must already be owner-owned restrictive directories; AVA authority roots remain disjoint from workspace, PATH, and "
         "executable authority, support exact protected credential files, are fingerprinted by logical spelling, and become stale on creation or replacement");
}

void test_capabilities_scopes_and_standard_recipes()
{
  CommandFixture fixture("policy");
  for (auto const name : {"git", "cmake", "ctest", "pytest", "npm", "python"}) fixture.executable(name);
  auto options = fixture.options();
  auto pull = command::seal_command_plan(*command::CommandIntent::structured({"git", "pull"}), options);
  auto fetch = command::seal_command_plan(*command::CommandIntent::structured({"git", "fetch"}), options);
  auto clone = command::seal_command_plan(*command::CommandIntent::structured({"git", "clone", "origin"}), options);
  auto submodule = command::seal_command_plan(*command::CommandIntent::structured({"git", "submodule", "update"}), options);
  auto remote_add = command::seal_command_plan(*command::CommandIntent::structured({"git", "remote", "add", "origin", "url"}), options);
  auto remote_set_url = command::seal_command_plan(*command::CommandIntent::structured({"git", "remote", "set-url", "origin", "url"}), options);
  auto remote_remove = command::seal_command_plan(*command::CommandIntent::structured({"git", "remote", "remove", "origin"}), options);
  auto remote_rename = command::seal_command_plan(*command::CommandIntent::structured({"git", "remote", "rename", "origin", "upstream"}), options);
  auto remote_update = command::seal_command_plan(*command::CommandIntent::structured({"git", "remote", "update"}), options);
  auto remote_prune = command::seal_command_plan(*command::CommandIntent::structured({"git", "remote", "prune", "origin"}), options);
  auto force_push = command::seal_command_plan(*command::CommandIntent::structured({"git", "push", "--force-with-lease=refs/heads/main"}), options);
  auto git_status = command::seal_command_plan(*command::CommandIntent::structured({"/usr/bin/git", "status"}), options);
  auto git_diff = command::seal_command_plan(*command::CommandIntent::structured({"/usr/bin/git", "diff"}), options);
  auto git_log = command::seal_command_plan(*command::CommandIntent::structured({"/usr/bin/git", "log", "-1"}), options);
  auto cmake = command::seal_command_plan(*command::CommandIntent::structured({"cmake", "--build", "build"}), options);
  auto ctest = command::seal_command_plan(*command::CommandIntent::structured({"ctest", "--test-dir", "build"}), options);
  auto pytest = command::seal_command_plan(*command::CommandIntent::structured({"pytest", "tests"}), options);
  auto install = command::seal_command_plan(*command::CommandIntent::structured({"npm", "install"}), options);
  auto inline_python = command::seal_command_plan(*command::CommandIntent::structured({"python", "-c", "print(1)"}), options);

  auto const remote_mutating = [](ava::core::Result<command::CommandPlan> const& plan) {
    return plan && plan->classification().level == command::CommandLevel::Sensitive && plan->classification().capabilities.network_enabled &&
           plan->classification().capabilities.mutates_workspace && plan->classification().max_interactive_scope == command::InteractiveScope::Workspace;
  };
  auto const local_remote_mutation = [](ava::core::Result<command::CommandPlan> const& plan) {
    return plan && plan->classification().level == command::CommandLevel::Sensitive && plan->classification().capabilities.mutates_workspace &&
           !plan->classification().capabilities.network_enabled;
  };
  expect(remote_mutating(pull) && remote_mutating(fetch) && remote_mutating(clone) && remote_mutating(submodule) && remote_mutating(remote_update) &&
             local_remote_mutation(remote_add) && local_remote_mutation(remote_set_url) && local_remote_mutation(remote_remove) &&
             local_remote_mutation(remote_rename) && local_remote_mutation(remote_prune) && force_push &&
             force_push->classification().level == command::CommandLevel::Critical && force_push->classification().capabilities.network_enabled &&
             force_push->classification().capabilities.destructive_or_privileged &&
             force_push->classification().max_interactive_scope == command::InteractiveScope::Once && is_standard_recipe(git_status) &&
             is_standard_recipe(git_diff) && is_standard_recipe(git_log) && git_status->classification().capabilities.executes_mutable_project_code &&
             git_status->classification().capabilities.requires_containment && git_diff->classification().capabilities.requires_containment &&
             git_log->classification().capabilities.requires_containment && is_standard_recipe(cmake) && is_standard_recipe(ctest) &&
             is_standard_recipe(pytest) && cmake->classification().capabilities.executes_mutable_project_code &&
             cmake->classification().capabilities.requires_containment && install &&
             install->classification().max_interactive_scope == command::InteractiveScope::Workspace && inline_python &&
             inline_python->classification().max_interactive_scope == command::InteractiveScope::Once &&
             command::to_string(command::InteractiveScope::Session) == "session",
         "git remote add/set-url/remove/rename/update/prune and fetch expose exact local mutation and network capabilities; force-push is critical, "
         "git inspection remains Standard but requires containment for repository-controlled helpers; Standard/Sensitive cap at workspace, Critical caps at "
         "once, and Session is available without global prompting");
}

void test_workspace_origin_capabilities_apply_to_every_family()
{
  CommandFixture fixture("workspace-origin-invariants");
  auto workspace_bin = fixture.workspace / ".venv" / "bin";
  fixture.executable_in(workspace_bin, "git");
  fixture.executable_in(workspace_bin, "python");
  fixture.executable_in(workspace_bin, "shell");
  ::chmod((fixture.workspace / ".venv").c_str(), S_IRWXU);
  auto options = fixture.options();
  options.startup_path = workspace_bin.string() + ":" + fixture.bin.string();
  auto sensitive = command::seal_command_plan(*command::CommandIntent::structured({"git", "push"}), options);
  auto critical = command::seal_command_plan(*command::CommandIntent::structured({"python", "-c", "print(1)"}), options);
  auto raw_options = options;
  raw_options.shell = workspace_bin / "shell";
  auto raw = command::seal_command_plan(*command::CommandIntent::raw_shell("external-tool --secret"), raw_options);
  auto const contained_project_code = [](ava::core::Result<command::CommandPlan> const& plan) {
    return plan && plan->resolved_executable() && plan->resolved_executable()->origin == command::ExecutableOrigin::Workspace &&
           plan->classification().capabilities.executes_mutable_project_code && plan->classification().capabilities.requires_containment;
  };
  expect(contained_project_code(sensitive) && contained_project_code(critical) && contained_project_code(raw),
         "workspace-origin executables require mutable-project-code containment after Sensitive, Critical, and RawShell classification");
}

void test_ancestor_freshness_detects_mode_and_replacement()
{
  CommandFixture mode_fixture("ancestor-mode-freshness");
  mode_fixture.executable("pwd");
  auto mode_plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), mode_fixture.options());
  ::chmod(mode_fixture.root.c_str(), S_IRWXU | S_IWGRP);
  auto mode_changed =
      mode_plan ? command::plan_is_fresh(*mode_plan) : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  CommandFixture replacement_fixture("ancestor-replacement-freshness");
  replacement_fixture.executable("pwd");
  auto replacement_plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), replacement_fixture.options());
  auto moved_root = replacement_fixture.root;
  moved_root += ".moved";
  std::filesystem::rename(replacement_fixture.root, moved_root);
  std::filesystem::create_directory_symlink(moved_root, replacement_fixture.root);
  auto replaced = replacement_plan ? command::plan_is_fresh(*replacement_plan)
                                   : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  CommandFixture symlink_fixture("ancestor-symlink-freshness");
  symlink_fixture.executable("pwd");
  auto const symlink_target = symlink_fixture.root / "symlink-target";
  auto const symlink_bin = symlink_target / "bin";
  symlink_fixture.executable_in(symlink_bin, "linked-tool");
  std::filesystem::create_directory_symlink(symlink_target.filename(), symlink_fixture.root / "tool-link");
  auto symlink_options = symlink_fixture.options();
  symlink_options.startup_path = (symlink_fixture.root / "tool-link" / "bin").string();
  auto symlink_plan = command::seal_command_plan(*command::CommandIntent::structured({"linked-tool"}), symlink_options);
  std::filesystem::remove(symlink_fixture.root / "tool-link");
  std::filesystem::create_directory_symlink(symlink_target, symlink_fixture.root / "tool-link");
  auto symlink_changed = symlink_plan
                             ? command::plan_is_fresh(*symlink_plan)
                             : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no ancestor symlink plan"))};

  CommandFixture ctime_fixture("ancestor-ctime-freshness");
  ctime_fixture.executable("pwd");
  auto ctime_plan = command::seal_command_plan(*command::CommandIntent::structured({"pwd"}), ctime_fixture.options());
  {
    std::ofstream output(ctime_fixture.root / "unrelated-entry");
  }
  auto ctime_changed = ctime_plan ? command::plan_is_fresh(*ctime_plan)
                                  : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no ancestor ctime plan"))};

  CommandFixture recipe_fixture("recipe-ancestor-freshness");
  recipe_fixture.executable("ls");
  auto parent = recipe_fixture.workspace / "recipe-parent";
  auto target = parent / "target";
  std::filesystem::create_directories(target);
  ::chmod(parent.c_str(), S_IRWXU);
  ::chmod(target.c_str(), S_IRWXU);
  auto recipe_plan = command::seal_command_plan(*command::CommandIntent::structured({"/usr/bin/ls", "recipe-parent/target"}), recipe_fixture.options());
  ::chmod(parent.c_str(), S_IRWXU | S_IWGRP);
  auto recipe_changed = recipe_plan ? command::plan_is_fresh(*recipe_plan)
                                    : ava::core::Result<bool>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "no plan"))};

  auto const stale_or_error = [](ava::core::Result<bool> const& result) { return !result || !*result; };
  expect(stale_or_error(mode_changed) && stale_or_error(replaced) && symlink_changed && !*symlink_changed && ctime_changed && !*ctime_changed &&
             recipe_changed && *recipe_changed && recipe_plan->classification().capabilities.requires_containment &&
             !recipe_plan->classification().capabilities.executes_mutable_project_code,
         "external sealed ancestors go stale after ctime churn, unsafe mode, namespace replacement, or symlink identity changes while workspace-internal "
         "inspection paths remain descriptor-bound and require containment against post-check path swaps");
}

void test_stable_recipe_identity_is_scope_aware_and_secret_free()
{
  CommandFixture fixture("stable-recipe-identity");
  for (auto const name : {"cmake", "npm", "python"}) fixture.executable(name);
  std::filesystem::create_directories(fixture.workspace / "build-other");
  ::chmod((fixture.workspace / "build-other").c_str(), S_IRWXU);

  ava::permissions::CommandContainmentInfo const contained{.available = true, .profile_id = "ava-development-v1", .network_allowed = false};
  auto const first = command::seal_command_plan(*command::CommandIntent::structured({"cmake", "--build", "build"}), fixture.options("recipe-profile-v1"));
  auto const normalized =
      command::seal_command_plan(*command::CommandIntent::structured({"cmake", "--build", "./build"}), fixture.options("recipe-profile-v1"));
  auto const other_build =
      command::seal_command_plan(*command::CommandIntent::structured({"cmake", "--build", "build-other"}), fixture.options("recipe-profile-v1"));
  auto const environment_policy_changed =
      command::seal_command_plan(*command::CommandIntent::structured({"cmake", "--build", "build"}), fixture.options("recipe-profile-v2"));

  auto changed_roots = fixture.options("recipe-profile-v1");
  changed_roots.environment.tmpdir = fixture.root / "different-synthetic-tmp";
  std::filesystem::create_directories(changed_roots.environment.tmpdir);
  ::chmod(changed_roots.environment.tmpdir.c_str(), S_IRWXU);
  changed_roots.anchor_set = fixture.open_anchors({}, {changed_roots.environment.tmpdir});
  auto const synthetic_changed = command::seal_command_plan(*command::CommandIntent::structured({"cmake", "--build", "build"}), changed_roots);

  auto const workspace_two = fixture.root / "workspace-two";
  std::filesystem::create_directories(workspace_two / "build");
  ::chmod(workspace_two.c_str(), S_IRWXU);
  ::chmod((workspace_two / "build").c_str(), S_IRWXU);
  auto workspace_two_options = fixture.options("recipe-profile-v1");
  workspace_two_options.workspace = workspace_two;
  workspace_two_options.anchor_set = fixture.open_anchors(workspace_two);
  auto const workspace_two_plan = command::seal_command_plan(*command::CommandIntent::structured({"cmake", "--build", "build"}), workspace_two_options);

  auto const other_bin = fixture.root / "other-bin";
  fixture.executable_in(other_bin, "cmake");
  auto changed_executable_options = fixture.options("recipe-profile-v1");
  changed_executable_options.startup_path = other_bin.string();
  auto const changed_executable = command::seal_command_plan(*command::CommandIntent::structured({"cmake", "--build", "build"}), changed_executable_options);

  auto const npm_test = command::seal_command_plan(*command::CommandIntent::structured({"npm", "run", "test"}), fixture.options("recipe-profile-v1"));
  auto const npm_lint = command::seal_command_plan(*command::CommandIntent::structured({"npm", "run", "lint"}), fixture.options("recipe-profile-v1"));
  auto const secret_npm = command::seal_command_plan(*command::CommandIntent::structured({"npm", "run", "test", "--", "--token", "never-display"}),
                                                     fixture.options("recipe-profile-v1"));
  auto const secret_raw = command::seal_command_plan(*command::CommandIntent::structured({"python", "-c", "print('token=never-display')"}), fixture.options());

  auto metadata = [&](ava::core::Result<command::CommandPlan> const& plan) {
    return plan ? ava::permissions::command_permission_metadata(*plan, contained) : ava::permissions::CommandPermissionMetadata{};
  };
  auto const first_metadata = metadata(first);
  auto const normalized_metadata = metadata(normalized);
  auto const other_build_metadata = metadata(other_build);
  auto const environment_policy_changed_metadata = metadata(environment_policy_changed);
  auto const synthetic_changed_metadata = metadata(synthetic_changed);
  auto const workspace_two_metadata = metadata(workspace_two_plan);
  auto const changed_executable_metadata = metadata(changed_executable);
  auto const npm_test_metadata = metadata(npm_test);
  auto const npm_lint_metadata = metadata(npm_lint);
  auto const secret_npm_metadata = metadata(secret_npm);
  auto const secret_raw_metadata = metadata(secret_raw);

  expect(first && normalized && other_build && environment_policy_changed && synthetic_changed && workspace_two_plan && changed_executable && npm_test &&
             npm_lint && secret_npm && secret_raw && first_metadata.global_recipe_key.starts_with("sha256:ava-command-recipe-v1:") &&
             first_metadata.workspace_recipe_key.starts_with("sha256:ava-command-workspace-recipe-v1:") &&
             first_metadata.global_recipe_key == normalized_metadata.global_recipe_key &&
             first_metadata.workspace_recipe_key == normalized_metadata.workspace_recipe_key &&
             first_metadata.global_recipe_key != environment_policy_changed_metadata.global_recipe_key &&
             first_metadata.workspace_recipe_key != environment_policy_changed_metadata.workspace_recipe_key &&
             first_metadata.global_recipe_key == synthetic_changed_metadata.global_recipe_key &&
             first_metadata.workspace_recipe_key == synthetic_changed_metadata.workspace_recipe_key &&
             first_metadata.global_recipe_key == workspace_two_metadata.global_recipe_key &&
             first_metadata.workspace_recipe_key != workspace_two_metadata.workspace_recipe_key &&
             first_metadata.global_recipe_key != changed_executable_metadata.global_recipe_key &&
             first_metadata.global_recipe_key != other_build_metadata.global_recipe_key &&
             npm_test_metadata.global_recipe_key != npm_lint_metadata.global_recipe_key &&
             ava::permissions::command_permission_allows_reusable_grant(first_metadata) &&
             first_metadata.recipe_display.find(fixture.workspace.string()) == std::string::npos && secret_npm_metadata.global_recipe_key.empty() &&
             secret_npm_metadata.workspace_recipe_key.empty() && secret_npm_metadata.recipe_display.empty() && secret_raw_metadata.global_recipe_key.empty() &&
             secret_raw_metadata.workspace_recipe_key.empty() && secret_raw_metadata.recipe_display.empty(),
         "stable recipe identities normalize workspace paths, ignore synthetic roots, bind environment-policy version, executable, and workspace scope, "
         "distinguish typed actions, and never mint or display secrets for standard or critical commands");
}

void test_recipe_argument_domains_and_unavailable_containment()
{
  CommandFixture fixture("recipe-argument-domains");
  fixture.executable("git");
  fixture.executable("curl");
  auto const remote_path = fixture.workspace / "origin";
  std::filesystem::create_directories(remote_path);
  ::chmod(remote_path.c_str(), S_IRWXU);

  auto const absolute_remote =
      command::seal_command_plan(*command::CommandIntent::structured({"git", "push", remote_path.string(), "refs/heads/main"}), fixture.options());
  auto const literal_remote =
      command::seal_command_plan(*command::CommandIntent::structured({"git", "push", "workspace:origin", "refs/heads/main"}), fixture.options());
  ava::permissions::CommandContainmentInfo const contained{.available = true, .profile_id = "ava-development-v1", .network_allowed = false};
  auto const absolute_metadata =
      absolute_remote ? ava::permissions::command_permission_metadata(*absolute_remote, contained) : ava::permissions::CommandPermissionMetadata{};
  auto const literal_metadata =
      literal_remote ? ava::permissions::command_permission_metadata(*literal_remote, contained) : ava::permissions::CommandPermissionMetadata{};

  auto const curl = command::seal_command_plan(*command::CommandIntent::structured({"curl", "https://example.test/releases"}), fixture.options());
  auto const unavailable = curl ? ava::permissions::command_permission_metadata(*curl) : ava::permissions::CommandPermissionMetadata{};
  auto const unavailable_decision = ava::permissions::decide(unavailable);
  auto forged_unavailable = absolute_metadata;
  forged_unavailable.level = command::CommandLevel::Sensitive;
  forged_unavailable.backend_maximum_scope = command::InteractiveScope::Workspace;
  forged_unavailable.containment_status = ava::permissions::CommandContainmentStatus::Unavailable;
  forged_unavailable.effective_allowed_scopes = {command::InteractiveScope::Once, command::InteractiveScope::Session, command::InteractiveScope::Workspace};
  auto const forged_unavailable_scopes = ava::permissions::command_permission_effective_scopes(forged_unavailable);

  ava::permissions::CommandPermissionMetadata session_bounded = absolute_metadata;
  session_bounded.backend_maximum_scope = command::InteractiveScope::Session;
  auto const session_scopes = ava::permissions::command_permission_effective_scopes(session_bounded);
  session_bounded.effective_allowed_scopes = session_scopes;
  ava::permissions::PermissionPrompt session_prompt;
  session_prompt.operation = ava::permissions::Operation::RunCommand;
  session_prompt.command_metadata = session_bounded;

  expect(absolute_remote && literal_remote && absolute_metadata.global_recipe_key != literal_metadata.global_recipe_key &&
             absolute_metadata.workspace_recipe_key != literal_metadata.workspace_recipe_key &&
             absolute_metadata.recipe_display.find("workspace_path=workspace:origin") != std::string::npos &&
             literal_metadata.recipe_display.find("literal=workspace:origin") != std::string::npos && curl &&
             unavailable.level == command::CommandLevel::Critical && unavailable.backend_maximum_scope == command::InteractiveScope::Once &&
             unavailable.global_recipe_key.empty() && unavailable.workspace_recipe_key.empty() &&
             unavailable.effective_allowed_scopes == std::vector<command::InteractiveScope>{command::InteractiveScope::Once} &&
             unavailable_decision.risk == ava::permissions::PermissionRisk::Critical &&
             !ava::permissions::command_permission_allows_reusable_grant(unavailable) &&
             forged_unavailable_scopes == std::vector<command::InteractiveScope>{command::InteractiveScope::Once} &&
             !ava::permissions::command_permission_allows_reusable_grant(forged_unavailable) &&
             session_scopes == std::vector<command::InteractiveScope>{command::InteractiveScope::Once, command::InteractiveScope::Session} &&
             ava::permissions::command_permission_allows_reusable_grant(session_bounded) &&
             !ava::permissions::command_prompt_allows_persistent_allow(session_prompt),
         "recipe hashes domain-separate workspace paths from git-push literals; unavailable Sensitive network commands are Critical one-shot without stable "
         "authority; and a Session backend maximum grants Session but never Workspace or a persistent workspace allow");
}

void test_redacted_diagnostics_and_value_equality()
{
  CommandFixture fixture("redacted-diagnostics");
  fixture.executable("secret-tool");
  auto direct_intent = command::CommandIntent::structured({"secret-tool", "--token=argv-secret"});
  auto first = command::prepare_command(*direct_intent, fixture.options());
  auto second = command::prepare_command(*direct_intent, fixture.options());
  auto raw_intent = command::CommandIntent::raw_shell("external-tool --token=raw-shell-secret");
  auto raw_plan = command::seal_command_plan(*raw_intent, fixture.options());
  auto direct_summary = first ? first->plan().redacted_summary() : std::string{};
  auto raw_summary = raw_plan ? raw_plan->redacted_summary() : std::string{};
  expect(first && second, "direct command preparation succeeds for both invocations");
  expect(first && second && *first == *second, "prepared command environments compare by value across invocations");
  expect(direct_summary.find("secret-tool") == std::string::npos, "redacted summary excludes the resolved executable name");
  expect(direct_summary.find("argv-secret") == std::string::npos, "redacted summary excludes secret-bearing argv values");
  expect(direct_summary.find(fixture.workspace.string()) == std::string::npos, "redacted summary excludes the local workspace path");
  expect(raw_plan.has_value(), "raw shell plan seals successfully");
  expect(raw_plan && raw_plan->display_json().find("raw-shell-secret") != std::string::npos,
         "raw shell display JSON retains local-sensitive request payloads for local diagnostics");
  expect(raw_summary.find("raw-shell-secret") == std::string::npos, "redacted summary redacts raw shell request payloads");
  expect(command::to_string(static_cast<command::CommandRuntimeMode>(999)) == "unknown", "unknown CommandRuntimeMode stays explicit as 'unknown'");
  expect(command::to_string(static_cast<command::CommandLevel>(999)) == "unknown", "unknown CommandLevel stays explicit as 'unknown'");
  expect(command::to_string(static_cast<command::CommandFamily>(999)) == "unknown", "unknown CommandFamily stays explicit as 'unknown'");
  expect(command::to_string(static_cast<command::InteractiveScope>(999)) == "unknown", "unknown InteractiveScope stays explicit as 'unknown'");
}

void test_unsafe_executables_and_ancestors_are_rejected_without_blocking()
{
  CommandFixture fixture("unsafe-metadata");
  fixture.executable("unsafe");
  ::chmod((fixture.bin / "unsafe").c_str(), S_IRUSR | S_IWUSR | S_IXUSR | S_IWGRP);
  auto writable_file = command::seal_command_plan(*command::CommandIntent::structured({"unsafe"}), fixture.options());

  fixture.executable("hard-target");
  std::filesystem::create_hard_link(fixture.bin / "hard-target", fixture.bin / "hard-linked");
  auto hard_link = command::seal_command_plan(*command::CommandIntent::structured({"hard-linked"}), fixture.options());

  auto const unsafe_dir = fixture.root / "unsafe-ancestor";
  fixture.executable_in(unsafe_dir, "ancestor-tool");
  ::chmod(unsafe_dir.c_str(), S_IRWXU | S_IRWXO);
  auto ancestor_options = fixture.options();
  ancestor_options.startup_path = unsafe_dir.string();
  auto unsafe_ancestor = command::seal_command_plan(*command::CommandIntent::structured({"ancestor-tool"}), ancestor_options);

  auto const fifo = fixture.bin / "fifo-tool";
  ::mkfifo(fifo.c_str(), S_IRUSR | S_IWUSR);
  auto fifo_plan = command::seal_command_plan(*command::CommandIntent::structured({"fifo-tool"}), fixture.options());

  expect(!writable_file && !hard_link && !unsafe_ancestor && !fifo_plan,
         "unsafe executable modes, link counts, writable ancestors, and FIFO candidates are rejected through nonblocking metadata inspection");
}

void test_credential_bearing_commands_never_mint_recipes()
{
  CommandFixture fixture("credential-recipe-refusal");
  for (auto const name : {"curl", "wget", "cmake"}) fixture.executable(name);
  ava::permissions::CommandContainmentInfo const contained{.available = true, .profile_id = "ava-development-v1", .network_allowed = false};
  auto metadata = [&](ava::core::Result<command::CommandPlan> const& plan) {
    return plan ? ava::permissions::command_permission_metadata(*plan, contained) : ava::permissions::CommandPermissionMetadata{};
  };

  struct SecretCase
  {
    std::string label;
    std::vector<std::string> argv;
    std::string secret_value;
  };
  std::vector<SecretCase> const cases{
      {"curl -u separate", {"curl", "-u", "alice:s3cr3t", "https://example.test/releases"}, "s3cr3t"},
      {"curl --user= concat", {"curl", "--user=alice:s3cr3t", "https://example.test/releases"}, "s3cr3t"},
      {"curl -H Authorization", {"curl", "-H", "Authorization: Bearer tok_abc123", "https://example.test/releases"}, "tok_abc123"},
      {"curl --header= concat", {"curl", "--header=Authorization: Bearer tok_abc123", "https://example.test/releases"}, "tok_abc123"},
      {"curl --cookie", {"curl", "--cookie", "session=cooksecret", "https://example.test/releases"}, "cooksecret"},
      {"curl --oauth2-bearer", {"curl", "--oauth2-bearer", "bear_tok", "https://example.test/releases"}, "bear_tok"},
      {"curl --proxy-user", {"curl", "--proxy-user", "proxy:proxypass", "https://example.test/releases"}, "proxypass"},
      {"curl --cert --key --pass", {"curl", "--cert", "client.pem", "--key", "client.key", "--pass", "certpass", "https://example.test/releases"}, "certpass"},
      {"curl --aws-sigv4", {"curl", "--aws-sigv4", "aws:amz:region:service", "https://example.test/releases"}, "amz"},
      {"wget --http-user --http-password", {"wget", "--http-user", "alice", "--http-password", "wpass", "https://example.test/releases"}, "wpass"},
      {"wget --http-header", {"wget", "--http-header", "Authorization: Bearer tok_wget", "https://example.test/releases"}, "tok_wget"},
      {"url query token", {"curl", "https://example.test/api?token=querysecret"}, "querysecret"},
      {"url query api_key", {"curl", "https://example.test/api?api_key=keysecret"}, "keysecret"},
      {"url query access_token", {"curl", "https://example.test/api?access_token=accsecret"}, "accsecret"},
      {"url query key", {"curl", "https://example.test/api?key=opaquequerysecret"}, "opaquequerysecret"},
      {"arbitrary queried URL", {"curl", "https://example.test/api?custom_name=customquerysecret"}, "customquerysecret"},
      {"url userinfo", {"curl", "https://alice:userpass@example.test/releases"}, "userpass"},
  };

  bool all_refused = true;
  for (auto const& test_case : cases)
  {
    auto plan = command::seal_command_plan(*command::CommandIntent::structured(test_case.argv), fixture.options("cred-profile"));
    auto const m = metadata(plan);
    all_refused = all_refused && m.global_recipe_key.empty() && m.workspace_recipe_key.empty() && m.recipe_display.empty();
    if (!m.global_recipe_key.empty() || !m.workspace_recipe_key.empty() || !m.recipe_display.empty())
    {
      expect(false, std::string("recipe minting must be refused for credential-bearing command: ") + std::string(test_case.label));
    }
  }

  // A non-secret curl URL must still mint a recipe when contained.
  auto safe_curl = command::seal_command_plan(*command::CommandIntent::structured({"curl", "https://example.test/releases"}), fixture.options("cred-profile"));
  auto const safe_metadata = metadata(safe_curl);
  expect(all_refused && safe_curl && !safe_metadata.global_recipe_key.empty() && !safe_metadata.recipe_display.empty(),
         "credential-bearing curl/wget commands with separate short, concatenated long, --option=value, cookie, oauth2, proxy-user, cert/key/pass, aws-sigv4, "
         "wget "
         "auth, arbitrary URL query, and URL userinfo forms never mint recipe keys or display, while a plain curl URL still does");
}

}  // namespace

void run_command_tests()
{
  test_compatibility_parser_is_lossless_or_raw_shell();
  test_compatibility_shell_words_are_critical_raw_shell();
  test_intent_bounds_and_path_bounds_do_not_underflow();
  test_private_primary_group_directories_are_accepted();
  test_world_writable_directories_are_rejected();
  test_shared_supplementary_group_directories_are_rejected();
  test_group_changes_invalidate_sealed_plans();
  test_known_symlink_alias_keeps_invoked_family();
  test_workspace_spoofs_are_not_inspection_recipes();
  test_recipe_paths_are_logical_sealed_and_fresh();
  test_sealed_freshness_and_symlink_provenance();
  test_workspace_cwd_path_and_interpreter_freshness();
  test_raw_shell_binds_configured_shell_and_accepts_env_shebang();
  test_environment_is_synthetic_and_digest_bound();
  test_delegated_context_omits_automatic_host_user_tools();
  test_logical_workspace_and_cwd_spelling_is_preserved();
  test_only_direct_trusted_home_entry_churn_stays_fresh_for_user_tools();
  test_user_tool_scope_keeps_final_and_symlink_identity_strict();
  test_trusted_home_stable_identity_changes_stale();
  test_synthetic_environment_roots_are_sealed_and_fresh();
  test_capabilities_scopes_and_standard_recipes();
  test_workspace_origin_capabilities_apply_to_every_family();
  test_ancestor_freshness_detects_mode_and_replacement();
  test_stable_recipe_identity_is_scope_aware_and_secret_free();
  test_credential_bearing_commands_never_mint_recipes();
  test_recipe_argument_domains_and_unavailable_containment();
  test_redacted_diagnostics_and_value_equality();
  test_unsafe_executables_and_ancestors_are_rejected_without_blocking();
}
