#include "sys.h"
#include "ava/agent/tool_dispatch_common.h"
#include "ava/agent/tool_dispatch_lsp.h"
#include "ava/tools/lsp_tools.h"
#include "ava/core/json.h"

#include <limits>
#include <utility>
#include <vector>

namespace ava::agent {
namespace {

constexpr std::size_t kMaxLspProviderDiagnostics = 200;
constexpr std::size_t kMaxLspProviderJsonBytes = 64 * 1024;
constexpr std::size_t kMaxLspProviderPathBytes = 4096;
constexpr std::size_t kMaxLspProviderQueryBytes = 1024;

bool is_safe_lsp_timeout_ms(std::string_view value)
{
  if (value.empty() || value.size() > 5)
    return false;
  int timeout_ms = 0;
  for (char const ch : value)
  {
    if (ch < '0' || ch > '9')
      return false;
    timeout_ms = timeout_ms * 10 + (ch - '0');
  }
  return timeout_ms >= 100 && timeout_ms <= 30000;
}

bool is_safe_lsp_phase(std::string_view value)
{
  return value == "startup" || value == "request";
}

bool is_known_lsp_method(std::string_view value)
{
  return value == "initialize" || value == "initialized" || value == "textDocument/didOpen" || value == "textDocument/diagnostic" ||
         value == "textDocument/documentSymbol" || value == "workspace/symbol" || value == "textDocument/definition" || value == "textDocument/references";
}

void append_safe_lsp_error_context(ava::core::Error& redacted, ava::core::Error const& error)
{
  for (auto const& context : error.context())
  {
    if ((context.key == "timeout_ms" && is_safe_lsp_timeout_ms(context.value)) || (context.key == "phase" && is_safe_lsp_phase(context.value)) ||
        (context.key == "method" && is_known_lsp_method(context.value)))
    {
      redacted.with_context(context.key, context.value);
    }
  }
}

ToolDispatchResult lsp_error_result(ProviderToolCall const& call, ava::core::Error const& error)
{
  if (error.code() == ava::core::ErrorCode::Canceled)
  {
    auto redacted = ava::core::Error(error.category(), "LSP query canceled", error.code());
    append_safe_lsp_error_context(redacted, error);
    redacted.with_context("tool", call.name);
    return tool_dispatch::tool_error_result(call, redacted);
  }
  auto redacted = ava::core::Error(error.category(), "LSP query failed", error.code());
  append_safe_lsp_error_context(redacted, error);
  redacted.with_context("tool", call.name);
  return tool_dispatch::tool_error_result(call, redacted);
}

std::string lsp_range_json(ava::lsp::Range const& range)
{
  return "{\"start_line\":" + std::to_string(range.start_line) + ",\"start_column\":" + std::to_string(range.start_column) +
         ",\"end_line\":" + std::to_string(range.end_line) + ",\"end_column\":" + std::to_string(range.end_column) + "}";
}

std::string lsp_path_for_result(ava::tools::ToolContext const& context, std::filesystem::path const& path)
{
  std::error_code error;
  auto relative = std::filesystem::relative(path, context.workspace_dir, error);
  if (!error && !relative.empty())
  {
    auto const native = relative.native();
    if (native != ".." && native.rfind("../", 0) != 0)
      return relative.generic_string();
  }
  return path.generic_string();
}

std::string lsp_symbol_entry_json(ava::tools::ToolContext const& context, ava::lsp::Symbol const& symbol)
{
  return "{\"name\":\"" + ava::core::json::escape(symbol.name) + "\",\"kind\":" + std::to_string(symbol.kind) + ",\"path\":\"" +
         ava::core::json::escape(lsp_path_for_result(context, symbol.path)) + "\",\"range\":" + lsp_range_json(symbol.range) + ",\"container\":\"" +
         ava::core::json::escape(symbol.container) + "\"}";
}

std::string lsp_location_entry_json(ava::tools::ToolContext const& context, ava::lsp::Location const& location)
{
  return "{\"path\":\"" + ava::core::json::escape(lsp_path_for_result(context, location.path)) + "\",\"range\":" + lsp_range_json(location.range) + "}";
}

template <typename Entry, typename Serializer>
void append_lsp_entries(std::string& text, std::vector<Entry> const& entries, Serializer serializer, bool& truncated)
{
  for (std::size_t index = 0; index < entries.size(); ++index)
  {
    if (index >= kMaxLspProviderDiagnostics)
    {
      truncated = true;
      break;
    }
    auto entry = serializer(entries[index]);
    if (text.size() + entry.size() + 96 > kMaxLspProviderJsonBytes)
    {
      truncated = true;
      break;
    }
    if (index > 0)
      text += ',';
    text += std::move(entry);
  }
}

ava::core::Result<int> required_nonnegative_int_arg(std::string_view arguments, std::string_view field, std::string_view tool_name)
{
  auto value = ava::core::json::integer_field(arguments, field);
  if (!value || *value < 0 || *value > static_cast<long long>(std::numeric_limits<int>::max()))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "tool integer argument is invalid");
    error.with_context("tool", std::string(tool_name));
    error.with_context("field", std::string(field));
    return std::unexpected(std::move(error));
  }
  return static_cast<int>(*value);
}

}  // namespace

ToolDispatchResult lsp_diagnostics_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto path = tool_dispatch::required_safe_string_arg(call.arguments_json, "path", call.name);
  if (!path)
    return tool_dispatch::tool_error_result(call, path.error());
  if (path->size() > kMaxLspProviderPathBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "lsp_diagnostics path is too long");
    error.with_context("max_bytes", std::to_string(kMaxLspProviderPathBytes));
    return tool_dispatch::tool_error_result(call, error);
  }
  auto const tool_context = tool_dispatch::context_for_provider_tool(context, call);
  auto result = ava::tools::lsp_diagnostics(tool_context, tool_dispatch::workspace_path(context, *path));
  if (!result)
    return lsp_error_result(call, result.error());

  std::string text = "{\"tool\":\"lsp_diagnostics\",\"ok\":true,\"path\":\"" + ava::core::json::escape(*path) + "\",\"diagnostics\":[";
  auto const total_diagnostics = result->diagnostics.size();
  bool truncated = false;
  for (std::size_t index = 0; index < result->diagnostics.size(); ++index)
  {
    if (index >= kMaxLspProviderDiagnostics)
    {
      truncated = true;
      break;
    }
    auto const& diagnostic = result->diagnostics[index];
    auto entry = std::string{"{\"severity\":"} + std::to_string(diagnostic.severity) + ",\"message\":\"" + ava::core::json::escape(diagnostic.message) +
                 "\",\"line\":" + std::to_string(diagnostic.line) + ",\"column\":" + std::to_string(diagnostic.column) + ",\"code\":\"" +
                 ava::core::json::escape(diagnostic.code) + "\"}";
    if (text.size() + entry.size() + 80 > kMaxLspProviderJsonBytes)
    {
      truncated = true;
      break;
    }
    if (index > 0)
      text += ',';
    text += std::move(entry);
  }
  text += "],\"truncated\":" + tool_dispatch::json_bool(truncated) + ",\"total_diagnostics\":" + std::to_string(total_diagnostics) + "}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = std::move(text)};
}

ToolDispatchResult lsp_document_symbols_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto path = tool_dispatch::required_safe_string_arg(call.arguments_json, "path", call.name);
  if (!path)
    return tool_dispatch::tool_error_result(call, path.error());
  if (path->size() > kMaxLspProviderPathBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "lsp_document_symbols path is too long");
    error.with_context("max_bytes", std::to_string(kMaxLspProviderPathBytes));
    return tool_dispatch::tool_error_result(call, error);
  }
  auto const tool_context = tool_dispatch::context_for_provider_tool(context, call);
  auto result = ava::tools::lsp_document_symbols(tool_context, tool_dispatch::workspace_path(context, *path));
  if (!result)
    return lsp_error_result(call, result.error());

  std::string text = "{\"tool\":\"lsp_document_symbols\",\"ok\":true,\"path\":\"" + ava::core::json::escape(*path) + "\",\"symbols\":[";
  bool truncated = false;
  append_lsp_entries(text, result->symbols, [&](ava::lsp::Symbol const& symbol) { return lsp_symbol_entry_json(context, symbol); }, truncated);
  text += "],\"truncated\":" + tool_dispatch::json_bool(truncated) + ",\"total_symbols\":" + std::to_string(result->symbols.size()) + "}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = std::move(text)};
}

ToolDispatchResult lsp_workspace_symbols_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto query = tool_dispatch::required_safe_string_arg(call.arguments_json, "query", call.name);
  if (!query)
    return tool_dispatch::tool_error_result(call, query.error());
  if (query->size() > kMaxLspProviderQueryBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "lsp_workspace_symbols query is too long");
    error.with_context("max_bytes", std::to_string(kMaxLspProviderQueryBytes));
    return tool_dispatch::tool_error_result(call, error);
  }
  auto const tool_context = tool_dispatch::context_for_provider_tool(context, call);
  auto result = ava::tools::lsp_workspace_symbols(tool_context, *query);
  if (!result)
    return lsp_error_result(call, result.error());

  std::string text = "{\"tool\":\"lsp_workspace_symbols\",\"ok\":true,\"query\":\"" + ava::core::json::escape(*query) + "\",\"symbols\":[";
  bool truncated = false;
  append_lsp_entries(text, result->symbols, [&](ava::lsp::Symbol const& symbol) { return lsp_symbol_entry_json(context, symbol); }, truncated);
  text += "],\"truncated\":" + tool_dispatch::json_bool(truncated) + ",\"total_symbols\":" + std::to_string(result->symbols.size()) + "}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = std::move(text)};
}

ToolDispatchResult lsp_definition_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto path = tool_dispatch::required_safe_string_arg(call.arguments_json, "path", call.name);
  if (!path)
    return tool_dispatch::tool_error_result(call, path.error());
  if (path->size() > kMaxLspProviderPathBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "lsp_definition path is too long");
    error.with_context("max_bytes", std::to_string(kMaxLspProviderPathBytes));
    return tool_dispatch::tool_error_result(call, error);
  }
  auto line = required_nonnegative_int_arg(call.arguments_json, "line", call.name);
  if (!line)
    return tool_dispatch::tool_error_result(call, line.error());
  auto column = required_nonnegative_int_arg(call.arguments_json, "column", call.name);
  if (!column)
    return tool_dispatch::tool_error_result(call, column.error());

  auto const tool_context = tool_dispatch::context_for_provider_tool(context, call);
  auto result = ava::tools::lsp_definition(tool_context, tool_dispatch::workspace_path(context, *path), *line, *column);
  if (!result)
    return lsp_error_result(call, result.error());

  std::string text = "{\"tool\":\"lsp_definition\",\"ok\":true,\"path\":\"" + ava::core::json::escape(*path) + "\",\"line\":" + std::to_string(*line) +
                     ",\"column\":" + std::to_string(*column) + ",\"locations\":[";
  bool truncated = false;
  append_lsp_entries(text, result->locations, [&](ava::lsp::Location const& location) { return lsp_location_entry_json(context, location); }, truncated);
  text += "],\"truncated\":" + tool_dispatch::json_bool(truncated) + ",\"total_locations\":" + std::to_string(result->locations.size()) + "}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = std::move(text)};
}

ToolDispatchResult lsp_references_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto path = tool_dispatch::required_safe_string_arg(call.arguments_json, "path", call.name);
  if (!path)
    return tool_dispatch::tool_error_result(call, path.error());
  if (path->size() > kMaxLspProviderPathBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "lsp_references path is too long");
    error.with_context("max_bytes", std::to_string(kMaxLspProviderPathBytes));
    return tool_dispatch::tool_error_result(call, error);
  }
  auto line = required_nonnegative_int_arg(call.arguments_json, "line", call.name);
  if (!line)
    return tool_dispatch::tool_error_result(call, line.error());
  auto column = required_nonnegative_int_arg(call.arguments_json, "column", call.name);
  if (!column)
    return tool_dispatch::tool_error_result(call, column.error());

  auto const tool_context = tool_dispatch::context_for_provider_tool(context, call);
  auto result = ava::tools::lsp_references(tool_context, tool_dispatch::workspace_path(context, *path), *line, *column);
  if (!result)
    return lsp_error_result(call, result.error());

  std::string text = "{\"tool\":\"lsp_references\",\"ok\":true,\"path\":\"" + ava::core::json::escape(*path) + "\",\"line\":" + std::to_string(*line) +
                     ",\"column\":" + std::to_string(*column) + ",\"locations\":[";
  bool truncated = false;
  append_lsp_entries(text, result->locations, [&](ava::lsp::Location const& location) { return lsp_location_entry_json(context, location); }, truncated);
  text += "],\"truncated\":" + tool_dispatch::json_bool(truncated) + ",\"total_locations\":" + std::to_string(result->locations.size()) + "}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = std::move(text)};
}

}  // namespace ava::agent
