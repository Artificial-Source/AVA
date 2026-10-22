#include "sys.h"
#include "ava/agent/tool_dispatch_common.h"
#include "ava/agent/tool_dispatch_search.h"
#include "ava/tools/search_tools.h"
#include "ava/core/json.h"

namespace ava::agent {

ToolDispatchResult glob_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto pattern = tool_dispatch::required_safe_string_arg(call.arguments_json, "pattern", call.name);
  if (!pattern)
    return tool_dispatch::tool_error_result(call, pattern.error());
  if (auto no_ignore_allowed = tool_dispatch::reject_provider_no_ignore(call.arguments_json, call.name); !no_ignore_allowed)
  {
    return tool_dispatch::tool_error_result(call, no_ignore_allowed.error());
  }
  auto const tool_context = tool_dispatch::context_for_provider_tool(context, call);
  auto result = ava::tools::glob_files(
      tool_context, *pattern, ava::tools::GlobOptions{.max_results = tool_dispatch::optional_size_arg(call.arguments_json, "max_results", 2000, 10000)});
  if (!result)
    return tool_dispatch::tool_error_result(call, result.error());
  std::string text = "{\"tool\":\"glob\",\"ok\":true,\"pattern\":\"" + ava::core::json::escape(*pattern) + "\",\"paths\":[";
  for (std::size_t index = 0; index < result->paths.size(); ++index)
  {
    if (index > 0)
      text += ',';
    text += "\"" + ava::core::json::escape(result->paths[index].generic_string()) + "\"";
  }
  text += "],\"truncated\":" + tool_dispatch::json_bool(result->truncated) + ",\"total_matches\":" + std::to_string(result->total_matches);
  if (result->skipped_paths > 0)
    text += ",\"skipped_paths\":" + std::to_string(result->skipped_paths);
  tool_dispatch::append_spill_fields(text, result->spill_path, result->spill_truncated);
  text += "}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = text};
}

ToolDispatchResult list_directory_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto path_value = ava::core::json::string_field(call.arguments_json, "path");
  if (path_value)
  {
    if (auto safe = tool_dispatch::reject_control_arg(*path_value, "path", call.name); !safe)
    {
      return tool_dispatch::tool_error_result(call, safe.error());
    }
  }
  auto const path = path_value.value_or(".");
  auto const tool_context = tool_dispatch::context_for_provider_tool(context, call);
  auto result = ava::tools::list_directory(
      tool_context, tool_dispatch::workspace_path(context, path),
      ava::tools::ListDirectoryOptions{.max_entries = tool_dispatch::optional_size_arg(call.arguments_json, "max_entries", 500, 5000)});
  if (!result)
    return tool_dispatch::tool_error_result(call, result.error());
  std::string text = "{\"tool\":\"list_directory\",\"ok\":true,\"path\":\"" + ava::core::json::escape(path) + "\",\"entries\":[";
  for (std::size_t index = 0; index < result->entries.size(); ++index)
  {
    auto const& entry = result->entries[index];
    if (index > 0)
      text += ',';
    text += "{\"name\":\"" + ava::core::json::escape(entry.name) + "\",\"type\":\"" + std::string(entry.directory ? "directory" : "file") +
            "\",\"size\":" + std::to_string(entry.size) + "}";
  }
  text += "],\"truncated\":" + tool_dispatch::json_bool(result->truncated) + ",\"total_entries\":" + std::to_string(result->total_entries) + "}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = std::move(text)};
}

ToolDispatchResult grep_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto pattern = tool_dispatch::required_text_arg(call.arguments_json, "pattern", call.name);
  if (!pattern)
    return tool_dispatch::tool_error_result(call, pattern.error());
  auto const include_value = ava::core::json::string_field(call.arguments_json, "include");
  if (include_value)
  {
    if (auto safe = tool_dispatch::reject_control_arg(*include_value, "include", call.name); !safe)
    {
      return tool_dispatch::tool_error_result(call, safe.error());
    }
  }
  auto const include = include_value.value_or("**/*");
  if (auto no_ignore_allowed = tool_dispatch::reject_provider_no_ignore(call.arguments_json, call.name); !no_ignore_allowed)
  {
    return tool_dispatch::tool_error_result(call, no_ignore_allowed.error());
  }
  auto literal = tool_dispatch::optional_bool_arg(call.arguments_json, "literal", true, call.name);
  if (!literal)
    return tool_dispatch::tool_error_result(call, literal.error());
  auto case_insensitive = tool_dispatch::optional_bool_arg(call.arguments_json, "case_insensitive", false, call.name);
  if (!case_insensitive)
    return tool_dispatch::tool_error_result(call, case_insensitive.error());
  auto const tool_context = tool_dispatch::context_for_provider_tool(context, call);
  auto result = ava::tools::grep_files(tool_context, *pattern, include,
                                       ava::tools::GrepOptions{.max_matches = tool_dispatch::optional_size_arg(call.arguments_json, "max_matches", 2000, 10000),
                                                               .literal = *literal,
                                                               .case_insensitive = *case_insensitive});
  if (!result)
    return tool_dispatch::tool_error_result(call, result.error());
  std::string text = "{\"tool\":\"grep\",\"ok\":true,\"pattern\":\"" + ava::core::json::escape(*pattern) + "\",\"include\":\"" +
                     ava::core::json::escape(include) + "\",\"literal\":" + tool_dispatch::json_bool(*literal) +
                     ",\"case_insensitive\":" + tool_dispatch::json_bool(*case_insensitive) + ",\"matches\":[";
  for (std::size_t index = 0; index < result->matches.size(); ++index)
  {
    auto const& match = result->matches[index];
    if (index > 0)
      text += ',';
    text += "{\"path\":\"" + ava::core::json::escape(match.path.generic_string()) + "\",\"line_number\":" + std::to_string(match.line_number) + ",\"line\":\"" +
            ava::core::json::escape(match.line) + "\",\"line_truncated\":" + tool_dispatch::json_bool(match.line_truncated) + "}";
  }
  text += "],\"truncated\":" + tool_dispatch::json_bool(result->truncated) + ",\"total_matches\":" + std::to_string(result->total_matches);
  if (result->skipped_paths > 0)
    text += ",\"skipped_paths\":" + std::to_string(result->skipped_paths);
  tool_dispatch::append_spill_fields(text, result->spill_path, result->spill_truncated);
  text += "}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = text};
}

}  // namespace ava::agent
