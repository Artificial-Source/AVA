#include "sys.h"
#include "ava/mcp/stdio_client_support.h"
#include "ava/core/json.h"
#include "ava/core/process_args.h"

#include <fcntl.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#include <cerrno>
#include <chrono>
#include <thread>
#include <utility>
#include <unistd.h>

namespace ava::mcp {

UniqueFd::UniqueFd(int fd) noexcept : fd_(fd)
{
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(other.release())
{
}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept
{
  if (this != &other)
    reset(other.release());
  return *this;
}

UniqueFd::~UniqueFd()
{
  reset();
}

int UniqueFd::get() const noexcept
{
  return fd_;
}

int UniqueFd::release() noexcept
{
  int const fd = fd_;
  fd_ = -1;
  return fd;
}

void UniqueFd::reset(int fd) noexcept
{
  if (fd_ >= 0)
    close(fd_);
  fd_ = fd;
}

ScopedSignalIgnore::ScopedSignalIgnore(int signal) : signal_(signal)
{
  struct sigaction action{};
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  if (sigaction(signal_, &action, &previous_) == 0)
    installed_ = true;
}

ScopedSignalIgnore::~ScopedSignalIgnore()
{
  if (installed_)
    sigaction(signal_, &previous_, nullptr);
}

ava::core::Error mcp_error(ava::core::ErrorCategory category, std::string message, McpServerConfig const& server)
{
  static_cast<void>(server);
  return ava::core::Error(category, std::move(message));
}

ava::core::Error errno_error(std::string message, McpServerConfig const& server)
{
  auto error = mcp_error(ava::core::ErrorCategory::Io, std::move(message), server);
  error.with_context("errno", std::to_string(errno));
  return error;
}

ava::core::Error protocol_error(std::string message, McpServerConfig const& server)
{
  return mcp_error(ava::core::ErrorCategory::Tool, std::move(message), server);
}

bool is_canceled(CancelCallback const& cancel_requested)
{
  return cancel_requested && cancel_requested();
}

ava::core::Error canceled_error(std::string message, McpServerConfig const& server)
{
  static_cast<void>(server);
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, std::move(message), ava::core::ErrorCode::Canceled);
  error.with_context("canceled", "true");
  return error;
}

ava::core::Result<std::array<int, 2>> make_pipe(McpServerConfig const& server)
{
  std::array<int, 2> fds{-1, -1};
  if (pipe(fds.data()) != 0)
    return std::unexpected(errno_error("failed to create MCP process pipe", server));
  for (auto& fd : fds)
  {
    if (fd > STDERR_FILENO)
      continue;
    int const moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    int const move_errno = errno;
    close(fd);
    if (moved < 0)
    {
      for (int const pipe_fd : fds)
      {
        if (pipe_fd >= 0 && pipe_fd != fd)
          close(pipe_fd);
      }
      errno = move_errno;
      return std::unexpected(errno_error("failed to move MCP pipe above standard fds", server));
    }
    fd = moved;
  }
  return fds;
}

bool set_child_process_group(pid_t pid)
{
  for (int attempt = 0; attempt < 20; ++attempt)
  {
    if (setpgid(pid, pid) == 0 || errno == EACCES)
      return true;
    if (errno != EINTR && errno != ESRCH)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

pid_t waitpid_retry(pid_t pid, int* status, int options)
{
  while (true)
  {
    auto const waited = waitpid(pid, status, options);
    if (waited < 0 && errno == EINTR)
      continue;
    return waited;
  }
}

ssize_t read_retry(int fd, char* data, std::size_t size)
{
  while (true)
  {
    auto const bytes = read(fd, data, size);
    if (bytes < 0 && errno == EINTR)
      continue;
    return bytes;
  }
}

ssize_t write_retry(int fd, char const* data, std::size_t size)
{
  while (true)
  {
    auto const bytes = write(fd, data, size);
    if (bytes < 0 && errno == EINTR)
      continue;
    return bytes;
  }
}

std::size_t remaining_ms(std::chrono::steady_clock::time_point deadline)
{
  auto const now = std::chrono::steady_clock::now();
  if (now >= deadline)
    return 0;
  return static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

void close_fd(int& fd) noexcept
{
  if (fd >= 0)
  {
    close(fd);
    fd = -1;
  }
}

void close_nonstandard_fds()
{
#if defined(__linux__) && defined(SYS_close_range)
  if (syscall(SYS_close_range, static_cast<unsigned int>(STDERR_FILENO + 1), ~0U, 0U) == 0)
    return;
#endif
  long const open_max = sysconf(_SC_OPEN_MAX);
  int const max_fd = open_max > 0 ? static_cast<int>(open_max) : 1024;
  for (int fd = STDERR_FILENO + 1; fd < max_fd; ++fd) close(fd);
}

std::string json_string(std::string_view value)
{
  return "\"" + ava::core::json::escape(value) + "\"";
}

std::string exit_detail(int status)
{
  if (WIFEXITED(status))
    return "exit " + std::to_string(WEXITSTATUS(status));
  if (WIFSIGNALED(status))
    return "signal " + std::to_string(WTERMSIG(status));
  return "unknown status " + std::to_string(status);
}

std::vector<std::string> mcp_argv(McpServerConfig const& server)
{
  std::vector<std::string> argv;
  argv.reserve(server.args.size() + 1);
  argv.push_back(server.command);
  argv.insert(argv.end(), server.args.begin(), server.args.end());
  return argv;
}

std::filesystem::path child_working_dir(McpServerConfig const& server, McpStdioClientOptions const& options)
{
  if (server.scope == McpServerScope::Global)
    return ava::core::safe_global_process_cwd(server.source_path, options.workspace_dir);
  if (!options.workspace_dir.empty())
    return options.workspace_dir;
  return std::filesystem::current_path();
}

}  // namespace ava::mcp
