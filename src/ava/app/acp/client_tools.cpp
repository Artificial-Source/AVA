#include "sys.h"
#include "ava/app/acp/client_tools.h"
#include "ava/app/acp/protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>

namespace ava::app::acp {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr auto kFileCallTimeout = 5s;
constexpr auto kTerminalCallTimeout = 5s;
constexpr auto kTerminalCleanupTimeout = 2s;
constexpr auto kTerminalPollInterval = 10ms;
constexpr auto kMaxCommandTimeout = 120s;
constexpr std::size_t kTerminalOutputByteLimit = kMaxStringBytes;

ava::core::Error client_tool_error(std::string message, std::string_view method = {}, ava::core::ErrorCode code = ava::core::ErrorCode::Unspecified)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Io, std::move(message), code);
  if (!method.empty())
    error.with_context("method", std::string(method));
  return error;
}

ava::core::Error rpc_error(std::string_view method, JsonRpcError const& rpc)
{
  auto error = client_tool_error("ACP client tool request failed", method);
  error.with_context("rpc_code", std::to_string(rpc.code));
  error.with_context("cause", rpc.message);
  return error;
}

ava::core::Error canceled_error(std::string_view method)
{
  auto error = client_tool_error("ACP client tool request canceled", method, ava::core::ErrorCode::Canceled);
  error.with_context("canceled", "true");
  return error;
}

ava::core::Error dto_error(std::string_view method, std::string message)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
  error.with_context("method", std::string(method));
  return error;
}

bool cancel_requested(ava::tools::ToolIoCancelCallback const& callback)
{
  return callback && callback();
}

ava::core::Result<Json> response_object(std::string_view method, std::string_view result_json)
{
  auto parsed = Json::parse(result_json, nullptr, false, true);
  if (!parsed.is_object())
    return std::unexpected(dto_error(method, std::string(method) + " result must be an object"));
  return parsed;
}

ava::core::Result<std::string> bounded_json(Json const& value, std::string_view method)
{
  auto encoded = value.dump(-1, ' ', false, Json::error_handler_t::strict);
  if (encoded.size() > kMaxRecordBytes)
    return std::unexpected(dto_error(method, std::string(method) + " params exceed the ACP record limit"));
  return encoded;
}

ava::core::Result<std::shared_ptr<ClientRequestGateway>> gateway_for(std::weak_ptr<ClientRequestGateway> const& weak)
{
  auto gateway = weak.lock();
  if (!gateway)
    return std::unexpected(client_tool_error("ACP client tool gateway is unavailable"));
  return gateway;
}

std::optional<ava::core::Error> observe_fail_stop_error(PendingCall& pending, std::string_view method)
{
  if (pending.completion.wait_for(kTerminalCleanupTimeout) != std::future_status::ready)
    return std::nullopt;
  auto response = pending.completion.get();
  if (response || response.error().code != -32603)
    return std::nullopt;
  return rpc_error(method, response.error());
}

ava::core::Result<std::string> await_call(std::shared_ptr<ClientRequestGateway> const& gateway, PendingCall& pending, std::string_view method,
                                          std::chrono::milliseconds timeout, ava::tools::ToolIoCancelCallback const& cancel, OutboundCallPolicy policy)
{
  auto const deadline = std::chrono::steady_clock::now() + timeout;
  while (pending.completion.wait_for(kTerminalPollInterval) != std::future_status::ready)
  {
    if (cancel_requested(cancel) && gateway->cancel(pending.id, "ACP client tool request canceled by the active prompt"))
    {
      if (policy == OutboundCallPolicy::AbortConnectionIfDelivered)
        if (auto peer_error = observe_fail_stop_error(pending, method))
          return std::unexpected(std::move(*peer_error));
      return std::unexpected(canceled_error(method));
    }
    if (std::chrono::steady_clock::now() >= deadline && gateway->cancel(pending.id, "ACP client tool request exceeded its local deadline"))
    {
      if (policy == OutboundCallPolicy::AbortConnectionIfDelivered)
        if (auto peer_error = observe_fail_stop_error(pending, method))
          return std::unexpected(std::move(*peer_error));
      auto error = client_tool_error("ACP client tool request timed out", method);
      error.with_context("timeout_ms", std::to_string(timeout.count()));
      return std::unexpected(std::move(error));
    }
  }
  auto response = pending.completion.get();
  if (!response)
    return std::unexpected(rpc_error(method, response.error()));
  return std::move(*response);
}

ava::core::Result<std::string> call(std::weak_ptr<ClientRequestGateway> const& weak, std::string method, std::string params, std::chrono::milliseconds timeout,
                                    ava::tools::ToolIoCancelCallback const& cancel, bool check_cancel_before_send = true,
                                    OutboundCallPolicy policy = OutboundCallPolicy::Normal)
{
  if (check_cancel_before_send && cancel_requested(cancel))
    return std::unexpected(canceled_error(method));
  auto gateway = gateway_for(weak);
  if (!gateway)
    return std::unexpected(std::move(gateway.error()));
  auto pending = (*gateway)->send(method, std::move(params), timeout + kTerminalCleanupTimeout, policy);
  if (!pending)
  {
    auto error = pending.error();
    error.with_context("method", method);
    return std::unexpected(std::move(error));
  }
  return await_call(*gateway, *pending, method, timeout, cancel, policy);
}

ava::core::VoidResult object_result(std::string_view method, std::string_view result_json)
{
  auto object = response_object(method, result_json);
  if (!object)
    return std::unexpected(std::move(object.error()));
  return {};
}

class ClientExactFileAccess final : public ava::tools::ExactFileAccess
{
 public:
  ClientExactFileAccess(std::string session_id, std::weak_ptr<ClientRequestGateway> gateway, bool read_text_file, bool write_text_file)
      : session_id_(std::move(session_id)), gateway_(std::move(gateway)), read_text_file_(read_text_file), write_text_file_(write_text_file)
  {
  }

  [[nodiscard]] bool supports_read_text_file() const noexcept override { return read_text_file_; }
  [[nodiscard]] bool supports_write_text_file() const noexcept override { return write_text_file_; }

  [[nodiscard]] ava::core::Result<std::string> read_text_file(std::filesystem::path const& absolute_path,
                                                              ava::tools::ToolIoCancelCallback cancel) const override
  {
    return read_text_file_impl(absolute_path, {}, std::move(cancel));
  }

  [[nodiscard]] ava::core::Result<std::string> read_text_file_window(std::filesystem::path const& absolute_path, ava::tools::ExactFileReadOptions options,
                                                                     ava::tools::ToolIoCancelCallback cancel) const override
  {
    return read_text_file_impl(absolute_path, std::move(options), std::move(cancel));
  }

  [[nodiscard]] ava::core::VoidResult write_text_file(std::filesystem::path const& absolute_path, std::string_view content,
                                                      ava::tools::ToolIoCancelCallback cancel) const override
  {
    if (!write_text_file_)
      return std::unexpected(dto_error("fs/write_text_file", "fs/write_text_file was not negotiated for this ACP session"));
    if (content.size() > kMaxStringBytes)
      return std::unexpected(dto_error("fs/write_text_file", "fs/write_text_file content exceeds the ACP string limit"));
    auto params = base_file_params("fs/write_text_file", absolute_path);
    if (!params)
      return std::unexpected(std::move(params.error()));
    (*params)["content"] = content;
    auto encoded = bounded_json(*params, "fs/write_text_file");
    if (!encoded)
      return std::unexpected(std::move(encoded.error()));
    auto response = call(gateway_, "fs/write_text_file", std::move(*encoded), kFileCallTimeout, cancel, true, OutboundCallPolicy::AbortConnectionIfDelivered);
    if (!response)
      return std::unexpected(std::move(response.error()));
    return object_result("fs/write_text_file", *response);
  }

 private:
  [[nodiscard]] ava::core::Result<std::string> read_text_file_impl(std::filesystem::path const& absolute_path, ava::tools::ExactFileReadOptions options,
                                                                   ava::tools::ToolIoCancelCallback cancel) const
  {
    if (!read_text_file_)
      return std::unexpected(dto_error("fs/read_text_file", "fs/read_text_file was not negotiated for this ACP session"));
    auto params = base_file_params("fs/read_text_file", absolute_path);
    if (!params)
      return std::unexpected(std::move(params.error()));
    constexpr auto max_uint32 = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
    if (options.line)
    {
      if (*options.line == 0 || *options.line > max_uint32)
        return std::unexpected(dto_error("fs/read_text_file", "fs/read_text_file line is outside the ACP uint32 1-based range"));
      (*params)["line"] = static_cast<std::uint32_t>(*options.line);
    }
    if (options.limit)
    {
      if (*options.limit > max_uint32)
        return std::unexpected(dto_error("fs/read_text_file", "fs/read_text_file limit is outside the ACP uint32 range"));
      (*params)["limit"] = static_cast<std::uint32_t>(*options.limit);
    }
    auto encoded = bounded_json(*params, "fs/read_text_file");
    if (!encoded)
      return std::unexpected(std::move(encoded.error()));
    auto response = call(gateway_, "fs/read_text_file", std::move(*encoded), kFileCallTimeout, cancel);
    if (!response)
      return std::unexpected(std::move(response.error()));
    auto object = response_object("fs/read_text_file", *response);
    if (!object)
      return std::unexpected(std::move(object.error()));
    auto content = object->find("content");
    if (content == object->end() || !content->is_string())
      return std::unexpected(dto_error("fs/read_text_file", "fs/read_text_file result requires string content"));
    auto text = content->get<std::string>();
    if (text.size() > kMaxStringBytes)
      return std::unexpected(dto_error("fs/read_text_file", "fs/read_text_file content exceeds the ACP string limit"));
    return text;
  }

  [[nodiscard]] ava::core::Result<Json> base_file_params(std::string_view method, std::filesystem::path const& absolute_path) const
  {
    auto path = absolute_path.string();
    if (session_id_.empty() || session_id_.size() > kMaxStringBytes || !absolute_path.is_absolute() || path.empty() || path.size() > kMaxStringBytes)
      return std::unexpected(dto_error(method, std::string(method) + " requires bounded sessionId and absolute path"));
    return Json{{"sessionId", session_id_}, {"path", std::move(path)}};
  }

  std::string session_id_;
  std::weak_ptr<ClientRequestGateway> gateway_;
  bool const read_text_file_;
  bool const write_text_file_;
};

struct ExitStatus
{
  std::optional<std::int64_t> exit_code;
  std::optional<std::string> signal;
};

std::optional<std::int64_t> optional_exit_code(Json const& object)
{
  auto const field = object.find("exitCode");
  if (field == object.end() || field->is_null())
    return std::nullopt;
  std::uint64_t value = 0;
  if (field->is_number_unsigned())
    value = field->get<std::uint64_t>();
  else if (field->is_number_integer())
  {
    auto const signed_value = field->get<std::int64_t>();
    if (signed_value < 0)
      return std::nullopt;
    value = static_cast<std::uint64_t>(signed_value);
  }
  else
    return std::nullopt;
  if (value > std::numeric_limits<std::uint32_t>::max())
    return std::nullopt;
  return static_cast<std::int64_t>(value);
}

std::optional<std::string> optional_signal(Json const& object)
{
  auto const field = object.find("signal");
  if (field == object.end() || !field->is_string())
    return std::nullopt;
  auto signal = field->get<std::string>();
  if (signal.empty() || signal.size() > kMaxStringBytes)
    return std::nullopt;
  return signal;
}

ava::core::Result<std::optional<ExitStatus>> exit_status(Json const& object, std::string_view method)
{
  auto code = optional_exit_code(object);
  auto signal = optional_signal(object);
  if (code && signal)
    return std::unexpected(dto_error(method, std::string(method) + " status cannot contain both a valid exitCode and a valid signal"));
  if (!code && !signal)
    return std::optional<ExitStatus>{};
  return std::optional<ExitStatus>(ExitStatus{.exit_code = std::move(code), .signal = std::move(signal)});
}

void add_failure(std::optional<ava::core::Error>& primary, std::string_view phase, ava::core::Error error)
{
  if (!primary)
  {
    error.with_context("terminal_phase", std::string(phase));
    primary = std::move(error);
    return;
  }
  primary->with_context("cleanup_" + std::string(phase), error.format());
}

bool statuses_conflict(ExitStatus const& lhs, ExitStatus const& rhs)
{
  return lhs.exit_code != rhs.exit_code || lhs.signal != rhs.signal;
}

class TerminalLease
{
 public:
  using ReleaseCallback = std::function<ava::core::VoidResult()>;

  explicit TerminalLease(ReleaseCallback release) : release_(std::move(release)) { }
  TerminalLease(TerminalLease const&) = delete;
  TerminalLease& operator=(TerminalLease const&) = delete;
  ~TerminalLease() noexcept
  {
    if (!release_attempted_)
    {
      try
      {
        static_cast<void>(release_());
      }
      catch (...)
      {
      }
    }
  }

  [[nodiscard]] ava::core::VoidResult release()
  {
    release_attempted_ = true;
    return release_();
  }

 private:
  ReleaseCallback release_;
  bool release_attempted_ = false;
};

class ClientCommandExecutor final : public ava::tools::CommandExecutor
{
 public:
  ClientCommandExecutor(std::string session_id, std::weak_ptr<ClientRequestGateway> gateway) : session_id_(std::move(session_id)), gateway_(std::move(gateway))
  {
  }

  [[nodiscard]] ava::core::Result<ava::tools::CommandExecutionResult> execute(ava::tools::CommandExecutionRequest request) const override
  {
    if (request.argv.empty() || request.argv.front().empty())
      return std::unexpected(dto_error("terminal/create", "terminal/create requires a parsed command argv"));
    if (request.plan_metadata && request.plan_metadata->executor_identity_verified)
      return std::unexpected(dto_error("terminal/create", "delegated terminal execution must not receive verified local execution authority"));
    if (request.environment_profile && request.environment_profile->local_execution_authority)
      return std::unexpected(dto_error("terminal/create", "delegated terminal execution must not receive reusable local environment authority"));
    if (cancel_requested(request.cancel_requested))
      return ava::tools::CommandExecutionResult{.exit_code = -1,
                                                .timed_out = false,
                                                .canceled = true,
                                                .truncated = false,
                                                .output = {},
                                                .containment_applied = false,
                                                .containment_profile_id = {},
                                                .containment_network_mode = {}};
    if (!request.cwd.is_absolute())
      return std::unexpected(dto_error("terminal/create", "terminal/create cwd must be absolute"));
    if (request.timeout <= 0ms || request.timeout > kMaxCommandTimeout)
      return std::unexpected(dto_error("terminal/wait_for_exit", "terminal command timeout must be between 1ms and 120s"));
    if (session_id_.empty() || session_id_.size() > kMaxStringBytes)
      return std::unexpected(dto_error("terminal/create", "terminal/create requires a bounded sessionId"));
    for (auto const& argument : request.argv)
      if (argument.size() > kMaxStringBytes)
        return std::unexpected(dto_error("terminal/create", "terminal/create argv contains an oversized string"));

    Json args = Json::array();
    for (std::size_t index = 1; index < request.argv.size(); ++index) args.push_back(request.argv[index]);
    Json create_params{{"sessionId", session_id_},    {"command", request.argv.front()},
                       {"args", std::move(args)},     {"env", Json::array()},
                       {"cwd", request.cwd.string()}, {"outputByteLimit", std::min(request.output_byte_limit, kTerminalOutputByteLimit)}};
    auto encoded_create = bounded_json(create_params, "terminal/create");
    if (!encoded_create)
      return std::unexpected(std::move(encoded_create.error()));
    auto create_response = call(gateway_, "terminal/create", std::move(*encoded_create), kTerminalCallTimeout, request.cancel_requested, false,
                                OutboundCallPolicy::AbortConnectionIfDelivered);
    if (!create_response)
    {
      auto error = std::move(create_response.error());
      auto const detail = error.format();
      bool const ambiguous = detail.find("timed out") != std::string::npos || detail.find("outcome is unknown") != std::string::npos ||
                             detail.find("delivered") != std::string::npos || detail.find("canceled: true") != std::string::npos;
      error.with_context("terminal_id", ambiguous ? "unavailable; terminal/create may have been delivered and cannot be cleaned up"
                                                  : "not acquired; terminal/create failed before a usable ID was returned");
      return std::unexpected(std::move(error));
    }

    auto create_object = response_object("terminal/create", *create_response);
    if (!create_object)
    {
      if (auto gateway = gateway_.lock())
        gateway->abort("terminal/create returned a delivered malformed result without a usable terminalId");
      auto error = std::move(create_object.error());
      error.with_context("connection", "aborted because terminal ownership cannot be recovered");
      return std::unexpected(std::move(error));
    }
    auto id = create_object->find("terminalId");
    if (id == create_object->end() || !id->is_string() || id->get_ref<std::string const&>().empty() ||
        id->get_ref<std::string const&>().size() > kMaxIdStringBytes)
    {
      if (auto gateway = gateway_.lock())
        gateway->abort("terminal/create returned a delivered malformed result without a usable terminalId");
      auto error = dto_error("terminal/create", "terminal/create result requires a bounded non-empty terminalId");
      error.with_context("connection", "aborted because terminal ownership cannot be recovered");
      return std::unexpected(std::move(error));
    }
    auto terminal_id = id->get<std::string>();

    std::optional<ava::core::Error> failure;
    ava::tools::CommandExecutionResult result;
    std::optional<ExitStatus> waited_status;
    bool kill_needed = failure.has_value();
    auto params = terminal_params(terminal_id);
    if (!params)
    {
      add_failure(failure, "params", std::move(params.error()));
      kill_needed = true;
    }
    std::optional<TerminalLease> lease;
    if (params)
    {
      lease.emplace([this, terminal_params = *params]() -> ava::core::VoidResult {
        auto released = call(gateway_, "terminal/release", terminal_params, kTerminalCleanupTimeout, nullptr);
        if (!released)
          return std::unexpected(std::move(released.error()));
        return object_result("terminal/release", *released);
      });
    }

    if (!failure)
    {
      auto gateway = gateway_for(gateway_);
      if (!gateway)
      {
        add_failure(failure, "wait", std::move(gateway.error()));
        kill_needed = true;
      }
      else
      {
        auto wait_timeout = request.timeout + kTerminalCleanupTimeout;
        auto pending = (*gateway)->send("terminal/wait_for_exit", *params, wait_timeout);
        if (!pending)
        {
          auto error = pending.error();
          error.with_context("method", "terminal/wait_for_exit");
          add_failure(failure, "wait", std::move(error));
          kill_needed = true;
        }
        else
        {
          auto const deadline = std::chrono::steady_clock::now() + request.timeout;
          while (pending->completion.wait_for(kTerminalPollInterval) != std::future_status::ready)
          {
            if (cancel_requested(request.cancel_requested) && (*gateway)->cancel(pending->id, "ACP terminal wait canceled by the active prompt"))
            {
              result.canceled = true;
              kill_needed = true;
              break;
            }
            if (std::chrono::steady_clock::now() >= deadline && (*gateway)->cancel(pending->id, "ACP terminal wait exceeded the local command timeout"))
            {
              result.timed_out = true;
              kill_needed = true;
              break;
            }
          }
          if (!result.canceled && !result.timed_out)
          {
            auto response = pending->completion.get();
            if (!response)
            {
              add_failure(failure, "wait", rpc_error("terminal/wait_for_exit", response.error()));
              kill_needed = true;
            }
            else
            {
              auto object = response_object("terminal/wait_for_exit", *response);
              if (!object)
              {
                add_failure(failure, "wait", std::move(object.error()));
                kill_needed = true;
              }
              else
              {
                auto status = exit_status(*object, "terminal/wait_for_exit");
                if (!status)
                {
                  add_failure(failure, "wait", std::move(status.error()));
                  kill_needed = true;
                }
                else if (*status)
                  waited_status = std::move(**status);
              }
            }
          }
        }
      }
    }

    if (kill_needed && params)
    {
      auto killed = call(gateway_, "terminal/kill", *params, kTerminalCleanupTimeout, nullptr);
      if (!killed)
        add_failure(failure, "kill", std::move(killed.error()));
      else if (auto object = object_result("terminal/kill", *killed); !object)
        add_failure(failure, "kill", std::move(object.error()));
    }

    std::optional<ExitStatus> output_status;
    if (params)
    {
      auto output = call(gateway_, "terminal/output", *params, kTerminalCallTimeout, nullptr);
      if (!output)
        add_failure(failure, "output", std::move(output.error()));
      else
      {
        auto object = response_object("terminal/output", *output);
        if (!object)
          add_failure(failure, "output", std::move(object.error()));
        else
        {
          auto text = object->find("output");
          auto truncated = object->find("truncated");
          if (text == object->end() || !text->is_string() || truncated == object->end() || !truncated->is_boolean())
            add_failure(failure, "output", dto_error("terminal/output", "terminal/output result requires string output and boolean truncated"));
          else if (text->get_ref<std::string const&>().size() > kMaxStringBytes)
            add_failure(failure, "output", dto_error("terminal/output", "terminal/output output exceeds the ACP string limit"));
          else
          {
            result.output = text->get<std::string>();
            result.truncated = truncated->get<bool>();
            auto status_field = object->find("exitStatus");
            if (status_field != object->end() && status_field->is_object())
            {
              auto status = exit_status(*status_field, "terminal/output");
              if (!status)
                add_failure(failure, "output", std::move(status.error()));
              else if (*status)
                output_status = std::move(**status);
            }
          }
        }
      }
    }

    if (waited_status && output_status && statuses_conflict(*waited_status, *output_status))
      add_failure(failure, "output", dto_error("terminal/output", "terminal output exitStatus conflicts with terminal/wait_for_exit"));
    auto const status = waited_status ? waited_status : output_status;
    if (!result.canceled && !result.timed_out && status)
      result.exit_code = status->exit_code.value_or(-1);

    if (lease)
    {
      auto released = lease->release();
      if (!released)
        add_failure(failure, "release", std::move(released.error()));
    }

    if (failure)
      return std::unexpected(std::move(*failure));
    return result;
  }

 private:
  [[nodiscard]] ava::core::Result<std::string> terminal_params(std::string_view terminal_id) const
  {
    if (terminal_id.empty() || terminal_id.size() > kMaxIdStringBytes)
      return std::unexpected(dto_error("terminal", "terminal request requires a bounded terminalId"));
    return bounded_json(Json{{"sessionId", session_id_}, {"terminalId", terminal_id}}, "terminal");
  }

  std::string session_id_;
  std::weak_ptr<ClientRequestGateway> gateway_;
};

}  // namespace

std::shared_ptr<ava::tools::ExactFileAccess const> make_client_exact_file_access(std::string session_id, std::weak_ptr<ClientRequestGateway> gateway,
                                                                                 bool read_text_file, bool write_text_file)
{
  return std::make_shared<ClientExactFileAccess>(std::move(session_id), std::move(gateway), read_text_file, write_text_file);
}

std::shared_ptr<ava::tools::CommandExecutor const> make_client_command_executor(std::string session_id, std::weak_ptr<ClientRequestGateway> gateway)
{
  return std::make_shared<ClientCommandExecutor>(std::move(session_id), std::move(gateway));
}

}  // namespace ava::app::acp
