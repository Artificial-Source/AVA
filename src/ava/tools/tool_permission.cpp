#include "sys.h"
#include "ava/tools/file_tools_internal.h"
#include "ava/tools/secure_workspace.h"
#include "ava/tools/tool_permission.h"
#include "ava/tools/tool_permission_internal.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/mode.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <string_view>
#include <utility>

namespace ava::tools {
namespace {

std::string safe_command_display(std::optional<ava::permissions::CommandPermissionMetadata> const& command_metadata)
{
  if (command_metadata && !command_metadata->recipe_display.empty())
    return command_metadata->recipe_display;
  return "<redacted one-shot command>";
}

void capture_permission_denial_guidance(ToolContext const& context, std::string_view user_guidance)
{
  if (!context.permission_denial_guidance_capture)
    return;
  // Revalidate at the backend trust boundary. Direct app command contexts leave
  // the capture null and discard guidance because no model continuation exists.
  if (auto validated = ava::permissions::validated_permission_user_guidance(user_guidance))
    context.permission_denial_guidance_capture->provider_user_guidance = std::move(*validated);
  else
    context.permission_denial_guidance_capture->provider_user_guidance.clear();
}

ava::core::Error permission_denied_error(std::string_view error_message, ava::permissions::Operation operation,
                                         ava::permissions::PermissionDecision const& decision, std::filesystem::path const& target_path,
                                         std::string_view command, std::optional<ava::permissions::CommandPermissionMetadata> const& command_metadata,
                                         std::string_view resolution_context, std::string_view resolution_reason = {},
                                         std::string_view permission_request_id = {})
{
  bool const run_command = operation == ava::permissions::Operation::RunCommand;
  auto error = ava::core::Error(ava::core::ErrorCategory::PermissionDenied, std::string(error_message));
  error.with_context("action", ava::permissions::to_string(decision.action));
  error.with_context("reason", run_command ? "command permission denied" : decision.reason);
  error.with_context("risk", ava::permissions::to_string(decision.risk));
  if (!permission_request_id.empty())
  {
    error.with_context("request_id", std::string(permission_request_id));
  }
  if (run_command)
  {
    error.with_context("command", safe_command_display(command_metadata));
  }
  else if (!command.empty())
  {
    error.with_context("command", std::string(command));
  }
  else
  {
    error.with_context("path", target_path.string());
  }
  // Guidance never enters Error context/format: public tool-result details,
  // diagnostics, audits, and events consume this sanitized Error only.
  if (decision.action == ava::permissions::PermissionAction::Ask)
  {
    error.with_context("resolution", std::string(resolution_context));
    if (!run_command && !resolution_reason.empty())
      error.with_context("resolution_reason", std::string(resolution_reason));
    if (resolution_context == "no_resolver")
      error.with_context("headless_hint", "permission ask failed closed because no interactive or RPC resolver was available");
  }
  if (!permission_request_id.empty())
  {
    auto const id = std::string(permission_request_id);
    error.with_context("inspect", "/permissions audit show " + id);
    error.with_context("diagnose", "/permissions diagnose " + id);
  }
  return error;
}

ava::core::VoidResult announce_execution_start_impl(ToolContext const& context)
{
  if (!context.announce_execution_after_permission || !context.progress_sink || !context.execution_started)
    return {};
  if (context.execution_started->exchange(true, std::memory_order_acq_rel))
    return {};
  return context.progress_sink(ToolProgressEvent{
      .text = "authorized; execution starting", .call_id = context.current_call_id, .tool_name = context.current_tool_name, .status = "in_progress"});
}

}  // namespace

namespace tool_permission_internal {

std::string effective_tool_name(ToolContext const& context, ava::permissions::Operation operation, std::string_view tool_name)
{
  if (!tool_name.empty())
    return std::string(tool_name);
  if (!context.permission_tool_name.empty())
    return context.permission_tool_name;
  return ava::permissions::to_string(operation);
}

ava::core::VoidResult record_permission_audit(ToolContext const& context, PermissionAuditEvent const& event)
{
  if (context.permission_request_ids && !event.permission_request_id.empty() &&
      std::ranges::find(*context.permission_request_ids, event.permission_request_id) == context.permission_request_ids->end())
  {
    context.permission_request_ids->push_back(event.permission_request_id);
  }
  if (!context.permission_audit_sink)
    return {};
  return context.permission_audit_sink(event);
}

PermissionAuditEvent audit_event(ToolContext const& context, std::string permission_request_id, ava::permissions::Operation operation, std::string tool_name,
                                 ava::permissions::PermissionDecision const& decision, std::filesystem::path const& target_path, std::string_view command,
                                 std::optional<ava::permissions::CommandPermissionMetadata> command_metadata)
{
  bool const run_command = operation == ava::permissions::Operation::RunCommand;
  return PermissionAuditEvent{
      .permission_request_id = std::move(permission_request_id),
      .operation = operation,
      .mode = context.mode,
      .tool_name = std::move(tool_name),
      .action = decision.action,
      .reason = run_command ? std::string("command permission decision") : decision.reason,
      .risk = decision.risk,
      .target_path = target_path,
      .command = run_command ? safe_command_display(command_metadata)
                             : (context.redact_permission_audit_arguments && !command.empty() ? std::string("[redacted]") : std::string(command)),
      .resolution = "",
      .resolution_source = "policy",
      .resolution_reason = "",
      .actor = context.permission_actor.empty() ? std::string("agent") : context.permission_actor,
      .rule_id = "",
      .command_arguments_redacted = context.redact_permission_audit_arguments,
      .command_metadata = std::move(command_metadata)};
}

}  // namespace tool_permission_internal

namespace {

using tool_permission_internal::audit_event;
using tool_permission_internal::effective_tool_name;
using tool_permission_internal::record_permission_audit;

ava::core::VoidResult ensure_permission_impl(ToolContext const& context, ava::permissions::Operation operation, std::filesystem::path const& target_path,
                                             std::string_view command, std::string_view tool_name, std::string_view error_message,
                                             std::string_view diff_preview, bool diff_truncated,
                                             std::optional<ava::permissions::CommandPermissionMetadata> command_metadata, bool omit_policy_allow_audit)
{
  auto permission_target = target_path;
  if (context.secure_workspace && (operation == ava::permissions::Operation::ReadFile || operation == ava::permissions::Operation::EditFile ||
                                   operation == ava::permissions::Operation::SearchFiles))
  {
    auto const remote_read = context.exact_file_access && context.exact_file_access->supports_read_text_file();
    auto const mode = operation == ava::permissions::Operation::EditFile || (operation == ava::permissions::Operation::ReadFile && remote_read)
                          ? SecureWorkspaceResolveMode::AllowMissing
                          : SecureWorkspaceResolveMode::Existing;
    auto resolved = context.secure_workspace->resolve(target_path, mode);
    if (!resolved)
      return std::unexpected(std::move(resolved.error()));
    permission_target = std::move(resolved->absolute);
  }

  if (operation == ava::permissions::Operation::EditFile)
  {
    if (auto protected_file = file_tools_internal::reject_permission_rules_file_mutation(context, permission_target); !protected_file)
    {
      return protected_file;
    }
  }

  auto const request_tool_name = effective_tool_name(context, operation, tool_name);
  auto const permission_request_id = ava::core::make_id("permreq");
  auto decision = ava::permissions::decide(ava::permissions::PermissionRequest{
      .operation = operation,
      .mode = context.mode,
      .workspace_dir = context.workspace_dir,
      .target_path = permission_target,
      .command = std::string(command),
      .command_metadata = command_metadata,
  });
  bool const backend_policy_allowed = decision.action == ava::permissions::PermissionAction::Allow;
  if (context.require_explicit_file_permissions && decision.action == ava::permissions::PermissionAction::Allow &&
      (operation == ava::permissions::Operation::ReadFile || operation == ava::permissions::Operation::EditFile))
  {
    decision.action = ava::permissions::PermissionAction::Ask;
    decision.reason = "The frontend requires explicit approval for model-initiated file access after backend safety checks";
    decision.risk = operation == ava::permissions::Operation::EditFile ? ava::permissions::PermissionRisk::Medium : ava::permissions::PermissionRisk::Low;
  }

  std::string preflight_source;
  std::string preflight_rule_id;
  std::string preflight_reason;
  auto const needs_auto_allow_deny_preflight = backend_policy_allowed && context.auto_allow_deny_preflight;
  if (needs_auto_allow_deny_preflight)
  {
    auto preflight = context.auto_allow_deny_preflight(ava::permissions::PermissionPrompt{
        .permission_request_id = permission_request_id,
        .tool_call_id = context.current_call_id,
        .operation = operation,
        .mode = context.mode,
        .workspace_dir = context.workspace_dir,
        .target_path = permission_target,
        .command = std::string(command),
        .tool_name = request_tool_name,
        .reason = decision.reason,
        .risk = decision.risk,
        .command_metadata = command_metadata,
    });
    if (!preflight || (*preflight != ava::permissions::PermissionResolution::Allow && *preflight != ava::permissions::PermissionResolution::AllowSessionGrant))
    {
      decision.action = ava::permissions::PermissionAction::Deny;
      decision.risk = ava::permissions::PermissionRisk::Critical;
      decision.reason = preflight ? (preflight->reason.empty() ? std::string("operation denied by persistent policy") : preflight->reason)
                                  : "persistent deny preflight failed: " + preflight.error().format();
      preflight_source = preflight && !preflight->resolution_source.empty() ? preflight->resolution_source : "persistent_rule_error";
      preflight_rule_id = preflight ? preflight->rule_id : std::string{};
      preflight_reason = decision.reason;
    }
  }

  auto policy_event = audit_event(context, permission_request_id, operation, request_tool_name, decision, permission_target, command, command_metadata);
  if (!preflight_source.empty())
  {
    policy_event.resolution_source = std::move(preflight_source);
    if (operation != ava::permissions::Operation::RunCommand)
      policy_event.resolution_reason = std::move(preflight_reason);
    policy_event.rule_id = std::move(preflight_rule_id);
  }
  if (decision.action == ava::permissions::PermissionAction::Allow || decision.action == ava::permissions::PermissionAction::Deny)
  {
    policy_event.resolution = ava::permissions::to_string(decision.action);
  }
  if (!(omit_policy_allow_audit && decision.action == ava::permissions::PermissionAction::Allow))
  {
    if (auto audited = record_permission_audit(context, policy_event); !audited)
    {
      return std::unexpected(std::move(audited.error()));
    }
  }
  if (decision.action == ava::permissions::PermissionAction::Allow)
  {
    return {};
  }
  if (decision.action == ava::permissions::PermissionAction::Deny)
  {
    return std::unexpected(
        permission_denied_error(error_message, operation, decision, permission_target, command, command_metadata, "policy", "", permission_request_id));
  }

  if (!context.permission_resolver)
  {
    auto outcome_event = policy_event;
    outcome_event.resolution = "deny";
    outcome_event.resolution_source = "no_resolver";
    if (auto audited = record_permission_audit(context, outcome_event); !audited)
    {
      return std::unexpected(std::move(audited.error()));
    }
    return std::unexpected(
        permission_denied_error(error_message, operation, decision, permission_target, command, command_metadata, "no_resolver", "", permission_request_id));
  }

  auto resolution = context.permission_resolver(ava::permissions::PermissionPrompt{
      .permission_request_id = permission_request_id,
      .tool_call_id = context.current_call_id,
      .operation = operation,
      .mode = context.mode,
      .workspace_dir = context.workspace_dir,
      .target_path = permission_target,
      .command = std::string(command),
      .tool_name = request_tool_name,
      .reason = decision.reason,
      .risk = decision.risk,
      .diff_preview = std::string(diff_preview),
      .diff_truncated = diff_truncated,
      .command_metadata = command_metadata,
  });
  if (resolution && *resolution == ava::permissions::PermissionResolution::AllowSessionGrant && command_metadata &&
      !ava::permissions::command_permission_allows_reusable_grant(*command_metadata))
  {
    ava::permissions::PermissionResolutionDecision denied(ava::permissions::PermissionResolution::Deny,
                                                          "the sealed command backend permits only one-shot approval");
    denied.resolution_source = "backend_scope";
    resolution = std::move(denied);
  }

  auto outcome_event = policy_event;
  outcome_event.resolution_source =
      resolution && !resolution->resolution_source.empty()
          ? resolution->resolution_source
          : (resolution && *resolution == ava::permissions::PermissionResolution::AllowSessionGrant ? "session_grant" : "resolver");
  if (!resolution)
    outcome_event.resolution_source = "resolver_failed";
  outcome_event.resolution = resolution ? ava::permissions::to_string(*resolution) : "deny";
  if (operation != ava::permissions::Operation::RunCommand)
  {
    if (resolution)
      outcome_event.resolution_reason = resolution->reason;
    else
      outcome_event.resolution_reason = resolution.error().format();
  }
  if (resolution)
    outcome_event.rule_id = resolution->rule_id;
  if (auto audited = record_permission_audit(context, outcome_event); !audited)
  {
    return std::unexpected(std::move(audited.error()));
  }
  if (resolution && (*resolution == ava::permissions::PermissionResolution::Allow || *resolution == ava::permissions::PermissionResolution::AllowSessionGrant))
  {
    return {};
  }
  if (resolution && *resolution == ava::permissions::PermissionResolution::Cancel)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "agent loop canceled", ava::core::ErrorCode::Canceled);
    error.with_context("permission_request_id", permission_request_id);
    if (operation != ava::permissions::Operation::RunCommand)
      error.with_context("resolution_reason", resolution->reason);
    return std::unexpected(std::move(error));
  }

  auto const resolution_context = resolution ? ava::permissions::to_string(*resolution) : std::string("resolver_failed");
  auto const resolution_reason = resolution ? resolution->reason : resolution.error().format();
  capture_permission_denial_guidance(context, resolution ? std::string_view(resolution->user_guidance) : std::string_view{});
  return std::unexpected(permission_denied_error(error_message, operation, decision, permission_target, command, command_metadata, resolution_context,
                                                 resolution_reason, permission_request_id));
}

}  // namespace

ava::core::VoidResult announce_tool_execution_start(ToolContext const& context)
{
  return announce_execution_start_impl(context);
}

ava::core::VoidResult ensure_permission(ToolContext const& context, ava::permissions::Operation operation, std::filesystem::path const& target_path,
                                        std::string_view command, std::string_view tool_name, std::string_view error_message, std::string_view diff_preview,
                                        bool diff_truncated, std::optional<ava::permissions::CommandPermissionMetadata> command_metadata)
{
  return ensure_permission_impl(context, operation, target_path, command, tool_name, error_message, diff_preview, diff_truncated, std::move(command_metadata),
                                false);
}

ava::core::VoidResult ensure_filtered_read_permission(ToolContext const& context, std::filesystem::path const& target_path, std::string_view tool_name,
                                                      std::string_view error_message)
{
  return ensure_permission_impl(context, ava::permissions::Operation::ReadFile, target_path, "", tool_name, error_message, "", false, std::nullopt, true);
}

ava::core::VoidResult ensure_command_permission(ToolContext const& context, std::string_view command, ava::command::CommandPreparation const& preparation,
                                                bool unverified_delegated_executor, std::string_view tool_name, std::string_view error_message)
{
  auto metadata = ava::permissions::command_permission_metadata(preparation.plan(), unverified_delegated_executor);
  return ensure_permission(context, ava::permissions::Operation::RunCommand, preparation.plan().cwd(), command, tool_name, error_message, {}, false,
                           std::move(metadata));
}

ava::core::VoidResult ensure_command_permission(ToolContext const& context, std::string_view command, ava::command::CommandPreparation const& preparation,
                                                ava::permissions::CommandContainmentInfo const& containment, bool unverified_delegated_executor,
                                                std::string_view tool_name, std::string_view error_message)
{
  auto metadata = ava::permissions::command_permission_metadata(preparation.plan(), containment, unverified_delegated_executor);
  return ensure_permission(context, ava::permissions::Operation::RunCommand, preparation.plan().cwd(), command, tool_name, error_message, {}, false,
                           std::move(metadata));
}

std::string permission_audit_data_json(PermissionAuditEvent const& event)
{
  std::string data = "{";
  if (!event.permission_request_id.empty())
  {
    data += "\"permission_request_id\":\"" + ava::core::json::escape(event.permission_request_id) + "\",";
  }
  data += "\"operation\":\"" + ava::core::json::escape(ava::permissions::to_string(event.operation)) + "\",\"mode\":\"" +
          ava::core::json::escape(ava::core::to_string(event.mode)) + "\",\"tool_name\":\"" + ava::core::json::escape(event.tool_name) + "\",\"action\":\"" +
          ava::core::json::escape(ava::permissions::to_string(event.action)) + "\",\"reason\":\"" + ava::core::json::escape(event.reason) + "\",\"risk\":\"" +
          ava::core::json::escape(ava::permissions::to_string(event.risk)) + "\"";
  if (event.operation != ava::permissions::Operation::RunCommand && event.operation != ava::permissions::Operation::NetworkFetch && !event.target_path.empty())
  {
    data += ",\"target_path\":\"" + ava::core::json::escape(event.target_path.string()) + "\"";
  }
  if (event.operation == ava::permissions::Operation::RunCommand || event.command_arguments_redacted || !event.command.empty())
  {
    std::string command;
    if (event.operation == ava::permissions::Operation::RunCommand)
    {
      command = event.command_arguments_redacted ? "[redacted]" : safe_command_display(event.command_metadata);
    }
    else
    {
      command = event.command_arguments_redacted ? "[redacted]" : event.command;
    }
    data += ",\"command\":\"" + ava::core::json::escape(command) + "\"";
  }
  if (!event.resolution.empty())
  {
    data += ",\"resolution\":\"" + ava::core::json::escape(event.resolution) + "\"";
  }
  if (!event.resolution_source.empty())
  {
    data += ",\"resolution_source\":\"" + ava::core::json::escape(event.resolution_source) + "\"";
  }
  if (event.operation != ava::permissions::Operation::RunCommand && !event.resolution_reason.empty())
  {
    data += ",\"resolution_reason\":\"" + ava::core::json::escape(event.resolution_reason) + "\"";
  }
  if (!event.actor.empty())
  {
    data += ",\"actor\":\"" + ava::core::json::escape(event.actor) + "\"";
  }
  if (!event.rule_id.empty())
  {
    data += ",\"rule_id\":\"" + ava::core::json::escape(event.rule_id) + "\"";
  }
  if (event.command_metadata)
  {
    auto const& metadata = *event.command_metadata;
    // This is local audit metadata, not a process environment dump. The
    // profile identifier and digest bind the contract without serializing any
    // HOME/XDG/TMP/PATH or other environment values.
    data += ",\"command_metadata\":{\"level\":\"" + ava::core::json::escape(ava::command::to_string(metadata.level)) + "\",\"family\":\"" +
            ava::core::json::escape(ava::command::to_string(metadata.family)) + "\",\"fingerprint\":\"" + ava::core::json::escape(metadata.fingerprint) +
            "\",\"execution_domain\":\"" + ava::core::json::escape(ava::command::to_string(metadata.execution_domain)) + "\",\"resolved_executable\":\"" +
            ava::core::json::escape(metadata.resolved_executable.string()) + "\",\"origin\":\"" +
            ava::core::json::escape(ava::command::to_string(metadata.executable_origin)) + "\",\"cwd\":\"" + ava::core::json::escape(metadata.cwd.string()) +
            "\",\"containment_available\":" + (metadata.containment_available ? "true" : "false") + ",\"containment_status\":\"" +
            ava::core::json::escape(ava::permissions::to_string(metadata.containment_status)) + "\",\"backend_maximum_scope\":\"" +
            ava::core::json::escape(ava::command::to_string(metadata.backend_maximum_scope)) + "\",\"recipe_payload_version\":\"" +
            ava::core::json::escape(metadata.recipe_payload_version) + "\",\"global_recipe_key\":\"" + ava::core::json::escape(metadata.global_recipe_key) +
            "\",\"workspace_recipe_key\":\"" + ava::core::json::escape(metadata.workspace_recipe_key) + "\"";
    if (!event.command_arguments_redacted)
    {
      data += ",\"recipe_display\":\"" + ava::core::json::escape(metadata.recipe_display) + "\"";
    }
    data += ",\"effective_allowed_scopes\":[";
    for (std::size_t index = 0; index < metadata.effective_allowed_scopes.size(); ++index)
    {
      if (index > 0)
        data += ',';
      data += "\"" + ava::core::json::escape(ava::command::to_string(metadata.effective_allowed_scopes[index])) + "\"";
    }
    data += "],\"containment_profile_id\":\"" + ava::core::json::escape(metadata.containment_profile_id) +
            "\",\"containment_network_allowed\":" + (metadata.containment_network_allowed ? "true" : "false") + ",\"environment_profile_id\":\"" +
            ava::core::json::escape(metadata.environment_profile_id) + "\",\"environment_digest\":\"" + ava::core::json::escape(metadata.environment_digest) +
            "\",\"executor_identity_verified\":" + (metadata.executor_identity_verified ? "true" : "false") + '}';
  }
  data += '}';
  return data;
}

}  // namespace ava::tools
