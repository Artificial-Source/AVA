#include "sys.h"
#include "DebugTerminal.h"

#ifdef CWDEBUG
#include "ava/core/Signals.h"
#include "ava/core/UniqueFd.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <ostream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <ext/stdio_filebuf.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ava::app {
namespace {

constexpr auto tty_report_timeout = std::chrono::seconds(10);
constexpr auto debugger_attach_timeout = std::chrono::seconds(60);
constexpr auto debugger_attach_poll_interval = std::chrono::milliseconds(100);
constexpr std::size_t max_tty_line_size = 256;

// The command run inside the new terminal window. It writes the window's tty path to the
// handshake FIFO ($1) and then either becomes the debugger ($2) attached to AVA ($3), or idles
// so that the window stays open to display debug output. All values arrive as positional
// parameters; nothing is interpolated into the script text.
constexpr char const* terminal_script =
    "tty > \"$1\" || exit 1; "
    "if [ -n \"$2\" ]; then exec \"$2\" -q -p \"$3\"; fi; "
    "exec sleep infinity";

std::string errno_message(std::string_view operation, std::string_view subject, int error_number)
{
  std::string message(operation);
  if (!subject.empty())
  {
    message += " '";
    message += subject;
    message += '\'';
  }
  message += ": ";
  message += std::error_code(error_number, std::generic_category()).message();
  return message;
}

// Owns the private handshake directory and the FIFO inside it; removes both on destruction.
class HandshakeDirectory final
{
 public:
  HandshakeDirectory() = default;
  ~HandshakeDirectory()
  {
    if (!fifo_.empty())
      static_cast<void>(::unlink(fifo_.c_str()));
    if (!directory_.empty())
      static_cast<void>(::rmdir(directory_.c_str()));
  }

  HandshakeDirectory(HandshakeDirectory const&) = delete;
  HandshakeDirectory& operator=(HandshakeDirectory const&) = delete;

  // Create a fresh 0700 directory below $XDG_RUNTIME_DIR, or below /tmp when that is unset,
  // relative or unusable, containing a 0600 FIFO.
  // Returns an empty string on success, otherwise an actionable error message.
  std::string create()
  {
    std::vector<std::string> bases;
    char const* runtime_directory = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_directory != nullptr && runtime_directory[0] == '/')
      bases.emplace_back(runtime_directory);
    bases.emplace_back("/tmp");
    std::string error;
    for (std::string const& base : bases)
    {
      std::string const pattern = base + "/ava-debug-terminal-XXXXXX";
      std::vector<char> buffer(pattern.begin(), pattern.end());
      buffer.push_back('\0');
      // mkdtemp creates the directory with mode 0700.
      if (::mkdtemp(buffer.data()) != nullptr)
      {
        directory_ = buffer.data();
        break;
      }
      error = errno_message("cannot create handshake directory", pattern, errno);
    }
    if (directory_.empty())
      return error;
    std::string fifo = directory_ + "/tty";
    if (::mkfifo(fifo.c_str(), 0600) != 0)
      return errno_message("cannot create handshake FIFO", fifo, errno);
    fifo_ = std::move(fifo);
    return {};
  }

  [[nodiscard]] std::string const& fifo() const noexcept { return fifo_; }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::string directory_;
  std::string fifo_;
};

// Fork a detached process that executes argv.
//
// The intermediate child starts a new session, restores default signal handling, points
// fds 0, 1 and 2 at /dev/null (terminal emulators print warnings that would otherwise corrupt
// AVA's TUI) and forks the final process, so no zombie remains and nothing waits for the window.
// Returns an empty string once the final process successfully exec-ed, otherwise an error message.
std::string spawn_detached(std::vector<std::string> const& arguments)
{
  // Prepare everything that allocates before forking.
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (std::string const& argument : arguments)
    argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);

  // The final process reports a failing execvp through this close-on-exec pipe.
  int exec_status_pipe[2];
  if (::pipe2(exec_status_pipe, O_CLOEXEC) != 0)
    return errno_message("cannot create exec status pipe", {}, errno);
  core::UniqueFd exec_status_reader(exec_status_pipe[0]);
  core::UniqueFd exec_status_writer(exec_status_pipe[1]);

  pid_t const child = ::fork();
  if (child < 0)
    return errno_message("cannot fork", arguments.front(), errno);
  if (child == 0)
  {
    if (::setsid() < 0 || !core::Signals::reset_child_signal_state())
      ::_exit(127);
    int const null_fd = ::open("/dev/null", O_RDWR);
    if (null_fd < 0 || ::dup2(null_fd, STDIN_FILENO) < 0 || ::dup2(null_fd, STDOUT_FILENO) < 0 || ::dup2(null_fd, STDERR_FILENO) < 0)
      ::_exit(127);
    if (null_fd > STDERR_FILENO)
      ::close(null_fd);
    pid_t const grandchild = ::fork();
    if (grandchild != 0)
      ::_exit(grandchild < 0 ? 127 : 0);
    ::execvp(argv[0], argv.data());
    int const exec_error = errno;
    [[maybe_unused]] int res = ::write(exec_status_writer.get(), &exec_error, sizeof(exec_error));
    ::_exit(127);
  }

  // Close our copy of the write end so that reading below sees EOF once the final process exec-ed.
  exec_status_writer.reset();

  int status = 0;
  while (::waitpid(child, &status, 0) < 0)
    if (errno != EINTR)
      return errno_message("cannot wait for spawn helper of", arguments.front(), errno);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return "the spawn helper for '" + arguments.front() + "' failed before executing it";

  int exec_error = 0;
  ssize_t received;
  while ((received = ::read(exec_status_reader.get(), &exec_error, sizeof(exec_error))) < 0 && errno == EINTR)
    ;
  if (received == static_cast<ssize_t>(sizeof(exec_error)))
    return errno_message("cannot execute terminal", arguments.front(), exec_error);
  return {};
}

// Read one newline-terminated line from the non-blocking fd until deadline.
// Returns the line without its newline, or std::nullopt on timeout, error or an overlong line.
std::optional<std::string> read_line_until(int fd, std::chrono::steady_clock::time_point deadline)
{
  std::string line;
  for (;;)
  {
    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0)
      return std::nullopt;
    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    int const ready = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0)
      return std::nullopt;
    char buffer[64];
    ssize_t const received = ::read(fd, buffer, sizeof(buffer));
    if (received < 0)
    {
      if (errno == EINTR || errno == EAGAIN)
        continue;
      return std::nullopt;
    }
    if (received == 0)
      continue;         // Cannot happen while we hold a write end ourselves; keep waiting regardless.
    line.append(buffer, static_cast<std::size_t>(received));
    if (auto const newline = line.find('\n'); newline != std::string::npos)
    {
      line.resize(newline);
      return line;
    }
    if (line.size() > max_tty_line_size)
      return std::nullopt;
  }
}

// Return true when this process is currently being ptraced, re-reading /proc/self/status.
bool debugger_attached()
{
  NAMESPACE_DEBUG::reset_being_traced();
  return NAMESPACE_DEBUG::being_traced();
}

}  // namespace

std::optional<DebugTerminalConfig> parse_debug_terminal_env(char const* terminal, char const* gdb)
{
  if (terminal == nullptr || terminal[0] == '\0')
    return std::nullopt;
  DebugTerminalConfig config;
  config.terminal = std::strcmp(terminal, "1") == 0 ? "konsole" : terminal;
  if (gdb == nullptr || gdb[0] == '\0' || std::strcmp(gdb, "1") == 0)
    config.gdb = "gdb";
  else if (std::strcmp(gdb, "0") != 0)
    config.gdb = gdb;
  return config;
}

bool valid_debug_tty_path(std::string_view tty_path)
{
  auto all_of = [](std::string_view text, auto predicate) {
    if (text.empty())
      return false;
    for (unsigned char character : text)
      if (!predicate(character))
        return false;
    return true;
  };
  auto is_digit = [](unsigned char c) { return c >= '0' && c <= '9'; };
  auto is_alnum = [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };

  constexpr std::string_view pts_prefix = "/dev/pts/";
  constexpr std::string_view tty_prefix = "/dev/tty";
  if (tty_path.starts_with(pts_prefix))
    return all_of(tty_path.substr(pts_prefix.size()), is_digit);
  if (tty_path.starts_with(tty_prefix))
    return all_of(tty_path.substr(tty_prefix.size()), is_alnum);
  return false;
}

struct DebugTerminal::Impl
{
  std::mutex stream_mutex;
  std::unique_ptr<__gnu_cxx::stdio_filebuf<char>> buffer;
  std::unique_ptr<std::ostream> stream;
  std::string error;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Each step either completes or records impl_->error and returns, leaving every earlier
// effect in place (an opened window, a redirected libcwd stream) because those remain useful.
DebugTerminal::DebugTerminal(DebugTerminalConfig config) : impl_(std::make_unique<Impl>())
{
  bool const attach_debugger = !config.gdb.empty();

  // Create the handshake FIFO and hold both ends: the extra write end guarantees that poll
  // neither reports a hangup before the window opened its writer nor after it closed it.
  HandshakeDirectory handshake;
  if (std::string error = handshake.create(); !error.empty())
  {
    impl_->error = std::move(error);
    return;
  }
  core::UniqueFd reader(::open(handshake.fifo().c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
  if (!reader)
  {
    impl_->error = errno_message("cannot open handshake FIFO", handshake.fifo(), errno);
    return;
  }
  core::UniqueFd keepalive_writer(::open(handshake.fifo().c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC));
  if (!keepalive_writer)
  {
    impl_->error = errno_message("cannot open handshake FIFO", handshake.fifo(), errno);
    return;
  }

  // With Yama ptrace_scope=1 only ancestors may attach; the debugger will be a descendant of the
  // terminal emulator instead, so allow any process of this user to attach. EINVAL means that
  // Yama is not present, in which case nothing needs to be allowed.
  if (attach_debugger && ::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0 && errno != EINVAL)
  {
    impl_->error = errno_message("cannot allow debugger attachment (prctl PR_SET_PTRACER)", {}, errno);
    return;
  }

  std::string const pid_text = std::to_string(::getpid());
  std::vector<std::string> arguments = {config.terminal};
  // Konsole (also when given as a path) opens its window fullscreen, to show as much debug output as possible.
  std::string_view const terminal_name = std::string_view(config.terminal).substr(config.terminal.rfind('/') + 1);
  if (terminal_name == "konsole")
  {
    // Run konsole full-screen using a dedicate profile.
    arguments.emplace_back("--profile");
    arguments.emplace_back("AVA-DebugTerminal");
    arguments.emplace_back("--fullscreen");
  }
  arguments.insert(arguments.end(), {"-e", "/bin/sh", "-c", terminal_script, "ava-debug-terminal", handshake.fifo(), config.gdb, pid_text});
  if (std::string error = spawn_detached(arguments); !error.empty())
  {
    impl_->error = std::move(error);
    return;
  }

  std::optional<std::string> const tty_path = read_line_until(reader.get(), std::chrono::steady_clock::now() + tty_report_timeout);
  if (!tty_path)
  {
    impl_->error = "terminal '" + config.terminal + "' did not report its tty within " + std::to_string(tty_report_timeout.count()) +
                   " seconds; it must accept `-e <command> <args>...`";
    return;
  }
  if (!valid_debug_tty_path(*tty_path))
  {
    impl_->error = "terminal '" + config.terminal + "' reported an unexpected tty: '" + tty_path->substr(0, 64) + "'";
    return;
  }

  core::UniqueFd output_fd(::open(tty_path->c_str(), O_WRONLY | O_NOCTTY | O_CLOEXEC));
  if (!output_fd)
  {
    impl_->error = errno_message("cannot open debug terminal", *tty_path, errno);
    return;
  }
  struct stat status{};
  if (::isatty(output_fd.get()) != 1 || ::fstat(output_fd.get(), &status) != 0 || status.st_uid != ::geteuid())
  {
    impl_->error = "debug terminal is not a terminal owned by the current user: '" + *tty_path + "'";
    return;
  }

  // The descriptor-taking GNU stream buffer owns and closes the fd from here on.
  impl_->buffer = std::make_unique<__gnu_cxx::stdio_filebuf<char>>(output_fd.release(), std::ios::out);
  if (!impl_->buffer->is_open())
  {
    impl_->buffer.reset();
    impl_->error = "cannot attach a stream to debug terminal '" + *tty_path + "'";
    return;
  }
  impl_->stream = std::make_unique<std::ostream>(impl_->buffer.get());
  *impl_->stream << std::unitbuf;
  Debug(libcw_do.set_ostream(impl_->stream.get(), &impl_->stream_mutex));

  if (!attach_debugger)
    return;

  // Wait until the debugger attached, so that breakpoints can be set before AVA continues.
  // The debugger stops this process upon attaching; the loop ends after the user continues it.
  *impl_->stream << "AVA (pid " << pid_text << "): waiting up to " << debugger_attach_timeout.count() << " seconds for " << config.gdb << " to attach...\n";
  // Also tell the invoking terminal where to look: from there AVA merely appears to hang while it
  // is stopped by the debugger, and the window may open on another display (e.g. a nested X server).
  char const* display = std::getenv("DISPLAY");
  std::cerr << "AVA_DEBUG_TERMINAL: AVA (pid " << pid_text << ") waits for " << config.gdb << " in the new " << config.terminal << " window ("
            << tty_path->c_str();
  if (display != nullptr && display[0] != '\0')
    std::cerr << " on DISPLAY=" << display;
  std::cerr << "); once it is attached, AVA stays stopped until you `continue` there." << std::endl;
  auto const deadline = std::chrono::steady_clock::now() + debugger_attach_timeout;
  while (!debugger_attached())
  {
    if (std::chrono::steady_clock::now() >= deadline)
    {
      // A debugger that attached but was killed or detached while AVA was stopped also ends up here:
      // AVA only resumes polling after the debugger is gone, by which time the deadline usually passed.
      impl_->error = config.gdb + " was not attached to AVA (pid " + pid_text + ") after " + std::to_string(debugger_attach_timeout.count()) +
                     " seconds: either it failed to attach (check its window and /proc/sys/kernel/yama/ptrace_scope)"
                     " or it detached or exited before AVA was continued";
      *impl_->stream << impl_->error << "; continuing without debugger.\n";
      // Let a later being_traced() query re-read the tracer state instead of trusting this timeout.
      NAMESPACE_DEBUG::reset_being_traced();
      return;
    }
    std::this_thread::sleep_for(debugger_attach_poll_interval);
  }
  NAMESPACE_DEBUG::set_being_traced();
}

DebugTerminal::~DebugTerminal()
{
  // Mirror LibcwdOutputSink: the stream is only destroyed during static teardown,
  // after which the process no longer produces libcwd output.
  if (impl_->stream)
    impl_->stream->flush();
  impl_->stream.reset();
  impl_->buffer.reset();
}

bool DebugTerminal::enabled() const noexcept
{
  return impl_->stream != nullptr;
}

std::string const& DebugTerminal::setup_error() const noexcept
{
  return impl_->error;
}

}  // namespace ava::app
#endif // CWDEBUG
