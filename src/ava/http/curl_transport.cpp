#include "sys.h"
#include "ava/http/curl_transport.h"
#include "ava/http/curl_transport_test_support.h"
#include "ava/process/environment.h"
#include "ava/process/supervisor.h"
#include "ava/core/error.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::http {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::size_t kMaxCurlResponseBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxCurlStreamingHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxCurlStderrBytes = 64 * 1024;
constexpr std::string_view kStatusMarker = "\nAVA_HTTP_STATUS:";
constexpr std::string_view kWriteOut = "\nAVA_HTTP_STATUS:%{http_code}";
constexpr std::size_t kStatusTailReserve = kStatusMarker.size() + 3;
constexpr auto kCleanupBudget = 2s;
constexpr auto kSettlementObservationBudget = 250ms;
constexpr std::uint32_t kInputWatch = 1;
constexpr std::uint32_t kStdoutWatch = 2;
constexpr std::uint32_t kStderrWatch = 3;

class UniqueFd
{
 public:
  explicit UniqueFd(int descriptor = -1) noexcept : descriptor_(descriptor) { }
  UniqueFd(UniqueFd const&) = delete;
  UniqueFd& operator=(UniqueFd const&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : descriptor_(other.release()) { }
  UniqueFd& operator=(UniqueFd&& other) noexcept
  {
    if (this != &other)
      reset(other.release());
    return *this;
  }
  ~UniqueFd() { reset(); }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] int release() noexcept { return std::exchange(descriptor_, -1); }
  void reset(int descriptor = -1) noexcept
  {
    if (descriptor_ >= 0)
      static_cast<void>(::close(descriptor_));
    descriptor_ = descriptor;
  }

 private:
  int descriptor_ = -1;
};

class TempBodyFile
{
 public:
  TempBodyFile() = default;
  TempBodyFile(TempBodyFile const&) = delete;
  TempBodyFile& operator=(TempBodyFile const&) = delete;
  TempBodyFile(TempBodyFile&& other) noexcept : path_(std::move(other.path_)) { other.path_.clear(); }
  TempBodyFile& operator=(TempBodyFile&& other) noexcept
  {
    if (this != &other)
    {
      cleanup();
      path_ = std::move(other.path_);
      other.path_.clear();
    }
    return *this;
  }
  ~TempBodyFile() { cleanup(); }

  [[nodiscard]] std::string const& path() const noexcept { return path_; }

  [[nodiscard]] static ava::core::Result<TempBodyFile> create(std::string_view body)
  {
    std::error_code temp_error;
    auto const temp_dir = std::filesystem::temp_directory_path(temp_error);
    if (temp_error)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to resolve temporary directory"));

    std::string path_template = (temp_dir / "ava-request-body-XXXXXX").string();
    int const descriptor = ::mkstemp(path_template.data());
    if (descriptor < 0)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to create temporary request body file"));
    UniqueFd file(descriptor);
    if (::fchmod(file.get(), S_IRUSR | S_IWUSR) != 0)
    {
      static_cast<void>(::unlink(path_template.c_str()));
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to secure temporary request body file"));
    }

    std::size_t written = 0;
    while (written < body.size())
    {
      auto const count = ::write(file.get(), body.data() + written, body.size() - written);
      if (count < 0 && errno == EINTR)
        continue;
      if (count <= 0)
      {
        static_cast<void>(::unlink(path_template.c_str()));
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to write temporary request body file"));
      }
      written += static_cast<std::size_t>(count);
    }

    TempBodyFile result;
    result.path_ = std::move(path_template);
    return result;
  }

 private:
  void cleanup() noexcept
  {
    if (!path_.empty())
    {
      static_cast<void>(::unlink(path_.c_str()));
      path_.clear();
    }
  }

  std::string path_;
};

[[nodiscard]] Clock::time_point saturating_add(Clock::time_point value, Clock::duration duration) noexcept
{
  if (duration <= Clock::duration::zero())
    return value;
  if (value.time_since_epoch() > Clock::time_point::max().time_since_epoch() - duration)
    return Clock::time_point::max();
  return value + duration;
}

[[nodiscard]] std::chrono::milliseconds bounded_timeout(int timeout_ms) noexcept
{
  return std::chrono::milliseconds(std::max(1, timeout_ms));
}

std::string curl_config_escape(std::string_view value)
{
  std::string escaped;
  escaped.reserve(value.size());
  for (char const ch : value)
  {
    switch (ch)
    {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
      case '\r':
        escaped += ' ';
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

std::string build_curl_config(HttpRequest const& request, std::string const& body_path, bool streaming)
{
  std::string config;
  config += "url = \"" + curl_config_escape(request.url) + "\"\n";
  config += "request = \"" + curl_config_escape(request.method.empty() ? "POST" : request.method) + "\"\n";
  if (request.follow_redirects)
    config += "location\n";
  config += "max-redirs = \"5\"\n";
  config += "proto = \"=http,https\"\n";
  config += "proto-redir = \"=http,https\"\n";
  if (request.include_response_headers)
    config += "include\n";
  if (streaming)
  {
    config += "dump-header = \"/dev/stderr\"\n";
    config += "suppress-connect-headers\n";
  }
  for (auto const& override : request.resolve_hosts)
    config += "resolve = \"" + curl_config_escape(override) + "\"\n";
  if (!request.resolve_hosts.empty())
    config += "noproxy = \"*\"\n";
  config += "silent\n";
  config += "show-error\n";
  config += "no-progress-meter\n";
  config += "max-time = \"" + std::to_string(static_cast<double>(std::max(1, request.timeout_ms)) / 1000.0) + "\"\n";
  for (auto const& [name, value] : request.headers)
    config += "header = \"" + curl_config_escape(name + ": " + value) + "\"\n";
  if (!request.body.empty())
    config += "data-binary = \"@" + curl_config_escape(body_path) + "\"\n";
  return config;
}

void append_bounded(std::string& value, char const* data, std::size_t size, std::size_t limit)
{
  if (value.size() >= limit)
    return;
  value.append(data, std::min(size, limit - value.size()));
}

bool is_http_status_line(std::string_view line)
{
  if (!line.starts_with("HTTP/"))
    return false;
  auto const space = line.find(' ');
  if (space == std::string_view::npos || space + 4 > line.size())
    return false;
  return std::ranges::all_of(line.substr(space + 1, 3), [](char ch) { return ch >= '0' && ch <= '9'; });
}

int http_status_line_code(std::string_view line)
{
  auto const space = line.find(' ');
  if (space == std::string_view::npos || space + 4 > line.size())
    return 0;
  int code = 0;
  for (char const ch : line.substr(space + 1, 3))
  {
    if (ch < '0' || ch > '9')
      return 0;
    code = (code * 10) + (ch - '0');
  }
  return code;
}

struct StreamingResponseHead
{
  int status_code = 0;
  std::map<std::string, std::string> headers;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

ava::core::Result<StreamingResponseHead> parse_streaming_response_head(std::string_view header_text)
{
  auto const status_end = header_text.find('\n');
  auto status_line = header_text.substr(0, status_end);
  if (!status_line.empty() && status_line.back() == '\r')
    status_line.remove_suffix(1);
  if (!is_http_status_line(status_line))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "curl streaming response had a malformed HTTP status line"));
  auto const status_space = status_line.find(' ');
  if (status_space + 4 < status_line.size() && status_line[status_space + 4] != ' ' && status_line[status_space + 4] != '\t')
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "curl streaming response had a malformed HTTP status line"));
  auto const status = http_status_line_code(status_line);
  if (status < 100 || status > 999)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "curl streaming response had an invalid HTTP status"));

  std::map<std::string, std::string> headers;
  auto line_start = status_end == std::string_view::npos ? header_text.size() : status_end + 1;
  while (line_start < header_text.size())
  {
    auto const line_end = header_text.find('\n', line_start);
    auto line = header_text.substr(line_start, line_end == std::string_view::npos ? header_text.size() - line_start : line_end - line_start);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    if (!line.empty())
    {
      auto const colon = line.find(':');
      if (colon == std::string_view::npos || colon == 0)
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "curl streaming response had a malformed HTTP header"));
      auto value = line.substr(colon + 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
        value.remove_suffix(1);
      headers[std::string(line.substr(0, colon))] = std::string(value);
    }
    if (line_end == std::string_view::npos)
      break;
    line_start = line_end + 1;
  }
  return StreamingResponseHead{.status_code = status, .headers = std::move(headers)};
}

bool has_nonempty_header(std::map<std::string, std::string> const& headers, std::string_view expected_name)
{
  return std::ranges::any_of(headers, [expected_name](auto const& entry) {
    if (entry.second.empty() || entry.first.size() != expected_name.size())
      return false;
    return std::ranges::equal(entry.first, expected_name, [](char lhs, char rhs) {
      return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
    });
  });
}

ava::core::Result<HttpResponse> parse_curl_output(std::string output, bool include_response_headers)
{
  auto const marker_position = output.rfind(kStatusMarker);
  if (marker_position == std::string::npos)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "curl response did not include an HTTP status");
    error.with_context("response_bytes", std::to_string(output.size()));
    return std::unexpected(std::move(error));
  }

  auto const status_text = output.substr(marker_position + kStatusMarker.size());
  int status = 0;
  for (char const ch : status_text)
  {
    if (ch < '0' || ch > '9')
      break;
    status = (status * 10) + (ch - '0');
  }
  output.resize(marker_position);

  std::map<std::string, std::string> headers;
  while (include_response_headers && output.starts_with("HTTP/"))
  {
    auto body_start = output.find("\r\n\r\n");
    std::size_t separator_size = 4;
    if (body_start == std::string::npos)
    {
      body_start = output.find("\n\n");
      separator_size = 2;
    }
    if (body_start == std::string::npos)
      break;
    headers.clear();
    auto const header_text = output.substr(0, body_start);
    auto status_line = header_text.substr(0, header_text.find('\n'));
    if (!status_line.empty() && status_line.back() == '\r')
      status_line.pop_back();
    if (!is_http_status_line(status_line))
      break;
    auto const block_status = http_status_line_code(status_line);
    std::size_t line_start = 0;
    bool first_line = true;
    while (line_start <= header_text.size())
    {
      auto const line_end = header_text.find('\n', line_start);
      auto line = header_text.substr(line_start, line_end == std::string::npos ? std::string::npos : line_end - line_start);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (!first_line)
      {
        if (auto const colon = line.find(':'); colon != std::string::npos)
        {
          auto name = line.substr(0, colon);
          auto value = line.substr(colon + 1);
          while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
          headers[std::move(name)] = std::move(value);
        }
      }
      first_line = false;
      if (line_end == std::string::npos)
        break;
      line_start = line_end + 1;
    }
    output.erase(0, body_start + separator_size);
    if (block_status == status)
      break;
  }
  return HttpResponse{.status_code = status, .headers = std::move(headers), .body = std::move(output)};
}

ava::core::Error generic_transport_error(ava::process::ExitStatusV1 const* status, std::uint64_t stdout_bytes, std::uint64_t stderr_bytes)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "curl transport failed");
  if (status != nullptr)
  {
    error.with_context("exit_code", status->has_exit_code ? std::to_string(status->exit_code) : "signaled");
    error.with_context("response_bytes", std::to_string(stdout_bytes));
    error.with_context("stderr_bytes", std::to_string(stderr_bytes));
  }
  return error;
}

ava::core::Error canceled_error()
{
  return ava::core::Error(ava::core::ErrorCategory::Unknown, "transport request canceled", ava::core::ErrorCode::Canceled);
}

ava::core::Error deadline_error()
{
  return ava::core::Error(ava::core::ErrorCategory::Provider, "curl transport deadline expired");
}

ava::core::Error output_limit_error()
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "curl response exceeded byte limit", ava::core::ErrorCode::BodyOutputLimit);
  error.with_context("max_bytes", std::to_string(kMaxCurlResponseBytes));
  return error;
}

ava::core::Error protocol_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
}

ava::core::Error cancellation_callback_error()
{
  return protocol_error("curl cancellation callback failed");
}

struct RequestFailure
{
  ava::process::TerminationReasonV1 reason = ava::process::TerminationReasonV1::ProtocolFailure;
  ava::core::Error error{ava::core::ErrorCategory::Io, "curl transport protocol failure"};

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] std::span<std::byte> writable_bytes(std::array<char, 4096>& buffer) noexcept
{
  return {reinterpret_cast<std::byte*>(buffer.data()), buffer.size()};
}

[[nodiscard]] std::span<std::byte const> readable_bytes(std::string_view value) noexcept
{
  return {reinterpret_cast<std::byte const*>(value.data()), value.size()};
}

class CurlRequest final
{
 public:
  CurlRequest(ava::process::ProcessScopeV1 const& parent_scope, std::string_view executable, HttpRequest const& request, bool streaming,
              Transport::BodyChunkSink body_sink, Transport::CancelCallback cancel_requested)
      : parent_scope_(parent_scope),
        executable_(executable),
        request_(request),
        streaming_(streaming),
        body_sink_(std::move(body_sink)),
        cancel_requested_(std::move(cancel_requested)),
        request_deadline_(saturating_add(Clock::now(), bounded_timeout(request.timeout_ms))),
        reservation_cleanup_deadline_(saturating_add(request_deadline_, kCleanupBudget)),
        reservation_settlement_deadline_(saturating_add(reservation_cleanup_deadline_, kSettlementObservationBudget))
  {
  }

  [[nodiscard]] ava::core::Result<HttpResponse> run()
  {
    if (auto checkpoint = prelaunch_checkpoint(); !checkpoint)
      return std::unexpected(std::move(checkpoint.error()));

    auto operation = parent_scope_.operation();
    if (!operation)
      return std::unexpected(generic_transport_error(nullptr, 0, 0));
    auto environment = ava::process::make_curl_environment_v1(operation->host_environment());
    if (!environment)
      return std::unexpected(generic_transport_error(nullptr, 0, 0));

    std::optional<TempBodyFile> body_file;
    std::string body_path;
    if (!request_.body.empty())
    {
      auto created = TempBodyFile::create(request_.body);
      if (!created)
        return std::unexpected(std::move(created.error()));
      body_file.emplace(std::move(*created));
      body_path = body_file->path();
    }
    auto config = build_curl_config(request_, body_path, streaming_);

    if (auto checkpoint = prelaunch_checkpoint(); !checkpoint)
      return std::unexpected(std::move(checkpoint.error()));

    auto& supervisor = operation->supervisor();
    auto reservation = supervisor.reserve(operation->owner_prefix(), ava::process::ProcessRoleV1::Curl,
                                          {.termination_grace = 0ms, .startup_timeout = 2000ms, .execution_deadline = request_deadline_});
    if (!reservation)
      return std::unexpected(generic_transport_error(nullptr, 0, 0));

    std::vector<std::string> arguments{"curl", "-q", "--config", "-", "--write-out", std::string(kWriteOut)};
    if (streaming_)
      arguments.emplace_back("--no-buffer");

    if (auto checkpoint = prelaunch_checkpoint(); !checkpoint)
      return std::unexpected(std::move(checkpoint.error()));

    auto spawned = supervisor.spawn(std::move(*reservation), {.executable = executable_.empty() ? std::string("curl") : std::string(executable_),
                                                              .argv = std::move(arguments),
                                                              .environment = std::move(*environment),
                                                              .cwd = "/",
                                                              .stdin_mode = ava::process::StreamModeV1::Capture,
                                                              .stdout_mode = ava::process::StreamModeV1::Capture,
                                                              .stderr_mode = ava::process::StreamModeV1::Capture});
    if (!spawned)
      return std::unexpected(generic_transport_error(nullptr, 0, 0));

    if (!spawned->standard_input || !spawned->standard_output || !spawned->standard_error)
    {
      auto failure = RequestFailure{.reason = ava::process::TerminationReasonV1::ProtocolFailure,
                                    .error = protocol_error("curl transport did not receive its captured streams")};
      return settle_without_streams(supervisor, *spawned, std::move(failure));
    }

    auto& input = *spawned->standard_input;
    auto& output = *spawned->standard_output;
    auto& error_output = *spawned->standard_error;
    auto& handle = spawned->handle;
    std::size_t config_offset = 0;
    bool input_open = true;
    bool output_open = true;
    bool error_open = true;
    std::optional<RequestFailure> failure;
    std::optional<ava::process::ExitStatusV1> terminal_status;
    bool stop_requested = false;

    auto request_cleanup = [&] {
      if (!failure || stop_requested)
        return;
      stop_requested = true;
      if (input_open)
      {
        input.close();
        input_open = false;
      }
      static_cast<void>(supervisor.request_stop(handle, failure->reason, failure_cleanup_deadline()));
    };

    while (true)
    {
      observe_cancellation(failure);
      request_cleanup();

      if (input_open && !failure)
      {
        while (config_offset < config.size())
        {
          auto const pending = readable_bytes(std::string_view(config).substr(config_offset));
          auto wrote = input.write(pending);
          if (!wrote)
          {
            set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, protocol_error("failed to write curl configuration"));
            break;
          }
          if (wrote->state == ava::process::PipeIoStateV1::WouldBlock)
            break;
          if (wrote->state != ava::process::PipeIoStateV1::Progress || wrote->bytes == 0)
          {
            set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, protocol_error("failed to write curl configuration"));
            break;
          }
          config_offset += wrote->bytes;
        }
        if (config_offset == config.size())
        {
          input.close();
          input_open = false;
        }
      }
      request_cleanup();

      drain_stderr(supervisor, handle, error_output, error_open, failure);
      request_cleanup();
      drain_stdout(supervisor, handle, output, output_open, failure);
      request_cleanup();

      auto waited = supervisor.try_wait(handle);
      if (!waited)
      {
        set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, protocol_error("failed to observe curl process completion"));
        request_cleanup();
      }
      else if (*waited)
      {
        terminal_status = **waited;
      }

      if (terminal_status && error_open)
        drain_stderr(supervisor, handle, error_output, error_open, failure);
      request_cleanup();
      if (terminal_status && output_open)
        drain_stdout(supervisor, handle, output, output_open, failure);
      request_cleanup();

      if (terminal_status && !output_open && !error_open)
        break;

      auto const loop_now = Clock::now();
      if (failure && loop_now >= failure_settlement_deadline())
        break;

      std::vector<ava::process::PipeWatchV1> watches;
      watches.reserve(3);
      auto add_watch = [&](ava::process::PipeEndpoint& endpoint, ava::process::PipeInterestV1 interest, std::uint32_t token) {
        auto watch = endpoint.watch(interest, token);
        if (!watch)
        {
          set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, protocol_error("failed to watch curl process streams"));
          return;
        }
        watches.push_back(std::move(*watch));
      };
      if (input_open && !failure)
        add_watch(input, ava::process::PipeInterestV1::Writable, kInputWatch);
      if (output_open)
        add_watch(output, ava::process::PipeInterestV1::Readable, kStdoutWatch);
      if (error_open)
        add_watch(error_output, ava::process::PipeInterestV1::Readable, kStderrWatch);
      request_cleanup();

      auto const observation_limit = failure ? failure_settlement_deadline() : request_deadline_;
      auto const activity_deadline = std::min(observation_limit, saturating_add(Clock::now(), 100ms));
      auto activity = supervisor.wait_for_activity(handle, watches, activity_deadline);
      if (!activity)
      {
        set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, protocol_error("failed while waiting for curl process activity"));
        request_cleanup();
      }
    }

    if (input_open)
      input.close();
    auto settled = supervisor.wait(handle, failure ? failure_settlement_deadline() : reservation_settlement_deadline_);
    if (settled)
      terminal_status = *settled;
    else
    {
      set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, protocol_error("failed to settle curl process cleanup"));
      request_cleanup();
    }
    while (error_open)
    {
      auto const progressed = drain_stderr(supervisor, handle, error_output, error_open, failure);
      request_cleanup();
      if (!progressed)
        break;
    }
    while (output_open)
    {
      auto const progressed = drain_stdout(supervisor, handle, output, output_open, failure);
      request_cleanup();
      if (!progressed)
        break;
    }
    if (output_open)
      output.close();
    if (error_open)
      error_output.close();

    if (failure)
      return std::unexpected(std::move(failure->error));
    if (!terminal_status)
      return std::unexpected(generic_transport_error(nullptr, stdout_bytes_, stderr_bytes_));
    if (terminal_status->reason != ava::process::TerminationReasonV1::NaturalExit)
      return std::unexpected(error_for_status(*terminal_status));
    if (terminal_status->cleanup != ava::process::CleanupStateV1::Complete || terminal_status->kind != ava::process::ExitKindV1::Exited ||
        !terminal_status->has_exit_code || terminal_status->exit_code != 0)
    {
      return std::unexpected(generic_transport_error(&*terminal_status, stdout_bytes_, stderr_bytes_));
    }

    if (streaming_ && streaming_metadata_error_)
      return std::unexpected(std::move(*streaming_metadata_error_));
    if (!parsed_response_)
      return std::unexpected(protocol_error("curl response streams closed without a parsed result"));
    if (streaming_ && parsed_response_->status_code == 0)
      return std::unexpected(protocol_error("curl streaming response ended before a final HTTP response head"));
    if (streaming_)
      parsed_response_->body = std::move(streamed_body_);
    return std::move(*parsed_response_);
  }

 private:
  [[nodiscard]] ava::core::VoidResult prelaunch_checkpoint()
  {
    if (Clock::now() >= request_deadline_)
      return std::unexpected(deadline_error());
    if (!cancel_requested_)
      return {};

    bool canceled = false;
    try
    {
      canceled = cancel_requested_();
    }
    catch (...)
    {
      return std::unexpected(cancellation_callback_error());
    }
    if (Clock::now() >= request_deadline_)
      return std::unexpected(deadline_error());
    if (canceled)
      return std::unexpected(canceled_error());
    return {};
  }

  void establish_failure_deadlines() noexcept
  {
    if (failure_cleanup_deadline_)
      return;
    // A deadline failure observes now >= request_deadline_, so this minimum
    // preserves its reservation-time cleanup horizon. Earlier failures start
    // their cleanup budget now without ever extending that policy cap.
    failure_cleanup_deadline_ = std::min(reservation_cleanup_deadline_, saturating_add(Clock::now(), kCleanupBudget));
    failure_settlement_deadline_ = saturating_add(*failure_cleanup_deadline_, kSettlementObservationBudget);
  }

  [[nodiscard]] Clock::time_point failure_cleanup_deadline() const noexcept { return failure_cleanup_deadline_.value_or(reservation_cleanup_deadline_); }

  [[nodiscard]] Clock::time_point failure_settlement_deadline() const noexcept
  {
    return failure_settlement_deadline_.value_or(reservation_settlement_deadline_);
  }

  void set_failure(std::optional<RequestFailure>& failure, ava::process::TerminationReasonV1 reason, ava::core::Error error)
  {
    if (failure)
      return;
    establish_failure_deadlines();
    failure.emplace(RequestFailure{.reason = reason, .error = std::move(error)});
  }

  void observe_cancellation(std::optional<RequestFailure>& failure)
  {
    if (failure)
      return;
    if (Clock::now() >= request_deadline_)
    {
      set_failure(failure, ava::process::TerminationReasonV1::DeadlineExpired, deadline_error());
      return;
    }
    if (!cancel_requested_)
      return;

    bool canceled = false;
    try
    {
      canceled = cancel_requested_();
    }
    catch (...)
    {
      set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, cancellation_callback_error());
      return;
    }
    if (Clock::now() >= request_deadline_)
      set_failure(failure, ava::process::TerminationReasonV1::DeadlineExpired, deadline_error());
    else if (canceled)
      set_failure(failure, ava::process::TerminationReasonV1::Canceled, canceled_error());
  }

  [[nodiscard]] ava::core::Result<HttpResponse> settle_without_streams(ava::process::Supervisor& supervisor, ava::process::SpawnResultV1& spawned,
                                                                       RequestFailure failure)
  {
    establish_failure_deadlines();
    if (spawned.standard_input)
      spawned.standard_input->close();
    if (spawned.standard_output)
      spawned.standard_output->close();
    if (spawned.standard_error)
      spawned.standard_error->close();
    static_cast<void>(supervisor.request_stop(spawned.handle, failure.reason, failure_cleanup_deadline()));
    static_cast<void>(supervisor.wait(spawned.handle, failure_settlement_deadline()));
    return std::unexpected(std::move(failure.error));
  }

  [[nodiscard]] ava::core::Error error_for_status(ava::process::ExitStatusV1 const& status) const
  {
    switch (status.reason)
    {
      case ava::process::TerminationReasonV1::Canceled:
        return canceled_error();
      case ava::process::TerminationReasonV1::DeadlineExpired:
        return deadline_error();
      case ava::process::TerminationReasonV1::OutputLimit:
        return output_limit_error();
      case ava::process::TerminationReasonV1::ProtocolFailure:
        return protocol_error("curl transport protocol failure");
      default:
        return generic_transport_error(&status, stdout_bytes_, stderr_bytes_);
    }
  }

  [[nodiscard]] ava::core::VoidResult retain_streaming_body(std::string_view chunk)
  {
    if (chunk.empty())
      return {};
    if (streamed_body_.size() > kMaxCurlResponseBytes || chunk.size() > kMaxCurlResponseBytes - streamed_body_.size())
      return std::unexpected(output_limit_error());
    streamed_body_.append(chunk);
    if (!streaming_head_ || streaming_head_->status_code < 200 || streaming_head_->status_code >= 300 || !body_sink_)
      return {};
    try
    {
      return body_sink_(chunk);
    }
    catch (...)
    {
      return std::unexpected(protocol_error("curl streaming sink failed"));
    }
  }

  [[nodiscard]] ava::core::VoidResult publish_pending_streaming_body()
  {
    if (!streaming_head_ || pending_stdout_.size() <= kStatusTailReserve)
      return {};
    auto const body_bytes = pending_stdout_.size() - kStatusTailReserve;
    auto retained = retain_streaming_body(std::string_view(pending_stdout_).substr(0, body_bytes));
    if (!retained)
      return std::unexpected(std::move(retained.error()));
    pending_stdout_.erase(0, body_bytes);
    return {};
  }

  [[nodiscard]] ava::core::VoidResult consume_streaming_stdout(std::string_view bytes)
  {
    if (pending_stdout_.size() > kMaxCurlResponseBytes + kStatusTailReserve ||
        bytes.size() > kMaxCurlResponseBytes + kStatusTailReserve - pending_stdout_.size())
    {
      return std::unexpected(output_limit_error());
    }
    pending_stdout_.append(bytes);
    return publish_pending_streaming_body();
  }

  [[nodiscard]] ava::core::VoidResult consume_streaming_metadata(std::string_view bytes)
  {
    if (streaming_head_ || streaming_metadata_rejected_ || streaming_metadata_error_)
      return {};
    pending_streaming_metadata_.append(bytes);

    constexpr std::string_view prefix = "HTTP/";
    if (!pending_streaming_metadata_.starts_with(prefix))
    {
      if (prefix.starts_with(pending_streaming_metadata_))
        return {};
      streaming_metadata_rejected_ = true;
      pending_streaming_metadata_.clear();
      return {};
    }

    while (!streaming_head_)
    {
      auto header_end = pending_streaming_metadata_.find("\r\n\r\n");
      std::size_t separator_size = 4;
      if (header_end == std::string::npos)
      {
        header_end = pending_streaming_metadata_.find("\n\n");
        separator_size = 2;
      }
      if (header_end == std::string::npos)
      {
        if (streaming_header_bytes_ > kMaxCurlStreamingHeaderBytes ||
            pending_streaming_metadata_.size() > kMaxCurlStreamingHeaderBytes - streaming_header_bytes_)
        {
          return std::unexpected(protocol_error("curl streaming response headers exceeded byte limit"));
        }
        return {};
      }

      auto const block_bytes = header_end + separator_size;
      if (streaming_header_bytes_ > kMaxCurlStreamingHeaderBytes || block_bytes > kMaxCurlStreamingHeaderBytes - streaming_header_bytes_)
        return std::unexpected(protocol_error("curl streaming response headers exceeded byte limit"));
      auto parsed = parse_streaming_response_head(std::string_view(pending_streaming_metadata_).substr(0, header_end));
      if (!parsed)
      {
        streaming_metadata_error_ = std::move(parsed.error());
        pending_streaming_metadata_.clear();
        return {};
      }
      streaming_header_bytes_ += block_bytes;
      pending_streaming_metadata_.erase(0, block_bytes);

      bool const informational = parsed->status_code >= 100 && parsed->status_code < 200 && parsed->status_code != 101;
      bool const followed_redirect =
          request_.follow_redirects && parsed->status_code >= 300 && parsed->status_code < 400 && has_nonempty_header(parsed->headers, "location");
      if (informational || followed_redirect)
        continue;
      streaming_head_ = std::move(*parsed);
      pending_streaming_metadata_.clear();
    }

    return publish_pending_streaming_body();
  }

  bool drain_stdout(ava::process::Supervisor& supervisor, ava::process::ProcessHandle const& handle, ava::process::PipeEndpoint& endpoint, bool& open,
                    std::optional<RequestFailure>& failure)
  {
    bool progressed = false;
    std::array<char, 4096> buffer{};
    while (open)
    {
      auto read = endpoint.read(writable_bytes(buffer));
      if (!read)
      {
        set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, protocol_error("failed to read curl output"));
        endpoint.close();
        open = false;
        break;
      }
      if (read->state == ava::process::PipeIoStateV1::WouldBlock)
        break;
      if (read->state == ava::process::PipeIoStateV1::EndOfStream)
      {
        stdout_eof_ = true;
        maybe_finalize_stdout(failure);
        endpoint.close();
        open = false;
        progressed = true;
        break;
      }
      if (read->bytes == 0)
        break;
      progressed = true;
      auto const bytes = read->bytes;
      bool truncated = failure.has_value();
      stdout_bytes_ = stdout_bytes_ > std::numeric_limits<std::uint64_t>::max() - bytes ? std::numeric_limits<std::uint64_t>::max() : stdout_bytes_ + bytes;

      if (!failure)
      {
        if (!streaming_ && stdout_bytes_ > kMaxCurlResponseBytes + kStatusTailReserve)
        {
          truncated = true;
          set_failure(failure, ava::process::TerminationReasonV1::OutputLimit, output_limit_error());
        }
        else if (!streaming_)
        {
          stdout_output_.append(buffer.data(), bytes);
        }
        else
        {
          auto consumed = consume_streaming_stdout(std::string_view(buffer.data(), bytes));
          if (!consumed)
          {
            truncated = consumed.error().code() == ava::core::ErrorCode::BodyOutputLimit;
            set_failure(failure, truncated ? ava::process::TerminationReasonV1::OutputLimit : ava::process::TerminationReasonV1::ProtocolFailure,
                        std::move(consumed.error()));
          }
        }
      }
      if (auto accounted = supervisor.account_output(handle, ava::process::StreamKindV1::StandardOutput, bytes, truncated); !accounted && !failure)
        set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, protocol_error("failed to account curl output"));
      if (failure)
        break;
    }
    return progressed;
  }

  void maybe_finalize_stdout(std::optional<RequestFailure>& failure)
  {
    if (!stdout_eof_ || (streaming_ && !streaming_head_ && !stderr_eof_))
      return;
    finalize_stdout(failure);
  }

  void finalize_stdout(std::optional<RequestFailure>& failure)
  {
    if (stdout_finalized_ || failure)
      return;
    stdout_finalized_ = true;
    if (!streaming_)
    {
      auto parsed = parse_curl_output(std::move(stdout_output_), request_.include_response_headers);
      if (!parsed)
      {
        set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, std::move(parsed.error()));
        return;
      }
      parsed_response_ = std::move(*parsed);
      return;
    }

    if (!streaming_head_)
    {
      pending_stdout_.clear();
      parsed_response_ = HttpResponse{};
      return;
    }
    if (pending_stdout_.size() < kStatusTailReserve)
    {
      set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure,
                  protocol_error("curl streaming response did not include an exact HTTP status trailer"));
      return;
    }
    auto const trailer_start = pending_stdout_.size() - kStatusTailReserve;
    auto const trailer = std::string_view(pending_stdout_).substr(trailer_start);
    if (!trailer.starts_with(kStatusMarker) || !std::ranges::all_of(trailer.substr(kStatusMarker.size()), [](char ch) { return ch >= '0' && ch <= '9'; }))
    {
      set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure,
                  protocol_error("curl streaming response did not include an exact HTTP status trailer"));
      return;
    }
    int trailer_status = 0;
    for (char const ch : trailer.substr(kStatusMarker.size()))
      trailer_status = trailer_status * 10 + (ch - '0');
    if (trailer_status != streaming_head_->status_code)
    {
      set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure,
                  protocol_error("curl streaming response status trailer did not match its final response head"));
      return;
    }
    auto retained = retain_streaming_body(std::string_view(pending_stdout_).substr(0, trailer_start));
    if (!retained)
    {
      auto const output_limited = retained.error().code() == ava::core::ErrorCode::BodyOutputLimit;
      set_failure(failure, output_limited ? ava::process::TerminationReasonV1::OutputLimit : ava::process::TerminationReasonV1::ProtocolFailure,
                  std::move(retained.error()));
      return;
    }
    pending_stdout_.clear();
    parsed_response_ = HttpResponse{.status_code = streaming_head_->status_code, .headers = std::move(streaming_head_->headers), .body = {}};
  }

  bool drain_stderr(ava::process::Supervisor& supervisor, ava::process::ProcessHandle const& handle, ava::process::PipeEndpoint& endpoint, bool& open,
                    std::optional<RequestFailure>& failure)
  {
    bool progressed = false;
    std::array<char, 4096> buffer{};
    while (open)
    {
      auto read = endpoint.read(writable_bytes(buffer));
      if (!read)
      {
        set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, protocol_error("failed to read curl stderr"));
        endpoint.close();
        open = false;
        break;
      }
      if (read->state == ava::process::PipeIoStateV1::WouldBlock)
        break;
      if (read->state == ava::process::PipeIoStateV1::EndOfStream)
      {
        stderr_eof_ = true;
        maybe_finalize_stdout(failure);
        endpoint.close();
        open = false;
        progressed = true;
        break;
      }
      if (read->bytes == 0)
        break;
      progressed = true;
      auto const retained_before = stderr_output_.size();
      append_bounded(stderr_output_, buffer.data(), read->bytes, kMaxCurlStderrBytes);
      bool const truncated = retained_before + read->bytes > kMaxCurlStderrBytes;
      if (streaming_ && !failure)
      {
        auto consumed = consume_streaming_metadata(std::string_view(buffer.data(), read->bytes));
        if (!consumed)
        {
          set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, std::move(consumed.error()));
        }
        else
        {
          maybe_finalize_stdout(failure);
        }
      }
      stderr_bytes_ =
          stderr_bytes_ > std::numeric_limits<std::uint64_t>::max() - read->bytes ? std::numeric_limits<std::uint64_t>::max() : stderr_bytes_ + read->bytes;
      if (auto accounted = supervisor.account_output(handle, ava::process::StreamKindV1::StandardError, read->bytes, truncated); !accounted && !failure)
        set_failure(failure, ava::process::TerminationReasonV1::ProtocolFailure, protocol_error("failed to account curl stderr"));
    }
    return progressed;
  }

  ava::process::ProcessScopeV1 const& parent_scope_;
  std::string_view executable_;
  HttpRequest const& request_;
  bool streaming_ = false;
  Transport::BodyChunkSink body_sink_;
  Transport::CancelCallback cancel_requested_;
  Clock::time_point request_deadline_;
  Clock::time_point reservation_cleanup_deadline_;
  Clock::time_point reservation_settlement_deadline_;
  std::optional<Clock::time_point> failure_cleanup_deadline_;
  std::optional<Clock::time_point> failure_settlement_deadline_;
  std::string stdout_output_;
  std::string pending_stdout_;
  std::string streamed_body_;
  std::string stderr_output_;
  std::string pending_streaming_metadata_;
  std::optional<StreamingResponseHead> streaming_head_;
  std::optional<ava::core::Error> streaming_metadata_error_;
  std::optional<HttpResponse> parsed_response_;
  std::size_t streaming_header_bytes_ = 0;
  std::uint64_t stdout_bytes_ = 0;
  std::uint64_t stderr_bytes_ = 0;
  bool streaming_metadata_rejected_ = false;
  bool stdout_eof_ = false;
  bool stderr_eof_ = false;
  bool stdout_finalized_ = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace

CurlCliTransport::CurlCliTransport(ava::process::ProcessScopeV1 parent_scope) : parent_scope_(std::move(parent_scope))
{
}

ava::core::Result<HttpResponse> CurlCliTransport::send(HttpRequest const& request)
{
  return send(request, nullptr);
}

ava::core::Result<HttpResponse> CurlCliTransport::send(HttpRequest const& request, CancelCallback cancel_requested)
{
  return CurlRequest(parent_scope_, test_executable_, request, false, nullptr, std::move(cancel_requested)).run();
}

bool CurlCliTransport::supports_streaming() const noexcept
{
  return true;
}

ava::core::Result<HttpResponse> CurlCliTransport::send_streaming(HttpRequest const& request, BodyChunkSink on_body_chunk, CancelCallback cancel_requested)
{
  if (request.include_response_headers)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "streaming curl transport does not support response headers"));
  }
  return CurlRequest(parent_scope_, test_executable_, request, true, std::move(on_body_chunk), std::move(cancel_requested)).run();
}

ava::core::VoidResult testing::CurlTransportTestAccess::set_executable(CurlCliTransport& transport, std::filesystem::path const& executable)
{
  auto const value = executable.string();
  if (!executable.is_absolute() || value.empty() || value.find('\0') != std::string::npos)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "fake curl executable must be one absolute path"));
  }
  transport.test_executable_ = value;
  return {};
}

}  // namespace ava::http
