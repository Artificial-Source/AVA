#include "sys.h"
#include "ava/process/environment.h"
#include "ava/mcp/config.h"
#include "ava/config/xdg_paths.h"
#include "ava/core/json.h"
#include "ava/core/process_args.h"
#include "ava/core/strict_json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <nlohmann/json.hpp>

namespace ava::mcp {
namespace {

constexpr std::size_t kMaxMcpConfigBytes = 256 * 1024;
constexpr std::size_t kMaxMcpArgBytes = 4096;

ava::core::Error config_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
}

bool has_forbidden_byte(std::string_view value)
{
  for (char const ch : value)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F)
      return true;
  }
  return false;
}

std::optional<std::string> workspace_relative_process_arg(std::string const& command, std::vector<std::string> const& args)
{
  if (ava::core::is_workspace_relative_process_arg(command))
    return command;
  auto const match = std::ranges::find_if(args, [](std::string const& value) { return ava::core::is_workspace_relative_process_arg(value); });
  if (match == args.end())
    return std::nullopt;
  return *match;
}

std::optional<bool> bool_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::nullopt;
  auto const valid_terminator = [](std::string_view value, std::size_t offset) {
    while (offset < value.size() && std::isspace(static_cast<unsigned char>(value[offset])) != 0)
      ++offset;
    return offset >= value.size() || value[offset] == ',' || value[offset] == '}';
  };
  if (object.substr(*start, 4) == "true" && valid_terminator(object, *start + 4))
    return true;
  if (object.substr(*start, 5) == "false" && valid_terminator(object, *start + 5))
    return false;
  return std::nullopt;
}

std::optional<std::string> array_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start || *start >= object.size() || object[*start] != '[')
    return std::nullopt;
  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  for (std::size_t index = *start; index < object.size(); ++index)
  {
    char const ch = object[index];
    if (escaped)
    {
      escaped = false;
      continue;
    }
    if (ch == '\\' && in_string)
    {
      escaped = true;
      continue;
    }
    if (ch == '"')
    {
      in_string = !in_string;
      continue;
    }
    if (in_string)
      continue;
    if (ch == '[')
      ++depth;
    if (ch == ']')
    {
      --depth;
      if (depth == 0)
        return std::string(object.substr(*start, index - *start + 1));
    }
  }
  return std::nullopt;
}

std::optional<std::string> parse_string_literal(std::string_view literal)
{
  return ava::core::json::string_field(std::string("{\"value\":") + std::string(literal) + '}', "value");
}

ava::core::Result<std::vector<std::string>> string_array_field(std::string_view object, std::string_view key)
{
  std::vector<std::string> values;
  if (!ava::core::json::field_value_start(object, key))
    return values;
  auto const array = array_field(object, key);
  if (!array)
  {
    return std::unexpected(config_error("MCP config field must be an array of strings").with_context("field", std::string(key)));
  }
  std::size_t index = 1;
  while (index + 1 < array->size())
  {
    while (index + 1 < array->size() && std::isspace(static_cast<unsigned char>((*array)[index])) != 0)
      ++index;
    if (index + 1 >= array->size() || (*array)[index] == ']')
      break;
    if ((*array)[index] != '"')
    {
      return std::unexpected(config_error("MCP config field must contain only strings").with_context("field", std::string(key)));
    }
    std::size_t end = index + 1;
    bool escaped = false;
    while (end < array->size())
    {
      char const ch = (*array)[end];
      if (escaped)
      {
        escaped = false;
      }
      else if (ch == '\\')
      {
        escaped = true;
      }
      else if (ch == '"')
      {
        break;
      }
      ++end;
    }
    if (end >= array->size())
    {
      return std::unexpected(config_error("MCP config string array has unterminated string").with_context("field", std::string(key)));
    }
    auto value = parse_string_literal(array->substr(index, end - index + 1));
    if (!value)
    {
      return std::unexpected(config_error("MCP config string array has invalid string escape").with_context("field", std::string(key)));
    }
    values.push_back(std::move(*value));
    index = end + 1;
    while (index + 1 < array->size() && std::isspace(static_cast<unsigned char>((*array)[index])) != 0)
      ++index;
    if (index + 1 < array->size() && (*array)[index] == ',')
    {
      ++index;
      continue;
    }
    if (index < array->size() && (*array)[index] == ']')
      break;
    return std::unexpected(config_error("MCP config string array has invalid separator").with_context("field", std::string(key)));
  }
  return values;
}

ava::core::Result<std::vector<std::pair<std::string, std::string>>> environment_field(std::string_view server_json)
{
  std::vector<std::pair<std::string, std::string>> environment;
  // The strict core validator already rejected duplicates. Decode field names
  // with the installed JSON implementation so escaped "env" is not ignored.
  auto const parsed = nlohmann::json::parse(server_json.begin(), server_json.end(), nullptr, false);
  if (!parsed.is_object())
    return std::unexpected(config_error("MCP server must be a JSON object"));
  auto const env = parsed.find("env");
  if (env == parsed.end())
    return environment;
  if (!env->is_object() || env->size() > ava::process::kMaxMcpExplicitEnvironmentEntriesV1)
    return std::unexpected(config_error("MCP env must be a bounded object of strings"));

  std::vector<ava::process::EnvironmentVariableV1> variables;
  for (auto const& [name, value] : env->items())
  {
    if (!value.is_string())
      return std::unexpected(config_error("MCP env values must be strings"));
    auto const& text = value.get_ref<std::string const&>();
    variables.push_back({.name = name, .value = text});
    environment.emplace_back(name, text);
  }
  // Use the shared policy, not a second copy of its name/value/PATH/byte rules.
  // Actual launch revalidates the complete environment with the actual child cwd.
  if (!ava::process::make_mcp_environment_v1("/", variables))
    return std::unexpected(config_error("MCP env violates the explicit process environment policy"));
  return environment;
}

ava::core::Result<std::string> read_config_file(std::filesystem::path const& path)
{
  std::error_code status_error;
  auto const status = std::filesystem::status(path, status_error);
  if (status_error)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect MCP config")
                               .with_context("path", path.string())
                               .with_context("cause", status_error.message()));
  }
  if (!std::filesystem::is_regular_file(status))
  {
    return std::unexpected(config_error("MCP config must be a regular file").with_context("path", path.string()));
  }
  std::error_code size_error;
  auto const size = std::filesystem::file_size(path, size_error);
  if (size_error)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect MCP config size")
                               .with_context("path", path.string())
                               .with_context("cause", size_error.message()));
  }
  if (size > kMaxMcpConfigBytes)
  {
    return std::unexpected(
        config_error("MCP config exceeds maximum size").with_context("path", path.string()).with_context("max_bytes", std::to_string(kMaxMcpConfigBytes)));
  }
  std::ifstream file(path, std::ios::binary);
  if (!file)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to open MCP config").with_context("path", path.string()));
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  if (file.bad())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to read MCP config").with_context("path", path.string()));
  }
  return buffer.str();
}

ava::core::Result<McpConfig> load_optional_config(std::filesystem::path const& path, McpServerScope scope)
{
  McpConfig config;
  if (scope == McpServerScope::Global)
  {
    config.global_config_file = path;
  }
  else
  {
    config.project_config_file = path;
  }
  if (path.empty())
    return config;
  std::error_code exists_error;
  bool const exists = std::filesystem::exists(path, exists_error);
  if (exists_error)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect MCP config")
                               .with_context("path", path.string())
                               .with_context("cause", exists_error.message()));
  }
  if (!exists)
    return config;
  auto content = read_config_file(path);
  if (!content)
    return std::unexpected(std::move(content.error()));
  return parse_mcp_config(*content, path, scope);
}

ava::core::VoidResult append_config(McpConfig& target, McpConfig source)
{
  if (!source.global_config_file.empty())
    target.global_config_file = std::move(source.global_config_file);
  if (!source.project_config_file.empty())
    target.project_config_file = std::move(source.project_config_file);
  for (auto& server : source.servers)
  {
    auto const duplicate = std::ranges::find_if(target.servers, [&](McpServerConfig const& existing) { return existing.id == server.id; });
    if (duplicate != target.servers.end())
    {
      return std::unexpected(config_error("duplicate MCP server id")
                                 .with_context("server", server.id)
                                 .with_context("first_config", duplicate->source_path.string())
                                 .with_context("second_config", server.source_path.string()));
    }
    target.servers.push_back(std::move(server));
  }
  return {};
}

}  // namespace

std::string_view to_string(McpServerScope scope)
{
  switch (scope)
  {
    case McpServerScope::Global:
      return "global";
    case McpServerScope::Project:
      return "project";
  }
  return "unknown";
}

bool is_valid_mcp_identifier(std::string_view id)
{
  if (id.empty() || id.size() > 128)
    return false;
  bool last_was_separator = false;
  for (char const ch : id)
  {
    auto const byte = static_cast<unsigned char>(ch);
    bool const allowed = std::isalnum(byte) != 0 || ch == '.' || ch == '_' || ch == '-';
    if (!allowed)
      return false;
    if ((ch == '.' || ch == '_' || ch == '-') && last_was_separator)
      return false;
    last_was_separator = ch == '.' || ch == '_' || ch == '-';
  }
  return !last_was_separator;
}

McpConfigLoadOptions default_mcp_config_options(std::filesystem::path const& workspace_dir)
{
  auto paths = ava::config::xdg_paths();
  McpConfigLoadOptions options;
  options.workspace_dir = workspace_dir;
  options.global_config_file = paths.ava_config_dir / "mcp.json";
  if (!workspace_dir.empty())
    options.project_config_file = workspace_dir / ".ava" / "mcp.json";
  return options;
}

ava::core::Result<McpConfig> parse_mcp_config(std::string_view json, std::filesystem::path config_path, McpServerScope scope)
{
  if (json.size() > kMaxMcpConfigBytes)
  {
    return std::unexpected(config_error("MCP config exceeds maximum size")
                               .with_context("config", config_path.string())
                               .with_context("max_bytes", std::to_string(kMaxMcpConfigBytes)));
  }
  if (!ava::core::json::is_valid_object(json))
  {
    return std::unexpected(config_error("MCP config must be a valid JSON object").with_context("config", config_path.string()));
  }
  if (ava::core::json::field_value_start(json, "servers") && !array_field(json, "servers"))
  {
    return std::unexpected(config_error("MCP config servers field must be an array"));
  }

  McpConfig config;
  if (scope == McpServerScope::Global)
  {
    config.global_config_file = config_path;
  }
  else
  {
    config.project_config_file = config_path;
  }

  for (auto const& server_json : ava::core::json::objects_in_array_field(json, "servers"))
  {
    auto id = ava::core::json::string_field(server_json, "id");
    if (!id || !is_valid_mcp_identifier(*id))
    {
      return std::unexpected(config_error("MCP server requires a valid id").with_context("config", config_path.string()));
    }
    if (ava::core::validate_strict_json(server_json, ava::core::json::kMaxNestingDepth) != ava::core::StrictJsonStatus::Valid)
    {
      return std::unexpected(config_error("MCP server JSON must not contain duplicate fields or keys")
                                 .with_context("config", config_path.string())
                                 .with_context("server", *id)
                                 .with_context("field", ava::core::json::field_value_start(server_json, "env") ? "env" : "server"));
    }
    auto env = environment_field(server_json);
    if (!env)
    {
      return std::unexpected(std::move(env.error()).with_context("config", config_path.string()).with_context("server", *id).with_context("field", "env"));
    }
    auto command = ava::core::json::string_field(server_json, "command");
    if (!command || command->empty() || command->size() > kMaxMcpArgBytes || has_forbidden_byte(*command))
    {
      return std::unexpected(
          config_error("MCP server requires a safe non-empty command").with_context("server", *id).with_context("config", config_path.string()));
    }
    auto args = string_array_field(server_json, "args");
    if (!args)
      return std::unexpected(std::move(args.error()));
    for (auto const& arg : *args)
    {
      if (arg.size() > kMaxMcpArgBytes || has_forbidden_byte(arg))
      {
        return std::unexpected(config_error("MCP server arg is unsafe").with_context("server", *id).with_context("config", config_path.string()));
      }
    }
    if (scope == McpServerScope::Global)
    {
      auto relative_arg = workspace_relative_process_arg(*command, *args);
      if (relative_arg)
      {
        return std::unexpected(
            config_error("global MCP server command/args must not contain workspace-relative paths")
                .with_context("server", *id)
                .with_context("config", config_path.string())
                .with_context("argument", *relative_arg)
                .with_context("resolution", "move the server to project MCP config and trust the project, or use an absolute path/PATH command"));
      }
    }
    auto const enabled = bool_field(server_json, "enabled").value_or(scope == McpServerScope::Global);
    auto const name = ava::core::json::string_field(server_json, "name").value_or(*id);
    if (name.empty() || name.size() > 256 || has_forbidden_byte(name))
    {
      return std::unexpected(config_error("MCP server name is unsafe").with_context("server", *id).with_context("config", config_path.string()));
    }

    config.servers.push_back(McpServerConfig{.id = std::move(*id),
                                             .name = std::move(name),
                                             .command = std::move(*command),
                                             .args = std::move(*args),
                                             .env = std::move(*env),
                                             .enabled = enabled,
                                             .scope = scope,
                                             .source_path = config_path});
  }
  if (ava::core::validate_strict_json(json, ava::core::json::kMaxNestingDepth) != ava::core::StrictJsonStatus::Valid)
    return std::unexpected(config_error("MCP config must be strict JSON without duplicate keys").with_context("config", config_path.string()));
  return config;
}

ava::core::Result<McpConfig> load_mcp_config(McpConfigLoadOptions const& options)
{
  McpConfig merged;
  merged.global_config_file = options.global_config_file;
  merged.project_config_file = options.project_config_file;

  auto global = load_optional_config(options.global_config_file, McpServerScope::Global);
  if (!global)
    return std::unexpected(std::move(global.error()));
  if (auto appended = append_config(merged, std::move(*global)); !appended)
  {
    return std::unexpected(std::move(appended.error()));
  }

  auto project = load_optional_config(options.project_config_file, McpServerScope::Project);
  if (!project)
    return std::unexpected(std::move(project.error()));
  if (auto appended = append_config(merged, std::move(*project)); !appended)
  {
    return std::unexpected(std::move(appended.error()));
  }

  return merged;
}

}  // namespace ava::mcp
