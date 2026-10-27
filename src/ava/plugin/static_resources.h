#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/plugin/diagnostics.h"
#include "ava/context/skill_loader.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ava::plugin {

inline constexpr std::size_t kStaticPluginResourceMaxBytes = 64 * 1024;

struct LoadedPluginStaticResource
{
  std::filesystem::path path;
  ava::core::NoLogString content;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginStaticSkillFile
{
  std::string scope;
  std::string plugin_id;
  std::string name;
  ava::core::NoLogString description;
  std::filesystem::path path;
  ava::core::NoLogString content;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] std::filesystem::path plugin_static_resource_display_path(PluginManifest const& manifest, PluginResourceContribution const& resource);
[[nodiscard]] ava::core::Result<LoadedPluginStaticResource> load_plugin_static_resource(PluginManifest const& manifest,
                                                                                        PluginResourceContribution const& resource,
                                                                                        std::size_t max_bytes = kStaticPluginResourceMaxBytes);
[[nodiscard]] std::vector<PluginStaticSkillFile> enabled_plugin_static_skill_files(PluginDiagnostics const& diagnostics);

// Convert enabled static plugin skills from `diagnostics` into declarations consumed by the context skill loader.
//
// Disabled plugins and static resources that failed inspection are absent because enabled_plugin_static_skill_files already filters them.
[[nodiscard]] std::vector<ava::context::DeclaredSkillFileOptions> declared_plugin_skill_files(PluginDiagnostics const& diagnostics);

}  // namespace ava::plugin
