#include "sys.h"
#include "ava/provider/openai_reasoning.h"
#include "ava/provider/openai_stream_parser.h"
#include "ava/provider/openai_stream_parser_internal.h"
#include "ava/core/NoLogString.h"
#include "ava/core/json.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::provider {
namespace {

enum class MessageContentKind
{
  OutputText,
  Refusal,
};

struct CompletedMessageContent
{
  ava::core::NoLogString output_text;
  ava::core::NoLogString refusal_text;
};

ava::core::Result<bool> completed_message_content_from_item(std::string_view item, std::string& output_text, std::string& refusal_text)
{
  if (!ava::core::json::field_value_start(item, "content"))
    return false;
  auto const contents = ava::core::json::strict_objects_in_array_field(item, "content", kMaxProviderParserArrayItems);
  if (!contents)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI response parser limit exceeded"));
  }

  std::string complete_output_text;
  std::string complete_refusal_text;
  for (auto const& content : *contents)
  {
    auto const type = ava::core::json::string_field(content, "type").value_or("");
    if (type == "output_text")
    {
      auto const text = ava::core::json::string_field(content, "text");
      if (!text)
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI output_text content part requires string text"));
      complete_output_text += *text;
      continue;
    }
    if (type == "refusal")
    {
      auto const refusal = ava::core::json::string_field(content, "refusal");
      if (!refusal)
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI refusal content part requires string refusal"));
      complete_refusal_text += *refusal;
      continue;
    }
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "OpenAI message output item has an unsupported content-part type"));
  }
  output_text = std::move(complete_output_text);
  refusal_text = std::move(complete_refusal_text);
  return true;
}

void append_documented_message_delta(std::vector<StreamEvent>& events, OpenAIStreamParser::MessageItemState& state, std::string_view item_id,
                                     std::string_view text, MessageContentKind kind)
{
  if (text.empty())
    return;
  state.text += text;
  if (kind == MessageContentKind::OutputText)
    state.output_text += text;
  else
    state.refusal_text += text;
  events.push_back(StreamEvent{.type = StreamEventType::TextDelta,
                               .text = std::string(text),
                               .tool_call_id = "",
                               .tool_name = "",
                               .error_message = "",
                               .usage = std::nullopt,
                               .provider_item_id = std::string(item_id),
                               .provider_output_index = state.provider_output_index,
                               .assistant_phase = state.phase});
}

bool reconcile_complete_message_text(std::vector<StreamEvent>& events, OpenAIStreamParser::MessageItemState& state, std::string_view item_id,
                                     std::string_view complete_text, MessageContentKind kind, bool& error_seen)
{
  auto const& captured = kind == MessageContentKind::OutputText ? state.output_text : state.refusal_text;
  if (complete_text == captured)
    return true;
  if (complete_text.starts_with(captured))
  {
    auto const suffix = complete_text.substr(captured.size());
    append_documented_message_delta(events, state, item_id, suffix, kind);
    return true;
  }
  openai_stream_parser_internal::append_stream_error(
      events, error_seen, kind == MessageContentKind::Refusal ? "conflicting OpenAI refusal text" : "conflicting OpenAI message text");
  return false;
}

}  // namespace

using namespace openai_stream_parser_internal;

std::pair<OpenAIStreamParser::MessageEventSource, OpenAIStreamParser::MessageItemState*> OpenAIStreamParser::documented_message_item_for_event(
    std::vector<StreamEvent>& events, std::string_view data)
{
  auto const item_id_start = ava::core::json::field_value_start(data, "item_id");
  auto const output_item_id_start = ava::core::json::field_value_start(data, "output_item_id");
  if (!item_id_start && !output_item_id_start)
    return {MessageEventSource::Legacy, nullptr};
  auto const item_id = ava::core::json::string_field(data, "item_id");
  auto const output_item_id = ava::core::json::string_field(data, "output_item_id");
  if ((item_id_start && (!item_id || !is_valid_openai_opaque_id(*item_id))) ||
      (output_item_id_start && (!output_item_id || !is_valid_openai_opaque_id(*output_item_id))) || (item_id && output_item_id && *item_id != *output_item_id))
  {
    append_stream_error(events, error_seen_, "OpenAI message text event has an invalid or conflicting documented item ID");
    return {MessageEventSource::Documented, nullptr};
  }
  auto const id = item_id ? item_id : output_item_id;
  auto const state = message_items_.find(*id);
  if (state == message_items_.end())
  {
    append_stream_error(events, error_seen_, "OpenAI message text event references an unbound item ID");
    return {MessageEventSource::Documented, nullptr};
  }
  if (state->second.ended)
  {
    append_stream_error(events, error_seen_, "OpenAI message text event emitted after item completion");
    return {MessageEventSource::Documented, nullptr};
  }
  auto const output_index = documented_output_index(data, "{}");
  if (!output_index)
  {
    append_stream_error(events, error_seen_, output_index.error().message());
    return {MessageEventSource::Documented, nullptr};
  }
  if (*output_index && state->second.provider_output_index != *output_index)
  {
    append_stream_error(events, error_seen_, "OpenAI message text event changed its output_index");
    return {MessageEventSource::Documented, nullptr};
  }
  auto const phase = documented_message_phase(data, "{}");
  if (!phase)
  {
    append_stream_error(events, error_seen_, phase.error().message());
    return {MessageEventSource::Documented, nullptr};
  }
  if (*phase != AssistantPhase::Unknown && state->second.phase != AssistantPhase::Unknown && *phase != state->second.phase)
  {
    append_stream_error(events, error_seen_, "OpenAI message text event changed its phase");
    return {MessageEventSource::Documented, nullptr};
  }
  if (state->second.phase == AssistantPhase::Unknown && *phase != AssistantPhase::Unknown)
    state->second.phase = *phase;
  return {MessageEventSource::Documented, &state->second};
}

void OpenAIStreamParser::handle_documented_message_output_item(std::vector<StreamEvent>& events, std::string_view item, std::string const& item_id,
                                                               std::optional<std::size_t> output_index, AssistantPhase phase, OutputItemLifecycle lifecycle)
{
  if (lifecycle == OutputItemLifecycle::Added)
  {
    CompletedMessageContent content;
    auto const parsed_content = completed_message_content_from_item(item, content.output_text, content.refusal_text);
    if (!parsed_content)
    {
      append_stream_error(events, error_seen_, parsed_content.error().message());
      return;
    }
    auto const [state, inserted] = message_items_.emplace(item_id, OpenAIStreamParser::MessageItemState{.provider_output_index = output_index,
                                                                                                        .phase = phase,
                                                                                                        .text = "",
                                                                                                        .output_text = "",
                                                                                                        .refusal_text = "",
                                                                                                        .output_text_completed = false,
                                                                                                        .refusal_completed = false,
                                                                                                        .ended = false});
    if (!inserted)
    {
      append_stream_error(events, error_seen_, "OpenAI message output item was added more than once");
      return;
    }
    events.push_back(StreamEvent{.type = StreamEventType::TextStart,
                                 .text = "",
                                 .tool_call_id = "",
                                 .tool_name = "",
                                 .error_message = "",
                                 .usage = std::nullopt,
                                 .provider_item_id = item_id,
                                 .provider_output_index = output_index,
                                 .assistant_phase = phase});
    if (*parsed_content)
    {
      append_documented_message_delta(events, state->second, item_id, content.output_text, MessageContentKind::OutputText);
      append_documented_message_delta(events, state->second, item_id, content.refusal_text, MessageContentKind::Refusal);
      if (!content.refusal_text.empty())
        saw_refusal_ = true;
    }
    return;
  }

  auto const state = message_items_.find(item_id);
  if (state == message_items_.end())
  {
    append_stream_error(events, error_seen_, "OpenAI message output item completed without one matching add lifecycle event");
    return;
  }
  if (state->second.ended)
  {
    append_stream_error(events, error_seen_, "OpenAI message output item completed more than once");
    return;
  }
  if ((output_index && state->second.provider_output_index != output_index) ||
      (phase != AssistantPhase::Unknown && state->second.phase != AssistantPhase::Unknown && phase != state->second.phase))
  {
    append_stream_error(events, error_seen_, "OpenAI message output item changed its output_index or phase");
    return;
  }
  if (state->second.phase == AssistantPhase::Unknown && phase != AssistantPhase::Unknown)
    state->second.phase = phase;
  CompletedMessageContent complete_content;
  auto const parsed_complete_content = completed_message_content_from_item(item, complete_content.output_text, complete_content.refusal_text);
  if (!parsed_complete_content)
  {
    append_stream_error(events, error_seen_, parsed_complete_content.error().message());
    return;
  }
  if (*parsed_complete_content)
  {
    if (!reconcile_complete_message_text(events, state->second, item_id, complete_content.output_text, MessageContentKind::OutputText, error_seen_) ||
        !reconcile_complete_message_text(events, state->second, item_id, complete_content.refusal_text, MessageContentKind::Refusal, error_seen_))
    {
      return;
    }
    if (!complete_content.refusal_text.empty())
      saw_refusal_ = true;
  }
  state->second.ended = true;
  events.push_back(StreamEvent{.type = StreamEventType::TextEnd,
                               .text = "",
                               .tool_call_id = "",
                               .tool_name = "",
                               .error_message = "",
                               .usage = std::nullopt,
                               .provider_item_id = item_id,
                               .provider_output_index = state->second.provider_output_index,
                               .assistant_phase = state->second.phase});
}

OpenAIStreamParser::EventHandling OpenAIStreamParser::handle_message_event(std::vector<StreamEvent>& events, std::string_view data, std::string_view type,
                                                                           EventRoutingPhase routing_phase)
{
  if (routing_phase == EventRoutingPhase::BeforeIgnoredLifecycle && type == "response.output_text.done")
  {
    auto const [source, state] = documented_message_item_for_event(events, data);
    if (source == MessageEventSource::Legacy)
      return EventHandling::Handled;  // Retain legacy output_text.done compatibility.
    if (!state)
      return EventHandling::Handled;
    if (state->output_text_completed)
    {
      append_stream_error(events, error_seen_, "OpenAI message text completed more than once");
      return EventHandling::Handled;
    }
    auto const text_start = ava::core::json::field_value_start(data, "text");
    auto const complete_text = ava::core::json::string_field(data, "text");
    if (text_start && !complete_text)
    {
      append_stream_error(events, error_seen_, "OpenAI message text completion requires a string text");
      return EventHandling::Handled;
    }
    auto const item_id = ava::core::json::string_field(data, "item_id").or_else([&data] { return ava::core::json::string_field(data, "output_item_id"); });
    if (!reconcile_complete_message_text(events, *state, *item_id, complete_text.value_or(""), MessageContentKind::OutputText, error_seen_))
      return EventHandling::Handled;
    state->output_text_completed = true;
    saw_content_ = true;
    return EventHandling::Handled;
  }
  if (routing_phase == EventRoutingPhase::BeforeIgnoredLifecycle && type == "response.refusal.done")
  {
    append_finish_reasoning_if_open(events);
    auto const [source, state] = documented_message_item_for_event(events, data);
    if (source == MessageEventSource::Legacy)
    {
      append_stream_error(events, error_seen_, "OpenAI refusal completion requires a documented message item ID");
      return EventHandling::Handled;
    }
    if (!state)
      return EventHandling::Handled;
    if (state->refusal_completed)
    {
      append_stream_error(events, error_seen_, "OpenAI message refusal completed more than once");
      return EventHandling::Handled;
    }
    auto const refusal = ava::core::json::string_field(data, "refusal");
    if (!refusal)
    {
      append_stream_error(events, error_seen_, "OpenAI refusal completion requires string refusal");
      return EventHandling::Handled;
    }
    auto const item_id = ava::core::json::string_field(data, "item_id").or_else([&data] { return ava::core::json::string_field(data, "output_item_id"); });
    if (!reconcile_complete_message_text(events, *state, *item_id, *refusal, MessageContentKind::Refusal, error_seen_))
      return EventHandling::Handled;
    state->refusal_completed = true;
    saw_content_ = true;
    saw_refusal_ = true;
    return EventHandling::Handled;
  }
  if (routing_phase == EventRoutingPhase::BeforeIgnoredLifecycle)
    return EventHandling::Unhandled;
  if (type == "response.refusal.delta")
  {
    append_finish_reasoning_if_open(events);
    auto const [source, state] = documented_message_item_for_event(events, data);
    if (source == MessageEventSource::Legacy)
    {
      append_stream_error(events, error_seen_, "OpenAI refusal delta requires a documented message item ID");
      return EventHandling::Handled;
    }
    if (!state)
      return EventHandling::Handled;
    if (state->refusal_completed)
    {
      append_stream_error(events, error_seen_, "OpenAI message refusal emitted after completion");
      return EventHandling::Handled;
    }
    auto const delta = ava::core::json::string_field(data, "delta");
    if (!delta)
    {
      append_stream_error(events, error_seen_, "OpenAI refusal delta requires string delta");
      return EventHandling::Handled;
    }
    auto const item_id = ava::core::json::string_field(data, "item_id").or_else([&data] { return ava::core::json::string_field(data, "output_item_id"); });
    append_documented_message_delta(events, *state, *item_id, *delta, MessageContentKind::Refusal);
    saw_content_ = true;
    saw_refusal_ = true;
    return EventHandling::Handled;
  }
  if (type == "response.output_text.delta" || type == "response.text.delta")
  {
    saw_content_ = true;
    append_finish_reasoning_if_open(events);
    auto const [source, state] = documented_message_item_for_event(events, data);
    if (source == MessageEventSource::Documented)
    {
      if (!state)
        return EventHandling::Handled;
      if (state->output_text_completed)
      {
        append_stream_error(events, error_seen_, "OpenAI message text emitted after completion");
        return EventHandling::Handled;
      }
      auto const item_id = ava::core::json::string_field(data, "item_id").or_else([&data] { return ava::core::json::string_field(data, "output_item_id"); });
      append_documented_message_delta(
          events, *state, *item_id,
          ava::core::json::string_field(data, "delta").or_else([&data] { return ava::core::json::string_field(data, "text"); }).value_or(""),
          MessageContentKind::OutputText);
      return EventHandling::Handled;
    }
    events.push_back(
        StreamEvent{.type = StreamEventType::TextDelta,
                    .text = ava::core::json::string_field(data, "delta").or_else([&data] { return ava::core::json::string_field(data, "text"); }).value_or(""),
                    .tool_call_id = "",
                    .tool_name = "",
                    .error_message = "",
                    .usage = std::nullopt});
    return EventHandling::Handled;
  }
  return EventHandling::Unhandled;
}

bool OpenAIStreamParser::reject_unended_documented_message_items(std::vector<StreamEvent>& events)
{
  for (auto const& [item_id, state] : message_items_)
  {
    static_cast<void>(item_id);
    if (!state.ended)
    {
      append_stream_error(events, error_seen_, "OpenAI response ended before documented message item completion");
      return true;
    }
  }
  return false;
}

}  // namespace ava::provider
