#include "sys.h"
#include "ava/lsp/bounded_file_reader.h"
#include "ava/lsp/builtin_recipes.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/AnchorSet.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::lsp {
namespace {

constexpr std::size_t kMaxRootDepth = 64;

bool path_is_within(std::filesystem::path const& candidate, std::filesystem::path const& root)
{
  if (root.empty() || !candidate.is_absolute() || !root.is_absolute())
    return false;
  auto const child = candidate.lexically_normal();
  auto const parent = root.lexically_normal();
  auto parent_component = parent.begin();
  auto child_component = child.begin();
  for (; parent_component != parent.end(); ++parent_component, ++child_component)
  {
    if (child_component == child.end() || *parent_component != *child_component)
      return false;
  }
  return true;
}

bool directory_chain_is_safe(std::filesystem::path const& directory, bool user_owned)
{
  auto const logical = directory.lexically_normal();
  if (!logical.is_absolute())
    return false;
  std::filesystem::path current("/");
  for (auto const& component : logical.relative_path())
  {
    current /= component;
    struct stat metadata{};
    if (::lstat(current.c_str(), &metadata) != 0 || (!S_ISDIR(metadata.st_mode) && !S_ISLNK(metadata.st_mode)) ||
        (metadata.st_uid != 0 && metadata.st_uid != ::geteuid()))
    {
      return false;
    }
    bool const root_sticky_namespace = metadata.st_uid == 0 && S_ISDIR(metadata.st_mode) && (metadata.st_mode & S_ISVTX) != 0;
    if (S_ISDIR(metadata.st_mode) && (metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0 && !root_sticky_namespace)
      return false;
  }

  int const fd = ::open(logical.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_DIRECTORY);
  if (fd < 0)
    return false;
  struct stat final_metadata{};
  bool const safe = ::fstat(fd, &final_metadata) == 0 && S_ISDIR(final_metadata.st_mode) &&
                    (final_metadata.st_uid == 0 || final_metadata.st_uid == ::geteuid()) && (!user_owned || final_metadata.st_uid == ::geteuid()) &&
                    (final_metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0;
  ::close(fd);
  return safe;
}

bool has_elf_identity(int fd)
{
  std::array<unsigned char, 4> magic{};
  ssize_t bytes = -1;
  do
  {
    bytes = ::pread(fd, magic.data(), magic.size(), 0);
  } while (bytes < 0 && errno == EINTR);
  return bytes == static_cast<ssize_t>(magic.size()) && magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

std::optional<ExecutableIdentity> inspect_executable(std::filesystem::path const& candidate, bool user_owned, std::filesystem::path const& workspace_root,
                                                     std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  auto const logical = candidate.lexically_normal();
  if (!logical.is_absolute() || !anchor_set || path_is_within(logical, workspace_root) || !directory_chain_is_safe(logical.parent_path(), user_owned))
    return std::nullopt;

  struct stat path_metadata{};
  if (::lstat(logical.c_str(), &path_metadata) != 0 || S_ISLNK(path_metadata.st_mode) || !S_ISREG(path_metadata.st_mode) || path_metadata.st_nlink != 1 ||
      (user_owned ? path_metadata.st_uid != ::geteuid() : path_metadata.st_uid != 0) || (path_metadata.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      (path_metadata.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
  {
    return std::nullopt;
  }

  auto opened = ava::core::open_readable(*anchor_set, logical, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (!opened)
    return std::nullopt;
  struct stat opened_metadata{};
  bool const valid = ::fstat(opened->fd(), &opened_metadata) == 0 && opened_metadata.st_dev == path_metadata.st_dev &&
                     opened_metadata.st_ino == path_metadata.st_ino && opened_metadata.st_uid == path_metadata.st_uid &&
                     opened_metadata.st_gid == path_metadata.st_gid && opened_metadata.st_mode == path_metadata.st_mode && opened_metadata.st_nlink == 1 &&
                     has_elf_identity(opened->fd());
  if (!valid)
    return std::nullopt;

  return ExecutableIdentity{
      .executable_path = logical,
      .owner_uid = static_cast<std::uintmax_t>(opened_metadata.st_uid),
      .owner_gid = static_cast<std::uintmax_t>(opened_metadata.st_gid),
      .mode = static_cast<std::uintmax_t>(opened_metadata.st_mode),
      .link_count = static_cast<std::uintmax_t>(opened_metadata.st_nlink),
      .device = static_cast<std::uintmax_t>(opened_metadata.st_dev),
      .inode = static_cast<std::uintmax_t>(opened_metadata.st_ino),
      .size = static_cast<std::uintmax_t>(opened_metadata.st_size),
      .changed_seconds = static_cast<std::int64_t>(opened_metadata.st_ctim.tv_sec),
      .changed_nanoseconds = static_cast<std::int64_t>(opened_metadata.st_ctim.tv_nsec),
  };
}

std::vector<std::filesystem::path> default_system_directories()
{
  return {"/usr/local/bin", "/usr/bin", "/bin"};
}

std::vector<std::filesystem::path> default_user_directories()
{
  auto const* home_value = std::getenv("HOME");
  if (home_value == nullptr || *home_value == '\0')
    return {};
  auto const home = std::filesystem::path(home_value).lexically_normal();
  if (!home.is_absolute())
    return {};
  return {home / ".local" / "bin"};
}

bool marker_exists(std::filesystem::path const& marker, std::filesystem::path const& workspace_root,
                   std::shared_ptr<ava::core::AnchorSet const> const& anchor_set)
{
  auto content = read_bounded_lsp_file(BoundedFileReadOptions{
      .path = marker,
      .workspace_root = workspace_root,
      .anchor_set = anchor_set,
      .max_bytes = 0,
      .scope = BoundedFileReadScope::Workspace,
      .missing_ok = true,
      .metadata_only = true,
  });
  return content && content->has_value();
}

std::vector<std::filesystem::path> ancestors_for_document(std::filesystem::path const& document_path, std::filesystem::path const& workspace_root)
{
  auto const workspace = workspace_root.lexically_normal();
  auto const document = document_path.is_absolute() ? document_path.lexically_normal() : (workspace / document_path).lexically_normal();
  auto current = document.parent_path();
  if (!workspace.is_absolute() || !path_is_within(current, workspace))
    return {workspace};
  std::vector<std::filesystem::path> ancestors;
  for (std::size_t depth = 0; depth < kMaxRootDepth; ++depth)
  {
    ancestors.push_back(current);
    if (current == workspace)
      break;
    auto const parent = current.parent_path();
    if (parent == current || !path_is_within(parent, workspace))
      break;
    current = parent;
  }
  if (ancestors.empty() || ancestors.back() != workspace)
    return {workspace};
  return ancestors;
}

}  // namespace

std::vector<BuiltinServerRecipe> builtin_server_recipes()
{
  return {{.id = "clangd",
           .arguments = {"--background-index"},
           .file_extensions = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm"},
           .language_id = "cpp"}};
}

std::vector<BuiltinServerInspection> inspect_builtin_servers(std::vector<std::string> const& enabled_ids, std::filesystem::path const& workspace_root,
                                                             std::shared_ptr<ava::core::AnchorSet const> anchor_set, BuiltinDiscoveryOptions const& options)
{
  auto system_directories = options.system_directories;
  auto user_directories = options.user_directories;
  if (options.use_default_search_directories)
  {
    auto defaults = default_system_directories();
    system_directories.insert(system_directories.end(), defaults.begin(), defaults.end());
    auto user_defaults = default_user_directories();
    user_directories.insert(user_directories.end(), user_defaults.begin(), user_defaults.end());
  }

  std::vector<BuiltinServerInspection> result;
  for (auto const& recipe : builtin_server_recipes())
  {
    if (std::ranges::find(enabled_ids, recipe.id) == enabled_ids.end())
    {
      result.push_back({.id = recipe.id, .status = BuiltinServerStatus::Disabled, .reason = "not_enabled"});
      continue;
    }

    bool unsafe_candidate = false;
    std::optional<ExecutableIdentity> executable;
    auto inspect_directories = [&](std::vector<std::filesystem::path> const& directories, bool user_owned) {
      for (auto const& directory : directories)
      {
        if (!directory.is_absolute())
          continue;
        auto const candidate = directory.lexically_normal() / recipe.id;
        struct stat metadata{};
        if (::lstat(candidate.c_str(), &metadata) != 0)
          continue;
        auto inspected = inspect_executable(candidate, user_owned, workspace_root, anchor_set);
        if (inspected)
        {
          executable = std::move(*inspected);
          return;
        }
        unsafe_candidate = true;
      }
    };
    inspect_directories(system_directories, false);
    if (!executable)
      inspect_directories(user_directories, true);

    if (executable)
    {
      result.push_back({.id = recipe.id, .status = BuiltinServerStatus::Available, .reason = "installed", .executable = std::move(executable)});
    }
    else if (unsafe_candidate)
    {
      result.push_back({.id = recipe.id, .status = BuiltinServerStatus::Unsafe, .reason = "unsafe_install"});
    }
    else
    {
      result.push_back({.id = recipe.id, .status = BuiltinServerStatus::NotFound, .reason = "not_found"});
    }
  }
  return result;
}

std::filesystem::path select_builtin_server_root(std::string_view server_id, std::filesystem::path const& document_path,
                                                 std::filesystem::path const& workspace_root, std::shared_ptr<ava::core::AnchorSet const> anchor_set)
{
  auto const workspace = workspace_root.lexically_normal();
  if (server_id != "clangd" || !anchor_set)
    return workspace;
  for (auto const& ancestor : ancestors_for_document(document_path, workspace))
  {
    for (auto const* marker : {"compile_commands.json", "compile_flags.txt", ".clangd"})
    {
      if (marker_exists(ancestor / marker, workspace, anchor_set))
        return ancestor;
    }
  }
  return workspace;
}

}  // namespace ava::lsp
