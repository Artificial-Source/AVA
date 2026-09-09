#include "sys.h"
#include "ava/core/AnchorSet.h"
#include "ava/core/path.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <cerrno>
#include <string>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "debug.h"

#ifndef O_PATH
#define O_PATH O_RDONLY
#endif

namespace ava::core {
namespace {

// Check whether candidate is lexically beneath root (root is a prefix of
// candidate). Both paths must be absolute and lexically normalized.
bool path_is_within(std::filesystem::path const& root, std::filesystem::path const& candidate)
{
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it)
    if (candidate_it == candidate.end() || *candidate_it != *root_it)
      return false;
  return true;
}

// Open a single anchor directory. The path is trusted (from configuration at
// startup), so symlinked components in the path are followed rather than
// rejected — the same pattern as SecureWorkspace::open and open_secure_root.
// Returns -1 with errno set on failure; the caller decides whether to skip
// or propagate the error.
int open_anchor_fd(std::filesystem::path const& absolute)
{
  int slash = ::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (slash < 0)
    return -1;
  int fd = absolute == "/" ? ::dup(slash) : ::openat(slash, absolute.relative_path().c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC);
  int const saved = errno;
  ::close(slash);
  errno = saved;
  return fd;
}

ava::core::Error anchor_error(std::string message, std::filesystem::path const& path, int error_number = 0)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
  if (!path.empty())
    error.with_context("path", path.string());
  if (error_number != 0)
    error.with_context("cause", std::generic_category().message(error_number));
  return error;
}

}  // namespace

// Return the launch directory as a logical path, preserving symlinks.
// See the header comment in AnchorSet.h for details.
Result<std::filesystem::path> launch_workspace_root()
{
  // Get the physical path via getcwd().
  std::error_code cwd_error;
  auto physical = std::filesystem::current_path(cwd_error);
  if (cwd_error)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Io, "failed to resolve current directory");
    error.with_context("cause", cwd_error.message());
    return std::unexpected(std::move(error));
  }

  // Check $PWD — the shell maintains it as the logical path (with symlinks
  // intact). If it is set and resolves to the same physical directory as
  // getcwd(), prefer it so that symlinked path components are preserved.
  char const* pwd = std::getenv("PWD");
  if (pwd != nullptr && pwd[0] == '/')
  {
    std::filesystem::path logical(pwd);
    struct stat physical_stat{};
    struct stat logical_stat{};
    if (::stat(physical.c_str(), &physical_stat) == 0 && ::stat(logical.c_str(), &logical_stat) == 0 &&
        physical_stat.st_dev == logical_stat.st_dev && physical_stat.st_ino == logical_stat.st_ino)
      return logical;
  }

  return physical;
}

AnchorSet::AnchorSet(AnchorSet&& other) noexcept : anchors_(std::move(other.anchors_))
{
}

AnchorSet& AnchorSet::operator=(AnchorSet&& other) noexcept
{
  if (this == &other)
    return *this;
  for (auto& anchor : anchors_)
  {
    if (anchor.fd >= 0)
      ::close(anchor.fd);
  }
  anchors_ = std::move(other.anchors_);
  return *this;
}

AnchorSet::~AnchorSet()
{
  for (auto& anchor : anchors_)
  {
    if (anchor.fd >= 0)
      ::close(anchor.fd);
  }
}

ava::core::Result<std::shared_ptr<AnchorSet>> AnchorSet::open(std::vector<std::filesystem::path> const& roots)
{
  DoutEntering(dc::avacore, "AnchorSet::open(" << roots << ")");

  auto set = std::shared_ptr<AnchorSet>(new AnchorSet());
  for (auto const& root : roots)
  {
    if (root.empty())
      continue;
    auto const absolute = normalized_absolute_path(root);
    if (!absolute.is_absolute())
      return std::unexpected(anchor_error("failed to resolve anchor root path", root, 0));
    int fd = open_anchor_fd(absolute);
    if (fd < 0)
    {
      // Non-existent directories are silently skipped. They may be created
      // later by the caller (e.g., spill_dir is created on first use).
      if (errno == ENOENT)
        continue;
      return std::unexpected(anchor_error("failed to open anchor directory", absolute, errno));
    }
    set->anchors_.push_back(Anchor{.fd = fd, .root = std::move(absolute)});
  }
  if (set->anchors_.empty())
    return std::unexpected(anchor_error("no anchor directories could be opened", {}));

  Dout(dc::avacore, "Returning set: " << *set);
  return set;
}

ava::core::Result<AnchorSet::AnchorRef> AnchorSet::find_anchor(std::filesystem::path const& candidate) const
{
  DoutEntering(dc::avacore, "AnchorSet::find_anchor(" << candidate << ")");

//  if (std::ranges::find(candidate, "physical") != candidate.end())
//    Debug(attach_gdb());

  if (candidate.empty())
    return std::unexpected(anchor_error("candidate path is empty", candidate));

  // If the candidate is relative, resolve it against the first anchor root
  // (the primary workspace). This matches the existing SecureWorkspace
  // behavior where relative paths are resolved against the workspace root.
  if (!candidate.is_absolute())
    try {
      // anchors_ is empty while a relative candidate needs the launch workspace; construct AnchorSet with the launch
      // workspace as its first entry before resolving paths.
      ASSERT(!anchors_.empty());
      return AnchorRef{anchors_.front(), candidate};
    } catch (std::runtime_error const& error) {
      return std::unexpected(anchor_error(error.what(), candidate));
    }

  // Lexically normalize the candidate — no filesystem canonicalization.
  std::filesystem::path absolute = candidate.lexically_normal();

  // Find the anchor whose root is the longest lexical prefix of the candidate.
  // Longest match wins so that nested anchors (e.g., workspace/a/b when
  // workspace/a is also an anchor) are preferred.
  Anchor const* best = nullptr;
  for (auto const& anchor : anchors_)
  {
    if (!path_is_within(anchor.root, absolute))
      continue;
    if (best == nullptr || anchor.root.native().size() > best->root.native().size())
      best = &anchor;
  }
  if (best == nullptr)
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "path is outside all writable anchor directories"));

  auto relative = absolute.lexically_relative(best->root);
  if (relative == ".")
    relative.clear();

  return AnchorRef{*best, relative};
}

bool AnchorSet::contains_lexical(std::filesystem::path const& candidate) const
{
  return find_anchor(candidate).has_value();
}

std::filesystem::path const& AnchorSet::launch_workspace_root() const
{
  // The caller asked for the launch workspace of an AnchorSet that has none; construct the AnchorSet with a launch
  // workspace entry before calling launch_workspace_root.
  ASSERT(!anchors_.empty());
  return anchors_.front().root;
}

}  // namespace ava::core
