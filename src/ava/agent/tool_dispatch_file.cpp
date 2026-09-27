#include "sys.h"
#include "ava/agent/tool_dispatch_common.h"
#include "ava/agent/tool_dispatch_file.h"
#include "ava/tools/file_tools.h"
#include "ava/core/json.h"

namespace ava::agent {

ToolDispatchResult read_file_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto path = tool_dispatch::required_safe_string_arg(call.arguments_json, "path", call.name);
  if (!path)
    return tool_dispatch::tool_error_result(call, path.error());
  auto const tool_context = tool_dispatch::context_for_provider_tool(context, call);
  auto result =
      ava::tools::read_file(tool_context, tool_dispatch::workspace_path(context, *path),
                            ava::tools::ReadOptions{.max_bytes = tool_dispatch::optional_size_arg(call.arguments_json, "max_bytes", 50 * 1024, 512 * 1024),
                                                    .offset_line = tool_dispatch::optional_size_arg(call.arguments_json, "offset", 1, 100000000),
                                                    .max_lines = tool_dispatch::optional_size_arg(call.arguments_json, "limit", 200, 100000)});
  if (!result)
    return tool_dispatch::tool_error_result(call, result.error());
  std::string text = "{\"tool\":\"read_file\",\"ok\":true,\"path\":\"" + ava::core::json::escape(*path) + "\",\"content\":\"" +
                     ava::core::json::escape(result->content) + "\",\"truncated\":" + tool_dispatch::json_bool(result->truncated) +
                     ",\"byte_limited\":" + tool_dispatch::json_bool(result->byte_limited) +
                     ",\"line_limited\":" + tool_dispatch::json_bool(result->line_limited);
  if (result->totals_known)
    text += ",\"total_bytes\":" + std::to_string(result->total_bytes);
  text += ",\"output_bytes\":" + std::to_string(result->output_bytes) + ",\"output_lines\":" + std::to_string(result->output_lines) +
          ",\"start_line\":" + std::to_string(result->start_line) + ",\"end_line\":" + std::to_string(result->end_line);
  if (result->totals_known)
    text += ",\"total_lines\":" + std::to_string(result->total_lines);
  if (result->next_offset_line > 0)
  {
    text += ",\"next_offset\":" + std::to_string(result->next_offset_line);
    text += ",\"next_offset_line\":" + std::to_string(result->next_offset_line);
    text += ",\"truncation_hint\":\"Call read_file again with offset=" + std::to_string(result->next_offset_line) + " to continue.\"";
  }
  else if (result->byte_limited)
  {
    text += ",\"truncation_hint\":\"Increase max_bytes to read more of this range.\"";
  }
  text += "}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = std::move(text)};
}

ToolDispatchResult write_file_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto path = tool_dispatch::required_safe_string_arg(call.arguments_json, "path", call.name);
  if (!path)
    return tool_dispatch::tool_error_result(call, path.error());
  auto content = tool_dispatch::required_text_arg(call.arguments_json, "content", call.name);
  if (!content)
    return tool_dispatch::tool_error_result(call, content.error());
  auto const tool_context = tool_dispatch::context_for_provider_tool(context, call);
  auto result = ava::tools::write_file(tool_context, tool_dispatch::workspace_path(context, *path), *content);
  if (!result)
    return tool_dispatch::tool_error_result(call, result.error());
  return ToolDispatchResult{.call_id = call.id,
                            .name = call.name,
                            .success = true,
                            .result_text = "{\"tool\":\"write_file\",\"ok\":true,\"path\":\"" + ava::core::json::escape(result->path.generic_string()) +
                                           "\",\"bytes_written\":" + std::to_string(result->bytes_written) + ",\"diff\":\"" +
                                           ava::core::json::escape(result->diff) + "\",\"diff_truncated\":" + tool_dispatch::json_bool(result->diff_truncated) +
                                           "}"};
}

ToolDispatchResult edit_file_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto path = tool_dispatch::required_safe_string_arg(call.arguments_json, "path", call.name);
  if (!path)
    return tool_dispatch::tool_error_result(call, path.error());
  auto old_text = tool_dispatch::required_text_arg(call.arguments_json, "old_text", call.name);
  if (!old_text)
    return tool_dispatch::tool_error_result(call, old_text.error());
  auto new_text = tool_dispatch::required_text_arg(call.arguments_json, "new_text", call.name);
  if (!new_text)
    return tool_dispatch::tool_error_result(call, new_text.error());
  auto const tool_context = tool_dispatch::context_for_provider_tool(context, call);
  auto result = ava::tools::edit_file(tool_context, tool_dispatch::workspace_path(context, *path), *old_text, *new_text);
  if (!result)
    return tool_dispatch::tool_error_result(call, result.error());
  return ToolDispatchResult{.call_id = call.id,
                            .name = call.name,
                            .success = true,
                            .result_text = "{\"tool\":\"edit_file\",\"ok\":true,\"path\":\"" + ava::core::json::escape(result->path.generic_string()) +
                                           "\",\"bytes_written\":" + std::to_string(result->bytes_written) + ",\"diff\":\"" +
                                           ava::core::json::escape(result->diff) + "\",\"diff_truncated\":" + tool_dispatch::json_bool(result->diff_truncated) +
                                           ",\"line_endings\":\"" + ava::core::json::escape(result->line_endings) +
                                           "\",\"utf8_bom\":" + tool_dispatch::json_bool(result->had_utf8_bom) + "}"};
}

}  // namespace ava::agent
