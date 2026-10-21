#pragma once

// Descriptor primitives return errno through int/std::expected<T, int> or a
// categorized errno carrier; open_regular_error adds shared operation diagnostics.

#include "ava/debug/print_members_on.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/error.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif

namespace ava::core {

// For syscalls whose only retryable failure is EINTR; never use this for close.
template <typename Operation>
inline auto restart_on_eintr(Operation operation) noexcept
{
  auto result = operation();
  while (result < 0 && errno == EINTR)
    result = operation();
  return result;
}

// These helpers return zero on success, otherwise the captured errno value.
[[nodiscard]] inline int fsync_with_restart(int fd) noexcept
{
  return restart_on_eintr([fd] { return ::fsync(fd); }) == 0 ? 0 : errno;
}

[[nodiscard]] inline int fdatasync_with_restart(int fd) noexcept
{
  return restart_on_eintr([fd] { return ::fdatasync(fd); }) == 0 ? 0 : errno;
}

[[nodiscard]] inline int write_all(int fd, std::string_view bytes) noexcept
{
  while (!bytes.empty())
  {
    auto const count = ::write(fd, bytes.data(), std::min(bytes.size(), static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())));
    if (count < 0)
    {
      if (errno == EINTR)
        continue;
      return errno;
    }
    if (count == 0)
      return EIO;
    bytes.remove_prefix(static_cast<std::size_t>(count));
  }
  return 0;
}

// Consumes the descriptor from its current offset, including at most one byte
// past max_bytes to distinguish an exact-sized input from EFBIG. Does not close it.
// The predicate is called immediately before every read, including EINTR retries
// and the EOF probe. Cancellation returns ECANCELED without consuming more input.
template <typename CancelRequested>
[[nodiscard]] inline std::expected<std::string, int> read_descriptor_bounded(int fd, std::size_t max_bytes, CancelRequested&& cancel_requested)
{
  std::string contents;
  std::array<char, 4096> buffer{};
  while (true)
  {
    if (cancel_requested())
      return std::unexpected(ECANCELED);
    auto const remaining = max_bytes - contents.size();
    auto const requested = remaining < buffer.size() ? remaining + 1 : buffer.size();
    auto const count = ::read(fd, buffer.data(), requested);
    if (count < 0)
    {
      if (errno == EINTR)
        continue;
      return std::unexpected(errno);
    }
    if (count == 0)
      return contents;
    if (static_cast<std::size_t>(count) > remaining)
      return std::unexpected(EFBIG);
    contents.append(buffer.data(), static_cast<std::size_t>(count));
  }
}

[[nodiscard]] inline std::expected<std::string, int> read_descriptor_bounded(int fd, std::size_t max_bytes)
{
  return read_descriptor_bounded(fd, max_bytes, [] { return false; });
}

enum class OpenRegularFailure
{
  Open,
  Stat,
  NotRegular,
  Flags,
};

struct OpenRegularError
{
  OpenRegularFailure failure;
  // Zero only for NotRegular, which is a metadata rejection rather than a syscall failure.
  int error_number;

  // Header-only error carrier; callers supply subsystem-specific diagnostics.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Callers supply path and subsystem context; metadata rejection has no errno cause.
[[nodiscard]] inline Error open_regular_error(OpenRegularError const& failure, std::string_view subject)
{
  std::string message;
  switch (failure.failure)
  {
    case OpenRegularFailure::NotRegular:
      return Error(ErrorCategory::InvalidArgument, std::string(subject) + " is not a regular file");
    case OpenRegularFailure::Open:
      message = "failed to open " + std::string(subject);
      break;
    case OpenRegularFailure::Stat:
      message = "failed to inspect " + std::string(subject);
      break;
    case OpenRegularFailure::Flags:
      message = "failed to prepare " + std::string(subject) + " for reading";
      break;
  }
  auto error = Error(ErrorCategory::Io, std::move(message));
  error.with_context("cause", std::strerror(failure.error_number));
  return error;
}

struct OpenRegularResult
{
  UniqueFd fd;
  struct stat status{};

  // stat is platform-owned metadata; generated member printing is not supported.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Borrow an already-open descriptor, retaining the caller's path-resolution and
// ownership policy. On success it is regular, blocking, and accompanied by its stat.
[[nodiscard]] inline std::expected<struct stat, OpenRegularError> prepare_regular_descriptor(int fd) noexcept
{
  struct stat status{};
  if (restart_on_eintr([&] { return ::fstat(fd, &status); }) != 0)
    return std::unexpected(OpenRegularError{.failure = OpenRegularFailure::Stat, .error_number = errno});
  if (!S_ISREG(status.st_mode))
    return std::unexpected(OpenRegularError{.failure = OpenRegularFailure::NotRegular, .error_number = 0});
  int const flags = restart_on_eintr([&] { return ::fcntl(fd, F_GETFL); });
  if (flags < 0 || restart_on_eintr([&] { return ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK); }) != 0)
    return std::unexpected(OpenRegularError{.failure = OpenRegularFailure::Flags, .error_number = errno});
  return status;
}

// Rejects a final symlink and non-regular descriptors without blocking on FIFOs.
// Intermediate components retain openat semantics; callers supply their own path authority.
[[nodiscard]] inline std::expected<OpenRegularResult, OpenRegularError> open_regular_nofollow(int dir_fd, char const* name)
{
  UniqueFd fd(restart_on_eintr([&] { return ::openat(dir_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK); }));
  if (!fd)
    return std::unexpected(OpenRegularError{.failure = OpenRegularFailure::Open, .error_number = errno});
  auto status = prepare_regular_descriptor(fd.get());
  if (!status)
    return std::unexpected(status.error());
  return OpenRegularResult{.fd = std::move(fd), .status = *status};
}

[[nodiscard]] inline std::string descriptor_proc_path(int fd)
{
  return "/proc/self/fd/" + std::to_string(fd);
}

enum class DescriptorPathFailure
{
  Readlink,
  Deleted,
  NotAbsolute,
};

struct DescriptorPathError
{
  DescriptorPathFailure failure;
  // Captured errno for Readlink, zero for semantic rejections.
  int error_number;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// A diagnostic snapshot, not authority for a subsequent pathname-based operation.
// Retains failure categories for callers that report operation-specific diagnostics.
[[nodiscard]] inline std::expected<std::filesystem::path, DescriptorPathError> descriptor_physical_path_checked(int fd)
{
  auto const proc_path = descriptor_proc_path(fd);
  std::string target(256, '\0');
  while (true)
  {
    auto const count = restart_on_eintr([&] { return ::readlink(proc_path.c_str(), target.data(), target.size()); });
    if (count < 0)
      return std::unexpected(DescriptorPathError{.failure = DescriptorPathFailure::Readlink, .error_number = errno});
    if (count == 0)
      return std::unexpected(DescriptorPathError{.failure = DescriptorPathFailure::Readlink, .error_number = EIO});
    auto const size = static_cast<std::size_t>(count);
    if (size < target.size())
    {
      target.resize(size);
      if (target.ends_with(" (deleted)"))
        return std::unexpected(DescriptorPathError{.failure = DescriptorPathFailure::Deleted, .error_number = 0});
      if (target.front() != '/')
        return std::unexpected(DescriptorPathError{.failure = DescriptorPathFailure::NotAbsolute, .error_number = 0});
      return std::filesystem::path(std::move(target));
    }
    if (target.size() > target.max_size() / 2)
      return std::unexpected(DescriptorPathError{.failure = DescriptorPathFailure::Readlink, .error_number = ENAMETOOLONG});
    target.resize(target.size() * 2);
  }
}

// Anonymous descriptors, deleted files and readlink failures have no physical path.
[[nodiscard]] inline std::optional<std::filesystem::path> descriptor_physical_path(int fd)
{
  auto result = descriptor_physical_path_checked(fd);
  if (!result)
    return std::nullopt;
  return std::move(*result);
}

// Never downgrade to a replacing rename; every failure propagates its captured errno.
[[nodiscard]] inline int rename_noreplace([[maybe_unused]] int old_dir, [[maybe_unused]] char const* old_name, [[maybe_unused]] int new_dir,
                                          [[maybe_unused]] char const* new_name) noexcept
{
#ifdef SYS_renameat2
  return ::syscall(SYS_renameat2, old_dir, old_name, new_dir, new_name, RENAME_NOREPLACE) == 0 ? 0 : errno;
#else
  return ENOSYS;
#endif
}

// Kernels or filesystems may reject the syscall or the RENAME_NOREPLACE flag.
[[nodiscard]] constexpr bool rename_noreplace_unavailable(int error_number) noexcept
{
  return error_number == ENOSYS || error_number == EINVAL || error_number == EOPNOTSUPP;
}

struct DirectoryStreamCloser
{
  void operator()(DIR* stream) const noexcept { static_cast<void>(::closedir(stream)); }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using DirectoryStream = std::unique_ptr<DIR, DirectoryStreamCloser>;

// Consumes the descriptor on both success and failure; fdopendir owns it only on success.
[[nodiscard]] inline std::expected<DirectoryStream, int> open_directory_stream(UniqueFd fd)
{
  DirectoryStream stream(::fdopendir(fd.get()));
  if (!stream)
    return std::unexpected(errno);
  static_cast<void>(fd.release());
  return stream;
}

}  // namespace ava::core
