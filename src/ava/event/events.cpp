#include "sys.h"
#include "events.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/mode.h"

#include <cstddef>
#include <type_traits>
#include <utility>

namespace ava::event {
namespace {

void append_string_field(std::string& out, std::string_view key, std::string_view value)
{
  if (value.empty())
    return;
  out += ",\"";
  out += key;
  out += "\":\"";
  out += ava::core::json::escape(value);
  out += '"';
}

void append_required_string_field(std::string& out, std::string_view key, std::string_view value)
{
  out += ",\"";
  out += key;
  out += "\":\"";
  out += ava::core::json::escape(value);
  out += '"';
}

void append_optional_string_field(std::string& out, std::string_view key, std::optional<std::string> const& value)
{
  if (!value || value->empty())
    return;
  append_required_string_field(out, key, *value);
}

void append_payload_string_field(std::string& out, bool& has_field, std::string_view key, std::string_view value)
{
  if (value.empty())
    return;
  if (has_field)
    out += ',';
  out += '"';
  out += key;
  out += "\":\"";
  out += ava::core::json::escape(value);
  out += '"';
  has_field = true;
}

void append_payload_number_field(std::string& out, bool& has_field, std::string_view key, std::size_t value)
{
  if (value == 0)
    return;
  if (has_field)
    out += ',';
  out += '"';
  out += key;
  out += "\":";
  out += std::to_string(value);
  has_field = true;
}

void append_payload_bool_field(std::string& out, bool& has_field, std::string_view key, bool value)
{
  if (!value)
    return;
  if (has_field)
    out += ',';
  out += '"';
  out += key;
  out += "\":true";
  has_field = true;
}

void append_payload_json_object_field(std::string& out, bool& has_field, std::string_view key, std::string_view value)
{
  if (value.empty())
    return;
  if (has_field)
    out += ',';
  out += '"';
  out += key;
  if (ava::core::json::is_valid_object(value))
  {
    out += "\":";
    out += value;
  }
  else
  {
    out += "_json\":\"";
    out += ava::core::json::escape(value);
    out += '"';
  }
  has_field = true;
}

void append_payload_string_array_field(std::string& out, bool& has_field, std::string_view key, std::vector<std::string> const& values)
{
  if (values.empty())
    return;
  if (has_field)
    out += ',';
  out += '"';
  out += key;
  out += "\":[";
  for (std::size_t index = 0; index < values.size(); ++index)
  {
    if (index > 0)
      out += ',';
    out += '"';
    out += ava::core::json::escape(values[index]);
    out += '"';
  }
  out += ']';
  has_field = true;
}

std::string payload_json_for_runtime_event(RuntimeEvent const& event)
{
  return std::visit([](auto const& family) { return serialize_payload_json(family.payload); }, event.payload());
}

void append_payload_aliases(std::string& out, std::string_view payload_json)
{
  for (std::string_view key : {"mode", "provider", "model", "text", "call_id", "tool", "status", "category", "error_code", "message", "details", "content_type",
                               "stop_reason", "trigger", "reason", "reasoning_format", "diff", "spill_path"})
  {
    if (auto value = ava::core::json::string_field(payload_json, key); value && !value->empty())
      append_required_string_field(out, key, *value);
  }
  for (std::string_view key : {"provider_iterations", "tool_calls",       "attempt",          "max_attempts",    "delay_ms",
                               "remaining_ms",        "estimated_tokens", "threshold_tokens", "retained_tokens", "post_compaction_tokens",
                               "summary_bytes",       "snapshot_entries", "current_entries",  "output_bytes",    "total_bytes",
                               "output_lines",        "total_lines",      "start_line",       "end_line",        "next_offset_line",
                               "omitted_bytes",       "omitted_lines",    "visible_matches",  "total_matches"})
  {
    if (auto value = ava::core::json::integer_field(payload_json, key); value && *value > 0)
    {
      out += ",\"";
      out += key;
      out += "\":";
      out += std::to_string(*value);
    }
  }
}

}  // namespace

std::string to_string(RuntimeEventType type)
{
  switch (type)
  {
    case RuntimeEventType::SessionStart:
      return "session_start";
    case RuntimeEventType::UserMessage:
      return "user_message";
    case RuntimeEventType::AssistantMessage:
      return "assistant_message";
    case RuntimeEventType::MessageUpdate:
      return "message_update";
    case RuntimeEventType::MessageEnd:
      return "message_end";
    case RuntimeEventType::ReasoningStart:
      return "reasoning_start";
    case RuntimeEventType::ReasoningDelta:
      return "reasoning_delta";
    case RuntimeEventType::ReasoningEnd:
      return "reasoning_end";
    case RuntimeEventType::ProviderEvent:
      return "provider_event";
    case RuntimeEventType::ToolStart:
      return "tool_start";
    case RuntimeEventType::ToolProgress:
      return "tool_progress";
    case RuntimeEventType::ToolResult:
      return "tool_result";
    case RuntimeEventType::CompactionStart:
      return "compaction_start";
    case RuntimeEventType::CompactionEnd:
      return "compaction_end";
    case RuntimeEventType::Retry:
      return "retry";
    case RuntimeEventType::RetryTick:
      return "retry_tick";
    case RuntimeEventType::Canceled:
      return "canceled";
    case RuntimeEventType::Error:
      return "error";
    case RuntimeEventType::Done:
      return "done";
  }
  return "error";
}

std::string_view to_string(PayloadType type) noexcept
{
  switch (type)
  {
    case PayloadType::Session:
      return "session";
    case PayloadType::Message:
      return "message";
    case PayloadType::Reasoning:
      return "reasoning";
    case PayloadType::Provider:
      return "provider";
    case PayloadType::Tool:
      return "tool";
    case PayloadType::Compaction:
      return "compaction";
    case PayloadType::Retry:
      return "retry";
    case PayloadType::Cancellation:
      return "cancellation";
    case PayloadType::Error:
      return "error";
    case PayloadType::Completion:
      return "completion";
    case PayloadType::Permission:
      return "permission";
    case PayloadType::Question:
      return "question";
    case PayloadType::Queue:
      return "queue";
  }
  return "error";
}

PayloadType payload_type_for_event(RuntimeEventType type) noexcept
{
  switch (type)
  {
    case RuntimeEventType::SessionStart:
      return PayloadType::Session;
    case RuntimeEventType::UserMessage:
    case RuntimeEventType::AssistantMessage:
    case RuntimeEventType::MessageUpdate:
    case RuntimeEventType::MessageEnd:
      return PayloadType::Message;
    case RuntimeEventType::ReasoningStart:
    case RuntimeEventType::ReasoningDelta:
    case RuntimeEventType::ReasoningEnd:
      return PayloadType::Reasoning;
    case RuntimeEventType::ProviderEvent:
      return PayloadType::Provider;
    case RuntimeEventType::ToolStart:
    case RuntimeEventType::ToolProgress:
    case RuntimeEventType::ToolResult:
      return PayloadType::Tool;
    case RuntimeEventType::CompactionStart:
    case RuntimeEventType::CompactionEnd:
      return PayloadType::Compaction;
    case RuntimeEventType::Retry:
    case RuntimeEventType::RetryTick:
      return PayloadType::Retry;
    case RuntimeEventType::Canceled:
      return PayloadType::Cancellation;
    case RuntimeEventType::Error:
      return PayloadType::Error;
    case RuntimeEventType::Done:
      return PayloadType::Completion;
  }
  return PayloadType::Error;
}

std::string serialize_payload_json(SessionPayload const& payload)
{
  std::string out = "{";
  bool has_field = false;
  append_payload_string_field(out, has_field, "mode", ava::core::to_string(payload.mode));
  append_payload_string_field(out, has_field, "provider", payload.provider);
  append_payload_string_field(out, has_field, "model", payload.model);
  out += '}';
  return out;
}

std::string serialize_payload_json(MessagePayload const& payload)
{
  std::string out = "{";
  bool has_field = false;
  append_payload_string_field(out, has_field, "text", payload.text);
  append_payload_string_field(out, has_field, "status", payload.status);
  append_payload_string_field(out, has_field, "message", payload.error_message);
  append_payload_string_field(out, has_field, "stop_reason", payload.stop_reason);
  out += '}';
  return out;
}

std::string serialize_payload_json(ReasoningPayload const& payload)
{
  std::string out = "{";
  bool has_field = false;
  append_payload_string_field(out, has_field, "text", payload.text);
  append_payload_string_field(out, has_field, "status", payload.status);
  append_payload_string_field(out, has_field, "message", payload.error_message);
  append_payload_string_field(out, has_field, "stop_reason", payload.stop_reason);
  append_payload_string_field(out, has_field, "reasoning_format", payload.reasoning_format);
  append_payload_bool_field(out, has_field, "reasoning_redacted", payload.reasoning_redacted);
  append_payload_bool_field(out, has_field, "reasoning_signature_present", payload.reasoning_signature_present);
  out += '}';
  return out;
}

std::string serialize_payload_json(ProviderPayload const& payload)
{
  std::string out = "{";
  bool has_field = false;
  append_payload_string_field(out, has_field, "text", payload.text);
  append_payload_string_field(out, has_field, "call_id", payload.call_id);
  append_payload_string_field(out, has_field, "tool", payload.tool);
  append_payload_string_field(out, has_field, "status", payload.status);
  append_payload_string_field(out, has_field, "message", payload.error_message);
  append_payload_string_field(out, has_field, "details", payload.error_details);
  append_payload_string_field(out, has_field, "stop_reason", payload.stop_reason);
  append_payload_string_field(out, has_field, "reason", payload.reason);
  append_payload_string_field(out, has_field, "reasoning_format", payload.reasoning_format);
  append_payload_string_array_field(out, has_field, "permission_request_ids", payload.permission_request_ids);
  append_payload_bool_field(out, has_field, "reasoning_redacted", payload.reasoning_redacted);
  append_payload_bool_field(out, has_field, "reasoning_signature_present", payload.reasoning_signature_present);
  out += '}';
  return out;
}

std::string serialize_payload_json(ToolPayload const& payload)
{
  std::string out = "{";
  bool has_field = false;
  append_payload_string_field(out, has_field, "text", payload.text);
  append_payload_string_field(out, has_field, "call_id", payload.call_id);
  append_payload_string_field(out, has_field, "tool", payload.tool);
  append_payload_json_object_field(out, has_field, "args", payload.args_json);
  append_payload_json_object_field(out, has_field, "result", payload.result_json);
  append_payload_json_object_field(out, has_field, "structured_result", payload.structured_result_json);
  append_payload_string_field(out, has_field, "status", payload.status);
  append_payload_string_field(out, has_field, "category", payload.error_category);
  append_payload_string_field(out, has_field, "error_code", payload.error_code);
  append_payload_string_field(out, has_field, "message", payload.error_message);
  append_payload_string_field(out, has_field, "details", payload.error_details);
  append_payload_string_field(out, has_field, "content_type", payload.content_type);
  append_payload_string_field(out, has_field, "diff", payload.diff);
  append_payload_string_array_field(out, has_field, "changed_paths", payload.changed_paths);
  append_payload_string_array_field(out, has_field, "permission_request_ids", payload.permission_request_ids);
  append_payload_string_field(out, has_field, "spill_path", payload.spill_path);
  append_payload_bool_field(out, has_field, "diff_truncated", payload.diff_truncated);
  append_payload_bool_field(out, has_field, "truncated", payload.truncated);
  append_payload_bool_field(out, has_field, "byte_limited", payload.byte_limited);
  append_payload_bool_field(out, has_field, "line_limited", payload.line_limited);
  append_payload_bool_field(out, has_field, "spill_truncated", payload.spill_truncated);
  append_payload_number_field(out, has_field, "output_bytes", payload.output_bytes);
  append_payload_number_field(out, has_field, "total_bytes", payload.total_bytes);
  append_payload_number_field(out, has_field, "output_lines", payload.output_lines);
  append_payload_number_field(out, has_field, "total_lines", payload.total_lines);
  append_payload_number_field(out, has_field, "start_line", payload.start_line);
  append_payload_number_field(out, has_field, "end_line", payload.end_line);
  append_payload_number_field(out, has_field, "next_offset_line", payload.next_offset_line);
  append_payload_number_field(out, has_field, "omitted_bytes", payload.omitted_bytes);
  append_payload_number_field(out, has_field, "omitted_lines", payload.omitted_lines);
  append_payload_number_field(out, has_field, "visible_matches", payload.visible_matches);
  append_payload_number_field(out, has_field, "total_matches", payload.total_matches);
  out += '}';
  return out;
}

std::string serialize_payload_json(CompactionPayload const& payload)
{
  std::string out = "{";
  bool has_field = false;
  append_payload_string_field(out, has_field, "provider", payload.provider);
  append_payload_string_field(out, has_field, "model", payload.model);
  append_payload_string_field(out, has_field, "status", payload.status);
  append_payload_string_field(out, has_field, "trigger", payload.trigger);
  append_payload_string_field(out, has_field, "reason", payload.reason);
  append_payload_number_field(out, has_field, "attempt", payload.attempt);
  append_payload_number_field(out, has_field, "max_attempts", payload.max_attempts);
  append_payload_number_field(out, has_field, "estimated_tokens", payload.estimated_tokens);
  append_payload_number_field(out, has_field, "threshold_tokens", payload.threshold_tokens);
  append_payload_number_field(out, has_field, "retained_tokens", payload.retained_tokens);
  append_payload_number_field(out, has_field, "post_compaction_tokens", payload.post_compaction_tokens);
  append_payload_number_field(out, has_field, "summary_bytes", payload.summary_bytes);
  out += '}';
  return out;
}

std::string serialize_payload_json(RetryPayload const& payload)
{
  std::string out = "{";
  bool has_field = false;
  append_payload_string_field(out, has_field, "text", payload.text);
  append_payload_string_field(out, has_field, "status", payload.status);
  append_payload_string_field(out, has_field, "category", payload.error_category);
  append_payload_string_field(out, has_field, "error_code", payload.error_code);
  append_payload_string_field(out, has_field, "message", payload.error_message);
  append_payload_string_field(out, has_field, "details", payload.error_details);
  append_payload_string_field(out, has_field, "trigger", payload.trigger);
  append_payload_string_field(out, has_field, "reason", payload.reason);
  append_payload_number_field(out, has_field, "attempt", payload.attempt);
  append_payload_number_field(out, has_field, "max_attempts", payload.max_attempts);
  append_payload_number_field(out, has_field, "delay_ms", payload.delay_ms);
  append_payload_number_field(out, has_field, "remaining_ms", payload.remaining_ms);
  out += '}';
  return out;
}

std::string serialize_payload_json(CancellationPayload const& payload)
{
  std::string out = "{";
  bool has_field = false;
  append_payload_string_field(out, has_field, "text", payload.text);
  append_payload_string_field(out, has_field, "status", payload.status);
  append_payload_string_field(out, has_field, "category", payload.error_category);
  append_payload_string_field(out, has_field, "error_code", payload.error_code);
  append_payload_string_field(out, has_field, "message", payload.error_message);
  append_payload_string_field(out, has_field, "details", payload.error_details);
  append_payload_string_field(out, has_field, "trigger", payload.trigger);
  append_payload_string_field(out, has_field, "reason", payload.reason);
  out += '}';
  return out;
}

std::string serialize_payload_json(ErrorPayload const& payload)
{
  std::string out = "{";
  bool has_field = false;
  append_payload_string_field(out, has_field, "text", payload.text);
  append_payload_string_field(out, has_field, "status", payload.status);
  append_payload_string_field(out, has_field, "category", payload.error_category);
  append_payload_string_field(out, has_field, "error_code", payload.error_code);
  append_payload_string_field(out, has_field, "message", payload.error_message);
  append_payload_string_field(out, has_field, "details", payload.error_details);
  append_payload_string_field(out, has_field, "content_type", payload.content_type);
  append_payload_string_field(out, has_field, "trigger", payload.trigger);
  append_payload_string_field(out, has_field, "reason", payload.reason);
  out += '}';
  return out;
}

std::string serialize_payload_json(CompletionPayload const& payload)
{
  std::string out = "{";
  bool has_field = false;
  append_payload_string_field(out, has_field, "status", payload.status);
  append_payload_string_field(out, has_field, "stop_reason", payload.stop_reason);
  append_payload_string_field(out, has_field, "reason", payload.reason);
  append_payload_number_field(out, has_field, "provider_iterations", payload.provider_iterations);
  append_payload_number_field(out, has_field, "tool_calls", payload.tool_calls);
  out += '}';
  return out;
}

void EventBus::subscribe(EventEnvelopeSink sink)
{
  if (sink)
    sinks_.push_back(std::move(sink));
}

ava::core::VoidResult EventBus::publish(EventEnvelope const& envelope) const
{
  auto const subscriber_count = sinks_.size();
  for (std::size_t index = 0; index < subscriber_count; ++index)
  {
    if (auto published = sinks_[index](envelope); !published)
      return std::unexpected(std::move(published.error()));
  }
  return {};
}

EventEnvelope to_event_envelope(RuntimeEvent const& event, EventEnvelopeContext const& context)
{
  auto const type = event.type();
  return EventEnvelope{.schema_version = 1,
                       .event_id = context.event_id ? *context.event_id : ava::core::make_id("event"),
                       .timestamp = event.metadata().timestamp,
                       .session_id = event.metadata().session_id,
                       .run_id = context.run_id,
                       .turn_id = context.turn_id,
                       .message_id = context.message_id,
                       .request_id = context.request_id,
                       .correlation_id = context.correlation_id,
                       .name = to_string(type),
                       .payload_json = payload_json_for_runtime_event(event),
                       .payload_type = std::string(to_string(payload_type_for_event(type)))};
}

ava::core::VoidResult emit_event(RuntimeEventSink const& sink, RuntimeEvent const& event)
{
  if (!sink)
    return {};
  return sink(event);
}

RuntimeEventSink make_runtime_event_bus_adapter(EventBus& bus, EventEnvelopeContext context, RuntimeEventSink next)
{
  return [&bus, context = std::move(context), next = std::move(next)](RuntimeEvent const& event) -> ava::core::VoidResult {
    auto envelope = to_event_envelope(event, context);
    if (auto published = bus.publish(envelope); !published)
      return std::unexpected(std::move(published.error()));
    return emit_event(next, event);
  };
}

std::string serialize_event_envelope_json(EventEnvelope const& envelope)
{
  std::string out = "{\"schema_version\":" + std::to_string(envelope.schema_version);
  append_required_string_field(out, "event_id", envelope.event_id);
  append_required_string_field(out, "timestamp", envelope.timestamp);
  append_required_string_field(out, "session_id", envelope.session_id);
  append_optional_string_field(out, "run_id", envelope.run_id);
  append_optional_string_field(out, "turn_id", envelope.turn_id);
  append_optional_string_field(out, "message_id", envelope.message_id);
  append_optional_string_field(out, "request_id", envelope.request_id);
  append_optional_string_field(out, "correlation_id", envelope.correlation_id);
  append_required_string_field(out, "name", envelope.name);
  append_required_string_field(out, "type", envelope.name);
  append_string_field(out, "payload_type", envelope.payload_type);
  out += ",\"payload\":";
  out += envelope.payload_json.empty() ? "{}" : envelope.payload_json;
  append_payload_aliases(out, envelope.payload_json.empty() ? std::string_view("{}") : envelope.payload_json);
  out += '}';
  return out;
}

std::string serialize_event_envelope_jsonl(EventEnvelope const& envelope)
{
  return serialize_event_envelope_json(envelope) + '\n';
}

}  // namespace ava::event
