#include "sys.h"
#include "protocol.h"
#include "serialization.h"
#include "serialization_detail.h"
#include "serialization_json.h"
#include "ava/app/command_session_support_internal.h"
#include "ava/app/runtime/Session.h"
#include "ava/config/model_config.h"
#include "ava/session/logical_projection.h"
#include "ava/session/session_tree.h"
#include "ava/session/Stats.h"
#include "ava/session/validation.h"
#include "ava/core/json.h"
#include "ava/core/string_utils.h"

#include <cctype>
#include <utility>
#include <vector>

namespace ava::app::rpc {
namespace {

std::string joined_output(std::vector<std::string> const& output)
{
  std::string text;
  for (std::size_t index = 0; index < output.size(); ++index)
  {
    if (index > 0)
      text += '\n';
    text += output[index];
  }
  return text;
}

bool json_bool_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return false;
  auto const value = core::trim(object.substr(*start));
  if (!value.starts_with("true"))
    return false;
  auto const rest = core::trim(value.substr(4));
  return rest.empty() || rest.front() == ',' || rest.front() == '}';
}

std::string sanitized_reasoning_entry_json(ava::session::SessionEntry const& entry)
{
  std::string data = "{";
  bool first = true;
  auto append_string = [&](std::string_view key, std::optional<std::string> const& value) {
    if (!value || value->empty())
      return;
    if (!first)
      data += ',';
    first = false;
    data += string_field_json(key, *value);
  };
  auto append_bool = [&](std::string_view key, bool value) {
    if (!first)
      data += ',';
    first = false;
    data += bool_field_json(key, value);
  };

  append_string("provider", ava::core::json::string_field(entry.data_json, "provider"));
  append_string("model", ava::core::json::string_field(entry.data_json, "model"));
  append_string("format", ava::core::json::string_field(entry.data_json, "format"));
  auto const redacted = json_bool_field(entry.data_json, "redacted");
  if (!redacted)
    append_string("text", ava::core::json::string_field(entry.data_json, "text"));
  append_bool("redacted", redacted);
  auto const omitted = ava::core::json::object_field(entry.data_json, "private_replay_metadata_omitted");
  bool const signature_present = ava::core::json::string_field(entry.data_json, "signature").has_value() || (omitted && json_bool_field(*omitted, "signature"));
  append_bool("signature_present", signature_present);
  data += '}';

  std::string json = "{";
  json += integer_field_json("version", entry.version);
  json += ',';
  json += string_field_json("id", entry.id);
  json += ',';
  json += string_field_json("parent_id", entry.parent_id);
  json += ',';
  json += string_field_json("type", ava::session::to_string(entry.type));
  json += ',';
  json += string_field_json("timestamp", entry.timestamp);
  json += ",\"data\":";
  json += data;
  json += '}';
  return json;
}

// Render a session entry as its v1-compatible JSON object, with optional ordered_output detail.
//
// User messages are always sanitized; assistant messages that carry ordered_output reuse
// the compatibility sanitizer (so the legacy payload is preserved when include_ordered_output
// is false, and the additive ordered block is included only when true); other entry types pass
// their stored data_json through unchanged.
std::string session_entry_json(ava::session::SessionEntry const& entry, bool include_ordered_output)
{
  auto const data_json =
      entry.type == ava::session::EntryType::UserMessage
          ? ava::session::sanitized_message_data_json(entry.data_json, true)
          : (entry.type == ava::session::EntryType::AssistantMessage && ava::core::json::field_value_start(entry.data_json, "ordered_output")
                 ? ava::session::sanitized_compatibility_assistant_message_data_json(entry.data_json, include_ordered_output)
                 : (entry.type == ava::session::EntryType::AssistantMessage ? ava::session::sanitized_message_data_json(entry.data_json, false)
                                                                            : entry.data_json));
  std::string json = "{";
  json += integer_field_json("version", entry.version);
  json += ',';
  json += string_field_json("id", entry.id);
  json += ',';
  json += string_field_json("parent_id", entry.parent_id);
  json += ',';
  json += string_field_json("type", ava::session::to_string(entry.type));
  json += ',';
  json += string_field_json("timestamp", entry.timestamp);
  json += ",\"data\":";
  json += data_json;
  json += '}';
  return json;
}

// Render a session entry, collapsing it to a metadata-only stub once it exceeds the per-entry cap.
//
// Reasoning blocks use their sanitized form; every other entry type uses session_entry_json.
// Anything larger than 8192 bytes is replaced by a truncated envelope that records the original
// size, so the caller can still count the entry without paying its byte cost in the response.
std::string capped_session_entry_json(ava::session::SessionEntry const& entry, bool include_ordered_output)
{
  auto json = entry.type == ava::session::EntryType::ReasoningBlock ? sanitized_reasoning_entry_json(entry) : session_entry_json(entry, include_ordered_output);
  if (json.size() <= 8192)
    return json;
  std::string capped = "{";
  capped += integer_field_json("version", entry.version);
  capped += ',';
  capped += string_field_json("id", entry.id);
  capped += ',';
  capped += string_field_json("parent_id", entry.parent_id);
  capped += ',';
  capped += string_field_json("type", ava::session::to_string(entry.type));
  capped += ',';
  capped += string_field_json("timestamp", entry.timestamp);
  capped += ",";
  capped += bool_field_json("truncated", true);
  capped += ',';
  capped += number_field_json("original_bytes", json.size());
  capped += '}';
  return capped;
}

std::string_view utf8_prefix_within(std::string_view value, std::size_t max_bytes)
{
  if (value.size() <= max_bytes)
    return value;

  std::size_t index = 0;
  std::size_t end = 0;
  while (index < value.size() && index < max_bytes)
  {
    auto const byte = static_cast<unsigned char>(value[index]);
    std::size_t length = 0;
    if (byte < 0x80)
    {
      length = 1;
    }
    else if ((byte & 0xE0U) == 0xC0U)
    {
      length = 2;
    }
    else if ((byte & 0xF0U) == 0xE0U)
    {
      length = 3;
    }
    else if ((byte & 0xF8U) == 0xF0U)
    {
      length = 4;
    }
    else
    {
      break;
    }
    if (index + length > max_bytes || index + length > value.size())
      break;
    bool valid_continuation = true;
    for (std::size_t offset = 1; offset < length; ++offset)
    {
      auto const continuation = static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xC0U) != 0x80U)
      {
        valid_continuation = false;
        break;
      }
    }
    if (!valid_continuation)
      break;
    end = index + length;
    index += length;
  }
  return value.substr(0, end);
}

std::string question_options_json(std::vector<ava::agent::QuestionOption> const& options)
{
  std::string json = "[";
  for (std::size_t index = 0; index < options.size(); ++index)
  {
    if (index > 0)
      json += ',';
    json += '{';
    json += string_field_json("value", options[index].value);
    json += ',';
    json += string_field_json("label", options[index].label);
    json += '}';
  }
  json += ']';
  return json;
}

void append_optional_string(std::string& json, std::string_view key, std::string_view value)
{
  if (value.empty())
    return;
  json += ',';
  json += string_field_json(key, value);
}

void append_optional_bool(std::string& json, std::string_view key, bool value)
{
  if (!value)
    return;
  json += ',';
  json += bool_field_json(key, value);
}

void append_optional_number(std::string& json, std::string_view key, std::optional<std::size_t> const& value)
{
  if (!value)
    return;
  json += ',';
  json += number_field_json(key, *value);
}

void append_optional_json_object(std::string& json, std::string_view key, std::string_view value)
{
  if (value.empty())
    return;
  json += ",\"";
  json += key;
  if (ava::core::json::is_valid_object(value))
  {
    json += "\":";
    json += value;
    return;
  }
  json += "_json\":\"";
  json += ava::core::json::escape(value);
  json += '"';
}

std::string tool_timeline_entry_json(ava::agent::ToolTimelineEntry const& entry)
{
  std::string json = "{";
  json += string_field_json("status", ava::agent::to_string(entry.status));
  json += ',';
  json += string_field_json("call_id", entry.call_id);
  json += ',';
  json += string_field_json("tool", entry.name);
  append_optional_string(json, "text", entry.status == ava::agent::ToolTimelineStatus::Running ? entry.argument_summary : entry.result_summary);
  append_optional_string(json, "argument_summary", entry.argument_summary);
  append_optional_string(json, "result_summary", entry.result_summary);
  append_optional_json_object(json, "args", entry.arguments_json);
  append_optional_json_object(json, "result", entry.result_json);
  append_optional_json_object(json, "structured_result", entry.structured_result_json);
  append_optional_string(json, "content_type", entry.content_type);
  append_optional_string(json, "category", entry.error_category);
  append_optional_string(json, "error_code", entry.error_code);
  append_optional_string(json, "message", entry.error_message);
  append_optional_string(json, "details", entry.error_details);
  append_optional_string(json, "diff", entry.diff);
  if (!entry.changed_paths.empty())
  {
    json += ",\"changed_paths\":";
    json += string_array_json(entry.changed_paths);
  }
  if (!entry.permission_request_ids.empty())
  {
    json += ",\"permission_request_ids\":";
    json += string_array_json(entry.permission_request_ids);
  }
  append_optional_string(json, "spill_path", entry.spill_path);
  append_optional_bool(json, "diff_truncated", entry.diff_truncated);
  append_optional_bool(json, "truncated", entry.truncated);
  append_optional_bool(json, "byte_limited", entry.byte_limited);
  append_optional_bool(json, "line_limited", entry.line_limited);
  append_optional_bool(json, "spill_truncated", entry.spill_truncated);
  append_optional_number(json, "output_bytes", entry.output_bytes);
  append_optional_number(json, "total_bytes", entry.total_bytes);
  append_optional_number(json, "output_lines", entry.output_lines);
  append_optional_number(json, "total_lines", entry.total_lines);
  append_optional_number(json, "start_line", entry.start_line);
  append_optional_number(json, "end_line", entry.end_line);
  append_optional_number(json, "next_offset_line", entry.next_offset_line);
  append_optional_number(json, "omitted_bytes", entry.omitted_bytes);
  append_optional_number(json, "omitted_lines", entry.omitted_lines);
  append_optional_number(json, "visible_matches", entry.visible_matches);
  append_optional_number(json, "total_matches", entry.total_matches);
  json += '}';
  return json;
}

std::string tool_timeline_json(std::vector<ava::agent::ToolTimelineEntry> const& entries)
{
  std::string json = "[";
  for (std::size_t index = 0; index < entries.size(); ++index)
  {
    if (index > 0)
      json += ',';
    json += tool_timeline_entry_json(entries[index]);
  }
  json += ']';
  return json;
}

}  // namespace

std::string command_result_json(CommandResult const& result)
{
  std::string json = "{";
  json += bool_field_json("handled", result.handled);
  json += ',';
  json += bool_field_json("quit", result.quit);
  if (result.prompt_message)
  {
    json += ',';
    json += bool_field_json("prompt", true);
    json += ',';
    json += string_field_json("prompt_command", result.prompt_command);
    json += ',';
    json += string_field_json("prompt_source", result.prompt_source);
  }
  json += ",\"output\":";
  json += output_array_json(result.output);
  json += ',';
  json += string_field_json("text", joined_output(result.output));
  if (!result.tool_timeline.empty())
  {
    json += ",\"tool_timeline\":";
    json += tool_timeline_json(result.tool_timeline);
  }
  json += '}';
  return json;
}

std::string command_registry_result_json(CommandRegistry const& registry)
{
  std::string json = "{\"commands\":[";
  for (std::size_t index = 0; index < registry.entries.size(); ++index)
  {
    auto const& entry = registry.entries[index];
    if (index > 0)
      json += ',';
    json += '{';
    json += string_field_json("command", entry.command);
    json += ",\"aliases\":";
    json += string_array_json(entry.aliases);
    json += ',';
    json += string_field_json("description", entry.description);
    json += ',';
    json += string_field_json("hint", entry.hint);
    json += ',';
    json += string_field_json("category", entry.category);
    json += ',';
    json += bool_field_json("enabled", entry.enabled);
    json += ',';
    json += string_field_json("disabled_reason", entry.disabled_reason);
    json += ',';
    json += string_field_json("source", ava::app::to_string(entry.source));
    json += ',';
    json += string_field_json("kind", ava::app::to_string(entry.kind));
    json += ',';
    json += string_field_json("source_id", entry.source_id);
    json += ',';
    json += string_field_json("source_path", entry.source_path.string());
    json += ',';
    json += string_field_json("source_scope", entry.source_scope);
    if (!entry.skill_name.empty())
    {
      json += ',';
      json += string_field_json("skill_name", entry.skill_name);
    }
    if (!entry.mcp_server_id.empty())
    {
      json += ',';
      json += string_field_json("mcp_server_id", entry.mcp_server_id);
      json += ',';
      json += string_field_json("mcp_prompt_name", entry.mcp_prompt_name);
      json += ",\"mcp_arguments\":[";
      for (std::size_t argument_index = 0; argument_index < entry.mcp_arguments.size(); ++argument_index)
      {
        auto const& argument = entry.mcp_arguments[argument_index];
        if (argument_index > 0)
          json += ',';
        json += '{';
        json += string_field_json("name", argument.name);
        json += ',';
        json += string_field_json("description", argument.description);
        json += ',';
        json += bool_field_json("required", argument.required);
        json += '}';
      }
      json += ']';
    }
    if (!entry.plugin_id.empty())
    {
      json += ',';
      json += string_field_json("plugin_id", entry.plugin_id);
      json += ',';
      json += string_field_json("plugin_command_name", entry.plugin_command_name);
    }
    json += '}';
  }
  json += "],\"diagnostics\":[";
  for (std::size_t index = 0; index < registry.diagnostics.size(); ++index)
  {
    auto const& diagnostic = registry.diagnostics[index];
    if (index > 0)
      json += ',';
    json += '{';
    json += string_field_json("command", diagnostic.command);
    json += ',';
    json += string_field_json("source", diagnostic.source);
    json += ',';
    json += string_field_json("source_id", diagnostic.source_id);
    json += ',';
    json += string_field_json("path", diagnostic.path.string());
    json += ',';
    json += string_field_json("message", diagnostic.message);
    json += ',';
    json += string_field_json("winner_source", diagnostic.winner_source);
    json += ',';
    json += string_field_json("winner_source_id", diagnostic.winner_source_id);
    json += ',';
    json += string_field_json("winner_path", diagnostic.winner_path.string());
    json += '}';
  }
  json += "]}";
  return json;
}

std::string permission_request_payload_json(std::string_view resolver_request_id, ava::permissions::PermissionPrompt const& prompt)
{
  std::string json = "{";
  json += string_field_json("resolver_request_id", resolver_request_id);
  if (!prompt.permission_request_id.empty())
  {
    json += ',';
    json += string_field_json("permission_request_id", prompt.permission_request_id);
  }
  json += ',';
  json += string_field_json("operation", ava::permissions::to_string(prompt.operation));
  json += ',';
  json += string_field_json("mode", ava::agent::to_string(prompt.mode));
  json += ',';
  json += string_field_json("target_path", prompt.target_path.string());
  json += ',';
  json += string_field_json("command", prompt.command);
  json += ',';
  json += string_field_json("tool_name", prompt.tool_name);
  json += ',';
  json += string_field_json("reason", prompt.reason);
  json += ',';
  json += string_field_json("risk", ava::permissions::to_string(prompt.risk));
  if (prompt.command_metadata)
  {
    auto const& metadata = *prompt.command_metadata;
    json += ",\"command_metadata\":{";
    json += string_field_json("level", ava::command::to_string(metadata.level));
    json += ',';
    json += string_field_json("family", ava::command::to_string(metadata.family));
    json += ',';
    json += string_field_json("fingerprint", metadata.fingerprint);
    json += ',';
    json += string_field_json("execution_domain", ava::command::to_string(metadata.execution_domain));
    json += ',';
    json += string_field_json("resolved_executable", metadata.resolved_executable.string());
    json += ',';
    json += string_field_json("origin", ava::command::to_string(metadata.executable_origin));
    json += ',';
    json += string_field_json("cwd", metadata.cwd.string());
    json += ",\"containment_available\":";
    json += metadata.containment_available ? "true" : "false";
    json += ',';
    json += string_field_json("containment_status", ava::permissions::to_string(metadata.containment_status));
    json += ',';
    json += string_field_json("backend_maximum_scope", ava::command::to_string(metadata.backend_maximum_scope));
    json += ',';
    json += string_field_json("recipe_payload_version", metadata.recipe_payload_version);
    json += ',';
    json += string_field_json("global_recipe_key", metadata.global_recipe_key);
    json += ',';
    json += string_field_json("workspace_recipe_key", metadata.workspace_recipe_key);
    json += ',';
    json += string_field_json("recipe_display", metadata.recipe_display);
    std::vector<std::string> allowed_scopes;
    allowed_scopes.reserve(metadata.effective_allowed_scopes.size());
    for (auto const scope : metadata.effective_allowed_scopes) allowed_scopes.emplace_back(ava::command::to_string(scope));
    json += ",\"effective_allowed_scopes\":" + string_array_json(allowed_scopes);
    json += ',';
    json += string_field_json("containment_profile_id", metadata.containment_profile_id);
    json += ',';
    json += bool_field_json("containment_network_allowed", metadata.containment_network_allowed);
    json += ',';
    json += string_field_json("environment_profile_id", metadata.environment_profile_id);
    json += ',';
    json += string_field_json("environment_digest", metadata.environment_digest);
    json += ",\"executor_identity_verified\":";
    json += metadata.executor_identity_verified ? "true" : "false";
    json += '}';
  }
  if (!prompt.diff_preview.empty())
  {
    json += ',';
    json += string_field_json("diff_preview", prompt.diff_preview);
    json += ',';
    json += bool_field_json("diff_truncated", prompt.diff_truncated);
  }
  json += '}';
  return json;
}

std::string question_request_payload_json(std::string_view resolver_request_id, ava::agent::QuestionPrompt const& prompt)
{
  std::string json = "{";
  json += string_field_json("resolver_request_id", resolver_request_id);
  json += ',';
  json += string_field_json("header", prompt.header);
  json += ',';
  json += string_field_json("question", prompt.question);
  json += ",\"options\":";
  json += question_options_json(prompt.options);
  json += ',';
  json += bool_field_json("multiple", prompt.multiple);
  json += ',';
  json += bool_field_json("allow_custom", prompt.allow_custom);
  json += ',';
  json += bool_field_json("secret", prompt.secret);
  json += ',';
  json += bool_field_json("modal", prompt.modal);
  json += ',';
  json += bool_field_json("searchable", prompt.searchable);
  json += '}';
  return json;
}

std::string permission_reply_payload_json(std::string_view resolver_request_id, std::string_view decision, std::optional<std::string> const& reason)
{
  std::string json = "{";
  json += string_field_json("resolver_request_id", resolver_request_id);
  json += ',';
  json += string_field_json("decision", decision);
  if (reason && !reason->empty())
  {
    json += ',';
    json += string_field_json("reason", *reason);
  }
  json += '}';
  return json;
}

std::string question_reply_payload_json(std::string_view resolver_request_id, std::optional<std::string> const& answer,
                                        std::optional<std::string> const& selected, std::optional<std::vector<std::string>> const& selected_options)
{
  std::string json = "{";
  json += string_field_json("resolver_request_id", resolver_request_id);
  if (answer)
  {
    json += ',';
    json += string_field_json("answer", *answer);
  }
  if (selected)
  {
    json += ',';
    json += string_field_json("selected", *selected);
  }
  if (selected_options)
  {
    json += ",\"selected_options\":";
    json += string_array_json(*selected_options);
  }
  json += '}';
  return json;
}

std::string cancel_requested_payload_json(bool active_run, std::size_t cleared_steer, std::size_t cleared_follow_up, std::string_view active_request_id)
{
  std::string json = "{";
  json += bool_field_json("active_run", active_run);
  json += ',';
  json += number_field_json("cleared_steer", cleared_steer);
  json += ',';
  json += number_field_json("cleared_follow_up", cleared_follow_up);
  if (!active_request_id.empty())
  {
    json += ',';
    json += string_field_json("active_request_id", active_request_id);
  }
  json += '}';
  return json;
}

std::string queued_message_payload_json(std::string_view message, std::string_view reason)
{
  bool const truncated = message.size() > kMaxRpcQueueEventMessageBytes;
  auto const event_message = truncated ? utf8_prefix_within(message, kMaxRpcQueueEventMessageBytes) : message;
  std::string json = "{";
  json += string_field_json("message", event_message);
  if (truncated)
  {
    json += ',';
    json += bool_field_json("message_truncated", true);
    json += ',';
    json += number_field_json("message_bytes", message.size());
  }
  if (!reason.empty())
  {
    json += ',';
    json += string_field_json("reason", reason);
  }
  json += '}';
  return json;
}

std::string prompt_result_json(std::string_view session_id, ava::agent::AgentLoopResult const& result)
{
  std::string json = "{";
  json += string_field_json("session_id", session_id);
  json += ',';
  json += string_field_json("final_text", result.final_text);
  json += ',';
  json += string_field_json("stop_reason", ava::core::to_string(result.outcome));
  json += ',';
  json += number_field_json("provider_iterations", result.provider_iterations);
  json += ',';
  json += number_field_json("tool_calls", result.tool_calls);
  if (!result.tool_timeline.empty())
  {
    json += ",\"tool_timeline\":";
    json += tool_timeline_json(result.tool_timeline);
  }
  json += '}';
  return json;
}

std::string context_sources_json_value(std::vector<runtime::ContextSourceMetadata> const& context_sources)
{
  std::string json = "[";
  for (std::size_t index = 0; index < context_sources.size(); ++index)
  {
    auto const& source = context_sources[index];
    if (index > 0)
      json += ',';
    json += '{';
    json += string_field_json("path", source.path.string());
    json += ',';
    json += string_field_json("source_type", ava::context::to_string(source.source_type));
    json += ',';
    json += number_field_json("byte_count", source.byte_count);
    json += '}';
  }
  json += ']';
  return json;
}

std::string context_sources_json(runtime::session_ts const& unlocked_session)
{
  auto const context_sources = runtime::session_ts::crat(unlocked_session)->context_sources();
  return context_sources_json_value(context_sources);
}

namespace detail {

MessagesResultSerializer::MessagesResultSerializer(utils::Badge<runtime::Session>, runtime::Session const& session) // `runtime::Session const&` is allowed here.
    : session_(session), prefix_("{" + string_field_json("session_id", session.store.session_id()) + ",\"messages\":[")
{
}

ava::core::Result<std::string> MessagesResultSerializer::run() &&
{
  if (auto loaded = load_and_project(); !loaded)
    return std::unexpected(std::move(loaded.error()));
  collect_candidates();
  select_legacy_messages();
  refill_ordered_output();
  return assemble_json();
}

// Load the raw session entries and project them into the logical view that the
// messages result reports. Both steps can fail on a corrupted or unreadable
// store; the projected vector is retained as a member because candidate entries
// hold pointers into it for the rest of the pipeline.
ava::core::VoidResult MessagesResultSerializer::load_and_project()
{
  auto read_authority = session_.read_authority_1();
  if (!read_authority)
    return std::unexpected(std::move(read_authority.error()));
  auto entries = read_authority->load();
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  auto projected = ava::session::project_logical_session_history(*entries);
  if (!projected)
    return std::unexpected(std::move(projected.error()));
  projected_ = std::move(*projected);
  return {};
}

// Build one MessageCandidate per retained entry type, pre-rendered with the
// capped legacy form. Ordered output presence is detected up front so the
// refill phase knows which entries could be upgraded later.
void MessagesResultSerializer::collect_candidates()
{
  candidates_.reserve(projected_.size());
  for (auto const& entry : projected_)
  {
    if (ava::session::is_internal_replay_user_message(entry))
      continue;
    if (entry.type != ava::session::EntryType::UserMessage && entry.type != ava::session::EntryType::AssistantMessage &&
        entry.type != ava::session::EntryType::ReasoningBlock && entry.type != ava::session::EntryType::ToolCall &&
        entry.type != ava::session::EntryType::ToolResult)
    {
      continue;
    }
    // First make the exact v1-compatible representation. Ordered output is
    // strictly additive and must never determine the retained message set.
    candidates_.push_back(MessageCandidate{.entry = &entry,
                                           .legacy_json = capped_session_entry_json(entry, false),
                                           .has_ordered_output = entry.type == ava::session::EntryType::AssistantMessage &&
                                                                 ava::core::json::field_value_start(entry.data_json, "ordered_output").has_value()});
  }
}

// Greedily fit legacy-rendered candidates into the entry and byte budgets.
// Truncation and the running ordered-output omission count are tested
// hypothetically (the values that would hold if this candidate were committed)
// so the reserve for the additive marker/count can never evict a legacy payload.
void MessagesResultSerializer::select_legacy_messages()
{
  selected_.reserve(std::min(candidates_.size(), kMaxRpcMessagesEntries));
  rendered_.reserve(std::min(candidates_.size(), kMaxRpcMessagesEntries));
  std::size_t potential_ordered_output_omissions = 0;
  for (std::size_t index = 0; index < candidates_.size(); ++index)
  {
    if (selected_.size() >= kMaxRpcMessagesEntries)
    {
      truncated_ = true;
      break;
    }
    auto const candidate_truncated = index + 1 < candidates_.size();
    auto const candidate_ordered_output_omissions = potential_ordered_output_omissions + (candidates_[index].has_ordered_output ? 1U : 0U);
    rendered_.push_back(candidates_[index].legacy_json);
    // Reserve the bounded additive marker/count while making the legacy-only
    // selection. This prevents ordered detail from removing legacy payload.
    if (rendered_size_for(candidate_truncated, candidate_ordered_output_omissions) > kMaxRpcMessagesResponseBytes)
    {
      rendered_.pop_back();
      truncated_ = true;
      break;
    }
    potential_ordered_output_omissions = candidate_ordered_output_omissions;
    selected_.push_back(&candidates_[index]);
  }
  ordered_output_omitted_count_ = potential_ordered_output_omissions;
}

// Try to upgrade each selected ordered-output entry to its detailed form.
// A detailed entry is rejected if it exceeds the legacy 8 KiB allowance or no
// longer carries ordered_output; otherwise the upgrade is committed only when
// the response still fits with one fewer ordered-output omission.
void MessagesResultSerializer::refill_ordered_output()
{
  for (std::size_t index = 0; index < selected_.size(); ++index)
  {
    auto const* candidate = selected_[index];
    if (!candidate->has_ordered_output)
      continue;
    auto detailed_json = session_entry_json(*candidate->entry, true);
    // A detailed entry may never consume the legacy entry's 8 KiB allowance:
    // otherwise a 5--8 KiB v1 assistant message becomes metadata-only.
    if (detailed_json.size() > 8192 || detailed_json.find("\"ordered_output\"") == std::string::npos)
      continue;
    auto const saved = std::move(rendered_[index]);
    rendered_[index] = std::move(detailed_json);
    if (rendered_size_for(truncated_, ordered_output_omitted_count_ - 1) <= kMaxRpcMessagesResponseBytes)
    {
      --ordered_output_omitted_count_;
      continue;
    }
    rendered_[index] = std::move(saved);
  }
}

std::string MessagesResultSerializer::assemble_json() const
{
  std::string json = prefix_;
  for (std::size_t index = 0; index < rendered_.size(); ++index)
  {
    if (index > 0)
      json += ',';
    json += rendered_[index];
  }
  json += tail_json_for(truncated_, rendered_.size(), ordered_output_omitted_count_);
  return json;
}

std::size_t MessagesResultSerializer::rendered_size_for(bool truncated, std::size_t ordered_output_omitted_count) const
{
  std::size_t size = prefix_.size() + tail_json_for(truncated, rendered_.size(), ordered_output_omitted_count).size();
  for (std::size_t index = 0; index < rendered_.size(); ++index) size += rendered_[index].size() + (index == 0 ? 0U : 1U);
  return size;
}

std::string MessagesResultSerializer::tail_json_for(bool truncated, std::size_t message_count, std::size_t ordered_output_omitted_count) const
{
  std::string tail = "]," + bool_field_json("truncated", truncated) + "," + number_field_json("message_count", message_count) + "," +
                     bool_field_json("ordered_output_truncated", ordered_output_omitted_count != 0) + "," +
                     number_field_json("ordered_output_omitted_count", ordered_output_omitted_count);
  tail += '}';
  return tail;
}

SessionResultSerializer::SessionResultSerializer(utils::Badge<runtime::Session>,
    runtime::Session const& session) : session_(session) // `runtime::Session const&` is allowed here.
{
}

std::string SessionResultSerializer::state_result_json(bool cancel_requested) const
{
  runtime::ModelSelection_ts::crat model_selection_r = session_.model_selection_r();
  std::string json = "{";
  json += "\"protocol_version\":";
  json += std::to_string(kRpcProtocolVersion);
  json += ',';
  json += string_field_json("session_id", session_.store.session_id());
  json += ',';
  json += string_field_json("session_path", session_.store.session_path().string());
  json += ',';
  json += string_field_json("mode", ava::agent::to_string(session_.mode()));
  json += ',';
  json += string_field_json("provider", model_selection_r->model.provider_id);
  json += ',';
  json += string_field_json("model", model_selection_r->model.model_id);
  json += ',';
  json += string_field_json("workspace_dir", session_.workspace_dir().string());
  json += ',';
  json += string_field_json("current_dir", session_.current_dir().string());
  json += ',';
  json += bool_field_json("created", session_.created);
  json += ',';
  json += bool_field_json("sessionless", session_.sessionless());
  json += ',';
  json += bool_field_json("cancel_requested", cancel_requested);
  json += ',';
  json += bool_field_json("reasoning_enabled", model_selection_r->reasoning.has_value());
  if (model_selection_r->reasoning)
  {
    json += ',';
    json += string_field_json("reasoning_level", model_selection_r->reasoning->level);
    if (model_selection_r->reasoning->provider_level && *model_selection_r->reasoning->provider_level != model_selection_r->reasoning->level)
    {
      json += ',';
      json += string_field_json("reasoning_provider_level", *model_selection_r->reasoning->provider_level);
    }
    if (model_selection_r->reasoning->budget_tokens)
    {
      json += ',';
      json += integer_field_json("reasoning_budget_tokens", *model_selection_r->reasoning->budget_tokens);
    }
    if (!model_selection_r->reasoning->display.empty())
    {
      json += ',';
      json += string_field_json("reasoning_display", model_selection_r->reasoning->display);
    }
  }
  json += ',';
  json += number_field_json("context_source_count", session_.context_sources().size());
  json += ",\"context_sources\":";
  json += context_sources_json_value(session_.context_sources());
  json += '}';
  return json;
}

ava::core::Result<std::string> SessionResultSerializer::list_sessions_result_json() const
{
  auto sessions = ava::session::SessionStore::list_sessions(session_.workspace_dir(), session_.paths().sessions_dir);
  if (!sessions)
    return std::unexpected(sessions.error());
  std::string json = "{\"sessions\":[";
  for (std::size_t index = 0; index < sessions->size(); ++index)
  {
    auto const& summary = (*sessions)[index];
    if (index > 0)
      json += ',';
    json += '{';
    json += string_field_json("session_id", summary.session_id);
    json += ',';
    json += string_field_json("path", summary.path.string());
    json += ',';
    json += string_field_json("last_updated", summary.last_updated);
    json += ',';
    json += number_field_json("entry_count", summary.entry_count);
    json += ',';
    json += string_field_json("title", summary.title);
    json += '}';
  }
  json += "]}";
  return json;
}

ava::core::Result<std::string> SessionResultSerializer::session_tree_result_json() const
{
  auto tree = ava::session::build_session_tree(session_.workspace_dir(), session_.paths().sessions_dir, session_.store.session_id());
  if (!tree)
    return std::unexpected(std::move(tree.error()));
  std::string json = "{";
  json += string_field_json("current_session_id", tree->current_session_id);
  json += ",\"roots\":" + string_array_json(tree->roots);
  json += ",\"leaves\":" + string_array_json(tree->leaves);
  json += ",\"path\":" + string_array_json(tree->current_path);
  json += ",\"sessions\":[";
  for (std::size_t index = 0; index < tree->sessions.size(); ++index)
  {
    if (index > 0)
      json += ',';
    auto const& node = tree->sessions[index];
    json += '{';
    json += string_field_json("session_id", node.summary.session_id);
    json += ',';
    json += string_field_json("path", node.summary.path.string());
    json += ',';
    json += string_field_json("last_updated", node.summary.last_updated);
    json += ',';
    json += number_field_json("entry_count", node.summary.entry_count);
    json += ',';
    json += string_field_json("name", node.metadata.name);
    json += ',';
    json += string_field_json("title", node.metadata.effective_title());
    json += ',';
    json += string_field_json("generated_title", node.metadata.generated_title);
    json += ',';
    json += bool_field_json("has_manual_name", node.metadata.has_manual_name);
    json += ",\"labels\":" + string_array_json(node.metadata.labels);
    json += ',';
    json += string_field_json("labels_updated", node.metadata.labels_updated);
    json += ',';
    json += bool_field_json("archived", node.metadata.archived);
    json += ',';
    json += string_field_json("parent_session_id", node.metadata.parent_session_id);
    json += ',';
    json += string_field_json("source_session_id", node.metadata.source_session_id);
    json += ',';
    json += string_field_json("branch_from_entry_id", node.metadata.branch_from_entry_id);
    json += ',';
    json += string_field_json("branch_origin", node.metadata.branch_origin);
    json += ',';
    json += string_field_json("actor", node.metadata.actor);
    json += ",\"children\":" + string_array_json(node.children);
    json += ',';
    json += bool_field_json("leaf", node.children.empty());
    json += ',';
    json += bool_field_json("current", node.current);
    json += '}';
  }
  json += "]}";
  return json;
}

ava::core::Result<std::string> SessionResultSerializer::list_models_result_json() const
{
  auto registry = ava::config::load_model_registry(session_.paths());
  if (!registry)
    return std::unexpected(std::move(registry.error()));

  ava::config::ModelInfo const current_model = session_.model_selection_r()->model;
  auto models = ava::config::effective_models(*registry);
  bool current_in_catalog = false;
  for (auto const& model : models)
  {
    current_in_catalog = current_in_catalog || (model.provider_id == current_model.provider_id && model.model_id == current_model.model_id);
  }
  std::string json = "{";
  json += string_field_json("default_provider", registry->default_provider_id);
  json += ',';
  json += string_field_json("default_model", registry->default_model_id);
  json += ',';
  json += string_field_json("current_provider", current_model.provider_id);
  json += ',';
  json += string_field_json("current_model", current_model.model_id);
  json += ",\"models\":[";
  for (std::size_t index = 0; index < models.size(); ++index)
  {
    if (index > 0)
      json += ',';
    json += model_info_json(models[index], true);
  }
  if (!current_in_catalog)
  {
    if (!models.empty())
      json += ',';
    json += model_info_json(current_model, false);
  }
  json += "]}";
  return json;
}

ava::core::Result<std::string> SessionResultSerializer::session_stats_result_json() const
{
  auto read_authority = session_.read_authority_1();
  if (!read_authority)
    return std::unexpected(read_authority.error());
  auto entries = read_authority->load();
  if (!entries)
    return std::unexpected(entries.error());
  auto stats_result = ava::session::compute_session_stats(*entries);
  if (!stats_result)
    return std::unexpected(std::move(stats_result.error()));
  auto const& stats = *stats_result;

  std::string json = "{";
  json += string_field_json("session_id", session_.store.session_id());
  json += ',';
  json += string_field_json("session_path", session_.store.session_path().string());
  json += ',';
  json += number_field_json("entry_count", stats.entry_count);
  json += ',';
  json += string_field_json("first_timestamp", stats.first_timestamp);
  json += ',';
  json += string_field_json("last_timestamp", stats.last_timestamp);
  if (stats.input_tokens)
  {
    json += ',';
    json += integer_field_json("input_tokens", *stats.input_tokens);
  }
  if (stats.output_tokens)
  {
    json += ',';
    json += integer_field_json("output_tokens", *stats.output_tokens);
  }
  if (stats.reasoning_tokens)
  {
    json += ',';
    json += integer_field_json("reasoning_tokens", *stats.reasoning_tokens);
  }
  if (stats.cache_read_tokens)
  {
    json += ',';
    json += integer_field_json("cache_read_tokens", *stats.cache_read_tokens);
  }
  if (stats.cache_write_tokens)
  {
    json += ',';
    json += integer_field_json("cache_write_tokens", *stats.cache_write_tokens);
  }
  if (stats.total_tokens)
  {
    json += ',';
    json += integer_field_json("total_tokens", *stats.total_tokens);
  }
  if (stats.estimated_input_bytes)
  {
    json += ',';
    json += integer_field_json("estimated_input_bytes", *stats.estimated_input_bytes);
  }
  if (stats.estimated_output_bytes)
  {
    json += ',';
    json += integer_field_json("estimated_output_bytes", *stats.estimated_output_bytes);
  }
  if (stats.estimated_total_bytes)
  {
    json += ',';
    json += integer_field_json("estimated_total_bytes", *stats.estimated_total_bytes);
  }
  if (stats.total_cost_usd)
  {
    json += ',';
    json += decimal_field_json("total_cost_usd", *stats.total_cost_usd);
  }
  if (stats.known_cost_usd)
  {
    json += ',';
    json += decimal_field_json("known_cost_usd", *stats.known_cost_usd);
  }
  json += ',';
  json += bool_field_json("cost_complete", stats.cost_complete);
  json += ',';
  json += number_field_json("unknown_cost_entries", stats.unknown_cost_entries);
  json += ',';
  json += number_field_json("exact_usage_entries", stats.exact_usage_entries);
  json += ',';
  json += number_field_json("estimated_usage_entries", stats.estimated_usage_entries);
  json += ",\"counts\":{";
  json += number_field_json("session_start", stats.counts.session_start);
  json += ',';
  json += number_field_json("session_metadata", stats.counts.session_metadata);
  json += ',';
  json += number_field_json("user_message", stats.counts.user_message);
  json += ',';
  json += number_field_json("assistant_message", stats.counts.assistant_message);
  json += ',';
  json += number_field_json("tool_call", stats.counts.tool_call);
  json += ',';
  json += number_field_json("tool_result", stats.counts.tool_result);
  json += ',';
  json += number_field_json("permission_decision", stats.counts.permission_decision);
  json += ',';
  json += number_field_json("mode_change", stats.counts.mode_change);
  json += ',';
  json += number_field_json("model_change", stats.counts.model_change);
  json += ',';
  json += number_field_json("reasoning_block", stats.counts.reasoning_block);
  json += ',';
  json += number_field_json("reasoning_change", stats.counts.reasoning_change);
  json += ',';
  json += number_field_json("compaction", stats.counts.compaction);
  json += ',';
  json += number_field_json("branch_summary", stats.counts.branch_summary);
  json += ',';
  json += number_field_json("error", stats.counts.error);
  json += ',';
  json += number_field_json("cancel", stats.counts.cancel);
  json += "}}";
  return json;
}

ava::core::Result<std::string> SessionResultSerializer::session_validation_result_json() const
{
  auto read_authority = session_.read_authority_1();
  if (!read_authority)
    return std::unexpected(read_authority.error());
  auto entries = read_authority->load();
  if (!entries)
    return std::unexpected(entries.error());
  auto const validation = ava::session::validate_session_replay(*entries);

  std::string json = "{";
  json += string_field_json("session_id", session_.store.session_id());
  json += ',';
  json += string_field_json("session_path", session_.store.session_path().string());
  json += ',';
  json += bool_field_json("ok", validation.ok());
  json += ',';
  json += number_field_json("error_count", validation.error_count);
  json += ',';
  json += number_field_json("warning_count", validation.warning_count);
  json += ",\"issues\":[";
  for (std::size_t index = 0; index < validation.issues.size(); ++index)
  {
    auto const& issue = validation.issues[index];
    if (index > 0)
      json += ',';
    json += '{';
    json += string_field_json("severity", ava::session::to_string(issue.severity));
    json += ',';
    json += string_field_json("kind", ava::session::to_string(issue.kind));
    json += ',';
    json += number_field_json("entry_index", issue.entry_index);
    if (!issue.entry_id.empty())
    {
      json += ',';
      json += string_field_json("entry_id", issue.entry_id);
    }
    if (!issue.call_id.empty())
    {
      json += ',';
      json += string_field_json("call_id", issue.call_id);
    }
    if (!issue.message.empty())
    {
      json += ',';
      json += string_field_json("message", issue.message);
    }
    json += '}';
  }
  json += "]}";
  return json;
}

}  // namespace detail

}  // namespace ava::app::rpc
