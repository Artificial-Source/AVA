#include "sys.h"
#include "test_timeout.h"

// event-hook-gated OBSERVED FIFO SIDE_EFFECT initializes the plugin, opens its
// private FIFO, then publishes OBSERVED after receiving the event. SIDE_EFFECT
// and event.observed are emitted only after a parent-written byte; a 30s watchdog
// exits nonzero without either side effect. No inherited FD or environment is used.
// endpoint-eof writes its optional PID marker before initialized; refusal mode
// installs SIG_IGN before initialized; descendant's PID marker acknowledges its
// installed SIG_IGN, not merely a successful fork.
// The descendant scenario starts immediately after initialization, without a
// tool request, so callers can observe natural settlement before probing EOF.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

using namespace std::chrono_literals;

void initialized()
{
  std::cout << "{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava.plugin.v1\",\"plugin_version\":\"1.2.3\","
               "\"contributions\":{\"tools\":[]}}\n"
            << std::flush;
}

[[noreturn]] void loop_forever()
{
  while (true)
    std::this_thread::sleep_for(1s);
}

bool write_marker(std::filesystem::path const& path, std::string_view value)
{
  if (path.empty())
    return true;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << value << '\n';
  output.close();
  return static_cast<bool>(output);
}

}  // namespace

int main(int argc, char** argv)
{
  std::string const scenario = argc > 1 ? argv[1] : "normal";
  std::filesystem::path const marker = argc > 2 ? argv[2] : "";
  std::string initialize_request;

  if (scenario == "argv0")
    write_marker(marker, argv[0]);
  if (scenario == "live-destructor")
    write_marker(marker, std::to_string(static_cast<long long>(::getpid())));
  if (scenario == "startup-hang")
    loop_forever();
  if (!std::getline(std::cin, initialize_request))
    return 2;
  if (scenario == "initialize-delay")
  {
    if (!write_marker(marker, "initialize-observed"))
      return 8;
    // Deliberate startup work: this delay begins only after initialize is received.
    std::this_thread::sleep_for(1200ms);
  }

  if (scenario == "oversized-initialize")
  {
    std::cout << std::string(8192, 'x') << '\n' << std::flush;
    loop_forever();
  }
  if (scenario == "stderr-large")
  {
    std::cerr << std::string(8192, 'e') << std::flush;
  }
  if (scenario == "environment")
  {
    std::ofstream output(marker, std::ios::binary | std::ios::trunc);
    for (char** current = environ; current != nullptr && *current != nullptr; ++current)
      output << *current << '\n';
  }
  if (scenario == "oversized-buffered-initialize")
  {
    std::string records = "{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava.plugin.v1\",\"plugin_version\":\"0.1.0\",\"contributions\":{}}\n";
    records += std::string(300, 'x') + '\n';
    if (records.size() > static_cast<std::size_t>(PIPE_BUF))
      return 3;

    ssize_t written;
    do
      written = ::write(STDOUT_FILENO, records.data(), records.size());
    while (written < 0 && errno == EINTR);
    if (written != static_cast<ssize_t>(records.size()))
      return 3;

    std::string request;
    while (std::getline(std::cin, request))
    {
    }
    return 0;
  }

  if (scenario == "endpoint-eof" && !write_marker(marker, std::to_string(static_cast<long long>(::getpid()))))
    return 8;
  if (scenario == "shutdown-term-refusal" && std::signal(SIGTERM, SIG_IGN) == SIG_ERR)
    return 8;
  initialized();
  if (scenario == "endpoint-eof")
    return 7;
  if (scenario == "descendant")
  {
    int ready_pipe[2];
    if (::pipe2(ready_pipe, O_CLOEXEC) != 0)
      return 3;
    auto const child = ::fork();
    if (child < 0)
    {
      ::close(ready_pipe[0]);
      ::close(ready_pipe[1]);
      return 3;
    }
    if (child == 0)
    {
      ::close(ready_pipe[0]);
      if (std::signal(SIGTERM, SIG_IGN) == SIG_ERR)
        ::_exit(3);
      ssize_t written;
      do
        written = ::write(ready_pipe[1], "r", 1);
      while (written < 0 && errno == EINTR);
      ::close(ready_pipe[1]);
      if (written != 1)
        ::_exit(3);
      loop_forever();
    }
    ::close(ready_pipe[1]);
    pollfd ready{.fd = ready_pipe[0], .events = POLLIN, .revents = 0};
    auto const deadline = ava::tests::now_plus_seconds(5);
    int result;
    do
    {
      auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      result = remaining > 0 ? ::poll(&ready, 1, static_cast<int>(std::min<std::chrono::milliseconds::rep>(remaining, INT_MAX))) : 0;
    }
    while (result < 0 && errno == EINTR);
    char acknowledged = 0;
    ssize_t received = -1;
    if (result > 0 && (ready.revents & POLLIN) != 0)
    {
      do
        received = ::read(ready_pipe[0], &acknowledged, 1);
      while (received < 0 && errno == EINTR);
    }
    ::close(ready_pipe[0]);
    if (received != 1 || acknowledged != 'r' || !write_marker(marker, std::to_string(static_cast<long long>(child))))
    {
      ::kill(child, SIGKILL);
      while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR)
      {
      }
      return 3;
    }
    return 0;
  }
  if (scenario == "blocked-stdin-short-line-flood" || scenario == "queued-byte-flood")
  {
    if (std::cin.peek() == std::char_traits<char>::eof())
      return 4;
    std::string records;
    if (scenario == "blocked-stdin-short-line-flood")
    {
      for (int index = 0; index < 256; ++index)
        records += "{}\n";
    }
    else
    {
      for (int index = 0; index < 8; ++index)
        records += std::string(500, 'q') + '\n';
    }
    while (true)
      std::cout << records << std::flush;
  }

  std::string request;
  if (!std::getline(std::cin, request))
  {
    // The outbound-limit test closes stdin before requesting its stop reason.
    // Stay alive so natural EOF exit cannot win that settlement race.
    if (scenario == "request-marker")
      loop_forever();
    if (scenario == "shutdown-term-refusal" || scenario == "live-destructor")
    {
      if (scenario == "live-destructor")
        std::signal(SIGTERM, SIG_IGN);
      loop_forever();
    }
    return 0;
  }

  if (scenario == "event-hook-gated")
  {
    if (argc != 5)
      return 5;
    int const descriptor = ::open(argv[3], O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0)
      return 5;
    struct stat status{};
    if (::fstat(descriptor, &status) != 0 || !S_ISFIFO(status.st_mode))
    {
      ::close(descriptor);
      return 5;
    }
    if (!write_marker(marker, "event-observed"))
    {
      ::close(descriptor);
      return 7;
    }
    pollfd ready{.fd = descriptor, .events = POLLIN, .revents = 0};
    auto const deadline = ava::tests::now_plus_seconds(30);
    int result;
    do
    {
      auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      if (remaining <= 0)
      {
        ::close(descriptor);
        return 6;
      }
      result = ::poll(&ready, 1, static_cast<int>(std::min<std::chrono::milliseconds::rep>(remaining, INT_MAX)));
    }
    while (result < 0 && errno == EINTR);
    char release = 0;
    ssize_t received = -1;
    if (result > 0 && (ready.revents & POLLIN) != 0)
    {
      do
        received = ::read(descriptor, &release, 1);
      while (received < 0 && errno == EINTR);
    }
    ::close(descriptor);
    if (received != 1)
      return 6;
    if (!write_marker(argv[4], "side-effect"))
      return 7;
    std::cout << "{\"id\":\"ava_2\",\"type\":\"event.observed\",\"ok\":true}\n" << std::flush;
    while (std::getline(std::cin, request))
    {
    }
    return 0;
  }
  if (scenario == "request-marker")
    write_marker(marker, "request-observed");
  if (scenario == "request-hang" || scenario == "shutdown-term-refusal")
  {
    write_marker(marker, "request-observed");
    loop_forever();
  }
  if (scenario == "malformed")
  {
    std::cout << "not-json\n" << std::flush;
    loop_forever();
  }

  std::cout << "{\"id\":\"ava_tool_compat\",\"type\":\"tool.result\",\"ok\":true,\"content\":\"compatible\",\"metadata\":{}}\n" << std::flush;
  while (std::getline(std::cin, request))
  {
  }
  return 0;
}
