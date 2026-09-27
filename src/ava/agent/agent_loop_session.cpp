#include "sys.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/agent/tool_result.h"
#include "ava/agent/usage_accounting.h"
#include "ava/tools/tool_permission.h"
#include "ava/session/assistant_output.h"
#include "ava/permissions/permission.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <type_traits>
#include <utility>
#include <variant>

namespace ava::agent {
namespace {

ava::core::VoidResult append_entry(SessionAppendSink const& sink, ava::session::EntryType type, std::string const& id, std::string data_json)
{
  if (!sink)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "active session append route is required"));
  return sink(
      ava::session::SessionEntry{.id = id, .parent_id = "", .type = type, .timestamp = ava::session::now_timestamp(), .data_json = std::move(data_json)});
}

std::string reasoning_block_data_json(ParsedReasoningBlock const& block, std::string_view provider_id, std::string_view model_id)
{
  std::string json = "{\"provider\":\"" + ava::core::json::escape(provider_id) + "\",\"model\":\"" + ava::core::json::escape(model_id) + "\"";
  if (!block.format.empty())
    json += ",\"format\":\"" + ava::core::json::escape(block.format) + "\"";
  if (!block.text.empty())
    json += ",\"text\":\"" + ava::core::json::escape(block.text) + "\"";
  if (!block.signature.empty())
    json += ",\"signature\":\"" + ava::core::json::escape(block.signature) + "\"";
  if (!block.redacted_data.empty())
  {
    json += ",\"redacted_data\":\"" + ava::core::json::escape(block.redacted_data) + "\"";
  }
  if (!block.native_item_json.empty())
  {
    json += ",\"native_item_json\":\"" + ava::core::json::escape(block.native_item_json) + "\"";
  }
  json += ",\"redacted\":";
  json += block.redacted ? "true" : "false";
  json += '}';
  return json;
}

std::string image_attachments_json(std::vector<ava::session::ImageAttachmentRef> const& attachments)
{
  std::string json;
  json += '[';
  for (std::size_t index = 0; index < attachments.size(); ++index)
  {
    auto const& attachment = attachments[index];
    if (index > 0)
      json += ',';
    json += "{\"id\":\"" + ava::core::json::escape(attachment.id) + "\",\"type\":\"image\",\"mime_type\":\"" + ava::core::json::escape(attachment.mime_type) +
            "\",\"byte_size\":" + std::to_string(attachment.byte_size) + ",\"sha256\":\"" + ava::core::json::escape(attachment.sha256) +
            "\",\"storage_path\":\"" + ava::core::json::escape(attachment.storage_path) + "\"}";
  }
  json += ']';
  return json;
}

std::string user_message_data_json(std::string const& text, std::vector<ava::session::ImageAttachmentRef> const& attachments,
                                   std::optional<ava::session::SyntheticDeliveryProvenance> const& provenance = std::nullopt)
{
  std::string json = "{\"text\":\"" + ava::core::json::escape(text) + "\"";
  if (!attachments.empty())
    json += ",\"attachments\":" + image_attachments_json(attachments);
  if (provenance)
  {
    json += ",\"provenance\":{\"source\":\"" + ava::core::json::escape(provenance->source) + "\",\"delivery_id\":\"" +
            ava::core::json::escape(provenance->delivery_id) + "\",\"prompt_fingerprint\":\"" + ava::core::json::escape(provenance->prompt_fingerprint) + "\"}";
  }
  json += '}';
  return json;
}

ava::session::AssistantOutputTextPhase persisted_text_phase(ava::provider::AssistantPhase phase)
{
  switch (phase)
  {
    case ava::provider::AssistantPhase::Commentary:
      return ava::session::AssistantOutputTextPhase::Commentary;
    case ava::provider::AssistantPhase::FinalAnswer:
      return ava::session::AssistantOutputTextPhase::FinalAnswer;
    case ava::provider::AssistantPhase::Unknown:
      return ava::session::AssistantOutputTextPhase::Unknown;
  }
  return ava::session::AssistantOutputTextPhase::Unknown;
}

bool has_reasoning_payload(ParsedReasoningBlock const& reasoning)
{
  return !reasoning.text.empty() || !reasoning.signature.empty() || !reasoning.redacted_data.empty() || !reasoning.native_item_json.empty();
}

ava::core::Result<PersistedAssistantTurn> append_assistant_turn_impl(SessionAppendBatchSink const& sink, ParsedAssistantTurn const& turn,
                                                                     std::string_view provider_id, std::string_view model_id,
                                                                     ava::provider::TokenUsage const& usage, std::optional<long double> const& cost_usd,
                                                                     std::optional<std::string_view> api_family,
                                                                     std::optional<std::string_view> reasoning_format)
{
  if (!sink)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "assistant turn persistence requires a batch append route"));
  }

  std::string const assistant_turn_id = ava::core::make_id("assistant_turn");
  std::vector<ava::session::SessionEntry> entries;
  entries.reserve(turn.ordered_items.size() + 1);
  PersistedAssistantTurn persisted;

  for (auto const& ordered : turn.ordered_items)
  {
    auto const item = std::visit(
        [&](auto const& source) -> std::optional<ava::session::AssistantOutputItem> {
          using Item = std::decay_t<decltype(source)>;
          if constexpr (std::same_as<Item, AssistantTextItem>)
          {
            return ava::session::AssistantOutputItem{
                .assistant_turn_id = assistant_turn_id,
                .sequence = entries.size(),
                .kind = ava::session::AssistantOutputItemKind::Text,
                .provider_item_id = source.metadata.provider_item_id.empty() ? std::nullopt : std::optional<std::string>(source.metadata.provider_item_id),
                .provider_output_index = source.metadata.provider_output_index,
                .payload = ava::session::AssistantOutputText{.text = source.text, .assistant_phase = persisted_text_phase(source.metadata.phase)}};
          }
          else if constexpr (std::same_as<Item, AssistantReasoningItem>)
          {
            if (!has_reasoning_payload(source.reasoning))
              return std::nullopt;
            return ava::session::AssistantOutputItem{
                .assistant_turn_id = assistant_turn_id,
                .sequence = entries.size(),
                .kind = ava::session::AssistantOutputItemKind::Reasoning,
                .provider_item_id = source.metadata.provider_item_id.empty() ? std::nullopt : std::optional<std::string>(source.metadata.provider_item_id),
                .provider_output_index = source.metadata.provider_output_index,
                .payload = ava::session::AssistantOutputReasoning{
                    .text = source.reasoning.text,
                    .format = source.reasoning.format,
                    .redacted = source.reasoning.redacted,
                    .signature = source.reasoning.signature.empty() ? std::nullopt : std::optional<std::string>(source.reasoning.signature),
                    .redacted_data = source.reasoning.redacted_data.empty() ? std::nullopt : std::optional<std::string>(source.reasoning.redacted_data),
                    .native_item_json =
                        source.reasoning.native_item_json.empty() ? std::nullopt : std::optional<std::string>(source.reasoning.native_item_json)}};
          }
          else
          {
            return ava::session::AssistantOutputItem{
                .assistant_turn_id = assistant_turn_id,
                .sequence = entries.size(),
                .kind = ava::session::AssistantOutputItemKind::FunctionCall,
                .provider_item_id = source.metadata.provider_item_id.empty() ? std::nullopt : std::optional<std::string>(source.metadata.provider_item_id),
                .provider_output_index = source.metadata.provider_output_index,
                .payload = ava::session::AssistantOutputFunctionCall{
                    .call_id = source.tool_call.id, .name = source.tool_call.name, .arguments_json = source.tool_call.arguments_json}};
          }
        },
        ordered.item);
    if (!item)
      continue;

    auto data = ava::session::serialize_assistant_output_item_data_json(*item);
    if (!data)
    {
      auto error = std::move(data.error());
      error.with_context("assistant_turn_id", assistant_turn_id).with_context("sequence", std::to_string(item->sequence));
      return std::unexpected(std::move(error));
    }
    auto const entry_id = ava::core::make_id("entry");
    if (auto const* function = std::get_if<ava::session::AssistantOutputFunctionCall>(&item->payload))
    {
      if (!persisted.function_output_entry_ids_by_call_id.emplace(function->call_id, entry_id).second)
      {
        auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "assistant turn contains duplicate function call ids");
        error.with_context("assistant_turn_id", assistant_turn_id).with_context("call_id", function->call_id);
        return std::unexpected(std::move(error));
      }
    }
    entries.push_back(ava::session::SessionEntry{.id = entry_id,
                                                 .parent_id = "",
                                                 .type = ava::session::EntryType::AssistantOutputItem,
                                                 .timestamp = ava::session::now_timestamp(),
                                                 .data_json = std::move(*data)});
  }

  auto commit_data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{
      .assistant_turn_id = assistant_turn_id,
      .item_count = entries.size(),
      .provider = std::string(provider_id),
      .model = std::string(model_id),
      .api_family = api_family && !api_family->empty() ? std::optional<std::string>(*api_family) : std::nullopt,
      .reasoning_format = reasoning_format && !reasoning_format->empty() ? std::optional<std::string>(*reasoning_format) : std::nullopt,
      .finish_reason = std::string(ava::provider::to_string(*turn.finish_reason)),
      .usage_json = usage_json(usage, cost_usd)});
  if (!commit_data)
    return std::unexpected(std::move(commit_data.error()));
  persisted.committed_turn_id = ava::core::make_id("entry");
  entries.push_back(ava::session::SessionEntry{.id = persisted.committed_turn_id,
                                               .parent_id = "",
                                               .type = ava::session::EntryType::AssistantTurnCommit,
                                               .timestamp = ava::session::now_timestamp(),
                                               .data_json = std::move(*commit_data)});

  if (auto appended = sink(std::move(entries)); !appended)
    return std::unexpected(std::move(appended.error()));
  return persisted;
}

std::string tool_result_data_json(ToolDispatchResult const& result, std::optional<std::string_view> assistant_output_entry_id)
{
  auto const materialized = with_tool_result_payload(result);
  std::string json = "{\"call_id\":\"" + ava::core::json::escape(materialized.call_id) + "\",\"name\":\"" + ava::core::json::escape(materialized.name) +
                     "\",\"success\":" + (materialized.success ? std::string("true") : std::string("false")) + ",\"status\":\"" +
                     ava::core::json::escape(to_string(materialized.payload.status)) + "\",\"result\":\"" + ava::core::json::escape(materialized.result_text) +
                     "\",\"structured_result\":" + serialize_tool_result_payload_json(materialized);
  // Dedicated model/provider-only replay field. Revalidate fail-closed so forged,
  // over-cap, or control-bearing session values never persist. Ordinary result and
  // structured_result remain guidance-free for public timeline/event surfaces.
  if (!materialized.success)
  {
    if (auto validated = ava::permissions::validated_permission_user_guidance(materialized.provider_user_guidance))
      json += ",\"provider_user_guidance\":\"" + ava::core::json::escape(*validated) + "\"";
  }
  if (assistant_output_entry_id)
    json += ",\"assistant_output_entry_id\":\"" + ava::core::json::escape(*assistant_output_entry_id) + "\"";
  json += '}';
  return json;
}

}  // namespace

ava::core::Result<PersistedAssistantTurn> append_assistant_turn(SessionAppendBatchSink const& sink, ParsedAssistantTurn const& turn,
                                                                std::string_view provider_id, std::string_view model_id, ava::provider::TokenUsage const& usage,
                                                                std::optional<long double> const& cost_usd, std::optional<std::string_view> api_family,
                                                                std::optional<std::string_view> reasoning_format)
{
  if (!turn.finish_reason)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "assistant turn has no normalized finish reason"));
  }
  return append_assistant_turn_impl(sink, turn, provider_id, model_id, usage, cost_usd, api_family, reasoning_format);
}

ava::core::Result<std::string> append_user_message(SessionAppendSink const& sink, std::string const& text,
                                                   std::vector<ava::session::ImageAttachmentRef> const& attachments,
                                                   std::optional<ava::session::SyntheticDeliveryProvenance> const& provenance)
{
  auto id = ava::core::make_id("entry");
  auto appended = append_entry(sink, ava::session::EntryType::UserMessage, id, user_message_data_json(text, attachments, provenance));
  if (!appended)
    return std::unexpected(std::move(appended.error()));
  return id;
}

ava::core::VoidResult append_replay_user_message(SessionAppendSink const& sink, std::string const& text,
                                                 std::vector<ava::session::ImageAttachmentRef> const& attachments, std::string const& replay_of)
{
  auto data = user_message_data_json(text, attachments);
  data.pop_back();
  data += ",\"internal_replay\":true,\"replay_of\":\"" + ava::core::json::escape(replay_of) + "\",\"reason\":\"context_compaction_active_prompt_replay\"}";
  return append_entry(sink, ava::session::EntryType::UserMessage, ava::core::make_id("entry"), std::move(data));
}

ava::core::VoidResult append_assistant_message(SessionAppendSink const& sink, std::string const& text, std::size_t tool_call_count,
                                               ava::provider::TokenUsage const& usage, std::optional<long double> const& cost_usd)
{
  return append_entry(sink, ava::session::EntryType::AssistantMessage, ava::core::make_id("entry"),
                      "{\"text\":\"" + ava::core::json::escape(text) + "\",\"tool_calls\":" + std::to_string(tool_call_count) +
                          ",\"usage\":" + usage_json(usage, cost_usd) + "}");
}

ava::core::VoidResult append_reasoning_block(SessionAppendSink const& sink, ParsedReasoningBlock const& block, std::string_view provider_id,
                                             std::string_view model_id)
{
  if (block.text.empty() && block.signature.empty() && block.redacted_data.empty() && block.native_item_json.empty())
    return {};
  return append_entry(sink, ava::session::EntryType::ReasoningBlock, ava::core::make_id("entry"), reasoning_block_data_json(block, provider_id, model_id));
}

ava::core::VoidResult append_tool_call(SessionAppendSink const& sink, ProviderToolCall const& call)
{
  return append_entry(sink, ava::session::EntryType::ToolCall, ava::core::make_id("entry"),
                      "{\"call_id\":\"" + ava::core::json::escape(call.id) + "\",\"name\":\"" + ava::core::json::escape(call.name) + "\",\"arguments\":\"" +
                          ava::core::json::escape(call.arguments_json) + "\"}");
}

ava::core::VoidResult append_tool_result(SessionAppendSink const& sink, ToolDispatchResult const& result,
                                         std::optional<std::string_view> assistant_output_entry_id)
{
  return append_entry(sink, ava::session::EntryType::ToolResult, ava::core::make_id("entry"), tool_result_data_json(result, assistant_output_entry_id));
}

ava::core::VoidResult append_permission_decision(SessionAppendSink const& sink, ava::tools::PermissionAuditEvent const& event)
{
  return append_entry(sink, ava::session::EntryType::PermissionDecision, ava::core::make_id("entry"), ava::tools::permission_audit_data_json(event));
}

ava::core::VoidResult append_error(SessionAppendSink const& sink, ava::core::Error const& error)
{
  return append_entry(sink, ava::session::EntryType::Error, ava::core::make_id("entry"),
                      "{\"category\":\"" + ava::core::json::escape(ava::core::to_string(error.category())) + "\",\"message\":\"" +
                          ava::core::json::escape(error.message()) + "\",\"details\":\"" + ava::core::json::escape(error.format()) + "\"}");
}

ava::core::VoidResult append_cancel(SessionAppendSink const& sink, std::string_view boundary)
{
  return append_entry(sink, ava::session::EntryType::Cancel, ava::core::make_id("entry"),
                      "{\"reason\":\"cancel_requested\",\"boundary\":\"" + ava::core::json::escape(boundary) + "\"}");
}

ToolDispatchResult synthetic_terminal_tool_result(ProviderToolCall const& call, ToolResultStatus status)
{
  bool const canceled = status == ToolResultStatus::Canceled;
  std::string const message =
      canceled ? "Tool execution was canceled before AVA durably recorded a terminal result; the execution outcome is unknown. Do not retry automatically."
               : "Tool execution was interrupted before AVA durably recorded a terminal result; the execution outcome is unknown. Do not retry automatically.";
  std::string result_text = "{\"tool\":\"" + ava::core::json::escape(call.name) + "\",\"ok\":false,\"retryable\":false";
  if (canceled)
    result_text += ",\"canceled\":true";
  result_text += ",\"error\":{\"category\":\"session\",\"code\":\"execution_outcome_unknown\",\"message\":\"" + ava::core::json::escape(message) + "\"}}";
  ToolResultPayload payload;
  payload.status = status;
  payload.summary = "Interrupted tool execution; outcome unknown and non-retriable";
  payload.content_type = "application/json";
  payload.error_category = "session";
  payload.error_code = "execution_outcome_unknown";
  payload.error_message = message;
  payload.error_details = "No tool is re-executed during recovery.";
  return ToolDispatchResult{.call_id = call.id, .name = call.name, .success = false, .result_text = std::move(result_text), .payload = std::move(payload)};
}

ava::core::VoidResult reconcile_unresolved_committed_function_calls(ava::session::SessionReadAuthority const& read_authority, SessionAppendSink const& sink,
                                                                    ava::session::SessionReadLimits limits)
{
  if (!sink)
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "committed function-call reconciliation requires an append authority route"));
  }
  auto entries = read_authority.load_bounded(limits);
  if (!entries)
    return std::unexpected(std::move(entries.error()));
  auto unresolved = ava::session::find_unresolved_committed_function_calls(*entries);
  if (!unresolved)
    return std::unexpected(std::move(unresolved.error()));
  for (auto const& function : *unresolved)
  {
    auto result =
        synthetic_terminal_tool_result(ProviderToolCall{.id = function.call_id, .name = function.name, .arguments_json = "{}"}, ToolResultStatus::Error);
    if (auto appended = append_tool_result(sink, result, function.assistant_output_entry_id); !appended)
    {
      auto error = std::move(appended.error());
      error.with_context("assistant_output_entry_id", function.assistant_output_entry_id)
          .with_context("call_id", function.call_id)
          .with_context("tool_name", function.name);
      return std::unexpected(std::move(error));
    }
  }
  return {};
}

}  // namespace ava::agent
