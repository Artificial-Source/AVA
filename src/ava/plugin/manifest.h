#pragma once

#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ava::plugin {

inline constexpr std::size_t kMaxManifestBytes = 256 * 1024;

inline constexpr std::string_view kPluginApiVersion = "ava.plugin.v1";
inline constexpr std::string_view kPluginProxyReadCapability = "proxy.read";
inline constexpr std::string_view kPluginProxySearchCapability = "proxy.search";
inline constexpr std::string_view kPluginProxySessionCapability = "proxy.session";
inline constexpr std::string_view kPluginUiStatusCapability = "ui.status";
inline constexpr std::string_view kPluginUiWidgetCapability = "ui.widget";
inline constexpr std::string_view kPluginUiSelectCapability = "ui.select";
inline constexpr std::string_view kPluginUiConfirmCapability = "ui.confirm";

enum class PluginScope
{
  Global,
  Project,
};

[[nodiscard]] std::string_view to_string(PluginScope scope);

struct PluginEntrypoint
{
  std::string command;
  std::vector<std::string> args;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginToolContribution
{
  std::string name;
  std::string description;
  std::string input_schema_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginCommandContribution
{
  std::string name;
  std::string description;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginResourceContribution
{
  std::string name;
  std::string description;
  std::string path;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginEventHookContribution
{
  std::string event;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginContributions
{
  std::vector<PluginToolContribution> tools;
  std::vector<PluginCommandContribution> commands;
  std::vector<PluginResourceContribution> prompts;
  std::vector<PluginResourceContribution> skills;
  std::vector<PluginEventHookContribution> event_hooks;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginManifest
{
  int schema_version = 0;
  std::string id;
  std::string name;
  std::string version;
  std::string api_version;
  std::string description;
  PluginEntrypoint entrypoint;
  std::vector<std::string> capabilities;
  PluginContributions contributes;
  std::filesystem::path path;
  std::filesystem::path directory;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct DiscoveredPlugin
{
  PluginManifest manifest;
  PluginScope scope = PluginScope::Global;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<PluginManifest> parse_plugin_manifest(std::string_view json, std::filesystem::path manifest_path = {});
[[nodiscard]] ava::core::Result<PluginManifest> load_plugin_manifest(std::filesystem::path const& manifest_path);
[[nodiscard]] bool plugin_has_capability(PluginManifest const& manifest, std::string_view capability);

}  // namespace ava::plugin
