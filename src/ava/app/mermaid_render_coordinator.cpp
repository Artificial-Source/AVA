// Runs application-owned Mermaid helper work; the coordinator queue publishes results before invoking completion notification.
#include "sys.h"
#include "ava/app/mermaid_render_coordinator.h"
#include "ava/app/tui/tui_display_settings.h"
#include "ava/core/Signals.h"
#include "ava/core/json.h"
#include "ava/core/thread.h"
#include "ava/core/utf8.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "debug.h"
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>

namespace ava::app {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kTerminationGrace = std::chrono::milliseconds(50);
constexpr int kIoPollMilliseconds = 10;

struct ScopedFd
{
  ScopedFd() = default;
  explicit ScopedFd(int value) noexcept : value(value) { }
  ~ScopedFd()
  {
    if (value >= 0)
      static_cast<void>(::close(value));
  }
  ScopedFd(ScopedFd const&) = delete;
  ScopedFd& operator=(ScopedFd const&) = delete;
  ScopedFd(ScopedFd&& other) noexcept : value(std::exchange(other.value, -1)) { }
  ScopedFd& operator=(ScopedFd&& other) noexcept
  {
    if (this != &other)
    {
      reset();
      value = std::exchange(other.value, -1);
    }
    return *this;
  }

  void reset(int replacement = -1) noexcept
  {
    if (value >= 0)
      static_cast<void>(::close(value));
    value = replacement;
  }
  [[nodiscard]] int get() const noexcept { return value; }
  [[nodiscard]] explicit operator bool() const noexcept { return value >= 0; }

  int value = -1;
};

struct Pipe
{
  ScopedFd read_end;
  ScopedFd write_end;
};

ava::core::Error coordinator_error(ava::core::ErrorCategory category, std::string message)
{
  return ava::core::Error(category, std::move(message));
}

bool set_nonblocking(int fd) noexcept
{
  auto const flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::optional<Pipe> make_pipe(bool nonblocking_read, bool nonblocking_write) noexcept
{
  std::array<int, 2> raw{-1, -1};
#if defined(__linux__)
  if (::pipe2(raw.data(), O_CLOEXEC) != 0)
    return std::nullopt;
#else
  if (::pipe(raw.data()) != 0)
    return std::nullopt;
  if (::fcntl(raw[0], F_SETFD, FD_CLOEXEC) != 0 || ::fcntl(raw[1], F_SETFD, FD_CLOEXEC) != 0)
  {
    static_cast<void>(::close(raw[0]));
    static_cast<void>(::close(raw[1]));
    return std::nullopt;
  }
#endif
  for (auto& fd : raw)
  {
    int const original = fd;
    auto const moved = ::fcntl(original, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    auto const saved_errno = errno;
    static_cast<void>(::close(original));
    fd = moved;
    if (moved < 0)
    {
      for (int const remaining : raw)
      {
        if (remaining >= 0)
          static_cast<void>(::close(remaining));
      }
      errno = saved_errno;
      return std::nullopt;
    }
  }
  Pipe pipe{.read_end = ScopedFd(raw[0]), .write_end = ScopedFd(raw[1])};
  if ((nonblocking_read && !set_nonblocking(pipe.read_end.get())) || (nonblocking_write && !set_nonblocking(pipe.write_end.get())))
    return std::nullopt;
  return pipe;
}

void drain_wake_fd(int fd) noexcept
{
  std::array<char, 64> bytes{};
  while (::read(fd, bytes.data(), bytes.size()) > 0)
  {
  }
}

void close_nonstandard_fds(int first_preserved, int second_preserved) noexcept
{
#if defined(__linux__) && defined(SYS_close_range)
  auto const lower = static_cast<unsigned int>(std::min(first_preserved, second_preserved));
  auto const upper = static_cast<unsigned int>(std::max(first_preserved, second_preserved));
  bool closed = true;
  if (lower > static_cast<unsigned int>(STDERR_FILENO + 1))
    closed = ::syscall(SYS_close_range, static_cast<unsigned int>(STDERR_FILENO + 1), lower - 1, 0U) == 0;
  if (upper > lower + 1)
    closed = (::syscall(SYS_close_range, lower + 1, upper - 1, 0U) == 0) && closed;
  if (upper < std::numeric_limits<unsigned int>::max())
    closed = (::syscall(SYS_close_range, upper + 1, std::numeric_limits<unsigned int>::max(), 0U) == 0) && closed;
  if (closed)
    return;
#endif
  long const open_max = ::sysconf(_SC_OPEN_MAX);
  int const max_fd = open_max > 0 && open_max <= std::numeric_limits<int>::max() ? static_cast<int>(open_max) : 1024;
  for (int fd = STDERR_FILENO + 1; fd < max_fd; ++fd)
  {
    if (fd != first_preserved && fd != second_preserved)
      static_cast<void>(::close(fd));
  }
}

[[noreturn]] void child_launch_failed(int error_fd) noexcept
{
  int const saved_errno = errno == 0 ? EIO : errno;
  auto const written = ::write(error_fd, &saved_errno, sizeof(saved_errno));
  static_cast<void>(written);
  _exit(127);
}

void descriptor_exec(int executable_fd, char* const argv[], char* const environment[], int error_fd) noexcept
{
#if defined(__linux__) && defined(SYS_execveat) && defined(AT_EMPTY_PATH)
  static_cast<void>(::syscall(SYS_execveat, executable_fd, "", argv, environment, AT_EMPTY_PATH));
#elif !defined(__linux__)
  static_cast<void>(::fexecve(executable_fd, argv, environment));
#else
  errno = ENOSYS;
#endif
  int const saved_errno = errno;
  auto const written = ::write(error_fd, &saved_errno, sizeof(saved_errno));
  static_cast<void>(written);
  _exit(127);
}

pid_t waitpid_retry(pid_t pid, int* status, int options) noexcept
{
  for (;;)
  {
    auto const result = ::waitpid(pid, status, options);
    if (result < 0 && errno == EINTR)
      continue;
    return result;
  }
}

int waitid_retry(pid_t pid, siginfo_t& info) noexcept
{
  for (;;)
  {
    std::memset(&info, 0, sizeof(info));
    auto const result = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT);
    if (result < 0 && errno == EINTR)
      continue;
    return result;
  }
}

void reap_direct_child(pid_t pid) noexcept
{
  if (pid <= 0)
    return;
  int status = 0;
  static_cast<void>(waitpid_retry(pid, &status, 0));
}

// This owns and verifies one process group rooted at the direct helper. It can
// terminate descendants that remain in that group. A descendant that calls
// setsid() escapes this mechanism and requires an external OS sandbox.
void terminate_owned_group(pid_t pid, pid_t pgid, bool group_verified) noexcept
{
  if (pid <= 1)
    return;
  if (!group_verified || pgid != pid || pgid <= 1 || ::getpgid(pid) != pgid)
  {
    static_cast<void>(::kill(pid, SIGKILL));
    reap_direct_child(pid);
    return;
  }

  static_cast<void>(::kill(-pgid, SIGTERM));
  auto const deadline = Clock::now() + kTerminationGrace;
  while (Clock::now() < deadline)
  {
    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    auto const delay = static_cast<int>(std::clamp<long long>(remaining.count(), 1, 5));
    static_cast<void>(::poll(nullptr, 0, delay));
  }
  // The unreaped leader keeps this owned process-group identity from being
  // recycled while escalation is issued.
  if (::getpgid(pid) == pgid)
    static_cast<void>(::kill(-pgid, SIGKILL));
  reap_direct_child(pid);
}

std::optional<std::string> accept_output(std::string output)
{
  if (output.empty() || output.size() > kMaxMermaidOutputBytes || !ava::core::json::is_valid_utf8(output))
    return std::nullopt;
  if (output.back() == '\n')
    output.pop_back();
  if (output.empty())
    return std::nullopt;

  std::size_t lines = 1;
  std::size_t line_bytes = 0;
  for (std::size_t index = 0; index < output.size();)
  {
    auto const decoded = ava::core::decode_utf8_scalar(output, index);
    if (!decoded)
      return std::nullopt;
    auto const code_point = decoded->scalar;
    if (code_point == '\n')
    {
      if (line_bytes > kMaxMermaidOutputLineBytes || ++lines > kMaxMermaidOutputLines)
        return std::nullopt;
      line_bytes = 0;
      ++index;
      continue;
    }
    if (code_point < 0x20U || (code_point >= 0x7fU && code_point <= 0x9fU) || (code_point >= 0x202aU && code_point <= 0x202eU) ||
        (code_point >= 0x2066U && code_point <= 0x2069U))
    {
      return std::nullopt;
    }
    if (line_bytes > kMaxMermaidOutputLineBytes - decoded->length)
      return std::nullopt;
    line_bytes += decoded->length;
    index += decoded->length;
  }
  if (line_bytes > kMaxMermaidOutputLineBytes)
    return std::nullopt;
  return output;
}

ava::core::VoidResult validate_configuration(MermaidRenderConfiguration const& configuration)
{
  if (configuration.argv.size() > kMaxMermaidArgCount)
    return std::unexpected(coordinator_error(ava::core::ErrorCategory::InvalidArgument, "Mermaid helper argv has too many arguments"));
  std::size_t total = 0;
  for (auto const& argument : configuration.argv)
  {
    if (argument.size() > kMaxMermaidArgBytes || argument.find('\0') != std::string::npos || total > kMaxMermaidArgvBytes - argument.size())
      return std::unexpected(coordinator_error(ava::core::ErrorCategory::InvalidArgument, "Mermaid helper argv exceeds its byte limits"));
    total += argument.size();
  }
  if (configuration.enabled && configuration.argv.empty())
    return std::unexpected(coordinator_error(ava::core::ErrorCategory::InvalidArgument, "enabled Mermaid rendering requires nonempty argv"));
  if (!configuration.argv.empty() && !std::filesystem::path(configuration.argv.front()).is_absolute())
    return std::unexpected(coordinator_error(ava::core::ErrorCategory::InvalidArgument, "Mermaid helper executable path must be absolute"));
  return {};
}

struct RenderResult
{
  MermaidRenderOutcome outcome = MermaidRenderOutcome::LaunchFailed;
  std::string text;
};

struct ChildState
{
  pid_t pid = -1;
  pid_t pgid = -1;
  bool group_verified = false;
  bool reaped = false;

  ~ChildState()
  {
    if (!reaped && pid > 1)
      terminate_owned_group(pid, pgid, group_verified);
  }
};

RenderResult render_one(std::vector<std::string> argv_values, std::string const& source, int wake_fd, std::function<bool()> const& canceled)
{
  auto const deadline = Clock::now() + kMermaidRenderDeadline;
  if (canceled())
    return {.outcome = MermaidRenderOutcome::Canceled, .text = {}};

  int executable_flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifndef O_NOFOLLOW
  return {.outcome = MermaidRenderOutcome::LaunchFailed, .text = {}};
#else
  executable_flags |= O_NOFOLLOW;
#endif
  ScopedFd executable(::open(argv_values.front().c_str(), executable_flags));
  if (!executable)
  {
    auto const outcome = errno == ENOENT || errno == ENOTDIR ? MermaidRenderOutcome::MissingHelper : MermaidRenderOutcome::LaunchFailed;
    return {.outcome = outcome, .text = {}};
  }
  auto const moved_executable = ::fcntl(executable.get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  if (moved_executable < 0)
    return {.outcome = MermaidRenderOutcome::LaunchFailed, .text = {}};
  executable.reset(moved_executable);
  struct stat executable_status{};
  if (::fstat(executable.get(), &executable_status) != 0 || !S_ISREG(executable_status.st_mode) ||
      (executable_status.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
  {
    return {.outcome = MermaidRenderOutcome::LaunchFailed, .text = {}};
  }

  auto stdin_pipe = make_pipe(false, true);
  auto stdout_pipe = make_pipe(true, false);
  auto exec_pipe = make_pipe(true, false);
  if (!stdin_pipe || !stdout_pipe || !exec_pipe)
    return {.outcome = MermaidRenderOutcome::LaunchFailed, .text = {}};

  std::vector<char*> arguments;
  arguments.reserve(argv_values.size() + 1);
  for (auto& argument : argv_values)
    arguments.push_back(argument.data());
  arguments.push_back(nullptr);
  std::array<std::string, 7> environment_values{
      "PATH=/usr/local/bin:/usr/bin:/bin", "LANG=C.UTF-8", "LC_ALL=C.UTF-8", "TERM=dumb", "NO_COLOR=1", "PWD=/", "AVA_MERMAID_PROTOCOL=1"};
  std::array<char*, 8> environment{};
  for (std::size_t index = 0; index < environment_values.size(); ++index)
    environment[index] = environment_values[index].data();
  environment.back() = nullptr;

  ChildState child;
  auto const parent_pgid = ::getpgrp();
  child.pid = ::fork();
  if (child.pid < 0)
    return {.outcome = MermaidRenderOutcome::LaunchFailed, .text = {}};
  if (child.pid == 0)
  {
    stdin_pipe->write_end.reset();
    stdout_pipe->read_end.reset();
    exec_pipe->read_end.reset();
    if (::setpgid(0, 0) != 0)
      _exit(127);
    static_cast<void>(::raise(SIGSTOP));
    if (!core::Signals::reset_child_signal_state() || ::dup2(stdin_pipe->read_end.get(), STDIN_FILENO) < 0 ||
        ::dup2(stdout_pipe->write_end.get(), STDOUT_FILENO) < 0)
      child_launch_failed(exec_pipe->write_end.get());
    int const dev_null = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (dev_null < 0 || ::dup2(dev_null, STDERR_FILENO) < 0 || ::chdir("/") != 0)
      child_launch_failed(exec_pipe->write_end.get());
    if (dev_null != STDERR_FILENO)
      static_cast<void>(::close(dev_null));
    auto const executable_fd = executable.get();
    auto const error_fd = exec_pipe->write_end.get();
    close_nonstandard_fds(executable_fd, error_fd);
    descriptor_exec(executable_fd, arguments.data(), environment.data(), error_fd);
  }

  stdin_pipe->read_end.reset();
  stdout_pipe->write_end.reset();
  exec_pipe->write_end.reset();
  executable.reset();

  bool stopped = false;
  while (!stopped && Clock::now() < deadline && !canceled())
  {
    int status = 0;
    auto const waited = waitpid_retry(child.pid, &status, WNOHANG | WUNTRACED);
    if (waited == child.pid)
    {
      if (WIFSTOPPED(status))
      {
        stopped = true;
        break;
      }
      child.reaped = true;
      return {.outcome = WIFSIGNALED(status) ? MermaidRenderOutcome::Signaled : MermaidRenderOutcome::LaunchFailed, .text = {}};
    }
    if (waited < 0)
    {
      child.reaped = errno == ECHILD;
      return {.outcome = MermaidRenderOutcome::LaunchFailed, .text = {}};
    }
    pollfd wake{.fd = wake_fd, .events = POLLIN, .revents = 0};
    static_cast<void>(::poll(&wake, 1, 1));
    if ((wake.revents & POLLIN) != 0)
      drain_wake_fd(wake_fd);
  }
  if (!stopped)
    return {.outcome = canceled() ? MermaidRenderOutcome::Canceled : MermaidRenderOutcome::Timeout, .text = {}};

  bool parent_group_set = false;
  for (int attempt = 0; attempt < 20; ++attempt)
  {
    if (::setpgid(child.pid, child.pid) == 0)
    {
      parent_group_set = true;
      break;
    }
    if (errno != EINTR)
      break;
  }
  auto const observed_pgid = ::getpgid(child.pid);
  child.group_verified = parent_group_set && child.pid > 1 && parent_pgid > 0 && observed_pgid == child.pid && observed_pgid != parent_pgid;
  child.pgid = child.group_verified ? observed_pgid : -1;
  if (!child.group_verified || ::kill(child.pid, SIGCONT) != 0)
    return {.outcome = MermaidRenderOutcome::LaunchFailed, .text = {}};

  std::size_t written = 0;
  std::string output;
  output.reserve(std::min<std::size_t>(kMaxMermaidOutputBytes, 16 * 1024));
  bool stdout_eof = false;
  bool exec_done = false;
  bool exec_failed = false;
  bool child_exited = false;
  siginfo_t child_info{};
  bool overflow = false;
  bool output_read_failed = false;

  while (!(child_exited && stdout_eof && exec_done))
  {
    if (canceled())
      return {.outcome = MermaidRenderOutcome::Canceled, .text = {}};
    if (Clock::now() >= deadline)
      return {.outcome = MermaidRenderOutcome::Timeout, .text = {}};

    std::array<pollfd, 4> descriptors{};
    nfds_t count = 0;
    auto const wake_index = count;
    descriptors[count++] = pollfd{.fd = wake_fd, .events = POLLIN, .revents = 0};
    std::optional<nfds_t> stdin_index;
    if (stdin_pipe->write_end)
    {
      stdin_index = count;
      descriptors[count++] = pollfd{.fd = stdin_pipe->write_end.get(), .events = POLLOUT, .revents = 0};
    }
    std::optional<nfds_t> stdout_index;
    if (!stdout_eof)
    {
      stdout_index = count;
      descriptors[count++] = pollfd{.fd = stdout_pipe->read_end.get(), .events = POLLIN, .revents = 0};
    }
    std::optional<nfds_t> exec_index;
    if (!exec_done)
    {
      exec_index = count;
      descriptors[count++] = pollfd{.fd = exec_pipe->read_end.get(), .events = POLLIN, .revents = 0};
    }

    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    auto const timeout = static_cast<int>(std::clamp<long long>(remaining, 1, kIoPollMilliseconds));
    auto const polled = ::poll(descriptors.data(), count, timeout);
    if (polled < 0 && errno != EINTR)
      return {.outcome = MermaidRenderOutcome::LaunchFailed, .text = {}};
    if ((descriptors[wake_index].revents & POLLIN) != 0)
      drain_wake_fd(wake_fd);

    if (stdin_index)
    {
      auto const events = descriptors[*stdin_index].revents;
      if ((events & POLLOUT) != 0)
      {
        auto const bytes = ::write(stdin_pipe->write_end.get(), source.data() + written, source.size() - written);
        if (bytes > 0)
          written += static_cast<std::size_t>(bytes);
        else if (bytes < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
          stdin_pipe->write_end.reset();
        if (written == source.size())
          stdin_pipe->write_end.reset();
      }
      if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        stdin_pipe->write_end.reset();
    }

    if (stdout_index)
    {
      auto const events = descriptors[*stdout_index].revents;
      if ((events & (POLLIN | POLLHUP)) != 0)
      {
        std::array<char, 8192> bytes{};
        for (;;)
        {
          auto const received = ::read(stdout_pipe->read_end.get(), bytes.data(), bytes.size());
          if (received > 0)
          {
            auto const size = static_cast<std::size_t>(received);
            if (output.size() > kMaxMermaidOutputBytes || size > kMaxMermaidOutputBytes - output.size())
            {
              overflow = true;
              break;
            }
            output.append(bytes.data(), size);
            continue;
          }
          if (received == 0)
          {
            stdout_eof = true;
            stdout_pipe->read_end.reset();
          }
          else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
          {
            output_read_failed = true;
            stdout_eof = true;
            stdout_pipe->read_end.reset();
          }
          if (received < 0 && errno == EINTR)
            continue;
          break;
        }
      }
      if ((events & (POLLERR | POLLNVAL)) != 0)
      {
        output_read_failed = true;
        stdout_eof = true;
        stdout_pipe->read_end.reset();
      }
      if (overflow)
        return {.outcome = MermaidRenderOutcome::OutputOverflow, .text = {}};
    }

    if (exec_index)
    {
      auto const events = descriptors[*exec_index].revents;
      if ((events & (POLLIN | POLLHUP)) != 0)
      {
        int child_errno = 0;
        auto const bytes = ::read(exec_pipe->read_end.get(), &child_errno, sizeof(child_errno));
        if (bytes > 0)
        {
          exec_failed = true;
          exec_done = true;
          exec_pipe->read_end.reset();
        }
        else if (bytes == 0)
        {
          exec_done = true;
          exec_pipe->read_end.reset();
        }
        else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
        {
          exec_failed = true;
          exec_done = true;
          exec_pipe->read_end.reset();
        }
      }
      if ((events & (POLLERR | POLLNVAL)) != 0)
      {
        exec_failed = true;
        exec_done = true;
        exec_pipe->read_end.reset();
      }
    }

    if (!child_exited)
    {
      siginfo_t observed{};
      if (waitid_retry(child.pid, observed) != 0)
      {
        if (errno == ECHILD)
          child.reaped = true;
        return {.outcome = MermaidRenderOutcome::LaunchFailed, .text = {}};
      }
      if (observed.si_pid != 0)
      {
        child_exited = true;
        child_info = observed;
      }
    }
  }

  int status = 0;
  if (waitpid_retry(child.pid, &status, 0) != child.pid)
    return {.outcome = MermaidRenderOutcome::LaunchFailed, .text = {}};
  child.reaped = true;
  if (exec_failed || output_read_failed || written != source.size())
    return {.outcome = MermaidRenderOutcome::LaunchFailed, .text = {}};
  if (child_info.si_code == CLD_KILLED || child_info.si_code == CLD_DUMPED || WIFSIGNALED(status))
    return {.outcome = MermaidRenderOutcome::Signaled, .text = {}};
  if (child_info.si_code != CLD_EXITED || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return {.outcome = MermaidRenderOutcome::NonzeroExit, .text = {}};
  auto accepted = accept_output(std::move(output));
  if (!accepted)
    return {.outcome = MermaidRenderOutcome::UnsafeOutput, .text = {}};
  return {.outcome = MermaidRenderOutcome::Accepted, .text = std::move(*accepted)};
}

}  // namespace

MermaidRenderConfiguration mermaid_render_configuration_from_display_settings(MermaidDisplaySettings const& settings, std::uint64_t epoch)
{
  return {.epoch = epoch, .enabled = settings.enabled, .argv = settings.argv};
}

// Coordinator-owned, heap-stable shared state for application callers and one rendering worker. mutex protects
// all mutable queue/configuration/cache fields; wake_pipe is stable until join. Worker copies process inputs
// under lock, performs helper I/O unlocked, then validates epoch before publishing. Owner must serialize
// shutdown and join before destruction.
struct MermaidRenderCoordinator::Impl
{
  // Publication snapshots share one registered callable. Copying
  // this handle cannot allocate; its captures survive unlocked invocation and
  // replacement. Captured state must support concurrent producer notifications.
  using CompletionNotifier = std::shared_ptr<std::function<void()> const>;
  // Owned queue/active value under Impl mutex. Identical source shares one helper execution; each admitted
  // identity receives its own completion unless canceled/reconfigured.
  struct Work
  {
    // Configuration identity captured at admission; worker publication rejects results when current epoch
    // differs.
    std::uint64_t epoch = 0;
    // Owned diagram source used for deduplication/cache identity; queue mutex protects stored values and worker
    // renders its own copy.
    std::string source;
    // Current admitted waiters under queue mutex; cancel removes one, and an empty active list tells worker to
    // stop the helper.
    std::vector<std::uint64_t> identities;
  };

  // Owned current-epoch result under Impl mutex. Successful text and deterministic fallback outcomes avoid
  // repeated helper work; cancellation is never cached.
  struct CacheEntry
  {
    // Owned diagram source used for deduplication/cache identity; queue mutex protects stored values and worker
    // renders its own copy.
    std::string source;
    // Cached validated result kind; accepted text contributes to the aggregate byte budget.
    MermaidRenderOutcome outcome = MermaidRenderOutcome::LaunchFailed;
    // Owned cached helper text protected by queue mutex; only accepted results normally retain bytes.
    std::string text;
  };

  // Construction thread: bind validated configuration and owned wake pipe before start publishes this stable
  // target to worker.
  explicit Impl(MermaidRenderConfiguration configuration_in, Pipe wake_pipe_in) : configuration(std::move(configuration_in)), wake_pipe(std::move(wake_pipe_in))
  {
  }

  // Construction thread, once: launch the owned joinable worker only after mutex, wake pipe, and configuration
  // exist.
  void start()
  {
    worker = ava::core::JoinThread::create("mermaid_render", [this](std::stop_token stop_token) { worker_loop(stop_token); });
  }

  // Caller thread with owner alive: nonblocking wake byte interrupts helper polling and condition notification
  // releases queue/diagnostic waits.
  void signal_worker() noexcept
  {
    char const byte = '1';
    auto const result = ::write(wake_pipe.write_end.get(), &byte, 1);
    static_cast<void>(result);
    changed.notify_all();
  }

  // Caller/worker with mutex held: count queued plus active waiters; callers add undrained completion count for
  // the admission bound.
  [[nodiscard]] std::size_t pending_identities_locked() const noexcept
  {
    std::size_t count = active ? active->identities.size() : 0;
    for (auto const& work : queue)
      count += work.identities.size();
    return count;
  }

  // Caller with mutex held: prevent duplicate identity admission across queued, active, and completed lifecycle
  // phases.
  [[nodiscard]] bool identity_present_locked(std::uint64_t identity) const noexcept
  {
    if (std::ranges::any_of(completions, [&](auto const& completion) { return completion.identity == identity; }))
      return true;
    if (active && std::ranges::find(active->identities, identity) != active->identities.end())
      return true;
    return std::ranges::any_of(queue, [&](auto const& work) { return std::ranges::find(work.identities, identity) != work.identities.end(); });
  }

  // Producer/caller after unlocking: invoke a retained snapshot, allowing queue
  // queries or replacement to reenter. Exceptions do not discard published work.
  // The callback must not destroy this coordinator or join its own worker.
  static void notify_completion(CompletionNotifier const& notifier) noexcept
  {
    if (!notifier)
      return;
    try
    {
      (*notifier)();
    }
    catch (...)
    {
      try
      {
        Dout(dc::warning, "Mermaid completion notification failed");
      }
      catch (...)
      { /* Notification diagnostics must not terminate the worker. */
      }
    }
  }

  // Publish before notification so a woken consumer cannot miss the result.
  // Called with mutex held for worker, cache, fallback and cancellation results.
  // The caller takes a notifier snapshot and invokes it only after unlocking.
  void add_completion_locked(std::uint64_t identity, MermaidRenderOutcome outcome, std::string text = {}, bool from_cache = false)
  {
    completions.push_back(MermaidRenderCompletion{
        .identity = identity, .config_epoch = configuration.epoch, .outcome = outcome, .text = std::move(text), .from_cache = from_cache});
  }

  // Caller/worker with mutex held: find current-epoch source cache entry; iterator is valid only while lock and
  // cache remain unchanged.
  auto find_cache_locked(std::string const& source) { return std::ranges::find(cache, source, &CacheEntry::source); }

  // Worker with mutex held: skip cancellation, replace source entry, and evict oldest results until
  // count/accepted-byte bounds hold.
  void cache_result_locked(std::string const& source, RenderResult const& result)
  {
    if (result.outcome == MermaidRenderOutcome::Canceled)
      return;
    if (auto existing = find_cache_locked(source); existing != cache.end())
    {
      accepted_cache_bytes -= existing->text.size();
      cache.erase(existing);
    }
    auto const new_bytes = result.outcome == MermaidRenderOutcome::Accepted ? result.text.size() : 0;
    while (!cache.empty() && (cache.size() >= kMaxMermaidCacheEntries || accepted_cache_bytes > kMaxMermaidAcceptedCacheBytes - new_bytes))
    {
      accepted_cache_bytes -= cache.front().text.size();
      cache.pop_front();
    }
    cache.push_back(CacheEntry{.source = source, .outcome = result.outcome, .text = result.text});
    accepted_cache_bytes += new_bytes;
  }

  // Worker helper callback: briefly acquire mutex to observe shutdown, reconfiguration, or last-waiter
  // cancellation without retaining active references.
  [[nodiscard]] bool active_canceled(std::uint64_t epoch, std::string const& source) const
  {
    std::lock_guard lock(mutex);
    return shutting_down || configuration.epoch != epoch || !active || active->epoch != epoch || active->source != source || active->identities.empty();
  }

  // Worker thread: move one queued request into active under lock, render outside lock, then publish only
  // still-needed same-epoch results; return retires active work before owner joins.
  void worker_loop(std::stop_token stop_token)
  {
    for (;;)
    {
      std::vector<std::string> argv;
      // Owned diagram source used for deduplication/cache identity; queue mutex protects stored values and worker
    // renders its own copy.
      std::string source;
      // Configuration identity captured at admission; worker publication rejects results when current epoch
    // differs.
      std::uint64_t epoch = 0;
      {
        // Claim and copy process inputs under the queue lock, then release it before helper I/O so frontend
        // admission/cancellation can proceed.
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stop_token.stop_requested() || shutting_down || !queue.empty(); });
        if ((stop_token.stop_requested() || shutting_down) && queue.empty())
          break;
        active = std::move(queue.front());
        queue.pop_front();
        source = active->source;
        argv = configuration.argv;
        epoch = active->epoch;
      }
      changed.notify_all();
      drain_wake_fd(wake_pipe.read_end.get());

      // Only this worker owns helper execution. Cancellation briefly reacquires mutex through active_canceled;
      // it never borrows mutable active storage outside the lock.
      auto result = render_one(std::move(argv), source, wake_pipe.read_end.get(), [this, epoch, &source] { return active_canceled(epoch, source); });
      CompletionNotifier notification;
      {
        std::lock_guard lock(mutex);
        // Revalidate active identity/epoch after I/O before caching or publishing; canceled/reconfigured
        // waiters must not receive stale output.
        if (active && active->epoch == epoch && active->source == source)
        {
          if (!shutting_down && configuration.epoch == epoch && !active->identities.empty())
          {
            cache_result_locked(source, result);
            for (auto const identity : active->identities)
              add_completion_locked(identity, result.outcome, result.text);
            notification = completion_notifier;
          }
          active.reset();
        }
      }
      // The callback can drain these results or query the coordinator. Neither
      // queue synchronization nor a worker-local reference escapes this scope.
      notify_completion(notification);
      changed.notify_all();
    }

    std::lock_guard lock(mutex);
    active.reset();
    queue.clear();
    changed.notify_all();
  }

  // Sole lock for configuration, queue/active/completions, cache, notifier, and
  // shutdown. Arbitrary callback invocation always occurs after releasing it.
  mutable std::mutex mutex;
  // Used with mutex for worker queue admission and caller diagnostic waits; mutations notify after changing
  // predicates.
  mutable std::condition_variable changed;
  // Queue-mutex-protected current epoch/argv/enabled state; worker snapshots argv before unlocking for helper
  // execution.
  MermaidRenderConfiguration configuration;
  // Queue-mutex-protected FIFO of unique source work; reconfigure/shutdown drop queued authority before waking
  // worker.
  std::deque<Work> queue;
  // Queue-mutex-protected current helper work; worker owns execution while callers may remove waiter identities
  // to cancel it.
  std::optional<Work> active;
  // Owned published results under mutex; append occurs before notification, and consumer swap releases
  // admission capacity.
  std::vector<MermaidRenderCompletion> completions;
  // Registration replaces this share under mutex. Producers retain snapshots
  // for unlocked invocation; replacement cannot retire an in-flight callback.
  CompletionNotifier completion_notifier;
  // Current-epoch result cache under mutex, oldest first; cache hits move entries to newest position.
  std::deque<CacheEntry> cache;
  // Queue-mutex-protected accepted text byte total; updated with every cache insert/erase and reset on
  // configuration change.
  std::size_t accepted_cache_bytes = 0;
  // Queue-mutex-protected one-way admission/cancellation predicate; worker observes it before further
  // work/publication.
  bool shutting_down = false;
  // Owned stable descriptors: callers write nonblocking wake bytes, worker drains/reads; remain alive through
  // worker join.
  Pipe wake_pipe;
  // Exclusive joinable worker; coordinator shutdown stops and joins it before earlier declared shared state is
  // destroyed.
  ava::core::JoinThread worker;
};

ava::core::Result<std::unique_ptr<MermaidRenderCoordinator>> MermaidRenderCoordinator::create(MermaidRenderConfiguration configuration)
{
  if (auto valid = validate_configuration(configuration); !valid)
    return std::unexpected(std::move(valid.error()));
  try
  {
    auto wake_pipe = make_pipe(true, true);
    if (!wake_pipe)
      return std::unexpected(coordinator_error(ava::core::ErrorCategory::Io, "failed to create Mermaid coordinator wake pipe"));
    auto coordinator =
        std::unique_ptr<MermaidRenderCoordinator>(new MermaidRenderCoordinator(std::make_unique<Impl>(std::move(configuration), std::move(*wake_pipe))));
    coordinator->impl_->start();
    return coordinator;
  }
  catch (...)
  {
    return std::unexpected(coordinator_error(ava::core::ErrorCategory::Unknown, "failed to start Mermaid render coordinator"));
  }
}

MermaidRenderCoordinator::MermaidRenderCoordinator(std::unique_ptr<Impl> impl) : impl_(std::move(impl))
{
}

MermaidRenderCoordinator::~MermaidRenderCoordinator()
{
  shutdown();
}

ava::core::VoidResult MermaidRenderCoordinator::reconfigure(MermaidRenderConfiguration configuration)
{
  if (auto valid = validate_configuration(configuration); !valid)
    return std::unexpected(std::move(valid.error()));
  // Validate caller data before locking; mutate configuration, queues, results, and cache as one epoch change
  // before waking active work.
  std::lock_guard lock(impl_->mutex);
  if (impl_->shutting_down)
    return std::unexpected(coordinator_error(ava::core::ErrorCategory::Io, "Mermaid coordinator is shut down"));
  if (configuration.epoch == impl_->configuration.epoch)
  {
    if (configuration.enabled == impl_->configuration.enabled && configuration.argv == impl_->configuration.argv)
      return {};
    return std::unexpected(coordinator_error(ava::core::ErrorCategory::InvalidArgument, "changed Mermaid configuration requires a new epoch"));
  }
  impl_->configuration = std::move(configuration);
  impl_->queue.clear();
  impl_->completions.clear();
  impl_->cache.clear();
  impl_->accepted_cache_bytes = 0;
  if (impl_->active)
    impl_->active->identities.clear();
  impl_->signal_worker();
  return {};
}

MermaidEnqueueResult MermaidRenderCoordinator::enqueue(MermaidRenderRequest request)
{
  Impl::CompletionNotifier notification;
  // Keep publication and its snapshot together, then release the lock before
  // invoking code supplied by another owner. The snapshot itself cannot allocate.
  auto const result = [&]() -> MermaidEnqueueResult {
  // One mutex covers deduplication and capacity accounting across queued, active, and undrained results; no
  // helper executes in this critical section.
    std::lock_guard lock(impl_->mutex);
    if (impl_->shutting_down || request.config_epoch != impl_->configuration.epoch)
      return MermaidEnqueueResult::StaleEpoch;
    if (impl_->identity_present_locked(request.identity))
      return MermaidEnqueueResult::AttachedToExisting;
    if (impl_->pending_identities_locked() + impl_->completions.size() >= kMaxMermaidQueuedRequests)
      return MermaidEnqueueResult::QueueFull;
    if (request.source.size() > kMaxMermaidSourceBytes)
    {
      impl_->add_completion_locked(request.identity, MermaidRenderOutcome::SourceTooLarge);
      notification = impl_->completion_notifier;
      return MermaidEnqueueResult::CompletedFallback;
    }
    if (!impl_->configuration.enabled)
    {
      impl_->add_completion_locked(request.identity, MermaidRenderOutcome::Disabled);
      notification = impl_->completion_notifier;
      return MermaidEnqueueResult::CompletedFallback;
    }
  // Immediate cache/fallback results are published before notification just like worker results, so the
  // consumer cannot miss synchronous completion.
    if (auto cached = impl_->find_cache_locked(request.source); cached != impl_->cache.end())
    {
      auto entry = std::move(*cached);
      impl_->cache.erase(cached);
      impl_->cache.push_back(entry);
      impl_->add_completion_locked(request.identity, entry.outcome, entry.text, true);
      notification = impl_->completion_notifier;
      return MermaidEnqueueResult::CompletedFromCache;
    }
  // Attach matching source waiters before creating new work; one helper execution can satisfy several opaque
  // identities.
    if (impl_->active && impl_->active->epoch == request.config_epoch && impl_->active->source == request.source)
    {
      impl_->active->identities.push_back(request.identity);
      return MermaidEnqueueResult::AttachedToExisting;
    }
    if (auto queued = std::ranges::find(impl_->queue, request.source, &Impl::Work::source); queued != impl_->queue.end())
    {
      queued->identities.push_back(request.identity);
      return MermaidEnqueueResult::AttachedToExisting;
    }
    impl_->queue.push_back(Impl::Work{.epoch = request.config_epoch, .source = std::move(request.source), .identities = {request.identity}});
    impl_->signal_worker();
    return MermaidEnqueueResult::Queued;
  }();
  Impl::notify_completion(notification);
  return result;
}

bool MermaidRenderCoordinator::cancel(std::uint64_t identity, std::uint64_t config_epoch)
{
  Impl::CompletionNotifier notification;
  // Keep publication and its snapshot together, then release the lock before
  // invoking code supplied by another owner. The snapshot itself cannot allocate.
  auto const result = [&]() -> bool {
  // Remove the caller identity while holding queue authority, then publish cancellation before waking the
  // consumer/worker.
    std::lock_guard lock(impl_->mutex);
    if (impl_->shutting_down || config_epoch != impl_->configuration.epoch)
      return false;
    if (impl_->active && impl_->active->epoch == config_epoch)
    {
      auto found = std::ranges::find(impl_->active->identities, identity);
      if (found != impl_->active->identities.end())
      {
        impl_->active->identities.erase(found);
        impl_->add_completion_locked(identity, MermaidRenderOutcome::Canceled);
        notification = impl_->completion_notifier;
        impl_->signal_worker();
        return true;
      }
    }
    for (auto work = impl_->queue.begin(); work != impl_->queue.end(); ++work)
    {
      auto found = std::ranges::find(work->identities, identity);
      if (found == work->identities.end())
        continue;
      work->identities.erase(found);
      if (work->identities.empty())
        impl_->queue.erase(work);
      impl_->add_completion_locked(identity, MermaidRenderOutcome::Canceled);
      notification = impl_->completion_notifier;
      impl_->changed.notify_all();
      return true;
    }
    return false;
  }();
  Impl::notify_completion(notification);
  return result;
}

std::vector<MermaidRenderCompletion> MermaidRenderCoordinator::take_completions()
{
  std::lock_guard lock(impl_->mutex);
  std::vector<MermaidRenderCompletion> result;
  result.swap(impl_->completions);
  return result;
}

void MermaidRenderCoordinator::set_completion_notifier(std::function<void()> notifier)
{
  // Allocate before changing registration. Keep the old share outside the lock:
  // destroying user captures may itself reenter an owner.
  auto replacement = notifier ? std::make_shared<std::function<void()> const>(std::move(notifier)) : Impl::CompletionNotifier{};
  Impl::CompletionNotifier retired;
  Impl::CompletionNotifier notification;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->shutting_down)
      return;
    retired = std::exchange(impl_->completion_notifier, std::move(replacement));
    if (!impl_->completions.empty())
      notification = impl_->completion_notifier;
  }
  // Late registration observes already published work without calling unknown
  // code under the queue lock. Empty registration does not wait for old calls.
  Impl::notify_completion(notification);
}

MermaidRenderCoordinatorStats MermaidRenderCoordinator::stats() const
{
  std::lock_guard lock(impl_->mutex);
  return {.config_epoch = impl_->configuration.epoch,
          .queued_requests = impl_->queue.size(),
          .pending_identities = impl_->pending_identities_locked(),
          .completion_count = impl_->completions.size(),
          .cache_entries = impl_->cache.size(),
          .accepted_cache_bytes = impl_->accepted_cache_bytes,
          .in_flight = impl_->active.has_value(),
          .shutting_down = impl_->shutting_down};
}

bool MermaidRenderCoordinator::wait_for_in_flight(std::chrono::milliseconds timeout) const
{
  std::unique_lock lock(impl_->mutex);
  return impl_->changed.wait_for(lock, timeout, [&] { return impl_->active.has_value() || impl_->shutting_down; }) && impl_->active.has_value();
}

bool MermaidRenderCoordinator::wait_until_idle(std::chrono::milliseconds timeout) const
{
  std::unique_lock lock(impl_->mutex);
  return impl_->changed.wait_for(lock, timeout, [&] { return !impl_->active && impl_->queue.empty(); });
}

void MermaidRenderCoordinator::shutdown() noexcept
{
  if (!impl_)
    return;
  Impl::CompletionNotifier retired;
  {
    std::lock_guard lock(impl_->mutex);
    retired = std::exchange(impl_->completion_notifier, {});
    if (!impl_->shutting_down)
    {
      impl_->shutting_down = true;
      impl_->queue.clear();
      impl_->completions.clear();
      impl_->cache.clear();
      impl_->accepted_cache_bytes = 0;
      if (impl_->active)
        impl_->active->identities.clear();
    }
    impl_->signal_worker();
  }
  // Release the queue mutex before joining: the worker must reacquire it to observe cancellation and retire
  // active state.
  impl_->worker.request_stop();
  impl_->changed.notify_all();
  if (impl_->worker.joinable())
    impl_->worker.join();
}

}  // namespace ava::app
