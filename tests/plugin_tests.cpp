#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/golden.h"
#include "tests/support/process_group_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/event/RuntimeEvent.h"
#include "ava/app/command_plugins.h"
#include "ava/app/plugin_event_hooks.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime/Session.h"
#include "ava/agent/tool_dispatcher.h"
#include "ava/tools/file_tools.h"
#include "ava/plugin/diagnostics.h"
#include "ava/plugin/discovery.h"
#include "ava/plugin/enablement.h"
#include "ava/plugin/manifest.h"
#include "ava/plugin/runner.h"
#include "ava/plugin/runner_protocol.h"
#include "ava/plugin/static_resources.h"
#include "ava/plugin/tool_broker.h"
#include "ava/permissions/permission.h"
#include "ava/core/json.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef AVA_SAMPLE_TODO_PLUGIN_DIR
#define AVA_SAMPLE_TODO_PLUGIN_DIR ""
#endif

#ifndef AVA_FAKE_PLUGIN_CHILD_PATH
#define AVA_FAKE_PLUGIN_CHILD_PATH ""
#endif

namespace {

using ava::test::wait_for_process_group_exit;

using PluginDescriptorExecutor = decltype(ava::plugin::PluginBrokeredTool::executor);
static_assert(
    std::is_invocable_r_v<ava::tools::ToolDispatchResult, PluginDescriptorExecutor, ava::tools::ToolContext const&, ava::tools::ProviderToolCall const&>);
static_assert(!std::is_invocable_v<PluginDescriptorExecutor, ava::tools::ToolContext const&, ava::agent::ToolDispatchServices const&,
                                   ava::tools::ProviderToolCall const&>);

std::string valid_manifest_json(std::string id = "com.example.todo")
{
  return std::string("{\n") +
         "  \"schema_version\": 1,\n"
         "  \"id\": \"" +
         ava::core::json::escape(id) +
         "\",\n"
         "  \"name\": \"Todo Tools\",\n"
         "  \"version\": \"0.1.0\",\n"
         "  \"api_version\": \"ava.plugin.v1\",\n"
         "  \"description\": \"test plugin\",\n"
         "  \"entrypoint\": {\"command\": \"node\", \"args\": [\"plugin.js\", \"--safe\"]},\n"
         "  \"capabilities\": [\"tools\", \"commands\"],\n"
         "  \"contributes\": {\n"
         "    \"tools\": [{\"name\": \"todo_add\", \"description\": \"Add todo\", \"input_schema\": {\"type\": "
         "\"object\", \"additionalProperties\": false}}],\n"
         "    \"commands\": [{\"name\": \"todo\", \"description\": \"Show todos\"}],\n"
         "    \"prompts\": [{\"name\": \"review\", \"description\": \"Review prompt\", \"path\": "
         "\"prompts/review.md\"}],\n"
         "    \"skills\": [{\"name\": \"triage\", \"description\": \"Triage skill\", \"path\": "
         "\"skills/triage.md\"}],\n"
         "    \"event_hooks\": [{\"event\": \"tool.result\"}]\n"
         "  },\n"
         "  \"unknown_future_field\": true\n"
         "}";
}

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
  return buffer.str();
}

class PluginWindowExactFileAccess final : public ava::tools::ExactFileAccess
{
 public:
  [[nodiscard]] bool supports_write_text_file() const noexcept override { return false; }

  [[nodiscard]] ava::core::Result<std::string> read_text_file(std::filesystem::path const&, ava::tools::ToolIoCancelCallback) const override
  {
    return "remote-one\nremote-two\nremote-three\n";
  }

  [[nodiscard]] ava::core::VoidResult write_text_file(std::filesystem::path const&, std::string_view, ava::tools::ToolIoCancelCallback) const override
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "exact writes are disabled in this fixture"));
  }
};

std::optional<pid_t> read_pid_file_for_test(std::filesystem::path const& path)
{
  std::ifstream file(path, std::ios::binary);
  long long value = 0;
  file >> value;
  if (!file || value <= 0)
    return std::nullopt;
  return static_cast<pid_t>(value);
}

std::string shell_single_quote(std::string_view value)
{
  std::string quoted = "'";
  for (char ch : value)
  {
    if (ch == '\'')
    {
      quoted += "'\\''";
    }
    else
    {
      quoted.push_back(ch);
    }
  }
  quoted += "'";
  return quoted;
}

std::filesystem::path sample_todo_plugin_dir()
{
  std::filesystem::path const path{AVA_SAMPLE_TODO_PLUGIN_DIR};
  expect(!path.empty(), "sample todo plugin fixture path is configured");
  expect(std::filesystem::is_directory(path), "sample todo plugin fixture directory exists");
  return path;
}

ava::plugin::PluginManifest load_sample_todo_manifest()
{
  auto const manifest_path = sample_todo_plugin_dir() / "plugin.json";
  auto parsed = ava::plugin::load_plugin_manifest(manifest_path);
  expect(parsed.has_value(), parsed ? "sample todo plugin manifest loads" : "sample todo plugin manifest loads: " + parsed.error().format());
  return parsed.value_or(ava::plugin::PluginManifest{});
}

std::string runner_manifest_json(std::string id, std::string script_name)
{
  return std::string("{\n") +
         "  \"schema_version\": 1,\n"
         "  \"id\": \"" +
         ava::core::json::escape(id) +
         "\",\n"
         "  \"name\": \"Runner Plugin\",\n"
         "  \"version\": \"0.1.0\",\n"
         "  \"api_version\": \"ava.plugin.v1\",\n"
         "  \"entrypoint\": {\"command\": \"/bin/sh\", \"args\": [\"" +
         ava::core::json::escape(script_name) +
         "\"]},\n"
         "  \"capabilities\": [\"tools\"],\n"
         "  \"contributes\": {\"tools\": [], \"commands\": []}\n"
         "}";
}

std::string tool_manifest_json(std::string id, std::string script_name, std::string tool_name = "todo_add", std::string capabilities_json = "[\"tools\"]")
{
  return std::string("{\n") +
         "  \"schema_version\": 1,\n"
         "  \"id\": \"" +
         ava::core::json::escape(id) +
         "\",\n"
         "  \"name\": \"Tool Plugin\",\n"
         "  \"version\": \"0.1.0\",\n"
         "  \"api_version\": \"ava.plugin.v1\",\n"
         "  \"entrypoint\": {\"command\": \"/bin/sh\", \"args\": [\"" +
         ava::core::json::escape(script_name) +
         "\"]},\n"
         "  \"capabilities\": " +
         capabilities_json + ",\n" + "  \"contributes\": {\"tools\": [{\"name\": \"" + ava::core::json::escape(tool_name) +
         "\", \"description\": \"Add todo\", \"input_schema\": {\"type\": \"object\", \"properties\": "
         "{\"text\": {\"type\": \"string\"}}, \"required\": [\"text\"], \"additionalProperties\": false}}], "
         "\"commands\": []}\n"
         "}";
}

std::string event_hook_manifest_json(std::string id, std::string script_name, std::string event_name = "tool.result")
{
  return std::string("{\n") +
         "  \"schema_version\": 1,\n"
         "  \"id\": \"" +
         ava::core::json::escape(id) +
         "\",\n"
         "  \"name\": \"Event Plugin\",\n"
         "  \"version\": \"0.1.0\",\n"
         "  \"api_version\": \"ava.plugin.v1\",\n"
         "  \"entrypoint\": {\"command\": \"/bin/sh\", \"args\": [\"" +
         ava::core::json::escape(script_name) +
         "\"]},\n"
         "  \"capabilities\": [\"event_hooks\"],\n"
         "  \"contributes\": {\"event_hooks\": [{\"event\": \"" +
         ava::core::json::escape(event_name) +
         "\"}]}\n"
         "}";
}

std::string dynamic_resource_manifest_json(std::string id, std::string script_name, std::string contributes_json = "{}",
                                           std::string capabilities_json = "[\"dynamic.prompts\", \"dynamic.skills\"]")
{
  return std::string("{\n") +
         "  \"schema_version\": 1,\n"
         "  \"id\": \"" +
         ava::core::json::escape(id) +
         "\",\n"
         "  \"name\": \"Dynamic Resource Plugin\",\n"
         "  \"version\": \"0.1.0\",\n"
         "  \"api_version\": \"ava.plugin.v1\",\n"
         "  \"entrypoint\": {\"command\": \"/bin/sh\", \"args\": [\"" +
         ava::core::json::escape(script_name) +
         "\"]},\n"
         "  \"capabilities\": " +
         capabilities_json +
         ",\n"
         "  \"contributes\": " +
         contributes_json +
         "\n"
         "}";
}

// Open a trusted, sessionless runtime through the production lifecycle API for plugin command tests.
//
// The caller supplies the isolated XDG paths and workspace. Test setup failures are reported before
// returning the successfully opened Session by value.
std::optional<ava::process::ProcessScopeV1> plugin_test_process_scope()
{
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto scope = ava::process::ProcessScopeV1::application(std::move(supervisor));
  expect(scope.has_value(), scope ? "plugin test process scope is available" : "plugin test process scope is available: " + scope.error().format());
  if (!scope)
    return std::nullopt;
  return std::move(*scope);
}

ava::app::runtime::session_ts plugin_command_test_session(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace)
{
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(), trusted ? "plugin command test workspace is trusted" : "plugin command test workspace is trusted: " + trusted.error().format());

  ava::app::runtime::OpenContext context;
  context.workspace_dir = workspace;
  context.current_dir = workspace;
  context.paths = paths;
  context.application_process_scope = plugin_test_process_scope();
  auto unlocked_session_result = ava::app::runtime::Session::open(context, {.sessionless = true,
                                                                            .requested_session_id = std::nullopt,
                                                                            .fork_session_id = std::nullopt,
                                                                            .initial_session_name = std::nullopt,
                                                                            .continue_last_session = false,
                                                                            .initial_reasoning_level = std::nullopt,
                                                                            .expected_original_cwd = std::nullopt});
  expect(unlocked_session_result.has_value(),
         unlocked_session_result ? "plugin command test session opens" : "plugin command test session opens: " + unlocked_session_result.error().format());
  return std::move(*unlocked_session_result);
}

std::string command_output_text(ava::core::Result<ava::app::CommandResult> const& command)
{
  if (!command)
    return command.error().format();
  if (command->output.empty())
    return {};
  return command->output.front();
}

ava::plugin::PluginManifest runner_manifest(std::filesystem::path const& plugin_dir, std::string id, std::string script_name)
{
  auto parsed = ava::plugin::parse_plugin_manifest(runner_manifest_json(std::move(id), std::move(script_name)), plugin_dir / "plugin.json");
  expect(parsed.has_value(), parsed ? "runner plugin manifest parses" : "runner plugin manifest parses: " + parsed.error().format());
  return parsed.value_or(ava::plugin::PluginManifest{});
}

ava::plugin::PluginRunnerOptions runner_options(std::filesystem::path const& workspace, std::chrono::milliseconds startup_timeout)
{
  ava::plugin::PluginRunnerOptions options;
  options.workspace_dir = workspace;
  options.startup_timeout = startup_timeout;
  options.max_record_bytes = 64 * 1024;
  options.max_stderr_bytes = 64 * 1024;
  options.process_scope = plugin_test_process_scope();
  return options;
}

std::string nested_arrays_json(std::size_t depth)
{
  std::string text;
  text.reserve(depth * 2);
  for (std::size_t index = 0; index < depth; ++index)
    text += '[';
  for (std::size_t index = 0; index < depth; ++index)
    text += ']';
  return text;
}

std::string proxy_tool_script(std::string proxy_request_json, std::filesystem::path const& response_file, std::string call_id = "call_proxy")
{
  return "IFS= read -r line\n"
         "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
         "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
         "IFS= read -r line\n"
         "printf '%s\\n' " +
         shell_single_quote(proxy_request_json) +
         "\n"
         "IFS= read -r proxy_response\n"
         "printf '%s\\n' \"$proxy_response\" > " +
         shell_single_quote(response_file.generic_string()) +
         "\n"
         "printf '%s\\n' '{\"id\":\"ava_tool_" +
         ava::core::json::escape(call_id) +
         "\",\"type\":\"tool.result\",\"ok\":true,\"content\":\"proxy observed\",\"metadata\":{}}'\n"
         "while IFS= read -r line; do :; done\n";
}

std::string proxy_request_json(std::string id, std::string operation, std::string arguments_json)
{
  return "{\"id\":\"" + ava::core::json::escape(id) + "\",\"type\":\"proxy.request\",\"operation\":\"" + ava::core::json::escape(operation) +
         "\",\"arguments\":" + arguments_json + "}";
}

ava::tools::ToolContext plugin_proxy_test_context(std::filesystem::path const& workspace, std::filesystem::path const& project_plugins,
                                                  std::filesystem::path const& state_file, std::vector<ava::permissions::PermissionPrompt>& prompts,
                                                  std::vector<ava::tools::PermissionAuditEvent>& audits, bool& cancel_requested)
{
  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  context.plugin_global_plugins_dir = workspace.parent_path() / "global-plugins";
  context.plugin_project_plugins_dir = project_plugins;
  context.plugin_enablement_file = state_file;
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
  context.process_scope = plugin_test_process_scope();
  context.session_id = "ses_proxy_test";
  context.provider_id = "openai";
  context.model_id = "gpt-test";
  context.current_dir = workspace / "src";
  return context;
}

std::optional<std::string> proxy_response_content(std::string_view response)
{
  return ava::core::json::string_field(response, "content");
}

void test_plugin_manifest_parsing()
{
  auto parsed = ava::plugin::parse_plugin_manifest(valid_manifest_json(), "/tmp/plugin/plugin.json");
  expect(parsed.has_value(), parsed ? "plugin manifest parses valid JSON" : "plugin manifest parses valid JSON: " + parsed.error().format());
  if (parsed)
  {
    expect(parsed->id == "com.example.todo", "plugin manifest id parsed");
    expect(parsed->entrypoint.command == "node", "plugin manifest entrypoint command parsed");
    expect(parsed->entrypoint.args.size() == 2 && parsed->entrypoint.args[1] == "--safe", "plugin manifest entrypoint args parsed");
    expect(parsed->capabilities.size() == 2, "plugin manifest capabilities parsed");
    expect(ava::plugin::plugin_has_capability(*parsed, "tools") && !ava::plugin::plugin_has_capability(*parsed, "proxy.read"),
           "plugin manifest capability helper gates explicit declarations");
    expect(parsed->contributes.tools.size() == 1, "plugin manifest tool contribution parsed");
    expect(parsed->contributes.tools[0].input_schema_json.find("additionalProperties") != std::string::npos,
           "plugin manifest preserves tool input schema JSON");
    expect(parsed->contributes.commands.size() == 1 && parsed->contributes.commands[0].name == "todo", "plugin manifest command contribution parsed");
    expect(parsed->contributes.prompts.size() == 1 && parsed->contributes.prompts[0].path == "prompts/review.md", "plugin manifest prompt contribution parsed");
    expect(parsed->contributes.skills.size() == 1 && parsed->contributes.skills[0].name == "triage", "plugin manifest skill contribution parsed");
    expect(parsed->contributes.event_hooks.size() == 1 && parsed->contributes.event_hooks[0].event == "tool.result",
           "plugin manifest event hook contribution parsed");
    expect(parsed->directory == parsed->path.parent_path(), "plugin manifest records directory");
  }

  auto bad_api = ava::plugin::parse_plugin_manifest(
      "{\"schema_version\":1,\"id\":\"com.example.bad\",\"name\":\"Bad\",\"version\":\"0.1\",\"api_version\":\"wrong\","
      "\"entrypoint\":{\"command\":\"node\"}}",
      {});
  expect(!bad_api && bad_api.error().message().find("api_version") != std::string::npos, "plugin manifest rejects unsupported API versions");

  auto bad_id = ava::plugin::parse_plugin_manifest(valid_manifest_json("Bad.Id"), {});
  expect(!bad_id && bad_id.error().message().find("id") != std::string::npos, "plugin manifest rejects non-lowercase ids");

  auto bad_schema = ava::plugin::parse_plugin_manifest(
      "{\"schema_version\":1,\"id\":\"com.example.bad\",\"name\":\"Bad\",\"version\":\"0.1\",\"api_version\":\"ava."
      "plugin.v1\",\"entrypoint\":{\"command\":\"node\"},\"contributes\":{\"tools\":[{\"name\":\"bad\",\"input_"
      "schema\":{not-json}}]}}",
      {});
  expect(!bad_schema && bad_schema.error().message().find("JSON object") != std::string::npos, "plugin manifest rejects malformed tool schemas");

  auto bad_args = ava::plugin::parse_plugin_manifest(
      "{\"schema_version\":1,\"id\":\"com.example.bad\",\"name\":\"Bad\",\"version\":\"0.1\",\"api_"
      "version\":\"ava.plugin.v1\",\"entrypoint\":{\"command\":\"node\",\"args\":[{\"bad\":\"arg\"}]}}",
      {});
  expect(!bad_args && bad_args.error().message().find("only strings") != std::string::npos, "plugin manifest rejects non-string entrypoint args");

  auto bad_resource_path = ava::plugin::parse_plugin_manifest(
      "{\"schema_version\":1,\"id\":\"com.example.bad\",\"name\":\"Bad\",\"version\":\"0.1\",\"api_"
      "version\":\"ava.plugin.v1\",\"entrypoint\":{\"command\":\"node\"},\"contributes\":{\"prompts\":[{\"name\":"
      "\"bad\","
      "\"path\":\"../escape.md\"}]}}",
      {});
  expect(!bad_resource_path && bad_resource_path.error().message().find("safe relative path") != std::string::npos,
         "plugin manifest rejects prompt and skill paths that escape the plugin directory");

  std::string deeply_nested_manifest =
      "{\"schema_version\":1,\"id\":\"com.example.deep\",\"name\":\"Deep\",\"version\":\"0.1\","
      "\"api_version\":\"ava.plugin.v1\",\"entrypoint\":{\"command\":\"node\"},\"extra\":";
  for (int index = 0; index < 70; ++index)
    deeply_nested_manifest += '[';
  deeply_nested_manifest += "0";
  for (int index = 0; index < 70; ++index)
    deeply_nested_manifest += ']';
  deeply_nested_manifest += '}';
  auto deep = ava::plugin::parse_plugin_manifest(deeply_nested_manifest, {});
  expect(!deep && deep.error().message().find("maximum JSON depth") != std::string::npos,
         "plugin manifest rejects excessive JSON nesting before recursive validation");
}

void test_static_plugin_resource_loader_is_descriptor_anchored()
{
  auto const root = create_empty_root("plugin-static-resource-containment");
  auto const plugin_dir = root / "plugin";
  auto const outside = root / "outside";
  write_text(plugin_dir / "resources" / "inside.md", "inside content\n");
  write_text(outside / "secret.md", "OUTSIDE STATIC RESOURCE MUST NOT LOAD\n");

  ava::plugin::PluginManifest manifest;
  manifest.id = "com.example.static-containment";
  manifest.directory = plugin_dir;
  ava::plugin::PluginResourceContribution regular{.name = "regular", .description = "regular", .path = "resources/inside.md"};
  auto loaded = ava::plugin::load_plugin_static_resource(manifest, regular);
  expect(loaded && loaded->content == "inside content\n" && loaded->path == (plugin_dir / regular.path).lexically_normal(),
         "static plugin resource loader returns bounded content with its logical display path");

  std::error_code link_error;
  auto const plugin_alias = root / "plugin-alias";
  std::filesystem::create_directory_symlink(plugin_dir, plugin_alias, link_error);
  expect(!link_error, "static plugin resource test creates a plugin-directory symlink");
  if (!link_error)
  {
    auto alias_manifest = manifest;
    alias_manifest.directory = plugin_alias;
    auto aliased_root = ava::plugin::load_plugin_static_resource(alias_manifest, regular);
    expect(!aliased_root, "static plugin resource loader rejects a symlink replacing the plugin directory authority");
  }

  link_error.clear();
  std::filesystem::create_directory_symlink(outside, plugin_dir / "escape", link_error);
  expect(!link_error, "static plugin resource test creates escaping intermediate symlink");
  if (!link_error)
  {
    ava::plugin::PluginResourceContribution escaping{.name = "escaping", .description = "escaping", .path = "escape/secret.md"};
    auto escaped = ava::plugin::load_plugin_static_resource(manifest, escaping);
    expect(!escaped, "static plugin resource loader rejects an intermediate symlink that exits the plugin directory");
  }

  link_error.clear();
  std::filesystem::create_symlink(outside / "secret.md", plugin_dir / "resources" / "final-link.md", link_error);
  expect(!link_error, "static plugin resource test creates final symlink");
  if (!link_error)
  {
    ava::plugin::PluginResourceContribution final_link{.name = "final-link", .description = "final-link", .path = "resources/final-link.md"};
    auto linked = ava::plugin::load_plugin_static_resource(manifest, final_link);
    expect(!linked, "static plugin resource loader preserves final-symlink rejection");
  }

  auto const fifo_path = plugin_dir / "resources" / "blocking-fifo.md";
  expect(::mkfifo(fifo_path.c_str(), 0600) == 0, "static plugin resource test creates a manifest-declared FIFO");
  ava::plugin::PluginResourceContribution fifo{.name = "blocking-fifo", .description = "blocking-fifo", .path = "resources/blocking-fifo.md"};
  manifest.contributes.prompts.push_back(fifo);
  std::mutex fifo_mutex;
  std::condition_variable fifo_changed;
  bool fifo_read_finished = false;
  ava::core::JoinThread fifo_unblocker = ava::core::JoinThread::create("fifo_unblocker", [&] {
    std::unique_lock lock(fifo_mutex);
    if (fifo_changed.wait_for(lock, std::chrono::seconds(5), [&] { return fifo_read_finished; }))
      return;
    lock.unlock();
    int const writer = ::open(fifo_path.c_str(), O_WRONLY | O_CLOEXEC);
    if (writer >= 0)
      ::close(writer);
  });
  auto const fifo_start = std::chrono::steady_clock::now();
  auto loaded_fifo = ava::plugin::load_plugin_static_resource(manifest, manifest.contributes.prompts.back());
  auto const fifo_elapsed = std::chrono::steady_clock::now() - fifo_start;
  {
    std::lock_guard lock(fifo_mutex);
    fifo_read_finished = true;
  }
  fifo_changed.notify_all();
  expect(!loaded_fifo && loaded_fifo.error().message().find("regular file") != std::string::npos && fifo_elapsed < std::chrono::seconds(4),
         "static plugin resource loader rejects a FIFO as nonregular without blocking");
}

void test_sample_todo_plugin_manifest_and_resources()
{
  auto const plugin_dir = sample_todo_plugin_dir();
  auto const manifest = load_sample_todo_manifest();

  expect(manifest.id == "com.example.todo", "sample todo plugin id stays stable");
  expect(manifest.name == "Todo Sample Plugin", "sample todo plugin name is parsed");
  expect(manifest.version == "0.1.0", "sample todo plugin version is parsed");
  expect(manifest.entrypoint.command == "/bin/sh", "sample todo plugin uses explicit shell entrypoint");
  expect(manifest.entrypoint.args.size() == 1 && manifest.entrypoint.args[0] == "plugin.sh",
         "sample todo plugin entrypoint does not depend on executable mode");
  expect(manifest.contributes.tools.size() == 1 && manifest.contributes.tools[0].name == "todo_add", "sample todo plugin declares todo_add tool");
  if (!manifest.contributes.tools.empty())
  {
    expect(manifest.contributes.tools[0].input_schema_json.find("\"required\"") != std::string::npos &&
               manifest.contributes.tools[0].input_schema_json.find("\"text\"") != std::string::npos,
           "sample todo plugin tool schema requires text");
  }
  expect(manifest.contributes.commands.size() == 1 && manifest.contributes.commands[0].name == "status", "sample todo plugin declares status command");
  expect(manifest.contributes.event_hooks.size() == 1 && manifest.contributes.event_hooks[0].event == "tool.result",
         "sample todo plugin declares tool.result event hook");

  auto const has_prompt = std::any_of(
      manifest.contributes.prompts.begin(), manifest.contributes.prompts.end(),
      [](ava::plugin::PluginResourceContribution const& prompt) { return prompt.name == "todo-review" && prompt.path == "prompts/todo-review.md"; });
  auto const has_skill =
      std::any_of(manifest.contributes.skills.begin(), manifest.contributes.skills.end(),
                  [](ava::plugin::PluginResourceContribution const& skill) { return skill.name == "todo-triage" && skill.path == "skills/todo-triage.md"; });
  expect(has_prompt, "sample todo plugin declares todo-review prompt resource");
  expect(has_skill, "sample todo plugin declares todo-triage skill resource");

  auto const prompt_path = plugin_dir / "prompts" / "todo-review.md";
  auto const skill_path = plugin_dir / "skills" / "todo-triage.md";
  auto const readme_path = plugin_dir / "README.md";
  expect(std::filesystem::is_regular_file(prompt_path), "sample todo prompt resource file exists");
  expect(std::filesystem::is_regular_file(skill_path), "sample todo skill resource file exists");
  expect(read_text(prompt_path).find("Review the current work") != std::string::npos, "sample todo prompt resource has expected authoring content");
  expect(read_text(skill_path).find("Todo Triage Skill") != std::string::npos, "sample todo skill resource has expected authoring content");
  expect(read_text(readme_path).find("/plugins validate") != std::string::npos, "sample todo README documents the validation workflow");
}

void test_sample_todo_plugin_protocol_path()
{
  auto manifest = load_sample_todo_manifest();
  auto const root = create_empty_root("sample-todo-plugin");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  auto options = runner_options(workspace, std::chrono::milliseconds(500));
  options.request_timeout = std::chrono::milliseconds(500);
  auto process = ava::plugin::PluginProcess::start(std::move(manifest), options);
  expect(process.has_value(), process ? "sample todo plugin initializes through actual entrypoint"
                                      : "sample todo plugin initializes through actual entrypoint: " + process.error().format());
  if (!process)
    return;

  expect((*process)->initialization().plugin_version == "0.1.0", "sample todo plugin reports the manifest version during initialize");
  expect((*process)->initialization().contributions_json.find("\"tools\":[]") != std::string::npos,
         "sample todo plugin keeps dynamic initialize contributions empty");

  auto command = (*process)->call_command("status", "{}", "sample_status");
  expect(command && command->ok && command->content == "Todo sample plugin is ready." && command->metadata_json.find("\"open_items\":0") != std::string::npos,
         command ? "sample todo plugin exchanges command.call/command.result records"
                 : "sample todo plugin exchanges command.call/command.result records: " + command.error().format());

  auto tool = (*process)->call_tool("todo_add", "{\"text\":\"write tests\"}", "sample_tool");
  expect(tool && tool->ok && tool->content == "Todo item accepted by the sample plugin." && tool->metadata_json.find("\"items\":1") != std::string::npos,
         tool ? "sample todo plugin exchanges tool.call/tool.result records"
              : "sample todo plugin exchanges tool.call/tool.result records: " + tool.error().format());

  auto event = (*process)->observe_event("tool_result", "{\"tool\":\"demo\",\"status\":\"success\"}", "sample_event");
  expect(event && event->ok && event->content == "Todo sample observed the event." && event->metadata_json.find("\"events\":1") != std::string::npos,
         event ? "sample todo plugin exchanges event.observe/event.observed records"
               : "sample todo plugin exchanges event.observe/event.observed records: " + event.error().format());

  auto shutdown = (*process)->shutdown(std::chrono::milliseconds(500));
  expect(shutdown.has_value(), shutdown ? "sample todo plugin shuts down cleanly" : "sample todo plugin shuts down cleanly: " + shutdown.error().format());
}

void test_plugin_runner_protocol_parsing()
{
  auto initialized = ava::plugin::parse_initialized_response(
      "{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava.plugin.v1\",\"plugin_version\":\"0.1.0\","
      "\"contributions\":{\"tools\":[],\"commands\":[]}}");
  expect(initialized && initialized->plugin_version == "0.1.0" && initialized->contributions_json.find("\"tools\":[]") != std::string::npos,
         "plugin runner protocol parses initialization responses");

  auto unsupported = ava::plugin::parse_initialized_response(
      "{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"wrong\",\"plugin_version\":\"0.1.0\","
      "\"contributions\":{}}");
  expect(!unsupported, "plugin runner protocol rejects unsupported API versions");

  auto tool_result = ava::plugin::parse_tool_result_response(
      "{\"id\":\"ava_tool_call_1\",\"type\":\"tool.result\",\"ok\":true,\"content\":\"done\","
      "\"metadata\":{\"count\":1}}",
      "ava_tool_call_1");
  expect(tool_result && tool_result->ok && tool_result->content == "done" && tool_result->metadata_json.find("\"count\":1") != std::string::npos,
         "plugin runner protocol parses tool result metadata");

  auto mismatched_tool_result =
      ava::plugin::parse_tool_result_response("{\"id\":\"ava_tool_other\",\"type\":\"tool.result\",\"ok\":true,\"content\":\"done\"}", "ava_tool_call_1");
  expect(!mismatched_tool_result, "plugin runner protocol rejects mismatched response IDs");

  constexpr std::string_view failure_canary = "CANARY_PLUGIN_FAILURE_CONTENT_3e7a";
  auto command_result = ava::plugin::parse_command_result_response(
      "{\"id\":\"ava_command_1\",\"type\":\"command.result\",\"ok\":false,\"content\":\"CANARY_PLUGIN_FAILURE_CONTENT_3e7a\","
      "\"metadata\":{\"secret\":\"CANARY_PLUGIN_FAILURE_METADATA_80b2\"}}",
      "ava_command_1");
  expect(command_result && !command_result->ok && command_result->content.find("external_failure") != std::string::npos &&
             command_result->content.find(failure_canary) == std::string::npos && command_result->metadata_json.empty() && command_result->raw_json.empty(),
         "plugin runner protocol replaces failed content, metadata, and raw JSON with SafeFailure");

  auto event_result = ava::plugin::parse_event_observed_response("{\"id\":\"ava_event_1\",\"type\":\"event.observed\",\"ok\":true}", "ava_event_1");
  expect(event_result && event_result->ok && event_result->content.empty(), "plugin runner protocol treats missing event content as empty text");

  auto proxy_request =
      ava::plugin::parse_proxy_request("{\"id\":\"px_1\",\"type\":\"proxy.request\",\"operation\":\"file.read\",\"arguments\":{\"path\":\"README.md\"}}");
  expect(proxy_request && proxy_request->id == "px_1" && proxy_request->operation == "file.read" &&
             proxy_request->arguments_json.find("README.md") != std::string::npos,
         "plugin runner protocol parses proxy.request records");

  auto malformed_proxy_request =
      ava::plugin::parse_proxy_request("{\"id\":\"px_bad\",\"type\":\"proxy.request\",\"operation\":\"file.read\",\"arguments\":[]}");
  expect(!malformed_proxy_request, "plugin runner protocol rejects proxy.request records without object arguments");

  auto too_deep = ava::plugin::parse_tool_result_response(
      "{\"id\":\"ava_tool_deep\",\"type\":\"tool.result\",\"ok\":true,\"content\":\"x\",\"metadata\":" + nested_arrays_json(130) + "}", "ava_tool_deep");
  expect(!too_deep, "plugin runner protocol rejects excessively deep JSON records");

  auto resource_list = ava::plugin::parse_resource_list_result_response(
      "{\"id\":\"ava_resource_1\",\"type\":\"resource.list.result\",\"ok\":true,\"kind\":\"prompt\","
      "\"resources\":[{\"name\":\"dyn-one\",\"description\":\"demo\"}]}",
      "ava_resource_1", ava::plugin::PluginDynamicResourceKind::Prompt);
  expect(resource_list && resource_list->ok && resource_list->resources.size() == 1 && resource_list->resources[0].name == "dyn-one",
         "plugin runner protocol parses dynamic resource list responses");

  auto resource_list_invalid_name = ava::plugin::parse_resource_list_result_response(
      "{\"id\":\"ava_resource_1\",\"type\":\"resource.list.result\",\"ok\":true,\"kind\":\"prompt\","
      "\"resources\":[{\"name\":\"../bad\",\"description\":\"demo\"}]}",
      "ava_resource_1", ava::plugin::PluginDynamicResourceKind::Prompt);
  expect(!resource_list_invalid_name, "plugin runner protocol rejects invalid dynamic resource names from plugins");

  auto resource_list_missing_resources =
      ava::plugin::parse_resource_list_result_response("{\"id\":\"ava_resource_1\",\"type\":\"resource.list.result\",\"ok\":true,\"kind\":\"prompt\"}",
                                                       "ava_resource_1", ava::plugin::PluginDynamicResourceKind::Prompt);
  expect(!resource_list_missing_resources, "plugin runner protocol rejects ok dynamic resource lists without resources arrays");

  auto long_description = std::string(4097, 'd');
  auto resource_list_long_description = ava::plugin::parse_resource_list_result_response(
      "{\"id\":\"ava_resource_1\",\"type\":\"resource.list.result\",\"ok\":true,\"kind\":\"prompt\","
      "\"resources\":[{\"name\":\"dyn-one\",\"description\":\"" +
          long_description + "\"}]}",
      "ava_resource_1", ava::plugin::PluginDynamicResourceKind::Prompt);
  expect(!resource_list_long_description, "plugin runner protocol rejects oversized dynamic resource descriptions");

  auto resource_list_false_missing_content =
      ava::plugin::parse_resource_list_result_response("{\"id\":\"ava_resource_1\",\"type\":\"resource.list.result\",\"ok\":false,\"kind\":\"prompt\"}",
                                                       "ava_resource_1", ava::plugin::PluginDynamicResourceKind::Prompt);
  expect(!resource_list_false_missing_content, "plugin runner protocol rejects dynamic list failures without content");

  auto resource_read = ava::plugin::parse_resource_read_result_response(
      "{\"id\":\"ava_resource_2\",\"type\":\"resource.read.result\",\"ok\":true,\"kind\":\"prompt\",\"name\":\"dyn-one\","
      "\"content\":\"body\"}",
      "ava_resource_2", ava::plugin::PluginDynamicResourceKind::Prompt, "dyn-one");
  expect(resource_read && resource_read->ok && resource_read->content == "body", "plugin runner protocol parses dynamic resource read responses");

  auto resource_read_name_mismatch = ava::plugin::parse_resource_read_result_response(
      "{\"id\":\"ava_resource_2\",\"type\":\"resource.read.result\",\"ok\":true,\"kind\":\"prompt\",\"name\":\"other\","
      "\"content\":\"body\"}",
      "ava_resource_2", ava::plugin::PluginDynamicResourceKind::Prompt, "dyn-one");
  expect(!resource_read_name_mismatch, "plugin runner protocol rejects dynamic resource read name mismatches");

  auto resource_read_kind_mismatch = ava::plugin::parse_resource_read_result_response(
      "{\"id\":\"ava_resource_2\",\"type\":\"resource.read.result\",\"ok\":true,\"kind\":\"skill\",\"name\":\"dyn-one\","
      "\"content\":\"body\"}",
      "ava_resource_2", ava::plugin::PluginDynamicResourceKind::Prompt, "dyn-one");
  expect(!resource_read_kind_mismatch, "plugin runner protocol rejects dynamic resource read kind mismatches");

  auto oversized_content = std::string(ava::plugin::kPluginResourceContentMaxBytes + 1, 'x');
  auto resource_read_oversized_content = ava::plugin::parse_resource_read_result_response(
      "{\"id\":\"ava_resource_2\",\"type\":\"resource.read.result\",\"ok\":true,\"kind\":\"prompt\",\"name\":\"dyn-one\","
      "\"content\":\"" +
          oversized_content + "\"}",
      "ava_resource_2", ava::plugin::PluginDynamicResourceKind::Prompt, "dyn-one");
  expect(!resource_read_oversized_content, "plugin runner protocol rejects oversized dynamic resource read content");
}

void test_plugin_discovery()
{
  auto const root = create_empty_root("plugins");

  auto const workspace = root / "workspace";
  auto const global_manifest = root / "config" / "ava" / "plugins" / "com.example.global" / "plugin.json";
  auto const project_manifest = workspace / ".ava" / "plugins" / "com.example.project" / "plugin.json";
  write_text(global_manifest, valid_manifest_json("com.example.global"));
  write_text(project_manifest, valid_manifest_json("com.example.project"));
  write_text(workspace / ".ava" / "plugins" / "com.example.project" / "should_not_run", "not executable");

  auto discovered = ava::plugin::discover_plugins(ava::plugin::PluginDiscoveryOptions{
      .global_plugins_dir = root / "config" / "ava" / "plugins",
      .project_plugins_dir = workspace / ".ava" / "plugins",
  });
  expect(discovered.has_value(), discovered ? "plugin discovery loads global and project manifests"
                                            : "plugin discovery loads global and project manifests: " + discovered.error().format());
  if (discovered)
  {
    expect(discovered->size() == 2, "plugin discovery finds two manifests");
    bool const has_global = std::any_of(discovered->begin(), discovered->end(), [](ava::plugin::DiscoveredPlugin const& plugin) {
      return plugin.manifest.id == "com.example.global" && plugin.scope == ava::plugin::PluginScope::Global;
    });
    bool const has_project = std::any_of(discovered->begin(), discovered->end(), [](ava::plugin::DiscoveredPlugin const& plugin) {
      return plugin.manifest.id == "com.example.project" && plugin.scope == ava::plugin::PluginScope::Project;
    });
    expect(has_global && has_project, "plugin discovery preserves plugin scope");
  }

  auto const duplicate_root = root / "duplicates";
  write_text(duplicate_root / "global" / "com.example.same" / "plugin.json", valid_manifest_json("com.example.same"));
  write_text(duplicate_root / "project" / "com.example.same" / "plugin.json", valid_manifest_json("com.example.same"));
  auto duplicate = ava::plugin::discover_plugins(ava::plugin::PluginDiscoveryOptions{
      .global_plugins_dir = duplicate_root / "global",
      .project_plugins_dir = duplicate_root / "project",
  });
  expect(!duplicate && duplicate.error().message().find("duplicate") != std::string::npos, "plugin discovery rejects duplicate plugin ids across scopes");

  auto const symlink_root = root / "symlink";
  auto const outside = symlink_root / "outside";
  write_text(outside / "plugin.json", valid_manifest_json("com.example.symlinked"));
  std::filesystem::create_directories(symlink_root / "global");
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(outside, symlink_root / "global" / "com.example.symlinked", symlink_error);
  if (!symlink_error)
  {
    std::error_code broken_symlink_error;
    std::filesystem::create_directory_symlink(symlink_root / "missing", symlink_root / "global" / "com.example.broken", broken_symlink_error);
    auto skipped = ava::plugin::discover_plugins(ava::plugin::PluginDiscoveryOptions{
        .global_plugins_dir = symlink_root / "global",
        .project_plugins_dir = {},
    });
    expect(skipped && skipped->empty(), "plugin discovery skips symlinked plugin directories");
  }

  auto const oversized = root / "oversized" / "plugin.json";
  write_text(oversized, std::string(300 * 1024, '{'));
  auto oversized_manifest = ava::plugin::load_plugin_manifest(oversized);
  expect(!oversized_manifest && oversized_manifest.error().message().find("maximum size") != std::string::npos,
         "plugin manifest load rejects oversized files before parsing");

  auto const directory_manifest = root / "directory-manifest" / "plugin.json";
  std::filesystem::create_directories(directory_manifest);
  auto non_regular_manifest = ava::plugin::load_plugin_manifest(directory_manifest);
  expect(!non_regular_manifest && non_regular_manifest.error().message().find("regular file") != std::string::npos,
         "plugin manifest load rejects non-regular files");
}

void test_plugin_enablement()
{
  auto const root = create_empty_root("plugin-enable");

  auto const workspace = root / "workspace";
  auto const state_file = root / "state" / "ava" / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  auto initially_enabled = ava::plugin::plugin_enabled(state_file, workspace, "com.example.todo");
  expect(initially_enabled && !*initially_enabled, "plugins are disabled by default");

  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.todo", true);
  expect(enabled.has_value(), "plugin enablement state can be written");
  auto after_enable = ava::plugin::plugin_enabled(state_file, workspace, "com.example.todo");
  expect(after_enable && *after_enable, "plugin enablement state can be read");

  auto disabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.todo", false);
  expect(disabled.has_value(), "plugin disablement state can be written");
  auto after_disable = ava::plugin::plugin_enabled(state_file, workspace, "com.example.todo");
  expect(after_disable && !*after_disable, "plugin disablement state can be read");

  auto records = ava::plugin::load_plugin_enablement(state_file);
  expect(records && records->size() == 1, "plugin enablement file keeps one workspace/plugin record");
  if (records && !records->empty())
  {
    expect(records->front().workspace == ava::plugin::canonical_workspace_key(workspace), "plugin enablement stores canonical workspace key");
    expect(records->front().scope == ava::plugin::PluginScope::Project, "plugin enablement stores plugin scope");
  }

  auto global_enable = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.todo", true, ava::plugin::PluginScope::Global);
  expect(global_enable.has_value(), "global plugin enablement can be stored separately");
  auto project_state = ava::plugin::plugin_enabled(state_file, workspace, "com.example.todo", ava::plugin::PluginScope::Project);
  auto global_state = ava::plugin::plugin_enabled(state_file, workspace, "com.example.todo", ava::plugin::PluginScope::Global);
  expect(project_state && !*project_state && global_state && *global_state, "global and project plugin enablement are isolated");

  auto invalid_id = ava::plugin::set_plugin_enabled(state_file, workspace, "Bad.Id", true);
  expect(!invalid_id && invalid_id.error().message().find("plugin id") != std::string::npos, "plugin enablement rejects invalid plugin ids");

  auto const malformed_state = root / "state" / "ava" / "malformed-plugin-enablement.json";
  write_text(malformed_state, "{\"workspaces\":{\"/tmp/work\":{\"project\":{\"com.example.todo\":{\"enabled\":truefalse}}}}}");
  auto malformed = ava::plugin::load_plugin_enablement(malformed_state);
  expect(!malformed && malformed.error().message().find("valid JSON") != std::string::npos,
         "plugin enablement rejects malformed JSON instead of partial parsing");

  auto const escaped_state = root / "state" / "ava" / "escaped-plugin-enablement.json";
  write_text(escaped_state, "{\"workspaces\":{\"/tmp/work{\\\"q\\\"}\":{\"project\":{\"com.example.todo\":{\"enabled\":true}}}}}");
  auto escaped = ava::plugin::load_plugin_enablement(escaped_state);
  expect(escaped && escaped->size() == 1 && escaped->front().workspace == std::filesystem::path("/tmp/work{\"q\"}") && escaped->front().enabled,
         "plugin enablement parses escaped keys and braces inside strings");

  auto const fifo_state = root / "state" / "ava" / "fifo-plugin-enablement.json";
  auto const fifo_created = ::mkfifo(fifo_state.c_str(), 0600) == 0;
  auto const fifo_started = std::chrono::steady_clock::now();
  auto fifo_enabled = ava::plugin::plugin_enabled(fifo_state, workspace, "com.example.todo");
  auto const fifo_elapsed = std::chrono::steady_clock::now() - fifo_started;
  expect(fifo_created && !fifo_enabled && fifo_elapsed < std::chrono::milliseconds(500),
         "plugin enablement rejects a FIFO without blocking an active revocation predicate");

  auto const oversized_state = root / "state" / "ava" / "oversized-plugin-enablement.json";
  write_text(oversized_state, std::string(1024 * 1024 + 1, ' '));
  auto oversized_enabled = ava::plugin::plugin_enabled(oversized_state, workspace, "com.example.todo");
  expect(!oversized_enabled, "plugin enablement reads are size-bounded and fail closed");
}

void test_plugin_runner_initializes_and_shuts_down()
{
  auto const root = create_empty_root("plugin-runner");

  auto const workspace = root / "workspace";
  auto const plugin_dir = root / "plugins" / "com.example.runner";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' \"$line\" > init.txt\n"
             "printf '%s\\n' '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ' >&2\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{\"tools\":[],\"commands\":[],"
             "\"prompts\":[],\"event_hooks\":[]}}'\n"
             "while read line; do :; done\n"
             "printf '%s\\n' stopped > stopped.txt\n");

  auto manifest = runner_manifest(plugin_dir, "com.example.runner", "plugin.sh");
  auto options = runner_options(workspace, std::chrono::milliseconds(500));
  options.max_stderr_bytes = 16;
  auto process = ava::plugin::PluginProcess::start(std::move(manifest), options);
  expect(process.has_value(), process ? "plugin runner initializes fake plugin" : "plugin runner initializes fake plugin: " + process.error().format());
  if (process)
  {
    expect((*process)->initialization().plugin_version == "0.1.0", "plugin runner records handshake version");
    expect((*process)->initialization().contributions_json.find("\"tools\":[]") != std::string::npos, "plugin runner records handshake contributions");
    expect((*process)->stderr_truncated(), "plugin runner bounds stderr diagnostics");
    expect((*process)->stderr_tail().size() == 16, "plugin runner keeps stderr tail");
    auto const init = read_text(plugin_dir / "init.txt");
    expect(init.find("\"type\":\"initialize\"") != std::string::npos && init.find("\"plugin_id\":\"com.example.runner\"") != std::string::npos &&
               init.find(workspace.string()) != std::string::npos,
           "plugin runner sends initialize record with plugin and workspace");
    auto shutdown = (*process)->shutdown(std::chrono::milliseconds(500));
    expect(shutdown.has_value(), shutdown ? "plugin runner shuts down cleanly" : "plugin runner shuts down cleanly: " + shutdown.error().format());
    expect(read_text(plugin_dir / "stopped.txt").find("stopped") != std::string::npos, "plugin runner closes stdin so fake plugin can exit cleanly");
  }
}

void test_plugin_runner_rejects_oversized_buffered_extra_records()
{
  auto const root = create_empty_root("plugin-runner-buffered");

  auto const workspace = root / "workspace";
  auto const plugin_dir = root / "plugins" / "com.example.buffered";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(plugin_dir);

  auto manifest = runner_manifest(plugin_dir, "com.example.buffered", "unused");
  manifest.entrypoint.command = AVA_FAKE_PLUGIN_CHILD_PATH;
  manifest.entrypoint.args = {"oversized-buffered-initialize"};
  auto options = runner_options(workspace, std::chrono::milliseconds(500));
  options.max_record_bytes = 256;
  auto process = ava::plugin::PluginProcess::start(std::move(manifest), options);
  expect(!process && process.error().format().find("output_limit: true") != std::string::npos,
         process ? "plugin runner rejects every oversized complete record already buffered after initialize"
                 : "plugin runner rejects every oversized complete record already buffered after initialize: " + process.error().format());
}

void test_plugin_runner_contained_failures()
{
  auto const root = create_empty_root("plugin-runner-failures");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  auto const malformed_dir = root / "plugins" / "com.example.malformed";
  write_text(malformed_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' 'not-json'\n");
  auto malformed_options = runner_options(workspace, std::chrono::milliseconds(500));
  auto malformed = ava::plugin::PluginProcess::start(runner_manifest(malformed_dir, "com.example.malformed", "plugin.sh"), malformed_options);
  expect(!malformed && malformed.error().message().find("malformed") != std::string::npos, "plugin runner rejects malformed initialize records");

  auto const unsupported_dir = root / "plugins" / "com.example.unsupported";
  write_text(unsupported_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"wrong\","
             "\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n");
  auto unsupported_options = runner_options(workspace, std::chrono::milliseconds(500));
  auto unsupported = ava::plugin::PluginProcess::start(runner_manifest(unsupported_dir, "com.example.unsupported", "plugin.sh"), unsupported_options);
  expect(!unsupported && unsupported.error().message().find("unsupported") != std::string::npos, "plugin runner rejects unsupported handshake API versions");

  std::string deep_contributions = "{}";
  for (int i = 0; i < 160; ++i)
    deep_contributions = "{\"x\":" + deep_contributions + "}";
  auto const deep_dir = root / "plugins" / "com.example.deep";
  write_text(deep_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":" +
                 deep_contributions + "}'\n");
  auto deep_options = runner_options(workspace, std::chrono::milliseconds(500));
  auto deep = ava::plugin::PluginProcess::start(runner_manifest(deep_dir, "com.example.deep", "plugin.sh"), deep_options);
  expect(!deep && deep.error().message().find("malformed") != std::string::npos, "plugin runner rejects deeply nested initialize JSON");

  auto const timeout_dir = root / "plugins" / "com.example.timeout";
  write_text(timeout_dir / "plugin.sh", "sleep 2\n");
  auto timeout_options = runner_options(workspace, std::chrono::milliseconds(100));
  auto timed_out = ava::plugin::PluginProcess::start(runner_manifest(timeout_dir, "com.example.timeout", "plugin.sh"), timeout_options);
  expect(!timed_out && timed_out.error().message().find("timed out") != std::string::npos, "plugin runner times out hung startup");

  auto const startup_cancel_dir = root / "plugins" / "com.example.startupcancel";
  auto const startup_cancel_pgid_file = startup_cancel_dir / "startup-cancel-pgid.txt";
  write_text(startup_cancel_dir / "plugin.sh", "printf '%s\\n' $$ > " + shell_single_quote(startup_cancel_pgid_file.generic_string()) + "\nsleep 30\n");
  auto startup_cancel_options = runner_options(workspace, std::chrono::milliseconds(1000));
  auto startup_canceled =
      ava::plugin::PluginProcess::start(runner_manifest(startup_cancel_dir, "com.example.startupcancel", "plugin.sh"), startup_cancel_options,
                                        [&] { return read_pid_file_for_test(startup_cancel_pgid_file).has_value(); });
  auto const startup_cancel_pgid = read_pid_file_for_test(startup_cancel_pgid_file);
  auto const startup_cancel_category = startup_canceled ? std::string{} : ava::core::to_string(startup_canceled.error().category());
  std::string const startup_cancel_message = startup_canceled ? std::string{} : startup_canceled.error().message();
  std::string const startup_cancel_format = startup_canceled ? std::string{} : startup_canceled.error().format();
  bool const startup_cancel_group_exited = startup_cancel_pgid && wait_for_process_group_exit(*startup_cancel_pgid);
  expect(!startup_canceled, "canceled plugin startup returns a failed result instead of a process");
  expect(!startup_canceled && startup_cancel_message.find("canceled") != std::string::npos && startup_cancel_format.find("canceled: true") != std::string::npos,
         "canceled plugin startup keeps a stable cancellation category, message, and context [category=" + startup_cancel_category +
             "]: " + startup_cancel_format);
  expect(startup_cancel_pgid.has_value(), "canceled plugin startup child publishes its process-group marker");
  expect(startup_cancel_group_exited, "canceled plugin startup terminates the plugin process group before timeout");

  auto const exited_dir = root / "plugins" / "com.example.exited";
  write_text(exited_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' 'CANARY_PLUGIN_STDERR_INIT_2f91' >&2\n"
             "exit 7\n");
  auto exited_options = runner_options(workspace, std::chrono::milliseconds(500));
  auto exited = ava::plugin::PluginProcess::start(runner_manifest(exited_dir, "com.example.exited", "plugin.sh"), exited_options);
  auto const exited_format = exited ? std::string{} : exited.error().format();
  expect(!exited && exited_format.find("exit 7") != std::string::npos && exited_format.find("CANARY_PLUGIN_STDERR_INIT_2f91") == std::string::npos &&
             exited_format.find("stderr_bytes") != std::string::npos,
         "plugin runner reports early process exit status and safe stderr metadata: " + exited_format);

  auto const oversized_dir = root / "plugins" / "com.example.oversized";
  write_text(oversized_dir / "plugin.sh", "read line\nprintf '%s\\n' '" + std::string(200, 'x') + "'\n");
  auto oversized_options = runner_options(workspace, std::chrono::milliseconds(500));
  oversized_options.max_record_bytes = 64;
  auto oversized = ava::plugin::PluginProcess::start(runner_manifest(oversized_dir, "com.example.oversized", "plugin.sh"), oversized_options);
  expect(!oversized && oversized.error().message().find("size cap") != std::string::npos, "plugin runner rejects oversized protocol records");

  auto const flood_dir = root / "plugins" / "com.example.flood";
  write_text(flood_dir / "plugin.sh",
             "read line\n"
             "while :; do printf x; done\n");
  auto flood_options = runner_options(workspace, std::chrono::milliseconds(500));
  flood_options.max_record_bytes = 64;
  auto flood = ava::plugin::PluginProcess::start(runner_manifest(flood_dir, "com.example.flood", "plugin.sh"), flood_options);
  expect(!flood && flood.error().message().find("size cap") != std::string::npos, "plugin runner bounds newline-free stdout floods");
}

void test_plugin_runner_tool_calls()
{
  auto const root = create_empty_root("plugin-runner-tools");

  auto const workspace = root / "workspace";
  auto const plugin_dir = root / "plugins" / "com.example.toolrunner";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "read line\n"
             "printf '%s\\n' \"$line\" > tool-request.txt\n"
             "printf '%s\\n' '{\"id\":\"ava_tool_call_1\",\"type\":\"tool.result\",\"ok\":true,"
             "\"content\":\"Added todo\",\"metadata\":{\"count\":1}}'\n"
             "while read line; do :; done\n");

  auto process = ava::plugin::PluginProcess::start(runner_manifest(plugin_dir, "com.example.toolrunner", "plugin.sh"),
                                                   runner_options(workspace, std::chrono::milliseconds(500)));
  expect(process.has_value(), process ? "plugin runner starts before tool call" : "plugin runner starts before tool call: " + process.error().format());
  if (process)
  {
    auto result = (*process)->call_tool("todo_add", "{\"text\":\"write tests\"}", "call_1");
    expect(
        result && result->ok && result->content == "Added todo" && result->metadata_json.find("\"count\":1") != std::string::npos,
        result ? "plugin runner exchanges tool.call/tool.result records" : "plugin runner exchanges tool.call/tool.result records: " + result.error().format());
    auto const request = read_text(plugin_dir / "tool-request.txt");
    expect(request.find("\"type\":\"tool.call\"") != std::string::npos && request.find("\"tool\":\"todo_add\"") != std::string::npos &&
               request.find("\"text\":\"write tests\"") != std::string::npos && request.find(workspace.string()) != std::string::npos,
           "plugin runner sends bounded tool call request context");
    auto pre_canceled = (*process)->call_tool("todo_add", "{}", "pre_canceled", [] { return true; });
    expect(
        !pre_canceled && pre_canceled.error().code() == ava::core::ErrorCode::Canceled && pre_canceled.error().message().find("canceled") != std::string::npos,
        "plugin runner maps cancellation before a tool call to a deterministic typed canceled error");
    auto shutdown = (*process)->shutdown(std::chrono::milliseconds(500));
    expect(shutdown.has_value(), "plugin runner shuts down after tool call");
  }

  auto const malformed_dir = root / "plugins" / "com.example.toolmalformed";
  write_text(malformed_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_tool_bad\",\"type\":\"tool.result\",\"ok\":true,"
             "\"content\":{\"secret\":\"CANARY_PLUGIN_MALFORMED_RESPONSE_d8f5\"}}'\n");
  auto malformed = ava::plugin::PluginProcess::start(runner_manifest(malformed_dir, "com.example.toolmalformed", "plugin.sh"),
                                                     runner_options(workspace, std::chrono::milliseconds(500)));
  expect(malformed.has_value(), "plugin runner starts malformed-result fake plugin");
  if (malformed)
  {
    auto result = (*malformed)->call_tool("todo_add", "{}", "bad");
    expect(!result && result.error().message().find("malformed") != std::string::npos &&
               result.error().format().find("CANARY_PLUGIN_MALFORMED_RESPONSE_d8f5") == std::string::npos,
           "plugin runner rejects malformed tool results without retaining raw response data");
  }

  auto const timeout_dir = root / "plugins" / "com.example.tooltimeout";
  write_text(timeout_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "read line\n"
             "sleep 2\n");
  auto timeout_options = runner_options(workspace, std::chrono::milliseconds(500));
  timeout_options.request_timeout = std::chrono::milliseconds(100);
  auto timeout = ava::plugin::PluginProcess::start(runner_manifest(timeout_dir, "com.example.tooltimeout", "plugin.sh"), timeout_options);
  expect(timeout.has_value(), "plugin runner starts timeout fake plugin");
  if (timeout)
  {
    auto result = (*timeout)->call_tool("todo_add", "{}", "slow");
    expect(!result && result.error().message().find("timed out") != std::string::npos, "plugin runner times out hung tool calls independently from startup");
  }

  auto const cancel_dir = root / "plugins" / "com.example.toolcancel";
  auto const cancel_pgid_file = cancel_dir / "tool-cancel-pgid.txt";
  write_text(cancel_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "read line\n"
             "printf '%s\\n' $$ > " +
                 shell_single_quote(cancel_pgid_file.generic_string()) + "\nsleep 30 &\nwait\n");
  auto cancel_options = runner_options(workspace, std::chrono::milliseconds(500));
  cancel_options.request_timeout = std::chrono::milliseconds(1000);
  auto cancel = ava::plugin::PluginProcess::start(runner_manifest(cancel_dir, "com.example.toolcancel", "plugin.sh"), cancel_options);
  expect(cancel.has_value(), "plugin runner starts cancellation fake plugin");
  if (cancel)
  {
    auto result = (*cancel)->call_tool("todo_add", "{}", "cancel", [&] { return read_pid_file_for_test(cancel_pgid_file).has_value(); });
    auto const cancel_pgid = read_pid_file_for_test(cancel_pgid_file);
    expect(!result && result.error().message().find("canceled") != std::string::npos && cancel_pgid && wait_for_process_group_exit(*cancel_pgid),
           "plugin runner cancels hung tool calls and terminates the plugin process group before timeout");
  }

  auto const crashed_dir = root / "plugins" / "com.example.toolcrash";
  write_text(crashed_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "read line\n"
             "printf '%s\\n' 'CANARY_PLUGIN_STDERR_TOOL_61c8' >&2\n"
             "exit 9\n");
  auto crashed = ava::plugin::PluginProcess::start(runner_manifest(crashed_dir, "com.example.toolcrash", "plugin.sh"),
                                                   runner_options(workspace, std::chrono::milliseconds(500)));
  expect(crashed.has_value(), "plugin runner starts crash fake plugin");
  if (crashed)
  {
    auto result = (*crashed)->call_tool("todo_add", "{}", "crash");
    auto const result_format = result ? std::string{} : result.error().format();
    expect(!result && result_format.find("exit 9") != std::string::npos && result_format.find("CANARY_PLUGIN_STDERR_TOOL_61c8") == std::string::npos &&
               result_format.find("stderr_bytes") != std::string::npos,
           "plugin runner reports tool-time process exits with safe stderr metadata: " + result_format);
  }
}

void test_plugin_runner_command_calls()
{
  auto const root = create_empty_root("plugin-runner-commands");

  auto const workspace = root / "workspace";
  auto const plugin_dir = root / "plugins" / "com.example.commandrunner";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "read line\n"
             "printf '%s\\n' \"$line\" > command-request.txt\n"
             "printf '%s\\n' '{\"id\":\"ava_command_cmd_1\",\"type\":\"command.result\",\"ok\":true,"
             "\"content\":\"Command complete\",\"metadata\":{\"count\":2}}'\n"
             "while read line; do :; done\n");

  auto process = ava::plugin::PluginProcess::start(runner_manifest(plugin_dir, "com.example.commandrunner", "plugin.sh"),
                                                   runner_options(workspace, std::chrono::milliseconds(500)));
  expect(process.has_value(), process ? "plugin runner starts before command call" : "plugin runner starts before command call: " + process.error().format());
  if (process)
  {
    auto result = (*process)->call_command("todo", "{\"filter\":\"open\"}", "cmd_1");
    expect(result && result->ok && result->content == "Command complete" && result->metadata_json.find("\"count\":2") != std::string::npos,
           result ? "plugin runner exchanges command.call/command.result records"
                  : "plugin runner exchanges command.call/command.result records: " + result.error().format());
    auto const request = read_text(plugin_dir / "command-request.txt");
    expect(request.find("\"type\":\"command.call\"") != std::string::npos && request.find("\"command\":\"todo\"") != std::string::npos &&
               request.find("\"filter\":\"open\"") != std::string::npos && request.find(workspace.string()) != std::string::npos,
           "plugin runner sends bounded command call request context");
    auto shutdown = (*process)->shutdown(std::chrono::milliseconds(500));
    expect(shutdown.has_value(), "plugin runner shuts down after command call");
  }
}

void test_plugin_runner_event_observation()
{
  auto const root = create_empty_root("plugin-runner-events");

  auto const workspace = root / "workspace";
  auto const plugin_dir = root / "plugins" / "com.example.eventrunner";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.sh",
             "read line\n"
             "printf '%s\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "read line\n"
             "printf '%s\n' \"$line\" > event-request.txt\n"
             "printf '%s\n' '{\"id\":\"ava_event_event_1\",\"type\":\"event.observed\",\"ok\":true,"
             "\"content\":\"observed\",\"metadata\":{\"count\":1}}'\n"
             "while read line; do :; done\n");

  auto process = ava::plugin::PluginProcess::start(runner_manifest(plugin_dir, "com.example.eventrunner", "plugin.sh"),
                                                   runner_options(workspace, std::chrono::milliseconds(500)));
  expect(process.has_value(),
         process ? "plugin runner starts before event observation" : "plugin runner starts before event observation: " + process.error().format());
  if (process)
  {
    auto result = (*process)->observe_event("tool_result", "{\"tool\":\"demo\"}", "event_1");
    expect(result && result->ok && result->content == "observed" && result->metadata_json.find("\"count\":1") != std::string::npos,
           result ? "plugin runner exchanges event.observe/event.observed records"
                  : "plugin runner exchanges event.observe/event.observed records: " + result.error().format());
    auto const request = read_text(plugin_dir / "event-request.txt");
    expect(request.find("\"type\":\"event.observe\"") != std::string::npos && request.find("\"event\":\"tool_result\"") != std::string::npos &&
               request.find("\"tool\":\"demo\"") != std::string::npos && request.find(workspace.string()) != std::string::npos,
           "plugin runner sends bounded event observation request context");
    auto shutdown = (*process)->shutdown(std::chrono::milliseconds(500));
    expect(shutdown.has_value(), "plugin runner shuts down after event observation");
  }
}

void test_enabled_plugin_dynamic_resources_list_and_read()
{
  auto const root = create_empty_root("plugin-dynamic-resources");

  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  auto const project_plugins = workspace / ".ava" / "plugins";
  auto const plugin_dir = project_plugins / "com.example.dynamic";
  auto const state_file = paths.ava_state_dir / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json", dynamic_resource_manifest_json("com.example.dynamic", "plugin.sh"));
  write_text(plugin_dir / "plugin.sh",
             "IFS= read -r line\n"
             "printf '%s\\n' \"$line\" >> requests.txt\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "IFS= read -r request\n"
             "printf '%s\\n' \"$request\" >> requests.txt\n"
             "case \"$request\" in\n"
             "  *resource.list*prompt*) printf '%s\\n' "
             "'{\"id\":\"ava_resource_2\",\"type\":\"resource.list.result\",\"ok\":true,\"kind\":\"prompt\",\"resources\":[{\"name\":\"dyn-review\","
             "\"description\":\"Generated review prompt\"}]}' ;;\n"
             "  *resource.list*skill*) printf '%s\\n' "
             "'{\"id\":\"ava_resource_2\",\"type\":\"resource.list.result\",\"ok\":true,\"kind\":\"skill\",\"resources\":[{\"name\":\"dyn-triage\","
             "\"description\":\"Generated triage skill\"}]}' ;;\n"
             "  *resource.read*prompt*) printf '%s\\n' "
             "'{\"id\":\"ava_resource_2\",\"type\":\"resource.read.result\",\"ok\":true,\"kind\":\"prompt\",\"name\":\"dyn-review\",\"content\":\"Dynamic "
             "prompt body\"}' ;;\n"
             "  *resource.read*skill*) printf '%s\\n' "
             "'{\"id\":\"ava_resource_2\",\"type\":\"resource.read.result\",\"ok\":true,\"kind\":\"skill\",\"name\":\"dyn-triage\",\"content\":\"Dynamic skill "
             "body\"}' ;;\n"
             "esac\n"
             "while IFS= read -r line; do :; done\n");
  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.dynamic", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "dynamic resource test enables project plugin");

  auto unlocked_session = plugin_command_test_session(paths, workspace);
  std::vector<ava::permissions::PermissionPrompt> prompts;
  auto allow = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Allow;
  };

  auto prompt_list =
      ava::app::run_plugins_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompts", .permission_resolver = allow});
  auto skill_list =
      ava::app::run_plugins_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-skills", .permission_resolver = allow});
  auto prompt_read = ava::app::run_plugins_command(
      unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompt com.example.dynamic dyn-review", .permission_resolver = allow});
  auto skill_read = ava::app::run_plugins_command(
      unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-skill com.example.dynamic dyn-triage", .permission_resolver = allow});

  expect(prompt_list && prompt_list->handled && command_output_text(prompt_list).find("com.example.dynamic/dyn-review") != std::string::npos &&
             command_output_text(prompt_list).find("Generated review prompt") != std::string::npos,
         "enabled plugin lists dynamic prompts through the explicit command surface: " + command_output_text(prompt_list));
  expect(skill_list && skill_list->handled && command_output_text(skill_list).find("com.example.dynamic/dyn-triage") != std::string::npos &&
             command_output_text(skill_list).find("Generated triage skill") != std::string::npos,
         "enabled plugin lists dynamic skills through the explicit command surface: " + command_output_text(skill_list));
  expect(prompt_read && prompt_read->handled &&
             command_output_text(prompt_read).find("Plugin dynamic prompt com.example.dynamic/dyn-review") != std::string::npos &&
             command_output_text(prompt_read).find("Dynamic prompt body") != std::string::npos,
         "enabled plugin reads a dynamic prompt through the runner protocol: " + command_output_text(prompt_read));
  expect(skill_read && skill_read->handled &&
             command_output_text(skill_read).find("Plugin dynamic skill com.example.dynamic/dyn-triage") != std::string::npos &&
             command_output_text(skill_read).find("Dynamic skill body") != std::string::npos,
         "enabled plugin reads a dynamic skill through the runner protocol: " + command_output_text(skill_read));
  expect(prompts.size() == 4 && std::all_of(prompts.begin(), prompts.end(),
                                            [](auto const& prompt) {
                                              return prompt.operation == ava::permissions::Operation::PluginExecute && prompt.tool_name == "plugin_resource";
                                            }),
         "dynamic resource commands require only plugin.execute permission, not plugin tool or command permission");
  auto const requests = read_text(plugin_dir / "requests.txt");
  expect(requests.find("\"type\":\"resource.list\"") != std::string::npos && requests.find("\"type\":\"resource.read\"") != std::string::npos &&
             requests.find("\"kind\":\"prompt\"") != std::string::npos && requests.find("\"kind\":\"skill\"") != std::string::npos,
         "dynamic resource commands send resource.list/resource.read records with prompt and skill kinds");
}

void test_disabled_plugin_dynamic_resources_do_not_execute()
{
  auto const root = create_empty_root("plugin-dynamic-disabled");

  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.disabled";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json", dynamic_resource_manifest_json("com.example.disabled", "plugin.sh"));
  write_text(plugin_dir / "plugin.sh",
             "printf '%s\\n' executed > executed.txt\n"
             "IFS= read -r line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n");

  auto unlocked_session = plugin_command_test_session(paths, workspace);
  std::vector<ava::permissions::PermissionPrompt> prompts;
  auto allow = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Allow;
  };

  auto list = ava::app::run_plugins_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompts", .permission_resolver = allow});
  auto read = ava::app::run_plugins_command(
      unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompt com.example.disabled any", .permission_resolver = allow});

  expect(list && list->handled && command_output_text(list).find("none") != std::string::npos &&
             command_output_text(list).find("com.example.disabled") == std::string::npos,
         "disabled plugin does not appear in dynamic prompt listings: " + command_output_text(list));
  expect(read && read->handled && command_output_text(read).find("plugin is disabled: com.example.disabled") != std::string::npos,
         "direct dynamic prompt read refuses disabled plugins before execution: " + command_output_text(read));
  expect(!std::filesystem::exists(plugin_dir / "executed.txt"), "disabled plugin entrypoint is not executed for dynamic resources");
  expect(prompts.empty(), "disabled dynamic resource commands do not request plugin.execute permission");
}

void test_dynamic_resources_require_explicit_manifest_capability()
{
  auto const root = create_empty_root("plugin-dynamic-capability");

  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.nodynamic";
  auto const state_file = paths.ava_state_dir / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json", dynamic_resource_manifest_json("com.example.nodynamic", "plugin.sh", "{}", "[\"tools\"]"));
  write_text(plugin_dir / "plugin.sh",
             "printf '%s\\n' executed > executed.txt\n"
             "IFS= read -r line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n");
  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.nodynamic", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "dynamic capability test enables project plugin");

  auto unlocked_session = plugin_command_test_session(paths, workspace);
  std::vector<ava::permissions::PermissionPrompt> prompts;
  auto allow = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Allow;
  };

  auto list = ava::app::run_plugins_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompts", .permission_resolver = allow});
  auto read = ava::app::run_plugins_command(
      unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompt com.example.nodynamic dyn-review", .permission_resolver = allow});

  expect(list && list->handled && command_output_text(list).find("none") != std::string::npos &&
             command_output_text(list).find("com.example.nodynamic") == std::string::npos,
         "dynamic list skips enabled plugins that do not opt into dynamic prompts: " + command_output_text(list));
  expect(read && read->handled && command_output_text(read).find("plugin does not declare dynamic.prompts: com.example.nodynamic") != std::string::npos,
         "direct dynamic read rejects non-opt-in plugins before launch: " + command_output_text(read));
  expect(!std::filesystem::exists(plugin_dir / "executed.txt"), "non-opt-in dynamic resource commands do not execute the plugin entrypoint");
  expect(prompts.empty(), "non-opt-in dynamic resource commands do not request plugin.execute permission");
}

void test_dynamic_resource_read_rejects_invalid_names_before_launch()
{
  auto const root = create_empty_root("plugin-dynamic-invalid-name");

  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.dynamicname";
  auto const state_file = paths.ava_state_dir / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json", dynamic_resource_manifest_json("com.example.dynamicname", "plugin.sh"));
  write_text(plugin_dir / "plugin.sh", "printf '%s\\n' executed > executed.txt\n");
  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.dynamicname", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "dynamic invalid-name test enables project plugin");

  auto unlocked_session = plugin_command_test_session(paths, workspace);
  std::vector<ava::permissions::PermissionPrompt> prompts;
  auto allow = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Allow;
  };
  auto read = ava::app::run_plugins_command(
      unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompt com.example.dynamicname ../bad", .permission_resolver = allow});

  expect(read && read->handled && command_output_text(read).find("invalid dynamic prompt name") != std::string::npos,
         "direct dynamic read validates resource names before sending resource.read: " + command_output_text(read));
  expect(!std::filesystem::exists(plugin_dir / "executed.txt"), "invalid dynamic resource name does not execute the plugin entrypoint");
  expect(prompts.empty(), "invalid dynamic resource names do not request plugin.execute permission");
}

void test_dynamic_resource_proxy_requests_use_core_service_handler()
{
  auto const root = create_empty_root("plugin-dynamic-proxy");

  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.dynamicproxy";
  auto const state_file = paths.ava_state_dir / "plugin-enablement.json";
  auto const response_file = plugin_dir / "proxy-response.txt";
  std::filesystem::create_directories(workspace / "src");

  write_text(plugin_dir / "plugin.json",
             dynamic_resource_manifest_json("com.example.dynamicproxy", "plugin.sh", "{}", "[\"dynamic.prompts\",\"proxy.session\"]"));
  write_text(plugin_dir / "plugin.sh", std::string("IFS= read -r line\n"
                                                   "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
                                                   "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
                                                   "IFS= read -r request\n"
                                                   "printf '%s\\n' ") +
                                           shell_single_quote(proxy_request_json("px_dynamic_session", "session.status", "{}")) +
                                           "\n"
                                           "IFS= read -r proxy_response\n"
                                           "printf '%s\\n' \"$proxy_response\" > " +
                                           shell_single_quote(response_file.generic_string()) +
                                           "\n"
                                           "printf '%s\\n' "
                                           "'{\"id\":\"ava_resource_2\",\"type\":\"resource.read.result\",\"ok\":true,\"kind\":\"prompt\",\"name\":\"dyn-"
                                           "proxy\",\"content\":\"Dynamic proxy prompt\"}'\n"
                                           "while IFS= read -r line; do :; done\n");
  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.dynamicproxy", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "dynamic proxy test enables project plugin");

  auto unlocked_session = plugin_command_test_session(paths, workspace);
  std::vector<ava::permissions::PermissionPrompt> prompts;
  auto allow = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Allow;
  };
  auto read = ava::app::run_plugins_command(
      unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompt com.example.dynamicproxy dyn-proxy", .permission_resolver = allow});
  auto const output = command_output_text(read);
  auto const response = read_text(response_file);
  auto content = proxy_response_content(response);
  expect(read && read->handled && output.find("Dynamic proxy prompt") != std::string::npos, "dynamic resource read succeeds after proxy request: " + output);
  expect(response.find("handler unavailable") == std::string::npos && response.find("\"ok\":true") != std::string::npos && content &&
             content->find("\"operation\":\"session.status\"") != std::string::npos,
         "dynamic resource requests can use declared core-service proxy operations");
}

void test_plugin_reported_dynamic_resource_errors_surface_text()
{
  auto const root = create_empty_root("plugin-dynamic-reported-errors");

  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.dynamicerrors";
  auto const state_file = paths.ava_state_dir / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json", dynamic_resource_manifest_json("com.example.dynamicerrors", "plugin.sh"));
  write_text(
      plugin_dir / "plugin.sh",
      "IFS= read -r line\n"
      "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
      "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
      "IFS= read -r request\n"
      "case \"$request\" in\n"
      "  *resource.list*) printf '%s\\n' '{\"id\":\"ava_resource_2\",\"type\":\"resource.list.result\",\"ok\":false,\"kind\":\"prompt\",\"content\":\"list "
      "boom\"}' ;;\n"
      "  *resource.read*) printf '%s\\n' "
      "'{\"id\":\"ava_resource_2\",\"type\":\"resource.read.result\",\"ok\":false,\"kind\":\"prompt\",\"name\":\"dyn-error\",\"content\":\"read boom\"}' ;;\n"
      "esac\n"
      "while IFS= read -r line; do :; done\n");
  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.dynamicerrors", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "dynamic error test enables project plugin");

  auto unlocked_session = plugin_command_test_session(paths, workspace);
  auto allow = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
  auto list = ava::app::run_plugins_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompts", .permission_resolver = allow});
  auto read = ava::app::run_plugins_command(
      unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompt com.example.dynamicerrors dyn-error", .permission_resolver = allow});
  auto const list_output = command_output_text(list);
  auto const read_output = command_output_text(read);
  expect(list && list->handled && list_output.find("list boom") == std::string::npos && list_output.find("external_failure") != std::string::npos,
         "plugin-reported dynamic resource list errors use SafeFailure: " + list_output);
  expect(read && read->handled && read_output.find("read boom") == std::string::npos && read_output.find("external_failure") != std::string::npos,
         "plugin-reported dynamic resource read errors use SafeFailure: " + read_output);
}

void test_dynamic_resource_commands_respect_prelaunch_cancellation()
{
  auto const root = create_empty_root("plugin-dynamic-cancel");

  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.dynamiccancel";
  auto const state_file = paths.ava_state_dir / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json", dynamic_resource_manifest_json("com.example.dynamiccancel", "plugin.sh"));
  write_text(plugin_dir / "plugin.sh",
             "printf '%s\\n' executed > executed.txt\n"
             "IFS= read -r line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n");
  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.dynamiccancel", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "dynamic cancellation test enables project plugin");

  auto unlocked_session = plugin_command_test_session(paths, workspace);
  std::vector<ava::permissions::PermissionPrompt> prompts;
  auto allow = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Allow;
  };
  auto canceled = [] { return true; };
  auto list = ava::app::run_plugins_command(
      unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompts", .permission_resolver = allow, .cancel_requested = canceled});
  auto read =
      ava::app::run_plugins_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompt com.example.dynamiccancel dyn-cancel",
                                                                               .permission_resolver = allow,
                                                                               .cancel_requested = canceled});
  auto const list_output = command_output_text(list);
  auto const read_output = command_output_text(read);
  expect(list && list->handled && list_output.find("canceled") != std::string::npos,
         "dynamic resource list commands report prelaunch cancellation: " + list_output);
  expect(read && read->handled && read_output.find("canceled") != std::string::npos,
         "dynamic resource read commands report prelaunch cancellation: " + read_output);
  expect(!std::filesystem::exists(plugin_dir / "executed.txt"), "prelaunch cancellation prevents dynamic resource plugin execution");
  expect(prompts.empty(), "prelaunch cancellation does not request plugin.execute permission");
}

void test_malformed_dynamic_resource_result_fails_safely()
{
  auto const root = create_empty_root("plugin-dynamic-malformed");

  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.malformedresource";
  auto const state_file = paths.ava_state_dir / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json", dynamic_resource_manifest_json("com.example.malformedresource", "plugin.sh"));
  write_text(plugin_dir / "plugin.sh",
             "IFS= read -r line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "IFS= read -r request\n"
             "printf '%s\\n' '{\"id\":\"ava_resource_2\",\"type\":\"resource.read.result\",\"ok\":true,\"kind\":\"prompt\",\"name\":\"broken\"}'\n");
  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.malformedresource", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "malformed dynamic resource test enables project plugin");

  auto unlocked_session = plugin_command_test_session(paths, workspace);
  auto allow = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
  auto read = ava::app::run_plugins_command(
      unlocked_session, ava::app::CommandRequest{.command = "/plugins dynamic-prompt com.example.malformedresource broken", .permission_resolver = allow});
  auto const output = command_output_text(read);
  expect(read && read->handled && output.find("plugin dynamic resource read result is malformed") != std::string::npos &&
             output.find("response_bytes") != std::string::npos,
         "malformed dynamic resource read results fail with bounded metadata: " + output);
}

void test_static_plugin_resources_remain_manifest_only()
{
  auto const root = create_empty_root("plugin-static-resource-manifest-only");

  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.staticresource";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json",
             dynamic_resource_manifest_json(
                 "com.example.staticresource", "plugin.sh",
                 "{\"prompts\":[{\"name\":\"static-review\",\"description\":\"Static review\",\"path\":\"prompts/review.md\"}],\"skills\":[]}"));
  write_text(plugin_dir / "prompts" / "review.md", "Static prompt body\n");
  write_text(plugin_dir / "plugin.sh", "printf '%s\\n' executed > executed.txt\n");

  auto unlocked_session = plugin_command_test_session(paths, workspace);
  auto prompt =
      ava::app::run_plugins_command(unlocked_session, ava::app::CommandRequest{.command = "/plugins prompt com.example.staticresource static-review"});
  auto const output = command_output_text(prompt);
  expect(prompt && prompt->handled && output.find("Static prompt body") != std::string::npos && output.find("path:") != std::string::npos,
         "static /plugins prompt still reads manifest-declared files: " + output);
  expect(!std::filesystem::exists(plugin_dir / "executed.txt"), "static /plugins prompt remains manifest-only and does not execute plugin entrypoints");
}

void test_enabled_plugin_event_hooks_observe_runtime_events()
{
  auto const root = create_empty_root("plugin-event-hooks");

  auto const workspace = root / "workspace";
  auto const project_plugins = workspace / ".ava" / "plugins";
  auto const plugin_dir = project_plugins / "com.example.events";
  auto const state_file = root / "state" / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json", event_hook_manifest_json("com.example.events", "plugin.sh"));
  write_text(plugin_dir / "plugin.sh",
             "read line\n"
             "printf '%s\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "read line\n"
             "printf '%s\n' \"$line\" > event-request.txt\n"
             "printf '%s\n' '{\"id\":\"ava_event_call_hook\",\"type\":\"event.observed\",\"ok\":true}'\n"
             "while read line; do :; done\n");
  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.events", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "event hook plugin test enables project plugin");

  std::vector<ava::permissions::PermissionPrompt> prompts;
  bool forwarded = false;
  bool hook_observed_before_forward = false;
  auto sink = ava::app::make_plugin_event_observer_sink(
      ava::app::PluginEventObserverOptions{.workspace_dir = workspace,
                                           .plugin_global_plugins_dir = root / "global-plugins",
                                           .plugin_project_plugins_dir = project_plugins,
                                           .plugin_enablement_file = state_file,
                                           .mode = ava::agent::Mode::Build,
                                           .permission_resolver = [&prompts](ava::permissions::PermissionPrompt const& prompt)
                                               -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
                                             prompts.push_back(prompt);
                                             return ava::permissions::PermissionResolution::Allow;
                                           },
                                           .process_scope = plugin_test_process_scope(),
                                           .session_id = "ses_event_test",
                                           .provider_id = "openai",
                                           .model_id = "gpt-test",
                                           .current_dir = workspace},
      [&forwarded, &hook_observed_before_forward, &plugin_dir](ava::event::RuntimeEvent const&) -> ava::core::VoidResult {
        forwarded = true;
        hook_observed_before_forward = std::filesystem::exists(plugin_dir / "event-request.txt");
        return {};
      });

  ava::event::ToolPayload payload;
  payload.text = "ok";
  payload.call_id = "call_hook";
  payload.tool = "demo";
  payload.status = "success";
  auto event = ava::event::RuntimeEvent{{}, ava::event::ToolResultEvent{.payload = std::move(payload)}};
  auto observed = sink(event);
  expect(observed.has_value(), "plugin event observer sink forwards runtime event successfully");
  expect(forwarded && hook_observed_before_forward, "plugin event observer runs matching hooks before forwarding to the next typed sink");
  expect(prompts.size() == 2 && prompts[0].operation == ava::permissions::Operation::PluginExecute &&
             prompts[1].operation == ava::permissions::Operation::PluginEventObserve && prompts[1].command == "com.example.events:tool.result",
         "plugin event hooks require plugin.execute and plugin.event.observe permission approval");
  auto const request = read_text(plugin_dir / "event-request.txt");
  expect(request.find("\"type\":\"event.observe\"") != std::string::npos && request.find("\"event\":\"tool_result\"") != std::string::npos &&
             request.find("\"payload\":{\"text\":\"ok\",\"call_id\":\"call_hook\",\"tool\":\"demo\",\"status\":\"success\"}") != std::string::npos &&
             request.find("\"context\":{\"call_id\":\"call_hook\"") != std::string::npos,
         "enabled plugin event hook observes the exact typed name, payload, and call id");
}

void test_plugin_event_hook_failures_report_to_opt_in_sink()
{
  struct CapturedFailure
  {
    std::string plugin_id;
    std::string event_name;
    ava::core::ErrorCategory category = ava::core::ErrorCategory::Unknown;
    std::string message;
    std::string details;
  };

  auto const root = create_empty_root("plugin-event-hook-failures");

  auto const workspace = root / "workspace";
  auto const project_plugins = workspace / ".ava" / "plugins";
  auto const plugin_dir = project_plugins / "com.example.eventdiag";
  auto const state_file = root / "state" / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json", event_hook_manifest_json("com.example.eventdiag", "plugin.sh"));
  write_text(plugin_dir / "plugin.sh",
             "read line\n"
             "printf '%s\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "while read line; do\n"
             "  printf '%s\n' \"$line\" > event-request.txt\n"
             "  printf '%s\n' '{\"id\":\"ava_event_call_diag\",\"type\":\"tool.result\",\"ok\":true,"
             "\"content\":\"wrong response type\"}'\n"
             "done\n");
  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.eventdiag", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "event hook diagnostic test enables project plugin");

  std::vector<CapturedFailure> failures;
  bool forwarded = false;
  bool fail_downstream = false;
  auto sink = ava::app::make_plugin_event_observer_sink(
      ava::app::PluginEventObserverOptions{
          .workspace_dir = workspace,
          .plugin_global_plugins_dir = root / "global-plugins",
          .plugin_project_plugins_dir = project_plugins,
          .plugin_enablement_file = state_file,
          .mode = ava::agent::Mode::Build,
          .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
            return ava::permissions::PermissionResolution::Allow;
          },
          .process_scope = plugin_test_process_scope(),
          .hook_failure_sink =
              [&failures](std::string_view plugin_id, std::string_view event_name, ava::core::Error const& error) {
                failures.push_back(CapturedFailure{.plugin_id = std::string(plugin_id),
                                                   .event_name = std::string(event_name),
                                                   .category = error.category(),
                                                   .message = error.message(),
                                                   .details = error.format()});
              },
          .session_id = "ses_event_diag",
          .provider_id = "openai",
          .model_id = "gpt-test",
          .current_dir = workspace},
      [&forwarded, &fail_downstream](ava::event::RuntimeEvent const&) -> ava::core::VoidResult {
        forwarded = true;
        if (fail_downstream)
          return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "stable downstream event sink failure"));
        return {};
      });

  ava::event::ToolPayload payload;
  payload.text = "ok";
  payload.call_id = "call_diag";
  payload.tool = "demo";
  payload.status = "success";
  auto event = ava::event::RuntimeEvent{{}, ava::event::ToolResultEvent{.payload = std::move(payload)}};
  auto observed = sink(event);
  expect(observed.has_value(), "plugin event observer keeps hook failures best-effort");
  expect(forwarded, "plugin event observer forwards runtime events after hook failures");
  expect(failures.size() == 1, "plugin event observer reports hook protocol failures to opt-in sink");
  if (!failures.empty())
  {
    expect(failures[0].plugin_id == "com.example.eventdiag", "hook failure sink receives plugin id");
    expect(failures[0].event_name == "tool.result", "hook failure sink receives contributed hook event name");
    expect(failures[0].category == ava::core::ErrorCategory::Tool, "hook failure sink receives error category");
    expect(failures[0].message.find("plugin event response is malformed") != std::string::npos, "hook failure sink receives error message");
    expect(failures[0].details.find("response_bytes") != std::string::npos, "hook failure sink receives bounded formatted error metadata");
  }

  fail_downstream = true;
  forwarded = false;
  auto downstream_failure = sink(event);
  expect(!downstream_failure && forwarded && downstream_failure.error().category() == ava::core::ErrorCategory::Io &&
             downstream_failure.error().message() == "stable downstream event sink failure",
         "plugin event observer remains best-effort for hook failures but propagates the downstream sink failure exactly");
}

void test_plugin_tool_dispatcher()
{
  auto const root = create_empty_root("plugin-tool-dispatcher");

  auto const workspace = root / "workspace";
  auto const project_plugins = workspace / ".ava" / "plugins";
  auto const plugin_dir = project_plugins / "com.example.todo";
  auto const state_file = root / "state" / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json", tool_manifest_json("com.example.todo", "plugin.sh"));
  write_text(plugin_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "read line\n"
             "printf '%s\\n' \"$line\" > tool-request.txt\n"
             "printf '%s\\n' '{\"id\":\"ava_tool_call_plugin\",\"type\":\"tool.result\",\"ok\":true,"
             "\"content\":\"Added via dispatcher\",\"metadata\":{\"count\":1}}'\n"
             "while read line; do :; done\n");
  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.todo", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "dispatcher plugin test enables project plugin");

  auto const model_tool_name = ava::plugin::plugin_model_tool_name("com.example.todo", "todo_add");
  std::vector<ava::permissions::PermissionPrompt> prompts;
  std::vector<ava::tools::PermissionAuditEvent> audits;
  bool cancel_requested = false;
  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  context.plugin_global_plugins_dir = root / "global-plugins";
  context.plugin_project_plugins_dir = project_plugins;
  context.plugin_enablement_file = state_file;
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
  context.process_scope = plugin_test_process_scope();

  auto const schemas = ava::agent::ToolDispatcher::tool_schemas_json(context);
  auto const plugin_schema = std::find_if(schemas.begin(), schemas.end(), [&](std::string const& schema) {
    return schema.find("\"name\":\"" + model_tool_name + "\"") != std::string::npos && schema.find("\"parameters\"") != std::string::npos;
  });
  expect(plugin_schema != schemas.end(), "enabled plugin tool contribution is exported as a provider schema");
  if (plugin_schema != schemas.end())
  {
    ava::tests::expect_json_matches_golden(*plugin_schema, "plugin-tool-schema.json", "plugin tool provider schema matches AVA 0.80 golden fixture");
  }
  {
    ava::agent::ToolVisibilityOptions const no_builtin_visibility{.mode = ava::agent::ToolVisibilityMode::NoBuiltinTools};
    auto const no_builtin_schemas = ava::agent::ToolDispatcher::tool_schemas_json(context, no_builtin_visibility);
    auto const visible_plugin_schema = std::find_if(no_builtin_schemas.begin(), no_builtin_schemas.end(), [&](std::string const& schema) {
      return schema.find("\"name\":\"" + model_tool_name + "\"") != std::string::npos;
    });
    auto const visible_builtin_schema = std::find_if(no_builtin_schemas.begin(), no_builtin_schemas.end(),
                                                     [](std::string const& schema) { return schema.find("\"name\":\"read_file\"") != std::string::npos; });
    expect(visible_plugin_schema != no_builtin_schemas.end() && visible_builtin_schema == no_builtin_schemas.end(),
           "no-builtin tool visibility keeps enabled plugin tools while hiding built-ins");
  }

  ava::agent::ToolDispatcher dispatcher(context);
  auto dispatched =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_plugin", .name = model_tool_name, .arguments_json = "{\"text\":\"write tests\"}"});
  expect(dispatched && dispatched->success && dispatched->result_text.find("Added via dispatcher") != std::string::npos,
         dispatched ? "dispatcher calls enabled plugin tool through broker"
                    : "dispatcher calls enabled plugin tool through broker: " + dispatched.error().format());
  expect(dispatched && dispatched->payload.status == ava::agent::ToolResultStatus::Success && dispatched->payload.content_type == "application/json" &&
             dispatched->payload.content.find("com.example.todo") != std::string::npos &&
             dispatched->payload.content.find("Added via dispatcher") != std::string::npos,
         "plugin tool dispatcher attaches structured semantic result payloads");
  expect(prompts.size() == 2 && prompts[0].operation == ava::permissions::Operation::PluginExecute &&
             prompts[1].operation == ava::permissions::Operation::PluginToolCall && prompts[1].tool_name == model_tool_name &&
             prompts[1].command == "com.example.todo:todo_add",
         "plugin tool calls require plugin.execute and plugin.tool.call permission approval");
  expect(!audits.empty() && audits.back().operation == ava::permissions::Operation::PluginToolCall && audits.back().resolution == "allow",
         "plugin tool permission decisions are audited");
  auto const request = read_text(plugin_dir / "tool-request.txt");
  expect(request.find("\"type\":\"tool.call\"") != std::string::npos && request.find("\"tool\":\"todo_add\"") != std::string::npos,
         "dispatcher broker sends plugin protocol tool call");

  write_text(plugin_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_tool_call_plugin_failure\",\"type\":\"tool.result\",\"ok\":false,"
             "\"content\":\"CANARY_PLUGIN_BROKER_CONTENT_70d3\",\"metadata\":{\"secret\":\"CANARY_PLUGIN_BROKER_METADATA_f410\"}}'\n"
             "while read line; do :; done\n");
  auto remote_failure =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_plugin_failure", .name = model_tool_name, .arguments_json = "{\"text\":\"fail\"}"});
  expect(remote_failure && !remote_failure->success && remote_failure->result_text.find("external_failure") != std::string::npos &&
             remote_failure->result_text.find("CANARY_PLUGIN_BROKER_CONTENT_70d3") == std::string::npos &&
             remote_failure->payload.content.find("CANARY_PLUGIN_BROKER_METADATA_f410") == std::string::npos &&
             remote_failure->payload.error_code == "external_failure",
         "plugin broker replaces failed remote content and metadata with SafeFailure");

  auto invalid_args = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_invalid", .name = model_tool_name, .arguments_json = "[]"});
  expect(invalid_args && !invalid_args->success && invalid_args->result_text.find("JSON object") == std::string::npos &&
             invalid_args->result_text.find("invalid_request") != std::string::npos,
         "plugin tool dispatcher rejects non-object arguments with SafeFailure");
  expect(invalid_args && invalid_args->payload.status == ava::agent::ToolResultStatus::Error && invalid_args->payload.error_category == "configuration" &&
             invalid_args->payload.error_code == "invalid_request",
         "plugin tool dispatcher attaches support-safe structured semantic errors");

  std::vector<ava::permissions::PermissionPrompt> denied_prompts;
  auto denied_context = context;
  denied_context.permission_resolver =
      [&denied_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    denied_prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Deny;
  };
  ava::agent::ToolDispatcher denied_dispatcher(denied_context);
  auto denied = denied_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_denied", .name = model_tool_name, .arguments_json = "{}"});
  expect(denied && !denied->success && denied->result_text.find("plugin process launch requires permission") == std::string::npos &&
             denied->result_text.find("permission_denied") != std::string::npos && denied_prompts.size() == 1,
         "plugin tool dispatcher reports permission denial with SafeFailure before process execution");
  expect(!denied_prompts.empty() && !denied_prompts.front().permission_request_id.empty(), "plugin permission denial prompt carries a request id");
  if (!denied_prompts.empty())
  {
    auto const& denied_id = denied_prompts.front().permission_request_id;
    auto const linked_audits = std::count_if(audits.begin(), audits.end(), [&](auto const& event) { return event.permission_request_id == denied_id; });
    auto const has_resolver_denial = std::any_of(audits.begin(), audits.end(), [&](auto const& event) {
      return event.permission_request_id == denied_id && event.operation == ava::permissions::Operation::PluginExecute && event.resolution == "deny" &&
             event.resolution_source == "resolver";
    });
    expect(linked_audits >= 2 && has_resolver_denial, "plugin permission denial audits policy and resolver events with the prompt request id");
    expect(denied && std::find(denied->payload.permission_request_ids.begin(), denied->payload.permission_request_ids.end(), denied_id) !=
                         denied->payload.permission_request_ids.end(),
           "plugin denial payload links the permission request id");
  }

  auto typed_cancel_context = context;
  typed_cancel_context.permission_audit_sink = [](ava::tools::PermissionAuditEvent const&) -> ava::core::VoidResult {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "renamed plugin interruption", ava::core::ErrorCode::Canceled));
  };
  ava::agent::ToolDispatcher typed_cancel_dispatcher(typed_cancel_context);
  auto typed_cancel =
      typed_cancel_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_typed_cancel", .name = model_tool_name, .arguments_json = "{}"});
  expect(typed_cancel && !typed_cancel->success && typed_cancel->payload.status == ava::agent::ToolResultStatus::Canceled,
         "plugin broker classifies a renamed typed cancellation by ErrorCode");

  auto legacy_cancel_context = context;
  legacy_cancel_context.permission_audit_sink = [](ava::tools::PermissionAuditEvent const&) -> ava::core::VoidResult {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "tool canceled");
    error.with_context("canceled", "true");
    return std::unexpected(std::move(error));
  };
  ava::agent::ToolDispatcher legacy_cancel_dispatcher(legacy_cancel_context);
  auto legacy_cancel =
      legacy_cancel_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_legacy_cancel", .name = model_tool_name, .arguments_json = "{}"});
  expect(legacy_cancel && !legacy_cancel->success && legacy_cancel->payload.status == ava::agent::ToolResultStatus::Error,
         "plugin broker does not classify untyped legacy cancellation text or context as cancellation");

  auto const prompts_before_cancel = prompts.size();
  cancel_requested = true;
  auto canceled = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_canceled", .name = model_tool_name, .arguments_json = "{}"});
  expect(canceled && !canceled->success && canceled->payload.status == ava::agent::ToolResultStatus::Canceled &&
             canceled->result_text.find("canceled") != std::string::npos && prompts.size() == prompts_before_cancel,
         "plugin tool dispatcher reports semantic cancellation before permission or process execution");
}

void test_plugin_core_service_proxy_read_search_slice()
{
  auto const root = create_empty_root("plugin-core-proxy");

  auto const workspace = root / "workspace";
  auto const project_plugins = workspace / ".ava" / "plugins";
  auto const state_file = root / "state" / "plugin-enablement.json";
  auto const outside_dir = root / "outside";
  std::filesystem::create_directories(workspace / "src");
  write_text(workspace / "visible.txt", "visible workspace content\n");
  write_text(workspace / "src" / "notes.txt", "alpha\nneedle is here\nomega\n");
  write_text(outside_dir / "read.txt", "outside proxy content\n" + std::string(20 * 1024, 'x'));
  write_text(outside_dir / "deny.txt", "denied outside content\n");

  auto install_plugin = [&](std::string plugin_id, std::string capabilities_json, std::string script) {
    auto const plugin_dir = project_plugins / plugin_id;
    write_text(plugin_dir / "plugin.json", tool_manifest_json(plugin_id, "plugin.sh", "proxy_tool", capabilities_json));
    write_text(plugin_dir / "plugin.sh", script);
    auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, plugin_id, true, ava::plugin::PluginScope::Project);
    expect(enabled.has_value(), "proxy test enables plugin " + plugin_id);
    return plugin_dir;
  };

  {
    auto const plugin_id = std::string("com.example.proxycap");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request =
        proxy_request_json("px_no_cap", "file.read", "{\"path\":\"" + ava::core::json::escape((workspace / "visible.txt").generic_string()) + "\"}");
    install_plugin(plugin_id, "[\"tools\"]", proxy_tool_script(request, response_file));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    expect(dispatched && dispatched->success, dispatched ? "manifest without proxy capability receives a handled proxy denial"
                                                         : "manifest without proxy capability dispatch failed: " + dispatched.error().format());
    auto const response = read_text(response_file);
    expect(ava::core::json::is_valid_object(response), "capability denial proxy.response is valid JSON");
    expect(response.find("\"type\":\"proxy.response\"") != std::string::npos && response.find("\"id\":\"px_no_cap\"") != std::string::npos &&
               response.find("\"ok\":false") != std::string::npos && response.find("required proxy capability") != std::string::npos,
           "manifest without proxy.read cannot use file.read proxy");
    auto const read_audit =
        std::any_of(audits.begin(), audits.end(), [](auto const& event) { return event.operation == ava::permissions::Operation::ReadFile; });
    expect(!read_audit, "capability-gated proxy denial does not reach the read-file service");
  }

  {
    auto const plugin_id = std::string("com.example.proxyread");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request =
        proxy_request_json("px_read", "file.read",
                           "{\"path\":\"" + ava::core::json::escape((outside_dir / "read.txt").generic_string()) + "\",\"max_bytes\":999999,\"limit\":999999}");
    install_plugin(plugin_id, "[\"tools\",\"proxy.read\"]", proxy_tool_script(request, response_file));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    expect(dispatched && dispatched->success, dispatched ? "read proxy dispatch succeeds" : "read proxy dispatch failed: " + dispatched.error().format());
    auto const response = read_text(response_file);
    expect(response.size() <= 64 * 1024 && ava::core::json::is_valid_object(response),
           "read proxy.response stays within the plugin record cap and is valid JSON");
    auto content = proxy_response_content(response);
    expect(content && ava::core::json::is_valid_object(*content) && content->find("outside proxy content") != std::string::npos &&
               content->find("\"truncated\":true") != std::string::npos,
           "read proxy returns bounded presentation JSON content");
    auto const read_prompt =
        std::any_of(prompts.begin(), prompts.end(), [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::ReadFile; });
    auto const read_audit = std::any_of(audits.begin(), audits.end(), [&](auto const& event) {
      return event.operation == ava::permissions::Operation::ReadFile && event.resolution == "allow" &&
             event.actor.find("plugin:" + plugin_id) != std::string::npos && event.tool_name.find("file.read") != std::string::npos;
    });
    expect(read_prompt && read_audit, "read proxy routes through existing read-file permission prompt and audit context");
  }

  {
    auto const plugin_id = std::string("com.example.proxywindow");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request = proxy_request_json("px_window", "file.read", R"({"path":"visible.txt","offset":2,"limit":1})");
    install_plugin(plugin_id, "[\"tools\",\"proxy.read\"]", proxy_tool_script(request, response_file, "call_proxy_window"));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    context.exact_file_access = std::make_shared<PluginWindowExactFileAccess>();
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy_window", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    auto const response = read_text(response_file);
    auto const content = proxy_response_content(response);
    long long output_lines = -1;
    long long next_offset = -1;
    bool has_total_bytes = false;
    bool has_total_lines = false;
    if (content)
    {
      output_lines = ava::core::json::integer_field(*content, "output_lines").value_or(-1);
      next_offset = ava::core::json::integer_field(*content, "next_offset_line").value_or(-1);
      has_total_bytes = ava::core::json::integer_field(*content, "total_bytes").has_value();
      has_total_lines = ava::core::json::integer_field(*content, "total_lines").has_value();
    }
    expect(
        dispatched && dispatched->success && content && content->find("remote-two") != std::string::npos && output_lines == 1 && next_offset == 3 &&
            !has_total_bytes && !has_total_lines,
        std::string("plugin file.read serialization omits unavailable exact-window totals while retaining output and sentinel continuation counts: success=") +
            (dispatched && dispatched->success ? "true" : "false") + " output_lines=" + std::to_string(output_lines) +
            " next_offset=" + std::to_string(next_offset) + " has_total_bytes=" + (has_total_bytes ? "true" : "false") +
            " has_total_lines=" + (has_total_lines ? "true" : "false") + " response=" + response + " content=" + content.value_or("<missing>"));
  }

  {
    auto const plugin_id = std::string("com.example.proxysearch");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request = proxy_request_json("px_search", "file.search", "{\"query\":\"needle\",\"include\":\"**/*.txt\",\"max_matches\":10}");
    install_plugin(plugin_id, "[\"tools\",\"proxy.search\"]", proxy_tool_script(request, response_file));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    expect(dispatched && dispatched->success, dispatched ? "search proxy dispatch succeeds" : "search proxy dispatch failed: " + dispatched.error().format());
    auto const response = read_text(response_file);
    auto content = proxy_response_content(response);
    expect(ava::core::json::is_valid_object(response) && content && ava::core::json::is_valid_object(*content) &&
               content->find("\"kind\":\"grep\"") != std::string::npos && content->find("needle is here") != std::string::npos,
           "search proxy returns grep presentation JSON");
    auto const search_audit = std::any_of(audits.begin(), audits.end(), [&](auto const& event) {
      return event.operation == ava::permissions::Operation::SearchFiles && event.actor.find("plugin:" + plugin_id) != std::string::npos &&
             event.tool_name.find("file.search") != std::string::npos;
    });
    expect(search_audit, "search proxy routes through existing search permission audit path");
  }

  {
    auto const plugin_id = std::string("com.example.proxysession");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request = proxy_request_json("px_session", "session.status", "{}");
    install_plugin(plugin_id, "[\"tools\",\"proxy.session\"]", proxy_tool_script(request, response_file));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    expect(dispatched && dispatched->success,
           dispatched ? "session.status proxy dispatch succeeds" : "session.status proxy dispatch failed: " + dispatched.error().format());
    auto const response = read_text(response_file);
    auto content = proxy_response_content(response);
    auto const expected_current_dir = "\"current_dir\":\"" + ava::core::json::escape((workspace / "src").generic_string()) + "\"";
    expect(ava::core::json::is_valid_object(response) && content && ava::core::json::is_valid_object(*content) &&
               content->find("\"operation\":\"session.status\"") != std::string::npos &&
               content->find("\"session_id\":\"ses_proxy_test\"") != std::string::npos && content->find("\"provider_id\":\"openai\"") != std::string::npos &&
               content->find("\"model_id\":\"gpt-test\"") != std::string::npos && content->find(expected_current_dir) != std::string::npos,
           "session.status proxy returns bounded read-only session metadata");
    auto const service_prompt = std::any_of(prompts.begin(), prompts.end(), [](auto const& prompt) {
      return prompt.operation != ava::permissions::Operation::PluginExecute && prompt.operation != ava::permissions::Operation::PluginToolCall;
    });
    expect(!service_prompt, "session.status proxy does not request file, search, or command permissions");
  }

  {
    auto const plugin_id = std::string("com.example.proxysessionargs");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request = proxy_request_json("px_session_args", "session.status", "{\"include\":\"stats\"}");
    install_plugin(plugin_id, "[\"tools\",\"proxy.session\"]", proxy_tool_script(request, response_file));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    expect(dispatched && dispatched->success, dispatched ? "session.status argument rejection returns to plugin"
                                                         : "session.status argument rejection dispatch failed: " + dispatched.error().format());
    auto const response = read_text(response_file);
    expect(ava::core::json::is_valid_object(response) && response.find("\"ok\":false") != std::string::npos &&
               response.find("session.status does not accept arguments") != std::string::npos,
           "session.status rejects non-empty arguments as structured proxy.response error");
  }

  {
    auto const plugin_id = std::string("com.example.proxydenied");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request =
        proxy_request_json("px_denied", "file.read", "{\"path\":\"" + ava::core::json::escape((outside_dir / "deny.txt").generic_string()) + "\"}");
    install_plugin(plugin_id, "[\"tools\",\"proxy.read\"]", proxy_tool_script(request, response_file));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    context.permission_resolver =
        [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
      prompts.push_back(prompt);
      if (prompt.operation == ava::permissions::Operation::ReadFile)
      {
        return ava::permissions::PermissionResolution::Deny;
      }
      return ava::permissions::PermissionResolution::Allow;
    };
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    expect(dispatched && dispatched->success,
           dispatched ? "permission-denied proxy dispatch returns to plugin" : "permission-denied proxy dispatch failed: " + dispatched.error().format());
    auto const response = read_text(response_file);
    expect(response.find("\"ok\":false") != std::string::npos && response.find("permission_denied") != std::string::npos &&
               response.find("denied outside content") == std::string::npos,
           "read proxy permission denial is returned as structured proxy.response without bypassing policy");
    auto const denied_audit = std::any_of(
        audits.begin(), audits.end(), [](auto const& event) { return event.operation == ava::permissions::Operation::ReadFile && event.resolution == "deny"; });
    expect(denied_audit, "read proxy permission denial is audited");
  }

  {
    auto const plugin_id = std::string("com.example.proxycancel");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request =
        proxy_request_json("px_cancel", "file.read", "{\"path\":\"" + ava::core::json::escape((outside_dir / "read.txt").generic_string()) + "\"}");
    install_plugin(plugin_id, "[\"tools\",\"proxy.read\"]", proxy_tool_script(request, response_file));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    context.permission_resolver =
        [&prompts, &cancel_requested](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
      prompts.push_back(prompt);
      if (prompt.operation == ava::permissions::Operation::ReadFile)
        cancel_requested = true;
      return ava::permissions::PermissionResolution::Allow;
    };
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    expect(dispatched && !dispatched->success && dispatched->payload.status == ava::agent::ToolResultStatus::Canceled &&
               dispatched->result_text.find("canceled") != std::string::npos,
           "parent cancellation token cancels pending read proxy work and plugin tool call");
  }

  {
    auto const plugin_id = std::string("com.example.proxyunknown");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request = proxy_request_json("px_unknown", "shell.run", "{\"command\":\"pwd\"}");
    install_plugin(plugin_id, "[\"tools\",\"proxy.read\",\"proxy.search\"]", proxy_tool_script(request, response_file));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    expect(dispatched && dispatched->success, "unknown proxy operation is returned to plugin as a safe error");
    auto const response = read_text(response_file);
    expect(
        ava::core::json::is_valid_object(response) && response.find("\"ok\":false") != std::string::npos && response.find("not supported") != std::string::npos,
        "unknown proxy operation produces bounded structured proxy.response error");
  }

  {
    auto const plugin_id = std::string("com.example.proxyescape");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    std::error_code symlink_error;
    std::filesystem::create_symlink(outside_dir / "read.txt", workspace / "outside-link.txt", symlink_error);
    if (!symlink_error)
    {
      auto const request = proxy_request_json("px_escape", "file.read", "{\"path\":\"outside-link.txt\"}");
      install_plugin(plugin_id, "[\"tools\",\"proxy.read\"]", proxy_tool_script(request, response_file));
      std::vector<ava::permissions::PermissionPrompt> prompts;
      std::vector<ava::tools::PermissionAuditEvent> audits;
      bool cancel_requested = false;
      auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
      ava::agent::ToolDispatcher dispatcher(context);
      auto dispatched = dispatcher.dispatch(
          ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
      expect(dispatched && dispatched->success, "relative symlink proxy request is returned as structured error");
      auto const response = read_text(response_file);
      expect(response.find("\"ok\":false") != std::string::npos && response.find("outside proxy content") == std::string::npos,
             "relative proxy paths cannot escape the workspace through symlinks");
    }
  }

  {
    auto const plugin_id = std::string("com.example.proxybadargs");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request = proxy_request_json("px_bad", "file.search", "{\"query\":\"needle\",\"root\":\"src\"}");
    install_plugin(plugin_id, "[\"tools\",\"proxy.search\"]", proxy_tool_script(request, response_file));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    expect(dispatched && dispatched->success, "unsupported search root is returned as structured proxy error");
    auto const response = read_text(response_file);
    expect(response.find("\"ok\":false") != std::string::npos && response.find("root is not supported") != std::string::npos,
           "file.search root argument is rejected instead of silently ignored");
  }

  {
    auto const plugin_id = std::string("com.example.proxybadnumber");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request = proxy_request_json("px_bad_number", "file.read", "{\"path\":\"visible.txt\",\"max_bytes\":1.5}");
    install_plugin(plugin_id, "[\"tools\",\"proxy.read\"]", proxy_tool_script(request, response_file));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    expect(dispatched && dispatched->success, "fractional proxy bound is returned as structured proxy error");
    auto const response = read_text(response_file);
    auto const read_prompt =
        std::any_of(prompts.begin(), prompts.end(), [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::ReadFile; });
    expect(response.find("\"ok\":false") != std::string::npos && response.find("positive integer") != std::string::npos && !read_prompt,
           "fractional numeric proxy bounds are rejected before read permission prompts");
  }

  {
    auto const plugin_id = std::string("com.example.proxyregex");
    auto const response_file = project_plugins / plugin_id / "proxy-response.txt";
    auto const request = proxy_request_json("px_regex", "file.search", "{\"query\":\"(a+)+$\",\"literal\":false}");
    install_plugin(plugin_id, "[\"tools\",\"proxy.search\"]", proxy_tool_script(request, response_file));
    std::vector<ava::permissions::PermissionPrompt> prompts;
    std::vector<ava::tools::PermissionAuditEvent> audits;
    bool cancel_requested = false;
    auto context = plugin_proxy_test_context(workspace, project_plugins, state_file, prompts, audits, cancel_requested);
    ava::agent::ToolDispatcher dispatcher(context);
    auto dispatched = dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "call_proxy", .name = ava::plugin::plugin_model_tool_name(plugin_id, "proxy_tool"), .arguments_json = "{}"});
    expect(dispatched && dispatched->success, "regex proxy search is returned as structured proxy error");
    auto const response = read_text(response_file);
    auto const search_prompt =
        std::any_of(prompts.begin(), prompts.end(), [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::SearchFiles; });
    expect(response.find("\"ok\":false") != std::string::npos && response.find("regex mode is not supported") != std::string::npos && !search_prompt,
           "file.search proxy rejects regex mode before dispatching grep work");
  }
}

void test_plugin_tool_dispatcher_rejects_invalid_result()
{
  auto const root = create_empty_root("plugin-tool-invalid-result");

  auto const workspace = root / "workspace";
  auto const project_plugins = workspace / ".ava" / "plugins";
  auto const plugin_dir = project_plugins / "com.example.invalid";
  auto const state_file = root / "state" / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  write_text(plugin_dir / "plugin.json", tool_manifest_json("com.example.invalid", "plugin.sh"));
  write_text(plugin_dir / "plugin.sh",
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava."
             "plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}'\n"
             "read line\n"
             "printf '%s\\n' '{\"id\":\"ava_tool_call_bad\",\"type\":\"tool.result\",\"ok\":true}'\n");
  auto enabled = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.invalid", true, ava::plugin::PluginScope::Project);
  expect(enabled.has_value(), "invalid-result plugin test enables project plugin");

  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  context.plugin_global_plugins_dir = root / "global-plugins";
  context.plugin_project_plugins_dir = project_plugins;
  context.plugin_enablement_file = state_file;
  context.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolution::Allow;
  };
  context.process_scope = plugin_test_process_scope();

  auto const model_tool_name = ava::plugin::plugin_model_tool_name("com.example.invalid", "todo_add");
  ava::agent::ToolDispatcher dispatcher(context);
  auto dispatched = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_bad", .name = model_tool_name, .arguments_json = "{}"});
  expect(dispatched && !dispatched->success && dispatched->result_text.find("malformed") == std::string::npos &&
             dispatched->result_text.find("external_failure") != std::string::npos,
         "dispatcher reports malformed plugin tool results with SafeFailure");
}

void test_plugin_failure_diagnostics_are_support_safe()
{
  constexpr std::string_view canary = "CANARY_PLUGIN_DISCOVERY_DIAGNOSTIC_5a2c";
  auto const root = temp_root() / "plugin-safe-diagnostics";
  std::error_code cleanup;
  std::filesystem::remove_all(root, cleanup);
  auto const plugin_dir = root / "workspace" / ".ava" / "plugins" / std::string(canary);
  write_text(plugin_dir / "plugin.json", "{\"schema_version\":1,\"id\":\"CANARY_PLUGIN_DISCOVERY_DIAGNOSTIC_5a2c\"}");

  auto const diagnostics = ava::plugin::collect_plugin_diagnostics(
      ava::plugin::PluginDiscoveryOptions{.global_plugins_dir = root / "global", .project_plugins_dir = root / "workspace" / ".ava" / "plugins"},
      root / "state" / "plugin-enablement.json", root / "workspace");
  expect(diagnostics.failures.size() == 1, "invalid plugin discovery produces one typed failure");
  if (!diagnostics.failures.empty())
  {
    auto const json = ava::diagnostics::serialize_safe_failure_json(diagnostics.failures.front().failure);
    auto const human = ava::diagnostics::serialize_safe_failure_human(diagnostics.failures.front().failure);
    expect(json.find(canary) == std::string::npos && human.find(canary) == std::string::npos &&
               diagnostics.failures.front().message.find(canary) == std::string::npos && diagnostics.failures.front().details.find(canary) == std::string::npos,
           "stored and rendered PluginFailure diagnostics exclude plugin ids, paths, and raw Error text");
  }
}

void test_plugin_tool_registry_skips_name_collisions()
{
  auto const root = create_empty_root("plugin-tool-collisions");

  auto const workspace = root / "workspace";
  auto const project_plugins = workspace / ".ava" / "plugins";
  auto const state_file = root / "state" / "plugin-enablement.json";
  std::filesystem::create_directories(workspace);

  write_text(project_plugins / "com.example.same" / "plugin.json", tool_manifest_json("com.example.same", "plugin.sh"));
  write_text(project_plugins / "com-example-same" / "plugin.json", tool_manifest_json("com-example-same", "plugin.sh"));
  auto enabled_first = ava::plugin::set_plugin_enabled(state_file, workspace, "com.example.same", true, ava::plugin::PluginScope::Project);
  auto enabled_second = ava::plugin::set_plugin_enabled(state_file, workspace, "com-example-same", true, ava::plugin::PluginScope::Project);
  expect(enabled_first.has_value() && enabled_second.has_value(), "collision plugin test enables both plugins");

  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  context.plugin_global_plugins_dir = root / "global-plugins";
  context.plugin_project_plugins_dir = project_plugins;
  context.plugin_enablement_file = state_file;

  auto const colliding_name = ava::plugin::plugin_model_tool_name("com.example.same", "todo_add");
  auto const schemas = ava::agent::ToolDispatcher::tool_schemas_json(context);
  auto const count = std::count_if(schemas.begin(), schemas.end(),
                                   [&](std::string const& schema) { return schema.find("\"name\":\"" + colliding_name + "\"") != std::string::npos; });
  expect(count == 1, "plugin registry exposes only one schema for colliding plugin tool names");
}

}  // namespace

void run_plugin_tests()
{
  test_plugin_manifest_parsing();
  test_static_plugin_resource_loader_is_descriptor_anchored();
  test_sample_todo_plugin_manifest_and_resources();
  test_sample_todo_plugin_protocol_path();
  test_plugin_runner_protocol_parsing();
  test_plugin_discovery();
  test_plugin_enablement();
  test_plugin_runner_initializes_and_shuts_down();
  test_plugin_runner_rejects_oversized_buffered_extra_records();
  test_plugin_runner_contained_failures();
  test_plugin_runner_tool_calls();
  test_plugin_runner_command_calls();
  test_plugin_runner_event_observation();
  test_enabled_plugin_dynamic_resources_list_and_read();
  test_disabled_plugin_dynamic_resources_do_not_execute();
  test_dynamic_resources_require_explicit_manifest_capability();
  test_dynamic_resource_read_rejects_invalid_names_before_launch();
  test_dynamic_resource_proxy_requests_use_core_service_handler();
  test_plugin_reported_dynamic_resource_errors_surface_text();
  test_dynamic_resource_commands_respect_prelaunch_cancellation();
  test_malformed_dynamic_resource_result_fails_safely();
  test_static_plugin_resources_remain_manifest_only();
  test_enabled_plugin_event_hooks_observe_runtime_events();
  test_plugin_event_hook_failures_report_to_opt_in_sink();
  test_plugin_tool_dispatcher();
  test_plugin_core_service_proxy_read_search_slice();
  test_plugin_tool_dispatcher_rejects_invalid_result();
  test_plugin_failure_diagnostics_are_support_safe();
  test_plugin_tool_registry_skips_name_collisions();
}
