#include "sys.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <system_error>
#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Bash containment intentionally denies the /proc task scan that LeakSanitizer
// performs at exit. Disable leak scanning only in this subprocess fixture;
// ASan/UBSan instrumentation and the test runner's leak checks remain enabled.
extern "C" char const* __asan_default_options()
{
  return "detect_leaks=0";
}

#if defined(_WIN32)
int main()
{
  return 77;
}
#else
namespace {

constexpr unsigned int kWatchdogSeconds = 30;

[[noreturn]] void watchdog_exit(int) noexcept
{
  _exit(124);
}

bool write_all(int descriptor, void const* data, std::size_t size) noexcept
{
  auto const* next = static_cast<unsigned char const*>(data);
  std::size_t offset = 0;
  while (offset < size)
  {
    auto const result = ::write(descriptor, next + offset, size - offset);
    if (result > 0)
    {
      offset += static_cast<std::size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

bool write_text(int descriptor, std::string_view text) noexcept
{
  return write_all(descriptor, text.data(), text.size());
}

bool read_control_byte(int descriptor) noexcept
{
  char value = '\0';
  while (true)
  {
    auto const result = ::read(descriptor, &value, 1);
    if (result == 1)
      return true;
    if (result < 0 && errno == EINTR)
      continue;
    return false;
  }
}

int parse_descriptor(std::string_view argument, std::string_view prefix, int fallback) noexcept
{
  if (!argument.starts_with(prefix))
    return fallback;
  int result = -1;
  auto const value = argument.substr(prefix.size());
  auto const parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && result >= 0 ? result : fallback;
}

int parse_positive(std::string_view argument, int fallback) noexcept
{
  int result = 0;
  auto const parsed = std::from_chars(argument.data(), argument.data() + argument.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == argument.data() + argument.size() && result > 0 ? result : fallback;
}

void ignore_term() noexcept
{
  struct sigaction ignored{};
  ignored.sa_handler = SIG_IGN;
  static_cast<void>(::sigemptyset(&ignored.sa_mask));
  static_cast<void>(::sigaction(SIGTERM, &ignored, nullptr));
}

[[noreturn]] void wait_forever() noexcept
{
  while (true)
    static_cast<void>(::pause());
}

int run_flood(int stdout_fd, int stderr_fd, int bytes) noexcept
{
  std::array<char, 4096> out{};
  std::array<char, 4096> err{};
  out.fill('O');
  err.fill('E');
  int remaining = bytes;
  while (remaining > 0)
  {
    auto const chunk = static_cast<std::size_t>(std::min<int>(remaining, static_cast<int>(out.size())));
    if (!write_all(stdout_fd, out.data(), chunk) || !write_all(stderr_fd, err.data(), chunk))
      return 3;
    remaining -= static_cast<int>(chunk);
  }
  return 0;
}

int create_exec_marker(char const* path) noexcept
{
  int const descriptor = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return 7;
  bool const written = write_text(descriptor, "EXECUTED\n");
  bool const closed = ::close(descriptor) == 0;
  return written && closed ? 0 : 7;
}

// Bash closes inherited descriptors, so this fixture opens its own private
// report files/FIFO. Readiness acknowledges handler installation, not a delay.
sig_atomic_t volatile term_receipt_fd = -1;

[[noreturn]] void record_term_and_exit(int) noexcept
{
  bool const written = write_text(static_cast<int>(term_receipt_fd), "TERM\n");
  _exit(written ? 0 : 6);
}

int bash_signal_tree(char const* disposition, char const* lifetime, char const* state_path, char const* receipt_path, char const* ready_path) noexcept
{
  int acknowledged[2];
  if (::pipe(acknowledged) != 0)
    return 4;
  auto const child = ::fork();
  if (child < 0)
  {
    ::close(acknowledged[0]);
    ::close(acknowledged[1]);
    return 4;
  }
  if (child == 0)
  {
    ::close(acknowledged[0]);
    ::alarm(kWatchdogSeconds);
    term_receipt_fd = ::open(receipt_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    struct sigaction action{};
    action.sa_handler = std::string_view(disposition) == "ignore" ? SIG_IGN : record_term_and_exit;
    ::sigemptyset(&action.sa_mask);
    sigset_t unblocked;
    ::sigemptyset(&unblocked);
    if (term_receipt_fd < 0 || ::sigaction(SIGTERM, &action, nullptr) != 0 || ::sigprocmask(SIG_SETMASK, &unblocked, nullptr) != 0 ||
        !write_text(acknowledged[1], "1"))
      _exit(5);
    ::close(acknowledged[1]);
    wait_forever();
  }
  ::close(acknowledged[1]);
  bool const installed = read_control_byte(acknowledged[0]);
  ::close(acknowledged[0]);
  // Two process ids and separators fit comfortably; a negative or truncated
  // length means the state line is wrong and the fixture must not look ready.
  std::array<char, 64> state{};
  int const state_length = std::snprintf(state.data(), state.size(), "%ld %ld\n", static_cast<long>(::getpgrp()), static_cast<long>(child));
  bool const formatted = state_length > 0 && static_cast<std::size_t>(state_length) < state.size();
  int const state_fd = installed && formatted ? ::open(state_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600) : -1;
  bool const recorded = state_fd >= 0 && write_all(state_fd, state.data(), static_cast<std::size_t>(state_length));
  if (state_fd >= 0)
    ::close(state_fd);
  int const ready_fd = recorded ? ::open(ready_path, O_WRONLY | O_CLOEXEC) : -1;
  bool const ready = ready_fd >= 0 && write_text(ready_fd, "READY\n");
  if (ready_fd >= 0)
    ::close(ready_fd);
  if (!ready)
  {
    ::kill(child, SIGKILL);
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR)
    {
    }
    return 5;
  }
  if (std::string_view(lifetime) == "exit")
    return 0;
  wait_forever();
}

}  // namespace

int main(int argc, char** argv)
{
  struct sigaction watchdog{};
  watchdog.sa_handler = watchdog_exit;
  static_cast<void>(::sigemptyset(&watchdog.sa_mask));
  static_cast<void>(::sigaction(SIGALRM, &watchdog, nullptr));
  static_cast<void>(::alarm(kWatchdogSeconds));

  if (argc < 2)
    return 2;
  std::string_view const mode(argv[1]);
  int status_fd = STDOUT_FILENO;
  int control_fd = STDIN_FILENO;
  for (int index = 2; index < argc; ++index)
  {
    std::string_view const argument(argv[index]);
    status_fd = parse_descriptor(argument, "--status-fd=", status_fd);
    control_fd = parse_descriptor(argument, "--control-fd=", control_fd);
  }

  if (mode == "bash-signal-tree")
    return argc == 7 ? bash_signal_tree(argv[2], argv[3], argv[4], argv[5], argv[6]) : 2;
  if (mode == "no-child-processes")
  {
    int status = 0;
    pid_t child;
    do
      child = ::waitpid(-1, &status, WNOHANG);
    while (child < 0 && errno == EINTR);
    bool const no_children = child < 0 && errno == ECHILD;
    if (!write_text(status_fd, no_children ? "no-unexpected-children\n" : "unexpected-child-wait-authority\n"))
      return 3;
    return no_children ? 0 : 6;
  }
  if (mode == "normal")
    return 0;
  if (mode == "nonzero")
    return 23;
  if (mode == "signal-exit")
  {
    static_cast<void>(::raise(SIGUSR2));
    return 125;
  }
  if (mode == "exec-marker")
    return argc >= 3 ? create_exec_marker(argv[2]) : 2;
  if (mode == "ready-gate")
  {
    if (!write_text(status_fd, "READY\n") || !read_control_byte(control_fd) || !write_text(status_fd, "RELEASED\n"))
      return 3;
    return 0;
  }
  if (mode == "input-gate")
    return read_control_byte(control_fd) ? 0 : 3;
  if (mode == "progress-gate")
  {
    if (!write_text(status_fd, "READY\n"))
      return 3;
    while (read_control_byte(control_fd))
    {
      if (!write_text(status_fd, "PROGRESS\n"))
        return 3;
    }
    return 0;
  }
  if (mode == "staged-stdio")
  {
    if (!write_text(STDOUT_FILENO, "OUT\n") || !read_control_byte(control_fd) || !write_text(STDERR_FILENO, "ERR\n"))
      return 3;
    return read_control_byte(control_fd) ? 0 : 3;
  }
  if (mode == "buffered-hup")
    return write_text(STDOUT_FILENO, "BUFFERED-BEFORE-HUP\n") ? 0 : 3;
  if (mode == "ignore-term")
  {
    ignore_term();
    if (!write_text(status_fd, "READY\n"))
      return 3;
    wait_forever();
  }
  if (mode == "stopped")
  {
    if (!write_text(status_fd, "READY\n"))
      return 3;
    static_cast<void>(::raise(SIGSTOP));
    wait_forever();
  }
  if (mode == "flood")
  {
    int bytes = 1024 * 1024;
    if (argc >= 3)
      bytes = parse_positive(argv[2], bytes);
    return run_flood(STDOUT_FILENO, STDERR_FILENO, bytes);
  }
  if (mode == "in-group-descendant")
  {
    pid_t const descendant = ::fork();
    if (descendant < 0)
      return 4;
    if (descendant == 0)
    {
      ignore_term();
      if (!write_text(status_fd, "DESCENDANT_READY\n"))
        _exit(5);
      wait_forever();
    }
    if (!write_text(status_fd, "LEADER_READY\n"))
      return 3;
    wait_forever();
  }
  if (mode == "leader-exits-first")
  {
    int readiness_pipe[2];
    if (::pipe(readiness_pipe) != 0)
      return 4;

    pid_t const descendant = ::fork();
    if (descendant < 0)
    {
      static_cast<void>(::close(readiness_pipe[0]));
      static_cast<void>(::close(readiness_pipe[1]));
      return 4;
    }
    if (descendant == 0)
    {
      static_cast<void>(::close(readiness_pipe[0]));
      ignore_term();
      bool const ready = write_text(status_fd, "DESCENDANT_READY\n");
      bool const signaled = ready && write_text(readiness_pipe[1], "1");
      static_cast<void>(::close(readiness_pipe[1]));
      if (!signaled)
        _exit(5);
      wait_forever();
    }

    static_cast<void>(::close(readiness_pipe[1]));
    bool const descendant_ready = read_control_byte(readiness_pipe[0]);
    static_cast<void>(::close(readiness_pipe[0]));
    if (!descendant_ready || !write_text(status_fd, "LEADER_EXITING\n"))
      return 3;
    return 0;
  }
  if (mode == "closed-pipes")
  {
    static_cast<void>(::close(STDOUT_FILENO));
    static_cast<void>(::close(STDERR_FILENO));
    return read_control_byte(control_fd) ? 0 : 3;
  }
  if (mode == "check-fd-closed")
  {
    if (argc < 3)
      return 2;
    int const descriptor = parse_positive(argv[2], -1);
    errno = 0;
    int const flags = descriptor < 0 ? 0 : ::fcntl(descriptor, F_GETFD);
    return write_text(status_fd, flags < 0 && errno == EBADF ? "CLOSED\n" : "OPEN\n") ? 0 : 3;
  }
  if (mode == "check-only-standard-fds")
  {
    bool clean = true;
    for (int descriptor = STDERR_FILENO + 1; clean && descriptor < 4096; ++descriptor)
    {
      errno = 0;
      clean = ::fcntl(descriptor, F_GETFD) < 0 && errno == EBADF;
    }
    return write_text(status_fd, clean ? "CLEAN\n" : "OPEN\n") ? (clean ? 0 : 6) : 3;
  }
  if (mode == "environment-clean")
  {
    constexpr std::array<std::string_view, 4> expected{"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "LANG=C.UTF-8", "LC_ALL=C.UTF-8",
                                                       "PWD=/"};
    bool clean = std::getenv("AVA_PROCESS_AMBIENT_CANARY") == nullptr && std::getenv("HOME") == nullptr;
    for (std::size_t index = 0; clean && index < expected.size(); ++index)
      clean = ::environ[index] != nullptr && std::string_view(::environ[index]) == expected[index];
    clean = clean && ::environ[expected.size()] == nullptr;
    return write_text(status_fd, clean ? "CLEAN\n" : "DIRTY\n") ? (clean ? 0 : 6) : 3;
  }
  if (mode == "adoption-environment")
  {
    if (argc < 3)
      return 2;
    constexpr std::array<std::string_view, 6> before_pwd{"LANG=C.UTF-8", "LC_ALL=C.UTF-8", "LC_CTYPE=C.UTF-8", "TZ=UTC", "USER=ava-test", "LOGNAME=ava-test"};
    constexpr std::array<std::string_view, 7> after_pwd{"PATH=/usr/bin:/bin",
                                                        "HOME=/tmp/ava-process-home",
                                                        "XDG_CONFIG_HOME=/tmp/ava-process-xdg-config",
                                                        "XDG_CACHE_HOME=/tmp/ava-process-xdg-cache",
                                                        "XDG_DATA_HOME=/tmp/ava-process-xdg-data",
                                                        "XDG_STATE_HOME=/tmp/ava-process-xdg-state",
                                                        "TMPDIR=/tmp/ava-process-tmp"};
    bool clean = std::getenv("AVA_PROCESS_AMBIENT_CANARY") == nullptr;
    std::size_t index = 0;
    for (auto const expected : before_pwd)
      clean = clean && ::environ[index] != nullptr && std::string_view(::environ[index++]) == expected;
    std::string const expected_pwd = std::string("PWD=") + argv[2];
    clean = clean && ::environ[index] != nullptr && std::string_view(::environ[index++]) == expected_pwd;
    for (auto const expected : after_pwd)
      clean = clean && ::environ[index] != nullptr && std::string_view(::environ[index++]) == expected;
    clean = clean && ::environ[index] == nullptr;
    return write_text(status_fd, clean ? "CLEAN\n" : "DIRTY\n") ? (clean ? 0 : 6) : 3;
  }
  if (mode == "check-retained-and-closed")
  {
    if (argc < 4)
      return 2;
    int const retained = parse_positive(argv[2], -1);
    int const unrelated = parse_positive(argv[3], -1);
    errno = 0;
    bool const retained_open = retained > STDERR_FILENO && ::fcntl(retained, F_GETFD) >= 0;
    errno = 0;
    bool const unrelated_closed = unrelated > STDERR_FILENO && ::fcntl(unrelated, F_GETFD) < 0 && errno == EBADF;
    bool const clean = retained_open && unrelated_closed;
    return write_text(status_fd, clean ? "CLEAN\n" : "DIRTY\n") ? (clean ? 0 : 6) : 3;
  }
  if (mode == "signal-state")
  {
    sigset_t mask{};
    struct sigaction term{};
    bool const clean = ::sigprocmask(SIG_SETMASK, nullptr, &mask) == 0 && ::sigismember(&mask, SIGUSR1) == 0 && ::sigaction(SIGTERM, nullptr, &term) == 0 &&
                       term.sa_handler == SIG_DFL;
    return write_text(status_fd, clean ? "CLEAN\n" : "DIRTY\n") ? (clean ? 0 : 6) : 3;
  }
  return 2;
}
#endif
