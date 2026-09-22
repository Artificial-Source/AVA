#include "sys.h"

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

void write_marker(std::filesystem::path const& path, std::string_view value)
{
  if (path.empty())
    return;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << value << '\n';
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
    write_marker(marker, "initialize-observed");
    std::this_thread::sleep_for(120ms);
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

  initialized();
  if (scenario == "endpoint-eof")
    return 7;
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
      std::signal(SIGTERM, SIG_IGN);
      loop_forever();
    }
    return 0;
  }

  if (scenario == "request-marker")
    write_marker(marker, "request-observed");
  if (scenario == "request-hang" || scenario == "shutdown-term-refusal")
  {
    write_marker(marker, "request-observed");
    if (scenario == "shutdown-term-refusal")
      std::signal(SIGTERM, SIG_IGN);
    loop_forever();
  }
  if (scenario == "malformed")
  {
    std::cout << "not-json\n" << std::flush;
    loop_forever();
  }
  if (scenario == "descendant")
  {
    auto const child = ::fork();
    if (child < 0)
      return 3;
    if (child == 0)
    {
      std::signal(SIGTERM, SIG_IGN);
      loop_forever();
    }
    write_marker(marker, std::to_string(static_cast<long long>(child)));
    return 0;
  }

  std::cout << "{\"id\":\"ava_tool_compat\",\"type\":\"tool.result\",\"ok\":true,\"content\":\"compatible\",\"metadata\":{}}\n" << std::flush;
  while (std::getline(std::cin, request))
  {
  }
  return 0;
}
