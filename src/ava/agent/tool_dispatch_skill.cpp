#include "sys.h"
#include "ava/agent/tool_dispatch_common.h"
#include "ava/agent/tool_dispatch_skill.h"
#include "ava/tools/tool_permission.h"
#include "ava/plugin/diagnostics.h"
#include "ava/plugin/discovery.h"
#include "ava/plugin/enablement.h"
#include "ava/plugin/static_resources.h"
#include "ava/context/skill_loader.h"
#include "ava/core/json.h"

#include <algorithm>
#include <vector>

namespace ava::agent {
namespace {

ava::plugin::PluginDiscoveryOptions plugin_discovery_options_for_context(ava::tools::ToolContext const& context)
{
  auto options = ava::plugin::default_plugin_discovery_options(context.workspace_dir);
  if (!context.plugin_global_plugins_dir.empty())
    options.global_plugins_dir = context.plugin_global_plugins_dir;
  if (!context.plugin_project_plugins_dir.empty())
    options.project_plugins_dir = context.plugin_project_plugins_dir;
  if (!context.include_project_plugins)
    options.project_plugins_dir = std::filesystem::path{};
  return options;
}

std::filesystem::path plugin_enablement_file_for_context(ava::tools::ToolContext const& context)
{
  if (!context.plugin_enablement_file.empty())
    return context.plugin_enablement_file;
  return ava::plugin::default_plugin_enablement_file();
}

}  // namespace

ToolDispatchResult skill_result(ava::tools::ToolContext const& context, ProviderToolCall const& call)
{
  auto name = tool_dispatch::required_safe_string_arg(call.arguments_json, "name", call.name);
  if (!name)
    return tool_dispatch::tool_error_result(call, name.error());
  std::vector<ava::context::DeclaredSkillFileOptions> plugin_skill_files;
  if (context.include_plugin_tools)
  {
    auto plugin_diagnostics = ava::plugin::collect_plugin_diagnostics(plugin_discovery_options_for_context(context),
                                                                      plugin_enablement_file_for_context(context), context.workspace_dir);
    plugin_skill_files = ava::plugin::declared_plugin_skill_files(plugin_diagnostics);
  }
  auto skills = ava::context::load_skills(ava::context::SkillLoadOptions{.workspace_root = context.workspace_dir,
                                                                         .global_skill_dirs = context.skill_global_dirs,
                                                                         .project_skill_dirs = context.skill_project_dirs,
                                                                         .declared_skill_files = std::move(plugin_skill_files),
                                                                         .include_global_skills = context.include_global_skills,
                                                                         .include_project_skills = context.include_project_skills});
  auto const match = std::ranges::find_if(skills.skills, [&](ava::context::LoadedSkill const& skill) { return skill.name == *name; });
  if (match == skills.skills.end())
  {
    std::string available;
    for (auto const& skill : skills.skills)
    {
      if (!available.empty())
        available += ", ";
      available += skill.name;
    }
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "skill not found");
    error.with_context("tool", call.name);
    error.with_context("skill", *name);
    error.with_context("available", available.empty() ? "none" : available);
    return tool_dispatch::tool_error_result(call, error);
  }

  auto tool_context = tool_dispatch::context_for_provider_tool(context, call);
  if (auto permission = ava::tools::ensure_permission(tool_context, ava::permissions::Operation::SkillLoad, match->path, match->name, "skill",
                                                      "skill loading requires permission");
      !permission)
  {
    return tool_dispatch::tool_error_result(call, permission.error());
  }
  auto sampled_files = ava::context::sample_skill_files(match->directory);
  auto content = ava::context::format_loaded_skill_for_tool(*match, sampled_files);
  return ToolDispatchResult{.call_id = call.id,
                            .name = call.name,
                            .success = true,
                            .result_text = "{\"tool\":\"skill\",\"ok\":true,\"name\":\"" + ava::core::json::escape(match->name) + "\",\"path\":\"" +
                                           ava::core::json::escape(match->path.string()) + "\",\"content\":\"" + ava::core::json::escape(content) + "\"}"};
}

}  // namespace ava::agent
