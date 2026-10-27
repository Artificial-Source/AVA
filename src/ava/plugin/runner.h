#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/plugin/manifest.h"
#include "ava/plugin/ui_protocol.h"
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

namespace ava::plugin {

using CancelCallback = std::function<bool()>;

inline constexpr std::size_t kPluginResourceContentMaxBytes = 64 * 1024;
inline constexpr std::size_t kPluginDynamicResourceNameMaxBytes = 96;
inline constexpr std::size_t kPluginRunnerMaxRecordBytes = 1024 * 1024;
inline constexpr std::size_t kPluginRunnerMaxStderrBytes = 1024 * 1024;
inline constexpr std::size_t kPluginRunnerQueuedRecordCap = 64;
inline constexpr std::size_t kPluginRunnerQueuedByteMultiplier = 4;

enum class PluginDynamicResourceKind
{
  Prompt,
  Skill,
};

[[nodiscard]] std::string_view plugin_dynamic_resource_kind_name(PluginDynamicResourceKind kind) noexcept;
[[nodiscard]] std::string_view plugin_dynamic_resource_capability(PluginDynamicResourceKind kind) noexcept;
[[nodiscard]] bool is_valid_dynamic_resource_name(std::string_view name) noexcept;

struct PluginRunnerOptions
{
  std::filesystem::path workspace_dir;
  std::chrono::milliseconds startup_timeout{3000};
  std::chrono::milliseconds request_timeout{5000};
  std::size_t max_record_bytes = 64 * 1024;
  std::size_t max_stderr_bytes = 64 * 1024;
  // Required at start(). The runner derives exactly one operation owner before
  // reserving a Plugin record and retains only that derived authority.
  std::optional<ava::process::ProcessScopeV1> process_scope = std::nullopt;

  // Includes process-launch authority and must not enter generated output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginInitialization
{
  std::string api_version;
  std::string plugin_version;
  ava::core::NoLogString contributions_json;
  ava::core::NoLogString raw_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginToolCallResult
{
  bool ok = false;
  ava::core::NoLogString content;
  ava::core::NoLogString metadata_json;
  ava::core::NoLogString raw_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginCommandCallResult
{
  bool ok = false;
  ava::core::NoLogString content;
  ava::core::NoLogString metadata_json;
  ava::core::NoLogString raw_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginEventObserveResult
{
  bool ok = false;
  ava::core::NoLogString content;
  ava::core::NoLogString metadata_json;
  ava::core::NoLogString raw_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginDynamicResource
{
  std::string name;
  ava::core::NoLogString description;
  ava::core::NoLogString raw_json;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginDynamicResourceListResult
{
  bool ok = false;
  std::vector<PluginDynamicResource> resources;
  ava::core::NoLogString content;
  ava::core::NoLogString metadata_json;
  ava::core::NoLogString raw_json;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginDynamicResourceReadResult
{
  bool ok = false;
  ava::core::NoLogString content;
  ava::core::NoLogString metadata_json;
  ava::core::NoLogString raw_json;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PluginProxyRequest
{
  std::string id;
  std::string operation;
  ava::core::NoLogString arguments_json;
  ava::core::NoLogString raw_json;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PluginProxyResponse
{
  bool ok = false;
  ava::core::NoLogString content;
  ava::core::NoLogString metadata_json;
  std::string error_category;
  ava::core::NoLogString error_message;
  ava::core::NoLogString error_details;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using PluginProxyHandler = std::function<ava::core::Result<PluginProxyResponse>(PluginProxyRequest const&, CancelCallback)>;

inline constexpr std::chrono::seconds kPluginUiCommandDeadlineMax{120};

struct PluginUiHandler
{
  using Callback = std::function<ava::core::Result<PluginUiAction>(PluginUiRequest const&, std::chrono::steady_clock::time_point, CancelCallback)>;

  std::chrono::steady_clock::time_point deadline{};
  Callback callback = nullptr;

  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(callback); }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class PluginProcess final
{
 public:
  PluginProcess(PluginManifest manifest, PluginRunnerOptions options, ava::process::ProcessScopeV1 operation_scope,
                std::chrono::steady_clock::time_point startup_deadline = {});
  ~PluginProcess() noexcept;

  PluginProcess(PluginProcess const&) = delete;
  PluginProcess& operator=(PluginProcess const&) = delete;
  PluginProcess(PluginProcess&&) = delete;
  PluginProcess& operator=(PluginProcess&&) = delete;

  [[nodiscard]] static ava::core::Result<std::unique_ptr<PluginProcess>> start(PluginManifest manifest, PluginRunnerOptions options,
                                                                               CancelCallback cancel_requested = nullptr);

  [[nodiscard]] PluginManifest const& manifest() const noexcept;
  [[nodiscard]] PluginInitialization const& initialization() const noexcept;
  [[nodiscard]] std::string const& stderr_tail() const noexcept;
  [[nodiscard]] bool stderr_truncated() const noexcept;

  [[nodiscard]] ava::core::Result<PluginToolCallResult> call_tool(std::string_view tool_name, std::string_view arguments_json, std::string_view call_id = {},
                                                                  CancelCallback cancel_requested = nullptr, PluginProxyHandler proxy_handler = nullptr);
  [[nodiscard]] ava::core::Result<PluginCommandCallResult> call_command(std::string_view command_name, std::string_view arguments_json,
                                                                        std::string_view call_id = {}, CancelCallback cancel_requested = nullptr,
                                                                        PluginProxyHandler proxy_handler = nullptr, PluginUiHandler ui_handler = {});
  [[nodiscard]] ava::core::Result<PluginEventObserveResult> observe_event(std::string_view event_name, std::string_view payload_json,
                                                                          std::string_view call_id = {}, CancelCallback cancel_requested = nullptr,
                                                                          PluginProxyHandler proxy_handler = nullptr);
  [[nodiscard]] ava::core::Result<PluginDynamicResourceListResult> list_resources(PluginDynamicResourceKind kind, CancelCallback cancel_requested = nullptr,
                                                                                  PluginProxyHandler proxy_handler = nullptr);
  [[nodiscard]] ava::core::Result<PluginDynamicResourceReadResult> read_resource(PluginDynamicResourceKind kind, std::string_view name,
                                                                                 CancelCallback cancel_requested = nullptr,
                                                                                 PluginProxyHandler proxy_handler = nullptr);
  [[nodiscard]] ava::core::VoidResult shutdown(std::chrono::milliseconds grace = std::chrono::milliseconds(250));

  // Protocol buffers and process authority must never enter debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  [[nodiscard]] ava::core::VoidResult launch(CancelCallback const& cancel_requested);
  [[nodiscard]] ava::core::VoidResult initialize(CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult write_record(std::string_view record, std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                   std::string_view timeout_message, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<std::string> read_record(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                           std::string_view timeout_message, std::string_view closed_message,
                                                           CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult wait_for_writable(std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                        std::string_view timeout_message, CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<bool> handle_proxy_record(std::string_view record, std::chrono::steady_clock::time_point deadline,
                                                            std::chrono::milliseconds timeout, PluginProxyHandler const& proxy_handler,
                                                            CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::Result<PluginProxyResponse> dispatch_proxy_request(PluginProxyRequest const& request, PluginProxyHandler const& proxy_handler,
                                                                              std::chrono::steady_clock::time_point deadline,
                                                                              CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult write_proxy_response(std::string_view request_id, PluginProxyResponse const& response,
                                                           std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                           CancelCallback cancel_requested = nullptr);
  [[nodiscard]] ava::core::VoidResult drain_stdout(bool enforce_record_limit);
  [[nodiscard]] ava::core::VoidResult drain_stderr();
  [[nodiscard]] ava::core::VoidResult drain_for_cleanup(std::chrono::steady_clock::time_point deadline) noexcept;
  [[nodiscard]] ava::core::VoidResult settle_failure(ava::process::TerminationReasonV1 reason);
  [[nodiscard]] ava::core::Error fail_process(ava::process::TerminationReasonV1 reason, ava::core::Error error);
  [[nodiscard]] ava::core::VoidResult settle_until(ava::process::TerminationReasonV1 reason, std::chrono::steady_clock::time_point cleanup_deadline,
                                                   std::chrono::steady_clock::time_point observation_deadline);
  [[nodiscard]] ava::core::VoidResult settled_cleanup_result() const;
  [[nodiscard]] std::optional<ava::process::ExitStatusV1> observe_settlement() const;
  void append_stdout_tail(std::string_view chunk);
  void append_stderr(std::string_view chunk);
  void close_endpoints() noexcept;

  PluginManifest manifest_;
  PluginRunnerOptions options_;
  PluginInitialization initialization_;
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
  std::size_t next_request_id_ = 2;
  ava::core::NoLogString stdout_buffer_;
  ava::core::NoLogString stderr_tail_;
};

}  // namespace ava::plugin
