#pragma once

#include "ava/debug/print_members_on.h"
#include "EventEnvelope.h"
#include "EventEnvelopeContext.h"
#include "RuntimeEvent.h"
#include "ava/core/result.h"

#include <deque>
#include <functional>
#include <string>
#include <string_view>

namespace ava::event {

enum class PayloadType
{
  Session,
  Message,
  Reasoning,
  Provider,
  Tool,
  Compaction,
  Retry,
  Cancellation,
  Error,
  Completion,
  Permission,
  Question,
  Queue,
};

using EventEnvelopeSink = std::function<ava::core::VoidResult(EventEnvelope const&)>;

class EventBus
{
 public:
  void subscribe(EventEnvelopeSink sink);
  // Subscribers are called synchronously in registration order. Publishing stops on the first failure.
  // Subscribers added by a callback participate in the next publication.
  [[nodiscard]] ava::core::VoidResult publish(EventEnvelope const& envelope) const;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  std::deque<EventEnvelopeSink> sinks_;
};

[[nodiscard]] std::string to_string(RuntimeEventType type);
[[nodiscard]] std::string_view to_string(PayloadType type) noexcept;
[[nodiscard]] PayloadType payload_type_for_event(RuntimeEventType type) noexcept;
[[nodiscard]] std::string serialize_payload_json(SessionPayload const& payload);
[[nodiscard]] std::string serialize_payload_json(MessagePayload const& payload);
[[nodiscard]] std::string serialize_payload_json(ReasoningPayload const& payload);
[[nodiscard]] std::string serialize_payload_json(ProviderPayload const& payload);
[[nodiscard]] std::string serialize_payload_json(ToolPayload const& payload);
[[nodiscard]] std::string serialize_payload_json(CompactionPayload const& payload);
[[nodiscard]] std::string serialize_payload_json(RetryPayload const& payload);
[[nodiscard]] std::string serialize_payload_json(CancellationPayload const& payload);
[[nodiscard]] std::string serialize_payload_json(ErrorPayload const& payload);
[[nodiscard]] std::string serialize_payload_json(CompletionPayload const& payload);
[[nodiscard]] EventEnvelope to_event_envelope(RuntimeEvent const& event, EventEnvelopeContext const& context = {});
[[nodiscard]] ava::core::VoidResult emit_event(RuntimeEventSink const& sink, RuntimeEvent const& event);
// The returned sink captures `bus` by reference and must not outlive it.
[[nodiscard]] RuntimeEventSink make_runtime_event_bus_adapter(EventBus& bus, EventEnvelopeContext context = {}, RuntimeEventSink next = nullptr);
[[nodiscard]] std::string serialize_event_envelope_json(EventEnvelope const& envelope);
[[nodiscard]] std::string serialize_event_envelope_jsonl(EventEnvelope const& envelope);

}  // namespace ava::event
