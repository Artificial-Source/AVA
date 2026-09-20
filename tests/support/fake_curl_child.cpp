#include "sys.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
extern char** environ;
#endif

#if defined(_WIN32)
int main()
{
  return 77;
}
#else
namespace {

using namespace std::chrono_literals;
constexpr std::string_view kWriteOut = "\nAVA_HTTP_STATUS:%{http_code}";

bool write_all(int descriptor, std::string_view value)
{
  std::size_t offset = 0;
  while (offset < value.size())
  {
    auto const count = ::write(descriptor, value.data() + offset, value.size() - offset);
    if (count > 0)
    {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

bool valid_arguments(int argc, char** argv, bool& streaming)
{
  streaming = argc == 7;
  if (argc != 6 && argc != 7)
    return false;
  if (std::string_view(argv[0]) != "curl" || std::string_view(argv[1]) != "-q" || std::string_view(argv[2]) != "--config" || std::string_view(argv[3]) != "-" ||
      std::string_view(argv[4]) != "--write-out" || std::string_view(argv[5]) != kWriteOut)
  {
    return false;
  }
  return argc == 6 || std::string_view(argv[6]) == "--no-buffer";
}

std::string quoted_value(std::string_view config, std::string_view prefix)
{
  auto position = config.find(prefix);
  if (position == std::string_view::npos)
    return {};
  position += prefix.size();
  auto const end = config.find('"', position);
  if (end == std::string_view::npos)
    return {};
  return std::string(config.substr(position, end - position));
}

std::string read_file(std::string const& path)
{
  if (path.empty())
    return {};
  int const descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (descriptor < 0)
    return {};
  std::string content;
  std::array<char, 4096> buffer{};
  while (true)
  {
    auto const count = ::read(descriptor, buffer.data(), buffer.size());
    if (count > 0)
    {
      content.append(buffer.data(), static_cast<std::size_t>(count));
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    break;
  }
  static_cast<void>(::close(descriptor));
  return content;
}

std::string environment_dump()
{
  std::vector<std::string> entries;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
    entries.emplace_back(*entry);
  std::ranges::sort(entries);
  std::string result;
  for (auto const& entry : entries)
    result += entry + "\n";
  return result;
}

void write_response_head(int descriptor, int code, std::string_view reason = "Test", std::string_view headers = {})
{
  static_cast<void>(write_all(descriptor, "HTTP/1.1 " + std::to_string(code) + " " + std::string(reason) + "\r\n"));
  static_cast<void>(write_all(descriptor, headers));
  static_cast<void>(write_all(descriptor, "\r\n"));
}

void write_status(int code)
{
  static_cast<void>(write_all(STDOUT_FILENO, "\nAVA_HTTP_STATUS:" + std::to_string(code)));
}

[[noreturn]] void idle_forever()
{
  while (true)
    ::pause();
}

}  // namespace

int main(int argc, char** argv)
{
  ::alarm(20);
  bool streaming = false;
  if (!valid_arguments(argc, argv, streaming))
  {
    static_cast<void>(write_all(STDERR_FILENO, "invalid fixed curl argv"));
    return 64;
  }

  std::string config;
  std::array<char, 512> buffer{};
  while (config.size() < 2U * 1024U * 1024U)
  {
    auto const count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
    if (count > 0)
    {
      config.append(buffer.data(), static_cast<std::size_t>(count));
      if (config.find("/config-failure") != std::string::npos)
      {
        static_cast<void>(::close(STDIN_FILENO));
        std::this_thread::sleep_for(2s);
        return 0;
      }
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    if (count == 0)
      break;
    return 65;
  }

  auto const url = quoted_value(config, "url = \"");
  auto const body_path = quoted_value(config, "data-binary = \"@");
  auto const body = read_file(body_path);
  bool const dumps_streaming_headers = config.find("dump-header = \"/dev/stderr\"\n") != std::string::npos;
  if (streaming &&
      (!dumps_streaming_headers || config.find("include\n") != std::string::npos || config.find("suppress-connect-headers\n") == std::string::npos))
  {
    static_cast<void>(write_all(STDERR_FILENO, "invalid streaming curl config"));
    return 66;
  }

  if (url.find("/environment") != std::string::npos)
  {
    static_cast<void>(write_all(STDOUT_FILENO, environment_dump()));
    write_status(200);
    return 0;
  }
  if (url.find("/separate") != std::string::npos)
  {
    static_cast<void>(write_all(STDERR_FILENO, "FAKE_CURL_STDERR_CANARY"));
    static_cast<void>(write_all(STDOUT_FILENO, "fake stdout body"));
    write_status(201);
    return 0;
  }
  if (url.find("/buffered-hup") != std::string::npos)
  {
    std::string output(32U * 1024U, 'h');
    output += "\nAVA_HTTP_STATUS:200";
    static_cast<void>(write_all(STDOUT_FILENO, output));
    return 0;
  }
  if (url.find("/retry-429") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 429, "Too Many Requests", "Retry-After: 0\r\n");
    static_cast<void>(write_all(STDOUT_FILENO, "rate limited response"));
    write_status(429);
    return 0;
  }
  if (url.find("/retry-quota") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 429, "Too Many Requests");
    static_cast<void>(write_all(STDOUT_FILENO, "insufficient_quota: billing hard limit"));
    write_status(429);
    return 0;
  }
  if (url.find("/retry-auth") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 401, "Unauthorized");
    static_cast<void>(write_all(STDOUT_FILENO, "authentication failed"));
    write_status(401);
    return 0;
  }
  if (url.find("/retry-503") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 503, "Unavailable");
    static_cast<void>(write_all(STDOUT_FILENO, "transient response"));
    write_status(503);
    return 0;
  }
  if (url.find("/retry-success") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 200, "OK");
    static_cast<void>(write_all(STDOUT_FILENO, "accepted response"));
    write_status(200);
    return 0;
  }
  if (url.find("/stream-output-limit") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 200, "OK");
    std::string output(32U * 1024U, 'x');
    while (write_all(STDOUT_FILENO, output))
    {
    }
    return 0;
  }
  if (url.find("/stream-error-cancel") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 503, "Unavailable");
    std::string output(4096, 'e');
    while (write_all(STDOUT_FILENO, output))
      std::this_thread::sleep_for(5ms);
    return 0;
  }
  if (url.find("/stream-gate-") != std::string::npos)
  {
    auto const token = url.substr(url.find("/stream-gate-") + std::string_view("/stream-gate-").size());
    auto const gate = "/tmp/ava-curl-stream-gate-" + token;
    write_response_head(STDERR_FILENO, 200, "OK");
    static_cast<void>(write_all(STDOUT_FILENO, "incremental accepted body before gate|"));
    for (int index = 0; index < 400 && ::access(gate.c_str(), F_OK) != 0; ++index)
      std::this_thread::sleep_for(5ms);
    if (::access(gate.c_str(), F_OK) != 0)
      return 69;
    static_cast<void>(write_all(STDOUT_FILENO, "after gate"));
    write_status(200);
    return 0;
  }
  if (url.find("/stream-http-trailers") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 200, "OK", "Trailer: X-Trace, data\r\n");
    static_cast<void>(write_all(STDOUT_FILENO, "expected body"));
    static_cast<void>(write_all(STDERR_FILENO, "X-Trace: trailer metadata\r\ndata: malicious trailer event\r\n"));
    write_status(200);
    return 0;
  }
  if (url.find("/stream-head-chain") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 100, "Continue");
    write_response_head(STDERR_FILENO, 103, "Early Hints", "Link: </hint>\r\n");
    write_response_head(STDERR_FILENO, 399, "Redirect", "Location: https://curl.test/final\r\n");
    write_response_head(STDERR_FILENO, 200, "OK", "X-Final: yes\r\n");
    static_cast<void>(write_all(STDOUT_FILENO, "chain body"));
    write_status(200);
    return 0;
  }
  if (url.find("/stream-upgrade") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 101, "Switching Protocols", "Upgrade: fake\r\n");
    static_cast<void>(write_all(STDOUT_FILENO, "upgrade body HTTP/1.1 200 Fake\r\n\r\nnot a second head"));
    write_status(101);
    return 0;
  }
  if (url.find("/stream-final-redirect") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 399, "Final Redirect");
    static_cast<void>(write_all(STDOUT_FILENO, "body HTTP/1.1 200 Fake\r\nX-Fake: yes\r\n\r\nstill redirect"));
    write_status(399);
    return 0;
  }
  if (url.find("/stream-disabled-redirect") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 399, "Disabled Redirect", "Location: https://curl.test/not-followed\r\n");
    static_cast<void>(write_all(STDOUT_FILENO, "disabled body HTTP/1.1 200 Fake\r\n\r\nnot a head"));
    write_status(399);
    return 0;
  }
  if (url.find("/stream-missing-trailer") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 200, "OK");
    static_cast<void>(write_all(STDOUT_FILENO, "body without trailer"));
    return 0;
  }
  if (url.find("/stream-malformed-trailer") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 200, "OK");
    static_cast<void>(write_all(STDOUT_FILENO, "body\nAVA_HTTP_STATUS:20x"));
    return 0;
  }
  if (url.find("/stream-mismatched-trailer") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 200, "OK");
    static_cast<void>(write_all(STDOUT_FILENO, "body"));
    write_status(201);
    return 0;
  }
  if (url.find("/stream-stderr-eof-first") != std::string::npos)
  {
    write_response_head(STDERR_FILENO, 200, "OK", "X-Order: headers-first\r\n");
    static_cast<void>(::close(STDERR_FILENO));
    static_cast<void>(write_all(STDOUT_FILENO, "headers-first body"));
    write_status(200);
    return 0;
  }
  if (url.find("/stream-stdout-eof-first") != std::string::npos)
  {
    static_cast<void>(write_all(STDOUT_FILENO, "body-pipe-first body"));
    write_status(200);
    static_cast<void>(::close(STDOUT_FILENO));
    std::this_thread::sleep_for(10ms);
    write_response_head(STDERR_FILENO, 200, "OK", "X-Order: body-pipe-first\r\n");
    return 0;
  }
  if (url.find("/stream-missing-metadata") != std::string::npos)
  {
    static_cast<void>(write_all(STDERR_FILENO, "curl: metadata unavailable\n"));
    static_cast<void>(write_all(STDOUT_FILENO, "must remain withheld"));
    write_status(200);
    return 0;
  }
  if (url.find("/stream-prefix-diagnostic-failure") != std::string::npos)
  {
    static_cast<void>(write_all(STDERR_FILENO, "curl: connection failed before response metadata\nHTTP/1.1 200 Fake\r\n\r\n"));
    static_cast<void>(write_all(STDOUT_FILENO, "\nAVA_HTTP_STATUS:000"));
    return 7;
  }
  if (url.find("/stream-header-limit") != std::string::npos)
  {
    static_cast<void>(write_all(STDERR_FILENO, "HTTP/1.1 200 OK\r\nX-Large: "));
    static_cast<void>(write_all(STDERR_FILENO, std::string(65U * 1024U, 'h')));
    static_cast<void>(write_all(STDERR_FILENO, "\r\n\r\n"));
    // Keep the child alive until the transport rejects the oversized header;
    // a natural exit can otherwise win settlement when the pipe holds it all.
    idle_forever();
  }
  if (url.find("/stream") != std::string::npos)
  {
    if (!streaming)
      return 66;
    static_cast<void>(write_all(STDERR_FILENO, "HTTP/1.1 206 Partial"));
    std::this_thread::sleep_for(5ms);
    static_cast<void>(write_all(STDERR_FILENO, " Content\r\nX-Split: yes\r\n"));
    std::this_thread::sleep_for(5ms);
    static_cast<void>(write_all(STDERR_FILENO, "\r\n"));
    static_cast<void>(write_all(STDOUT_FILENO, "chunk-one|"));
    std::this_thread::sleep_for(15ms);
    static_cast<void>(write_all(STDERR_FILENO, "stream-stderr"));
    static_cast<void>(write_all(STDOUT_FILENO, "chunk-two"));
    std::this_thread::sleep_for(15ms);
    static_cast<void>(write_all(STDOUT_FILENO, "\nAVA_HTTP_"));
    std::this_thread::sleep_for(15ms);
    static_cast<void>(write_all(STDOUT_FILENO, "STATUS:206"));
    return 0;
  }
  if (url.find("/output-limit") != std::string::npos)
  {
    std::string output(32U * 1024U, 'x');
    while (write_all(STDOUT_FILENO, output))
    {
    }
    return 0;
  }
  if (url.find("/stderr-limit") != std::string::npos)
  {
    std::string chunk(4096, 'e');
    for (std::size_t bytes = 0; bytes < 72U * 1024U; bytes += chunk.size())
      static_cast<void>(write_all(STDERR_FILENO, chunk));
    static_cast<void>(write_all(STDOUT_FILENO, "stderr bounded"));
    write_status(200);
    return 0;
  }
  if (url.find("/progress-timeout") != std::string::npos)
  {
    for (int index = 0; index < 500; ++index)
    {
      if (!write_all(STDOUT_FILENO, "p"))
        break;
      std::this_thread::sleep_for(10ms);
    }
    write_status(200);
    return 0;
  }
  if (url.find("/cancel") != std::string::npos)
    idle_forever();
  if (url.find("/term-refusal") != std::string::npos)
  {
    static_cast<void>(::signal(SIGTERM, SIG_IGN));
    idle_forever();
  }
  if (url.find("/descendant") != std::string::npos)
  {
    pid_t const child = ::fork();
    if (child == 0)
    {
      static_cast<void>(::signal(SIGTERM, SIG_IGN));
      idle_forever();
    }
    if (child < 0)
      return 67;
    static_cast<void>(write_all(STDOUT_FILENO, "descendant body"));
    write_status(200);
    return 0;
  }
  if (url.find("/nonzero") != std::string::npos)
  {
    static_cast<void>(write_all(STDOUT_FILENO, "nonzero body"));
    write_status(500);
    static_cast<void>(write_all(STDERR_FILENO, "NONZERO_SECRET_CANARY"));
    return 7;
  }
  if (url.find("/signal") != std::string::npos)
  {
    static_cast<void>(write_all(STDOUT_FILENO, "signal body"));
    write_status(500);
    static_cast<void>(::raise(SIGUSR1));
    return 68;
  }
  if (url.find("/protocol") != std::string::npos)
  {
    static_cast<void>(write_all(STDOUT_FILENO, "missing status marker"));
    static_cast<void>(::close(STDOUT_FILENO));
    std::this_thread::sleep_for(2s);
    return 0;
  }

  if (config.find("include\n") != std::string::npos)
    static_cast<void>(write_all(STDOUT_FILENO, "HTTP/1.1 200 OK\r\nX-Fake: yes\r\n\r\n"));
  std::string response = "argv=fixed\nstreaming=" + std::string(streaming ? "true" : "false") + "\nconfig-begin\n" + config + "config-end\nbody=" + body;
  static_cast<void>(write_all(STDOUT_FILENO, response));
  write_status(200);
  return 0;
}
#endif
