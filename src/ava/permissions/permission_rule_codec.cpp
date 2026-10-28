#include "sys.h"
#include "ava/permissions/permission_rules_internal.h"
#include "ava/core/ascii.h"
#include "ava/core/json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ava::permissions::permission_rules_internal {

constexpr std::size_t kMaxPermissionRuleReasonBytes = 1024;
constexpr std::size_t kMaxPermissionRuleCommandBytes = 8192;
constexpr std::size_t kMaxPermissionRuleIdentifierBytes = 256;
constexpr std::size_t kMaxPermissionRecipeKeyBytes = 128;
constexpr std::size_t kMaxPermissionRecipeDisplayBytes = 1024;

std::string_view recipe_key_prefix(PermissionRuleScope scope)
{
  return scope == PermissionRuleScope::Global ? std::string_view("sha256:ava-command-recipe-v1:") : std::string_view("sha256:ava-command-workspace-recipe-v1:");
}

bool valid_recipe_key(std::string_view value, PermissionRuleScope scope)
{
  auto const prefix = recipe_key_prefix(scope);
  if (!value.starts_with(prefix) || value.size() != prefix.size() + 64 || value.size() > kMaxPermissionRecipeKeyBytes)
    return false;
  return std::ranges::all_of(value.substr(prefix.size()), [](unsigned char ch) { return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f'); });
}

bool valid_identifier(std::string_view value)
{
  if (value.empty() || value.size() > kMaxPermissionRuleIdentifierBytes)
    return false;
  return std::ranges::all_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return std::isalnum(byte) != 0 || ch == '_' || ch == '-' || ch == '.';
  });
}

bool is_path_operation(Operation operation)
{
  return operation == Operation::ReadFile || operation == Operation::SearchFiles || operation == Operation::EditFile || operation == Operation::LspQuery;
}

bool is_command_operation(Operation operation)
{
  return operation == Operation::RunCommand || operation == Operation::NetworkFetch || operation == Operation::NetworkSearch ||
         operation == Operation::LspServerLaunch || operation == Operation::McpResourceRead;
}

void skip_ws(std::string_view text, std::size_t& index)
{
  while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index])) != 0) ++index;
}

std::optional<std::string> balanced_json(std::string_view text, std::size_t start, char open, char close)
{
  if (start >= text.size() || text[start] != open)
    return std::nullopt;
  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  for (std::size_t index = start; index < text.size(); ++index)
  {
    char const ch = text[index];
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
    if (ch == open)
      ++depth;
    if (ch == close)
    {
      --depth;
      if (depth == 0)
        return std::string(text.substr(start, index - start + 1));
      if (depth < 0)
        return std::nullopt;
    }
  }
  return std::nullopt;
}

ava::core::Result<std::vector<std::string>> v2_rule_member_names(std::string_view object, std::filesystem::path const& path, std::size_t rule_index)
{
  std::vector<std::string> names;
  std::size_t index = 1;
  while (index + 1 < object.size())
  {
    skip_ws(object, index);
    if (index + 1 >= object.size() || object[index] == '}')
      break;
    if (object[index] != '"')
      return std::unexpected(rule_parse_error("schema-v2 permission rule has malformed member name", path, "rules", rule_index));
    ++index;
    std::string name;
    while (index < object.size() && object[index] != '"')
    {
      if (object[index] == '\\')
        return std::unexpected(rule_parse_error("schema-v2 permission rule member names must be unescaped ASCII", path, "rules", rule_index));
      name.push_back(object[index++]);
    }
    if (index >= object.size())
      return std::unexpected(rule_parse_error("schema-v2 permission rule has malformed member name", path, "rules", rule_index));
    ++index;
    skip_ws(object, index);
    if (index >= object.size() || object[index] != ':')
      return std::unexpected(rule_parse_error("schema-v2 permission rule has malformed member separator", path, "rules", rule_index));
    ++index;
    skip_ws(object, index);
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    for (; index < object.size(); ++index)
    {
      char const ch = object[index];
      if (escaped)
      {
        escaped = false;
        continue;
      }
      if (in_string && ch == '\\')
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
      if (ch == '{' || ch == '[')
        ++depth;
      else if (ch == '}' || ch == ']')
      {
        if (depth == 0)
          break;
        --depth;
      }
      else if (ch == ',' && depth == 0)
        break;
    }
    if (index >= object.size())
      return std::unexpected(rule_parse_error("schema-v2 permission rule has malformed value", path, "rules", rule_index));
    if (std::ranges::find(names, name) != names.end())
      return std::unexpected(rule_parse_error("schema-v2 permission rule has duplicate member", path, name, rule_index));
    names.push_back(std::move(name));
    if (object[index] == ',')
      ++index;
  }
  return names;
}

ava::core::VoidResult validate_v2_rule_members(std::string_view object, std::filesystem::path const& path, std::size_t rule_index)
{
  static constexpr std::array<std::string_view, 15> kAllowed{"rule_id",        "scope",
                                                             "workspace_dir",  "action",
                                                             "operation",      "mode",
                                                             "tool_name",      "target_path",
                                                             "command",        "command_recipe_key",
                                                             "recipe_display", "critical_acknowledged",
                                                             "reason",         "actor",
                                                             "created_at"};
  auto names = v2_rule_member_names(object, path, rule_index);
  if (!names)
    return std::unexpected(std::move(names.error()));
  for (auto const& name : *names)
  {
    if (std::ranges::find(kAllowed, std::string_view(name)) == kAllowed.end())
      return std::unexpected(rule_parse_error("schema-v2 permission rule has unsupported member", path, name, rule_index));
  }
  return {};
}

ava::core::Result<std::vector<std::string>> rule_objects_array(std::string_view document, std::filesystem::path const& path)
{
  auto const start = ava::core::json::field_value_start(document, "rules");
  if (!start)
    return std::unexpected(rule_parse_error("permission rules file is missing rules array", path, "rules"));
  if (*start >= document.size() || document[*start] != '[')
  {
    return std::unexpected(rule_parse_error("permission rules field must be an array", path, "rules"));
  }
  auto const array = balanced_json(document, *start, '[', ']');
  if (!array)
    return std::unexpected(rule_parse_error("permission rules array is malformed", path, "rules"));

  std::vector<std::string> objects;
  std::size_t index = 1;
  while (index + 1 < array->size())
  {
    skip_ws(*array, index);
    if (index + 1 >= array->size() || (*array)[index] == ']')
      break;
    if ((*array)[index] != '{')
    {
      return std::unexpected(rule_parse_error("permission rules array entries must be objects", path, "rules", objects.size()));
    }
    auto object = balanced_json(*array, index, '{', '}');
    if (!object)
      return std::unexpected(rule_parse_error("permission rule object is malformed", path, "rules", objects.size()));
    index += object->size();
    objects.push_back(std::move(*object));
    skip_ws(*array, index);
    if (index + 1 >= array->size() || (*array)[index] == ']')
      break;
    if ((*array)[index] != ',')
    {
      return std::unexpected(rule_parse_error("permission rules array is malformed", path, "rules", objects.size()));
    }
    ++index;
  }
  return objects;
}

ava::core::Result<std::string> required_string_field(std::string_view object, std::string_view key, std::filesystem::path const& path, std::size_t rule_index,
                                                     bool allow_empty = false)
{
  if (!ava::core::json::field_value_start(object, key))
  {
    return std::unexpected(rule_parse_error("permission rule is missing required field", path, key, rule_index));
  }
  auto value = ava::core::json::string_field(object, key);
  if (!value || (!allow_empty && value->empty()))
  {
    return std::unexpected(rule_parse_error("permission rule field must be a non-empty string", path, key, rule_index));
  }
  return *value;
}

ava::core::Result<std::string> optional_string_field(std::string_view object, std::string_view key, std::filesystem::path const& path, std::size_t rule_index)
{
  if (!ava::core::json::field_value_start(object, key))
    return std::string{};
  auto value = ava::core::json::string_field(object, key);
  if (!value)
  {
    return std::unexpected(rule_parse_error("permission rule field must be a string", path, key, rule_index));
  }
  return *value;
}

ava::core::Result<bool> optional_bool_field(std::string_view object, std::string_view key, std::filesystem::path const& path, std::size_t rule_index)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return false;
  auto const tail = object.substr(*start);
  auto token_ends = [&](std::size_t end) {
    while (end < tail.size() && std::isspace(static_cast<unsigned char>(tail[end])) != 0) ++end;
    return end == tail.size() || tail[end] == ',' || tail[end] == '}';
  };
  if (tail.starts_with("true") && token_ends(4))
    return true;
  if (tail.starts_with("false") && token_ends(5))
    return false;
  return std::unexpected(rule_parse_error("permission rule field must be a boolean", path, key, rule_index));
}

ava::core::Result<long long> exact_integer_field(std::string_view object, std::string_view key, std::filesystem::path const& path)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return std::unexpected(rule_parse_error("permission rules file is missing integer field", path, key));
  std::size_t end = *start;
  if (end < object.size() && object[end] == '-')
    ++end;
  auto const digits_start = end;
  while (end < object.size() && std::isdigit(static_cast<unsigned char>(object[end])) != 0) ++end;
  if (end == digits_start)
  {
    return std::unexpected(rule_parse_error("permission rules integer field is malformed", path, key));
  }
  auto const digits_end = end;
  while (end < object.size() && std::isspace(static_cast<unsigned char>(object[end])) != 0) ++end;
  if (end < object.size() && object[end] != ',' && object[end] != '}')
  {
    return std::unexpected(rule_parse_error("permission rules integer field is malformed", path, key));
  }
  long long value = 0;
  auto const parsed = std::from_chars(object.data() + *start, object.data() + digits_end, value);
  if (parsed.ec != std::errc{} || parsed.ptr != object.data() + digits_end)
    return std::unexpected(rule_parse_error("permission rules integer field is out of range", path, key));
  return value;
}

ava::core::VoidResult validate_rule(PersistentPermissionRule const& rule, std::filesystem::path const& path, std::optional<std::size_t> rule_index)
{
  if (!valid_identifier(rule.rule_id))
  {
    auto error = rule_parse_error("permission rule id is invalid", path, "rule_id", rule_index);
    error.with_context("rule_id", rule.rule_id);
    return std::unexpected(std::move(error));
  }
  if (rule.action == PermissionAction::Ask)
  {
    return std::unexpected(rule_parse_error("persistent permission rules only support allow or deny", path, "action", rule_index));
  }
  if (rule.schema_version != kLegacyPermissionRulesSchemaVersion && rule.schema_version != kCurrentPermissionRulesSchemaVersion)
  {
    return std::unexpected(rule_parse_error("permission rule schema_version is invalid", path, "schema_version", rule_index));
  }
  if (rule.critical_acknowledged && (rule.operation != Operation::RunCommand || rule.action != PermissionAction::Allow))
  {
    return std::unexpected(rule_parse_error("critical_acknowledged is valid only for exact RunCommand allows", path, "critical_acknowledged", rule_index));
  }
  if (rule.critical_acknowledged && is_repository_controlled_build_or_test_command(rule.command))
  {
    return std::unexpected(rule_parse_error("critical_acknowledged cannot authorize repository-controlled cmake build or ctest commands", path,
                                            "critical_acknowledged", rule_index));
  }
  if (!rule.actor.empty() && (!valid_identifier(rule.actor) || ava::core::has_c0_or_del(rule.actor)))
  {
    return std::unexpected(rule_parse_error("permission rule actor is invalid", path, "actor", rule_index));
  }
  if (rule.reason.empty() || rule.reason.size() > kMaxPermissionRuleReasonBytes || ava::core::has_c0_or_del(rule.reason) ||
      !ava::core::json::is_valid_utf8(rule.reason))
  {
    return std::unexpected(rule_parse_error("permission rule reason is invalid", path, "reason", rule_index));
  }
  if (!rule.tool_name.empty() && (!valid_identifier(rule.tool_name) || ava::core::has_c0_or_del(rule.tool_name)))
  {
    return std::unexpected(rule_parse_error("permission rule tool_name is invalid", path, "tool_name", rule_index));
  }
  if (rule.command.size() > kMaxPermissionRuleCommandBytes || ava::core::has_c0_or_del(rule.command) || !ava::core::json::is_valid_utf8(rule.command))
  {
    return std::unexpected(rule_parse_error("permission rule command is invalid", path, "command", rule_index));
  }
  if (rule.schema_version == kLegacyPermissionRulesSchemaVersion &&
      (!rule.command_recipe_key.empty() || !rule.recipe_display.empty() || rule.critical_acknowledged))
  {
    return std::unexpected(rule_parse_error("schema-v1 rules cannot contain stable command recipe fields", path, "command_recipe_key", rule_index));
  }
  if (rule.command_recipe_key.size() > kMaxPermissionRecipeKeyBytes || ava::core::has_c0_or_del(rule.command_recipe_key) ||
      (!rule.command_recipe_key.empty() && !valid_recipe_key(rule.command_recipe_key, rule.scope)))
  {
    return std::unexpected(rule_parse_error("permission rule command_recipe_key is invalid for its scope", path, "command_recipe_key", rule_index));
  }
  if (rule.recipe_display.size() > kMaxPermissionRecipeDisplayBytes || ava::core::has_c0_or_del(rule.recipe_display) ||
      !ava::core::json::is_valid_utf8(rule.recipe_display))
  {
    return std::unexpected(rule_parse_error("permission rule recipe_display is invalid", path, "recipe_display", rule_index));
  }
  if (rule.scope == PermissionRuleScope::Workspace && rule.workspace_dir.empty())
  {
    return std::unexpected(rule_parse_error("workspace permission rule is missing workspace_dir", path, "workspace_dir", rule_index));
  }
  if (is_path_operation(rule.operation) && rule.target_path.empty())
  {
    return std::unexpected(rule_parse_error("path permission rule is missing target_path", path, "target_path", rule_index));
  }
  if (rule.operation == Operation::RunCommand)
  {
    if (rule.action == PermissionAction::Allow && rule.schema_version == kCurrentPermissionRulesSchemaVersion)
    {
      bool const recipe_allow = !rule.command_recipe_key.empty() && !rule.critical_acknowledged;
      bool const exact_critical_allow = rule.command_recipe_key.empty() && rule.critical_acknowledged && !rule.command.empty();
      if (!recipe_allow && !exact_critical_allow)
      {
        return std::unexpected(rule_parse_error("schema-v2 command allow requires an exact scope recipe key, or critical_acknowledged=true with exact command",
                                                path, "command_recipe_key", rule_index));
      }
    }
    if (rule.action == PermissionAction::Deny && rule.command.empty() && rule.command_recipe_key.empty())
    {
      return std::unexpected(rule_parse_error("command deny rule requires exact command or recipe key", path, "command", rule_index));
    }
    if (rule.schema_version == kLegacyPermissionRulesSchemaVersion && rule.action == PermissionAction::Allow && rule.command.empty())
    {
      return std::unexpected(rule_parse_error("legacy command allow is missing exact command", path, "command", rule_index));
    }
  }
  else if (is_command_operation(rule.operation) && rule.command.empty())
  {
    return std::unexpected(rule_parse_error("command permission rule is missing command", path, "command", rule_index));
  }
  if (!is_path_operation(rule.operation) && !is_command_operation(rule.operation) && rule.tool_name.empty())
  {
    return std::unexpected(rule_parse_error("permission rule is missing tool_name", path, "tool_name", rule_index));
  }
  return {};
}

ava::core::Result<PersistentPermissionRule> parse_rule_object(std::string_view object, std::filesystem::path const& path, PermissionRuleScope file_scope,
                                                              long long schema_version, std::size_t rule_index)
{
  if (!ava::core::json::is_valid_object(object))
  {
    return std::unexpected(rule_parse_error("permission rule object is not valid JSON", path, "rules", rule_index));
  }
  if (schema_version == kCurrentPermissionRulesSchemaVersion)
  {
    if (auto members = validate_v2_rule_members(object, path, rule_index); !members)
      return std::unexpected(std::move(members.error()));
  }
  auto id = required_string_field(object, "rule_id", path, rule_index);
  if (!id)
    return std::unexpected(std::move(id.error()));
  auto scope_text = required_string_field(object, "scope", path, rule_index);
  if (!scope_text)
    return std::unexpected(std::move(scope_text.error()));
  auto scope = parse_permission_rule_scope(*scope_text);
  if (!scope)
    return std::unexpected(rule_parse_error("permission rule has unsupported scope", path, "scope", rule_index));
  if (*scope != file_scope)
  {
    return std::unexpected(rule_parse_error("permission rule scope does not match its storage file", path, "scope", rule_index));
  }
  auto action_text = required_string_field(object, "action", path, rule_index);
  if (!action_text)
    return std::unexpected(std::move(action_text.error()));
  auto action = parse_permission_action(*action_text);
  if (!action || *action == PermissionAction::Ask)
  {
    return std::unexpected(rule_parse_error("permission rule has unsupported action", path, "action", rule_index));
  }
  auto operation_text = required_string_field(object, "operation", path, rule_index);
  if (!operation_text)
    return std::unexpected(std::move(operation_text.error()));
  auto operation = parse_operation(*operation_text);
  if (!operation)
  {
    return std::unexpected(rule_parse_error("permission rule has unsupported operation", path, "operation", rule_index));
  }
  auto mode_text = required_string_field(object, "mode", path, rule_index);
  if (!mode_text)
    return std::unexpected(std::move(mode_text.error()));
  auto mode = parse_permission_rule_mode(*mode_text);
  if (!mode)
    return std::unexpected(rule_parse_error("permission rule has unsupported mode", path, "mode", rule_index));
  auto reason = required_string_field(object, "reason", path, rule_index);
  if (!reason)
    return std::unexpected(std::move(reason.error()));
  auto actor = required_string_field(object, "actor", path, rule_index);
  if (!actor)
    return std::unexpected(std::move(actor.error()));
  auto created_at = required_string_field(object, "created_at", path, rule_index);
  if (!created_at)
    return std::unexpected(std::move(created_at.error()));
  auto workspace_dir = optional_string_field(object, "workspace_dir", path, rule_index);
  if (!workspace_dir)
    return std::unexpected(std::move(workspace_dir.error()));
  auto target_path = optional_string_field(object, "target_path", path, rule_index);
  if (!target_path)
    return std::unexpected(std::move(target_path.error()));
  auto command = optional_string_field(object, "command", path, rule_index);
  if (!command)
    return std::unexpected(std::move(command.error()));
  auto tool_name = optional_string_field(object, "tool_name", path, rule_index);
  if (!tool_name)
    return std::unexpected(std::move(tool_name.error()));
  auto command_recipe_key = optional_string_field(object, "command_recipe_key", path, rule_index);
  if (!command_recipe_key)
    return std::unexpected(std::move(command_recipe_key.error()));
  auto recipe_display = optional_string_field(object, "recipe_display", path, rule_index);
  if (!recipe_display)
    return std::unexpected(std::move(recipe_display.error()));
  auto critical_acknowledged = optional_bool_field(object, "critical_acknowledged", path, rule_index);
  if (!critical_acknowledged)
    return std::unexpected(std::move(critical_acknowledged.error()));
  if (!workspace_dir->empty() && std::filesystem::path(*workspace_dir).is_relative())
  {
    return std::unexpected(rule_parse_error("permission rule workspace_dir must be absolute", path, "workspace_dir", rule_index));
  }
  if (!target_path->empty() && std::filesystem::path(*target_path).is_relative())
  {
    return std::unexpected(rule_parse_error("permission rule target_path must be absolute", path, "target_path", rule_index));
  }

  PersistentPermissionRule rule{.rule_id = std::move(*id),
                                .scope = *scope,
                                .workspace_dir = workspace_dir->empty() ? std::filesystem::path{} : normalized_path(*workspace_dir),
                                .action = *action,
                                .operation = *operation,
                                .mode = *mode,
                                .tool_name = std::move(*tool_name),
                                .target_path = target_path->empty() ? std::filesystem::path{} : normalized_path(*target_path),
                                .command = std::move(*command),
                                .command_recipe_key = std::move(*command_recipe_key),
                                .recipe_display = std::move(*recipe_display),
                                .critical_acknowledged = *critical_acknowledged,
                                .schema_version = schema_version,
                                .reason = std::move(*reason),
                                .actor = std::move(*actor),
                                .created_at = std::move(*created_at)};
  if (auto valid = validate_rule(rule, path, rule_index); !valid)
    return std::unexpected(std::move(valid.error()));
  return rule;
}

ava::core::Result<std::vector<PersistentPermissionRule>> parse_rules_file(std::string_view content, std::filesystem::path const& path,
                                                                          PermissionRuleScope file_scope)
{
  if (!ava::core::json::is_valid_object(content))
  {
    return std::unexpected(rule_parse_error("permission rules file is not a valid JSON object", path));
  }
  auto const version = exact_integer_field(content, "schema_version", path);
  if (!version)
  {
    return std::unexpected(std::move(version.error()));
  }
  if (*version != kLegacyPermissionRulesSchemaVersion && *version != kCurrentPermissionRulesSchemaVersion)
  {
    auto error = rule_parse_error("unsupported permission rules schema_version", path, "schema_version");
    error.with_context("schema_version", std::to_string(*version));
    error.with_context("supported_schema_version", std::to_string(kCurrentPermissionRulesSchemaVersion));
    return std::unexpected(std::move(error));
  }
  if (*version == kCurrentPermissionRulesSchemaVersion)
  {
    static constexpr std::array<std::string_view, 2> kAllowed{"schema_version", "rules"};
    auto names = v2_rule_member_names(content, path, 0);
    if (!names)
      return std::unexpected(std::move(names.error()));
    for (auto const& name : *names)
    {
      if (std::ranges::find(kAllowed, std::string_view(name)) == kAllowed.end())
        return std::unexpected(rule_parse_error("schema-v2 permission rules file has unsupported member", path, name));
    }
  }

  auto objects = rule_objects_array(content, path);
  if (!objects)
    return std::unexpected(std::move(objects.error()));
  std::vector<PersistentPermissionRule> rules;
  rules.reserve(objects->size());
  for (std::size_t index = 0; index < objects->size(); ++index)
  {
    auto rule = parse_rule_object((*objects)[index], path, file_scope, *version, index);
    if (!rule)
      return std::unexpected(std::move(rule.error()));
    rules.push_back(std::move(*rule));
  }
  return rules;
}
std::string rules_file_json(std::vector<PersistentPermissionRule> const& rules)
{
  // A v1 command Allow cannot be represented in schema v2. Preserve a valid
  // v1 document until the final such Allow is explicitly removed; the next
  // write then deliberately migrates the remaining rules to v2.
  auto const schema_version = contains_legacy_command_allow(rules) ? kLegacyPermissionRulesSchemaVersion : kCurrentPermissionRulesSchemaVersion;
  std::string json = "{\"schema_version\":";
  json += std::to_string(schema_version);
  json += ",\"rules\":[";
  for (std::size_t index = 0; index < rules.size(); ++index)
  {
    if (index > 0)
      json += ',';
    auto serialized_rule = rules[index];
    serialized_rule.schema_version = schema_version;
    json += permission_rule_json(serialized_rule);
  }
  json += "]}\n";
  return json;
}

}  // namespace ava::permissions::permission_rules_internal

namespace ava::permissions {

std::string permission_rule_json(PersistentPermissionRule const& rule)
{
  std::string json = "{";
  json += "\"rule_id\":\"" + ava::core::json::escape(rule.rule_id) + "\"";
  json += ",\"scope\":\"" + ava::core::json::escape(to_string(rule.scope)) + "\"";
  json += ",\"workspace_dir\":\"" + ava::core::json::escape(rule.workspace_dir.string()) + "\"";
  json += ",\"action\":\"" + ava::core::json::escape(to_string(rule.action)) + "\"";
  json += ",\"operation\":\"" + ava::core::json::escape(to_string(rule.operation)) + "\"";
  json += ",\"mode\":\"" + ava::core::json::escape(to_string(rule.mode)) + "\"";
  json += ",\"tool_name\":\"" + ava::core::json::escape(rule.tool_name) + "\"";
  json += ",\"target_path\":\"" + ava::core::json::escape(rule.target_path.string()) + "\"";
  json += ",\"command\":\"" + ava::core::json::escape(rule.command) + "\"";
  if (rule.schema_version != kLegacyPermissionRulesSchemaVersion)
  {
    json += ",\"command_recipe_key\":\"" + ava::core::json::escape(rule.command_recipe_key) + "\"";
    json += ",\"recipe_display\":\"" + ava::core::json::escape(rule.recipe_display) + "\"";
    json += ",\"critical_acknowledged\":" + std::string(rule.critical_acknowledged ? "true" : "false");
  }
  json += ",\"reason\":\"" + ava::core::json::escape(rule.reason) + "\"";
  json += ",\"actor\":\"" + ava::core::json::escape(rule.actor) + "\"";
  json += ",\"created_at\":\"" + ava::core::json::escape(rule.created_at) + "\"";
  json += '}';
  return json;
}

}  // namespace ava::permissions
