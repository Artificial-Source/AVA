#pragma once
#include "ava/debug/print_members_on.h"
#include "ava/plugin/diagnostics.h"
#include "ava/plugin/manifest.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/result.h"

#include <filesystem>
#include <ostream>
#include <utility>

namespace ava::plugin {

struct PluginInstallResult;

// Opaque, move-only token for a validated local plugin install source.
// Only inspect_plugin_install_source may construct it.
class PluginInstallSource final
{
 public:
  PluginInstallSource(PluginInstallSource const&) = delete;
  PluginInstallSource& operator=(PluginInstallSource const&) = delete;
  PluginInstallSource(PluginInstallSource&&) noexcept = default;
  PluginInstallSource& operator=(PluginInstallSource&&) noexcept = default;
  ~PluginInstallSource() = default;

  [[nodiscard]] std::filesystem::path const& directory() const noexcept { return directory_; }
  [[nodiscard]] PluginManifest const& manifest() const noexcept { return manifest_; }

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  friend ava::core::Result<PluginInstallSource> inspect_plugin_install_source(std::filesystem::path const& resolved_source);
  friend ava::core::Result<PluginInstallResult> install_global_plugin(PluginInstallSource source, std::filesystem::path const& global_root);

  PluginInstallSource(std::filesystem::path directory, PluginManifest manifest, ava::core::UniqueFd directory_fd)
      : directory_(std::move(directory)), manifest_(std::move(manifest)), directory_fd_(std::move(directory_fd))
  {
  }

  std::filesystem::path directory_;
  PluginManifest manifest_;
  ava::core::UniqueFd directory_fd_;
};

struct PluginInstallResult
{
  PluginManifest manifest;
  std::filesystem::path source_directory;
  std::filesystem::path destination_directory;

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginRemovalResult
{
  PluginManifest manifest;
  std::filesystem::path removed_directory;

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Classify a caller-resolved local path as a plugin install source directory.
// Accepts a real directory or a real plugin.json whose parent is a real directory.
[[nodiscard]] ava::core::Result<PluginInstallSource> inspect_plugin_install_source(std::filesystem::path const& resolved_source);

// Copy a validated local source into global_root/<id> via staging, without enabling or starting it.
// The source pathname must still identify the inspected, held directory. Contents
// may change within that directory; inspection is not an immutable content snapshot.
[[nodiscard]] ava::core::Result<PluginInstallResult> install_global_plugin(PluginInstallSource source, std::filesystem::path const& global_root);

// Remove a disabled global plugin that is a direct child of global_root, using
// held directory descriptors and never following symlinks in the selected tree.
[[nodiscard]] ava::core::Result<PluginRemovalResult> remove_global_plugin(PluginStatus status, std::filesystem::path const& global_root);

}  // namespace ava::plugin
