#include "sys.h"
#include "ava/agent/job_control.h"
#include "ava/agent/tool_dispatch_common.h"
#include "ava/agent/tool_dispatch_job.h"
#include "ava/core/json.h"
#include "ava/core/strict_json.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace ava::agent {
namespace {

using namespace ava::agent::tool_dispatch;

using Json = nlohmann::json;

constexpr std::size_t kMaxJobSteeringMessageBytes = 16U * 1024U;

struct JobToolRequest
{
  std::string action;
  std::string job_id;
  std::string message;
  std::chrono::milliseconds timeout{std::chrono::milliseconds(kDefaultPublicJobWaitTimeoutMs)};
};

ava::core::Result<JobToolRequest> parse_job_tool_request(std::string_view arguments, std::string_view tool_name)
{
  auto const strict = ava::core::validate_strict_json(arguments, ava::core::json::kMaxNestingDepth);
  if (strict == ava::core::StrictJsonStatus::DuplicateObjectKey)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job arguments contain duplicate member names"));
  if (strict != ava::core::StrictJsonStatus::Valid)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job arguments must be one valid JSON object"));
  auto root = Json::parse(arguments.begin(), arguments.end(), nullptr, false, true);
  if (root.is_discarded() || !root.is_object())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job arguments must be one valid JSON object"));
  for (auto const& [key, _] : root.items())
  {
    if (key != "action" && key != "job_id" && key != "timeout_ms" && key != "message")
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job arguments contain an unknown field");
      error.with_context("tool", std::string(tool_name)).with_context("argument", key);
      return std::unexpected(std::move(error));
    }
  }
  if (!root.contains("action") || !root["action"].is_string())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job action is required"));
  JobToolRequest request;
  request.action = root["action"].get<std::string>();
  if (request.action != "list" && request.action != "status" && request.action != "wait" && request.action != "result" && request.action != "cancel" &&
      request.action != "steer")
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job action is unsupported"));
  if (root.contains("job_id"))
  {
    if (!root["job_id"].is_string())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job_id must be a string"));
    request.job_id = root["job_id"].get<std::string>();
  }
  if (root.contains("message"))
  {
    if (!root["message"].is_string())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job message must be a string"));
    request.message = root["message"].get<std::string>();
  }
  if (request.action == "list")
  {
    if (!request.job_id.empty() || root.contains("timeout_ms") || root.contains("message"))
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job list accepts only action"));
    return request;
  }
  if (request.job_id.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job_id is required for this action"));
  if (request.job_id.size() > kMaxPublicJobIdBytes)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job_id is too long"));
  if (auto safe = reject_control_arg(request.job_id, "job_id", tool_name); !safe)
    return std::unexpected(std::move(safe.error()));
  if (request.action == "steer")
  {
    if (request.message.empty())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "message is required for job steer"));
    if (request.message.size() > kMaxJobSteeringMessageBytes)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "job steering message is too long"));
    if (auto safe = reject_nul_arg(request.message, "message", tool_name); !safe)
      return std::unexpected(std::move(safe.error()));
  }
  else if (root.contains("message"))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "message is accepted only for job steer"));
  }
  if (root.contains("timeout_ms"))
  {
    if (request.action != "wait")
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "timeout_ms is accepted only for job wait"));
    if (!root["timeout_ms"].is_number_integer())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "timeout_ms must be a positive integer"));
    long long timeout_ms = 0;
    try
    {
      timeout_ms = root["timeout_ms"].get<long long>();
    }
    catch (...)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "timeout_ms is out of range"));
    }
    if (timeout_ms <= 0)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "timeout_ms must be a positive integer"));
    request.timeout = std::chrono::milliseconds(std::min(timeout_ms, kMaxPublicJobWaitTimeoutMs));
  }
  return request;
}

ToolDispatchResult job_control_error(ProviderToolCall const& call, ava::core::Error const& source)
{
  auto const code =
      std::ranges::any_of(source.context(), [](ava::core::ErrorContext const& item) { return item.key == "job_error_code" && item.value == "job_not_ready"; })
          ? std::string_view("job_not_ready")
          : std::string_view("job_error");
  std::string text = "{\"tool\":\"job\",\"ok\":false,\"error\":{\"category\":\"" + ava::core::json::escape(ava::core::to_string(source.category())) +
                     "\",\"code\":\"" + std::string(code) + "\",\"message\":\"" + ava::core::json::escape(source.message()) + "\"}}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = false, .result_text = std::move(text)};
}

}  // namespace

ToolDispatchResult job_result(ava::tools::ToolContext const& context, ToolDispatchServices const& services, ProviderToolCall const& call)
{
  auto request = parse_job_tool_request(call.arguments_json, call.name);
  if (!request)
    return job_control_error(call, request.error());
  if (!services.subagent_coordinator || context.session_id.empty())
    return job_control_error(call, ava::core::Error(ava::core::ErrorCategory::Tool, "job controls are unavailable"));
  if (request->action == "list")
  {
    return ToolDispatchResult{
        .call_id = call.id, .name = call.name, .success = true, .result_text = public_job_list_json(services.subagent_coordinator->list(context.session_id))};
  }

  ava::core::Result<SubagentCoordinatorJobSnapshot> snapshot =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "job action was not dispatched"));
  PublicJobContent content = PublicJobContent::OmitTerminalContent;
  if (request->action == "status")
    snapshot = services.subagent_coordinator->snapshot(context.session_id, request->job_id);
  else if (request->action == "wait")
    snapshot = services.subagent_coordinator->wait(context.session_id, request->job_id, request->timeout);
  else if (request->action == "result")
  {
    snapshot = services.subagent_coordinator->result(context.session_id, request->job_id);
    content = PublicJobContent::IncludeTerminalResult;
  }
  else if (request->action == "cancel")
    snapshot = services.subagent_coordinator->cancel(context.session_id, request->job_id);
  else if (request->action == "steer")
    snapshot = services.subagent_coordinator->steer(context.session_id, request->job_id, std::move(request->message));
  if (!snapshot)
    return job_control_error(call, snapshot.error());
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = public_job_snapshot_json(*snapshot, content)};
}

}  // namespace ava::agent
