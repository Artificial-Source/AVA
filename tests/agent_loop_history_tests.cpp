#include "sys.h"
#include "tests/agent_loop_test_declarations.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/agent/assistant_turn.h"
#include "ava/agent/history_projection.h"
#include "ava/agent/message_builder.h"
#include "ava/session/assistant_output.h"
#include "ava/session/session_metadata.h"
#include "ava/session/validation.h"
#include "ava/provider/anthropic_provider.h"
#include "ava/provider/openai_compatible_provider.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/result.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

void test_legacy_provider_text_runs_preserve_v4_order()
{
  auto event = [](ava::provider::StreamEventType type) {
    auto value = ava::provider::StreamEvent{};
    value.type = type;
    return value;
  };
  auto text = [&](std::string value) {
    auto delta = event(ava::provider::StreamEventType::TextDelta);
    delta.text = std::move(value);
    return delta;
  };
  auto done = [&](ava::provider::ProviderFinishReason reason) {
    auto terminal = event(ava::provider::StreamEventType::Done);
    terminal.finish_reason = reason;
    return terminal;
  };
  auto tool = [&](ava::provider::StreamEventType type, std::string id, std::string name = {}, std::string arguments = {}) {
    auto value = event(type);
    value.tool_call_id = std::move(id);
    value.tool_name = std::move(name);
    value.text = std::move(arguments);
    return value;
  };

  auto const anthropic_streaming = ava::agent::parse_assistant_turn(
      {text("before"), text(" "), tool(ava::provider::StreamEventType::ToolCallStart, "call_anthropic", "read_file"),
       tool(ava::provider::StreamEventType::ToolCallDelta, "call_anthropic", "", R"({"path":"before.txt"})"),
       tool(ava::provider::StreamEventType::ToolCallEnd, "call_anthropic"), text("after"), done(ava::provider::ProviderFinishReason::ToolCalls)},
      {});
  auto const gemini_non_stream = ava::agent::parse_assistant_turn(
      {text("before"), tool(ava::provider::StreamEventType::ToolCallStart, "call_gemini", "read_file"),
       tool(ava::provider::StreamEventType::ToolCallDelta, "call_gemini", "", R"({"path":"after.txt"})"),
       tool(ava::provider::StreamEventType::ToolCallEnd, "call_gemini"), text("after"), done(ava::provider::ProviderFinishReason::ToolCalls)},
      {});
  auto reasoning_start = event(ava::provider::StreamEventType::ReasoningStart);
  reasoning_start.reasoning_format = "anthropic_thinking";
  auto reasoning_delta = event(ava::provider::StreamEventType::ReasoningDelta);
  reasoning_delta.text = "inspect";
  reasoning_delta.reasoning_format = "anthropic_thinking";
  auto reasoning_end = event(ava::provider::StreamEventType::ReasoningEnd);
  reasoning_end.reasoning_format = "anthropic_thinking";
  auto const anthropic_reasoning = ava::agent::parse_assistant_turn(
      {text("before"), reasoning_start, reasoning_delta, reasoning_end, text("after"), done(ava::provider::ProviderFinishReason::Completed)}, {});

  auto native_start = event(ava::provider::StreamEventType::TextStart);
  native_start.provider_item_id = "msg_native";
  native_start.provider_output_index = 1;
  native_start.assistant_phase = ava::provider::AssistantPhase::Commentary;
  auto native_delta = native_start;
  native_delta.type = ava::provider::StreamEventType::TextDelta;
  native_delta.text = "native";
  auto native_end = native_start;
  native_end.type = ava::provider::StreamEventType::TextEnd;
  auto const mixed_native = ava::agent::parse_assistant_turn(
      {text("before"), native_start, native_delta, native_end, text("after"), done(ava::provider::ProviderFinishReason::Completed)}, {});

  auto text_tool_text = [](ava::core::Result<ava::agent::ParsedAssistantTurn> const& turn, std::string_view call_id) {
    if (!turn || turn->ordered_items.size() != 3)
      return false;
    auto const* before = std::get_if<ava::agent::AssistantTextItem>(&turn->ordered_items[0].item);
    auto const* function = std::get_if<ava::agent::AssistantFunctionCallItem>(&turn->ordered_items[1].item);
    auto const* after = std::get_if<ava::agent::AssistantTextItem>(&turn->ordered_items[2].item);
    return before && function && after && before->text == "before" + std::string(call_id == "call_anthropic" ? " " : "") && function->tool_call.id == call_id &&
           after->text == "after";
  };
  auto reasoning_ordered = [&] {
    if (!anthropic_reasoning || anthropic_reasoning->ordered_items.size() != 3)
      return false;
    auto const* before = std::get_if<ava::agent::AssistantTextItem>(&anthropic_reasoning->ordered_items[0].item);
    auto const* reasoning = std::get_if<ava::agent::AssistantReasoningItem>(&anthropic_reasoning->ordered_items[1].item);
    auto const* after = std::get_if<ava::agent::AssistantTextItem>(&anthropic_reasoning->ordered_items[2].item);
    return before && reasoning && after && before->text == "before" && reasoning->reasoning.text == "inspect" && after->text == "after";
  };
  auto mixed_ordered = [&] {
    if (!mixed_native || mixed_native->ordered_items.size() != 3)
      return false;
    auto const* before = std::get_if<ava::agent::AssistantTextItem>(&mixed_native->ordered_items[0].item);
    auto const* native = std::get_if<ava::agent::AssistantTextItem>(&mixed_native->ordered_items[1].item);
    auto const* after = std::get_if<ava::agent::AssistantTextItem>(&mixed_native->ordered_items[2].item);
    return before && native && after && before->text == "before" && native->text == "native" && native->metadata.provider_item_id == "msg_native" &&
           after->text == "after";
  };
  expect(text_tool_text(anthropic_streaming, "call_anthropic") && text_tool_text(gemini_non_stream, "call_gemini") && reasoning_ordered() && mixed_ordered(),
         "legacy Anthropic/Gemini-style deltas form contiguous text runs across tool, reasoning, and native text lifecycles");

  auto store = ava::session::SessionStore::create_ephemeral(create_empty_root("legacy-text-run-v4-order"));
  auto target = store ? ava::session::SessionAppendTarget::create_ephemeral(*store)
                      : ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>>(std::unexpected(store.error()));
  if (!ava::tests::require(store.has_value(), "legacy text-run persistence creates an ephemeral store") ||
      !ava::tests::require(target.has_value(), "legacy text-run persistence creates an append target") ||
      !ava::tests::require(anthropic_streaming && gemini_non_stream && anthropic_reasoning && mixed_native,
                           "all provider text-run inputs parse before persistence"))
    return;
  auto append_batch = [append_target = *target](std::vector<ava::session::SessionEntry> entries) { return append_target->append_batch(std::move(entries)); };
  auto append_entry = [append_target = *target](ava::session::SessionEntry entry) { return append_target->append(std::move(entry)); };
  auto persisted_anthropic = ava::agent::append_assistant_turn(append_batch, *anthropic_streaming, "anthropic", "claude-test", {}, std::nullopt);
  auto anthropic_result =
      persisted_anthropic
          ? ava::agent::append_tool_result(append_entry, {.call_id = "call_anthropic", .name = "read_file", .success = true, .result_text = "anthropic result"},
                                           persisted_anthropic->function_output_entry_ids_by_call_id.at("call_anthropic"))
          : ava::core::VoidResult(std::unexpected(persisted_anthropic.error()));
  auto persisted_gemini = ava::agent::append_assistant_turn(append_batch, *gemini_non_stream, "gemini", "gemini-test", {}, std::nullopt);
  auto gemini_result = persisted_gemini ? ava::agent::append_tool_result(
                                              append_entry, {.call_id = "call_gemini", .name = "read_file", .success = true, .result_text = "gemini result"},
                                              persisted_gemini->function_output_entry_ids_by_call_id.at("call_gemini"))
                                        : ava::core::VoidResult(std::unexpected(persisted_gemini.error()));
  auto persisted_reasoning = ava::agent::append_assistant_turn(append_batch, *anthropic_reasoning, "anthropic", "claude-test", {}, std::nullopt);
  auto persisted_mixed = ava::agent::append_assistant_turn(append_batch, *mixed_native, "openai", "gpt-test", {}, std::nullopt);
  auto entries = store->load();
  auto projection = entries ? ava::session::classify_assistant_output(*entries) : ava::session::AssistantOutputProjection{};
  auto messages = entries ? ava::agent::build_provider_messages_from_entries(
                                *entries, ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = "openai",
                                                                                                                    .model_id = "gpt-test",
                                                                                                                    .api_family = "openai_responses",
                                                                                                                    .reasoning_format = "openai_responses",
                                                                                                                    .supports_tools = true,
                                                                                                                    .supports_images = false}})
                          : ava::core::Result<std::vector<ava::provider::ChatMessage>>{};
  auto const persisted_order = projection.turns.size() == 4 && projection.turns[0].items.size() == 3 && projection.turns[1].items.size() == 3 &&
                               projection.turns[2].items.size() == 3 && projection.turns[3].items.size() == 3 &&
                               std::holds_alternative<ava::session::AssistantOutputText>(projection.turns[0].items[0].item.payload) &&
                               std::holds_alternative<ava::session::AssistantOutputFunctionCall>(projection.turns[0].items[1].item.payload) &&
                               std::holds_alternative<ava::session::AssistantOutputText>(projection.turns[0].items[2].item.payload) &&
                               std::holds_alternative<ava::session::AssistantOutputText>(projection.turns[1].items[0].item.payload) &&
                               std::holds_alternative<ava::session::AssistantOutputFunctionCall>(projection.turns[1].items[1].item.payload) &&
                               std::holds_alternative<ava::session::AssistantOutputText>(projection.turns[1].items[2].item.payload) &&
                               std::holds_alternative<ava::session::AssistantOutputText>(projection.turns[2].items[0].item.payload) &&
                               std::holds_alternative<ava::session::AssistantOutputReasoning>(projection.turns[2].items[1].item.payload) &&
                               std::holds_alternative<ava::session::AssistantOutputText>(projection.turns[2].items[2].item.payload) &&
                               std::holds_alternative<ava::session::AssistantOutputText>(projection.turns[3].items[0].item.payload) &&
                               std::holds_alternative<ava::session::AssistantOutputText>(projection.turns[3].items[1].item.payload) &&
                               std::holds_alternative<ava::session::AssistantOutputText>(projection.turns[3].items[2].item.payload);
  auto const replay_order =
      messages && messages->size() == 6 && (*messages)[0].content_parts.size() == 3 && (*messages)[1].content_parts.size() == 1 &&
      (*messages)[2].content_parts.size() == 3 && (*messages)[3].content_parts.size() == 1 && (*messages)[4].content_parts.size() == 2 &&
      (*messages)[5].content_parts.size() == 3 && (*messages)[0].content_parts[0].text == "before " &&
      (*messages)[0].content_parts[1].type == ava::provider::ContentPartType::ToolUse && (*messages)[0].content_parts[2].text == "after" &&
      (*messages)[2].content_parts[0].text == "before" && (*messages)[2].content_parts[1].type == ava::provider::ContentPartType::ToolUse &&
      (*messages)[2].content_parts[2].text == "after" && (*messages)[4].content_parts[0].text == "before" && (*messages)[4].content_parts[1].text == "after" &&
      (*messages)[5].content_parts[0].text == "before" && (*messages)[5].content_parts[1].text == "native" && (*messages)[5].content_parts[2].text == "after";
  expect(
      persisted_anthropic && anthropic_result && persisted_gemini && gemini_result && persisted_reasoning && persisted_mixed && persisted_order && replay_order,
      "legacy text runs retain their v4 persisted sequence while unknown mixed-source reasoning projects portably");
}

void test_legacy_reasoning_replay_requires_exact_entry_source()
{
  using ava::session::EntryType;
  using ava::session::SessionEntry;

  auto const kimi_start = SessionEntry{
      .id = "legacy_kimi_start",
      .parent_id = "",
      .type = EntryType::SessionStart,
      .timestamp = "2026-07-24T00:00:00Z",
      .data_json = R"({"provider":"kimi","model":"kimi-k2-thinking","api_family":"openai_chat_completions","reasoning_format":"reasoning_content"})"};
  auto const assistant = SessionEntry{.id = "legacy_reasoning_answer",
                                      .parent_id = "",
                                      .type = EntryType::AssistantMessage,
                                      .timestamp = "2026-07-24T00:00:02Z",
                                      .data_json = R"({"text":"VISIBLE_LEGACY_ANSWER"})"};
  auto reasoning = [](std::string provider, std::string model, std::string text) {
    return SessionEntry{.id = "legacy_reasoning_block",
                        .parent_id = "",
                        .type = EntryType::ReasoningBlock,
                        .timestamp = "2026-07-24T00:00:01Z",
                        .data_json = "{\"provider\":\"" + provider + "\",\"model\":\"" + model + "\",\"format\":\"reasoning_content\",\"text\":\"" + text +
                                     "\",\"signature\":\"LEGACY_SIGNATURE_CANARY\",\"redacted_data\":\"LEGACY_REDACTED_CANARY\","
                                     "\"redacted\":false}"};
  };
  auto const kimi_target = ava::agent::HistoryReplayTarget{.provider_id = "kimi",
                                                           .model_id = "kimi-k2-thinking",
                                                           .api_family = "openai_chat_completions",
                                                           .reasoning_format = "reasoning_content",
                                                           .supports_tools = false,
                                                           .supports_images = false};
  ava::provider::OpenAICompatibleProvider const kimi_provider(ava::provider::OpenAICompatibleProviderOptions{
      .base_url = "https://compat.example.test", .provider_name = "Kimi", .reasoning_format = "reasoning_content", .preserve_reasoning_content = true});
  auto kimi_body = [&](SessionEntry block) {
    auto messages =
        ava::agent::build_provider_messages_from_entries({kimi_start, std::move(block), assistant}, ava::agent::MessageBuildOptions{.target = kimi_target});
    if (!messages)
      return std::string{};
    auto request = kimi_provider.build_request(
        ava::provider::ProviderRequest{
            .provider_id = "kimi", .model_id = "kimi-k2-thinking", .system_prompt = "", .messages = std::move(*messages), .tools_json = {}, .stream = false},
        "token");
    return request ? request->body : std::string{};
  };

  auto const cross_provider_body = kimi_body(reasoning("deepseek", "deepseek-reasoner", "LEGACY_CROSS_PROVIDER_REASONING"));
  auto const cross_model_body = kimi_body(reasoning("kimi", "deepseek-reasoner", "LEGACY_CROSS_MODEL_REASONING"));
  auto const exact_body = kimi_body(reasoning("kimi", "kimi-k2-thinking", "LEGACY_EXACT_REASONING"));
  auto omits_legacy_private = [](std::string const& body) {
    return !body.empty() && body.find("LEGACY_CROSS_") == std::string::npos && body.find("LEGACY_SIGNATURE_CANARY") == std::string::npos &&
           body.find("LEGACY_REDACTED_CANARY") == std::string::npos;
  };
  expect(
      omits_legacy_private(cross_provider_body) && omits_legacy_private(cross_model_body) &&
          cross_provider_body.find("VISIBLE_LEGACY_ANSWER") != std::string::npos && cross_model_body.find("VISIBLE_LEGACY_ANSWER") != std::string::npos &&
          exact_body.find("LEGACY_EXACT_REASONING") != std::string::npos,
      "legacy reasoning_content replay requires the block provider/model to match the complete snapshot and target, while an exact control replays natively");

  auto cross_provider_compaction_entries =
      std::vector<SessionEntry>{kimi_start, reasoning("deepseek", "deepseek-reasoner", "LEGACY_COMPACTION_REASONING"), assistant};
  cross_provider_compaction_entries.push_back(
      SessionEntry{.id = "legacy_reasoning_compaction",
                   .parent_id = "",
                   .type = EntryType::Compaction,
                   .timestamp = "2026-07-24T00:00:03Z",
                   .data_json = R"({"summary":"LEGACY_COMPACTION_PRIVATE_CANARY","provider":"kimi","model":"kimi-k2-thinking"})"});
  auto const cross_provider_compaction =
      ava::agent::build_provider_messages_from_entries(cross_provider_compaction_entries, ava::agent::MessageBuildOptions{.target = kimi_target});
  expect(cross_provider_compaction && cross_provider_compaction->size() == 1 &&
             cross_provider_compaction->front().content ==
                 "Earlier compacted provider history was omitted because exact replay compatibility could not be proven." &&
             cross_provider_compaction->front().content.find("LEGACY_COMPACTION_PRIVATE_CANARY") == std::string::npos,
         "legacy compaction exact-source proof rejects a same-format reasoning block whose own provider/model contradicts the snapshot");

  std::vector<SessionEntry> const openai_entries = {
      SessionEntry{.id = "legacy_openai_start",
                   .parent_id = "",
                   .type = EntryType::SessionStart,
                   .timestamp = "2026-07-24T00:00:00Z",
                   .data_json = R"({"provider":"openai","model":"gpt-5.5","api_family":"openai_responses","reasoning_format":"openai_responses"})"},
      SessionEntry{
          .id = "legacy_openai_reasoning",
          .parent_id = "",
          .type = EntryType::ReasoningBlock,
          .timestamp = "2026-07-24T00:00:01Z",
          .data_json =
              R"({"provider":"anthropic","model":"claude-test","format":"openai_responses","text":"OPENAI_LEGACY_REASONING_CANARY","signature":"OPENAI_LEGACY_SIGNATURE_CANARY","redacted_data":"OPENAI_LEGACY_REDACTED_CANARY","native_item_json":"{\"id\":\"rs_legacy_cross_source\",\"type\":\"reasoning\",\"summary\":[],\"encrypted_content\":\"OPENAI_LEGACY_NATIVE_CANARY\"}","redacted":false})"},
      assistant};
  auto openai_messages = ava::agent::build_provider_messages_from_entries(
      openai_entries, ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = "openai",
                                                                                                .model_id = "gpt-5.5",
                                                                                                .api_family = "openai_responses",
                                                                                                .reasoning_format = "openai_responses",
                                                                                                .supports_tools = false,
                                                                                                .supports_images = false}});
  ava::provider::OpenAIProvider const openai_provider("https://api.example.test");
  auto openai_request =
      openai_messages
          ? openai_provider.build_request(
                ava::provider::ProviderRequest{
                    .provider_id = "openai", .model_id = "gpt-5.5", .system_prompt = "", .messages = *openai_messages, .tools_json = {}, .stream = false},
                "token")
          : ava::core::Result<ava::http::HttpRequest>{std::unexpected(openai_messages.error())};
  auto const openai_body = openai_request ? openai_request->body : std::string{};
  expect(
      openai_request && openai_body.find("VISIBLE_LEGACY_ANSWER") != std::string::npos && openai_body.find("OPENAI_LEGACY_") == std::string::npos &&
          openai_body.find("rs_legacy_cross_source") == std::string::npos,
      "OpenAI Responses generated history omits native reasoning text, signature, redacted data, and native item metadata from a contradictory legacy block");
}

void test_v4_no_tools_fallback_survives_native_content_serializers()
{
  using ava::session::AssistantOutputFunctionCall;
  using ava::session::AssistantOutputItem;
  using ava::session::AssistantOutputItemKind;
  using ava::session::AssistantOutputReasoning;
  using ava::session::AssistantOutputText;
  using ava::session::AssistantOutputTextPhase;
  using ava::session::AssistantTurnCommit;
  using ava::session::EntryType;
  using ava::session::SessionEntry;

  auto item_entry = [](std::string id, AssistantOutputItem item) {
    auto data = ava::session::serialize_assistant_output_item_data_json(item);
    return SessionEntry{
        .id = std::move(id), .parent_id = "", .type = EntryType::AssistantOutputItem, .timestamp = "2026-07-24T00:00:00Z", .data_json = data.value_or("{}")};
  };
  std::vector<SessionEntry> entries = {
      item_entry("portable_commentary",
                 AssistantOutputItem{.assistant_turn_id = "turn_no_tools",
                                     .sequence = 0,
                                     .kind = AssistantOutputItemKind::Text,
                                     .provider_item_id = "msg_private_commentary",
                                     .provider_output_index = 0,
                                     .payload = AssistantOutputText{.text = "VISIBLE_COMMENTARY", .assistant_phase = AssistantOutputTextPhase::Commentary}}),
      item_entry(
          "portable_reasoning",
          AssistantOutputItem{
              .assistant_turn_id = "turn_no_tools",
              .sequence = 1,
              .kind = AssistantOutputItemKind::Reasoning,
              .provider_item_id = "rs_private_no_tools",
              .provider_output_index = 1,
              .payload =
                  AssistantOutputReasoning{
                      .text = "PRIVATE_REASONING_CANARY",
                      .format = "openai_responses",
                      .redacted = false,
                      .signature = "PRIVATE_SIGNATURE_CANARY",
                      .redacted_data = "PRIVATE_REDACTED_CANARY",
                      .native_item_json = R"({"id":"rs_private_no_tools","type":"reasoning","summary":[],"encrypted_content":"PRIVATE_NATIVE_CANARY"})"}}),
      item_entry("portable_function", AssistantOutputItem{.assistant_turn_id = "turn_no_tools",
                                                          .sequence = 2,
                                                          .kind = AssistantOutputItemKind::FunctionCall,
                                                          .provider_item_id = "fc_private_no_tools",
                                                          .provider_output_index = 2,
                                                          .payload = AssistantOutputFunctionCall{.call_id = "call_private_no_tools",
                                                                                                 .name = "read_file",
                                                                                                 .arguments_json = R"({"path":"README.md"})"}}),
      item_entry("portable_answer",
                 AssistantOutputItem{.assistant_turn_id = "turn_no_tools",
                                     .sequence = 3,
                                     .kind = AssistantOutputItemKind::Text,
                                     .provider_item_id = "msg_private_answer",
                                     .provider_output_index = 3,
                                     .payload = AssistantOutputText{.text = "VISIBLE_ANSWER", .assistant_phase = AssistantOutputTextPhase::FinalAnswer}})};
  auto commit_data = ava::session::serialize_assistant_turn_commit_data_json(AssistantTurnCommit{.assistant_turn_id = "turn_no_tools",
                                                                                                 .item_count = 4,
                                                                                                 .provider = "openai",
                                                                                                 .model = "gpt-5.5",
                                                                                                 .api_family = "openai_responses",
                                                                                                 .reasoning_format = "openai_responses",
                                                                                                 .finish_reason = "tool_calls",
                                                                                                 .usage_json = std::nullopt});
  entries.push_back(SessionEntry{.id = "portable_commit",
                                 .parent_id = "",
                                 .type = EntryType::AssistantTurnCommit,
                                 .timestamp = "2026-07-24T00:00:01Z",
                                 .data_json = commit_data.value_or("{}")});
  entries.push_back(SessionEntry{
      .id = "portable_result",
      .parent_id = "",
      .type = EntryType::ToolResult,
      .timestamp = "2026-07-24T00:00:02Z",
      .data_json =
          R"({"assistant_output_entry_id":"portable_function","call_id":"call_private_no_tools","name":"read_file","success":true,"result":"VISIBLE_TOOL_RESULT"})"});

  auto messages_for = [&](ava::agent::HistoryReplayTarget target) {
    return ava::agent::build_provider_messages_from_entries(entries, ava::agent::MessageBuildOptions{.target = std::move(target)});
  };
  auto openai_messages = messages_for(ava::agent::HistoryReplayTarget{.provider_id = "openai",
                                                                      .model_id = "gpt-5.5",
                                                                      .api_family = "openai_responses",
                                                                      .reasoning_format = "openai_responses",
                                                                      .supports_tools = false,
                                                                      .supports_images = false});
  auto anthropic_messages = messages_for(ava::agent::HistoryReplayTarget{.provider_id = "anthropic",
                                                                         .model_id = "claude-test",
                                                                         .api_family = "anthropic_messages",
                                                                         .reasoning_format = "anthropic_thinking",
                                                                         .supports_tools = false,
                                                                         .supports_images = false});
  auto compatible_messages = messages_for(ava::agent::HistoryReplayTarget{.provider_id = "kimi",
                                                                          .model_id = "kimi-k2-thinking",
                                                                          .api_family = "openai_chat_completions",
                                                                          .reasoning_format = "reasoning_content",
                                                                          .supports_tools = false,
                                                                          .supports_images = false});

  ava::provider::OpenAIProvider const openai("https://api.example.test");
  ava::provider::AnthropicProvider const anthropic("https://api.example.test");
  ava::provider::OpenAICompatibleProvider const compatible(ava::provider::OpenAICompatibleProviderOptions{
      .base_url = "https://api.example.test", .provider_name = "Kimi", .reasoning_format = "reasoning_content", .preserve_reasoning_content = true});
  auto request_for = [](std::string provider_id, std::string model_id, std::vector<ava::provider::ChatMessage> messages) {
    return ava::provider::ProviderRequest{.provider_id = std::move(provider_id),
                                          .model_id = std::move(model_id),
                                          .system_prompt = "",
                                          .messages = std::move(messages),
                                          .tools_json = {},
                                          .stream = false,
                                          .max_output_tokens = std::nullopt,
                                          .reasoning = std::nullopt,
                                          .system_prompt_cache_ttl = ""};
  };
  auto openai_request = openai_messages ? openai.build_request(request_for("openai", "gpt-5.5", *openai_messages), "token")
                                        : ava::core::Result<ava::http::HttpRequest>{std::unexpected(openai_messages.error())};
  auto anthropic_request = anthropic_messages ? anthropic.build_request(request_for("anthropic", "claude-test", *anthropic_messages), "token")
                                              : ava::core::Result<ava::http::HttpRequest>{std::unexpected(anthropic_messages.error())};
  auto compatible_request = compatible_messages ? compatible.build_request(request_for("kimi", "kimi-k2-thinking", *compatible_messages), "token")
                                                : ava::core::Result<ava::http::HttpRequest>{std::unexpected(compatible_messages.error())};

  auto has_portable_turn = [](std::string const& body) {
    auto const commentary = body.find("VISIBLE_COMMENTARY");
    auto const call = body.find("Tool call (read_file)");
    auto const answer = body.find("VISIBLE_ANSWER");
    auto const result = body.find("VISIBLE_TOOL_RESULT");
    return commentary != std::string::npos && call != std::string::npos && answer != std::string::npos && result != std::string::npos && commentary < call &&
           call < answer && answer < result && body.find("Tool call (read_file)", call + 1) == std::string::npos &&
           body.find("README.md") != std::string::npos && body.find("call_private_no_tools") == std::string::npos && body.find("PRIVATE_") == std::string::npos;
  };
  auto const openai_body = openai_request ? openai_request->body : std::string{};
  auto const anthropic_body = anthropic_request ? anthropic_request->body : std::string{};
  auto const compatible_body = compatible_request ? compatible_request->body : std::string{};
  expect(openai_request && anthropic_request && compatible_request && has_portable_turn(openai_body) && has_portable_turn(anthropic_body) &&
             has_portable_turn(compatible_body) && openai_body.find(R"("type":"function_call")") == std::string::npos &&
             openai_body.find(R"("type":"function_call_output")") == std::string::npos && anthropic_body.find(R"("type":"tool_use")") == std::string::npos &&
             anthropic_body.find(R"("type":"tool_result")") == std::string::npos && compatible_body.find(R"("tool_calls")") == std::string::npos &&
             compatible_body.find(R"("role":"tool")") == std::string::npos,
         "OpenAI Responses, Anthropic Messages, and OpenAI-compatible bodies retain ordered no-tools call/result fallback text without private or native tool "
         "data");
}

namespace {
using ava::session::AssistantOutputFunctionCall;
using ava::session::AssistantOutputItem;
using ava::session::AssistantOutputItemKind;
using ava::session::AssistantOutputReasoning;
using ava::session::AssistantOutputText;
using ava::session::AssistantOutputTextPhase;
using ava::session::AssistantTurnCommit;
using ava::session::EntryType;
using ava::session::SessionEntry;

struct RequestHistoryFixture
{
  std::vector<SessionEntry> entries;
  ava::agent::HistoryReplayTarget target;
};

RequestHistoryFixture request_history_fixture()
{
  auto item_entry = [](std::string id, AssistantOutputItem item) {
    auto data = ava::session::serialize_assistant_output_item_data_json(item);
    return SessionEntry{
        .id = std::move(id), .parent_id = "", .type = EntryType::AssistantOutputItem, .timestamp = "2026-07-24T00:00:00Z", .data_json = data.value_or("{}")};
  };
  auto commit_entry = [](AssistantTurnCommit commit) {
    auto data = ava::session::serialize_assistant_turn_commit_data_json(commit);
    return SessionEntry{.id = "commit_projection",
                        .parent_id = "",
                        .type = EntryType::AssistantTurnCommit,
                        .timestamp = "2026-07-24T00:00:01Z",
                        .data_json = data.value_or("{}")};
  };

  std::vector<SessionEntry> entries = {
      item_entry(
          "reasoning_projection",
          AssistantOutputItem{
              .assistant_turn_id = "turn_projection",
              .sequence = 0,
              .kind = AssistantOutputItemKind::Reasoning,
              .provider_item_id = "rs_source_canary",
              .provider_output_index = 0,
              .payload =
                  AssistantOutputReasoning{
                      .text = "PRIVATE_REASONING_CANARY",
                      .format = "openai_responses",
                      .redacted = false,
                      .signature = "PRIVATE_SIGNATURE_CANARY",
                      .redacted_data = "PRIVATE_REDACTED_CANARY",
                      .native_item_json = R"({"id":"rs_source_canary","type":"reasoning","summary":[],"encrypted_content":"PRIVATE_ENCRYPTED_CANARY"})"}}),
      item_entry("function_projection", AssistantOutputItem{.assistant_turn_id = "turn_projection",
                                                            .sequence = 1,
                                                            .kind = AssistantOutputItemKind::FunctionCall,
                                                            .provider_item_id = "fc_source_canary",
                                                            .provider_output_index = 1,
                                                            .payload = AssistantOutputFunctionCall{.call_id = "call_source_canary",
                                                                                                   .name = "read_file",
                                                                                                   .arguments_json = R"({"path":"README.md"})"}}),
      item_entry("text_projection",
                 AssistantOutputItem{.assistant_turn_id = "turn_projection",
                                     .sequence = 2,
                                     .kind = AssistantOutputItemKind::Text,
                                     .provider_item_id = "msg_source_canary",
                                     .provider_output_index = 2,
                                     .payload = AssistantOutputText{.text = "VISIBLE_ANSWER", .assistant_phase = AssistantOutputTextPhase::FinalAnswer}}),
      commit_entry(AssistantTurnCommit{.assistant_turn_id = "turn_projection",
                                       .item_count = 3,
                                       .provider = "openai",
                                       .model = "gpt-5.5",
                                       .api_family = "openai_responses",
                                       .reasoning_format = "openai_responses",
                                       .finish_reason = "tool_calls",
                                       .usage_json = std::nullopt}),
      SessionEntry{
          .id = "result_projection",
          .parent_id = "",
          .type = EntryType::ToolResult,
          .timestamp = "2026-07-24T00:00:02Z",
          .data_json =
              R"({"assistant_output_entry_id":"function_projection","call_id":"call_source_canary","name":"read_file","success":true,"result":"VISIBLE_TOOL_RESULT"})"}};

  auto target = ava::agent::HistoryReplayTarget{.provider_id = "openai",
                                                .model_id = "gpt-5.5",
                                                .api_family = "openai_responses",
                                                .reasoning_format = "openai_responses",
                                                .supports_tools = true,
                                                .supports_images = true};
  return {std::move(entries), std::move(target)};
}
}  // namespace

void test_request_time_history_projection_preserves_only_exact_native_replay()
{
  auto [entries, target] = request_history_fixture();
  auto exact = ava::agent::build_provider_messages_from_entries(entries, ava::agent::MessageBuildOptions{.target = target});
  bool exact_native = exact && exact->size() == 2 && (*exact)[0].content_parts.size() == 3 && (*exact)[1].content_parts.size() == 1;
  if (exact_native)
  {
    auto const& reasoning = (*exact)[0].content_parts[0];
    auto const& function = (*exact)[0].content_parts[1];
    auto const& answer = (*exact)[0].content_parts[2];
    auto const& result = (*exact)[1].content_parts[0];
    exact_native = reasoning.type == ava::provider::ContentPartType::Reasoning && reasoning.provider_item_id == "rs_source_canary" &&
                   reasoning.reasoning_native_item_json.find("PRIVATE_ENCRYPTED_CANARY") != std::string::npos &&
                   function.type == ava::provider::ContentPartType::ToolUse && function.provider_item_id == "fc_source_canary" &&
                   function.tool_call_id == "call_source_canary" && answer.provider_item_id == "msg_source_canary" &&
                   result.tool_call_id == "call_source_canary";
  }
  expect(exact_native, "an exact source provider/model/API/reasoning target preserves native reasoning and provider/tool identities");

  auto duplicate_provider_id_entries = entries;
  auto const duplicate_id_offset = duplicate_provider_id_entries[2].data_json.find("msg_source_canary");
  if (duplicate_id_offset != std::string::npos)
    duplicate_provider_id_entries[2].data_json.replace(duplicate_id_offset, std::string("msg_source_canary").size(), "rs_source_canary");
  auto duplicate_provider_ids =
      ava::agent::build_provider_messages_from_entries(duplicate_provider_id_entries, ava::agent::MessageBuildOptions{.target = target});
  expect(!duplicate_provider_ids, "duplicate provider item identities are a hard v4 classification error rather than partial native replay");

  auto malformed_provider_id_entries = entries;
  auto const malformed_id_offset = malformed_provider_id_entries[2].data_json.find("msg_source_canary");
  if (malformed_id_offset != std::string::npos)
    malformed_provider_id_entries[2].data_json.replace(malformed_id_offset, std::string("msg_source_canary").size(), "");
  auto malformed_provider_id =
      ava::agent::build_provider_messages_from_entries(malformed_provider_id_entries, ava::agent::MessageBuildOptions{.target = target});
  expect(!malformed_provider_id, "a malformed provider item identity is a hard v4 classification error rather than silent native replay");
  auto unresolved_entries = entries;
  unresolved_entries.pop_back();
  auto unresolved = ava::agent::build_provider_messages_from_entries(unresolved_entries, ava::agent::MessageBuildOptions{.target = target});
  expect(!unresolved && unresolved.error().message().find("tool result") != std::string::npos,
         "an unresolved committed v4 function still fails at request time instead of inventing a portable completion");
}

void test_request_time_history_projection_portable_targets_drop_native_identity()
{
  auto [entries, target] = request_history_fixture();
  auto missing_target = ava::agent::build_provider_messages_from_entries(entries);
  std::string missing_target_fields;
  if (missing_target)
  {
    for (auto const& message : *missing_target)
    {
      missing_target_fields += message.content;
      for (auto const& part : message.content_parts)
        missing_target_fields += part.text + part.tool_call_id + part.provider_item_id + part.reasoning_signature + part.reasoning_native_item_json;
    }
  }
  expect(missing_target && missing_target_fields.find("VISIBLE_ANSWER") != std::string::npos &&
             missing_target_fields.find("VISIBLE_TOOL_RESULT") != std::string::npos && missing_target_fields.find("PRIVATE_") == std::string::npos &&
             missing_target_fields.find("source_canary") == std::string::npos &&
             std::ranges::all_of(*missing_target,
                                 [](auto const& message) {
                                   return std::ranges::none_of(message.content_parts, [](auto const& part) {
                                     return part.type == ava::provider::ContentPartType::Reasoning || part.type == ava::provider::ContentPartType::ToolUse ||
                                            part.type == ava::provider::ContentPartType::ToolResult || !part.provider_item_id.empty();
                                   });
                                 }),
         "an omitted request target is force-portable and can never activate exact native replay");

  auto forced_portable = ava::agent::build_provider_messages_from_entries(
      entries, ava::agent::MessageBuildOptions{.target = target, .replay_mode = ava::agent::HistoryReplayMode::ForcePortable});
  expect(forced_portable && forced_portable->size() == 2 && forced_portable->front().content_parts.size() == 2 &&
             forced_portable->front().content_parts.front().type == ava::provider::ContentPartType::ToolUse &&
             forced_portable->front().content_parts.front().tool_call_id != "call_source_canary" &&
             std::ranges::none_of(forced_portable->front().content_parts,
                                  [](auto const& part) { return part.type == ava::provider::ContentPartType::Reasoning || !part.provider_item_id.empty(); }),
         "explicit ForcePortable preserves known tool capability while still preventing native reasoning and source identities");

  auto incomplete_target = target;
  incomplete_target.api_family.clear();
  auto incomplete = ava::agent::build_provider_messages_from_entries(entries, ava::agent::MessageBuildOptions{.target = incomplete_target});
  expect(incomplete && std::ranges::all_of(*incomplete,
                                           [](auto const& message) {
                                             return std::ranges::none_of(message.content_parts, [](auto const& part) {
                                               return part.type == ava::provider::ContentPartType::Reasoning ||
                                                      part.type == ava::provider::ContentPartType::ToolUse ||
                                                      part.type == ava::provider::ContentPartType::ToolResult || !part.provider_item_id.empty();
                                             });
                                           }),
         "an incomplete target is conservatively force-portable and supplies no unproven tool or image capability");

  target.model_id = "gpt-5.6-sol";
  auto portable = ava::agent::build_provider_messages_from_entries(entries, ava::agent::MessageBuildOptions{.target = target});
  bool portable_pair = portable && portable->size() == 2 && (*portable)[0].content_parts.size() == 2 && (*portable)[1].content_parts.size() == 1;
  std::string portable_fields;
  if (portable_pair)
  {
    auto const& function = (*portable)[0].content_parts[0];
    auto const& answer = (*portable)[0].content_parts[1];
    auto const& result = (*portable)[1].content_parts[0];
    portable_pair = function.type == ava::provider::ContentPartType::ToolUse && result.type == ava::provider::ContentPartType::ToolResult &&
                    !function.tool_call_id.empty() && function.tool_call_id == result.tool_call_id && function.tool_call_id != "call_source_canary" &&
                    function.provider_item_id.empty() && answer.text == "VISIBLE_ANSWER" && answer.provider_item_id.empty() &&
                    result.text == "VISIBLE_TOOL_RESULT";
    for (auto const& message : *portable)
    {
      portable_fields += message.content;
      for (auto const& part : message.content_parts)
      {
        portable_fields +=
            part.text + part.tool_call_id + part.provider_item_id + part.reasoning_signature + part.reasoning_redacted_data + part.reasoning_native_item_json;
      }
    }
  }
  expect(portable_pair && portable_fields.find("PRIVATE_") == std::string::npos && portable_fields.find("source_canary") == std::string::npos,
         "a model switch keeps visible answer/tool semantics with paired request-local IDs and drops every private/source identity canary");

  target.model_id = "gpt-5.5";
  target.api_family = "openai_chat_completions";
  auto cross_api = ava::agent::build_provider_messages_from_entries(entries, ava::agent::MessageBuildOptions{.target = target});
  expect(cross_api && cross_api->size() == 2 && cross_api->front().content_parts.size() == 2 &&
             cross_api->front().content_parts.front().type == ava::provider::ContentPartType::ToolUse &&
             cross_api->front().content_parts.front().tool_call_id != "call_source_canary" &&
             std::ranges::none_of(cross_api->front().content_parts,
                                  [](auto const& part) { return part.type == ava::provider::ContentPartType::Reasoning || !part.provider_item_id.empty(); }),
         "a same-provider, same-model API-family change forces portable replay rather than sending endpoint-native identities");

  target.api_family = "openai_responses";
  target.reasoning_format = "reasoning_content";
  auto cross_reasoning_format = ava::agent::build_provider_messages_from_entries(entries, ava::agent::MessageBuildOptions{.target = target});
  expect(cross_reasoning_format && cross_reasoning_format->size() == 2 && cross_reasoning_format->front().content_parts.size() == 2 &&
             std::ranges::none_of(cross_reasoning_format->front().content_parts,
                                  [](auto const& part) { return part.type == ava::provider::ContentPartType::Reasoning || !part.provider_item_id.empty(); }),
         "an incompatible reasoning format drops reasoning and forces the containing turn through portable replay");

  target.reasoning_format = "openai_responses";
  auto exact_after_portable = ava::agent::build_provider_messages_from_entries(entries, ava::agent::MessageBuildOptions{.target = target});
  expect(exact_after_portable && exact_after_portable->size() == 2 && exact_after_portable->front().content_parts.size() == 3 &&
             exact_after_portable->front().content_parts[0].reasoning_native_item_json.find("PRIVATE_ENCRYPTED_CANARY") != std::string::npos &&
             exact_after_portable->front().content_parts[1].tool_call_id == "call_source_canary" &&
             exact_after_portable->back().content_parts.front().tool_call_id == "call_source_canary",
         "A-to-B-to-A request projection is copy-only: returning to the exact target can safely reconstruct native replay from unchanged session records");
}

void test_request_time_history_projection_pairs_collision_free_portable_tools()
{
  auto [entries, target] = request_history_fixture();
  target.model_id = "gpt-5.6-sol";

  std::vector<SessionEntry> const synthetic_collision_entries = {
      SessionEntry{.id = "legacy_collision_call",
                   .parent_id = "",
                   .type = EntryType::ToolCall,
                   .timestamp = "2026-07-24T00:00:00Z",
                   .data_json = R"({"call_id":"ava_history_tool_1","name":"read_file","arguments":"{\"path\":\"README.md\"}"})"},
      SessionEntry{.id = "legacy_collision_result",
                   .parent_id = "",
                   .type = EntryType::ToolResult,
                   .timestamp = "2026-07-24T00:00:01Z",
                   .data_json = R"({"call_id":"ava_history_tool_1","name":"read_file","success":true,"result":"collision-safe"})"}};
  auto synthetic_collision = ava::agent::build_provider_messages_from_entries(synthetic_collision_entries, ava::agent::MessageBuildOptions{.target = target});
  expect(synthetic_collision && synthetic_collision->size() == 2 && synthetic_collision->front().content_parts.size() == 1 &&
             synthetic_collision->back().content_parts.size() == 1 && synthetic_collision->front().content_parts.front().tool_call_id == "ava_history_tool_2" &&
             synthetic_collision->back().content_parts.front().tool_call_id == "ava_history_tool_2" &&
             synthetic_collision->front().content.find("ava_history_tool_1") == std::string::npos &&
             synthetic_collision->back().content.find("ava_history_tool_1") == std::string::npos,
         "request-local tool IDs skip every persisted source ID so a source cannot collide with or leak through the synthetic namespace");

  target.supports_tools = false;
  auto text_tools = ava::agent::build_provider_messages_from_entries(entries, ava::agent::MessageBuildOptions{.target = target});
  std::string text_tool_projection;
  if (text_tools)
    for (auto const& message : *text_tools) text_tool_projection += message.content;
  expect(text_tools &&
             std::ranges::all_of(*text_tools,
                                 [](auto const& message) {
                                   return std::ranges::none_of(message.content_parts, [](auto const& part) {
                                     return part.type == ava::provider::ContentPartType::ToolUse || part.type == ava::provider::ContentPartType::ToolResult;
                                   });
                                 }) &&
             text_tool_projection.find("Tool call") != std::string::npos && text_tool_projection.find("Tool result") != std::string::npos &&
             text_tool_projection.find("VISIBLE_TOOL_RESULT") != std::string::npos && text_tool_projection.find("call_source_canary") == std::string::npos,
         "a no-tools target receives a labelled textual call/result pair without source or synthetic call IDs");
}

void test_request_time_history_projection_rejects_unproven_source_identity()
{
  auto [entries, target] = request_history_fixture();
  auto unknown_entries = entries;
  auto unknown_commit = ava::session::serialize_assistant_turn_commit_data_json(AssistantTurnCommit{.assistant_turn_id = "turn_projection",
                                                                                                    .item_count = 3,
                                                                                                    .provider = "openai",
                                                                                                    .model = "gpt-5.5",
                                                                                                    .finish_reason = "tool_calls",
                                                                                                    .usage_json = std::nullopt});
  if (!ava::tests::require(unknown_commit.has_value(), "unknown-provenance fixture serializes a valid commit"))
    return;
  unknown_entries[3].data_json = *unknown_commit;
  target.model_id = "gpt-5.5";
  target.supports_tools = true;
  auto unknown = ava::agent::build_provider_messages_from_entries(unknown_entries, ava::agent::MessageBuildOptions{.target = target});
  expect(unknown && !unknown->empty() &&
             std::ranges::none_of(unknown->front().content_parts,
                                  [](auto const& part) {
                                    return part.type == ava::provider::ContentPartType::Reasoning || !part.provider_item_id.empty() ||
                                           part.tool_call_id == "call_source_canary";
                                  }),
         "a child-style committed turn without snapshot or commit API provenance is always portable");

  auto contradictory_entries = entries;
  contradictory_entries.insert(contradictory_entries.begin(),
                               SessionEntry{.id = "contradictory_start",
                                            .parent_id = "",
                                            .type = EntryType::SessionStart,
                                            .timestamp = "2026-07-23T23:59:59Z",
                                            .data_json = R"({"provider":"anthropic","model":"claude-sonnet-4-5","original_cwd":"/tmp"})"});
  auto contradictory = ava::agent::build_provider_messages_from_entries(contradictory_entries, ava::agent::MessageBuildOptions{.target = target});
  expect(contradictory && !contradictory->empty() &&
             std::ranges::none_of(contradictory->front().content_parts,
                                  [](auto const& part) {
                                    return part.type == ava::provider::ContentPartType::Reasoning || !part.provider_item_id.empty() ||
                                           part.tool_call_id == "call_source_canary";
                                  }),
         "a commit cannot claim native replay authority when even a partial session snapshot contradicts its provider and model");

  auto changed_before_result_entries = entries;
  changed_before_result_entries.insert(
      changed_before_result_entries.begin() + 4,
      SessionEntry{
          .id = "changed_before_result",
          .parent_id = "",
          .type = EntryType::ModelChange,
          .timestamp = "2026-07-24T00:00:01Z",
          .data_json =
              R"({"previous_provider":"openai","previous_model":"gpt-5.5","provider":"anthropic","model":"claude-sonnet-4-5","api_family":"anthropic_messages","reasoning_format":"anthropic_thinking"})"});
  auto changed_before_result =
      ava::agent::build_provider_messages_from_entries(changed_before_result_entries, ava::agent::MessageBuildOptions{.target = target});
  expect(changed_before_result && !changed_before_result->empty() &&
             std::ranges::none_of(changed_before_result->front().content_parts,
                                  [](auto const& part) {
                                    return part.type == ava::provider::ContentPartType::Reasoning || !part.provider_item_id.empty() ||
                                           part.tool_call_id == "call_source_canary";
                                  }),
         "a v4 result recorded across a contradictory model snapshot is projected portably rather than retaining native call identities");
}

void test_request_time_history_projection_compaction_requires_replay_provenance()
{
  auto [entries, target] = request_history_fixture();
  auto legacy_compaction_entries = entries;
  legacy_compaction_entries.push_back(SessionEntry{.id = "older_portable_compaction_projection",
                                                   .parent_id = "",
                                                   .type = EntryType::Compaction,
                                                   .timestamp = "2026-07-24T00:00:02Z",
                                                   .data_json = R"({"summary":"OLDER_PORTABLE_SUMMARY","history_projection":"portable-v1"})"});
  legacy_compaction_entries.push_back(SessionEntry{
      .id = "legacy_compaction_projection",
      .parent_id = "",
      .type = EntryType::Compaction,
      .timestamp = "2026-07-24T00:00:03Z",
      .data_json = R"({"summary":"PRIVATE_LEGACY_COMPACTION_CANARY","instructions":"PRIVATE_LEGACY_INSTRUCTIONS","provider":"openai","model":"gpt-5.5"})"});
  auto legacy_compaction = ava::agent::build_provider_messages_from_entries(legacy_compaction_entries, ava::agent::MessageBuildOptions{.target = target});
  expect(legacy_compaction && legacy_compaction->size() == 1 &&
             legacy_compaction->front().content == "Earlier compacted provider history was omitted because exact replay compatibility could not be proven." &&
             legacy_compaction->front().content.find("PRIVATE_LEGACY") == std::string::npos,
         "an unmarked legacy compaction omits all replay-bearing material when its own provenance is unknown, even after an older portable checkpoint");

  std::vector<SessionEntry> const exact_legacy_compaction_entries = {
      SessionEntry{
          .id = "exact_legacy_start",
          .parent_id = "",
          .type = EntryType::SessionStart,
          .timestamp = "2026-07-24T00:00:00Z",
          .data_json =
              R"({"provider":"openai","model":"gpt-5.5","api_family":"openai_responses","reasoning_format":"openai_responses","original_cwd":"/tmp"})"},
      SessionEntry{.id = "exact_legacy_answer",
                   .parent_id = "",
                   .type = EntryType::AssistantMessage,
                   .timestamp = "2026-07-24T00:00:01Z",
                   .data_json = R"({"text":"represented answer"})"},
      SessionEntry{.id = "exact_legacy_compaction",
                   .parent_id = "",
                   .type = EntryType::Compaction,
                   .timestamp = "2026-07-24T00:00:02Z",
                   .data_json = R"({"summary":"EXACT_LEGACY_SUMMARY","provider":"openai","model":"gpt-5.5"})"}};
  auto exact_legacy_compaction =
      ava::agent::build_provider_messages_from_entries(exact_legacy_compaction_entries, ava::agent::MessageBuildOptions{.target = target});
  expect(exact_legacy_compaction && exact_legacy_compaction->size() == 1 &&
             exact_legacy_compaction->front().content.find("EXACT_LEGACY_SUMMARY") != std::string::npos,
         "an unmarked legacy compaction retains represented summary data only when its complete source range and boundary are exact-compatible");

  legacy_compaction_entries.back().data_json =
      R"({"summary":"PORTABLE_COMPACTION_SUMMARY","instructions":"portable carry","provider":"openai","model":"gpt-5.5","history_projection":"portable-v1"})";
  auto marked_compaction = ava::agent::build_provider_messages_from_entries(legacy_compaction_entries);
  expect(marked_compaction && marked_compaction->size() == 1 && marked_compaction->front().content.find("PORTABLE_COMPACTION_SUMMARY") != std::string::npos &&
             marked_compaction->front().content.find("portable carry") != std::string::npos,
         "a portable-v1 compaction remains replayable even when the request target is omitted and therefore forced portable");
}

void test_request_time_history_projection_reserves_images_for_active_turn()
{
  auto image_entry = [](int index, std::size_t byte_size) {
    auto const id = "img_" + std::to_string(index);
    return ava::session::SessionEntry{
        .id = "user_" + std::to_string(index),
        .parent_id = "",
        .type = ava::session::EntryType::UserMessage,
        .timestamp = "2026-07-24T00:00:00Z",
        .data_json = "{\"text\":\"image " + std::to_string(index) + "\",\"attachments\":[{\"id\":\"" + id +
                     "\",\"type\":\"image\",\"mime_type\":\"image/png\",\"byte_size\":" + std::to_string(byte_size) +
                     ",\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"storage_path\":\"attachments/" + id + ".png\"}]}"};
  };

  std::vector<ava::session::SessionEntry> entries;
  for (int index = 0; index < 17; ++index) entries.push_back(image_entry(index, 1));
  auto target = ava::agent::HistoryReplayTarget{.provider_id = "openai",
                                                .model_id = "gpt-image",
                                                .api_family = "openai_responses",
                                                .reasoning_format = "openai_responses",
                                                .supports_tools = true,
                                                .supports_images = true};
  auto projected =
      ava::agent::build_provider_messages_from_entries(entries, ava::agent::MessageBuildOptions{.target = target, .active_turn_user_entry_ids = {"user_16"}});
  std::size_t image_parts = 0;
  if (projected)
  {
    for (auto const& message : *projected)
      image_parts += static_cast<std::size_t>(
          std::ranges::count_if(message.content_parts, [](auto const& part) { return part.type == ava::provider::ContentPartType::Image; }));
  }
  expect(projected && projected->size() == 17 && image_parts == 16 &&
             projected->front().content.find("[historical image omitted: mime=image/png bytes=1]") != std::string::npos &&
             projected->front().content.find("img_0") == std::string::npos && projected->front().content_parts.size() == 2 &&
             projected->front().content_parts.back().type == ava::provider::ContentPartType::Text &&
             projected->front().content_parts.back().text == "[historical image omitted: mime=image/png bytes=1]" &&
             projected->back().content.find("id=img_16") != std::string::npos && projected->back().content_parts.size() == 2,
         "historical image count selection is deterministic, reserves capacity for the active image, and omits historical attachment identity");

  std::vector<ava::session::SessionEntry> aggregate = {image_entry(30, 15 * 1024 * 1024), image_entry(31, 15 * 1024 * 1024), image_entry(32, 15 * 1024 * 1024),
                                                       image_entry(33, 1 * 1024 * 1024)};
  auto aggregate_projection =
      ava::agent::build_provider_messages_from_entries(aggregate, ava::agent::MessageBuildOptions{.target = target, .active_turn_user_entry_ids = {"user_33"}});
  std::size_t aggregate_image_parts = 0;
  if (aggregate_projection)
  {
    for (auto const& message : *aggregate_projection)
      aggregate_image_parts += static_cast<std::size_t>(
          std::ranges::count_if(message.content_parts, [](auto const& part) { return part.type == ava::provider::ContentPartType::Image; }));
  }
  expect(aggregate_projection && aggregate_image_parts == 3 &&
             aggregate_projection->front().content.find("[historical image omitted: mime=image/png bytes=15728640]") != std::string::npos &&
             (*aggregate_projection)[1].content.find("[historical image: mime=image/png bytes=15728640]") != std::string::npos &&
             (*aggregate_projection)[2].content.find("[historical image: mime=image/png bytes=15728640]") != std::string::npos,
         "historical image aggregate-byte admission keeps the newest compatible images after reserving the active image bytes");

  std::vector<ava::session::SessionEntry> oversized = {image_entry(20, 6 * 1024 * 1024), image_entry(21, 6 * 1024 * 1024)};
  target.provider_id = "anthropic";
  target.model_id = "claude-image";
  target.api_family = "anthropic_messages";
  target.reasoning_format = "anthropic_thinking";
  auto anthropic =
      ava::agent::build_provider_messages_from_entries(oversized, ava::agent::MessageBuildOptions{.target = target, .active_turn_user_entry_ids = {"user_21"}});
  expect(anthropic && anthropic->size() == 2 && anthropic->front().content_parts.size() == 2 &&
             anthropic->front().content_parts.back().text == "[historical image omitted: mime=image/png bytes=6291456]" &&
             anthropic->front().content.find("[historical image omitted: mime=image/png bytes=6291456]") != std::string::npos &&
             anthropic->front().content.find("img_20") == std::string::npos && anthropic->back().content_parts.size() == 2,
         "provider-specific per-image policy replaces an incompatible historical image while never spending active-turn capacity on history");

  target.supports_images = false;
  auto text_only = ava::agent::build_provider_messages_from_entries(oversized, ava::agent::MessageBuildOptions{.target = target});
  expect(text_only && std::ranges::all_of(*text_only,
                                          [](auto const& message) {
                                            return std::ranges::none_of(message.content_parts,
                                                                        [](auto const& part) { return part.type == ava::provider::ContentPartType::Image; }) &&
                                                   message.content.find("[historical image omitted: mime=image/png bytes=6291456]") != std::string::npos &&
                                                   message.content.find("img_") == std::string::npos &&
                                                   message.content.find("attachments/") == std::string::npos;
                                          }),
         "a text-only switched target receives only MIME-and-size historical image placeholders without attachment identifiers or paths");
}

void test_agent_loop_assistant_turn_lifecycle_validation()
{
  auto event = [](ava::provider::StreamEventType type) {
    auto value = ava::provider::StreamEvent{};
    value.type = type;
    return value;
  };
  auto text_start = event(ava::provider::StreamEventType::TextStart);
  text_start.provider_item_id = "msg_lifecycle";
  text_start.provider_output_index = 1;
  text_start.assistant_phase = ava::provider::AssistantPhase::Commentary;
  auto text_delta = event(ava::provider::StreamEventType::TextDelta);
  text_delta.text = "working";
  text_delta.provider_item_id = "msg_lifecycle";
  text_delta.provider_output_index = 1;
  text_delta.assistant_phase = ava::provider::AssistantPhase::Commentary;
  auto text_end = text_start;
  text_end.type = ava::provider::StreamEventType::TextEnd;
  auto call_start = event(ava::provider::StreamEventType::ToolCallStart);
  call_start.tool_call_id = "call_lifecycle";
  call_start.tool_name = "read_file";
  call_start.provider_item_id = "fc_lifecycle";
  call_start.provider_output_index = 2;
  auto call_delta = call_start;
  call_delta.type = ava::provider::StreamEventType::ToolCallDelta;
  call_delta.text = "{}";
  call_delta.tool_name.clear();
  auto call_end = call_start;
  call_end.type = ava::provider::StreamEventType::ToolCallEnd;
  call_end.tool_name.clear();
  auto done = event(ava::provider::StreamEventType::Done);
  done.finish_reason = ava::provider::ProviderFinishReason::ToolCalls;
  auto const complete = ava::agent::parse_assistant_turn({text_start, text_delta, text_end, call_start, call_delta, call_end, done}, {});

  auto incomplete_start = event(ava::provider::StreamEventType::ToolCallStart);
  incomplete_start.tool_call_id = "call_incomplete";
  incomplete_start.tool_name = "read_file";
  incomplete_start.provider_item_id = "fc_incomplete";
  auto const incomplete_call = ava::agent::parse_assistant_turn({incomplete_start, done}, {});
  auto legacy_call_start = event(ava::provider::StreamEventType::ToolCallStart);
  legacy_call_start.tool_call_id = "call_legacy";
  legacy_call_start.tool_name = "read_file";
  auto const legacy_call = ava::agent::parse_assistant_turn({legacy_call_start, done}, {});
  auto reordered_text_start = text_start;
  reordered_text_start.provider_item_id = "msg_index_one";
  reordered_text_start.provider_output_index = 1;
  auto reordered_text_delta = text_delta;
  reordered_text_delta.provider_item_id = "msg_index_one";
  reordered_text_delta.provider_output_index = 1;
  auto reordered_text_end = text_end;
  reordered_text_end.provider_item_id = "msg_index_one";
  reordered_text_end.provider_output_index = 1;
  auto reordered_reasoning_start = event(ava::provider::StreamEventType::ReasoningStart);
  reordered_reasoning_start.provider_item_id = "rs_index_zero";
  reordered_reasoning_start.provider_output_index = 0;
  auto reordered_reasoning_end = reordered_reasoning_start;
  reordered_reasoning_end.type = ava::provider::StreamEventType::ReasoningEnd;
  auto completed_done = done;
  completed_done.finish_reason = ava::provider::ProviderFinishReason::Completed;
  auto const reordered = ava::agent::parse_assistant_turn(
      {reordered_text_start, reordered_text_delta, reordered_text_end, reordered_reasoning_start, reordered_reasoning_end, completed_done}, {});
  auto const reordered_first_is_reasoning =
      reordered && !reordered->ordered_items.empty() && std::holds_alternative<ava::agent::AssistantReasoningItem>(reordered->ordered_items.front().item);
  expect(complete && complete->ordered_items.size() == 2 && complete->text == "working" && complete->tool_calls.size() == 1 &&
             complete->tool_calls[0].id == "call_lifecycle" && !incomplete_call && legacy_call && legacy_call->tool_calls.size() == 1 &&
             reordered_first_is_reasoning,
         "agent turn assembly orders fully indexed native items by output_index, rejects incomplete native calls, and retains legacy tool turns");
}
