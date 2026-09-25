#include "sys.h"
#include "ava/lsp/bounded_file_reader.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/AnchorSet.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::lsp {
namespace {

ava::core::Error read_error(ava::core::ErrorCategory category, std::string message, std::filesystem::path const& path, int error_number = 0)
{
  auto error = ava::core::Error(category, std::move(message));
  error.with_context("path", path.string());
  if (error_number != 0)
    error.with_context("cause", std::strerror(error_number));
  return error;
}

bool is_canceled(BoundedFileReadOptions const& options)
{
  return options.cancel_requested && options.cancel_requested();
}

bool deadline_expired(BoundedFileReadOptions const& options)
{
  return std::chrono::steady_clock::now() >= options.deadline;
}

std::optional<ava::core::Error> abort_error(BoundedFileReadOptions const& options)
{
  if (is_canceled(options))
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "LSP file read canceled", ava::core::ErrorCode::Canceled);
    error.with_context("path", options.path.string());
    error.with_context("canceled", "true");
    return error;
  }
  if (deadline_expired(options))
  {
    auto error = read_error(ava::core::ErrorCategory::Tool, "timed out reading LSP file", options.path);
    error.with_context("deadline", "expired");
    return error;
  }
  return std::nullopt;
}

bool path_is_within(std::filesystem::path const& candidate, std::filesystem::path const& root)
{
  auto root_component = root.begin();
  auto candidate_component = candidate.begin();
  for (; root_component != root.end(); ++root_component, ++candidate_component)
  {
    if (candidate_component == candidate.end() || *root_component != *candidate_component)
      return false;
  }
  return true;
}

ava::core::Result<std::filesystem::path> logical_read_path(BoundedFileReadOptions const& options)
{
  if (options.scope == BoundedFileReadScope::External)
  {
    if (!options.path.is_absolute())
      return std::unexpected(read_error(ava::core::ErrorCategory::InvalidArgument, "external LSP file path must be absolute", options.path));
    return options.path.lexically_normal();
  }

  if (options.workspace_root.empty() || !options.workspace_root.is_absolute())
  {
    return std::unexpected(
        read_error(ava::core::ErrorCategory::InvalidArgument, "LSP workspace root must be an absolute logical path", options.workspace_root));
  }
  auto const workspace = options.workspace_root.lexically_normal();
  auto const path = options.path.is_absolute() ? options.path.lexically_normal() : (workspace / options.path).lexically_normal();
  if (!path.is_absolute() || !path_is_within(path, workspace))
    return std::unexpected(read_error(ava::core::ErrorCategory::PermissionDenied, "LSP file is outside the workspace", options.path));
  return path;
}

}  // namespace

ava::core::Result<std::optional<std::string>> read_bounded_lsp_file(BoundedFileReadOptions const& options)
{
  if (options.path.empty() || !options.anchor_set || (!options.metadata_only && options.max_bytes == 0))
    return std::unexpected(read_error(ava::core::ErrorCategory::InvalidArgument, "LSP bounded file reader options are invalid", options.path));
  if (auto aborted = abort_error(options))
    return std::unexpected(std::move(*aborted));

  auto logical_path = logical_read_path(options);
  if (!logical_path)
    return std::unexpected(std::move(logical_path.error()));

  // AnchorOpen performs the one shared lexical anchor selection and the
  // descriptor-relative open. Internal symlinks are accepted only when they
  // remain beneath that selected anchor; external symlinks may not enter any
  // writable anchor.
  auto opened = ava::core::open_readable(*options.anchor_set, *logical_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (!opened)
  {
    if (auto aborted = abort_error(options))
      return std::unexpected(std::move(*aborted));
    if (options.missing_ok && opened.error().category() == ava::core::ErrorCategory::NotFound)
      return std::optional<std::string>{};
    return std::unexpected(std::move(opened.error()));
  }

  struct stat status{};
  if (::fstat(opened->fd(), &status) != 0)
  {
    int const saved_errno = errno;
    if (auto aborted = abort_error(options))
      return std::unexpected(std::move(*aborted));
    return std::unexpected(read_error(ava::core::ErrorCategory::Io, "failed to inspect opened LSP file", options.path, saved_errno));
  }
  if (auto aborted = abort_error(options))
    return std::unexpected(std::move(*aborted));
  if (!S_ISREG(status.st_mode))
    return std::unexpected(read_error(ava::core::ErrorCategory::InvalidArgument, "LSP file is not a regular file", options.path));
  if (options.require_private_owner && (status.st_uid != ::geteuid() || status.st_nlink != 1 || (status.st_mode & static_cast<mode_t>(S_IWGRP | S_IWOTH)) != 0))
  {
    return std::unexpected(read_error(ava::core::ErrorCategory::PermissionDenied, "LSP built-in opt-in config is not owner-safe", options.path));
  }
  if (!options.metadata_only && (status.st_size < 0 || static_cast<std::uintmax_t>(status.st_size) > options.max_bytes))
  {
    auto error = read_error(ava::core::ErrorCategory::InvalidArgument, "LSP file exceeds maximum size", options.path);
    error.with_context("max_bytes", std::to_string(options.max_bytes));
    return std::unexpected(std::move(error));
  }

  if (options.after_open_for_testing)
    options.after_open_for_testing();
  if (auto aborted = abort_error(options))
    return std::unexpected(std::move(*aborted));
  if (options.metadata_only)
    return std::optional<std::string>(std::string{});

  std::string content;
  content.reserve(static_cast<std::size_t>(status.st_size));
  std::array<char, 4096> buffer{};
  while (true)
  {
    if (auto aborted = abort_error(options))
      return std::unexpected(std::move(*aborted));
    auto const count = ::read(opened->fd(), buffer.data(), buffer.size());
    if (count == 0)
    {
      if (auto aborted = abort_error(options))
        return std::unexpected(std::move(*aborted));
      return std::optional<std::string>(std::move(content));
    }
    if (count < 0)
    {
      int const saved_errno = errno;
      if (auto aborted = abort_error(options))
        return std::unexpected(std::move(*aborted));
      if (saved_errno == EINTR)
        continue;
      return std::unexpected(read_error(ava::core::ErrorCategory::Io, "failed to read LSP file", options.path, saved_errno));
    }
    if (auto aborted = abort_error(options))
      return std::unexpected(std::move(*aborted));
    auto const bytes = static_cast<std::size_t>(count);
    if (content.size() > options.max_bytes || bytes > options.max_bytes - content.size())
    {
      auto error = read_error(ava::core::ErrorCategory::InvalidArgument, "LSP file exceeds maximum size", options.path);
      error.with_context("max_bytes", std::to_string(options.max_bytes));
      return std::unexpected(std::move(error));
    }
    content.append(buffer.data(), bytes);
  }
}

}  // namespace ava::lsp
