#include "sys.h"
#include "command_registry.h"
#include "runtime/ExtensionResourcePolicy.h"
#include "runtime/command_names.h"
#include "runtime/markdown_files.h"
#include "runtime_prompt.h"
#include "ava/agent/subagent_config.h"
#include "ava/plugin/diagnostics.h"
#include "ava/plugin/static_resources.h"
#include "ava/config/prompt_config.h"
#include "ava/context/markdown_resource.h"
#include "ava/context/skill_loader.h"
#include "ava/core/error.h"
#include "ava/core/fingerprint.h"
#include "ava/core/path.h"
#include "ava/core/string_utils.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::app::runtime {

namespace {

constexpr std::size_t kMaxRuntimeFreshnessBytes = 256 * 1024;
constexpr std::size_t kMaxPluginResourceFreshnessBytes = 64 * 1024;

struct RuntimePromptResource
{
  FreshnessSourceKind kind = FreshnessSourceKind::SystemPrompt;
  std::string scope;
  std::string name;
  std::filesystem::path path;
  std::string text;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct LoadedPluginPromptResource
{
  std::string scope;
  std::string plugin_id;
  std::string name;
  std::filesystem::path path;
  std::string text;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct LoadedPluginSkillResource
{
  std::string scope;
  std::string plugin_id;
  std::string name;
  std::size_t byte_count = 0;
  std::uint64_t content_fingerprint = 0;
  ava::context::LoadedSkill skill;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginResourceLoadFailure
{
  FreshnessSourceKind kind = FreshnessSourceKind::PluginPrompt;
  std::string scope;
  std::string plugin_id;
  std::string name;
  std::filesystem::path path;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginRuntimeResources
{
  ava::plugin::PluginDiagnostics diagnostics;
  std::vector<LoadedPluginPromptResource> prompts;
  std::vector<LoadedPluginSkillResource> skills;
  std::vector<PluginResourceLoadFailure> failures;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

void add_freshness_file(std::vector<FreshnessSourceMetadata>& sources, FreshnessSourceKind kind, std::string scope, std::string source_id, std::string name,
                        std::filesystem::path const& path, std::size_t max_bytes = kMaxRuntimeFreshnessBytes)
{
  auto content = ava::context::read_resource_file(path, {.max_bytes = max_bytes, .resource_description = "freshness source"});
  if (!content)
    return;
  sources.push_back(FreshnessSourceMetadata{.kind = kind,
                                            .scope = std::move(scope),
                                            .source_id = std::move(source_id),
                                            .name = std::move(name),
                                            .path = ava::core::normalized_absolute_path(path),
                                            .byte_count = content->size(),
                                            .content_fingerprint = ava::core::content_fingerprint(*content)});
}

void add_prompt_command_source_files(std::vector<PromptCommandSourceFile>& sources, std::filesystem::path const& root, UnifiedCommandSource source,
                                     std::string_view scope)
{
  std::vector<CommandRegistryDiagnostic> diagnostics;
  auto files = markdown_files(root, diagnostics, source);
  for (auto const& file : files)
  {
    auto name = command_name_for_file(root, file);
    if (!name)
      continue;
    auto content = ava::context::read_resource_file(file, {.max_bytes = kMaxCommandFileBytes, .resource_description = "command file"});
    if (!content)
      continue;
    auto parsed = ava::context::parse_markdown(*content);
    auto body = ava::context::markdown_field(parsed, "template");
    if (body.empty())
      body = std::move(parsed.body);
    if (core::trim_view(body).empty())
      continue;
    sources.push_back(PromptCommandSourceFile{.command_name = std::move(*name),
                                              .scope = std::string(scope),
                                              .path = file,
                                              .byte_count = content->size(),
                                              .content_fingerprint = ava::core::content_fingerprint(*content)});
  }
}

std::vector<PromptCommandSourceFile> prompt_command_source_files(std::filesystem::path const& workspace_dir, ava::config::XdgPaths const& paths,
                                                                 bool include_project_commands)
{
  std::vector<PromptCommandSourceFile> sources;
  if (include_project_commands)
  {
    add_prompt_command_source_files(sources, workspace_dir / ".ava" / "commands", UnifiedCommandSource::PromptProject, "project");
    add_prompt_command_source_files(sources, workspace_dir / ".ava" / "command", UnifiedCommandSource::PromptProject, "project");
  }
  add_prompt_command_source_files(sources, paths.ava_config_dir / "commands", UnifiedCommandSource::PromptGlobal, "global");
  add_prompt_command_source_files(sources, paths.ava_config_dir / "command", UnifiedCommandSource::PromptGlobal, "global");
  std::ranges::sort(sources, [](PromptCommandSourceFile const& left, PromptCommandSourceFile const& right) {
    if (left.scope != right.scope)
      return left.scope < right.scope;
    return left.command_name < right.command_name;
  });
  return sources;
}

void add_prompt_command_freshness_sources(std::vector<FreshnessSourceMetadata>& sources, ava::config::XdgPaths const& paths,
                                          std::filesystem::path const& workspace_dir, bool include_project_resources)
{
  for (auto const& command : prompt_command_source_files(workspace_dir, paths, include_project_resources))
  {
    sources.push_back(FreshnessSourceMetadata{.kind = FreshnessSourceKind::PromptCommand,
                                              .scope = command.scope,
                                              .source_id = command.command_name,
                                              .name = command.command_name,
                                              .path = ava::core::normalized_absolute_path(command.path),
                                              .byte_count = command.byte_count,
                                              .content_fingerprint = command.content_fingerprint});
  }
}

ava::core::Result<std::optional<RuntimePromptResource>> load_prompt_resource(ava::config::XdgPaths const& paths, std::filesystem::path const& workspace_dir,
                                                                             bool include_project_resources, FreshnessSourceKind kind, std::string name)
{
  std::filesystem::path selected_path;
  std::string scope;
  auto const project_path = workspace_dir / ".ava" / name;
  if (include_project_resources && std::filesystem::exists(project_path))
  {
    selected_path = project_path;
    scope = "project";
  }
  else
  {
    auto const global_path = paths.ava_config_dir / name;
    if (!std::filesystem::exists(global_path))
      return std::optional<RuntimePromptResource>{};
    selected_path = global_path;
    scope = "global";
  }

  auto text = ava::context::read_resource_file(selected_path, {.max_bytes = kMaxRuntimeFreshnessBytes, .resource_description = "freshness source"});
  if (!text)
    return std::unexpected(std::move(text.error()));
  return RuntimePromptResource{
      .kind = kind, .scope = std::move(scope), .name = std::move(name), .path = ava::core::normalized_absolute_path(selected_path), .text = std::move(*text)};
}

void add_prompt_resource_freshness_source(std::vector<FreshnessSourceMetadata>& sources, RuntimePromptResource const& resource)
{
  sources.push_back(FreshnessSourceMetadata{.kind = resource.kind,
                                            .scope = resource.scope,
                                            .source_id = resource.name,
                                            .name = resource.name,
                                            .path = resource.path,
                                            .byte_count = resource.text.size(),
                                            .content_fingerprint = ava::core::content_fingerprint(resource.text)});
}

void add_inline_prompt_freshness_source(std::vector<FreshnessSourceMetadata>& sources, FreshnessSourceKind kind, std::string source_id, std::string name,
                                        std::string_view text)
{
  sources.push_back(FreshnessSourceMetadata{.kind = kind,
                                            .scope = "cli",
                                            .source_id = std::move(source_id),
                                            .name = std::move(name),
                                            .path = {},
                                            .byte_count = text.size(),
                                            .content_fingerprint = ava::core::content_fingerprint(text)});
}

ava::core::Result<LoadedPluginPromptResource> load_plugin_prompt_resource(ava::plugin::PluginStatus const& status,
                                                                          ava::plugin::PluginResourceContribution const& resource)
{
  auto const& manifest = status.plugin.manifest;
  auto loaded = ava::plugin::load_plugin_static_resource(manifest, resource, kMaxPluginResourceFreshnessBytes);
  if (!loaded)
    return std::unexpected(std::move(loaded.error()));
  return LoadedPluginPromptResource{.scope = std::string(ava::plugin::to_string(status.plugin.scope)),
                                    .plugin_id = manifest.id,
                                    .name = resource.name,
                                    .path = std::move(loaded->path),
                                    .text = std::move(loaded->content)};
}

ava::core::Result<LoadedPluginSkillResource> load_plugin_skill_resource(ava::plugin::PluginStatus const& status,
                                                                        ava::plugin::PluginResourceContribution const& resource)
{
  auto const& manifest = status.plugin.manifest;
  auto loaded = ava::plugin::load_plugin_static_resource(manifest, resource, kMaxPluginResourceFreshnessBytes);
  if (!loaded)
    return std::unexpected(std::move(loaded.error()));
  auto const byte_count = loaded->content.size();
  auto const fingerprint = ava::core::content_fingerprint(loaded->content);
  auto skill = ava::context::load_declared_skill_content(ava::context::DeclaredSkillFileOptions{.path = loaded->path,
                                                                                                .name = resource.name,
                                                                                                .description = resource.description,
                                                                                                .source_type = ava::context::SkillSourceType::Plugin,
                                                                                                .preloaded_content = std::nullopt,
                                                                                                .max_file_bytes = kMaxPluginResourceFreshnessBytes},
                                                         std::move(loaded->content));
  if (!skill)
    return std::unexpected(std::move(skill.error()));
  return LoadedPluginSkillResource{.scope = std::string(ava::plugin::to_string(status.plugin.scope)),
                                   .plugin_id = manifest.id,
                                   .name = resource.name,
                                   .byte_count = byte_count,
                                   .content_fingerprint = fingerprint,
                                   .skill = std::move(*skill)};
}

PluginRuntimeResources load_plugin_runtime_resources(ExtensionResourcePolicy const& policy, std::filesystem::path const& workspace_dir)
{
  PluginRuntimeResources resources;
  resources.diagnostics = ava::plugin::collect_plugin_diagnostics(policy.plugin_discovery, policy.plugin_enablement_file, workspace_dir);
  for (auto const& status : resources.diagnostics.plugins)
  {
    if (!status.enabled)
      continue;
    auto const& manifest = status.plugin.manifest;
    for (auto const& prompt : manifest.contributes.prompts)
    {
      auto loaded = load_plugin_prompt_resource(status, prompt);
      if (loaded)
        resources.prompts.push_back(std::move(*loaded));
      else
        resources.failures.push_back(PluginResourceLoadFailure{.kind = FreshnessSourceKind::PluginPrompt,
                                                               .scope = std::string(ava::plugin::to_string(status.plugin.scope)),
                                                               .plugin_id = manifest.id,
                                                               .name = prompt.name,
                                                               .path = ava::plugin::plugin_static_resource_display_path(manifest, prompt)});
    }
    for (auto const& skill : manifest.contributes.skills)
    {
      auto loaded = load_plugin_skill_resource(status, skill);
      if (loaded)
        resources.skills.push_back(std::move(*loaded));
      else
        resources.failures.push_back(PluginResourceLoadFailure{.kind = FreshnessSourceKind::PluginSkill,
                                                               .scope = std::string(ava::plugin::to_string(status.plugin.scope)),
                                                               .plugin_id = manifest.id,
                                                               .name = skill.name,
                                                               .path = ava::plugin::plugin_static_resource_display_path(manifest, skill)});
    }
  }
  return resources;
}

void add_plugin_prompt_context_files(std::vector<ava::context::LoadedContextFile>& files, std::vector<LoadedPluginPromptResource> const& prompts)
{
  for (auto const& prompt : prompts)
  {
    files.push_back(ava::context::LoadedContextFile{
        .path = prompt.path, .source_type = ava::context::ContextSourceType::Plugin, .byte_count = prompt.text.size(), .content = prompt.text});
  }
}

void add_or_replace_loaded_skill(std::vector<ava::context::LoadedSkill>& skills, ava::context::LoadedSkill skill)
{
  auto const existing = std::ranges::find_if(skills, [&](ava::context::LoadedSkill const& item) { return item.name == skill.name; });
  if (existing != skills.end())
  {
    *existing = std::move(skill);
    return;
  }
  skills.push_back(std::move(skill));
}

void add_plugin_skills(std::vector<ava::context::LoadedSkill>& skills, std::vector<LoadedPluginSkillResource> const& plugin_skills)
{
  for (auto const& plugin_skill : plugin_skills)
  {
    add_or_replace_loaded_skill(skills, plugin_skill.skill);
  }
  std::ranges::sort(skills, [](ava::context::LoadedSkill const& left, ava::context::LoadedSkill const& right) { return left.name < right.name; });
}

void add_skill_freshness_sources(std::vector<FreshnessSourceMetadata>& sources, std::vector<ava::context::LoadedSkill> const& skills)
{
  for (auto const& skill : skills)
  {
    if (skill.source_type == ava::context::SkillSourceType::Plugin)
      continue;
    add_freshness_file(sources, FreshnessSourceKind::Skill, ava::context::to_string(skill.source_type), skill.name, skill.name, skill.path,
                       skill.byte_count == 0 ? kMaxRuntimeFreshnessBytes : skill.byte_count);
  }
}

void add_loaded_plugin_prompt_freshness_source(std::vector<FreshnessSourceMetadata>& sources, LoadedPluginPromptResource const& resource)
{
  sources.push_back(FreshnessSourceMetadata{.kind = FreshnessSourceKind::PluginPrompt,
                                            .scope = resource.scope,
                                            .source_id = resource.plugin_id,
                                            .name = resource.name,
                                            .path = resource.path,
                                            .byte_count = resource.text.size(),
                                            .content_fingerprint = ava::core::content_fingerprint(resource.text)});
}

void add_loaded_plugin_skill_freshness_source(std::vector<FreshnessSourceMetadata>& sources, LoadedPluginSkillResource const& resource)
{
  sources.push_back(FreshnessSourceMetadata{.kind = FreshnessSourceKind::PluginSkill,
                                            .scope = resource.scope,
                                            .source_id = resource.plugin_id,
                                            .name = resource.name,
                                            .path = resource.skill.path,
                                            .byte_count = resource.byte_count,
                                            .content_fingerprint = resource.content_fingerprint});
}

void add_failed_plugin_resource_freshness_source(std::vector<FreshnessSourceMetadata>& sources, PluginResourceLoadFailure const& failure)
{
  sources.push_back(FreshnessSourceMetadata{.kind = failure.kind,
                                            .scope = failure.scope,
                                            .source_id = failure.plugin_id,
                                            .name = failure.name,
                                            .path = failure.path,
                                            .byte_count = 0,
                                            .content_fingerprint = 0});
}

void add_plugin_freshness_sources(std::vector<FreshnessSourceMetadata>& sources, PluginRuntimeResources const& resources)
{
  auto const& diagnostics = resources.diagnostics;
  for (auto const& status : diagnostics.plugins)
  {
    auto const& manifest = status.plugin.manifest;
    add_freshness_file(sources, FreshnessSourceKind::PluginManifest, std::string(ava::plugin::to_string(status.plugin.scope)), manifest.id, "manifest",
                       manifest.path);
  }
  for (auto const& prompt : resources.prompts) add_loaded_plugin_prompt_freshness_source(sources, prompt);
  for (auto const& skill : resources.skills) add_loaded_plugin_skill_freshness_source(sources, skill);
  for (auto const& failure : resources.failures) add_failed_plugin_resource_freshness_source(sources, failure);
}

BasePromptMetadata base_prompt_metadata(ava::config::PromptSelection const& prompt)
{
  return BasePromptMetadata{.from_override = prompt.from_override,
                            .source_path = prompt.source_path,
                            .byte_count = prompt.text.size(),
                            .content_fingerprint = ava::core::content_fingerprint(prompt.text)};
}

}  // namespace

ava::core::Result<std::optional<ava::agent::AgentDefinition>> resolve_runtime_primary_agent(ava::config::XdgPaths const& paths,
                                                                                               std::filesystem::path const& workspace_dir,
                                                                                               bool include_project_resources,
                                                                                               std::optional<std::string> const& requested_primary_agent,
                                                                                               PrimaryAgentResolutionPolicy policy)
{
  if (!requested_primary_agent)
    return std::optional<ava::agent::AgentDefinition>{};

  auto global_agent_dirs = ava::agent::default_global_subagent_dirs();
  if (global_agent_dirs.size() >= 2)
  {
    global_agent_dirs[0] = paths.ava_config_dir / "agents";
    global_agent_dirs[1] = paths.ava_config_dir / "agent";
  }
  auto loaded_agents = ava::agent::load_subagents(ava::agent::SubagentLoadOptions{
      .workspace_root = workspace_dir, .global_agent_dirs = std::move(global_agent_dirs), .include_project_agents = include_project_resources});
  if (policy == PrimaryAgentResolutionPolicy::AllowUnavailable)
  {
    auto const* definition = ava::agent::find_subagent(loaded_agents.primary_agents, *requested_primary_agent);
    return definition ? std::optional<ava::agent::AgentDefinition>(*definition) : std::optional<ava::agent::AgentDefinition>{};
  }

  auto resolved = ava::agent::resolve_primary_agent(loaded_agents, *requested_primary_agent);
  if (!resolved)
    return std::unexpected(std::move(resolved.error()));
  return std::optional<ava::agent::AgentDefinition>(std::move(*resolved));
}

ava::core::Result<PromptState> load_runtime_prompt_state(ava::config::XdgPaths const& paths, ava::config::ModelInfo const& model, ava::agent::Mode mode,
                                                         std::filesystem::path const& workspace_dir, std::filesystem::path const& current_dir,
                                                         bool include_project_resources, PromptOverrides const& prompt_overrides,
                                                         std::optional<ava::agent::AgentDefinition> const& selected_primary_agent)
{
  auto const resource_policy = make_extension_resource_policy(paths, workspace_dir, include_project_resources);
  auto prompt = ava::config::select_prompt(paths, model, mode);
  if (!prompt)
    return std::unexpected(prompt.error());

  auto loaded_context = ava::context::load_context_files(ava::context::ContextLoadOptions{
      .workspace_root = workspace_dir,
      .current_dir = current_dir,
      .global_agents_file = paths.global_agents_file,
  });
  if (!loaded_context)
    return std::unexpected(loaded_context.error());
  auto ordinary_context_prompt = ava::context::format_context_for_prompt(*loaded_context);

  auto plugin_resources = load_plugin_runtime_resources(resource_policy, workspace_dir);
  add_plugin_prompt_context_files(*loaded_context, plugin_resources.prompts);

  std::vector<ContextSourceMetadata> context_sources;
  context_sources.reserve(loaded_context->size());
  for (auto const& file : *loaded_context)
  {
    context_sources.push_back(ContextSourceMetadata{.path = file.path,
                                                    .source_type = file.source_type,
                                                    .byte_count = file.byte_count,
                                                    .content_fingerprint = ava::core::content_fingerprint(file.content)});
  }

  auto loaded_skills = ava::context::load_skills(ava::context::SkillLoadOptions{
      .workspace_root = workspace_dir,
      .global_skill_dirs = resource_policy.global_skill_dirs,
      .project_skill_dirs = resource_policy.project_skill_dirs,
      .include_project_skills = resource_policy.include_project_resources,
  });
  add_plugin_skills(loaded_skills.skills, plugin_resources.skills);
  auto loaded_subagents = ava::agent::load_subagents(
      ava::agent::SubagentLoadOptions{.workspace_root = workspace_dir, .include_project_agents = resource_policy.include_project_resources});
  std::vector<FreshnessSourceMetadata> freshness_sources;
  auto selected_prompt = std::move(*prompt);
  auto system_prompt = selected_prompt.text;

  if (prompt_overrides.system_prompt)
  {
    selected_prompt = ava::config::PromptSelection{.text = *prompt_overrides.system_prompt, .from_override = true};
    system_prompt = *prompt_overrides.system_prompt;
    add_inline_prompt_freshness_source(freshness_sources, FreshnessSourceKind::SystemPrompt, "--system-prompt", "--system-prompt",
                                       *prompt_overrides.system_prompt);
  }
  else
  {
    auto system_resource = load_prompt_resource(paths, workspace_dir, include_project_resources, FreshnessSourceKind::SystemPrompt, "SYSTEM.md");
    if (!system_resource)
      return std::unexpected(std::move(system_resource.error()));
    if (*system_resource)
    {
      add_prompt_resource_freshness_source(freshness_sources, **system_resource);
      system_prompt = (*system_resource)->text;
    }
  }

  if (!prompt_overrides.append_system_prompts.empty())
  {
    std::string append_text;
    std::size_t append_index = 0;
    for (auto const& append_prompt : prompt_overrides.append_system_prompts)
    {
      ++append_index;
      add_inline_prompt_freshness_source(freshness_sources, FreshnessSourceKind::AppendSystemPrompt, "--append-system-prompt", std::to_string(append_index),
                                         append_prompt);
      if (append_prompt.empty())
        continue;
      if (!append_text.empty())
        append_text += "\n\n";
      append_text += append_prompt;
    }
    if (!append_text.empty())
      system_prompt += "\n\n" + append_text;
  }
  else
  {
    auto append_resource = load_prompt_resource(paths, workspace_dir, include_project_resources, FreshnessSourceKind::AppendSystemPrompt, "APPEND_SYSTEM.md");
    if (!append_resource)
      return std::unexpected(std::move(append_resource.error()));
    if (*append_resource)
    {
      add_prompt_resource_freshness_source(freshness_sources, **append_resource);
      if (!(*append_resource)->text.empty())
        system_prompt += "\n\n" + (*append_resource)->text;
    }
  }

  bool const selected_primary_agent_is_permitted =
      selected_primary_agent && (include_project_resources || selected_primary_agent->provenance == ava::agent::AgentDefinitionProvenance::Builtin ||
                                 selected_primary_agent->provenance == ava::agent::AgentDefinitionProvenance::Global);
  if (selected_primary_agent_is_permitted)
  {
    system_prompt += "\n\n# Selected Primary Agent Instructions\nAgent: " + selected_primary_agent->name;
    if (!selected_primary_agent->system_prompt.empty())
      system_prompt += "\n\n" + selected_primary_agent->system_prompt;
  }

  add_prompt_command_freshness_sources(freshness_sources, paths, workspace_dir, include_project_resources);
  add_skill_freshness_sources(freshness_sources, loaded_skills.skills);
  add_plugin_freshness_sources(freshness_sources, plugin_resources);

  auto ambient_extension_free_system_prompt = system_prompt + ordinary_context_prompt;
  system_prompt += ava::context::format_context_for_prompt(*loaded_context) + ava::context::format_available_skills_for_prompt(loaded_skills.skills) +
                   ava::agent::format_available_subagents_for_prompt(loaded_subagents.subagents);
  return PromptState{.mode = mode,
                     .base_prompt = base_prompt_metadata(selected_prompt),
                     .context_sources = std::move(context_sources),
                     .freshness_sources = std::move(freshness_sources),
                     .system_prompt = std::move(system_prompt),
                     .ambient_extension_free_system_prompt = std::move(ambient_extension_free_system_prompt)};
}

}  // namespace ava::app::runtime
