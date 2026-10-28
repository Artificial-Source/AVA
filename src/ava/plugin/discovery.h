#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/plugin/manifest.h"
#include "ava/core/result.h"

#include <filesystem>
#include <vector>

namespace ava::plugin {

struct PluginDiscoveryOptions
{
  std::filesystem::path global_plugins_dir;
  std::filesystem::path project_plugins_dir;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] PluginDiscoveryOptions default_plugin_discovery_options(std::filesystem::path const& workspace_root);
[[nodiscard]] ava::core::Result<std::vector<DiscoveredPlugin>> discover_plugins(PluginDiscoveryOptions const& options);

}  // namespace ava::plugin
