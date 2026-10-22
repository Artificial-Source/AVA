#include "sys.h"
#include "tests/provider_openai_test_suite.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/agent/assistant_turn.h"
#include "ava/provider/finish_reason.h"
#include "ava/provider/openai_response_parser.h"
#include "ava/provider/provider.h"
#include "ava/provider/registry.h"
#include "ava/core/error.h"
#include "ava/core/result.h"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace ava::tests::provider_openai_suite {

void test_openai_responses_refusal_and_unsupported_output()
{
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto const non_stream = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"status":"completed","output":[{"id":"msg_refusal_nonstream","type":"message","output_index":0,"phase":"final_answer","content":[{"type":"output_text","text":"Visible preface."},{"type":"refusal","refusal":"I cannot help with that."}]}]})"},
      false);
  auto const parsed_text = ava::provider::parse_openai_response_text(
      R"({"output":[{"type":"message","content":[{"type":"output_text","text":"Visible preface."},{"type":"refusal","refusal":"I cannot help with that."}]}]})");
  auto const non_stream_turn = non_stream ? ava::agent::parse_assistant_turn(*non_stream, {}) : ava::core::Result<ava::agent::ParsedAssistantTurn>{};
  expect(non_stream && non_stream_turn && non_stream_turn->finish_reason == ava::provider::ProviderFinishReason::Refusal &&
             non_stream_turn->text == "Visible preface.I cannot help with that." && parsed_text &&
             *parsed_text == "Visible preface.\n\nI cannot help with that.",
         "OpenAI non-stream Responses preserves visible refusal content and marks the terminal outcome as refusal");

  auto const stream = ava::provider::parse_openai_sse(
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_refusal_stream\",\"type\":\"message\",\"phase\":\"final_answer\"}}"
      "\n\n"
      "data: {\"type\":\"response.refusal.delta\",\"item_id\":\"msg_refusal_stream\",\"output_index\":0,\"content_index\":0,\"delta\":\"I cannot \"}\n\n"
      "data: {\"type\":\"response.refusal.done\",\"item_id\":\"msg_refusal_stream\",\"output_index\":0,\"content_index\":0,\"refusal\":\"I cannot help.\"}\n\n"
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_refusal_stream\",\"type\":\"message\",\"phase\":\"final_answer\","
      "\"content\":[{\"type\":\"refusal\",\"refusal\":\"I cannot help.\"}]}}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n");
  auto const stream_turn = stream ? ava::agent::parse_assistant_turn(*stream, {}) : ava::core::Result<ava::agent::ParsedAssistantTurn>{};
  expect(stream && stream_turn && stream_turn->finish_reason == ava::provider::ProviderFinishReason::Refusal && stream_turn->text == "I cannot help.",
         "OpenAI streaming Responses reconciles refusal deltas/done text and marks the terminal outcome as refusal");

  auto has_content_free_error = [](auto const& events, std::string_view discriminator, std::string_view canary) {
    if (!events)
      return false;
    auto const error =
        std::ranges::find_if(*events, [](ava::provider::StreamEvent const& event) { return event.type == ava::provider::StreamEventType::Error; });
    return error != events->end() && error->error_message.find("unsupported") != std::string::npos &&
           error->error_message.find(discriminator) == std::string::npos && error->error_message.find(canary) == std::string::npos;
  };
  auto const non_stream_unsupported_item = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body = R"({"status":"completed","output":[{"id":"unknown","type":"image_generation_call","output_index":0,"private_canary":"NO_LEAK"}]})"},
      false);
  auto const non_stream_unsupported_content = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"status":"completed","output":[{"id":"message_unknown_content","type":"message","output_index":0,"content":[{"type":"input_image","private_canary":"NO_LEAK"}]}]})"},
      false);
  auto const stream_unsupported_added = ava::provider::parse_openai_sse(
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":5,\"item\":{\"id\":\"unknown_stream\",\"type\":\"image_generation_call\",\"private_canary\":"
      "\"NO_LEAK\"}}\n\n");
  auto const stream_unsupported_done = ava::provider::parse_openai_sse(
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":6,\"item\":{\"id\":\"unknown_stream_done\",\"type\":\"image_generation_call\",\"private_"
      "canary\":\"NO_LEAK\"}}\n\n");
  auto const stream_unsupported_content = ava::provider::parse_openai_sse(
      "data: "
      "{\"type\":\"response.output_item.added\",\"output_index\":7,\"item\":{\"id\":\"message_unknown_content_stream\",\"type\":\"message\",\"content\":[{"
      "\"type\":\"input_image\",\"private_canary\":\"NO_LEAK\"}]}}\n\n");
  expect(has_content_free_error(non_stream_unsupported_item, "image_generation_call", "NO_LEAK") &&
             has_content_free_error(non_stream_unsupported_content, "input_image", "NO_LEAK") &&
             has_content_free_error(stream_unsupported_added, "image_generation_call", "NO_LEAK") &&
             has_content_free_error(stream_unsupported_done, "image_generation_call", "NO_LEAK") &&
             has_content_free_error(stream_unsupported_content, "input_image", "NO_LEAK"),
         "OpenAI Responses rejects unsupported discriminators without exposing their values or payloads");
}

void test_closed_provider_finish_reason_catalog()
{
  bool exhaustive = true;
  for (auto const& mapping : ava::provider::kProviderFinishReasonCatalog)
  {
    exhaustive = exhaustive && ava::provider::normalize_provider_finish_reason(mapping.protocol, mapping.raw_reason) == mapping.reason;
  }
  exhaustive =
      exhaustive &&
      ava::provider::normalize_provider_finish_reason(ava::provider::ProviderProtocol::OpenAIChat, "provider_custom") ==
          ava::provider::ProviderFinishReason::Error &&
      ava::provider::normalize_provider_finish_reason(ava::provider::ProviderProtocol::Gemini, "OTHER") == ava::provider::ProviderFinishReason::Error &&
      ava::provider::normalize_provider_finish_reason(ava::provider::ProviderProtocol::Gemini, "SAFETY") == ava::provider::ProviderFinishReason::Refusal &&
      ava::provider::normalize_provider_finish_reason(ava::provider::ProviderProtocol::OpenAIResponses, "incomplete") ==
          ava::provider::ProviderFinishReason::Error;
  expect(exhaustive, "provider finish reasons use one closed catalog and unknown/failed terminals cannot become completed");
}

void test_openai_provider_parser_budgets()
{
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto array_json = [](std::size_t count, auto const& item) {
    std::string array = "[";
    for (std::size_t index = 0; index < count; ++index)
    {
      if (index != 0)
        array += ',';
      array += item(index);
    }
    return array + "]";
  };
  auto is_one_limit_error = [](auto const& events) {
    return events && events->size() == 1 && events->front().type == ava::provider::StreamEventType::Error &&
           events->front().error_message == "OpenAI response parser limit exceeded";
  };
  auto check_boundary = [&](auto const& events, bool at_limit, std::string const& label) {
    if (!at_limit)
    {
      expect(is_one_limit_error(events), label + " rejects limit+1 before expanding provider events");
      return;
    }
    if (!ava::tests::require(events.has_value(), label + " accepts exactly the array limit"))
      return;
    ava::tests::expect_all(label + " completes at the array limit",
                           {{"events present", !events->empty()},
                            {"no error", std::ranges::none_of(*events, [](auto const& event) { return event.type == ava::provider::StreamEventType::Error; })},
                            {"completed terminal", !events->empty() && events->back().type == ava::provider::StreamEventType::Done &&
                                                       events->back().finish_reason == ava::provider::ProviderFinishReason::Completed}});
  };
  for (auto const count : {ava::provider::kMaxProviderParserArrayItems, ava::provider::kMaxProviderParserArrayItems + 1})
  {
    bool const at_limit = count == ava::provider::kMaxProviderParserArrayItems;
    auto const output =
        "{\"status\":\"completed\",\"output\":" +
        array_json(count,
                   [](std::size_t index) {
                     return "{\"id\":\"msg_" + std::to_string(index) + "\",\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"x\"}]}";
                   }) +
        "}";
    auto const non_stream = provider.parse_response({.status_code = 200, .headers = {}, .body = output}, false);
    check_boundary(non_stream, at_limit, "OpenAI non-stream output array");

    auto const content = array_json(count, [](std::size_t) { return "{\"type\":\"output_text\",\"text\":\"x\"}"; });
    auto const summary = array_json(count, [](std::size_t) { return "{\"text\":\"x\"}"; });
    std::string const terminal = "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n";
    auto const content_events = ava::provider::parse_openai_sse(
        "data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"id\":\"msg_nested\",\"type\":\"message\",\"content\":" + content +
        "}}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"msg_nested\",\"type\":\"message\"}}\n\n" +
        terminal);
    auto const summary_events = ava::provider::parse_openai_sse(
        "data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"id\":\"rs_nested\",\"type\":\"reasoning\",\"summary\":" + summary +
        "}}\n\n" + terminal);
    check_boundary(content_events, at_limit, "OpenAI SSE nested content array");
    check_boundary(summary_events, at_limit, "OpenAI SSE reasoning-summary array");
    if (at_limit && non_stream && content_events && summary_events)
    {
      auto text = [](auto const& events, ava::provider::StreamEventType type) {
        std::string value;
        for (auto const& event : events)
          if (event.type == type)
            value += event.text;
        return value;
      };
      auto const expected_text = std::string(count, 'x');
      ava::tests::expect_eq(text(*non_stream, ava::provider::StreamEventType::TextDelta), expected_text, "at-limit output retains every item");
      ava::tests::expect_eq(text(*content_events, ava::provider::StreamEventType::TextDelta), expected_text, "at-limit content retains every part");
      std::string expected_summary = "x";
      for (std::size_t index = 1; index < count; ++index)
        expected_summary += "\n\nx";
      ava::tests::expect_eq(text(*summary_events, ava::provider::StreamEventType::ReasoningDelta), expected_summary,
                            "at-limit reasoning summary retains every part in order");
    }
  }

  ava::provider::OpenAIStreamParser parser;
  std::string single_append;
  for (std::size_t index = 0; index <= ava::provider::kMaxProviderParserEvents; ++index)
    single_append += "data: {\"type\":\"response.output_text.delta\",\"delta\":\"x\"}\n\n";
  auto streamed = parser.append(single_append);
  auto after_limit = parser.append("data: {\"type\":\"response.output_text.delta\",\"delta\":\"STREAM_AFTER_LIMIT_CANARY\"}\n\n");
  auto finished = parser.finish();
  expect(streamed && streamed->size() == ava::provider::kMaxProviderParserEvents && !streamed->empty() &&
             streamed->back().type == ava::provider::StreamEventType::Error && streamed->back().error_message == "OpenAI response parser limit exceeded" &&
             streamed->back().error_message.find("STREAM_AFTER_LIMIT_CANARY") == std::string::npos && after_limit && after_limit->empty() && finished &&
             finished->empty(),
         "one large OpenAI SSE append is bounded to 4096 events, emits one fixed error, and terminally ignores later provider records");

  ava::provider::OpenAIStreamParser fragmented_parser;
  std::string budget_prefix;
  for (std::size_t index = 1; index < ava::provider::kMaxProviderParserEvents; ++index)
    budget_prefix += "data: {\"type\":\"response.output_text.delta\",\"delta\":\"x\"}\n\n";
  auto fragmented_prefix = fragmented_parser.append(budget_prefix);
  auto fragmented_terminal = fragmented_parser.append("data: {\"type\":\"response.output_text.delta\",\"delta\":\"FRAGMENT_AFTER_LIMIT_CANARY\"}\n\n");
  expect(fragmented_prefix && fragmented_prefix->size() == ava::provider::kMaxProviderParserEvents - 1 && fragmented_terminal &&
             fragmented_terminal->size() == 1 && (*fragmented_terminal)[0].type == ava::provider::StreamEventType::Error &&
             (*fragmented_terminal)[0].error_message == "OpenAI response parser limit exceeded" &&
             (*fragmented_terminal)[0].error_message.find("FRAGMENT_AFTER_LIMIT_CANARY") == std::string::npos,
         "OpenAI SSE parser applies its event budget across fragmented provider chunks");

  std::string const record_prefix = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"";
  std::string const record_suffix = "\"}\n\n";
  for (auto const size : {ava::provider::kMaxProviderSseBufferedBytes, ava::provider::kMaxProviderSseBufferedBytes + 1})
  {
    ava::provider::OpenAIStreamParser record_parser;
    auto const text = std::string(size - record_prefix.size() - record_suffix.size(), 'x');
    auto const record = record_prefix + text + record_suffix;
    auto const parsed = record_parser.append(record);
    if (size > ava::provider::kMaxProviderSseBufferedBytes)
    {
      expect(is_one_limit_error(parsed), "OpenAI SSE rejects a valid record exactly one byte above its buffer limit");
      continue;
    }
    auto const done = record_parser.append("data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n");
    auto const final = record_parser.finish();
    ava::tests::expect_all(
        "OpenAI SSE accepts a valid record exactly at its buffer limit",
        {{"complete text", parsed && parsed->size() == 1 && parsed->front().type == ava::provider::StreamEventType::TextDelta && parsed->front().text == text},
         {"completed terminal", done && done->size() == 1 && done->front().type == ava::provider::StreamEventType::Done &&
                                    done->front().finish_reason == ava::provider::ProviderFinishReason::Completed},
         {"no trailing error", final && final->empty()}});
  }
}

void test_builtin_provider_registry()
{
  auto registry = ava::provider::builtin_provider_registry();
  expect(registry.contains("openai"), "builtin provider registry contains OpenAI");
  expect(registry.contains("deepseek") && registry.contains("kimi") && registry.contains("moonshot") && registry.contains("openrouter") &&
             registry.contains("zai") && registry.contains("zai-coding-cn"),
         "builtin provider registry contains OpenAI-compatible provider shims");
  auto provider = registry.create("openai");
  expect(provider.has_value() && *provider, "builtin provider registry creates OpenAI provider");
  auto kimi = registry.create("kimi");
  expect(kimi.has_value() && *kimi, "builtin provider registry creates Kimi provider shim");
  auto deepseek = registry.create("deepseek");
  expect(deepseek.has_value() && *deepseek, "builtin provider registry creates DeepSeek provider shim");
  auto zai = registry.create("zai");
  expect(zai.has_value() && *zai, "builtin provider registry creates Z.AI provider shim");
  auto zai_cn = registry.create("zai-coding-cn");
  expect(zai_cn.has_value() && *zai_cn, "builtin provider registry creates Z.AI Coding CN provider shim");

  auto missing = registry.create("missing-provider");
  expect(!missing && missing.error().category() == ava::core::ErrorCategory::NotFound, "provider registry rejects unknown providers");
}

}  // namespace ava::tests::provider_openai_suite
