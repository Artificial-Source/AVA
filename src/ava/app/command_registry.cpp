#include "sys.h"
#include "ava/app/command_catalog.h"
#include "ava/app/command_format.h"
#include "ava/app/command_registry.h"
#include "ava/app/command_registry_detail.h"
#include "ava/app/command_tools.h"
#include "ava/app/project_trust.h"
#include "ava/app/runtime/ExtensionResourcePolicy.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime/command_names.h"
#include "ava/app/runtime/markdown_files.h"
#include "ava/plugin/diagnostics.h"
#include "ava/plugin/static_resources.h"
#include "ava/context/markdown_resource.h"
#include "ava/context/skill_loader.h"
#include "ava/core/fingerprint.h"
#include "ava/core/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ava::app {

namespace {

std::string_view command_token(std::string_view line) noexcept
{
  auto const end = line.find_first_of(" \t\r\n");
  return line.substr(0, end == std::string_view::npos ? line.size() : end);
}

ava::core::Result<std::vector<std::string>> parse_argument_tokens(std::string_view text)
{
  std::vector<std::string> tokens;
  std::string current;
  std::optional<char> quote;
  bool escaped = false;
  bool in_token = false;
  for (char const ch : text)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20 || byte == 0x7F)
    {
      if (ch != '\t' && ch != '\r' && ch != '\n')
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "command arguments contain control bytes"));
      }
    }
    if (escaped)
    {
      current.push_back(ch);
      escaped = false;
      in_token = true;
      continue;
    }
    if (ch == '\\')
    {
      escaped = true;
      in_token = true;
      continue;
    }
    if (quote)
    {
      if (ch == *quote)
      {
        quote.reset();
      }
      else
      {
        current.push_back(ch);
      }
      in_token = true;
      continue;
    }
    if (ch == '"' || ch == '\'')
    {
      quote = ch;
      in_token = true;
      continue;
    }
    if (std::isspace(byte) != 0)
    {
      if (in_token)
      {
        tokens.push_back(std::move(current));
        current.clear();
        in_token = false;
      }
      continue;
    }
    current.push_back(ch);
    in_token = true;
  }
  if (escaped)
    current.push_back('\\');
  if (quote)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "command arguments contain an unterminated quote"));
  }
  if (in_token)
    tokens.push_back(std::move(current));
  return tokens;
}

std::string join_tokens(std::vector<std::string> const& tokens, std::size_t start, std::optional<std::size_t> count)
{
  if (start >= tokens.size())
    return {};
  auto end = tokens.size();
  if (count)
    end = std::min(end, start + *count);
  std::string joined;
  for (std::size_t index = start; index < end; ++index)
  {
    if (!joined.empty())
      joined += ' ';
    joined += tokens[index];
  }
  return joined;
}

std::optional<std::pair<std::size_t, std::optional<std::size_t>>> parse_argument_slice(std::string_view value)
{
  if (!value.starts_with("${@:") || !value.ends_with('}'))
    return std::nullopt;
  value.remove_prefix(4);
  value.remove_suffix(1);
  auto const colon = value.find(':');
  auto parse_size = [](std::string_view text) -> std::optional<std::size_t> {
    if (text.empty())
      return std::nullopt;
    std::size_t value = 0;
    for (char const ch : text)
    {
      if (ch < '0' || ch > '9')
        return std::nullopt;
      value = value * 10 + static_cast<std::size_t>(ch - '0');
    }
    return value;
  };
  auto start = parse_size(colon == std::string_view::npos ? value : value.substr(0, colon));
  if (!start || *start == 0)
    return std::nullopt;
  if (colon == std::string_view::npos)
    return std::pair<std::size_t, std::optional<std::size_t>>{*start - 1, std::nullopt};
  auto count = parse_size(value.substr(colon + 1));
  if (!count)
    return std::nullopt;
  return std::pair<std::size_t, std::optional<std::size_t>>{*start - 1, *count};
}

std::optional<std::pair<std::size_t, std::string_view>> parse_argument_default(std::string_view value)
{
  if (!value.starts_with("${") || !value.ends_with('}'))
    return std::nullopt;
  value.remove_prefix(2);
  value.remove_suffix(1);
  if (value.empty() || value.front() < '1' || value.front() > '9')
    return std::nullopt;

  std::size_t index = 0;
  std::size_t number = 0;
  while (index < value.size() && std::isdigit(static_cast<unsigned char>(value[index])) != 0)
  {
    number = number * 10 + static_cast<std::size_t>(value[index] - '0');
    ++index;
  }
  if (number == 0 || value.substr(index, 2) != ":-")
    return std::nullopt;
  return std::pair<std::size_t, std::string_view>{number - 1, value.substr(index + 2)};
}

}  // namespace

ava::core::Result<std::vector<std::string>> parse_command_argument_tokens(std::string_view text)
{
  return parse_argument_tokens(text);
}

std::string to_string(UnifiedCommandSource source)
{
  switch (source)
  {
    case UnifiedCommandSource::Builtin:
      return "builtin";
    case UnifiedCommandSource::PromptProject:
      return "prompt_project";
    case UnifiedCommandSource::PromptGlobal:
      return "prompt_global";
    case UnifiedCommandSource::Skill:
      return "skill";
    case UnifiedCommandSource::McpPrompt:
      return "mcp_prompt";
    case UnifiedCommandSource::PluginCommand:
      return "plugin_command";
  }
  return "unknown";
}

std::string to_string(UnifiedCommandKind kind)
{
  switch (kind)
  {
    case UnifiedCommandKind::Backend:
      return "backend";
    case UnifiedCommandKind::PromptTemplate:
      return "prompt_template";
    case UnifiedCommandKind::SkillPrompt:
      return "skill_prompt";
    case UnifiedCommandKind::McpPrompt:
      return "mcp_prompt";
    case UnifiedCommandKind::PluginCommand:
      return "plugin_command";
  }
  return "unknown";
}

CommandRegistryEntry const* find_command_registry_entry(CommandRegistry const& registry, std::string_view line) noexcept
{
  if (!line.starts_with('/'))
    return nullptr;
  auto const token = command_token(line);
  for (auto const& entry : registry.entries)
  {
    if (entry.command == token)
      return &entry;
    if (std::ranges::find(entry.aliases, token) != entry.aliases.end())
      return &entry;
  }
  return nullptr;
}

bool command_registry_contains(runtime::session_ts& unlocked_session, std::string_view line)
{
  if (!line.starts_with('/'))
    return false;
  auto const token = command_token(line);
  if (token.starts_with("/skill:") || token.starts_with("/mcp:") || token.starts_with("/plugin:"))
    return true;

  auto registry = load_command_registry(
      unlocked_session,
      CommandRegistryOptions{
          .include_builtins = true, .include_prompt_commands = true, .include_skills = true, .include_plugin_commands = true, .include_mcp_prompts = false});
  if (find_command_registry_entry(registry, line) != nullptr)
    return true;
  registry = load_command_registry(
      unlocked_session,
      CommandRegistryOptions{
          .include_builtins = false, .include_prompt_commands = false, .include_skills = false, .include_plugin_commands = false, .include_mcp_prompts = true});
  return find_command_registry_entry(registry, line) != nullptr;
}

ava::core::Result<std::string> expand_prompt_command_template(std::string_view template_text, std::string_view argument_text)
{
  auto tokens = parse_argument_tokens(argument_text);
  if (!tokens)
    return std::unexpected(std::move(tokens.error()));
  auto const raw_arguments = core::trim(argument_text);

  std::string output;
  for (std::size_t index = 0; index < template_text.size();)
  {
    if (template_text[index] != '$')
    {
      output.push_back(template_text[index++]);
      continue;
    }
    if (template_text.substr(index, 10) == "$ARGUMENTS")
    {
      output += raw_arguments;
      index += 10;
      continue;
    }
    if (template_text.substr(index, 2) == "$@")
    {
      output += join_tokens(*tokens, 0, std::nullopt);
      index += 2;
      continue;
    }
    if (index + 1 < template_text.size() && std::isdigit(static_cast<unsigned char>(template_text[index + 1])) != 0)
    {
      std::size_t end = index + 1;
      std::size_t number = 0;
      while (end < template_text.size() && std::isdigit(static_cast<unsigned char>(template_text[end])) != 0)
      {
        number = number * 10 + static_cast<std::size_t>(template_text[end] - '0');
        ++end;
      }
      if (number > 0 && number <= tokens->size())
        output += (*tokens)[number - 1];
      index = end;
      continue;
    }
    if (template_text.substr(index, 4) == "${@:")
    {
      auto const close = template_text.find('}', index);
      if (close != std::string_view::npos)
      {
        auto const expression = template_text.substr(index, close - index + 1);
        if (auto slice = parse_argument_slice(expression))
        {
          output += join_tokens(*tokens, slice->first, slice->second);
          index = close + 1;
          continue;
        }
      }
    }
    if (template_text.substr(index, 2) == "${")
    {
      auto const close = template_text.find('}', index);
      if (close != std::string_view::npos)
      {
        auto const expression = template_text.substr(index, close - index + 1);
        if (auto default_value = parse_argument_default(expression))
        {
          if (default_value->first < tokens->size() && !(*tokens)[default_value->first].empty())
            output += (*tokens)[default_value->first];
          else
            output += default_value->second;
          index = close + 1;
          continue;
        }
      }
    }
    if (template_text.substr(index, 2) == "$$")
    {
      output.push_back('$');
      index += 2;
      continue;
    }
    output.push_back(template_text[index++]);
  }
  return output;
}

namespace {

bool valid_command_token(std::string_view command)
{
  if (!command.starts_with('/') || command.size() < 2 || command.size() > runtime::kMaxCommandTokenBytes)
    return false;
  for (char const ch : command)
  {
    auto const byte = static_cast<unsigned char>(ch);
    bool const allowed = std::isalnum(byte) != 0 || ch == '/' || ch == '.' || ch == '_' || ch == '-' || ch == ':';
    if (!allowed || byte < 0x20 || byte == 0x7F)
      return false;
  }
  return true;
}

void load_builtin_commands(RegistryBuilder& builder)
{
  for (auto const& entry : command_catalog())
  {
    add_entry(builder, CommandRegistryEntry{.command = entry.command,
                                            .aliases = entry.aliases,
                                            .description = entry.description,
                                            .hint = entry.hint,
                                            .category = entry.category,
                                            .enabled = entry.enabled,
                                            .disabled_reason = entry.disabled_reason,
                                            .source = UnifiedCommandSource::Builtin,
                                            .kind = UnifiedCommandKind::Backend,
                                            .source_id = "builtin"});
  }
}

void load_prompt_command_dir(RegistryBuilder& builder, std::filesystem::path const& root, UnifiedCommandSource source, std::string source_scope)
{
  auto files = runtime::markdown_files(root, builder.registry.diagnostics, source);
  for (auto const& file : files)
  {
    auto name = runtime::command_name_for_file(root, file);
    if (!name)
    {
      add_diagnostic(builder,
                     CommandRegistryDiagnostic{.source = to_string(source), .path = file, .message = "command file name does not form a safe slash command"});
      continue;
    }
    auto content = ava::context::read_resource_file(file, {.max_bytes = runtime::kMaxCommandFileBytes, .resource_description = "command file"});
    if (!content)
    {
      add_diagnostic(builder,
                     CommandRegistryDiagnostic{.command = "/" + *name, .source = to_string(source), .path = file, .message = content.error().format()});
      continue;
    }
    auto parsed = ava::context::parse_markdown(*content);
    auto description = ava::context::markdown_field(parsed, "description");
    auto hint = ava::context::markdown_field(parsed, "argument-hint");
    if (hint.empty())
      hint = ava::context::markdown_field(parsed, "argument_hint");
    if (hint.empty())
      hint = ava::context::markdown_field(parsed, "hint");
    auto body = ava::context::markdown_field(parsed, "template");
    if (body.empty())
      body = std::move(parsed.body);
    if (core::trim_view(body).empty())
    {
      add_diagnostic(builder,
                     CommandRegistryDiagnostic{.command = "/" + *name, .source = to_string(source), .path = file, .message = "command template is empty"});
      continue;
    }
    add_entry(builder, CommandRegistryEntry{.command = "/" + *name,
                                            .description = description.empty() ? "Prompt command " + *name : description,
                                            .hint = std::move(hint),
                                            .category = "Prompts",
                                            .source = source,
                                            .kind = UnifiedCommandKind::PromptTemplate,
                                            .source_id = std::move(*name),
                                            .source_path = file,
                                            .source_scope = source_scope,
                                            .template_text = std::move(body)});
  }
}

void load_prompt_commands(RegistryBuilder& builder, runtime::session_ts const& unlocked_session, runtime::ExtensionResourcePolicy const& policy)
{
  CRITICAL_AREA_BEGIN_CR(session);
  auto workspace_dir = session_r->workspace_dir();
  auto ava_config_dir = session_r->paths().ava_config_dir;
  CRITICAL_AREA_END_R(session);
  if (policy.include_project_resources)
  {
    load_prompt_command_dir(builder, workspace_dir / ".ava" / "commands", UnifiedCommandSource::PromptProject, "project");
    load_prompt_command_dir(builder, workspace_dir / ".ava" / "command", UnifiedCommandSource::PromptProject, "project");
  }
  load_prompt_command_dir(builder, ava_config_dir / "commands", UnifiedCommandSource::PromptGlobal, "global");
  load_prompt_command_dir(builder, ava_config_dir / "command", UnifiedCommandSource::PromptGlobal, "global");
}

void load_plugin_commands(RegistryBuilder& builder, runtime::session_ts const& unlocked_session, runtime::ExtensionResourcePolicy const& policy)
{
  auto diagnostics = ava::plugin::collect_plugin_diagnostics(policy.plugin_discovery, policy.plugin_enablement_file, runtime::session_ts::crat(unlocked_session)->workspace_dir());
  for (auto const& failure : diagnostics.failures)
  {
    add_diagnostic(builder, CommandRegistryDiagnostic{.source = to_string(UnifiedCommandSource::PluginCommand),
                                                      .path = failure.path,
                                                      .message = failure.details.empty() ? failure.message : failure.details});
  }

  for (auto const& status : diagnostics.plugins)
  {
    auto const& manifest = status.plugin.manifest;
    for (auto const& command : manifest.contributes.commands)
    {
      if (!runtime::valid_command_segment(command.name) || !runtime::valid_command_segment(manifest.id))
      {
        add_diagnostic(builder, CommandRegistryDiagnostic{.source = to_string(UnifiedCommandSource::PluginCommand),
                                                          .source_id = manifest.id,
                                                          .path = manifest.path,
                                                          .message = "plugin command id does not form a safe slash command"});
        continue;
      }
      CommandRegistryEntry entry{.command = namespaced_command("plugin", manifest.id, command.name),
                                 .description = command.description.empty() ? "Plugin command " + command.name : command.description,
                                 .category = "Plugins",
                                 .enabled = status.enabled,
                                 .disabled_reason = status.enabled ? "" : "plugin is disabled",
                                 .source = UnifiedCommandSource::PluginCommand,
                                 .kind = UnifiedCommandKind::PluginCommand,
                                 .source_id = manifest.id,
                                 .source_path = manifest.path,
                                 .source_scope = std::string(ava::plugin::to_string(status.plugin.scope)),
                                 .plugin_id = manifest.id,
                                 .plugin_command_name = command.name};
      add_entry(builder, entry);
      entry.command = "/" + command.name;
      add_entry(builder, std::move(entry));
    }
  }
}

void load_skill_commands(RegistryBuilder& builder, runtime::session_ts const& unlocked_session, runtime::ExtensionResourcePolicy const& policy)
{
  CRITICAL_AREA_BEGIN_CR(session);
  auto workspace_dir = session_r->workspace_dir();
  CRITICAL_AREA_END_R(session);
  auto plugin_diagnostics = ava::plugin::collect_plugin_diagnostics(policy.plugin_discovery, policy.plugin_enablement_file, workspace_dir);
  auto loaded = ava::context::load_skills(ava::context::SkillLoadOptions{
      .workspace_root = workspace_dir,
      .global_skill_dirs = policy.global_skill_dirs,
      .project_skill_dirs = policy.project_skill_dirs,
      .declared_skill_files = ava::plugin::declared_plugin_skill_files(plugin_diagnostics),
      .include_project_skills = policy.include_project_resources,
  });
  for (auto const& diagnostic : loaded.diagnostics)
  {
    add_diagnostic(builder,
                   CommandRegistryDiagnostic{.source = to_string(UnifiedCommandSource::Skill), .path = diagnostic.path, .message = diagnostic.message});
  }
  for (auto const& skill : loaded.skills)
  {
    CommandRegistryEntry entry{.command = namespaced_command("skill", skill.name),
                               .description = skill.description,
                               .category = "Skills",
                               .source = UnifiedCommandSource::Skill,
                               .kind = UnifiedCommandKind::SkillPrompt,
                               .source_id = skill.name,
                               .source_path = skill.path,
                               .source_scope = ava::context::to_string(skill.source_type),
                               .template_text = skill.content,
                               .skill_name = skill.name};
    add_entry(builder, entry);
    entry.command = "/" + skill.name;
    add_entry(builder, std::move(entry));
  }
}

}  // namespace

void add_diagnostic(RegistryBuilder& builder, CommandRegistryDiagnostic diagnostic)
{
  builder.registry.diagnostics.push_back(std::move(diagnostic));
}

bool add_entry(RegistryBuilder& builder, CommandRegistryEntry entry)
{
  std::vector<std::string> tokens;
  tokens.push_back(entry.command);
  for (auto const& alias : entry.aliases) tokens.push_back(alias);
  for (auto const& token : tokens)
  {
    if (!valid_command_token(token))
    {
      add_diagnostic(builder, CommandRegistryDiagnostic{.command = token,
                                                        .source = to_string(entry.source),
                                                        .source_id = entry.source_id,
                                                        .path = entry.source_path,
                                                        .message = "command token is invalid"});
      return false;
    }
    auto const existing = builder.occupied.find(token);
    if (existing == builder.occupied.end())
      continue;
    auto const& winner = builder.registry.entries[existing->second];
    add_diagnostic(builder, CommandRegistryDiagnostic{.command = token,
                                                      .source = to_string(entry.source),
                                                      .source_id = entry.source_id,
                                                      .path = entry.source_path,
                                                      .message = "command collision; existing command kept",
                                                      .winner_source = to_string(winner.source),
                                                      .winner_source_id = winner.source_id,
                                                      .winner_path = winner.source_path});
    return false;
  }

  auto const index = builder.registry.entries.size();
  builder.registry.entries.push_back(std::move(entry));
  for (auto& token : tokens) builder.occupied.emplace(std::move(token), index);
  return true;
}

std::string namespaced_command(std::string_view prefix, std::string_view id, std::string_view name)
{
  std::string command = "/";
  command += prefix;
  command += ':';
  command += id;
  if (!name.empty())
  {
    command += ':';
    command += name;
  }
  return command;
}

CommandRegistry load_command_registry(runtime::session_ts& unlocked_session, CommandRegistryOptions options)
{
  RegistryBuilder builder;
  auto const resource_policy = make_extension_resource_policy_1(unlocked_session);
  if (options.include_builtins)
    load_builtin_commands(builder);
  if (options.include_prompt_commands)
    load_prompt_commands(builder, unlocked_session, resource_policy);
  if (options.include_plugin_commands)
    load_plugin_commands(builder, unlocked_session, resource_policy);
  if (options.include_mcp_prompts)
    load_mcp_prompt_commands(builder, unlocked_session, options, resource_policy);
  if (options.include_skills)
    load_skill_commands(builder, unlocked_session, resource_policy);
  return std::move(builder.registry);
}

}  // namespace ava::app
