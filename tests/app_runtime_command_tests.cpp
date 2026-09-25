#include "sys.h"
#include "tests/app_runtime_test_declarations.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/test_harness.h"
#include "ava/app/command_catalog.h"
#include "ava/app/commands.h"
#include "ava/app/interactive_internal.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/OpenContext.h"
#include "ava/agent/mode.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_runtime_tests {

using namespace ava::tests;

void test_tui_request_presentation_capture()
{
  using ava::app::interactive_internal::capture_tui_request_presentation;
  using ava::app::interactive_internal::InteractiveResult;
  using ava::app::interactive_internal::TuiRequestPresentation;

  TuiRequestPresentation compact;
  capture_tui_request_presentation(compact, true, "/compact", "request-compact",
                                   InteractiveResult{.output = {"compaction summary recorded"}, .tool_timeline = {{.name = "compact-local"}}});
  capture_tui_request_presentation(
      compact, true, "queued provider question", "request-failed",
      InteractiveResult{.output = {"Moonshot HTTP request failed with status 400"}, .tool_timeline = {{.name = "failed-follow-up-tool"}}});
  capture_tui_request_presentation(compact, true, "queued canceled question", "request-canceled",
                                   InteractiveResult{.output = {"agent turn canceled"}, .tool_timeline = {}});
  capture_tui_request_presentation(
      compact, true, "queued successful question", "request-success",
      InteractiveResult{.ordinary_turn_committed = true, .output = {"queued provider answer"}, .tool_timeline = {{.name = "successful-follow-up-tool"}}});
  expect(
      compact.has_local_command &&
          compact.local_command.output ==
              std::vector<std::string>{"compaction summary recorded", "Moonshot HTTP request failed with status 400", "agent turn canceled"} &&
          compact.local_command.tool_timeline.size() == 2 && compact.local_command.tool_timeline.front().name == "compact-local" &&
          compact.local_command.tool_timeline.back().name == "failed-follow-up-tool" &&
          compact.ordinary_turn_request_ids == std::vector<std::string>{"request-success"} &&
          compact.conversation.output == std::vector<std::string>{"queued provider answer"} && compact.conversation.tool_timeline.size() == 1 &&
          compact.conversation.tool_timeline.front().name == "successful-follow-up-tool",
      "TUI request capture keeps every uncommitted queued result with an initial local command while granting conversation authority only to committed turns");

  TuiRequestPresentation normal;
  capture_tui_request_presentation(normal, false, "ordinary initial prompt", "request-normal-failed",
                                   InteractiveResult{.output = {"ordinary initial failure"}, .tool_timeline = {{.name = "ordinary-failed-tool"}}});
  expect(!normal.has_local_command && normal.local_command.output.empty() && normal.local_command.tool_timeline.empty() &&
             normal.ordinary_turn_request_ids.empty(),
         "TUI request capture does not globally classify a failed normal initial prompt as local");
}

void test_app_command_dispatcher()
{
  auto const root = create_empty_root("app-command-dispatcher");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace / "src");
  std::filesystem::create_directories(paths.ava_config_dir);
  expect(::chmod(temp_root().c_str(), S_IRWXU) == 0 && ::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "command dispatcher workspace is owner-only for sealed command planning");
  write_app_test_file(paths.models_file,
                      "{\n"
                      "  \"models\": [\n"
                      "    {\"provider\":\"openai\",\"id\":\"diagnostic-local\",\"name\":\"Diagnostic Local\","
                      "\"api_family\":\"anthropic_messages\",\"supports_reasoning\":true},\n"
                      "    {\"provider\":\"ghost\",\"id\":\"remote-model\",\"name\":\"Remote Missing\",\"family\":\"remote\","
                      "\"context_window_tokens\":4096,\"api_family\":\"chat_completions\",\"input_modalities\":[\"text\"],"
                      "\"supports_tools\":false,\"supports_streaming\":false,\"supports_reasoning\":false,\"reports_usage\":false}\n"
                      "  ]\n"
                      "}\n");
  {
    std::ofstream file(workspace / "AGENTS.md", std::ios::binary | std::ios::trunc);
    file << "dispatcher context\n";
  }
  {
    std::filesystem::create_directories(paths.prompts_dir / "openai" / "gpt-5");
    std::ofstream file(paths.prompts_dir / "openai" / "gpt-5" / "plan.txt", std::ios::binary | std::ios::trunc);
    file << "dispatcher plan prompt\n";
  }
  write_app_test_file(workspace / ".ava" / "skills" / "dispatcher-skill" / "SKILL.md",
                      "---\n"
                      "name: dispatcher-skill\n"
                      "description: Dispatcher skill\n"
                      "---\n"
                      "Use the dispatcher skill.\n");
  write_app_test_file(workspace / ".ava" / "commands" / "prompt-check.md",
                      "---\n"
                      "description: Check prompt command freshness\n"
                      "argument-hint: \"[topic]\"\n"
                      "---\n"
                      "Check prompt command freshness for $ARGUMENTS.\n");
  {
    std::ofstream file(workspace / "src" / "main.cpp", std::ios::binary | std::ios::trunc);
    file << "int main() { return 0; }\n";
  }
  {
    std::filesystem::create_directories(workspace / "my folder");
    std::ofstream file(workspace / "my folder" / "space file.txt", std::ios::binary | std::ios::trunc);
    file << "space path\n";
  }
  {
    std::filesystem::create_directories(workspace / "docs" / "reference-code" / "pi");
    std::ofstream file(workspace / "docs" / "reference-code" / "pi" / "reference-only.md", std::ios::binary | std::ios::trunc);
    file << "reference code stays out of normal path completion\n";
  }
  write_app_test_file(paths.ava_config_dir / "plugins" / "com.example.global" / "plugin.json",
                      app_test_plugin_manifest_json("com.example.global", "Global Plugin"));
  write_app_test_file(
      workspace / ".ava" / "plugins" / "com.example.project" / "plugin.json",
      "{\n"
      "  \"schema_version\": 1,\n"
      "  \"id\": \"com.example.project\",\n"
      "  \"name\": \"Project Plugin\",\n"
      "  \"version\": \"0.1.0\",\n"
      "  \"api_version\": \"ava.plugin.v1\",\n"
      "  \"description\": \"test plugin\",\n"
      "  \"entrypoint\": {\"command\": \"node\", \"args\": [\"plugin.js\", \"--safe\"]},\n"
      "  \"capabilities\": [\"tools\", \"commands\"],\n"
      "  \"contributes\": {\n"
      "    \"tools\": [{\"name\": \"todo_add\", \"description\": \"Add todo\", \"input_schema\": {\"type\": \"object\", \"additionalProperties\": false}}],\n"
      "    \"commands\": [{\"name\": \"todo\", \"description\": \"Show todos\"}],\n"
      "    \"prompts\": [{\"name\": \"review\", \"description\": \"Review prompt\", \"path\": \"prompts/review.md\"}],\n"
      "    \"skills\": [{\"name\": \"triage\", \"description\": \"Triage skill\", \"path\": \"skills/triage.md\"}]\n"
      "  }\n"
      "}");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.project" / "prompts" / "review.md", "Review todos from the project plugin.\n");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.project" / "skills" / "triage.md", "Triage todos from the project plugin.\n");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.bad" / "plugin.json", "{not-json");
  write_app_test_file(workspace / ".ava" / "mcp.json", app_test_mcp_config_json("fs", "Filesystem Server", AVA_FAKE_MCP_SERVER_PATH));
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(),
         trusted ? "command dispatcher test trusts project resources" : "command dispatcher test trusts project resources: " + trusted.error().format());

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Plan;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "command dispatcher test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
  CRITICAL_AREA_BEGIN_W(session);
  auto const plan_system_prompt = session_w->system_prompt();
  {
    std::ofstream file(workspace / "AGENTS.md", std::ios::binary | std::ios::trunc);
    file << "dispatcher context changed after session open\n";
  }
  write_app_test_file(workspace / ".ava" / "skills" / "dispatcher-skill" / "SKILL.md",
                      "---\n"
                      "name: dispatcher-skill\n"
                      "description: Dispatcher skill\n"
                      "---\n"
                      "Use the changed dispatcher skill.\n");
  write_app_test_file(workspace / ".ava" / "commands" / "prompt-check.md",
                      "---\n"
                      "description: Check changed prompt command freshness\n"
                      "argument-hint: \"[topic]\"\n"
                      "---\n"
                      "Check changed prompt command freshness for $ARGUMENTS.\n");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.project" / "prompts" / "review.md", "Review todos from the changed project plugin.\n");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.project" / "skills" / "triage.md", "Triage todos from the changed project plugin.\n");

  expect(ava::app::is_backend_command("/model") && ava::app::is_backend_command("/models") && ava::app::is_backend_command("/providers") &&
             ava::app::is_backend_command("/hotkeys") && ava::app::is_backend_command("/keybindings") && ava::app::is_backend_command("/theme") &&
             ava::app::is_backend_command("/images") && ava::app::is_backend_command("/image-width") && ava::app::is_backend_command("/cursor") &&
             ava::app::is_backend_command("/details") && ava::app::is_backend_command("/sidebar") && ava::app::is_backend_command("/overview") &&
             ava::app::is_backend_command("/tool") && ava::app::is_backend_command("/tools write") && ava::app::is_backend_command("/diff") &&
             ava::app::is_backend_command("/copy") && ava::app::is_backend_command("/find src/*.cpp") && ava::app::is_backend_command("/ls src") &&
             ava::app::is_backend_command("/thinking") && ava::app::is_backend_command("/status") && ava::app::is_backend_command("/reload") &&
             ava::app::is_backend_command("/plugins") && ava::app::is_backend_command("/packages") && ava::app::is_backend_command("/permissions") &&
             ava::app::is_backend_command("/permission-rules") && ava::app::is_backend_command("!pwd") && ava::app::is_backend_command("!!pwd"),
         "command catalog classifies display toggles, status aliases, disabled aliases, hotkeys, and shell helpers as backend commands");

  std::vector<ava::app::CommandHotkey> const custom_hotkeys = {
      ava::app::CommandHotkey{.action = "submit", .description = "Submit custom", .keys = "Ctrl+M"},
      ava::app::CommandHotkey{.action = "variant_cycle", .description = "Cycle variants", .keys = "Ctrl+T"}};

  CRITICAL_AREA_END_W(session);
  app_command_dispatcher_ui_part(unlocked_session, paths, workspace, custom_hotkeys);
  app_command_dispatcher_catalog_part(unlocked_session, paths, workspace, custom_hotkeys);
  app_command_dispatcher_auth_part(unlocked_session, plan_system_prompt);
  app_command_dispatcher_tool_part(unlocked_session, workspace);
  app_command_dispatcher_session_part(unlocked_session, workspace);
}

}  // namespace ava::tests::app_runtime_tests
