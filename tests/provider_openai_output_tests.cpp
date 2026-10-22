#include "sys.h"
#include "tests/provider_openai_test_suite.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/agent/assistant_turn.h"
#include "ava/agent/message_builder.h"
#include "ava/agent/stream_bridge.h"
#include "ava/session/assistant_output.h"
#include "ava/session/session_store.h"
#include "ava/provider/openai_response_parser.h"
#include "ava/provider/provider.h"
#include "ava/core/json.h"
#include "ava/core/result.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ava::tests::provider_openai_suite {

void test_openai_ordered_output_capture()
{
  auto const stream = ava::provider::parse_openai_sse(
      "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"rs_order_0\",\"type\":\"reasoning\",\"output_index\":0}}\n\n"
      "data: {\"type\":\"response.reasoning_summary_text.delta\",\"item_id\":\"rs_order_0\",\"delta\":\"inspect\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"rs_order_0\",\"type\":\"reasoning\",\"output_index\":0,\"summary\":[{"
      "\"type\":\"summary_text\",\"text\":\"inspect\"}]}}\n\n"
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":1,\"item\":{\"id\":\"msg_commentary\",\"type\":\"message\",\"output_index\":1,\"phase\":"
      "\"commentary\"}}\n\n"
      "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_commentary\",\"output_index\":1,\"delta\":\"Checking.\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{\"id\":\"msg_commentary\",\"type\":\"message\",\"output_index\":1,\"phase\":"
      "\"commentary\"}}\n\n"
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":2,\"item\":{\"id\":\"fc_order_0\",\"type\":\"function_call\",\"output_index\":2,\"call_id\":"
      "\"call_order_0\",\"name\":\"read_file\",\"arguments\":\"\"}}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":2,\"item\":{\"id\":\"fc_order_0\",\"type\":\"function_call\",\"output_index\":2,\"call_id\":"
      "\"call_order_0\",\"name\":\"read_file\",\"arguments\":\"{}\"}}\n\n"
      "data: {\"type\":\"response.output_item.added\",\"output_index\":3,\"item\":{\"id\":\"rs_order_1\",\"type\":\"reasoning\",\"output_index\":3}}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":3,\"item\":{\"id\":\"rs_order_1\",\"type\":\"reasoning\",\"output_index\":3,\"summary\":[{"
      "\"type\":\"summary_text\",\"text\":\"verify\"}]}}\n\n"
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":4,\"item\":{\"id\":\"msg_final\",\"type\":\"message\",\"output_index\":4,\"phase\":\"final_"
      "answer\"}}\n\n"
      "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_final\",\"output_index\":4,\"delta\":\"Done.\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":4,\"item\":{\"id\":\"msg_final\",\"type\":\"message\",\"output_index\":4,\"phase\":\"final_"
      "answer\"}}\n\n"
      "data: [DONE]\n\n");
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto const non_stream = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"status":"completed","output":[{"id":"rs_order_0","type":"reasoning","output_index":0,"summary":[{"type":"summary_text","text":"inspect"}]},{"id":"msg_commentary","type":"message","output_index":1,"phase":"commentary","content":[{"type":"output_text","text":"Checking."}]},{"id":"fc_order_0","type":"function_call","output_index":2,"call_id":"call_order_0","name":"read_file","arguments":"{}"},{"id":"rs_order_1","type":"reasoning","output_index":3,"summary":[{"type":"summary_text","text":"verify"}]},{"id":"msg_final","type":"message","output_index":4,"phase":"final_answer","content":[{"type":"output_text","text":"Done."}]}]})"},
      false);
  expect(stream && non_stream && stream->size() == non_stream->size(), "OpenAI stream and non-stream output-item capture have matching event counts");
  if (stream && non_stream && stream->size() == non_stream->size())
  {
    bool parity = true;
    for (std::size_t index = 0; index < stream->size(); ++index)
    {
      auto const& streamed = (*stream)[index];
      auto const& buffered = (*non_stream)[index];
      parity = parity && streamed.type == buffered.type && streamed.text == buffered.text && streamed.provider_item_id == buffered.provider_item_id &&
               streamed.provider_output_index == buffered.provider_output_index && streamed.assistant_phase == buffered.assistant_phase;
    }
    expect(parity, "OpenAI stream and non-stream output-item capture preserve lifecycle order, IDs, indexes, and phases");
  }

  auto const parsed = stream ? ava::agent::parse_assistant_turn(*stream, {}) : ava::core::Result<ava::agent::ParsedAssistantTurn>{};
  expect(parsed && parsed->ordered_items.size() == 5 && parsed->text == "Checking.Done." && parsed->reasoning_blocks.size() == 2 &&
             parsed->tool_calls.size() == 1 && parsed->tool_calls[0].id == "call_order_0",
         "assistant turn derives legacy aggregates from one canonical ordered OpenAI item sequence");
  if (parsed && parsed->ordered_items.size() == 5)
  {
    auto const* commentary = std::get_if<ava::agent::AssistantTextItem>(&parsed->ordered_items[1].item);
    auto const* function = std::get_if<ava::agent::AssistantFunctionCallItem>(&parsed->ordered_items[2].item);
    auto const* final = std::get_if<ava::agent::AssistantTextItem>(&parsed->ordered_items[4].item);
    expect(commentary && function && final && commentary->metadata.provider_item_id == "msg_commentary" && commentary->metadata.provider_output_index == 1 &&
               commentary->metadata.phase == ava::provider::AssistantPhase::Commentary && function->metadata.provider_item_id == "fc_order_0" &&
               function->tool_call.id == "call_order_0" && final->metadata.provider_item_id == "msg_final" &&
               final->metadata.phase == ava::provider::AssistantPhase::FinalAnswer,
           "assistant turn retains provider item identity separately from logical call IDs and assistant phase");
  }

  auto const adjacent_messages = ava::provider::parse_openai_sse(
      "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_adjacent_a\",\"type\":\"message\",\"phase\":\"commentary\"}}\n\n"
      "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_adjacent_a\",\"delta\":\"A\"}\n\n"
      "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"msg_adjacent_a\",\"type\":\"message\",\"phase\":\"commentary\"}}\n\n"
      "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_adjacent_b\",\"type\":\"message\",\"phase\":\"commentary\"}}\n\n"
      "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_adjacent_b\",\"delta\":\"B\"}\n\n"
      "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"msg_adjacent_b\",\"type\":\"message\",\"phase\":\"commentary\"}}\n\n"
      "data: [DONE]\n\n");
  auto const adjacent_turn =
      adjacent_messages ? ava::agent::parse_assistant_turn(*adjacent_messages, {}) : ava::core::Result<ava::agent::ParsedAssistantTurn>{};
  expect(adjacent_turn && adjacent_turn->ordered_items.size() == 2 && adjacent_turn->text == "AB",
         "adjacent same-phase OpenAI messages remain distinct ordered items");
  if (adjacent_turn && adjacent_turn->ordered_items.size() == 2)
  {
    auto const* first = std::get_if<ava::agent::AssistantTextItem>(&adjacent_turn->ordered_items[0].item);
    auto const* second = std::get_if<ava::agent::AssistantTextItem>(&adjacent_turn->ordered_items[1].item);
    expect(first && second && first->metadata.provider_item_id == "msg_adjacent_a" && second->metadata.provider_item_id == "msg_adjacent_b" &&
               first->metadata.phase == ava::provider::AssistantPhase::Commentary && second->metadata.phase == ava::provider::AssistantPhase::Commentary,
           "adjacent same-phase messages retain their separate provider IDs");
  }

  auto const duplicate_index = ava::provider::parse_openai_sse(
      "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_index_a\",\"type\":\"message\",\"phase\":\"commentary\"}}\n\n"
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_index_b\",\"type\":\"message\",\"phase\":\"commentary\"}}\n\n");
  auto const duplicate_id = ava::provider::parse_openai_sse(
      "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_duplicate\",\"type\":\"message\",\"phase\":\"commentary\"}}\n\n"
      "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_duplicate\",\"type\":\"message\",\"phase\":\"commentary\"}}\n\n");
  auto has_error = [](auto const& parsed_events) {
    return parsed_events &&
           std::any_of(parsed_events->begin(), parsed_events->end(), [](auto const& event) { return event.type == ava::provider::StreamEventType::Error; });
  };
  expect(has_error(duplicate_index) && has_error(duplicate_id), "OpenAI documented output lifecycles reject duplicate output indexes and IDs");

  auto event = [](ava::provider::StreamEventType type) {
    auto value = ava::provider::StreamEvent{};
    value.type = type;
    return value;
  };
  auto unbalanced_start = event(ava::provider::StreamEventType::TextStart);
  unbalanced_start.provider_item_id = "msg_unbalanced";
  unbalanced_start.assistant_phase = ava::provider::AssistantPhase::Commentary;
  auto completed = event(ava::provider::StreamEventType::Done);
  completed.finish_reason = ava::provider::ProviderFinishReason::Completed;
  auto legacy_delta = event(ava::provider::StreamEventType::TextDelta);
  legacy_delta.text = "legacy";
  auto const unbalanced = ava::agent::parse_assistant_turn({unbalanced_start, completed}, {});
  auto const legacy = ava::agent::parse_assistant_turn({legacy_delta, completed}, {});
  expect(!unbalanced && legacy && legacy->ordered_items.size() == 1 && legacy->text == "legacy",
         "assistant turn rejects unbalanced native text items while retaining legacy TextDelta synthesis");

  auto const zero_item =
      provider.parse_response(ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = R"({"status":"completed","output":[]})"}, false);
  auto const zero_turn = zero_item ? ava::agent::parse_assistant_turn(*zero_item, {}) : ava::core::Result<ava::agent::ParsedAssistantTurn>{};
  expect(zero_item && zero_item->size() == 1 && zero_turn && zero_turn->ordered_items.empty(),
         "OpenAI non-stream parser preserves an explicit zero-item terminal response");
}

void test_v4_ordered_turn_persistence_replay_and_openai_serialization()
{
  auto const root = create_empty_root("openai-v4-ordered-turn");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store({.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "ordered"});
  auto lease = ava::session::SessionLease::create_and_acquire(store.session_path());
  expect(lease.has_value(), "v4 ordered replay fixture acquires its persistent session lease");
  if (!lease)
    return;
  auto target = ava::session::SessionAppendTarget::create_persistent(store, *lease, ava::session::SessionReadLimits{});
  expect(target.has_value(), "v4 ordered replay fixture creates one guarded append target");
  if (!target)
    return;

  ava::agent::ParsedAssistantTurn turn;
  turn.ordered_items = {
      {.sequence = 0,
       .item =
           ava::agent::AssistantReasoningItem{
               .metadata = {.provider_item_id = "rs_v4_0", .provider_output_index = 0, .phase = ava::provider::AssistantPhase::Unknown},
               .reasoning = {.text = "inspect",
                             .format = "openai_responses",
                             .signature = "",
                             .redacted_data = "",
                             .native_item_json = R"({"id":"rs_v4_0","type":"reasoning","summary":[]})",
                             .redacted = false}}},
      {.sequence = 1,
       .item =
           ava::agent::AssistantTextItem{
               .metadata = {.provider_item_id = "msg_v4_commentary", .provider_output_index = 1, .phase = ava::provider::AssistantPhase::Commentary},
               .text = "Checking."}},
      {.sequence = 2,
       .item =
           ava::agent::AssistantFunctionCallItem{
               .metadata = {.provider_item_id = "fc_v4_0", .provider_output_index = 2, .phase = ava::provider::AssistantPhase::Unknown},
               .tool_call = {.id = "call_v4_0", .name = "read_file", .arguments_json = R"({"path":"one.txt"})"}}},
      {.sequence = 3,
       .item =
           ava::agent::AssistantFunctionCallItem{
               .metadata = {.provider_item_id = "fc_v4_1", .provider_output_index = 3, .phase = ava::provider::AssistantPhase::Unknown},
               .tool_call = {.id = "call_v4_1", .name = "list_directory", .arguments_json = R"({"path":"."})"}}},
      {.sequence = 4,
       .item =
           ava::agent::AssistantReasoningItem{
               .metadata = {.provider_item_id = "rs_v4_1", .provider_output_index = 4, .phase = ava::provider::AssistantPhase::Unknown},
               .reasoning = {.text = "verify",
                             .format = "openai_responses",
                             .signature = "",
                             .redacted_data = "",
                             .native_item_json = R"({"id":"rs_v4_1","type":"reasoning","summary":[]})",
                             .redacted = false}}},
      {.sequence = 5,
       .item =
           ava::agent::AssistantTextItem{
               .metadata = {.provider_item_id = "msg_v4_final", .provider_output_index = 5, .phase = ava::provider::AssistantPhase::FinalAnswer},
               .text = "Done."}},
  };
  turn.finish_reason = ava::provider::ProviderFinishReason::Completed;
  auto append_batch = [append_target = *target](std::vector<ava::session::SessionEntry> entries) { return append_target->append_batch(std::move(entries)); };
  auto appended = ava::agent::append_assistant_turn(append_batch, turn, "openai", "gpt-5.5", {}, std::nullopt, "openai_responses", "openai_responses");
  expect(appended.has_value() && appended->function_output_entry_ids_by_call_id.size() == 2,
         "one v4 batch persists all ordered output items and returns exact function bindings");
  if (!appended)
    return;

  auto append_entry = [append_target = *target](ava::session::SessionEntry entry) { return append_target->append(entry); };
  auto second_result =
      ava::agent::append_tool_result(append_entry, {.call_id = "call_v4_1", .name = "list_directory", .success = true, .result_text = "second result"},
                                     appended->function_output_entry_ids_by_call_id.at("call_v4_1"));
  auto first_result =
      ava::agent::append_tool_result(append_entry, {.call_id = "call_v4_0", .name = "read_file", .success = true, .result_text = "first result"},
                                     appended->function_output_entry_ids_by_call_id.at("call_v4_0"));
  expect(second_result && first_result, "v4 tool results persist their exact output-item bindings even when physical completion order differs");

  auto staging_data = ava::session::serialize_assistant_output_item_data_json(
      {.assistant_turn_id = "staged_v4_tail",
       .sequence = 0,
       .kind = ava::session::AssistantOutputItemKind::Text,
       .provider_item_id = std::nullopt,
       .provider_output_index = std::nullopt,
       .payload = ava::session::AssistantOutputText{.text = "must stay hidden", .assistant_phase = ava::session::AssistantOutputTextPhase::Commentary}});
  expect(staging_data.has_value(), "v4 incomplete-tail fixture serializes a valid staging item");
  if (staging_data)
  {
    auto staged = (*target)->append({.id = "staged_v4_tail_entry",
                                     .parent_id = "",
                                     .type = ava::session::EntryType::AssistantOutputItem,
                                     .timestamp = ava::session::now_timestamp(),
                                     .data_json = std::move(*staging_data)});
    expect(static_cast<bool>(staged), "guarded append target accepts a valid incomplete final staging suffix");
  }

  auto restarted = ava::session::SessionStore::open(workspace, "ordered", root / "sessions");
  auto entries = restarted ? restarted->load() : ava::core::Result<std::vector<ava::session::SessionEntry>>{};
  auto projection = entries ? ava::session::classify_assistant_output(*entries) : ava::session::AssistantOutputProjection{};
  auto const exact_item_order = projection.turns.size() == 1 && projection.turns.front().items.size() == 6 &&
                                projection.turns.front().items[0].item.provider_item_id.value_or("") == "rs_v4_0" &&
                                projection.turns.front().items[1].item.provider_item_id.value_or("") == "msg_v4_commentary" &&
                                projection.turns.front().items[2].item.provider_item_id.value_or("") == "fc_v4_0" &&
                                projection.turns.front().items[3].item.provider_item_id.value_or("") == "fc_v4_1" &&
                                projection.turns.front().items[4].item.provider_item_id.value_or("") == "rs_v4_1" &&
                                projection.turns.front().items[5].item.provider_item_id.value_or("") == "msg_v4_final";
  auto const exact_tool_bindings = entries && std::count_if(entries->begin(), entries->end(), [&](auto const& entry) {
                                                return entry.type == ava::session::EntryType::ToolResult &&
                                                       ((ava::core::json::string_field(entry.data_json, "call_id").value_or("") == "call_v4_0" &&
                                                         ava::core::json::string_field(entry.data_json, "assistant_output_entry_id").value_or("") ==
                                                             appended->function_output_entry_ids_by_call_id.at("call_v4_0")) ||
                                                        (ava::core::json::string_field(entry.data_json, "call_id").value_or("") == "call_v4_1" &&
                                                         ava::core::json::string_field(entry.data_json, "assistant_output_entry_id").value_or("") ==
                                                             appended->function_output_entry_ids_by_call_id.at("call_v4_1")));
                                              }) == 2;
  expect(entries && exact_item_order && exact_tool_bindings &&
             std::none_of(entries->begin(), entries->end(),
                          [](auto const& entry) {
                            return entry.type == ava::session::EntryType::AssistantMessage || entry.type == ava::session::EntryType::ReasoningBlock ||
                                   entry.type == ava::session::EntryType::ToolCall;
                          }),
         "restart retains exact v4 item order and tool-result bindings with no duplicate v3 assistant records");
  if (!entries)
    return;

  auto messages = ava::agent::build_provider_messages_from_entries(
      *entries, ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = "openai",
                                                                                          .model_id = "gpt-5.5",
                                                                                          .api_family = "openai_responses",
                                                                                          .reasoning_format = "openai_responses",
                                                                                          .supports_tools = true,
                                                                                          .supports_images = true}});
  expect(messages && messages->size() == 2 && (*messages)[0].role == "assistant" && (*messages)[0].content_parts.size() == 6 &&
             (*messages)[0].content_parts[1].provider_item_id == "msg_v4_commentary" &&
             (*messages)[0].content_parts[1].assistant_phase == ava::provider::AssistantPhase::Commentary &&
             (*messages)[0].content_parts[5].provider_item_id == "msg_v4_final" &&
             (*messages)[0].content_parts[5].assistant_phase == ava::provider::AssistantPhase::FinalAnswer && (*messages)[1].role == "user" &&
             (*messages)[1].content_parts.size() == 2 && (*messages)[1].content_parts[0].tool_call_id == "call_v4_0" &&
             (*messages)[1].content_parts[1].tool_call_id == "call_v4_1" && (*messages)[0].content.find("must stay hidden") == std::string::npos,
         "restart reconstruction preserves provider identities, phases, and logical function-result order while ignoring incomplete staging");
  if (!messages)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto request = provider.build_request({.provider_id = "openai",
                                         .model_id = "gpt-5.5",
                                         .system_prompt = "system",
                                         .messages = *messages,
                                         .tools_json = {},
                                         .stream = true,
                                         .max_output_tokens = std::nullopt,
                                         .reasoning = std::nullopt,
                                         .system_prompt_cache_ttl = ""},
                                        "token");
  auto const body = request ? request->body : std::string{};
  auto const commentary = body.find(R"({"type":"message","id":"msg_v4_commentary")");
  auto const first_function = body.find(R"({"type":"function_call","id":"fc_v4_0","call_id":"call_v4_0")");
  auto const second_function = body.find(R"({"type":"function_call","id":"fc_v4_1","call_id":"call_v4_1")");
  auto const final = body.find(R"({"type":"message","id":"msg_v4_final")");
  auto const first_output = body.find(R"({"type":"function_call_output","call_id":"call_v4_0")");
  auto const second_output = body.find(R"({"type":"function_call_output","call_id":"call_v4_1")");
  expect(request && body.find(R"({"id":"rs_v4_0","type":"reasoning")") != std::string::npos && commentary != std::string::npos &&
             first_function != std::string::npos && second_function != std::string::npos &&
             body.find(R"({"id":"rs_v4_1","type":"reasoning")") != std::string::npos && final != std::string::npos &&
             body.find(R"("phase":"commentary")") != std::string::npos && body.find(R"("phase":"final_answer")") != std::string::npos &&
             commentary < first_function && first_function < second_function && second_function < final && first_output < second_output,
         "OpenAI replay serializes exact ordered reasoning, commentary/final message IDs and phases, function IDs, and logical result order");

  std::string const long_message_id(80, 'x');
  ava::provider::ChatMessage terminal_text_only{.role = "assistant",
                                                .content = "progress\n\nfinal",
                                                .content_parts = {
                                                    {.type = ava::provider::ContentPartType::Text,
                                                     .text = "progress",
                                                     .provider_item_id = "msg_terminal_commentary",
                                                     .assistant_phase = ava::provider::AssistantPhase::Commentary},
                                                    {.type = ava::provider::ContentPartType::Text,
                                                     .text = "final",
                                                     .provider_item_id = long_message_id,
                                                     .assistant_phase = ava::provider::AssistantPhase::FinalAnswer},
                                                }};
  auto text_only_request = provider.build_request({.provider_id = "openai",
                                                   .model_id = "gpt-5.5",
                                                   .system_prompt = "system",
                                                   .messages = {terminal_text_only},
                                                   .tools_json = {},
                                                   .stream = true,
                                                   .max_output_tokens = std::nullopt,
                                                   .reasoning = std::nullopt,
                                                   .system_prompt_cache_ttl = ""},
                                                  "token");
  auto const text_only_body = text_only_request ? text_only_request->body : std::string{};
  auto const terminal_commentary = text_only_body.find(R"({"type":"message","id":"msg_terminal_commentary")");
  auto const terminal_final = text_only_body.find("{\"type\":\"message\",\"id\":\"msg_ava_");
  expect(text_only_request && terminal_commentary != std::string::npos && terminal_final != std::string::npos && terminal_commentary < terminal_final &&
             text_only_body.find(long_message_id) == std::string::npos && text_only_body.find(R"("phase":"commentary")") != std::string::npos &&
             text_only_body.find(R"("phase":"final_answer")") != std::string::npos,
         "OpenAI replays terminal commentary/final text without tool pairs and bounds overlong opaque message IDs deterministically");
}

void test_openai_documented_message_reconciliation()
{
  auto has_error_message = [](auto const& events, std::string_view message) {
    return events && std::any_of(events->begin(), events->end(), [message](auto const& event) {
             return event.type == ava::provider::StreamEventType::Error && event.error_message == message;
           });
  };
  auto const stream = ava::provider::parse_openai_sse(
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_done_only\",\"type\":\"message\",\"phase\":\"commentary\"}}\n\n"
      "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_done_only\",\"output_index\":0,\"text\":\"done-only\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_done_only\",\"type\":\"message\",\"phase\":null,\"content\":[{"
      "\"type\":\"output_text\",\"text\":\"done-only\"}]}}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"usage\":{\"input_tokens\":2,\"output_tokens\":3,\"total_tokens\":5}}}\n\n");
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto const non_stream = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"status":"completed","usage":{"input_tokens":2,"output_tokens":3,"total_tokens":5},"output":[{"id":"msg_done_only","type":"message","phase":"commentary","content":[{"type":"output_text","text":"done-only"}]}]})"},
      false);
  expect(stream && non_stream && stream->size() == non_stream->size(), "OpenAI stream and non-stream parsers produce matching done-only message lifecycles");
  if (stream && non_stream && stream->size() == non_stream->size())
  {
    bool parity = true;
    for (std::size_t index = 0; index < stream->size(); ++index)
    {
      auto const& streamed = (*stream)[index];
      auto const& buffered = (*non_stream)[index];
      parity = parity && streamed.type == buffered.type && streamed.text == buffered.text && streamed.provider_item_id == buffered.provider_item_id &&
               streamed.provider_output_index == buffered.provider_output_index && streamed.assistant_phase == buffered.assistant_phase &&
               streamed.finish_reason == buffered.finish_reason;
    }
    expect(parity && (*stream)[2].assistant_phase == ava::provider::AssistantPhase::Commentary && (*stream)[3].usage && (*stream)[3].usage->total_tokens == 5,
           "OpenAI message completion preserves the added phase and terminal usage across stream and non-stream parsing");
  }

  auto const missing_phase = ava::provider::parse_openai_sse(
      "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_missing_phase\",\"type\":\"message\"}}\n\n"
      "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"msg_missing_phase\",\"type\":\"message\",\"phase\":null}}\n\n"
      "data: [DONE]\n\n");
  auto const null_phase = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"status":"completed","output":[{"id":"msg_null_phase","type":"message","phase":null,"content":[{"type":"output_text","text":"null phase"}]}]})"},
      false);
  auto const non_stream_missing_phase = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"status":"completed","output":[{"id":"msg_non_stream_missing_phase","type":"message","content":[{"type":"output_text","text":"missing phase"}]}]})"},
      false);
  auto const invalid_stream_phase = ava::provider::parse_openai_sse(
      "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_invalid_phase\",\"type\":\"message\",\"phase\":{}}}\n\n"
      "data: [DONE]\n\n");
  auto const unknown_non_stream_phase = provider.parse_response(
      ava::http::HttpResponse{.status_code = 200,
                              .headers = {},
                              .body = R"({"status":"completed","output":[{"id":"msg_unknown_phase","type":"message","phase":"not_a_phase","content":[]}]})"},
      false);
  expect(missing_phase && missing_phase->size() == 3 && (*missing_phase)[0].assistant_phase == ava::provider::AssistantPhase::Unknown &&
             (*missing_phase)[1].assistant_phase == ava::provider::AssistantPhase::Unknown && null_phase && null_phase->size() == 4 &&
             (*null_phase)[0].assistant_phase == ava::provider::AssistantPhase::Unknown &&
             (*null_phase)[1].assistant_phase == ava::provider::AssistantPhase::Unknown && non_stream_missing_phase &&
             (*non_stream_missing_phase)[0].assistant_phase == ava::provider::AssistantPhase::Unknown &&
             has_error_message(invalid_stream_phase, "OpenAI message output item has an empty or invalid phase") &&
             has_error_message(unknown_non_stream_phase, "OpenAI message output item has an unknown phase"),
         "OpenAI missing and explicit-null message phases normalize to Unknown while non-null invalid phases are rejected");

  auto const resolved_stream_phase = ava::provider::parse_openai_sse(
      "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_resolved_phase\",\"type\":\"message\",\"phase\":null}}\n\n"
      "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_resolved_phase\",\"output_index\":0,\"delta\":\"resolved\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_resolved_phase\",\"type\":\"message\",\"phase\":"
      "\"final_answer\",\"content\":[{\"type\":\"output_text\",\"text\":\"resolved\"}]}}\n\n"
      "data: [DONE]\n\n");
  auto const resolved_non_stream_phase = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"status":"completed","output":[{"id":"msg_resolved_phase","type":"message","phase":"final_answer","content":[{"type":"output_text","text":"resolved"}]}]})"},
      false);
  auto const resolved_stream_turn =
      resolved_stream_phase ? ava::agent::parse_assistant_turn(*resolved_stream_phase, {}) : ava::core::Result<ava::agent::ParsedAssistantTurn>{};
  auto const resolved_non_stream_turn =
      resolved_non_stream_phase ? ava::agent::parse_assistant_turn(*resolved_non_stream_phase, {}) : ava::core::Result<ava::agent::ParsedAssistantTurn>{};
  auto resolved_phase = [](auto const& turn) {
    if (!turn || turn->ordered_items.size() != 1)
      return ava::provider::AssistantPhase::Unknown;
    auto const* text = std::get_if<ava::agent::AssistantTextItem>(&turn->ordered_items.front().item);
    return text ? text->metadata.phase : ava::provider::AssistantPhase::Unknown;
  };
  expect(resolved_stream_turn && resolved_non_stream_turn && resolved_stream_turn->text == resolved_non_stream_turn->text &&
             resolved_phase(resolved_stream_turn) == ava::provider::AssistantPhase::FinalAnswer &&
             resolved_phase(resolved_non_stream_turn) == ava::provider::AssistantPhase::FinalAnswer,
         "OpenAI stream completion resolves an initially unknown message phase to the same canonical phase as non-stream capture");

  auto const suffix = ava::provider::parse_openai_sse(
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_suffix\",\"type\":\"message\",\"phase\":\"final_answer\"}}\n\n"
      "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_suffix\",\"output_index\":0,\"delta\":\"hel\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_suffix\",\"type\":\"message\",\"content\":[{\"type\":\"output_"
      "text\","
      "\"text\":\"hello\"}]}}\n\n"
      "data: [DONE]\n\n");
  auto const suffix_turn =
      suffix ? ava::agent::parse_assistant_turn(*suffix, {.max_assistant_text_bytes = 5}) : ava::core::Result<ava::agent::ParsedAssistantTurn>{};
  expect(suffix && suffix->size() == 5 && (*suffix)[1].text == "hel" && (*suffix)[2].type == ava::provider::StreamEventType::TextDelta &&
             (*suffix)[2].text == "lo" && suffix_turn && suffix_turn->text == "hello",
         "OpenAI output_item.done appends only missing message text before TextEnd and retains turn byte accounting");
  auto const done_only_limit =
      stream ? ava::agent::parse_assistant_turn(*stream, {.max_assistant_text_bytes = 8}) : ava::core::Result<ava::agent::ParsedAssistantTurn>{};
  expect(!done_only_limit && done_only_limit.error().message().find("assistant text byte limit exceeded") != std::string::npos,
         "OpenAI done-only message text remains subject to assistant turn output limits");

  auto const conflict = ava::provider::parse_openai_sse(
      "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_conflict\",\"type\":\"message\",\"phase\":\"commentary\"}}\n\n"
      "data: {\"type\":\"response.output_text.delta\",\"item_id\":\"msg_conflict\",\"delta\":\"actual\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"item\":{\"id\":\"msg_conflict\",\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"different\"}]}}\n\n"
      "data: [DONE]\n\n");
  auto const duplicate = ava::provider::parse_openai_sse(
      "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"msg_duplicate_done\",\"type\":\"message\",\"phase\":\"commentary\"}}\n\n"
      "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_duplicate_done\",\"text\":\"complete\"}\n\n"
      "data: {\"type\":\"response.output_text.done\",\"item_id\":\"msg_duplicate_done\",\"text\":\"complete\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"item\":{\"id\":\"msg_duplicate_done\",\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":"
      "\"complete\"}]}}\n\n"
      "data: [DONE]\n\n");
  expect(has_error_message(conflict, "conflicting OpenAI message text") && has_error_message(duplicate, "OpenAI message text completed more than once"),
         "OpenAI message completion rejects conflicting and duplicate terminal content in terminally complete streams");
}

void test_openai_non_stream_output_order_and_strictness()
{
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto const convenience_with_native_message = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"status":"completed","output_text":"convenience text must not duplicate","usage":{"input_tokens":2,"output_tokens":3,"total_tokens":5},"output":[{"id":"msg_physical_zero","type":"message","phase":"final_answer","content":[{"type":"output_text","text":"native message"}]},{"id":"fc_physical_one","type":"function_call","call_id":"call_physical_one","name":"read_file","arguments":"{}"}]})"},
      false);
  expect(convenience_with_native_message && convenience_with_native_message->size() == 7 &&
             (*convenience_with_native_message)[0].type == ava::provider::StreamEventType::TextStart &&
             (*convenience_with_native_message)[1].text == "native message" && (*convenience_with_native_message)[1].provider_output_index == 0 &&
             (*convenience_with_native_message)[3].provider_output_index == 1 && (*convenience_with_native_message)[6].usage &&
             (*convenience_with_native_message)[6].usage->total_tokens == 5 &&
             (*convenience_with_native_message)[6].finish_reason == ava::provider::ProviderFinishReason::Completed &&
             std::none_of(convenience_with_native_message->begin(), convenience_with_native_message->end(),
                          [](auto const& event) { return event.text.find("convenience text") != std::string::npos; }),
         "OpenAI non-stream parsing follows physical output order and never fabricates a convenience text item or index");

  auto const mismatched_native_index = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"status":"completed","output":[{"id":"msg_wrong_index","type":"message","output_index":4,"content":[{"type":"output_text","text":"no"}]}]})"},
      false);
  auto const malformed_output = provider.parse_response(
      ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = R"({"status":"completed","output":[{"type":"message"},false]})"}, false);
  auto has_error = [](auto const& events, std::string_view message) {
    return events && std::any_of(events->begin(), events->end(), [message](auto const& event) {
             return event.type == ava::provider::StreamEventType::Error && event.error_message == message;
           });
  };
  expect(has_error(mismatched_native_index, "OpenAI output item output_index disagrees with its physical output array position") &&
             has_error(malformed_output, "OpenAI response parser limit exceeded"),
         "OpenAI non-stream output extraction rejects mismatched indexes and non-object arrays without exposing provider contents");
}

void test_openai_documented_function_completion_validation()
{
  auto has_error_message = [](auto const& events, std::string_view message) {
    return events && std::any_of(events->begin(), events->end(), [message](auto const& event) {
             return event.type == ava::provider::StreamEventType::Error && event.error_message == message;
           });
  };
  auto const conflicting_index = ava::provider::parse_openai_sse(
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"fc_index\",\"type\":\"function_call\",\"call_id\":\"call_index\","
      "\"name\":\"read_file\",\"arguments\":\"\"}}\n\n"
      "data: {\"type\":\"response.function_call_arguments.done\",\"item_id\":\"fc_index\",\"output_index\":1,\"arguments\":\"{}\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"fc_index\",\"type\":\"function_call\",\"call_id\":\"call_index\","
      "\"name\":"
      "\"read_file\",\"arguments\":\"{}\"}}\n\n"
      "data: [DONE]\n\n");
  auto const duplicate_item_done = ava::provider::parse_openai_sse(
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"fc_duplicate_done\",\"type\":\"function_call\",\"call_id\":\"call_"
      "duplicate_done\",\"name\":\"read_file\",\"arguments\":\"\"}}\n\n"
      "data: {\"type\":\"response.function_call_arguments.done\",\"item_id\":\"fc_duplicate_done\",\"output_index\":0,\"arguments\":\"{}\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"fc_duplicate_done\",\"type\":\"function_call\",\"call_id\":\"call_"
      "duplicate_done\",\"name\":\"read_file\",\"arguments\":\"{}\"}}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"fc_duplicate_done\",\"type\":\"function_call\",\"call_id\":\"call_"
      "duplicate_done\",\"name\":\"read_file\",\"arguments\":\"{}\"}}\n\n"
      "data: [DONE]\n\n");
  auto const post_completion_argument = ava::provider::parse_openai_sse(
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"fc_after_done\",\"type\":\"function_call\",\"call_id\":\"call_after_"
      "done\",\"name\":\"read_file\",\"arguments\":\"\"}}\n\n"
      "data: {\"type\":\"response.function_call_arguments.done\",\"item_id\":\"fc_after_done\",\"output_index\":0,\"arguments\":\"{}\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"fc_after_done\",\"type\":\"function_call\",\"call_id\":\"call_after_"
      "done\",\"name\":\"read_file\",\"arguments\":\"{}\"}}\n\n"
      "data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_after_done\",\"output_index\":0,\"delta\":\"{}\"}\n\n"
      "data: [DONE]\n\n");
  expect(has_error_message(conflicting_index, "OpenAI function call arguments changed their output_index") &&
             has_error_message(duplicate_item_done, "OpenAI function call output item completed more than once") &&
             has_error_message(post_completion_argument, "OpenAI function call arguments emitted after completion"),
         "OpenAI documented function events validate indexes and reject duplicate or post-completion events before terminal completion");
}

void test_openai_stream_bridge_hides_internal_text_lifecycle_events()
{
  std::vector<ava::provider::StreamEvent> observed;
  ava::agent::AgentLoopOptions options;
  options.on_stream_event = [&observed](ava::provider::StreamEvent const& event) -> ava::core::VoidResult {
    observed.push_back(event);
    return {};
  };
  auto start = ava::provider::StreamEvent{};
  start.type = ava::provider::StreamEventType::TextStart;
  start.provider_item_id = "msg_private";
  start.provider_output_index = 0;
  auto delta = start;
  delta.type = ava::provider::StreamEventType::TextDelta;
  delta.text = "public text";
  delta.assistant_phase = ava::provider::AssistantPhase::FinalAnswer;
  auto end = start;
  end.type = ava::provider::StreamEventType::TextEnd;
  auto done = ava::provider::StreamEvent{};
  done.type = ava::provider::StreamEventType::Done;
  done.finish_reason = ava::provider::ProviderFinishReason::Completed;
  auto const start_result = ava::agent::publish_stream_event(options, start);
  auto const delta_result = ava::agent::publish_stream_event(options, delta);
  auto const end_result = ava::agent::publish_stream_event(options, end);
  auto const done_result = ava::agent::publish_stream_event(options, done);
  expect(start_result && delta_result && end_result && done_result && observed.size() == 2 && observed[0].type == ava::provider::StreamEventType::TextDelta &&
             observed[0].assistant_phase == ava::provider::AssistantPhase::Unknown && observed[0].provider_item_id.empty() &&
             !observed[0].provider_output_index && observed[1].type == ava::provider::StreamEventType::Done,
         "public stream callbacks receive text deltas and terminal events without internal lifecycle or phase metadata");
}

void test_openai_incremental_sse_parser()
{
  ava::provider::OpenAIStreamParser parser;
  std::vector<ava::provider::StreamEvent> events;
  auto append = [&](std::string_view chunk) {
    auto parsed = parser.append(chunk);
    expect(parsed.has_value(), "incremental OpenAI SSE chunk parses");
    if (parsed)
      events.insert(events.end(), parsed->begin(), parsed->end());
  };

  append("data: {\"type\":\"response.output_text.delta\",\"del");
  expect(events.empty(), "incremental OpenAI SSE waits for a complete split frame");
  append("ta\":\"he");
  append("llo\"}\n\n");
  append("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_1\",\"name\":\"bash\"}\n\n");
  append("data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_1\",\"delta\":\"{\"}\n");
  append("\n");
  append("data: {not-json}\n\n");
  append("data: {\"type\":\"response.error\",\"error\":{\"message\":\"bad stream\"}}\n\n");
  append("data: [DONE]");
  auto final_events = parser.finish();
  expect(final_events.has_value(), "incremental OpenAI SSE finish parses pending data");
  if (final_events)
    events.insert(events.end(), final_events->begin(), final_events->end());

  expect(events.size() == 6, "incremental OpenAI SSE emits text, tool, error, and done events");
  if (events.size() == 6)
  {
    expect(events[0].type == ava::provider::StreamEventType::TextDelta && events[0].text == "hello", "incremental OpenAI SSE preserves split text delta");
    expect(events[1].type == ava::provider::StreamEventType::ToolCallStart && events[1].tool_call_id == "call_1" && events[1].tool_name == "bash",
           "incremental OpenAI SSE preserves tool call start");
    expect(events[2].type == ava::provider::StreamEventType::ToolCallDelta && events[2].text == "{", "incremental OpenAI SSE preserves tool argument deltas");
    expect(events[3].type == ava::provider::StreamEventType::Error, "incremental OpenAI SSE reports malformed data as an error event");
    expect(events[4].type == ava::provider::StreamEventType::Error && events[4].error_message == "OpenAI provider reported a streaming error",
           "incremental OpenAI SSE replaces provider error messages with fixed local diagnostics");
    expect(events[5].type == ava::provider::StreamEventType::Done, "incremental OpenAI SSE emits done on finish");
  }

  // Byte-sized chunks split CRLF pairs and UTF-8 code points as well as JSON.
  // Comments and multi-line data fields must not change the emitted content.
  std::string const framed =
      ": keepalive\r\n"
      "data:{\"type\":\"response.output_text.delta\",\"delta\":\"café 🌍\"}\r\n\r\n"
      ": another comment\r\n"
      "data: {\"type\":\"response.output_text.delta\",\r\n"
      "data: \"delta\":\"!\"}\r\n\r\n"
      "data:[DONE]\r\n\r\n"
      "data: [DONE]\r\n\r\n";
  auto const whole = ava::provider::parse_openai_sse(framed);
  if (!ava::tests::require(whole.has_value() && whole->size() == 3, "OpenAI framing fixture emits exactly two text deltas and one terminal"))
    return;
  ava::tests::expect_all("OpenAI framing semantics are independent of chunking",
                         {{"UTF-8 text", (*whole)[0].type == ava::provider::StreamEventType::TextDelta && (*whole)[0].text == "café 🌍"},
                          {"multiline data", (*whole)[1].type == ava::provider::StreamEventType::TextDelta && (*whole)[1].text == "!"},
                          {"sentinel terminal", whole->back().type == ava::provider::StreamEventType::Done}});
  for (std::size_t const chunk_size : {1U, 2U, 3U, 7U})
  {
    ava::provider::OpenAIStreamParser fragmented;
    std::vector<ava::provider::StreamEvent> actual;
    auto const label = "OpenAI SSE chunk size " + std::to_string(chunk_size);
    for (std::size_t offset = 0; offset < framed.size(); offset += chunk_size)
    {
      auto const parsed = fragmented.append(std::string_view(framed).substr(offset, chunk_size));
      if (!ava::tests::require(parsed.has_value(), label + " parses each fragment"))
        return;
      actual.insert(actual.end(), parsed->begin(), parsed->end());
    }
    auto const final = fragmented.finish();
    if (!ava::tests::require(final.has_value(), label + " finishes"))
      return;
    actual.insert(actual.end(), final->begin(), final->end());
    ava::tests::expect_eq(actual.size(), whole->size(), label + " preserves the exact event count");
    for (std::size_t index = 0; index < std::min(actual.size(), whole->size()); ++index)
    {
      auto const& expected = (*whole)[index];
      auto const& observed = actual[index];
      ava::tests::expect_all(label + " preserves event " + std::to_string(index), {{"type", observed.type == expected.type},
                                                                                   {"text", observed.text == expected.text},
                                                                                   {"terminal reason", observed.finish_reason == expected.finish_reason},
                                                                                   {"no tool id", observed.tool_call_id.empty()},
                                                                                   {"no tool name", observed.tool_name.empty()},
                                                                                   {"no error", observed.error_message.empty()}});
    }
  }
}

}  // namespace ava::tests::provider_openai_suite
