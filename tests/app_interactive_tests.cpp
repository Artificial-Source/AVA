#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/test_harness.h"
#include "ava/process/supervisor.h"
#include "ava/app/commands.h"
#include "ava/app/interactive_internal.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/OpenContext.h"
#include "ava/app/runtime/Session.h"
#include "ava/permissions/permission.h"
#include "ava/core/json.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace {

using namespace ava::tests;

ava::app::runtime::session_ts open_test_session(std::filesystem::path const& root, std::filesystem::path const& workspace)
{
  auto paths = app_test_paths(root);
  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  auto application_scope = ava::process::ProcessScopeV1::application(std::make_shared<ava::process::Supervisor>());
  expect(application_scope.has_value(), "interactive command fixture owns application process authority");
  if (application_scope)
    options.application_process_scope = std::move(*application_scope);
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(), "interactive command test opens runtime session");
  return std::move(*unlocked_session_result);
}

ava::permissions::PermissionResolver allow_all_permissions()
{
  return [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
}

ava::permissions::PermissionResolver deny_all_permissions()
{
  return [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Deny;
  };
}

std::size_t count_exact_lines(std::string const& text, std::string_view needle)
{
  std::size_t count = 0;
  std::size_t pos = 0;
  while (pos < text.size())
  {
    auto const end = text.find('\n', pos);
    auto const line = std::string_view(text).substr(pos, (end == std::string::npos ? text.size() : end) - pos);
    if (line == needle)
      ++count;
    pos = end == std::string::npos ? text.size() : end + 1;
  }
  return count;
}

std::string mcp_config_with_event_log(std::string_view command, std::filesystem::path const& event_log)
{
  return std::string("{\"servers\":[{\"id\":\"demo\",\"name\":\"Demo MCP\",\"command\":\"") + ava::core::json::escape(command) +
         "\",\"env\":{\"AVA_FAKE_MCP_EVENT_LOG\":\"" + ava::core::json::escape(event_log.string()) + "\"},\"enabled\":true}]}";
}

std::string joined_output(ava::app::interactive_internal::InteractiveResult const& result)
{
  std::string text;
  for (auto const& line : result.output)
  {
    if (!text.empty())
      text += '\n';
    text += line;
  }
  return text;
}

void test_display_settings_command_classification_includes_cursor()
{
  using ava::app::interactive_internal::is_display_settings_command;
  expect(is_display_settings_command("/cursor default") && is_display_settings_command("  /cursor underline steady\t") &&
             !is_display_settings_command("/cursor-shaped") && !is_display_settings_command("/cursorbar"),
         "cursor commands trigger TUI display hydration without broad prefix matches");
}

void test_interactive_unknown_and_builtin_slash_behavior()
{
  auto const root = create_empty_root("interactive-slash-classification");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const private_home = root / "home";
  std::filesystem::create_directories(private_home);
  ScopedEnvVar home("HOME", private_home.string());
  auto unlocked_session = open_test_session(root, workspace);
  ava::app::interactive_internal::InteractiveState state{.unlocked_session = unlocked_session};

  auto help = ava::app::interactive_internal::handle_interactive_submission(state, "/help");
  expect(!help.ordinary_turn_committed && !help.output.empty() && help.output[0].find("/help") != std::string::npos,
         "interactive builtin slash commands still dispatch locally");

  auto unknown = ava::app::interactive_internal::handle_interactive_submission(state, "/definitely-unknown-slash");
  expect(
      !unknown.ordinary_turn_committed && !unknown.output.empty() && unknown.output[0].find("Unknown command: /definitely-unknown-slash") != std::string::npos,
      "unknown slash commands are rejected and never sent to the model");

  auto missing_skill = ava::app::interactive_internal::handle_interactive_submission(state, "/skill:missing");
  expect(!missing_skill.ordinary_turn_committed && !missing_skill.output.empty() &&
             missing_skill.output[0].find("command not found: /skill:missing") != std::string::npos,
         "missing namespaced skills stay local command-not-found: " + joined_output(missing_skill));

  auto missing_plugin = ava::app::interactive_internal::handle_interactive_submission(state, "/plugin:missing:cmd");
  expect(!missing_plugin.ordinary_turn_committed && !missing_plugin.output.empty() &&
             missing_plugin.output[0].find("command not found: /plugin:missing:cmd") != std::string::npos,
         "missing namespaced plugins stay local command-not-found: " + joined_output(missing_plugin));
}

void test_interactive_mcp_prompt_submission_resolves_once()
{
  if (std::string_view(AVA_FAKE_MCP_SERVER_PATH).empty())
    return;

  auto const root = create_empty_root("interactive-mcp-resolve");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  auto const event_log = root / "mcp-events.log";
  auto const private_home = root / "home";
  std::filesystem::create_directories(private_home);
  ScopedEnvVar home("HOME", private_home.string());
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(), trusted ? "interactive MCP test trusts project" : "interactive MCP test trusts project: " + trusted.error().format());
  write_app_test_file(workspace / ".ava" / "mcp.json", mcp_config_with_event_log(AVA_FAKE_MCP_SERVER_PATH, event_log));
  write_app_test_file(workspace / ".ava" / "skills" / "release" / "SKILL.md",
                      "---\nname: release\ndescription: Prepare release work\n---\nRelease skill body\n");

  auto unlocked_session = open_test_session(root, workspace);
  ava::app::interactive_internal::InteractiveState state{.unlocked_session = unlocked_session};
  auto ask = allow_all_permissions();

  expect(!ava::app::is_backend_command_1("/release-notes", unlocked_session), "interactive passive lookup does not start MCP for short aliases");
  expect(!std::filesystem::exists(event_log) || app_read_binary_file(event_log).empty(), "interactive passive lookup starts zero MCP processes");

  auto alias = ava::app::interactive_internal::handle_interactive_submission(state, "/release-notes AVA", ask);
  auto alias_text = joined_output(alias);
  expect(!alias.ordinary_turn_committed && alias_text.find("Unknown command") == std::string::npos,
         "interactive MCP short-alias submission is not treated as unknown");
  auto log_after_alias = app_read_binary_file(event_log);
  expect(count_exact_lines(log_after_alias, "prompts/list") == 1 && count_exact_lines(log_after_alias, "prompts/get") == 1,
         "one interactive short-alias submission performs exactly one prompts/list and one prompts/get");

  auto namespaced = ava::app::interactive_internal::handle_interactive_submission(state, "/mcp:demo:release-notes AVA", ask);
  auto namespaced_text = joined_output(namespaced);
  expect(namespaced_text.find("Unknown command") == std::string::npos && namespaced_text.find("command not found") == std::string::npos,
         "interactive namespaced MCP prompt submission dispatches the resolved entry");
  auto log_after_namespaced = app_read_binary_file(event_log);
  expect(count_exact_lines(log_after_namespaced, "prompts/list") == 2 && count_exact_lines(log_after_namespaced, "prompts/get") == 2,
         "namespaced interactive submission performs one additional list and get");

  auto denied = ava::app::interactive_internal::handle_interactive_submission(state, "/release-notes", deny_all_permissions());
  auto denied_text = joined_output(denied);
  expect(
      !denied.ordinary_turn_committed && denied_text.find("Unknown command") == std::string::npos && denied_text.find("permission_denied") != std::string::npos,
      "interactive denied MCP listing reports the permission diagnostic");

  auto canceled = ava::app::interactive_internal::handle_interactive_submission(state, "/release-notes", ask, nullptr, {}, nullptr, [] { return true; });
  auto canceled_text = joined_output(canceled);
  expect(!canceled.ordinary_turn_committed && canceled_text.find("Unknown command") == std::string::npos && canceled_text.find("canceled") != std::string::npos,
         "interactive cancellation during listing is surfaced rather than unknown");
}

}  // namespace

void run_app_interactive_tests()
{
  test_display_settings_command_classification_includes_cursor();
  test_interactive_unknown_and_builtin_slash_behavior();
  test_interactive_mcp_prompt_submission_resolves_once();
}
