#include "sys.h"
#include "ava/plugin/manifest.h"

#include "ava/core/json.h"

#include <array>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

namespace ava::plugin {
namespace {

constexpr std::size_t kMaxManifestJsonDepth = 64;

ava::core::Error manifest_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
}

ava::core::VoidResult validate_manifest_json_depth(std::string_view json)
{
  bool in_string = false;
  bool escaped = false;
  std::size_t depth = 0;
  for (char const ch : json) {
    if (escaped) {
      escaped = false;
      continue;
    }
    if (in_string && ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      in_string = !in_string;
      continue;
    }
    if (in_string) continue;
    if (ch == '{' || ch == '[') {
      ++depth;
      if (depth > kMaxManifestJsonDepth) {
        return std::unexpected(manifest_error("plugin manifest exceeds maximum JSON depth")
                                   .with_context("max_depth", std::to_string(kMaxManifestJsonDepth)));
      }
    } else if ((ch == '}' || ch == ']') && depth > 0) {
      --depth;
    }
  }
  return {};
}

bool is_valid_plugin_id(std::string_view id)
{
  if (id.empty() || id.size() > 128) return false;
  bool last_was_separator = false;
  for (char const ch : id) {
    bool const allowed = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
    if (!allowed) return false;
    if ((ch == '.' || ch == '_' || ch == '-') && last_was_separator) return false;
    last_was_separator = ch == '.' || ch == '_' || ch == '-';
  }
  return !last_was_separator;
}

bool is_valid_contribution_name(std::string_view name)
{
  if (name.empty() || name.size() > 96) return false;
  for (char const ch : name) {
    bool const allowed = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                         ch == '_' || ch == '-' || ch == '.';
    if (!allowed) return false;
  }
  return true;
}

bool is_valid_resource_path(std::string_view path)
{
  if (path.empty() || path.size() > 512) return false;
  std::filesystem::path const parsed{std::string(path)};
  if (parsed.is_absolute()) return false;
  for (auto const& part : parsed.lexically_normal()) {
    if (part == "..") return false;
  }
  for (char const ch : path) {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F) return false;
  }
  return true;
}

std::optional<std::string> array_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start || *start >= object.size() || object[*start] != '[') return std::nullopt;
  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  for (std::size_t index = *start; index < object.size(); ++index) {
    char const ch = object[index];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (ch == '\\' && in_string) {
      escaped = true;
      continue;
    }
    if (ch == '"') {
      in_string = !in_string;
      continue;
    }
    if (in_string) continue;
    if (ch == '[') ++depth;
    if (ch == ']') {
      --depth;
      if (depth == 0) return std::string(object.substr(*start, index - *start + 1));
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
  if (!ava::core::json::field_value_start(object, key)) return values;
  auto const array = array_field(object, key);
  if (!array) {
    return std::unexpected(
        manifest_error("plugin manifest field must be an array of strings").with_context("field", std::string(key)));
  }
  std::size_t index = 1;
  while (index + 1 < array->size()) {
    while (index + 1 < array->size() && std::isspace(static_cast<unsigned char>((*array)[index])) != 0) ++index;
    if (index + 1 >= array->size() || (*array)[index] == ']') break;
    if ((*array)[index] != '"') {
      return std::unexpected(
          manifest_error("plugin manifest field must contain only strings").with_context("field", std::string(key)));
    }
    std::size_t end = index + 1;
    bool escaped = false;
    while (end < array->size()) {
      char const ch = (*array)[end];
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        break;
      }
      ++end;
    }
    if (end >= array->size()) {
      return std::unexpected(manifest_error("plugin manifest string array has unterminated string")
                                 .with_context("field", std::string(key)));
    }
    auto value = parse_string_literal(array->substr(index, end - index + 1));
    if (!value) {
      return std::unexpected(manifest_error("plugin manifest string array has invalid string escape")
                                 .with_context("field", std::string(key)));
    }
    values.push_back(std::move(*value));
    index = end + 1;
    while (index + 1 < array->size() && std::isspace(static_cast<unsigned char>((*array)[index])) != 0) ++index;
    if (index + 1 < array->size() && (*array)[index] == ',') {
      ++index;
      continue;
    }
    if (index < array->size() && (*array)[index] == ']') {
      break;
    }
    return std::unexpected(
        manifest_error("plugin manifest string array has invalid separator").with_context("field", std::string(key)));
  }
  return values;
}

std::vector<std::string> object_array_field(std::string_view object, std::string_view key)
{
  return ava::core::json::objects_in_array_field(object, key);
}

ava::core::Result<PluginEntrypoint> parse_entrypoint(std::string_view manifest_json)
{
  auto const entrypoint = ava::core::json::object_field(manifest_json, "entrypoint");
  if (!entrypoint) return std::unexpected(manifest_error("plugin manifest requires entrypoint object"));
  auto command = ava::core::json::string_field(*entrypoint, "command");
  if (!command || command->empty()) {
    return std::unexpected(manifest_error("plugin manifest entrypoint.command must be a non-empty string"));
  }
  auto args = string_array_field(*entrypoint, "args");
  if (!args) return std::unexpected(args.error());
  return PluginEntrypoint{.command = std::move(*command), .args = std::move(*args)};
}

ava::core::Result<PluginContributions> parse_contributions(std::string_view manifest_json)
{
  PluginContributions contributions;
  auto const contributes = ava::core::json::object_field(manifest_json, "contributes");
  if (!contributes) return contributions;

  for (auto const& tool : object_array_field(*contributes, "tools")) {
    auto name = ava::core::json::string_field(tool, "name");
    if (!name || !is_valid_contribution_name(*name)) {
      return std::unexpected(manifest_error("plugin tool contribution requires a valid name"));
    }
    auto description = ava::core::json::string_field(tool, "description").value_or("");
    auto schema = ava::core::json::object_field(tool, "input_schema");
    if (!schema || !ava::core::json::is_valid_object(*schema)) {
      return std::unexpected(
          manifest_error("plugin tool contribution requires a valid input_schema object").with_context("tool", *name));
    }
    contributions.tools.push_back(PluginToolContribution{
        .name = std::move(*name), .description = std::move(description), .input_schema_json = std::move(*schema)});
  }

  for (auto const& command : object_array_field(*contributes, "commands")) {
    auto name = ava::core::json::string_field(command, "name");
    if (!name || !is_valid_contribution_name(*name)) {
      return std::unexpected(manifest_error("plugin command contribution requires a valid name"));
    }
    auto description = ava::core::json::string_field(command, "description").value_or("");
    contributions.commands.push_back(
        PluginCommandContribution{.name = std::move(*name), .description = std::move(description)});
  }

  auto parse_resources = [](std::string_view contributes_json, std::string_view field,
                            std::string_view label) -> ava::core::Result<std::vector<PluginResourceContribution>> {
    std::vector<PluginResourceContribution> resources;
    for (auto const& resource : object_array_field(contributes_json, field)) {
      auto name = ava::core::json::string_field(resource, "name");
      if (!name || !is_valid_contribution_name(*name)) {
        return std::unexpected(manifest_error("plugin " + std::string(label) + " contribution requires a valid name"));
      }
      auto path = ava::core::json::string_field(resource, "path");
      if (!path || !is_valid_resource_path(*path)) {
        return std::unexpected(
            manifest_error("plugin " + std::string(label) + " contribution requires a safe relative path")
                .with_context(std::string(label), *name));
      }
      auto description = ava::core::json::string_field(resource, "description").value_or("");
      resources.push_back(PluginResourceContribution{
          .name = std::move(*name), .description = std::move(description), .path = std::move(*path)});
    }
    return resources;
  };

  auto prompts = parse_resources(*contributes, "prompts", "prompt");
  if (!prompts) return std::unexpected(prompts.error());
  contributions.prompts = std::move(*prompts);

  auto skills = parse_resources(*contributes, "skills", "skill");
  if (!skills) return std::unexpected(skills.error());
  contributions.skills = std::move(*skills);

  for (auto const& hook : object_array_field(*contributes, "event_hooks")) {
    auto event = ava::core::json::string_field(hook, "event");
    if (!event || !is_valid_contribution_name(*event)) {
      return std::unexpected(manifest_error("plugin event hook contribution requires a valid event"));
    }
    contributions.event_hooks.push_back(PluginEventHookContribution{.event = std::move(*event)});
  }

  return contributions;
}

}  // namespace

std::string_view to_string(PluginScope scope)
{
  switch (scope) {
    case PluginScope::Global:
      return "global";
    case PluginScope::Project:
      return "project";
  }
  return "unknown";
}

ava::core::Result<PluginManifest> parse_plugin_manifest(std::string_view json, std::filesystem::path manifest_path)
{
  if (json.size() > kMaxManifestBytes) {
    return std::unexpected(manifest_error("plugin manifest exceeds maximum size")
                               .with_context("max_bytes", std::to_string(kMaxManifestBytes)));
  }
  if (auto depth = validate_manifest_json_depth(json); !depth) {
    return std::unexpected(depth.error());
  }
  if (!ava::core::json::is_valid_object(json)) {
    return std::unexpected(manifest_error("plugin manifest must be a valid JSON object"));
  }

  auto const schema_version = ava::core::json::integer_field(json, "schema_version");
  if (!schema_version || *schema_version != 1) {
    return std::unexpected(manifest_error("plugin manifest schema_version must be 1"));
  }
  auto id = ava::core::json::string_field(json, "id");
  if (!id || !is_valid_plugin_id(*id)) {
    return std::unexpected(manifest_error("plugin manifest id must be lowercase and stable"));
  }
  auto name = ava::core::json::string_field(json, "name");
  if (!name || name->empty()) return std::unexpected(manifest_error("plugin manifest requires name"));
  auto version = ava::core::json::string_field(json, "version");
  if (!version || version->empty()) return std::unexpected(manifest_error("plugin manifest requires version"));
  auto api_version = ava::core::json::string_field(json, "api_version");
  if (!api_version || *api_version != kPluginApiVersion) {
    return std::unexpected(manifest_error("plugin manifest api_version is unsupported")
                               .with_context("expected", std::string(kPluginApiVersion)));
  }
  auto entrypoint = parse_entrypoint(json);
  if (!entrypoint) return std::unexpected(entrypoint.error());
  auto capabilities = string_array_field(json, "capabilities");
  if (!capabilities) return std::unexpected(capabilities.error());
  auto contributes = parse_contributions(json);
  if (!contributes) return std::unexpected(contributes.error());

  auto const manifest_directory = manifest_path.empty() ? std::filesystem::path{} : manifest_path.parent_path();
  return PluginManifest{.schema_version = static_cast<int>(*schema_version),
                        .id = std::move(*id),
                        .name = std::move(*name),
                        .version = std::move(*version),
                        .api_version = std::move(*api_version),
                        .description = ava::core::json::string_field(json, "description").value_or(""),
                        .entrypoint = std::move(*entrypoint),
                        .capabilities = std::move(*capabilities),
                        .contributes = std::move(*contributes),
                        .path = std::move(manifest_path),
                        .directory = manifest_directory};
}

ava::core::Result<PluginManifest> load_plugin_manifest(std::filesystem::path const& manifest_path)
{
  std::error_code type_error;
  if (!std::filesystem::is_regular_file(manifest_path, type_error)) {
    auto error = ava::core::Error(type_error ? ava::core::ErrorCategory::Io : ava::core::ErrorCategory::InvalidArgument,
                                  "plugin manifest must be a regular file")
                     .with_context("path", manifest_path.string());
    if (type_error) error.with_context("cause", type_error.message());
    return std::unexpected(std::move(error));
  }

  std::error_code size_error;
  auto const size = std::filesystem::file_size(manifest_path, size_error);
  if (size_error) {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect plugin manifest size")
                               .with_context("path", manifest_path.string())
                               .with_context("cause", size_error.message()));
  }
  if (size > kMaxManifestBytes) {
    return std::unexpected(manifest_error("plugin manifest exceeds maximum size")
                               .with_context("path", manifest_path.string())
                               .with_context("max_bytes", std::to_string(kMaxManifestBytes)));
  }
  std::ifstream file(manifest_path, std::ios::binary);
  if (!file) {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to open plugin manifest")
                               .with_context("path", manifest_path.string()));
  }
  std::string contents;
  std::array<char, 4096> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    auto const read_count = file.gcount();
    if (read_count <= 0) continue;
    if (contents.size() + static_cast<std::size_t>(read_count) > kMaxManifestBytes) {
      return std::unexpected(manifest_error("plugin manifest exceeds maximum size")
                                 .with_context("path", manifest_path.string())
                                 .with_context("max_bytes", std::to_string(kMaxManifestBytes)));
    }
    contents.append(buffer.data(), static_cast<std::size_t>(read_count));
  }
  if (file.bad()) {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to read plugin manifest")
                               .with_context("path", manifest_path.string()));
  }
  auto parsed = parse_plugin_manifest(contents, manifest_path);
  if (!parsed) parsed.error().with_context("path", manifest_path.string());
  return parsed;
}

bool plugin_has_capability(PluginManifest const& manifest, std::string_view capability)
{
  for (auto const& declared : manifest.capabilities) {
    if (declared == capability) return true;
  }
  return false;
}

}  // namespace ava::plugin
