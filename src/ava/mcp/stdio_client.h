#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/mcp/config.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::mcp {

using CancelCallback = std::function<bool()>;

struct McpStdioClientOptions
{
  std::filesystem::path workspace_dir;
  // Required at start(). The client derives exactly one operation owner before
  // reserving an Mcp record and retains only that derived authority.
  std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt;
  std::chrono::milliseconds startup_timeout{3000};
  std::chrono::milliseconds request_timeout{5000};
  std::size_t max_message_bytes = 64 * 1024;
  std::size_t max_stderr_bytes = 64 * 1024;

  // Includes process-launch authority and must not enter generated output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct McpInitialization
{
  std::string server_name;
  std::string server_version;
  ava::core::NoLogString capabilities_json;
  ava::core::NoLogString raw_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct McpToolDescription
{
  std::string name;
  ava::core::NoLogString description;
  ava::core::NoLogString input_schema_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct McpPromptArgumentDescription
{
  std::string name;
  ava::core::NoLogString description;
  bool required = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct McpPromptDescription
{
  std::string name;
  ava::core::NoLogString description;
  std::vector<McpPromptArgumentDescription> arguments;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct McpResourceDescription
{
  ava::core::NoLogString uri;
  std::string name;
  ava::core::NoLogString description;
  std::string mime_type;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct McpToolCallResult
{
  bool is_error = false;
  ava::core::NoLogString content;
  ava::core::NoLogString raw_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct McpPromptGetResult
{
  ava::core::NoLogString content;
  ava::core::NoLogString raw_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct McpResourceReadResult
{
  ava::core::NoLogString content;
  ava::core::NoLogString uri;
  std::string mime_type;
  ava::core::NoLogString raw_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class McpStdioClient final
{
 public:
  McpStdioClient(McpServerConfig server, McpStdioClientOptions options, ava::process::ProcessScopeV1 operation_scope,
                 std::chrono::steady_clock::time_point startup_deadline = {});
  ~McpStdioClient() noexcept;

  McpStdioClient(McpStdioClient const&) = delete;
  McpStdioClient& operator=(McpStdioClient const&) = delete;
  McpStdioClient(McpStdioClient&&) = delete;
  McpStdioClient& operator=(McpStdioClient&&) = delete;

  [[nodiscard]] static ava::core::Result<std::unique_ptr<McpStdioClient>> start(McpServerConfig server, McpStdioClientOptions options,
                                                                                CancelCallback cancel_requested = nullptr);

  [[nodiscard]] McpServerConfig const& server() const noexcept;
  [[nodiscard]] McpInitialization const& initialization() const noexcept;
  [[nodiscard]] std::string const& stderr_tail() const noexcept;
  [[nodiscard]] bool stderr_truncated() const noexcept;

  [[nodiscard]] ava::core::Result<std::vector<McpToolDescription>> list_tools(CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<McpToolCallResult> call_tool(std::string_view tool_name, std::string_view arguments_json,
                                                               CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<std::vector<McpPromptDescription>> list_prompts(CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<McpPromptGetResult> get_prompt(std::string_view prompt_name, std::string_view arguments_json,
                                                                 CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<std::vector<McpResourceDescription>> list_resources(CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<McpResourceReadResult> read_resource(std::string_view uri, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult shutdown(std::chrono::milliseconds grace = std::chrono::milliseconds(250));

  // Protocol buffers and process authority must never enter debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct JsonRpcResponse
  {
    ava::core::NoLogString result_json;
    ava::core::NoLogString raw_json;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

  [[nodiscard]] ava::core::VoidResult check_committed_stop();
  [[nodiscard]] ava::core::VoidResult check_interruption(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                         std::string_view timeout_message, CancelCallback const& cancel_requested);
  [[nodiscard]] ava::core::VoidResult launch(CancelCallback const& cancel_requested);
  [[nodiscard]] ava::core::VoidResult initialize(CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<JsonRpcResponse> request(std::string_view method, std::string_view params_json, std::chrono::milliseconds timeout,
                                                           std::string_view timeout_message, CancelCallback cancel_requested = nullptr,
                                                           std::chrono::steady_clock::time_point deadline = {});
  [[nodiscard]] ava::core::VoidResult write_message(std::string_view message, std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                    std::string_view timeout_message, std::string_view closed_message,
                                                    CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<std::string> read_message(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                            std::string_view timeout_message, std::string_view closed_message,
                                                            CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<std::optional<std::string>> try_extract_message();
  [[nodiscard]] ava::core::VoidResult wait_for_writable(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                        std::string_view timeout_message, std::string_view closed_message,
                                                        CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Error peer_closed_error(std::string_view closed_message, std::optional<std::size_t> response_bytes = std::nullopt) const;
  [[nodiscard]] ava::core::VoidResult drain_stdout(bool enforce_message_limit);
  [[nodiscard]] ava::core::VoidResult drain_stderr(bool observe_stop = true);
  [[nodiscard]] ava::core::VoidResult drain_for_cleanup(std::chrono::steady_clock::time_point deadline) noexcept;
  [[nodiscard]] ava::core::VoidResult settle_failure(ava::process::TerminationReasonV1 reason);
  [[nodiscard]] ava::core::Error fail_process(ava::process::TerminationReasonV1 reason, ava::core::Error error);
  [[nodiscard]] ava::core::VoidResult settle_until(ava::process::TerminationReasonV1 reason, std::chrono::steady_clock::time_point cleanup_deadline,
                                                   std::chrono::steady_clock::time_point observation_deadline);
  [[nodiscard]] ava::core::VoidResult settled_cleanup_result() const;
  [[nodiscard]] std::optional<ava::process::ExitStatusV1> observe_settlement() const;
  void append_stderr(std::string_view chunk);
  void close_endpoints() noexcept;

  McpServerConfig server_;
  McpStdioClientOptions options_;
  McpInitialization initialization_;
  std::chrono::steady_clock::time_point startup_deadline_{};
  ava::process::ProcessScopeV1 operation_scope_;
  ava::process::ProcessHandle process_handle_;
  ava::process::PipeEndpoint standard_input_;
  ava::process::PipeEndpoint standard_output_;
  ava::process::PipeEndpoint standard_error_;
  std::optional<ava::process::ExitStatusV1> settlement_ = std::nullopt;
  std::optional<ava::core::Error> settlement_error_ = std::nullopt;
  std::optional<std::chrono::steady_clock::time_point> cleanup_deadline_ = std::nullopt;
  std::optional<std::chrono::steady_clock::time_point> observation_deadline_ = std::nullopt;
  bool stop_requested_ = false;
  bool stderr_truncated_ = false;
  std::size_t next_request_id_ = 1;
  ava::core::NoLogString stdout_buffer_;
  ava::core::NoLogString stderr_tail_;
};

}  // namespace ava::mcp
