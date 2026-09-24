#include "sys.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/app/commands.h"
#include "ava/app/headless_policy.h"
#include "ava/app/print_mode.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/mode.h"
#include "ava/agent/tool_dispatcher.h"
#include "ava/tools/bash_tool.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/search_tools.h"
#include "ava/tui/composer.h"
#include "ava/tui/terminal.h"
#include "ava/config/auth.h"
#include "ava/config/model_config.h"
#include "ava/config/openai_oauth.h"
#include "ava/config/prompt_config.h"
#include "ava/config/xdg_paths.h"
#include "ava/session/compaction.h"
#include "ava/session/export.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission.h"
#include "ava/provider/openai_provider.h"
#include "ava/provider/provider_utils.h"
#include "ava/context/context_loader.h"
#include "ava/core/atomic_file.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/mode.h"
#include "ava/core/path.h"
#include "ava/core/process_args.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

void test_mode_parsing()
{
  static_assert(std::is_same_v<ava::agent::Mode, ava::core::Mode>, "agent::Mode must be an alias of core::Mode");

  auto const build = ava::agent::parse_mode("build");
  auto const plan = ava::agent::parse_mode("plan");
  auto const bad = ava::agent::parse_mode("other");
  auto const core_build = ava::core::parse_mode("build");
  auto const core_plan = ava::core::parse_mode("plan");
  auto const core_bad = ava::core::parse_mode("other");

  expect(build && *build == ava::agent::Mode::Build, "build mode parses");
  expect(plan && *plan == ava::agent::Mode::Plan, "plan mode parses");
  expect(!bad, "unknown mode fails");
  expect(ava::agent::toggle_mode(ava::agent::Mode::Build) == ava::agent::Mode::Plan, "build toggles to plan");
  expect(core_build && *core_build == ava::core::Mode::Build && *core_build == ava::agent::Mode::Build, "core build mode parses identically");
  expect(core_plan && *core_plan == ava::core::Mode::Plan && *core_plan == ava::agent::Mode::Plan, "core plan mode parses identically");
  expect(!core_bad, "core unknown mode fails");
  expect(ava::agent::to_string(ava::agent::Mode::Build) == ava::core::to_string(ava::core::Mode::Build) &&
             ava::agent::to_string(ava::agent::Mode::Build) == "build",
         "agent/core build mode strings match");
  expect(
      ava::agent::to_string(ava::agent::Mode::Plan) == ava::core::to_string(ava::core::Mode::Plan) && ava::agent::to_string(ava::agent::Mode::Plan) == "plan",
      "agent/core plan mode strings match");
  expect(ava::agent::toggle_mode(ava::agent::Mode::Build) == ava::core::toggle_mode(ava::core::Mode::Build) &&
             ava::agent::toggle_mode(ava::agent::Mode::Plan) == ava::core::toggle_mode(ava::core::Mode::Plan),
         "agent/core toggle_mode remain identical");
}

constexpr int lifecycle_child_setup_failed = 125;
constexpr int lifecycle_child_still_dumpable = 126;

struct ChildWaitResult
{
  bool reaped = false;
  bool timed_out = false;
  int status = 0;
};

// Poll one helper until it exits or the lifecycle-test deadline expires.
ChildWaitResult wait_for_child_until(pid_t child, std::chrono::steady_clock::duration timeout)
{
  auto const deadline = ava::tests::now_plus_seconds(timeout);
  while (std::chrono::steady_clock::now() < deadline)
  {
    int status = 0;
    auto const waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child)
      return {.reaped = true, .status = status};
    if (waited < 0 && errno != EINTR)
      return {};
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return {.timed_out = true};
}

// Force a stuck isolated child to terminate and synchronously reap it without relying on a catchable termination signal.
bool kill_and_reap_child(pid_t child)
{
  static_cast<void>(::kill(child, SIGKILL));
  int status = 0;
  pid_t waited;
  do
  {
    waited = ::waitpid(child, &status, 0);
  }
  while (waited < 0 && errno == EINTR);
  return waited == child;
}

enum class LifecycleScenarioResult
{
  success,
  abort,
};

// Execute one lifecycle scenario in a freshly initialized helper process and enforce bounded cleanup without forking the test runner.
void expect_lifecycle_scenario(std::string_view scenario, LifecycleScenarioResult expected_result, std::string_view description)
{
  posix_spawn_file_actions_t actions;
  int const actions_result = ::posix_spawn_file_actions_init(&actions);
  if (actions_result != 0)
  {
    expect(false, "Application lifecycle test could not initialize spawn actions: " + std::string(description));
    return;
  }

  if (expected_result == LifecycleScenarioResult::abort)
  {
    int const redirect_result = ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (redirect_result != 0)
    {
      ::posix_spawn_file_actions_destroy(&actions);
      expect(false, "Application lifecycle death test could not silence helper stderr: " + std::string(description));
      return;
    }
  }

  std::string scenario_argument{scenario};
  std::array<char*, 3> arguments{const_cast<char*>(AVA_CORE_APPLICATION_LIFECYCLE_HELPER_PATH), scenario_argument.data(), nullptr};
  pid_t child = -1;
  int const spawn_result = ::posix_spawn(&child, AVA_CORE_APPLICATION_LIFECYCLE_HELPER_PATH, &actions, nullptr, arguments.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);
  if (spawn_result != 0)
  {
    expect(false, "Application lifecycle test could not spawn helper (" + std::string(std::strerror(spawn_result)) + "): " + std::string(description));
    return;
  }

  auto const result = wait_for_child_until(child, std::chrono::seconds(4));
  if (result.timed_out)
  {
    bool const reaped = kill_and_reap_child(child);
    expect(false, "Application lifecycle helper timed out and required SIGKILL cleanup: " + std::string(description));
    expect(reaped, "Application lifecycle test reaped its SIGKILL cleanup helper: " + std::string(description));
    return;
  }
  if (!result.reaped)
  {
    bool const reaped = kill_and_reap_child(child);
    expect(false, "Application lifecycle test could not observe its helper: " + std::string(description));
    expect(reaped, "Application lifecycle test reaped its unobserved helper: " + std::string(description));
    return;
  }
  if (WIFEXITED(result.status) && WEXITSTATUS(result.status) == lifecycle_child_setup_failed)
  {
    expect(false, "Application lifecycle death helper could not disable core dumps: " + std::string(description));
    return;
  }
  if (WIFEXITED(result.status) && WEXITSTATUS(result.status) == lifecycle_child_still_dumpable)
  {
    expect(false, "Application lifecycle death helper allowed external core handling: " + std::string(description));
    return;
  }

  bool const succeeded = WIFEXITED(result.status) && WEXITSTATUS(result.status) == EXIT_SUCCESS;
  bool const aborted = WIFSIGNALED(result.status) && WTERMSIG(result.status) == SIGABRT;
  expect(expected_result == LifecycleScenarioResult::success ? succeeded : aborted,
         "Application lifecycle helper produced the expected result: " + std::string(description));
}

// Cover each supported Application lifecycle contract in its own executable lifetime.
void test_application_lifecycle_contracts()
{
#ifdef CWDEBUG
  expect_lifecycle_scenario("initialized-instance", LifecycleScenarioResult::success, "initialized instance publication");
  expect_lifecycle_scenario("instance-before-initialize", LifecycleScenarioResult::abort, "instance access before initialize");
  expect_lifecycle_scenario("instance-after-destruction", LifecycleScenarioResult::abort, "instance access after destruction");
  expect_lifecycle_scenario("duplicate-live-instance", LifecycleScenarioResult::abort, "duplicate live Application construction");
#endif
  expect_lifecycle_scenario("signal-construction", LifecycleScenarioResult::success, "signal construction, pending delivery, and atomic claim");
  expect_lifecycle_scenario("signal-immediate-output", LifecycleScenarioResult::success, "immediate output preserves permanent SIGPIPE suppression");
  expect_lifecycle_scenario("signal-teardown", LifecycleScenarioResult::success, "ordinary signal teardown postcondition");
  expect_lifecycle_scenario("joined-worker", LifecycleScenarioResult::success, "joined worker before Application destruction");
#if CW_DEBUG && defined(__linux__)
  expect_lifecycle_scenario("live-worker-destruction", LifecycleScenarioResult::abort, "Application destruction with a live worker");
#endif
}

void test_json_escape_control_characters()
{
  auto const escaped = ava::session::json_escape(std::string("a\x01\b\f", 4));
  expect(escaped == "a\\u0001\\b\\f", "json_escape escapes all JSON control characters");
}

std::string nested_json_object(std::size_t depth)
{
  std::string json;
  json.reserve(depth * 6 + 1);
  for (std::size_t index = 0; index < depth; ++index)
    json += "{\"x\":";
  json += '0';
  json.append(depth, '}');
  return json;
}

void test_core_json_nesting_limit()
{
  auto const below = nested_json_object(ava::core::json::kMaxNestingDepth - 1);
  auto const at = nested_json_object(ava::core::json::kMaxNestingDepth);
  auto const above = nested_json_object(ava::core::json::kMaxNestingDepth + 1);
  auto const adversarial = nested_json_object(4096);
  expect(ava::core::json::is_valid_object(below) && ava::core::json::is_valid_object(at) && !ava::core::json::is_valid_object(above) &&
             !ava::core::json::is_valid_object(adversarial),
         "core JSON validation accepts the documented nesting boundary and rejects deeper input before recursive parsing");
  expect(ava::provider::is_valid_json_object(at) && !ava::provider::is_valid_json_object(above),
         "provider strict object validation inherits the shared core JSON nesting limit");

  std::string invalid_object = "{\"argument\":\"";
  invalid_object.push_back(static_cast<char>(0xFF));
  invalid_object += "\"}";
  std::string invalid_array = "[{\"argument\":\"";
  invalid_array.push_back(static_cast<char>(0xFF));
  invalid_array += "\"}]";
  expect(!ava::core::json::is_valid_object(invalid_object) && !ava::core::json::strict_objects_in_array_field("{\"items\":" + invalid_array + "}", "items") &&
             !ava::provider::is_valid_json_object(invalid_object) && ava::core::json::is_valid_object("{\"unicode\":\"é\"}") &&
             ava::core::json::is_valid_object("{\"pair\":\"\\uD834\\uDD1E\"}"),
         "core object and array validation reject invalid UTF-8 while preserving valid Unicode and surrogate pairs");
}

void test_core_json_top_level_lookup()
{
  std::string const document =
      "{\"data\":{\"type\":\"bad\"},\"items\":[{\"type\":\"array_bad\"}],"
      "\"text\":\"contains \\\"type\\\":\\\"string_bad\\\"\",\"type\":\"good\"}";
  auto const type = ava::core::json::string_field(document, "type");
  expect(type && *type == "good", "JSON string_field reads only top-level object keys");
  expect(!ava::core::json::string_field(document, "items.type"), "JSON lookup does not invent nested paths");

  std::string const arrays_first = "{\"items\":[{\"models\":[{\"id\":\"bad\"}]}],\"models\":[{\"id\":\"ok\"}]}";
  auto const models = ava::core::json::objects_in_array_field(arrays_first, "models");
  auto const model_id = models.empty() ? std::optional<std::string>{} : ava::core::json::string_field(models[0], "id");
  expect(models.size() == 1 && model_id && *model_id == "ok", "JSON array lookup ignores nested arrays before top-level field");
  auto const paths = ava::core::json::strings_in_array_field(
      "{\"items\":[\"bad\"],\"changed_paths\":[\"src/main.cpp\",\"a\\\\b\",{\"skip\":\"nested\"},[\"also skip\"]]}", "changed_paths");
  expect(paths.size() == 2 && paths[0] == "src/main.cpp" && paths[1] == "a\\b", "JSON string array lookup reads only top-level string array elements");

  auto const surrogate_pair = ava::core::json::string_field("{\"text\":\"\\uD834\\uDD1E\"}", "text");
  expect(surrogate_pair && *surrogate_pair == std::string("\xF0\x9D\x84\x9E"), "JSON string_field decodes UTF-16 surrogate pairs");
  auto const lone_high_surrogate = ava::core::json::string_field("{\"text\":\"\\uD834x\"}", "text");
  expect(lone_high_surrogate && *lone_high_surrogate == std::string("\xEF\xBF\xBDx"), "JSON string_field replaces lone high surrogates");
  auto const lone_low_surrogate = ava::core::json::string_field("{\"text\":\"\\uDD1E\"}", "text");
  expect(lone_low_surrogate && *lone_low_surrogate == std::string("\xEF\xBF\xBD"), "JSON string_field replaces lone low surrogates");
  auto const high_then_non_low = ava::core::json::string_field("{\"text\":\"\\uD834\\u0061\"}", "text");
  expect(high_then_non_low && *high_then_non_low == std::string("\xEF\xBF\xBD") + "a",
         "JSON string_field leaves non-low escape after replacing high surrogate");
  expect(!ava::core::json::string_field("{\"text\":\"\\u12xz\"}", "text"), "JSON string_field rejects malformed unicode escapes");
  expect(!ava::core::json::string_field("{\"text\":\"\\q\"}", "text"), "JSON string_field rejects invalid escapes");
}

std::string read_test_file(std::filesystem::path const& path)
{
  std::ifstream input(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void write_test_file(std::filesystem::path const& path, std::string_view content)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
}

void test_process_arg_workspace_relative_detection()
{
  expect(!ava::core::is_workspace_relative_process_arg("node"), "bare executable names are not treated as workspace-relative paths");
  expect(!ava::core::is_workspace_relative_process_arg("--stdio"), "plain flags are not treated as workspace-relative paths");
  expect(!ava::core::is_workspace_relative_process_arg("/usr/bin/node"), "absolute paths are not workspace-relative paths");
  expect(ava::core::is_workspace_relative_process_arg("."), "current-directory process arg is workspace-relative");
  expect(ava::core::is_workspace_relative_process_arg("../server.js"), "parent-relative process arg is workspace-relative");
  expect(ava::core::is_workspace_relative_process_arg(".ava/server.js"), "slash-containing relative process arg is workspace-relative");
  expect(ava::core::is_workspace_relative_process_arg("--loader=.ava/loader.js"), "flag value paths are checked for workspace-relative launches");

  auto const root = create_empty_root("core-process-cwd");

  auto const workspace = root / "workspace";
  auto const global_config_dir = root / "config";
  std::filesystem::create_directories(workspace / ".ava");
  std::filesystem::create_directories(global_config_dir);
  expect(ava::core::safe_global_process_cwd(global_config_dir / "mcp.json", workspace) == ava::core::normalized_absolute_path(global_config_dir),
         "global process cwd uses the config directory when it is outside the workspace");
  expect(ava::core::safe_global_process_cwd(workspace / "global-lsp.json", workspace) == std::filesystem::path{"/"},
         "global process cwd falls back outside the workspace when the config source is workspace-contained");
}

void test_atomic_text_file_write()
{
  auto const root = create_empty_root("core-atomic-file");

  auto const target = root / "config" / "settings.json";
  auto first = ava::core::write_text_file_atomic(target, "{\"ok\":true}\n", "test config");
  expect(first.has_value(),
         first ? "atomic text writer creates parent directories" : "atomic text writer creates parent directories: " + first.error().format());
  expect(read_test_file(target) == "{\"ok\":true}\n", "atomic text writer stores full file content");

  struct stat target_stat{};
  expect(::stat(target.c_str(), &target_stat) == 0 && (target_stat.st_mode & (S_IRWXG | S_IRWXO)) == 0,
         "atomic text writer uses owner-only target permissions");

  auto replaced = ava::core::write_text_file_atomic(target, "{\"ok\":false}\n", "test config");
  expect(replaced.has_value(),
         replaced ? "atomic text writer replaces regular files" : "atomic text writer replaces regular files: " + replaced.error().format());
  expect(read_test_file(target) == "{\"ok\":false}\n", "atomic text writer replacement updates content");

  auto const outside = root / "outside.json";
  write_test_file(outside, "outside\n");
  auto const link = root / "config" / "linked.json";
  std::error_code symlink_error;
  std::filesystem::create_symlink(outside, link, symlink_error);
  if (!symlink_error)
  {
    auto rejected = ava::core::write_text_file_atomic(link, "changed\n", "test config");
    expect(!rejected && rejected.error().category() == ava::core::ErrorCategory::PermissionDenied, "atomic text writer rejects symlink replacement targets");
    expect(read_test_file(outside) == "outside\n", "atomic text writer does not modify symlink referents");
  }
}

void test_permission_defaults()
{
  auto const workspace = std::filesystem::current_path();

  auto const normal_edit = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::EditFile,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = workspace / "src/main.cpp",
      .command = "",
  });
  expect(normal_edit.action == ava::permissions::PermissionAction::Allow && normal_edit.risk == ava::permissions::PermissionRisk::Medium,
         "build mode allows workspace edits with medium mutation risk");

  auto const plan_source_edit = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::EditFile,
      .mode = ava::agent::Mode::Plan,
      .workspace_dir = workspace,
      .target_path = workspace / "src/main.cpp",
      .command = "",
  });
  expect(plan_source_edit.action == ava::permissions::PermissionAction::Deny && plan_source_edit.risk == ava::permissions::PermissionRisk::High,
         "plan mode denies source edits with high semantic risk");

  auto const plan_doc_edit = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::EditFile,
      .mode = ava::agent::Mode::Plan,
      .workspace_dir = workspace,
      .target_path = workspace / "docs/versions/0.1.md",
      .command = "",
  });
  expect(plan_doc_edit.action == ava::permissions::PermissionAction::Allow, "plan mode allows planning markdown");

  auto const secret_read = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::ReadFile,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = workspace / ".env",
      .command = "",
  });
  expect(secret_read.action == ava::permissions::PermissionAction::Deny && secret_read.risk == ava::permissions::PermissionRisk::Critical,
         "secret files are denied with critical semantic risk");

  auto const npmrc_read = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::ReadFile,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = workspace / ".npmrc",
      .command = "",
  });
  expect(npmrc_read.action == ava::permissions::PermissionAction::Deny, "common credential files are denied");

  auto const ssh_read = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::ReadFile,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = workspace / ".ssh/work_key",
      .command = "",
  });
  expect(ssh_read.action == ava::permissions::PermissionAction::Deny, "credential directories are denied");

  auto const external_read = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::ReadFile,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = workspace.parent_path() / "outside.txt",
      .command = "",
  });
  expect(external_read.action == ava::permissions::PermissionAction::Ask && external_read.risk == ava::permissions::PermissionRisk::High,
         "external paths ask with high semantic risk");

  auto const workspace_lsp = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::LspQuery,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = workspace / "src/main.cpp",
      .command = "",
  });
  expect(workspace_lsp.action == ava::permissions::PermissionAction::Allow, "workspace LSP diagnostics are allowed");
  auto const task_launch = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::TaskRun,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = workspace,
      .command = "general",
  });
  expect(task_launch.action == ava::permissions::PermissionAction::Allow && task_launch.risk == ava::permissions::PermissionRisk::Medium,
         "task launch is automatically allowed with an auditable medium-risk policy decision");

  expect(ava::permissions::to_string(ava::permissions::Operation::LspQuery) == "lsp.query", "LSP query operation string is stable");
  expect(ava::permissions::to_string(ava::permissions::Operation::NetworkSearch) == "network.search", "network search operation string is stable");
  expect(ava::permissions::to_string(ava::permissions::Operation::SkillLoad) == "skill", "skill operation string is stable");
  expect(ava::permissions::to_string(ava::permissions::PermissionRisk::Critical) == "critical", "permission risk string is stable");

  auto const secret_lsp = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::LspQuery,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = workspace / ".env",
      .command = "",
  });
  expect(secret_lsp.action == ava::permissions::PermissionAction::Deny, "LSP diagnostics deny secret paths");

  auto const external_lsp = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = ava::permissions::Operation::LspQuery,
      .mode = ava::agent::Mode::Build,
      .workspace_dir = workspace,
      .target_path = workspace.parent_path() / "outside.cpp",
      .command = "",
  });
  expect(external_lsp.action == ava::permissions::PermissionAction::Ask, "external LSP diagnostic paths ask");

  auto const root = create_empty_root("test_permission_defaults");
  auto const symlink_workspace = root / "symlink-workspace";
  auto const outside = root / "outside";
  std::filesystem::create_directories(symlink_workspace);
  std::filesystem::create_directories(outside);
  std::error_code symlink_error;
  auto const link = symlink_workspace / "link-outside";
  std::filesystem::remove(link, symlink_error);
  std::filesystem::create_directory_symlink(outside, link, symlink_error);
  if (!symlink_error)
  {
    auto const symlink_escape = ava::permissions::decide(ava::permissions::PermissionRequest{
        .operation = ava::permissions::Operation::ReadFile,
        .mode = ava::agent::Mode::Build,
        .workspace_dir = symlink_workspace,
        .target_path = link / "outside.txt",
        .command = "",
    });
    expect(symlink_escape.action == ava::permissions::PermissionAction::Ask, "symlink workspace escape asks");
  }
}

}  // namespace

void run_core_mode_tests()
{
  test_mode_parsing();

  test_application_lifecycle_contracts();
}

void run_core_json_permission_tests()
{
  test_json_escape_control_characters();
  test_core_json_nesting_limit();
  test_core_json_top_level_lookup();
  test_process_arg_workspace_relative_detection();
  test_atomic_text_file_write();
  test_permission_defaults();
}
