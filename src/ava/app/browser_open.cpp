#include "sys.h"
#include "ava/app/browser_open.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/close_descriptors.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ava::app {
namespace {

bool is_browser_url(std::string_view url)
{
  return url.starts_with("https://") || url.starts_with("http://");
}

bool contains_ascii_space(std::string_view value)
{
  return std::ranges::any_of(value, [](char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r'; });
}

bool executable_exists(std::string_view command)
{
  if (command.empty())
    return false;
  auto const command_text = std::string(command);
  if (command.find('/') != std::string_view::npos)
    return ::access(command_text.c_str(), X_OK) == 0;

  char const* path_env = std::getenv("PATH");
  if (path_env == nullptr)
    return false;
  std::string_view path(path_env);
  std::size_t start = 0;
  while (start <= path.size())
  {
    auto const end = path.find(':', start);
    auto const entry = path.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    auto candidate = entry.empty() ? std::string(".") : std::string(entry);
    candidate += "/";
    candidate += command_text;
    if (::access(candidate.c_str(), X_OK) == 0)
      return true;
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return false;
}

[[maybe_unused]] void redirect_stdio_to_devnull()
{
  auto const devnull = ::open("/dev/null", O_RDWR);
  if (devnull < 0)
    return;
  static_cast<void>(::dup2(devnull, STDIN_FILENO));
  static_cast<void>(::dup2(devnull, STDOUT_FILENO));
  static_cast<void>(::dup2(devnull, STDERR_FILENO));
  if (devnull > STDERR_FILENO)
    static_cast<void>(::close(devnull));
}

[[noreturn]] void browser_child_failed(int descriptor, int error_number) noexcept
{
  int const failure = error_number == 0 ? EIO : error_number;
  ssize_t written;
  do
    written = ::write(descriptor, &failure, sizeof(failure));
  while (written < 0 && errno == EINTR);
  _exit(127);
}

bool spawn_detached(std::vector<std::string> const& arguments)
{
  if (arguments.empty())
    return false;
  auto const launcher = arguments.front();
  if (!executable_exists(launcher))
    return false;

  // Resolve PATH candidates and snapshot argv/environment before either fork.
  // execve, unlike execvp, cannot enter libc's PATH or shell-fallback machinery.
  std::vector<std::string> candidates;
  if (launcher.find('/') != std::string::npos)
    candidates.push_back(launcher);
  else if (char const* path = std::getenv("PATH"))
  {
    std::string_view remaining(path);
    while (true)
    {
      auto const separator = remaining.find(':');
      auto const directory = remaining.substr(0, separator);
      candidates.push_back((directory.empty() ? "." : std::string(directory)) + "/" + launcher);
      if (separator == std::string_view::npos)
        break;
      remaining.remove_prefix(separator + 1);
    }
  }
  auto argument_storage = arguments;
  std::vector<char*> argv;
  argv.reserve(argument_storage.size() + 1);
  for (auto& argument : argument_storage)
    argv.push_back(argument.data());
  argv.push_back(nullptr);
  // Preserve execvp's ENOEXEC shell fallback with storage prepared here.
  std::string shell_path = "/bin/sh";
  std::vector<char*> shell_argv;
  shell_argv.reserve(argv.size() + 1);
  shell_argv.push_back(shell_path.data());
  shell_argv.push_back(nullptr);  // Child selects an already-owned candidate.
  for (std::size_t index = 1; index < argv.size(); ++index)
    shell_argv.push_back(argv[index]);
  std::vector<std::string> environment_storage;
  for (char** entry = ::environ; entry && *entry; ++entry)
    environment_storage.emplace_back(*entry);
  std::vector<char*> environment;
  environment.reserve(environment_storage.size() + 1);
  for (auto& entry : environment_storage)
    environment.push_back(entry.data());
  environment.push_back(nullptr);
  ava::core::UniqueFd null_stream(::open("/dev/null", O_RDWR | O_CLOEXEC));
  if (!null_stream)
    return false;
  std::array<int, 2> pipe{-1, -1};
  if (::pipe2(pipe.data(), O_CLOEXEC) != 0)
    return false;
  ava::core::UniqueFd status_read(pipe[0]);
  ava::core::UniqueFd status_write(pipe[1]);
  // Keep the handshake above stdio even when the application inherited a
  // closed standard stream, so dup2 cannot overwrite the success/failure pipe.
  if (status_write.get() <= STDERR_FILENO)
  {
    int const moved = ::fcntl(status_write.get(), F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0)
      return false;
    status_write.reset(moved);
  }

  auto const pid = ::fork();
  if (pid < 0)
    return false;
  if (pid == 0)
  {
    // A second libc fork would run inherited pthread_atfork callbacks in the
    // already-forked child. Linux clone with SIGCHLD has fork semantics here.
    auto const grandchild = ::syscall(SYS_clone, SIGCHLD, 0, nullptr, nullptr, 0);
    if (grandchild < 0)
      browser_child_failed(status_write.get(), errno);
    if (grandchild < 0)
      _exit(127);
    if (grandchild > 0)
      _exit(0);
    if (::setsid() < 0)
      browser_child_failed(status_write.get(), errno);
    for (int target : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO})
    {
      if (::dup2(null_stream.get(), target) < 0 || ::fcntl(target, F_SETFD, 0) < 0)
        browser_child_failed(status_write.get(), errno);
    }
    std::array<int, 1> const preserved{status_write.get()};
    if (int const error = ava::core::close_nonstandard_fds_except(preserved); error != 0)
      browser_child_failed(status_write.get(), error);
    int execute_error = ENOENT;
    for (auto& candidate : candidates)
    {
      ::execve(candidate.c_str(), argv.data(), environment.data());
      int const error = errno;
      if (error == ENOEXEC)
      {
        shell_argv[1] = candidate.data();
        ::execve(shell_path.c_str(), shell_argv.data(), environment.data());
        browser_child_failed(status_write.get(), errno);
      }
      if (error == EACCES)
        execute_error = error;
      else if (error != ENOENT && error != ENOTDIR)
        browser_child_failed(status_write.get(), error);
    }
    browser_child_failed(status_write.get(), execute_error);
  }

  status_write.reset();
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0)
  {
    if (errno == EINTR)
      continue;
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return false;
  int execute_error = 0;
  ssize_t count;
  do
    count = ::read(status_read.get(), &execute_error, sizeof(execute_error));
  while (count < 0 && errno == EINTR);
  // The writer is CLOEXEC. Only EOF after a successful intermediate child
  // means the actual browser launcher reached exec, not merely the second fork.
  return count == 0;
}

std::vector<std::vector<std::string>> browser_commands(std::string_view url)
{
  std::vector<std::vector<std::string>> commands;
  if (char const* browser = std::getenv("BROWSER"); browser != nullptr && *browser != '\0')
  {
    std::string_view browser_view(browser);
    if (!contains_ascii_space(browser_view))
      commands.push_back({std::string(browser_view), std::string(url)});
  }
  commands.push_back({"xdg-open", std::string(url)});
  commands.push_back({"gio", "open", std::string(url)});
  commands.push_back({"open", std::string(url)});
  commands.push_back({"wslview", std::string(url)});
  return commands;
}

}  // namespace

bool open_url_in_browser(std::string_view url)
{
  if (!is_browser_url(url))
    return false;
  if (std::getenv("AVA_DISABLE_BROWSER_OPEN") != nullptr)
    return false;

  for (auto const& command : browser_commands(url))
  {
    if (spawn_detached(command))
      return true;
  }
  return false;
}

}  // namespace ava::app
