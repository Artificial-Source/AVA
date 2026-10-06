#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/runtime_event_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/event/events.h"
#include "ava/http/transport.h"
#include "ava/observability/run_observer.h"
#include "ava/app/clipboard_image.h"
#include "ava/app/commands.h"
#include "ava/app/interactive_run_queue.h"
#include "ava/app/onboarding.h"
#include "ava/app/project_trust.h"
#include "ava/app/rpc/serialization.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/OpenContext.h"
#include "ava/app/runtime/RunOptions.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_retry.h"
#include "ava/app/subagent_delivery_manager.h"
#include "ava/agent/mode.h"
#include "ava/plugin/diagnostics.h"
#include "ava/plugin/enablement.h"
#include "ava/plugin/tool_broker.h"
#include "ava/mcp/config.h"
#include "ava/mcp/tool_broker.h"
#include "ava/config/auth.h"
#include "ava/session/attachments.h"
#include "ava/session/record.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission.h"
#include "ava/provider/openai_provider.h"
#include "ava/provider/provider.h"
#include "ava/core/error.h"
#include "ava/core/path.h"
#include "ava/core/result.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_runtime_tests {

using namespace ava::tests;

class RuntimeTraceClock final : public ava::observability::Clock
{
 public:
  [[nodiscard]] std::int64_t now_ms() override { return next_.fetch_add(1, std::memory_order_relaxed); }

 private:
  std::atomic<std::int64_t> next_ = 1;
};

class RuntimeTraceCollector final : public ava::observability::RunObserver
{
 public:
  void on_event(ava::observability::TraceEvent const& event) override
  {
    std::lock_guard lock(mutex);
    events.push_back(event);
  }

  std::mutex mutex;
  std::vector<ava::observability::TraceEvent> events;
};

std::string app_tiny_png_bytes()
{
  std::string bytes;
  bytes.push_back(static_cast<char>(0x89));
  bytes += "PNG\r\n";
  bytes.push_back(static_cast<char>(0x1A));
  bytes += "\nava-runtime-image";
  return bytes;
}

std::string ambient_extension_plugin_manifest_json()
{
  return R"({
  "schema_version": 1,
  "id": "com.example.acp-isolation",
  "name": "ACP isolation canary",
  "version": "0.1.0",
  "api_version": "ava.plugin.v1",
  "description": "ACP ambient extension isolation fixture",
  "entrypoint": {"command": "/bin/false", "args": []},
  "capabilities": ["tools"],
  "contributes": {
    "tools": [{"name": "ambient_probe", "description": "ACP_PLUGIN_TOOL_CANARY_92f1", "input_schema": {"type": "object", "additionalProperties": false}}],
    "prompts": [{"name": "ambient-prompt", "description": "Ambient prompt", "path": "prompts/ambient.md"}],
    "skills": [{"name": "ambient-plugin-skill", "description": "ACP_PLUGIN_SKILL_CANARY_1b64", "path": "skills/ambient.md"}]
  }
})";
}

// Verify debug session-lock tracking and the checked cooperative-thread owner.
//
// The JoinThread scope must request stop and join normally. Debug builds additionally verify that session mutexes participate in the neutral core
// registry consulted at join boundaries.
void test_debug_session_mutex_tracks_current_thread()
{
  std::atomic<bool> worker_finished = false;
  {
    auto worker = ava::core::JoinThread::create("join_thread_test", [&worker_finished](std::stop_token stop_token) {
      while (!stop_token.stop_requested())
        std::this_thread::yield();
      worker_finished.store(true, std::memory_order_release);
    });
  }
  expect(worker_finished.load(std::memory_order_acquire), "JoinThread destruction requests stop and joins its worker");

#ifdef CWDEBUG
  using ava::core::SessionDebugMutex;

  expect(!SessionDebugMutex::current_thread_holds_session_lock(), "debug session mutex registry starts empty");
  expect(!ava::core::current_thread_holds_long_wait_incompatible_lock(), "core long-wait registry starts empty");
  SessionDebugMutex first;
  SessionDebugMutex second;
  first.lock();
  expect(SessionDebugMutex::current_thread_holds_session_lock(), "debug session mutex registry observes one held session lock");
  expect(ava::core::current_thread_holds_long_wait_incompatible_lock(), "session mutex registers with core long-wait tracking");
  second.lock();
  expect(SessionDebugMutex::current_thread_holds_session_lock(), "debug session mutex registry supports nested locks for different sessions");
  first.unlock();
  expect(SessionDebugMutex::current_thread_holds_session_lock(), "debug session mutex registry supports non-LIFO unlock order");
  second.unlock();
  expect(!SessionDebugMutex::current_thread_holds_session_lock(), "debug session mutex registry empties after every session lock is released");
  expect(!ava::core::current_thread_holds_long_wait_incompatible_lock(), "core long-wait registry empties after every session lock is released");

  AVA_ASSERT_NO_SESSION_LOCK_HELD("debug session mutex test boundary");
  // Expose the debug mutex through the minimal session_ts-shaped interface required by the session-specific assertion.
  struct SessionMutexWitness
  {
    SessionDebugMutex& value;
    [[nodiscard]] SessionDebugMutex& mutex() const { return value; }
  } witness{first};
  AVA_ASSERT_SESSION_UNLOCKED(witness, "debug specific session mutex test boundary");
#endif
}

void test_app_run_prompt_isolates_ambient_extensions()
{
  auto const root = create_empty_root("app-runtime-ambient-extension-isolation");
  auto const workspace = root / "workspace";
  auto const home = root / "home";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(home);

  ScopedEnvVar home_env("HOME", home.string());
  ScopedEnvVar config_env("XDG_CONFIG_HOME", paths.config_home.string());
  ScopedEnvVar state_env("XDG_STATE_HOME", paths.state_home.string());
  ScopedEnvVar data_env("XDG_DATA_HOME", paths.data_home.string());

  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(), "ambient extension isolation fixture trusts its synthetic workspace");

  write_app_test_file(workspace / "AGENTS.md", "ACP_ORDINARY_CONTEXT_CANARY_846d\n");
  write_app_test_file(workspace / ".ava" / "skills" / "ordinary-canary" / "SKILL.md",
                      "---\nname: ordinary-canary\ndescription: ACP_ORDINARY_SKILL_CANARY_193c\n---\nOrdinary skill body.\n");
  write_app_test_file(workspace / ".ava" / "agents" / "ambient-agent.md",
                      "---\nname: ambient-agent-canary\ndescription: ACP_SUBAGENT_CANARY_629e\nmode: subagent\n---\nCustom agent prompt.\n");
  auto const selected_agent_path = workspace / ".ava" / "agents" / "selected-primary.md";
  write_app_test_file(selected_agent_path,
                      "---\nname: selected-primary\ndescription: Selected primary canary.\nmode: primary\n---\nSELECTED_PRIMARY_PROMPT_CANARY_11f4\n");
  auto const global_skill_name = "ambient-global-skill";
  write_app_test_file(paths.ava_config_dir / "skills" / global_skill_name / "SKILL.md",
                      "---\nname: ambient-global-skill\ndescription: ACP_GLOBAL_SKILL_CANARY_73c2\n---\nACP_GLOBAL_SKILL_BODY_CANARY_8a91\n");

  auto const plugin_id = "com.example.acp-isolation";
  auto const plugin_dir = paths.ava_config_dir / "plugins" / plugin_id;
  write_app_test_file(plugin_dir / "plugin.json", ambient_extension_plugin_manifest_json());
  write_app_test_file(plugin_dir / "prompts" / "ambient.md", "ACP_PLUGIN_PROMPT_CANARY_e8ad\n");
  write_app_test_file(plugin_dir / "skills" / "ambient.md", "ACP_PLUGIN_SKILL_BODY_CANARY_a53f\n");
  auto enabled = ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, plugin_id, true, ava::plugin::PluginScope::Global);
  expect(enabled.has_value(), "ambient extension isolation fixture enables its synthetic global plugin");

  auto const ambient_mcp_name = ava::mcp::mcp_model_tool_name("ambient_canary", "echo");
  write_app_test_file(paths.ava_config_dir / "mcp.json", app_test_mcp_config_json("ambient_canary", "Ambient MCP canary", AVA_FAKE_MCP_SERVER_PATH));

  auto diagnostics = ava::plugin::collect_plugin_diagnostics(
      ava::plugin::PluginDiscoveryOptions{.global_plugins_dir = paths.ava_config_dir / "plugins", .project_plugins_dir = workspace / ".ava" / "plugins"},
      paths.ava_state_dir / "plugin-enablement.json", workspace);
  bool const plugin_diagnostic_enabled = std::ranges::any_of(
      diagnostics.plugins, [plugin_id](ava::plugin::PluginStatus const& status) { return status.plugin.manifest.id == plugin_id && status.enabled; });
  expect(plugin_diagnostic_enabled, "ambient extension isolation fixture diagnostics report its plugin enabled");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  open_context.prompt_overrides.system_prompt = "ACP_BASE_PROMPT_CANARY_5fa7";
  open_context.requested_primary_agent = "selected-primary";
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime opens the ambient extension isolation fixture");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  CRITICAL_AREA_BEGIN_W(session);
  auto const ordinary_prompt = session_w->system_prompt();
  expect(ordinary_prompt.find("ACP_BASE_PROMPT_CANARY_5fa7") != std::string::npos &&
             ordinary_prompt.find("ACP_ORDINARY_CONTEXT_CANARY_846d") != std::string::npos &&
             ordinary_prompt.find("ACP_PLUGIN_PROMPT_CANARY_e8ad") != std::string::npos &&
             ordinary_prompt.find("ACP_PLUGIN_SKILL_CANARY_1b64") != std::string::npos &&
             ordinary_prompt.find("ACP_GLOBAL_SKILL_CANARY_73c2") != std::string::npos &&
             ordinary_prompt.find("ACP_ORDINARY_SKILL_CANARY_193c") != std::string::npos &&
             ordinary_prompt.find("ACP_SUBAGENT_CANARY_629e") != std::string::npos &&
             ordinary_prompt.find("SELECTED_PRIMARY_PROMPT_CANARY_11f4") != std::string::npos &&
             ordinary_prompt.find("<available_skills>") != std::string::npos && ordinary_prompt.find("<available_subagents>") != std::string::npos,
         "ordinary runtime prompt retains base, context, plugin, skill, and subagent extension canaries");

  session_w->resources().mcp_config = std::make_shared<ava::mcp::McpConfig const>();
  CRITICAL_AREA_END_W(session);

  write_app_test_file(selected_agent_path,
                      "---\nname: selected-primary\ndescription: Changed selected primary.\nmode: primary\n---\nCHANGED_PRIMARY_MUST_NOT_RELOAD\n");
  auto reloaded_prompt = ava::app::select_runtime_prompt_state(unlocked_session, ava::agent::Mode::Plan);
  expect(reloaded_prompt && reloaded_prompt->system_prompt.find("SELECTED_PRIMARY_PROMPT_CANARY_11f4") != std::string::npos &&
             reloaded_prompt->ambient_extension_free_system_prompt.find("SELECTED_PRIMARY_PROMPT_CANARY_11f4") != std::string::npos &&
             reloaded_prompt->system_prompt.find("CHANGED_PRIMARY_MUST_NOT_RELOAD") == std::string::npos,
         "runtime prompt reloads reuse the resolved selected primary in ordinary and ambient-free variants");
  {
    SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);
    auto switched_model = session_w->model();
    switched_model.model_id += "-selected-primary-switch";
    auto switched = session_w->switch_model(std::move(switched_model));
    expect(switched && *switched && session_w->system_prompt().find("SELECTED_PRIMARY_PROMPT_CANARY_11f4") != std::string::npos &&
               session_w->ambient_extension_free_system_prompt().find("SELECTED_PRIMARY_PROMPT_CANARY_11f4") != std::string::npos &&
               session_w->system_prompt().find("CHANGED_PRIMARY_MUST_NOT_RELOAD") == std::string::npos,
           "model switches preserve the resolved selected primary prompt variants");
  }

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"isolated answer\"}\n\n"
              "data: [DONE]\n\n",
  }});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "fake";
  run_options.isolate_ambient_extensions = true;
  run_options.exact_builtin_tool_names = std::vector<std::string>{"read_file", "list_directory"};
  auto result = ava::app::run_prompt(unlocked_session, "isolated prompt", provider, transport, run_options);
  expect(result && result->final_text == "isolated answer", "ambient-extension-free runtime prompt completes through the fake transport");
  if (!result || transport.requests().size() != 1)
    return;

  auto const& request = transport.requests()[0].body;
  expect(request.find("ACP_BASE_PROMPT_CANARY_5fa7") != std::string::npos && request.find("SELECTED_PRIMARY_PROMPT_CANARY_11f4") != std::string::npos &&
             request.find("ACP_ORDINARY_CONTEXT_CANARY_846d") != std::string::npos,
         "isolated runtime request preserves explicit base prompt, selected primary, and ordinary AGENTS context");
  expect(request.find("ACP_PLUGIN_PROMPT_CANARY_e8ad") == std::string::npos && request.find("ACP_PLUGIN_SKILL_CANARY_1b64") == std::string::npos &&
             request.find("ACP_PLUGIN_SKILL_BODY_CANARY_a53f") == std::string::npos && request.find(global_skill_name) == std::string::npos &&
             request.find("ACP_GLOBAL_SKILL_CANARY_73c2") == std::string::npos && request.find("ACP_GLOBAL_SKILL_BODY_CANARY_8a91") == std::string::npos,
         "isolated runtime request omits plugin and global skill names, catalogs, and bodies");
  expect(request.find("ACP_ORDINARY_SKILL_CANARY_193c") == std::string::npos && request.find("<available_skills>") == std::string::npos,
         "isolated runtime request omits ordinary available-skill catalogs");
  expect(request.find("ACP_SUBAGENT_CANARY_629e") == std::string::npos && request.find("ambient-agent-canary") == std::string::npos &&
             request.find("<available_subagents>") == std::string::npos,
         "isolated runtime request omits builtin and custom subagent catalogs");
  expect(request.find(ava::plugin::plugin_model_tool_name(plugin_id, "ambient_probe")) == std::string::npos,
         "isolated runtime request omits enabled ambient plugin model tools");
  expect(request.find(ambient_mcp_name) == std::string::npos,
         "isolated runtime request does not fall back to ambient MCP when session MCP is explicitly empty");
  expect(request.find("\"name\":\"read_file\"") != std::string::npos && request.find("\"name\":\"list_directory\"") != std::string::npos &&
             request.find("\"name\":\"write_file\"") == std::string::npos,
         "isolated runtime exact composition preserves requested builtins and omits non-exact builtins");

  ava::app::runtime::session_ts::wat(unlocked_session)->resources().mcp_config.reset();
  ava::tests::FakeTransport generic_transport({
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body = "data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_global_skill\",\"name\":\"skill\"}\n\n"
                  "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_global_skill\","
                  "\"delta\":\"{\\\"name\\\":\\\"ambient-global-skill\\\"}\"}\n\n"
                  "data: [DONE]\n\n",
      },
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body = "data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_plugin_skill\",\"name\":\"skill\"}\n\n"
                  "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_plugin_skill\","
                  "\"delta\":\"{\\\"name\\\":\\\"ambient-plugin-skill\\\"}\"}\n\n"
                  "data: [DONE]\n\n",
      },
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"generic isolated answer\"}\n\n"
                  "data: [DONE]\n\n",
      },
  });
  ava::app::runtime::RunOptions generic_options;
  generic_options.access_token = "fake";
  generic_options.isolate_ambient_extensions = true;
  generic_options.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
  auto generic_result = ava::app::run_prompt(unlocked_session, "generic isolated prompt", provider, generic_transport, generic_options);
  expect(generic_result && generic_result->final_text == "generic isolated answer" && generic_transport.requests().size() == 3,
         "generic ambient isolation with null session MCP completes successive unavailable global and plugin skill calls");
  for (auto const& isolated_request : generic_transport.requests())
  {
    expect(isolated_request.body.find(ambient_mcp_name) == std::string::npos &&
               isolated_request.body.find("ACP_GLOBAL_SKILL_CANARY_73c2") == std::string::npos &&
               isolated_request.body.find("ACP_GLOBAL_SKILL_BODY_CANARY_8a91") == std::string::npos &&
               isolated_request.body.find("ACP_PLUGIN_SKILL_CANARY_1b64") == std::string::npos &&
               isolated_request.body.find("ACP_PLUGIN_SKILL_BODY_CANARY_a53f") == std::string::npos,
           "every generic isolated request omits ambient MCP schema and global/plugin skill catalog and body canaries");
  }
  if (!generic_transport.requests().empty())
  {
    expect(generic_transport.requests().front().body.find(global_skill_name) == std::string::npos &&
               generic_transport.requests().front().body.find("ambient-plugin-skill") == std::string::npos,
           "generic isolated initial request omits ambient global and plugin skill names");
    expect(generic_transport.requests().front().body.find(R"("name":"task")") != std::string::npos &&
               generic_transport.requests().front().body.find(R"("name":"job")") != std::string::npos,
           "generic ambient isolation preserves compiled-in task and job product capabilities");
  }

  ava::tests::FakeTransport exact_null_transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"exact null answer\"}\n\n"
              "data: [DONE]\n\n",
  }});
  ava::app::runtime::RunOptions exact_null_options;
  exact_null_options.access_token = "fake";
  exact_null_options.isolate_ambient_extensions = true;
  exact_null_options.exact_builtin_tool_names = std::vector<std::string>{"read_file"};
  auto exact_null_result = ava::app::run_prompt(unlocked_session, "exact null MCP prompt", provider, exact_null_transport, exact_null_options);
  expect(exact_null_result && exact_null_result->final_text == "exact null answer" && exact_null_transport.requests().size() == 1,
         "exact isolated composition allocates an empty immutable MCP config when the session config is null");
  expect(ava::app::runtime::session_ts::rat(unlocked_session)->system_prompt() == ordinary_prompt,
         "isolated runtime requests leave the ordinary session system prompt unchanged");
}

void test_project_primary_revocation_removes_authority_without_broadening_tools()
{
  constexpr std::string_view kAgentName = "layered-security-primary";
  constexpr std::string_view kGlobalCanary = "GLOBAL_PRIMARY_SURVIVAL_CANARY_58cc";
  constexpr std::string_view kInitialProjectCanary = "PROJECT_PRIMARY_REVOKE_CANARY_47e1";
  constexpr std::string_view kReselectedProjectCanary = "PROJECT_PRIMARY_RESELECT_CANARY_715b";
  constexpr std::string_view kReloadedProjectCanary = "PROJECT_PRIMARY_RELOAD_CANARY_a92d";

  auto const root = create_empty_root("app-runtime-project-primary-revocation");
  auto const workspace = root / "workspace";
  auto const home = root / "home";
  auto const paths = app_test_paths(root);
  auto const global_primary_path = paths.ava_config_dir / "agents" / "layered-security-primary.md";
  auto const project_primary_path = workspace / ".ava" / "agents" / "layered-security-primary.md";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(home);

  ScopedEnvVar home_env("HOME", home.string());
  ScopedEnvVar config_env("XDG_CONFIG_HOME", paths.config_home.string());
  ScopedEnvVar state_env("XDG_STATE_HOME", paths.state_home.string());
  ScopedEnvVar data_env("XDG_DATA_HOME", paths.data_home.string());

  auto write_global_primary = [&] {
    write_app_test_file(global_primary_path,
                        "---\nname: layered-security-primary\ndescription: Global fallback primary.\nmode: primary\ntools: inherit\n---\n"
                        "GLOBAL_PRIMARY_SURVIVAL_CANARY_58cc\n");
  };
  auto write_project_primary = [&](std::string_view canary, std::string_view tools) {
    write_app_test_file(project_primary_path, "---\nname: layered-security-primary\ndescription: Project authority canary.\nmode: primary\ntools: " +
                                                  std::string(tools) + "\n---\n" + std::string(canary) + "\n");
  };
  write_global_primary();
  write_project_primary(kInitialProjectCanary, "read-only");
  auto initially_trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(initially_trusted.has_value(), "project-primary revocation fixture starts trusted");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  open_context.requested_primary_agent = std::string(kAgentName);
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), unlocked_session_result
                                                  ? "trusted runtime selects the project primary"
                                                  : "trusted runtime selects the project primary: " + unlocked_session_result.error().format());
  if (!unlocked_session_result)
    return;
  auto& unlocked_session = *unlocked_session_result;

  auto restricted_tools_are_retained = [](ava::agent::ToolVisibilityOptions const& visibility) {
    return visibility.mode == ava::agent::ToolVisibilityMode::Default &&
           visibility.included_tools == std::vector<std::string>({"read_file", "list_directory", "glob", "grep"}) && visibility.excluded_tools.empty();
  };
  auto expect_project_selection = [&](std::string_view canary, ava::agent::SubagentToolPreset preset, std::string_view label) {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    auto const& selected = session_r->selected_primary_agent();
    expect(selected && selected->provenance == ava::agent::AgentDefinitionProvenance::Project && selected->tool_preset == preset &&
               selected->system_prompt.find(canary) != std::string::npos && session_r->requested_primary_agent() == std::optional<std::string>(kAgentName) &&
               session_r->system_prompt().find(canary) != std::string::npos &&
               session_r->ambient_extension_free_system_prompt().find(canary) != std::string::npos &&
               session_r->system_prompt().find(kGlobalCanary) == std::string::npos && restricted_tools_are_retained(session_r->tool_visibility()),
           std::string(label));
  };
  auto expect_project_authority_absent = [&](bool expect_global_fallback, std::string_view label) {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    auto const& selected = session_r->selected_primary_agent();
    bool const selection_safe = expect_global_fallback ? selected && selected->provenance == ava::agent::AgentDefinitionProvenance::Global &&
                                                             selected->system_prompt.find(kGlobalCanary) != std::string::npos
                                                       : !selected;
    auto const prompts_safe = [&](std::string const& prompt) {
      return prompt.find(kInitialProjectCanary) == std::string::npos && prompt.find(kReselectedProjectCanary) == std::string::npos &&
             prompt.find(kReloadedProjectCanary) == std::string::npos &&
             (expect_global_fallback ? prompt.find(kGlobalCanary) != std::string::npos : prompt.find(kGlobalCanary) == std::string::npos);
    };
    expect(selection_safe && session_r->requested_primary_agent() == std::optional<std::string>(kAgentName) && prompts_safe(session_r->system_prompt()) &&
               prompts_safe(session_r->ambient_extension_free_system_prompt()) && restricted_tools_are_retained(session_r->tool_visibility()),
           std::string(label));
  };
  auto run_trust = [&](std::string command, std::string_view label) {
    auto result = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = std::move(command)});
    expect(result && result->handled && !result->output.empty(), std::string(label));
    return result;
  };

  expect_project_selection(kInitialProjectCanary, ava::agent::SubagentToolPreset::ReadOnly,
                           "trusted startup records project provenance and applies its closed read-only visibility");

  auto denied = run_trust("/trust deny", "/trust deny succeeds when replacing a project primary with the permitted global definition");
  expect(denied && denied->output[0].find("decision=denied") != std::string::npos, "/trust deny reports the applied denied decision");
  expect_project_authority_absent(true, "/trust deny removes both project-primary prompt variants and preserves the prior effective tool restriction");

  write_project_primary(kReselectedProjectCanary, "inherit");
  auto reenabled = run_trust("/trust project", "/trust project re-enables and re-resolves the requested primary");
  expect(reenabled && reenabled->output[0].find("decision=trusted") != std::string::npos, "/trust project reports the re-enabled trust decision");
  expect_project_selection(kReselectedProjectCanary, ava::agent::SubagentToolPreset::Inherit,
                           "trust re-enable uses the current permitted project definition without broadening the previous read-only visibility");

  std::error_code remove_global_error;
  std::filesystem::remove(global_primary_path, remove_global_error);
  expect(!remove_global_error, "project-primary revocation fixture removes its global fallback before clear");
  auto cleared = run_trust("/trust clear", "/trust clear succeeds when the requested project-only primary becomes unavailable");
  expect(cleared && cleared->output[0].find("decision=unknown") != std::string::npos, "/trust clear reports the applied unknown decision");
  expect_project_authority_absent(false, "/trust clear falls back to no primary while retaining the previous effective tool restriction");

  auto replacement_result = ava::app::runtime::Session::create_like(unlocked_session, {});
  expect(replacement_result.has_value(),
         replacement_result
             ? "replacement session tolerates an inherited requested primary that is unavailable while untrusted"
             : "replacement session tolerates an inherited requested primary that is unavailable while untrusted: " + replacement_result.error().format());
  if (replacement_result)
  {
    SCOPED_CRITICAL_AREA_R(replacement_r, *replacement_result);
    expect(!replacement_r->selected_primary_agent() && replacement_r->requested_primary_agent() == std::optional<std::string>(kAgentName) &&
               replacement_r->system_prompt().find(kInitialProjectCanary) == std::string::npos &&
               replacement_r->system_prompt().find(kReselectedProjectCanary) == std::string::npos &&
               replacement_r->tool_visibility().mode == ava::agent::ToolVisibilityMode::Default && replacement_r->tool_visibility().included_tools.empty(),
           "a new replacement session preserves selection intent but resets the prior session's sticky tool restriction at the session boundary");
  }

  write_global_primary();
  (void)run_trust("/trust project", "/trust project re-enables the project primary after clear");
  expect_project_selection(kReselectedProjectCanary, ava::agent::SubagentToolPreset::Inherit,
                           "project primary is reselected after clear when trust is explicitly enabled again");
  (void)run_trust("/trust untrust", "/trust untrust revokes a reselected project primary");
  expect_project_authority_absent(true, "/trust untrust removes project authority and preserves the valid global primary");

  auto externally_trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(externally_trusted.has_value(), "project-primary fixture externally re-enables trust for reload coverage");
  (void)run_trust("/reload trust", "/reload trust applies external trust enablement");
  expect_project_selection(kReselectedProjectCanary, ava::agent::SubagentToolPreset::Inherit,
                           "/reload trust re-resolves the requested primary from the newly permitted project catalog");

  write_project_primary(kReloadedProjectCanary, "inherit");
  (void)run_trust("/reload trust", "/reload trust refreshes a trusted primary definition");
  expect_project_selection(kReloadedProjectCanary, ava::agent::SubagentToolPreset::Inherit,
                           "/reload trust never accepts the cached project definition across a trust reload");

  auto externally_denied = ava::app::set_project_trust_decision(paths, workspace, false);
  expect(externally_denied.has_value(), "project-primary fixture externally denies trust for reload coverage");
  (void)run_trust("/reload trust", "/reload trust applies external denial");
  expect_project_authority_absent(true, "/reload trust to untrusted atomically replaces project authority with the permitted global primary");

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"revoked authority absent\"}\n\n"
              "data: [DONE]\n\n",
  }});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "fake";
  auto provider_result = ava::app::run_prompt(unlocked_session, "verify revoked primary", provider, transport, run_options);
  expect(provider_result && provider_result->final_text == "revoked authority absent" && transport.requests().size() == 1,
         "a provider request runs after project-primary revocation");
  if (transport.requests().size() == 1)
  {
    auto const& request = transport.requests().front().body;
    expect(request.find(kInitialProjectCanary) == std::string::npos && request.find(kReselectedProjectCanary) == std::string::npos &&
               request.find(kReloadedProjectCanary) == std::string::npos && request.find(kGlobalCanary) != std::string::npos,
           "the next provider request cannot observe any revoked project-primary prompt content and retains the global primary");
    expect(request.find("\"name\":\"read_file\"") != std::string::npos && request.find("\"name\":\"write_file\"") == std::string::npos &&
               request.find("\"name\":\"bash\"") == std::string::npos,
           "the next provider request retains read tools without silently restoring broader mutation or shell tools");
  }

  write_app_test_file(global_primary_path,
                      "---\nname: layered-security-primary\ndescription: Restricted global fallback.\nmode: primary\ntools: read-only\n---\n"
                      "GLOBAL_PRIMARY_SURVIVAL_CANARY_58cc\n");
  write_project_primary(kReloadedProjectCanary, "inherit");
  expect(ava::app::set_project_trust_decision(paths, workspace, true).has_value(), "inverse preset fixture restores project trust on disk");
  auto inverse_opened = ava::app::runtime::Session::open(open_context);
  bool inverse_starts_broad = false;
  if (inverse_opened)
  {
    SCOPED_CRITICAL_AREA_R(session_r, *inverse_opened);
    inverse_starts_broad = session_r->selected_primary_agent() &&
                           session_r->selected_primary_agent()->provenance == ava::agent::AgentDefinitionProvenance::Project &&
                           session_r->tool_visibility().included_tools.empty();
  }
  auto inverse_denied = inverse_opened ? ava::app::run_command(*inverse_opened, ava::app::CommandRequest{.command = "/trust deny"})
                                       : ava::core::Result<ava::app::CommandResult>(std::unexpected(inverse_opened.error()));
  bool inverse_narrowed = false;
  if (inverse_opened)
  {
    SCOPED_CRITICAL_AREA_R(session_r, *inverse_opened);
    inverse_narrowed = session_r->selected_primary_agent() &&
                       session_r->selected_primary_agent()->provenance == ava::agent::AgentDefinitionProvenance::Global &&
                       restricted_tools_are_retained(session_r->tool_visibility());
  }
  expect(inverse_opened && inverse_starts_broad && inverse_denied && inverse_narrowed,
         "inverse project-inherit/global-read-only presets narrow on revocation while the project-read-only/global-inherit case never broadened");
}

void test_clear_trust_retires_only_for_effective_untrusted_state()
{
  auto const root = create_empty_root("app-runtime-clear-effective-trust");
  auto const ancestor = root / "ancestor";
  auto const workspace = ancestor / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  expect(ava::app::set_project_trust_decision(paths, ancestor, true) && ava::app::set_project_trust_decision(paths, workspace, true),
         "clear-effective fixture starts with trusted ancestor and exact trusted decision");

  ava::app::runtime::OpenContext context;
  context.workspace_dir = workspace;
  context.current_dir = workspace;
  context.paths = paths;
  auto opened = ava::app::runtime::Session::open(context);
  if (!opened)
  {
    expect(false, "clear-effective fixture opens");
    return;
  }
  auto& session = *opened;
  auto inherited_controller = ava::app::runtime::session_ts::rat(session)->run_controller();
  auto inherited_clear = ava::app::run_command(session, ava::app::CommandRequest{.command = "/trust clear"});
  bool inherited_trusted = false;
  {
    SCOPED_CRITICAL_AREA_R(session_r, session);
    inherited_trusted = session_r->project_trust().decision == ava::app::ProjectTrustDecision::Trusted &&
                        session_r->project_trust().matched_path == ava::core::normalized_absolute_path(ancestor) &&
                        session_r->run_controller() == inherited_controller;
  }
  expect(inherited_clear && inherited_trusted && !inherited_controller->authority_retired(),
         "clearing an exact decision that reveals a trusted ancestor keeps the current controller and ordinary refresh behavior");

  auto denied = ava::app::run_command(session, ava::app::CommandRequest{.command = "/trust deny"});
  expect(denied && inherited_controller->authority_retired(), "explicit denial retires the pre-denial trusted controller");
  auto before_untrusted_clear = ava::app::runtime::session_ts::rat(session)->run_controller();
  expect(ava::app::clear_project_trust_decision(paths, ancestor).has_value(),
         "clear-effective fixture removes the ancestor while the exact workspace denial remains effective");
  auto untrusted_clear = ava::app::run_command(session, ava::app::CommandRequest{.command = "/trust clear"});
  bool effective_unknown = false;
  {
    SCOPED_CRITICAL_AREA_R(session_r, session);
    effective_unknown = session_r->project_trust().decision == ava::app::ProjectTrustDecision::Unknown &&
                        session_r->run_controller() != before_untrusted_clear && !session_r->run_controller()->authority_retired();
  }
  expect(untrusted_clear && effective_unknown && before_untrusted_clear->authority_retired(),
         "clearing the final effective decision to unknown performs the same permanent authority retirement transaction");
}

void test_untrusted_mutation_rejects_active_run_and_append_before_write()
{
  auto const root = create_empty_root("app-runtime-revocation-admission-conflicts");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  expect(ava::app::set_project_trust_decision(paths, workspace, true).has_value(), "revocation-conflict fixture starts trusted");

  std::mutex append_mutex;
  std::condition_variable append_changed;
  bool append_entered = false;
  bool release_append = false;
  auto store = ava::session::SessionStore::create(workspace, paths.sessions_dir);
  auto lease = store ? ava::session::SessionLease::create_and_acquire(store->session_path())
                     : ava::core::Result<ava::session::SessionLease>(std::unexpected(store.error()));
  if (store)
  {
    store->set_before_append_identity_check_for_test([&] {
      std::unique_lock lock(append_mutex);
      append_entered = true;
      append_changed.notify_all();
      append_changed.wait(lock, [&] { return release_append; });
    });
  }

  ava::app::runtime::OpenContext context;
  context.workspace_dir = workspace;
  context.current_dir = workspace;
  context.paths = paths;
  auto opened = store && lease ? ava::app::runtime::Session::open_owned(context, *store, *lease, true)
                               : ava::core::Result<ava::app::runtime::session_ts>(std::unexpected(store ? lease.error() : store.error()));
  if (!opened)
  {
    expect(false, "revocation-conflict fixture opens a runtime session");
    return;
  }
  auto& session = *opened;
  auto controller = ava::app::runtime::session_ts::rat(session)->run_controller();
  auto active = controller->admit({.request_id = "provider-run-conflict"});
  auto denied_during_run = ava::app::run_command(session, ava::app::CommandRequest{.command = "/trust deny"});
  auto trust_after_run_conflict = ava::app::load_project_trust_state(paths, workspace);
  expect(active && !denied_during_run && trust_after_run_conflict.decision == ava::app::ProjectTrustDecision::Trusted && !controller->authority_retired(),
         "an admitted provider run makes revocation fail before persistence without retiring the current controller");
  if (active)
    static_cast<void>(active->complete({.run_id = {}, .reason = ava::app::StopReason::ProviderError}));

  ava::agent::SessionAppendSink owner_route;
  {
    SCOPED_CRITICAL_AREA_R(session_r, session);
    owner_route = session_r->owner_append_route_1();
  }
  std::optional<ava::core::VoidResult> append_result;
  std::jthread appender([&] {
    append_result.emplace(owner_route(ava::session::SessionEntry{.id = "revocation-append-conflict",
                                                                 .parent_id = "",
                                                                 .type = ava::session::EntryType::UserMessage,
                                                                 .timestamp = ava::session::now_timestamp(),
                                                                 .data_json = "{\"text\":\"append conflict\"}"}));
  });
  {
    std::unique_lock lock(append_mutex);
    expect(append_changed.wait_for(lock, std::chrono::seconds(3), [&] { return append_entered; }),
           "owner append reaches deterministic in-flight persistence gate");
  }
  auto denied_during_append = ava::app::run_command(session, ava::app::CommandRequest{.command = "/trust deny"});
  auto trust_after_append_conflict = ava::app::load_project_trust_state(paths, workspace);
  expect(!denied_during_append && trust_after_append_conflict.decision == ava::app::ProjectTrustDecision::Trusted && !controller->authority_retired(),
         "an in-flight owner append makes revocation fail before persistence and leaves controller authority unchanged");
  {
    std::lock_guard lock(append_mutex);
    release_append = true;
  }
  append_changed.notify_all();
  appender.join();
  expect(append_result && append_result->has_value(), "the append that won revocation arbitration persists normally");

  auto denied_after_idle = ava::app::run_command(session, ava::app::CommandRequest{.command = "/trust deny"});
  expect(denied_after_idle && ava::app::load_project_trust_state(paths, workspace).decision == ava::app::ProjectTrustDecision::Denied &&
             controller->authority_retired(),
         "the same revocation persists and retires old authority once run and append activity are idle");
}

void test_post_persistence_publication_failure_leaves_reopen_required()
{
  constexpr std::string_view kProjectCanary = "PUBLICATION_FAILURE_PROJECT_CANARY_2a14";
  auto const root = create_empty_root("app-runtime-revocation-publication-failure");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  write_app_test_file(workspace / ".ava" / "APPEND_SYSTEM.md", std::string(kProjectCanary) + "\n");
  expect(ava::app::set_project_trust_decision(paths, workspace, true).has_value(), "publication-failure fixture starts trusted");

  auto coordinator = ava::agent::SubagentCoordinator::create();
  if (!coordinator)
    return;
  auto manager =
      ava::app::SubagentDeliveryManager::create({.coordinator = *coordinator, .revocation_before_publication_for_test = []() -> ava::core::VoidResult {
                                                   return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "injected publication failure"));
                                                 }});
  if (!manager)
    return;
  ava::app::runtime::OpenContext context;
  context.workspace_dir = workspace;
  context.current_dir = workspace;
  context.paths = paths;
  context.subagent_coordinator = *coordinator;
  context.subagent_delivery_manager = *manager;
  auto opened = ava::app::runtime::Session::open(context);
  if (!opened)
    return;
  auto& session = *opened;
  auto old_controller = ava::app::runtime::session_ts::rat(session)->run_controller();
  auto denied = ava::app::run_command(session, ava::app::CommandRequest{.command = "/trust deny"});
  auto const persisted = ava::app::load_project_trust_state(paths, workspace);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "data: [DONE]\n\n"}});
  ava::app::runtime::RunOptions options;
  options.access_token = "fake";
  auto run = ava::app::run_prompt(session, "must require reopen", provider, transport, options);
  auto current_controller = ava::app::runtime::session_ts::rat(session)->run_controller();
  expect(!denied && denied.error().format().find(workspace.string()) == std::string::npos &&
             denied.error().format().find(kProjectCanary) == std::string::npos && persisted.decision == ava::app::ProjectTrustDecision::Denied &&
             current_controller == old_controller && old_controller->authority_retired() && !run && transport.requests().empty(),
         "a post-persistence publication failure never restores trusted disk, retires old and fresh authority, and leaves current transport reopen-required");
  (*manager)->shutdown();
}

void test_session_construction_linearizes_with_workspace_revocation()
{
  constexpr std::string_view kProjectCanary = "CONSTRUCTION_RACE_PROJECT_CANARY_14b7";
  auto const root = create_empty_root("app-runtime-construction-revocation-race");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  write_app_test_file(workspace / ".ava" / "APPEND_SYSTEM.md", std::string(kProjectCanary) + "\n");
  expect(ava::app::set_project_trust_decision(paths, workspace, true).has_value(), "construction race fixture starts trusted");

  struct RaceBarrier
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool construction_armed = false;
    bool construction_reached = false;
    bool release_construction = false;
    bool revocation_armed = false;
    bool revocation_reached = false;
    bool release_revocation = false;
  };
  auto barrier = std::make_shared<RaceBarrier>();
  auto coordinator = ava::agent::SubagentCoordinator::create();
  if (!coordinator)
  {
    expect(false, "construction race fixture creates a coordinator");
    return;
  }
  auto manager = ava::app::SubagentDeliveryManager::create({.coordinator = *coordinator,
                                                            .construction_after_trusted_prompt_resolution_for_test =
                                                                [barrier] {
                                                                  std::unique_lock lock(barrier->mutex);
                                                                  if (!barrier->construction_armed)
                                                                    return;
                                                                  barrier->construction_reached = true;
                                                                  barrier->changed.notify_all();
                                                                  barrier->changed.wait(lock, [&] { return barrier->release_construction; });
                                                                  barrier->construction_armed = false;
                                                                },
                                                            .revocation_after_retirement_for_test =
                                                                [barrier] {
                                                                  std::unique_lock lock(barrier->mutex);
                                                                  if (!barrier->revocation_armed)
                                                                    return;
                                                                  barrier->revocation_reached = true;
                                                                  barrier->changed.notify_all();
                                                                  barrier->changed.wait(lock, [&] { return barrier->release_revocation; });
                                                                  barrier->revocation_armed = false;
                                                                }});
  if (!manager)
  {
    expect(false, "construction race fixture creates a delivery manager");
    return;
  }

  ava::app::runtime::OpenContext context;
  context.workspace_dir = workspace;
  context.current_dir = workspace;
  context.paths = paths;
  context.subagent_coordinator = *coordinator;
  context.subagent_delivery_manager = *manager;
  auto opened = ava::app::runtime::Session::open(context);
  if (!opened)
  {
    expect(false, "construction race fixture opens the revoking session");
    return;
  }
  auto& session = *opened;
  auto trust_file_contents = [&] {
    std::ifstream file(ava::app::project_trust_file(paths), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  };

  {
    std::lock_guard lock(barrier->mutex);
    barrier->construction_armed = true;
    barrier->construction_reached = false;
    barrier->release_construction = false;
  }
  using SessionResult = ava::core::Result<ava::app::runtime::session_ts>;
  std::optional<SessionResult> opening;
  std::jthread opener([&] { opening.emplace(ava::app::runtime::Session::create_like(session, {})); });
  bool construction_reached = false;
  {
    std::unique_lock lock(barrier->mutex);
    construction_reached = barrier->changed.wait_for(lock, std::chrono::seconds(3), [&] { return barrier->construction_reached; });
  }
  expect(construction_reached, "create_like pauses after trusted prompt resolution while holding construction authority");
  if (!construction_reached)
  {
    {
      std::lock_guard lock(barrier->mutex);
      barrier->release_construction = true;
    }
    barrier->changed.notify_all();
    opener.join();
    return;
  }

  auto const trust_before_conflict = trust_file_contents();
  auto conflicting_denial = ava::app::run_command(session, ava::app::CommandRequest{.command = "/trust deny"});
  auto const trust_after_conflict = trust_file_contents();
  auto const disk_after_conflict = ava::app::load_project_trust_state(paths, workspace);
  bool const conflict_is_redacted = !conflicting_denial && conflicting_denial.error().format().find(workspace.string()) == std::string::npos &&
                                    conflicting_denial.error().format().find(kProjectCanary) == std::string::npos;
  expect(!conflicting_denial && conflict_is_redacted && trust_before_conflict == trust_after_conflict &&
             disk_after_conflict.decision == ava::app::ProjectTrustDecision::Trusted,
         "construction wins the workspace barrier and revocation fails retryably before changing trusted disk");

  {
    std::lock_guard lock(barrier->mutex);
    barrier->release_construction = true;
  }
  barrier->changed.notify_all();
  opener.join();
  expect(opening && opening->has_value(), "create_like publishes its trusted Session after the construction barrier releases");
  if (!opening || !*opening)
    return;

  auto& constructed = **opening;
  std::shared_ptr<ava::app::SessionRunController> constructed_controller;
  bool constructed_is_trusted = false;
  {
    SCOPED_CRITICAL_AREA_R(session_r, constructed);
    constructed_controller = session_r->run_controller();
    constructed_is_trusted = session_r->project_trust().decision == ava::app::ProjectTrustDecision::Trusted &&
                             session_r->system_prompt().find(kProjectCanary) != std::string::npos &&
                             session_r->ambient_extension_free_system_prompt().find(kProjectCanary) != std::string::npos;
  }
  expect(constructed_is_trusted, "the construction winner returns both trusted prompt variants");

  auto retried_denial = ava::app::run_command(session, ava::app::CommandRequest{.command = "/trust deny"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport stale_transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "data: [DONE]\n\n"}});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "fake";
  auto stale_run = ava::app::run_prompt(constructed, "must not reach transport", provider, stale_transport, run_options);
  expect(retried_denial && constructed_controller && constructed_controller->authority_retired() && !stale_run && stale_transport.requests().empty(),
         "retrying revocation captures and retires the newly registered opener before any stale provider request");

  auto restored = ava::app::run_command(session, ava::app::CommandRequest{.command = "/trust project"});
  auto owned_store = ava::session::SessionStore::create(workspace, paths.sessions_dir);
  auto owned_lease = owned_store ? ava::session::SessionLease::create_and_acquire(owned_store->session_path())
                                 : ava::core::Result<ava::session::SessionLease>(std::unexpected(owned_store.error()));
  if (!restored || !owned_store || !owned_lease)
  {
    expect(false, "inverse construction race restores trust and creates owned session inputs");
    return;
  }

  {
    std::lock_guard lock(barrier->mutex);
    barrier->revocation_armed = true;
    barrier->revocation_reached = false;
    barrier->release_revocation = false;
  }
  std::optional<ava::core::Result<ava::app::CommandResult>> inverse_denial;
  std::jthread revoker([&] { inverse_denial.emplace(ava::app::run_command(session, ava::app::CommandRequest{.command = "/trust deny"})); });
  bool revocation_reached = false;
  {
    std::unique_lock lock(barrier->mutex);
    revocation_reached = barrier->changed.wait_for(lock, std::chrono::seconds(3), [&] { return barrier->revocation_reached; });
  }
  expect(revocation_reached, "inverse revocation pauses with workspace maintenance reserved");
  if (!revocation_reached)
  {
    {
      std::lock_guard lock(barrier->mutex);
      barrier->release_revocation = true;
    }
    barrier->changed.notify_all();
    revoker.join();
    return;
  }

  auto inverse_open = ava::app::runtime::Session::open_owned(context, *owned_store, *owned_lease, true);
  bool const inverse_error_is_redacted = !inverse_open && inverse_open.error().format().find(workspace.string()) == std::string::npos &&
                                         inverse_open.error().format().find(kProjectCanary) == std::string::npos;
  expect(!inverse_open && inverse_error_is_redacted && !owned_lease->canonical_path().empty(),
         "maintenance wins the inverse race and open_owned fails retryably before reading or consuming owned authority");

  {
    std::lock_guard lock(barrier->mutex);
    barrier->release_revocation = true;
  }
  barrier->changed.notify_all();
  revoker.join();
  expect(inverse_denial && *inverse_denial && ava::app::load_project_trust_state(paths, workspace).decision == ava::app::ProjectTrustDecision::Denied,
         "the inverse revocation completes its denied persistence and publication");

  auto inverse_retry = ava::app::runtime::Session::open_owned(context, *owned_store, *owned_lease, true);
  bool inverse_retry_is_denied = false;
  if (inverse_retry)
  {
    SCOPED_CRITICAL_AREA_R(session_r, *inverse_retry);
    inverse_retry_is_denied = session_r->project_trust().decision == ava::app::ProjectTrustDecision::Denied &&
                              session_r->system_prompt().find(kProjectCanary) == std::string::npos &&
                              session_r->ambient_extension_free_system_prompt().find(kProjectCanary) == std::string::npos;
  }
  expect(inverse_retry_is_denied, "retrying open_owned after revocation reads denied disk and excludes the project canary from both prompt variants");
  (*manager)->shutdown();
}

void test_workspace_revocation_retires_retained_sessions_transactionally()
{
  constexpr std::string_view kProjectCanary = "RETAINED_WORKSPACE_PROJECT_CANARY_5d72";
  auto const root = create_empty_root("app-runtime-workspace-revocation-transaction");
  auto const workspace = root / "workspace";
  auto const other_workspace = root / "other-workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(other_workspace);
  write_app_test_file(workspace / ".ava" / "APPEND_SYSTEM.md", std::string(kProjectCanary) + "\n");
  expect(ava::app::set_project_trust_decision(paths, workspace, true) && ava::app::set_project_trust_decision(paths, other_workspace, true),
         "retained-workspace transaction fixtures start with explicit independent trust decisions");

  struct RetirementBarrier
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool reached = false;
    bool release = false;
  };
  auto barrier = std::make_shared<RetirementBarrier>();
  auto coordinator = ava::agent::SubagentCoordinator::create();
  if (!coordinator)
  {
    expect(false, "retained-workspace transaction creates a coordinator");
    return;
  }
  auto manager = ava::app::SubagentDeliveryManager::create({.coordinator = *coordinator, .revocation_after_retirement_for_test = [barrier] {
                                                              std::unique_lock lock(barrier->mutex);
                                                              barrier->reached = true;
                                                              barrier->changed.notify_all();
                                                              barrier->changed.wait(lock, [&] { return barrier->release; });
                                                            }});
  if (!manager)
  {
    expect(false, "retained-workspace transaction creates a delivery manager");
    return;
  }

  auto context_for = [&](std::filesystem::path directory) {
    ava::app::runtime::OpenContext context;
    context.workspace_dir = directory;
    context.current_dir = directory;
    context.paths = paths;
    context.subagent_coordinator = *coordinator;
    context.subagent_delivery_manager = *manager;
    return context;
  };
  auto workspace_context = context_for(workspace);
  auto other_context = context_for(other_workspace);
  auto opened_a = ava::app::runtime::Session::open(workspace_context);
  auto opened_b = ava::app::runtime::Session::open(workspace_context);
  auto opened_other = ava::app::runtime::Session::open(other_context);
  expect(opened_a && opened_b && opened_other, "one manager opens two target-workspace sessions and one isolated workspace session");
  if (!opened_a || !opened_b || !opened_other)
    return;
  std::optional<ava::app::runtime::session_ts> session_a(std::in_place, std::move(*opened_a));
  auto& session_b = *opened_b;
  auto& other_session = *opened_other;

  std::shared_ptr<ava::app::SessionRunController> controller_a;
  std::shared_ptr<ava::app::SessionRunController> controller_b;
  std::shared_ptr<ava::app::SessionRunController> other_controller;
  std::string session_a_id;
  ava::agent::SessionAppendSink stale_owner_route;
  {
    SCOPED_CRITICAL_AREA_R(session_r, *session_a);
    controller_a = session_r->run_controller();
    session_a_id = session_r->store.session_id();
    stale_owner_route = session_r->owner_append_route_1();
    expect(session_r->system_prompt().find(kProjectCanary) != std::string::npos &&
               session_r->ambient_extension_free_system_prompt().find(kProjectCanary) != std::string::npos,
           "captured session A begins with project authority in both prompt variants");
  }
  controller_b = ava::app::runtime::session_ts::rat(session_b)->run_controller();
  other_controller = ava::app::runtime::session_ts::rat(other_session)->run_controller();
  ava::app::runtime::RunOptions retained_options;
  retained_options.access_token = "discarded-retained-token";
  expect((*manager)->refresh_parent(*session_a, retained_options).has_value(), "session A publishes a retained parent capsule before session B revokes trust");

  std::optional<ava::core::Result<ava::app::CommandResult>> denial;
  std::jthread revoker([&] { denial.emplace(ava::app::run_command(session_b, ava::app::CommandRequest{.command = "/trust deny"})); });
  {
    std::unique_lock lock(barrier->mutex);
    expect(barrier->changed.wait_for(lock, std::chrono::seconds(3), [&] { return barrier->reached; }),
           "revocation pauses deterministically after all old controllers retire and before capsule purge/publication");
  }

  auto persisted_during_pause = ava::app::load_project_trust_state(paths, workspace);
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport stale_transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "data: [DONE]\n\n"}});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "fake";
  auto stale_run_during_pause = ava::app::run_prompt(*session_a, "must not reach transport", provider, stale_transport, run_options);
  auto stale_append_during_pause = stale_owner_route(ava::session::SessionEntry{.id = "retired-stale-append",
                                                                                .parent_id = "",
                                                                                .type = ava::session::EntryType::UserMessage,
                                                                                .timestamp = ava::session::now_timestamp(),
                                                                                .data_json = "{\"text\":\"blocked\"}"});
  auto other_guard = other_controller->admit({.request_id = "unaffected-workspace-run"});
  if (other_guard)
    static_cast<void>(other_guard->complete({.run_id = {}, .reason = ava::app::StopReason::ProviderError}));
  expect(persisted_during_pause.decision == ava::app::ProjectTrustDecision::Denied && controller_a->authority_retired() && controller_b->authority_retired() &&
             !stale_run_during_pause && stale_transport.requests().empty() && !stale_append_during_pause && other_guard &&
             !other_controller->authority_retired(),
         "after persistence and retirement, captured A cannot admit, append, or send transport while another workspace remains runnable");

  {
    std::lock_guard lock(barrier->mutex);
    barrier->release = true;
  }
  barrier->changed.notify_all();
  revoker.join();
  expect(denial && *denial && (*denial)->handled, "session B completes the denied-trust transaction after the retirement barrier releases");

  std::shared_ptr<ava::app::SessionRunController> fresh_b;
  ava::app::ProjectTrustDecision session_b_decision = ava::app::ProjectTrustDecision::Unknown;
  std::string session_b_prompt;
  std::string session_b_ambient_prompt;
  {
    SCOPED_CRITICAL_AREA_R(session_r, session_b);
    fresh_b = session_r->run_controller();
    session_b_decision = session_r->project_trust().decision;
    session_b_prompt = session_r->system_prompt();
    session_b_ambient_prompt = session_r->ambient_extension_free_system_prompt();
  }
  ava::tests::FakeTransport stale_transport_after({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "data: [DONE]\n\n"}});
  auto stale_run_after = ava::app::run_prompt(*session_a, "still must not reach transport", provider, stale_transport_after, run_options);
  bool retained_found = true;
  auto retained = (*manager)->retained_session(session_a_id, workspace, retained_found, {.exact_session_id = true});
  expect(fresh_b && fresh_b != controller_b && !fresh_b->authority_retired() && session_b_decision == ava::app::ProjectTrustDecision::Denied &&
             session_b_prompt.find(kProjectCanary) == std::string::npos && session_b_ambient_prompt.find(kProjectCanary) == std::string::npos &&
             !stale_run_after && stale_transport_after.requests().empty() && !retained_found && !retained,
         "purge publishes a fresh denied controller with both safe prompts while stale A remains permanently retired and its capsule is unavailable");

  session_a.reset();
  workspace_context.exact_session_id = true;
  auto resumed_a = ava::app::runtime::Session::open(workspace_context, {.sessionless = false,
                                                                        .requested_session_id = session_a_id,
                                                                        .fork_session_id = std::nullopt,
                                                                        .initial_session_name = std::nullopt,
                                                                        .continue_last_session = false,
                                                                        .initial_reasoning_level = std::nullopt,
                                                                        .expected_original_cwd = std::nullopt});
  bool resumed_safe = false;
  if (resumed_a)
  {
    SCOPED_CRITICAL_AREA_R(session_r, *resumed_a);
    resumed_safe = session_r->run_controller() != controller_a && session_r->project_trust().decision == ava::app::ProjectTrustDecision::Denied &&
                   session_r->system_prompt().find(kProjectCanary) == std::string::npos;
  }
  expect(resumed_safe, "resuming A bypasses the purged stale capsule, reloads denied trust from disk, and constructs a new controller");
  bool other_unchanged = false;
  {
    SCOPED_CRITICAL_AREA_R(session_r, other_session);
    other_unchanged = session_r->project_trust().decision == ava::app::ProjectTrustDecision::Trusted && session_r->run_controller() == other_controller;
  }
  expect(other_unchanged, "the transaction does not retire or mutate another workspace managed in the same application process");
  (*manager)->shutdown();
}

void test_app_run_prompt_sources_private_launch_display_from_runtime_invocation()
{
  auto const root = create_empty_root("app-runtime-private-launch-source");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(paths.ava_config_dir);
  write_app_test_file(paths.models_file, R"JSON({
    "default_provider":"openai",
    "default_model":"launch-source",
    "models":[{
      "provider":"openai",
      "id":"launch-source",
      "name":"Configured   Launch Model",
      "family":"launch-source",
      "api_family":"openai_responses",
      "context_window_tokens":100000,
      "max_output_tokens":4096,
      "supports_tools":true,
      "supports_streaming":true,
      "supports_reasoning":true,
      "reasoning_levels":["high"],
      "input_modalities":["text"],
      "output_modalities":["text"]
    }]
  })JSON");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime private-launch source fixture opens a configured session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  auto const session_id = session_w->store.session_id();

  auto task_response = [](std::string_view call_id) {
    return ava::http::HttpResponse{.status_code = 200,
                                   .headers = {},
                                   .body = "data: {\"type\":\"response.function_call.added\",\"call_id\":\"" + std::string(call_id) +
                                           "\",\"name\":\"task\"}\n\n"
                                           "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"" +
                                           std::string(call_id) +
                                           "\",\"delta\":\"{\\\"description\\\":\\\"runtime launch\\\",\\\"prompt\\\":\\\"return child\\\","
                                           "\\\"subagent_type\\\":\\\"general\\\"}\"}\n\n"
                                           "data: {\"type\":\"response.function_call.done\",\"call_id\":\"" +
                                           std::string(call_id) + "\"}\n\ndata: [DONE]\n\n"};
  };
  auto text_response = [](std::string_view text) {
    return ava::http::HttpResponse{.status_code = 200,
                                   .headers = {},
                                   .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"" + std::string(text) + "\"}\n\ndata: [DONE]\n\n"};
  };
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport default_transport({task_response("runtime_task_default"), text_response("parent default")});
  std::vector<ava::agent::SubagentLaunchNotification> launches;
  std::vector<ava::event::RuntimeEvent> public_events;
  std::vector<ava::event::EventEnvelope> public_task_running_envelopes;
  ava::app::InteractiveRunQueue tui_turns(session_id, "tui-request-initial", nullptr);
  auto const initial_request_id = tui_turns.active_request_id();
  ava::event::EventEnvelopeContext tui_envelope_context;
  tui_envelope_context.request_id = initial_request_id;
  tui_envelope_context.correlation_id = initial_request_id;
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  run_options.request_id = initial_request_id;
  run_options.on_subagent_launch = [&launches](auto const& launch) { launches.push_back(launch); };
  run_options.event_sink = [&public_events, &public_task_running_envelopes, &tui_envelope_context](auto const& event) {
    public_events.push_back(event);
    if (auto const* start = std::get_if<ava::event::ToolStartEvent>(&event.payload()); start && start->payload.tool == "task")
      public_task_running_envelopes.push_back(ava::event::to_event_envelope(event, tui_envelope_context));
    return ava::core::VoidResult{};
  };

  CRITICAL_AREA_END_W(session);

  auto default_result = ava::app::run_prompt(unlocked_session, "launch default", provider, default_transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  auto selected = session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "high", .budget_tokens = std::nullopt, .display = {}});
  auto queued = tui_turns.queue_follow_up("launch high");
  auto follow_up = tui_turns.take_next_follow_up();
  auto started = follow_up ? tui_turns.mark_follow_up_started(*follow_up) : ava::core::VoidResult{};
  expect(queued.has_value() && follow_up.has_value() && started.has_value(),
         "runtime private-launch source fixture activates a production TUI queued follow-up identity");
  if (follow_up)
  {
    tui_envelope_context.request_id = follow_up->request_id;
    tui_envelope_context.correlation_id = follow_up->request_id;
  }
  run_options.request_id = follow_up ? follow_up->request_id : std::string("missing-follow-up");
  ava::tests::FakeTransport high_transport({task_response("runtime_task_high"), text_response("parent high")});

  CRITICAL_AREA_END_W(session);

  auto high_result = selected ? ava::app::run_prompt(unlocked_session, "launch high", provider, high_transport, run_options)
                              : ava::core::Result<ava::agent::AgentLoopResult>(std::unexpected(selected.error()));

  expect(default_result && high_result, "runtime private-launch source fixture completes both delegated turns: default=" +
                                            (default_result ? std::string("ok") : default_result.error().format()) +
                                            ", high=" + (high_result ? std::string("ok") : high_result.error().format()));
  expect(launches.size() == 2, "runtime private-launch source fixture emits exactly two callbacks; actual=" + std::to_string(launches.size()) +
                                   ", requests=" + std::to_string(default_transport.requests().size() + high_transport.requests().size()) +
                                   ", default_text=" + (default_result ? default_result->final_text : std::string("error")) +
                                   ", high_text=" + (high_result ? high_result->final_text : std::string("error")) +
                                   (launches.empty() ? std::string{} : ", first_id=" + launches.front().tool_call_id));
  if (launches.size() == 2)
  {
    expect(launches[0].tool_call_id == "runtime_task_default" && launches[1].tool_call_id == "runtime_task_high",
           "runtime private launch callback captures exact backend task call identities");
    expect(launches[0].request_id == initial_request_id && launches[0].correlation_id == initial_request_id && follow_up &&
               launches[1].request_id == follow_up->request_id && launches[1].correlation_id == follow_up->request_id &&
               launches[1].correlation_id != launches[0].correlation_id,
           "runtime private launch callback captures initial and queued TUI request identities");
    expect(public_task_running_envelopes.size() == 2 && public_task_running_envelopes[0].request_id == launches[0].request_id &&
               public_task_running_envelopes[0].correlation_id == launches[0].correlation_id &&
               public_task_running_envelopes[1].request_id == launches[1].request_id &&
               public_task_running_envelopes[1].correlation_id == launches[1].correlation_id,
           "initial and queued TUI turns give public task Running envelopes and private launch notifications exactly matching identities");
    expect(launches[0].display.model_display_name() == "Configured Launch Model" && launches[1].display.model_display_name() == "Configured Launch Model",
           "runtime private launch display uses configured ModelInfo display name; actual=" + launches[0].display.model_display_name());
    expect(launches[0].display.reasoning_label() == "default" && launches[1].display.reasoning_label() == "high",
           "runtime private launch display uses literal default then explicit AVA level; actual=" + launches[0].display.reasoning_label() + "/" +
               launches[1].display.reasoning_label());
  }

  std::string public_event_bytes;
  std::size_t event_index = 0;
  for (auto const& event : public_events)
  {
    ava::event::EventEnvelopeContext context;
    context.event_id = "event_" + std::to_string(event_index++);
    public_event_bytes += ava::event::serialize_event_envelope_jsonl(ava::event::to_event_envelope(event, context));
  }
  auto const session_bytes = app_read_binary_file(ava::app::runtime::session_ts::rat(unlocked_session)->store.session_path());
  expect(public_event_bytes.find("Configured Launch Model") == std::string::npos,
         "private launch model display stays absent from public runtime event envelopes");
  expect(session_bytes.find("Configured Launch Model") == std::string::npos, "private launch model display stays absent from persisted session bytes");

  write_app_test_file(paths.models_file, R"JSON({
    "default_provider":"openai",
    "default_model":"unnamed-launch-model",
    "models":[{
      "provider":"openai",
      "id":"unnamed-launch-model",
      "family":"unnamed-launch-model",
      "api_family":"openai_responses",
      "supports_tools":true,
      "supports_streaming":true,
      "input_modalities":["text"],
      "output_modalities":["text"]
    }]
  })JSON");
  auto unlocked_unnamed_session = ava::app::runtime::Session::open(open_context);
  expect(unlocked_unnamed_session.has_value(), "runtime private-launch source fixture opens an unnamed custom model session");
  if (unlocked_unnamed_session)
  {
    std::vector<ava::agent::SubagentLaunchNotification> unnamed_launches;
    ava::app::runtime::RunOptions unnamed_options;
    unnamed_options.access_token = "token";
    unnamed_options.request_id = std::nullopt;
    unnamed_options.on_subagent_launch = [&unnamed_launches](auto const& launch) { unnamed_launches.push_back(launch); };
    ava::tests::FakeTransport unnamed_transport({task_response("runtime_task_unnamed"), text_response("parent unnamed")});
    auto unnamed_result = ava::app::run_prompt(*unlocked_unnamed_session, "launch unnamed", provider, unnamed_transport, unnamed_options);
    SCOPED_CRITICAL_AREA_R(unnamed_session_r, *unlocked_unnamed_session);
    expect(unnamed_result && unnamed_launches.size() == 1 && unnamed_session_r->model().display_name == "unnamed-launch-model" &&
               ava::config::proven_configured_model_display_name(unnamed_session_r->model()).empty() &&
               unnamed_launches.front().display.model_display_name().empty() && !unnamed_launches.front().request_id.empty() &&
               unnamed_launches.front().request_id == unnamed_launches.front().correlation_id,
           "direct run_prompt generates and consistently uses one admitted request identity while unnamed custom launch display never falls back to ids");
  }
}

void test_app_run_prompt_emits_events()
{
  auto const root = create_empty_root("app-runtime-run");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  {
    std::ofstream file(workspace / "AGENTS.md", std::ios::binary | std::ios::trunc);
    file << "runtime run context\n";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime run test opens session");
  if (!unlocked_session_result)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"runtime answer\"}\n\n"
              "data: [DONE]\n\n",
  }});
  std::vector<ava::event::RuntimeEvent> events;
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  run_options.event_sink = [&events](ava::event::RuntimeEvent const& event) {
    events.push_back(event);
    return ava::core::VoidResult{};
  };

  auto result = ava::app::run_prompt(*unlocked_session_result, "hello runtime", provider, transport, run_options);

  expect(result && result->final_text == "runtime answer", "runtime run_prompt returns agent loop result");
  auto const* assistant = events.size() == 4 ? ava::tests::runtime_event_as<ava::event::AssistantMessageEvent>(events[2]) : nullptr;
  auto const* completion = events.size() == 4 ? ava::tests::runtime_event_as<ava::event::CompletionEvent>(events[3]) : nullptr;
  expect(events.size() == 4 && ava::tests::runtime_event_as<ava::event::SessionStartEvent>(events[0]) &&
             ava::tests::runtime_event_as<ava::event::UserMessageEvent>(events[1]) && assistant && completion,
         "runtime run_prompt emits session, user, assistant, and done events");
  expect(assistant && assistant->payload.text == "runtime answer" && completion && completion->payload.provider_iterations == 1,
         "runtime run_prompt events include final text and completion counters");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("runtime run context") != std::string::npos,
         "runtime run_prompt sends context-augmented system prompt to provider");
  auto entries = ava::app::runtime::session_ts::rat(*unlocked_session_result)->store.load();
  expect(entries && entries->size() == 4 && (*entries)[1].type == ava::session::EntryType::UserMessage &&
             (*entries)[2].type == ava::session::EntryType::AssistantOutputItem && (*entries)[3].type == ava::session::EntryType::AssistantTurnCommit,
         "runtime run_prompt persists one committed v4 assistant turn in the runtime session");
}

void test_app_run_prompt_expands_file_references()
{
  auto const root = create_empty_root("app-runtime-file-reference");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace / "src");
  std::filesystem::create_directories(workspace / "my folder");
  {
    std::ofstream file(workspace / "src" / "reference.cpp", std::ios::binary | std::ios::trunc);
    file << "int referenced_symbol() { return 42; }\n";
  }
  {
    std::ofstream file(workspace / "my folder" / "reference file.cpp", std::ios::binary | std::ios::trunc);
    file << "int spaced_reference_symbol() { return 24; }\n";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime file reference test opens session");
  if (!unlocked_session_result)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"reference answer\"}\n\n"
              "data: [DONE]\n\n",
  }});
  std::vector<ava::event::RuntimeEvent> events;
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  run_options.event_sink = [&events](ava::event::RuntimeEvent const& event) {
    events.push_back(event);
    return ava::core::VoidResult{};
  };

  auto result =
      ava::app::run_prompt(*unlocked_session_result, "review @src/reference.cpp and @\"my folder/reference file.cpp\"", provider, transport, run_options);

  expect(result && result->final_text == "reference answer", "runtime file reference prompt succeeds");
  auto const* user_message = events.size() >= 2 ? ava::tests::runtime_event_as<ava::event::UserMessageEvent>(events[1]) : nullptr;
  expect(user_message && user_message->payload.text.find("Referenced files:") != std::string::npos &&
             user_message->payload.text.find("int referenced_symbol()") != std::string::npos &&
             user_message->payload.text.find("int spaced_reference_symbol()") != std::string::npos,
         "runtime user_message event contains expanded plain and quoted file reference content");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("review @src/reference.cpp") != std::string::npos &&
             transport.requests()[0].body.find("--- src/reference.cpp ---") != std::string::npos &&
             transport.requests()[0].body.find("int referenced_symbol()") != std::string::npos &&
             transport.requests()[0].body.find("--- my folder/reference file.cpp ---") != std::string::npos &&
             transport.requests()[0].body.find("int spaced_reference_symbol()") != std::string::npos,
         "runtime provider request receives bounded plain and quoted referenced file content");
  auto entries = ava::app::runtime::session_ts::rat(*unlocked_session_result)->store.load();
  auto const expanded_user_entry = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                                     return entry.type == ava::session::EntryType::UserMessage &&
                                            entry.data_json.find("int referenced_symbol()") != std::string::npos &&
                                            entry.data_json.find("int spaced_reference_symbol()") != std::string::npos;
                                   });
  expect(expanded_user_entry, "runtime session persists expanded plain and quoted file reference content in the user entry");
}

void test_app_run_prompt_sends_imported_image_attachment()
{
  auto const root = create_empty_root("app-runtime-image-attachment");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const image_path = workspace / "screen.png";
  write_app_test_file(image_path, app_tiny_png_bytes());

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime image attachment test opens session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  auto imported = ava::session::import_image_attachment(session_w->store, image_path);
  expect(imported.has_value(), "runtime image attachment test imports local image into session storage");
  if (!imported)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"image answer\"}\n\n"
              "data: [DONE]\n\n",
  }});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  run_options.image_attachments = {*imported};

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "describe this image", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  expect(result && result->final_text == "image answer", "runtime run_prompt accepts imported image attachments");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("\"type\":\"input_image\"") != std::string::npos &&
             transport.requests()[0].body.find("data:image/png;base64,") != std::string::npos,
         "runtime provider request includes a verified image data URL payload");
  auto entries = session_w->store.load();
  auto const persisted_metadata = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                                    return entry.type == ava::session::EntryType::UserMessage && entry.data_json.find("\"attachments\"") != std::string::npos &&
                                           entry.data_json.find("\"mime_type\":\"image/png\"") != std::string::npos &&
                                           entry.data_json.find("data_base64") == std::string::npos;
                                  });
  expect(persisted_metadata, "runtime persists image attachment metadata without inline image bytes");
}

void test_app_clipboard_image_file_override_imports_attachment()
{
  auto const root = create_empty_root("app-runtime-clipboard-image");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const image_path = workspace / "clipboard.png";
  write_app_test_file(image_path, app_tiny_png_bytes());

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime clipboard image test opens session");
  if (!unlocked_session_result)
    return;

  auto const store = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, *unlocked_session_result);
    return session_r->store;
  }();

  ScopedEnvVar clipboard_file("AVA_CLIPBOARD_IMAGE_FILE", image_path.string());
  auto imported = ava::app::import_clipboard_image_attachment(store, std::nullopt);
  expect(imported && imported->has_value() && (*imported)->mime_type == "image/png" && (*imported)->byte_size == app_tiny_png_bytes().size(),
         "runtime clipboard image override imports supported image bytes into session storage");
  if (!imported || !*imported)
    return;

  auto loaded = ava::session::load_image_attachment(store, **imported);
  expect(loaded && loaded->bytes == app_tiny_png_bytes(), "runtime clipboard image override writes reusable session-owned attachment bytes");
}

void test_app_run_prompt_emits_provider_retry_events_when_enabled()
{
  auto const root = create_empty_root("app-runtime-provider-retry");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime provider retry test opens session");
  if (!unlocked_session_result)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{.status_code = 429, .headers = {{"Retry-After", "0"}}, .body = "{\"error\":{\"message\":\"rate limited\"}}"},
       sse_response(final_text_sse("retried answer"))});
  std::vector<ava::event::RuntimeEvent> events;
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  run_options.enable_transport_retries = true;
  run_options.event_sink = [&events](ava::event::RuntimeEvent const& event) {
    events.push_back(event);
    return ava::core::VoidResult{};
  };
  bool runtime_retry_cancel = false;
  run_options.cancel_requested = [&runtime_retry_cancel] { return runtime_retry_cancel; };

  auto result = ava::app::run_prompt(*unlocked_session_result, "retry runtime", provider, transport, run_options);
  expect(result && result->final_text == "retried answer" && transport.requests().size() == 2,
         "runtime run_prompt retries transient provider transport failures when enabled");
  expect(std::ranges::any_of(events,
                             [](ava::event::RuntimeEvent const& event) {
                               auto const* retry = ava::tests::runtime_event_as<ava::event::RetryEvent>(event);
                               return retry && retry->payload.trigger == "provider_transport" && retry->payload.reason == "rate_limited" &&
                                      retry->payload.attempt == 2 && retry->payload.max_attempts == 3 && retry->payload.delay_ms == 0 &&
                                      retry->payload.text == "HTTP status 429";
                             }),
         "runtime run_prompt emits provider retry metadata through the shared event sink");
  events.clear();
  auto retry_options = ava::app::runtime::runtime_retry_options(*unlocked_session_result, run_options);
  expect(retry_options.on_retry != nullptr, "runtime retry options expose provider retry event mapping");
  expect(retry_options.response_retry_decision != nullptr, "runtime retry options install provider response retry classification");
  if (retry_options.response_retry_decision)
  {
    expect(retry_options.response_retry_decision(ava::http::HttpResponse{.status_code = 429, .headers = {}, .body = "rate limited"}) ==
               ava::http::ResponseRetryDecision::RateLimited,
           "runtime retry options classify generic 429 responses as rate-limited retries");
    expect(retry_options.response_retry_decision(ava::http::HttpResponse{.status_code = 503, .headers = {}, .body = "try again"}) ==
               ava::http::ResponseRetryDecision::Transient,
           "runtime retry options classify generic 503 responses as transient retries");
    expect(retry_options.response_retry_decision(ava::http::HttpResponse{.status_code = 401, .headers = {}, .body = "unauthorized"}) ==
               ava::http::ResponseRetryDecision::NoRetry,
           "runtime retry options classify authentication failures as non-retryable");
    expect(retry_options.response_retry_decision(ava::http::HttpResponse{
               .status_code = 429, .headers = {}, .body = "insufficient_quota: billing hard limit"}) == ava::http::ResponseRetryDecision::NoRetry,
           "runtime retry options classify 429 quota bodies as non-retryable");
  }
  runtime_retry_cancel = true;
  expect(retry_options.cancel_requested && retry_options.cancel_requested(), "runtime retry options preserve the active run cancellation callback");
  runtime_retry_cancel = false;
  if (retry_options.on_retry)
  {
    auto emitted_tick = retry_options.on_retry(ava::http::RetryOptions::Event{.attempt = 2,
                                                                              .max_attempts = 3,
                                                                              .delay_ms = 1000,
                                                                              .remaining_ms = 500,
                                                                              .reason = "rate_limited",
                                                                              .status_code = 429,
                                                                              .streaming = true,
                                                                              .countdown_tick = true});
    auto const* tick = events.size() == 1 ? ava::tests::runtime_event_as<ava::event::RetryTickEvent>(events[0]) : nullptr;
    expect(emitted_tick.has_value() && tick && tick->payload.trigger == "provider_transport" && tick->payload.remaining_ms == 500 &&
               tick->payload.delay_ms == 1000 && tick->payload.status == "streaming",
           "runtime retry options map provider countdown ticks to explicit backend retry_tick events");
  }
}

void test_app_run_prompt_observation_shares_context_across_compaction_and_retry()
{
  auto const root = create_empty_root("app-runtime-observation-context");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "observed runtime compaction test opens session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  CRITICAL_AREA_BEGIN_W(session);

  session_w->model_selection().model.context_window_tokens = 100;
  auto seeded = session_w->append_owned({.id = "observed-old-context",
                                         .parent_id = "",
                                         .type = ava::session::EntryType::UserMessage,
                                         .timestamp = ava::session::now_timestamp(),
                                         .data_json = "{\"text\":\"" + std::string(420, 'x') + "\"}"});
  expect(seeded.has_value(), "observed runtime compaction test seeds enough context to require a summary request");
  if (!seeded)
    return;

  auto collector = std::make_shared<RuntimeTraceCollector>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<RuntimeTraceClock>(),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{.status_code = 429, .headers = {{"Retry-After", "0"}}, .body = "{\"error\":{\"message\":\"rate limited\"}}"},
       ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"OBSERVED SUMMARY\"}"},
       sse_response(final_text_sse("observed answer"))});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "CANARY_RUNTIME_TOKEN";
  run_options.enable_transport_retries = true;
  run_options.observation = observation;

  CRITICAL_AREA_END_W(session);

  auto result = ava::app::run_prompt(unlocked_session, "continue observed run", provider, transport, run_options);

  CRITICAL_AREA_CONTINUE_W(session);

  expect(result && result->final_text == "observed answer" && transport.requests().size() == 3,
         "one scripted run_prompt performs a retried compaction summary then its agent request");
  if (!result)
    return;

  std::vector<ava::observability::TraceEvent> trace_events;
  {
    std::lock_guard lock(collector->mutex);
    trace_events = collector->events;
  }
  std::string run_id;
  std::string turn_id;
  unsigned logical_requests = 0;
  unsigned attempt_results = 0;
  bool retry = false;
  bool terminal = false;
  for (auto const& event : trace_events)
  {
    if (run_id.empty())
    {
      run_id = event.run_id;
      turn_id = event.turn_id;
    }
    expect(!event.run_id.empty() && !event.turn_id.empty() && event.run_id == run_id && event.turn_id == turn_id,
           "compaction, retries, attempts, and agent events share one runtime-generated run/turn context");
    logical_requests += event.type == ava::observability::TraceEventType::TransportRequestResult;
    attempt_results += event.type == ava::observability::TraceEventType::TransportAttemptResult;
    retry = retry || event.type == ava::observability::TraceEventType::TransportRetry;
    terminal = terminal || (event.type == ava::observability::TraceEventType::AgentRunTerminal && event.outcome == ava::observability::TraceOutcome::Completed);
  }
  expect(!run_id.empty() && logical_requests == 2 && attempt_results == 3 && retry && terminal,
         "trace contains both logical requests, the summary retry attempts, retry record, and completed agent terminal");

  std::ifstream session_file(session_w->store.session_path(), std::ios::binary);
  std::string session_json((std::istreambuf_iterator<char>(session_file)), std::istreambuf_iterator<char>());
  auto messages_json = session_w->messages_result_json();
  auto prompt_json = ava::app::rpc::prompt_result_json(session_w->store.session_id(), *result);
  auto state_json = session_w->state_result_json_1(false);
  auto has_observer_fields = [](std::string_view json) {
    return json.find("agent.run_start") != std::string_view::npos || json.find("transport.attempt_result") != std::string_view::npos ||
           json.find("\"timestamp_ms\":") != std::string_view::npos || json.find("\"run_id\":") != std::string_view::npos ||
           json.find("CANARY_RUNTIME_TOKEN") != std::string_view::npos;
  };
  expect(messages_json && !has_observer_fields(session_json) && !has_observer_fields(*messages_json) && !has_observer_fields(prompt_json) &&
             !has_observer_fields(state_json),
         "observer-only fields and canaries never enter authoritative session or RPC JSON");
}

void test_app_run_prompt_emits_tool_progress_and_session_spill()
{
  auto const root = create_empty_root("app-runtime-tool-progress");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  expect(::chmod(temp_root().c_str(), S_IRWXU) == 0 && ::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "runtime tool progress workspace is owner-only for sealed command planning");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime tool progress test opens session");
  if (!unlocked_session_result)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
                                           .status_code = 200,
                                           .headers = {},
                                           .body = "data: {\"type\":\"response.function_call.added\",\"call_id\":"
                                                   "\"call_bash\",\"name\":\"bash\"}\n\n"
                                                   "data: {\"type\":\"response.function_call_arguments.delta\","
                                                   "\"call_id\":\"call_bash\",\"delta\":\"{\\\"command\\\":"
                                                   "\\\"pwd\\\",\\\"max_bytes\\\":4}\"}\n\n"
                                                   "data: [DONE]\n\n",
                                       },
                                       ava::http::HttpResponse{
                                           .status_code = 200,
                                           .headers = {},
                                           .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":"
                                                   "\"tool done\"}\n\n"
                                                   "data: [DONE]\n\n",
                                       }});
  std::vector<ava::event::RuntimeEvent> events;
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  run_options.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
  run_options.event_sink = [&events](ava::event::RuntimeEvent const& event) {
    events.push_back(event);
    return ava::core::VoidResult{};
  };

  auto result = ava::app::run_prompt(*unlocked_session_result, "run pwd", provider, transport, run_options);
  auto const spill_dir = ava::app::runtime::session_ts::rat(*unlocked_session_result)->store.session_path().parent_path() / "spill";
  bool has_spill_file = false;
  std::error_code iter_error;
  for (std::filesystem::directory_iterator it(spill_dir, iter_error), end; !iter_error && it != end; it.increment(iter_error))
  {
    has_spill_file = true;
    expect(it->path().parent_path() == spill_dir, "runtime spill file stays under the session-local spill directory");
    break;
  }
  expect(result && result->final_text == "tool done" &&
             std::ranges::any_of(events,
                                 [](ava::event::RuntimeEvent const& event) {
                                   auto const* progress = ava::tests::runtime_event_as<ava::event::ToolProgressEvent>(event);
                                   return progress && progress->payload.call_id == "call_bash" && progress->payload.tool == "bash" &&
                                          !progress->payload.text.empty();
                                 }),
         "runtime run_prompt emits additive tool_progress events from tool callbacks");
  expect(std::ranges::any_of(events,
                             [](ava::event::RuntimeEvent const& event) {
                               auto const* start = ava::tests::runtime_event_as<ava::event::ToolStartEvent>(event);
                               return start && start->payload.call_id == "call_bash" && start->payload.tool == "bash" &&
                                      start->payload.args_json.find("\"command\":\"pwd\"") != std::string::npos;
                             }) &&
             std::ranges::any_of(events,
                                 [](ava::event::RuntimeEvent const& event) {
                                   auto const* result = ava::tests::runtime_event_as<ava::event::ToolResultEvent>(event);
                                   return result && result->payload.call_id == "call_bash" && result->payload.tool == "bash" && result->payload.truncated &&
                                          result->payload.total_bytes > 0 && result->payload.output_lines > 0 && result->payload.total_lines > 0 &&
                                          !result->payload.spill_path.empty() && result->payload.result_json.find("\"spill_file\"") != std::string::npos;
                                 }),
         "runtime run_prompt emits semantic tool args, result, and spill metadata for frontend adapters");
  expect(has_spill_file, "runtime run_prompt configures session-local spill files for truncated tool output");
}

void test_app_first_run_auth_onboarding()
{
  auto const root = create_empty_root("app-first-run-onboarding");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  auto const home = root / "home";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(paths.ava_config_dir);
  auto const home_text = home.string();
  auto const config_home_text = paths.config_home.string();
  auto const state_home_text = paths.state_home.string();
  auto const data_home_text = paths.data_home.string();
  setenv("HOME", home_text.c_str(), 1);
  setenv("XDG_CONFIG_HOME", config_home_text.c_str(), 1);
  setenv("XDG_STATE_HOME", state_home_text.c_str(), 1);
  setenv("XDG_DATA_HOME", data_home_text.c_str(), 1);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "first-run onboarding test opens session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;

  unsetenv("OPENAI_API_KEY");
  auto missing = ava::app::first_run_auth_onboarding_message(unlocked_session);
  expect(missing && *missing == "! OpenAI not connected · /connect" && missing->find(paths.auth_file.string()) == std::string::npos &&
             std::ranges::count(*missing, '\n') == 0,
         "first-run TUI onboarding is one actionable advisory row without auth paths or environment dumps");

  auto required = ava::app::provider_auth_required_message(unlocked_session, "\nslash tool commands still work offline.");
  expect(required.find("Auth is required for provider `openai`") != std::string::npos &&
             required.find("Connect with /connect or /login") != std::string::npos &&
             required.find("slash tool commands still work offline") != std::string::npos,
         "provider auth failure reuses onboarding guidance before a prompt runs");

  setenv("OPENAI_API_KEY", "test-openai-key", 1);
  auto env_ready = ava::app::first_run_auth_onboarding_message(unlocked_session);
  expect(!env_ready, "first-run onboarding stays hidden when provider auth comes from the environment");
  unsetenv("OPENAI_API_KEY");

  auto stored = ava::config::store_provider_credential(
      paths, ava::config::ProviderCredential{.provider_id = "openai", .access_token = "stored-openai-key", .credential_type = "api_key", .source = "test"});
  expect(stored.has_value(), "first-run onboarding test stores OpenAI credential");
  auto stored_ready = ava::app::first_run_auth_onboarding_message(unlocked_session);
  expect(!stored_ready, "first-run onboarding stays hidden when provider auth is stored");
}

void test_app_run_prompt_event_sink_failure_cancels_before_next_provider_call()
{
  auto const root = create_empty_root("app-runtime-event-sink-cancel");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  {
    std::ofstream file(workspace / "note.txt", std::ios::binary | std::ios::trunc);
    file << "event sink cancel data";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime event sink failure test opens session");
  if (!unlocked_session_result)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
                                           .status_code = 200,
                                           .headers = {},
                                           .body = "data: {\"type\":\"response.function_call.added\",\"call_id\":"
                                                   "\"call_read\",\"name\":\"read_file\"}\n\n"
                                                   "data: "
                                                   "{\"type\":\"response.function_call_arguments.delta\","
                                                   "\"call_id\":\"call_read\",\"delta\":\"{\\\"path\\\":"
                                                   "\\\"note.txt\\\"}\"}\n\n"
                                                   "data: [DONE]\n\n",
                                       },
                                       ava::http::HttpResponse{
                                           .status_code = 200,
                                           .headers = {},
                                           .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":"
                                                   "\"should not request\"}\n\n"
                                                   "data: [DONE]\n\n",
                                       }});
  ava::app::runtime::RunOptions run_options;
  run_options.access_token = "token";
  run_options.event_sink = [](ava::event::RuntimeEvent const& event) {
    if (event.type() == ava::event::RuntimeEventType::ToolStart)
    {
      return ava::core::VoidResult{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "event sink failed"))};
    }
    return ava::core::VoidResult{};
  };

  auto result = ava::app::run_prompt(*unlocked_session_result, "read with failing sink", provider, transport, run_options);
  expect(!result && result.error().category() == ava::core::ErrorCategory::Io && result.error().message() == "event sink failed",
         "runtime returns the event sink write failure");
  expect(transport.requests().size() == 1, "event sink failure cancels before the next provider request");
}

}  // namespace ava::tests::app_runtime_tests
