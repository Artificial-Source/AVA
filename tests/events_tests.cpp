#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/event/events.h"
#include "ava/app/interactive_internal.h"
#include "ava/app/interactive_run_queue.h"
#include "ava/core/json.h"

#include <string>
#include <variant>
#include <vector>

#if defined(__GNUC__)
#pragma GCC diagnostic push
// Typed fixture payloads intentionally retain declared defaults for fields omitted by each concrete v1 event.
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace {

void test_event_envelope_serialization_is_deterministic()
{
  ava::event::EventEnvelope const envelope{.schema_version = 1,
                                           .event_id = "event_1",
                                           .timestamp = "2026-04-30T00:00:00Z",
                                           .session_id = "session_1",
                                           .run_id = "run_1",
                                           .turn_id = "turn_1",
                                           .message_id = "message_1",
                                           .request_id = "request_1",
                                           .correlation_id = "correlation_1",
                                           .name = "user_message",
                                           .payload_json = "{\"text\":\"hello\\n\\\"ava\\\"\"}"};

  auto const json = ava::event::serialize_event_envelope_json(envelope);
  expect(json ==
             "{\"schema_version\":1,\"event_id\":\"event_1\","
             "\"timestamp\":\"2026-04-30T00:00:00Z\",\"session_id\":\"session_1\","
             "\"run_id\":\"run_1\",\"turn_id\":\"turn_1\",\"message_id\":\"message_1\","
             "\"request_id\":\"request_1\",\"correlation_id\":\"correlation_1\","
             "\"name\":\"user_message\",\"type\":\"user_message\","
             "\"payload\":{\"text\":\"hello\\n\\\"ava\\\"\"},\"text\":\"hello\\n\\\"ava\\\"\"}",
         "event envelope JSON serialization uses deterministic field ordering");

  auto const jsonl = ava::event::serialize_event_envelope_jsonl(envelope);
  expect(jsonl == json + '\n', "event envelope JSONL appends one newline");
}

void test_typed_runtime_event_preserves_metadata_and_payload_shape()
{
  using namespace ava::event;
  RuntimeEvent const event(RuntimeEventMetadata{.timestamp = "2026-04-30T00:00:01Z", .session_id = "session_1"},
                           ToolResultEvent{ToolPayload{.text = "read ok", .call_id = "call_1", .tool = "read", .status = "completed"}});

  EventEnvelopeContext context;
  context.event_id = "event_2";
  context.run_id = "run_1";
  context.turn_id = "turn_2";
  auto const envelope = to_event_envelope(event, context);
  expect(envelope.schema_version == 1, "runtime event conversion sets schema version");
  expect(envelope.event_id == "event_2", "runtime event conversion uses supplied event id");
  expect(envelope.timestamp == "2026-04-30T00:00:01Z", "runtime event conversion carries timestamp");
  expect(envelope.session_id == "session_1", "runtime event conversion carries session id");
  expect(envelope.run_id == "run_1" && envelope.turn_id == "turn_2", "runtime event conversion carries optional ids");
  expect(envelope.name == "tool_result", "runtime event conversion maps the concrete alternative name");
  expect(envelope.payload_type == "tool", "runtime event conversion classifies tool payloads");
  expect(envelope.payload_json == "{\"text\":\"read ok\",\"call_id\":\"call_1\",\"tool\":\"read\",\"status\":\"completed\"}",
         "runtime event conversion serializes the authoritative typed payload");
  auto const envelope_json = serialize_event_envelope_json(envelope);
  expect(envelope_json.find("\"payload_type\":\"tool\"") != std::string::npos,
         "runtime event envelopes advertise payload family without changing payload shape");
}

void test_neutral_emit_invokes_once_and_is_null_safe()
{
  using namespace ava::event;
  RuntimeEvent const event(RuntimeEventMetadata{.timestamp = "2026-04-30T00:00:01Z", .session_id = "session_projection"},
                           ProviderEvent{ProviderPayload{.text = "provider payload", .call_id = "call_projection", .tool = "read_file", .status = "running"}});

  std::size_t calls = 0;
  auto emitted = emit_event(
      [&calls](RuntimeEvent const& emitted_event) -> ava::core::VoidResult {
        ++calls;
        auto const* provider = std::get_if<ProviderEvent>(&emitted_event.payload());
        expect(provider && emitted_event.metadata().timestamp == "2026-04-30T00:00:01Z" && emitted_event.metadata().session_id == "session_projection" &&
                   provider->payload.text == "provider payload" && provider->payload.call_id == "call_projection" && provider->payload.tool == "read_file" &&
                   provider->payload.status == "running",
               "neutral event emit retains the exact typed alternative, metadata, and payload");
        return {};
      },
      event);
  expect(emitted.has_value() && calls == 1, "neutral event emit invokes the typed sink once");

  RuntimeEventSink null_sink;
  expect(emit_event(null_sink, event).has_value(), "neutral typed emit treats a null sink as a successful no-op");
}

void test_event_bus_preserves_order_and_stops_on_first_failure()
{
  ava::event::EventBus bus;
  std::vector<std::string> calls;
  auto expected_error = ava::core::Error(ava::core::ErrorCategory::Io, "stable second sink failure").with_context("sink", "second");
  bus.subscribe([&calls](ava::event::EventEnvelope const&) {
    calls.push_back("first");
    return ava::core::VoidResult{};
  });
  bus.subscribe([&calls](ava::event::EventEnvelope const&) -> ava::core::VoidResult {
    calls.push_back("second");
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "stable second sink failure").with_context("sink", "second"));
  });
  bus.subscribe([&calls](ava::event::EventEnvelope const&) {
    calls.push_back("third");
    return ava::core::VoidResult{};
  });

  ava::event::EventEnvelope envelope;
  envelope.event_id = "event_order";
  envelope.name = "done";
  auto const published = bus.publish(envelope);
  expect(!published && calls == std::vector<std::string>({"first", "second"}),
         "EventBus calls sinks synchronously in registration order and stops on the first failure");
  expect(!published && published.error().category() == expected_error.category() && published.error().message() == expected_error.message() &&
             published.error().format() == expected_error.format(),
         "EventBus propagates the first sink failure exactly");
}

void test_runtime_event_bus_adapter_publishes_and_forwards()
{
  using namespace ava::event;
  EventBus bus;
  std::vector<std::string> call_order;
  std::vector<EventEnvelope> published;
  std::vector<RuntimeEvent> forwarded;
  bus.subscribe([&call_order, &published](EventEnvelope const& envelope) {
    call_order.push_back("publish");
    published.push_back(envelope);
    return ava::core::VoidResult{};
  });

  EventEnvelopeContext context;
  context.event_id = "event_3";
  context.correlation_id = "correlation_1";
  auto sink = make_runtime_event_bus_adapter(bus, context, [&call_order, &forwarded](RuntimeEvent const& event) {
    call_order.push_back("next");
    forwarded.push_back(event);
    return ava::core::VoidResult{};
  });

  RuntimeEvent const event(RuntimeEventMetadata{.timestamp = "2026-04-30T00:00:02Z", .session_id = "session_1"},
                           AssistantMessageEvent{MessagePayload{.text = "done"}});
  auto const emitted = sink(event);
  expect(emitted.has_value(), "runtime event bus adapter succeeds when bus and next sink succeed");
  expect(published.size() == 1 && published.front().name == "assistant_message" && published.front().correlation_id == "correlation_1",
         "runtime event bus adapter publishes converted envelopes");
  auto const* forwarded_message = forwarded.size() == 1 ? std::get_if<AssistantMessageEvent>(&forwarded.front().payload()) : nullptr;
  expect(forwarded_message && forwarded_message->payload.text == "done" && call_order == std::vector<std::string>({"publish", "next"}),
         "runtime event bus adapter publishes before forwarding typed runtime events to the next sink");
}

void test_runtime_event_bus_adapter_failure_order()
{
  using namespace ava::event;
  RuntimeEvent const event(RuntimeEventMetadata{.timestamp = "2026-04-30T00:00:03Z", .session_id = "session_1"}, CompletionEvent{});

  EventBus failing_bus;
  bool next_called = false;
  auto expected_publish_error = ava::core::Error(ava::core::ErrorCategory::Io, "stable envelope publish failure");
  failing_bus.subscribe([](EventEnvelope const&) -> ava::core::VoidResult {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "stable envelope publish failure"));
  });
  auto publish_failure_sink = make_runtime_event_bus_adapter(failing_bus, {}, [&next_called](RuntimeEvent const&) {
    next_called = true;
    return ava::core::VoidResult{};
  });
  auto const publish_failure = publish_failure_sink(event);
  expect(!publish_failure && !next_called && publish_failure.error().format() == expected_publish_error.format(),
         "runtime event bus adapter propagates envelope publish failure without calling the next sink");

  EventBus successful_bus;
  std::vector<std::string> calls;
  successful_bus.subscribe([&calls](EventEnvelope const&) {
    calls.push_back("publish");
    return ava::core::VoidResult{};
  });
  auto expected_next_error = ava::core::Error(ava::core::ErrorCategory::Tool, "stable next sink failure").with_context("sink", "next");
  auto next_failure_sink = make_runtime_event_bus_adapter(successful_bus, {}, [&calls](RuntimeEvent const&) -> ava::core::VoidResult {
    calls.push_back("next");
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "stable next sink failure").with_context("sink", "next"));
  });
  auto const next_failure = next_failure_sink(event);
  expect(!next_failure && calls == std::vector<std::string>({"publish", "next"}) && next_failure.error().format() == expected_next_error.format(),
         "runtime event bus adapter publishes first and propagates the next sink failure exactly");
}

void test_runtime_event_bus_adapter_allows_default_next_sink()
{
  using namespace ava::event;
  EventBus bus;
  std::vector<EventEnvelope> published;
  bus.subscribe([&published](EventEnvelope const& envelope) {
    published.push_back(envelope);
    return ava::core::VoidResult{};
  });

  auto sink = make_runtime_event_bus_adapter(bus);
  RuntimeEvent const event(RuntimeEventMetadata{.timestamp = "2026-04-30T00:00:03Z", .session_id = "session_1"}, CompletionEvent{});
  auto const emitted = sink(event);
  expect(emitted.has_value() && published.size() == 1 && published.front().name == "done", "runtime event bus adapter supports a null next sink");
}

void test_tool_progress_payload_serialization_and_bus_adapter()
{
  using namespace ava::event;
  ToolPayload const payload{.text = "reading", .call_id = "call_2", .tool = "read_file", .status = "running"};
  expect(serialize_payload_json(payload) == "{\"text\":\"reading\",\"call_id\":\"call_2\",\"tool\":\"read_file\",\"status\":\"running\"}",
         "tool progress uses the authoritative typed payload serializer");

  RuntimeEvent const event(RuntimeEventMetadata{.timestamp = "2026-04-30T00:00:04Z", .session_id = "session_1"}, ToolProgressEvent{payload});
  EventBus bus;
  std::vector<EventEnvelope> published;
  bus.subscribe([&published](EventEnvelope const& envelope) {
    published.push_back(envelope);
    return ava::core::VoidResult{};
  });
  EventEnvelopeContext context;
  context.event_id = "event_progress";
  auto sink = make_runtime_event_bus_adapter(bus, context);
  auto const emitted = sink(event);
  expect(emitted.has_value() && published.size() == 1 && published.front().name == "tool_progress", "event bus adapter publishes tool progress envelopes");
  expect(published.front().payload_json == serialize_payload_json(payload), "tool progress envelope keeps the typed tool payload fields");
}

void test_tool_payload_serializes_semantic_frontend_fields()
{
  using namespace ava::event;
  ToolPayload const payload{.text = "edited src/main.cpp",
                            .call_id = "call_edit",
                            .tool = "edit_file",
                            .args_json = "{\"path\":\"src/main.cpp\"}",
                            .result_json = "{\"ok\":true,\"path\":\"src/main.cpp\"}",
                            .structured_result_json =
                                "{\"schema_version\":1,\"call_id\":\"call_edit\",\"tool\":\"edit_file\",\"status\":\"success\","
                                "\"ok\":true,\"content_type\":\"application/json\",\"content\":{\"ok\":true,\"path\":\"src/main.cpp\"},"
                                "\"changed_paths\":[\"src/main.cpp\"]}",
                            .status = "success",
                            .content_type = "application/json",
                            .diff = "--- src/main.cpp\n+++ src/main.cpp\n-old\n+new",
                            .changed_paths = {"src/main.cpp", "src/ava/event/events.h"},
                            .permission_request_ids = {"permreq_edit"},
                            .spill_path = "/tmp/ava-spill/tool.txt",
                            .diff_truncated = true,
                            .truncated = true,
                            .byte_limited = true,
                            .line_limited = true,
                            .spill_truncated = true,
                            .output_bytes = 128,
                            .total_bytes = 512,
                            .output_lines = 4,
                            .total_lines = 11,
                            .start_line = 2,
                            .end_line = 5,
                            .next_offset_line = 6,
                            .omitted_bytes = 384,
                            .omitted_lines = 7};

  auto const json = serialize_payload_json(payload);
  expect(json.find("\"args\":{\"path\":\"src/main.cpp\"}") != std::string::npos &&
             json.find("\"result\":{\"ok\":true,\"path\":\"src/main.cpp\"}") != std::string::npos &&
             json.find("\"structured_result\":{\"schema_version\":1") != std::string::npos &&
             json.find("\"content_type\":\"application/json\"") != std::string::npos &&
             json.find("\"changed_paths\":[\"src/main.cpp\",\"src/ava/event/events.h\"]") != std::string::npos &&
             json.find("\"permission_request_ids\":[\"permreq_edit\"]") != std::string::npos && json.find("\"diff_truncated\":true") != std::string::npos &&
             json.find("\"byte_limited\":true") != std::string::npos && json.find("\"line_limited\":true") != std::string::npos &&
             json.find("\"output_lines\":4") != std::string::npos && json.find("\"next_offset_line\":6") != std::string::npos &&
             json.find("\"omitted_lines\":7") != std::string::npos,
         "typed tool payloads serialize semantic args, result, permission ids, diffs, and truncation metadata");

  RuntimeEvent const event(RuntimeEventMetadata{}, ToolResultEvent{payload});
  auto const envelope = to_event_envelope(event, EventEnvelopeContext{.event_id = "event_semantic_tool"});
  auto const args = ava::core::json::object_field(envelope.payload_json, "args");
  auto const result = ava::core::json::object_field(envelope.payload_json, "result");
  auto const structured_result = ava::core::json::object_field(envelope.payload_json, "structured_result");
  auto const paths = ava::core::json::strings_in_array_field(envelope.payload_json, "changed_paths");
  auto const permission_ids = ava::core::json::strings_in_array_field(envelope.payload_json, "permission_request_ids");
  expect(envelope.name == "tool_result" && args && *args == "{\"path\":\"src/main.cpp\"}" && result && *result == "{\"ok\":true,\"path\":\"src/main.cpp\"}" &&
             structured_result && envelope.payload_type == "tool" && structured_result->find("\"status\":\"success\"") != std::string::npos &&
             paths.size() == 2 && paths[1] == "src/ava/event/events.h" && permission_ids.size() == 1 && permission_ids[0] == "permreq_edit" &&
             envelope.payload_json.find("\"output_lines\":4") != std::string::npos && envelope.payload_json.find("\"total_lines\":11") != std::string::npos &&
             envelope.payload_json.find("\"next_offset_line\":6") != std::string::npos &&
             envelope.payload_json.find("\"spill_path\":\"/tmp/ava-spill/tool.txt\"") != std::string::npos,
         "tool event envelopes preserve semantic payloads for frontend replay");
}

void test_high_risk_typed_payloads_preserve_wire_shapes()
{
  using namespace ava::event;
  ToolPayload const tool_payload{.text = "read ok",
                                 .call_id = "call_read",
                                 .tool = "read_file",
                                 .args_json = "{\"path\":\"README.md\"}",
                                 .result_json = "{\"ok\":true}",
                                 .structured_result_json = "{\"schema_version\":1,\"ok\":true}",
                                 .status = "success",
                                 .content_type = "application/json",
                                 .changed_paths = {"README.md"},
                                 .permission_request_ids = {"permreq_read"},
                                 .output_lines = 2,
                                 .total_lines = 3};
  auto const tool_json = serialize_payload_json(tool_payload);
  expect(tool_json ==
             "{\"text\":\"read ok\",\"call_id\":\"call_read\",\"tool\":\"read_file\","
             "\"args\":{\"path\":\"README.md\"},\"result\":{\"ok\":true},"
             "\"structured_result\":{\"schema_version\":1,\"ok\":true},\"status\":\"success\","
             "\"content_type\":\"application/json\",\"changed_paths\":[\"README.md\"],"
             "\"permission_request_ids\":[\"permreq_read\"],\"output_lines\":2,\"total_lines\":3}",
         "typed tool payload preserves the existing JSON field contract");
  expect(to_event_envelope(RuntimeEvent(RuntimeEventMetadata{}, ToolResultEvent{tool_payload})).payload_json == tool_json,
         "tool event envelopes use the typed payload serializer");

  RetryPayload const retry_payload{.text = "HTTP status 429",
                                   .status = "request",
                                   .trigger = "provider_transport",
                                   .reason = "rate_limited",
                                   .attempt = 2,
                                   .max_attempts = 3,
                                   .delay_ms = 1000,
                                   .remaining_ms = 250};
  auto const retry_json = serialize_payload_json(retry_payload);
  expect(retry_json ==
             "{\"text\":\"HTTP status 429\",\"status\":\"request\",\"trigger\":\"provider_transport\","
             "\"reason\":\"rate_limited\",\"attempt\":2,\"max_attempts\":3,\"delay_ms\":1000,"
             "\"remaining_ms\":250}",
         "typed retry payload preserves retry timing fields");
  expect(to_event_envelope(RuntimeEvent(RuntimeEventMetadata{}, RetryTickEvent{retry_payload})).payload_json == retry_json,
         "retry event envelopes use the typed payload serializer");

  CancellationPayload const cancellation_payload{.text = "stopped by user",
                                                 .error_category = "canceled",
                                                 .error_message = "agent loop canceled",
                                                 .error_details = "canceled: agent loop canceled",
                                                 .reason = "agent loop canceled"};
  auto const cancellation_json = serialize_payload_json(cancellation_payload);
  expect(cancellation_json ==
             "{\"text\":\"stopped by user\",\"category\":\"canceled\",\"message\":\"agent loop canceled\","
             "\"details\":\"canceled: agent loop canceled\",\"reason\":\"agent loop canceled\"}",
         "typed cancellation payload preserves cancellation reason fields");
  expect(to_event_envelope(RuntimeEvent(RuntimeEventMetadata{}, CancellationEvent{cancellation_payload})).payload_json == cancellation_json,
         "cancellation event envelopes use the typed payload serializer");

  ErrorPayload const error_payload{.error_category = "io",
                                   .error_code = "write_failed",
                                   .error_message = "failed to write RPC JSONL record",
                                   .error_details = "io: failed to write RPC JSONL record"};
  auto const error_json = serialize_payload_json(error_payload);
  expect(error_json ==
             "{\"category\":\"io\",\"error_code\":\"write_failed\","
             "\"message\":\"failed to write RPC JSONL record\","
             "\"details\":\"io: failed to write RPC JSONL record\"}",
         "typed error payload preserves error diagnostic fields");
  expect(to_event_envelope(RuntimeEvent(RuntimeEventMetadata{}, ErrorEvent{error_payload})).payload_json == error_json,
         "error event envelopes use the typed payload serializer");

  CompletionPayload const completion_payload{.stop_reason = "completed", .provider_iterations = 2, .tool_calls = 1};
  auto const completion_json = serialize_payload_json(completion_payload);
  expect(completion_json == "{\"stop_reason\":\"completed\",\"provider_iterations\":2,\"tool_calls\":1}",
         "typed completion payload preserves usage summary counters");
  expect(to_event_envelope(RuntimeEvent(RuntimeEventMetadata{}, CompletionEvent{completion_payload})).payload_json == completion_json,
         "completion event envelopes use the typed payload serializer");
}

void test_tool_and_cancellation_event_envelopes_have_golden_wire_shapes()
{
  using namespace ava::event;
  ToolPayload const success_payload_value{.text = "wrote note",
                                          .call_id = "call_write",
                                          .tool = "write_file",
                                          .args_json = "{\"path\":\"note.txt\"}",
                                          .result_json = "{\"ok\":true,\"path\":\"note.txt\"}",
                                          .structured_result_json =
                                              "{\"schema_version\":1,\"call_id\":\"call_write\",\"tool\":\"write_file\",\"status\":\"success\","
                                              "\"ok\":true,\"content_type\":\"application/json\",\"content\":{\"ok\":true,\"path\":\"note.txt\"},"
                                              "\"changed_paths\":[\"note.txt\"],\"permission_request_ids\":[\"permreq_write\"]}",
                                          .status = "success",
                                          .content_type = "application/json",
                                          .diff = "--- note.txt\n+++ note.txt\n-old\n+new",
                                          .changed_paths = {"note.txt"},
                                          .permission_request_ids = {"permreq_write"},
                                          .spill_path = "spill/tool.txt",
                                          .diff_truncated = true,
                                          .truncated = true,
                                          .byte_limited = true,
                                          .line_limited = true,
                                          .spill_truncated = true,
                                          .output_bytes = 128,
                                          .total_bytes = 512,
                                          .output_lines = 2,
                                          .total_lines = 5,
                                          .start_line = 1,
                                          .end_line = 2,
                                          .next_offset_line = 3,
                                          .omitted_bytes = 384,
                                          .omitted_lines = 3};
  RuntimeEvent const success_event(RuntimeEventMetadata{.timestamp = "2026-05-07T00:00:00Z", .session_id = "session_golden"},
                                   ToolResultEvent{success_payload_value});
  auto const success_payload = std::string{"{\"text\":\"wrote note\",\"call_id\":\"call_write\",\"tool\":\"write_file\","} +
                               "\"args\":{\"path\":\"note.txt\"},\"result\":{\"ok\":true,\"path\":\"note.txt\"}," +
                               "\"structured_result\":{\"schema_version\":1,\"call_id\":\"call_write\",\"tool\":\"write_file\"," +
                               "\"status\":\"success\",\"ok\":true,\"content_type\":\"application/json\"," +
                               "\"content\":{\"ok\":true,\"path\":\"note.txt\"},\"changed_paths\":[\"note.txt\"]," +
                               "\"permission_request_ids\":[\"permreq_write\"]},\"status\":\"success\"," +
                               "\"content_type\":\"application/json\",\"diff\":\"--- note.txt\\n+++ note.txt\\n-old\\n+new\"," +
                               "\"changed_paths\":[\"note.txt\"],\"permission_request_ids\":[\"permreq_write\"]," +
                               "\"spill_path\":\"spill/tool.txt\",\"diff_truncated\":true,\"truncated\":true," +
                               "\"byte_limited\":true,\"line_limited\":true,\"spill_truncated\":true,\"output_bytes\":128," +
                               "\"total_bytes\":512,\"output_lines\":2,\"total_lines\":5,\"start_line\":1,\"end_line\":2," +
                               "\"next_offset_line\":3,\"omitted_bytes\":384,\"omitted_lines\":3}";
  auto const success_envelope = to_event_envelope(success_event, EventEnvelopeContext{.event_id = "event_tool_success"});
  auto const success_json = serialize_event_envelope_json(success_envelope);
  auto const expected_success_json = std::string{"{\"schema_version\":1,\"event_id\":\"event_tool_success\","} +
                                     "\"timestamp\":\"2026-05-07T00:00:00Z\",\"session_id\":\"session_golden\"," +
                                     "\"name\":\"tool_result\",\"type\":\"tool_result\",\"payload_type\":\"tool\",\"payload\":" + success_payload +
                                     ",\"text\":\"wrote note\",\"call_id\":\"call_write\",\"tool\":\"write_file\",\"status\":\"success\"," +
                                     "\"content_type\":\"application/json\",\"diff\":\"--- note.txt\\n+++ note.txt\\n-old\\n+new\"," +
                                     "\"spill_path\":\"spill/tool.txt\",\"output_bytes\":128,\"total_bytes\":512,\"output_lines\":2," +
                                     "\"total_lines\":5,\"start_line\":1,\"end_line\":2,\"next_offset_line\":3," + "\"omitted_bytes\":384,\"omitted_lines\":3}";
  expect(success_envelope.payload_type == "tool" && success_envelope.payload_json == success_payload && success_json == expected_success_json,
         "successful tool event envelope locks structured result, permission ids, diffs, truncation, and spill metadata");

  ToolPayload const denied_payload_value{.text = "permission denied",
                                         .call_id = "call_denied",
                                         .tool = "write_file",
                                         .structured_result_json =
                                             "{\"schema_version\":1,\"call_id\":\"call_denied\",\"tool\":\"write_file\",\"status\":\"error\","
                                             "\"ok\":false,\"summary\":\"permission denied\",\"content_type\":\"text/plain\","
                                             "\"content\":\"permission denied\",\"error\":{\"category\":\"permission\",\"code\":\"permission_denied\","
                                             "\"message\":\"permission denied\",\"details\":\"resolution: deny\"}}",
                                         .status = "error",
                                         .error_category = "permission",
                                         .error_code = "permission_denied",
                                         .error_message = "permission denied",
                                         .error_details = "resolution: deny",
                                         .content_type = "text/plain"};
  RuntimeEvent const denied_event(RuntimeEventMetadata{.timestamp = "2026-05-07T00:00:01Z", .session_id = "session_golden"},
                                  ToolResultEvent{denied_payload_value});
  auto const denied_payload = std::string{"{\"text\":\"permission denied\",\"call_id\":\"call_denied\",\"tool\":\"write_file\","} +
                              "\"structured_result\":{\"schema_version\":1,\"call_id\":\"call_denied\",\"tool\":\"write_file\"," +
                              "\"status\":\"error\",\"ok\":false,\"summary\":\"permission denied\",\"content_type\":\"text/plain\"," +
                              "\"content\":\"permission denied\",\"error\":{\"category\":\"permission\",\"code\":\"permission_denied\"," +
                              "\"message\":\"permission denied\",\"details\":\"resolution: deny\"}},\"status\":\"error\"," +
                              "\"category\":\"permission\",\"error_code\":\"permission_denied\",\"message\":\"permission denied\"," +
                              "\"details\":\"resolution: deny\",\"content_type\":\"text/plain\"}";
  auto const denied_envelope = to_event_envelope(denied_event, EventEnvelopeContext{.event_id = "event_tool_denied"});
  auto const denied_json = serialize_event_envelope_json(denied_envelope);
  auto const expected_denied_json = std::string{"{\"schema_version\":1,\"event_id\":\"event_tool_denied\","} +
                                    "\"timestamp\":\"2026-05-07T00:00:01Z\",\"session_id\":\"session_golden\"," +
                                    "\"name\":\"tool_result\",\"type\":\"tool_result\",\"payload_type\":\"tool\",\"payload\":" + denied_payload +
                                    ",\"text\":\"permission denied\",\"call_id\":\"call_denied\",\"tool\":\"write_file\"," +
                                    "\"status\":\"error\",\"category\":\"permission\",\"error_code\":\"permission_denied\"," +
                                    "\"message\":\"permission denied\",\"details\":\"resolution: deny\",\"content_type\":\"text/plain\"}";
  auto const denied_structured = ava::core::json::object_field(denied_envelope.payload_json, "structured_result");
  expect(denied_envelope.payload_type == "tool" && denied_envelope.payload_json == denied_payload && denied_json == expected_denied_json && denied_structured &&
             denied_structured->find("\"status\":\"error\"") != std::string::npos && denied_structured->find("\"ok\":false") != std::string::npos,
         "denied tool event envelope locks structured error fields with status/ok consistency");

  CancellationPayload const canceled_payload_value{.text = "stopped by user",
                                                   .status = "canceled",
                                                   .error_category = "canceled",
                                                   .error_code = "user_canceled",
                                                   .error_message = "agent loop canceled",
                                                   .error_details = "canceled at permission wait",
                                                   .trigger = "rpc_cancel",
                                                   .reason = "user_requested"};
  RuntimeEvent const canceled_event(RuntimeEventMetadata{.timestamp = "2026-05-07T00:00:02Z", .session_id = "session_golden"},
                                    CancellationEvent{canceled_payload_value});
  auto const canceled_payload =
      "{\"text\":\"stopped by user\",\"status\":\"canceled\",\"category\":\"canceled\","
      "\"error_code\":\"user_canceled\",\"message\":\"agent loop canceled\","
      "\"details\":\"canceled at permission wait\",\"trigger\":\"rpc_cancel\",\"reason\":\"user_requested\"}";
  auto const canceled_envelope = to_event_envelope(canceled_event, EventEnvelopeContext{.event_id = "event_canceled"});
  auto const canceled_json = serialize_event_envelope_json(canceled_envelope);
  auto const expected_canceled_json = std::string{"{\"schema_version\":1,\"event_id\":\"event_canceled\","} +
                                      "\"timestamp\":\"2026-05-07T00:00:02Z\",\"session_id\":\"session_golden\"," +
                                      "\"name\":\"canceled\",\"type\":\"canceled\",\"payload_type\":\"cancellation\",\"payload\":" + canceled_payload +
                                      ",\"text\":\"stopped by user\",\"status\":\"canceled\",\"category\":\"canceled\"," +
                                      "\"error_code\":\"user_canceled\",\"message\":\"agent loop canceled\"," +
                                      "\"details\":\"canceled at permission wait\",\"trigger\":\"rpc_cancel\",\"reason\":\"user_requested\"}";
  expect(canceled_envelope.payload_type == "cancellation" && canceled_envelope.payload_json == canceled_payload && canceled_json == expected_canceled_json,
         "canceled event envelope locks payload_type, status, trigger, and reason fields");
}

void test_reasoning_event_serialization_hides_provider_private_state()
{
  using namespace ava::event;
  ReasoningPayload const payload{
      .text = "visible reasoning summary", .reasoning_format = "anthropic_thinking", .reasoning_redacted = true, .reasoning_signature_present = true};
  auto const payload_json = serialize_payload_json(payload);
  expect(payload_json ==
             "{\"text\":\"visible reasoning summary\",\"reasoning_format\":\"anthropic_thinking\",\"reasoning_redacted\":true,"
             "\"reasoning_signature_present\":true}",
         "reasoning payload serializes visible text, format, and bounded boolean metadata only");

  RuntimeEvent const event(RuntimeEventMetadata{.timestamp = "2026-04-30T00:00:05Z", .session_id = "session_1"}, ReasoningDeltaEvent{payload});
  auto const envelope = to_event_envelope(event, EventEnvelopeContext{.event_id = "event_reasoning"});
  expect(envelope.name == "reasoning_delta" && envelope.payload_type == "reasoning" && envelope.payload_json == payload_json,
         "reasoning envelope carries frontend-visible reasoning payload");
  auto const envelope_json = serialize_event_envelope_json(envelope);
  expect(envelope_json.find("super-secret-signature") == std::string::npos && envelope_json.find("reasoning_signature\":") == std::string::npos &&
             envelope_json.find("\"signature\":") == std::string::npos,
         "reasoning frontend event envelope never exposes provider-private signatures");
}

void test_lifecycle_event_serialization_and_payload_mapping()
{
  using namespace ava::event;
  CompactionPayload const payload{.status = "completed",
                                  .trigger = "context_overflow",
                                  .attempt = 1,
                                  .max_attempts = 2,
                                  .estimated_tokens = 9000,
                                  .threshold_tokens = 8000,
                                  .summary_bytes = 512};
  auto const payload_json = serialize_payload_json(payload);
  expect(payload_json ==
             "{\"status\":\"completed\",\"trigger\":\"context_overflow\",\"attempt\":1,\"max_attempts\":2,"
             "\"estimated_tokens\":9000,\"threshold_tokens\":8000,\"summary_bytes\":512}",
         "compaction payload serializes backend-owned compaction metadata");

  RuntimeEvent const event(RuntimeEventMetadata{.timestamp = "2026-04-30T00:00:06Z", .session_id = "session_1"}, CompactionEndEvent{payload});
  auto const envelope = to_event_envelope(event, EventEnvelopeContext{.event_id = "event_compaction"});
  auto const envelope_json = serialize_event_envelope_json(envelope);
  expect(envelope.name == "compaction_end" && envelope.payload_type == "compaction" &&
             envelope.payload_json.find("\"trigger\":\"context_overflow\"") != std::string::npos &&
             envelope.payload_json.find("\"max_attempts\":2") != std::string::npos && envelope_json.find("\"summary_bytes\":512") != std::string::npos,
         "compaction lifecycle envelope preserves payload and top-level aliases for stream clients");

  RuntimeEvent const canceled(RuntimeEventMetadata{}, CancellationEvent{CancellationPayload{.reason = "cancel_requested"}});
  expect(to_event_envelope(canceled).name == "canceled", "explicit cancellation events have a stable shared stream name");

  RuntimeEvent const retry_tick(
      RuntimeEventMetadata{},
      RetryTickEvent{
          RetryPayload{.trigger = "provider_transport", .reason = "rate_limited", .attempt = 2, .max_attempts = 3, .delay_ms = 1000, .remaining_ms = 500}});
  auto const retry_tick_json = serialize_event_envelope_json(to_event_envelope(retry_tick));
  expect(retry_tick_json.find("\"type\":\"retry_tick\"") != std::string::npos && retry_tick_json.find("\"remaining_ms\":500") != std::string::npos,
         "retry countdown tick events serialize explicit backend timing data");
  expect(to_string(payload_type_for_event(RuntimeEventType::SessionStart)) == "session" &&
             to_string(payload_type_for_event(RuntimeEventType::AssistantMessage)) == "message" &&
             to_string(payload_type_for_event(RuntimeEventType::RetryTick)) == "retry" &&
             to_string(payload_type_for_event(RuntimeEventType::Canceled)) == "cancellation" && to_string(PayloadType::Permission) == "permission" &&
             to_string(PayloadType::Question) == "question" && to_string(PayloadType::Queue) == "queue" &&
             to_string(payload_type_for_event(RuntimeEventType::Done)) == "completion",
         "runtime event types map to stable payload families");
}

void test_interactive_run_queue_emits_steer_queued_and_applied_events()
{
  std::vector<ava::event::EventEnvelope> events;
  ava::app::InteractiveRunQueue queue("session_queue", "request_active", [&events](ava::event::EventEnvelope const& envelope) {
    events.push_back(envelope);
    return ava::core::VoidResult{};
  });

  auto queued = queue.queue_steering("adjust this turn");
  expect(queued.has_value(), "interactive run queue accepts a bounded active-run steering message");
  expect(events.size() == 1 && events.back().name == "steer_queued" && events.back().correlation_id == "request_active" && events.back().request_id &&
             *events.back().request_id != "request_active" && events.back().payload_json.find("\"message\":\"adjust this turn\"") != std::string::npos,
         "interactive run queue emits a correlated steer queued event");

  auto taken = queue.take_steering_messages();
  expect(taken.has_value() && taken->size() == 1 && taken->front() == "adjust this turn",
         "interactive run queue returns queued steering at the backend safe point");
  expect(events.size() == 2 && events.back().name == "steer_applied" && events.back().correlation_id == "request_active",
         "interactive run queue emits a steer applied event when consumed by the backend");
}

void test_interactive_run_queue_runs_follow_up_lifecycle()
{
  std::vector<ava::event::EventEnvelope> events;
  ava::app::InteractiveRunQueue queue("session_queue", "request_active", [&events](ava::event::EventEnvelope const& envelope) {
    events.push_back(envelope);
    return ava::core::VoidResult{};
  });

  auto queued = queue.queue_follow_up("next turn");
  auto next = queue.take_next_follow_up();
  auto started = next ? queue.mark_follow_up_started(*next) : ava::core::VoidResult{};
  auto steer_after_start = queue.queue_steering("steer follow-up");

  expect(queued.has_value() && next && next->message == "next turn" && started.has_value() && steer_after_start.has_value(),
         "interactive run queue accepts and starts a queued follow-up");
  expect(events.size() == 3 && events[0].name == "follow_up_queued" && events[1].name == "follow_up_started" && events[1].request_id == next->request_id &&
             events[1].correlation_id == next->request_id && events[2].name == "steer_queued" && events[2].correlation_id == next->request_id,
         "interactive run queue retargets active correlation when a follow-up starts");
}

void test_interactive_tui_stops_follow_ups_at_session_transition()
{
  std::vector<ava::event::EventEnvelope> events;
  ava::app::InteractiveRunQueue queue("session_old", "request_active", [&events](ava::event::EventEnvelope const& envelope) {
    events.push_back(envelope);
    return ava::core::VoidResult{};
  });
  static_cast<void>(queue.queue_follow_up("/new"));
  static_cast<void>(queue.queue_follow_up("OLD-FOLLOW-UP-MUST-NOT-RUN"));

  ava::tui::TuiSubmitContext context;
  context.skip_active_steering = [&queue](std::string_view reason) { return queue.skip_active_steering(reason); };
  context.take_next_follow_up = [&queue]() -> std::optional<ava::tui::TuiQueuedFollowUp> {
    auto next = queue.take_next_follow_up();
    if (!next)
      return std::nullopt;
    return ava::tui::TuiQueuedFollowUp{.request_id = next->request_id, .message = next->message};
  };
  context.mark_follow_up_started = [&queue](ava::tui::TuiQueuedFollowUp const& follow_up) {
    return queue.mark_follow_up_started(
        ava::app::InteractiveQueuedMessage{.request_id = follow_up.request_id, .correlation_id = follow_up.request_id, .message = follow_up.message});
  };

  auto session_id = std::string("session_old");
  auto result = ava::app::interactive_internal::InteractiveResult{
      .ordinary_turn_committed = true,
      .output = {"OLD-OUTPUT-MUST-NOT-SURVIVE"},
      .tool_timeline = {{.name = "old_tool"}},
  };
  bool workspace_catalog_reload = false;
  std::vector<std::string> executed;
  std::vector<std::string> executed_request_ids;
  auto const transitioned = ava::app::interactive_internal::run_queued_follow_ups_until_session_transition(
      result, workspace_catalog_reload, "session_old", context, [&session_id]() { return session_id; },
      [&](ava::tui::TuiQueuedFollowUp const& follow_up) {
        executed.push_back(follow_up.message);
        executed_request_ids.push_back(follow_up.request_id);
        if (follow_up.message == "/new")
        {
          session_id = "session_new";
          return ava::app::interactive_internal::InteractiveResult{
              .session_tree_changed = true,
              .output = {"NEW-SESSION-RECEIPT"},
              .tool_timeline = {{.name = "transition_tool"}},
          };
        }
        return ava::app::interactive_internal::InteractiveResult{.output = {"OLD-LATE-OUTPUT"}, .tool_timeline = {{.name = "old_late_tool"}}};
      });
  auto const finished = queue.finish(false);

  expect(transitioned && finished.has_value() && executed == std::vector<std::string>{"/new"} && executed_request_ids.size() == 1 && events.size() >= 3 &&
             executed_request_ids.front() == events[2].request_id && result.session_tree_changed && result.ordinary_turn_committed &&
             result.output == std::vector<std::string>{"NEW-SESSION-RECEIPT"} && result.tool_timeline.size() == 1 &&
             result.tool_timeline.front().name == "transition_tool" && events.size() == 4 && events[2].name == "follow_up_started" &&
             events[3].name == "follow_up_skipped" && events[3].payload_json.find("OLD-FOLLOW-UP-MUST-NOT-RUN") != std::string::npos &&
             events[3].payload_json.find("run_completed_before_safe_point") != std::string::npos,
         "interactive TUI follow-ups stop at the first authoritative session change, retain only the transition receipt/tool fallback, preserve control flags, "
         "and let normal queue finish skip the old-session remainder");
}

void test_interactive_run_queue_skips_pending_messages_on_finish()
{
  std::vector<ava::event::EventEnvelope> events;
  ava::app::InteractiveRunQueue queue("session_queue", "request_active", [&events](ava::event::EventEnvelope const& envelope) {
    events.push_back(envelope);
    return ava::core::VoidResult{};
  });

  auto steer = queue.queue_steering("too late");
  auto follow = queue.queue_follow_up("also too late");
  auto finished = queue.finish(false);
  auto after_finish = queue.queue_follow_up("must not queue");

  expect(steer.has_value() && follow.has_value() && finished.has_value(), "interactive run queue finishes cleanly");
  expect(!after_finish.has_value(), "interactive run queue rejects messages after active run finish");
  expect(events.size() == 4 && events[2].name == "steer_skipped" && events[3].name == "follow_up_skipped" &&
             events.back().payload_json.find("\"reason\":\"run_completed_before_safe_point\"") != std::string::npos,
         "interactive run queue emits skipped events for unconsumed messages");
}

void test_interactive_run_queue_restores_latest_pending_message()
{
  std::vector<ava::event::EventEnvelope> events;
  ava::app::InteractiveRunQueue queue("session_queue", "request_active", [&events](ava::event::EventEnvelope const& envelope) {
    events.push_back(envelope);
    return ava::core::VoidResult{};
  });

  auto follow = queue.queue_follow_up("first follow-up");
  auto steer = queue.queue_steering("latest steer");
  auto restored = queue.restore_latest();
  auto taken = queue.take_steering_messages();
  auto next = queue.take_next_follow_up();

  expect(follow.has_value() && steer.has_value() && restored.has_value() && restored->steering && restored->message == "latest steer",
         "interactive run queue restores the latest queued message with its kind");
  expect(taken.has_value() && taken->empty() && next && next->message == "first follow-up",
         "interactive run queue removes restored steering without disturbing older follow-up messages");
  expect(events.size() == 3 && events.back().name == "steer_skipped" &&
             events.back().payload_json.find("\"reason\":\"restored_to_composer\"") != std::string::npos,
         "interactive run queue emits a skipped event when restoring a queued message to the composer");
}

void test_interactive_run_queue_bounds_and_truncates_event_payloads()
{
  std::vector<ava::event::EventEnvelope> events;
  ava::app::InteractiveRunQueue queue("session_queue", "request_active", [&events](ava::event::EventEnvelope const& envelope) {
    events.push_back(envelope);
    return ava::core::VoidResult{};
  });

  std::string const long_message(ava::app::kMaxInteractiveQueueEventMessageBytes + 16, 'x');
  auto queued = queue.queue_steering(long_message);
  std::string const too_large(ava::app::kMaxInteractiveQueuedMessageBytes + 1, 'y');
  auto rejected = queue.queue_steering(too_large);

  expect(queued.has_value(), "interactive run queue accepts messages within the aggregate byte limit");
  expect(!rejected.has_value(), "interactive run queue rejects oversized messages");
  expect(events.size() == 1 && events.back().name == "steer_queued" && events.back().payload_json.find("\"message_truncated\":true") != std::string::npos &&
             events.back().payload_json.find("\"message_bytes\":") != std::string::npos,
         "interactive run queue truncates event payloads without truncating queued content");
  auto taken = queue.take_steering_messages();
  expect(taken.has_value() && taken->size() == 1 && taken->front().size() == long_message.size(),
         "interactive run queue preserves full content for backend consumption");
}

}  // namespace

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void run_app_event_bus_tests()
{
  test_event_envelope_serialization_is_deterministic();
  test_typed_runtime_event_preserves_metadata_and_payload_shape();
  test_neutral_emit_invokes_once_and_is_null_safe();
  test_event_bus_preserves_order_and_stops_on_first_failure();
  test_runtime_event_bus_adapter_publishes_and_forwards();
  test_runtime_event_bus_adapter_failure_order();
  test_runtime_event_bus_adapter_allows_default_next_sink();
  test_tool_progress_payload_serialization_and_bus_adapter();
  test_tool_payload_serializes_semantic_frontend_fields();
  test_high_risk_typed_payloads_preserve_wire_shapes();
  test_tool_and_cancellation_event_envelopes_have_golden_wire_shapes();
  test_reasoning_event_serialization_hides_provider_private_state();
  test_lifecycle_event_serialization_and_payload_mapping();
  test_interactive_run_queue_emits_steer_queued_and_applied_events();
  test_interactive_run_queue_runs_follow_up_lifecycle();
  test_interactive_tui_stops_follow_ups_at_session_transition();
  test_interactive_run_queue_skips_pending_messages_on_finish();
  test_interactive_run_queue_restores_latest_pending_message();
  test_interactive_run_queue_bounds_and_truncates_event_payloads();
}
