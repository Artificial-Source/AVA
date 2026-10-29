#include "sys.h"
#include "ava/app/connect_oauth_callback.h"
#include "ava/core/result.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ava::app {
namespace {

std::string errno_message()
{
  return std::strerror(errno);
}

ava::core::Error connect_error(ava::core::ErrorCategory category, std::string message)
{
  return ava::core::Error(category, std::move(message));
}

class ScopedSocket
{
 public:
  explicit ScopedSocket(int fd) : fd_(fd) { }
  ScopedSocket(ScopedSocket const&) = delete;
  ScopedSocket& operator=(ScopedSocket const&) = delete;
  ScopedSocket(ScopedSocket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) { }
  ScopedSocket& operator=(ScopedSocket&& other) noexcept
  {
    if (this != &other)
    {
      close_if_open();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  ~ScopedSocket() { close_if_open(); }

  [[nodiscard]] int get() const noexcept { return fd_; }

 private:
  void close_if_open() noexcept
  {
    if (fd_ >= 0)
      static_cast<void>(::close(fd_));
  }

  int fd_ = -1;
};

ava::core::VoidResult send_all_to_socket(int fd, std::string_view text)
{
  std::size_t offset = 0;
  while (offset < text.size())
  {
    auto const written = ::send(fd, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
    if (written < 0)
    {
      if (errno == EINTR)
        continue;
      auto error = connect_error(ava::core::ErrorCategory::Io, "failed to write OAuth callback response");
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    if (written == 0)
    {
      return std::unexpected(connect_error(ava::core::ErrorCategory::Io, "OAuth callback response write made no progress"));
    }
    offset += static_cast<std::size_t>(written);
  }
  return {};
}

std::string url_decode(std::string_view value)
{
  auto hex_value = [](char ch) -> std::optional<unsigned char> {
    if (ch >= '0' && ch <= '9')
      return static_cast<unsigned char>(ch - '0');
    if (ch >= 'A' && ch <= 'F')
      return static_cast<unsigned char>(ch - 'A' + 10);
    if (ch >= 'a' && ch <= 'f')
      return static_cast<unsigned char>(ch - 'a' + 10);
    return std::nullopt;
  };

  std::string output;
  output.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index)
  {
    if (value[index] == '+')
    {
      output.push_back(' ');
      continue;
    }
    if (value[index] == '%' && index + 2 < value.size())
    {
      auto const high = hex_value(value[index + 1]);
      auto const low = hex_value(value[index + 2]);
      if (high && low)
      {
        output.push_back(static_cast<char>((*high << 4U) | *low));
        index += 2;
        continue;
      }
    }
    output.push_back(value[index]);
  }
  return output;
}

std::map<std::string, std::string> parse_query(std::string_view query)
{
  std::map<std::string, std::string> fields;
  std::size_t start = 0;
  while (start <= query.size())
  {
    auto const end = query.find('&', start);
    auto const part = query.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    auto const split = part.find('=');
    if (split != std::string_view::npos)
    {
      fields[url_decode(part.substr(0, split))] = url_decode(part.substr(split + 1));
    }
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return fields;
}

struct OAuthCallback
{
  std::string code;
  std::string state;
  std::string error;
  std::string error_description;
};

ava::core::Result<OAuthCallback> parse_oauth_callback_request(std::string_view request)
{
  auto const request_line_end = request.find("\r\n");
  auto const request_line = request.substr(0, request_line_end);
  constexpr std::string_view prefix = "GET ";
  if (!request_line.starts_with(prefix))
  {
    return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "OAuth callback was not a GET"));
  }
  auto const target_end = request_line.find(' ', prefix.size());
  if (target_end == std::string_view::npos)
  {
    return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "OAuth callback request line was invalid"));
  }
  auto const target = request_line.substr(prefix.size(), target_end - prefix.size());
  auto const query_start = target.find('?');
  auto const path = target.substr(0, query_start);
  if (path != "/auth/callback")
  {
    auto error = connect_error(ava::core::ErrorCategory::InvalidArgument, "OAuth callback path was invalid");
    error.with_context("path", std::string(path));
    return std::unexpected(std::move(error));
  }
  if (query_start == std::string_view::npos)
  {
    return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "OAuth callback query missing"));
  }
  auto const fields = parse_query(target.substr(query_start + 1));
  OAuthCallback callback;
  if (auto const state = fields.find("state"); state != fields.end())
    callback.state = state->second;
  if (auto const error = fields.find("error"); error != fields.end())
    callback.error = error->second;
  if (auto const description = fields.find("error_description"); description != fields.end())
  {
    callback.error_description = description->second;
  }
  if (!callback.error.empty())
    return callback;

  auto const code = fields.find("code");
  if (code == fields.end() || code->second.empty() || callback.state.empty())
  {
    return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "OAuth callback was missing code or state"));
  }
  callback.code = code->second;
  return callback;
}

ava::core::Result<std::string> read_http_request_with_deadline(int fd, std::chrono::steady_clock::time_point deadline)
{
  std::string request;
  std::array<char, 4096> buffer{};
  while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384)
  {
    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0)
    {
      return std::unexpected(connect_error(ava::core::ErrorCategory::Io, "timed out reading OAuth callback"));
    }
    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    int ready = 0;
    do
    {
      ready = ::poll(&descriptor, 1, static_cast<int>(std::min<long long>(5000, remaining.count())));
    } while (ready < 0 && errno == EINTR);
    if (ready == 0)
    {
      return std::unexpected(connect_error(ava::core::ErrorCategory::Io, "timed out reading OAuth callback"));
    }
    if (ready < 0)
    {
      auto error = connect_error(ava::core::ErrorCategory::Io, "failed while reading OAuth callback");
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }

    auto const count = ::read(fd, buffer.data(), std::min(buffer.size(), std::size_t{16384} - request.size()));
    if (count < 0)
    {
      if (errno == EINTR)
        continue;
      auto error = connect_error(ava::core::ErrorCategory::Io, "failed to read OAuth callback");
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    if (count == 0)
      break;
    request.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (request.find("\r\n\r\n") == std::string::npos)
    return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "OAuth callback headers are incomplete or too large"));
  return request;
}

void send_callback_response(int fd, std::string_view body)
{
  std::string const response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) +
                               "\r\nConnection: close\r\n\r\n" + std::string(body);
  static_cast<void>(send_all_to_socket(fd, response));
}

}  // namespace

ava::core::Result<OAuthCallbackResult> wait_for_oauth_callback(std::string_view expected_state, std::function<bool()> const& cancel_requested)
{
  return detail::wait_for_oauth_callback(expected_state, cancel_requested, {});
}

namespace detail {
ava::core::Result<OAuthCallbackResult> wait_for_oauth_callback(std::string_view expected_state, std::function<bool()> const& cancel_requested,
                                                               OAuthCallbackOptions const& options)
{
  ScopedSocket const server(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
  if (server.get() < 0)
  {
    auto error = connect_error(ava::core::ErrorCategory::Io, "failed to create OAuth callback socket");
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  int reuse = 1;
  static_cast<void>(::setsockopt(server.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(options.port);
  if (::bind(server.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
  {
    auto error = connect_error(ava::core::ErrorCategory::Io, "failed to bind OAuth callback server");
    error.with_context("address", "127.0.0.1:" + std::to_string(options.port));
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  if (::listen(server.get(), 1) != 0)
  {
    auto error = connect_error(ava::core::ErrorCategory::Io, "failed to listen for OAuth callback");
    error.with_context("cause", errno_message());
    return std::unexpected(std::move(error));
  }
  if (options.listener_ready)
  {
    socklen_t length = sizeof(address);
    if (::getsockname(server.get(), reinterpret_cast<sockaddr*>(&address), &length) != 0)
    {
      auto error = connect_error(ava::core::ErrorCategory::Io, "failed to inspect OAuth callback server");
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }
    options.listener_ready(ntohs(address.sin_port));
  }

  auto const deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  while (std::chrono::steady_clock::now() < deadline)
  {
    if (options.deadline && std::chrono::steady_clock::now() >= *options.deadline)
      break;
    if (cancel_requested && cancel_requested())
    {
      return std::unexpected(connect_error(ava::core::ErrorCategory::InvalidArgument, "OpenAI OAuth login cancelled"));
    }
    auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    pollfd descriptor{.fd = server.get(), .events = POLLIN, .revents = 0};
    int ready = 0;
    do
    {
      ready = ::poll(&descriptor, 1, static_cast<int>(std::min<long long>(500, std::max<long long>(1, remaining.count()))));
    } while (ready < 0 && errno == EINTR);
    if (ready == 0)
      continue;
    if (ready < 0)
    {
      auto error = connect_error(ava::core::ErrorCategory::Io, "failed while waiting for OAuth callback");
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }

    ScopedSocket const client(::accept4(server.get(), nullptr, nullptr, SOCK_CLOEXEC));
    if (client.get() < 0)
    {
      auto error = connect_error(ava::core::ErrorCategory::Io, "failed to accept OAuth callback");
      error.with_context("cause", errno_message());
      return std::unexpected(std::move(error));
    }

    auto const client_deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(5));
    auto request = read_http_request_with_deadline(client.get(), options.deadline ? std::min(client_deadline, *options.deadline) : client_deadline);
    if (!request)
    {
      send_callback_response(client.get(), "AVA could not read this OAuth callback. Return to the terminal.\n");
      continue;
    }
    auto callback = parse_oauth_callback_request(*request);
    if (!callback)
    {
      send_callback_response(client.get(), "AVA ignored this OAuth callback. Return to the terminal.\n");
      continue;
    }
    if (callback->state != expected_state)
    {
      send_callback_response(client.get(), "AVA ignored an OAuth callback with the wrong state.\n");
      continue;
    }
    if (!callback->error.empty())
    {
      send_callback_response(client.get(), "AVA could not complete OAuth. Return to the terminal.\n");
      auto error = connect_error(ava::core::ErrorCategory::Provider, "OpenAI OAuth callback returned an error");
      error.with_context("error", callback->error);
      if (!callback->error_description.empty())
        error.with_context("description", callback->error_description);
      return std::unexpected(std::move(error));
    }
    send_callback_response(client.get(), "AVA connected. You can close this tab.\n");
    return OAuthCallbackResult{.code = std::move(callback->code)};
  }
  return std::unexpected(connect_error(ava::core::ErrorCategory::Io, "timed out waiting for OAuth callback"));
}
}  // namespace detail

}  // namespace ava::app
