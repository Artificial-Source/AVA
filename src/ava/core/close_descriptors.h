#pragma once

#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace ava::core {

// Allocation/lock-free Linux descriptor closure; returns an errno
// value, never an allocator-backed Error. Call before filesystem containment,
// or pass a directory opened as /proc/self/fd in this child before containment.
// The supplied directory is consumed (closed) on every return. Unlike an
// RLIMIT_NOFILE sweep this covers descriptors above lowered limits.
[[nodiscard]] inline int close_nonstandard_fds_except(std::span<int const> preserved, int directory = -1) noexcept
{
#if defined(__linux__)
#if defined(SYS_close_range)
  unsigned int first = STDERR_FILENO + 1;
  bool closed = true;
  while (true)
  {
    unsigned int next = directory >= 0 && static_cast<unsigned int>(directory) >= first ? static_cast<unsigned int>(directory) : UINT_MAX;
    for (int descriptor : preserved)
    {
      if (descriptor >= 0 && static_cast<unsigned int>(descriptor) >= first && static_cast<unsigned int>(descriptor) < next)
        next = static_cast<unsigned int>(descriptor);
    }
    if (first < next && ::syscall(SYS_close_range, first, next - 1, 0U) != 0)
      closed = false;
    if (next == UINT_MAX)
      break;
    first = next + 1;
  }
  if (closed)
  {
    if (directory >= 0 && ::close(directory) != 0 && errno != EINTR)
      return errno;
    return 0;
  }
#endif
  if (directory < 0)
  {
    do
      directory = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    while (directory < 0 && errno == EINTR);
  }
  if (directory < 0)
    return errno;

  int error = 0;
  std::array<unsigned char, 4096> buffer{};
  while (error == 0)
  {
    long count;
    do
      count = ::syscall(SYS_getdents64, directory, buffer.data(), buffer.size());
    while (count < 0 && errno == EINTR);
    if (count <= 0)
    {
      if (count < 0)
        error = errno;
      break;
    }
    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(count))
    {
      // Linux getdents64 ABI: u64 inode, s64 offset, u16 record length,
      // u8 type, then the NUL-terminated name. memcpy avoids alignment casts.
      constexpr std::size_t name_offset = 19;
      if (static_cast<std::size_t>(count) - offset <= name_offset)
      {
        error = EIO;
        break;
      }
      std::uint16_t length = 0;
      std::memcpy(&length, buffer.data() + offset + 16, sizeof(length));
      if (length <= name_offset || length > static_cast<std::size_t>(count) - offset)
      {
        error = EIO;
        break;
      }
      int descriptor = 0;
      std::size_t position = name_offset;
      bool numeric = true;
      for (; position < length && buffer[offset + position] != 0; ++position)
      {
        auto const digit = buffer[offset + position];
        if (digit < '0' || digit > '9' || descriptor > (INT_MAX - (digit - '0')) / 10)
        {
          numeric = false;
          break;
        }
        descriptor = descriptor * 10 + digit - '0';
      }
      if (numeric && (position == length || position == name_offset))
      {
        error = EIO;
        break;
      }
      if (numeric && descriptor > STDERR_FILENO && descriptor != directory)
      {
        bool keep = false;
        for (int retained : preserved)
          keep = keep || retained == descriptor;
        // On Linux close releases the fd even when interrupted; never retry.
        if (!keep && ::close(descriptor) != 0 && errno != EBADF && errno != EINTR)
        {
          error = errno;
          break;
        }
      }
      offset += length;
    }
  }
  if (::close(directory) != 0 && errno != EINTR && error == 0)
    error = errno;
  return error;
#else
  static_cast<void>(preserved);
  static_cast<void>(directory);
  return ENOTSUP;
#endif
}

}  // namespace ava::core
