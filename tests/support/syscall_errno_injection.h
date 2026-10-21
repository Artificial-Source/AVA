#pragma once

#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/debug/print_members_on.h"
#include "ava/core/UniqueFd.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ava::tests {

// Exit 77 denotes an unavailable child-only test facility, not a failed assertion.
inline void expect_child_success(std::expected<int, int> const& status, std::string_view message,
                                 std::string_view skip_reason = "syscall errno injection is unavailable")
{
  if (status && WIFEXITED(*status) && WEXITSTATUS(*status) == 77)
  {
    request_skip(std::string(skip_reason));
    return;
  }
  expect(status && WIFEXITED(*status) && WEXITSTATUS(*status) == 0, std::string(message));
}

// Return raw wait status, or nullopt with errno set. A timeout kills the child
// (and optionally its process group) and reaps it before returning ETIMEDOUT.
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
      int const wait_error = errno;
      notification.reset();
      errno = wait_error;
      return std::nullopt;
    }
    auto const remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero())
      break;
    auto const remaining_ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    // pidfds block until exit. Older kernels or restricted environments retain
    // a bounded 10 ms fallback instead of waking the runner every millisecond.
    auto const maximum_wait = notification ? std::numeric_limits<int>::max() : 10;
    int const timeout = static_cast<int>(std::min<std::chrono::milliseconds::rep>(remaining_ms, maximum_wait));
    pollfd descriptor{.fd = notification.get(), .events = POLLIN, .revents = 0};
    if (::poll(notification ? &descriptor : nullptr, notification ? 1U : 0U, timeout) < 0 && errno != EINTR)
    {
      error_number = errno;
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

// Match the low 32 bits of one syscall argument, or all values except value.
struct SyscallArgumentFilter
{
  std::size_t index;
  std::uint32_t value;
  bool equal = true;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct SyscallErrno
{
  long number;
  std::optional<SyscallArgumentFilter> argument;
  int error_number;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Return raw wait status, or errno for invalid rules/fork/wait/timeout failures.
// The parent is never filtered. Exit 77 means seccomp installation is unavailable.
// Multiple rules support distinct errors for different arguments of one syscall.
// errno zero deliberately simulates a successful syscall with a zero result.
// An empty rule span runs an unfiltered child with the same bounded wait/reaping.
[[nodiscard]] inline std::expected<int, int> run_in_child_with_syscall_errno(std::span<SyscallErrno const> rules, std::function<int()> const& body)
{
  for (auto const& rule : rules)
  {
    if (rule.number < 0 || rule.error_number < 0 || rule.error_number > 4095 || (rule.argument && rule.argument->index >= 6))
      return std::unexpected(EINVAL);
  }
  auto const child = ::fork();
  if (child < 0)
    return std::unexpected(errno);
  if (child == 0)
  {
    if (!rules.empty() && ::prctl(PR_SET_NO_NEW_PRIVS, 1L, 0L, 0L, 0L) != 0)
      ::_exit(77);
    for (auto const& rule : rules)
    {
      constexpr auto low_word = std::endian::native == std::endian::big ? sizeof(std::uint32_t) : 0;
      auto const offset = offsetof(seccomp_data, args) + (rule.argument ? rule.argument->index * sizeof(std::uint64_t) : 0) + low_word;
      bool const equal = !rule.argument || rule.argument->equal;
      std::array<sock_filter, 6> filter{{
          BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
          // A different syscall jumps directly to ALLOW.
          BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(rule.number), 0, 3),
          BPF_STMT(rule.argument ? BPF_LD | BPF_W | BPF_ABS : BPF_LD | BPF_IMM, rule.argument ? static_cast<std::uint32_t>(offset) : 0),
          // A matching argument falls through to ERRNO, otherwise ALLOW (inverted for !equal).
          BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, rule.argument ? rule.argument->value : 0, static_cast<unsigned char>(equal ? 0 : 1),
                   static_cast<unsigned char>(equal ? 1 : 0)),
          BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | static_cast<std::uint32_t>(rule.error_number)),
          BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
      }};
      sock_fprog program{.len = static_cast<unsigned short>(filter.size()), .filter = filter.data()};
      if (::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
        ::_exit(77);
    }
    ::_exit(body());
  }

  auto const status = wait_child_until(child, now_plus_seconds(5));
  if (!status)
    return std::unexpected(errno);
  return *status;
}

[[nodiscard]] inline std::expected<int, int> run_in_child_with_syscall_errno(long number, std::optional<SyscallArgumentFilter> argument, int error_number,
                                                                             std::function<int()> const& body)
{
  SyscallErrno const rule{.number = number, .argument = argument, .error_number = error_number};
  return run_in_child_with_syscall_errno(std::span(&rule, 1), body);
}

}  // namespace ava::tests
