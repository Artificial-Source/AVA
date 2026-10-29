#include "sys.h"
#include "ava/observability/run_observer.h"
#include "ava/agent/tool_dispatch_common.h"
#include "ava/agent/tool_dispatcher.h"
#include "ava/agent/tool_registration.h"
#include "ava/agent/tool_result.h"
#include "ava/tools/lsp_tools.h"
#include "ava/tools/mutation_queue.h"
#include "ava/permissions/permission.h"

#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::agent {
namespace {

constexpr std::size_t kMaxProviderToolCallIdBytes = 256;

using namespace ava::agent::tool_dispatch;

ToolDispatchResult execute_registered_tool(RegisteredTool const& tool, ava::tools::ToolContext const& context, ToolDispatchServices const& services,
                                           ProviderToolCall const& call)
{
  try
  {
    return tool.executor(context, services, call);
  }
  catch (std::exception const& exception)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "tool executor threw an exception");
    error.with_context("tool", call.name).with_context("call_id", call.id).with_context("cause", exception.what());
    return tool_error_result(call, error);
  }
  catch (...)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Tool, "tool executor threw an exception");
    error.with_context("tool", call.name).with_context("call_id", call.id).with_context("cause", "unknown exception");
    return tool_error_result(call, error);
  }
}

}  // namespace

ToolDispatcher::ToolDispatcher(ava::tools::ToolContext context, ToolDispatchServices services, ToolVisibilityOptions visibility)
{
  // Context copies share the same queue to serialize mutations across dispatches.
  if (!context.mutation_queue)
    context.mutation_queue = std::make_shared<ava::tools::MutationQueue>();
  context_ = std::move(context);
  services_ = std::move(services);
  // Compatibility for ownership-protected callers; all other construction is fallible.
  auto registry = compose_tool_registry(context_, visibility);
  if (registry)
    registry_ = std::move(*registry);
}

ToolDispatcher::ToolDispatcher(ava::tools::ToolContext context, ToolDispatchServices services, ToolRegistry registry)
    : context_(std::move(context)), services_(std::move(services)), registry_(std::move(registry))
{
  // Context copies share the same queue to serialize mutations across dispatches.
  if (!context_.mutation_queue)
    context_.mutation_queue = std::make_shared<ava::tools::MutationQueue>();
}

ava::core::Result<ToolDispatcher> ToolDispatcher::create_strict(ava::tools::ToolContext context, ToolDispatchServices services,
                                                                ToolVisibilityOptions visibility)
{
  if (context.require_descriptor_secure_workspace && !context.secure_workspace)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "strict dispatcher requires an open secure workspace"));
  auto registry = compose_tool_registry(context, visibility);
  if (!registry)
    return std::unexpected(std::move(registry.error()));
  return ToolDispatcher(std::move(context), std::move(services), std::move(*registry));
}

ava::core::Result<ToolDispatchResult> ToolDispatcher::dispatch(ProviderToolCall const& call) const
{
  return dispatch_with_context(context_, services_, call);
}

ava::core::Result<ToolDispatchResult> ToolDispatcher::dispatch_with_context(ava::tools::ToolContext context, ToolDispatchServices const& services,
                                                                            ProviderToolCall const& call) const
{
  auto const arguments = call.arguments_json.empty() ? std::string("{}") : call.arguments_json;
  ProviderToolCall const normalized{.id = call.id, .name = call.name, .arguments_json = arguments};
  if (normalized.id.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "tool call id is required");
    error.with_context("tool", normalized.name);
    return std::unexpected(std::move(error));
  }
  if (normalized.id.size() > kMaxProviderToolCallIdBytes)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "tool call id is too long");
    error.with_context("tool", normalized.name);
    error.with_context("max_bytes", std::to_string(kMaxProviderToolCallIdBytes));
    return std::unexpected(std::move(error));
  }
  if (auto safe_id = reject_control_value(normalized.id, "id", "tool call id contains a forbidden control byte"); !safe_id)
  {
    safe_id.error().with_context("tool", normalized.name);
    return std::unexpected(std::move(safe_id.error()));
  }
  auto const* tool = registry_.find(normalized.name);
  // Resolve before observing so only registry-owned canonical tool metadata can
  // cross the trace boundary. Provider-supplied names and call IDs remain
  // content/product data and are never used as trace correlation.
  auto const trace_call_id = context.observation ? context.observation->next_id("tool") : std::string{};
  auto emit_start = [&](std::string_view canonical_name, bool resolved) {
    if (!context.observation)
      return;
    context.observation->emit(
        ava::observability::TraceEventType::ToolDispatchStart, context.trace_context, [&normalized, &trace_call_id, canonical_name, resolved](auto& event) {
          event.call_id = trace_call_id;
          event.phase = ava::observability::TracePhase::Tool;
          event.outcome = ava::observability::TraceOutcome::Started;
          event.fields = {{.key = "tool_name",
                           .value = resolved ? std::string(canonical_name) : "[omitted]",
                           .provenance = resolved ? ava::observability::FieldProvenance::PublicMetadata : ava::observability::FieldProvenance::Content},
                          {.key = "arguments_bytes", .value = std::to_string(normalized.arguments_json.size())}};
        });
  };
  auto emit_result = [&](ToolDispatchResult const& result) {
    if (!context.observation)
      return;
    context.observation->emit(ava::observability::TraceEventType::ToolDispatchResult, context.trace_context, [&result, &trace_call_id](auto& event) {
      event.call_id = trace_call_id;
      event.phase = ava::observability::TracePhase::Tool;
      event.outcome = result.success ? ava::observability::TraceOutcome::Success
                                     : (result.payload.status == ToolResultStatus::Canceled ? ava::observability::TraceOutcome::Canceled
                                                                                            : ava::observability::TraceOutcome::Error);
      event.fields = {{.key = "result_bytes", .value = std::to_string(result.result_text.size())}};
    });
  };
  if (tool == nullptr)
  {
    emit_start({}, false);
    auto result = with_tool_result_payload(simple_error_result(normalized, ava::core::ErrorCategory::Tool, "unknown tool"));
    emit_result(result);
    return result;
  }

  emit_start(tool->metadata.name, true);
  context.trace_call_id = trace_call_id;
  if (is_canceled(context))
  {
    auto result = with_tool_result_payload(tool_error_result(normalized, canceled_error(normalized)));
    emit_result(result);
    return result;
  }
  context.permission_request_ids = std::make_shared<std::vector<std::string>>();
  // Each dispatch owns an independent guidance capture. Parallel workers call
  // dispatch_with_context with their own ToolContext copy, so captures never share.
  context.permission_denial_guidance_capture = std::make_shared<ava::tools::PermissionDenialGuidanceCapture>();
  if (context.announce_execution_after_permission)
    context.execution_started = std::make_shared<std::atomic_bool>(false);
  if (context.lsp_diagnostics_provider)
    context.lsp_diagnostics_provider->set_permission_request_ids(context.permission_request_ids);
  auto result = execute_registered_tool(*tool, context, services, normalized);
  if (context.permission_request_ids && !context.permission_request_ids->empty())
  {
    result.payload.permission_request_ids = *context.permission_request_ids;
  }
  result = with_tool_result_payload(std::move(result));
  // Attach validated capture guidance only onto denial/error dispatch results.
  // Ordinary result_text and ToolResultPayload stay guidance-free for timeline,
  // events, RPC, ACP, and diagnostics.
  result.provider_user_guidance.clear();
  if (!result.success && context.permission_denial_guidance_capture)
  {
    if (auto validated = ava::permissions::validated_permission_user_guidance(context.permission_denial_guidance_capture->provider_user_guidance))
      result.provider_user_guidance = std::move(*validated);
  }
  emit_result(result);
  return result;
}

std::vector<ToolMetadata> ToolDispatcher::registered_tool_metadata() const
{
  return registry_.metadata();
}

std::vector<std::string> ToolDispatcher::registered_tool_schemas_json() const
{
  return registry_.tool_schemas_json(context_);
}

std::span<ToolMetadata const> ToolDispatcher::tool_metadata()
{
  return builtin_tool_metadata();
}

ava::core::Result<std::vector<RegisteredToolMetadata>> ToolDispatcher::tool_metadata(ava::tools::ToolContext const& context,
                                                                                     ToolVisibilityOptions const& visibility)
{
  auto registry = compose_tool_registry(context, visibility);
  if (!registry)
    return std::unexpected(std::move(registry.error()));
  std::vector<RegisteredToolMetadata> metadata;
  metadata.reserve(registry->entries().size());
  for (auto const& tool : registry->entries())
    metadata.push_back(tool.metadata);
  return metadata;
}

ava::core::Result<std::vector<std::string>> ToolDispatcher::compose_tool_schemas_json(ava::tools::ToolContext const& context,
                                                                                      ToolVisibilityOptions const& visibility)
{
  auto registry = compose_tool_registry(context, visibility);
  if (!registry)
    return std::unexpected(std::move(registry.error()));
  return registry->tool_schemas_json(context);
}

std::vector<std::string> ToolDispatcher::tool_schemas_json()
{
  return tool_schemas_json(ava::tools::ToolContext{}, ToolVisibilityOptions{});
}

std::vector<std::string> ToolDispatcher::tool_schemas_json(ava::tools::ToolContext const& context, ToolVisibilityOptions const& visibility)
{
  // Compatibility for ownership-protected callers; all other schema composition is fallible.
  auto schemas = compose_tool_schemas_json(context, visibility);
  return schemas ? std::move(*schemas) : std::vector<std::string>{};
}

}  // namespace ava::agent
