#include "sys.h"
#include "ava/plugin/install.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/fd_io.h"
#include "ava/core/ids.h"
#include "ava/core/path.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <utility>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::plugin {
namespace {

constexpr std::size_t kMaxInstallDirectoryDepth = 32;

ava::core::Error install_depth_error(std::filesystem::path const& path, std::string operation)
{
  return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin install directory exceeds maximum depth")
      .with_context("operation", std::move(operation))
      .with_context("path", path.string())
      .with_context("max_depth", std::to_string(kMaxInstallDirectoryDepth));
}

struct InstallStagingDirectory
{
  ava::core::UniqueFd fd;
  std::string name;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

bool path_is_within(std::filesystem::path const& base, std::filesystem::path const& target)
{
  if (target == base)
    return true;
  std::error_code relative_error;
  auto const relative = std::filesystem::relative(target, base, relative_error);
  if (relative_error || relative.empty())
    return false;
  return *relative.begin() != "..";
}

std::filesystem::path normalized_absolute(std::filesystem::path const& path)
{
  return ava::core::normalized_absolute_path(path);
}

ava::core::Error filesystem_errno_error(ava::core::ErrorCategory category, std::string message, std::filesystem::path const& path, int error_number)
{
  return ava::core::Error(category, std::move(message)).with_context("path", path.string()).with_context("cause", std::strerror(error_number));
}

mode_t safe_installed_file_mode(mode_t source_mode)
{
  mode_t mode = S_IRUSR | S_IWUSR;
  if ((source_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0)
    mode |= S_IXUSR;
  return mode;
}

ava::core::Result<ava::core::UniqueFd> open_install_directory_at(int parent_fd, char const* name, std::filesystem::path const& path, std::string message)
{
  int const fd = ava::core::restart_on_eintr([&] { return ::openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); });
  if (fd < 0)
  {
    auto const error_number = errno;
    auto const category = error_number == ELOOP || error_number == ENOTDIR ? ava::core::ErrorCategory::InvalidArgument : ava::core::ErrorCategory::Io;
    return std::unexpected(filesystem_errno_error(category, std::move(message), path, error_number));
  }
  return ava::core::UniqueFd(fd);
}

ava::core::Result<ava::core::DirectoryStream> open_install_directory_stream(int directory_fd, std::filesystem::path const& path)
{
  auto fd = open_install_directory_at(directory_fd, ".", path, "failed to inspect plugin install directory");
  if (!fd)
    return std::unexpected(std::move(fd.error()));
  auto stream = ava::core::open_directory_stream(std::move(*fd));
  if (!stream)
    return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to inspect plugin install directory", path, stream.error()));
  return std::move(*stream);
}

bool valid_install_entry_name(std::string_view name)
{
  return !name.empty() && name != "." && name != ".." && name.find('/') == std::string_view::npos;
}

ava::core::Result<ava::core::OpenRegularResult> open_install_source_file_at(int source_directory_fd, char const* name, std::filesystem::path const& source)
{
  auto opened = ava::core::open_regular_nofollow(source_directory_fd, name);
  if (!opened)
  {
    auto const& failure = opened.error();
    if (failure.failure == ava::core::OpenRegularFailure::Open && failure.error_number == ELOOP)
      return std::unexpected(
          filesystem_errno_error(ava::core::ErrorCategory::InvalidArgument, "plugin install source must not contain symlinks", source, failure.error_number));
    auto error = ava::core::open_regular_error(failure, "plugin install source file");
    error.with_context("path", source.string());
    return std::unexpected(std::move(error));
  }
  return std::move(*opened);
}

ava::core::Result<ava::core::UniqueFd> create_install_target_file_at(int target_directory_fd, char const* name, std::filesystem::path const& target,
                                                                     mode_t mode)
{
  int const fd = ava::core::restart_on_eintr([&] { return ::openat(target_directory_fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode); });
  if (fd < 0)
  {
    auto const error_number = errno;
    return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to create plugin install file", target, error_number));
  }
  return ava::core::UniqueFd(fd);
}

ava::core::VoidResult copy_regular_file_at(int source_directory_fd, int target_directory_fd, char const* name, std::filesystem::path const& source,
                                           std::filesystem::path const& target)
{
  // Open nonblocking and recheck the descriptor: a leaf can become a FIFO after fstatat.
  auto source_file = open_install_source_file_at(source_directory_fd, name, source);
  if (!source_file)
    return std::unexpected(std::move(source_file.error()));

  auto const target_mode = safe_installed_file_mode(source_file->status.st_mode);
  auto target_file = create_install_target_file_at(target_directory_fd, name, target, target_mode);
  if (!target_file)
    return std::unexpected(std::move(target_file.error()));

  std::array<char, 64 * 1024> buffer{};
  while (true)
  {
    auto const read_count = ava::core::restart_on_eintr([&] { return ::read(source_file->fd.get(), buffer.data(), buffer.size()); });
    if (read_count < 0)
    {
      auto const error_number = errno;
      return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to read plugin install source file", source, error_number));
    }
    if (read_count == 0)
      break;

    if (int const error_number = ava::core::write_all(target_file->get(), std::string_view(buffer.data(), static_cast<std::size_t>(read_count)));
        error_number != 0)
    {
      return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to write plugin install file", target, error_number));
    }
  }

  if (::fchmod(target_file->get(), target_mode) != 0)
  {
    auto const error_number = errno;
    return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to set plugin install file permissions", target, error_number));
  }
  return {};
}

ava::core::Result<ava::core::UniqueFd> ensure_global_plugin_install_root(std::filesystem::path const& root)
{
  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(root, status_error);
  if (status_error && status_error != std::errc::no_such_file_or_directory)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to inspect global plugin directory")
                               .with_context("path", root.string())
                               .with_context("cause", status_error.message()));
  }
  if (!status_error && std::filesystem::exists(status))
  {
    if (std::filesystem::is_symlink(status))
    {
      return std::unexpected(
          ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "global plugin directory must not be a symlink").with_context("path", root.string()));
    }
    if (!std::filesystem::is_directory(status))
    {
      return std::unexpected(
          ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "global plugin directory is not a directory").with_context("path", root.string()));
    }
  }
  else
  {
    std::error_code create_error;
    std::filesystem::create_directories(root, create_error);
    if (create_error)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to create global plugin directory")
                                 .with_context("path", root.string())
                                 .with_context("cause", create_error.message()));
    }
  }

  auto directory = open_install_directory_at(AT_FDCWD, root.c_str(), root, "failed to open global plugin directory");
  if (!directory)
    return std::unexpected(std::move(directory.error()));
  if (::fchmod(directory->get(), S_IRWXU) != 0)
    return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to set plugin install directory permissions", root, errno));
  return directory;
}

ava::core::VoidResult ensure_install_source_is_separate(std::filesystem::path const& source_dir, std::filesystem::path const& global_root)
{
  auto const source = normalized_absolute(source_dir);
  auto const root = normalized_absolute(global_root);
  if (path_is_within(source, root))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin install source must not contain the global plugin directory")
                               .with_context("source", source_dir.string())
                               .with_context("global_plugins", global_root.string()));
  }
  return {};
}

ava::core::VoidResult ensure_destination_available(int global_fd, std::filesystem::path const& destination)
{
  struct stat status{};
  if (::fstatat(global_fd, destination.filename().c_str(), &status, AT_SYMLINK_NOFOLLOW) == 0)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin install destination already exists").with_context("path", destination.string()));
  }
  if (errno != ENOENT)
    return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to inspect plugin install destination", destination, errno));
  return {};
}

ava::core::Result<InstallStagingDirectory> make_install_temp_dir(int global_fd, std::filesystem::path const& global_root, std::string const& plugin_id)
{
  for (int attempt = 0; attempt < 8; ++attempt)
  {
    auto name = plugin_id + ".installing-" + ava::core::make_id("plugin");
    auto const path = global_root / name;
    if (::mkdirat(global_fd, name.c_str(), S_IRWXU) == 0)
    {
      auto directory = open_install_directory_at(global_fd, name.c_str(), path, "failed to open plugin install staging directory");
      if (!directory)
      {
        static_cast<void>(::unlinkat(global_fd, name.c_str(), AT_REMOVEDIR));
        return std::unexpected(std::move(directory.error()));
      }
      if (::fchmod(directory->get(), S_IRWXU) != 0)
      {
        auto const error_number = errno;
        static_cast<void>(::unlinkat(global_fd, name.c_str(), AT_REMOVEDIR));
        return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to set plugin install directory permissions", path, error_number));
      }
      return InstallStagingDirectory{.fd = std::move(*directory), .name = std::move(name)};
    }
    if (errno != EEXIST)
      return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to create plugin install staging directory", path, errno));
  }
  return std::unexpected(
      ava::core::Error(ava::core::ErrorCategory::Io, "failed to allocate plugin install staging directory").with_context("path", global_root.string()));
}

// Entry paths below are diagnostic text only. Copy authority comes exclusively from
// held parent descriptors and single lexical names, never filesystem::relative.
ava::core::VoidResult copy_plugin_directory(int source_fd, int target_fd, std::filesystem::path const& source_path, std::filesystem::path const& target_path,
                                            std::size_t depth = 0)
{
  if (depth > kMaxInstallDirectoryDepth)
    return std::unexpected(install_depth_error(source_path, "copy source directory"));
  auto stream = open_install_directory_stream(source_fd, source_path);
  if (!stream)
    return std::unexpected(std::move(stream.error()));
  while (true)
  {
    errno = 0;
    auto const* entry = ::readdir(stream->get());
    if (!entry)
    {
      if (errno != 0)
        return std::unexpected(
            filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to inspect plugin install source directory entry", source_path, errno));
      return {};
    }
    std::string_view const name(entry->d_name);
    if (name == "." || name == "..")
      continue;
    if (!valid_install_entry_name(name))
    {
      return std::unexpected(
          ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "invalid plugin install source entry name").with_context("path", source_path.string()));
    }
    auto const source = source_path / name;
    auto const target = target_path / name;
    struct stat status{};
    if (::fstatat(source_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0)
      return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to inspect plugin install source entry", source, errno));
    if (S_ISLNK(status.st_mode))
    {
      return std::unexpected(
          ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin install source must not contain symlinks").with_context("path", source.string()));
    }
    if (S_ISDIR(status.st_mode))
    {
      if (depth == kMaxInstallDirectoryDepth)
        return std::unexpected(install_depth_error(source, "copy source directory"));
      auto source_child = open_install_directory_at(source_fd, entry->d_name, source, "failed to open plugin install source directory");
      if (!source_child)
        return std::unexpected(std::move(source_child.error()));
      if (::mkdirat(target_fd, entry->d_name, S_IRWXU) != 0)
        return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to create plugin install directory", target, errno));
      auto target_child = open_install_directory_at(target_fd, entry->d_name, target, "failed to open plugin install directory");
      if (!target_child)
        return std::unexpected(std::move(target_child.error()));
      if (::fchmod(target_child->get(), S_IRWXU) != 0)
        return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to set plugin install directory permissions", target, errno));
      if (auto copied = copy_plugin_directory(source_child->get(), target_child->get(), source, target, depth + 1); !copied)
        return copied;
    }
    else if (S_ISREG(status.st_mode))
    {
      if (auto copied = copy_regular_file_at(source_fd, target_fd, entry->d_name, source, target); !copied)
        return copied;
    }
    else
    {
      return std::unexpected(
          ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin install source must contain only directories and regular files")
              .with_context("path", source.string()));
    }
  }
}

ava::core::VoidResult cleanup_install_directory_contents(int directory_fd, std::filesystem::path const& path, std::size_t depth = 0)
{
  if (depth > kMaxInstallDirectoryDepth)
    return std::unexpected(install_depth_error(path, "clean staging directory"));
  auto stream = open_install_directory_stream(directory_fd, path);
  if (!stream)
    return std::unexpected(std::move(stream.error()));
  while (true)
  {
    errno = 0;
    auto const* entry = ::readdir(stream->get());
    if (!entry)
    {
      if (errno != 0)
        return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to inspect plugin install cleanup directory", path, errno));
      return {};
    }
    std::string_view const name(entry->d_name);
    if (!valid_install_entry_name(name))
      continue;
    auto const child_path = path / name;
    struct stat status{};
    if (::fstatat(directory_fd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0)
      return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to inspect plugin install cleanup entry", child_path, errno));
    if (S_ISDIR(status.st_mode))
    {
      if (depth == kMaxInstallDirectoryDepth)
        return std::unexpected(install_depth_error(child_path, "clean staging directory"));
      auto child = open_install_directory_at(directory_fd, entry->d_name, child_path, "failed to open plugin install cleanup directory");
      if (!child)
        return std::unexpected(std::move(child.error()));
      if (auto cleaned = cleanup_install_directory_contents(child->get(), child_path, depth + 1); !cleaned)
        return cleaned;
      if (::unlinkat(directory_fd, entry->d_name, AT_REMOVEDIR) != 0)
        return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to remove plugin install cleanup directory", child_path, errno));
    }
    else if (::unlinkat(directory_fd, entry->d_name, 0) != 0)
    {
      return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to remove plugin install cleanup file", child_path, errno));
    }
  }
}

ava::core::Error cleanup_failed_install(int global_fd, InstallStagingDirectory const& staging, std::filesystem::path const& path, ava::core::Error error)
{
  if (auto cleaned = cleanup_install_directory_contents(staging.fd.get(), path); !cleaned)
    error.with_context("cleanup", cleaned.error().format());
  else if (::unlinkat(global_fd, staging.name.c_str(), AT_REMOVEDIR) != 0)
    error.with_context("cleanup",
                       filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to remove plugin install staging directory", path, errno).format());
  return error;
}

ava::core::Result<PluginManifest> load_install_manifest_at(int directory_fd, std::filesystem::path const& manifest_path)
{
  auto file = ava::core::open_regular_nofollow(directory_fd, "plugin.json");
  if (!file)
  {
    auto error = ava::core::open_regular_error(file.error(), "plugin manifest");
    error.with_context("path", manifest_path.string());
    return std::unexpected(std::move(error));
  }
  auto const size_error = [&] {
    return ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin manifest exceeds maximum size")
        .with_context("path", manifest_path.string())
        .with_context("max_bytes", std::to_string(kMaxManifestBytes));
  };
  if (file->status.st_size < 0 || static_cast<std::uintmax_t>(file->status.st_size) > kMaxManifestBytes)
    return std::unexpected(size_error());
  auto contents = ava::core::read_descriptor_bounded(file->fd.get(), kMaxManifestBytes);
  if (!contents)
  {
    if (contents.error() == EFBIG)
      return std::unexpected(size_error());
    return std::unexpected(filesystem_errno_error(ava::core::ErrorCategory::Io, "failed to read plugin manifest", manifest_path, contents.error()));
  }
  auto parsed = parse_plugin_manifest(*contents, manifest_path);
  if (!parsed)
    parsed.error().with_context("path", manifest_path.string());
  return parsed;
}

ava::core::VoidResult ensure_removable_global_plugin(PluginStatus const& status, std::filesystem::path const& global_root)
{
  if (status.plugin.scope != PluginScope::Global)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "only global installed plugins can be removed")
                               .with_context("plugin", status.plugin.manifest.id));
  }
  if (status.enabled)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "disable plugin before removing it").with_context("plugin", status.plugin.manifest.id));
  }
  auto const root = normalized_absolute(global_root);
  auto const directory = normalized_absolute(status.plugin.manifest.directory);
  if (directory == root || !path_is_within(root, directory))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin directory is outside global plugin directory")
                               .with_context("plugin", status.plugin.manifest.id)
                               .with_context("path", status.plugin.manifest.directory.string()));
  }
  std::error_code status_error;
  auto const directory_status = std::filesystem::symlink_status(status.plugin.manifest.directory, status_error);
  if (status_error || std::filesystem::is_symlink(directory_status) || !std::filesystem::is_directory(directory_status))
  {
    auto error = ava::core::Error(status_error ? ava::core::ErrorCategory::Io : ava::core::ErrorCategory::InvalidArgument,
                                  "plugin directory is not a removable directory");
    error.with_context("plugin", status.plugin.manifest.id);
    error.with_context("path", status.plugin.manifest.directory.string());
    if (status_error)
      error.with_context("cause", status_error.message());
    return std::unexpected(std::move(error));
  }
  return {};
}

}  // namespace

ava::core::Result<PluginInstallSource> inspect_plugin_install_source(std::filesystem::path const& resolved_source)
{
  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(resolved_source, status_error);
  if (status_error)
  {
    auto error = ava::core::Error(status_error == std::errc::no_such_file_or_directory ? ava::core::ErrorCategory::NotFound : ava::core::ErrorCategory::Io,
                                  "failed to inspect plugin install source");
    error.with_context("path", resolved_source.string());
    error.with_context("cause", status_error.message());
    return std::unexpected(std::move(error));
  }
  std::filesystem::path source_dir;
  if (std::filesystem::is_symlink(status))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin install source must not be a symlink")
                               .with_context("path", resolved_source.string()));
  }
  if (std::filesystem::is_directory(status))
  {
    source_dir = resolved_source;
  }
  else if (std::filesystem::is_regular_file(status) && resolved_source.filename() == "plugin.json")
  {
    auto const parent = resolved_source.parent_path();
    std::error_code parent_error;
    auto const parent_status = std::filesystem::symlink_status(parent, parent_error);
    if (parent_error || std::filesystem::is_symlink(parent_status) || !std::filesystem::is_directory(parent_status))
    {
      auto error = ava::core::Error(parent_error ? ava::core::ErrorCategory::Io : ava::core::ErrorCategory::InvalidArgument,
                                    "plugin install manifest parent must be a real directory");
      error.with_context("path", parent.string());
      if (parent_error)
        error.with_context("cause", parent_error.message());
      return std::unexpected(std::move(error));
    }
    source_dir = parent;
  }
  else
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin install source must be a directory or plugin.json file")
                               .with_context("path", resolved_source.string()));
  }

  auto directory = open_install_directory_at(AT_FDCWD, source_dir.c_str(), source_dir, "failed to open plugin install source directory");
  if (!directory)
    return std::unexpected(std::move(directory.error()));
  auto manifest = load_install_manifest_at(directory->get(), source_dir / "plugin.json");
  if (!manifest)
    return std::unexpected(std::move(manifest.error()));
  return PluginInstallSource(std::move(source_dir), std::move(*manifest));
}

ava::core::Result<PluginInstallResult> install_global_plugin(PluginInstallSource source, std::filesystem::path const& global_root)
{
  auto const& source_dir = source.directory();
  auto const& initial_manifest = source.manifest();

  auto global_directory = ensure_global_plugin_install_root(global_root);
  if (!global_directory)
    return std::unexpected(std::move(global_directory.error()));
  if (auto separate = ensure_install_source_is_separate(source_dir, global_root); !separate)
    return std::unexpected(std::move(separate.error()));
  auto source_directory = open_install_directory_at(AT_FDCWD, source_dir.c_str(), source_dir, "failed to open plugin install source directory");
  if (!source_directory)
    return std::unexpected(std::move(source_directory.error()));

  auto const destination = global_root / initial_manifest.id;
  if (auto available = ensure_destination_available(global_directory->get(), destination); !available)
    return std::unexpected(std::move(available.error()));

  auto temp_dir = make_install_temp_dir(global_directory->get(), global_root, initial_manifest.id);
  if (!temp_dir)
    return std::unexpected(std::move(temp_dir.error()));
  auto const temp_path = global_root / temp_dir->name;

  if (auto copied = copy_plugin_directory(source_directory->get(), temp_dir->fd.get(), source_dir, temp_path); !copied)
  {
    return std::unexpected(cleanup_failed_install(global_directory->get(), *temp_dir, temp_path, std::move(copied.error())));
  }

  auto installed_manifest = load_install_manifest_at(temp_dir->fd.get(), temp_path / "plugin.json");
  if (!installed_manifest)
  {
    return std::unexpected(cleanup_failed_install(global_directory->get(), *temp_dir, temp_path, std::move(installed_manifest.error())));
  }
  if (installed_manifest->id != initial_manifest.id)
  {
    return std::unexpected(cleanup_failed_install(global_directory->get(), *temp_dir, temp_path,
                                                  ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "installed plugin id changed during copy")
                                                      .with_context("expected", initial_manifest.id)
                                                      .with_context("actual", installed_manifest->id)));
  }

  if (int const error_number =
          ava::core::rename_noreplace(global_directory->get(), temp_dir->name.c_str(), global_directory->get(), initial_manifest.id.c_str());
      error_number != 0)
  {
    if (error_number == EEXIST)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "plugin install destination already exists");
      error.with_context("path", destination.string());
      return std::unexpected(cleanup_failed_install(global_directory->get(), *temp_dir, temp_path, std::move(error)));
    }
    bool const unavailable = ava::core::rename_noreplace_unavailable(error_number);
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, unavailable ? "Linux renameat2 RENAME_NOREPLACE is unavailable for plugin installation"
                                                                            : "failed to install plugin directory")
                     .with_context("source", temp_path.string())
                     .with_context("target", destination.string())
                     .with_context("cause", std::strerror(error_number));
    if (unavailable)
      error.with_context("hint", "use a kernel and filesystem supporting renameat2 RENAME_NOREPLACE");
    return std::unexpected(cleanup_failed_install(global_directory->get(), *temp_dir, temp_path, std::move(error)));
  }

  installed_manifest->directory = destination;
  installed_manifest->path = destination / "plugin.json";
  return PluginInstallResult{
      .manifest = std::move(*installed_manifest),
      .source_directory = source_dir,
      .destination_directory = destination,
  };
}

ava::core::Result<PluginRemovalResult> remove_global_plugin(PluginStatus status, std::filesystem::path const& global_root)
{
  if (auto root = ensure_global_plugin_install_root(global_root); !root)
    return std::unexpected(std::move(root.error()));
  if (auto removable = ensure_removable_global_plugin(status, global_root); !removable)
    return std::unexpected(std::move(removable.error()));

  auto const removed_dir = status.plugin.manifest.directory;
  auto const manifest = status.plugin.manifest;
  std::error_code remove_error;
  std::filesystem::remove_all(removed_dir, remove_error);
  if (remove_error)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to remove plugin directory")
                               .with_context("plugin", manifest.id)
                               .with_context("path", removed_dir.string())
                               .with_context("cause", remove_error.message()));
  }
  return PluginRemovalResult{
      .manifest = manifest,
      .removed_directory = removed_dir,
  };
}

}  // namespace ava::plugin
