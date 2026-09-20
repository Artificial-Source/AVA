#include "sys.h"
#include "ava/agent/subagent_config.h"
#include "ava/agent/tool_dispatch_common.h"
#include "ava/agent/tool_dispatch_task.h"
#include "ava/core/json.h"
#include "ava/core/strict_json.h"

#include <optional>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace ava::agent {
namespace {

using namespace ava::agent::tool_dispatch;

constexpr std::size_t kMaxTaskDescriptionBytes = 256;
constexpr std::size_t kMaxTaskPromptBytes = 64 * 1024;
constexpr std::size_t kMaxTaskSubagentTypeBytes = 128;
constexpr std::size_t kMaxTaskIdBytes = 256;
constexpr std::size_t kMaxTaskCommandBytes = 1024;
constexpr std::size_t kMaxTaskToolIterations = 1000;

using Json = nlohmann::json;

std::string xml_escape(std::string_view value)
{
  std::string escaped;
  escaped.reserve(value.size());
  for (char const ch : value)
  {
    switch (ch)
    {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&apos;";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

ava::core::VoidResult reject_oversized_task_arg(std::string_view value, std::string_view field, std::size_t max_bytes, std::string_view tool_name)
{
  if (value.size() <= max_bytes)
    return {};
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "task argument is too long");
  error.with_context("tool", std::string(tool_name));
  error.with_context("argument", std::string(field));
  error.with_context("max_bytes", std::to_string(max_bytes));
  return std::unexpected(std::move(error));
}

ava::core::Result<std::optional<std::string>> optional_task_string_arg(std::string_view arguments, std::string_view field, std::size_t max_bytes,
                                                                       std::string_view tool_name)
{
  auto value = ava::core::json::string_field(arguments, field);
  if (!value || value->empty())
    return std::optional<std::string>{};
  if (auto safe = reject_control_arg(*value, field, tool_name); !safe)
    return std::unexpected(std::move(safe.error()));
  if (auto bounded = reject_oversized_task_arg(*value, field, max_bytes, tool_name); !bounded)
  {
    return std::unexpected(std::move(bounded.error()));
  }
  return std::optional<std::string>{std::move(*value)};
}

bool is_allowed_task_field(std::string_view key)
{
  return key == "description" || key == "prompt" || key == "subagent_type" || key == "task_id" || key == "command" || key == "mode" || key == "background" ||
         key == "max_tool_iterations";
}

ava::core::Result<std::optional<std::size_t>> task_max_tool_iterations(std::string_view arguments, std::string_view tool_name)
{
  auto const strict = ava::core::validate_strict_json(arguments, ava::core::json::kMaxNestingDepth);
  if (strict == ava::core::StrictJsonStatus::DuplicateObjectKey)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "task arguments contain duplicate member names"));
  if (strict != ava::core::StrictJsonStatus::Valid)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "task arguments must be one valid JSON object"));
  auto root = Json::parse(arguments.begin(), arguments.end(), nullptr, false, true);
  if (root.is_discarded() || !root.is_object())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "task arguments must be one valid JSON object"));
  for (auto const& [key, _] : root.items())
  {
    if (!is_allowed_task_field(key))
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "task arguments contain an unknown field");
      error.with_context("tool", std::string(tool_name)).with_context("argument", key);
      return std::unexpected(std::move(error));
    }
  }
  if (!root.contains("max_tool_iterations"))
    return std::optional<std::size_t>{};
  if (!root["max_tool_iterations"].is_number_integer())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "max_tool_iterations must be an integer from 1 through 1000"));
  }
  long long value = 0;
  try
  {
    value = root["max_tool_iterations"].get<long long>();
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "max_tool_iterations must be an integer from 1 through 1000"));
  }
  if (value < 1 || value > static_cast<long long>(kMaxTaskToolIterations))
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "max_tool_iterations must be an integer from 1 through 1000"));
  }
  return std::optional<std::size_t>{static_cast<std::size_t>(value)};
}

ava::core::Result<bool> task_background_mode(std::string_view arguments, std::string_view tool_name)
{
  auto background = optional_bool_arg(arguments, "background", false, tool_name);
  if (!background)
    return std::unexpected(std::move(background.error()));
  bool const has_background = ava::core::json::field_value_start(arguments, "background").has_value();
  auto const mode_start = ava::core::json::field_value_start(arguments, "mode");
  if (!mode_start)
    return *background;
  auto mode = ava::core::json::string_field(arguments, "mode");
  if (!mode)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "task mode must be foreground or background");
    error.with_context("tool", std::string(tool_name)).with_context("argument", "mode");
    return std::unexpected(std::move(error));
  }
  if (*mode != "foreground" && *mode != "background")
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "task mode must be foreground or background");
    error.with_context("tool", std::string(tool_name)).with_context("argument", "mode");
    return std::unexpected(std::move(error));
  }
  bool const mode_background = *mode == "background";
  if (has_background && *background != mode_background)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "task mode conflicts with legacy background flag");
    error.with_context("tool", std::string(tool_name));
    return std::unexpected(std::move(error));
  }
  return mode_background;
}

ava::core::Result<SubagentDefinition> selected_subagent_definition(ToolDispatchServices const& services, std::string_view subagent_type,
                                                                   std::string_view tool_name)
{
  auto subagents = services.subagents.empty() ? builtin_subagents() : services.subagents;
  auto const* match = find_subagent(subagents, subagent_type);
  if (match)
    return *match;
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "unsupported subagent type");
  error.with_context("tool", std::string(tool_name));
  error.with_context("subagent_type", std::string(subagent_type));
  error.with_context("supported", subagent_names_csv(subagents));
  return std::unexpected(std::move(error));
}

void publish_subagent_launch_best_effort(ToolDispatchServices const& services, std::string_view tool_call_id) noexcept
{
  if (!services.subagent_launch.sink)
    return;
  try
  {
    services.subagent_launch.sink(SubagentLaunchNotification{.tool_call_id = std::string(tool_call_id),
                                                             .request_id = services.subagent_launch.request_id,
                                                             .correlation_id = services.subagent_launch.correlation_id,
                                                             .display = services.subagent_launch.display});
  }
  catch (...)
  {
    // Private presentation observers cannot affect validated task execution.
  }
}

}  // namespace

ToolDispatchResult task_result(ava::tools::ToolContext const& context, ToolDispatchServices const& services, ProviderToolCall const& call)
{
  auto max_tool_iterations = task_max_tool_iterations(call.arguments_json, call.name);
  if (!max_tool_iterations)
    return tool_error_result(call, max_tool_iterations.error());
  auto description = required_safe_string_arg(call.arguments_json, "description", call.name);
  if (!description)
    return tool_error_result(call, description.error());
  if (auto bounded = reject_oversized_task_arg(*description, "description", kMaxTaskDescriptionBytes, call.name); !bounded)
  {
    return tool_error_result(call, bounded.error());
  }
  auto prompt = required_text_arg(call.arguments_json, "prompt", call.name);
  if (!prompt)
    return tool_error_result(call, prompt.error());
  if (auto bounded = reject_oversized_task_arg(*prompt, "prompt", kMaxTaskPromptBytes, call.name); !bounded)
  {
    return tool_error_result(call, bounded.error());
  }
  auto subagent_type = required_safe_string_arg(call.arguments_json, "subagent_type", call.name);
  if (!subagent_type)
    return tool_error_result(call, subagent_type.error());
  if (auto bounded = reject_oversized_task_arg(*subagent_type, "subagent_type", kMaxTaskSubagentTypeBytes, call.name); !bounded)
  {
    return tool_error_result(call, bounded.error());
  }
  auto subagent = selected_subagent_definition(services, *subagent_type, call.name);
  if (!subagent)
    return tool_error_result(call, subagent.error());
  auto task_id = optional_task_string_arg(call.arguments_json, "task_id", kMaxTaskIdBytes, call.name);
  if (!task_id)
    return tool_error_result(call, task_id.error());
  auto command = optional_task_string_arg(call.arguments_json, "command", kMaxTaskCommandBytes, call.name);
  if (!command)
    return tool_error_result(call, command.error());
  auto background = task_background_mode(call.arguments_json, call.name);
  if (!background)
    return tool_error_result(call, background.error());
  // AgentTurnExecutor has already published the ordinary public Running event
  // on this thread. Emit private launch association only after all built-in task
  // arguments and catalog identity have validated, but before permission/start
  // can fail so the future card can retain truthful requested launch metadata.
  publish_subagent_launch_best_effort(services, call.id);

  if (!services.task_subagent_runner)
  {
    return simple_error_result(call, ava::core::ErrorCategory::Tool, "task subagent runner is unavailable");
  }

  auto tool_context = context_for_provider_tool(context, call);
  if (auto permission = ava::tools::ensure_permission(tool_context, ava::permissions::Operation::TaskRun, context.workspace_dir, *subagent_type, "task",
                                                      "task launch permission check failed");
      !permission)
  {
    return tool_error_result(call, permission.error());
  }

  auto run = services.task_subagent_runner(
      TaskSubagentRequest{.description = *description,
                          .prompt = *prompt,
                          .subagent_type = *subagent_type,
                          .subagent_system_prompt = subagent->system_prompt,
                          .tool_preset = subagent->tool_preset,
                          .max_tool_iterations = max_tool_iterations->has_value() ? *max_tool_iterations : subagent->max_tool_iterations,
                          .task_id = *task_id,
                          .command = command->value_or(""),
                          .background = *background});
  if (!run)
    return tool_error_result(call, run.error());

  auto const state = run->state.empty() ? std::string("completed") : run->state;
  auto const summary = state == "running" ? std::string("Background subagent task started: ") : std::string("Subagent task completed: ");
  auto const job_attr = run->job_id.empty() ? std::string{} : std::string(" job_id=\"") + xml_escape(run->job_id) + "\"";
  auto const content = std::string("<task id=\"") + xml_escape(run->task_id) + "\"" + job_attr + " state=\"" + xml_escape(state) + "\">" + "<summary>" +
                       summary + xml_escape(*description) + "</summary>" + "<task_result>" + xml_escape(run->final_text) + "</task_result></task>";
  std::string text = "{\"tool\":\"task\",\"ok\":true,\"task_id\":\"" + ava::core::json::escape(run->task_id) + "\",\"subagent_type\":\"" +
                     ava::core::json::escape(run->subagent_type) + "\",\"description\":\"" + ava::core::json::escape(*description) + "\",\"session_path\":\"" +
                     ava::core::json::escape(run->session_path.generic_string()) + "\",\"state\":\"" + ava::core::json::escape(state) + "\"" +
                     (run->job_id.empty() ? std::string{} : ",\"job_id\":\"" + ava::core::json::escape(run->job_id) + "\"") + ",\"stop_reason\":\"" +
                     ava::core::json::escape(run->stop_reason) + "\",\"provider_iterations\":" + std::to_string(run->provider_iterations) +
                     ",\"tool_calls\":" + std::to_string(run->tool_calls) + ",\"tool_iterations\":" + std::to_string(run->tool_iterations) +
                     ",\"task_result\":\"" + ava::core::json::escape(run->final_text) + "\",\"content\":\"" + ava::core::json::escape(content) + "\"}";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = true, .result_text = std::move(text)};
}

}  // namespace ava::agent
