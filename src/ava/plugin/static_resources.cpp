#include "sys.h"
#include "ava/plugin/static_resources.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/error.h"
#include "ava/core/open_beneath.h"
#include "ava/core/path.h"

#include <array>
#include <cerrno>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::plugin {
namespace {

ava::core::Error resource_error(ava::core::ErrorCategory category, std::string message, PluginManifest const& manifest,
                                PluginResourceContribution const& resource, std::filesystem::path const& path, int error_number = 0)
{
  auto error = ava::core::Error(category, std::move(message));
  error.with_context("plugin", manifest.id);
  error.with_context("resource", resource.name);
  error.with_context("path", path.string());
  if (error_number != 0)
    error.with_context("cause", std::generic_category().message(error_number));
  return error;
}

ava::core::Result<std::filesystem::path> relative_resource_path(PluginManifest const& manifest, PluginResourceContribution const& resource)
{
  auto const relative = std::filesystem::path(resource.path).lexically_normal();
  auto const first = relative.begin();
  if (relative.empty() || relative.is_absolute() || (first != relative.end() && *first == ".."))
  {
    return std::unexpected(resource_error(ava::core::ErrorCategory::InvalidArgument, "plugin resource path escapes plugin directory", manifest, resource,
                                          plugin_static_resource_display_path(manifest, resource)));
  }
  return relative;
}

}  // namespace

std::filesystem::path plugin_static_resource_display_path(PluginManifest const& manifest, PluginResourceContribution const& resource)
{
  return (ava::core::normalized_absolute_path(manifest.directory) / resource.path).lexically_normal();
}

ava::core::Result<LoadedPluginStaticResource> load_plugin_static_resource(PluginManifest const& manifest, PluginResourceContribution const& resource,
                                                                          std::size_t max_bytes)
{
  auto const display_path = plugin_static_resource_display_path(manifest, resource);
  auto relative = relative_resource_path(manifest, resource);
  if (!relative)
    return std::unexpected(std::move(relative.error()));

  ava::core::UniqueFd plugin_directory(::open(manifest.directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (plugin_directory.get() < 0)
  {
    return std::unexpected(resource_error(ava::core::ErrorCategory::Io, "failed to open plugin directory", manifest, resource, manifest.directory, errno));
  }

  ava::core::UniqueFd resource_fd(ava::core::open_beneath(plugin_directory.get(), *relative, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
  if (resource_fd.get() < 0)
  {
    int const error_number = errno;
    auto const category = (error_number == ELOOP || error_number == EXDEV) ? ava::core::ErrorCategory::PermissionDenied : ava::core::ErrorCategory::Io;
    return std::unexpected(resource_error(category, "failed to open plugin resource beneath plugin directory", manifest, resource, display_path, error_number));
  }

  struct stat status{};
  if (::fstat(resource_fd.get(), &status) < 0)
  {
    return std::unexpected(resource_error(ava::core::ErrorCategory::Io, "failed to inspect plugin resource", manifest, resource, display_path, errno));
  }
  if (!S_ISREG(status.st_mode))
  {
    return std::unexpected(
        resource_error(ava::core::ErrorCategory::InvalidArgument, "plugin resource must be a regular file", manifest, resource, display_path));
  }
  if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > max_bytes)
  {
    auto error = resource_error(ava::core::ErrorCategory::InvalidArgument, "plugin resource exceeds size cap", manifest, resource, display_path);
    error.with_context("max_bytes", std::to_string(max_bytes));
    return std::unexpected(std::move(error));
  }

  std::string content;
  content.reserve(static_cast<std::size_t>(status.st_size));
  std::array<char, 4096> buffer{};
  for (;;)
  {
    ssize_t const count = ::read(resource_fd.get(), buffer.data(), buffer.size());
    if (count < 0)
    {
      if (errno == EINTR)
        continue;
      return std::unexpected(resource_error(ava::core::ErrorCategory::Io, "failed to read plugin resource", manifest, resource, display_path, errno));
    }
    if (count == 0)
      break;
    if (content.size() + static_cast<std::size_t>(count) > max_bytes)
    {
      auto error =
          resource_error(ava::core::ErrorCategory::InvalidArgument, "plugin resource exceeds size cap while reading", manifest, resource, display_path);
      error.with_context("max_bytes", std::to_string(max_bytes));
      return std::unexpected(std::move(error));
    }
    content.append(buffer.data(), static_cast<std::size_t>(count));
  }

  return LoadedPluginStaticResource{.path = display_path, .content = std::move(content)};
}

std::vector<PluginStaticSkillFile> enabled_plugin_static_skill_files(PluginDiagnostics const& diagnostics)
{
  std::vector<PluginStaticSkillFile> files;
  for (auto const& status : diagnostics.plugins)
  {
    if (!status.enabled)
      continue;
    auto const& manifest = status.plugin.manifest;
    for (auto const& skill : manifest.contributes.skills)
    {
      auto loaded = load_plugin_static_resource(manifest, skill);
      if (!loaded)
        continue;
      files.push_back(PluginStaticSkillFile{.scope = std::string(to_string(status.plugin.scope)),
                                            .plugin_id = manifest.id,
                                            .name = skill.name,
                                            .description = skill.description,
                                            .path = std::move(loaded->path),
                                            .content = std::move(loaded->content)});
    }
  }
  return files;
}

std::vector<ava::context::DeclaredSkillFileOptions> declared_plugin_skill_files(PluginDiagnostics const& diagnostics)
{
  std::vector<ava::context::DeclaredSkillFileOptions> files;
  for (auto const& skill : enabled_plugin_static_skill_files(diagnostics))
  {
    files.push_back(ava::context::DeclaredSkillFileOptions{.path = skill.path,
                                                           .name = skill.name,
                                                           .description = skill.description,
                                                           .source_type = ava::context::SkillSourceType::Plugin,
                                                           .preloaded_content = skill.content});
  }
  return files;
}

}  // namespace ava::plugin
