#include "sys.h"
#include "tests/acp_test_declarations.h"
#include "tests/support/acp_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/command/command.h"
#include "ava/http/transport.h"
#include "ava/app/acp/peer.h"
#include "ava/app/acp/service.h"
#include "ava/app/runtime_credentials.h"
#include "ava/app/session_run_controller.h"
#include "ava/app/subagent_delivery_manager.h"
#include "ava/agent/mode.h"
#include "ava/mcp/tool_broker.h"
#include "ava/session/record.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission.h"
#include "ava/permissions/permission_rules.h"
#include "ava/provider/provider.h"
#include "ava/provider/registry.h"
#include "ava/core/error.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/path.h"
#include "ava/core/result.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <future>
#include <ios>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <sys/stat.h>

using namespace std::chrono_literals;
using ava::app::acp::JsonRpcId;
using namespace acp_test;
namespace runtime = ava::app::runtime;

void test_acp_cancel_terminal_arbitration_and_provider_setup_paths()
{
  using namespace ava::app::acp;

  auto run_phase_case = [](ava::app::RunPhase phase, bool expect_cancel) {
    auto root = std::filesystem::temp_directory_path() / ava::core::make_id(expect_cancel ? "acp-cancel-before" : "acp-cancel-late");
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    configure_acp_test_model(root);
    auto paths = ava::tests::app_test_paths(root);
    auto transport_state = std::make_shared<CapturingSequenceState>();
    auto barrier = std::make_shared<RunPhaseBarrier>();
    barrier->target = phase;

    AgentServiceOptions options;
    options.agent_version = "1";
    options.launch_root = ava::core::normalized_absolute_path(workspace);
    options.paths = paths;
    options.run_options.on_phase = [barrier](ava::app::RunPhase observed) { return barrier->observe(observed); };
    options.provider_bundle_factory = sequence_bundle_factory(transport_state, {acp_text_response("terminal success")});
    AgentService service(options);
    std::mutex updates_mutex;
    std::vector<std::string> updates;
    service.bind_update_sender([&](std::string_view, std::string_view update) -> ava::core::VoidResult {
      std::lock_guard lock(updates_mutex);
      updates.emplace_back(update);
      return {};
    });
    static_cast<void>(service.handle_request(initialize_request(), {}));
    auto created = service.handle_request(
        Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
    auto id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
    expect(id.has_value(), "ACP terminal arbitration fixture creates a session");
    if (!id)
      return;

    RequestResult prompt_result;
    ava::core::JoinThread prompt_thread = ava::core::JoinThread::create("prompt_thread", [&] {
      prompt_result =
          service.handle_request(Request{.id = std::int64_t(3),
                                         .method = "session/prompt",
                                         .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"race\"}]}"},
                                 {});
    });
    bool const reached = barrier->wait_until_reached();
    expect(reached,
           expect_cancel ? "ACP prompt reaches deterministic pre-Completing barrier" : "ACP prompt reaches deterministic committed Completing barrier");
    if (reached)
      service.handle_notification(Notification{.method = "session/cancel", .params_json = std::string("{\"sessionId\":\"") + *id + "\"}"}, {});
    barrier->release();
    prompt_thread.join();
    service.shutdown();

    std::size_t request_count = 0;
    {
      std::lock_guard lock(transport_state->mutex);
      request_count = transport_state->request_bodies.size();
    }
    std::size_t update_count = 0;
    {
      std::lock_guard lock(updates_mutex);
      update_count = updates.size();
    }
    auto store = ava::session::SessionStore::open(workspace, *id, paths.sessions_dir);
    auto entries = store ? store->load()
                         : ava::core::Result<std::vector<ava::session::SessionEntry>>(
                               std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "store unavailable")));
    std::size_t assistant_entries = 0;
    if (entries)
      assistant_entries = static_cast<std::size_t>(
          std::count_if(entries->begin(), entries->end(), [](auto const& entry) { return entry.type == ava::session::EntryType::AssistantTurnCommit; }));

    if (expect_cancel)
    {
      expect(prompt_result && *prompt_result == R"({"stopReason":"cancelled"})" && request_count == 0 && update_count == 0 && assistant_entries == 0,
             "cancel accepted before Completing yields only a canceled terminal with no provider output or durable assistant");
    }
    else
    {
      expect(prompt_result && *prompt_result == R"({"stopReason":"end_turn"})" && request_count == 1 && update_count == 1 && assistant_entries == 1,
             "cancel after Completing is a no-op and preserves emitted and durable terminal success");
    }
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
  };

  run_phase_case(ava::app::RunPhase::AwaitingProvider, true);
  run_phase_case(ava::app::RunPhase::Completing, false);

  auto run_setup_case = [](bool fail_setup, bool cancel_setup, ava::core::ErrorCategory failure_category = ava::core::ErrorCategory::Provider) {
    auto root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-provider-setup-cancel");
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    configure_acp_test_model(root);
    auto paths = ava::tests::app_test_paths(root);
    struct SetupGate
    {
      std::mutex mutex;
      std::condition_variable cv;
      bool entered = false;
      bool released = false;
    };
    auto gate = std::make_shared<SetupGate>();
    auto transport_state = std::make_shared<CapturingSequenceState>();

    AgentServiceOptions options;
    options.agent_version = "1";
    options.launch_root = ava::core::normalized_absolute_path(workspace);
    options.paths = paths;
    options.provider_bundle_factory = [gate, transport_state, fail_setup, failure_category](
                                          ava::app::runtime::session_ts const&, ava::app::runtime::RunOptions run_options,
                                          std::string_view) -> ava::core::Result<ava::app::RuntimeProviderRunBundle> {
      {
        AVA_ASSERT_NO_SESSION_LOCK_HELD("provider_bundle_factory lambda");
        std::unique_lock lock(gate->mutex);
        gate->entered = true;
        gate->cv.notify_all();
        gate->cv.wait(lock, [&] { return gate->released; });
      }
      if (fail_setup)
        return std::unexpected(ava::core::Error(failure_category, "provider setup failed deterministically"));
      auto provider = ava::provider::builtin_provider_registry().create("moonshot");
      if (!provider)
        return std::unexpected(std::move(provider.error()));
      run_options.access_token = "test";
      run_options.stream = false;
      std::unique_ptr<ava::http::Transport> transport =
          std::make_unique<CapturingSequenceTransport>(transport_state, std::vector<ava::http::HttpResponse>{acp_text_response()});
      std::unique_ptr<ava::http::Transport> auth = std::make_unique<ava::tests::FakeTransport>(std::vector<ava::http::HttpResponse>{});
      return ava::app::RuntimeProviderRunBundle{
          .provider = std::move(*provider), .transport = std::move(transport), .auth_transport = std::move(auth), .options = std::move(run_options)};
    };
    AgentService service(options);
    service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
    static_cast<void>(service.handle_request(initialize_request(), {}));
    auto created = service.handle_request(
        Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
    auto id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
    expect(id.has_value(), "ACP provider-setup arbitration fixture creates a session");
    if (!id)
      return;

    RequestResult prompt_result;
    ava::core::JoinThread prompt_thread = ava::core::JoinThread::create("prompt_thread", [&] {
      prompt_result =
          service.handle_request(Request{.id = std::int64_t(3),
                                         .method = "session/prompt",
                                         .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"setup\"}]}"},
                                 {});
    });
    {
      std::unique_lock lock(gate->mutex);
      static_cast<void>(gate->cv.wait_for(lock, 2s, [&] { return gate->entered; }));
    }
    if (cancel_setup)
      service.handle_notification(Notification{.method = "session/cancel", .params_json = std::string("{\"sessionId\":\"") + *id + "\"}"}, {});
    {
      std::lock_guard lock(gate->mutex);
      gate->released = true;
    }
    gate->cv.notify_all();
    prompt_thread.join();
    service.shutdown();

    if (cancel_setup)
      expect(prompt_result && *prompt_result == R"({"stopReason":"cancelled"})", fail_setup
                                                                                     ? "cancel accepted during failing provider setup wins the setup error"
                                                                                     : "cancel accepted during provider setup stops the admitted prompt");
    else
      expect(!prompt_result && prompt_result.error().code == -32603 &&
                 prompt_result.error().message.find("provider setup failed deterministically") != std::string::npos,
             "non-authentication provider and permission setup errors remain ACP internal errors when cancellation did not win");
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
  };

  run_setup_case(false, true);
  run_setup_case(true, true);
  run_setup_case(true, false);
  run_setup_case(true, false, ava::core::ErrorCategory::PermissionDenied);
}

void test_acp_critical_commands_remain_one_shot()
{
  using namespace ava::app::acp;
  auto const root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-critical-persistent-command");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "ACP critical persistent-command fixture keeps sealed planning roots owner-only");
  configure_acp_tool_test_model(root);
  auto const paths = ava::tests::app_test_paths(root);
  ava::permissions::PermissionRuleStore const rule_store{.global_rules_file = paths.ava_config_dir / "permission-rules.json",
                                                         .workspace_rules_file = workspace / ".ava" / "permission-rules.json",
                                                         .workspace_dir = workspace};
  auto added = ava::permissions::add_persistent_permission_rule(rule_store,
                                                                ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                      .action = ava::permissions::PermissionAction::Allow,
                                                                                                      .operation = ava::permissions::Operation::RunCommand,
                                                                                                      .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                      .tool_name = "bash",
                                                                                                      .target_path = {},
                                                                                                      .command = "true",
                                                                                                      .command_recipe_key = {},
                                                                                                      .recipe_display = {},
                                                                                                      .critical_acknowledged = true,
                                                                                                      .reason = "operator acknowledged exact critical command",
                                                                                                      .actor = "test"});
  expect(!added && added.error().category() == ava::core::ErrorCategory::InvalidArgument, "ACP cannot persist an exact acknowledged Critical command Allow");

  auto state = std::make_shared<CapturingSequenceState>();
  auto const bash_response = ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body =
          R"({"choices":[{"message":{"tool_calls":[{"id":"call_critical","function":{"name":"bash","arguments":"{\"command\":\"true\"}"}}]},"finish_reason":"tool_calls"}]})"};
  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = workspace;
  options.paths = paths;
  options.provider_bundle_factory = sequence_bundle_factory(state, {bash_response, acp_text_response("critical persistent allow completed")});
  AgentService service(options);
  service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  std::atomic_int permission_requests = 0;
  service.bind_client_request_sender(
      [&permission_requests](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        if (method == "session/request_permission")
          ++permission_requests;
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "unexpected ACP client permission request"));
      },
      [](JsonRpcId const&, std::string) { return true; });
  static_cast<void>(service.handle_request(initialize_request_with_capabilities(R"({"terminal":true})"), {}));
  auto created = service.handle_request(
      Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto const session_id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  RequestResult prompted;
  if (session_id)
    prompted = service.handle_request(
        Request{.id = std::int64_t(3),
                .method = "session/prompt",
                .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"run true\"}]}"},
        {});
  std::size_t provider_requests = 0;
  {
    std::lock_guard lock(state->mutex);
    provider_requests = state->request_bodies.size();
  }
  expect(session_id && prompted && *prompted == R"({"stopReason":"end_turn"})" && permission_requests.load() == 1 && provider_requests == 2,
         "ACP requests one-shot client authority for a verified Critical command instead of recovering persistent command text authority: prompt=" +
             (prompted ? *prompted : prompted.error().message) + ", permission_requests=" + std::to_string(permission_requests.load()) +
             ", provider_requests=" + std::to_string(provider_requests));
  service.shutdown();

  int fallback_calls = 0;
  auto resolver = ava::permissions::build_persistent_permission_rule_resolver(
      rule_store, [&fallback_calls](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++fallback_calls;
        ava::permissions::PermissionResolutionDecision denied(ava::permissions::PermissionResolution::Deny, "client approval required");
        denied.resolution_source = "client";
        return denied;
      });
  ava::permissions::CommandPermissionMetadata unavailable_metadata;
  unavailable_metadata.level = ava::command::CommandLevel::Critical;
  unavailable_metadata.executor_identity_verified = true;
  unavailable_metadata.containment_status = ava::permissions::CommandContainmentStatus::Unavailable;
  unavailable_metadata.backend_maximum_scope = ava::command::InteractiveScope::Once;
  auto unavailable = resolver(ava::permissions::PermissionPrompt{.permission_request_id = "permreq_acp_unavailable",
                                                                 .tool_call_id = "call_unavailable",
                                                                 .operation = ava::permissions::Operation::RunCommand,
                                                                 .mode = ava::agent::Mode::Build,
                                                                 .workspace_dir = workspace,
                                                                 .target_path = workspace,
                                                                 .command = "true",
                                                                 .tool_name = "bash",
                                                                 .reason = "critical command",
                                                                 .risk = ava::permissions::PermissionRisk::Critical,
                                                                 .command_metadata = unavailable_metadata});
  expect(unavailable && *unavailable == ava::permissions::PermissionResolution::Deny && fallback_calls == 1 && unavailable->resolution_source == "client",
         "Critical command metadata with Unavailable containment falls through to ACP client denial without persistent authority");

  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_session_mcp_requires_persistent_operator_authorization()
{
  using namespace ava::app::acp;
  auto const root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-mcp-operator-auth");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  configure_acp_tool_test_model(root);
  auto const paths = ava::tests::app_test_paths(root);
  auto const marker = root / "mcp-must-not-start";
  auto provider_state = std::make_shared<CapturingSequenceState>();

  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = paths;
  options.provider_bundle_factory = sequence_bundle_factory(provider_state, {acp_text_response("must not run")});
  AgentService service(options);
  service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  std::atomic_int permission_requests = 0;
  service.bind_client_request_sender(
      [&](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        if (method == "session/request_permission")
          ++permission_requests;
        std::promise<CallResult> promise;
        promise.set_value(std::string(R"({"outcome":{"outcome":"selected","optionId":"allow_always"}})"));
        return PendingCall{.id = "untrusted-mcp-allow", .completion = promise.get_future()};
      },
      [](JsonRpcId const&, std::string) { return true; });
  static_cast<void>(service.handle_request(initialize_request(), {}));

  auto const server_json =
      std::string("{\"name\":\"touch\",\"command\":\"/usr/bin/touch\",\"args\":[\"") + ava::core::json::escape(marker.string()) + "\"],\"env\":[]}";
  auto created = service.handle_request(Request{.id = std::int64_t(2),
                                                .method = "session/new",
                                                .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[" + server_json + "]}"},
                                        {});
  auto const id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  RequestResult prompted;
  if (id)
    prompted = service.handle_request(
        Request{.id = std::int64_t(3),
                .method = "session/prompt",
                .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"discover untrusted MCP\"}]}"},
        {});
  service.shutdown();

  std::size_t provider_requests = 0;
  {
    std::lock_guard lock(provider_state->mutex);
    provider_requests = provider_state->request_bodies.size();
  }
  auto const detail =
      std::string("ACP session MCP requires protected persistent operator authorization: result=") + (prompted ? *prompted : prompted.error().message);
  expect(id && !prompted && prompted.error().message.find("persistent operator authorization is required") != std::string::npos &&
             permission_requests.load() == 0 && provider_requests == 0 && !std::filesystem::exists(marker),
         detail);

  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_strict_session_mcp_registry_and_error_propagation()
{
  using namespace ava::app::acp;
  auto root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-strict-mcp");
  auto workspace = root / "workspace";
  auto nested = workspace / "nested";
  std::filesystem::create_directories(nested);
  configure_acp_tool_test_model(root);
  auto paths = ava::tests::app_test_paths(root);
  auto const cwd_marker = root / "mcp-cwd.txt";
  auto transport_state = std::make_shared<CapturingSequenceState>();
  std::vector<ava::http::HttpResponse> responses{
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"choices":[{"message":{"tool_calls":[{"id":"call_mcp","function":{"name":"mcp_demo_echo","arguments":"{\"text\":\"hello\"}"}}]},"finish_reason":"tool_calls"}]})"},
      acp_text_response("strict MCP complete")};

  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = paths;
  options.provider_bundle_factory = sequence_bundle_factory(transport_state, std::move(responses));
  AgentService service(options);
  service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  static_cast<void>(service.handle_request(initialize_request(), {}));
  auto const server_json = std::string("{\"name\":\"demo\",\"command\":\"") + ava::core::json::escape(AVA_FAKE_MCP_SERVER_PATH) +
                           "\",\"args\":[\"cwd-marker\",\"" + ava::core::json::escape(cwd_marker.string()) + "\"],\"env\":[]}";
  ava::permissions::PermissionRuleStore const rule_store{.global_rules_file = paths.ava_config_dir / "permission-rules.json",
                                                         .workspace_rules_file = workspace / ".ava" / "permission-rules.json",
                                                         .workspace_dir = workspace};
  auto install_mcp_allow = [&](ava::permissions::Operation operation, std::string command, std::string tool_name) {
    auto added = ava::permissions::add_persistent_permission_rule(
        rule_store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                          .action = ava::permissions::PermissionAction::Allow,
                                                          .operation = operation,
                                                          .mode = ava::permissions::PermissionRuleMode::Build,
                                                          .tool_name = std::move(tool_name),
                                                          .target_path = {},
                                                          .command = std::move(command),
                                                          .command_recipe_key = {},
                                                          .recipe_display = {},
                                                          .critical_acknowledged = false,
                                                          .reason = "authorize exact ACP session MCP operation",
                                                          .actor = "test_operator"});
    expect(added.has_value(), added ? "protected persistent ACP MCP rule installed" : "protected persistent ACP MCP rule installed: " + added.error().format());
    return added.has_value();
  };
  ava::mcp::McpServerConfig const demo_server{.id = "demo",
                                              .name = "demo",
                                              .command = AVA_FAKE_MCP_SERVER_PATH,
                                              .args = {"cwd-marker", cwd_marker.string()},
                                              .env = {},
                                              .enabled = true,
                                              .scope = ava::mcp::McpServerScope::Project,
                                              .source_path = {}};
  auto const demo_launch_identity = ava::mcp::session_mcp_launch_identity(demo_server, ava::core::normalized_absolute_path(nested));
  auto const demo_tool_name = ava::mcp::mcp_model_tool_name("demo", "echo");
  bool valid_rules = install_mcp_allow(ava::permissions::Operation::McpServerLaunch, demo_launch_identity, "mcp_discovery");
  valid_rules = install_mcp_allow(ava::permissions::Operation::McpServerConnect, "demo", "mcp_discovery") && valid_rules;
  valid_rules = install_mcp_allow(ava::permissions::Operation::McpServerLaunch, demo_launch_identity, demo_tool_name) && valid_rules;
  valid_rules = install_mcp_allow(ava::permissions::Operation::McpServerConnect, "demo", demo_tool_name) && valid_rules;
  valid_rules = install_mcp_allow(ava::permissions::Operation::McpToolCall, "demo:echo", demo_tool_name) && valid_rules;
  expect(valid_rules, "ACP strict MCP fixture has exact protected launch, connect, and tool-call operator Allows");

  auto created = service.handle_request(Request{.id = std::int64_t(2),
                                                .method = "session/new",
                                                .params_json = std::string("{\"cwd\":\"") + nested.string() + "\",\"mcpServers\":[" + server_json + "]}"},
                                        {});
  auto id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  expect(id.has_value(), "ACP strict MCP fixture creates a nested-cwd session");
  if (id)
  {
    auto prompted =
        service.handle_request(Request{.id = std::int64_t(3),
                                       .method = "session/prompt",
                                       .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"use MCP\"}]}"},
                               {});
    std::vector<std::string> bodies;
    {
      std::lock_guard lock(transport_state->mutex);
      bodies = transport_state->request_bodies;
    }
    auto const valid_detail = std::string("ACP registry composes approved built-ins and valid session MCP from persisted cwd: result=") +
                              (prompted ? *prompted : prompted.error().message) + " bodies=" + std::to_string(bodies.size()) +
                              " marker=" + read_acp_test_file(cwd_marker) + (bodies.empty() ? std::string{} : " first=" + bodies.front().substr(0, 256));
    expect(prompted && *prompted == R"({"stopReason":"end_turn"})" && bodies.size() == 2 &&
               bodies.front().find("\"name\":\"mcp_demo_echo\"") != std::string::npos && bodies.front().find("\"name\":\"read_file\"") != std::string::npos &&
               bodies.front().find("\"name\":\"question\"") == std::string::npos && bodies.front().find("\"name\":\"task\"") == std::string::npos &&
               bodies.front().find("\"name\":\"bash\"") == std::string::npos && bodies.front().find("\"name\":\"webfetch\"") == std::string::npos &&
               bodies.front().find("\"name\":\"lsp_") == std::string::npos && bodies.front().find("\"name\":\"plugin_") == std::string::npos &&
               bodies.back().find("MCP call ok") != std::string::npos && read_acp_test_file(cwd_marker) == ava::core::normalized_absolute_path(nested).string(),
           valid_detail);
  }
  service.shutdown();

  auto const bash_marker = nested / "bash-must-not-run";
  auto bash_state = std::make_shared<CapturingSequenceState>();
  auto const bash_arguments = std::string("{\"command\":\"printf exposed > ") + ava::core::json::escape(bash_marker.string()) + "\"}";
  std::vector<ava::http::HttpResponse> bash_responses{
      ava::http::HttpResponse{.status_code = 200,
                              .headers = {},
                              .body = std::string(R"({"choices":[{"message":{"tool_calls":[{"id":"call_bash","function":{"name":"bash","arguments":")") +
                                      ava::core::json::escape(bash_arguments) + R"("}}]},"finish_reason":"tool_calls"}]})"},
      acp_text_response("bash stayed unavailable")};
  auto bash_options = options;
  bash_options.provider_bundle_factory = sequence_bundle_factory(bash_state, std::move(bash_responses));
  AgentService bash_service(std::move(bash_options));
  bash_service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  static_cast<void>(bash_service.handle_request(initialize_request(), {}));
  auto bash_created = bash_service.handle_request(
      Request{.id = std::int64_t(20), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + nested.string() + "\",\"mcpServers\":[]}"}, {});
  auto bash_id = bash_created ? ava::core::json::string_field(*bash_created, "sessionId") : std::nullopt;
  RequestResult bash_prompt;
  if (bash_id)
    bash_prompt = bash_service.handle_request(
        Request{.id = std::int64_t(21),
                .method = "session/prompt",
                .params_json = std::string("{\"sessionId\":\"") + *bash_id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"attempt bash\"}]}"},
        {});
  std::vector<std::string> bash_bodies;
  {
    std::lock_guard lock(bash_state->mutex);
    bash_bodies = bash_state->request_bodies;
  }
  expect(bash_id && bash_prompt && bash_bodies.size() == 2 && bash_bodies.front().find("\"name\":\"bash\"") == std::string::npos &&
             bash_bodies.back().find("unknown tool") != std::string::npos && !std::filesystem::exists(bash_marker),
         "ACP exact M4 registry neither exposes nor dispatches bash when a model attempts an unadvertised call");
  bash_service.shutdown();

  auto run_error_case = [&](std::string mcp_servers, std::string_view expected,
                            std::vector<std::pair<ava::permissions::Operation, std::string>> const& authorizations) {
    bool rules_installed = true;
    for (auto const& [operation, command] : authorizations) rules_installed = install_mcp_allow(operation, command, "mcp_discovery") && rules_installed;
    expect(rules_installed, "ACP strict MCP error fixture has exact protected launch/connect operator Allows");

    auto error_state = std::make_shared<CapturingSequenceState>();
    AgentServiceOptions error_options;
    error_options.agent_version = "1";
    error_options.launch_root = ava::core::normalized_absolute_path(workspace);
    error_options.paths = paths;
    error_options.provider_bundle_factory = sequence_bundle_factory(error_state, {acp_text_response()});
    AgentService error_service(error_options);
    error_service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
    static_cast<void>(error_service.handle_request(initialize_request(), {}));
    auto error_created =
        error_service.handle_request(Request{.id = std::int64_t(10),
                                             .method = "session/new",
                                             .params_json = std::string("{\"cwd\":\"") + nested.string() + "\",\"mcpServers\":" + mcp_servers + "}"},
                                     {});
    auto error_id = error_created ? ava::core::json::string_field(*error_created, "sessionId") : std::nullopt;
    RequestResult error_result;
    if (error_id)
      error_result = error_service.handle_request(
          Request{.id = std::int64_t(11),
                  .method = "session/prompt",
                  .params_json = std::string("{\"sessionId\":\"") + *error_id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"fail discovery\"}]}"},
          {});
    std::size_t provider_requests = 0;
    {
      std::lock_guard lock(error_state->mutex);
      provider_requests = error_state->request_bodies.size();
    }
    auto const error_detail = std::string("ACP prompt propagates strict MCP registry error: expected=") + std::string(expected) +
                              " actual=" + (error_result ? *error_result : error_result.error().message);
    expect(error_id && !error_result && error_result.error().message.find(expected) != std::string::npos && provider_requests == 0, error_detail);
    error_service.shutdown();
  };

  auto const fake_command = ava::core::json::escape(AVA_FAKE_MCP_SERVER_PATH);
  ava::mcp::McpServerConfig const collision_server{.id = "demo-one",
                                                   .name = "demo-one",
                                                   .command = AVA_FAKE_MCP_SERVER_PATH,
                                                   .args = {},
                                                   .env = {},
                                                   .enabled = true,
                                                   .scope = ava::mcp::McpServerScope::Project,
                                                   .source_path = {}};
  auto const collision_launch_identity = ava::mcp::session_mcp_launch_identity(collision_server, ava::core::normalized_absolute_path(nested));
  run_error_case("[{\"name\":\"demo-one\",\"command\":\"" + fake_command + "\",\"args\":[],\"env\":[]},{\"name\":\"demo_one\",\"command\":\"" + fake_command +
                     "\",\"args\":[],\"env\":[]}]",
                 "duplicate model tool name",
                 {{ava::permissions::Operation::McpServerLaunch, collision_launch_identity},
                  {ava::permissions::Operation::McpServerConnect, "demo-one"},
                  {ava::permissions::Operation::McpServerConnect, "demo_one"}});
  auto const missing_command = (root / "missing-server").string();
  auto missing_server = collision_server;
  missing_server.id = "missing";
  missing_server.name = "missing";
  missing_server.command = missing_command;
  auto const missing_launch_identity = ava::mcp::session_mcp_launch_identity(missing_server, ava::core::normalized_absolute_path(nested));
  run_error_case("[{\"name\":\"missing\",\"command\":\"" + ava::core::json::escape(missing_command) + "\",\"args\":[],\"env\":[]}]",
                 "MCP server closed before response",
                 {{ava::permissions::Operation::McpServerLaunch, missing_launch_identity}, {ava::permissions::Operation::McpServerConnect, "missing"}});

  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_negotiated_client_filesystem_and_terminal_routing()
{
  using namespace ava::app::acp;
  auto const root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-client-tools");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "ACP terminal fixture workspace is owner-only for sealed command planning");
  configure_acp_tool_test_model(root);
  auto const paths = ava::tests::app_test_paths(root);
  auto const note = workspace / "note.txt";
  {
    std::ofstream file(note, std::ios::binary | std::ios::trunc);
    file << "local bytes";
  }

  auto ready = [](std::string id, CallResult result) {
    std::promise<CallResult> promise;
    promise.set_value(std::move(result));
    return PendingCall{.id = std::move(id), .completion = promise.get_future()};
  };
  auto const read_tool_response = ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = std::string(
          R"({"choices":[{"message":{"tool_calls":[{"id":"call_read","function":{"name":"read_file","arguments":"{\"path\":\"note.txt\"}"}}]},"finish_reason":"tool_calls"}]})")};

  auto run_read_case = [&](std::string capabilities, bool expect_remote_read, bool coherent_pair, bool fail_remote = false) {
    {
      std::ofstream file(note, std::ios::binary | std::ios::trunc);
      file << "local bytes";
    }
    auto provider_state = std::make_shared<CapturingSequenceState>();
    AgentServiceOptions options;
    options.agent_version = "1";
    options.launch_root = ava::core::normalized_absolute_path(workspace);
    options.paths = paths;
    options.provider_bundle_factory = sequence_bundle_factory(provider_state, {read_tool_response, acp_text_response("file complete")});
    AgentService service(options);
    service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
    std::vector<std::string> methods;
    service.bind_client_request_sender(
        [&](std::string method, std::optional<std::string> params, std::chrono::milliseconds, OutboundCallPolicy policy) -> ava::core::Result<PendingCall> {
          methods.push_back(method);
          if (method == "session/request_permission")
            return ready("permission", CallResult(std::string(R"({"outcome":{"outcome":"selected","optionId":"allow_once"}})")));
          expect(method == "fs/read_text_file" && params && params->find(ava::core::normalized_absolute_path(note).string()) != std::string::npos &&
                     policy == OutboundCallPolicy::Normal,
                 "negotiated ACP read uses the canonical absolute path DTO with normal delivery policy");
          if (fail_remote)
            return ready("read-failed", std::unexpected(JsonRpcError{.code = -32603,
                                                                     .message = "injected remote read failure",
                                                                     .data_json = std::nullopt,
                                                                     .id = std::nullopt,
                                                                     .intent = EnvelopeIntent::Response,
                                                                     .suppress_response = true}));
          return ready("read", CallResult(std::string(R"({"content":"remote bytes","future":true})")));
        },
        [](JsonRpcId const&, std::string) { return true; });
    auto initialized = service.handle_request(initialize_request_with_capabilities(std::move(capabilities)), {});
    auto created = service.handle_request(
        Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
    auto session_id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
    RequestResult prompted;
    if (session_id)
      prompted = service.handle_request(
          Request{.id = std::int64_t(3),
                  .method = "session/prompt",
                  .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"read\"}]}"},
          {});
    std::vector<std::string> bodies;
    {
      std::lock_guard lock(provider_state->mutex);
      bodies = provider_state->request_bodies;
    }
    auto const expected_content = expect_remote_read && !fail_remote ? "remote bytes" : "local bytes";
    auto const visibility_matches = bodies.size() == 2 && (bodies.front().find("\"name\":\"edit_file\"") != std::string::npos) == coherent_pair &&
                                    (bodies.front().find("\"name\":\"apply_patch\"") != std::string::npos) == coherent_pair;
    auto const content_matches = bodies.size() == 2 && (fail_remote ? bodies.back().find("injected remote read failure") != std::string::npos &&
                                                                          bodies.back().find("local bytes") == std::string::npos
                                                                    : bodies.back().find(expected_content) != std::string::npos);
    expect(initialized && session_id && prompted && visibility_matches && content_matches &&
               std::count(methods.begin(), methods.end(), "fs/read_text_file") == (expect_remote_read ? 1 : 0),
           fail_remote ? "a negotiated read failure never falls back to descriptor-secure local bytes"
                       : "ACP routes read_file independently and hides edit/patch only for partial filesystem snapshots");
    service.shutdown();
  };

  run_read_case(R"({"fs":{"readTextFile":true,"writeTextFile":true}})", true, true);
  run_read_case(R"({"fs":{"readTextFile":true,"writeTextFile":false}})", true, false);
  run_read_case(R"({"fs":{"readTextFile":false,"writeTextFile":true}})", false, false);
  run_read_case(R"({"fs":{"readTextFile":false,"writeTextFile":false}})", false, true);
  run_read_case(R"({"fs":{"readTextFile":true,"writeTextFile":false}})", true, false, true);

  auto const write_tool_response = ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = std::string(
          R"({"choices":[{"message":{"tool_calls":[{"id":"call_write","function":{"name":"write_file","arguments":"{\"path\":\"note.txt\",\"content\":\"updated bytes\"}"}}]},"finish_reason":"tool_calls"}]})")};
  auto run_write_case = [&](std::string capabilities, bool expect_remote_write, bool fail_remote = false) {
    {
      std::ofstream file(note, std::ios::binary | std::ios::trunc);
      file << "local bytes";
    }
    auto provider_state = std::make_shared<CapturingSequenceState>();
    AgentServiceOptions options;
    options.agent_version = "1";
    options.launch_root = ava::core::normalized_absolute_path(workspace);
    options.paths = paths;
    options.provider_bundle_factory = sequence_bundle_factory(provider_state, {write_tool_response, acp_text_response("write complete")});
    AgentService service(options);
    service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
    std::vector<std::string> methods;
    service.bind_client_request_sender(
        [&](std::string method, std::optional<std::string> params, std::chrono::milliseconds, OutboundCallPolicy policy) -> ava::core::Result<PendingCall> {
          methods.push_back(method);
          if (method == "session/request_permission")
            return ready("write-permission", CallResult(std::string(R"({"outcome":{"outcome":"selected","optionId":"allow_once"}})")));
          expect(method == "fs/write_text_file" && params && params->find("updated bytes") != std::string::npos &&
                     policy == OutboundCallPolicy::AbortConnectionIfDelivered,
                 "negotiated ACP write uses the exact client write DTO with fail-stop delivery policy");
          if (fail_remote)
            return ready("write-failed", std::unexpected(JsonRpcError{.code = -32603,
                                                                      .message = "injected remote write failure",
                                                                      .data_json = std::nullopt,
                                                                      .id = std::nullopt,
                                                                      .intent = EnvelopeIntent::Response,
                                                                      .suppress_response = true}));
          return ready("write", CallResult(std::string(R"({"_meta":[]})")));
        },
        [](JsonRpcId const&, std::string) { return true; });
    static_cast<void>(service.handle_request(initialize_request_with_capabilities(std::move(capabilities)), {}));
    auto created = service.handle_request(
        Request{.id = std::int64_t(12), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
    auto session_id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
    RequestResult prompted;
    if (session_id)
      prompted = service.handle_request(
          Request{.id = std::int64_t(13),
                  .method = "session/prompt",
                  .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"write\"}]}"},
          {});
    std::string local_content;
    {
      std::ifstream file(note, std::ios::binary);
      local_content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    expect(session_id && prompted && std::count(methods.begin(), methods.end(), "fs/write_text_file") == (expect_remote_write ? 1 : 0) &&
               local_content == (expect_remote_write ? "local bytes" : "updated bytes"),
           fail_remote ? "a negotiated write failure never falls back to a local mutation"
                       : "ACP routes write_file independently while unsupported writes remain descriptor-secure local");
    service.shutdown();
  };

  run_write_case(R"({"fs":{"readTextFile":true,"writeTextFile":false}})", false);
  run_write_case(R"({"fs":{"readTextFile":false,"writeTextFile":true}})", true);
  run_write_case(R"({"fs":{"readTextFile":false,"writeTextFile":true}})", true, true);

  auto terminal_state = std::make_shared<CapturingSequenceState>();
  auto const marker = workspace / "terminal-must-not-run-locally";
  auto const bash_args = std::string("{\"command\":\"touch ") + marker.string() + "\"}";
  auto const bash_tool_response =
      ava::http::HttpResponse{.status_code = 200,
                              .headers = {},
                              .body = std::string(R"({"choices":[{"message":{"tool_calls":[{"id":"call_terminal","function":{"name":"bash","arguments":")") +
                                      ava::core::json::escape(bash_args) + R"("}}]},"finish_reason":"tool_calls"}]})"};
  AgentServiceOptions terminal_options;
  terminal_options.agent_version = "1";
  terminal_options.launch_root = ava::core::normalized_absolute_path(workspace);
  terminal_options.paths = paths;
  terminal_options.provider_bundle_factory = sequence_bundle_factory(terminal_state, {bash_tool_response, acp_text_response("terminal complete")});
  AgentService terminal_service(terminal_options);
  terminal_service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  std::vector<std::string> terminal_methods;
  terminal_service.bind_client_request_sender(
      [&](std::string method, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        terminal_methods.push_back(method);
        if (method == "session/request_permission")
          return ready("terminal-permission", CallResult(std::string(R"({"outcome":{"outcome":"selected","optionId":"allow_once"}})")));
        if (method == "terminal/create")
          return ready("terminal-create", CallResult(std::string(R"({"terminalId":"negotiated-terminal"})")));
        if (method == "terminal/wait_for_exit")
          return ready("terminal-wait", CallResult(std::string(R"({"exitCode":4294967295,"signal":null})")));
        if (method == "terminal/output")
          return ready("terminal-output", CallResult(std::string(R"({"output":"remote terminal output","truncated":false})")));
        return ready("terminal-release", CallResult(std::string("{}")));
      },
      [](JsonRpcId const&, std::string) { return true; });
  static_cast<void>(terminal_service.handle_request(initialize_request_with_capabilities(R"({"terminal":true})"), {}));
  auto terminal_created = terminal_service.handle_request(
      Request{.id = std::int64_t(10), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto terminal_session = terminal_created ? ava::core::json::string_field(*terminal_created, "sessionId") : std::nullopt;
  RequestResult terminal_prompt;
  if (terminal_session)
    terminal_prompt = terminal_service.handle_request(
        Request{.id = std::int64_t(11),
                .method = "session/prompt",
                .params_json = std::string("{\"sessionId\":\"") + *terminal_session + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"run\"}]}"},
        {});
  std::vector<std::string> terminal_bodies;
  {
    std::lock_guard lock(terminal_state->mutex);
    terminal_bodies = terminal_state->request_bodies;
  }
  expect(terminal_session && terminal_prompt && terminal_bodies.size() == 2 && terminal_bodies.front().find("\"name\":\"bash\"") != std::string::npos &&
             terminal_bodies.back().find("remote terminal output") != std::string::npos && terminal_bodies.back().find("4294967295") != std::string::npos &&
             terminal_methods ==
                 std::vector<std::string>({"session/request_permission", "terminal/create", "terminal/wait_for_exit", "terminal/output", "terminal/release"}) &&
             !std::filesystem::exists(marker),
         "terminal negotiation exposes bash and routes exact create-wait-output-release without local process fallback");
  terminal_service.shutdown();

  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_builtin_permission_gateway_one_shot_mutations_and_updates()
{
  using namespace ava::app::acp;
  auto root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-permission-tools");
  auto workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  configure_acp_tool_test_model(root);
  auto paths = ava::tests::app_test_paths(root);
  auto state = std::make_shared<CapturingSequenceState>();
  auto const arguments = R"({\"path\":\"approved.txt\",\"content\":\"approved content\"})";
  auto tool_response = ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = std::string(R"({"choices":[{"message":{"tool_calls":[{"id":"call_write_1","function":{"name":"write_file","arguments":")") + arguments +
              R"("}},{"id":"call_write_2","function":{"name":"write_file","arguments":")" + arguments + R"("}}]},"finish_reason":"tool_calls"}]})"};

  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = paths;
  options.provider_bundle_factory = sequence_bundle_factory(state, {std::move(tool_response), acp_text_response("permission complete")});
  AgentService service(options);
  std::mutex sequence_mutex;
  std::vector<std::string> sequence;
  std::vector<std::string> permission_params;
  service.bind_update_sender([&](std::string_view, std::string_view update) -> ava::core::VoidResult {
    std::lock_guard lock(sequence_mutex);
    sequence.push_back(std::string("update:") + std::string(update));
    return {};
  });
  std::atomic_int request_sequence = 0;
  service.bind_client_request_sender(
      [&](std::string method, std::optional<std::string> params, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        expect(method == "session/request_permission" && params.has_value(), "ACP built-in tool asks through the exact client permission method");
        {
          std::lock_guard lock(sequence_mutex);
          sequence.push_back("permission");
          permission_params.push_back(params.value_or(""));
        }
        std::promise<CallResult> promise;
        promise.set_value(std::string(R"({"outcome":{"outcome":"selected","optionId":"allow_once"}})"));
        return PendingCall{.id = std::string("permission-") + std::to_string(++request_sequence), .completion = promise.get_future()};
      },
      [](JsonRpcId const&, std::string) { return true; });
  static_cast<void>(service.handle_request(initialize_request(), {}));
  auto created = service.handle_request(
      Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  RequestResult prompted;
  if (id)
    prompted =
        service.handle_request(Request{.id = std::int64_t(3),
                                       .method = "session/prompt",
                                       .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"write twice\"}]}"},
                               {});
  service.shutdown();

  std::vector<std::string> observed;
  std::vector<std::string> requests;
  {
    std::lock_guard lock(sequence_mutex);
    observed = sequence;
    requests = permission_params;
  }
  auto const first_tool =
      std::ranges::find_if(observed, [](std::string const& item) { return item.find(R"("sessionUpdate":"tool_call")") != std::string::npos; });
  auto const permission = std::ranges::find(observed, "permission");
  auto const execution_start = std::ranges::find_if(observed, [](std::string const& item) {
    return item.find(R"("toolCallId":"call_write_1")") != std::string::npos && item.find(R"("status":"in_progress")") != std::string::npos;
  });
  auto const tool_result = std::ranges::find_if(observed, [](std::string const& item) { return item.find(R"("status":"completed")") != std::string::npos; });
  auto const agent_text = std::ranges::find_if(observed, [](std::string const& item) { return item.find("agent_message_chunk") != std::string::npos; });
  expect(id && prompted && *prompted == R"({"stopReason":"end_turn"})" && request_sequence.load() == 2 && requests.size() == 2 &&
             requests.front().find(R"("toolCallId":"call_write_1")") != std::string::npos &&
             requests.back().find(R"("toolCallId":"call_write_2")") != std::string::npos &&
             requests.front().find(R"("status":"pending")") != std::string::npos && requests.front().find("approved content") != std::string::npos &&
             requests.front().find(R"("optionId":"allow_always")") == std::string::npos && first_tool != observed.end() && permission != observed.end() &&
             execution_start != observed.end() && first_tool->find(R"("status":"pending")") != std::string::npos &&
             std::filesystem::exists(workspace / "approved.txt") && read_acp_test_file(workspace / "approved.txt") == "approved content" &&
             first_tool < permission && permission < execution_start && execution_start < tool_result && tool_result < agent_text,
         "ACP built-in writes show each bounded mutation and order pending, one-shot permission, in-progress, and completion exactly");

  if (id)
  {
    auto store = ava::session::SessionStore::open(workspace, *id, paths.sessions_dir);
    auto entries = store ? store->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(store.error()));
    bool audited = false;
    if (entries)
      for (auto const& entry : *entries)
        if (entry.type == ava::session::EntryType::PermissionDecision && ava::core::json::string_field(entry.data_json, "resolution_source") == "client")
          audited = true;
    expect(audited, "ACP one-shot file permission decisions persist the generic client source and permission/tool identity in the normal session audit stream");
  }
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_session_grant_cannot_follow_retargeted_parent_symlink()
{
  using namespace ava::app::acp;
  auto const root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-grant-retarget");
  auto const workspace = root / "workspace";
  auto const outside = root / "outside";
  std::filesystem::create_directories(workspace / "grant-parent");
  std::filesystem::create_directories(outside);
  configure_acp_tool_test_model(root);
  auto const paths = ava::tests::app_test_paths(root);
  auto const first_args = R"({\"path\":\"grant-parent/target.txt\",\"content\":\"inside approved\"})";
  auto const second_args = R"({\"path\":\"grant-parent/target.txt\",\"content\":\"outside denied\"})";
  auto tool_response = [](std::string_view id, std::string_view arguments) {
    return ava::http::HttpResponse{.status_code = 200,
                                   .headers = {},
                                   .body = std::string(R"({"choices":[{"message":{"tool_calls":[{"id":")") + std::string(id) +
                                           R"(","function":{"name":"write_file","arguments":")" + std::string(arguments) +
                                           R"("}}]},"finish_reason":"tool_calls"}]})"};
  };
  auto provider_state = std::make_shared<CapturingSequenceState>();
  std::atomic_int bundle_number = 0;
  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = paths;
  options.provider_bundle_factory = [&, provider_state](ava::app::runtime::session_ts const& unlocked_session, ava::app::runtime::RunOptions run_options,
                                                        std::string_view label) -> ava::core::Result<ava::app::RuntimeProviderRunBundle> {
    auto const current = bundle_number.fetch_add(1);
    auto responses = current == 0 ? std::vector<ava::http::HttpResponse>{tool_response("grant_first", first_args), acp_text_response("first complete")}
                                  : std::vector<ava::http::HttpResponse>{tool_response("grant_second", second_args), acp_text_response("second complete")};
    auto factory = sequence_bundle_factory(provider_state, std::move(responses));
    return factory(unlocked_session, std::move(run_options), label);
  };
  AgentService service(options);
  service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  std::atomic_int permission_requests = 0;
  service.bind_client_request_sender(
      [&](std::string, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
        ++permission_requests;
        std::promise<CallResult> promise;
        promise.set_value(std::string(R"({"outcome":{"outcome":"selected","optionId":"allow_once"}})"));
        return PendingCall{.id = std::string("grant-permission-") + std::to_string(permission_requests.load()), .completion = promise.get_future()};
      },
      [](JsonRpcId const&, std::string) { return true; });
  static_cast<void>(service.handle_request(initialize_request(), {}));
  auto created = service.handle_request(
      Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto session_id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  RequestResult first;
  if (session_id)
    first = service.handle_request(
        Request{.id = std::int64_t(3),
                .method = "session/prompt",
                .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"first\"}]}"},
        {});
  std::filesystem::rename(workspace / "grant-parent", workspace / "grant-parent-original");
  std::error_code link_error;
  std::filesystem::create_directory_symlink(outside, workspace / "grant-parent", link_error);
  RequestResult second;
  if (session_id && !link_error)
    second = service.handle_request(
        Request{.id = std::int64_t(4),
                .method = "session/prompt",
                .params_json = std::string("{\"sessionId\":\"") + *session_id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"second\"}]}"},
        {});
  expect(session_id && first && second && permission_requests.load() == 1 && !std::filesystem::exists(outside / "target.txt") &&
             read_acp_test_file(workspace / "grant-parent-original" / "target.txt") == "inside approved",
         "ACP one-shot mutation approvals cannot authorize a later path retargeted through a symlink");
  service.shutdown();
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_acp_permission_once_always_reject_cancel_invalid_and_hard_policy_matrix()
{
  using namespace ava::app::acp;

  auto run_case = [](std::string_view option_id, bool cancelled, bool hard_deny, int expected_requests, bool expect_file, bool expect_cancelled) {
    auto root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-permission-matrix");
    auto workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    configure_acp_tool_test_model(root);
    auto paths = ava::tests::app_test_paths(root);
    auto provider_state = std::make_shared<CapturingSequenceState>();
    auto const target = hard_deny ? std::string(".env") : std::string("matrix.txt");
    auto const args = std::string("{\\\"path\\\":\\\"") + target + "\\\",\\\"content\\\":\\\"matrix-secret\\\"}";
    auto const tool_body = std::string(R"({"choices":[{"message":{"tool_calls":[{"id":"matrix_1","function":{"name":"write_file","arguments":")") + args +
                           R"("}},{"id":"matrix_2","function":{"name":"write_file","arguments":")" + args + R"("}}]},"finish_reason":"tool_calls"}]})";
    AgentServiceOptions options;
    options.agent_version = "1";
    options.launch_root = ava::core::normalized_absolute_path(workspace);
    options.paths = paths;
    options.provider_bundle_factory = sequence_bundle_factory(
        provider_state, {ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = tool_body}, acp_text_response("matrix complete")});
    AgentService service(options);
    std::atomic_bool execution_started = false;
    service.bind_update_sender([&](std::string_view, std::string_view update) -> ava::core::VoidResult {
      if (update.find(R"("status":"in_progress")") != std::string_view::npos)
        execution_started.store(true, std::memory_order_release);
      return {};
    });
    std::atomic_int requests = 0;
    service.bind_client_request_sender(
        [&](std::string, std::optional<std::string>, std::chrono::milliseconds, OutboundCallPolicy) -> ava::core::Result<PendingCall> {
          ++requests;
          std::promise<CallResult> promise;
          if (cancelled)
            promise.set_value(std::string(R"({"outcome":{"outcome":"cancelled"}})"));
          else
            promise.set_value(std::string("{\"outcome\":{\"outcome\":\"selected\",\"optionId\":\"") + std::string(option_id) + "\"}}");
          return PendingCall{.id = std::string("matrix-permission-") + std::to_string(requests.load()), .completion = promise.get_future()};
        },
        [](JsonRpcId const&, std::string) { return true; });
    static_cast<void>(service.handle_request(initialize_request(), {}));
    auto created = service.handle_request(
        Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
    auto id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
    RequestResult result;
    if (id)
      result = service.handle_request(
          Request{.id = std::int64_t(3),
                  .method = "session/prompt",
                  .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"permission matrix\"}]}"},
          {});
    service.shutdown();
    bool const canceled_result = result && *result == R"({"stopReason":"cancelled"})";
    bool const completed_result = result && *result == R"({"stopReason":"end_turn"})";
    expect(id && requests.load() == expected_requests && std::filesystem::exists(workspace / target) == expect_file &&
               execution_started.load(std::memory_order_acquire) == expect_file && (expect_cancelled ? canceled_result : completed_result),
           "ACP permission matrix never reports execution started for denied, canceled, invalid, or hard-denied calls");
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
  };

  run_case("allow_once", false, false, 2, true, false);
  run_case("reject_once", false, false, 2, false, false);
  run_case("reject_always", false, false, 2, false, false);
  run_case("allow_once", true, false, 1, false, true);
  run_case("not_offered", false, false, 2, false, false);
  run_case("allow_always", false, true, 0, false, false);
}

void test_acp_close_timeout_is_internal_error_with_eventual_cleanup()
{
  using namespace ava::app::acp;
  auto root = std::filesystem::temp_directory_path() / ava::core::make_id("acp-close-timeout");
  auto workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  configure_acp_test_model(root);
  std::string body;
  std::atomic_bool entered = false;
  std::atomic_bool release = false;
  auto coordinator = ava::agent::SubagentCoordinator::create();
  expect(coordinator.has_value(), "ACP close timeout creates coordinator");
  if (!coordinator)
    return;
  auto manager = ava::app::SubagentDeliveryManager::create({.coordinator = *coordinator, .max_retained_parents = 1});
  expect(manager.has_value(), "ACP close timeout uses a one-parent manager");
  if (!manager)
    return;
  AgentServiceOptions options;
  options.agent_version = "1";
  options.launch_root = ava::core::normalized_absolute_path(workspace);
  options.paths = ava::tests::app_test_paths(root);
  options.provider_bundle_factory = recording_bundle_factory(&body, &entered, &release);
  options.open_context.subagent_delivery_manager = *manager;
  options.close_grace = 5ms;
  AgentService service(options);
  service.bind_update_sender([](std::string_view, std::string_view) -> ava::core::VoidResult { return {}; });
  static_cast<void>(service.handle_request(initialize_request(), {}));
  auto created = service.handle_request(
      Request{.id = std::int64_t(2), .method = "session/new", .params_json = std::string("{\"cwd\":\"") + workspace.string() + "\",\"mcpServers\":[]}"}, {});
  auto id = created ? ava::core::json::string_field(*created, "sessionId") : std::nullopt;
  RequestResult prompt_result;
  ava::core::JoinThread prompt_thread;
  if (id)
  {
    prompt_thread = ava::core::JoinThread::create("prompt_thread", [&] {
      prompt_result =
          service.handle_request(Request{.id = std::int64_t(3),
                                         .method = "session/prompt",
                                         .params_json = std::string("{\"sessionId\":\"") + *id + "\",\"prompt\":[{\"type\":\"text\",\"text\":\"block\"}]}"},
                                 {});
    });
    while (!entered.load(std::memory_order_acquire))
      std::this_thread::sleep_for(1ms);
    auto closed =
        service.handle_request(Request{.id = std::int64_t(4), .method = "session/close", .params_json = std::string("{\"sessionId\":\"") + *id + "\"}"}, {});
    expect(!closed && closed.error().code == -32603, "session/close reports internal stop timeout as -32603, not resource-not-found");
    release.store(true, std::memory_order_release);
    prompt_thread.join();
    bool retained_found = true;
    auto retained = (*manager)->retained_session(*id, workspace, retained_found, {.exact_session_id = true});
    expect(!retained_found && !retained, "end-run cleanup releases the timed-out host's unused capsule after cancellation completes");
    auto absent =
        service.handle_request(Request{.id = std::int64_t(5), .method = "session/close", .params_json = std::string("{\"sessionId\":\"") + *id + "\"}"}, {});
    expect(!absent && absent.error().code == -32002, "timed-out close removes registry ownership and eventually releases host resources");
  }
  service.shutdown();
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}
