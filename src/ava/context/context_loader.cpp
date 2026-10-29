#include "sys.h"
#include "ava/context/context_loader.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/error.h"
#include "ava/core/fd_io.h"
#include "ava/core/open_beneath.h"
#include "ava/core/path.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::context {
namespace {

constexpr std::array<std::string_view, 4> kContextFileNames{"AGENTS.md", "AGENTS.MD", "CLAUDE.md", "CLAUDE.MD"};

bool is_context_file_name(std::filesystem::path const& path)
{
  auto const filename = path.filename().string();
  return std::ranges::any_of(kContextFileNames, [&](std::string_view candidate) { return filename == candidate; });
}

using ava::core::normalized_absolute_path;

bool is_same_or_child(std::filesystem::path const& child, std::filesystem::path const& parent)
{
  auto child_it = child.begin();
  auto parent_it = parent.begin();
  for (; parent_it != parent.end(); ++parent_it, ++child_it)
    if (child_it == child.end() || *child_it != *parent_it)
      return false;
  return true;
}

ava::core::Error context_io_error(std::string message, std::filesystem::path const& path, int error_number = 0)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
  error.with_context("path", path.string());
  if (error_number != 0)
    error.with_context("cause", std::strerror(error_number));
  return error;
}

ava::core::Result<std::optional<ava::core::UniqueFd>> open_secure_root(std::filesystem::path const& root, bool missing_ok)
{
  auto const absolute = normalized_absolute_path(root);
  if (!absolute.is_absolute())
    return std::unexpected(context_io_error("context root must be absolute", absolute));
  // The context root is a trusted anchor opened at startup from configuration,
  // so symlinked components in its path are followed rather than rejected.
  // open_beneath (which uses openat2 RESOLVE_BENEATH) cannot be used here
  // because it rejects absolute symlink components unconditionally; the anchor
  // is opened directly and only the per-file reads beneath it are contained.
  ava::core::UniqueFd filesystem_root(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (filesystem_root.get() < 0)
    return std::unexpected(context_io_error("failed to open filesystem root for context validation", "/", errno));
  auto relative = absolute.lexically_relative("/");
  int const fd = ::openat(filesystem_root.get(), relative.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
  {
    if (missing_ok && errno == ENOENT)
      return std::optional<ava::core::UniqueFd>{};
    return std::unexpected(context_io_error("failed to open context root", absolute, errno));
  }
  return std::optional<ava::core::UniqueFd>(std::in_place, fd);
}

ava::core::Result<std::optional<std::string>> read_file_beneath(ava::core::UniqueFd const& root, std::filesystem::path const& relative,
                                                                std::filesystem::path const& display_path, std::size_t max_file_bytes)
{
  int const fd = ava::core::open_beneath(root.get(), relative, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    if (errno == ENOENT)
      return std::optional<std::string>{};
    return std::unexpected(context_io_error("failed to open context file", display_path, errno));
  }
  ava::core::UniqueFd file(fd);
  struct stat status{};
  if (::fstat(file.get(), &status) != 0)
    return std::unexpected(context_io_error("failed to inspect opened context file", display_path, errno));
  if (!S_ISREG(status.st_mode))
    return std::unexpected(context_io_error("context file is not a regular file", display_path));
  if (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > max_file_bytes)
  {
    auto error = context_io_error("context file is too large", display_path);
    error.with_context("max_bytes", std::to_string(max_file_bytes));
    return std::unexpected(std::move(error));
  }

  auto content = ava::core::read_descriptor_bounded(file.get(), max_file_bytes);
  if (!content)
  {
    if (content.error() == EFBIG)
    {
      auto error = context_io_error("context file is too large", display_path);
      error.with_context("max_bytes", std::to_string(max_file_bytes));
      return std::unexpected(std::move(error));
    }
    return std::unexpected(context_io_error("failed while reading context file", display_path, content.error()));
  }
  return std::optional<std::string>(std::move(*content));
}

ava::core::VoidResult append_if_present(std::vector<LoadedContextFile>& files, std::set<std::string>& seen_paths, ava::core::UniqueFd const& root,
                                        std::filesystem::path const& root_path, std::filesystem::path const& path, ContextSourceType source_type,
                                        std::size_t max_file_bytes)
{
  auto const normalized = normalized_absolute_path(path);
  auto const relative = normalized.lexically_relative(root_path);
  if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
    return std::unexpected(context_io_error("context file escapes its anchored root", normalized));
  auto content = read_file_beneath(root, relative, normalized, max_file_bytes);
  if (!content)
    return std::unexpected(std::move(content.error()));
  if (!*content || !seen_paths.insert(normalized.string()).second)
    return {};
  files.push_back(LoadedContextFile{.path = normalized, .source_type = source_type, .byte_count = (*content)->size(), .content = std::move(**content)});
  return {};
}

ava::core::VoidResult append_first_context_file_from_dir(std::vector<LoadedContextFile>& files, std::set<std::string>& seen_paths,
                                                         ava::core::UniqueFd const& root, std::filesystem::path const& root_path,
                                                         std::filesystem::path const& dir, ContextSourceType source_type, std::size_t max_file_bytes)
{
  auto const initial_size = files.size();
  for (auto const name : kContextFileNames)
  {
    auto appended = append_if_present(files, seen_paths, root, root_path, dir / std::string(name), source_type, max_file_bytes);
    if (!appended)
      return appended;
    if (files.size() != initial_size)
      return {};
  }
  return {};
}

std::vector<std::filesystem::path> context_dirs_root_to_current(std::filesystem::path const& workspace_root, std::filesystem::path const& current_dir)
{
  std::vector<std::filesystem::path> dirs;
  auto const root = normalized_absolute_path(workspace_root);
  auto const current = normalized_absolute_path(current_dir.empty() ? workspace_root : current_dir);
  if (!is_same_or_child(current, root))
  {
    dirs.push_back(root);
    return dirs;
  }

  auto cursor = current;
  while (true)
  {
    dirs.push_back(cursor);
    if (cursor == root || cursor == cursor.root_path())
      break;
    cursor = cursor.parent_path();
  }
  std::reverse(dirs.begin(), dirs.end());
  return dirs;
}

}  // namespace

std::string to_string(ContextSourceType source_type)
{
  switch (source_type)
  {
    case ContextSourceType::Workspace:
      return "workspace";
    case ContextSourceType::Global:
      return "global";
    case ContextSourceType::Plugin:
      return "plugin";
  }
  return "unknown";
}

ava::core::Result<std::vector<LoadedContextFile>> load_context_files(ContextLoadOptions const& options)
{
  std::vector<LoadedContextFile> files;
  std::set<std::string> seen_paths;
  auto const workspace_root = normalized_absolute_path(options.workspace_root);
  auto workspace = open_secure_root(workspace_root, false);
  if (!workspace)
    return std::unexpected(std::move(workspace.error()));
  for (auto const& dir : context_dirs_root_to_current(workspace_root, options.current_dir))
  {
    auto appended =
        append_first_context_file_from_dir(files, seen_paths, **workspace, workspace_root, dir, ContextSourceType::Workspace, options.max_file_bytes);
    if (!appended)
      return std::unexpected(std::move(appended.error()));
  }

  if (!options.global_agents_file.empty())
  {
    auto const global_root = normalized_absolute_path(options.global_agents_file.parent_path());
    auto global = open_secure_root(global_root, true);
    if (!global)
      return std::unexpected(std::move(global.error()));
    if (*global)
    {
      auto const initial_size = files.size();
      auto appended =
          append_if_present(files, seen_paths, **global, global_root, options.global_agents_file, ContextSourceType::Global, options.max_file_bytes);
      if (!appended)
        return std::unexpected(std::move(appended.error()));
      if (files.size() == initial_size && is_context_file_name(options.global_agents_file))
      {
        for (auto const name : kContextFileNames)
        {
          auto const candidate = options.global_agents_file.parent_path() / std::string(name);
          if (candidate == options.global_agents_file)
            continue;
          appended = append_if_present(files, seen_paths, **global, global_root, candidate, ContextSourceType::Global, options.max_file_bytes);
          if (!appended)
            return std::unexpected(std::move(appended.error()));
          if (files.size() != initial_size)
            break;
        }
      }
    }
  }

  return files;
}

std::string format_context_for_prompt(std::vector<LoadedContextFile> const& files)
{
  if (files.empty())
    return {};
  std::string output = "\n\n# Loaded Project Instructions\n";
  for (auto const& file : files)
  {
    output += "\n## " + to_string(file.source_type) + ": " + file.path.string() + "\n";
    output += file.content;
    if (!output.ends_with('\n'))
      output += '\n';
  }
  return output;
}

}  // namespace ava::context
