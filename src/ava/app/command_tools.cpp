#include "sys.h"
#include "ava/event/events.h"
#include "ava/app/command_format.h"
#include "ava/app/command_tools.h"
#include "ava/tools/file_tools.h"
#include "ava/tools/tool_permission.h"
#include "ava/app/runtime/ExtensionResourcePolicy.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_event_adapters.h"
#include "ava/agent/tool_result.h"
#include "ava/tools/bash_tool.h"
#include "ava/tools/search_tools.h"
#include "ava/session/session_store.h"
#include "ava/permissions/permission_rules.h"
#include "ava/lsp/configured_provider.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <tuple>
#include <utility>

namespace ava::app {
namespace {

std::string json_bool(bool value)
{
  return value ? "true" : "false";
}

void append_spill_fields(std::string& text, std::filesystem::path const& path, bool spill_truncated)
{
  if (path.empty())
    return;
  text += ",\"spill_file\":\"" + ava::core::json::escape(path.filename().generic_string()) + "\"";
  text += ",\"spill_truncated\":" + json_bool(spill_truncated);
}

std::string read_line_summary(ava::tools::TextOutput const& output)
{
  if (!output.totals_known)
  {
    if (output.output_lines == 0)
      return "0 retained lines (original total unknown)";
    return "lines " + std::to_string(output.start_line) + "-" + std::to_string(output.end_line) + " (" + std::to_string(output.output_lines) +
           " retained; original total unknown)";
  }
  if (output.output_lines == 0)
    return "0/" + std::to_string(output.total_lines) + " lines";
  return "lines " + std::to_string(output.start_line) + "-" + std::to_string(output.end_line) + "/" + std::to_string(output.total_lines);
}

ava::core::VoidResult emit_tool_event(runtime::session_ts const& unlocked_session, ava::event::RuntimeEventSink const& sink,
                                     ava::agent::ToolTimelineEntry const& entry)
{
  auto const session_id = runtime::session_ts::crat(unlocked_session)->store.session_id();
  ava::event::RuntimeEventMetadata metadata{
      .timestamp = ava::session::now_timestamp(),
      .session_id = session_id,
  };
  return ava::event::emit_event(sink, runtime_event_from_tool_timeline_entry(std::move(metadata), entry));
}

ava::agent::ToolTimelineEntry command_result_entry(std::string const& call_id, std::string name, ava::agent::ToolTimelineStatus status,
                                                   std::string result_summary, std::string result_content)
{
  if (result_content.empty())
    result_content = result_summary;
  auto dispatch_result = ava::agent::with_tool_result_payload(ava::agent::ToolDispatchResult{
      .call_id = call_id,
      .name = name,
      .success = status == ava::agent::ToolTimelineStatus::Success,
      .result_text = result_content,
  });
  dispatch_result.payload.summary = result_summary;
  if (status == ava::agent::ToolTimelineStatus::Canceled)
  {
    dispatch_result.payload.status = ava::agent::ToolResultStatus::Canceled;
    dispatch_result.payload.error_category = "canceled";
    dispatch_result.payload.error_message = result_summary;
  }
  else if (status == ava::agent::ToolTimelineStatus::Error && !ava::core::json::is_valid_object(result_content))
  {
    dispatch_result.payload.error_message = result_summary;
    if (result_content != result_summary)
      dispatch_result.payload.error_details = result_content;
  }

  auto const result_json = ava::core::json::is_valid_object(result_content) ? result_content : std::string{};
  auto const& payload = dispatch_result.payload;
  return ava::agent::ToolTimelineEntry{
      .status = status,
      .call_id = call_id,
      .name = std::move(name),
      .result_summary = std::move(result_summary),
      .result_json = result_json,
      .structured_result_json = ava::agent::serialize_tool_result_payload_json(dispatch_result),
      .content_type = payload.content_type,
      .error_category = payload.error_category,
      .error_code = payload.error_code,
      .error_message = payload.error_message,
      .error_details = payload.error_details,
      .diff = payload.diff,
      .diff_truncated = payload.diff_truncated,
      .changed_paths = payload.changed_paths,
      .permission_request_ids = payload.permission_request_ids,
      .truncated = payload.truncated,
      .byte_limited = payload.byte_limited,
      .line_limited = payload.line_limited,
      .output_bytes = payload.output_bytes,
      .total_bytes = payload.total_bytes,
      .output_lines = payload.output_lines,
      .total_lines = payload.total_lines,
      .start_line = payload.start_line,
      .end_line = payload.end_line,
      .next_offset_line = payload.next_offset_line,
      .omitted_bytes = payload.omitted_bytes,
      .omitted_lines = payload.omitted_lines,
      .visible_matches = payload.visible_matches,
      .total_matches = payload.total_matches,
      .spill_path = payload.spill_path,
      .spill_truncated = payload.spill_truncated,
  };
}

ava::core::VoidResult record_tool_event(runtime::session_ts const& unlocked_session, ava::event::RuntimeEventSink const& sink, CommandResult& result,
                                        ava::agent::ToolTimelineEntry entry)
{
  if (auto emitted = emit_tool_event(unlocked_session, sink, entry); !emitted)
    return std::unexpected(std::move(emitted.error()));
  result.tool_timeline.push_back(std::move(entry));
  return {};
}

void add_permission_request_ids(ava::agent::ToolTimelineEntry& entry, std::vector<std::string> const& permission_request_ids)
{
  for (auto const& id : permission_request_ids)
  {
    if (id.empty())
      continue;
    if (std::ranges::find(entry.permission_request_ids, id) == entry.permission_request_ids.end())
    {
      entry.permission_request_ids.push_back(id);
    }
  }
}

}  // namespace

ava::tools::ToolContext make_tool_context(runtime::session_ts& unlocked_session, ava::permissions::PermissionResolver permission_resolver)
{
  auto const resource_policy = runtime::make_extension_resource_policy_1(unlocked_session);
  auto snapshot = [&] {
    SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
    return std::tuple{session_r->workspace_dir(),
                      session_r->store.session_path().parent_path() / "spill",
                      session_r->mode(),
                      session_r->permission_rule_store(),
                      session_r->anchor_set(),
                      session_r->ava_authority_roots_1(),
                      session_r->session_process_scope(),
                      session_r->store.session_id(),
                      session_r->model().provider_id,
                      session_r->model().model_id,
                      session_r->current_dir()};
  }();
  auto& [workspace_dir, spill_dir, mode, permission_rule_store, anchor_set, ava_authority_roots, process_scope, session_id, provider_id, model_id, current_dir] =
      snapshot;
  auto lsp_provider = ava::lsp::make_configured_lsp_provider(ava::lsp::ConfiguredLspProviderFiles{
      .global_config_file = resource_policy.global_lsp_config_file,
      .project_config_file = resource_policy.project_lsp_config_file,
      .workspace_root = workspace_dir,
      .anchor_set = anchor_set,
      .mode = mode,
      .permission_resolver = permission_resolver,
  });
  return ava::tools::ToolContext{
      .workspace_dir = workspace_dir,
      .spill_dir = spill_dir,
      .mode = mode,
      .permission_resolver = std::move(permission_resolver),
      .auto_allow_deny_preflight = ava::permissions::build_persistent_permission_deny_preflight(permission_rule_store),
      .permission_audit_sink = [&unlocked_session](ava::tools::PermissionAuditEvent const& event) -> ava::core::VoidResult {
        auto entry = ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                .parent_id = "",
                                                .type = ava::session::EntryType::PermissionDecision,
                                                 .timestamp = ava::session::now_timestamp(),
                                                 .data_json = ava::tools::permission_audit_data_json(event)};
        return runtime::session_ts::wat(unlocked_session)->append_owned(std::move(entry));
      },
      .protected_permission_rule_store =
          ava::permissions::PermissionRuleStore{.global_rules_file = permission_rule_store.global_rules_file,
                                                .workspace_rules_file = permission_rule_store.workspace_rules_file,
                                                .workspace_dir = permission_rule_store.workspace_dir},
      .anchor_set = anchor_set,
      .ava_authority_roots = ava_authority_roots,
      .process_scope = process_scope,
      .lsp_diagnostics_provider = lsp_provider ? *lsp_provider : nullptr,
      .plugin_global_plugins_dir = resource_policy.plugin_discovery.global_plugins_dir,
      .plugin_project_plugins_dir = resource_policy.plugin_discovery.project_plugins_dir,
      .plugin_enablement_file = resource_policy.plugin_enablement_file,
      .include_project_plugins = resource_policy.include_project_resources,
      .mcp_global_config_file = resource_policy.mcp_config.global_config_file,
      .mcp_project_config_file = resource_policy.mcp_config.project_config_file,
      .include_project_mcp_config = resource_policy.include_project_resources,
      .include_project_skills = resource_policy.include_project_resources,
      .session_id = session_id,
      .provider_id = provider_id,
      .model_id = model_id,
      .current_dir = current_dir};
}

ava::core::VoidResult record_tool_start(runtime::session_ts const& unlocked_session, ava::event::RuntimeEventSink const& sink, CommandResult& result,
                                        std::string const& call_id, std::string name, std::string argument_summary)
{
  return record_tool_event(
      unlocked_session, sink, result,
      ava::agent::ToolTimelineEntry{
          .status = ava::agent::ToolTimelineStatus::Running, .call_id = call_id, .name = std::move(name), .argument_summary = std::move(argument_summary)});
}

ava::core::VoidResult record_tool_result(runtime::session_ts const& unlocked_session, ava::event::RuntimeEventSink const& sink, CommandResult& result,
                                         std::string const& call_id, std::string name, ava::agent::ToolTimelineStatus status, std::string result_summary,
                                         std::string result_content, std::vector<std::string> permission_request_ids)
{
  auto entry = command_result_entry(call_id, std::move(name), status, std::move(result_summary), std::move(result_content));
  add_permission_request_ids(entry, permission_request_ids);
  return record_tool_event(unlocked_session, sink, result, std::move(entry));
}

ava::core::Result<CommandResult> run_tool_command(runtime::session_ts& unlocked_session, CommandRequest& request)
{
  CommandResult result;
  result.handled = true;
  auto const& line = request.command;
  auto context = make_tool_context(unlocked_session, request.permission_resolver);
  context.cancel_requested = request.cancel_requested;
  context.permission_request_ids = std::make_shared<std::vector<std::string>>();
  if (context.lsp_diagnostics_provider)
    context.lsp_diagnostics_provider->set_permission_request_ids(context.permission_request_ids);
  auto linked_permission_ids = [&]() -> std::vector<std::string> {
    return context.permission_request_ids ? *context.permission_request_ids : std::vector<std::string>{};
  };

  if (line.starts_with("/read "))
  {
    auto const argument = line.substr(6);
    auto const call_id = ava::core::make_id("cmd");
    if (auto recorded = record_tool_start(unlocked_session, request.event_sink, result, call_id, "read", argument); !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    auto const output = ava::tools::read_file(context, context.current_dir / argument);
    if (!output)
    {
      auto const text = output.error().format();
      if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "read", ava::agent::ToolTimelineStatus::Error, text, {},
                                             linked_permission_ids());
          !recorded)
      {
        return std::unexpected(std::move(recorded.error()));
      }
      add_output(result, text);
      return result;
    }
    std::string text = output->content;
    if (output->truncated)
    {
      text += "\n[truncated " + read_line_summary(*output);
      if (output->next_offset_line > 0)
        text += "; next offset " + std::to_string(output->next_offset_line);
      if (output->byte_limited)
        text += "; byte cap reached";
      text += "]";
    }
    auto read_result_json = "{\"tool\":\"read\",\"ok\":true,\"path\":\"" + ava::core::json::escape(argument) + "\",\"content\":\"" +
                            ava::core::json::escape(output->content) + "\",\"truncated\":" + json_bool(output->truncated) +
                            ",\"byte_limited\":" + json_bool(output->byte_limited) + ",\"line_limited\":" + json_bool(output->line_limited);
    if (output->totals_known)
      read_result_json += ",\"total_bytes\":" + std::to_string(output->total_bytes);
    read_result_json += ",\"output_bytes\":" + std::to_string(output->output_bytes) + ",\"output_lines\":" + std::to_string(output->output_lines) +
                        ",\"start_line\":" + std::to_string(output->start_line) + ",\"end_line\":" + std::to_string(output->end_line);
    if (output->totals_known)
      read_result_json += ",\"total_lines\":" + std::to_string(output->total_lines);
    if (output->next_offset_line > 0)
      read_result_json += ",\"next_offset_line\":" + std::to_string(output->next_offset_line);
    read_result_json += "}";
    if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "read", ava::agent::ToolTimelineStatus::Success,
                                           read_line_summary(*output), read_result_json, linked_permission_ids());
        !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    add_output(result, std::move(text));
    return result;
  }

  if (line.starts_with("/glob ") || line.starts_with("/find "))
  {
    bool const pi_find_alias = line.starts_with("/find ");
    auto const pattern = line.substr(6);
    auto const call_id = ava::core::make_id("cmd");
    auto const timeline_name = pi_find_alias ? std::string("find") : std::string("glob");
    if (auto recorded = record_tool_start(unlocked_session, request.event_sink, result, call_id, timeline_name, pattern); !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    auto const glob = ava::tools::glob_files(context, pattern);
    if (!glob)
    {
      auto const text = glob.error().format();
      if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, timeline_name, ava::agent::ToolTimelineStatus::Error, text, {},
                                             linked_permission_ids());
          !recorded)
      {
        return std::unexpected(std::move(recorded.error()));
      }
      add_output(result, text);
      return result;
    }
    std::string output;
    for (auto const& path : glob->paths) output += display_path(path, context.current_dir) + '\n';
    if (glob->truncated)
    {
      output += "[truncated " + std::to_string(glob->paths.size()) + '/' + std::to_string(glob->total_matches) + " matches]\n";
    }
    std::string glob_result_json = "{\"tool\":\"glob\",\"ok\":true,\"pattern\":\"" + ava::core::json::escape(pattern) + "\",\"paths\":[";
    for (std::size_t index = 0; index < glob->paths.size(); ++index)
    {
      if (index > 0)
        glob_result_json += ',';
      glob_result_json += "\"" + ava::core::json::escape(glob->paths[index].generic_string()) + "\"";
    }
    glob_result_json += "],\"truncated\":" + json_bool(glob->truncated) + ",\"total_matches\":" + std::to_string(glob->total_matches) +
                        ",\"output_matches\":" + std::to_string(glob->paths.size());
    append_spill_fields(glob_result_json, glob->spill_path, glob->spill_truncated);
    glob_result_json += "}";
    if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, timeline_name, ava::agent::ToolTimelineStatus::Success,
                                           std::to_string(glob->paths.size()) + " matches", glob_result_json, linked_permission_ids());
        !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    add_output(result, std::move(output));
    return result;
  }

  if (line == "/ls" || line.starts_with("/ls "))
  {
    auto const argument = line == "/ls" ? std::string(".") : line.substr(4);
    auto const call_id = ava::core::make_id("cmd");
    if (auto recorded = record_tool_start(unlocked_session, request.event_sink, result, call_id, "ls", argument); !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    auto const listed = ava::tools::list_directory(context, context.current_dir / argument);
    if (!listed)
    {
      auto const text = listed.error().format();
      if (auto recorded =
              record_tool_result(unlocked_session, request.event_sink, result, call_id, "ls", ava::agent::ToolTimelineStatus::Error, text, {},
                                 linked_permission_ids());
          !recorded)
      {
        return std::unexpected(std::move(recorded.error()));
      }
      add_output(result, text);
      return result;
    }
    std::string output;
    for (auto const& entry : listed->entries)
    {
      output += entry.name;
      if (entry.directory)
        output += '/';
      output += '\n';
    }
    if (listed->truncated)
    {
      output += "[truncated " + std::to_string(listed->entries.size()) + '/' + std::to_string(listed->total_entries) + " entries]\n";
    }
    std::string list_result_json = "{\"tool\":\"list_directory\",\"ok\":true,\"path\":\"" + ava::core::json::escape(argument) + "\",\"entries\":[";
    for (std::size_t index = 0; index < listed->entries.size(); ++index)
    {
      if (index > 0)
        list_result_json += ',';
      auto const& entry = listed->entries[index];
      list_result_json += "{\"name\":\"" + ava::core::json::escape(entry.name) + "\",\"type\":\"" + std::string(entry.directory ? "directory" : "file") +
                          "\",\"size\":" + std::to_string(entry.size) + "}";
    }
    list_result_json += "],\"truncated\":" + json_bool(listed->truncated) + ",\"total_entries\":" + std::to_string(listed->total_entries) + "}";
    if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "ls", ava::agent::ToolTimelineStatus::Success,
                                           std::to_string(listed->entries.size()) + " entries", list_result_json, linked_permission_ids());
        !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    add_output(result, std::move(output));
    return result;
  }

  if (line.starts_with("/grep "))
  {
    auto const rest = line.substr(6);
    auto const split = rest.find(' ');
    auto const pattern = split == std::string::npos ? rest : rest.substr(0, split);
    auto const include = split == std::string::npos ? std::string("**/*") : rest.substr(split + 1);
    auto const call_id = ava::core::make_id("cmd");
    if (auto recorded = record_tool_start(unlocked_session, request.event_sink, result, call_id, "grep", pattern); !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    auto const grep = ava::tools::grep_files(context, pattern, include);
    if (!grep)
    {
      auto const text = grep.error().format();
      if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "grep", ava::agent::ToolTimelineStatus::Error, text, {},
                                             linked_permission_ids());
          !recorded)
      {
        return std::unexpected(std::move(recorded.error()));
      }
      add_output(result, text);
      return result;
    }
    std::string output;
    for (auto const& match : grep->matches)
    {
      output += display_path(match.path, context.current_dir) + ':' + std::to_string(match.line_number) + ": " + match.line;
      if (match.line_truncated)
        output += " [line truncated]";
      output += '\n';
    }
    if (grep->truncated)
    {
      output += "[truncated " + std::to_string(grep->matches.size()) + '/' + std::to_string(grep->total_matches) + " matches]\n";
    }
    std::string grep_result_json = "{\"tool\":\"grep\",\"ok\":true,\"pattern\":\"" + ava::core::json::escape(pattern) + "\",\"include\":\"" +
                                   ava::core::json::escape(include) + "\",\"content\":\"" + ava::core::json::escape(output) +
                                   "\",\"truncated\":" + json_bool(grep->truncated) + ",\"total_matches\":" + std::to_string(grep->total_matches) +
                                   ",\"output_matches\":" + std::to_string(grep->matches.size());
    append_spill_fields(grep_result_json, grep->spill_path, grep->spill_truncated);
    grep_result_json += "}";
    if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "grep", ava::agent::ToolTimelineStatus::Success,
                                           std::to_string(grep->matches.size()) + " matches", grep_result_json, linked_permission_ids());
        !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    add_output(result, std::move(output));
    return result;
  }

  if (line.starts_with("/write "))
  {
    auto const rest = line.substr(7);
    auto const split = rest.find(' ');
    if (split == std::string::npos)
    {
      add_output(result, missing_argument("/write <path> <text>"));
      return result;
    }
    auto const path_text = rest.substr(0, split);
    auto const text = rest.substr(split + 1);
    auto const call_id = ava::core::make_id("cmd");
    if (auto recorded = record_tool_start(unlocked_session, request.event_sink, result, call_id, "write", path_text); !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    auto const write = ava::tools::write_file(context, context.current_dir / path_text, text);
    if (!write)
    {
      auto const error_text = write.error().format();
      if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "write", ava::agent::ToolTimelineStatus::Error,
                                             error_text, {},
                                             linked_permission_ids());
          !recorded)
      {
        return std::unexpected(std::move(recorded.error()));
      }
      add_output(result, error_text);
      return result;
    }
    auto const output = "wrote " + std::to_string(write->bytes_written) + " bytes to " + write->path.string();
    auto const write_result_json = "{\"tool\":\"write\",\"ok\":true,\"path\":\"" + ava::core::json::escape(write->path.generic_string()) +
                                   "\",\"bytes_written\":" + std::to_string(write->bytes_written) + ",\"diff\":\"" + ava::core::json::escape(write->diff) +
                                   "\",\"diff_truncated\":" + json_bool(write->diff_truncated) + ",\"changed_paths\":[\"" +
                                   ava::core::json::escape(write->path.generic_string()) + "\"]}";
    if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "write", ava::agent::ToolTimelineStatus::Success, output,
                                           write_result_json, linked_permission_ids());
        !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    add_output(result, output);
    return result;
  }

  if (line.starts_with("/bash "))
  {
    auto const command = line.substr(6);
    auto const call_id = ava::core::make_id("cmd");
    if (auto recorded = record_tool_start(unlocked_session, request.event_sink, result, call_id, "bash", "<redacted one-shot command>"); !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    auto const bash =
        ava::tools::run_bash(context, command, ava::tools::BashOptions{.invocation_source = ava::tools::BashOptions::InvocationSource::UserRawShell});
    if (!bash)
    {
      auto const text = bash.error().format();
      if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "bash", ava::agent::ToolTimelineStatus::Error, text, {},
                                             linked_permission_ids());
          !recorded)
      {
        return std::unexpected(std::move(recorded.error()));
      }
      add_output(result, text);
      return result;
    }
    std::string output = "exit: " + std::to_string(bash->exit_code);
    if (bash->timed_out)
      output += " (timed out)";
    if (bash->truncated)
    {
      if (bash->totals_known)
        output += " (output truncated to last " + std::to_string(bash->output_lines) + '/' + std::to_string(bash->total_lines) + " lines";
      else
        output += " (output truncated to " + std::to_string(bash->output_lines) + " retained lines; original total unknown";
      if (bash->byte_limited)
        output += "; byte cap reached";
      output += ")";
    }
    output += '\n' + bash->output;
    std::string bash_result_json = "{\"tool\":\"bash\",\"ok\":" + json_bool(bash->exit_code == 0 && !bash->timed_out && !bash->canceled) +
                                   ",\"exit_code\":" + std::to_string(bash->exit_code) + ",\"timed_out\":" + json_bool(bash->timed_out) +
                                   ",\"canceled\":" + json_bool(bash->canceled) + ",\"truncated\":" + json_bool(bash->truncated) +
                                   ",\"byte_limited\":" + json_bool(bash->byte_limited) + ",\"line_limited\":" + json_bool(bash->line_limited);
    if (bash->totals_known)
      bash_result_json += ",\"total_bytes\":" + std::to_string(bash->total_bytes);
    bash_result_json += ",\"output_bytes\":" + std::to_string(bash->output_bytes);
    if (bash->totals_known)
      bash_result_json += ",\"total_lines\":" + std::to_string(bash->total_lines);
    bash_result_json += ",\"output_lines\":" + std::to_string(bash->output_lines);
    if (bash->totals_known)
      bash_result_json += ",\"omitted_lines\":" + std::to_string(bash->omitted_lines);
    bash_result_json += ",\"output\":\"" + ava::core::json::escape(bash->output) + "\"";
    append_spill_fields(bash_result_json, bash->spill_path, bash->spill_truncated);
    bash_result_json += "}";
    if (auto recorded = record_tool_result(unlocked_session, request.event_sink, result, call_id, "bash",
                                           bash->canceled                             ? ava::agent::ToolTimelineStatus::Canceled
                                           : bash->exit_code == 0 && !bash->timed_out ? ava::agent::ToolTimelineStatus::Success
                                                                                      : ava::agent::ToolTimelineStatus::Error,
                                           "exit " + std::to_string(bash->exit_code), bash_result_json, linked_permission_ids());
        !recorded)
    {
      return std::unexpected(std::move(recorded.error()));
    }
    add_output(result, std::move(output));
    return result;
  }

  result.handled = false;
  return result;
}

}  // namespace ava::app
