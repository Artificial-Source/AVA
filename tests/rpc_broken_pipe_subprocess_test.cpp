#include "sys.h"
#include "support/process_test_support.h"
#include "support/test_env.h"
#include "support/test_harness.h"
#include "support/test_timeout.h"
#include "ava/core/UniqueFd.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#ifndef AVA_EXE_PATH
#define AVA_EXE_PATH ""
#endif

namespace {

std::optional<std::size_t> fd_count()
{
  std::error_code error;
  std::size_t count = 0;
  for (auto const& entry : std::filesystem::directory_iterator("/proc/self/fd", error))
  {
    static_cast<void>(entry);
    ++count;
  }
  if (error)
    return std::nullopt;
  return count;
}

struct Pipe
{
  ava::core::UniqueFd reader;
  ava::core::UniqueFd writer;

  bool open()
  {
    int descriptors[2];
    if (::pipe(descriptors) != 0)
      return false;
    reader.reset(descriptors[0]);
    writer.reset(descriptors[1]);
    return true;
  }

  void close()
  {
    reader.reset();
    writer.reset();
  }
};

int fail(std::string const& message)
{
  std::cerr << "FAIL: " << message << '\n';
  return 1;
}

}  // namespace

namespace {

int run_closed_pipe_test(bool close_stdout)
{
  auto const before_fds = fd_count();
  ava::tests::HermeticTestEnvironment environment("ava-rpc-broken-pipe-");
  ScopedEnvVar no_color("NO_COLOR", "1");
  auto const& root = environment.root();
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  Pipe input;
  Pipe output;
  Pipe errors;
  if (!input.open() || !output.open() || !errors.open())
    return fail("could not create subprocess pipes: " + std::string(std::strerror(errno)));

  pid_t const child = ::fork();
  if (child < 0)
    return fail("could not fork ava: " + std::string(std::strerror(errno)));
  if (child == 0)
  {
    struct sigaction default_sigpipe{};
    default_sigpipe.sa_handler = SIG_DFL;
    sigemptyset(&default_sigpipe.sa_mask);
    if (::sigaction(SIGPIPE, &default_sigpipe, nullptr) != 0 || ::dup2(input.reader.get(), STDIN_FILENO) < 0 ||
        ::dup2(output.writer.get(), STDOUT_FILENO) < 0 || ::dup2(errors.writer.get(), STDERR_FILENO) < 0)
      _exit(126);
    input.close();
    output.close();
    errors.close();
    if (::chdir(workspace.c_str()) != 0)
      _exit(126);
    ::execl(AVA_EXE_PATH, AVA_EXE_PATH, "--rpc", "--offline", static_cast<char*>(nullptr));
    _exit(127);
  }
  ava::tests::ScopedChild owner(child);
  struct sigaction ignore_sigpipe{};
  ignore_sigpipe.sa_handler = SIG_IGN;
  sigemptyset(&ignore_sigpipe.sa_mask);
  if (::sigaction(SIGPIPE, &ignore_sigpipe, nullptr) != 0)
    return fail("could not protect harness pipe writes: " + std::string(std::strerror(errno)));
  input.reader.reset();
  output.writer.reset();
  errors.writer.reset();
  if (close_stdout)
    output.reader.reset();
  else
    errors.reader.reset();
  std::string const request = close_stdout ? "{\"id\":\"broken-pipe\",\"type\":\"prompt\",\"message\":\"trigger worker response\"}\n"
                                           : "{\"id\":\"stderr-closed\",\"type\":\"get_protocol\"}\n";
  bool const request_written = write_all_to_descriptor_for_test(input.writer.get(), request.data(), request.size());
  if (!close_stdout)
    input.writer.reset();

  auto const deadline = ava::tests::now_plus_seconds(5);
  auto const status = owner.wait_until(deadline);
  input.writer.reset();
  std::string captured;
  bool const observed = close_stdout
                            ? ava::tests::read_until(errors.reader.get(), captured, "failed to write RPC JSONL record", ava::tests::now_plus_seconds(2))
                            : ava::tests::read_until(output.reader.get(), captured, "\"id\":\"stderr-closed\",\"type\":\"response\",\"success\":true",
                                                     ava::tests::now_plus_seconds(2));
  output.close();
  errors.close();
  auto const after_fds = fd_count();
  auto const scenario = close_stdout ? std::string("closed stdout") : std::string("closed stderr");
  if (!request_written)
    return fail(scenario + ": could not write the request");
  if (!status)
    return fail(scenario + ": ava did not terminate; captured: " + captured);
  int const expected_exit = close_stdout ? 1 : 0;
  if (!WIFEXITED(*status) || WEXITSTATUS(*status) != expected_exit)
    return fail(scenario + ": unexpected exit; raw status=" + std::to_string(*status) + "; captured: " + captured);
  if (!observed)
    return fail(scenario + ": missing " + (close_stdout ? "output I/O diagnostic" : "successful protocol response") + "; captured: " + captured);
  if (before_fds && after_fds && *before_fds != *after_fds)
    return fail("subprocess regression leaked parent file descriptors: before=" + std::to_string(*before_fds) + " after=" + std::to_string(*after_fds));
  std::cout << "PASS: " << scenario << " preserves the expected RPC outcome without fd leaks\n";
  return 0;
}

}  // namespace

int main()
{
  int const stdout_status = run_closed_pipe_test(true);
  int const stderr_status = run_closed_pipe_test(false);
  return stdout_status != 0 ? stdout_status : (stderr_status != 0 ? stderr_status : (ava::tests::failures() != 0 ? 1 : 0));
}
