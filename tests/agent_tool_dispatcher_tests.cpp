#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/process_test_support.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/event/events.h"
#include "ava/app/acp/session_update.h"
#include "ava/app/commands.h"
#include "ava/app/headless_policy.h"
#include "ava/app/print_mode.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime_event_adapters.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/agent/message_builder.h"
#include "ava/agent/mode.h"
#include "ava/agent/tool_dispatch_common.h"
#include "ava/agent/tool_dispatch_services.h"
#include "ava/agent/tool_dispatcher.h"
#include "ava/agent/tool_registration.h"
#include "ava/agent/tool_registry.h"
#include "ava/agent/tool_result.h"
#include "ava/agent/tool_summaries.h"
#include "ava/agent/tool_timeline.h"
#include "ava/tools/bash_tool.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/mutation_queue.h"
#include "ava/tools/search_tools.h"
#include "ava/tools/secure_workspace.h"
#include "ava/tools/tool_permission.h"
#include "ava/tui/composer.h"
#include "ava/tui/terminal.h"
#include "ava/plugin/enablement.h"
#include "ava/plugin/tool_broker.h"
#include "ava/mcp/config.h"
#include "ava/config/auth.h"
#include "ava/config/model_config.h"
#include "ava/config/openai_oauth.h"
#include "ava/config/prompt_config.h"
#include "ava/config/xdg_paths.h"
#include "ava/session/compaction.h"
#include "ava/session/export.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission.h"
#include "ava/permissions/permission_rules.h"
#include "ava/provider/openai_provider.h"
#include "ava/context/context_loader.h"
#include "ava/lsp/lsp_client.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/path.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class DispatcherExactFileAccess final : public ava::tools::ExactFileAccess
{
 public:
  [[nodiscard]] bool supports_read_text_file() const noexcept override { return supports_reads; }
  [[nodiscard]] bool supports_write_text_file() const noexcept override { return supports_writes; }

  [[nodiscard]] ava::core::Result<std::string> read_text_file(std::filesystem::path const& path, ava::tools::ToolIoCancelCallback) const override
  {
    ++reads;
    auto found = files.find(path);
    if (found == files.end())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::NotFound, "remote patch file missing"));
    return found->second;
  }

  [[nodiscard]] ava::core::VoidResult write_text_file(std::filesystem::path const& path, std::string_view content,
                                                      ava::tools::ToolIoCancelCallback) const override
  {
    ++writes;
    files[path] = std::string(content);
    return {};
  }

  mutable std::map<std::filesystem::path, std::string> files;
  mutable int reads = 0;
  mutable int writes = 0;
  bool supports_reads = true;
  bool supports_writes = true;
};

class EmptyDiagnosticsProvider final : public ava::lsp::DiagnosticsProvider
{
 public:
  [[nodiscard]] ava::core::Result<std::vector<ava::lsp::Diagnostic>> diagnostics(std::filesystem::path const&, ava::lsp::CancelCallback = nullptr) override
  {
    return std::vector<ava::lsp::Diagnostic>{};
  }
};

bool schemas_contain_tool(std::vector<std::string> const& schemas, std::string_view name)
{
  auto const needle = "\"name\":\"" + std::string(name) + "\"";
  return std::ranges::any_of(schemas, [&](std::string const& schema) { return schema.find(needle) != std::string::npos; });
}

void test_tool_dispatcher_plugin_tool_inclusion_control()
{
  auto const root = create_empty_root("dispatcher-plugin-tool-inclusion-control");
  auto const workspace = root / "workspace";
  auto const paths = ava::tests::app_test_paths(root);
  std::filesystem::create_directories(workspace);

  auto const plugin_id = "com.example.dispatch-isolation";
  ava::tests::write_app_test_file(paths.ava_config_dir / "plugins" / plugin_id / "plugin.json",
                                  ava::tests::app_test_plugin_manifest_json(plugin_id, "Dispatcher isolation canary"));
  auto enabled = ava::plugin::set_plugin_enabled(paths.ava_state_dir / "plugin-enablement.json", workspace, plugin_id, true, ava::plugin::PluginScope::Global);
  expect(enabled.has_value(), "dispatcher plugin-tool inclusion fixture enables its global plugin");

  ava::tools::ToolContext context;
  context.workspace_dir = workspace;
  context.plugin_global_plugins_dir = paths.ava_config_dir / "plugins";
  context.plugin_project_plugins_dir = workspace / ".ava" / "plugins";
  context.plugin_enablement_file = paths.ava_state_dir / "plugin-enablement.json";
  context.session_mcp_config = std::make_shared<ava::mcp::McpConfig const>();
  context.exact_builtin_tool_names = std::nullopt;
  auto const plugin_tool_name = ava::plugin::plugin_model_tool_name(plugin_id, "todo_add");
  auto contains = [](std::vector<ava::agent::ToolMetadata> const& metadata, std::string_view name) {
    return std::ranges::any_of(metadata, [name](ava::agent::ToolMetadata const& tool) { return tool.name == name; });
  };

  auto included = ava::agent::ToolDispatcher::create_strict(context);
  expect(included && contains(included->registered_tool_metadata(), "read_file") && contains(included->registered_tool_metadata(), plugin_tool_name),
         "ordinary dispatcher includes builtins and enabled plugin tools by default");

  context.include_plugin_tools = false;
  auto excluded = ava::agent::ToolDispatcher::create_strict(context);
  expect(excluded && contains(excluded->registered_tool_metadata(), "read_file") && !contains(excluded->registered_tool_metadata(), plugin_tool_name),
         "ordinary dispatcher can exclude plugin tools while preserving builtins and empty immutable session MCP");
}

using PermissionBits = std::filesystem::perms (*)(std::filesystem::path const&);
void test_dispatcher_visibility(std::filesystem::path const& workspace);
void test_dispatcher_read_search_and_validation(std::filesystem::path const& root, std::filesystem::path const& workspace,
                                                ava::agent::ToolDispatcher const& dispatcher);
void test_dispatcher_skill(std::filesystem::path const& root, std::filesystem::path const& workspace);
void test_dispatcher_cancellation(std::filesystem::path const& root, std::filesystem::path const& workspace);
void test_dispatcher_permission_audit(std::filesystem::path const& root, std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher);
void test_dispatcher_patch_result(ava::agent::ToolDispatcher const& dispatcher);
void test_dispatcher_exact_file_patch(std::filesystem::path const& workspace);
void test_dispatcher_diff_and_permissions(std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher, PermissionBits permission_bits);
void test_dispatcher_same_file_patch(std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher);
void test_dispatcher_external_permissions(std::filesystem::path const& root, std::filesystem::path const& workspace,
                                          ava::agent::ToolDispatcher const& dispatcher);
void test_dispatcher_transactional_staging(std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher);
void test_dispatcher_patch_cancellation_and_bounds(std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher);
void test_dispatcher_question(std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher);
void test_dispatcher_schemas_and_registry();

void test_tool_dispatcher()
{
  auto const root = create_empty_root("dispatcher");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const permission_bits = [](std::filesystem::path const& permission_path) {
    constexpr auto mask = std::filesystem::perms::owner_all | std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    std::error_code status_error;
    return std::filesystem::status(permission_path, status_error).permissions() & mask;
  };
  ava::tests::write_test_file(workspace / "note.txt", "hello dispatcher");
  ava::tests::write_test_file(workspace / "lines.txt", "one\nTwo\nthree\n");

  auto const dispatcher_result =
      ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build});
  if (!ava::tests::require(dispatcher_result.has_value(), dispatcher_result ? "dispatcher fixture composes" : dispatcher_result.error().format()))
    return;
  auto const& dispatcher = *dispatcher_result;
  test_dispatcher_visibility(workspace);
  test_dispatcher_read_search_and_validation(root, workspace, dispatcher);
  test_dispatcher_skill(root, workspace);
  test_dispatcher_cancellation(root, workspace);
  test_dispatcher_permission_audit(root, workspace, dispatcher);
  test_dispatcher_patch_result(dispatcher);
  test_dispatcher_exact_file_patch(workspace);
  test_dispatcher_diff_and_permissions(workspace, dispatcher, permission_bits);
  test_dispatcher_same_file_patch(workspace, dispatcher);
  test_dispatcher_external_permissions(root, workspace, dispatcher);
  test_dispatcher_transactional_staging(workspace, dispatcher);
  test_dispatcher_patch_cancellation_and_bounds(workspace, dispatcher);
  test_dispatcher_question(workspace, dispatcher);
  test_dispatcher_schemas_and_registry();
}

void test_dispatcher_visibility(std::filesystem::path const& workspace)
{
  {
    ava::tools::ToolContext allowlisted_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build};
    ava::agent::ToolVisibilityOptions const allowlisted_visibility{.included_tools = {"read_file", "grep"}};
    auto const schemas_result = ava::agent::ToolDispatcher::compose_tool_schemas_json(allowlisted_context, allowlisted_visibility);
    if (!ava::tests::require(schemas_result.has_value(), schemas_result ? "schema fixture composes" : schemas_result.error().format()))
      return;
    auto const& schemas = *schemas_result;
    expect(schemas_contain_tool(schemas, "read_file") && schemas_contain_tool(schemas, "grep") && !schemas_contain_tool(schemas, "bash") &&
               !schemas_contain_tool(schemas, "write_file"),
           "tool visibility allowlist limits exported provider schemas");

    auto const allowlisted_dispatcher_result = ava::agent::ToolDispatcher::create_strict(allowlisted_context, {}, allowlisted_visibility);
    if (!ava::tests::require(allowlisted_dispatcher_result.has_value(),
                             allowlisted_dispatcher_result ? "dispatcher fixture composes" : allowlisted_dispatcher_result.error().format()))
      return;
    auto const& allowlisted_dispatcher = *allowlisted_dispatcher_result;
    auto hidden =
        allowlisted_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_hidden_bash", .name = "bash", .arguments_json = "{\"command\":\"pwd\"}"});
    expect(hidden && !hidden->success && hidden->result_text.find("unknown tool") != std::string::npos,
           "tool visibility allowlist removes hidden tools from dispatch");
  }
  {
    ava::tools::ToolContext excluded_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build};
    ava::agent::ToolVisibilityOptions const excluded_visibility{.included_tools = {"read_file", "grep"}, .excluded_tools = {"grep"}};
    auto const schemas_result = ava::agent::ToolDispatcher::compose_tool_schemas_json(excluded_context, excluded_visibility);
    if (!ava::tests::require(schemas_result.has_value(), schemas_result ? "schema fixture composes" : schemas_result.error().format()))
      return;
    auto const& schemas = *schemas_result;
    expect(schemas_contain_tool(schemas, "read_file") && !schemas_contain_tool(schemas, "grep"), "tool visibility exclusion overrides an explicit allowlist");
  }
  {
    ava::tools::ToolContext pi_alias_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build};
    ava::agent::ToolVisibilityOptions const pi_alias_visibility{.included_tools = {"read", "grep", "find", "ls"}, .excluded_tools = {"find"}};
    auto const schemas_result = ava::agent::ToolDispatcher::compose_tool_schemas_json(pi_alias_context, pi_alias_visibility);
    if (!ava::tests::require(schemas_result.has_value(), schemas_result ? "schema fixture composes" : schemas_result.error().format()))
      return;
    auto const& schemas = *schemas_result;
    expect(schemas_contain_tool(schemas, "read_file") && schemas_contain_tool(schemas, "grep") && schemas_contain_tool(schemas, "list_directory") &&
               !schemas_contain_tool(schemas, "glob") && !schemas_contain_tool(schemas, "write_file"),
           "tool visibility accepts Pi read/find/ls aliases while exporting native AVA schema names");
  }
  {
    ava::tools::ToolContext no_tools_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build};
    ava::agent::ToolVisibilityOptions const no_tools_visibility{.mode = ava::agent::ToolVisibilityMode::NoTools};
    auto const schemas_result = ava::agent::ToolDispatcher::compose_tool_schemas_json(no_tools_context, no_tools_visibility);
    if (!ava::tests::require(schemas_result.has_value(), schemas_result ? "schema fixture composes" : schemas_result.error().format()))
      return;
    auto const& schemas = *schemas_result;
    expect(schemas.empty(), "tool visibility no-tools hides built-in provider schemas");

    auto const no_tools_dispatcher_result = ava::agent::ToolDispatcher::create_strict(no_tools_context, {}, no_tools_visibility);
    if (!ava::tests::require(no_tools_dispatcher_result.has_value(),
                             no_tools_dispatcher_result ? "dispatcher fixture composes" : no_tools_dispatcher_result.error().format()))
      return;
    auto const& no_tools_dispatcher = *no_tools_dispatcher_result;
    auto hidden =
        no_tools_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_hidden_read", .name = "read_file", .arguments_json = "{\"path\":\"note.txt\"}"});
    expect(hidden && !hidden->success && hidden->result_text.find("unknown tool") != std::string::npos,
           "tool visibility no-tools removes built-in tools from dispatch");
  }
  {
    ava::tools::ToolContext no_builtin_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build};
    ava::agent::ToolVisibilityOptions const no_builtin_visibility{.mode = ava::agent::ToolVisibilityMode::NoBuiltinTools};
    auto const schemas_result = ava::agent::ToolDispatcher::compose_tool_schemas_json(no_builtin_context, no_builtin_visibility);
    if (!ava::tests::require(schemas_result.has_value(), schemas_result ? "schema fixture composes" : schemas_result.error().format()))
      return;
    auto const& schemas = *schemas_result;
    expect(schemas.empty(), "tool visibility no-builtin-tools hides built-in provider schemas when no external tools exist");
  }
}

void test_dispatcher_read_search_and_validation(std::filesystem::path const& root, std::filesystem::path const& workspace,
                                                ava::agent::ToolDispatcher const& dispatcher)
{
  auto read =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_read", .name = "read_file", .arguments_json = "{\"path\":\"note.txt\",\"max_bytes\":5}"});
  expect(read && read->success && read->result_text.find("hello") != std::string::npos, "tool dispatcher maps read_file provider call to file tool");
  auto const read_structured = read ? ava::agent::serialize_tool_result_payload_json(*read) : std::string{};
  expect(read && read_structured.find("\"changed_paths\"") == std::string::npos, "tool dispatcher keeps read-only file results out of changed_paths");
  auto read_range = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_read_range", .name = "read_file", .arguments_json = "{\"path\":\"lines.txt\",\"offset\":2,\"limit\":1}"});
  expect(read_range && read_range->success && read_range->result_text.find("Two\\n") != std::string::npos &&
             read_range->result_text.find("\"start_line\":2") != std::string::npos && read_range->result_text.find("\"output_lines\":1") != std::string::npos &&
             read_range->result_text.find("\"next_offset\":3") != std::string::npos &&
             read_range->result_text.find("\"next_offset_line\":3") != std::string::npos,
         "tool dispatcher exposes read_file line offset and continuation metadata");

  auto listed = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_list_directory", .name = "list_directory", .arguments_json = "{\"path\":\".\",\"max_entries\":20}"});
  auto const listed_structured = listed ? ava::agent::serialize_tool_result_payload_json(*listed) : std::string{};
  expect(listed && listed->success && listed->result_text.find("\"name\":\"note.txt\"") != std::string::npos &&
             listed->result_text.find("\"type\":\"file\"") != std::string::npos,
         "tool dispatcher maps list_directory provider call to directory listing tool");
  expect(listed && listed_structured.find("\"changed_paths\"") == std::string::npos, "tool dispatcher keeps directory listings out of changed_paths");
  auto const list_arguments = ava::agent::summarize_tool_arguments(
      ava::agent::ProviderToolCall{.id = "call_list_directory", .name = "list_directory", .arguments_json = "{\"path\":\".\",\"max_entries\":20}"});
  auto const default_list_arguments =
      ava::agent::summarize_tool_arguments(ava::agent::ProviderToolCall{.id = "call_list_directory_default", .name = "list_directory", .arguments_json = "{}"});
  auto const empty_list_arguments = ava::agent::summarize_tool_arguments(
      ava::agent::ProviderToolCall{.id = "call_list_directory_empty", .name = "list_directory", .arguments_json = "{\"path\":\"\"}"});
  auto const list_result = listed ? ava::agent::summarize_tool_result(*listed) : std::string{};
  expect(list_arguments == "path=., max_entries=20" && default_list_arguments == "path=." && empty_list_arguments == "path=." &&
             list_result.find("entries") != std::string::npos && list_arguments.find("arguments provided") == std::string::npos && list_result != "ok",
         "tool dispatcher summarizes list_directory paths, bounds, and results without generic placeholders");

  auto grep_case_insensitive = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_grep_ci",
                                                                                .name = "grep",
                                                                                .arguments_json = "{\"pattern\":\"HELLO\",\"include\":\"note.txt\","
                                                                                                  "\"case_insensitive\":true}"});
  expect(grep_case_insensitive && grep_case_insensitive->success && grep_case_insensitive->result_text.find("\"case_insensitive\":true") != std::string::npos &&
             grep_case_insensitive->result_text.find("hello dispatcher") != std::string::npos,
         "tool dispatcher passes case_insensitive grep through to search tools");
  auto grep_regex = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_grep_regex",
                                                                     .name = "grep",
                                                                     .arguments_json = "{\"pattern\":\"hello (dispatcher|missing)\",\"include\":\"note.txt\","
                                                                                       "\"literal\":false}"});
  expect(grep_regex && grep_regex->success && grep_regex->result_text.find("\"literal\":false") != std::string::npos &&
             grep_regex->result_text.find("hello dispatcher") != std::string::npos,
         "tool dispatcher passes regex grep through to search tools");

  auto control_call_id = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = std::string("call_") + '\x01' + "bad", .name = "read_file", .arguments_json = "{\"path\":\"note.txt\"}"});
  expect(!control_call_id && control_call_id.error().message().find("control byte") != std::string::npos,
         "tool dispatcher rejects provider call ids with control bytes before tool use");

  auto long_call_id =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = std::string(300, 'a'), .name = "read_file", .arguments_json = "{\"path\":\"note.txt\"}"});
  expect(!long_call_id && long_call_id.error().message().find("too long") != std::string::npos,
         "tool dispatcher rejects overlong provider call ids before tool use");

  auto nul_path =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_nul_path", .name = "read_file", .arguments_json = "{\"path\":\"note\\u0000.txt\"}"});
  expect(nul_path && !nul_path->success && nul_path->result_text.find("control byte") != std::string::npos,
         "tool dispatcher rejects NUL bytes decoded into file paths");

  auto nul_command =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_nul_command", .name = "bash", .arguments_json = "{\"command\":\"pwd\\u0000whoami\"}"});
  expect(nul_command && !nul_command->success && nul_command->result_text.find("control byte") != std::string::npos,
         "tool dispatcher rejects NUL bytes decoded into commands");

  auto nul_content = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_nul_content", .name = "write_file", .arguments_json = "{\"path\":\"nul.txt\",\"content\":\"bad\\u0000text\"}"});
  expect(
      nul_content && !nul_content->success && nul_content->result_text.find("NUL byte") != std::string::npos && !std::filesystem::exists(workspace / "nul.txt"),
      "tool dispatcher rejects NUL bytes in text arguments before writing");

  auto nul_include = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_nul_include", .name = "grep", .arguments_json = "{\"pattern\":\"hello\",\"include\":\"**/*\\u0000\"}"});
  expect(nul_include && !nul_include->success && nul_include->result_text.find("control byte") != std::string::npos,
         "tool dispatcher rejects NUL bytes decoded into grep include globs");

  ava::tests::write_test_file(workspace / ".gitignore", "ignored.txt\n");
  ava::tests::write_test_file(workspace / "ignored.txt", "hello ignored dispatcher");
  auto ignored_glob =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_ignored_glob", .name = "glob", .arguments_json = "{\"pattern\":\"ignored.txt\"}"});
  expect(ignored_glob && ignored_glob->success && ignored_glob->result_text.find("\"paths\":[]") != std::string::npos &&
             ignored_glob->result_text.find("no_ignore") == std::string::npos,
         "tool dispatcher keeps glob .gitignore filtering enabled by default");

  auto no_ignore_glob = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_no_ignore_glob", .name = "glob", .arguments_json = "{\"pattern\":\"ignored.txt\",\"no_ignore\":true}"});
  expect(no_ignore_glob && !no_ignore_glob->success && no_ignore_glob->result_text.find("explicit local control") != std::string::npos,
         "tool dispatcher rejects provider-controlled glob no_ignore");

  auto no_ignore_grep = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_no_ignore_grep", .name = "grep", .arguments_json = "{\"pattern\":\"ignored dispatcher\",\"no_ignore\":true}"});
  expect(no_ignore_grep && !no_ignore_grep->success && no_ignore_grep->result_text.find("explicit local control") != std::string::npos,
         "tool dispatcher rejects provider-controlled grep no_ignore");

  auto const dispatcher_spill_dir = root / "session" / "spill";
  auto const spilling_dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace, .spill_dir = dispatcher_spill_dir, .mode = ava::agent::Mode::Build});
  if (!ava::tests::require(spilling_dispatcher_result.has_value(),
                           spilling_dispatcher_result ? "dispatcher fixture composes" : spilling_dispatcher_result.error().format()))
    return;
  auto const& spilling_dispatcher = *spilling_dispatcher_result;
  auto dispatcher_spill = spilling_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call/dispatcher-spill", .name = "glob", .arguments_json = "{\"pattern\":\"**/*\",\"max_results\":1}"});
  auto const dispatcher_spill_file =
      dispatcher_spill ? ava::core::json::string_field(dispatcher_spill->result_text, "spill_file") : std::optional<std::string>{};
  expect(dispatcher_spill && dispatcher_spill->success && dispatcher_spill_file &&
             dispatcher_spill->result_text.find("\"spill_truncated\":false") != std::string::npos &&
             std::filesystem::path(*dispatcher_spill_file).parent_path().empty(),
         "tool dispatcher includes local-only spill metadata without exposing absolute spill paths");

  auto bad_no_ignore = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_bad_no_ignore", .name = "glob", .arguments_json = "{\"pattern\":\"**/*\",\"no_ignore\":\"yes\"}"});
  expect(bad_no_ignore && !bad_no_ignore->success && bad_no_ignore->result_text.find("boolean") != std::string::npos,
         "tool dispatcher rejects non-boolean no_ignore arguments");

  auto bad_webfetch =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_bad_webfetch", .name = "webfetch", .arguments_json = "{\"url\":\"file:///etc/passwd\"}"});
  expect(bad_webfetch && !bad_webfetch->success && bad_webfetch->result_text.find("http") != std::string::npos,
         "tool dispatcher rejects unsupported webfetch URL schemes before network access");
}

void test_dispatcher_skill(std::filesystem::path const& root, std::filesystem::path const& workspace)
{
  auto const skill_root = root / "project-skills";
  std::filesystem::create_directories(skill_root / "debugging");
  ava::tests::write_test_file(skill_root / "debugging" / "SKILL.md", "---\nname: debugging\ndescription: Debug failures\n---\nUse systematic debugging.\n");
  int skill_permission_prompts = 0;
  auto const skill_dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&skill_permission_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++skill_permission_prompts;
        expect(prompt.operation == ava::permissions::Operation::SkillLoad, "skill tool requests skill-load permission");
        expect(prompt.command == "debugging", "skill permission prompt carries skill name");
        return ava::permissions::PermissionResolution::Allow;
      },
      .plugin_global_plugins_dir = root / "no-global-plugins",
      .plugin_project_plugins_dir = root / "no-project-plugins",
      .mcp_global_config_file = root / "no-global-mcp.json",
      .mcp_project_config_file = root / "no-project-mcp.json",
      .skill_global_dirs = {root / "no-global-skills"},
      .skill_project_dirs = {skill_root}});
  if (!ava::tests::require(skill_dispatcher_result.has_value(),
                           skill_dispatcher_result ? "dispatcher fixture composes" : skill_dispatcher_result.error().format()))
    return;
  auto const& skill_dispatcher = *skill_dispatcher_result;
  auto loaded_skill =
      skill_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_skill", .name = "skill", .arguments_json = "{\"name\":\"debugging\"}"});
  expect(loaded_skill && loaded_skill->success && loaded_skill->result_text.find("Use systematic debugging") != std::string::npos &&
             loaded_skill->result_text.find("Base directory for this skill") != std::string::npos && skill_permission_prompts == 1,
         "tool dispatcher exposes local SKILL.md content through the skill tool");
}

void test_dispatcher_cancellation(std::filesystem::path const& root, std::filesystem::path const& workspace)
{
  ava::agent::ToolDispatcher const canceled_dispatcher(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .cancel_requested = [] { return true; },
  });
  auto canceled_glob =
      canceled_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_canceled_glob", .name = "glob", .arguments_json = "{\"pattern\":\"**/*\"}"});
  auto const canceled_structured = canceled_glob ? ava::agent::serialize_tool_result_payload_json(*canceled_glob) : std::string{};
  expect(canceled_glob && !canceled_glob->success && canceled_glob->payload.status == ava::agent::ToolResultStatus::Canceled &&
             canceled_structured.find("\"status\":\"canceled\"") != std::string::npos,
         "tool dispatcher maps tool cancellation errors to semantic canceled payloads");
  auto canceled_read =
      canceled_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_canceled_read", .name = "read_file", .arguments_json = "{\"path\":\"note.txt\"}"});
  auto canceled_write = canceled_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_canceled_write", .name = "write_file", .arguments_json = "{\"path\":\"cancel.txt\",\"content\":\"bad\"}"});
  auto canceled_edit = canceled_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "call_canceled_edit", .name = "edit_file", .arguments_json = "{\"path\":\"note.txt\",\"old_text\":\"hello\",\"new_text\":\"bad\"}"});
  expect(canceled_read && canceled_write && canceled_edit && !canceled_read->success && !canceled_write->success && !canceled_edit->success &&
             canceled_read->payload.status == ava::agent::ToolResultStatus::Canceled &&
             canceled_write->payload.status == ava::agent::ToolResultStatus::Canceled &&
             canceled_edit->payload.status == ava::agent::ToolResultStatus::Canceled && !std::filesystem::exists(workspace / "cancel.txt"),
         "tool dispatcher reports semantic cancellation for file tools without filesystem mutation");

  auto const canceled_outside_path = root / "dispatcher-canceled-outside.txt";
  ava::tests::write_test_file(canceled_outside_path, "outside canceled");
  int canceled_permission_prompts = 0;
  ava::agent::ToolDispatcher const canceled_permission_dispatcher(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&canceled_permission_prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++canceled_permission_prompts;
        return ava::permissions::PermissionResolution::Allow;
      },
      .cancel_requested = [] { return true; }});
  auto canceled_outside_read = canceled_permission_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_canceled_outside_read",
                                   .name = "read_file",
                                   .arguments_json = "{\"path\":\"" + ava::core::json::escape(canceled_outside_path.generic_string()) + "\"}"});
  expect(canceled_outside_read && !canceled_outside_read->success && canceled_outside_read->payload.status == ava::agent::ToolResultStatus::Canceled &&
             canceled_permission_prompts == 0,
         "tool dispatcher cancellation prevents permission prompts for file tools");
}

void test_dispatcher_permission_audit(std::filesystem::path const& root, std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher)
{
  auto const linked_permission_path = root / "dispatcher-linked-permission.txt";
  ava::tests::write_test_file(linked_permission_path, "linked permission");
  std::vector<ava::tools::PermissionAuditEvent> linked_permission_audits;
  std::string linked_prompt_request_id;
  auto const linked_permission_dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&linked_prompt_request_id](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        linked_prompt_request_id = prompt.permission_request_id;
        return ava::permissions::PermissionResolution::Allow;
      },
      .permission_audit_sink = [&linked_permission_audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
        linked_permission_audits.push_back(event);
        return {};
      }});
  if (!ava::tests::require(linked_permission_dispatcher_result.has_value(),
                           linked_permission_dispatcher_result ? "dispatcher fixture composes" : linked_permission_dispatcher_result.error().format()))
    return;
  auto const& linked_permission_dispatcher = *linked_permission_dispatcher_result;
  auto linked_permission_read = linked_permission_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_linked_permission_read",
                                   .name = "read_file",
                                   .arguments_json = "{\"path\":\"" + ava::core::json::escape(linked_permission_path.generic_string()) + "\"}"});
  auto const linked_permission_structured = linked_permission_read ? ava::agent::serialize_tool_result_payload_json(*linked_permission_read) : std::string{};
  auto const linked_permission_ids = ava::core::json::strings_in_array_field(linked_permission_structured, "permission_request_ids");
  expect(linked_permission_read && linked_permission_read->success && linked_permission_audits.size() == 2 && !linked_prompt_request_id.empty() &&
             linked_permission_ids.size() == 1 && linked_permission_ids[0] == linked_prompt_request_id &&
             linked_permission_audits[0].permission_request_id == linked_prompt_request_id &&
             linked_permission_audits[1].permission_request_id == linked_prompt_request_id,
         "tool dispatcher links structured tool results to permission audit request ids");

  auto const first_per_dispatch_permission_path = root / "dispatcher-per-dispatch-one.txt";
  auto const second_per_dispatch_permission_path = root / "dispatcher-per-dispatch-two.txt";
  ava::tests::write_test_file(first_per_dispatch_permission_path, "first per-dispatch permission");
  ava::tests::write_test_file(second_per_dispatch_permission_path, "second per-dispatch permission");
  std::vector<std::string> per_dispatch_permission_request_ids;
  auto const per_dispatch_permission_dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [&per_dispatch_permission_request_ids](
                                 ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        per_dispatch_permission_request_ids.push_back(prompt.permission_request_id);
        return ava::permissions::PermissionResolution::Allow;
      }});
  if (!ava::tests::require(per_dispatch_permission_dispatcher_result.has_value(), per_dispatch_permission_dispatcher_result
                                                                                      ? "dispatcher fixture composes"
                                                                                      : per_dispatch_permission_dispatcher_result.error().format()))
    return;
  auto const& per_dispatch_permission_dispatcher = *per_dispatch_permission_dispatcher_result;
  auto first_per_dispatch_permission_read = per_dispatch_permission_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_first_per_dispatch_permission",
                                   .name = "read_file",
                                   .arguments_json = "{\"path\":\"" + ava::core::json::escape(first_per_dispatch_permission_path.generic_string()) + "\"}"});
  auto second_per_dispatch_permission_read = per_dispatch_permission_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_second_per_dispatch_permission",
                                   .name = "read_file",
                                   .arguments_json = "{\"path\":\"" + ava::core::json::escape(second_per_dispatch_permission_path.generic_string()) + "\"}"});
  expect(first_per_dispatch_permission_read && second_per_dispatch_permission_read && first_per_dispatch_permission_read->success &&
             second_per_dispatch_permission_read->success && per_dispatch_permission_request_ids.size() == 2 &&
             per_dispatch_permission_request_ids[0].starts_with("permreq_") && per_dispatch_permission_request_ids[1].starts_with("permreq_") &&
             per_dispatch_permission_request_ids[0] != per_dispatch_permission_request_ids[1] &&
             first_per_dispatch_permission_read->payload.permission_request_ids.size() == 1 &&
             second_per_dispatch_permission_read->payload.permission_request_ids.size() == 1 &&
             first_per_dispatch_permission_read->payload.permission_request_ids[0] == per_dispatch_permission_request_ids[0] &&
             second_per_dispatch_permission_read->payload.permission_request_ids[0] == per_dispatch_permission_request_ids[1],
         "tool dispatcher keeps each dispatch result attached to its own permission request id");

  auto malformed_args = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_bad_args", .name = "read_file", .arguments_json = "{not-json}"});
  expect(malformed_args && !malformed_args->success && malformed_args->result_text.find("required") != std::string::npos,
         "tool dispatcher returns structured errors for malformed tool arguments");
  auto const malformed_structured = malformed_args ? ava::agent::serialize_tool_result_payload_json(*malformed_args) : std::string{};
  expect(malformed_args && malformed_args->payload.status == ava::agent::ToolResultStatus::Error &&
             malformed_args->payload.error_category == "invalid_argument" && malformed_args->payload.error_message.find("required") != std::string::npos &&
             malformed_structured.find("\"error\"") != std::string::npos,
         "tool dispatcher semantic payload carries error category and message");
}

void test_dispatcher_patch_result(ava::agent::ToolDispatcher const& dispatcher)
{
  auto patch = dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "call_patch", .name = "apply_patch", .arguments_json = "{\"edits\":[{\"path\":\"note.txt\",\"old_text\":\"dispatcher\",\"new_text\":\"patch\"}]}"});
  auto const patch_diff = patch ? ava::core::json::string_field(patch->result_text, "diff") : std::optional<std::string>{};
  expect(patch && patch->success && patch->result_text.find("apply_patch") != std::string::npos && patch_diff &&
             patch_diff->find("--- ") != std::string::npos && patch_diff->find("-hello dispatcher") != std::string::npos &&
             patch_diff->find("+hello patch") != std::string::npos && patch_diff->size() <= 32 * 1024 &&
             patch->result_text.find("\"diff_truncated\":false") != std::string::npos,
         "tool dispatcher applies exact patch edits and returns a bounded unified diff");
  auto const patch_structured = patch ? ava::agent::serialize_tool_result_payload_json(*patch) : std::string{};
  expect(patch && patch_diff && patch->payload.status == ava::agent::ToolResultStatus::Success && patch->payload.content_type == "application/json" &&
             patch->payload.diff == *patch_diff && patch->payload.changed_paths.size() == 1 &&
             patch->payload.changed_paths[0].find("note.txt") != std::string::npos && patch_structured.find("\"status\":\"success\"") != std::string::npos &&
             patch_structured.find("\"content_type\":\"application/json\"") != std::string::npos &&
             patch_structured.find("\"changed_paths\"") != std::string::npos,
         "tool dispatcher attaches a structured semantic payload alongside legacy tool result JSON");
  auto patched_read =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_patched_read", .name = "read_file", .arguments_json = "{\"path\":\"note.txt\"}"});
  expect(patched_read && patched_read->result_text.find("hello patch") != std::string::npos, "apply_patch updates file content through file tools");
}

void test_dispatcher_exact_file_patch(std::filesystem::path const& workspace)
{
  auto remote_workspace = ava::tools::SecureWorkspace::open(ava::core::normalized_absolute_path(workspace));
  auto remote_files = std::make_shared<DispatcherExactFileAccess>();
  auto const remote_patch_path = ava::core::normalized_absolute_path(workspace) / "remote-patch.txt";
  remote_files->files[remote_patch_path] = "remote old stale";
  auto const remote_patch_dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{
      .workspace_dir = ava::core::normalized_absolute_path(workspace),
      .mode = ava::agent::Mode::Build,
      .secure_workspace = remote_workspace ? *remote_workspace : nullptr,
      .exact_file_access = remote_files,
  });
  if (!ava::tests::require(remote_patch_dispatcher_result.has_value(),
                           remote_patch_dispatcher_result ? "dispatcher fixture composes" : remote_patch_dispatcher_result.error().format()))
    return;
  auto const& remote_patch_dispatcher = *remote_patch_dispatcher_result;
  auto remote_patch = remote_patch_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_remote_patch",
                                   .name = "apply_patch",
                                   .arguments_json = "{\"edits\":[{\"path\":\"remote-patch.txt\",\"old_text\":\"old\",\"new_text\":\"new\"},"
                                                     "{\"path\":\"./remote-patch.txt\",\"old_text\":\"stale\",\"new_text\":\"fresh\"}]}"});
  expect(remote_workspace && remote_patch && remote_patch->success && remote_files->reads == 1 && remote_files->writes == 1 &&
             remote_files->files[remote_patch_path] == "remote new fresh" && !std::filesystem::exists(workspace / "remote-patch.txt"),
         "apply_patch supports multiple exact-file adapter edits to one distinct target");

  auto multi_remote_files = std::make_shared<DispatcherExactFileAccess>();
  auto const multi_remote_a = ava::core::normalized_absolute_path(workspace) / "remote-multi-a.txt";
  auto const multi_remote_b = ava::core::normalized_absolute_path(workspace) / "remote-multi-b.txt";
  multi_remote_files->files[multi_remote_a] = "remote alpha old";
  multi_remote_files->files[multi_remote_b] = "remote beta old";
  auto multi_remote_started = std::make_shared<std::atomic_bool>(false);
  int multi_remote_progress = 0;
  auto const multi_remote_dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{
      .workspace_dir = ava::core::normalized_absolute_path(workspace),
      .mode = ava::agent::Mode::Build,
      .progress_sink = [&multi_remote_progress](ava::tools::ToolProgressEvent const&) -> ava::core::VoidResult {
        ++multi_remote_progress;
        return {};
      },
      .announce_execution_after_permission = true,
      .execution_started = multi_remote_started,
      .secure_workspace = remote_workspace ? *remote_workspace : nullptr,
      .exact_file_access = multi_remote_files,
  });
  if (!ava::tests::require(multi_remote_dispatcher_result.has_value(),
                           multi_remote_dispatcher_result ? "dispatcher fixture composes" : multi_remote_dispatcher_result.error().format()))
    return;
  auto const& multi_remote_dispatcher = *multi_remote_dispatcher_result;
  auto multi_remote_patch = multi_remote_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_remote_multi_patch",
                                   .name = "apply_patch",
                                   .arguments_json = "{\"edits\":[{\"path\":\"remote-multi-a.txt\",\"old_text\":\"old\",\"new_text\":\"new\"},"
                                                     "{\"path\":\"remote-multi-b.txt\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
  expect(multi_remote_patch && !multi_remote_patch->success && multi_remote_patch->result_text.find("invalid_argument") != std::string::npos &&
             multi_remote_patch->result_text.find("transactional multi-file client apply_patch calls are unsupported") != std::string::npos &&
             multi_remote_files->reads == 0 && multi_remote_files->writes == 0 && multi_remote_files->files[multi_remote_a] == "remote alpha old" &&
             multi_remote_files->files[multi_remote_b] == "remote beta old" && !multi_remote_started->load(std::memory_order_acquire) &&
             multi_remote_progress == 0,
         "apply_patch rejects exact-file adapter multi-target calls before execution announcement or adapter writes");

  auto partial_remote_files = std::make_shared<DispatcherExactFileAccess>();
  partial_remote_files->supports_writes = false;
  int partial_patch_permissions = 0;
  int partial_patch_progress = 0;
  auto const partial_patch_dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{
      .workspace_dir = ava::core::normalized_absolute_path(workspace),
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [&](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++partial_patch_permissions;
        return ava::permissions::PermissionResolution::Allow;
      },
      .progress_sink = [&](ava::tools::ToolProgressEvent const&) -> ava::core::VoidResult {
        ++partial_patch_progress;
        return {};
      },
      .require_explicit_file_permissions = true,
      .secure_workspace = remote_workspace ? *remote_workspace : nullptr,
      .exact_file_access = partial_remote_files,
  });
  if (!ava::tests::require(partial_patch_dispatcher_result.has_value(),
                           partial_patch_dispatcher_result ? "dispatcher fixture composes" : partial_patch_dispatcher_result.error().format()))
    return;
  auto const& partial_patch_dispatcher = *partial_patch_dispatcher_result;
  auto partial_capability_patch = partial_patch_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_partial_patch",
                                   .name = "apply_patch",
                                   .arguments_json = "{\"edits\":[{\"path\":\"remote-patch.txt\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
  expect(partial_capability_patch && !partial_capability_patch->success &&
             partial_capability_patch->result_text.find("capabilities are partial") != std::string::npos && partial_patch_permissions == 0 &&
             partial_patch_progress == 0 && partial_remote_files->reads == 0 && partial_remote_files->writes == 0,
         "partial exact-file capabilities reject apply_patch before permissions, progress, or I/O");
}

void test_dispatcher_diff_and_permissions(std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher, PermissionBits permission_bits)
{
  ava::tests::write_test_file(workspace / "edit-diff.txt", "red green blue\n");
  auto edit_diff_result = dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "call_edit_diff", .name = "edit_file", .arguments_json = "{\"path\":\"edit-diff.txt\",\"old_text\":\"green\",\"new_text\":\"gold\"}"});
  auto const edit_diff = edit_diff_result ? ava::core::json::string_field(edit_diff_result->result_text, "diff") : std::optional<std::string>{};
  expect(edit_diff_result && edit_diff_result->success && edit_diff && edit_diff->find("-red green blue") != std::string::npos &&
             edit_diff->find("+red gold blue") != std::string::npos && edit_diff->size() <= 32 * 1024 &&
             edit_diff_result->result_text.find("\"diff_truncated\":false") != std::string::npos,
         "edit_file provider result includes a bounded unified diff preview");

  auto const private_patch_path = workspace / "private-patch.txt";
  ava::tests::write_test_file(private_patch_path, "private old");
  std::error_code chmod_error;
  std::filesystem::permissions(private_patch_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, chmod_error);
  expect(!chmod_error, "test can set private patch file permissions");
  auto private_patch = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_private_patch",
                                   .name = "apply_patch",
                                   .arguments_json = "{\"edits\":[{\"path\":\"private-patch.txt\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
  auto private_patch_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, private_patch_path);
  expect(private_patch && private_patch->success && private_patch_read && private_patch_read->content == "private new" &&
             permission_bits(private_patch_path) == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write),
         "apply_patch preserves 0600 permissions when replacing an existing file");

  auto const audit_patch_path = workspace / "audit-patch.txt";
  ava::tests::write_test_file(audit_patch_path, "audit old");
  std::vector<ava::tools::PermissionAuditEvent> patch_audits;
  auto const audit_patch_dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace,
                              .mode = ava::agent::Mode::Build,
                              .permission_audit_sink = [&patch_audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
                                patch_audits.push_back(event);
                                return {};
                              }});
  if (!ava::tests::require(audit_patch_dispatcher_result.has_value(),
                           audit_patch_dispatcher_result ? "dispatcher fixture composes" : audit_patch_dispatcher_result.error().format()))
    return;
  auto const& audit_patch_dispatcher = *audit_patch_dispatcher_result;
  auto audited_patch = audit_patch_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_audit_patch",
                                   .name = "apply_patch",
                                   .arguments_json = "{\"edits\":[{\"path\":\"audit-patch.txt\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
  expect(audited_patch && audited_patch->success && patch_audits.size() == 2 && patch_audits[0].operation == ava::permissions::Operation::ReadFile &&
             patch_audits[0].tool_name == "apply_patch" && patch_audits[1].operation == ava::permissions::Operation::EditFile &&
             patch_audits[1].tool_name == "apply_patch",
         "apply_patch audits read permission before edit permission");
}

void test_dispatcher_same_file_patch(std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher)
{
  ava::tests::write_test_file(workspace / "sequential.txt", "one two");
  auto sequential_patch =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_sequential_patch",
                                                       .name = "apply_patch",
                                                       .arguments_json = "{\"edits\":[{\"path\":\"sequential.txt\",\"old_text\":\"one\",\"new_text\":\"two\"},"
                                                                         "{\"path\":\"sequential.txt\",\"old_text\":\"two\",\"new_text\":\"three\"}]}"});
  auto sequential_read =
      ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, workspace / "sequential.txt");
  expect(sequential_patch && sequential_patch->success && sequential_read && sequential_read->content == "two three",
         "apply_patch validates same-file edits against original content before applying replacements");

  ava::tests::write_test_file(workspace / "alias.txt", "alpha gamma");
  auto alias_patch =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_alias_patch",
                                                       .name = "apply_patch",
                                                       .arguments_json = "{\"edits\":[{\"path\":\"./alias.txt\",\"old_text\":\"alpha\",\"new_text\":\"beta\"},"
                                                                         "{\"path\":\"alias.txt\",\"old_text\":\"gamma\",\"new_text\":\"delta\"}]}"});
  auto alias_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, workspace / "alias.txt");
  expect(alias_patch && alias_patch->success && alias_read && alias_read->content == "beta delta",
         "apply_patch canonicalizes aliased same-file paths before staging writes");

  ava::tests::write_test_file(workspace / "multi-diff.txt", "A\nB\nC\nD\nE\n");
  auto multi_diff_patch =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_multi_diff_patch",
                                                       .name = "apply_patch",
                                                       .arguments_json = "{\"edits\":[{\"path\":\"multi-diff.txt\",\"old_text\":\"B\",\"new_text\":\"X\"},"
                                                                         "{\"path\":\"multi-diff.txt\",\"old_text\":\"E\",\"new_text\":\"Y\"}]}"});
  auto const multi_diff = multi_diff_patch ? ava::core::json::string_field(multi_diff_patch->result_text, "diff") : std::optional<std::string>{};
  expect(multi_diff_patch && multi_diff_patch->success && multi_diff && multi_diff->find("-B") != std::string::npos &&
             multi_diff->find("+X") != std::string::npos && multi_diff->find(" C\n") != std::string::npos && multi_diff->find("-C") == std::string::npos &&
             multi_diff->find("+C") == std::string::npos && multi_diff->find(" D\n") != std::string::npos && multi_diff->find("-D") == std::string::npos &&
             multi_diff->find("+D") == std::string::npos && multi_diff->find("-E") != std::string::npos && multi_diff->find("+Y") != std::string::npos,
         "apply_patch diff keeps unchanged middle lines as context for separated edits");

  ava::tests::write_test_file(workspace / "queued-a.txt", "a old");
  ava::tests::write_test_file(workspace / "queued-b.txt", "b old");
  auto dispatcher_queue = std::make_shared<ava::tools::MutationQueue>();
  auto const queued_patch_dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build, .mutation_queue = dispatcher_queue});
  if (!ava::tests::require(queued_patch_dispatcher_result.has_value(),
                           queued_patch_dispatcher_result ? "dispatcher fixture composes" : queued_patch_dispatcher_result.error().format()))
    return;
  auto const& queued_patch_dispatcher = *queued_patch_dispatcher_result;
  auto queued_patch = queued_patch_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_queued_patch",
                                   .name = "apply_patch",
                                   .arguments_json = "{\"edits\":[{\"path\":\"queued-b.txt\",\"old_text\":\"old\",\"new_text\":\"new\"},"
                                                     "{\"path\":\"queued-a.txt\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
  auto queued_a_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, workspace / "queued-a.txt");
  auto queued_b_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, workspace / "queued-b.txt");
  expect(queued_patch && queued_patch->success && queued_a_read && queued_b_read && queued_a_read->content == "a new" && queued_b_read->content == "b new" &&
             queued_patch->result_text.find("\"changed_files\"") != std::string::npos,
         "apply_patch can acquire multiple shared mutation-queue locks and commit both paths");

  ava::tests::write_test_file(workspace / "overlap.txt", "abcde");
  auto overlap_patch =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_overlap_patch",
                                                       .name = "apply_patch",
                                                       .arguments_json = "{\"edits\":[{\"path\":\"overlap.txt\",\"old_text\":\"abc\",\"new_text\":\"x\"},"
                                                                         "{\"path\":\"overlap.txt\",\"old_text\":\"cde\",\"new_text\":\"y\"}]}"});
  auto overlap_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, workspace / "overlap.txt");
  expect(overlap_patch && !overlap_patch->success && overlap_read && overlap_read->content == "abcde" &&
             overlap_patch->result_text.find("patch edits overlap") != std::string::npos,
         "apply_patch rejects overlapping same-file edits before writing");

  auto empty_old_patch = dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "call_empty_old_patch", .name = "apply_patch", .arguments_json = "{\"edits\":[{\"path\":\"note.txt\",\"old_text\":\"\",\"new_text\":\"bad\"}]}"});
  expect(empty_old_patch && !empty_old_patch->success && empty_old_patch->result_text.find("old_text must not be empty") != std::string::npos,
         "apply_patch rejects empty old_text before attempting a match");
}

void test_dispatcher_external_permissions(std::filesystem::path const& root, std::filesystem::path const& workspace,
                                          ava::agent::ToolDispatcher const& dispatcher)
{
  auto const outside_path = root / "outside.txt";
  ava::tests::write_test_file(outside_path, "dispatcher outside");
  int dispatcher_prompts = 0;
  ava::agent::ToolDispatcher const resolving_dispatcher(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&dispatcher_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++dispatcher_prompts;
        expect(prompt.tool_name == "read_file", "dispatcher threads provider tool prompt metadata");
        return ava::permissions::PermissionResolution::Allow;
      }});
  auto outside_read = resolving_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "call_outside_read", .name = "read_file", .arguments_json = "{\"path\":\"" + ava::core::json::escape(outside_path.generic_string()) + "\"}"});
  expect(outside_read && outside_read->success && outside_read->result_text.find("dispatcher outside") != std::string::npos && dispatcher_prompts == 1,
         "tool dispatcher threads resolver into file tools");

  auto const outside_patch_path = root / "outside-patch.txt";
  ava::tests::write_test_file(outside_patch_path, "outside old");
  std::vector<ava::permissions::PermissionPrompt> apply_patch_prompts;
  ava::agent::ToolDispatcher const patch_resolving_dispatcher(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&apply_patch_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        apply_patch_prompts.push_back(prompt);
        expect(prompt.tool_name == "apply_patch", "apply_patch resolver receives tool name");
        return ava::permissions::PermissionResolution::Allow;
      }});
  auto outside_patch = patch_resolving_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "call_outside_patch",
      .name = "apply_patch",
      .arguments_json = "{\"edits\":[{\"path\":\"" + ava::core::json::escape(outside_patch_path.generic_string()) +
                        "\",\"old_text\":\"old\",\"new_text\":\"new\"},{\"path\":\"" + ava::core::json::escape(outside_patch_path.generic_string()) +
                        "\",\"old_text\":\"outside\",\"new_text\":\"inside\"}]}"});
  auto outside_patch_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = root, .mode = ava::agent::Mode::Build}, outside_patch_path);
  expect(outside_patch && outside_patch->success && outside_patch_read && outside_patch_read->content == "inside new" && apply_patch_prompts.size() == 2 &&
             apply_patch_prompts[0].operation == ava::permissions::Operation::ReadFile &&
             apply_patch_prompts[1].operation == ava::permissions::Operation::EditFile &&
             apply_patch_prompts[1].diff_preview.find("-outside old") != std::string::npos &&
             apply_patch_prompts[1].diff_preview.find("+inside new") != std::string::npos,
         "apply_patch resolves external read permission before edit permission");

  auto const outside_no_resolver_path = root / "outside-patch-no-resolver.txt";
  ava::tests::write_test_file(outside_no_resolver_path, "keep old");
  auto outside_patch_no_resolver = dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "call_outside_patch_no_resolver",
      .name = "apply_patch",
      .arguments_json =
          "{\"edits\":[{\"path\":\"" + ava::core::json::escape(outside_no_resolver_path.generic_string()) + "\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
  auto outside_no_resolver_read =
      ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = root, .mode = ava::agent::Mode::Build}, outside_no_resolver_path);
  expect(outside_patch_no_resolver && !outside_patch_no_resolver->success && outside_no_resolver_read && outside_no_resolver_read->content == "keep old" &&
             outside_patch_no_resolver->result_text.find("no_resolver") != std::string::npos,
         "apply_patch fails closed without resolver and does not write external targets");

  auto const outside_denied_patch_path = root / "outside-patch-denied.txt";
  ava::tests::write_test_file(outside_denied_patch_path, "keep old");
  int denied_patch_prompts = 0;
  ava::agent::ToolDispatcher const patch_denying_dispatcher(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&denied_patch_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++denied_patch_prompts;
        expect(prompt.operation == ava::permissions::Operation::ReadFile, "apply_patch resolver sees read operation before denied external patch");
        return ava::permissions::PermissionResolution::Deny;
      }});
  auto outside_patch_denied = patch_denying_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_outside_patch_denied",
                                   .name = "apply_patch",
                                   .arguments_json = "{\"edits\":[{\"path\":\"" + ava::core::json::escape(outside_denied_patch_path.generic_string()) +
                                                     "\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
  auto outside_denied_patch_read =
      ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = root, .mode = ava::agent::Mode::Build}, outside_denied_patch_path);
  expect(outside_patch_denied && !outside_patch_denied->success && denied_patch_prompts == 1 && outside_denied_patch_read &&
             outside_denied_patch_read->content == "keep old" && outside_patch_denied->result_text.find("resolution: deny") != std::string::npos,
         "apply_patch resolver read denial prevents all external writes");

  auto const outside_edit_denied_patch_path = root / "outside-patch-edit-denied.txt";
  ava::tests::write_test_file(outside_edit_denied_patch_path, "keep old");
  std::vector<ava::permissions::PermissionPrompt> edit_denied_patch_prompts;
  ava::agent::ToolDispatcher const patch_edit_denying_dispatcher(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver =
          [&edit_denied_patch_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        edit_denied_patch_prompts.push_back(prompt);
        if (prompt.operation == ava::permissions::Operation::ReadFile)
        {
          return ava::permissions::PermissionResolution::Allow;
        }
        return ava::permissions::PermissionResolution::Deny;
      }});
  auto outside_patch_edit_denied = patch_edit_denying_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_outside_patch_edit_denied",
                                   .name = "apply_patch",
                                   .arguments_json = "{\"edits\":[{\"path\":\"" + ava::core::json::escape(outside_edit_denied_patch_path.generic_string()) +
                                                     "\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
  auto outside_edit_denied_patch_read =
      ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = root, .mode = ava::agent::Mode::Build}, outside_edit_denied_patch_path);
  expect(outside_patch_edit_denied && !outside_patch_edit_denied->success && edit_denied_patch_prompts.size() == 2 && outside_edit_denied_patch_read &&
             outside_edit_denied_patch_read->content == "keep old" && outside_patch_edit_denied->result_text.find("resolution: deny") != std::string::npos,
         "apply_patch edit denial prevents all external writes after read-approved diff computation");
  if (edit_denied_patch_prompts.size() >= 2)
  {
    expect(edit_denied_patch_prompts[0].operation == ava::permissions::Operation::ReadFile &&
               edit_denied_patch_prompts[1].operation == ava::permissions::Operation::EditFile &&
               edit_denied_patch_prompts[1].diff_preview.find("-keep old") != std::string::npos &&
               edit_denied_patch_prompts[1].diff_preview.find("+keep new") != std::string::npos,
           "apply_patch edit prompt carries backend-generated diff preview");
  }

  auto const outside_failed_patch_path = root / "outside-patch-failed.txt";
  ava::tests::write_test_file(outside_failed_patch_path, "keep old");
  ava::agent::ToolDispatcher const patch_failing_dispatcher(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "resolver failed"));
      }});
  auto outside_patch_failed = patch_failing_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_outside_patch_failed",
                                   .name = "apply_patch",
                                   .arguments_json = "{\"edits\":[{\"path\":\"" + ava::core::json::escape(outside_failed_patch_path.generic_string()) +
                                                     "\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
  auto outside_failed_patch_read =
      ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = root, .mode = ava::agent::Mode::Build}, outside_failed_patch_path);
  expect(outside_patch_failed && !outside_patch_failed->success && outside_failed_patch_read && outside_failed_patch_read->content == "keep old" &&
             outside_patch_failed->result_text.find("resolver_failed") != std::string::npos,
         "apply_patch resolver failure prevents all external writes");
}

void test_dispatcher_transactional_staging(std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher)
{
  auto const partial_a = workspace / "partial-a.txt";
  auto const partial_b = workspace / "partial-b.txt";
  ava::tests::write_test_file(partial_a, "alpha old");
  ava::tests::write_test_file(partial_b, "beta stays");
  auto partial_patch =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_partial_patch",
                                                       .name = "apply_patch",
                                                       .arguments_json = "{\"edits\":[{\"path\":\"partial-a.txt\",\"old_text\":\"old\",\"new_text\":\"new\"},"
                                                                         "{\"path\":\"partial-b.txt\",\"old_text\":\"missing\",\"new_text\":\"new\"}]}"});
  auto partial_a_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, partial_a);
  auto partial_b_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, partial_b);
  expect(partial_patch && !partial_patch->success && partial_a_read && partial_b_read && partial_a_read->content == "alpha old" &&
             partial_b_read->content == "beta stays",
         "apply_patch validates all edits before writing so failures do not partially write");

  auto const staged_a = workspace / "staged-a.txt";
  ava::tests::write_test_file(staged_a, "stage alpha old");
  long const name_max = pathconf(workspace.c_str(), _PC_NAME_MAX);
  expect(name_max > 64 && name_max < 10000, "patch staging fixture has a bounded usable NAME_MAX");
  if (name_max > 64 && name_max < 10000)
  {
    std::string const long_patch_name(static_cast<std::size_t>(name_max) - 4, 'l');
    auto const staged_long = workspace / (long_patch_name + ".txt");
    // Fixture creation must not consume the temporary-name headroom under test.
    ava::tests::write_test_file(staged_long, "stage beta old");
    {
      auto staged_patch = dispatcher.dispatch(ava::agent::ProviderToolCall{
          .id = "call_staged_patch_failure",
          .name = "apply_patch",
          .arguments_json = "{\"edits\":[{\"path\":\"staged-a.txt\",\"old_text\":\"old\",\"new_text\":\"new\"},"
                            "{\"path\":\"" +
                            ava::core::json::escape(staged_long.filename().generic_string()) + "\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
      auto staged_a_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, staged_a);
      auto staged_long_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, staged_long);
      bool has_leftover_stage_temp = false;
      std::error_code iter_error;
      for (std::filesystem::directory_iterator it(workspace, iter_error), end; !iter_error && it != end; it.increment(iter_error))
      {
        has_leftover_stage_temp = has_leftover_stage_temp || it->path().filename().string().find(".ava-patch-") != std::string::npos;
      }
      expect(staged_patch && !staged_patch->success && staged_a_read && staged_long_read && staged_a_read->content == "stage alpha old" &&
                 staged_long_read->content == "stage beta old" && !has_leftover_stage_temp &&
                 staged_patch->result_text.find("temporary_patch_write") != std::string::npos,
             "apply_patch stages all writes before commit and leaves originals unchanged when staging later files fails");
    }
  }

  auto const secure_staged_a = workspace / "secure-staged-a.txt";
  ava::tests::write_test_file(secure_staged_a, "secure alpha old");
  if (name_max > 64 && name_max < 10000)
  {
    std::string const secure_long_patch_name(static_cast<std::size_t>(name_max) - 4, 's');
    auto const secure_staged_long = workspace / (secure_long_patch_name + ".txt");
    ava::tests::write_test_file(secure_staged_long, "secure beta old");
    auto secure_patch_workspace = ava::tools::SecureWorkspace::open(ava::core::normalized_absolute_path(workspace));
    if (ava::tests::require(secure_patch_workspace.has_value(), "secure staging fixture opens workspace authority"))
    {
      auto const secure_patch_dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{
          .workspace_dir = ava::core::normalized_absolute_path(workspace),
          .mode = ava::agent::Mode::Build,
          .secure_workspace = *secure_patch_workspace,
      });
      if (!ava::tests::require(secure_patch_dispatcher_result.has_value(),
                               secure_patch_dispatcher_result ? "dispatcher fixture composes" : secure_patch_dispatcher_result.error().format()))
        return;
      auto const& secure_patch_dispatcher = *secure_patch_dispatcher_result;
      auto secure_staged_patch = secure_patch_dispatcher.dispatch(ava::agent::ProviderToolCall{
          .id = "call_secure_staged_patch_failure",
          .name = "apply_patch",
          .arguments_json = "{\"edits\":[{\"path\":\"secure-staged-a.txt\",\"old_text\":\"old\",\"new_text\":\"new\"},"
                            "{\"path\":\"" +
                            ava::core::json::escape(secure_staged_long.filename().generic_string()) + "\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
      auto secure_staged_a_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, secure_staged_a);
      auto secure_staged_long_read =
          ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, secure_staged_long);
      bool has_leftover_secure_temp = false;
      std::error_code iter_error;
      for (std::filesystem::directory_iterator it(workspace, iter_error), end; !iter_error && it != end; it.increment(iter_error))
      {
        has_leftover_secure_temp = has_leftover_secure_temp || it->path().filename().string().find(".ava-write-") != std::string::npos;
      }
      expect(secure_staged_patch && !secure_staged_patch->success && secure_staged_a_read && secure_staged_long_read &&
                 secure_staged_a_read->content == "secure alpha old" && secure_staged_long_read->content == "secure beta old" && !has_leftover_secure_temp &&
                 secure_staged_patch->result_text.find("temporary_patch_write") != std::string::npos,
             "descriptor-secure apply_patch cleans earlier staged writes and preserves all originals when a later stage fails");
    }
  }
}

void test_dispatcher_patch_cancellation_and_bounds(std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher)
{
  auto const patch_cancel_path = workspace / "patch-cancel-stage.txt";
  ava::tests::write_test_file(patch_cancel_path, "stage cancel old");
  auto const has_patch_temp = [&workspace] {
    std::error_code iter_error;
    for (std::filesystem::directory_iterator it(workspace, iter_error), end; !iter_error && it != end; it.increment(iter_error))
    {
      if (it->path().filename().string().find(".ava-patch-") != std::string::npos)
        return true;
    }
    return false;
  };
  bool completed_patch_stage_observed = false;
  auto const cancel_after_completed_patch_stage = [&workspace, &completed_patch_stage_observed] {
    std::error_code iter_error;
    for (std::filesystem::directory_iterator it(workspace, iter_error), end; !iter_error && it != end; it.increment(iter_error))
    {
      auto const name = it->path().filename().string();
      if (name.find(".ava-patch-") != std::string::npos && name.find(".ava-write-") == std::string::npos)
      {
        completed_patch_stage_observed = true;
        return true;
      }
    }
    return false;
  };
  ava::agent::ToolDispatcher const patch_cancel_dispatcher(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .cancel_requested = cancel_after_completed_patch_stage,
  });
  auto patch_canceled_after_stage = patch_cancel_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_patch_cancel_after_stage",
                                   .name = "apply_patch",
                                   .arguments_json = "{\"edits\":[{\"path\":\"patch-cancel-stage.txt\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
  auto patch_canceled_read = ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, patch_cancel_path);
  expect(completed_patch_stage_observed && patch_canceled_after_stage && !patch_canceled_after_stage->success &&
             patch_canceled_after_stage->payload.status == ava::agent::ToolResultStatus::Canceled && patch_canceled_read &&
             patch_canceled_read->content == "stage cancel old" && !has_patch_temp(),
         "apply_patch cleanup removes a completed staged file when cancellation arrives before commit");

  auto const secure_patch_cancel_path = workspace / "secure-patch-cancel-stage.txt";
  ava::tests::write_test_file(secure_patch_cancel_path, "secure stage cancel old");
  auto secure_cancel_workspace = ava::tools::SecureWorkspace::open(ava::core::normalized_absolute_path(workspace));
  expect(secure_cancel_workspace.has_value(), "descriptor-secure cancellation test anchors its workspace");
  if (secure_cancel_workspace)
  {
    auto const has_secure_write_temp = [&workspace] {
      std::error_code iter_error;
      for (std::filesystem::directory_iterator it(workspace, iter_error), end; !iter_error && it != end; it.increment(iter_error))
      {
        if (it->path().filename().string().find(".ava-write-") != std::string::npos)
          return true;
      }
      return false;
    };
    bool secure_stage_ready = false;
    bool secure_precommit_cancellation_observed = false;
    auto const cancel_at_secure_precommit_boundary = [&has_secure_write_temp, &secure_stage_ready, &secure_precommit_cancellation_observed] {
      if (!has_secure_write_temp())
        return false;
      if (!secure_stage_ready)
      {
        secure_stage_ready = true;
        return false;
      }
      secure_precommit_cancellation_observed = true;
      return true;
    };
    auto const secure_patch_cancel_dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{
        .workspace_dir = ava::core::normalized_absolute_path(workspace),
        .mode = ava::agent::Mode::Build,
        .cancel_requested = cancel_at_secure_precommit_boundary,
        .secure_workspace = *secure_cancel_workspace,
    });
    if (!ava::tests::require(secure_patch_cancel_dispatcher_result.has_value(),
                             secure_patch_cancel_dispatcher_result ? "dispatcher fixture composes" : secure_patch_cancel_dispatcher_result.error().format()))
      return;
    auto const& secure_patch_cancel_dispatcher = *secure_patch_cancel_dispatcher_result;
    auto secure_patch_canceled_after_stage = secure_patch_cancel_dispatcher.dispatch(ava::agent::ProviderToolCall{
        .id = "call_secure_patch_cancel_after_stage",
        .name = "apply_patch",
        .arguments_json = "{\"edits\":[{\"path\":\"secure-patch-cancel-stage.txt\",\"old_text\":\"secure stage cancel old\",\"new_text\":\"\"}]}"});
    auto secure_patch_canceled_read =
        ava::tools::read_file(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build}, secure_patch_cancel_path);
    expect(secure_stage_ready && secure_precommit_cancellation_observed && secure_patch_canceled_after_stage && !secure_patch_canceled_after_stage->success &&
               secure_patch_canceled_after_stage->payload.status == ava::agent::ToolResultStatus::Canceled && secure_patch_canceled_read &&
               secure_patch_canceled_read->content == "secure stage cancel old" && !has_secure_write_temp(),
           "descriptor-secure apply_patch cancels at the explicit pre-commit boundary and cleans its staged write");
  }

  auto const too_large_patch_path = workspace / "too-large-patch.txt";
  ava::tests::write_test_file(too_large_patch_path, "old" + std::string((10 * 1024 * 1024) + 1, 'x'));
  auto too_large_patch = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_too_large_patch",
                                   .name = "apply_patch",
                                   .arguments_json = "{\"edits\":[{\"path\":\"too-large-patch.txt\",\"old_text\":\"old\",\"new_text\":\"new\"}]}"});
  std::ifstream too_large_patch_read(too_large_patch_path, std::ios::binary);
  std::string too_large_prefix(3, '\0');
  too_large_patch_read.read(too_large_prefix.data(), static_cast<std::streamsize>(too_large_prefix.size()));
  expect(too_large_patch && !too_large_patch->success && too_large_patch->result_text.find("too large") != std::string::npos && too_large_prefix == "old",
         "apply_patch rejects files that exceed its full-read bound before writing");
}

void test_dispatcher_question(std::filesystem::path const& workspace, ava::agent::ToolDispatcher const& dispatcher)
{
  auto unavailable_question = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_question_unavailable", .name = "question", .arguments_json = "{\"question\":\"Which approach?\"}"});
  expect(unavailable_question && !unavailable_question->success && unavailable_question->result_text.find("unavailable") != std::string::npos,
         "question tool fails closed when no backend resolver is supplied");

  int question_prompts = 0;
  auto const question_dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build},
      ava::agent::ToolDispatchServices{
          .question_resolver = [&question_prompts](ava::agent::QuestionPrompt const& prompt) -> ava::core::Result<ava::agent::QuestionAnswer> {
            ++question_prompts;
            expect(prompt.header == "Choose" && prompt.question == "Which approach?", "question resolver receives prompt text");
            expect(prompt.options.size() == 2 && prompt.options[0].value == "safe" && prompt.options[0].label == "Safe",
                   "question resolver receives structured options");
            expect(!prompt.multiple && !prompt.allow_custom, "question resolver receives default selection flags");
            return ava::agent::QuestionAnswer{.selected_options = {"safe"}, .custom_text = ""};
          }});
  if (!ava::tests::require(question_dispatcher_result.has_value(),
                           question_dispatcher_result ? "dispatcher fixture composes" : question_dispatcher_result.error().format()))
    return;
  auto const& question_dispatcher = *question_dispatcher_result;
  auto question =
      question_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_question",
                                                                .name = "question",
                                                                .arguments_json = "{\"header\":\"Choose\",\"question\":\"Which "
                                                                                  "approach?\",\"options\":[{\"value\":\"safe\",\"label\":\"Safe\"},"
                                                                                  "{\"value\":\"fast\",\"label\":\"Fast\"}]}"});
  expect(question && question->success && question_prompts == 1 && question->result_text.find("\"selected_options\":[\"safe\"]") != std::string::npos,
         "question tool calls resolver and serializes selected answer");

  auto const multi_question_dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build},
      ava::agent::ToolDispatchServices{.question_resolver = [](ava::agent::QuestionPrompt const& prompt) -> ava::core::Result<ava::agent::QuestionAnswer> {
        expect(prompt.multiple && prompt.allow_custom, "question resolver receives multi/custom flags");
        expect(prompt.options.size() == 2 && prompt.options[1].value == "Beta", "question resolver accepts string options");
        return ava::agent::QuestionAnswer{.selected_options = {"alpha", "Beta"}, .custom_text = "Use both"};
      }});
  if (!ava::tests::require(multi_question_dispatcher_result.has_value(),
                           multi_question_dispatcher_result ? "dispatcher fixture composes" : multi_question_dispatcher_result.error().format()))
    return;
  auto const& multi_question_dispatcher = *multi_question_dispatcher_result;
  auto multi_question =
      multi_question_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_question_multi",
                                                                      .name = "question",
                                                                      .arguments_json = "{\"question\":\"Pick "
                                                                                        "options\",\"options\":[{\"value\":\"alpha\",\"label\":\"Alpha\"},"
                                                                                        "\"Beta\"],\"multiple\":true,\"custom\":true}"});
  expect(multi_question && multi_question->success && multi_question->result_text.find("\"selected_options\":[\"alpha\",\"Beta\"]") != std::string::npos &&
             multi_question->result_text.find("\"custom_text\":\"Use both\"") != std::string::npos,
         "question tool serializes multi-select and custom resolver answers");

  auto secret_question = question_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_question_secret", .name = "question", .arguments_json = "{\"question\":\"Paste a secret\",\"secret\":true}"});
  expect(secret_question && !secret_question->success && secret_question->result_text.find("trusted local commands") != std::string::npos,
         "question tool rejects model-originated secret prompts");

  auto const too_many_answers_dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build},
      ava::agent::ToolDispatchServices{.question_resolver = [](ava::agent::QuestionPrompt const&) -> ava::core::Result<ava::agent::QuestionAnswer> {
        return ava::agent::QuestionAnswer{.selected_options = std::vector<std::string>(65, "option"), .custom_text = ""};
      }});
  if (!ava::tests::require(too_many_answers_dispatcher_result.has_value(),
                           too_many_answers_dispatcher_result ? "dispatcher fixture composes" : too_many_answers_dispatcher_result.error().format()))
    return;
  auto const& too_many_answers_dispatcher = *too_many_answers_dispatcher_result;
  auto too_many_answers = too_many_answers_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_question_too_many_answers", .name = "question", .arguments_json = "{\"question\":\"Pick\"}"});
  expect(too_many_answers && !too_many_answers->success && too_many_answers->result_text.find("too many selected options") != std::string::npos,
         "question tool rejects resolver answers with too many selected options");

  std::string const oversized_answer_text(9000, 'x');
  auto const oversized_selected_dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build},
      ava::agent::ToolDispatchServices{
          .question_resolver = [&oversized_answer_text](ava::agent::QuestionPrompt const&) -> ava::core::Result<ava::agent::QuestionAnswer> {
            return ava::agent::QuestionAnswer{.selected_options = {oversized_answer_text}, .custom_text = ""};
          }});
  if (!ava::tests::require(oversized_selected_dispatcher_result.has_value(),
                           oversized_selected_dispatcher_result ? "dispatcher fixture composes" : oversized_selected_dispatcher_result.error().format()))
    return;
  auto const& oversized_selected_dispatcher = *oversized_selected_dispatcher_result;
  auto oversized_selected = oversized_selected_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_question_oversized_selected", .name = "question", .arguments_json = "{\"question\":\"Pick\"}"});
  expect(oversized_selected && !oversized_selected->success && oversized_selected->result_text.find("selected option is too long") != std::string::npos,
         "question tool rejects oversized resolver selected option strings");

  auto const oversized_custom_dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build},
      ava::agent::ToolDispatchServices{
          .question_resolver = [&oversized_answer_text](ava::agent::QuestionPrompt const&) -> ava::core::Result<ava::agent::QuestionAnswer> {
            return ava::agent::QuestionAnswer{.selected_options = {}, .custom_text = oversized_answer_text};
          }});
  if (!ava::tests::require(oversized_custom_dispatcher_result.has_value(),
                           oversized_custom_dispatcher_result ? "dispatcher fixture composes" : oversized_custom_dispatcher_result.error().format()))
    return;
  auto const& oversized_custom_dispatcher = *oversized_custom_dispatcher_result;
  auto oversized_custom = oversized_custom_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_question_oversized_custom", .name = "question", .arguments_json = "{\"question\":\"Pick\"}"});
  expect(oversized_custom && !oversized_custom->success && oversized_custom->result_text.find("custom text is too long") != std::string::npos,
         "question tool rejects oversized resolver custom text");

  auto const failing_question_dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Build},
      ava::agent::ToolDispatchServices{.question_resolver = [](ava::agent::QuestionPrompt const&) -> ava::core::Result<ava::agent::QuestionAnswer> {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question UI unavailable"));
      }});
  if (!ava::tests::require(failing_question_dispatcher_result.has_value(),
                           failing_question_dispatcher_result ? "dispatcher fixture composes" : failing_question_dispatcher_result.error().format()))
    return;
  auto const& failing_question_dispatcher = *failing_question_dispatcher_result;
  auto failed_question = failing_question_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_question_failed", .name = "question", .arguments_json = "{\"question\":\"Continue?\"}"});
  expect(failed_question && !failed_question->success && failed_question->result_text.find("question UI unavailable") != std::string::npos,
         "question tool returns resolver errors as backend failures");

  auto nul_question = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_nul_question", .name = "question", .arguments_json = "{\"question\":\"bad\\u0000question\"}"});
  expect(nul_question && !nul_question->success && nul_question->result_text.find("control byte") != std::string::npos,
         "question tool rejects NUL bytes in question text as control bytes");

  auto control_header = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_control_header", .name = "question", .arguments_json = "{\"header\":\"bad\\u001B\",\"question\":\"Ok?\"}"});
  expect(control_header && !control_header->success && control_header->result_text.find("control byte") != std::string::npos,
         "question tool rejects control bytes in header text");

  auto control_option_value =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_control_option_value",
                                                       .name = "question",
                                                       .arguments_json = "{\"question\":\"Pick\",\"options\":[{\"value\":\"bad\\u001F\",\"label\":\"Bad\"}]}"});
  expect(control_option_value && !control_option_value->success && control_option_value->result_text.find("control byte") != std::string::npos,
         "question tool rejects control bytes in option values");

  auto control_option_label =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_control_option_label",
                                                       .name = "question",
                                                       .arguments_json = "{\"question\":\"Pick\",\"options\":[{\"value\":\"bad\",\"label\":\"Bad\\u007F\"}]}"});
  expect(control_option_label && !control_option_label->success && control_option_label->result_text.find("control byte") != std::string::npos,
         "question tool rejects control bytes in option labels");

  auto trailing_comma_question = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_question_trailing_comma", .name = "question", .arguments_json = "{\"question\":\"Pick\",\"options\":[\"A\",]}"});
  expect(trailing_comma_question && !trailing_comma_question->success &&
             trailing_comma_question->result_text.find("options array is malformed") != std::string::npos,
         "question tool rejects trailing commas in options arrays");

  auto malformed_question = question_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "call_question_malformed", .name = "question", .arguments_json = "{\"question\":\"Bad options\",\"options\":\"not-an-array\"}"});
  expect(malformed_question && !malformed_question->success && malformed_question->result_text.find("options must be an array") != std::string::npos,
         "question tool rejects malformed option arguments before resolver dispatch");

  auto unknown = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_unknown", .name = "missing_tool", .arguments_json = "{}"});
  expect(unknown && !unknown->success && unknown->result_text.find("unknown tool") != std::string::npos,
         "tool dispatcher returns structured unknown tool errors");
}

void test_dispatcher_contains_executor_exceptions()
{
  for (bool const standard_exception : {true, false})
  {
    auto dispatcher_result = ava::agent::ToolDispatcher::create_strict(
        ava::tools::ToolContext{},
        ava::agent::ToolDispatchServices{.question_resolver = [standard_exception](auto const&) -> ava::core::Result<ava::agent::QuestionAnswer> {
          if (standard_exception)
            throw std::runtime_error("question resolver failed");
          throw 7;
        }});
    if (!ava::tests::require(dispatcher_result.has_value(), dispatcher_result ? "dispatcher fixture composes" : dispatcher_result.error().format()))
      return;
    auto& dispatcher = *dispatcher_result;
    auto result = dispatcher.dispatch({.id = "call_throw", .name = "question", .arguments_json = R"({"question":"Continue?"})"});
    expect(result && !result->success && result->payload.status == ava::agent::ToolResultStatus::Error &&
               result->result_text.find(standard_exception ? "question resolver failed" : "unknown exception") != std::string::npos,
           "dispatcher converts executor exceptions into structured tool failures");
  }
}

void test_dispatcher_context_metadata_owns_strings()
{
  auto metadata_result = ava::agent::ToolDispatcher::tool_metadata(ava::tools::ToolContext{});
  if (!ava::tests::require(metadata_result.has_value(), metadata_result ? "metadata fixture composes" : metadata_result.error().format()))
    return;
  auto metadata = std::move(*metadata_result);
  static_assert(std::is_same_v<typename decltype(metadata)::value_type, ava::agent::RegisteredToolMetadata>);
  auto const read = std::ranges::find_if(metadata, [](auto const& tool) { return tool.name == "read_file"; });
  expect(read != metadata.end() && read->schema_json.find("read_file") != std::string::npos && !read->description.empty(),
         "context metadata owns its strings after the temporary registry is destroyed");
}

void test_dispatcher_composition_errors_remain_errors()
{
  ava::tools::ToolContext context;
  context.include_plugin_tools = false;
  context.exact_builtin_tool_names = std::vector<std::string>{"missing_builtin"};
  context.session_mcp_config = std::make_shared<ava::mcp::McpConfig const>();
  auto registry = ava::agent::compose_tool_registry(context, {});
  auto dispatcher = ava::agent::ToolDispatcher::create_strict(context);
  auto schemas = ava::agent::ToolDispatcher::compose_tool_schemas_json(context);
  auto metadata = ava::agent::ToolDispatcher::tool_metadata(context);
  auto const reports_missing_builtin = [](auto const& result) {
    return !result && result.error().category() == ava::core::ErrorCategory::InvalidArgument &&
           result.error().message() == "requested built-in tool is unavailable" && result.error().format().find("missing_builtin") != std::string::npos;
  };
  expect(reports_missing_builtin(registry) && reports_missing_builtin(dispatcher) && reports_missing_builtin(schemas) && reports_missing_builtin(metadata),
         "composition, dispatcher, schema and metadata APIs preserve unavailable-tool errors instead of returning empty registries");

  context.exact_builtin_tool_names = std::vector<std::string>{};
  auto empty_dispatcher = ava::agent::ToolDispatcher::create_strict(context);
  auto empty_schemas = ava::agent::ToolDispatcher::compose_tool_schemas_json(context);
  auto empty_metadata = ava::agent::ToolDispatcher::tool_metadata(context);
  expect(empty_dispatcher && empty_dispatcher->registered_tool_metadata().empty() && empty_schemas && empty_schemas->empty() && empty_metadata &&
             empty_metadata->empty(),
         "an intentionally empty composition remains a successful result");

  context.session_mcp_config.reset();
  auto missing_config = ava::agent::ToolDispatcher::compose_tool_schemas_json(context);
  expect(!missing_config && missing_config.error().message() == "exact tool composition requires an immutable session MCP configuration",
         "schema composition preserves missing immutable-session configuration errors");
}

void test_dispatcher_schemas_and_registry()
{
  auto const default_schemas = ava::agent::ToolDispatcher::tool_schemas_json();
  ava::tools::ToolContext lsp_schema_context;
  lsp_schema_context.lsp_diagnostics_provider = std::make_shared<EmptyDiagnosticsProvider>();
  auto const configured_schemas = ava::agent::ToolDispatcher::tool_schemas_json(lsp_schema_context);
  auto const& schemas = configured_schemas;
  auto const metadata = ava::agent::ToolDispatcher::tool_metadata();
  auto const job_schema = std::ranges::find_if(default_schemas, [](std::string const& schema) { return schema.find(R"("name":"job")") != std::string::npos; });
  expect(job_schema != default_schemas.end() && job_schema->find(R"("enum":["list","status","wait","result","cancel","steer"])") != std::string::npos &&
             job_schema->find("promote") == std::string::npos,
         "model-visible job schema exposes steering while omitting backend-only promotion");
  auto const& registry = ava::agent::builtin_tool_registry();
  auto const* registered_read_tool = registry.find("read_file");
  expect(registry.entries().size() == metadata.size(), "built-in tool registry covers all static tool metadata");
  expect(registered_read_tool != nullptr && registered_read_tool->source == ava::agent::ToolSource::Builtin,
         "built-in tool registry can resolve read_file with source identity");
  expect(registry.find("missing_tool") == nullptr, "built-in tool registry reports missing tools explicitly");
  ava::agent::ToolRegistry scratch_registry;
  if (!ava::tests::require(registered_read_tool != nullptr, "registry fixture resolves read_file"))
    return;
  if (registered_read_tool != nullptr)
  {
    auto registered_read = scratch_registry.register_tool(*registered_read_tool);
    expect(registered_read.has_value(), "tool registry accepts a valid tool registration");
    auto duplicate_read = scratch_registry.register_tool(*registered_read_tool);
    expect(!duplicate_read && duplicate_read.error().message().find("duplicate") != std::string::npos, "tool registry rejects duplicate tool names");
    auto mismatched_schema = *registered_read_tool;
    mismatched_schema.metadata.name = "different_name";
    auto mismatched = ava::agent::ToolRegistry{}.register_tool(std::move(mismatched_schema));
    expect(!mismatched && mismatched.error().message().find("schema name") != std::string::npos,
           "tool registry rejects schema names that do not match metadata names");
    auto plugin_direct = *registered_read_tool;
    plugin_direct.metadata.name = "plugin_read_file";
    plugin_direct.metadata.schema_json = R"({"type":"function","name":"plugin_read_file","description":"Plugin read","parameters":{"type":"object"}})";
    plugin_direct.source = ava::agent::ToolSource::Plugin;
    plugin_direct.source_id = "com.example.plugin";
    auto plugin_brokered = plugin_direct;
    plugin_brokered.brokered_external = true;
    auto plugin_direct_result = ava::agent::ToolRegistry{}.register_tool(std::move(plugin_direct));
    expect(!plugin_direct_result && plugin_direct_result.error().message().find("broker") != std::string::npos,
           "tool registry rejects direct external executors without broker marking");
    auto plugin_brokered_result = ava::agent::ToolRegistry{}.register_tool(std::move(plugin_brokered));
    expect(plugin_brokered_result.has_value(), "tool registry accepts AVA-owned brokered external tools");
  }
  bool has_apply_patch = false;
  bool has_question = false;
  bool has_webfetch = false;
  bool has_websearch = false;
  bool has_skill = false;
  bool has_list_directory = false;
  bool has_lsp_diagnostics = false;
  bool lsp_schema_exposes_command = false;
  bool read_has_offset = false;
  bool glob_has_no_ignore = false;
  bool grep_has_no_ignore = false;
  bool grep_has_literal = false;
  bool grep_has_case_insensitive = false;
  bool question_has_allow_multiple = false;
  bool task_rejects_unknown_properties = false;
  auto const default_has_lsp_schema =
      std::ranges::any_of(default_schemas, [](std::string const& schema) { return schema.find("\"name\":\"lsp_") != std::string::npos; });
  auto const configured_lsp_schema_count =
      std::ranges::count_if(configured_schemas, [](std::string const& schema) { return schema.find("\"name\":\"lsp_") != std::string::npos; });
  expect(!default_has_lsp_schema && configured_lsp_schema_count == 5 && default_schemas.size() + configured_lsp_schema_count == configured_schemas.size(),
         "LSP schemas are gated until a local diagnostics provider is configured");
  expect(metadata.size() == schemas.size(), "tool metadata and configured schema exports cover the same built-in tools");
  for (std::size_t index = 0; index < metadata.size(); ++index)
  {
    auto const& tool = metadata[index];
    expect(!tool.name.empty() && !tool.description.empty() && !tool.schema_json.empty() && !tool.permission_category.empty() &&
               !tool.output_bound_summary.empty() && !tool.execution_mode.empty() && !tool.event_rendering_hint.empty() && tool.description_family.has_value(),
           "built-in tool metadata includes required generic fields");
    expect(index < schemas.size() && schemas[index] == tool.schema_json, "tool schema export is derived from built-in metadata registry");
    auto const schema = std::string(tool.schema_json);
    has_apply_patch = has_apply_patch || schema.find("apply_patch") != std::string::npos;
    has_list_directory = has_list_directory || schema.find("\"name\":\"list_directory\"") != std::string::npos;
    has_webfetch = has_webfetch || schema.find("webfetch") != std::string::npos;
    has_websearch = has_websearch || schema.find("\"name\":\"websearch\"") != std::string::npos;
    has_skill = has_skill || schema.find("\"name\":\"skill\"") != std::string::npos;
    read_has_offset = read_has_offset || (schema.find("\"name\":\"read_file\"") != std::string::npos && schema.find("\"offset\"") != std::string::npos &&
                                          schema.find("\"limit\"") != std::string::npos);
    bool const is_lsp_schema = schema.find("\"name\":\"lsp_") != std::string::npos;
    has_lsp_diagnostics = has_lsp_diagnostics || schema.find("\"name\":\"lsp_diagnostics\"") != std::string::npos;
    lsp_schema_exposes_command =
        lsp_schema_exposes_command || (is_lsp_schema && (schema.find("command") != std::string::npos || schema.find("argv") != std::string::npos));
    glob_has_no_ignore = glob_has_no_ignore || (schema.find("\"name\":\"glob\"") != std::string::npos && schema.find("no_ignore") != std::string::npos);
    grep_has_no_ignore = grep_has_no_ignore || (schema.find("\"name\":\"grep\"") != std::string::npos && schema.find("no_ignore") != std::string::npos);
    grep_has_literal = grep_has_literal || (schema.find("\"name\":\"grep\"") != std::string::npos && schema.find("\"literal\"") != std::string::npos);
    grep_has_case_insensitive =
        grep_has_case_insensitive || (schema.find("\"name\":\"grep\"") != std::string::npos && schema.find("\"case_insensitive\"") != std::string::npos);
    bool const is_question_schema = schema.find("\"name\":\"question\"") != std::string::npos;
    has_question = has_question || is_question_schema;
    question_has_allow_multiple = question_has_allow_multiple || (is_question_schema && schema.find("allow_multiple") != std::string::npos);
    task_rejects_unknown_properties =
        task_rejects_unknown_properties || (tool.name == "task" && schema.find("\"additionalProperties\":false") != std::string::npos);
  }
  expect(!schemas.empty() && schemas[0].find("read_file") != std::string::npos && has_apply_patch && has_question && has_webfetch && has_websearch &&
             has_skill && has_list_directory && has_lsp_diagnostics && read_has_offset && grep_has_literal && grep_has_case_insensitive,
         "tool dispatcher exposes provider tool schemas");
  expect(!lsp_schema_exposes_command, "lsp_diagnostics schema keeps server command out of provider control");
  expect(!glob_has_no_ignore && !grep_has_no_ignore, "search tool schemas keep no_ignore out of provider control");
  expect(question_has_allow_multiple, "question tool schema exposes the allow_multiple alias");
  expect(task_rejects_unknown_properties, "task schema sets additionalProperties false");
}

void test_task_persistent_deny_preflight_blocks_runner()
{
  auto const root = create_empty_root("dispatcher-task-persistent-deny");
  auto const workspace = root / "workspace";
  auto const config_dir = root / "config" / "ava";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(config_dir);
  expect(::chmod(temp_root().c_str(), S_IRWXU) == 0 && ::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0 &&
             ::chmod(config_dir.c_str(), S_IRWXU) == 0,
         "task persistent-deny fixture keeps rule storage and workspace owner-only");

  auto anchors = ava::core::AnchorSet::open({workspace, config_dir});
  expect(anchors.has_value(), "task persistent-deny fixture opens trusted rule-storage anchors");
  if (!anchors)
    return;
  auto const rule_store = ava::permissions::PermissionRuleStore{.global_rules_file = config_dir / "permission-rules.json",
                                                                .workspace_rules_file = workspace / ".ava" / "permission-rules.json",
                                                                .workspace_dir = workspace,
                                                                .anchor_set = *anchors};
  auto denied_rule = ava::permissions::add_persistent_permission_rule(
      rule_store, ava::permissions::PermissionRuleDraft{.scope = ava::permissions::PermissionRuleScope::Workspace,
                                                        .action = ava::permissions::PermissionAction::Deny,
                                                        .operation = ava::permissions::Operation::TaskRun,
                                                        .mode = ava::permissions::PermissionRuleMode::Build,
                                                        .tool_name = "task",
                                                        .target_path = workspace,
                                                        .command = "general",
                                                        .command_recipe_key = {},
                                                        .recipe_display = {},
                                                        .critical_acknowledged = false,
                                                        .reason = "test task deny",
                                                        .actor = "test"});
  expect(denied_rule.has_value(), "task persistent-deny fixture writes an exact TaskRun deny rule");
  if (!denied_rule)
    return;

  int resolver_prompts = 0;
  int runner_invocations = 0;
  std::vector<ava::tools::PermissionAuditEvent> audits;
  auto dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace,
                              .permission_resolver = [&resolver_prompts](ava::permissions::PermissionPrompt const&)
                                  -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
                                ++resolver_prompts;
                                return ava::permissions::PermissionResolution::Allow;
                              },
                              .auto_allow_deny_preflight = ava::permissions::build_persistent_permission_deny_preflight(rule_store),
                              .permission_audit_sink = [&audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
                                audits.push_back(event);
                                return {};
                              }},
      ava::agent::ToolDispatchServices{
          .task_subagent_runner = [&runner_invocations](ava::agent::TaskSubagentRequest const&) -> ava::core::Result<ava::agent::TaskSubagentResult> {
            ++runner_invocations;
            return ava::agent::TaskSubagentResult{};
          }});
  if (!ava::tests::require(dispatcher_result.has_value(), dispatcher_result ? "dispatcher fixture composes" : dispatcher_result.error().format()))
    return;
  auto& dispatcher = *dispatcher_result;

  auto result = dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "task_persistent_deny", .name = "task", .arguments_json = R"({"description":"blocked","prompt":"do not run","subagent_type":"general"})"});
  expect(result && !result->success && runner_invocations == 0 && resolver_prompts == 0 && audits.size() == 1 &&
             audits.front().operation == ava::permissions::Operation::TaskRun && audits.front().target_path == workspace &&
             audits.front().command == "general" && audits.front().resolution == "deny" && audits.front().resolution_source == "persistent_rule" &&
             audits.front().rule_id == denied_rule->rule_id && audits.front().resolution_reason == "test task deny",
         "an exact persistent TaskRun deny blocks prompt-free launch before runner invocation and retains rule audit evidence");
}

void test_task_mode_and_job_tool_controls()
{
  auto const root = create_empty_root("dispatcher-job-controls");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  bool captured_background = false;
  std::optional<std::size_t> captured_max_tool_iterations;
  std::size_t task_runs = 0;
  ava::tools::ToolContext task_context{.workspace_dir = workspace,
                                       .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
                                         return ava::permissions::PermissionResolution::Allow;
                                       }};
  ava::agent::ToolDispatchServices task_services{
      .task_subagent_runner =
          [&](ava::agent::TaskSubagentRequest const& request) {
            ++task_runs;
            captured_background = request.background;
            captured_max_tool_iterations = request.max_tool_iterations;
            return ava::agent::TaskSubagentResult{.task_id = "task_mode",
                                                  .job_id = {},
                                                  .session_path = {},
                                                  .subagent_type = request.subagent_type,
                                                  .state = "completed",
                                                  .final_text = {},
                                                  .stop_reason = {},
                                                  .provider_iterations = 0,
                                                  .tool_calls = 0,
                                                  .tool_iterations = 0};
          },
      .subagents = {ava::agent::AgentDefinition{.name = "general", .description = "general", .system_prompt = "general", .max_tool_iterations = 7}}};
  auto task_dispatcher_result = ava::agent::ToolDispatcher::create_strict(task_context, task_services);
  if (!ava::tests::require(task_dispatcher_result.has_value(),
                           task_dispatcher_result ? "dispatcher fixture composes" : task_dispatcher_result.error().format()))
    return;
  auto& task_dispatcher = *task_dispatcher_result;
  auto preferred = task_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "task_mode", .name = "task", .arguments_json = R"({"description":"mode","prompt":"run","subagent_type":"general","mode":"background"})"});
  expect(preferred && preferred->success && captured_background, "task prefers explicit background mode while preserving the runner contract");
  auto legacy = task_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "task_legacy", .name = "task", .arguments_json = R"({"description":"legacy","prompt":"run","subagent_type":"general","background":false})"});
  expect(legacy && legacy->success && !captured_background, "task preserves the legacy background boolean");
  auto conflict = task_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "task_conflict",
      .name = "task",
      .arguments_json = R"({"description":"conflict","prompt":"run","subagent_type":"general","mode":"foreground","background":true})"});
  expect(conflict && !conflict->success && conflict->result_text.find("conflicts") != std::string::npos && task_runs == 2,
         "task rejects conflicting preferred and legacy mode fields before dispatch");
  auto inherited_limit = task_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "task_limit_default", .name = "task", .arguments_json = R"({"description":"limit","prompt":"run","subagent_type":"general"})"});
  bool const inherited_definition_limit = inherited_limit && inherited_limit->success && captured_max_tool_iterations == 7;
  auto overridden_limit = task_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "task_limit_override",
                                   .name = "task",
                                   .arguments_json = R"({"description":"limit","prompt":"run","subagent_type":"general","max_tool_iterations":12})"});
  bool const explicit_override = overridden_limit && overridden_limit->success && captured_max_tool_iterations == 12;
  std::array<std::string, 7> const invalid_limits{
      R"({"description":"limit","prompt":"run","subagent_type":"general","max_tool_iterations":true})",
      R"({"description":"limit","prompt":"run","subagent_type":"general","max_tool_iterations":1.5})",
      R"({"description":"limit","prompt":"run","subagent_type":"general","max_tool_iterations":0})",
      R"({"description":"limit","prompt":"run","subagent_type":"general","max_tool_iterations":-1})",
      R"({"description":"limit","prompt":"run","subagent_type":"general","max_tool_iterations":1001})",
      R"({"description":"limit","prompt":"run","subagent_type":"general","max_tool_iterations":18446744073709551616})",
      R"({"description":"limit","prompt":"run","subagent_type":"general","max_tool_iterations":2,"max_tool_iterations":3})"};
  for (std::size_t index = 0; index < invalid_limits.size(); ++index)
  {
    auto invalid = task_dispatcher.dispatch(
        ava::agent::ProviderToolCall{.id = "task_limit_invalid_" + std::to_string(index), .name = "task", .arguments_json = invalid_limits[index]});
    ava::tests::expect_all("task max_tool_iterations rejects " + invalid_limits[index],
                           {{"failed tool result", invalid && !invalid->success}, {"runner not invoked", task_runs == 4}});
  }
  ava::tests::expect_all("task max_tool_iterations inherits the definition and accepts an invocation override",
                         {{"inherited limit", inherited_definition_limit}, {"explicit override", explicit_override}, {"runner calls", task_runs == 4}});

  std::size_t unknown_runs = 0;
  std::size_t unknown_permissions = 0;
  std::size_t unknown_launches = 0;
  ava::tools::ToolContext unknown_context{
      .workspace_dir = workspace,
      .permission_resolver = [&unknown_permissions](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++unknown_permissions;
        return ava::permissions::PermissionResolution::Allow;
      }};
  ava::agent::ToolDispatchServices unknown_services{
      .task_subagent_runner = [&unknown_runs](ava::agent::TaskSubagentRequest const&) -> ava::core::Result<ava::agent::TaskSubagentResult> {
        ++unknown_runs;
        return ava::agent::TaskSubagentResult{};
      },
      .subagent_launch = {.sink = [&unknown_launches](ava::agent::SubagentLaunchNotification const&) { ++unknown_launches; }},
      .subagents = {ava::agent::AgentDefinition{.name = "general", .description = "general", .system_prompt = "general"}}};
  auto unknown_dispatcher_result = ava::agent::ToolDispatcher::create_strict(unknown_context, unknown_services);
  if (!ava::tests::require(unknown_dispatcher_result.has_value(),
                           unknown_dispatcher_result ? "dispatcher fixture composes" : unknown_dispatcher_result.error().format()))
    return;
  auto& unknown_dispatcher = *unknown_dispatcher_result;
  auto typo_iterations = unknown_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "task_typo_iterations",
                                   .name = "task",
                                   .arguments_json = R"({"description":"limit","prompt":"run","subagent_type":"general","max_tool_iteration":12})"});
  auto typo_task_id = unknown_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "task_typo_id", .name = "task", .arguments_json = R"({"description":"limit","prompt":"run","subagent_type":"general","task_idd":"child"})"});
  expect(typo_iterations && !typo_iterations->success && typo_iterations->result_text.find("unknown field") != std::string::npos && typo_task_id &&
             !typo_task_id->success && typo_task_id->result_text.find("unknown field") != std::string::npos && unknown_runs == 0 && unknown_permissions == 0 &&
             unknown_launches == 0,
         "task rejects unknown argument names such as max_tool_iteration and task_idd before permission, launch, or runner invocation");

  std::filesystem::permissions(root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(), "job dispatcher fixture creates coordinator");
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  struct WorkerState
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool release = false;
    ava::agent::BackgroundJobCompletion run(ava::agent::BackgroundJobContext const& context)
    {
      std::stop_callback wake(context.stop_token, [&] { changed.notify_all(); });
      std::unique_lock lock(mutex);
      started = true;
      changed.notify_all();
      changed.wait(lock, [&] { return release || context.stop_token.stop_requested(); });
      if (context.stop_token.stop_requested())
        return {.state = ava::agent::BackgroundJobState::Canceled,
                .final_text = {},
                .stop_reason = "canceled",
                .error = std::nullopt,
                .provider_iterations = 0,
                .tool_calls = 0,
                .tool_iterations = 0};
      return {.state = ava::agent::BackgroundJobState::Completed,
              .final_text = "safe terminal summary",
              .stop_reason = "completed",
              .error = std::nullopt,
              .provider_iterations = 0,
              .tool_calls = 0,
              .tool_iterations = 0};
    }
  };
  auto state = std::make_shared<WorkerState>();
  auto steering_queue = ava::agent::SubagentSteeringQueue::create();
  auto started = coordinator->start(ava::agent::SubagentCoordinatorStartRequest{.parent_session_id = "owner",
                                                                                .mode = ava::agent::SubagentJobMode::Background,
                                                                                .job = {.child_session_id = "child_job_tool"},
                                                                                .launch_display = ava::agent::SubagentLaunchDisplay::normalized(
                                                                                    "MODEL_JOB_SENTINEL", std::string_view("REASONING_JOB_SENTINEL")),
                                                                                .steering_queue = steering_queue},
                                    [state](auto const& context) { return state->run(context); });
  expect(started.has_value(), "job dispatcher fixture starts owned job");
  if (!started)
    return;
  {
    std::unique_lock lock(state->mutex);
    expect(state->changed.wait_until(lock, ava::tests::now_plus_seconds(3), [&] { return state->started; }), "job dispatcher worker reaches running state");
  }
  auto const job_id = started->job.identity.job_id;
  auto job_dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{.workspace_dir = workspace, .session_id = "owner"},
                                                                         ava::agent::ToolDispatchServices{.subagent_coordinator = coordinator});
  if (!ava::tests::require(job_dispatcher_result.has_value(), job_dispatcher_result ? "dispatcher fixture composes" : job_dispatcher_result.error().format()))
    return;
  auto& job_dispatcher = *job_dispatcher_result;
  auto list = job_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "job_list", .name = "job", .arguments_json = R"({"action":"list"})"});
  auto status = job_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "job_status", .name = "job", .arguments_json = "{\"action\":\"status\",\"job_id\":\"" + job_id + "\"}"});
  auto timed = job_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "job_wait", .name = "job", .arguments_json = "{\"action\":\"wait\",\"job_id\":\"" + job_id + "\",\"timeout_ms\":1}"});
  auto not_ready = job_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "job_result", .name = "job", .arguments_json = "{\"action\":\"result\",\"job_id\":\"" + job_id + "\"}"});
  auto steered = job_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "job_steer", .name = "job", .arguments_json = "{\"action\":\"steer\",\"job_id\":\"" + job_id + "\",\"message\":\"inspect next\"}"});
  auto steering_messages = steering_queue->take();
  auto duplicate =
      job_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "job_duplicate", .name = "job", .arguments_json = R"({"action":"list","action":"status"})"});
  expect(list && list->success && list->result_text.find(job_id) != std::string::npos && list->result_text.find("safe terminal summary") == std::string::npos &&
             status && status->success && status->result_text.find("\"state\":\"running\"") != std::string::npos && timed && timed->success &&
             timed->result_text.find("\"timed_out\":true") != std::string::npos && not_ready && !not_ready->success &&
             not_ready->result_text.find("\"code\":\"job_not_ready\"") != std::string::npos && steered && steered->success && steering_messages &&
             *steering_messages == std::vector<std::string>{"inspect next"} && duplicate && !duplicate->success &&
             list->result_text.find("MODEL_JOB_SENTINEL") == std::string::npos && status->result_text.find("REASONING_JOB_SENTINEL") == std::string::npos &&
             timed->result_text.find("MODEL_JOB_SENTINEL") == std::string::npos && not_ready->result_text.find("REASONING_JOB_SENTINEL") == std::string::npos,
         "job tool shares bounded snapshots, strict parsing, timeout snapshots, and stable not-ready status");

  auto other_owner_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{.workspace_dir = workspace, .session_id = "other"},
                                                                      ava::agent::ToolDispatchServices{.subagent_coordinator = coordinator});
  if (!ava::tests::require(other_owner_result.has_value(), other_owner_result ? "dispatcher fixture composes" : other_owner_result.error().format()))
    return;
  auto& other_owner = *other_owner_result;
  auto hidden = other_owner.dispatch(
      ava::agent::ProviderToolCall{.id = "job_hidden", .name = "job", .arguments_json = "{\"action\":\"status\",\"job_id\":\"" + job_id + "\"}"});
  expect(hidden && !hidden->success && hidden->result_text.find("not_found") != std::string::npos, "job tool maps owner mismatch to NotFound");

  {
    std::lock_guard lock(state->mutex);
    state->release = true;
    state->changed.notify_all();
  }
  auto completed = coordinator->wait("owner", job_id, std::chrono::seconds(1));
  auto terminal_result = job_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "job_terminal", .name = "job", .arguments_json = "{\"action\":\"result\",\"job_id\":\"" + job_id + "\"}"});
  expect(completed && terminal_result && terminal_result->success && terminal_result->result_text.find("safe terminal summary") != std::string::npos &&
             terminal_result->result_text.find("MODEL_JOB_SENTINEL") == std::string::npos &&
             terminal_result->result_text.find("REASONING_JOB_SENTINEL") == std::string::npos,
         "job result includes a completed bounded summary only after terminal completion");

  auto promoted_state = std::make_shared<WorkerState>();
  auto foreground = coordinator->start("owner", ava::agent::SubagentJobMode::Foreground, {.child_session_id = "child_job_promote"},
                                       [promoted_state](auto const& context) { return promoted_state->run(context); });
  expect(foreground.has_value(), "job promotion fixture publishes its foreground job");
  if (foreground)
  {
    {
      std::unique_lock lock(promoted_state->mutex);
      expect(promoted_state->changed.wait_until(lock, ava::tests::now_plus_seconds(3), [&] { return promoted_state->started; }),
             "job promotion fixture reaches running state");
    }
    auto promoted = job_dispatcher.dispatch(ava::agent::ProviderToolCall{
        .id = "job_promote", .name = "job", .arguments_json = "{\"action\":\"promote\",\"job_id\":\"" + foreground->job.identity.job_id + "\"}"});
    expect(promoted && !promoted->success && promoted->result_text.find("unsupported") != std::string::npos,
           "model-visible job tool rejects unreachable promotion while backend promotion remains out-of-band");
    {
      std::lock_guard lock(promoted_state->mutex);
      promoted_state->release = true;
      promoted_state->changed.notify_all();
    }
    static_cast<void>(coordinator->wait("owner", foreground->job.identity.job_id, std::chrono::seconds(1)));
  }

  auto canceled_state = std::make_shared<WorkerState>();
  auto cancel_started = coordinator->start_background("owner", {.child_session_id = "child_job_cancel"},
                                                      [canceled_state](auto const& context) { return canceled_state->run(context); });
  expect(cancel_started.has_value(), "job cancellation fixture publishes its background job");
  if (cancel_started)
  {
    {
      std::unique_lock lock(canceled_state->mutex);
      expect(canceled_state->changed.wait_until(lock, ava::tests::now_plus_seconds(3), [&] { return canceled_state->started; }),
             "job cancellation fixture reaches running state");
    }
    auto canceled = job_dispatcher.dispatch(ava::agent::ProviderToolCall{
        .id = "job_cancel", .name = "job", .arguments_json = "{\"action\":\"cancel\",\"job_id\":\"" + cancel_started->job.identity.job_id + "\"}"});
    auto canceled_terminal = coordinator->wait("owner", cancel_started->job.identity.job_id, std::chrono::seconds(1));
    expect(canceled && canceled->success && canceled_terminal && canceled_terminal->job.execution == ava::agent::SubagentExecutionState::Canceled,
           "job tool durably requests cancellation for one owned job");
  }
}

void exercise_job_wait_parent_cancellation_keeps_background_running(std::filesystem::path const& workspace, bool abandon_at_checkpoint = false)
{
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(), "parent-cancel wait fixture creates coordinator");
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  struct WorkerState
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    bool release = false;
    bool cancellation_checkpoint = false;
    bool release_cancellation = false;
    bool checkpoint_timed_out = false;
    bool dispatch_finished = false;
  };
  auto state = std::make_shared<WorkerState>();
  auto started = coordinator->start_background("wait-owner", {.child_session_id = "independent-child"}, [state](auto const& context) {
    std::stop_callback wake(context.stop_token, [&] { state->changed.notify_all(); });
    std::unique_lock lock(state->mutex);
    state->started = true;
    state->changed.notify_all();
    state->changed.wait(lock, [&] { return state->release || context.stop_token.stop_requested(); });
    return ava::agent::BackgroundJobCompletion{
        .state = context.stop_token.stop_requested() ? ava::agent::BackgroundJobState::Canceled : ava::agent::BackgroundJobState::Completed,
        .final_text = "independent child finished",
        .stop_reason = context.stop_token.stop_requested() ? "canceled" : "completed"};
  });
  expect(started.has_value(), "parent-cancel wait fixture starts independent background job");
  if (!started)
    return;
  {
    std::unique_lock lock(state->mutex);
    expect(state->changed.wait_until(lock, ava::tests::now_plus_seconds(3), [&] { return state->started; }), "independent background worker starts");
  }
  auto const job_id = started->job.identity.job_id;
  auto deadline_dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{.workspace_dir = workspace, .session_id = "wait-owner"},
                                                                              ava::agent::ToolDispatchServices{.subagent_coordinator = coordinator});
  if (!ava::tests::require(deadline_dispatcher_result.has_value(),
                           deadline_dispatcher_result ? "dispatcher fixture composes" : deadline_dispatcher_result.error().format()))
    return;
  auto& deadline_dispatcher = *deadline_dispatcher_result;
  auto timed_out = deadline_dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "sliced_deadline_wait", .name = "job", .arguments_json = "{\"action\":\"wait\",\"job_id\":\"" + job_id + "\",\"timeout_ms\":125}"});
  expect(timed_out && timed_out->success && timed_out->result_text.find("\"timed_out\":true") != std::string::npos,
         "job wait reports the overall deadline expiring while its background worker remains blocked");
  std::atomic_bool cancel_requested = false;
  auto const deadline = ava::tests::now_plus_seconds(5);
  auto dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace,
                              .cancel_requested =
                                  [&] {
                                    std::unique_lock lock(state->mutex);
                                    state->cancellation_checkpoint = true;
                                    state->changed.notify_all();
                                    if (!state->changed.wait_until(lock, deadline, [&] { return state->release_cancellation; }))
                                      state->checkpoint_timed_out = true;
                                    return cancel_requested.load();
                                  },
                              .session_id = "wait-owner"},
      ava::agent::ToolDispatchServices{.subagent_coordinator = coordinator});
  if (!ava::tests::require(dispatcher_result.has_value(), dispatcher_result ? "dispatcher fixture composes" : dispatcher_result.error().format()))
    return;
  auto& dispatcher = *dispatcher_result;
  auto waiting = std::async(std::launch::async, [&] {
    auto result = dispatcher.dispatch(ava::agent::ProviderToolCall{
        .id = "parent_long_wait", .name = "job", .arguments_json = "{\"action\":\"wait\",\"job_id\":\"" + job_id + "\",\"timeout_ms\":30000}"});
    std::lock_guard lock(state->mutex);
    state->dispatch_finished = true;
    state->changed.notify_all();
    return result;
  });
  auto release = [&](WorkerState*) {
    cancel_requested = true;
    std::lock_guard lock(state->mutex);
    state->release_cancellation = true;
    state->release = true;
    state->changed.notify_all();
  };
  // Declared after the future so every exit releases both gates before joining.
  std::unique_ptr<WorkerState, decltype(release)> cleanup(state.get(), release);
  {
    std::unique_lock lock(state->mutex);
    auto const signaled = state->changed.wait_until(lock, deadline, [&] { return state->cancellation_checkpoint || state->dispatch_finished; });
    ava::tests::expect_all("job dispatch reaches a cancellation checkpoint rather than returning early",
                           {{"checkpoint signaled", signaled},
                            {"checkpoint entered", state->cancellation_checkpoint},
                            {"checkpoint did not time out", !state->checkpoint_timed_out}});
    if (abandon_at_checkpoint)
      return;
    cancel_requested = true;
    state->release_cancellation = true;
    state->changed.notify_all();
  }
  auto const ready = waiting.wait_until(deadline) == std::future_status::ready;
  expect(ready, "parent cancellation releases the job dispatch before its watchdog");
  if (!ready)
    cleanup.reset();
  auto canceled = waiting.get();
  auto still_running = coordinator->snapshot("wait-owner", job_id);
  cleanup.reset();
  auto completed = coordinator->wait("wait-owner", job_id, std::chrono::seconds(3));
  ava::tests::expect_all("parent cancellation at a dispatch checkpoint returns typed cancellation",
                         {{"failed tool result", canceled && !canceled->success},
                          {"typed cancellation", canceled && canceled->payload.status == ava::agent::ToolResultStatus::Canceled}});
  ava::tests::expect_all("canceling the parent dispatch leaves its independent background job able to complete",
                         {{"child remained running", still_running && still_running->job.execution == ava::agent::SubagentExecutionState::Running},
                          {"child not canceled", still_running && !still_running->job.cancel_requested},
                          {"child completed", completed && completed->job.execution == ava::agent::SubagentExecutionState::Completed}});
}

void test_job_wait_parent_cancellation_keeps_background_running()
{
  auto const workspace = create_empty_root("dispatcher-job-wait-parent-cancel");
  auto const child = ::fork();
  if (!ava::tests::require(child >= 0, "job-wait cancellation watchdog forks"))
    return;
  if (child == 0)
  {
    auto const failures_before = ava::tests::failures();
    exercise_job_wait_parent_cancellation_keeps_background_running(workspace);
    exercise_job_wait_parent_cancellation_keeps_background_running(workspace, true);
    ::_exit(ava::tests::failures() == failures_before ? 0 : 1);
  }
  ava::tests::ScopedChild child_owner(child);
  auto const status = child_owner.wait_until(ava::tests::now_plus_seconds(15));
  ava::tests::expect_all("job-wait cancellation and failure-path teardown finish under the outer watchdog",
                         {{"child completed", status.has_value()},
                          {"normal exit", status && WIFEXITED(*status)},
                          {"child assertions passed", status && WIFEXITED(*status) && WEXITSTATUS(*status) == 0}});
}

void test_empty_worker_services_disable_interactive_tools()
{
  auto const root = create_empty_root("dispatcher-empty-worker-services");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  int question_prompts = 0;
  int task_runs = 0;
  ava::agent::ToolDispatchServices owned_services{
      .question_resolver = [&question_prompts](ava::agent::QuestionPrompt const&) -> ava::core::Result<ava::agent::QuestionAnswer> {
        ++question_prompts;
        return ava::agent::QuestionAnswer{.selected_options = {"ok"}, .custom_text = ""};
      },
      .task_subagent_runner = [&task_runs](ava::agent::TaskSubagentRequest const& request) -> ava::core::Result<ava::agent::TaskSubagentResult> {
        ++task_runs;
        return ava::agent::TaskSubagentResult{.task_id = "task_worker",
                                              .job_id = {},
                                              .session_path = {},
                                              .subagent_type = request.subagent_type,
                                              .state = "completed",
                                              .final_text = "done",
                                              .stop_reason = {}};
      }};
  auto const dispatcher_result = ava::agent::ToolDispatcher::create_strict(
      ava::tools::ToolContext{.workspace_dir = workspace,
                              .mode = ava::agent::Mode::Build,
                              .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
                                return ava::permissions::PermissionResolution::Allow;
                              },
                              .session_id = "owner"},
      owned_services);
  if (!ava::tests::require(dispatcher_result.has_value(), dispatcher_result ? "dispatcher fixture composes" : dispatcher_result.error().format()))
    return;
  auto const& dispatcher = *dispatcher_result;

  auto owned_question =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "owned_question", .name = "question", .arguments_json = R"({"question":"Ready?"})"});
  auto owned_task = dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "owned_task", .name = "task", .arguments_json = R"({"description":"work","prompt":"do it","subagent_type":"general"})"});
  expect(owned_question && owned_question->success && question_prompts == 1 && owned_task && owned_task->success && task_runs == 1,
         "dispatcher-owned services retain question and task behavior");

  ava::tools::ToolContext worker_context{.workspace_dir = workspace, .mode = ava::agent::Mode::Build, .session_id = "owner"};
  auto worker_question =
      dispatcher.dispatch_with_context(worker_context, ava::agent::ToolDispatchServices{},
                                       ava::agent::ProviderToolCall{.id = "worker_question", .name = "question", .arguments_json = R"({"question":"Ready?"})"});
  auto worker_task = dispatcher.dispatch_with_context(
      worker_context, ava::agent::ToolDispatchServices{},
      ava::agent::ProviderToolCall{
          .id = "worker_task", .name = "task", .arguments_json = R"({"description":"work","prompt":"do it","subagent_type":"general"})"});
  auto worker_job = dispatcher.dispatch_with_context(worker_context, ava::agent::ToolDispatchServices{},
                                                     ava::agent::ProviderToolCall{.id = "worker_job", .name = "job", .arguments_json = R"({"action":"list"})"});
  expect(worker_question && !worker_question->success && worker_question->result_text.find("unavailable") != std::string::npos && worker_task &&
             !worker_task->success && worker_task->result_text.find("unavailable") != std::string::npos && worker_job && !worker_job->success &&
             worker_job->result_text.find("unavailable") != std::string::npos && question_prompts == 1 && task_runs == 1,
         "explicit empty worker-style services make question/task/job unavailable without touching dispatcher-owned services");
}

void test_tool_dispatcher_plan_mode_denies_mutation()
{
  auto const root = create_empty_root("dispatcher-plan");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  auto const dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::agent::Mode::Plan});
  if (!ava::tests::require(dispatcher_result.has_value(), dispatcher_result ? "dispatcher fixture composes" : dispatcher_result.error().format()))
    return;
  auto const& dispatcher = *dispatcher_result;
  auto denied = dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "call_write", .name = "write_file", .arguments_json = "{\"path\":\"main.cpp\",\"content\":\"bad\"}"});
  expect(denied && !denied->success && denied->result_text.find("permission_denied") != std::string::npos &&
             denied->result_text.find("request_id") != std::string::npos && denied->result_text.find("/permissions audit show permreq_") != std::string::npos,
         "tool dispatcher keeps plan mode source mutation denied inside tools");
  auto const denied_structured = denied ? ava::agent::serialize_tool_result_payload_json(*denied) : std::string{};
  expect(denied_structured.find("\"permission_request_ids\":[\"permreq_") != std::string::npos &&
             denied_structured.find("/permissions diagnose permreq_") != std::string::npos,
         "tool dispatcher exposes actionable permission denial details in structured results");
  expect(!std::filesystem::exists(workspace / "main.cpp"), "denied plan mode write does not create source file");
}

void test_subagent_launch_display_normalization_and_validated_task_callback()
{
  auto const normalized = ava::agent::SubagentLaunchDisplay::normalized("  Configured\tModel  ", std::string_view("  high\n"));
  auto const idempotent = ava::agent::SubagentLaunchDisplay::normalized(normalized.model_display_name(), std::string_view(normalized.reasoning_label()));
  auto const defaulted = ava::agent::SubagentLaunchDisplay::normalized("");
  auto const controlled = ava::agent::SubagentLaunchDisplay::normalized("unsafe\x1b[31m", std::string_view("low\x7f"));
  auto const malformed = ava::agent::SubagentLaunchDisplay::normalized(std::string("\xC3\x28", 2), std::string_view(std::string("\xC2\x9B", 2)));
  auto const bounded = ava::agent::SubagentLaunchDisplay::normalized(std::string(200, 'm'), std::string_view(std::string(80, 'r')));
  auto const utf8_bounded = ava::agent::SubagentLaunchDisplay::normalized(std::string(127, 'u') + "\xF0\x9F\x98\x80");
  expect(normalized.model_display_name() == "Configured Model" && normalized.reasoning_label() == "high" && idempotent == normalized &&
             defaulted.model_display_name().empty() && defaulted.reasoning_label() == "default" && controlled.model_display_name().empty() &&
             controlled.reasoning_label() == "default" && malformed.model_display_name().empty() && malformed.reasoning_label() == "default" &&
             bounded.model_display_name().size() == ava::agent::kMaxSubagentLaunchModelDisplayNameBytes &&
             bounded.reasoning_label().size() == ava::agent::kMaxSubagentLaunchReasoningLabelBytes && utf8_bounded.model_display_name().size() == 127 &&
             ava::core::json::is_valid_utf8(utf8_bounded.model_display_name()),
         "subagent launch display normalization is idempotent, bounded, UTF-8/control-safe, defaulted, and never falls back to ids");

  auto const root = create_empty_root("dispatcher-private-task-launch");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::tools::ToolContext allow_context{.workspace_dir = workspace,
                                        .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
                                          return ava::permissions::PermissionResolution::Allow;
                                        }};
  std::vector<ava::agent::SubagentLaunchNotification> notifications;
  std::size_t runs = 0;
  ava::agent::ToolDispatchServices services{
      .task_subagent_runner = [&runs](ava::agent::TaskSubagentRequest const& request) -> ava::core::Result<ava::agent::TaskSubagentResult> {
        ++runs;
        return ava::agent::TaskSubagentResult{.task_id = "child",
                                              .job_id = {},
                                              .session_path = {},
                                              .subagent_type = request.subagent_type,
                                              .state = "completed",
                                              .final_text = "done",
                                              .stop_reason = "completed"};
      },
      .subagent_launch = {.display = normalized,
                          .request_id = "request-exact",
                          .correlation_id = "correlation-exact",
                          .sink = [&notifications](ava::agent::SubagentLaunchNotification const& notification) { notifications.push_back(notification); }}};
  auto dispatcher_result = ava::agent::ToolDispatcher::create_strict(allow_context, services);
  if (!ava::tests::require(dispatcher_result.has_value(), dispatcher_result ? "dispatcher fixture composes" : dispatcher_result.error().format()))
    return;
  auto& dispatcher = *dispatcher_result;
  auto valid = dispatcher.dispatch(ava::agent::ProviderToolCall{
      .id = "backend-call-exact", .name = "task", .arguments_json = R"({"description":"work","prompt":"do it","subagent_type":"general"})"});
  expect(valid && valid->success && runs == 1 && notifications.size() == 1 && notifications.front().tool_call_id == "backend-call-exact" &&
             notifications.front().request_id == "request-exact" && notifications.front().correlation_id == "correlation-exact" &&
             notifications.front().display == normalized,
         "validated built-in task callback carries exact backend/request/correlation identity and immutable display");

  auto malformed_task =
      dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "malformed", .name = "task", .arguments_json = R"({"description":"missing fields"})"});
  auto similar = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "similar", .name = "task_similar", .arguments_json = "{}"});
  auto job = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "job", .name = "job", .arguments_json = R"({"action":"list"})"});
  auto excluded_result = ava::agent::ToolDispatcher::create_strict(allow_context, services, ava::agent::ToolVisibilityOptions{.excluded_tools = {"task"}});
  if (!ava::tests::require(excluded_result.has_value(), excluded_result ? "dispatcher fixture composes" : excluded_result.error().format()))
    return;
  auto& excluded = *excluded_result;
  auto excluded_task = excluded.dispatch(
      ava::agent::ProviderToolCall{.id = "excluded", .name = "task", .arguments_json = R"({"description":"work","prompt":"do it","subagent_type":"general"})"});
  expect(malformed_task && !malformed_task->success && similar && !similar->success && job && !job->success && excluded_task && !excluded_task->success &&
             notifications.size() == 1 && runs == 1,
         "malformed, unknown, similarly named, job, and excluded calls never emit private task launch metadata");

  std::size_t preflights = 0;
  auto checked_context = allow_context;
  checked_context.auto_allow_deny_preflight = [&](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    ++preflights;
    return ava::permissions::PermissionResolution::Allow;
  };
  auto checked_dispatcher_result = ava::agent::ToolDispatcher::create_strict(checked_context, services);
  if (!ava::tests::require(checked_dispatcher_result.has_value(),
                           checked_dispatcher_result ? "dispatcher fixture composes" : checked_dispatcher_result.error().format()))
    return;
  auto& checked_dispatcher = *checked_dispatcher_result;
  for (auto const* field : {"task_id", "command"})
  {
    for (auto const* value : {"null", "true", "42", "[]", "{}"})
    {
      auto arguments = std::string(R"({"description":"work","prompt":"do it","subagent_type":"general",")") + field + "\":" + value + "}";
      auto rejected = checked_dispatcher.dispatch({.id = "bad-optional", .name = "task", .arguments_json = arguments});
      expect(rejected && !rejected->success && runs == 1 && notifications.size() == 1 && preflights == 0,
             "non-string optional task arguments fail before runner, permission, or launch notification");
    }
  }
  auto typo = checked_dispatcher.dispatch(
      {.id = "typo", .name = "task", .arguments_json = R"({"description":"work","prompt":"do it","subagent_type":"general","taskid":"child"})"});
  expect(typo && !typo->success && runs == 1 && notifications.size() == 1 && preflights == 0, "task field typos fail before permission or publication");

  std::size_t denied_notifications = 0;
  ava::tools::ToolContext deny_context{.workspace_dir = workspace,
                                       .auto_allow_deny_preflight = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
                                         return ava::permissions::PermissionResolution::Deny;
                                       }};
  auto denied_services = services;
  denied_services.subagent_launch.sink = [&](auto const&) { ++denied_notifications; };
  auto denied_dispatcher_result = ava::agent::ToolDispatcher::create_strict(deny_context, denied_services);
  if (!ava::tests::require(denied_dispatcher_result.has_value(),
                           denied_dispatcher_result ? "dispatcher fixture composes" : denied_dispatcher_result.error().format()))
    return;
  auto& denied_dispatcher = *denied_dispatcher_result;
  auto denied = denied_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "denied", .name = "task", .arguments_json = R"({"description":"work","prompt":"do it","subagent_type":"general"})"});

  std::size_t failed_notifications = 0;
  auto failed_services = services;
  failed_services.task_subagent_runner = [](auto const&) -> ava::core::Result<ava::agent::TaskSubagentResult> {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "start failed"));
  };
  failed_services.subagent_launch.sink = [&](auto const&) {
    ++failed_notifications;
    throw std::runtime_error("private sink failure");
  };
  auto failed_dispatcher_result = ava::agent::ToolDispatcher::create_strict(allow_context, failed_services);
  if (!ava::tests::require(failed_dispatcher_result.has_value(),
                           failed_dispatcher_result ? "dispatcher fixture composes" : failed_dispatcher_result.error().format()))
    return;
  auto& failed_dispatcher = *failed_dispatcher_result;
  auto failed = failed_dispatcher.dispatch(
      ava::agent::ProviderToolCall{.id = "failed", .name = "task", .arguments_json = R"({"description":"work","prompt":"do it","subagent_type":"general"})"});
  expect(denied && !denied->success && denied_notifications == 1 && failed && !failed->success && failed_notifications == 1 && runs == 1,
         "valid task permission denial and start failure retain launch metadata while a throwing private sink cannot affect execution");
}

void test_permission_denial_guidance_provider_only_channel()
{
  auto const root = create_empty_root("dispatcher-guidance-channel");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const outside = root / "outside-note.txt";
  ava::tests::write_test_file(outside, "outside");

  constexpr char const* kGuidance = "stay on the sealed workspace path";
  std::vector<ava::tools::PermissionAuditEvent> audits;
  auto const dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ava::permissions::PermissionResolutionDecision denied{ava::permissions::PermissionResolution::Deny, "not approved"};
        denied.user_guidance = kGuidance;
        return denied;
      },
      .permission_audit_sink = [&audits](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
        audits.push_back(event);
        return {};
      }});
  if (!ava::tests::require(dispatcher_result.has_value(), dispatcher_result ? "dispatcher fixture composes" : dispatcher_result.error().format()))
    return;
  auto const& dispatcher = *dispatcher_result;

  auto const path_args = std::string("{\"path\":\"") + ava::core::json::escape(outside.string()) + "\"}";
  auto denied = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_guided_read", .name = "read_file", .arguments_json = path_args});
  expect(denied && !denied->success, "guided provider-tool denial returns a failed ToolDispatchResult");
  if (!ava::tests::require(denied.has_value(), "guided denial returns a tool result"))
    return;

  expect(denied->provider_user_guidance == kGuidance, "ToolDispatchResult dedicated provider field carries validated guidance");
  expect(denied->result_text.find(kGuidance) == std::string::npos && denied->result_text.find("user_guidance") == std::string::npos &&
             denied->result_text.find("provider_user_guidance") == std::string::npos && denied->payload.error_details.find(kGuidance) == std::string::npos &&
             denied->payload.error_message.find(kGuidance) == std::string::npos && denied->payload.summary.find(kGuidance) == std::string::npos,
         "guided denial keeps ordinary result_text and ToolResultPayload guidance-free");
  auto const structured = ava::agent::serialize_tool_result_payload_json(*denied);
  expect(structured.find(kGuidance) == std::string::npos && structured.find("provider_user_guidance") == std::string::npos,
         "structured_result serialization never includes provider_user_guidance");

  bool audits_clean = !audits.empty();
  for (auto const& event : audits)
  {
    auto const json = ava::tools::permission_audit_data_json(event);
    audits_clean = audits_clean && json.find(kGuidance) == std::string::npos && json.find("user_guidance") == std::string::npos &&
                   json.find("provider_user_guidance") == std::string::npos;
  }
  expect(audits_clean, "permission audits remain free of one-shot denial guidance");

  std::optional<ava::session::SessionEntry> persisted;
  auto append_sink = [&](ava::session::SessionEntry entry) -> ava::core::VoidResult {
    persisted = std::move(entry);
    return {};
  };
  expect(static_cast<bool>(ava::agent::append_tool_result(append_sink, *denied)), "guided denial persists as a session tool_result");
  expect(persisted && ava::core::json::string_field(persisted->data_json, "provider_user_guidance") == std::string(kGuidance),
         "session ToolResult dedicated provider_user_guidance field contains validated guidance");
  if (!ava::tests::require(persisted.has_value(), "guided denial persists a tool-result entry"))
    return;
  auto const stored_result = ava::core::json::string_field(persisted->data_json, "result").value_or("");
  auto const stored_structured = ava::core::json::object_field(persisted->data_json, "structured_result").value_or("");
  expect(stored_result.find(kGuidance) == std::string::npos && stored_structured.find(kGuidance) == std::string::npos,
         "session ordinary result and structured_result stay guidance-free");

  auto const session_guidance = ava::core::json::string_field(persisted->data_json, "provider_user_guidance").value_or("");
  auto const reconstructed = ava::permissions::with_provider_user_guidance(stored_result, session_guidance);
  expect(reconstructed.find(kGuidance) != std::string::npos && reconstructed != stored_result,
         "reconstructed provider-facing tool-result content includes validated guidance");
  expect(ava::permissions::with_provider_user_guidance(stored_result, "") == stored_result, "empty guidance leaves unguided provider content bytes unchanged");

  // Minimal valid paired ToolCall/ToolResult sequence exercises the real provider
  // message builder path (not only the with_provider_user_guidance helper).
  auto messages = ava::agent::build_provider_messages_from_entries(
      {ava::session::SessionEntry{.id = "call_entry",
                                  .parent_id = "",
                                  .type = ava::session::EntryType::ToolCall,
                                  .timestamp = "2026-07-29T00:00:02Z",
                                  .data_json = R"({"call_id":"call_guided_read","name":"read_file","arguments":"{}"})"},
       *persisted},
      ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = "openai",
                                                                                .model_id = "gpt-test",
                                                                                .api_family = "openai_responses",
                                                                                .reasoning_format = "openai_responses",
                                                                                .supports_tools = true,
                                                                                .supports_images = false}});
  expect(static_cast<bool>(messages), messages ? "provider message reconstruction succeeds for paired guided tool result"
                                               : "provider message reconstruction succeeds for paired guided tool result: " + messages.error().format());
  bool provider_message_content_has_guidance = false;
  bool provider_content_part_has_guidance = false;
  if (messages)
  {
    for (auto const& message : *messages)
    {
      if (message.content.find(kGuidance) != std::string::npos)
        provider_message_content_has_guidance = true;
      for (auto const& part : message.content_parts)
      {
        if (part.type == ava::provider::ContentPartType::ToolResult && part.text.find(kGuidance) != std::string::npos)
          provider_content_part_has_guidance = true;
      }
    }
  }
  expect(provider_message_content_has_guidance && provider_content_part_has_guidance,
         "rebuilt provider messages and ToolResult content parts contain validated guidance");

  ava::agent::ToolTimelineEntry timeline{.status = ava::agent::ToolTimelineStatus::Error,
                                         .call_id = denied->call_id,
                                         .name = denied->name,
                                         .argument_summary = "read",
                                         .result_summary = denied->payload.summary,
                                         .arguments_json = "{}"};
  ava::agent::populate_tool_timeline_metadata(timeline, *denied);
  expect(timeline.result_json.find(kGuidance) == std::string::npos && timeline.structured_result_json.find(kGuidance) == std::string::npos &&
             timeline.error_details.find(kGuidance) == std::string::npos,
         "tool timeline metadata built from dispatch result stays guidance-free");

  auto const runtime_event = ava::app::runtime_event_from_tool_timeline_entry({}, timeline);
  auto const envelope = ava::event::to_event_envelope(runtime_event);
  expect(envelope.payload_json.find(kGuidance) == std::string::npos && envelope.payload_json.find("provider_user_guidance") == std::string::npos,
         "typed RuntimeEvent / RPC-equivalent envelope JSON stays guidance-free");

  ava::app::acp::RuntimeSessionUpdateMapper mapper(ava::app::acp::RuntimeSessionUpdateMapperOptions{.workspace_root = workspace, .message_id = "msg"});
  auto encoded = mapper.map_and_encode(runtime_event);
  expect(encoded.has_value(), "ACP mapper encodes tool result events");
  if (encoded && *encoded)
  {
    expect((*encoded)->find(kGuidance) == std::string::npos && (*encoded)->find("provider_user_guidance") == std::string::npos,
           "ACP session update content built from the timeline stays guidance-free");
  }

  auto const forged_dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ava::permissions::PermissionResolutionDecision denied_decision{ava::permissions::PermissionResolution::Deny};
        denied_decision.user_guidance = "evil\nforged\x01";
        return denied_decision;
      }});
  if (!ava::tests::require(forged_dispatcher_result.has_value(),
                           forged_dispatcher_result ? "dispatcher fixture composes" : forged_dispatcher_result.error().format()))
    return;
  auto const& forged_dispatcher = *forged_dispatcher_result;
  auto forged = forged_dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "call_forged", .name = "read_file", .arguments_json = path_args});
  expect(forged && !forged->success && forged->provider_user_guidance.empty() && forged->result_text.find("evil") == std::string::npos,
         "invalid forged capture guidance is omitted fail-closed");

  ava::agent::ToolDispatchResult forged_session_result = *denied;
  forged_session_result.provider_user_guidance = "session\nforged\x7f";
  std::optional<ava::session::SessionEntry> forged_persisted;
  auto forged_sink = [&](ava::session::SessionEntry entry) -> ava::core::VoidResult {
    forged_persisted = std::move(entry);
    return {};
  };
  expect(static_cast<bool>(ava::agent::append_tool_result(forged_sink, forged_session_result)), "forged session guidance still appends the tool_result");
  expect(forged_persisted && !ava::core::json::string_field(forged_persisted->data_json, "provider_user_guidance"),
         "invalid forged session guidance is omitted fail-closed on append");

  expect(ava::permissions::with_provider_user_guidance(stored_result, "evil\nforged") == stored_result,
         "invalid forged session-file guidance is omitted fail-closed on replay");

  auto const injected = ava::permissions::with_provider_user_guidance(R"({"tool":"read_file","ok":false})", kGuidance);
  expect(injected.find("\"provider_user_guidance\":\"stay on the sealed workspace path\"") != std::string::npos,
         "controlled JSON field injection places validated guidance for provider replay");
  auto const unguided_bytes = std::string(R"({"tool":"read_file","ok":false})");
  expect(ava::permissions::with_provider_user_guidance(unguided_bytes, "bad\nguidance") == unguided_bytes,
         "invalid guidance leaves unguided provider content bytes unchanged");
}

void test_apply_patch_rejects_symlink_targets()
{
  auto const root = create_empty_root("dispatcher-patch-symlink-target");
  auto const workspace = ava::core::normalized_absolute_path(root / "workspace");
  std::filesystem::create_directories(workspace);
  auto const link_target = workspace / "AGENTS.md";
  auto const link = workspace / "CLAUDE.md";
  ava::tests::write_test_file(link_target, "agents old\n");
  std::error_code link_error;
  std::filesystem::create_symlink("AGENTS.md", link, link_error);
  auto secure = ava::tools::SecureWorkspace::open(workspace);
  expect(!link_error && secure.has_value(), "patch symlink fixture creates an in-workspace link and anchors the workspace");
  if (link_error || !secure)
    return;

  auto const read_text = [](std::filesystem::path const& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  };
  for (bool const secure_mode : {true, false})
  {
    auto const dispatcher_result = ava::agent::ToolDispatcher::create_strict(ava::tools::ToolContext{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .secure_workspace = secure_mode ? *secure : nullptr,
    });
    if (!ava::tests::require(dispatcher_result.has_value(), dispatcher_result ? "dispatcher fixture composes" : dispatcher_result.error().format()))
      return;
    auto const& dispatcher = *dispatcher_result;
    auto patched = dispatcher.dispatch(ava::agent::ProviderToolCall{
        .id = "call_patch_symlink_target",
        .name = "apply_patch",
        .arguments_json = R"({"edits":[{"path":"CLAUDE.md","old_text":"old","new_text":"new"}]})",
    });
    std::error_code status_error;
    expect(patched && !patched->success && patched->result_text.find("does not patch through or replace symlinks") != std::string::npos &&
               std::filesystem::is_symlink(std::filesystem::symlink_status(link, status_error)) && read_text(link_target) == "agents old\n",
           std::string(secure_mode ? "descriptor-secure" : "non-secure") +
               " apply_patch rejects a symlink target before reading it and leaves both the link and its target unchanged");
  }
}

void test_strict_dispatcher_requires_secure_workspace()
{
  auto const workspace = create_empty_root("strict-dispatcher-missing-anchor");
  ava::tests::write_test_file(workspace / "note.txt", "original");
  ava::tools::ToolContext const context{.workspace_dir = workspace, .require_descriptor_secure_workspace = true};
  auto dispatcher = ava::agent::ToolDispatcher::create_strict(context);
  expect(!dispatcher && dispatcher.error().category() == ava::core::ErrorCategory::InvalidArgument &&
             dispatcher.error().message() == "strict dispatcher requires an open secure workspace",
         "strict dispatcher rejects missing descriptor authority at construction");
  auto read = ava::tools::read_file(context, workspace / "note.txt");
  auto write = ava::tools::write_file(context, workspace / "new.txt", "bad", ava::tools::WriteOptions{.permission_already_checked = true});
  auto list = ava::tools::list_directory(context, workspace);
  expect(!read && !write && !list && !std::filesystem::exists(workspace / "new.txt"),
         "direct strict file contexts never fall through to pathname I/O when authority is missing");
}

void test_mixed_patch_reports_all_committed_paths()
{
  auto const root = create_empty_root("mixed-patch-commit-report");
  auto const workspace = root / "workspace";
  auto const outside = root / "outside";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(outside);
  auto const inside = workspace / "inside.txt";
  auto const first = outside / "first.txt";
  auto const second = outside / "second.txt";
  for (auto const& path : {inside, first, second})
    ava::tests::write_test_file(path, "old");
  auto secure = ava::tools::SecureWorkspace::open(workspace);
  expect(secure.has_value(), "mixed patch fixture opens its secure workspace");
  if (!secure)
    return;
  auto anchors = ava::core::AnchorSet::open({workspace});
  expect(anchors.has_value(), "mixed patch fixture opens explicit authority for approved external reads");
  if (!anchors)
    return;
  bool sabotaged = false;
  ava::tools::ToolContext context{.workspace_dir = workspace,
                                  .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
                                    return ava::permissions::PermissionResolution::Allow;
                                  },
                                  .cancel_requested =
                                      [&] {
                                        if (sabotaged)
                                          return false;
                                        std::size_t complete_stages = 0;
                                        for (auto const& entry : std::filesystem::directory_iterator(outside))
                                        {
                                          auto const name = entry.path().filename().string();
                                          if ((name.starts_with("first.txt.ava-patch-") || name.starts_with("second.txt.ava-patch-")) && entry.file_size() == 3)
                                            ++complete_stages;
                                        }
                                        if (complete_stages == 2)
                                        {
                                          std::filesystem::remove(second);
                                          std::filesystem::create_directory(second);
                                          sabotaged = true;
                                        }
                                        return false;
                                      },
                                  .secure_workspace = *secure};
  // External reads require AnchorSet authority even though their later patch
  // writes still use the non-strict pathname staging/commit path.
  context.anchor_set = *anchors;
  auto outside_read = ava::tools::read_file(context, first);
  expect(outside_read && outside_read->content == "old", "mixed patch fixture authorizes its external read before exercising partial commit");
  if (!outside_read)
    return;
  auto const dispatcher_result = ava::agent::ToolDispatcher::create_strict(context);
  if (!ava::tests::require(dispatcher_result.has_value(), dispatcher_result ? "dispatcher fixture composes" : dispatcher_result.error().format()))
    return;
  auto const& dispatcher = *dispatcher_result;
  std::string edits = "{\"edits\":[";
  for (auto const& path : {inside, first, second})
  {
    if (edits.back() != '[')
      edits += ',';
    edits += "{\"path\":\"" + ava::core::json::escape(path.generic_string()) + "\",\"old_text\":\"old\",\"new_text\":\"new\"}";
  }
  edits += "]}";
  auto result = dispatcher.dispatch(ava::agent::ProviderToolCall{.id = "mixed_commit", .name = "apply_patch", .arguments_json = edits});
  auto const read_text = [](std::filesystem::path const& path) {
    std::ifstream file(path);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  };
  expect(sabotaged && result && !result->success && read_text(inside) == "new" && read_text(first) == "new" && std::filesystem::is_directory(second),
         "mixed patch failure fixture commits the secure target and one pathname target before the failing rename");
  auto const committed = inside.generic_string() + ", " + first.generic_string();
  expect(result && result->result_text.find("already_committed_paths") != std::string::npos &&
             result->result_text.find(ava::core::json::escape(committed)) != std::string::npos,
         "mixed patch error reports both secure and already-renamed pathname targets together");
}

}  // namespace

void run_agent_tool_dispatcher_tests()
{
  ava::tests::HermeticTestEnvironment environment("tool-dispatcher-env-");
  ava::agent::ProviderToolCall const call{.id = "typed", .name = "read_file", .arguments_json = "{}"};
  auto negative = ava::agent::tool_dispatch::tool_error_result(call, ava::core::Error(ava::core::ErrorCategory::Tool, "not canceled"));
  auto typed =
      ava::agent::tool_dispatch::tool_error_result(call, ava::core::Error(ava::core::ErrorCategory::Tool, "operation stopped", ava::core::ErrorCode::Canceled));
  ava::tests::expect_all("tool cancellation uses typed identity rather than arbitrary error wording",
                         {{"untyped error not canceled", negative.payload.status != ava::agent::ToolResultStatus::Canceled},
                          {"typed error canceled", typed.payload.status == ava::agent::ToolResultStatus::Canceled}});
  test_tool_dispatcher_plugin_tool_inclusion_control();
  test_tool_dispatcher();
  test_dispatcher_contains_executor_exceptions();
  test_dispatcher_context_metadata_owns_strings();
  test_dispatcher_composition_errors_remain_errors();
  test_task_persistent_deny_preflight_blocks_runner();
  test_subagent_launch_display_normalization_and_validated_task_callback();
  test_task_mode_and_job_tool_controls();
  test_job_wait_parent_cancellation_keeps_background_running();
  test_empty_worker_services_disable_interactive_tools();
  test_tool_dispatcher_plan_mode_denies_mutation();
  test_permission_denial_guidance_provider_only_channel();
  test_apply_patch_rejects_symlink_targets();
  test_strict_dispatcher_requires_secure_workspace();
  test_mixed_patch_reports_all_committed_paths();
}
