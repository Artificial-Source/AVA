#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/runtime_event_test_support.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/http/transport.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/app/command_catalog.h"
#include "ava/app/command_palette.h"
#include "ava/app/commands.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/OpenContext.h"
#include "ava/app/runtime/RunOptions.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/tui/command_palette_views.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/tool_dispatcher.h"
#include "ava/agent/tool_types.h"
#include "ava/tools/file_tools.h"
#include "ava/plugin/enablement.h"
#include "ava/permissions/permission.h"
#include "ava/provider/openai_provider.h"
#include "ava/context/context_loader.h"
#include "ava/core/json.h"
#include "ava/core/result.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_runtime_tests {

using namespace ava::tests;
using namespace std::chrono_literals;

bool wait_for_app_plugin_path(std::filesystem::path const& path, std::chrono::milliseconds timeout = 2s)
{
  auto const deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (std::filesystem::exists(path))
      return true;
    std::this_thread::sleep_for(10ms);
  }
  return std::filesystem::exists(path);
}

std::string shell_single_quoted(std::string_view value)
{
  std::string quoted = "'";
  for (char const ch : value)
  {
    if (ch == '\'')
      quoted += "'\\''";
    else
      quoted.push_back(ch);
  }
  quoted += "'";
  return quoted;
}

std::string app_plugin_resource_manifest_json(std::string_view id, std::string_view name, std::filesystem::path const& marker_path,
                                              std::string_view prompt_name, std::string_view prompt_description, std::string_view prompt_path,
                                              std::string_view skill_name, std::string_view skill_description, std::string_view skill_path)
{
  return std::string("{\n") +
         "  \"schema_version\": 1,\n"
         "  \"id\": \"" +
         ava::core::json::escape(id) +
         "\",\n"
         "  \"name\": \"" +
         ava::core::json::escape(name) +
         "\",\n"
         "  \"version\": \"0.1.0\",\n"
         "  \"api_version\": \"ava.plugin.v1\",\n"
         "  \"description\": \"static resource autoload fixture\",\n"
         "  \"entrypoint\": {\"command\": \"/bin/sh\", \"args\": [\"-c\", \"printf executed > " +
         ava::core::json::escape(shell_single_quoted(marker_path.string())) +
         "\"]},\n"
         "  \"capabilities\": [],\n"
         "  \"contributes\": {\n"
         "    \"prompts\": [{\"name\": \"" +
         ava::core::json::escape(prompt_name) + "\", \"description\": \"" + ava::core::json::escape(prompt_description) + "\", \"path\": \"" +
         ava::core::json::escape(prompt_path) +
         "\"}],\n"
         "    \"skills\": [{\"name\": \"" +
         ava::core::json::escape(skill_name) + "\", \"description\": \"" + ava::core::json::escape(skill_description) + "\", \"path\": \"" +
         ava::core::json::escape(skill_path) +
         "\"}]\n"
         "  }\n"
         "}";
}

std::string app_plugin_event_cancel_manifest_json()
{
  return R"({
  "schema_version": 1,
  "id": "com.example.run-cancel-event",
  "name": "Run cancellation event fixture",
  "version": "0.1.0",
  "api_version": "ava.plugin.v1",
  "description": "runtime run cancellation fixture",
  "entrypoint": {"command": "/bin/sh", "args": ["hook.sh"]},
  "capabilities": ["event_hooks"],
  "contributes": {"event_hooks": [{"event": "session.start"}]}
})";
}

std::string app_plugin_install_manifest_json(std::string_view id, std::string_view name, std::filesystem::path const& marker_path)
{
  return std::string("{\n") +
         "  \"schema_version\": 1,\n"
         "  \"id\": \"" +
         ava::core::json::escape(id) +
         "\",\n"
         "  \"name\": \"" +
         ava::core::json::escape(name) +
         "\",\n"
         "  \"version\": \"0.1.0\",\n"
         "  \"api_version\": \"ava.plugin.v1\",\n"
         "  \"description\": \"plugin install fixture\",\n"
         "  \"entrypoint\": {\"command\": \"/bin/sh\", \"args\": [\"-c\", \"printf executed > " +
         ava::core::json::escape(shell_single_quoted(marker_path.string())) +
         "\"]},\n"
         "  \"capabilities\": [\"tools\", \"commands\"],\n"
         "  \"contributes\": {\n"
         "    \"tools\": [{\"name\": \"todo_add\", \"description\": \"Add todo\", \"input_schema\": {\"type\": \"object\", "
         "\"additionalProperties\": false}}],\n"
         "    \"commands\": [{\"name\": \"status\", \"description\": \"Report install fixture status\"}],\n"
         "    \"prompts\": [{\"name\": \"install-review\", \"description\": \"Review installed plugin\", \"path\": \"prompts/review.md\"}],\n"
         "    \"skills\": [{\"name\": \"install-triage\", \"description\": \"Triage installed plugin\", \"path\": \"skills/triage.md\"}]\n"
         "  }\n"
         "}";
}

void test_app_runtime_project_trust_malformed_diagnostics()
{
  auto const root = create_empty_root("app-runtime-project-trust-malformed");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  write_app_test_file(ava::app::project_trust_file(paths), "{\"schema_version\":1,\"decisions\":[\n");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime opens with malformed project trust file fail-closed");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  auto project_trust = ava::app::runtime::session_ts::rat(unlocked_session)->project_trust();
  expect(project_trust.decision == ava::app::ProjectTrustDecision::Unknown && !ava::app::project_resources_trusted(project_trust),
         "malformed project trust file leaves project resources skipped");
  expect(project_trust.diagnostic.find("malformed project trust file") != std::string::npos &&
             project_trust.diagnostic.find(ava::app::project_trust_file(paths).string()) != std::string::npos,
         "runtime records a path-specific project trust parse diagnostic");

  auto status = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/trust status"});
  expect(status && status->handled && !status->output.empty() && status->output[0].find("decision=unknown") != std::string::npos &&
             status->output[0].find("project_resources=skipped") != std::string::npos &&
             status->output[0].find("diagnostic=invalid_argument: malformed project trust file") != std::string::npos,
         "/trust status surfaces malformed trust-file diagnostics");

  auto trusted = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/trust project"});
  project_trust = ava::app::runtime::session_ts::rat(unlocked_session)->project_trust();
  expect(trusted && trusted->handled && !trusted->output.empty() && trusted->output[0].find("project_resources=enabled") != std::string::npos &&
             trusted->output[0].find("diagnostic=") == std::string::npos && project_trust.decision == ava::app::ProjectTrustDecision::Trusted,
         "/trust project repairs a malformed trust file without retaining stale diagnostics");

  write_app_test_file(ava::app::project_trust_file(paths), "{\"decisions\":\"not an array\"}\n");
  auto reloaded = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/reload trust"});
  project_trust = ava::app::runtime::session_ts::rat(unlocked_session)->project_trust();
  expect(reloaded && reloaded->handled && !reloaded->output.empty() && reloaded->output[0].find("trust: loaded") != std::string::npos &&
             reloaded->output[0].find("decision: unknown") != std::string::npos &&
             reloaded->output[0].find("project_resources: skipped") != std::string::npos &&
             reloaded->output[0].find("diagnostic: invalid_argument: malformed project trust file") != std::string::npos &&
             project_trust.decision == ava::app::ProjectTrustDecision::Unknown && !ava::app::project_resources_trusted(project_trust),
         "/reload trust surfaces malformed trust diagnostics and keeps project resources fail-closed");
}

void test_app_runtime_enabled_plugin_resources_autoload()
{
  auto const root = create_empty_root("app-runtime-plugin-resource-autoload");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  auto const marker = root / "plugin-entrypoint-executed";
  std::filesystem::create_directories(workspace);

  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(), trusted ? "plugin resource autoload test trusts project resources"
                                      : "plugin resource autoload test trusts project resources: " + trusted.error().format());

  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.autoload" / "plugin.json",
                      app_plugin_resource_manifest_json("com.example.autoload", "Autoload Plugin", marker, "review", "Enabled review prompt",
                                                        "prompts/review.md", "plugin-triage", "Enabled plugin triage skill", "skills/plugin-triage.md"));
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.autoload" / "prompts" / "review.md", "Enabled plugin prompt autoload marker.\n");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.autoload" / "skills" / "plugin-triage.md", "Enabled plugin skill body autoload marker.\n");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.disabled" / "plugin.json",
                      app_plugin_resource_manifest_json("com.example.disabled", "Disabled Plugin", marker, "disabled-review", "Disabled review prompt",
                                                        "prompts/review.md", "disabled-triage", "Disabled plugin triage skill", "skills/disabled-triage.md"));
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.disabled" / "prompts" / "review.md", "Disabled plugin prompt must not load.\n");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.disabled" / "skills" / "disabled-triage.md", "Disabled plugin skill must not load.\n");

  auto enabled = ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, "com.example.autoload", true,
                                                 ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), enabled ? "plugin resource autoload test enables project plugin"
                                      : "plugin resource autoload test enables project plugin: " + enabled.error().format());

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime opens with enabled plugin static resources");
  if (!unlocked_session_result)
    return;

  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);
  CRITICAL_AREA_BEGIN_R(session);

  auto has_context_source = [&](std::string_view needle, ava::context::ContextSourceType source_type) {
    return std::ranges::any_of(session_r->context_sources(),
                               [&](auto const& source) { return source.source_type == source_type && source.path.string().find(needle) != std::string::npos; });
  };
  auto has_freshness_source = [&](ava::app::runtime::FreshnessSourceKind kind, std::string_view source_id, std::string_view name) {
    return std::ranges::any_of(session_r->freshness_sources(),
                               [&](auto const& source) { return source.kind == kind && source.source_id == source_id && source.name == name; });
  };

  expect(session_r->system_prompt().find("Enabled plugin prompt autoload marker") != std::string::npos &&
             session_r->system_prompt().find("<name>plugin-triage</name>") != std::string::npos &&
             session_r->system_prompt().find("Enabled plugin triage skill") != std::string::npos &&
             session_r->system_prompt().find("<scope>plugin</scope>") != std::string::npos,
         "enabled plugin prompt and skill resources appear in the runtime system prompt");
  expect(session_r->system_prompt().find("Disabled plugin prompt must not load") == std::string::npos &&
             session_r->system_prompt().find("disabled-triage") == std::string::npos &&
             session_r->system_prompt().find("Disabled plugin triage skill") == std::string::npos,
         "disabled plugin prompt and skill resources are not added to runtime context");
  expect(has_context_source("com.example.autoload/prompts/review.md", ava::context::ContextSourceType::Plugin) &&
             !has_context_source("com.example.disabled/prompts/review.md", ava::context::ContextSourceType::Plugin),
         "enabled plugin prompt resource is tracked as a plugin context source while disabled resources are skipped");
  expect(has_freshness_source(ava::app::runtime::FreshnessSourceKind::PluginPrompt, "com.example.autoload", "review") &&
             has_freshness_source(ava::app::runtime::FreshnessSourceKind::PluginSkill, "com.example.autoload", "plugin-triage") &&
             !has_freshness_source(ava::app::runtime::FreshnessSourceKind::PluginPrompt, "com.example.disabled", "disabled-review") &&
             !has_freshness_source(ava::app::runtime::FreshnessSourceKind::PluginSkill, "com.example.disabled", "disabled-triage"),
         "runtime freshness records enabled plugin resources without loading disabled resources");

  CRITICAL_AREA_END_R(session);

  std::vector<ava::permissions::Operation> operations;
  auto allow = [&operations](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    operations.push_back(prompt.operation);
    return ava::permissions::PermissionResolution::Allow;
  };
  auto skill_command = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/skill:plugin-triage", .permission_resolver = allow});
  expect(skill_command && skill_command->handled && skill_command->prompt_message &&
             skill_command->prompt_message->find("<skill_content name=\"plugin-triage\">") != std::string::npos &&
             skill_command->prompt_message->find("Enabled plugin skill body autoload marker") != std::string::npos,
         "enabled plugin static skill is loadable through slash skill commands");
  ava::tools::ToolContext tool_context;
  tool_context.workspace_dir = workspace;
  tool_context.plugin_global_plugins_dir = paths.ava_config_dir / "plugins";
  tool_context.plugin_project_plugins_dir = workspace / ".ava" / "plugins";
  tool_context.plugin_enablement_file = paths.ava_state_dir / "plugin-enablement.json";
  tool_context.include_project_plugins = true;
  tool_context.include_project_skills = true;
  tool_context.permission_resolver = allow;
  ava::agent::ToolDispatcher dispatcher(tool_context);
  auto tool_skill =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_plugin_skill", .name = "skill", .arguments_json = "{\"name\":\"plugin-triage\"}"});
  expect(tool_skill && tool_skill->success && tool_skill->result_text.find("Enabled plugin skill body autoload marker") != std::string::npos,
         "enabled plugin static skill is loadable through the provider skill tool");

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
                                           .status_code = 200,
                                           .headers = {},
                                           .body = "data: {\"type\":\"response.function_call.added\",\"call_id\":"
                                                   "\"call_plugin_skill_runtime\",\"name\":\"skill\"}\n\n"
                                                   "data: {\"type\":\"response.function_call_arguments.delta\","
                                                   "\"call_id\":\"call_plugin_skill_runtime\",\"delta\":\"{\\\"name\\\":"
                                                   "\\\"plugin-triage\\\"}\"}\n\n"
                                                   "data: [DONE]\n\n",
                                       },
                                       ava::http::HttpResponse{
                                           .status_code = 200,
                                           .headers = {},
                                           .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":"
                                                   "\"plugin skill done\"}\n\n"
                                                   "data: [DONE]\n\n",
                                       }});
  std::vector<ava::event::RuntimeEvent> events;
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  run_options.permission_resolver = allow;
  run_options.event_sink = [&events](ava::event::RuntimeEvent const& event) {
    events.push_back(event);
    return ava::core::VoidResult{};
  };
  auto run = ava::app::run_prompt(unlocked_session, "load the plugin skill", provider, transport, run_options);
  expect(run && run->final_text == "plugin skill done" &&
             std::ranges::any_of(events,
                                 [](ava::event::RuntimeEvent const& event) {
                                   auto const* result = ava::tests::runtime_event_as<ava::event::ToolResultEvent>(event);
                                   return result && result->payload.tool == "skill" &&
                                          result->payload.result_json.find("Enabled plugin skill body autoload marker") != std::string::npos;
                                 }),
         "runtime agent loop loads enabled plugin static skills from the session plugin paths");
  expect(std::ranges::find(operations, ava::permissions::Operation::SkillLoad) != operations.end(), "plugin static skill loading remains permission-gated");
  expect(!std::filesystem::exists(marker), "static plugin resource autoload does not execute plugin entrypoints");
}

void test_app_runtime_project_plugin_resources_follow_trust_gate()
{
  auto const root = create_empty_root("app-runtime-plugin-resource-trust-gate");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  auto const global_marker = root / "global-plugin-entrypoint-executed";
  auto const project_marker = root / "project-plugin-entrypoint-executed";
  std::filesystem::create_directories(workspace);

  write_app_test_file(
      paths.ava_config_dir / "plugins" / "com.example.globalautoload" / "plugin.json",
      app_plugin_resource_manifest_json("com.example.globalautoload", "Global Autoload Plugin", global_marker, "global-review", "Global review prompt",
                                        "prompts/review.md", "global-triage", "Global plugin triage skill", "skills/global-triage.md"));
  write_app_test_file(paths.ava_config_dir / "plugins" / "com.example.globalautoload" / "prompts" / "review.md",
                      "Global enabled plugin prompt loads while project is untrusted.\n");
  write_app_test_file(paths.ava_config_dir / "plugins" / "com.example.globalautoload" / "skills" / "global-triage.md", "Global enabled plugin skill body.\n");
  write_app_test_file(
      workspace / ".ava" / "plugins" / "com.example.projectautoload" / "plugin.json",
      app_plugin_resource_manifest_json("com.example.projectautoload", "Project Autoload Plugin", project_marker, "project-review", "Project review prompt",
                                        "prompts/review.md", "project-triage", "Project plugin triage skill", "skills/project-triage.md"));
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.projectautoload" / "prompts" / "review.md",
                      "Project enabled plugin prompt must stay gated while untrusted.\n");
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.projectautoload" / "skills" / "project-triage.md",
                      "Project enabled plugin skill body must stay gated.\n");

  auto global_enabled = ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, "com.example.globalautoload", true,
                                                        ava::plugin::PluginScope::Global);
  auto project_enabled = ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, "com.example.projectautoload", true,
                                                         ava::plugin::PluginScope::Project);
  expect(global_enabled.has_value() && project_enabled.has_value(), "plugin resource trust-gate test enables global and project plugins");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime opens with untrusted project plugin resources skipped");
  if (!unlocked_session_result)
    return;
  SCOPED_CRITICAL_AREA_W(session_w, *unlocked_session_result);

  auto has_freshness_source = [&](ava::app::runtime::FreshnessSourceKind kind, std::string_view source_id, std::string_view name) {
    return std::ranges::any_of(session_w->freshness_sources(),
                               [&](auto const& source) { return source.kind == kind && source.source_id == source_id && source.name == name; });
  };

  expect(session_w->project_trust().decision == ava::app::ProjectTrustDecision::Unknown && !ava::app::project_resources_trusted(session_w->project_trust()),
         "project plugin resource trust-gate test starts with project resources excluded");
  expect(session_w->system_prompt().find("Global enabled plugin prompt loads") != std::string::npos &&
             session_w->system_prompt().find("<name>global-triage</name>") != std::string::npos &&
             session_w->system_prompt().find("Project enabled plugin prompt must stay gated") == std::string::npos &&
             session_w->system_prompt().find("project-triage") == std::string::npos,
         "global enabled plugin resources load normally while project plugin resources remain gated");
  expect(has_freshness_source(ava::app::runtime::FreshnessSourceKind::PluginPrompt, "com.example.globalautoload", "global-review") &&
             has_freshness_source(ava::app::runtime::FreshnessSourceKind::PluginSkill, "com.example.globalautoload", "global-triage") &&
             !has_freshness_source(ava::app::runtime::FreshnessSourceKind::PluginPrompt, "com.example.projectautoload", "project-review") &&
             !has_freshness_source(ava::app::runtime::FreshnessSourceKind::PluginSkill, "com.example.projectautoload", "project-triage"),
         "project plugin prompt and skill freshness sources are skipped when project resources are excluded");
  expect(!std::filesystem::exists(global_marker) && !std::filesystem::exists(project_marker),
         "global and project static plugin resource autoload does not execute plugin entrypoints");
}

void test_app_runtime_enabled_plugin_resource_failures_are_context_visible()
{
  auto const root = create_empty_root("app-runtime-plugin-resource-failures");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  auto const marker = root / "plugin-entrypoint-executed";
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.failedresources";
  std::filesystem::create_directories(workspace);

  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(), trusted ? "plugin resource failure test trusts project resources"
                                      : "plugin resource failure test trusts project resources: " + trusted.error().format());

  write_app_test_file(plugin_dir / "plugin.json", app_plugin_resource_manifest_json("com.example.failedresources", "Failed Resource Plugin", marker,
                                                                                    "missing-review", "Missing review prompt", "prompts/review.md",
                                                                                    "broken-skill", "Broken plugin skill", "skills/broken.md"));
  write_app_test_file(root / "outside-prompts" / "review.md", "Outside prompt target must not load through an intermediate symlink.\n");
  write_app_test_file(root / "outside-skill.md", "Outside skill target must not load.\n");
  std::error_code dir_error;
  std::filesystem::create_directories(plugin_dir / "skills", dir_error);
  expect(!dir_error, "plugin resource failure test creates skill directory");
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(root / "outside-prompts", plugin_dir / "prompts", symlink_error);
  expect(!symlink_error, "plugin resource failure test creates escaping intermediate prompt symlink fixture");
  if (symlink_error)
    return;
  std::filesystem::create_symlink(root / "outside-skill.md", plugin_dir / "skills" / "broken.md", symlink_error);
  expect(!symlink_error, "plugin resource failure test creates final symlink skill fixture");
  if (symlink_error)
    return;

  auto enabled = ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, "com.example.failedresources", true,
                                                 ava::plugin::PluginScope::Project);
  expect(enabled.has_value(),
         enabled ? "plugin resource failure test enables project plugin" : "plugin resource failure test enables project plugin: " + enabled.error().format());

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime opens with failed enabled plugin static resources tracked");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  auto missing_prompt = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context missing-review"});
  expect(missing_prompt && missing_prompt->handled && !missing_prompt->output.empty() &&
             missing_prompt->output[0].find("plugin_prompt  project  com.example.failedresources/missing-review") != std::string::npos &&
             missing_prompt->output[0].find("status=unavailable") != std::string::npos,
         "failed enabled plugin prompt resources remain visible in /context without re-reading the escaped path: " +
             (missing_prompt ? (missing_prompt->output.empty() ? std::string("empty output") : missing_prompt->output[0]) : missing_prompt.error().format()));

  auto broken_skill = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context broken-skill"});
  expect(broken_skill && broken_skill->handled && !broken_skill->output.empty() &&
             broken_skill->output[0].find("plugin_skill  project  com.example.failedresources/broken-skill") != std::string::npos &&
             broken_skill->output[0].find("status=unavailable") != std::string::npos,
         "failed enabled plugin skill resources remain visible in /context without re-reading the rejected final symlink");
  auto const system_prompt = ava::app::runtime::session_ts::rat(unlocked_session)->system_prompt();
  expect(system_prompt.find("Outside prompt target must not load") == std::string::npos &&
             system_prompt.find("Outside skill target must not load") == std::string::npos && !std::filesystem::exists(marker),
         "failed plugin static resources never consume outside content through intermediate or final symlinks and do not execute plugin entrypoints");
}

void test_app_runtime_plugin_install_remove_commands()
{
  auto const root = create_empty_root("app-runtime-plugin-install-remove");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  auto const marker = root / "installed-plugin-entrypoint-executed";
  auto const source_plugin = root / "local-plugin-source";
  auto const installed_plugin_dir = paths.ava_config_dir / "plugins" / "com.example.installed";
  auto const stale_staging_dir = paths.ava_config_dir / "plugins" / "com.example.installed.installing-stale";
  std::filesystem::create_directories(workspace);

  write_app_test_file(source_plugin / "plugin.json", app_plugin_install_manifest_json("com.example.installed", "Installed Plugin", marker));
  write_app_test_file(source_plugin / "prompts" / "review.md", "Installed plugin prompt content.\n");
  write_app_test_file(source_plugin / "skills" / "triage.md", "Installed plugin skill content.\n");
  write_app_test_file(stale_staging_dir / "plugin.json", app_plugin_install_manifest_json("com.example.installed", "Stale Installing Plugin", marker));
  std::error_code source_permissions_error;
  std::filesystem::permissions(source_plugin / "prompts" / "review.md",
                               std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_read | std::filesystem::perms::group_write | std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_read | std::filesystem::perms::others_write | std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::replace, source_permissions_error);
  expect(!source_permissions_error, "plugin install/remove command test widens source file permissions");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "plugin install/remove command test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  auto install = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins install ../local-plugin-source/plugin.json"});
  expect(install && install->handled && !install->output.empty() &&
             install->output[0].find("Installed global plugin com.example.installed") != std::string::npos &&
             install->output[0].find("status: disabled") != std::string::npos &&
             install->output[0].find("no plugin process was started") != std::string::npos && std::filesystem::exists(installed_plugin_dir / "plugin.json") &&
             !std::filesystem::exists(marker),
         "plugin install resolves a relative plugin.json path against Session.current_dir, ignores stale staging directories, and copies without starting its "
         "entrypoint");
  auto const installed_prompt_permissions = std::filesystem::status(installed_plugin_dir / "prompts" / "review.md").permissions();
  expect((installed_prompt_permissions & (std::filesystem::perms::group_write | std::filesystem::perms::others_write | std::filesystem::perms::group_exec |
                                          std::filesystem::perms::others_exec)) == std::filesystem::perms::none &&
             (installed_prompt_permissions & std::filesystem::perms::owner_exec) != std::filesystem::perms::none,
         "plugin install normalizes copied file permissions to owner-only while preserving owner execute when present");

  auto plugins = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins list"});
  expect(plugins && plugins->handled && !plugins->output.empty() && plugins->output[0].find("com.example.installed  disabled  global") != std::string::npos,
         "installed plugin is discoverable and disabled by default");

  auto prompt = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins prompt com.example.installed install-review"});
  auto skill = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins skill com.example.installed install-triage"});
  expect(prompt && prompt->handled && !prompt->output.empty() && prompt->output[0].find("Installed plugin prompt content") != std::string::npos && skill &&
             skill->handled && !skill->output.empty() && skill->output[0].find("Installed plugin skill content") != std::string::npos,
         "installed plugin static resources are readable from the installed copy before enablement");

  auto enable = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins enable com.example.installed"});
  expect(enable && enable->handled && !enable->output.empty() && enable->output[0].find("Enabled global plugin com.example.installed") != std::string::npos,
         "installed global plugin can be enabled after install");

  auto const slash_items = ava::app::command_catalog_slash_items_1(unlocked_session);
  auto const* plugin_item = tui_test_support::find_slash_command_item(slash_items, "/plugin");
  expect(tui_test_support::has_slash_argument_completion(plugin_item, 1, "com.example.installed", {"run"}) &&
             tui_test_support::has_slash_argument_completion(plugin_item, 2, "status", {"run", "com.example.installed"}) && !std::filesystem::exists(marker),
         "enabled installed plugin contributes command completions without executing the plugin process");

  auto remove_enabled = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins remove com.example.installed"});
  expect(remove_enabled && remove_enabled->handled && !remove_enabled->output.empty() &&
             remove_enabled->output[0].find("disable plugin before removing") != std::string::npos && std::filesystem::exists(installed_plugin_dir),
         "plugin remove rejects enabled plugins and keeps the installed directory");

  auto disable = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins disable com.example.installed"});
  auto remove = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins remove com.example.installed"});
  expect(disable && disable->handled && remove && remove->handled && !remove->output.empty() &&
             remove->output[0].find("Removed global plugin com.example.installed") != std::string::npos && !std::filesystem::exists(installed_plugin_dir) &&
             !std::filesystem::exists(marker),
         "disabled installed global plugin can be removed without executing or stopping plugin processes");

  auto plugins_after_remove = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins list"});
  expect(plugins_after_remove && plugins_after_remove->handled && !plugins_after_remove->output.empty() &&
             plugins_after_remove->output[0].find("com.example.installed") == std::string::npos && std::filesystem::exists(stale_staging_dir),
         "removed plugin no longer appears in plugin discovery and stale install staging dirs remain ignored");

  auto const has_install_staging_residue = [&](std::string_view plugin_id) {
    auto const plugins_root = paths.ava_config_dir / "plugins";
    std::error_code iterate_error;
    std::filesystem::directory_iterator iterator(plugins_root, iterate_error);
    if (iterate_error)
      return true;
    auto const prefix = std::string(plugin_id) + ".installing-";
    for (auto const& entry : iterator)
    {
      if (entry.path().filename().generic_string().starts_with(prefix))
        return true;
    }
    return false;
  };

  auto const symlink_source = root / "symlink-plugin-source";
  auto const symlink_marker = root / "symlink-plugin-entrypoint-executed";
  write_app_test_file(symlink_source / "plugin.json", app_plugin_install_manifest_json("com.example.symlinkinstall", "Symlink Install Plugin", symlink_marker));
  write_app_test_file(symlink_source / "prompts" / "review.md", "Symlink plugin prompt content.\n");
  write_app_test_file(symlink_source / "skills" / "triage.md", "Symlink plugin skill content.\n");
  write_app_test_file(root / "external.txt", "external target must not copy\n");
  std::error_code symlink_error;
  std::filesystem::create_symlink(root / "external.txt", symlink_source / "linked.txt", symlink_error);
  expect(!symlink_error, "plugin install/remove command test creates a symlink package fixture");
  if (!symlink_error)
  {
    auto symlink_install = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins install " + symlink_source.generic_string()});
    expect(symlink_install && symlink_install->handled && !symlink_install->output.empty() &&
               symlink_install->output[0].find("plugin install source must not contain symlinks") != std::string::npos &&
               !std::filesystem::exists(paths.ava_config_dir / "plugins" / "com.example.symlinkinstall") &&
               !has_install_staging_residue("com.example.symlinkinstall") && !std::filesystem::exists(symlink_marker),
           "plugin install rejects symlinked package contents and cleans the staging directory");
  }

  auto const top_level_symlink_source = root / "top-level-symlink-plugin-source";
  auto const top_level_symlink_marker = root / "top-level-symlink-plugin-entrypoint-executed";
  auto const top_level_real_source = root / "top-level-real-plugin-source";
  write_app_test_file(top_level_real_source / "plugin.json",
                      app_plugin_install_manifest_json("com.example.toplevelsymlink", "Top-Level Symlink Plugin", top_level_symlink_marker));
  std::filesystem::create_directory_symlink(top_level_real_source, top_level_symlink_source, symlink_error);
  expect(!symlink_error, "plugin install/remove command test creates a top-level source directory symlink fixture");
  if (!symlink_error)
  {
    auto top_level_symlink_install =
        ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins install " + top_level_symlink_source.generic_string()});
    expect(top_level_symlink_install && top_level_symlink_install->handled && !top_level_symlink_install->output.empty() &&
               top_level_symlink_install->output[0].find("invalid_argument: plugin install source must not be a symlink") != std::string::npos &&
               !std::filesystem::exists(paths.ava_config_dir / "plugins" / "com.example.toplevelsymlink") &&
               !has_install_staging_residue("com.example.toplevelsymlink") && !std::filesystem::exists(top_level_symlink_marker),
           "plugin install rejects a top-level source directory symlink without creating destination or staging residue");
  }

  auto const symlink_parent_real = root / "symlink-parent-real-plugin-source";
  auto const symlink_parent = root / "symlink-parent-plugin-source";
  auto const symlink_parent_marker = root / "symlink-parent-plugin-entrypoint-executed";
  write_app_test_file(symlink_parent_real / "plugin.json",
                      app_plugin_install_manifest_json("com.example.symlinkparent", "Symlink Parent Plugin", symlink_parent_marker));
  std::filesystem::create_directory_symlink(symlink_parent_real, symlink_parent, symlink_error);
  expect(!symlink_error, "plugin install/remove command test creates a plugin.json parent directory symlink fixture");
  if (!symlink_error)
  {
    auto parent_symlink_install =
        ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins install " + (symlink_parent / "plugin.json").generic_string()});
    expect(parent_symlink_install && parent_symlink_install->handled && !parent_symlink_install->output.empty() &&
               parent_symlink_install->output[0].find("invalid_argument: plugin install manifest parent must be a real directory") != std::string::npos &&
               !std::filesystem::exists(paths.ava_config_dir / "plugins" / "com.example.symlinkparent") &&
               !has_install_staging_residue("com.example.symlinkparent") && !std::filesystem::exists(symlink_parent_marker),
           "plugin install rejects a plugin.json path whose parent is a symlink without creating destination or staging residue");
  }
}

void test_runtime_plugin_event_hook_uses_admitted_run_cancellation_scope()
{
  if (ava::process::platform_support_v1() != ava::process::PlatformSupportV1::Posix)
  {
    expect(true, "runtime plugin event cancellation is intentionally unsupported on this platform");
    return;
  }
  auto const root = create_empty_root("app-runtime-plugin-event-run-cancel");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.run-cancel-event";
  auto const observed_marker = root / "event-observed";
  auto const side_effect_marker = root / "post-cancel-side-effect";
  std::filesystem::create_directories(workspace);

  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(), trusted ? "runtime event cancellation fixture trusts project plugins" : trusted.error().format());
  write_app_test_file(plugin_dir / "plugin.json", app_plugin_event_cancel_manifest_json());
  write_app_test_file(plugin_dir / "hook.sh",
                      "#!/bin/sh\n"
                      "IFS= read -r initialize || exit 2\n"
                      "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava.plugin.v1\","
                      "\"plugin_version\":\"1.0\",\"contributions\":{}}'\n"
                      "IFS= read -r event || exit 3\n"
                      "printf observed > " +
                          shell_single_quoted(observed_marker.string()) +
                          "\n"
                          "sleep 1\n"
                          "printf side-effect > " +
                          shell_single_quoted(side_effect_marker.string()) +
                          "\n"
                          "printf '%s\\n' '{\"id\":\"ava_2\",\"type\":\"event.observed\",\"ok\":true}'\n"
                          "while IFS= read -r line; do :; done\n");
  auto enabled = ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, "com.example.run-cancel-event", true,
                                                 ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), enabled ? "runtime event cancellation fixture enables its plugin" : enabled.error().format());

  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto application_scope = ava::process::ProcessScopeV1::application(supervisor);
  expect(application_scope.has_value(),
         application_scope ? "runtime event cancellation fixture creates application process scope" : application_scope.error().format());
  if (!trusted || !enabled || !application_scope)
    return;

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  open_context.application_process_scope = *application_scope;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), session ? "runtime event cancellation fixture opens a scoped session" : session.error().format());
  if (!session)
    return;

  std::shared_ptr<ava::app::SessionRunController> controller;
  {
    SCOPED_CRITICAL_AREA_R(session_r, *session);
    controller = session_r->run_controller();
  }
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  ava::app::runtime::RunOptions options;
  options.access_token = "fake";
  options.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };

  auto run = std::async(std::launch::async, [&] { return ava::app::run_prompt(*session, "cancel the event hook", provider, transport, options); });
  bool const hook_observed = wait_for_app_plugin_path(observed_marker);
  auto stopped = controller ? controller->request_stop()
                            : ava::core::Result<bool>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing run controller")));
  bool const prompt_settlement = run.wait_for(2s) == std::future_status::ready;
  if (!prompt_settlement)
  {
    static_cast<void>(supervisor->request_stop(application_scope->owner_prefix(), ava::process::TerminationReasonV1::ApplicationShutdown,
                                               std::chrono::steady_clock::now() + 2s));
  }
  auto run_result = run.get();
  std::this_thread::sleep_for(1100ms);

  auto snapshot = supervisor->snapshot();
  std::vector<ava::process::ProcessSnapshotRecordV1> plugin_records;
  for (auto const& record : snapshot.records)
    if (record.role == ava::process::ProcessRoleV1::Plugin)
      plugin_records.push_back(record);
  bool const canceled_once = plugin_records.size() == 1 && plugin_records.front().state == ava::process::ProcessStateV1::Finished &&
                             plugin_records.front().reason == ava::process::TerminationReasonV1::Canceled &&
                             plugin_records.front().cleanup == ava::process::CleanupStateV1::Complete && plugin_records.front().settlement_count == 1;
  expect(hook_observed && stopped && *stopped && prompt_settlement && !run_result && canceled_once && snapshot.live_records == 0 &&
             !std::filesystem::exists(side_effect_marker) && transport.requests().empty(),
         "SessionRunController stop alone cancels a hanging event hook in the admitted run scope with one complete Plugin settlement and no later side effect");

  supervisor->stop_accepting();
  auto const shutdown = supervisor->shutdown(std::chrono::steady_clock::now() + 2s);
  expect(shutdown.complete, "runtime event cancellation fixture leaves Supervisor cleanup complete");
}

void test_app_context_reports_lsp_config_load_errors()
{
  auto const root = create_empty_root("app-runtime-lsp-config-diagnostics");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  write_app_test_file(paths.ava_config_dir / "lsp.json", "{\"version\":1,\"servers\":[{\"id\":\"bad id\",\"argv\":[\"server\"]}]}\n");
  write_app_test_file(workspace / ".ava" / "lsp.json", "{\"version\":1,\"servers\":[{\"id\":\"project\",\"argv\":[\"server\"],\"timeout_ms\":99}]}\n");
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(), trusted ? "LSP context diagnostic test trusts project resources"
                                      : "LSP context diagnostic test trusts project resources: " + trusted.error().format());

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime opens even when configured LSP provider would fail to load");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  auto context = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/context lsp"});
  expect(context && context->handled && !context->output.empty() && context->output[0].find("lsp_status=error") != std::string::npos &&
             context->output[0].find("lsp_configs=2") != std::string::npos && context->output[0].find("lsp_servers=0") != std::string::npos &&
             context->output[0].find("lsp_errors=2") != std::string::npos && context->output[0].find("lsp_config  global") != std::string::npos &&
             context->output[0].find((paths.ava_config_dir / "lsp.json").string()) != std::string::npos &&
             context->output[0].find("LSP server id is invalid") != std::string::npos && context->output[0].find("lsp_config  project") != std::string::npos &&
             context->output[0].find((workspace / ".ava" / "lsp.json").string()) != std::string::npos &&
             context->output[0].find("LSP server timeout is outside supported limits") != std::string::npos &&
             context->output[0].find("lsp_builtin  id=clangd status=disabled reason=not_enabled") != std::string::npos &&
             context->output[0].find("--background-index") == std::string::npos,
         "/context lsp surfaces global and trusted-project LSP config load errors without treating them as generic provider unavailability");
}

}  // namespace ava::tests::app_runtime_tests
