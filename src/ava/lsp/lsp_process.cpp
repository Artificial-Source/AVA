#include "sys.h"
#include "ava/lsp/lsp_client.h"
#include "ava/lsp/lsp_client_internal.h"
#include "ava/core/AnchorOpen.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <unistd.h>

namespace ava::lsp::lsp_client_internal {

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
}  // namespace ava::lsp::lsp_client_internal

namespace ava::lsp {

using namespace lsp_client_internal;

namespace {

constexpr char kTrustedExecPath[] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
constexpr auto kTerminationGrace = std::chrono::milliseconds(50);
constexpr auto kTerminationPollInterval = std::chrono::milliseconds(5);

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

void close_fd(int& fd) noexcept
{
  if (fd >= 0)
  {
    close(fd);
    fd = -1;
  }
}

ava::core::Result<std::array<int, 2>> make_pipe(ServerConfig const& config)
{
  std::array<int, 2> fds{-1, -1};
  if (pipe(fds.data()) != 0)
    return std::unexpected(errno_error("failed to create LSP process pipe", config));

  for (auto& fd : fds)
  {
    int const original = fd;
    int const moved = fcntl(original, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    int const saved_errno = errno;
    close(original);
    fd = -1;
    if (moved < 0)
    {
      for (int const remaining : fds)
      {
        if (remaining >= 0)
          close(remaining);
      }
      errno = saved_errno;
      return std::unexpected(errno_error("failed to move LSP process pipe above standard fds", config));
    }
    fd = moved;
  }
  return fds;
}

void close_nonstandard_fds(int preserved_fd, int second_preserved_fd = -1)
{
#if defined(__linux__) && defined(SYS_close_range)
  if (preserved_fd < 0 && second_preserved_fd < 0 && syscall(SYS_close_range, static_cast<unsigned int>(STDERR_FILENO + 1), ~0U, 0U) == 0)
    return;
#endif
  long const open_max = sysconf(_SC_OPEN_MAX);
  int const max_fd = open_max > 0 ? static_cast<int>(open_max) : 1024;
  for (int fd = STDERR_FILENO + 1; fd < max_fd; ++fd)
  {
    if (fd != preserved_fd && fd != second_preserved_fd)
      close(fd);
  }
}

bool allowlisted_lsp_environment_name(std::string_view name)
{
  constexpr std::array<std::string_view, 15> names{
      "HOME",           "USER",          "LOGNAME",        "TMPDIR", "TMP",       "TEMP", "LANG", "LANGUAGE", "LC_ALL", "XDG_CONFIG_HOME",
      "XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "TERM",   "COLORTERM",
  };
  return std::ranges::find(names, name) != names.end() || (name.starts_with("LC_") && name.size() > 3);
}

std::vector<std::string> lsp_environment()
{
  // LSP servers are arbitrary local programs. Retain only terminal, locale,
  // temporary-directory, XDG, and identity compatibility variables. AVA has no
  // configured/tested toolchain home requirement, so no compiler/runtime home
  // is inherited. In particular, provider/cloud/token variables and AVA_*
  // variables are never forwarded.
  std::vector<std::string> values;
  std::vector<std::string_view> names;
  for (char** inherited = ::environ; inherited != nullptr && *inherited != nullptr; ++inherited)
  {
    std::string_view const variable(*inherited);
    auto const separator = variable.find('=');
    if (separator == std::string_view::npos || separator == 0)
      continue;
    auto const name = variable.substr(0, separator);
    if (!allowlisted_lsp_environment_name(name) || std::ranges::find(names, name) != names.end())
      continue;
    names.push_back(name);
    values.emplace_back(variable);
  }
  values.emplace_back(std::string("PATH=") + kTrustedExecPath);
  return values;
}

std::vector<std::string> trusted_executable_candidates(std::string_view command)
{
  if (command.find('/') != std::string_view::npos)
    return {std::string(command)};

  std::vector<std::string> candidates;
  std::string_view remaining(kTrustedExecPath);
  while (!remaining.empty())
  {
    auto const separator = remaining.find(':');
    auto const directory = remaining.substr(0, separator);
    if (!directory.empty())
      candidates.emplace_back(std::string(directory) + "/" + std::string(command));
    if (separator == std::string_view::npos)
      break;
    remaining.remove_prefix(separator + 1);
  }
  return candidates;
}

std::string exit_detail(siginfo_t const& info)
{
  switch (info.si_code)
  {
    case CLD_EXITED:
      return "exit " + std::to_string(info.si_status);
    case CLD_KILLED:
      return "signal " + std::to_string(info.si_status);
    case CLD_DUMPED:
      return "signal " + std::to_string(info.si_status) + " (core dumped)";
    default:
      return "unexpected child status code " + std::to_string(info.si_code) + " value " + std::to_string(info.si_status);
  }
}

int waitid_retry(idtype_t id_type, id_t id, siginfo_t* info, int options)
{
  while (true)
  {
    auto const result = waitid(id_type, id, info, options);
    if (result < 0 && errno == EINTR)
      continue;
    return result;
  }
}

}  // namespace

ava::core::VoidResult SubprocessLspClient::launch()
{
  auto stdin_pipe = make_pipe(config_);
  if (!stdin_pipe)
    return std::unexpected(std::move(stdin_pipe.error()));
  auto stdout_pipe = make_pipe(config_);
  if (!stdout_pipe)
  {
    close((*stdin_pipe)[0]);
    close((*stdin_pipe)[1]);
    return std::unexpected(std::move(stdout_pipe.error()));
  }
  auto gate_pipe = make_pipe(config_);
  if (!gate_pipe)
  {
    close((*stdin_pipe)[0]);
    close((*stdin_pipe)[1]);
    close((*stdout_pipe)[0]);
    close((*stdout_pipe)[1]);
    return std::unexpected(std::move(gate_pipe.error()));
  }

  int const stdout_flags = fcntl((*stdout_pipe)[0], F_GETFL, 0);
  int const stdin_flags = fcntl((*stdin_pipe)[1], F_GETFL, 0);
  if (stdout_flags < 0 || stdin_flags < 0 || fcntl((*stdout_pipe)[0], F_SETFL, stdout_flags | O_NONBLOCK) < 0 ||
      fcntl((*stdin_pipe)[1], F_SETFL, stdin_flags | O_NONBLOCK) < 0)
  {
    auto error = errno_error("failed to configure LSP stdio pipes", config_);
    close((*stdin_pipe)[0]);
    close((*stdin_pipe)[1]);
    close((*stdout_pipe)[0]);
    close((*stdout_pipe)[1]);
    close((*gate_pipe)[0]);
    close((*gate_pipe)[1]);
    return std::unexpected(std::move(error));
  }

  // Prepare all storage used by exec before fork. The child only performs
  // async-signal-safe descriptor/process operations before execve.
  auto const child_cwd_path = config_.process_cwd.empty() ? config_.workspace_root : config_.process_cwd;
  auto executable_candidates = trusted_executable_candidates(config_.argv.front());
  auto environment_strings = lsp_environment();
  // Preserve the caller's logical cwd identity for servers that inspect PWD.
  environment_strings.push_back("PWD=" + child_cwd_path.lexically_normal().string());
  std::vector<char*> environment;
  environment.reserve(environment_strings.size() + 1);
  for (auto& value : environment_strings) environment.push_back(value.data());
  environment.push_back(nullptr);
  std::vector<char*> argv;
  argv.reserve(config_.argv.size() + 1);
  for (auto& argument : config_.argv) argv.push_back(argument.data());
  argv.push_back(nullptr);

  auto opened_cwd = ava::core::open_readable(*config_.anchor_set, child_cwd_path, O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC);
  if (!opened_cwd)
  {
    close((*stdin_pipe)[0]);
    close((*stdin_pipe)[1]);
    close((*stdout_pipe)[0]);
    close((*stdout_pipe)[1]);
    close((*gate_pipe)[0]);
    close((*gate_pipe)[1]);
    return std::unexpected(lsp_error(opened_cwd.error().category(), "failed to open LSP process cwd", config_));
  }
  int const child_cwd_fd = ::fcntl(opened_cwd->fd(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
  if (child_cwd_fd < 0)
  {
    auto error = errno_error("failed to duplicate LSP process cwd", config_);
    close((*stdin_pipe)[0]);
    close((*stdin_pipe)[1]);
    close((*stdout_pipe)[0]);
    close((*stdout_pipe)[1]);
    close((*gate_pipe)[0]);
    close((*gate_pipe)[1]);
    return std::unexpected(std::move(error));
  }

  int executable_fd = -1;
  if (config_.executable_identity)
  {
    auto const& identity = *config_.executable_identity;
    auto opened = ava::core::open_readable(*config_.anchor_set, identity.executable_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (opened)
      executable_fd = ::fcntl(opened->fd(), F_DUPFD_CLOEXEC, 3);
    struct stat metadata{};
    bool const valid =
        executable_fd >= 0 && ::fstat(executable_fd, &metadata) == 0 && S_ISREG(metadata.st_mode) && metadata.st_nlink == 1 &&
        static_cast<std::uintmax_t>(metadata.st_uid) == identity.owner_uid && static_cast<std::uintmax_t>(metadata.st_gid) == identity.owner_gid &&
        static_cast<std::uintmax_t>(metadata.st_mode) == identity.mode && static_cast<std::uintmax_t>(metadata.st_nlink) == identity.link_count &&
        static_cast<std::uintmax_t>(metadata.st_dev) == identity.device && static_cast<std::uintmax_t>(metadata.st_ino) == identity.inode &&
        static_cast<std::uintmax_t>(metadata.st_size) == identity.size && static_cast<std::int64_t>(metadata.st_ctim.tv_sec) == identity.changed_seconds &&
        static_cast<std::int64_t>(metadata.st_ctim.tv_nsec) == identity.changed_nanoseconds && (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
        (metadata.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
    if (!valid)
    {
      if (executable_fd >= 0)
        close(executable_fd);
      close(child_cwd_fd);
      close((*stdin_pipe)[0]);
      close((*stdin_pipe)[1]);
      close((*stdout_pipe)[0]);
      close((*stdout_pipe)[1]);
      close((*gate_pipe)[0]);
      close((*gate_pipe)[1]);
      return std::unexpected(lsp_error(ava::core::ErrorCategory::PermissionDenied, "built-in LSP executable identity is stale or unsafe", config_));
    }
  }

  pid_t const parent_pgid = getpgrp();
  pid_t const pid = fork();
  if (pid < 0)
  {
    auto const saved_errno = errno;
    close((*stdin_pipe)[0]);
    close((*stdin_pipe)[1]);
    close((*stdout_pipe)[0]);
    close((*stdout_pipe)[1]);
    close((*gate_pipe)[0]);
    close((*gate_pipe)[1]);
    close(child_cwd_fd);
    if (executable_fd >= 0)
      close(executable_fd);
    errno = saved_errno;
    return std::unexpected(errno_error("failed to fork LSP server", config_));
  }

  if (pid == 0)
  {
    close((*stdin_pipe)[1]);
    close((*stdout_pipe)[0]);
    close((*gate_pipe)[1]);
    if (setpgid(0, 0) != 0)
      _exit(127);
    char release = '\0';
    if (read_retry((*gate_pipe)[0], &release, 1) != 1)
      _exit(127);
    close((*gate_pipe)[0]);

    if (dup2((*stdin_pipe)[0], STDIN_FILENO) < 0)
      _exit(127);
    if (dup2((*stdout_pipe)[1], STDOUT_FILENO) < 0)
      _exit(127);
    int const dev_null = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (dev_null < 0 || dup2(dev_null, STDERR_FILENO) < 0)
      _exit(127);
    if (dev_null != STDERR_FILENO)
      close(dev_null);
    close((*stdin_pipe)[0]);
    close((*stdout_pipe)[1]);
    if (fchdir(child_cwd_fd) != 0)
      _exit(127);
    close(child_cwd_fd);
    close_nonstandard_fds(executable_fd);
    if (executable_fd >= 0)
      fexecve(executable_fd, argv.data(), environment.data());
    else
      for (auto const& executable : executable_candidates) execve(executable.c_str(), argv.data(), environment.data());
    _exit(127);
  }

  close((*stdin_pipe)[0]);
  close((*stdout_pipe)[1]);
  close((*gate_pipe)[0]);
  close(child_cwd_fd);
  stdin_fd_ = (*stdin_pipe)[1];
  stdout_fd_ = (*stdout_pipe)[0];

  auto abort_before_exec = [&](std::string message, int saved_errno) -> ava::core::VoidResult {
    close((*gate_pipe)[1]);
    if (executable_fd >= 0)
      close(executable_fd);
    kill(pid, SIGKILL);
    int status = 0;
    waitpid_retry(pid, &status, 0);
    close_fds();
    pid_ = -1;
    owned_pgid_ = -1;
    errno = saved_errno;
    return std::unexpected(errno_error(std::move(message), config_));
  };

  bool group_set = false;
  for (int attempt = 0; attempt < 20; ++attempt)
  {
    if (setpgid(pid, pid) == 0)
    {
      group_set = true;
      break;
    }
    if (errno != EINTR)
      break;
  }
  int const setpgid_errno = errno;
  pid_t const child_pgid = getpgid(pid);
  int const getpgid_errno = errno;
  if (!group_set || pid <= 1 || parent_pgid <= 0 || child_pgid != pid || child_pgid == parent_pgid)
  {
    return abort_before_exec("failed to establish verified LSP process group", group_set ? getpgid_errno : setpgid_errno);
  }

  pid_ = pid;
  owned_pgid_ = child_pgid;
  char const release = '1';
  if (write_retry((*gate_pipe)[1], &release, 1) != 1)
  {
    int const saved_errno = errno;
    return abort_before_exec("failed to release LSP server for exec", saved_errno);
  }
  close((*gate_pipe)[1]);
  if (executable_fd >= 0)
    close(executable_fd);
  return {};
}

bool SubprocessLspClient::is_alive()
{
  std::lock_guard<std::mutex> lock(operation_mutex_);
  return check_child_running().has_value();
}

ava::core::VoidResult SubprocessLspClient::check_child_running()
{
  if (pid_ < 0)
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Io, "LSP server is not running", config_));
  }

  siginfo_t info{};
  if (waitid_retry(P_PID, static_cast<id_t>(pid_), &info, WEXITED | WNOHANG | WNOWAIT) != 0)
  {
    if (errno == ECHILD)
    {
      close_fd(stdin_fd_);
      pid_ = -1;
      owned_pgid_ = -1;
      return std::unexpected(lsp_error(ava::core::ErrorCategory::Io, "LSP server is already reaped", config_));
    }
    return std::unexpected(errno_error("failed to inspect LSP server", config_));
  }
  if (info.si_pid == 0)
    return {};

  auto error = lsp_error(ava::core::ErrorCategory::Io, "LSP server exited", config_);
  error.with_context("status", exit_detail(info));
  terminate_child();
  return std::unexpected(std::move(error));
}

void SubprocessLspClient::close_fds() noexcept
{
  close_fd(stdin_fd_);
  close_fd(stdout_fd_);
}

void SubprocessLspClient::terminate_child() noexcept
{
  pid_t const pid = pid_;
  pid_t const pgid = owned_pgid_;
  close_fd(stdin_fd_);

  bool const verified_group = pid > 1 && pgid > 1 && pgid == pid && getpgid(pid) == pgid;
  if (!verified_group)
  {
    if (pid > 1)
    {
      kill(pid, SIGKILL);
      int status = 0;
      waitpid_retry(pid, &status, 0);
    }
    pid_ = -1;
    owned_pgid_ = -1;
    return;
  }

  kill(-pgid, SIGTERM);
  bool group_still_verified = true;
  auto const grace_deadline = std::chrono::steady_clock::now() + kTerminationGrace;
  while (std::chrono::steady_clock::now() < grace_deadline)
  {
    siginfo_t info{};
    if (waitid_retry(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) != 0)
    {
      if (errno == ECHILD)
        group_still_verified = false;
      break;
    }
    if (info.si_pid != 0)
      break;
    std::this_thread::sleep_for(kTerminationPollInterval);
  }

  if (group_still_verified)
    kill(-pgid, SIGKILL);
  int status = 0;
  waitpid_retry(pid, &status, 0);
  pid_ = -1;
  owned_pgid_ = -1;
}

}  // namespace ava::lsp
