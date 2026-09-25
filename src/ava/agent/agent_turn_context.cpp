#include "sys.h"
#include "ava/agent/agent_turn_executor_internal.h"
#include "ava/agent/provider_output_validation.h"
#include "ava/session/assistant_output.h"
#include "ava/session/attachments.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace ava::agent::detail {
namespace {

ava::core::Result<std::unordered_set<std::string>> load_persisted_provider_tool_call_ids(ava::session::SessionReadAuthority read_authority,
                                                                                         ava::session::SessionReadLimits const& read_limits)
{
  auto entries = read_authority.load_bounded(read_limits);
  if (!entries)
  {
    auto error = std::move(entries.error());
    error.with_context("operation", "seed persistent provider tool-call ids");
    return std::unexpected(std::move(error));
  }

  std::unordered_set<std::string> ids;
  auto const assistant_output = ava::session::classify_assistant_output(*entries);
  for (auto const& diagnostic : assistant_output.diagnostics)
  {
    if (diagnostic.severity == ava::session::AssistantOutputDiagnosticSeverity::Warning &&
        diagnostic.kind == ava::session::AssistantOutputDiagnosticKind::IncompleteAssistantTurn)
    {
      continue;
    }
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "persisted assistant-output records are malformed");
    error.with_context("diagnostic", diagnostic.message).with_context("entry_id", diagnostic.entry_id);
    return std::unexpected(std::move(error));
  }
  auto add_id = [&](std::string id, std::string_view source) -> ava::core::VoidResult {
    if (auto valid = validate_provider_tool_call_id(id); !valid)
    {
      auto error = std::move(valid.error());
      error.with_context("source", std::string(source));
      return std::unexpected(std::move(error));
    }
    if (!ids.insert(std::move(id)).second)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "provider tool call id is duplicated in persisted session history");
      error.with_context("source", std::string(source));
      return std::unexpected(std::move(error));
    }
    return {};
  };
  for (auto const& entry : *entries)
  {
    if (entry.type == ava::session::EntryType::ToolCall)
    {
      if (auto added = add_id(ava::core::json::string_field(entry.data_json, "call_id").value_or(""), "persisted_session_tool_call"); !added)
        return std::unexpected(std::move(added.error()));
    }
  }
  for (auto const& turn : assistant_output.turns)
  {
    for (auto const& item : turn.items)
    {
      auto const* function = std::get_if<ava::session::AssistantOutputFunctionCall>(&item.item.payload);
      if (!function)
        continue;
      if (auto added = add_id(function->call_id, "committed_assistant_output_function"); !added)
        return std::unexpected(std::move(added.error()));
    }
  }
  return ids;
}

}  // namespace

AgentTurnSession::AgentTurnSession(AgentLoopOptions const& options, ava::session::SessionStore& store) noexcept : options_(options), store_(store)
{
}

bool AgentTurnSession::is_canceled() const
{
  return options_.cancel_requested && options_.cancel_requested();
}

ava::core::VoidResult AgentTurnSession::check_canceled(std::string_view boundary)
{
  if (!is_canceled())
    return {};
  static_cast<void>(append_cancel(options_.append_entry, boundary));
  auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "agent loop canceled", ava::core::ErrorCode::Canceled);
  error.with_context("boundary", std::string(boundary));
  return std::unexpected(std::move(error));
}

ava::core::Result<std::string> AgentTurnSession::append_user_message(std::string const& text, std::vector<ava::session::ImageAttachmentRef> const& attachments)
{
  return ava::agent::append_user_message(options_.append_entry, text, attachments, options_.synthetic_user_message_provenance);
}

ava::core::VoidResult AgentTurnSession::append_replay_user_message(ActiveTurnUserMessage const& message)
{
  return ava::agent::append_replay_user_message(options_.append_entry, message.text, message.image_attachments, message.id);
}

ava::core::Result<PersistedAssistantTurn> AgentTurnSession::append_assistant_turn(ParsedAssistantTurn const& turn, ava::provider::TokenUsage const& usage,
                                                                                  std::optional<long double> const& cost_usd)
{
  auto const source_api_family =
      options_.model.api_family.empty() ? std::optional<std::string_view>{} : std::optional<std::string_view>{options_.model.api_family};
  auto const source_reasoning_format =
      options_.model.reasoning_format.empty() ? std::optional<std::string_view>{} : std::optional<std::string_view>{options_.model.reasoning_format};
  return ava::agent::append_assistant_turn(options_.append_batch, turn, options_.model.provider_id, options_.model.model_id, usage, cost_usd, source_api_family,
                                           source_reasoning_format);
}

ava::core::VoidResult AgentTurnSession::append_tool_result(ToolDispatchResult const& dispatch_result, std::optional<std::string_view> assistant_output_entry_id)
{
  return ava::agent::append_tool_result(options_.append_entry, dispatch_result, assistant_output_entry_id);
}

ava::core::VoidResult AgentTurnSession::append_permission_decision(ava::tools::PermissionAuditEvent const& event)
{
  return ava::agent::append_permission_decision(options_.append_entry, event);
}

ava::core::VoidResult AgentTurnSession::append_error(ava::core::Error const& error)
{
  return ava::agent::append_error(options_.append_entry, error);
}

ava::core::Result<BuiltProviderMessages> AgentTurnSession::build_messages(MessageBuildOptions options)
{
  return ava::agent::build_messages(*options_.session_read_authority, std::move(options));
}

ava::core::Result<std::unordered_set<std::string>> AgentTurnSession::persisted_provider_tool_call_ids()
{
  return load_persisted_provider_tool_call_ids(*options_.session_read_authority, options_.session_read_limits);
}

ava::core::VoidResult AgentTurnSession::attach_verified_image_payloads(ava::provider::ProviderRequest& request) const
{
  for (auto& message : request.messages)
  {
    for (auto& part : message.content_parts)
    {
      if (part.type != ava::provider::ContentPartType::Image)
        continue;
      ava::session::ImageAttachmentRef const attachment{
          .id = part.attachment_id, .mime_type = part.mime_type, .storage_path = part.storage_path, .sha256 = part.sha256, .byte_size = part.byte_size};
      auto loaded = ava::session::load_image_attachment(store_, attachment);
      if (!loaded)
        return std::unexpected(std::move(loaded.error()));
      part.data_base64 = ava::provider::base64_encode(loaded->bytes);
    }
  }
  return {};
}

ava::core::VoidResult AgentTurnExecutor::publish_phase(RunPhase phase) const
{
  if (!options_.on_phase)
    return {};
  try
  {
    return options_.on_phase(phase);
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "run phase callback threw"));
  }
}

MessageBuildOptions AgentTurnExecutor::message_build_options() const
{
  auto api_family = options_.model.api_family;
  auto reasoning_format = options_.model.reasoning_format;
  if (api_family.empty())
  {
    if (options_.model.provider_id == "openai")
      api_family = "openai_responses";
    else if (options_.model.provider_id == "anthropic")
      api_family = "anthropic_messages";
    else if (options_.model.provider_id == "gemini")
      api_family = "gemini_generate_content";
    else
      api_family = "openai_chat_completions";
  }
  if (reasoning_format.empty())
  {
    if (api_family == "openai_responses")
      reasoning_format = "openai_responses";
    else if (api_family == "anthropic_messages")
      reasoning_format = "anthropic_thinking";
  }
  std::vector<std::string> active_entry_ids;
  active_entry_ids.reserve(active_turn_user_messages_.size());
  for (auto const& message : active_turn_user_messages_)
    active_entry_ids.push_back(message.id);
  bool const supports_images =
      std::find(options_.model.input_modalities.begin(), options_.model.input_modalities.end(), "image") != options_.model.input_modalities.end();
  return MessageBuildOptions{.max_tool_result_context_bytes = options_.max_tool_result_context_bytes,
                             .target = HistoryReplayTarget{.provider_id = options_.model.provider_id,
                                                           .model_id = options_.model.model_id,
                                                           .api_family = std::move(api_family),
                                                           .reasoning_format = std::move(reasoning_format),
                                                           .supports_tools = options_.model.supports_tools,
                                                           .supports_images = supports_images},
                             .active_turn_user_entry_ids = std::move(active_entry_ids)};
}

ava::core::Result<BuiltProviderMessages> AgentTurnExecutor::build_messages()
{
  return session_.build_messages(message_build_options());
}

std::vector<std::string> AgentTurnExecutor::replayable_active_turn_texts() const
{
  std::vector<std::string> messages;
  messages.reserve(active_turn_user_messages_.size());
  for (auto const& message : active_turn_user_messages_)
    messages.push_back(message.text);
  return messages;
}

ava::core::Result<bool> AgentTurnExecutor::compact_context(std::string_view trigger)
{
  if (auto phase = publish_phase(RunPhase::Compacting); !phase)
    return std::unexpected(std::move(phase.error()));
  auto const replayed_messages = replayable_active_turn_texts();
  if (options_.child_compaction_binding)
  {
    return compact_child_context(*options_.child_compaction_binding, trigger, replayed_messages, provider_, *effective_transport_,
                                 ContextCompactionInvocation{.model = options_.model,
                                                             .access_token = options_.access_token,
                                                             .credential_type = options_.credential_type,
                                                             .openai_oauth = options_.openai_oauth,
                                                             .openai_account_id = options_.openai_account_id,
                                                             .cancel_requested = options_.cancel_requested});
  }
  if (options_.compact_context)
    return options_.compact_context(*options_.session_read_authority, trigger, replayed_messages);
  auto error = ava::core::Error(ava::core::ErrorCategory::Provider, "context compaction is unavailable");
  error.with_context("trigger", std::string(trigger));
  return std::unexpected(std::move(error));
}

ava::core::VoidResult AgentTurnExecutor::append_active_turn_user_message(std::string const& text,
                                                                         std::vector<ava::session::ImageAttachmentRef> const& attachments)
{
  auto appended = session_.append_user_message(text, attachments);
  if (!appended)
    return std::unexpected(std::move(appended.error()));
  active_turn_user_messages_.push_back(ActiveTurnUserMessage{.id = *appended, .text = text, .image_attachments = attachments});
  return {};
}

ava::core::VoidResult AgentTurnExecutor::replay_active_turn_user_messages()
{
  for (auto const& message : active_turn_user_messages_)
  {
    if (auto replayed = session_.append_replay_user_message(message); !replayed)
      return replayed;
  }
  return {};
}

ava::core::Result<bool> AgentTurnExecutor::prepare_context_overflow_retry(ava::core::Error const& error)
{
  if (!ava::provider::is_context_overflow_error(error) || context_overflow_retry_used_ || (!options_.compact_context && !options_.child_compaction_binding))
  {
    return false;
  }
  context_overflow_retry_used_ = true;
  if (auto not_canceled = session_.check_canceled("before_context_overflow_compaction"); !not_canceled)
  {
    return std::unexpected(std::move(not_canceled.error()));
  }
  auto compacted = compact_context("context_overflow");
  if (!compacted)
  {
    if (compacted.error().code() == ava::core::ErrorCode::Canceled)
      return std::unexpected(std::move(compacted.error()));
    // Both errors can originate in provider callbacks. Do not carry their
    // diagnostics into the session or public runtime error path.
    auto compact_error = ava::core::Error(ava::core::ErrorCategory::Provider, "context overflow compaction failed");
    compact_error.with_context("provider_error_kind", "context_overflow");
    compact_error.with_context("compaction_status", "failed");
    for (auto const& context : compacted.error().context())
    {
      bool const decimal_status =
          context.key == "status" && context.value.size() == 3 && std::ranges::all_of(context.value, [](unsigned char ch) { return std::isdigit(ch) != 0; });
      if (decimal_status)
      {
        compact_error.with_context("compaction_provider_status", context.value);
        break;
      }
    }
    return std::unexpected(std::move(compact_error));
  }
  if (*compacted)
  {
    if (auto replayed = replay_active_turn_user_messages(); !replayed)
    {
      return std::unexpected(std::move(replayed.error()));
    }
    skip_auto_compaction_after_overflow_retry_ = true;
  }
  if (auto not_canceled = session_.check_canceled("after_context_overflow_compaction"); !not_canceled)
  {
    return std::unexpected(std::move(not_canceled.error()));
  }
  return true;
}

}  // namespace ava::agent::detail
