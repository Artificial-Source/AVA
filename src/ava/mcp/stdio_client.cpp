#include "sys.h"
#include "ava/diagnostics/safe_failure.h"
#include "ava/process/environment.h"
#include "ava/mcp/protocol.h"
#include "ava/mcp/stdio_client.h"
#include "ava/mcp/stdio_client_internal.h"
#include "ava/core/error.h"
#include "ava/core/json.h"
#include "ava/core/strict_json.h"
#include "ava/core/version.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "debug.h"

namespace ava::mcp {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int kMaxDrainReadsPerObservation = 16;
constexpr std::size_t kMaxMcpListPages = 8;
constexpr std::size_t kMaxMcpPaginationCursorBytes = 64 * 1024;
constexpr auto kMcpTerminationGrace = 100ms;
constexpr auto kMcpCleanupBudget = 2s;
constexpr auto kMcpSettlementObservationBudget = 250ms;
constexpr auto kMcpMaximumShutdownGrace = 5s;
constexpr auto kCancellationObservationInterval = 10ms;
constexpr auto kNaturalStatusObservation = 50ms;
constexpr std::uint32_t kInputWatch = 1;
constexpr std::uint32_t kStdoutWatch = 2;
constexpr std::uint32_t kStderrWatch = 3;

std::span<std::byte> writable_bytes(std::array<char, 4096>& buffer) noexcept
{
  return {reinterpret_cast<std::byte*>(buffer.data()), buffer.size()};
}

std::span<std::byte const> readable_bytes(std::string_view value) noexcept
{
  return {reinterpret_cast<std::byte const*>(value.data()), value.size()};
}

bool output_limit_error(ava::core::Error const& error)
{
  return std::ranges::any_of(error.context(), [](ava::core::ErrorContext const& context) { return context.key == "output_limit" && context.value == "true"; });
}

ava::core::Error process_io_error(std::string message, McpServerConfig const& server)
{
  return mcp_error(ava::core::ErrorCategory::Io, std::move(message), server);
}

ava::core::Error cleanup_error(std::string message)
{
  return ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
}

Clock::time_point mcp_lifetime_deadline(McpStdioClientOptions const& options) noexcept
{
  auto deadline = saturating_add(Clock::now(), options.startup_timeout);
  deadline = saturating_add(deadline, options.request_timeout);
  return saturating_add(deadline, kMcpCleanupBudget);
}

ava::core::Result<std::filesystem::path> absolute_working_directory(McpServerConfig const& server, McpStdioClientOptions const& options)
{
  auto working_directory = child_working_dir(server, options);
  if (working_directory.empty())
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP working directory is unavailable", server));
  if (!working_directory.is_absolute())
  {
    std::error_code error;
    working_directory = std::filesystem::absolute(working_directory, error);
    if (error || !working_directory.is_absolute())
      return std::unexpected(mcp_error(ava::core::ErrorCategory::Io, "failed to resolve the MCP working directory", server));
  }
  return working_directory.lexically_normal();
}

std::vector<ava::process::EnvironmentVariableV1> explicit_environment(McpServerConfig const& server)
{
  std::vector<ava::process::EnvironmentVariableV1> variables;
  variables.reserve(server.env.size());
  for (auto const& [name, value] : server.env)
    variables.push_back({.name = name, .value = value});
  return variables;
}

std::string compact_json_line(std::string_view message)
{
  std::string compact;
  compact.reserve(message.size() + 1);
  bool in_string = false;
  bool escaped = false;
  for (char const ch : message)
  {
    if (in_string)
    {
      compact.push_back(ch);
      if (escaped)
        escaped = false;
      else if (ch == '\\')
        escaped = true;
      else if (ch == '"')
        in_string = false;
      continue;
    }
    if (ch == '"')
    {
      in_string = true;
      compact.push_back(ch);
    }
    else if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
    {
      compact.push_back(ch);
    }
  }
  compact.push_back('\n');
  return compact;
}

ava::core::Result<std::optional<std::string>> pagination_cursor(std::string_view result_json, McpServerConfig const& server)
{
  if (!ava::core::json::field_value_start(result_json, "nextCursor"))
    return std::optional<std::string>{};
  auto cursor = ava::core::json::string_field(result_json, "nextCursor");
  if (!cursor || cursor->size() > kMaxMcpPaginationCursorBytes)
  {
    return std::unexpected(protocol_error("MCP list result has invalid nextCursor", server));
  }
  return std::optional<std::string>(std::move(*cursor));
}

std::optional<ava::process::TerminationReasonV1> interruption_reason(Clock::time_point deadline, CancelCallback const& cancel_requested) noexcept
{
  if (Clock::now() >= deadline)
    return ava::process::TerminationReasonV1::DeadlineExpired;
  if (is_canceled(cancel_requested))
    return ava::process::TerminationReasonV1::Canceled;
  if (Clock::now() >= deadline)
    return ava::process::TerminationReasonV1::DeadlineExpired;
  return std::nullopt;
}

void add_bounded_stream_metadata(ava::core::Error& error, std::string const& stdout_buffer, std::string const& stderr_tail, bool stderr_truncated)
{
  error.with_context("response_bytes", std::to_string(stdout_buffer.size()));
  error.with_context("stderr_bytes", std::to_string(stderr_tail.size()));
  error.with_context("stderr_truncated", stderr_truncated ? "true" : "false");
}

ava::core::Error stdout_limit_error(std::string message, McpServerConfig const& server, McpStdioClientOptions const& options, std::size_t queued_bytes)
{
  auto error = protocol_error(std::move(message), server);
  error.with_context("max_bytes", std::to_string(options.max_message_bytes));
  error.with_context("queued_bytes", std::to_string(queued_bytes));
  error.with_context("output_limit", "true");
  return error;
}

}  // namespace

McpStdioClient::McpStdioClient(McpServerConfig server, McpStdioClientOptions options, ava::process::ProcessScopeV1 operation_scope,
                               Clock::time_point startup_deadline)
    : server_(std::move(server)),
      options_(std::move(options)),
      startup_deadline_(startup_deadline == Clock::time_point{} ? saturating_add(Clock::now(), options_.startup_timeout) : startup_deadline),
      operation_scope_(std::move(operation_scope))
{
  options_.process_scope.reset();
}

McpStdioClient::~McpStdioClient() noexcept
{
  try
  {
    standard_input_.close();
    if (process_handle_.valid() && !settlement_)
    {
      if (settlement_error_)
      {
        // A bounded explicit wait may fail while the Supervisor still owns a
        // runnable group. Destruction gets one fresh bounded observation so a
        // sticky reporting error cannot suppress owner cleanup.
        settlement_error_.reset();
        cleanup_deadline_.reset();
        observation_deadline_.reset();
      }
      auto const cleanup_deadline = cleanup_deadline_.value_or(saturating_add(Clock::now(), kMcpCleanupBudget));
      auto const observation_deadline = observation_deadline_.value_or(saturating_add(cleanup_deadline, kMcpSettlementObservationBudget));
      static_cast<void>(settle_until(ava::process::TerminationReasonV1::OwnerShutdown, cleanup_deadline, observation_deadline));
    }
  }
  catch (...)
  {
  }
  close_endpoints();
}

ava::core::Result<std::unique_ptr<McpStdioClient>> McpStdioClient::start(McpServerConfig server, McpStdioClientOptions options, CancelCallback cancel_requested)
{
  if (server.command.empty())
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP server command must not be empty", server));
  }
  if (options.workspace_dir.empty())
  {
    std::error_code error;
    options.workspace_dir = std::filesystem::current_path(error);
    if (error || options.workspace_dir.empty())
      return std::unexpected(mcp_error(ava::core::ErrorCategory::Io, "MCP workspace directory is unavailable", server));
  }
  if (options.startup_timeout < 50ms || options.startup_timeout > 30s)
  {
    auto error = mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP startup timeout is out of bounds", server);
    error.with_context("min_ms", "50");
    error.with_context("max_ms", "30000");
    return std::unexpected(std::move(error));
  }
  if (options.request_timeout < 50ms || options.request_timeout > 30s)
  {
    auto error = mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP request timeout is out of bounds", server);
    error.with_context("min_ms", "50");
    error.with_context("max_ms", "30000");
    return std::unexpected(std::move(error));
  }
  if (options.max_message_bytes == 0 || options.max_stderr_bytes == 0)
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP client byte limits must be non-zero", server));
  }
  if (!options.process_scope)
    return std::unexpected(mcp_error(ava::core::ErrorCategory::Configuration, "MCP process authority is required", server));
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP startup canceled", server));
  }

  auto operation_scope = options.process_scope->operation();
  if (!operation_scope)
  {
    auto error = mcp_error(ava::core::ErrorCategory::Configuration, "failed to derive MCP operation process authority", server);
    error.with_context("cause", operation_scope.error().message());
    return std::unexpected(std::move(error));
  }
  options.process_scope.reset();

  auto const startup_deadline = saturating_add(Clock::now(), options.startup_timeout);
  auto client = std::make_unique<McpStdioClient>(std::move(server), std::move(options), std::move(*operation_scope), startup_deadline);
  if (auto launched = client->launch(cancel_requested); !launched)
    return std::unexpected(std::move(launched.error()));
  if (auto initialized = client->initialize(cancel_requested); !initialized)
    return std::unexpected(std::move(initialized.error()));
  return client;
}

McpServerConfig const& McpStdioClient::server() const noexcept
{
  return server_;
}

McpInitialization const& McpStdioClient::initialization() const noexcept
{
  return initialization_;
}

std::string const& McpStdioClient::stderr_tail() const noexcept
{
  return stderr_tail_;
}

bool McpStdioClient::stderr_truncated() const noexcept
{
  return stderr_truncated_;
}

ava::core::VoidResult McpStdioClient::launch(CancelCallback const& cancel_requested)
{
  auto working_directory = absolute_working_directory(server_, options_);
  if (!working_directory)
    return std::unexpected(std::move(working_directory.error()));
  auto environment = ava::process::make_mcp_environment_v1(*working_directory, explicit_environment(server_));
  if (!environment)
    return std::unexpected(mcp_error(ava::core::ErrorCategory::Configuration, "failed to create the MCP process environment", server_));
  auto arguments = mcp_argv(server_);
  auto executable = mcp_executable(server_, *working_directory);
  if (is_canceled(cancel_requested))
    return std::unexpected(canceled_error("MCP startup canceled", server_));

  auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds>(startup_deadline_ - Clock::now());
  if (remaining <= 0ms)
  {
    auto error = protocol_error("timed out preparing MCP startup", server_);
    error.with_context("timeout_ms", std::to_string(options_.startup_timeout.count()));
    return std::unexpected(std::move(error));
  }

  auto& supervisor = operation_scope_.supervisor();
  auto reservation =
      supervisor.reserve(operation_scope_.owner_prefix(), ava::process::ProcessRoleV1::Mcp,
                         {.termination_grace = kMcpTerminationGrace, .startup_timeout = remaining, .execution_deadline = mcp_lifetime_deadline(options_)});
  if (!reservation)
    return std::unexpected(std::move(reservation.error()));

  auto spawned = supervisor.spawn(std::move(*reservation), {.executable = std::move(executable),
                                                            .argv = std::move(arguments),
                                                            .environment = std::move(*environment),
                                                            .cwd = working_directory->string(),
                                                            .stdin_mode = ava::process::StreamModeV1::Capture,
                                                            .stdout_mode = ava::process::StreamModeV1::Capture,
                                                            .stderr_mode = ava::process::StreamModeV1::Capture,
                                                            .cancel_requested = cancel_requested});
  if (!spawned)
  {
    auto error = std::move(spawned.error());
    bool const startup_canceled = std::ranges::any_of(error.context(), [](ava::core::ErrorContext const& context) {
      return context.key == "reason" && context.value == ava::process::to_string(ava::process::TerminationReasonV1::Canceled);
    });
    if (!startup_canceled)
      return std::unexpected(std::move(error));
    auto canceled = canceled_error("MCP startup canceled", server_);
    canceled.with_context("reason", std::string(ava::process::to_string(ava::process::TerminationReasonV1::Canceled)));
    return std::unexpected(std::move(canceled));
  }

  process_handle_ = std::move(spawned->handle);
  if (spawned->standard_input)
    standard_input_ = std::move(*spawned->standard_input);
  if (spawned->standard_output)
    standard_output_ = std::move(*spawned->standard_output);
  if (spawned->standard_error)
    standard_error_ = std::move(*spawned->standard_error);
  if (!standard_input_.valid() || !standard_output_.valid() || !standard_error_.valid())
  {
    auto error = protocol_error("MCP process did not provide its captured streams", server_);
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }
  return {};
}

std::optional<ava::process::ExitStatusV1> McpStdioClient::observe_settlement() const
{
  if (settlement_)
    return settlement_;
  if (!process_handle_.valid())
    return std::nullopt;
  auto observed = operation_scope_.supervisor().try_wait(process_handle_);
  if (!observed || !*observed)
    return std::nullopt;
  return **observed;
}

ava::core::VoidResult McpStdioClient::settled_cleanup_result() const
{
  if (settlement_error_)
    return std::unexpected(*settlement_error_);
  if (!settlement_)
    return std::unexpected(cleanup_error("MCP process cleanup has not settled"));
  if (settlement_->cleanup != ava::process::CleanupStateV1::Complete)
    return std::unexpected(cleanup_error("MCP process cleanup is incomplete"));
  return {};
}

ava::core::VoidResult McpStdioClient::settle_until(ava::process::TerminationReasonV1 reason, Clock::time_point cleanup_deadline,
                                                   Clock::time_point observation_deadline)
{
  if (settlement_ || settlement_error_)
    return settled_cleanup_result();
  if (!process_handle_.valid())
  {
    settlement_error_ = cleanup_error("MCP process cleanup handle is unavailable");
    return std::unexpected(*settlement_error_);
  }

  if (!cleanup_deadline_)
    cleanup_deadline_ = cleanup_deadline;
  if (!observation_deadline_)
    observation_deadline_ = observation_deadline;
  cleanup_deadline = *cleanup_deadline_;
  observation_deadline = *observation_deadline_;
  standard_input_.close();

  if (auto observed = observe_settlement())
    settlement_ = *observed;
  if (!settlement_ && !stop_requested_)
  {
    // The caller observation remains bounded by observation_deadline. Retain
    // Supervisor cleanup authority for one destructor retry beyond that bound
    // so a surfaced wait error cannot immediately strand a runnable child at
    // the Supervisor's hard cleanup horizon.
    auto const supervisor_cleanup_deadline = saturating_add(observation_deadline, kMcpCleanupBudget);
    auto stopped = operation_scope_.supervisor().request_stop(process_handle_, reason, supervisor_cleanup_deadline);
    if (!stopped)
    {
      settlement_error_ = cleanup_error("failed to request MCP process cleanup");
      settlement_error_->with_context("cause", stopped.error().message());
      return std::unexpected(*settlement_error_);
    }
    stop_requested_ = true;
  }

  while (!settlement_ && Clock::now() < observation_deadline)
  {
    static_cast<void>(drain_for_cleanup(std::min(observation_deadline, saturating_add(Clock::now(), 10ms))));
    if (auto observed = observe_settlement())
      settlement_ = *observed;
  }
  if (!settlement_)
  {
    auto observed = operation_scope_.supervisor().wait(process_handle_, observation_deadline);
    if (observed)
      settlement_ = *observed;
  }
  while (settlement_ && (standard_output_.valid() || standard_error_.valid()) && Clock::now() < observation_deadline)
    static_cast<void>(drain_for_cleanup(std::min(observation_deadline, saturating_add(Clock::now(), 10ms))));
  static_cast<void>(drain_for_cleanup(Clock::now()));
  close_endpoints();

  if (!settlement_)
  {
    settlement_error_ = cleanup_error("MCP process cleanup did not settle before its bounded deadline");
    return std::unexpected(*settlement_error_);
  }
  return settled_cleanup_result();
}

ava::core::VoidResult McpStdioClient::settle_failure(ava::process::TerminationReasonV1 reason)
{
  if (settlement_ || settlement_error_)
    return settled_cleanup_result();
  auto const cleanup_deadline = cleanup_deadline_.value_or(saturating_add(Clock::now(), kMcpCleanupBudget));
  auto const observation_deadline = observation_deadline_.value_or(saturating_add(cleanup_deadline, kMcpSettlementObservationBudget));
  return settle_until(reason, cleanup_deadline, observation_deadline);
}

ava::core::Error McpStdioClient::fail_process(ava::process::TerminationReasonV1 reason, ava::core::Error error)
{
  auto settled = settle_failure(reason);
  if (!settled)
    return std::move(settled.error());
  return error;
}

ava::core::VoidResult McpStdioClient::shutdown(std::chrono::milliseconds grace)
{
  if (grace < 0ms || grace > kMcpMaximumShutdownGrace)
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP shutdown grace is out of bounds", server_));
  if (settlement_ || settlement_error_)
    return settled_cleanup_result();

  standard_input_.close();
  auto const natural_deadline = saturating_add(Clock::now(), grace);
  while (!settlement_ && Clock::now() < natural_deadline)
  {
    static_cast<void>(drain_for_cleanup(std::min(natural_deadline, saturating_add(Clock::now(), 10ms))));
    if (auto observed = observe_settlement())
      settlement_ = *observed;
  }
  if (auto observed = observe_settlement())
    settlement_ = *observed;
  if (settlement_)
  {
    auto const drain_deadline = saturating_add(Clock::now(), kMcpSettlementObservationBudget);
    while ((standard_output_.valid() || standard_error_.valid()) && Clock::now() < drain_deadline)
      static_cast<void>(drain_for_cleanup(std::min(drain_deadline, saturating_add(Clock::now(), 10ms))));
    static_cast<void>(drain_for_cleanup(Clock::now()));
    close_endpoints();
    return settled_cleanup_result();
  }

  auto const cleanup_deadline = cleanup_deadline_.value_or(saturating_add(Clock::now(), kMcpCleanupBudget));
  auto const observation_deadline = observation_deadline_.value_or(saturating_add(cleanup_deadline, kMcpSettlementObservationBudget));
  return settle_until(ava::process::TerminationReasonV1::OwnerShutdown, cleanup_deadline, observation_deadline);
}

void McpStdioClient::close_endpoints() noexcept
{
  standard_input_.close();
  standard_output_.close();
  standard_error_.close();
}

ava::core::VoidResult McpStdioClient::initialize(CancelCallback cancel_requested)
{
  std::string const params =
      "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"ava\","
      "\"version\":\"" +
      std::string(ava::core::version::kFullVersion) + "\"}}";
  auto response = request("initialize", params, options_.startup_timeout, "timed out waiting for MCP initialization", cancel_requested, startup_deadline_);
  if (!response)
    return std::unexpected(std::move(response.error()));
  auto const protocol_version = ava::core::json::string_field(response->result_json, "protocolVersion");
  auto const server_info = ava::core::json::object_field(response->result_json, "serverInfo");
  auto const capabilities = ava::core::json::object_field(response->result_json, "capabilities");
  if (!protocol_version || *protocol_version != "2024-11-05" || !server_info || !capabilities || !ava::core::json::is_valid_object(*capabilities))
  {
    auto error = protocol_error("MCP initialize response is malformed", server_);
    error.with_context("response_bytes", std::to_string(response->raw_json.size()));
    return std::unexpected(std::move(error));
  }
  initialization_ = McpInitialization{.server_name = ava::core::json::string_field(*server_info, "name").value_or(server_.id),
                                      .server_version = ava::core::json::string_field(*server_info, "version").value_or(""),
                                      .capabilities_json = *capabilities,
                                      .raw_json = response->raw_json};

  std::string const notification = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
  return write_message(notification, startup_deadline_, options_.startup_timeout, "timed out writing MCP initialized notification",
                       "MCP server closed before initialized notification", cancel_requested);
}

ava::core::Result<std::vector<McpToolDescription>> McpStdioClient::list_tools(CancelCallback cancel_requested)
{
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP tools/list canceled", server_));
  }

  std::vector<McpToolDescription> tools;
  std::optional<std::string> cursor;
  for (std::size_t page = 0; page < kMaxMcpListPages; ++page)
  {
    std::string const params = cursor ? ("{\"cursor\":" + json_string(*cursor) + "}") : "{}";
    auto response = request("tools/list", params, options_.request_timeout, "timed out waiting for MCP tools/list", cancel_requested);
    if (!response)
      return std::unexpected(std::move(response.error()));
    auto const tools_start = ava::core::json::field_value_start(response->result_json, "tools");
    if (!tools_start || *tools_start >= response->result_json.size() || response->result_json[*tools_start] != '[')
    {
      auto error = protocol_error("MCP tools/list result has invalid tools field", server_);
      error.with_context("response_bytes", std::to_string(response->raw_json.size()));
      return std::unexpected(std::move(error));
    }

    for (auto const& tool_json : ava::core::json::objects_in_array_field(response->result_json, "tools"))
    {
      auto name = ava::core::json::string_field(tool_json, "name");
      if (!name || !ava::mcp::is_valid_mcp_tool_name(*name))
      {
        auto error = protocol_error("MCP tool has invalid name", server_);
        error.with_context("response_bytes", std::to_string(tool_json.size()));
        return std::unexpected(std::move(error));
      }
      auto input_schema = ava::core::json::object_field(tool_json, "inputSchema").value_or("{\"type\":\"object\"}");
      if (!ava::core::json::is_valid_object(input_schema))
      {
        return std::unexpected(protocol_error("MCP tool inputSchema is invalid", server_));
      }
      tools.push_back(McpToolDescription{.name = std::move(*name),
                                         .description = ava::core::json::string_field(tool_json, "description").value_or(""),
                                         .input_schema_json = std::move(input_schema)});
    }
    auto next_cursor = pagination_cursor(response->result_json, server_);
    if (!next_cursor)
      return std::unexpected(std::move(next_cursor.error()));
    if (!*next_cursor)
      return tools;
    cursor = std::move(**next_cursor);
  }
  auto error = protocol_error("MCP tools/list exceeded pagination limit", server_);
  error.with_context("max_pages", std::to_string(kMaxMcpListPages));
  return std::unexpected(std::move(error));
}

ava::core::Result<McpToolCallResult> McpStdioClient::call_tool(std::string_view tool_name, std::string_view arguments_json, CancelCallback cancel_requested)
{
  if (!ava::mcp::is_valid_mcp_tool_name(tool_name))
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP tool name is invalid", server_));
  }
  if (!ava::core::json::is_valid_object(arguments_json))
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP tool arguments must be a JSON object", server_));
  }
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP tools/call canceled", server_));
  }
  std::string const params = "{\"name\":" + json_string(tool_name) + ",\"arguments\":" + std::string(arguments_json) + "}";
  auto response = request("tools/call", params, options_.request_timeout, "timed out waiting for MCP tools/call", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  bool const is_error = mcp_bool_field(response->result_json, "isError").value_or(false);
  if (is_error)
  {
    Dout(dc::mcp, "operation=tool_call state=remote_failure response_bytes=" << response->raw_json.size());
    auto const failure = ava::diagnostics::external_failure(ava::diagnostics::ComponentClass::Mcp);
    return McpToolCallResult{.is_error = true, .content = ava::diagnostics::serialize_safe_failure_json(failure), .raw_json = {}};
  }
  return McpToolCallResult{.is_error = false, .content = mcp_text_content_from_result(response->result_json), .raw_json = response->raw_json};
}

ava::core::Result<std::vector<McpPromptDescription>> McpStdioClient::list_prompts(CancelCallback cancel_requested)
{
  if (is_canceled(cancel_requested))
    return std::unexpected(canceled_error("MCP prompts/list canceled", server_));

  std::vector<McpPromptDescription> prompts;
  std::optional<std::string> cursor;
  for (std::size_t page = 0; page < kMaxMcpListPages; ++page)
  {
    std::string const params = cursor ? ("{\"cursor\":" + json_string(*cursor) + "}") : "{}";
    auto response = request("prompts/list", params, options_.request_timeout, "timed out waiting for MCP prompts/list", cancel_requested);
    if (!response)
      return std::unexpected(std::move(response.error()));
    auto const prompts_start = ava::core::json::field_value_start(response->result_json, "prompts");
    if (!prompts_start || *prompts_start >= response->result_json.size() || response->result_json[*prompts_start] != '[')
    {
      auto error = protocol_error("MCP prompts/list result has invalid prompts field", server_);
      error.with_context("response_bytes", std::to_string(response->raw_json.size()));
      return std::unexpected(std::move(error));
    }

    for (auto const& prompt_json : ava::core::json::objects_in_array_field(response->result_json, "prompts"))
    {
      auto name = ava::core::json::string_field(prompt_json, "name");
      if (!name || !ava::mcp::is_valid_mcp_tool_name(*name))
      {
        auto error = protocol_error("MCP prompt has invalid name", server_);
        error.with_context("response_bytes", std::to_string(prompt_json.size()));
        return std::unexpected(std::move(error));
      }
      McpPromptDescription prompt{
          .name = std::move(*name), .description = ava::core::json::string_field(prompt_json, "description").value_or(""), .arguments = {}};
      for (auto const& argument_json : ava::core::json::objects_in_array_field(prompt_json, "arguments"))
      {
        auto argument_name = ava::core::json::string_field(argument_json, "name");
        if (!argument_name || !ava::mcp::is_valid_mcp_tool_name(*argument_name))
        {
          auto error = protocol_error("MCP prompt argument has invalid name", server_);
          error.with_context("response_bytes", std::to_string(argument_json.size()));
          return std::unexpected(std::move(error));
        }
        prompt.arguments.push_back(McpPromptArgumentDescription{.name = std::move(*argument_name),
                                                                .description = ava::core::json::string_field(argument_json, "description").value_or(""),
                                                                .required = mcp_bool_field(argument_json, "required").value_or(false)});
      }
      prompts.push_back(std::move(prompt));
    }

    auto next_cursor = pagination_cursor(response->result_json, server_);
    if (!next_cursor)
      return std::unexpected(std::move(next_cursor.error()));
    if (!*next_cursor)
      return prompts;
    cursor = std::move(**next_cursor);
  }
  auto error = protocol_error("MCP prompts/list exceeded pagination limit", server_);
  error.with_context("max_pages", std::to_string(kMaxMcpListPages));
  return std::unexpected(std::move(error));
}

ava::core::Result<McpPromptGetResult> McpStdioClient::get_prompt(std::string_view prompt_name, std::string_view arguments_json, CancelCallback cancel_requested)
{
  if (!ava::mcp::is_valid_mcp_tool_name(prompt_name))
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP prompt name is invalid", server_));
  }
  if (!ava::core::json::is_valid_object(arguments_json))
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP prompt arguments must be a JSON object", server_));
  }
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP prompts/get canceled", server_));
  }
  std::string const params = "{\"name\":" + json_string(prompt_name) + ",\"arguments\":" + std::string(arguments_json) + "}";
  auto response = request("prompts/get", params, options_.request_timeout, "timed out waiting for MCP prompts/get", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  return McpPromptGetResult{.content = mcp_prompt_text_from_result(response->result_json), .raw_json = response->raw_json};
}

ava::core::Result<std::vector<McpResourceDescription>> McpStdioClient::list_resources(CancelCallback cancel_requested)
{
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP resources/list canceled", server_));
  }

  std::vector<McpResourceDescription> resources;
  std::optional<std::string> cursor;
  for (std::size_t page = 0; page < kMaxMcpListPages; ++page)
  {
    std::string const params = cursor ? ("{\"cursor\":" + json_string(*cursor) + "}") : "{}";
    auto response = request("resources/list", params, options_.request_timeout, "timed out waiting for MCP resources/list", cancel_requested);
    if (!response)
      return std::unexpected(std::move(response.error()));
    auto const resources_start = ava::core::json::field_value_start(response->result_json, "resources");
    if (!resources_start || *resources_start >= response->result_json.size() || response->result_json[*resources_start] != '[')
    {
      auto error = protocol_error("MCP resources/list result has invalid resources field", server_);
      error.with_context("response_bytes", std::to_string(response->raw_json.size()));
      return std::unexpected(std::move(error));
    }
    for (auto const& resource_json : ava::core::json::objects_in_array_field(response->result_json, "resources"))
    {
      auto uri = ava::core::json::string_field(resource_json, "uri");
      if (!uri || !ava::mcp::is_valid_mcp_resource_uri(*uri))
      {
        auto error = protocol_error("MCP resource has invalid uri", server_);
        error.with_context("response_bytes", std::to_string(resource_json.size()));
        return std::unexpected(std::move(error));
      }
      resources.push_back(McpResourceDescription{.uri = std::move(*uri),
                                                 .name = ava::core::json::string_field(resource_json, "name").value_or(""),
                                                 .description = ava::core::json::string_field(resource_json, "description").value_or(""),
                                                 .mime_type = ava::core::json::string_field(resource_json, "mimeType").value_or("")});
    }
    auto next_cursor = pagination_cursor(response->result_json, server_);
    if (!next_cursor)
      return std::unexpected(std::move(next_cursor.error()));
    if (!*next_cursor)
      return resources;
    cursor = std::move(**next_cursor);
  }
  auto error = protocol_error("MCP resources/list exceeded pagination limit", server_);
  error.with_context("max_pages", std::to_string(kMaxMcpListPages));
  return std::unexpected(std::move(error));
}

ava::core::Result<McpResourceReadResult> parse_resource_read_result(std::string_view result_json, std::string_view raw_json, McpServerConfig const& server)
{
  auto const contents_start = ava::core::json::field_value_start(result_json, "contents");
  if (!contents_start || *contents_start >= result_json.size() || result_json[*contents_start] != '[')
  {
    auto error = protocol_error("MCP resources/read result has invalid contents field", server);
    error.with_context("response_bytes", std::to_string(raw_json.size()));
    return std::unexpected(std::move(error));
  }

  McpResourceReadResult result;
  result.raw_json = std::string(raw_json);
  bool saw_text = false;
  for (auto const& item : ava::core::json::objects_in_array_field(result_json, "contents"))
  {
    auto text = ava::core::json::string_field(item, "text");
    if (!text)
      continue;
    if (!saw_text)
    {
      result.uri = ava::core::json::string_field(item, "uri").value_or("");
      result.mime_type = ava::core::json::string_field(item, "mimeType").value_or("");
    }
    else
    {
      result.content += '\n';
    }
    saw_text = true;
    result.content += *text;
  }
  if (!saw_text)
  {
    auto error = protocol_error("MCP resources/read returned no text content", server);
    error.with_context("response_bytes", std::to_string(raw_json.size()));
    return std::unexpected(std::move(error));
  }
  return result;
}

ava::core::Result<McpResourceReadResult> McpStdioClient::read_resource(std::string_view uri, CancelCallback cancel_requested)
{
  if (!ava::mcp::is_valid_mcp_resource_uri(uri))
  {
    return std::unexpected(mcp_error(ava::core::ErrorCategory::InvalidArgument, "MCP resource uri is invalid", server_));
  }
  if (is_canceled(cancel_requested))
  {
    return std::unexpected(canceled_error("MCP resources/read canceled", server_));
  }
  std::string const params = "{\"uri\":" + json_string(uri) + "}";
  auto response = request("resources/read", params, options_.request_timeout, "timed out waiting for MCP resources/read", cancel_requested);
  if (!response)
    return std::unexpected(std::move(response.error()));
  return parse_resource_read_result(response->result_json, response->raw_json, server_);
}

ava::core::Result<McpStdioClient::JsonRpcResponse> McpStdioClient::request(std::string_view method, std::string_view params_json,
                                                                           std::chrono::milliseconds timeout, std::string_view timeout_message,
                                                                           CancelCallback cancel_requested, Clock::time_point deadline)
{
  if (deadline == Clock::time_point{})
    deadline = Clock::now() + timeout;
  auto const request_id = "ava_mcp_" + std::to_string(next_request_id_++);
  std::string const request_json =
      "{\"jsonrpc\":\"2.0\",\"id\":" + json_string(request_id) + ",\"method\":" + json_string(method) + ",\"params\":" + std::string(params_json) + "}";
  Dout(dc::mcp, "operation=request state=write request_bytes=" << request_json.size());
  if (auto written = write_message(request_json, deadline, timeout, "timed out writing MCP request", "MCP server closed before response", cancel_requested);
      !written)
  {
    return std::unexpected(std::move(written.error()));
  }

  while (true)
  {
    auto message = read_message(deadline, timeout, timeout_message, "MCP server closed before response", cancel_requested);
    if (!message)
      return std::unexpected(std::move(message.error()));
    Dout(dc::mcp, "operation=request state=read response_bytes=" << message->size());
    if (!mcp_json_depth_within_limit(*message, kMaxMcpJsonDepth) ||
        ava::core::validate_strict_json(*message, kMaxMcpJsonDepth) != ava::core::StrictJsonStatus::Valid || !ava::core::json::is_valid_object(*message))
    {
      auto error = protocol_error("MCP response is not a valid JSON object with strict bounds", server_);
      error.with_context("response_bytes", std::to_string(message->size()));
      return std::unexpected(std::move(error));
    }
    auto const id = mcp_response_id(*message);
    if (!id)
      continue;
    if (*id != request_id)
    {
      auto error = protocol_error("MCP response id did not match request", server_);
      error.with_context("response_bytes", std::to_string(message->size()));
      return std::unexpected(std::move(error));
    }
    if (auto error_json = ava::core::json::object_field(*message, "error"))
    {
      Dout(dc::mcp, "operation=request state=remote_error error_bytes=" << error_json->size());
      auto error = protocol_error("MCP request returned a remote protocol error", server_);
      error.with_context("response_bytes", std::to_string(message->size()));
      error.with_context("error_bytes", std::to_string(error_json->size()));
      return std::unexpected(std::move(error));
    }
    auto result_json = ava::core::json::object_field(*message, "result");
    if (!result_json)
    {
      auto error = protocol_error("MCP response is missing result object", server_);
      error.with_context("response_bytes", std::to_string(message->size()));
      return std::unexpected(std::move(error));
    }
    return JsonRpcResponse{.result_json = std::move(*result_json), .raw_json = std::move(*message)};
  }
}

ava::core::VoidResult McpStdioClient::write_message(std::string_view message, Clock::time_point deadline, std::chrono::milliseconds timeout,
                                                    std::string_view timeout_message, std::string_view closed_message, CancelCallback cancel_requested)
{
  auto const frame = compact_json_line(message);
  if (frame.size() - 1 > options_.max_message_bytes)
  {
    auto error = protocol_error("MCP request exceeds message size cap", server_);
    error.with_context("max_bytes", std::to_string(options_.max_message_bytes));
    return std::unexpected(std::move(error));
  }
  if (!standard_input_.valid())
    return std::unexpected(peer_closed_error(closed_message));

  std::size_t offset = 0;
  while (offset < frame.size())
  {
    if (auto interrupted = interruption_reason(deadline, cancel_requested))
    {
      if (*interrupted == ava::process::TerminationReasonV1::Canceled)
        return std::unexpected(fail_process(*interrupted, canceled_error("MCP request canceled", server_)));
      auto error = protocol_error(std::string(timeout_message), server_);
      error.with_context("timeout_ms", std::to_string(timeout.count()));
      return std::unexpected(fail_process(*interrupted, std::move(error)));
    }

    auto written = standard_input_.write(readable_bytes(std::string_view(frame).substr(offset)));
    if (!written)
    {
      auto error = process_io_error("failed to write MCP request", server_);
      return std::unexpected(std::move(error));
    }
    if (written->state == ava::process::PipeIoStateV1::Progress && written->bytes != 0)
    {
      offset += written->bytes;
      continue;
    }
    if (written->state == ava::process::PipeIoStateV1::WouldBlock)
    {
      if (auto writable = wait_for_writable(deadline, timeout, timeout_message, closed_message, cancel_requested); !writable)
        return std::unexpected(std::move(writable.error()));
      continue;
    }
    return std::unexpected(peer_closed_error(closed_message));
  }
  return {};
}

ava::core::Result<std::optional<std::string>> McpStdioClient::try_extract_message()
{
  auto const newline = stdout_buffer_.find('\n');
  if (newline == std::string::npos)
  {
    if (stdout_buffer_.size() > options_.max_message_bytes)
    {
      auto error = stdout_limit_error("MCP message exceeds size cap before newline", server_, options_, stdout_buffer_.size());
      static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, 0, true));
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::OutputLimit, std::move(error)));
    }
    return std::optional<std::string>{};
  }
  if (newline > options_.max_message_bytes)
  {
    auto error = stdout_limit_error("MCP message exceeds size cap", server_, options_, stdout_buffer_.size());
    static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, 0, true));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::OutputLimit, std::move(error)));
  }
  auto message = stdout_buffer_.substr(0, newline);
  stdout_buffer_.erase(0, newline + 1);
  if (!message.empty() && message.back() == '\r')
    message.pop_back();
  return std::optional<std::string>{std::move(message)};
}

ava::core::Result<std::string> McpStdioClient::read_message(Clock::time_point deadline, std::chrono::milliseconds timeout, std::string_view timeout_message,
                                                            std::string_view closed_message, CancelCallback cancel_requested)
{
  while (true)
  {
    if (is_canceled(cancel_requested))
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::Canceled, canceled_error("MCP request canceled", server_)));

    auto extracted = try_extract_message();
    if (!extracted)
      return std::unexpected(std::move(extracted.error()));
    if (*extracted)
      return std::move(**extracted);

    if (auto interrupted = interruption_reason(deadline, cancel_requested))
    {
      if (*interrupted == ava::process::TerminationReasonV1::Canceled)
        return std::unexpected(fail_process(*interrupted, canceled_error("MCP request canceled", server_)));
      auto error = protocol_error(std::string(timeout_message), server_);
      error.with_context("timeout_ms", std::to_string(timeout.count()));
      add_bounded_stream_metadata(error, stdout_buffer_, stderr_tail_, stderr_truncated_);
      return std::unexpected(fail_process(*interrupted, std::move(error)));
    }

    if (auto drained = drain_stderr(); !drained)
      return std::unexpected(std::move(drained.error()));
    if (auto drained = drain_stdout(true); !drained)
    {
      if (output_limit_error(drained.error()))
        return std::unexpected(fail_process(ava::process::TerminationReasonV1::OutputLimit, std::move(drained.error())));
      return std::unexpected(std::move(drained.error()));
    }
    if (stdout_buffer_.find('\n') != std::string::npos)
      continue;

    if (!standard_output_.valid())
    {
      auto const status_deadline = std::min(deadline, saturating_add(Clock::now(), kNaturalStatusObservation));
      auto observed = operation_scope_.supervisor().wait(process_handle_, status_deadline);
      if (observed)
        settlement_ = *observed;
      return std::unexpected(peer_closed_error(closed_message, stdout_buffer_.size()));
    }

    std::vector<ava::process::PipeWatchV1> watches;
    watches.reserve(2);
    auto stdout_watch = standard_output_.watch(ava::process::PipeInterestV1::Readable, kStdoutWatch);
    if (!stdout_watch)
      return std::unexpected(process_io_error("failed to watch MCP stdout", server_));
    watches.push_back(std::move(*stdout_watch));
    if (standard_error_.valid())
    {
      auto stderr_watch = standard_error_.watch(ava::process::PipeInterestV1::Readable, kStderrWatch);
      if (!stderr_watch)
        return std::unexpected(process_io_error("failed to watch MCP stderr", server_));
      watches.push_back(std::move(*stderr_watch));
    }

    auto const activity_deadline = std::min(deadline, saturating_add(Clock::now(), kCancellationObservationInterval));
    auto activity = operation_scope_.supervisor().wait_for_activity(process_handle_, watches, activity_deadline);
    if (!activity)
      return std::unexpected(process_io_error("failed while observing MCP process streams", server_));
  }
}

ava::core::VoidResult McpStdioClient::wait_for_writable(Clock::time_point deadline, std::chrono::milliseconds timeout, std::string_view timeout_message,
                                                        std::string_view closed_message, CancelCallback cancel_requested)
{
  while (true)
  {
    if (auto interrupted = interruption_reason(deadline, cancel_requested))
    {
      if (*interrupted == ava::process::TerminationReasonV1::Canceled)
        return std::unexpected(fail_process(*interrupted, canceled_error("MCP request canceled", server_)));
      auto error = protocol_error(std::string(timeout_message), server_);
      error.with_context("timeout_ms", std::to_string(timeout.count()));
      return std::unexpected(fail_process(*interrupted, std::move(error)));
    }
    if (!standard_input_.valid())
      return std::unexpected(peer_closed_error(closed_message));

    std::vector<ava::process::PipeWatchV1> watches;
    watches.reserve(3);
    auto input_watch = standard_input_.watch(ava::process::PipeInterestV1::Writable, kInputWatch);
    if (!input_watch)
      return std::unexpected(process_io_error("failed to watch MCP stdin", server_));
    watches.push_back(std::move(*input_watch));
    if (standard_output_.valid())
    {
      auto stdout_watch = standard_output_.watch(ava::process::PipeInterestV1::Readable, kStdoutWatch);
      if (!stdout_watch)
        return std::unexpected(process_io_error("failed to watch MCP stdout", server_));
      watches.push_back(std::move(*stdout_watch));
    }
    if (standard_error_.valid())
    {
      auto stderr_watch = standard_error_.watch(ava::process::PipeInterestV1::Readable, kStderrWatch);
      if (!stderr_watch)
        return std::unexpected(process_io_error("failed to watch MCP stderr", server_));
      watches.push_back(std::move(*stderr_watch));
    }

    auto activity = operation_scope_.supervisor().wait_for_activity(process_handle_, watches,
                                                                    std::min(deadline, saturating_add(Clock::now(), kCancellationObservationInterval)));
    if (!activity)
      return std::unexpected(process_io_error("failed while observing the MCP request stream", server_));
    if (auto drained = drain_stderr(); !drained)
      return std::unexpected(std::move(drained.error()));
    if (auto drained = drain_stdout(true); !drained)
    {
      if (output_limit_error(drained.error()))
        return std::unexpected(fail_process(ava::process::TerminationReasonV1::OutputLimit, std::move(drained.error())));
      return std::unexpected(std::move(drained.error()));
    }
    auto const input_ready = std::ranges::find_if(activity->ready, [](ava::process::PipeReadyV1 const& ready) { return ready.token == kInputWatch; });
    if (input_ready != activity->ready.end() && input_ready->writable)
      return {};
    if (input_ready != activity->ready.end() && (input_ready->peer_closed || input_ready->error))
      return std::unexpected(peer_closed_error(closed_message));
    if (activity->process_finished)
    {
      if (auto observed = observe_settlement())
        settlement_ = *observed;
      return std::unexpected(peer_closed_error(closed_message));
    }
  }
}

ava::core::Error McpStdioClient::peer_closed_error(std::string_view closed_message, std::optional<std::size_t> response_bytes) const
{
  auto error = protocol_error(std::string(closed_message), server_);
  if (auto status = observe_settlement())
    error.with_context("status", exit_detail(*status));
  if (response_bytes)
    error.with_context("response_bytes", std::to_string(*response_bytes));
  error.with_context("stderr_bytes", std::to_string(stderr_tail_.size()));
  error.with_context("stderr_truncated", stderr_truncated_ ? "true" : "false");
  return error;
}

ava::core::VoidResult McpStdioClient::drain_stdout(bool enforce_message_limit)
{
  if (!standard_output_.valid())
    return {};
  std::array<char, 4096> buffer{};
  for (int reads = 0; reads < kMaxDrainReadsPerObservation; ++reads)
  {
    auto read = standard_output_.read(writable_bytes(buffer));
    if (!read)
      return std::unexpected(process_io_error("failed to read MCP stdout", server_));
    if (read->state == ava::process::PipeIoStateV1::WouldBlock)
      return {};
    if (read->state == ava::process::PipeIoStateV1::EndOfStream)
    {
      standard_output_.close();
      return {};
    }
    if (read->state != ava::process::PipeIoStateV1::Progress || read->bytes == 0)
      return std::unexpected(process_io_error("MCP stdout returned an invalid stream state", server_));

    Dout(dc::mcp, "operation=stdout state=read bytes=" << read->bytes);
    if (enforce_message_limit)
    {
      if (stdout_buffer_.size() > options_.max_message_bytes || read->bytes > std::numeric_limits<std::size_t>::max() - stdout_buffer_.size() ||
          stdout_buffer_.size() + read->bytes > options_.max_message_bytes + 1)
      {
        auto const attempted_bytes = read->bytes > std::numeric_limits<std::size_t>::max() - stdout_buffer_.size() ? std::numeric_limits<std::size_t>::max()
                                                                                                                   : stdout_buffer_.size() + read->bytes;
        static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, read->bytes, true));
        return std::unexpected(stdout_limit_error("MCP message exceeds size cap before newline", server_, options_, attempted_bytes));
      }
      try
      {
        stdout_buffer_.append(buffer.data(), read->bytes);
      }
      catch (...)
      {
        static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, read->bytes, true));
        return std::unexpected(process_io_error("failed to retain bounded MCP stdout", server_));
      }
      bool const truncated = stdout_buffer_.size() > options_.max_message_bytes;
      if (auto accounted = operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, read->bytes, truncated);
          !accounted)
      {
        return std::unexpected(process_io_error("failed to account MCP stdout", server_));
      }
      if (truncated)
        return std::unexpected(stdout_limit_error("MCP message exceeds size cap before newline", server_, options_, stdout_buffer_.size()));
      continue;
    }

    bool const truncated =
        read->bytes > options_.max_message_bytes || stdout_buffer_.size() > options_.max_message_bytes - std::min(options_.max_message_bytes, read->bytes);
    try
    {
      if (read->bytes >= options_.max_message_bytes)
        stdout_buffer_.assign(buffer.data() + (read->bytes - options_.max_message_bytes), options_.max_message_bytes);
      else
      {
        if (stdout_buffer_.size() > options_.max_message_bytes - read->bytes)
          stdout_buffer_.erase(0, stdout_buffer_.size() - (options_.max_message_bytes - read->bytes));
        stdout_buffer_.append(buffer.data(), read->bytes);
      }
    }
    catch (...)
    {
      static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, read->bytes, true));
      return std::unexpected(process_io_error("failed to retain bounded MCP stdout", server_));
    }
    if (auto accounted = operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, read->bytes, truncated);
        !accounted)
    {
      return std::unexpected(process_io_error("failed to account MCP stdout", server_));
    }
  }
  return {};
}

ava::core::VoidResult McpStdioClient::drain_stderr()
{
  if (!standard_error_.valid())
    return {};
  std::array<char, 4096> buffer{};
  for (int reads = 0; reads < kMaxDrainReadsPerObservation; ++reads)
  {
    auto read = standard_error_.read(writable_bytes(buffer));
    if (!read)
      return std::unexpected(process_io_error("failed to read MCP stderr", server_));
    if (read->state == ava::process::PipeIoStateV1::WouldBlock)
      return {};
    if (read->state == ava::process::PipeIoStateV1::EndOfStream)
    {
      standard_error_.close();
      return {};
    }
    if (read->state != ava::process::PipeIoStateV1::Progress || read->bytes == 0)
      return std::unexpected(process_io_error("MCP stderr returned an invalid stream state", server_));

    auto const retained_before = stderr_tail_.size();
    bool const truncated = stderr_truncated_ || retained_before > options_.max_stderr_bytes - std::min(options_.max_stderr_bytes, read->bytes);
    Dout(dc::mcp, "operation=stderr state=read bytes=" << read->bytes << " retained_bytes=" << retained_before);
    try
    {
      append_stderr(std::string_view(buffer.data(), read->bytes));
    }
    catch (...)
    {
      static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardError, read->bytes, true));
      return std::unexpected(process_io_error("failed to retain bounded MCP stderr", server_));
    }
    if (auto accounted = operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardError, read->bytes,
                                                                      truncated || stderr_truncated_);
        !accounted)
    {
      return std::unexpected(process_io_error("failed to account MCP stderr", server_));
    }
  }
  return {};
}

void McpStdioClient::append_stderr(std::string_view chunk)
{
  if (chunk.empty())
    return;
  if (chunk.size() >= options_.max_stderr_bytes)
  {
    stderr_tail_ = std::string(chunk.substr(chunk.size() - options_.max_stderr_bytes));
    stderr_truncated_ = true;
    return;
  }
  if (stderr_tail_.size() + chunk.size() > options_.max_stderr_bytes)
  {
    auto const drop = stderr_tail_.size() + chunk.size() - options_.max_stderr_bytes;
    stderr_tail_.erase(0, drop);
    stderr_truncated_ = true;
  }
  stderr_tail_.append(chunk);
}

ava::core::VoidResult McpStdioClient::drain_for_cleanup(Clock::time_point deadline) noexcept
{
  try
  {
    if (stdout_buffer_.size() > options_.max_message_bytes)
    {
      stdout_buffer_.erase(0, stdout_buffer_.size() - options_.max_message_bytes);
      static_cast<void>(operation_scope_.supervisor().account_output(process_handle_, ava::process::StreamKindV1::StandardOutput, 0, true));
    }
    static_cast<void>(drain_stdout(false));
    static_cast<void>(drain_stderr());
    if (Clock::now() >= deadline || !process_handle_.valid())
      return {};

    std::vector<ava::process::PipeWatchV1> watches;
    watches.reserve(2);
    if (standard_output_.valid())
    {
      auto watch = standard_output_.watch(ava::process::PipeInterestV1::Readable, kStdoutWatch);
      if (watch)
        watches.push_back(std::move(*watch));
    }
    if (standard_error_.valid())
    {
      auto watch = standard_error_.watch(ava::process::PipeInterestV1::Readable, kStderrWatch);
      if (watch)
        watches.push_back(std::move(*watch));
    }
    static_cast<void>(operation_scope_.supervisor().wait_for_activity(process_handle_, watches, deadline));
    static_cast<void>(drain_stdout(false));
    static_cast<void>(drain_stderr());
  }
  catch (...)
  {
  }
  return {};
}

}  // namespace ava::mcp
