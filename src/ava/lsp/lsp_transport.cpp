#include "sys.h"
#include "ava/lsp/lsp_client.h"
#include "ava/lsp/lsp_client_internal.h"
#include "ava/core/json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <poll.h>
#include <signal.h>

namespace ava::lsp {

using namespace lsp_client_internal;

namespace {

constexpr std::size_t kMaxLspHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxLspMessageBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaxConfigurationItems = 64;

std::size_t remaining_ms(std::chrono::steady_clock::time_point deadline)
{
  auto const now = std::chrono::steady_clock::now();
  if (now >= deadline)
    return 0;
  return static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

ava::core::Result<std::size_t> parse_content_length(std::string_view header, ServerConfig const& config)
{
  constexpr std::string_view key = "Content-Length:";
  auto const position = header.find(key);
  if (position == std::string_view::npos)
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP response is missing Content-Length", config));
  }
  std::size_t index = position + key.size();
  while (index < header.size() && (header[index] == ' ' || header[index] == '\t')) ++index;
  std::size_t end = index;
  while (end < header.size() && std::isdigit(static_cast<unsigned char>(header[end])) != 0) ++end;
  if (end == index)
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP response has invalid Content-Length", config));
  }
  try
  {
    auto const parsed = static_cast<std::size_t>(std::stoull(std::string(header.substr(index, end - index))));
    if (parsed > kMaxLspMessageBytes)
    {
      auto error = lsp_error(ava::core::ErrorCategory::Tool, "LSP response Content-Length exceeds message cap", config);
      error.with_context("max_bytes", std::to_string(kMaxLspMessageBytes));
      return std::unexpected(std::move(error));
    }
    return parsed;
  }
  catch (...)
  {
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP response Content-Length is too large", config));
  }
}

}  // namespace

ava::core::VoidResult SubprocessLspClient::send_notification(std::string_view method, std::string_view params_json,
                                                             std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                             std::string_view phase, CancelCallback cancel_requested)
{
  std::string const body = "{\"jsonrpc\":\"2.0\",\"method\":" + json_string(method) + ",\"params\":" + std::string(params_json) + "}";
  return write_message(body, deadline, timeout, phase, method, cancel_requested);
}

ava::core::VoidResult SubprocessLspClient::respond_to_server_request(std::string_view message, std::int64_t id, std::chrono::steady_clock::time_point deadline,
                                                                     std::chrono::milliseconds timeout, std::string_view phase, CancelCallback cancel_requested)
{
  auto const method = ava::core::json::string_field(message, "method");
  if (!method)
    return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP server request is malformed", config_));

  std::string body;
  if (*method == "workspace/configuration")
  {
    auto const params = ava::core::json::object_field(message, "params");
    auto const items = params ? ava::core::json::strict_objects_in_array_field(*params, "items", kMaxConfigurationItems) : std::nullopt;
    if (!items)
      return std::unexpected(lsp_error(ava::core::ErrorCategory::Tool, "LSP workspace configuration request is malformed", config_));
    std::string result = "[";
    for (std::size_t index = 0; index < items->size(); ++index)
    {
      if (index != 0)
        result += ',';
      result += "null";
    }
    result += ']';
    body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"result\":" + result + "}";
  }
  else
  {
    body = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"error\":{\"code\":-32601,\"message\":\"unsupported LSP server request\"}}";
  }
  return write_message(body, deadline, timeout, phase, "server/request", cancel_requested);
}

ava::core::Result<std::string> SubprocessLspClient::request_response(std::string_view method, std::string_view params_json,
                                                                     std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                                     std::string_view phase, CancelCallback cancel_requested)
{
  int const id = next_id_++;
  std::string const body =
      "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"method\":" + json_string(method) + ",\"params\":" + std::string(params_json) + "}";
  if (auto written = write_message(body, deadline, timeout, phase, method, cancel_requested); !written)
  {
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("LSP request canceled", config_));
    }
    return std::unexpected(std::move(written.error()));
  }

  while (true)
  {
    auto message = read_message(deadline, timeout, phase, method, cancel_requested);
    if (!message)
    {
      if (is_canceled(cancel_requested))
      {
        terminate_child();
        return std::unexpected(canceled_error("LSP request canceled", config_));
      }
      return std::unexpected(std::move(message.error()));
    }
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("LSP request canceled", config_));
    }
    auto const response_id = ava::core::json::integer_field(*message, "id");
    if (auto const incoming_method = ava::core::json::string_field(*message, "method"))
    {
      static_cast<void>(incoming_method);
      if (response_id)
      {
        if (auto response = respond_to_server_request(*message, *response_id, deadline, timeout, phase, cancel_requested); !response)
          return std::unexpected(std::move(response.error()));
      }
      else if (auto dispatched = dispatch_notification(*message); !dispatched)
      {
        return std::unexpected(std::move(dispatched.error()));
      }
      continue;
    }
    if (!response_id)
    {
      auto error = lsp_error(ava::core::ErrorCategory::Tool, "LSP response is malformed", config_);
      error.with_context("method", std::string(method));
      return std::unexpected(std::move(error));
    }
    if (*response_id != id)
      continue;
    if (ava::core::json::object_field(*message, "error"))
    {
      auto error = lsp_error(ava::core::ErrorCategory::Tool, "LSP request returned an error", config_);
      error.with_context("method", std::string(method));
      return std::unexpected(std::move(error));
    }
    return *message;
  }
}

ava::core::VoidResult SubprocessLspClient::write_message(std::string_view body, std::chrono::steady_clock::time_point deadline,
                                                         std::chrono::milliseconds timeout, std::string_view phase, std::string_view method,
                                                         CancelCallback cancel_requested)
{
  if (is_canceled(cancel_requested))
  {
    terminate_child();
    return std::unexpected(canceled_error("LSP request canceled", config_));
  }
  if (auto running = check_child_running(); !running)
  {
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("LSP request canceled", config_));
    }
    return std::unexpected(std::move(running.error()));
  }
  if (is_canceled(cancel_requested))
  {
    terminate_child();
    return std::unexpected(canceled_error("LSP request canceled", config_));
  }
  if (std::chrono::steady_clock::now() >= deadline)
  {
    auto error = lsp_error(ava::core::ErrorCategory::Tool, "timed out writing LSP request", config_);
    error.with_context("timeout_ms", std::to_string(timeout.count()));
    error.with_context("phase", std::string(phase));
    error.with_context("method", std::string(method));
    terminate_child();
    return std::unexpected(std::move(error));
  }
  std::string const frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
  std::size_t offset = 0;
  while (offset < frame.size())
  {
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("LSP request canceled", config_));
    }
    auto const bytes = write_retry(stdin_fd_, frame.data() + offset, frame.size() - offset);
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("LSP request canceled", config_));
    }
    if (bytes < 0)
    {
      if (is_canceled(cancel_requested))
      {
        terminate_child();
        return std::unexpected(canceled_error("LSP request canceled", config_));
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        if (auto writable = wait_for_writable(deadline, timeout, phase, method, cancel_requested); !writable)
        {
          if (is_canceled(cancel_requested))
          {
            terminate_child();
            return std::unexpected(canceled_error("LSP request canceled", config_));
          }
          return std::unexpected(std::move(writable.error()));
        }
        continue;
      }
      if (errno == EPIPE)
      {
        if (auto running = check_child_running(); !running)
          return std::unexpected(std::move(running.error()));
      }
      terminate_child();
      return std::unexpected(errno_error("failed to write LSP request", config_));
    }
    if (bytes == 0)
    {
      if (is_canceled(cancel_requested))
      {
        terminate_child();
        return std::unexpected(canceled_error("LSP request canceled", config_));
      }
      if (auto running = check_child_running(); !running)
        return std::unexpected(std::move(running.error()));
      terminate_child();
      return std::unexpected(lsp_error(ava::core::ErrorCategory::Io, "failed to write LSP request", config_));
    }
    offset += static_cast<std::size_t>(bytes);
  }
  return {};
}

ava::core::Result<std::string> SubprocessLspClient::read_message(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                                 std::string_view phase, std::string_view method, CancelCallback cancel_requested)
{
  while (true)
  {
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("LSP request canceled", config_));
    }
    auto const header_end = read_buffer_.find("\r\n\r\n");
    if (header_end != std::string::npos)
    {
      auto length = parse_content_length(std::string_view(read_buffer_).substr(0, header_end), config_);
      if (!length)
      {
        if (is_canceled(cancel_requested))
        {
          terminate_child();
          return std::unexpected(canceled_error("LSP request canceled", config_));
        }
        return std::unexpected(std::move(length.error()));
      }
      auto const body_start = header_end + 4;
      if (read_buffer_.size() >= body_start + *length)
      {
        auto body = read_buffer_.substr(body_start, *length);
        read_buffer_.erase(0, body_start + *length);
        return body;
      }
    }
    else if (read_buffer_.size() > kMaxLspHeaderBytes)
    {
      if (is_canceled(cancel_requested))
      {
        terminate_child();
        return std::unexpected(canceled_error("LSP request canceled", config_));
      }
      auto error = lsp_error(ava::core::ErrorCategory::Tool, "LSP response header exceeds size cap", config_);
      error.with_context("max_bytes", std::to_string(kMaxLspHeaderBytes));
      return std::unexpected(std::move(error));
    }
    if (read_buffer_.size() > kMaxLspMessageBytes + kMaxLspHeaderBytes)
    {
      if (is_canceled(cancel_requested))
      {
        terminate_child();
        return std::unexpected(canceled_error("LSP request canceled", config_));
      }
      auto error = lsp_error(ava::core::ErrorCategory::Tool, "LSP response exceeds message cap", config_);
      error.with_context("max_bytes", std::to_string(kMaxLspMessageBytes));
      return std::unexpected(std::move(error));
    }

    if (auto readable = wait_for_readable(deadline, timeout, phase, method, cancel_requested); !readable)
    {
      if (is_canceled(cancel_requested))
      {
        terminate_child();
        return std::unexpected(canceled_error("LSP request canceled", config_));
      }
      return std::unexpected(std::move(readable.error()));
    }
    std::array<char, 4096> buffer{};
    auto const bytes = read_retry(stdout_fd_, buffer.data(), buffer.size());
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("LSP request canceled", config_));
    }
    if (bytes > 0)
    {
      read_buffer_.append(buffer.data(), static_cast<std::size_t>(bytes));
      continue;
    }
    if (bytes == 0)
    {
      if (is_canceled(cancel_requested))
      {
        terminate_child();
        return std::unexpected(canceled_error("LSP request canceled", config_));
      }
      if (auto running = check_child_running(); !running)
        return std::unexpected(std::move(running.error()));
      auto error = lsp_error(ava::core::ErrorCategory::Io, "LSP server closed stdout", config_);
      terminate_child();
      return std::unexpected(std::move(error));
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      continue;
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("LSP request canceled", config_));
    }
    terminate_child();
    return std::unexpected(errno_error("failed to read LSP response", config_));
  }
}

ava::core::VoidResult SubprocessLspClient::wait_for_readable(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                             std::string_view phase, std::string_view method, CancelCallback cancel_requested)
{
  if (is_canceled(cancel_requested))
  {
    terminate_child();
    return std::unexpected(canceled_error("LSP request canceled", config_));
  }
  if (auto running = check_child_running(); !running)
  {
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("LSP request canceled", config_));
    }
    return std::unexpected(std::move(running.error()));
  }
  if (is_canceled(cancel_requested))
  {
    terminate_child();
    return std::unexpected(canceled_error("LSP request canceled", config_));
  }
  auto const timeout_ms = remaining_ms(deadline);
  if (timeout_ms == 0)
  {
    auto error = lsp_error(ava::core::ErrorCategory::Tool, "timed out waiting for LSP response", config_);
    error.with_context("timeout_ms", std::to_string(timeout.count()));
    error.with_context("phase", std::string(phase));
    error.with_context("method", std::string(method));
    terminate_child();
    return std::unexpected(std::move(error));
  }

  pollfd fd{.fd = stdout_fd_, .events = POLLIN, .revents = 0};
  int const poll_timeout = static_cast<int>(std::min<std::size_t>(timeout_ms, 100));
  int const polled = poll(&fd, 1, poll_timeout);
  int const poll_errno = errno;
  if (is_canceled(cancel_requested))
  {
    terminate_child();
    return std::unexpected(canceled_error("LSP request canceled", config_));
  }
  if (polled < 0)
  {
    if (poll_errno == EINTR)
      return {};
    errno = poll_errno;
    return std::unexpected(errno_error("failed to poll LSP response", config_));
  }
  if (polled == 0)
    return check_child_running();
  if ((fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (fd.revents & POLLIN) == 0)
  {
    if (auto running = check_child_running(); !running)
      return std::unexpected(std::move(running.error()));
    auto error = lsp_error(ava::core::ErrorCategory::Io, "LSP server pipe closed", config_);
    terminate_child();
    return std::unexpected(std::move(error));
  }
  return {};
}

ava::core::VoidResult SubprocessLspClient::wait_for_writable(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                             std::string_view phase, std::string_view method, CancelCallback cancel_requested)
{
  if (is_canceled(cancel_requested))
  {
    terminate_child();
    return std::unexpected(canceled_error("LSP request canceled", config_));
  }
  if (auto running = check_child_running(); !running)
  {
    if (is_canceled(cancel_requested))
    {
      terminate_child();
      return std::unexpected(canceled_error("LSP request canceled", config_));
    }
    return std::unexpected(std::move(running.error()));
  }
  if (is_canceled(cancel_requested))
  {
    terminate_child();
    return std::unexpected(canceled_error("LSP request canceled", config_));
  }
  auto const timeout_ms = remaining_ms(deadline);
  if (timeout_ms == 0)
  {
    auto error = lsp_error(ava::core::ErrorCategory::Tool, "timed out writing LSP request", config_);
    error.with_context("timeout_ms", std::to_string(timeout.count()));
    error.with_context("phase", std::string(phase));
    error.with_context("method", std::string(method));
    terminate_child();
    return std::unexpected(std::move(error));
  }

  pollfd fd{.fd = stdin_fd_, .events = POLLOUT, .revents = 0};
  int const poll_timeout = static_cast<int>(std::min<std::size_t>(timeout_ms, 100));
  int const polled = poll(&fd, 1, poll_timeout);
  int const poll_errno = errno;
  if (is_canceled(cancel_requested))
  {
    terminate_child();
    return std::unexpected(canceled_error("LSP request canceled", config_));
  }
  if (polled < 0)
  {
    if (poll_errno == EINTR)
      return {};
    errno = poll_errno;
    return std::unexpected(errno_error("failed to poll LSP request pipe", config_));
  }
  if (polled == 0)
    return check_child_running();
  if ((fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (fd.revents & POLLOUT) == 0)
  {
    if (auto running = check_child_running(); !running)
      return std::unexpected(std::move(running.error()));
    auto error = lsp_error(ava::core::ErrorCategory::Io, "LSP server request pipe closed", config_);
    terminate_child();
    return std::unexpected(std::move(error));
  }
  return {};
}

}  // namespace ava::lsp
