#include "sys.h"
#include "ava/lsp/lsp_client.h"
#include "ava/lsp/lsp_client_internal.h"
#include "ava/core/hex.h"
#include "ava/core/json.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::lsp::lsp_client_internal {
namespace {

constexpr std::size_t kMaxDiagnosticDocuments = 64;
constexpr std::size_t kMaxDiagnostics = 2048;
constexpr std::size_t kMaxDiagnosticTextBytes = 16 * 1024;
constexpr std::size_t kMaxDiagnosticCacheBytes = 2 * 1024 * 1024;

std::filesystem::path logical_path(std::filesystem::path const& path, std::filesystem::path const& workspace_root)
{
  return path.is_absolute() ? path.lexically_normal() : (workspace_root.lexically_normal() / path).lexically_normal();
}

std::string percent_encoded_file_path(std::filesystem::path const& path)
{
  auto const value = path.lexically_normal().generic_string();
  constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (unsigned char const byte : value)
  {
    bool const unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                            byte == '.' || byte == '~';
    if (byte == '/')
    {
      encoded.push_back('/');
    }
    else if (unreserved)
    {
      encoded.push_back(static_cast<char>(byte));
    }
    else
    {
      encoded.push_back('%');
      encoded.push_back(hex[byte >> 4]);
      encoded.push_back(hex[byte & 0x0F]);
    }
  }
  return encoded;
}

ava::core::Result<std::filesystem::path> path_from_file_uri(std::string_view uri, ServerConfig const& config)
{
  constexpr std::string_view prefix = "file://";
  if (!uri.starts_with(prefix))
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP location URI is not a file URI", config));
  }
  std::string decoded;
  auto const path = uri.substr(prefix.size());
  decoded.reserve(path.size());
  for (std::size_t index = 0; index < path.size(); ++index)
  {
    if (path[index] == '%')
    {
      if (index + 2 >= path.size())
      {
        return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP location URI has invalid escaping", config));
      }
      int const hi = ava::core::hex_digit_value(path[index + 1]);
      int const lo = ava::core::hex_digit_value(path[index + 2]);
      if (hi < 0 || lo < 0)
      {
        return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP location URI has invalid escaping", config));
      }
      char const byte = static_cast<char>((hi << 4) | lo);
      if (byte == '/' || byte == '\0')
      {
        return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP location URI escapes a path separator", config));
      }
      decoded.push_back(byte);
      index += 2;
    }
    else
    {
      decoded.push_back(path[index]);
    }
  }
  auto const candidate = std::filesystem::path(decoded).lexically_normal();
  auto const workspace = config.workspace_root.lexically_normal();
  if (!candidate.is_absolute() || !path_is_within(candidate, workspace))
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::PermissionDenied, "LSP location is outside the workspace", config));
  }
  return candidate;
}

std::string diagnostic_code(std::string_view object)
{
  if (auto code = ava::core::json::string_field(object, "code"))
    return *code;
  if (auto code = ava::core::json::integer_field(object, "code"))
    return std::to_string(*code);
  return {};
}

ava::core::Result<std::vector<Diagnostic>> parse_diagnostic_array(std::string_view container, std::string_view field, ServerConfig const& config,
                                                                  std::filesystem::path const& path)
{
  auto const start = ava::core::json::field_value_start(container, field);
  if (!start || *start >= container.size() || container[*start] != '[')
  {
    auto error = lsp_error(ava::core::ErrorCategory::Tool, "LSP diagnostics payload is malformed", config);
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  auto objects = ava::core::json::strict_objects_in_array_field(container, field, kMaxDiagnostics);
  if (!objects)
  {
    auto const loose_objects = ava::core::json::objects_in_array_field(container, field);
    if (loose_objects.size() > kMaxDiagnostics)
    {
      auto error = lsp_error(ava::core::ErrorCategory::Tool, "LSP diagnostics count exceeds cap", config);
      error.with_context("max_items", std::to_string(kMaxDiagnostics));
      return std::unexpected(std::move(error));
    }
    auto error = lsp_error(ava::core::ErrorCategory::Tool, "LSP diagnostics payload contains invalid items", config);
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }

  std::vector<Diagnostic> diagnostics;
  diagnostics.reserve(objects->size());
  for (auto const& object : *objects)
  {
    auto message = ava::core::json::string_field(object, "message");
    auto range = ava::core::json::object_field(object, "range");
    auto range_start = range ? ava::core::json::object_field(*range, "start") : std::optional<std::string>{};
    if (!message || message->size() > kMaxDiagnosticTextBytes || !range_start)
    {
      auto error = lsp_error(ava::core::ErrorCategory::Tool, "LSP diagnostic item is malformed", config);
      error.with_context("path", path.string());
      return std::unexpected(std::move(error));
    }

    auto const severity = ava::core::json::integer_field(object, "severity").value_or(0);
    auto const line = ava::core::json::integer_field(*range_start, "line").value_or(0);
    auto const character = ava::core::json::integer_field(*range_start, "character").value_or(0);
    if (severity < std::numeric_limits<int>::min() || severity > std::numeric_limits<int>::max() || line < 0 || line > std::numeric_limits<int>::max() ||
        character < 0 || character > std::numeric_limits<int>::max())
      return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP diagnostic item is malformed", config));
    auto code = diagnostic_code(object);
    if (code.size() > kMaxDiagnosticTextBytes)
      return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP diagnostic item is malformed", config));
    diagnostics.push_back(Diagnostic{.severity = static_cast<int>(severity),
                                     .message = std::move(*message),
                                     .line = static_cast<int>(line),
                                     .column = static_cast<int>(character),
                                     .code = std::move(code)});
  }
  return diagnostics;
}

std::size_t retained_diagnostic_bytes(std::vector<Diagnostic> const& diagnostics)
{
  std::size_t bytes = diagnostics.capacity() * sizeof(Diagnostic);
  for (auto const& diagnostic : diagnostics) bytes += diagnostic.message.capacity() + diagnostic.code.capacity();
  return bytes;
}

ava::core::Result<Range> parse_range(std::string_view object, ServerConfig const& config)
{
  auto range = ava::core::json::object_field(object, "range");
  auto start = range ? ava::core::json::object_field(*range, "start") : std::optional<std::string>{};
  auto end = range ? ava::core::json::object_field(*range, "end") : std::optional<std::string>{};
  if (!start || !end)
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP range is malformed", config));
  }
  auto const start_line = ava::core::json::integer_field(*start, "line");
  auto const start_character = ava::core::json::integer_field(*start, "character");
  auto const end_line = ava::core::json::integer_field(*end, "line");
  auto const end_character = ava::core::json::integer_field(*end, "character");
  if (!start_line || !start_character || !end_line || !end_character || *start_line < 0 || *start_character < 0 || *end_line < 0 || *end_character < 0)
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP range is malformed", config));
  }
  return Range{.start_line = static_cast<int>(*start_line),
               .start_column = static_cast<int>(*start_character),
               .end_line = static_cast<int>(*end_line),
               .end_column = static_cast<int>(*end_character)};
}

ava::core::Result<Location> parse_location(std::string_view object, ServerConfig const& config)
{
  auto uri = ava::core::json::string_field(object, "uri");
  if (!uri)
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP location is missing URI", config));
  auto path = path_from_file_uri(*uri, config);
  if (!path)
    return std::unexpected(std::move(path.error()));
  auto range = parse_range(object, config);
  if (!range)
    return std::unexpected(std::move(range.error()));
  return Location{.path = std::move(*path), .range = *range};
}

ava::core::VoidResult parse_document_symbol_object(std::string_view object, ServerConfig const& config, std::filesystem::path const& path,
                                                   std::string const& container, std::vector<Symbol>& symbols)
{
  auto name = ava::core::json::string_field(object, "name");
  auto kind = ava::core::json::integer_field(object, "kind");
  auto range = parse_range(object, config);
  if (!name || !kind || !range)
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP document symbol item is malformed", config));
  }
  symbols.push_back(Symbol{.name = std::move(*name), .kind = static_cast<int>(*kind), .path = path, .range = *range, .container = container});
  auto const child_container = symbols.back().name;
  for (auto const& child : ava::core::json::objects_in_array_field(object, "children"))
  {
    if (auto parsed = parse_document_symbol_object(child, config, path, child_container, symbols); !parsed)
    {
      return parsed;
    }
  }
  return {};
}

}  // namespace

bool path_is_within(std::filesystem::path const& candidate, std::filesystem::path const& root)
{
  auto root_component = root.begin();
  auto candidate_component = candidate.begin();
  for (; root_component != root.end(); ++root_component, ++candidate_component)
  {
    if (candidate_component == candidate.end() || *root_component != *candidate_component)
      return false;
  }
  return true;
}

std::string file_uri(std::filesystem::path const& path, std::filesystem::path const& workspace_root)
{
  return "file://" + percent_encoded_file_path(logical_path(path, workspace_root));
}

std::string json_string(std::string_view value)
{
  return "\"" + ava::core::json::escape(value) + "\"";
}

ava::core::Result<std::vector<Diagnostic>> parse_diagnostics_response(std::string_view response, ServerConfig const& config, std::filesystem::path const& path)
{
  auto const result = ava::core::json::object_field(response, "result");
  if (!result)
  {
    auto error = lsp_error(ava::core::ErrorCategory::Tool, "LSP diagnostics response is missing result", config);
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  return parse_diagnostic_array(*result, "items", config, path);
}

ava::core::Result<bool> parse_pull_diagnostics_capability(std::string_view response, ServerConfig const& config)
{
  auto const result = ava::core::json::object_field(response, "result");
  auto const capabilities = result ? ava::core::json::object_field(*result, "capabilities") : std::nullopt;
  if (!result || !capabilities)
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP initialize capabilities are malformed", config));
  auto const start = ava::core::json::field_value_start(*capabilities, "diagnosticProvider");
  if (!start)
    return false;
  if (*start >= capabilities->size())
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP initialize diagnostic capability is malformed", config));
  auto const remaining = std::string_view(*capabilities).substr(*start);
  if (remaining.starts_with("false") || remaining.starts_with("null"))
    return false;
  if ((*capabilities)[*start] == '{' || remaining.starts_with("true"))
    return true;
  return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP initialize diagnostic capability is malformed", config));
}

ava::core::Result<std::vector<Symbol>> parse_document_symbols_response(std::string_view response, ServerConfig const& config, std::filesystem::path const& path)
{
  auto result = ava::core::json::field_value_start(response, "result");
  if (result && *result < response.size() && response[*result] == 'n')
    return std::vector<Symbol>{};
  if (!result || *result >= response.size() || response[*result] != '[')
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP document symbols response is missing result", config));
  }
  std::vector<Symbol> symbols;
  for (auto const& object : ava::core::json::objects_in_array_field(response, "result"))
  {
    if (ava::core::json::object_field(object, "location"))
    {
      auto name = ava::core::json::string_field(object, "name");
      auto kind = ava::core::json::integer_field(object, "kind");
      auto location = ava::core::json::object_field(object, "location");
      if (!name || !kind || !location)
      {
        return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP symbol item is malformed", config));
      }
      auto parsed = parse_location(*location, config);
      if (!parsed)
        return std::unexpected(std::move(parsed.error()));
      symbols.push_back(Symbol{.name = std::move(*name),
                               .kind = static_cast<int>(*kind),
                               .path = parsed->path,
                               .range = parsed->range,
                               .container = ava::core::json::string_field(object, "containerName").value_or(std::string{})});
      continue;
    }
    if (auto parsed = parse_document_symbol_object(object, config, path, {}, symbols); !parsed)
    {
      return std::unexpected(std::move(parsed.error()));
    }
  }
  return symbols;
}

ava::core::Result<std::vector<Symbol>> parse_workspace_symbols_response(std::string_view response, ServerConfig const& config)
{
  auto result = ava::core::json::field_value_start(response, "result");
  if (result && *result < response.size() && response[*result] == 'n')
    return std::vector<Symbol>{};
  if (!result || *result >= response.size() || response[*result] != '[')
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP workspace symbols response is missing result", config));
  }
  std::vector<Symbol> symbols;
  for (auto const& object : ava::core::json::objects_in_array_field(response, "result"))
  {
    auto name = ava::core::json::string_field(object, "name");
    auto kind = ava::core::json::integer_field(object, "kind");
    auto location = ava::core::json::object_field(object, "location");
    if (!name || !kind || !location)
    {
      return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP workspace symbol item is malformed", config));
    }
    auto parsed = parse_location(*location, config);
    if (!parsed)
      return std::unexpected(std::move(parsed.error()));
    symbols.push_back(Symbol{.name = std::move(*name),
                             .kind = static_cast<int>(*kind),
                             .path = parsed->path,
                             .range = parsed->range,
                             .container = ava::core::json::string_field(object, "containerName").value_or(std::string{})});
  }
  return symbols;
}

ava::core::Result<std::vector<Location>> parse_definition_response(std::string_view response, ServerConfig const& config)
{
  auto result_start = ava::core::json::field_value_start(response, "result");
  if (!result_start)
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP definition response is missing result", config));
  }
  std::vector<Location> locations;
  if (*result_start < response.size() && response[*result_start] == '[')
  {
    for (auto const& object : ava::core::json::objects_in_array_field(response, "result"))
    {
      auto location = parse_location(object, config);
      if (!location)
        return std::unexpected(std::move(location.error()));
      locations.push_back(std::move(*location));
    }
    return locations;
  }
  if (*result_start < response.size() && response[*result_start] == '{')
  {
    auto result = ava::core::json::object_field(response, "result");
    if (!result)
      return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP definition result is malformed", config));
    auto location = parse_location(*result, config);
    if (!location)
      return std::unexpected(std::move(location.error()));
    locations.push_back(std::move(*location));
    return locations;
  }
  return locations;
}

}  // namespace ava::lsp::lsp_client_internal

namespace ava::lsp {

using namespace lsp_client_internal;

ava::core::VoidResult SubprocessLspClient::dispatch_notification(std::string_view message)
{
  auto const method = ava::core::json::string_field(message, "method");
  if (!method)
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP notification is malformed", config_));
  if (*method != "textDocument/publishDiagnostics")
    return {};

  auto const params = ava::core::json::object_field(message, "params");
  auto const uri = params ? ava::core::json::string_field(*params, "uri") : std::nullopt;
  if (!params || !uri)
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP publish diagnostics notification is malformed", config_));
  auto path = path_from_file_uri(*uri, config_);
  if (!path)
    return std::unexpected(lsp_error(ava::core::ErrorCategory::PermissionDenied, "LSP publish diagnostics notification is outside the workspace", config_));
  auto diagnostics = parse_diagnostic_array(*params, "diagnostics", config_, *path);
  if (!diagnostics)
    return std::unexpected(std::move(diagnostics.error()));

  auto const version_start = ava::core::json::field_value_start(*params, "version");
  auto const published_version = ava::core::json::integer_field(*params, "version");
  if (version_start && (!published_version || *published_version < 0 || *published_version > std::numeric_limits<int>::max()))
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP publish diagnostics notification version is malformed", config_));
  auto const normalized_uri = file_uri(*path, config_.workspace_root);
  auto const opened_version = open_document_versions_.find(normalized_uri);
  // The LSP version field is optional. Reject a demonstrably stale version,
  // but accept unversioned publications after didChange rather than waiting
  // forever for metadata that conforming servers are not required to send.
  if (opened_version != open_document_versions_.end() && published_version && opened_version->second != *published_version)
    return {};

  bool const new_document = !diagnostics_cache_.contains(normalized_uri);
  if (new_document && diagnostics_cache_.size() >= kMaxDiagnosticDocuments)
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP diagnostic document count exceeds cap", config_));
  auto const retained_bytes = retained_diagnostic_bytes(*diagnostics);
  auto const previous_bytes = new_document ? 0 : diagnostics_cache_bytes_[normalized_uri];
  if (retained_bytes > kMaxDiagnosticCacheBytes || diagnostics_cache_total_bytes_ - previous_bytes > kMaxDiagnosticCacheBytes - retained_bytes)
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP diagnostic cache exceeds byte cap", config_));
  diagnostics_cache_total_bytes_ = diagnostics_cache_total_bytes_ - previous_bytes + retained_bytes;
  diagnostics_cache_[normalized_uri] = std::move(*diagnostics);
  diagnostics_cache_bytes_[normalized_uri] = retained_bytes;
  return {};
}

}  // namespace ava::lsp
