#include "sys.h"
#include "ava/plugin/runner.h"
#include "ava/plugin/runner_protocol.h"
#include "ava/plugin/runner_support.h"
#include "ava/core/error.h"
#include "ava/core/json.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace ava::plugin {
namespace {

std::string proxy_response_json(std::string_view request_id, PluginProxyResponse const& response)
{
  std::string text = "{\"id\":" + json_string(request_id) + ",\"type\":\"proxy.response\",\"ok\":" + std::string(response.ok ? "true" : "false") +
                     ",\"content\":" + json_string(response.content);
  if (!response.metadata_json.empty() && ava::core::json::is_valid_object(response.metadata_json))
    text += ",\"metadata\":" + response.metadata_json;
  if (!response.ok)
  {
    auto const category = response.error_category.empty() ? std::string("tool") : response.error_category;
    auto const message = response.error_message.empty() ? std::string("plugin proxy request failed") : response.error_message;
    text += ",\"error\":{\"category\":" + json_string(category) + ",\"message\":" + json_string(message);
    if (!response.error_details.empty())
      text += ",\"details\":" + json_string(response.error_details);
    text += '}';
  }
  text += '}';
  return text;
}

PluginProxyResponse proxy_error_response(ava::core::Error const& error)
{
  return PluginProxyResponse{.ok = false,
                             .content = "",
                             .metadata_json = "",
                             .error_category = ava::core::to_string(error.category()),
                             .error_message = error.message(),
                             .error_details = error.format()};
}

PluginProxyResponse proxy_error_response(ava::core::ErrorCategory category, std::string message, PluginManifest const& manifest)
{
  return proxy_error_response(plugin_error(category, std::move(message), manifest));
}

bool error_is_canceled(ava::core::Error const& error)
{
  return error.code() == ava::core::ErrorCode::Canceled;
}

std::string_view proxy_capability_for_operation(std::string_view operation)
{
  if (operation == "file.read")
    return kPluginProxyReadCapability;
  if (operation == "file.search")
    return kPluginProxySearchCapability;
  if (operation == "session.status")
    return kPluginProxySessionCapability;
  return {};
}

}  // namespace

std::string_view plugin_dynamic_resource_kind_name(PluginDynamicResourceKind kind) noexcept
{
  switch (kind)
  {
    case PluginDynamicResourceKind::Prompt:
      return "prompt";
    case PluginDynamicResourceKind::Skill:
      return "skill";
  }
  return "resource";
}

std::string_view plugin_dynamic_resource_capability(PluginDynamicResourceKind kind) noexcept
{
  switch (kind)
  {
    case PluginDynamicResourceKind::Prompt:
      return "dynamic.prompts";
    case PluginDynamicResourceKind::Skill:
      return "dynamic.skills";
  }
  return "dynamic.resources";
}

bool is_valid_dynamic_resource_name(std::string_view name) noexcept
{
  if (name.empty() || name.size() > kPluginDynamicResourceNameMaxBytes)
    return false;
  return std::ranges::all_of(
      name, [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'; });
}

ava::core::VoidResult PluginProcess::initialize(CancelCallback cancel_requested)
{
  std::string const request = "{\"id\":\"ava_1\",\"type\":\"initialize\",\"api_version\":" + json_string(kPluginApiVersion) +
                              ",\"plugin_id\":" + json_string(manifest_.id) + ",\"workspace\":" + json_string(options_.workspace_dir.string()) + "}";
  if (auto written = write_record(request, startup_deadline_, options_.startup_timeout, "timed out writing plugin initialization", cancel_requested); !written)
    return std::unexpected(std::move(written.error()));

  auto record = read_record(startup_deadline_, options_.startup_timeout, "timed out waiting for plugin initialization",
                            "plugin process closed stdout before initialization", cancel_requested);
  if (!record)
    return std::unexpected(std::move(record.error()));
  auto initialized = parse_initialized_response(*record);
  if (!initialized)
  {
    auto error = protocol_error("plugin initialize response is malformed or unsupported", manifest_);
    error.with_context("response_bytes", std::to_string(record->size()));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }
  initialization_ = std::move(*initialized);
  return {};
}

ava::core::Result<PluginToolCallResult> PluginProcess::call_tool(std::string_view tool_name, std::string_view arguments_json, std::string_view call_id,
                                                                 CancelCallback cancel_requested, PluginProxyHandler proxy_handler)
{
  if (tool_name.empty())
    return std::unexpected(plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin tool name must not be empty", manifest_));
  if (!ava::core::json::is_valid_object(arguments_json))
  {
    auto error = plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin tool arguments must be a JSON object", manifest_);
    error.with_context("tool", std::string(tool_name));
    return std::unexpected(std::move(error));
  }
  if (is_canceled(cancel_requested))
  {
    auto error = canceled_error("plugin tool call canceled", manifest_);
    error.with_context("tool", std::string(tool_name));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::Canceled, std::move(error)));
  }

  auto const deadline = std::chrono::steady_clock::now() + options_.request_timeout;
  std::string const request_id = call_id.empty() ? "ava_" + std::to_string(next_request_id_++) : "ava_tool_" + std::string(call_id);
  std::string request = "{\"id\":" + json_string(request_id) + ",\"type\":\"tool.call\",\"tool\":" + json_string(tool_name) +
                        ",\"arguments\":" + std::string(arguments_json) + ",\"context\":{\"call_id\":" + json_string(call_id) +
                        ",\"workspace\":" + json_string(options_.workspace_dir.string()) + "}}";
  if (auto written = write_record(request, deadline, options_.request_timeout, "timed out writing plugin tool request", cancel_requested); !written)
    return std::unexpected(std::move(written.error()));

  while (true)
  {
    auto record = read_record(deadline, options_.request_timeout, "timed out waiting for plugin tool result",
                              "plugin process closed stdout before plugin tool result", cancel_requested);
    if (!record)
      return std::unexpected(std::move(record.error()));
    auto result = parse_tool_result_response(*record, request_id);
    if (result)
      return *result;
    auto proxy_handled = handle_proxy_record(*record, deadline, options_.request_timeout, proxy_handler, cancel_requested);
    if (!proxy_handled)
      return std::unexpected(std::move(proxy_handled.error()));
    if (*proxy_handled)
      continue;
    auto error = protocol_error("plugin tool result is malformed", manifest_);
    error.with_context("response_bytes", std::to_string(record->size()));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }
}

ava::core::Result<PluginCommandCallResult> PluginProcess::call_command(std::string_view command_name, std::string_view arguments_json, std::string_view call_id,
                                                                       CancelCallback cancel_requested, PluginProxyHandler proxy_handler,
                                                                       PluginUiHandler ui_handler)
{
  if (command_name.empty())
    return std::unexpected(plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin command name must not be empty", manifest_));
  if (!ava::core::json::is_valid_object(arguments_json))
  {
    auto error = plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin command arguments must be a JSON object", manifest_);
    error.with_context("command", std::string(command_name));
    return std::unexpected(std::move(error));
  }
  if (is_canceled(cancel_requested))
  {
    auto error = canceled_error("plugin command call canceled", manifest_);
    error.with_context("command", std::string(command_name));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::Canceled, std::move(error)));
  }

  auto const started_at = std::chrono::steady_clock::now();
  auto deadline = started_at + options_.request_timeout;
  auto timeout = options_.request_timeout;
  if (ui_handler)
  {
    if (ui_handler.deadline <= started_at || ui_handler.deadline - started_at > kPluginUiCommandDeadlineMax)
    {
      auto error = protocol_error("plugin UI command deadline is invalid or expired", manifest_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }
    deadline = ui_handler.deadline;
    timeout = std::max(std::chrono::milliseconds(1), std::chrono::duration_cast<std::chrono::milliseconds>(ui_handler.deadline - started_at));
  }

  std::string const request_id = call_id.empty() ? "ava_" + std::to_string(next_request_id_++) : "ava_command_" + std::string(call_id);
  std::string request = "{\"id\":" + json_string(request_id) + ",\"type\":\"command.call\",\"command\":" + json_string(command_name) +
                        ",\"arguments\":" + std::string(arguments_json) + ",\"context\":{\"call_id\":" + json_string(call_id) +
                        ",\"workspace\":" + json_string(options_.workspace_dir.string()) + "}}";
  if (auto written = write_record(request, deadline, timeout, "timed out writing plugin command request", cancel_requested); !written)
    return std::unexpected(std::move(written.error()));

  PluginUiProtocolState ui_state;
  while (true)
  {
    auto record = read_record(deadline, timeout, "timed out waiting for plugin command result", "plugin process closed stdout before plugin command result",
                              cancel_requested);
    if (!record)
      return std::unexpected(std::move(record.error()));
    auto result = parse_command_result_response(*record, request_id);
    if (result)
      return *result;

    auto const record_type = ava::core::json::string_field(*record, "type");
    if (record_type && is_plugin_ui_record_type(*record_type))
    {
      auto ui_request = parse_plugin_ui_request(*record, ui_state);
      if (!ui_request || !ui_handler || !plugin_has_capability(manifest_, ui_request ? plugin_ui_request_capability(*ui_request) : std::string_view{}))
      {
        auto error = protocol_error("plugin UI request is invalid or unauthorized", manifest_);
        return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
      }
      auto ui_cancel_requested = [this, cancel_requested, deadline] {
        return is_canceled(cancel_requested) || std::chrono::steady_clock::now() >= deadline || observe_settlement().has_value();
      };
      ava::core::Result<PluginUiAction> action = [&]() -> ava::core::Result<PluginUiAction> {
        try
        {
          return ui_handler.callback(*ui_request, deadline, ui_cancel_requested);
        }
        catch (...)
        {
          return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "plugin UI handler failed"));
        }
      }();
      if (std::chrono::steady_clock::now() >= deadline)
      {
        auto error = protocol_error("timed out handling plugin UI request", manifest_);
        return std::unexpected(fail_process(ava::process::TerminationReasonV1::DeadlineExpired, std::move(error)));
      }
      if (is_canceled(cancel_requested))
        return std::unexpected(fail_process(ava::process::TerminationReasonV1::Canceled, canceled_error("plugin command call canceled", manifest_)));
      if (!action)
      {
        auto error = protocol_error("plugin UI handler failed", manifest_);
        return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
      }
      if (auto valid = validate_plugin_ui_action(*ui_request, *action); !valid)
      {
        auto error = protocol_error("plugin UI handler returned an invalid action", manifest_);
        return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
      }
      auto response = serialize_plugin_ui_action(plugin_ui_request_id(*ui_request), *action);
      if (!response)
      {
        auto error = protocol_error("plugin UI action serialization failed", manifest_);
        return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
      }
      if (auto written = write_record(*response, deadline, timeout, "timed out writing plugin UI action", cancel_requested); !written)
        return std::unexpected(std::move(written.error()));
      continue;
    }
    if (record_type && record_type->starts_with("ui."))
    {
      auto error = protocol_error("plugin UI request is invalid or unauthorized", manifest_);
      return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
    }

    auto proxy_handled = handle_proxy_record(*record, deadline, timeout, proxy_handler, cancel_requested);
    if (!proxy_handled)
      return std::unexpected(std::move(proxy_handled.error()));
    if (*proxy_handled)
      continue;
    auto error = protocol_error("plugin command result is malformed", manifest_);
    error.with_context("response_bytes", std::to_string(record->size()));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }
}

ava::core::Result<PluginEventObserveResult> PluginProcess::observe_event(std::string_view event_name, std::string_view payload_json, std::string_view call_id,
                                                                         CancelCallback cancel_requested, PluginProxyHandler proxy_handler)
{
  if (event_name.empty())
    return std::unexpected(plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin event name must not be empty", manifest_));
  if (!ava::core::json::is_valid_object(payload_json))
  {
    auto error = plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin event payload must be a JSON object", manifest_);
    error.with_context("event", std::string(event_name));
    return std::unexpected(std::move(error));
  }
  if (is_canceled(cancel_requested))
  {
    auto error = canceled_error("plugin event observation canceled", manifest_);
    error.with_context("event", std::string(event_name));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::Canceled, std::move(error)));
  }

  auto const deadline = std::chrono::steady_clock::now() + options_.request_timeout;
  std::string const request_id = call_id.empty() ? "ava_" + std::to_string(next_request_id_++) : "ava_event_" + std::string(call_id);
  std::string request = "{\"id\":" + json_string(request_id) + ",\"type\":\"event.observe\",\"event\":" + json_string(event_name) +
                        ",\"payload\":" + std::string(payload_json) + ",\"context\":{\"call_id\":" + json_string(call_id) +
                        ",\"workspace\":" + json_string(options_.workspace_dir.string()) + "}}";
  if (auto written = write_record(request, deadline, options_.request_timeout, "timed out writing plugin event request", cancel_requested); !written)
    return std::unexpected(std::move(written.error()));

  while (true)
  {
    auto record = read_record(deadline, options_.request_timeout, "timed out waiting for plugin event response",
                              "plugin process closed stdout before plugin event response", cancel_requested);
    if (!record)
      return std::unexpected(std::move(record.error()));
    auto result = parse_event_observed_response(*record, request_id);
    if (result)
      return *result;
    auto proxy_handled = handle_proxy_record(*record, deadline, options_.request_timeout, proxy_handler, cancel_requested);
    if (!proxy_handled)
      return std::unexpected(std::move(proxy_handled.error()));
    if (*proxy_handled)
      continue;
    auto error = protocol_error("plugin event response is malformed", manifest_);
    error.with_context("response_bytes", std::to_string(record->size()));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }
}

ava::core::Result<PluginDynamicResourceListResult> PluginProcess::list_resources(PluginDynamicResourceKind kind, CancelCallback cancel_requested,
                                                                                 PluginProxyHandler proxy_handler)
{
  auto const kind_name = plugin_dynamic_resource_kind_name(kind);
  auto const capability = plugin_dynamic_resource_capability(kind);
  if (!plugin_has_capability(manifest_, capability))
  {
    auto error = plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin does not declare dynamic resource capability", manifest_);
    error.with_context("capability", std::string(capability));
    error.with_context("resource_kind", std::string(kind_name));
    return std::unexpected(std::move(error));
  }
  if (is_canceled(cancel_requested))
  {
    auto error = canceled_error("plugin resource list canceled", manifest_);
    error.with_context("resource_kind", std::string(kind_name));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::Canceled, std::move(error)));
  }

  auto const deadline = std::chrono::steady_clock::now() + options_.request_timeout;
  std::string const request_id = "ava_resource_" + std::to_string(next_request_id_++);
  std::string const request = "{\"id\":" + json_string(request_id) + ",\"type\":\"resource.list\",\"kind\":" + json_string(kind_name) +
                              ",\"context\":{\"workspace\":" + json_string(options_.workspace_dir.string()) + "}}";
  if (auto written = write_record(request, deadline, options_.request_timeout, "timed out writing plugin resource list request", cancel_requested); !written)
    return std::unexpected(std::move(written.error()));

  while (true)
  {
    auto record = read_record(deadline, options_.request_timeout, "timed out waiting for plugin resource list result",
                              "plugin process closed stdout before plugin resource list result", cancel_requested);
    if (!record)
      return std::unexpected(std::move(record.error()));
    auto result = parse_resource_list_result_response(*record, request_id, kind);
    if (result)
      return *result;
    auto proxy_handled = handle_proxy_record(*record, deadline, options_.request_timeout, proxy_handler, cancel_requested);
    if (!proxy_handled)
      return std::unexpected(std::move(proxy_handled.error()));
    if (*proxy_handled)
      continue;
    auto error = protocol_error("plugin dynamic resource list result is malformed", manifest_);
    error.with_context("response_bytes", std::to_string(record->size()));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }
}

ava::core::Result<PluginDynamicResourceReadResult> PluginProcess::read_resource(PluginDynamicResourceKind kind, std::string_view name,
                                                                                CancelCallback cancel_requested, PluginProxyHandler proxy_handler)
{
  auto const kind_name = plugin_dynamic_resource_kind_name(kind);
  auto const capability = plugin_dynamic_resource_capability(kind);
  if (!is_valid_dynamic_resource_name(name))
  {
    auto error = plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin dynamic resource name is invalid", manifest_);
    error.with_context("resource_kind", std::string(kind_name));
    error.with_context("resource", std::string(name));
    return std::unexpected(std::move(error));
  }
  if (!plugin_has_capability(manifest_, capability))
  {
    auto error = plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin does not declare dynamic resource capability", manifest_);
    error.with_context("capability", std::string(capability));
    error.with_context("resource_kind", std::string(kind_name));
    error.with_context("resource", std::string(name));
    return std::unexpected(std::move(error));
  }
  if (is_canceled(cancel_requested))
  {
    auto error = canceled_error("plugin resource read canceled", manifest_);
    error.with_context("resource_kind", std::string(kind_name));
    error.with_context("resource", std::string(name));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::Canceled, std::move(error)));
  }

  auto const deadline = std::chrono::steady_clock::now() + options_.request_timeout;
  std::string const request_id = "ava_resource_" + std::to_string(next_request_id_++);
  std::string const request = "{\"id\":" + json_string(request_id) + ",\"type\":\"resource.read\",\"kind\":" + json_string(kind_name) +
                              ",\"name\":" + json_string(name) + ",\"context\":{\"workspace\":" + json_string(options_.workspace_dir.string()) + "}}";
  if (auto written = write_record(request, deadline, options_.request_timeout, "timed out writing plugin resource read request", cancel_requested); !written)
    return std::unexpected(std::move(written.error()));

  while (true)
  {
    auto record = read_record(deadline, options_.request_timeout, "timed out waiting for plugin resource read result",
                              "plugin process closed stdout before plugin resource read result", cancel_requested);
    if (!record)
      return std::unexpected(std::move(record.error()));
    auto result = parse_resource_read_result_response(*record, request_id, kind, name);
    if (result)
      return *result;
    auto proxy_handled = handle_proxy_record(*record, deadline, options_.request_timeout, proxy_handler, cancel_requested);
    if (!proxy_handled)
      return std::unexpected(std::move(proxy_handled.error()));
    if (*proxy_handled)
      continue;
    auto error = protocol_error("plugin dynamic resource read result is malformed", manifest_);
    error.with_context("response_bytes", std::to_string(record->size()));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }
}

ava::core::Result<bool> PluginProcess::handle_proxy_record(std::string_view record, std::chrono::steady_clock::time_point deadline,
                                                           std::chrono::milliseconds timeout, PluginProxyHandler const& proxy_handler,
                                                           CancelCallback cancel_requested)
{
  auto const type = ava::core::json::string_field(record, "type");
  if (!type || *type != "proxy.request")
    return false;

  auto request = parse_proxy_request(record);
  if (!request)
  {
    auto id = ava::core::json::string_field(record, "id");
    if (id && !id->empty())
    {
      auto response = proxy_error_response(ava::core::ErrorCategory::InvalidArgument, "plugin proxy request is malformed", manifest_);
      if (auto written = write_proxy_response(*id, response, deadline, timeout, cancel_requested); !written)
        return std::unexpected(std::move(written.error()));
      return true;
    }
    auto error = protocol_error("plugin proxy request is malformed", manifest_);
    error.with_context("response_bytes", std::to_string(record.size()));
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::ProtocolFailure, std::move(error)));
  }

  auto response = dispatch_proxy_request(*request, proxy_handler, deadline, cancel_requested);
  if (!response)
  {
    auto const reason =
        std::chrono::steady_clock::now() >= deadline
            ? ava::process::TerminationReasonV1::DeadlineExpired
            : (error_is_canceled(response.error()) ? ava::process::TerminationReasonV1::Canceled : ava::process::TerminationReasonV1::ProtocolFailure);
    return std::unexpected(fail_process(reason, std::move(response.error())));
  }
  if (auto written = write_proxy_response(request->id, *response, deadline, timeout, cancel_requested); !written)
    return std::unexpected(std::move(written.error()));
  return true;
}

ava::core::Result<PluginProxyResponse> PluginProcess::dispatch_proxy_request(PluginProxyRequest const& request, PluginProxyHandler const& proxy_handler,
                                                                             std::chrono::steady_clock::time_point deadline, CancelCallback cancel_requested)
{
  if (is_canceled(cancel_requested))
    return std::unexpected(canceled_error("plugin request canceled", manifest_));
  auto proxy_cancel_requested = [cancel_requested, deadline] { return is_canceled(cancel_requested) || std::chrono::steady_clock::now() >= deadline; };

  auto const required_capability = proxy_capability_for_operation(request.operation);
  if (required_capability.empty())
    return proxy_error_response(plugin_error(ava::core::ErrorCategory::InvalidArgument, "plugin proxy operation is not supported", manifest_));
  if (!plugin_has_capability(manifest_, required_capability))
  {
    auto error = plugin_error(ava::core::ErrorCategory::PermissionDenied, "plugin manifest does not declare required proxy capability", manifest_);
    error.with_context("required_capability", std::string(required_capability));
    return proxy_error_response(error);
  }
  if (!proxy_handler)
    return proxy_error_response(plugin_error(ava::core::ErrorCategory::Tool, "plugin proxy handler is unavailable", manifest_));

  auto response = proxy_handler(request, proxy_cancel_requested);
  if (!response)
  {
    if (!is_canceled(cancel_requested) && std::chrono::steady_clock::now() >= deadline)
    {
      auto error = plugin_error(ava::core::ErrorCategory::Tool, "timed out handling plugin proxy request", manifest_);
      error.with_context("timeout_ms", std::to_string(options_.request_timeout.count()));
      return std::unexpected(std::move(error));
    }
    if (is_canceled(cancel_requested) || error_is_canceled(response.error()))
      return std::unexpected(std::move(response.error()));
    return proxy_error_response(response.error());
  }
  if (!is_canceled(cancel_requested) && std::chrono::steady_clock::now() >= deadline)
  {
    auto error = plugin_error(ava::core::ErrorCategory::Tool, "timed out handling plugin proxy request", manifest_);
    error.with_context("timeout_ms", std::to_string(options_.request_timeout.count()));
    return std::unexpected(std::move(error));
  }
  if (is_canceled(cancel_requested))
    return std::unexpected(canceled_error("plugin request canceled", manifest_));
  return *response;
}

ava::core::VoidResult PluginProcess::write_proxy_response(std::string_view request_id, PluginProxyResponse const& response,
                                                          std::chrono::steady_clock::time_point deadline, std::chrono::milliseconds timeout,
                                                          CancelCallback cancel_requested)
{
  std::size_t raw_bytes = 0;
  bool over_limit = false;
  auto add_raw_bytes = [&](std::string_view value) {
    if (value.size() > options_.max_record_bytes - std::min(raw_bytes, options_.max_record_bytes))
      over_limit = true;
    else
      raw_bytes += value.size();
  };
  add_raw_bytes(request_id);
  add_raw_bytes(response.content);
  add_raw_bytes(response.metadata_json);
  add_raw_bytes(response.error_category);
  add_raw_bytes(response.error_message);
  add_raw_bytes(response.error_details);
  if (over_limit)
  {
    auto error = protocol_error("plugin outbound proxy response exceeds size cap", manifest_);
    error.with_context("response_bytes", ">" + std::to_string(options_.max_record_bytes));
    error.with_context("max_bytes", std::to_string(options_.max_record_bytes));
    error.with_context("output_limit", "true");
    return std::unexpected(fail_process(ava::process::TerminationReasonV1::OutputLimit, std::move(error)));
  }
  auto record = proxy_response_json(request_id, response);
  return write_record(record, deadline, timeout, "timed out writing plugin proxy response", cancel_requested);
}

}  // namespace ava::plugin
