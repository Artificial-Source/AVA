#pragma once

#include "tests/support/test_timeout.h"
#include "ava/process/supervisor.h"
#include "ava/core/UniqueFd.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ava::tests {

// Append until marker is present, never exceeding the TOTAL byte bound or the
// absolute deadline (including EINTR and continuous output). Pass either a
// PipeEndpoint& or a borrowed int pipe descriptor with no competing reader.
// Keeps all captured bytes, including bytes after marker, for subsequent calls.
// False means EOF/error/timeout/bound without the marker. Build positive-wait
// deadlines with now_plus_seconds(); deliberately expired deadlines stay expired.
template <typename Source>
  requires(std::same_as<std::remove_cvref_t<Source>, int> || std::same_as<std::remove_cvref_t<Source>, ava::process::PipeEndpoint>)
[[nodiscard]] bool read_until(Source&& source, std::string& received, std::string_view marker, std::chrono::steady_clock::time_point deadline,
                              std::size_t bound = 4 * 1024 * 1024)
{
  if (received.size() > bound)
    return false;
  std::array<char, 8192> buffer{};
  while (received.find(marker) == std::string::npos)
  {
    auto const remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero() || received.size() == bound)
      return false;
    auto const capacity = std::min(buffer.size(), bound - received.size());
    if constexpr (std::same_as<std::remove_cvref_t<Source>, int>)
    {
      if (source < 0)
        return false;
      auto const milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
      auto const timeout = static_cast<int>(std::min<std::chrono::milliseconds::rep>(milliseconds, std::numeric_limits<int>::max()));
      pollfd descriptor{.fd = source, .events = POLLIN, .revents = 0};
      int const ready = ::poll(&descriptor, 1, timeout);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready <= 0 || (descriptor.revents & POLLNVAL) != 0)
        return false;
      // A pipe read returns available bytes, not a complete line. Poll first so
      // an unterminated record cannot defeat the deadline on a blocking pipe.
      auto const count = ::read(source, buffer.data(), capacity);
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      if (count <= 0)
        return false;
      received.append(buffer.data(), static_cast<std::size_t>(count));
    }
    else
    {
      auto result = source.read(std::span(reinterpret_cast<std::byte*>(buffer.data()), capacity));
      if (!result || result->state == ava::process::PipeIoStateV1::EndOfStream)
        return false;
      if (result->state == ava::process::PipeIoStateV1::Progress)
      {
        if (result->bytes == 0)
          return false;
        received.append(buffer.data(), result->bytes);
        continue;
      }
      auto ready = source.wait_readable(deadline);
      if (!ready || !*ready)
        return false;
    }
  }
  return true;
}

// Own the exact direct child until this returns. Return raw wait status, or
// nullopt with errno set; timeout/error kills and reaps before returning.
// ECHILD means ownership was already lost: never signal a possibly reused PID.
// process_group requires a private, child-led group established by the caller;
// it kills that group on failure, but can only reap the caller's direct child.
// Do not use for children owned by Supervisor or alongside another waiter.
[[nodiscard]] inline std::optional<int> wait_child_until(pid_t child, std::chrono::steady_clock::time_point deadline, bool process_group = false)
{
  if (child <= 0)
  {
    errno = EINVAL;
    return std::nullopt;
  }
  ava::core::UniqueFd notification;
#ifdef SYS_pidfd_open
  notification.reset(static_cast<int>(::syscall(SYS_pidfd_open, child, 0)));
#endif
  int status = 0;
  int error_number = ETIMEDOUT;
  while (true)
  {
    auto const waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child)
      return status;
    if (waited < 0 && errno != EINTR)
    {
      error_number = errno;
      if (error_number == ECHILD)
      {
        notification.reset();
        errno = error_number;
        return std::nullopt;
      }
      break;
    }
    auto const remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero())
      break;
    auto const remaining_ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    // pidfds block until exit; restricted/older kernels use a bounded fallback.
    auto const maximum_wait = notification ? std::numeric_limits<int>::max() : 10;
    int const timeout = static_cast<int>(std::min<std::chrono::milliseconds::rep>(remaining_ms, maximum_wait));
    pollfd descriptor{.fd = notification.get(), .events = POLLIN, .revents = 0};
    int const ready = ::poll(notification ? &descriptor : nullptr, notification ? 1U : 0U, timeout);
    if (ready < 0 && errno != EINTR)
    {
      error_number = errno;
      break;
    }
    if (ready > 0 && (descriptor.revents & POLLNVAL) != 0)
    {
      error_number = EBADF;
      break;
    }
  }
  if (process_group)
    static_cast<void>(::kill(-child, SIGKILL));
  static_cast<void>(::kill(child, SIGKILL));
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
  {
  }
  notification.reset();
  errno = error_number;
  return std::nullopt;
}

// Immediately adopt a successful raw fork. Early returns/exceptions kill and
// reap it; wait_until consumes ownership, including timeout/error cleanup.
// Fork only when no other threads are running if the child executes C++ before
// exec. This owner does not discover descendants or replace Supervisor custody.
class ScopedChild
{
 public:
  explicit ScopedChild(pid_t child, bool process_group = false) noexcept : child_(child > 0 ? child : -1), process_group_(process_group) { }
  ScopedChild(ScopedChild const&) = delete;
  ScopedChild& operator=(ScopedChild const&) = delete;
  ScopedChild(ScopedChild&& other) noexcept : child_(std::exchange(other.child_, -1)), process_group_(other.process_group_) { }
  ScopedChild& operator=(ScopedChild&&) = delete;
  ~ScopedChild() noexcept
  {
    if (child_ > 0)
    {
      int const saved_errno = errno;
      if (process_group_)
        static_cast<void>(::kill(-child_, SIGKILL));
      static_cast<void>(wait_until(std::chrono::steady_clock::now()));
      errno = saved_errno;
    }
  }

  [[nodiscard]] pid_t pid() const noexcept { return child_; }
  [[nodiscard]] std::optional<int> wait_until(std::chrono::steady_clock::time_point deadline)
  {
    return wait_child_until(std::exchange(child_, -1), deadline, process_group_);
  }

 private:
  pid_t child_;
  bool process_group_;
};

// True for an executable process, false for an absent PID or zombie/dead state.
// If procfs cannot be read, conservatively fall back to kill(pid, 0): EPERM and
// unknown errors remain live, never turning an inaccessible process into success.
// Numeric PIDs are observations, not authority to signal an arbitrary process.
[[nodiscard]] inline bool process_is_live(pid_t pid)
{
  if (pid <= 0)
    return false;
  std::ifstream input("/proc/" + std::to_string(pid) + "/stat", std::ios::binary);
  std::array<char, 4096> buffer;
  input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
  if (input.gcount() > 0)
  {
    // comm can contain newlines and parentheses; parse the final closing ')'.
    std::string_view const stat(buffer.data(), static_cast<std::size_t>(input.gcount()));
    auto const command_end = stat.rfind(") ");
    if (command_end != std::string::npos && command_end + 2 < stat.size())
    {
      char const state = stat[command_end + 2];
      return state != 'Z' && state != 'X' && state != 'x';
    }
  }
  return ::kill(pid, 0) == 0 || errno != ESRCH;
}

// Wait for absence of a LIVE process, not removal of an externally reaped zombie
// from /proc. This does not reap; direct children still need wait_child_until.
// Invalid/nonpositive PIDs fail rather than satisfying a missing-fixture check.
[[nodiscard]] inline bool wait_for_process_absent(pid_t pid, std::chrono::steady_clock::time_point deadline)
{
  if (pid <= 0)
  {
    errno = EINVAL;
    return false;
  }
  while (process_is_live(pid))
  {
    auto const now = std::chrono::steady_clock::now();
    if (now >= deadline)
      return false;
    std::this_thread::sleep_until(std::min(deadline, now + std::chrono::milliseconds(10)));
  }
  return true;
}

}  // namespace ava::tests
