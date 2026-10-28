#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/plugin/manifest.h"
#include "ava/core/result.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ava::plugin {

struct PluginEnablementRecord
{
  std::filesystem::path workspace;
  std::string plugin_id;
  PluginScope scope = PluginScope::Project;
  bool enabled = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::filesystem::path default_plugin_enablement_file();
[[nodiscard]] std::filesystem::path canonical_workspace_key(std::filesystem::path const& workspace_root);
[[nodiscard]] ava::core::Result<std::vector<PluginEnablementRecord>> load_plugin_enablement(std::filesystem::path const& state_file);
[[nodiscard]] ava::core::Result<bool> plugin_enabled(std::filesystem::path const& state_file, std::filesystem::path const& workspace_root,
                                                     std::string_view plugin_id, PluginScope scope = PluginScope::Project);
[[nodiscard]] ava::core::VoidResult set_plugin_enabled(std::filesystem::path const& state_file, std::filesystem::path const& workspace_root,
                                                       std::string_view plugin_id, bool enabled, PluginScope scope = PluginScope::Project);

}  // namespace ava::plugin
