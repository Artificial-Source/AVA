#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/golden.h"
#include "tests/support/process_group_test_support.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/app/headless_policy.h"
#include "ava/agent/tool_dispatcher.h"
#include "ava/tools/file_tools.h"
#include "ava/plugin/enablement.h"
#include "ava/plugin/tool_broker.h"
#include "ava/mcp/config.h"
#include "ava/mcp/protocol.h"
#include "ava/mcp/stdio_client.h"
#include "ava/mcp/tool_broker.h"
#include "ava/permissions/permission.h"
#include "ava/permissions/permission_rules.h"
#include "ava/core/json.h"
#include "ava/core/mode.h"
#include "ava/core/path.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include <sys/types.h>

#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace {

using McpDescriptorExecutor = decltype(ava::mcp::McpBrokeredTool::executor);
static_assert(
    std::is_invocable_r_v<ava::tools::ToolDispatchResult, McpDescriptorExecutor, ava::tools::ToolContext const&, ava::tools::ProviderToolCall const&>);
static_assert(
    !std::is_invocable_v<McpDescriptorExecutor, ava::tools::ToolContext const&, ava::agent::ToolDispatchServices const&, ava::tools::ProviderToolCall const&>);

void write_text(std::filesystem::path const& path, std::string const& text)
{
  std::filesystem::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << text;
}

std::string read_text(std::filesystem::path const& path)
{
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  auto value = buffer.str();
  if (!value.empty() && value.back() == '\n')
    value.pop_back();
  return value;
}

std::optional<pid_t> read_pid_file_for_test(std::filesystem::path const& path)
{
  std::ifstream file(path, std::ios::binary);
  long long value = 0;
  file >> value;
  if (!file || value <= 0)
    return std::nullopt;
  return static_cast<pid_t>(value);
}

bool wait_for_process_group_exit(pid_t pgid)
{
  auto const deadline = ava::tests::now_plus_seconds(5);
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (!ava::test::process_group_has_live_member(pgid))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return !ava::test::process_group_has_live_member(pgid);
}

std::string mcp_config_json(std::string id, std::string command, bool enabled = true, std::string args_json = "[]")
{
  return std::string("{\"servers\":[{\"id\":\"") + ava::core::json::escape(id) + "\",\"name\":\"Demo MCP\",\"command\":\"" + ava::core::json::escape(command) +
         "\",\"args\":" + args_json + ",\"enabled\":" + (enabled ? "true" : "false") + "}]}";
}

ava::mcp::McpServerConfig fake_server_config(std::filesystem::path const& root)
{
  return ava::mcp::McpServerConfig{.id = "demo",
                                   .name = "Demo MCP",
                                   .command = AVA_FAKE_MCP_SERVER_PATH,
                                   .args = {},
                                   .env = {},
                                   .enabled = true,
                                   .scope = ava::mcp::McpServerScope::Project,
                                   .source_path = root / "mcp.json"};
}

ava::mcp::McpStdioClientOptions fake_client_options(std::filesystem::path const& workspace)
{
  ava::mcp::McpStdioClientOptions options;
  options.workspace_dir = workspace;
  options.startup_timeout = std::chrono::seconds(5);
  options.request_timeout = std::chrono::seconds(5);
  return options;
}

std::string nested_arrays_json(std::size_t depth)
{
  std::string text;
  text.reserve(depth * 2);
  for (std::size_t index = 0; index < depth; ++index) text += '[';
  for (std::size_t index = 0; index < depth; ++index) text += ']';
  return text;
}

ava::permissions::PermissionPrompt const* prompt_for_operation(std::vector<ava::permissions::PermissionPrompt> const& prompts,
                                                               ava::permissions::Operation operation)
{
  auto const found = std::find_if(prompts.begin(), prompts.end(), [&](auto const& prompt) { return prompt.operation == operation; });
  return found == prompts.end() ? nullptr : &*found;
}

bool has_audit_for_prompt(std::vector<ava::tools::PermissionAuditEvent> const& audits, ava::permissions::PermissionPrompt const& prompt,
                          std::string const& resolution, std::string const& resolution_source)
{
  return std::any_of(audits.begin(), audits.end(), [&](auto const& event) {
    return event.permission_request_id == prompt.permission_request_id && event.operation == prompt.operation && event.resolution == resolution &&
           event.resolution_source == resolution_source;
  });
}

void test_mcp_config_parsing()
{
  auto global = ava::mcp::parse_mcp_config("{\"servers\":[{\"id\":\"demo\",\"command\":\"/bin/demo\",\"args\":[\"--stdio\"]}]}", "/tmp/global-mcp.json",
                                           ava::mcp::McpServerScope::Global);
  expect(global && global->servers.size() == 1 && global->servers[0].enabled && global->servers[0].args.size() == 1 &&
             global->servers[0].scope == ava::mcp::McpServerScope::Global,
         global ? "MCP config parses global server defaults" : "MCP config parses global server defaults: " + global.error().format());

  auto project = ava::mcp::parse_mcp_config("{\"servers\":[{\"id\":\"demo-project\",\"command\":\"/bin/demo\"}]}", "/tmp/project-mcp.json",
                                            ava::mcp::McpServerScope::Project);
  expect(project && project->servers.size() == 1 && !project->servers[0].enabled,
         project ? "MCP project config servers default to disabled" : "MCP project config servers default to disabled: " + project.error().format());

  auto bad_id = ava::mcp::parse_mcp_config("{\"servers\":[{\"id\":\"bad id\",\"command\":\"demo\"}]}", "/tmp/bad-mcp.json", ava::mcp::McpServerScope::Global);
  expect(!bad_id && bad_id.error().message().find("valid id") != std::string::npos, "MCP config rejects invalid server ids");

  auto global_relative = ava::mcp::parse_mcp_config("{\"servers\":[{\"id\":\"relative\",\"command\":\"node\",\"args\":[\".ava/mcp-server.js\"]}]}",
                                                    "/tmp/global-mcp.json", ava::mcp::McpServerScope::Global);
  expect(!global_relative && global_relative.error().message().find("workspace-relative") != std::string::npos,
         "MCP global config rejects workspace-relative command arguments before project trust can be bypassed");

  auto project_relative =
      ava::mcp::parse_mcp_config("{\"servers\":[{\"id\":\"relative\",\"command\":\"node\",\"args\":[\".ava/mcp-server.js\"],\"enabled\":true}]}",
                                 "/tmp/project-mcp.json", ava::mcp::McpServerScope::Project);
  expect(project_relative && project_relative->servers.size() == 1,
         project_relative ? "MCP project config may use workspace-relative commands after project trust"
                          : "MCP project config may use workspace-relative commands after project trust: " + project_relative.error().format());

  auto const root = create_empty_root("mcp-config");

  auto const workspace = root / "workspace";
  auto const global_path = root / "global" / "mcp.json";
  auto const project_path = workspace / ".ava" / "mcp.json";
  write_text(global_path, mcp_config_json("same", "/bin/demo"));
  write_text(project_path, mcp_config_json("same", "/bin/demo", true));
  auto duplicate = ava::mcp::load_mcp_config(
      ava::mcp::McpConfigLoadOptions{.workspace_dir = workspace, .global_config_file = global_path, .project_config_file = project_path});
  expect(!duplicate && duplicate.error().message().find("duplicate") != std::string::npos, "MCP config rejects duplicate server ids across scopes");
}

void test_session_mcp_launch_identity_is_logical_and_exact()
{
  auto const root = create_empty_root("mcp-session-launch-identity");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const logical_cwd = ava::core::normalized_absolute_path(workspace);

  ava::mcp::McpServerConfig authorized{.id = "demo",
                                       .name = "Demo",
                                       .command = "/usr/bin/demo",
                                       .args = {"alpha beta"},
                                       .env = {{"Z_VALUE", "two"}, {"A_VALUE", "one"}},
                                       .enabled = true,
                                       .scope = ava::mcp::McpServerScope::Project,
                                       .source_path = {}};
  auto reordered = authorized;
  std::ranges::reverse(reordered.env);
  auto changed_env = authorized;
  changed_env.env[0].second = "changed";
  auto changed_argv_boundary = authorized;
  changed_argv_boundary.args = {"alpha", "beta"};

  auto const identity = ava::mcp::session_mcp_launch_identity(authorized, logical_cwd);
  auto const reordered_identity = ava::mcp::session_mcp_launch_identity(reordered, logical_cwd);
  auto const changed_env_identity = ava::mcp::session_mcp_launch_identity(changed_env, logical_cwd);
  auto const changed_argv_identity = ava::mcp::session_mcp_launch_identity(changed_argv_boundary, logical_cwd);
  auto const expected_identity =
      std::string(R"({"argv":["/usr/bin/demo","alpha beta"],"env":[{"name":"A_VALUE","value":"one"},{"name":"Z_VALUE","value":"two"}],"cwd":")") +
      ava::core::json::escape(logical_cwd.string()) + R"(","clean_environment":true})";
  expect(identity == expected_identity && identity == reordered_identity && identity != changed_env_identity && identity != changed_argv_identity,
         "session MCP launch identity binds argv boundaries, sorted explicit env values, logical cwd spelling, and clean environment");

  auto anchors = ava::core::AnchorSet::open({root});
  expect(anchors.has_value(), "persistent session MCP launch test opens its shared logical-root anchor");
  if (!anchors)
    return;
  ava::permissions::PermissionRuleStore const store{.global_rules_file = root / "config" / "permission-rules.json",
                                                    .workspace_rules_file = workspace / ".ava" / "permission-rules.json",
                                                    .workspace_dir = workspace,
                                                    .anchor_set = *anchors};
  auto rule =
      ava::permissions::add_persistent_permission_rule(store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                                                                    .action = ava::permissions::PermissionAction::Allow,
                                                                                                    .operation = ava::permissions::Operation::McpServerLaunch,
                                                                                                    .mode = ava::permissions::PermissionRuleMode::Build,
                                                                                                    .tool_name = "mcp_discovery",
                                                                                                    .target_path = {},
                                                                                                    .command = identity,
                                                                                                    .command_recipe_key = {},
                                                                                                    .recipe_display = {},
                                                                                                    .critical_acknowledged = false,
                                                                                                    .reason = "authorize exact session MCP launch",
                                                                                                    .actor = "test_operator"});
  auto prompt = [&](std::string command) {
    return ava::permissions::PermissionPrompt{.permission_request_id = "permreq_mcp_identity",
                                              .operation = ava::permissions::Operation::McpServerLaunch,
                                              .mode = ava::core::Mode::Build,
                                              .workspace_dir = workspace,
                                              .target_path = {},
                                              .command = std::move(command),
                                              .tool_name = "mcp_discovery",
                                              .reason = "MCP server launch requires explicit approval",
                                              .risk = ava::permissions::PermissionRisk::High};
  };
  auto exact_match = ava::permissions::match_persistent_permission_rule(store, prompt(identity));
  auto env_mismatch = ava::permissions::match_persistent_permission_rule(store, prompt(changed_env_identity));
  auto argv_mismatch = ava::permissions::match_persistent_permission_rule(store, prompt(changed_argv_identity));
  expect(rule && exact_match && *exact_match && (*exact_match)->rule_id == rule->rule_id && env_mismatch && !*env_mismatch && argv_mismatch && !*argv_mismatch,
         "persistent session MCP launch Allow does not match a changed env value or colliding space-joined argv boundary");

  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
}

void test_mcp_protocol_parsing()
{
  expect(ava::mcp::mcp_response_id("{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"result\":{}}").value_or("") == "abc" &&
             ava::mcp::mcp_response_id("{\"jsonrpc\":\"2.0\",\"id\":42,\"result\":{}}").value_or("") == "42",
         "MCP protocol parses string and numeric response IDs");

  expect(ava::mcp::mcp_bool_field("{\"isError\":false}", "isError").value_or(true) == false, "MCP protocol parses boolean result fields");

  expect(ava::mcp::is_valid_mcp_tool_name("server.tool") && !ava::mcp::is_valid_mcp_tool_name("") && !ava::mcp::is_valid_mcp_tool_name("bad\nname"),
         "MCP protocol validates model-facing tool names");
  expect(ava::mcp::is_valid_mcp_resource_uri("file:///workspace/notes.md") && !ava::mcp::is_valid_mcp_resource_uri("") &&
             !ava::mcp::is_valid_mcp_resource_uri("bad\nuri"),
         "MCP protocol validates resource URIs");

  auto text_content = ava::mcp::mcp_text_content_from_result(
      "{\"content\":[{\"type\":\"text\",\"text\":\"one\"},{\"type\":\"image\",\"data\":\"ignored\"},"
      "{\"type\":\"text\",\"text\":\"two\"}]}");
  expect(text_content == "one\ntwo", "MCP protocol joins text content blocks");

  auto structured_content = ava::mcp::mcp_text_content_from_result("{\"structuredContent\":{\"ok\":true}}");
  expect(structured_content == "{\"ok\":true}", "MCP protocol falls back to structuredContent JSON");

  expect(!ava::mcp::mcp_json_depth_within_limit("{\"deep\":" + nested_arrays_json(130) + "}"), "MCP protocol rejects excessively deep JSON records");

  auto resource_content = ava::mcp::mcp_resource_text_from_result(
      "{\"contents\":[{\"uri\":\"file:///a\",\"mimeType\":\"text/plain\",\"text\":\"one\"},"
      "{\"uri\":\"file:///b\",\"blob\":\"ignored\"},{\"uri\":\"file:///c\",\"text\":\"two\"}]}");
  expect(resource_content == "one\ntwo", "MCP protocol joins text resource contents");
}

void test_mcp_permission_audit_golden_shape()
{
  ava::tools::PermissionAuditEvent event;
  event.permission_request_id = "permreq_ava080";
  event.operation = ava::permissions::Operation::McpToolCall;
  event.mode = ava::core::Mode::Build;
  event.tool_name = "mcp_demo_echo";
  event.action = ava::permissions::PermissionAction::Ask;
  event.reason = "MCP tool calls require explicit approval";
  event.risk = ava::permissions::PermissionRisk::High;
  event.target_path = "/workspace/.ava/mcp.json";
  event.command = "demo:echo";
  event.resolution = "deny";
  event.resolution_source = "resolver";
  event.resolution_reason = "denied by contract test";

  ava::tests::expect_json_matches_golden(ava::tools::permission_audit_data_json(event), "permission-audit.json",
                                         "permission audit JSON shape matches AVA 0.80 golden fixture");
}

void test_mcp_stdio_client_lists_and_calls_tools()
{
  auto const root = create_empty_root("mcp-client");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  auto const startup_cancel_pgid_file = root / "mcp-startup-cancel-pgid.txt";
  auto startup_cancel_server = fake_server_config(root);
  startup_cancel_server.args = {"timeout-initialize-marker", startup_cancel_pgid_file.generic_string()};
  auto startup_canceled = ava::mcp::McpStdioClient::start(startup_cancel_server, fake_client_options(workspace),
                                                          [&] { return read_pid_file_for_test(startup_cancel_pgid_file).has_value(); });
  auto const startup_cancel_pgid = read_pid_file_for_test(startup_cancel_pgid_file);
  expect(!startup_canceled && startup_canceled.error().code() == ava::core::ErrorCode::Canceled &&
             startup_canceled.error().message().find("canceled") != std::string::npos && startup_cancel_pgid &&
             wait_for_process_group_exit(*startup_cancel_pgid),
         "MCP stdio client cancels hung startup and terminates the server process group before timeout");

  auto malformed_server = fake_server_config(root);
  malformed_server.args = {"malformed"};
  auto malformed = ava::mcp::McpStdioClient::start(malformed_server, fake_client_options(workspace));
  expect(!malformed && malformed.error().message().find("valid JSON object") != std::string::npos, "MCP stdio client contains malformed initialize responses");

  auto duplicate_server = fake_server_config(root);
  duplicate_server.args = {"duplicate-initialize"};
  auto duplicate = ava::mcp::McpStdioClient::start(duplicate_server, fake_client_options(workspace));
  expect(!duplicate && duplicate.error().message().find("valid JSON object with strict bounds") != std::string::npos,
         "MCP stdio client rejects duplicate JSON object keys from an untrusted session server");

  for (auto const* mode : {"missing-version", "mismatched-version", "missing-capabilities"})
  {
    auto version_server = fake_server_config(root);
    version_server.args = {mode};
    auto version_client = ava::mcp::McpStdioClient::start(version_server, fake_client_options(workspace));
    expect(!version_client && version_client.error().message().find("initialize response is malformed") != std::string::npos,
           std::string("MCP stdio client rejects an incompatible initialize protocol version: ") + mode);
  }

  auto exit_start_server = fake_server_config(root);
  exit_start_server.args = {"exit-initialize"};
  auto exit_start = ava::mcp::McpStdioClient::start(exit_start_server, fake_client_options(workspace));
  auto const exit_start_format = exit_start ? std::string{} : exit_start.error().format();
  expect(!exit_start && exit_start.error().message() == "MCP server closed before response" && exit_start_format.find("status: exit 42") != std::string::npos &&
             exit_start_format.find("CANARY_MCP_STDERR_INIT_a138") == std::string::npos && exit_start_format.find("stderr_bytes") != std::string::npos,
         "MCP stdio client reports early startup exit with status and safe stderr metadata: " + exit_start_format);

  auto close_stdin_server = fake_server_config(root);
  close_stdin_server.args = {"close-stdin-after-initialize"};
  auto close_stdin = ava::mcp::McpStdioClient::start(close_stdin_server, fake_client_options(workspace));
  expect(!close_stdin && close_stdin.error().message() == "MCP server closed before initialized notification" &&
             close_stdin.error().format().find("stderr_bytes") != std::string::npos,
         "MCP stdio client reports a peer-closed initialized notification with safe metadata");

  auto const global_config_dir = root / "global-config";
  std::filesystem::create_directories(global_config_dir);
  auto const global_cwd_file = root / "mcp-global-cwd.txt";
  auto global_server = fake_server_config(root);
  global_server.scope = ava::mcp::McpServerScope::Global;
  global_server.source_path = global_config_dir / "mcp.json";
  global_server.args = {"cwd-marker", global_cwd_file.generic_string()};
  auto global_cwd_client = ava::mcp::McpStdioClient::start(global_server, fake_client_options(workspace));
  expect(global_cwd_client.has_value(), global_cwd_client ? "MCP global server initializes from safe config cwd"
                                                          : "MCP global server initializes from safe config cwd: " + global_cwd_client.error().format());
  if (global_cwd_client)
  {
    expect(read_text(global_cwd_file) == ava::core::normalized_absolute_path(global_config_dir).string(),
           "MCP global server process cwd is the global config directory, not the workspace");
    auto global_shutdown = (*global_cwd_client)->shutdown(std::chrono::milliseconds(500));
    expect(global_shutdown.has_value(), "MCP global cwd test server shuts down cleanly");
  }

  auto client = ava::mcp::McpStdioClient::start(fake_server_config(root), fake_client_options(workspace));
  expect(client.has_value(), client ? "MCP stdio client initializes fake server" : "MCP stdio client initializes fake server: " + client.error().format());
  if (!client)
    return;

  expect((*client)->initialization().server_name == "fake-mcp", "MCP stdio client records server info");
  auto tools = (*client)->list_tools();
  expect(tools && tools->size() == 1 && tools->front().name == "echo" && tools->front().input_schema_json.find("required") != std::string::npos,
         tools ? "MCP stdio client lists tools" : "MCP stdio client lists tools: " + tools.error().format());
  auto pre_canceled = (*client)->call_tool("echo", "{\"text\":\"hello\"}", [] { return true; });
  expect(!pre_canceled && pre_canceled.error().code() == ava::core::ErrorCode::Canceled && pre_canceled.error().message().find("canceled") != std::string::npos,
         "MCP stdio client maps cancellation before tools/call to a deterministic typed canceled error");
  auto result = (*client)->call_tool("echo", "{\n  \"text\": \"hello\"\n}");
  expect(result && !result->is_error && result->content == "MCP call ok",
         result ? "MCP stdio client sends newline-delimited compact JSON to standard stdio servers"
                : "MCP stdio client sends newline-delimited compact JSON to standard stdio servers: " + result.error().format());
  auto resources = (*client)->list_resources();
  expect(resources && resources->size() == 1 && resources->front().uri == "file:///workspace/notes.md" && resources->front().mime_type == "text/markdown",
         resources ? "MCP stdio client lists resources" : "MCP stdio client lists resources: " + resources.error().format());
  auto resource = (*client)->read_resource("file:///workspace/notes.md");
  expect(resource && resource->content == "MCP resource content" && resource->uri == "file:///workspace/notes.md" && resource->mime_type == "text/markdown",
         resource ? "MCP stdio client reads resources" : "MCP stdio client reads resources: " + resource.error().format());
  auto shutdown = (*client)->shutdown(std::chrono::milliseconds(500));
  expect(shutdown.has_value(), shutdown ? "MCP stdio client shuts down cleanly" : "MCP stdio client shuts down cleanly: " + shutdown.error().format());

  auto paginated_tools_server = fake_server_config(root);
  paginated_tools_server.args = {"paginated-tools"};
  auto paginated_tools_client = ava::mcp::McpStdioClient::start(paginated_tools_server, fake_client_options(workspace));
  expect(paginated_tools_client.has_value(), paginated_tools_client
                                                 ? "MCP stdio client initializes paginated tool fake server"
                                                 : "MCP stdio client initializes paginated tool fake server: " + paginated_tools_client.error().format());
  if (paginated_tools_client)
  {
    auto paginated_tools = (*paginated_tools_client)->list_tools();
    expect(paginated_tools && paginated_tools->size() == 2 && paginated_tools->at(1).name == "second",
           paginated_tools ? "MCP stdio client follows bounded tool list pagination"
                           : "MCP stdio client follows bounded tool list pagination: " + paginated_tools.error().format());
    auto paginated_tools_shutdown = (*paginated_tools_client)->shutdown(std::chrono::milliseconds(500));
    expect(paginated_tools_shutdown.has_value(), "MCP stdio client shuts down after paginated tools/list");
  }

  auto empty_cursor_tools_server = fake_server_config(root);
  empty_cursor_tools_server.args = {"empty-cursor-tools"};
  auto empty_cursor_tools_client = ava::mcp::McpStdioClient::start(empty_cursor_tools_server, fake_client_options(workspace));
  expect(empty_cursor_tools_client.has_value(), "MCP stdio client initializes empty-cursor tool fake server");
  if (empty_cursor_tools_client)
  {
    auto empty_cursor_tools = (*empty_cursor_tools_client)->list_tools();
    expect(empty_cursor_tools && empty_cursor_tools->size() == 2 && empty_cursor_tools->at(1).name == "second",
           "MCP treats a present empty pagination cursor as an opaque continuation token");
    expect((*empty_cursor_tools_client)->shutdown(std::chrono::milliseconds(500)).has_value(), "MCP stdio client shuts down after empty-cursor tools/list");
  }

  auto paginated_prompts_server = fake_server_config(root);
  paginated_prompts_server.args = {"paginated-prompts"};
  auto paginated_prompts_client = ava::mcp::McpStdioClient::start(paginated_prompts_server, fake_client_options(workspace));
  expect(paginated_prompts_client.has_value(), "MCP stdio client initializes paginated prompt fake server");
  if (paginated_prompts_client)
  {
    auto paginated_prompts = (*paginated_prompts_client)->list_prompts();
    expect(paginated_prompts && paginated_prompts->size() == 2 && paginated_prompts->at(1).name == "second-prompt",
           "MCP stdio client follows bounded prompt list pagination");
    expect((*paginated_prompts_client)->shutdown(std::chrono::milliseconds(500)).has_value(), "MCP stdio client shuts down after paginated prompts/list");
  }

  auto missing_prompts_server = fake_server_config(root);
  missing_prompts_server.args = {"missing-prompts"};
  auto missing_prompts_client = ava::mcp::McpStdioClient::start(missing_prompts_server, fake_client_options(workspace));
  expect(missing_prompts_client.has_value(), "MCP stdio client initializes missing-prompts fake server");
  if (missing_prompts_client)
  {
    auto missing_prompts = (*missing_prompts_client)->list_prompts();
    expect(!missing_prompts && missing_prompts.error().message().find("prompts field") != std::string::npos,
           "MCP stdio client rejects prompts/list results without the required prompts array");
    expect((*missing_prompts_client)->shutdown(std::chrono::milliseconds(500)).has_value(), "MCP stdio client shuts down after malformed prompts/list");
  }

  auto missing_tools_server = fake_server_config(root);
  missing_tools_server.args = {"missing-tools"};
  auto missing_tools_client = ava::mcp::McpStdioClient::start(missing_tools_server, fake_client_options(workspace));
  expect(missing_tools_client.has_value(), missing_tools_client
                                               ? "MCP stdio client initializes missing-tools fake server"
                                               : "MCP stdio client initializes missing-tools fake server: " + missing_tools_client.error().format());
  if (missing_tools_client)
  {
    auto missing_tools = (*missing_tools_client)->list_tools();
    expect(!missing_tools && missing_tools.error().message().find("tools field") != std::string::npos,
           "MCP stdio client rejects tools/list results without the required tools array");
    auto missing_tools_shutdown = (*missing_tools_client)->shutdown(std::chrono::milliseconds(500));
    expect(missing_tools_shutdown.has_value(), "MCP stdio client shuts down after malformed tools/list");
  }

  auto paginated_server = fake_server_config(root);
  paginated_server.args = {"paginated-resources"};
  auto paginated_client = ava::mcp::McpStdioClient::start(paginated_server, fake_client_options(workspace));
  expect(paginated_client.has_value(), paginated_client ? "MCP stdio client initializes paginated resource fake server"
                                                        : "MCP stdio client initializes paginated resource fake server: " + paginated_client.error().format());
  if (paginated_client)
  {
    auto paginated_resources = (*paginated_client)->list_resources();
    expect(paginated_resources && paginated_resources->size() == 2 && paginated_resources->at(1).uri == "file:///workspace/two.md",
           paginated_resources ? "MCP stdio client follows bounded resource list pagination"
                               : "MCP stdio client follows bounded resource list pagination: " + paginated_resources.error().format());
    auto paginated_shutdown = (*paginated_client)->shutdown(std::chrono::milliseconds(500));
    expect(paginated_shutdown.has_value(), "MCP stdio client shuts down after paginated resources/list");
  }

  auto missing_resources_server = fake_server_config(root);
  missing_resources_server.args = {"missing-resources"};
  auto missing_resources_client = ava::mcp::McpStdioClient::start(missing_resources_server, fake_client_options(workspace));
  expect(missing_resources_client.has_value(), "MCP stdio client initializes missing-resources fake server");
  if (missing_resources_client)
  {
    auto missing_resources = (*missing_resources_client)->list_resources();
    expect(!missing_resources && missing_resources.error().message().find("resources field") != std::string::npos,
           "MCP stdio client rejects resources/list results without the required resources array");
    expect((*missing_resources_client)->shutdown(std::chrono::milliseconds(500)).has_value(), "MCP stdio client shuts down after malformed resources/list");
  }

  auto blob_server = fake_server_config(root);
  blob_server.args = {"resource-blob"};
  auto blob_client = ava::mcp::McpStdioClient::start(blob_server, fake_client_options(workspace));
  expect(blob_client.has_value(), blob_client ? "MCP stdio client initializes blob resource fake server"
                                              : "MCP stdio client initializes blob resource fake server: " + blob_client.error().format());
  if (blob_client)
  {
    auto blob = (*blob_client)->read_resource("file:///workspace/blob.bin");
    expect(!blob && blob.error().message().find("no text content") != std::string::npos,
           "MCP stdio client rejects blob-only resource reads as unsupported text content");
    auto blob_shutdown = (*blob_client)->shutdown(std::chrono::milliseconds(500));
    expect(blob_shutdown.has_value(), "MCP stdio client shuts down after blob resource rejection");
  }

  auto missing_server = fake_server_config(root);
  missing_server.args = {"resource-missing-contents"};
  auto missing_client = ava::mcp::McpStdioClient::start(missing_server, fake_client_options(workspace));
  expect(missing_client.has_value(), missing_client ? "MCP stdio client initializes malformed resource fake server"
                                                    : "MCP stdio client initializes malformed resource fake server: " + missing_client.error().format());
  if (missing_client)
  {
    auto missing = (*missing_client)->read_resource("file:///workspace/missing.md");
    expect(!missing && missing.error().message().find("contents field") != std::string::npos,
           "MCP stdio client rejects resource reads without contents arrays");
    auto missing_shutdown = (*missing_client)->shutdown(std::chrono::milliseconds(500));
    expect(missing_shutdown.has_value(), "MCP stdio client shuts down after malformed resource rejection");
  }

  auto tool_error_server = fake_server_config(root);
  tool_error_server.args = {"tool-error"};
  auto tool_error_client = ava::mcp::McpStdioClient::start(tool_error_server, fake_client_options(workspace));
  expect(tool_error_client.has_value(), tool_error_client ? "MCP stdio client initializes tool-error fake server"
                                                          : "MCP stdio client initializes tool-error fake server: " + tool_error_client.error().format());
  if (tool_error_client)
  {
    auto tool_error = (*tool_error_client)->call_tool("echo", "{\"text\":\"hello\"}");
    expect(tool_error && tool_error->is_error && tool_error->content.find("external_failure") != std::string::npos &&
               tool_error->content.find("CANARY_MCP_TOOL_CONTENT_682e") == std::string::npos && tool_error->raw_json.empty(),
           tool_error ? "MCP stdio client replaces tool-level error content with SafeFailure"
                      : "MCP stdio client replaces tool-level error content with SafeFailure: " + tool_error.error().format());
    auto tool_error_shutdown = (*tool_error_client)->shutdown(std::chrono::milliseconds(500));
    expect(tool_error_shutdown.has_value(), "MCP stdio client shuts down after tool-level error result");
  }

  auto error_call_server = fake_server_config(root);
  error_call_server.args = {"error-call"};
  auto error_call_client = ava::mcp::McpStdioClient::start(error_call_server, fake_client_options(workspace));
  expect(error_call_client.has_value(), error_call_client ? "MCP stdio client initializes JSON-RPC error fake server"
                                                          : "MCP stdio client initializes JSON-RPC error fake server: " + error_call_client.error().format());
  if (error_call_client)
  {
    auto error_call = (*error_call_client)->call_tool("echo", "{\"text\":\"hello\"}");
    expect(!error_call && error_call.error().message() == "MCP request returned a remote protocol error" &&
               error_call.error().format().find("CANARY_MCP_RESPONSE_ERROR_f2a7") == std::string::npos,
           "MCP stdio client classifies JSON-RPC errors without preserving remote messages");
    auto error_call_shutdown = (*error_call_client)->shutdown(std::chrono::milliseconds(500));
    expect(error_call_shutdown.has_value(), "MCP stdio client shuts down after JSON-RPC error result");
  }

  auto exit_list_server = fake_server_config(root);
  exit_list_server.args = {"exit-after-initialize"};
  auto exit_list_client = ava::mcp::McpStdioClient::start(exit_list_server, fake_client_options(workspace));
  expect(exit_list_client.has_value(), exit_list_client
                                           ? "MCP stdio client initializes exit-after-initialize fake server"
                                           : "MCP stdio client initializes exit-after-initialize fake server: " + exit_list_client.error().format());
  if (exit_list_client)
  {
    auto exited_tools = (*exit_list_client)->list_tools();
    expect(!exited_tools && exited_tools.error().message() == "MCP server closed before response" &&
               exited_tools.error().format().find("status: exit 43") != std::string::npos &&
               exited_tools.error().format().find("CANARY_MCP_STDERR_LIST_4b72") == std::string::npos,
           "MCP stdio client reports discovery-time process exit with status and safe stderr metadata");
    auto exit_list_shutdown = (*exit_list_client)->shutdown(std::chrono::milliseconds(500));
    expect(exit_list_shutdown.has_value(), "MCP stdio client shuts down after discovery-time exit");
  }

  auto stderr_server = fake_server_config(root);
  stderr_server.args = {"stderr-noise"};
  auto stderr_options = fake_client_options(workspace);
  stderr_options.max_stderr_bytes = 16;
  auto stderr_client = ava::mcp::McpStdioClient::start(stderr_server, stderr_options);
  expect(stderr_client.has_value(), stderr_client ? "MCP stdio client initializes noisy fake server"
                                                  : "MCP stdio client initializes noisy fake server: " + stderr_client.error().format());
  if (stderr_client)
  {
    auto stderr_tools = (*stderr_client)->list_tools();
    expect(stderr_tools.has_value(), "MCP stdio client drains stderr while listing tools");
    auto stderr_shutdown = (*stderr_client)->shutdown(std::chrono::milliseconds(500));
    expect(stderr_shutdown.has_value(), "MCP stdio client shuts down noisy fake server");
    expect((*stderr_client)->stderr_truncated(), "MCP stdio client bounds stderr diagnostics");
    expect((*stderr_client)->stderr_tail() == "_MCP_STDERR_19d4", "MCP stdio client keeps a bounded internal stderr tail");
  }

  auto const slow_tool_pgid_file = root / "mcp-tool-cancel-pgid.txt";
  auto slow_server = fake_server_config(root);
  slow_server.args = {"slow-tool-marker", slow_tool_pgid_file.generic_string()};
  auto slow_client = ava::mcp::McpStdioClient::start(slow_server, fake_client_options(workspace));
  expect(slow_client.has_value(),
         slow_client ? "MCP stdio client initializes slow fake server" : "MCP stdio client initializes slow fake server: " + slow_client.error().format());
  if (slow_client)
  {
    auto canceled = (*slow_client)->call_tool("echo", "{\"text\":\"hello\"}", [&] { return read_pid_file_for_test(slow_tool_pgid_file).has_value(); });
    auto const slow_tool_pgid = read_pid_file_for_test(slow_tool_pgid_file);
    expect(!canceled && canceled.error().message().find("canceled") != std::string::npos && slow_tool_pgid && wait_for_process_group_exit(*slow_tool_pgid),
           "MCP stdio client cancels hung tool calls and terminates the server process group before timeout");
    auto slow_shutdown = (*slow_client)->shutdown(std::chrono::milliseconds(500));
    expect(slow_shutdown.has_value(), "MCP stdio client shuts down after canceled tool call");
  }

  auto exit_tool_server = fake_server_config(root);
  exit_tool_server.args = {"exit-tool"};
  auto exit_tool_client = ava::mcp::McpStdioClient::start(exit_tool_server, fake_client_options(workspace));
  expect(exit_tool_client.has_value(), exit_tool_client ? "MCP stdio client initializes exit-tool fake server"
                                                        : "MCP stdio client initializes exit-tool fake server: " + exit_tool_client.error().format());
  if (exit_tool_client)
  {
    auto exited_tool = (*exit_tool_client)->call_tool("echo", "{\"text\":\"hello\"}");
    auto const exited_tool_format = exited_tool ? std::string{} : exited_tool.error().format();
    expect(!exited_tool && exited_tool.error().message() == "MCP server closed before response" &&
               exited_tool_format.find("status: exit 44") != std::string::npos && exited_tool_format.find("CANARY_MCP_STDERR_CALL_0c95") == std::string::npos,
           "MCP stdio client reports tool-call process exit with status and safe stderr metadata: " + exited_tool_format);
    auto exit_tool_shutdown = (*exit_tool_client)->shutdown(std::chrono::milliseconds(500));
    expect(exit_tool_shutdown.has_value(), "MCP stdio client shuts down after tool-call process exit");
  }

  auto const slow_prompt_pgid_file = root / "mcp-prompt-cancel-pgid.txt";
  auto slow_prompt_server = fake_server_config(root);
  slow_prompt_server.args = {"slow-prompt-marker", slow_prompt_pgid_file.generic_string()};
  auto slow_prompt_client = ava::mcp::McpStdioClient::start(slow_prompt_server, fake_client_options(workspace));
  expect(slow_prompt_client.has_value(), slow_prompt_client ? "MCP stdio client initializes slow-prompt fake server"
                                                            : "MCP stdio client initializes slow-prompt fake server: " + slow_prompt_client.error().format());
  if (slow_prompt_client)
  {
    auto canceled_prompt =
        (*slow_prompt_client)->get_prompt("release-notes", "{\"topic\":\"demo\"}", [&] { return read_pid_file_for_test(slow_prompt_pgid_file).has_value(); });
    auto const slow_prompt_pgid = read_pid_file_for_test(slow_prompt_pgid_file);
    expect(!canceled_prompt && canceled_prompt.error().message().find("canceled") != std::string::npos && slow_prompt_pgid &&
               wait_for_process_group_exit(*slow_prompt_pgid),
           "MCP stdio client cancels hung prompts/get calls and terminates the server process group before timeout");
    auto slow_prompt_shutdown = (*slow_prompt_client)->shutdown(std::chrono::milliseconds(500));
    expect(slow_prompt_shutdown.has_value(), "MCP stdio client shuts down after canceled prompt get");
  }

  auto const slow_resource_pgid_file = root / "mcp-resource-cancel-pgid.txt";
  auto slow_resource_server = fake_server_config(root);
  slow_resource_server.args = {"slow-resource-marker", slow_resource_pgid_file.generic_string()};
  auto slow_resource_client = ava::mcp::McpStdioClient::start(slow_resource_server, fake_client_options(workspace));
  expect(slow_resource_client.has_value(), slow_resource_client
                                               ? "MCP stdio client initializes slow-resource fake server"
                                               : "MCP stdio client initializes slow-resource fake server: " + slow_resource_client.error().format());
  if (slow_resource_client)
  {
    auto canceled_resource =
        (*slow_resource_client)->read_resource("file:///workspace/notes.md", [&] { return read_pid_file_for_test(slow_resource_pgid_file).has_value(); });
    auto const slow_resource_pgid = read_pid_file_for_test(slow_resource_pgid_file);
    expect(!canceled_resource && canceled_resource.error().message().find("canceled") != std::string::npos && slow_resource_pgid &&
               wait_for_process_group_exit(*slow_resource_pgid),
           "MCP stdio client cancels hung resources/read calls and terminates the server process group before timeout");
    auto slow_resource_shutdown = (*slow_resource_client)->shutdown(std::chrono::milliseconds(500));
    expect(slow_resource_shutdown.has_value(), "MCP stdio client shuts down after canceled resource read");
  }
}

void test_mcp_tool_dispatcher()
{
  auto const root = create_empty_root("mcp-dispatcher");

  auto const workspace = root / "workspace";
  auto const project_config = workspace / ".ava" / "mcp.json";
  std::filesystem::create_directories(workspace);
  write_text(project_config, mcp_config_json("demo", AVA_FAKE_MCP_SERVER_PATH, true));

  std::vector<ava::permissions::PermissionPrompt> prompts;
  std::vector<ava::tools::PermissionAuditEvent> audits;
  bool cancel_requested = false;
  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  context.mcp_global_config_file = root / "missing-global-mcp.json";
  context.mcp_project_config_file = project_config;
  context.permission_resolver =
      [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Allow;
  };
  context.permission_audit_sink = [&audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
    audits.push_back(event);
    return {};
  };
  context.cancel_requested = [&] { return cancel_requested; };

  auto const model_tool_name = ava::mcp::mcp_model_tool_name("demo", "echo");
  auto const resource_tool_name = ava::mcp::mcp_model_resource_tool_name("demo", "file:///workspace/notes.md");
  auto const schemas = ava::agent::ToolDispatcher::tool_schemas_json(context);
  auto const mcp_schema = std::find_if(schemas.begin(), schemas.end(), [&](std::string const& schema) {
    return schema.find("\"name\":\"" + model_tool_name + "\"") != std::string::npos && schema.find("\"parameters\"") != std::string::npos;
  });
  expect(mcp_schema != schemas.end(), "enabled MCP tool is exported as a provider schema");
  if (mcp_schema != schemas.end())
  {
    ava::tests::expect_json_matches_golden(*mcp_schema, "mcp-tool-schema.json", "MCP tool provider schema matches AVA 0.80 golden fixture");
  }
  auto const resource_schema = std::find_if(schemas.begin(), schemas.end(), [&](std::string const& schema) {
    return schema.find("\"name\":\"" + resource_tool_name + "\"") != std::string::npos && schema.find("\"additionalProperties\":false") != std::string::npos &&
           schema.find("file:///workspace/notes.md") == std::string::npos && schema.find("Project notes resource") == std::string::npos;
  });
  expect(resource_schema != schemas.end(), "enabled MCP resource is exported as an opaque no-argument provider schema");

  ava::agent::ToolDispatcher dispatcher(context);
  auto dispatched = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_mcp", .name = model_tool_name, .arguments_json = "{\"text\":\"hello\"}"});
  expect(dispatched && dispatched->success && dispatched->result_text.find("MCP call ok") != std::string::npos,
         dispatched ? "dispatcher calls enabled MCP tool through broker" : "dispatcher calls enabled MCP tool through broker: " + dispatched.error().format());
  expect(dispatched && dispatched->payload.status == ava::agent::ToolResultStatus::Success && dispatched->payload.content_type == "application/json" &&
             dispatched->payload.content.find("\"server\":\"demo\"") != std::string::npos &&
             dispatched->payload.content.find("MCP call ok") != std::string::npos,
         "MCP tool dispatcher attaches structured semantic result payloads");

  auto const has_launch =
      std::any_of(prompts.begin(), prompts.end(), [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::McpServerLaunch; });
  auto const has_connect =
      std::any_of(prompts.begin(), prompts.end(), [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::McpServerConnect; });
  auto const has_tool_call = std::any_of(prompts.begin(), prompts.end(), [&](auto const& prompt) {
    return prompt.operation == ava::permissions::Operation::McpToolCall && prompt.tool_name == model_tool_name && prompt.command == "demo:echo";
  });
  expect(has_launch && has_connect && has_tool_call, "MCP tools require launch, connect, and mcp.tool.call permission approval");
  auto const audited_tool_call = std::any_of(audits.begin(), audits.end(), [](auto const& event) {
    return event.operation == ava::permissions::Operation::McpToolCall && event.resolution == "allow";
  });
  expect(audited_tool_call, "MCP tool permission decisions are audited");

  auto resource_dispatched = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_mcp_resource", .name = resource_tool_name, .arguments_json = "{}"});
  expect(resource_dispatched && resource_dispatched->success && resource_dispatched->result_text.find("MCP resource content") != std::string::npos,
         resource_dispatched ? "dispatcher reads enabled MCP resources through broker"
                             : "dispatcher reads enabled MCP resources through broker: " + resource_dispatched.error().format());
  expect(resource_dispatched && resource_dispatched->payload.status == ava::agent::ToolResultStatus::Success &&
             resource_dispatched->payload.content_type == "application/json" &&
             resource_dispatched->payload.content.find("\"mcp_resource\":\"file:///workspace/notes.md\"") != std::string::npos,
         "MCP resource dispatcher attaches structured semantic result payloads");
  auto const has_resource_read = std::any_of(prompts.begin(), prompts.end(), [&](auto const& prompt) {
    return prompt.operation == ava::permissions::Operation::McpResourceRead && prompt.tool_name == resource_tool_name &&
           prompt.command == "demo:file:///workspace/notes.md";
  });
  expect(has_resource_read, "MCP resources require mcp.resource.read permission approval");
  auto const audited_resource_read = std::any_of(audits.begin(), audits.end(), [](auto const& event) {
    return event.operation == ava::permissions::Operation::McpResourceRead && event.resolution == "allow";
  });
  expect(audited_resource_read, "MCP resource permission decisions are audited");

  auto resource_bad_args = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_mcp_resource_bad_args", .name = resource_tool_name, .arguments_json = "{\"uri\":\"other\"}"});
  expect(resource_bad_args && !resource_bad_args->success && resource_bad_args->result_text.find("empty JSON object") == std::string::npos &&
             resource_bad_args->result_text.find("invalid_request") != std::string::npos,
         "MCP resource dispatcher rejects caller-supplied arguments with SafeFailure before execution");
  expect(resource_bad_args && resource_bad_args->result_text.find("file:///workspace/notes.md") == std::string::npos,
         "MCP resource invalid-argument errors do not leak raw resource URIs before read approval");

  auto invalid_args = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_bad_args", .name = model_tool_name, .arguments_json = "[]"});
  expect(invalid_args && !invalid_args->success && invalid_args->result_text.find("JSON object") == std::string::npos &&
             invalid_args->result_text.find("invalid_request") != std::string::npos,
         "MCP tool dispatcher rejects non-object arguments with SafeFailure before execution");
  expect(invalid_args && invalid_args->payload.status == ava::agent::ToolResultStatus::Error && invalid_args->payload.error_category == "configuration" &&
             invalid_args->payload.error_code == "invalid_request",
         "MCP tool dispatcher attaches support-safe structured semantic errors");

  auto typed_cancel_context = context;
  typed_cancel_context.permission_audit_sink = [](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
    if (event.operation == ava::permissions::Operation::McpToolCall)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "renamed MCP interruption", ava::core::ErrorCode::Canceled));
    return {};
  };
  ava::agent::ToolDispatcher typed_cancel_dispatcher(typed_cancel_context);
  auto typed_cancel =
      typed_cancel_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_mcp_typed_cancel", .name = model_tool_name, .arguments_json = "{}"});
  expect(typed_cancel && !typed_cancel->success && typed_cancel->payload.status == ava::agent::ToolResultStatus::Canceled,
         "MCP broker classifies a renamed typed cancellation by ErrorCode");

  auto legacy_cancel_context = context;
  legacy_cancel_context.permission_audit_sink = [](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
    if (event.operation != ava::permissions::Operation::McpToolCall)
      return {};
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "tool canceled");
    error.with_context("canceled", "true");
    return std::unexpected(std::move(error));
  };
  ava::agent::ToolDispatcher legacy_cancel_dispatcher(legacy_cancel_context);
  auto legacy_cancel =
      legacy_cancel_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_mcp_legacy_cancel", .name = model_tool_name, .arguments_json = "{}"});
  expect(legacy_cancel && !legacy_cancel->success && legacy_cancel->payload.status == ava::agent::ToolResultStatus::Error,
         "MCP broker does not classify untyped legacy cancellation text or context as cancellation");

  auto const prompts_before_cancel = prompts.size();
  cancel_requested = true;
  auto canceled = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_canceled", .name = model_tool_name, .arguments_json = "{}"});
  expect(canceled && !canceled->success && canceled->payload.status == ava::agent::ToolResultStatus::Canceled &&
             canceled->result_text.find("canceled") != std::string::npos && prompts.size() == prompts_before_cancel,
         "MCP tool dispatcher reports semantic cancellation before permission or process execution");
}

void test_mcp_tool_dispatcher_audits_permission_denials()
{
  auto run_denial_case = [](std::string const& suffix, ava::permissions::Operation denied_operation) {
    auto const root = create_empty_root("mcp-denial-" + suffix);

    auto const workspace = root / "workspace";
    auto const project_config = workspace / ".ava" / "mcp.json";
    std::filesystem::create_directories(workspace);
    write_text(project_config, mcp_config_json("demo", AVA_FAKE_MCP_SERVER_PATH, true));

    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    std::optional<ava::permissions::Operation> operation_to_deny;
    ava::tools::ToolContext context;
    context.workspace_dir = workspace;
    context.mcp_global_config_file = root / "missing-global-mcp.json";
    context.mcp_project_config_file = project_config;
    context.permission_resolver = [&](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
      prompts.push_back(prompt);
      if (operation_to_deny && prompt.operation == *operation_to_deny)
      {
        return ava::permissions::PermissionResolutionDecision(ava::permissions::PermissionResolution::Deny, "denied by contract test");
      }
      return ava::permissions::PermissionResolutionDecision(ava::permissions::PermissionResolution::Allow, "allowed by contract test setup");
    };
    context.permission_audit_sink = [&audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
      audits.push_back(event);
      return {};
    };

    auto const model_tool_name = denied_operation == ava::permissions::Operation::McpResourceRead
                                     ? ava::mcp::mcp_model_resource_tool_name("demo", "file:///workspace/notes.md")
                                     : ava::mcp::mcp_model_tool_name("demo", "echo");
    auto const arguments_json = denied_operation == ava::permissions::Operation::McpResourceRead ? "{}" : "{\"text\":\"hello\"}";
    ava::agent::ToolDispatcher dispatcher(context);
    prompts.clear();
    audits.clear();
    operation_to_deny = denied_operation;

    auto dispatched =
        dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_mcp_denied_" + suffix, .name = model_tool_name, .arguments_json = arguments_json});
    expect(dispatched && !dispatched->success && dispatched->result_text.find("permission_denied") != std::string::npos,
           dispatched ? "MCP dispatcher reports support-safe permission denial for " + suffix
                      : "MCP dispatcher reports support-safe permission denial for " + suffix + ": " + dispatched.error().format());
    if (denied_operation == ava::permissions::Operation::McpResourceRead)
    {
      expect(dispatched && dispatched->result_text.find("file:///workspace/notes.md") == std::string::npos,
             "MCP resource denial result does not leak raw resource URI before approval");
    }

    auto const* denied_prompt = prompt_for_operation(prompts, denied_operation);
    expect(denied_prompt != nullptr && !denied_prompt->permission_request_id.empty(), "MCP denial prompt carries a request id for " + suffix);
    if (denied_prompt != nullptr)
    {
      auto const linked_audits =
          std::count_if(audits.begin(), audits.end(), [&](auto const& event) { return event.permission_request_id == denied_prompt->permission_request_id; });
      expect(linked_audits >= 2, "MCP denial records policy and resolver audit events with the prompt request id for " + suffix);
      expect(has_audit_for_prompt(audits, *denied_prompt, "deny", "resolver"), "MCP denial audit records resolver denial for " + suffix);
      if (dispatched)
      {
        expect(std::find(dispatched->payload.permission_request_ids.begin(), dispatched->payload.permission_request_ids.end(),
                         denied_prompt->permission_request_id) != dispatched->payload.permission_request_ids.end(),
               "MCP denial result payload links the permission request id for " + suffix);
      }
    }
  };

  run_denial_case("launch", ava::permissions::Operation::McpServerLaunch);
  run_denial_case("connect", ava::permissions::Operation::McpServerConnect);
  run_denial_case("tool", ava::permissions::Operation::McpToolCall);
  run_denial_case("resource", ava::permissions::Operation::McpResourceRead);
}

void test_mcp_headless_policy_allows_resource_reads()
{
  ava::app::HeadlessPermissionPolicyOptions options;
  auto added = ava::app::add_headless_allowed_tools(options, "mcp");
  expect(added.has_value(), "headless policy accepts mcp allow-tool value");
  auto resolver = ava::app::build_headless_permission_resolver(std::move(options));

  ava::permissions::PermissionPrompt prompt;
  prompt.operation = ava::permissions::Operation::McpResourceRead;
  prompt.tool_name = ava::mcp::mcp_model_resource_tool_name("demo", "file:///workspace/notes.md");
  prompt.command = "demo:file:///workspace/notes.md";
  auto decision = resolver(prompt);
  expect(
      decision && decision->resolution == ava::permissions::PermissionResolution::Allow,
      decision ? "headless --allow-tool mcp permits MCP resource reads" : "headless --allow-tool mcp permits MCP resource reads: " + decision.error().format());
}

void test_mcp_tool_dispatcher_contains_tool_errors()
{
  auto const root = create_empty_root("mcp-dispatcher-tool-error");

  auto const workspace = root / "workspace";
  auto const project_config = workspace / ".ava" / "mcp.json";
  std::filesystem::create_directories(workspace);
  write_text(project_config, mcp_config_json("demo", AVA_FAKE_MCP_SERVER_PATH, true, "[\"tool-error\"]"));

  std::vector<ava::permissions::PermissionPrompt> prompts;
  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  context.mcp_global_config_file = root / "missing-global-mcp.json";
  context.mcp_project_config_file = project_config;
  context.permission_resolver =
      [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Allow;
  };

  auto const model_tool_name = ava::mcp::mcp_model_tool_name("demo", "echo");
  ava::agent::ToolDispatcher dispatcher(context);
  auto dispatched =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_mcp_tool_error", .name = model_tool_name, .arguments_json = "{\"text\":\"hello\"}"});
  expect(dispatched && !dispatched->success && dispatched->payload.status == ava::agent::ToolResultStatus::Error &&
             dispatched->payload.content_type == "application/json" && dispatched->payload.content.find("external_failure") != std::string::npos &&
             dispatched->payload.content.find("CANARY_MCP_TOOL_CONTENT_682e") == std::string::npos && dispatched->payload.error_code == "external_failure",
         dispatched ? "MCP tool dispatcher replaces tool-level error content with SafeFailure"
                    : "MCP tool dispatcher replaces tool-level error content with SafeFailure: " + dispatched.error().format());
  expect(!prompts.empty(), "MCP tool dispatcher still gates tool-level error calls through permission prompts");

  auto const text_root = create_empty_root("mcp-dispatcher-canceled-text-error");

  auto const text_workspace = text_root / "workspace";
  auto const text_project_config = text_workspace / ".ava" / "mcp.json";
  std::filesystem::create_directories(text_workspace);
  write_text(text_project_config, mcp_config_json("demo", AVA_FAKE_MCP_SERVER_PATH, true, "[\"tool-error-canceled-text\"]"));

  ava::tools::ToolContext text_context;
  text_context.workspace_dir = text_workspace;
  text_context.mcp_global_config_file = text_root / "missing-global-mcp.json";
  text_context.mcp_project_config_file = text_project_config;
  text_context.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
  ava::agent::ToolDispatcher text_dispatcher(text_context);
  auto text_dispatched = text_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_mcp_canceled_text_error", .name = model_tool_name, .arguments_json = "{\"text\":\"hello\"}"});
  expect(text_dispatched && !text_dispatched->success && text_dispatched->payload.status == ava::agent::ToolResultStatus::Error &&
             text_dispatched->payload.error_message.find("canceled upstream") == std::string::npos && text_dispatched->payload.error_code == "external_failure",
         text_dispatched ? "MCP tool dispatcher safely classifies ordinary remote errors as integration failures"
                         : "MCP tool dispatcher safely classifies ordinary remote errors as integration failures: " + text_dispatched.error().format());
}

void test_mcp_strict_session_registry_failures_and_nested_cwd()
{
  auto const root = create_empty_root("mcp-strict-session");

  auto const workspace = root / "workspace";
  auto const nested = workspace / "nested";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  std::filesystem::create_directories(nested);

  auto const paths = ava::tests::app_test_paths(root);
  auto context_for_servers = [&](std::vector<ava::mcp::McpServerConfig> servers) {
    ava::tools::ToolContext context;
    context.workspace_dir = workspace;
    context.current_dir = nested;
    context.plugin_global_plugins_dir = paths.ava_config_dir / "plugins";
    context.plugin_project_plugins_dir = workspace / ".ava" / "plugins";
    context.plugin_enablement_file = paths.ava_state_dir / "plugin-enablement.json";
    context.session_mcp_config =
        std::make_shared<ava::mcp::McpConfig const>(ava::mcp::McpConfig{.servers = std::move(servers), .global_config_file = {}, .project_config_file = {}});
    context.exact_builtin_tool_names = std::vector<std::string>{};
    context.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
      return ava::permissions::PermissionResolution::Allow;
    };
    return context;
  };
  auto context_for = [&](ava::mcp::McpServerConfig server) {
    std::vector<ava::mcp::McpServerConfig> servers;
    servers.push_back(std::move(server));
    return context_for_servers(std::move(servers));
  };

  auto startup = fake_server_config(root);
  startup.command = (root / "missing-mcp-server").string();
  auto startup_failed = ava::agent::ToolDispatcher::create_strict(context_for(std::move(startup)));
  expect(!startup_failed && startup_failed.error().format().find("missing-mcp-server") == std::string::npos,
         "strict session MCP registry returns support-safe startup failures instead of omitting the mandatory server");

  auto initialize = fake_server_config(root);
  initialize.args = {"exit-initialize"};
  auto initialize_failed = ava::agent::ToolDispatcher::create_strict(context_for(std::move(initialize)));
  expect(!initialize_failed && initialize_failed.error().format().find("status: exit 42") != std::string::npos &&
             initialize_failed.error().format().find("CANARY_MCP_STDERR_INIT_a138") == std::string::npos,
         "strict session MCP registry returns support-safe initialization failures: " +
             (initialize_failed ? std::string("unexpected success") : initialize_failed.error().format()));

  auto tools_list = fake_server_config(root);
  tools_list.args = {"exit-after-initialize"};
  auto tools_failed = ava::agent::ToolDispatcher::create_strict(context_for(std::move(tools_list)));
  expect(!tools_failed && tools_failed.error().format().find("status: exit 43") != std::string::npos &&
             tools_failed.error().format().find("CANARY_MCP_STDERR_LIST_4b72") == std::string::npos,
         "strict session MCP registry returns support-safe tools/list failures: " +
             (tools_failed ? std::string("unexpected success") : tools_failed.error().format()));

  auto resources_list = fake_server_config(root);
  resources_list.args = {"exit-resources-list"};
  auto resources_ignored = ava::agent::ToolDispatcher::create_strict(context_for(std::move(resources_list)));
  expect(resources_ignored && resources_ignored->registered_tool_metadata().size() == 1,
         "strict session MCP registry mirrors tools/list without legacy MCP resource pseudo-tools");

  auto const plugin_id = "com.example.strict-shadow";
  auto const plugin_dir = paths.ava_config_dir / "plugins" / plugin_id;
  write_text(plugin_dir / "plugin.json", ava::tests::app_test_plugin_manifest_json(plugin_id, "Strict Shadow"));
  auto plugin_enabled =
      ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, plugin_id, true, ava::plugin::PluginScope::Global);
  expect(plugin_enabled.has_value(), "strict MCP registry fixture enables an installed plugin tool");

  auto const cwd_marker = root / "session-cwd.txt";
  auto cwd_server = fake_server_config(root);
  cwd_server.args = {"cwd-marker", cwd_marker.string()};
  auto strict_context = context_for(cwd_server);
  auto strict = ava::agent::ToolDispatcher::create_strict(strict_context);
  auto const mcp_name = ava::mcp::mcp_model_tool_name("demo", "echo");
  auto const plugin_name = ava::plugin::plugin_model_tool_name(plugin_id, "todo_add");
  bool strict_has_mcp = false;
  bool strict_has_plugin = false;
  bool strict_has_builtin = false;
  std::size_t strict_tool_count = 0;
  if (strict)
  {
    auto const metadata_entries = strict->registered_tool_metadata();
    strict_tool_count = metadata_entries.size();
    for (auto const& metadata : metadata_entries)
    {
      strict_has_mcp = strict_has_mcp || metadata.name == mcp_name;
      strict_has_plugin = strict_has_plugin || metadata.name == plugin_name;
      strict_has_builtin = strict_has_builtin || metadata.name == "read_file";
    }
  }
  expect(strict && strict_tool_count == 1 && strict_has_mcp && !strict_has_plugin && !strict_has_builtin &&
             read_text(cwd_marker) == ava::core::normalized_absolute_path(nested).string(),
         strict ? "strict session registry exposes only valid session MCP tools from persisted current_dir"
                : "session MCP nested-cwd registry build failed: " + strict.error().format());

  auto legacy_context = strict_context;
  legacy_context.exact_builtin_tool_names = std::nullopt;
  auto legacy = ava::agent::ToolDispatcher::create_strict(std::move(legacy_context));
  bool legacy_has_plugin = false;
  if (legacy)
    for (auto const& metadata : legacy->registered_tool_metadata()) legacy_has_plugin = legacy_has_plugin || metadata.name == plugin_name;
  expect(legacy && legacy_has_plugin, "legacy registry behavior still includes an enabled plugin beside session MCP");

  auto first_collision = fake_server_config(root);
  first_collision.id = "demo-one";
  auto second_collision = fake_server_config(root);
  second_collision.id = "demo_one";
  auto collision = ava::agent::ToolDispatcher::create_strict(
      context_for_servers(std::vector<ava::mcp::McpServerConfig>{std::move(first_collision), std::move(second_collision)}));
  auto const collision_details = collision ? std::string{} : collision.error().format();
  expect(!collision && collision_details.find("duplicate model tool name") != std::string::npos &&
             collision_details.find("tool: mcp_demo_one_echo") != std::string::npos && collision_details.find("mcp_server: demo_one") != std::string::npos &&
             collision_details.find("mcp_name: echo") != std::string::npos && collision_details.find("existing_source: mcp") != std::string::npos &&
             collision_details.find("existing_source_id: demo-one") != std::string::npos,
         "strict session MCP registry preserves exact collision diagnostic fields: " + collision_details);

  first_collision = fake_server_config(root);
  first_collision.id = "demo-one";
  second_collision = fake_server_config(root);
  second_collision.id = "demo_one";
  auto best_effort_context = context_for_servers(std::vector<ava::mcp::McpServerConfig>{std::move(first_collision), std::move(second_collision)});
  best_effort_context.exact_builtin_tool_names = std::nullopt;
  auto best_effort = ava::agent::ToolDispatcher::create_strict(std::move(best_effort_context));
  expect(best_effort.has_value(), "legacy session/global registry keeps best-effort collision shadowing unchanged");

  auto unavailable_context = context_for_servers({});
  unavailable_context.exact_builtin_tool_names = std::vector<std::string>{"missing_builtin"};
  auto unavailable = ava::agent::ToolDispatcher::create_strict(std::move(unavailable_context));
  expect(!unavailable && unavailable.error().message().find("requested built-in tool is unavailable") != std::string::npos,
         "exact tool composition fails closed when an injected built-in is unavailable");

  std::filesystem::remove_all(root, remove_error);
}

void test_mcp_session_environment_is_clean()
{
  auto const root = create_empty_root("mcp-session-clean-env");

  std::filesystem::create_directories(root);
  auto const marker = root / "environment.txt";
  ScopedEnvVar const parent_secret("AVA_MCP_PARENT_SECRET", "must-not-leak");
  ScopedEnvVar const parent_marker("AVA_MCP_PARENT_MARKER", "parent-marker");
  ScopedEnvVar const parent_explicit("AVA_MCP_EXPLICIT", "parent-value");

  auto context_for = [&](ava::mcp::McpServerConfig server) {
    ava::tools::ToolContext context;
    context.workspace_dir = root;
    context.current_dir = root;
    context.session_mcp_config =
        std::make_shared<ava::mcp::McpConfig const>(ava::mcp::McpConfig{.servers = {std::move(server)}, .global_config_file = {}, .project_config_file = {}});
    context.exact_builtin_tool_names = std::vector<std::string>{};
    context.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
      return ava::permissions::PermissionResolution::Allow;
    };
    return context;
  };

  auto server = fake_server_config(root);
  server.args = {"env-marker", marker.string()};
  server.env = {{"AVA_MCP_EXPLICIT", "session-value"}};
  auto trusted_path = ava::agent::ToolDispatcher::create_strict(context_for(server));
  auto const trusted_marker = read_text(marker);
  expect(trusted_path && trusted_marker.find("EXPLICIT=session-value") != std::string::npos && trusted_marker.find("INHERITED=<unset>") != std::string::npos &&
             trusted_marker.find("SECRET=<unset>") != std::string::npos &&
             trusted_marker.find("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin") != std::string::npos,
         trusted_path ? "immutable session MCP receives only trusted PATH and explicit environment values"
                      : "immutable session MCP clean-environment discovery failed: " + trusted_path.error().format());

  server.env = {{"AVA_MCP_EXPLICIT", "session-value"}, {"PATH", "/bounded/session/path"}};
  auto explicit_path = ava::agent::ToolDispatcher::create_strict(context_for(std::move(server)));
  auto const explicit_marker = read_text(marker);
  expect(explicit_path && explicit_marker.find("EXPLICIT=session-value") != std::string::npos &&
             explicit_marker.find("INHERITED=<unset>") != std::string::npos && explicit_marker.find("SECRET=<unset>") != std::string::npos &&
             explicit_marker.find("PATH=/bounded/session/path") != std::string::npos,
         explicit_path ? "immutable session MCP forwards an explicit bounded PATH without parent environment leakage"
                       : "immutable session MCP explicit-PATH discovery failed: " + explicit_path.error().format());

  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
}

void test_mcp_ordinary_global_and_project_environment_is_inherited()
{
  auto const root = create_empty_root("mcp-ordinary-inherited-env");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const marker = root / "environment.txt";
  ScopedEnvVar const parent_marker("AVA_MCP_PARENT_MARKER", "parent-marker");
  ScopedEnvVar const parent_explicit("AVA_MCP_EXPLICIT", "parent-value");
  ScopedEnvVar const parent_path("PATH", "/untrusted/parent/path");

  auto const global_config = root / "global-mcp.json";
  auto const args_json = "[\"env-marker\",\"" + ava::core::json::escape(marker.string()) + "\"]";
  write_text(global_config, mcp_config_json("ordinary-global", AVA_FAKE_MCP_SERVER_PATH, true, args_json));
  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  context.mcp_global_config_file = global_config;
  context.include_project_mcp_config = false;
  context.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
  auto inherited = ava::agent::ToolDispatcher::create_strict(std::move(context));
  auto const inherited_marker = read_text(marker);
  expect(inherited && inherited_marker.find("EXPLICIT=parent-value") != std::string::npos &&
             inherited_marker.find("INHERITED=parent-marker") != std::string::npos &&
             inherited_marker.find("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin") != std::string::npos,
         inherited ? "ordinary loaded global MCP inherits benign parent variables and replaces parent PATH with the trusted default"
                   : "ordinary loaded global MCP environment discovery failed: " + inherited.error().format());

  std::filesystem::remove(marker);
  ava::tools::ToolContext suppressed_context;
  suppressed_context.workspace_dir = workspace;
  suppressed_context.mcp_global_config_file = global_config;
  suppressed_context.include_global_mcp_config = false;
  suppressed_context.include_project_mcp_config = false;
  suppressed_context.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
  auto suppressed = ava::agent::ToolDispatcher::create_strict(std::move(suppressed_context));
  expect(suppressed && !std::filesystem::exists(marker),
         suppressed ? "disabled global MCP ignores an explicitly supplied config path"
                    : "disabled global MCP config unexpectedly affected strict dispatcher creation: " + suppressed.error().format());

  auto server = fake_server_config(root);
  server.scope = ava::mcp::McpServerScope::Project;
  server.args = {"env-marker", marker.string()};
  server.env = {{"AVA_MCP_EXPLICIT", "project-value"}, {"PATH", "/bounded/project/path"}};
  auto explicit_path = ava::mcp::McpStdioClient::start(server, fake_client_options(root));
  expect(explicit_path.has_value(), explicit_path ? "ordinary project MCP starts with explicit environment overrides"
                                                  : "ordinary project MCP start failed: " + explicit_path.error().format());
  if (explicit_path)
  {
    static_cast<void>((*explicit_path)->shutdown());
    auto const explicit_marker = read_text(marker);
    expect(explicit_marker.find("EXPLICIT=project-value") != std::string::npos && explicit_marker.find("INHERITED=parent-marker") != std::string::npos &&
               explicit_marker.find("PATH=/bounded/project/path") != std::string::npos,
           "ordinary project MCP retains inherited variables while applying exact explicit value and PATH overrides");
  }

  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
}

void test_mcp_stdio_environment_validation()
{
  auto const root = create_empty_root("mcp-environment-validation");
  auto server = fake_server_config(root);
  server.env = {{"DUPLICATE", "one"}, {"DUPLICATE", "two"}};
  auto duplicate = ava::mcp::McpStdioClient::start(server, fake_client_options(root));
  expect(!duplicate && duplicate.error().message().find("duplicate") != std::string::npos, "MCP stdio rejects duplicate environment names before launch");
}

}  // namespace

void run_mcp_tests()
{
  test_mcp_config_parsing();
  test_session_mcp_launch_identity_is_logical_and_exact();
  test_mcp_protocol_parsing();
  test_mcp_permission_audit_golden_shape();
  test_mcp_stdio_client_lists_and_calls_tools();
  test_mcp_tool_dispatcher();
  test_mcp_tool_dispatcher_audits_permission_denials();
  test_mcp_tool_dispatcher_contains_tool_errors();
  test_mcp_headless_policy_allows_resource_reads();
  test_mcp_strict_session_registry_failures_and_nested_cwd();
  test_mcp_session_environment_is_clean();
  test_mcp_ordinary_global_and_project_environment_is_inherited();
  test_mcp_stdio_environment_validation();
}
