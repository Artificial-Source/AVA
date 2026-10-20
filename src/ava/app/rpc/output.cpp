#include "sys.h"
#include "output.h"
#include "protocol.h"
#include "serialization.h"
#include "ava/app/runtime/Session.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"

#include <utility>
#include "debug.h"

namespace ava::app::rpc {
namespace {

std::string payload_type_for_resolver_event(std::string_view name)
{
  if (name == "permission_requested" || name == "permission_replied" || name == "permission_rule_added" || name == "permission_rule_removed" ||
      name == "permission_grant_revoked" || name == "permission_grants_cleared")
  {
    return std::string(ava::event::to_string(ava::event::PayloadType::Permission));
  }
  if (name == "question_requested" || name == "question_replied")
  {
    return std::string(ava::event::to_string(ava::event::PayloadType::Question));
  }
  if (name == "cancel_requested")
  {
    return std::string(ava::event::to_string(ava::event::PayloadType::Cancellation));
  }
  if (name == "steer_queued" || name == "steer_applied" || name == "steer_skipped" || name == "follow_up_queued" || name == "follow_up_started" ||
      name == "follow_up_skipped")
  {
    return std::string(ava::event::to_string(ava::event::PayloadType::Queue));
  }
  return {};
}

std::string normalized_record(std::string_view record)
{
  auto safe_record = ava::core::json::is_valid_utf8(record) ? std::string(record) : ava::core::json::replace_invalid_utf8(record);
  if (!ava::core::json::is_valid_utf8(safe_record))
  {
    safe_record =
        "{\"id\":\"\",\"type\":\"response\",\"success\":false,\"error\":{\"category\":\"unknown\",\"code\":\"internal_error\","
        "\"message\":\"RPC output encoding failure\",\"details\":\"RPC output encoding failure\"}}\n";
  }
  return safe_record;
}

}  // namespace

ResolverEventPayload resolver_permission_payload(std::string payload_json)
{
  return ResolverEventPayload{.payload_type = ava::event::PayloadType::Permission, .json = std::move(payload_json)};
}

ResolverEventPayload resolver_question_payload(std::string payload_json)
{
  return ResolverEventPayload{.payload_type = ava::event::PayloadType::Question, .json = std::move(payload_json)};
}

ResolverEventPayload resolver_queue_payload(std::string payload_json)
{
  return ResolverEventPayload{.payload_type = ava::event::PayloadType::Queue, .json = std::move(payload_json)};
}

//static
ava::core::Result<OutputWriteResult> Output::write_record_if(output_ts& output, std::string_view record, std::function<bool()> const& gate)
{
  DoutEntering(dc::rpc, "Output::write_record_if(output [" << (void*)&output << "], bytes=" << record.size() << ")");

  // UTF-8 normalization may allocate, so complete it before serializing on the output mutex.
  auto safe_record = normalized_record(record);
  std::function<void()> on_write_failure;
  {
    output_ts::wat output_w(output);
    if (!gate())
      return OutputWriteResult::Skipped;

    if (output_w->out_ << safe_record << std::flush)
      return OutputWriteResult::Written;

    // The handler can acquire output and pending-state mutexes. Copy it while locked, then invoke it
    // only after releasing the output lock to preserve output -> pending lock ordering.
    on_write_failure = output_w->on_write_failure_;
  }

  if (on_write_failure)
    on_write_failure();
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to write RPC JSONL record"));
}

//static
ava::core::VoidResult Output::write_record(output_ts& output, std::string_view record)
{
  auto written = write_record_if(output, record, [] { return true; });
  if (!written)
    return std::unexpected(std::move(written.error()));
  return {};
}

ava::core::VoidResult write_success(output_ts& output, std::string_view id, std::string_view result_json)
{
  return Output::write_record(output, ava::app::serialize_rpc_success_jsonl(id, result_json));
}

ava::core::VoidResult write_error(output_ts& output, std::string_view id, ava::core::Error const& error)
{
  return Output::write_record(output, ava::app::serialize_rpc_error_jsonl(id, error));
}

void subscribe_event_envelope_writer(ava::event::EventBus& bus, output_ts& output)
{
  bus.subscribe([&output](ava::event::EventEnvelope const& envelope) {
    return Output::write_record(output, ava::event::serialize_event_envelope_jsonl(envelope));
  });
}

ava::event::EventEnvelopeContext rpc_event_context(std::string_view request_id)
{
  auto const id = std::string(request_id);
  ava::event::EventEnvelopeContext context;
  context.request_id = id;
  context.correlation_id = id;
  return context;
}

std::string session_id_snapshot(runtime::session_ts const& unlocked_session)
{
  return runtime::session_ts::crat(unlocked_session)->store.options_r()->session_id;
}

ava::event::EventEnvelope resolver_event_envelope(std::string name, std::string request_id, std::string correlation_id, std::string session_id,
                                                  std::string payload_json)
{
  ava::event::EventEnvelope envelope;
  envelope.schema_version = 1;
  envelope.event_id = ava::core::make_id("event");
  envelope.timestamp = ava::session::now_timestamp();
  envelope.session_id = std::move(session_id);
  envelope.request_id = std::move(request_id);
  envelope.correlation_id = std::move(correlation_id);
  envelope.name = is_rpc_event_name(name) ? std::move(name) : std::string("error");
  envelope.payload_type = payload_type_for_resolver_event(envelope.name);
  envelope.payload_json = std::move(payload_json);
  return envelope;
}

ava::event::EventEnvelope resolver_event_envelope(std::string name, std::string request_id, std::string correlation_id, std::string session_id,
                                                  ResolverEventPayload payload)
{
  ava::event::EventEnvelope envelope;
  envelope.schema_version = 1;
  envelope.event_id = ava::core::make_id("event");
  envelope.timestamp = ava::session::now_timestamp();
  envelope.session_id = std::move(session_id);
  envelope.request_id = std::move(request_id);
  envelope.correlation_id = std::move(correlation_id);
  envelope.name = is_rpc_event_name(name) ? std::move(name) : std::string("error");
  envelope.payload_type = std::string(ava::event::to_string(payload.payload_type));
  envelope.payload_json = std::move(payload.json);
  return envelope;
}

ava::core::VoidResult write_queue_event(output_ts& output, runtime::session_ts const& unlocked_session, std::string_view name,
                                        QueuedRpcMessage const& queued, std::string_view reason)
{
  auto envelope = resolver_event_envelope(std::string(name), queued.request_id, queued.correlation_id,
                                          session_id_snapshot(unlocked_session), queued_message_payload_json(queued.message, reason));
  return Output::write_record(output, ava::event::serialize_event_envelope_jsonl(envelope));
}

ava::core::VoidResult write_skipped_queue_events(output_ts& output, runtime::session_ts const& unlocked_session, ClearedRpcQueues const& cleared,
                                                 std::string_view reason)
{
  for (auto const& queued : cleared.steering_messages)
  {
    if (auto written = write_queue_event(output, unlocked_session, "steer_skipped", queued, reason); !written)
    {
      return written;
    }
  }
  for (auto const& queued : cleared.follow_up_messages)
  {
    if (auto written = write_queue_event(output, unlocked_session, "follow_up_skipped", queued, reason); !written)
    {
      return written;
    }
  }
  return {};
}

ava::core::VoidResult write_follow_up_errors(output_ts& output, RpcRunState& run_state, std::vector<QueuedRpcMessage> const& follow_ups,
                                             std::string_view reason)
{
  for (auto const& queued : follow_ups)
  {
    auto const error = reason == "canceled" ? canceled_error() : skipped_follow_up_error(reason);
    if (auto written = write_error(output, queued.request_id, error); !written)
      return written;
    complete_outstanding_request(run_state, queued.request_id);
  }
  return {};
}

}  // namespace ava::app::rpc
