#include "sys.h"
#include "ava/plugin/enablement.h"
#include "ava/config/xdg_paths.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/fd_io.h"
#include "ava/core/json.h"
#include "ava/core/path.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::plugin {
namespace {

constexpr std::size_t kPluginEnablementMaxBytes = 1024 * 1024;

std::optional<bool> bool_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::nullopt;
  auto const valid_terminator = [](std::string_view value, std::size_t offset) {
    while (offset < value.size() && std::isspace(static_cast<unsigned char>(value[offset])) != 0) ++offset;
    return offset >= value.size() || value[offset] == ',' || value[offset] == '}';
  };
  if (object.substr(*start, 4) == "true" && valid_terminator(object, *start + 4))
    return true;
  if (object.substr(*start, 5) == "false" && valid_terminator(object, *start + 5))
    return false;
  return std::nullopt;
}

bool is_valid_plugin_id(std::string_view id)
{
  if (id.empty() || id.size() > 128)
    return false;
  bool last_was_separator = false;
  for (char const ch : id)
  {
    bool const allowed = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
    if (!allowed)
      return false;
    if ((ch == '.' || ch == '_' || ch == '-') && last_was_separator)
      return false;
    last_was_separator = ch == '.' || ch == '_' || ch == '-';
  }
  return !last_was_separator;
}

std::optional<PluginScope> parse_scope(std::string_view scope)
{
  if (scope == "global")
    return PluginScope::Global;
  if (scope == "project")
    return PluginScope::Project;
  return std::nullopt;
}

std::optional<std::string> parse_string_literal(std::string_view literal)
{
  return ava::core::json::string_field(std::string("{\"key\":") + std::string(literal) + '}', "key");
}

std::vector<std::pair<std::string, std::string>> object_entries_with_object_values(std::string_view object)
{
  std::vector<std::pair<std::string, std::string>> entries;
  bool in_string = false;
  bool escaped = false;
  int object_depth = 0;
  int array_depth = 0;
  for (std::size_t index = 0; index < object.size(); ++index)
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
      if (!in_string && object_depth == 1 && array_depth == 0)
      {
        auto key_end = index + 1;
        bool key_escaped = false;
        while (key_end < object.size())
        {
          char const key_ch = object[key_end];
          if (key_escaped)
          {
            key_escaped = false;
          }
          else if (key_ch == '\\')
          {
            key_escaped = true;
          }
          else if (key_ch == '"')
          {
            break;
          }
          ++key_end;
        }
        if (key_end >= object.size())
          break;
        auto key = parse_string_literal(object.substr(index, key_end - index + 1));
        auto colon = key_end + 1;
        while (colon < object.size() && std::isspace(static_cast<unsigned char>(object[colon])) != 0) ++colon;
        if (key && colon < object.size() && object[colon] == ':')
        {
          auto value_start = colon + 1;
          while (value_start < object.size() && std::isspace(static_cast<unsigned char>(object[value_start])) != 0)
          {
            ++value_start;
          }
          if (value_start < object.size() && object[value_start] == '{')
          {
            if (auto value = ava::core::json::object_field(std::string("{\"value\":") + std::string(object.substr(value_start)), "value"))
            {
              entries.emplace_back(std::move(*key), std::move(*value));
              index = value_start + entries.back().second.size() - 1;
              continue;
            }
          }
        }
        index = key_end;
        continue;
      }
      in_string = !in_string;
      continue;
    }
    if (in_string)
      continue;
    if (ch == '{')
    {
      ++object_depth;
    }
    else if (ch == '}')
    {
      if (object_depth > 0)
        --object_depth;
    }
    else if (ch == '[')
    {
      ++array_depth;
    }
    else if (ch == ']')
    {
      if (array_depth > 0)
        --array_depth;
    }
  }
  return entries;
}

ava::core::Result<std::optional<std::string>> read_file_text(std::filesystem::path const& path)
{
  ava::core::UniqueFd const fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (fd.get() < 0)
  {
    if (errno == ENOENT)
      return std::optional<std::string>{};
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to read plugin enablement file").with_context("path", path.string()));
  }

  struct stat status{};
  if (::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uintmax_t>(status.st_size) > kPluginEnablementMaxBytes)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to read plugin enablement file").with_context("path", path.string()));
  }

  auto text = ava::core::read_descriptor_bounded(fd.get(), kPluginEnablementMaxBytes);
  if (!text)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to read plugin enablement file").with_context("path", path.string()));
  return std::optional<std::string>(std::move(*text));
}

ava::core::VoidResult write_file_atomic(std::filesystem::path const& path, std::string_view content)
{
  if (content.size() > kPluginEnablementMaxBytes)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin enablement state exceeds size cap"));
  }
  std::error_code create_error;
  std::filesystem::create_directories(path.parent_path(), create_error);
  if (create_error)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to create plugin state directory")
                               .with_context("path", path.parent_path().string())
                               .with_context("cause", create_error.message()));
  }
  auto const unique = std::to_string(::getpid()) + "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  auto const temp = path.parent_path() / (path.filename().string() + ".tmp." + unique);
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to write plugin state file").with_context("path", temp.string()));
    }
    file << content;
  }
  std::error_code rename_error;
  std::filesystem::rename(temp, path, rename_error);
  if (rename_error)
  {
    std::error_code cleanup_error;
    std::filesystem::remove(temp, cleanup_error);
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to commit plugin state file")
                               .with_context("path", path.string())
                               .with_context("cause", rename_error.message()));
  }
  return {};
}

std::string enablement_json(std::vector<PluginEnablementRecord> const& records)
{
  std::string json = "{\"workspaces\":{";
  bool first_workspace = true;
  std::vector<std::filesystem::path> workspaces;
  for (auto const& record : records)
  {
    if (std::ranges::find(workspaces, record.workspace) == workspaces.end())
      workspaces.push_back(record.workspace);
  }
  std::ranges::sort(workspaces);
  for (auto const& workspace : workspaces)
  {
    if (!first_workspace)
      json += ',';
    first_workspace = false;
    json += "\"" + ava::core::json::escape(workspace.string()) + "\":{";
    bool first_scope = true;
    for (auto const scope : {PluginScope::Global, PluginScope::Project})
    {
      bool has_scope = false;
      for (auto const& record : records)
      {
        if (record.workspace == workspace && record.scope == scope)
          has_scope = true;
      }
      if (!has_scope)
        continue;
      if (!first_scope)
        json += ',';
      first_scope = false;
      json += "\"" + ava::core::json::escape(std::string(to_string(scope))) + "\":{";
      bool first_plugin = true;
      for (auto const& record : records)
      {
        if (record.workspace != workspace || record.scope != scope)
          continue;
        if (!first_plugin)
          json += ',';
        first_plugin = false;
        json += "\"" + ava::core::json::escape(record.plugin_id) + "\":{";
        json += "\"enabled\":";
        json += record.enabled ? "true" : "false";
        json += '}';
      }
      json += '}';
    }
    json += '}';
  }
  json += "}}";
  return json;
}

}  // namespace

std::filesystem::path default_plugin_enablement_file()
{
  return ava::config::xdg_paths().ava_state_dir / "plugin-enablement.json";
}

std::filesystem::path canonical_workspace_key(std::filesystem::path const& workspace_root)
{
  return ava::core::normalized_absolute_path(workspace_root);
}

ava::core::Result<std::vector<PluginEnablementRecord>> load_plugin_enablement(std::filesystem::path const& state_file)
{
  std::vector<PluginEnablementRecord> records;
  if (state_file.empty())
    return records;
  auto json = read_file_text(state_file);
  if (!json)
    return std::unexpected(json.error());
  if (!*json)
    return records;
  if (!ava::core::json::is_valid_object(**json))
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin enablement state must be valid JSON").with_context("path", state_file.string()));
  }
  auto const workspaces = ava::core::json::object_field(**json, "workspaces");
  if (!workspaces)
    return records;

  for (auto const& [workspace, workspace_object] : object_entries_with_object_values(*workspaces))
  {
    for (auto const& [scope_name, scope_object] : object_entries_with_object_values(workspace_object))
    {
      auto const scope = parse_scope(scope_name);
      if (!scope)
        continue;
      for (auto const& [plugin_id, plugin_object] : object_entries_with_object_values(scope_object))
      {
        if (!is_valid_plugin_id(plugin_id))
          continue;
        records.push_back(PluginEnablementRecord{.workspace = std::filesystem::path(workspace),
                                                 .plugin_id = plugin_id,
                                                 .scope = *scope,
                                                 .enabled = bool_field(plugin_object, "enabled").value_or(false)});
      }
    }
  }
  return records;
}

ava::core::Result<bool> plugin_enabled(std::filesystem::path const& state_file, std::filesystem::path const& workspace_root, std::string_view plugin_id,
                                       PluginScope scope)
{
  auto records = load_plugin_enablement(state_file);
  if (!records)
    return std::unexpected(records.error());
  auto const workspace = canonical_workspace_key(workspace_root);
  for (auto const& record : *records)
  {
    if (record.workspace == workspace && record.plugin_id == plugin_id && record.scope == scope)
      return record.enabled;
  }
  return false;
}

ava::core::VoidResult set_plugin_enabled(std::filesystem::path const& state_file, std::filesystem::path const& workspace_root, std::string_view plugin_id,
                                         bool enabled, PluginScope scope)
{
  if (!is_valid_plugin_id(plugin_id))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin id is invalid for enablement state"));
  }
  auto records = load_plugin_enablement(state_file);
  if (!records)
    return std::unexpected(records.error());
  auto const workspace = canonical_workspace_key(workspace_root);
  bool updated = false;
  for (auto& record : *records)
  {
    if (record.workspace == workspace && record.plugin_id == plugin_id && record.scope == scope)
    {
      record.enabled = enabled;
      updated = true;
      break;
    }
  }
  if (!updated)
  {
    records->push_back(PluginEnablementRecord{.workspace = workspace, .plugin_id = std::string(plugin_id), .scope = scope, .enabled = enabled});
  }
  std::ranges::sort(*records, [](PluginEnablementRecord const& left, PluginEnablementRecord const& right) {
    if (left.workspace != right.workspace)
      return left.workspace < right.workspace;
    if (left.scope != right.scope)
      return static_cast<int>(left.scope) < static_cast<int>(right.scope);
    return left.plugin_id < right.plugin_id;
  });
  return write_file_atomic(state_file, enablement_json(*records));
}

}  // namespace ava::plugin
