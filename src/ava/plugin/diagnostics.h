#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/diagnostics/safe_failure.h"
#include "ava/plugin/discovery.h"
#include "ava/core/NoLogString.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ava::plugin {

enum class PluginFailureKind
{
  Discovery,
  DuplicateId,
  Enablement,
  ModelToolNameCollision,
};

struct PluginFailure
{
  PluginScope scope = PluginScope::Project;
  std::filesystem::path path;
  PluginFailureKind kind = PluginFailureKind::Discovery;
  ava::diagnostics::SafeFailure failure = ava::diagnostics::external_failure(ava::diagnostics::ComponentClass::Plugin);
  ava::core::NoLogString message;
  ava::core::NoLogString details;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginModelToolStatus
{
  std::string model_tool_name;
  bool name_collision = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginStatus
{
  DiscoveredPlugin plugin;
  bool enabled = false;
  // In manifest contribution order, populated only for enabled plugins.
  std::vector<PluginModelToolStatus> model_tools = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginDiagnostics
{
  PluginDiscoveryOptions discovery_options;
  std::filesystem::path enablement_file;
  std::vector<PluginStatus> plugins;
  std::vector<PluginFailure> failures;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] PluginDiagnostics collect_plugin_diagnostics(PluginDiscoveryOptions const& options, std::filesystem::path const& enablement_file,
                                                           std::filesystem::path const& workspace_root);

}  // namespace ava::plugin
