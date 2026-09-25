#include "sys.h"
#include "ava/agent/subagent_config.h"
#include "ava/config/xdg_paths.h"
#include "ava/context/markdown_resource.h"
#include "ava/core/error.h"
#include "ava/core/string_utils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <map>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace ava::agent {
namespace {

constexpr std::size_t kMaxSubagentNameBytes = 128;
constexpr std::size_t kMaxSubagentDescriptionBytes = 1024;
constexpr std::size_t kMaxSubagentPromptBytes = 64 * 1024;
constexpr std::size_t kMaxSubagents = 128;
constexpr std::size_t kMaxDiagnostics = 128;
constexpr std::size_t kMaxSubagentToolIterations = 1000;

void add_diagnostic(std::vector<SubagentDiagnostic>& diagnostics, std::filesystem::path path, std::string message,
                    std::optional<std::string> agent_name = std::nullopt, bool blocks_primary_selection = false)
{
  if (diagnostics.size() < kMaxDiagnostics)
    diagnostics.push_back(SubagentDiagnostic{
        .path = std::move(path), .message = std::move(message), .agent_name = std::move(agent_name), .blocks_primary_selection = blocks_primary_selection});
}

std::optional<std::string> diagnostic_agent_name(std::filesystem::path const& path, std::optional<std::string> configured_name = std::nullopt)
{
  auto name = core::trim(configured_name.value_or(path.stem().string()));
  return valid_subagent_name(name) ? std::optional<std::string>(std::move(name)) : std::nullopt;
}

std::optional<std::filesystem::path> home_dir()
{
  if (auto const* home = std::getenv("HOME"); home != nullptr && *home != '\0')
    return std::filesystem::path(home);
  return std::nullopt;
}

bool has_control_byte(std::string_view value)
{
  return std::ranges::any_of(value, [](char ch) {
    auto const byte = static_cast<unsigned char>(ch);
    return byte < 0x20 && ch != '\n' && ch != '\r' && ch != '\t';
  });
}

std::string xml_escape(std::string_view value)
{
  std::string escaped;
  escaped.reserve(value.size());
  for (char const ch : value)
  {
    switch (ch)
    {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&apos;";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

std::optional<std::string> field(std::map<std::string, std::string> const& frontmatter, std::string_view name)
{
  auto const it = frontmatter.find(std::string(name));
  if (it == frontmatter.end())
    return std::nullopt;
  return it->second;
}

std::optional<SubagentToolPreset> parse_tool_preset(std::string value)
{
  value = core::trim(value);
  std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (value == "read-only" || value == "read_only" || value == "readonly" || value == "explore")
    return SubagentToolPreset::ReadOnly;
  if (value.empty() || value == "inherit" || value == "inherited" || value == "default")
    return SubagentToolPreset::Inherit;
  return std::nullopt;
}

bool hidden_field(std::map<std::string, std::string> const& frontmatter)
{
  auto value = field(frontmatter, "hidden");
  if (!value)
    return false;
  auto text = core::trim(*value);
  std::ranges::transform(text, text.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return text == "true" || text == "yes" || text == "1";
}

std::size_t frontmatter_field_count(std::string_view content, std::string_view field_name)
{
  if (!(content.starts_with("---\n") || content.starts_with("---\r\n")))
    return 0;
  auto const body_start = content.starts_with("---\r\n") ? 5U : 4U;
  auto const delimiter = content.find("\n---", body_start);
  if (delimiter == std::string_view::npos)
    return 0;
  auto const frontmatter = content.substr(body_start, delimiter - body_start);
  std::size_t count = 0;
  for (std::size_t line_start = 0; line_start <= frontmatter.size();)
  {
    auto const line_end = frontmatter.find('\n', line_start);
    auto line = frontmatter.substr(line_start, line_end == std::string_view::npos ? std::string_view::npos : line_end - line_start);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    auto const colon = line.find(':');
    if (colon != std::string_view::npos && core::trim_view(line.substr(0, colon)) == field_name)
      ++count;
    if (line_end == std::string_view::npos)
      break;
    line_start = line_end + 1;
  }
  return count;
}

std::optional<std::optional<std::size_t>> parse_max_tool_iterations(std::map<std::string, std::string> const& frontmatter)
{
  auto value = field(frontmatter, "max_tool_iterations");
  if (!value)
    return std::optional<std::size_t>{};
  auto const text = core::trim_view(*value);
  std::size_t parsed = 0;
  auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size() || parsed == 0 || parsed > kMaxSubagentToolIterations)
    return std::nullopt;
  return std::optional<std::size_t>{parsed};
}

void add_or_replace_definition(std::vector<SubagentDefinition>& definitions, std::vector<SubagentDiagnostic>& diagnostics, SubagentDefinition definition,
                               bool protect_builtins)
{
  auto const existing = std::ranges::find_if(definitions, [&](SubagentDefinition const& item) { return item.name == definition.name; });
  if (existing == definitions.end())
  {
    if (definitions.size() < kMaxSubagents)
      definitions.push_back(std::move(definition));
    else
      add_diagnostic(diagnostics, definition.path, "too many agent definitions; entry ignored");
    return;
  }
  if (protect_builtins && existing->provenance == SubagentDefinitionProvenance::Builtin)
  {
    add_diagnostic(diagnostics, definition.path, "subagent name collides with a builtin: " + definition.name);
    return;
  }
  *existing = std::move(definition);
}

void invalidate_primary(std::vector<SubagentDefinition>& primary_agents, std::vector<std::string>& invalid_primary_agents,
                        std::optional<std::string> const& name)
{
  if (!name)
    return;
  std::erase_if(primary_agents, [&](SubagentDefinition const& definition) { return definition.name == *name; });
  if (std::ranges::find(invalid_primary_agents, *name) == invalid_primary_agents.end())
    invalid_primary_agents.push_back(*name);
}

void load_subagent_file(std::vector<SubagentDefinition>& subagents, std::vector<SubagentDefinition>& primary_agents,
                        std::vector<std::string>& invalid_primary_agents, std::vector<SubagentDiagnostic>& diagnostics, std::filesystem::path const& path,
                        SubagentDefinitionProvenance provenance, std::size_t max_file_bytes)
{
  auto content = context::read_resource_file(path, {.max_bytes = max_file_bytes, .resource_description = "subagent file"});
  if (!content)
  {
    auto const candidate_name = diagnostic_agent_name(path);
    add_diagnostic(diagnostics, path, content.error().format(), candidate_name, true);
    invalidate_primary(primary_agents, invalid_primary_agents, candidate_name);
    return;
  }
  auto parsed = context::parse_markdown(*content);
  auto name = field(parsed.frontmatter, "name").value_or(path.stem().string());
  name = core::trim(name);
  auto const candidate_name = diagnostic_agent_name(path, name);
  auto mode = field(parsed.frontmatter, "mode").value_or("subagent");
  mode = core::trim(mode);
  std::ranges::transform(mode, mode.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (mode != "subagent" && mode != "primary" && mode != "all")
  {
    add_diagnostic(diagnostics, path, "agent mode is invalid: " + mode, candidate_name, true);
    invalidate_primary(primary_agents, invalid_primary_agents, candidate_name);
    return;
  }

  auto description = field(parsed.frontmatter, "description").value_or("");
  description = core::trim(description);
  if (!valid_subagent_name(name))
  {
    auto const stem_name = diagnostic_agent_name(path);
    add_diagnostic(diagnostics, path, "agent name is invalid", stem_name, true);
    invalidate_primary(primary_agents, invalid_primary_agents, stem_name);
    return;
  }
  if (description.empty() || description.size() > kMaxSubagentDescriptionBytes || has_control_byte(description))
  {
    bool const primary_mode = mode == "primary" || mode == "all";
    add_diagnostic(diagnostics, path, "agent description is missing or invalid", candidate_name, primary_mode);
    if (primary_mode)
      invalidate_primary(primary_agents, invalid_primary_agents, candidate_name);
    return;
  }
  if (parsed.body.size() > kMaxSubagentPromptBytes || has_control_byte(parsed.body))
  {
    bool const primary_mode = mode == "primary" || mode == "all";
    add_diagnostic(diagnostics, path, "agent prompt is too large or invalid", candidate_name, primary_mode);
    if (primary_mode)
      invalidate_primary(primary_agents, invalid_primary_agents, candidate_name);
    return;
  }
  auto tools = parse_tool_preset(field(parsed.frontmatter, "tools").value_or(""));
  bool const valid_primary_tools = tools.has_value();
  if (!valid_primary_tools)
  {
    bool const primary_mode = mode == "primary" || mode == "all";
    add_diagnostic(diagnostics, path, "agent tools preset is invalid", candidate_name, primary_mode);
    if (primary_mode)
      invalidate_primary(primary_agents, invalid_primary_agents, candidate_name);
    tools = SubagentToolPreset::Inherit;
  }
  auto max_tool_iterations = parse_max_tool_iterations(parsed.frontmatter);
  if (!max_tool_iterations || frontmatter_field_count(*content, "max_tool_iterations") > 1)
  {
    bool const primary_mode = mode == "primary" || mode == "all";
    add_diagnostic(diagnostics, path, "agent max_tool_iterations must be one integer from 1 through 1000", candidate_name, primary_mode);
    if (primary_mode)
      invalidate_primary(primary_agents, invalid_primary_agents, candidate_name);
    return;
  }
  SubagentDefinition definition{.name = std::move(name),
                                .description = std::move(description),
                                .system_prompt = std::move(parsed.body),
                                .tool_preset = *tools,
                                .max_tool_iterations = *max_tool_iterations,
                                .hidden = hidden_field(parsed.frontmatter),
                                .provenance = provenance,
                                .path = path};
  if (mode == "subagent" || mode == "all")
    add_or_replace_definition(subagents, diagnostics, definition, true);
  if ((mode == "primary" || mode == "all") && valid_primary_tools)
  {
    auto const primary_name = definition.name;
    add_or_replace_definition(primary_agents, diagnostics, std::move(definition), false);
    if (find_subagent(primary_agents, primary_name))
      std::erase(invalid_primary_agents, primary_name);
  }
}

void discover_from_root(std::vector<SubagentDefinition>& subagents, std::vector<SubagentDefinition>& primary_agents,
                        std::vector<std::string>& invalid_primary_agents, std::vector<SubagentDiagnostic>& diagnostics, std::filesystem::path const& root,
                        SubagentDefinitionProvenance provenance, std::size_t max_file_bytes)
{
  if (root.empty())
    return;
  std::error_code exists_error;
  if (!std::filesystem::exists(root, exists_error))
    return;
  if (exists_error)
  {
    add_diagnostic(diagnostics, root, "failed to inspect agent directory");
    return;
  }
  std::vector<std::filesystem::path> definition_paths;
  std::error_code iter_error;
  for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, iter_error), end;
       !iter_error && it != end; it.increment(iter_error))
  {
    std::error_code entry_error;
    if (it->is_symlink(entry_error) || entry_error)
      continue;
    if (!it->is_regular_file(entry_error) || entry_error)
      continue;
    if (it->path().extension() != ".md")
      continue;
    definition_paths.push_back(it->path());
  }
  if (iter_error)
    add_diagnostic(diagnostics, root, "failed to iterate agent directory");
  std::ranges::sort(definition_paths);
  for (auto const& path : definition_paths)
    load_subagent_file(subagents, primary_agents, invalid_primary_agents, diagnostics, path, provenance, max_file_bytes);
}

}  // namespace

std::string_view to_string(SubagentDefinitionProvenance provenance) noexcept
{
  switch (provenance)
  {
    case SubagentDefinitionProvenance::Unknown:
      return "unknown";
    case SubagentDefinitionProvenance::Builtin:
      return "builtin";
    case SubagentDefinitionProvenance::Global:
      return "global";
    case SubagentDefinitionProvenance::Project:
      return "project";
  }
  return "unknown";
}

bool valid_subagent_name(std::string_view name)
{
  if (name.empty() || name.size() > kMaxSubagentNameBytes)
    return false;
  bool last_was_separator = false;
  for (char const ch : name)
  {
    auto const byte = static_cast<unsigned char>(ch);
    bool const allowed = std::isalnum(byte) != 0 || ch == '.' || ch == '_' || ch == '-';
    if (!allowed)
      return false;
    bool const separator = ch == '.' || ch == '_' || ch == '-';
    if (separator && last_was_separator)
      return false;
    last_was_separator = separator;
  }
  return !last_was_separator;
}

std::vector<SubagentDefinition> builtin_subagents()
{
  return {
      SubagentDefinition{.name = "general",
                         .description = "General-purpose subagent for delegated coding, analysis, and verification tasks.",
                         .system_prompt = "You are AVA's general subagent. Complete the delegated task and return only the result needed by the parent agent.",
                         .tool_preset = SubagentToolPreset::Inherit,
                         .provenance = SubagentDefinitionProvenance::Builtin},
      SubagentDefinition{.name = "explore",
                         .description = "Read-only subagent for fast codebase exploration and concise findings.",
                         .system_prompt = "You are AVA's explore subagent. Read, list, and search files only. Return concise findings to the parent agent.",
                         .tool_preset = SubagentToolPreset::ReadOnly,
                         .provenance = SubagentDefinitionProvenance::Builtin}};
}

std::vector<std::filesystem::path> default_global_subagent_dirs()
{
  auto const paths = ava::config::xdg_paths();
  std::vector<std::filesystem::path> dirs{paths.ava_config_dir / "agents", paths.ava_config_dir / "agent"};
  if (auto home = home_dir())
  {
    dirs.push_back(*home / ".agents" / "agents");
    dirs.push_back(*home / ".agents" / "agent");
    dirs.push_back(*home / ".claude" / "agents");
    dirs.push_back(*home / ".claude" / "agent");
  }
  return dirs;
}

std::vector<std::filesystem::path> default_project_subagent_dirs(std::filesystem::path const& workspace_root)
{
  if (workspace_root.empty())
    return {};
  return {workspace_root / ".ava" / "agents",   workspace_root / ".ava" / "agent",     workspace_root / ".agents" / "agents",
          workspace_root / ".agents" / "agent", workspace_root / ".claude" / "agents", workspace_root / ".claude" / "agent"};
}

SubagentLoadResult load_subagents(SubagentLoadOptions options)
{
  if (options.global_agent_dirs.empty())
    options.global_agent_dirs = default_global_subagent_dirs();
  if (options.include_project_agents && options.project_agent_dirs.empty())
    options.project_agent_dirs = default_project_subagent_dirs(options.workspace_root);
  if (options.max_file_bytes == 0)
    options.max_file_bytes = 64 * 1024;

  SubagentLoadResult result;
  result.subagents = builtin_subagents();
  for (auto const& dir : options.global_agent_dirs)
    discover_from_root(result.subagents, result.primary_agents, result.invalid_primary_agents, result.diagnostics, dir, SubagentDefinitionProvenance::Global,
                       options.max_file_bytes);
  if (options.include_project_agents)
  {
    for (auto const& dir : options.project_agent_dirs)
      discover_from_root(result.subagents, result.primary_agents, result.invalid_primary_agents, result.diagnostics, dir, SubagentDefinitionProvenance::Project,
                         options.max_file_bytes);
  }
  auto by_name = [](SubagentDefinition const& left, SubagentDefinition const& right) { return left.name < right.name; };
  std::ranges::sort(result.subagents, by_name);
  std::ranges::sort(result.primary_agents, by_name);
  return result;
}

SubagentDefinition const* find_subagent(std::vector<SubagentDefinition> const& subagents, std::string_view name)
{
  auto const it = std::ranges::find_if(subagents, [name](SubagentDefinition const& subagent) { return subagent.name == name; });
  return it == subagents.end() ? nullptr : &*it;
}

ava::core::Result<SubagentDefinition> resolve_primary_agent(SubagentLoadResult const& loaded, std::string_view name)
{
  if (!valid_subagent_name(name))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "selected primary agent name is invalid");
    error.with_context("hint", "use a name containing only letters, digits, '.', '_', or '-'");
    return std::unexpected(std::move(error));
  }
  if (std::ranges::find(loaded.invalid_primary_agents, name) != loaded.invalid_primary_agents.end())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "selected primary agent definition failed validation");
    error.with_context("agent", std::string(name));
    error.with_context("hint", "fix or remove the matching definition, then start or replace the session");
    return std::unexpected(std::move(error));
  }
  if (auto const* definition = find_subagent(loaded.primary_agents, name))
    return *definition;

  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "selected primary agent is unavailable or invalid");
  error.with_context("agent", std::string(name));
  error.with_context("available", primary_agent_names_csv(loaded.primary_agents));
  error.with_context("hint", "define it with mode: primary or mode: all in a trusted agent root");
  return std::unexpected(std::move(error));
}

std::string subagent_names_csv(std::vector<SubagentDefinition> const& subagents)
{
  std::string names;
  for (auto const& subagent : subagents)
  {
    if (subagent.hidden)
      continue;
    if (!names.empty())
      names += ", ";
    names += subagent.name;
  }
  return names.empty() ? "none" : names;
}

std::string primary_agent_names_csv(std::vector<SubagentDefinition> const& primary_agents)
{
  return subagent_names_csv(primary_agents);
}

ToolVisibilityOptions narrow_tool_visibility_to_read_only(ToolVisibilityOptions visibility)
{
  std::vector<std::string> const read_only_tools{"read_file", "read", "list_directory", "ls", "glob", "find", "grep"};
  if (!visibility.included_tools.empty())
  {
    std::erase_if(visibility.included_tools, [&](std::string const& name) { return std::ranges::find(read_only_tools, name) == read_only_tools.end(); });
    if (visibility.included_tools.empty())
      visibility.mode = ToolVisibilityMode::NoTools;
    return visibility;
  }

  if (visibility.mode == ToolVisibilityMode::Default)
    visibility.included_tools = {"read_file", "list_directory", "glob", "grep"};
  else
    visibility.mode = ToolVisibilityMode::NoTools;
  return visibility;
}

std::string format_available_subagents_for_prompt(std::vector<SubagentDefinition> const& subagents)
{
  std::string output;
  for (auto const& subagent : subagents)
  {
    if (subagent.hidden)
      continue;
    if (output.empty())
    {
      output =
          "\n\n# Available Subagents\n"
          "Use the task tool to delegate self-contained work to a subagent when parallel exploration or isolation helps.\n"
          "<available_subagents>\n";
    }
    output += "  <subagent>\n";
    output += "    <name>" + xml_escape(subagent.name) + "</name>\n";
    output += "    <description>" + xml_escape(subagent.description) + "</description>\n";
    output += "    <tools>" + std::string(subagent.tool_preset == SubagentToolPreset::ReadOnly ? "read-only" : "inherited") + "</tools>\n";
    output += "  </subagent>\n";
  }
  if (!output.empty())
    output += "</available_subagents>\n";
  return output;
}

}  // namespace ava::agent
