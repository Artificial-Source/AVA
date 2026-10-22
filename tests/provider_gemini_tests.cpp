#include "tests/support/agent_loop_test_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/agent/assistant_turn.h"
#include "ava/agent/message_builder.h"
#include "ava/agent/stream_bridge.h"
#include "ava/config/model_config.h"
#include "ava/config/provider_profiles.h"
#include "ava/provider/gemini_provider.h"
#include "ava/provider/registry.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void test_gemini_profiles_and_registry()
{
  auto const profile = ava::config::find_provider_profile("gemini");
  expect(profile && profile->display_name == "Google Gemini" && profile->default_base_url_env == "GEMINI_BASE_URL" && profile->runtime_selectable,
         "Gemini provider profile is runtime selectable with base URL override metadata");

  auto const registry = ava::config::builtin_model_registry();
  auto const model = ava::config::find_model(registry, "gemini", "gemini-2.5-pro");
  expect(model && model->provider_id == "gemini" && model->api_family == "gemini_generate_content" &&
             std::ranges::find(model->input_modalities, "image") != model->input_modalities.end() && model->supports_tools.value_or(false) &&
             model->supports_streaming.value_or(false),
         "Gemini model profile is available with generateContent capabilities");

  auto providers = ava::provider::builtin_provider_registry();
  auto gemini = providers.create("gemini");
  expect(gemini.has_value() && *gemini, "builtin provider registry creates Gemini provider");
}

void test_gemini_provider_contract()
{
  constexpr std::string_view system_prompt_marker = "AVA-ISSUE-54-GEMINI-SYSTEM-PROMPT";
  ava::provider::GeminiProvider const provider("https://gemini.example.test/");
  auto const request = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "gemini",
          .model_id = "gemini-2.5-pro",
          .system_prompt = std::string(system_prompt_marker),
          .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello \"ava\""},
                       ava::provider::ChatMessage{.role = "assistant", .content = "hi"}},
          .tools_json =
              {R"({"type":"function","name":"read_file","description":"Read","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}})"},
          .stream = true,
          .max_output_tokens = 2048},
      "gemini-key");
  expect(request.has_value(), "Gemini request builds with API key");
  if (request)
  {
    expect(request->method == "POST" && request->url == "https://gemini.example.test/v1beta/models/gemini-2.5-pro:streamGenerateContent?alt=sse",
           "Gemini streaming request targets streamGenerateContent SSE endpoint with trimmed base URL");
    expect(request->headers.at("x-goog-api-key") == "gemini-key", "Gemini request uses x-goog-api-key header");
    expect(request->headers.at("Accept") == "text/event-stream", "Gemini streaming request asks for SSE");
    expect(ava::tests::count_occurrences(request->body, system_prompt_marker) == 1, "Gemini request serializes the effective system prompt exactly once");
    auto const expected_system_instruction = std::string("\"systemInstruction\":{\"parts\":[{\"text\":\"") + std::string(system_prompt_marker) + "\"}]}";
    expect(request->body.find(expected_system_instruction) != std::string::npos,
           "Gemini request places the effective system prompt in the top-level systemInstruction parts text");
    expect(
        request->body.find(R"("contents":[{"role":"user","parts":[{"text":"hello \"ava\""}]},{"role":"model","parts":[{"text":"hi"}]})") != std::string::npos,
        "Gemini request maps assistant role to model and escapes content");
    expect(
        request->body.find(R"("tools":[{"functionDeclarations":[{"name":"read_file","description":"Read","parameters":{"type":"object")") != std::string::npos,
        "Gemini request converts OpenAI-style tools to functionDeclarations");
    expect(request->body.find(R"("generationConfig":{"maxOutputTokens":2048})") != std::string::npos,
           "Gemini request serializes max output tokens in generationConfig");
  }

  auto const non_stream = provider.build_request(ava::provider::ProviderRequest{.provider_id = "gemini",
                                                                                .model_id = "models/gemini-2.5-pro",
                                                                                .system_prompt = "",
                                                                                .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
                                                                                .tools_json = {},
                                                                                .stream = false},
                                                 "gemini-key");
  expect(non_stream && non_stream->url == "https://gemini.example.test/v1beta/models/gemini-2.5-pro:generateContent" &&
             non_stream->headers.at("Accept") == "application/json",
         "Gemini non-streaming request targets generateContent and accepts JSON");

  auto const native_parts = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "gemini",
          .model_id = "gemini-2.5-pro",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "assistant",
                                                  .content = "fallback call",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                                               .tool_call_id = "call_1",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = R"({"path":"README.md"})"}}},
                       ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback result",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolResult,
                                                                                               .text = "file contents",
                                                                                               .tool_call_id = "call_1",
                                                                                               .tool_name = "read_file",
                                                                                               .is_error = false}}}},
          .tools_json = {},
          .stream = false},
      "gemini-key");
  expect(native_parts && native_parts->body.find(R"({"functionCall":{"id":"call_1","name":"read_file","args":{"path":"README.md"}}})") != std::string::npos &&
             native_parts->body.find(R"({"functionResponse":{"id":"call_1","name":"read_file","response":{"content":"file contents"}}})") != std::string::npos,
         "Gemini request serializes native functionCall and functionResponse parts");

  auto const image_request = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "gemini",
          .model_id = "gemini-2.5-pro",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{
              .role = "user",
              .content = "fallback image metadata",
              .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text, .text = "describe this"},
                                ava::provider::ContentPart{.type = ava::provider::ContentPartType::Image,
                                                           .attachment_id = "img_1",
                                                           .mime_type = "image/png",
                                                           .storage_path = "attachments/img_1.png",
                                                           .sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                                           .byte_size = 3,
                                                           .data_base64 = "aGk="}}}},
          .tools_json = {},
          .stream = false},
      "gemini-key");
  expect(image_request && image_request->body.find(R"({"text":"describe this"})") != std::string::npos &&
             image_request->body.find(R"({"inlineData":{"mimeType":"image/png","data":"aGk="}})") != std::string::npos,
         "Gemini request serializes verified image content parts");

  auto const missing_image_bytes = provider.build_request(
      ava::provider::ProviderRequest{.provider_id = "gemini",
                                     .model_id = "gemini-2.5-pro",
                                     .system_prompt = "system",
                                     .messages = {ava::provider::ChatMessage{.role = "user",
                                                                             .content = "fallback image metadata",
                                                                             .content_parts = {ava::provider::ContentPart{
                                                                                 .type = ava::provider::ContentPartType::Image,
                                                                                 .attachment_id = "img_1",
                                                                                 .mime_type = "image/png",
                                                                                 .storage_path = "attachments/img_1.png",
                                                                                 .sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                                                                 .byte_size = 3}}}},
                                     .tools_json = {},
                                     .stream = false},
      "gemini-key");
  expect(!missing_image_bytes && missing_image_bytes.error().message().find("verified attachment bytes") != std::string::npos,
         "Gemini request rejects image content without verified attachment bytes");

  auto const missing_token = provider.build_request(
      ava::provider::ProviderRequest{.provider_id = "gemini", .model_id = "gemini-2.5-pro", .system_prompt = "", .messages = {}, .tools_json = {}}, "");
  expect(!missing_token && missing_token.error().category() == ava::core::ErrorCategory::PermissionDenied, "Gemini request rejects empty API key");

  auto const invalid_model = provider.build_request(
      ava::provider::ProviderRequest{.provider_id = "gemini", .model_id = "bad/model", .system_prompt = "", .messages = {}, .tools_json = {}}, "gemini-key");
  expect(!invalid_model && invalid_model.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Gemini request rejects unsafe model IDs before URL construction");

  auto const unsupported_reasoning = provider.build_request(ava::provider::ProviderRequest{.provider_id = "gemini",
                                                                                           .model_id = "gemini-2.5-pro",
                                                                                           .system_prompt = "",
                                                                                           .messages = {},
                                                                                           .tools_json = {},
                                                                                           .reasoning = ava::provider::ProviderReasoningOptions{.type = "low"}},
                                                            "gemini-key");
  expect(!unsupported_reasoning && unsupported_reasoning.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Gemini request fails closed for unsupported reasoning options");
}

void test_gemini_response_parsing()
{
  ava::provider::GeminiProvider const provider("https://gemini.example.test");
  auto const response = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"candidates":[{"content":{"role":"model","parts":[{"text":"I will read it."},{"functionCall":{"id":"call_1","name":"read_file","args":{"path":"README.md"}}}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":2,"candidatesTokenCount":3,"totalTokenCount":5,"thoughtsTokenCount":1,"cachedContentTokenCount":1}})"},
      false);
  expect(response && response->size() == 5 && (*response)[0].type == ava::provider::StreamEventType::TextDelta && (*response)[0].text == "I will read it." &&
             (*response)[1].type == ava::provider::StreamEventType::ToolCallStart && (*response)[1].tool_call_id == "call_1" &&
             (*response)[1].tool_name == "read_file" && (*response)[2].type == ava::provider::StreamEventType::ToolCallDelta &&
             (*response)[2].text.find("README.md") != std::string::npos && (*response)[3].type == ava::provider::StreamEventType::ToolCallEnd &&
             (*response)[4].type == ava::provider::StreamEventType::Done && (*response)[4].finish_reason == ava::provider::ProviderFinishReason::Completed &&
             (*response)[4].usage && (*response)[4].usage->input_tokens == 2 && (*response)[4].usage->output_tokens == 3 &&
             (*response)[4].usage->total_tokens == 5 && (*response)[4].usage->reasoning_tokens == 1 && (*response)[4].usage->cache_read_tokens == 1,
         "Gemini non-stream response parses text, functionCall, finish reason, and usage");

  auto const missing_id_body = ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body =
          R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"read_file","args":{"path":"README.md"}}}]},"finishReason":"STOP"}]})"};
  auto const first_missing_id = provider.parse_response(missing_id_body, false);
  auto const second_missing_id = provider.parse_response(missing_id_body, false);
  auto const first_fallback = first_missing_id && first_missing_id->size() >= 3 ? (*first_missing_id)[0].tool_call_id : std::string{};
  auto const second_fallback = second_missing_id && second_missing_id->size() >= 3 ? (*second_missing_id)[0].tool_call_id : std::string{};
  expect(!first_fallback.empty() && !second_fallback.empty() && first_fallback != second_fallback && (*first_missing_id)[1].tool_call_id == first_fallback &&
             (*first_missing_id)[2].tool_call_id == first_fallback,
         "Gemini missing function-call IDs use globally distinct per-response fallbacks while same-turn fragments merge");

  auto const missing_content =
      provider.parse_response(ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = R"({"candidates":[{"finishReason":"STOP"}]})"}, false);
  expect(!missing_content && missing_content.error().message().find("content is missing") != std::string::npos,
         "Gemini non-stream response requires content events");

  auto const http_error = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 403,
          .headers = {},
          .body =
              R"({"error":{"message":"bad key","x-goog-api-key":"GEMINI_HTTP_KEY_CANARY","unknown":{"private":"GEMINI_HTTP_NESTED_CANARY"}},"unknown":"GEMINI_HTTP_OUTER_CANARY"})"},
      false);
  auto const error_text = http_error ? std::string{} : http_error.error().format();
  expect(!http_error && error_text.find("provider_error_kind: authentication") != std::string::npos &&
             error_text.find("provider_message") == std::string::npos && error_text.find("body_snippet") == std::string::npos &&
             error_text.find("GEMINI_HTTP_KEY_CANARY") == std::string::npos && error_text.find("GEMINI_HTTP_NESTED_CANARY") == std::string::npos &&
             error_text.find("GEMINI_HTTP_OUTER_CANARY") == std::string::npos,
         "Gemini HTTP errors expose only fixed local status and classification diagnostics");
  // Prompt-level blocks carry no content; candidate-level refusals can carry a
  // visible refusal. Both must remain semantic refusals, never successful work.
  struct RefusalCase
  {
    char const* name;
    char const* body;
    char const* text;
  };
  for (
      auto const& refusal :
      {RefusalCase{"prompt SAFETY", R"({"promptFeedback":{"blockReason":"SAFETY"}})", ""},
       RefusalCase{
           "prompt SAFETY suppresses candidate tools",
           R"({"promptFeedback":{"blockReason":"SAFETY"},"candidates":[{"content":{"parts":[{"functionCall":{"name":"read_file","args":{"path":"blocked.txt"}}}]}}]})",
           ""},
       RefusalCase{"candidate SAFETY", R"({"candidates":[{"content":{"parts":[{"text":"Cannot comply."}]},"finishReason":"SAFETY"}]})", "Cannot comply."},
       RefusalCase{"candidate RECITATION", R"({"candidates":[{"content":{"parts":[{"text":"Cannot comply."}]},"finishReason":"RECITATION"}]})",
                   "Cannot comply."}})
  {
    for (bool const stream : {false, true})
    {
      auto const body = std::string(refusal.body);
      auto const blocked = provider.parse_response({.status_code = 200, .headers = {}, .body = stream ? "data: " + body + "\n\n" : body}, stream);
      auto const label = std::string("Gemini ") + (stream ? "SSE " : "non-stream ") + refusal.name;
      if (!ava::tests::require(blocked.has_value(), label + " parses"))
        continue;
      auto const turn = ava::agent::parse_assistant_turn(*blocked, {});
      ava::tests::expect_all(
          label + " cannot become an empty completed answer or a partial tool call",
          {{"one terminal", std::ranges::count_if(*blocked, [](auto const& event) { return event.type == ava::provider::StreamEventType::Done; }) == 1},
           {"refusal terminal", !blocked->empty() && blocked->back().type == ava::provider::StreamEventType::Done &&
                                    blocked->back().finish_reason == ava::provider::ProviderFinishReason::Refusal},
           {"no tool lifecycle", std::ranges::none_of(*blocked,
                                                      [](auto const& event) {
                                                        return event.type == ava::provider::StreamEventType::ToolCallStart ||
                                                               event.type == ava::provider::StreamEventType::ToolCallDelta ||
                                                               event.type == ava::provider::StreamEventType::ToolCallEnd;
                                                      })},
           {"assistant turn refusal", turn && turn->finish_reason == ava::provider::ProviderFinishReason::Refusal},
           {"no dispatchable tools", turn && turn->tool_calls.empty()}});
      if (turn)
        ava::tests::expect_eq(turn->text, std::string(refusal.text), label + " preserves only the visible refusal content");
    }
  }
}

void test_gemini_sse_parsing_and_fake_transport()
{
  std::string const sse =
      "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hel\"}]}}]}\n\n"
      "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"lo\"}]},\"finishReason\":\"STOP\"}],"
      "\"usageMetadata\":{\"promptTokenCount\":4,\"candidatesTokenCount\":5,\"totalTokenCount\":9}}\n\n";
  auto events = ava::provider::parse_gemini_sse(sse);
  expect(events && events->size() == 3 && (*events)[0].type == ava::provider::StreamEventType::TextDelta && (*events)[0].text == "hel" &&
             (*events)[1].type == ava::provider::StreamEventType::TextDelta && (*events)[1].text == "lo" &&
             (*events)[2].type == ava::provider::StreamEventType::Done && (*events)[2].usage && (*events)[2].usage->input_tokens == 4 &&
             (*events)[2].usage->output_tokens == 5 && (*events)[2].usage->total_tokens == 9,
         "Gemini SSE parser streams text deltas and emits terminal usage on finish");

  ava::provider::GeminiStreamParser parser;
  auto first = parser.append("data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"par");
  expect(first && first->empty(), "Gemini incremental parser buffers partial SSE data");
  auto second = parser.append("tial\"}]}}]}\n\n");
  auto final = parser.finish();
  expect(second && second->size() == 1 && (*second)[0].text == "partial" && final && final->size() == 1 &&
             (*final)[0].type == ava::provider::StreamEventType::Done,
         "Gemini incremental parser handles split chunks");

  ava::provider::GeminiProvider const provider("https://gemini.example.test");
  auto request = provider.build_request(ava::provider::ProviderRequest{.provider_id = "gemini",
                                                                       .model_id = "gemini-2.5-pro",
                                                                       .system_prompt = "system",
                                                                       .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
                                                                       .tools_json = {},
                                                                       .stream = false},
                                        "gemini-key");
  expect(request.has_value(), "Gemini fake transport request builds");
  if (request)
  {
    ava::tests::FakeTransport transport({ava::http::HttpResponse{
        .status_code = 200, .headers = {}, .body = R"({"candidates":[{"content":{"parts":[{"text":"ok"}]},"finishReason":"STOP"}]})"}});
    auto fake_response = transport.send(*request);
    auto parsed = fake_response ? provider.parse_response(*fake_response, false) : ava::core::Result<std::vector<ava::provider::StreamEvent>>{};
    expect(fake_response && parsed && parsed->size() == 2 && (*parsed)[0].text == "ok" && transport.requests().size() == 1 &&
               transport.requests()[0].headers.at("x-goog-api-key") == "gemini-key",
           "Gemini provider works through fake transport without live credentials");
  }
}

void test_gemini_requests_disable_redirects()
{
  ava::provider::GeminiProvider const provider("https://gemini.example.test");
  ava::provider::ProviderRequest request{
      .provider_id = "gemini", .model_id = "gemini-2.5-pro", .system_prompt = "", .messages = {}, .tools_json = {}, .stream = false};
  auto const non_stream = provider.build_request(request, "gemini-key");
  expect(non_stream && !non_stream->follow_redirects && non_stream->headers.at("x-goog-api-key") == "gemini-key",
         "Gemini non-streaming requests never follow redirects with the API key");

  request.stream = true;
  auto const stream = provider.build_request(request, "gemini-key");
  expect(stream && !stream->follow_redirects && stream->headers.at("x-goog-api-key") == "gemini-key",
         "Gemini streaming requests never follow redirects with the API key");
}

void test_gemini_function_call_ids_across_stream_records()
{
  std::string const sse =
      "data: {\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{\"name\":\"read_file\",\"args\":{\"path\":\"README.md\"}}}]}}]}\n\n"
      "data: "
      "{\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{\"id\":\"provider-call_7\",\"name\":\"read_file\",\"args\":{\"path\":\"src/"
      "main.cpp\"}}}]}}]}\n\n"
      "data: "
      "{\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{\"name\":\"read_file\",\"args\":{\"path\":\"AGENTS.md\"}}}]},\"finishReason\":\"STOP\"}]}";

  for (bool const terminated : {false, true})
  {
    auto const input = sse + (terminated ? "\n\n" : "");
    for (std::size_t const chunk_size : {input.size(), std::size_t{1}})
    {
      ava::provider::GeminiStreamParser parser;
      std::vector<ava::provider::StreamEvent> events;
      for (std::size_t offset = 0; offset < input.size(); offset += chunk_size)
      {
        auto chunk_events = parser.append(std::string_view(input).substr(offset, chunk_size));
        expect(chunk_events.has_value(), "Gemini function-call stream accepts complete and byte-fragmented chunks");
        if (!chunk_events)
          return;
        events.insert(events.end(), chunk_events->begin(), chunk_events->end());
      }
      auto final_events = parser.finish();
      expect(final_events.has_value(), "Gemini function-call stream accepts a final record without a trailing delimiter");
      if (!final_events)
        return;
      events.insert(events.end(), final_events->begin(), final_events->end());
      expect(events.size() == 10, "Gemini emits three complete function-call lifecycles and one terminal event");
      if (events.size() != 10)
        return;

      expect(!events[0].tool_call_id.empty() && !events[6].tool_call_id.empty() && events[0].tool_call_id != events[6].tool_call_id,
             "Gemini no-ID function calls in separate SSE records receive distinct response-wide IDs");
      for (std::size_t call = 0; call < 3; ++call)
      {
        auto const start = call * 3;
        expect(events[start].type == ava::provider::StreamEventType::ToolCallStart && events[start + 1].type == ava::provider::StreamEventType::ToolCallDelta &&
                   events[start + 2].type == ava::provider::StreamEventType::ToolCallEnd && events[start].tool_call_id == events[start + 1].tool_call_id &&
                   events[start].tool_call_id == events[start + 2].tool_call_id,
               "Gemini preserves a function-call ID throughout its complete lifecycle");
      }
      expect(events[3].tool_call_id == "provider-call_7", "Gemini preserves an explicit provider function-call ID verbatim");

      auto const turn = ava::agent::parse_assistant_turn(events, {});
      expect(turn && turn->tool_calls.size() == 3 && turn->tool_calls[0].id == events[0].tool_call_id && turn->tool_calls[1].id == "provider-call_7" &&
                 turn->tool_calls[2].id == events[6].tool_call_id && turn->tool_calls[0].name == "read_file" && turn->tool_calls[2].name == "read_file" &&
                 turn->tool_calls[0].arguments_json == R"({"path":"README.md"})" && turn->tool_calls[1].arguments_json == R"({"path":"src/main.cpp"})" &&
                 turn->tool_calls[2].arguments_json == R"({"path":"AGENTS.md"})",
             "Gemini calls from separate SSE records pass through the real assistant-turn consumer without ID rejection");
    }
  }
}

void test_gemini_non_streaming_function_call_ids()
{
  ava::provider::GeminiProvider const provider("https://gemini.example.test");
  auto const events = provider.parse_response(
      ava::http::HttpResponse{
          .status_code = 200,
          .headers = {},
          .body =
              R"({"candidates":[{"content":{"parts":[{"functionCall":{"name":"read_file","args":{"path":"README.md"}}},{"functionCall":{"id":"provider-call_7","name":"read_file","args":{"path":"src/main.cpp"}}},{"functionCall":{"name":"read_file","args":{"path":"AGENTS.md"}}}]},"finishReason":"STOP"}]})"},
      false);
  expect(events.has_value(), "Gemini non-streaming response accepts mixed explicit and missing function-call IDs");
  if (!events)
    return;
  auto const turn = ava::agent::parse_assistant_turn(*events, {});
  expect(turn && turn->tool_calls.size() == 3 && !turn->tool_calls[0].id.empty() && !turn->tool_calls[2].id.empty() &&
             turn->tool_calls[0].id != turn->tool_calls[2].id && turn->tool_calls[1].id == "provider-call_7",
         "Gemini non-streaming calls use distinct fallback IDs and preserve explicit IDs through the assistant-turn consumer");
}

void test_gemini_thought_text_is_reasoning()
{
  ava::provider::GeminiProvider const provider("https://gemini.example.test");
  std::string const body =
      R"({"candidates":[{"content":{"parts":[{"text":"Inspect the file first.","thought":true},{"text":"I will read it."}]},"finishReason":"STOP"}]})";
  for (bool const stream : {false, true})
  {
    auto const events = provider.parse_response({.status_code = 200, .headers = {}, .body = stream ? "data: " + body + "\n\n" : body}, stream);
    expect(events.has_value(), "Gemini accepts thought text in buffered and streamed responses");
    if (!events)
      return;
    auto const turn = ava::agent::parse_assistant_turn(*events, {});
    expect(turn && turn->text == "I will read it." && turn->reasoning_blocks.size() == 1 && turn->reasoning_blocks[0].text == "Inspect the file first." &&
               turn->reasoning_blocks[0].format == "gemini",
           "Gemini thought text reaches only the reasoning channel, never the answer text");
  }
}

void test_gemini_thought_stream_closes_once()
{
  for (bool const signed_thought : {false, true})
    for (bool const answer : {false, true})
    {
      ava::provider::GeminiStreamParser parser;
      std::vector<ava::provider::StreamEvent> events;
      for (char const letter : std::string_view("abc"))
      {
        auto const signature = signed_thought && letter == 'c' ? ",\"thoughtSignature\":\"PRIVATE_THOUGHT_SIGNATURE\"" : "";
        auto chunk = parser.append("data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"" + std::string(1, letter) + "\",\"thought\":true" + signature +
                                   "}]}}]}\n\n");
        expect(chunk && chunk->size() == 1 && chunk->front().type == ava::provider::StreamEventType::ReasoningDelta,
               "Gemini contiguous thought chunks emit deltas without per-chunk ReasoningEnd events");
        if (!chunk)
          return;
        events.insert(events.end(), chunk->begin(), chunk->end());
      }
      auto ending = parser.append(answer ? "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"answer\"}]},\"finishReason\":\"STOP\"}]}\n\n"
                                         : "data: {\"candidates\":[{\"finishReason\":\"STOP\"}]}\n\n");
      expect(ending.has_value(), "Gemini thought stream accepts its terminal candidate");
      if (!ending)
        return;
      events.insert(events.end(), ending->begin(), ending->end());
      auto final = parser.finish();
      expect(final.has_value(), "Gemini closes any remaining thought stream on completion");
      if (!final)
        return;
      events.insert(events.end(), final->begin(), final->end());
      auto const expected_ends = signed_thought && answer ? 2 : 1;
      expect(std::ranges::count_if(events, [](auto const& event) { return event.type == ava::provider::StreamEventType::ReasoningEnd; }) == expected_ends,
             "Gemini closes visible thinking once and uses only the necessary trailing private-envelope event");
      auto const turn = ava::agent::parse_assistant_turn(events, {});
      expect(turn && turn->text == (answer ? "answer" : "") && turn->reasoning_blocks.size() == static_cast<std::size_t>(expected_ends) &&
                 turn->reasoning_blocks.front().text == "abc" &&
                 std::ranges::any_of(turn->reasoning_blocks, [](auto const& block) { return !block.native_item_json.empty(); }) == signed_thought,
             "Gemini coalesces thought chunks while retaining the complete private signature envelope");
    }
}

ava::core::Result<std::vector<ava::provider::ChatMessage>> persist_gemini_turn_and_rebuild_history(ava::agent::ParsedAssistantTurn const& turn)
{
  std::vector<ava::session::SessionEntry> entries;
  auto append = [&](ava::session::SessionEntry entry) -> ava::core::VoidResult {
    entry.parent_id = entries.empty() ? "" : entries.back().id;
    entries.push_back(std::move(entry));
    return {};
  };
  auto persisted = ava::agent::append_assistant_turn(
      [&](std::vector<ava::session::SessionEntry> batch) -> ava::core::VoidResult {
        for (auto& entry : batch)
          if (auto result = append(std::move(entry)); !result)
            return result;
        return {};
      },
      turn, "gemini", "gemini-2.5-pro", {}, std::nullopt, "gemini_generate_content", "gemini");
  if (!persisted)
    return std::unexpected(std::move(persisted.error()));
  for (auto const& call : turn.tool_calls)
  {
    auto result = ava::agent::append_tool_result(append, {.call_id = call.id, .name = call.name, .success = true, .result_text = "contents"},
                                                 persisted->function_output_entry_ids_by_call_id.at(call.id));
    if (!result)
      return std::unexpected(std::move(result.error()));
  }
  return ava::agent::build_provider_messages_from_entries(entries, {.target = ava::agent::HistoryReplayTarget{.provider_id = "gemini",
                                                                                                              .model_id = "gemini-2.5-pro",
                                                                                                              .api_family = "gemini_generate_content",
                                                                                                              .reasoning_format = "gemini",
                                                                                                              .supports_tools = true}});
}

void test_gemini_signed_parts_survive_history_replay()
{
  ava::provider::GeminiProvider const provider("https://gemini.example.test");
  std::string const first =
      R"({"candidates":[{"content":{"parts":[{"text":"Inspect.","thought":true,"thoughtSignature":"PRIVATE_THOUGHT_SIGNATURE"},{"text":"Reading."},{"functionCall":{"id":"call_signed","name":"read_file","args":{"path":"README.md"}},"thoughtSignature":"PRIVATE_CALL_SIGNATURE"}]}}]})";
  std::string const last =
      R"({"candidates":[{"content":{"parts":[{"functionCall":{"name":"read_file","args":{"path":"AGENTS.md"}}},{"text":"","thoughtSignature":"PRIVATE_LATE_SIGNATURE"}]},"finishReason":"STOP"}]})";
  std::string const buffered =
      R"({"candidates":[{"content":{"parts":[{"text":"Inspect.","thought":true,"thoughtSignature":"PRIVATE_THOUGHT_SIGNATURE"},{"text":"Reading."},{"functionCall":{"id":"call_signed","name":"read_file","args":{"path":"README.md"}},"thoughtSignature":"PRIVATE_CALL_SIGNATURE"},{"functionCall":{"name":"read_file","args":{"path":"AGENTS.md"}}},{"text":"","thoughtSignature":"PRIVATE_LATE_SIGNATURE"}]},"finishReason":"STOP"}]})";
  for (std::size_t const chunk_size : {std::size_t{0}, std::size_t{1}, std::size_t{4096}})
  {
    std::vector<ava::provider::StreamEvent> events;
    if (chunk_size == 0)
    {
      auto parsed = provider.parse_response({.status_code = 200, .headers = {}, .body = buffered}, false);
      expect(parsed.has_value(), "Gemini buffered signed parts parse");
      if (!parsed)
        return;
      events = std::move(*parsed);
    }
    else
    {
      ava::provider::GeminiStreamParser parser;
      auto const sse = "data: " + first + "\n\n" + "data: " + last + (chunk_size == 1 ? "\n\ndata: [DONE]\n\n" : "");
      for (std::size_t offset = 0; offset < sse.size(); offset += chunk_size)
      {
        auto parsed = parser.append(std::string_view(sse).substr(offset, chunk_size));
        expect(parsed.has_value(), "Gemini signed stream accepts fragmented records");
        if (!parsed)
          return;
        events.insert(events.end(), parsed->begin(), parsed->end());
      }
      auto final = parser.finish();
      expect(final.has_value(), "Gemini signed stream finishes");
      if (!final)
        return;
      events.insert(events.end(), final->begin(), final->end());
    }

    auto const turn = ava::agent::parse_assistant_turn(events, {});
    expect(turn && turn->text == "Reading." && turn->tool_calls.size() == 2 &&
               std::ranges::any_of(turn->reasoning_blocks,
                                   [](auto const& block) { return block.native_item_json.find("PRIVATE_LATE_SIGNATURE") != std::string::npos; }),
           "Gemini retains signed native parts, including a later empty streaming part, privately in the assistant turn");
    if (!turn || turn->tool_calls.size() != 2)
      return;

    ava::agent::AgentLoopOptions options;
    std::size_t public_events = 0;
    options.on_stream_event = [&](ava::provider::StreamEvent const& event) -> ava::core::VoidResult {
      ++public_events;
      expect(event.reasoning_signature.empty() && event.reasoning_native_item_json.empty() && event.reasoning_redacted_data.empty() &&
                 event.text.find("PRIVATE_") == std::string::npos && event.error_message.find("PRIVATE_") == std::string::npos,
             "Gemini signatures never appear in public stream events");
      return {};
    };
    for (auto const& event : events)
      expect(ava::agent::publish_stream_event(options, event).has_value(), "Gemini private events cross the existing public sanitizer");
    expect(public_events > 0, "Gemini public-event privacy assertions exercise the real bridge");

    auto messages = persist_gemini_turn_and_rebuild_history(*turn);
    expect(messages && messages->size() == 2, "Gemini exact-provenance history reconstructs the signed assistant turn and tool results");
    if (!messages)
      return;
    ava::provider::ProviderRequest request{
        .provider_id = "gemini", .model_id = "gemini-2.5-pro", .system_prompt = "", .messages = std::move(*messages), .tools_json = {}, .stream = false};
    auto const followup = provider.build_request(request, "gemini-key");
    expect(followup && followup->body.find(R"({"text":"Inspect.","thought":true,"thoughtSignature":"PRIVATE_THOUGHT_SIGNATURE"})") != std::string::npos &&
               followup->body.find(
                   R"({"functionCall":{"id":"call_signed","name":"read_file","args":{"path":"README.md"}},"thoughtSignature":"PRIVATE_CALL_SIGNATURE"})") !=
                   std::string::npos &&
               followup->body.find(R"({"text":"","thoughtSignature":"PRIVATE_LATE_SIGNATURE"})") != std::string::npos &&
               ava::tests::count_occurrences(followup->body, "\"functionCall\"") == 2 &&
               ava::tests::count_occurrences(followup->body, "\"functionResponse\"") == 2 &&
               ava::tests::count_occurrences(followup->body, "\"thoughtSignature\"") == 3,
           "Gemini next request replays each original signature only on its matching native part without duplicate calls");

    for (auto& part : request.messages.front().content_parts)
      if (part.type == ava::provider::ContentPartType::ToolUse && part.tool_call_id == "call_signed")
        part.tool_name = "different_tool";
    auto const mismatched = provider.build_request(request, "gemini-key");
    expect(!mismatched && mismatched.error().format().find("PRIVATE_") == std::string::npos,
           "Gemini fails closed without leaking signatures when normalized history no longer matches signed native parts");
  }
}

void test_gemini_native_replay_metadata_limits()
{
  ava::provider::GeminiProvider const provider("https://gemini.example.test");
  auto check_replay = [&](auto const& events, std::string const& expected_text, std::string const& expected_args, bool retained) {
    expect(events && !events->empty() && events->back().type == ava::provider::StreamEventType::Done &&
               events->back().finish_reason == ava::provider::ProviderFinishReason::Completed &&
               std::ranges::none_of(*events, [](auto const& event) { return event.type == ava::provider::StreamEventType::Error; }),
           "Gemini replay metadata limits never discard ordinary content or replace successful completion with an error");
    if (!events)
      return;
    auto const turn = ava::agent::parse_assistant_turn(*events, {});
    expect(turn && turn->text == expected_text &&
               (expected_args.empty() ? turn->tool_calls.empty() : turn->tool_calls.size() == 1 && turn->tool_calls.front().arguments_json == expected_args),
           "Gemini keeps complete text and tool arguments when native replay metadata reaches its independent bound");
    if (!turn)
      return;
    bool const has_envelope = std::ranges::any_of(turn->reasoning_blocks, [](auto const& block) { return !block.native_item_json.empty(); });
    expect(has_envelope == retained, "Gemini emits only complete in-budget private replay envelopes");
    if (retained && !expected_args.empty())
      expect(std::ranges::all_of(
                 turn->reasoning_blocks,
                 [&](auto const& block) { return block.native_item_json.size() < 1024 && block.native_item_json.find(expected_args) == std::string::npos; }),
             "Gemini compact call metadata retains identity and signature without duplicating large canonical arguments");
    auto messages = persist_gemini_turn_and_rebuild_history(*turn);
    expect(messages.has_value(), "Gemini history reconstruction accepts metadata-free fallback and compact signed calls");
    if (!messages)
      return;
    auto const request = provider.build_request(
        {.provider_id = "gemini", .model_id = "gemini-2.5-pro", .system_prompt = "", .messages = std::move(*messages), .tools_json = {}, .stream = false},
        "gemini-key");
    expect(request && (request->body.find("\"thoughtSignature\"") != std::string::npos) == retained &&
               (expected_text.empty() || request->body.find(expected_text) != std::string::npos) &&
               (expected_args.empty() || request->body.find(expected_args) != std::string::npos),
           "Gemini next request still builds from the same history without truncated signatures or lost arguments");
  };
  auto const oversized = provider.parse_response({.status_code = 200,
                                                  .headers = {},
                                                  .body = R"({"candidates":[{"content":{"parts":[{"text":"answer","thoughtSignature":")" +
                                                          std::string(64U * 1024U, 'x') + R"("}]},"finishReason":"STOP"}]})"},
                                                 false);
  check_replay(oversized, "answer", "", false);
  std::string const large_text(70U * 1024U, 'x');
  std::string const large_answer = "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"" + large_text +
                                   "\",\"thoughtSignature\":\"PRIVATE_LIMIT_SIGNATURE\"}]},\"finishReason\":\"STOP\"}]}";
  for (bool const stream : {false, true})
    check_replay(provider.parse_response({.status_code = 200, .headers = {}, .body = stream ? "data: " + large_answer + "\n\n" : large_answer}, stream),
                 large_text, "", false);

  for (bool const signature_first : {false, true})
  {
    std::string sse;
    if (signature_first)
      sse += "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"\",\"thoughtSignature\":\"PRIVATE_LIMIT_SIGNATURE\"}]}}]}\n\n";
    for (int index = 0; index < 17; ++index)
      sse += "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"" + std::string(4096, 'x') + "\"}]}}]}\n\n";
    if (!signature_first)
      sse += "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"\",\"thoughtSignature\":\"PRIVATE_LIMIT_SIGNATURE\"}]}}]}\n\n";
    sse += "data: {\"candidates\":[{\"finishReason\":\"STOP\"}]}\n\ndata: [DONE]\n\n";
    auto const events = ava::provider::parse_gemini_sse(sse);
    check_replay(events, std::string(17U * 4096U, 'x'), "", false);
  }
  std::string many_parts = "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"\",\"thoughtSignature\":\"PRIVATE_LIMIT_SIGNATURE\"}]}}]}\n\n";
  for (std::size_t index = 0; index < 1025; ++index)
    many_parts += "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"x\"}]}}]}\n\n";
  many_parts += "data: {\"candidates\":[{\"finishReason\":\"STOP\"}]}\n\ndata: [DONE]\n\n";
  check_replay(ava::provider::parse_gemini_sse(many_parts), std::string(1025, 'x'), "", false);

  std::string const large_args = "{\"content\":\"" + std::string(70U * 1024U, 'x') + "\"}";
  std::string const large_call =
      "{\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{\"id\":\"large_call\",\"name\":\"write_file\",\"args\":" + large_args +
      "},\"thoughtSignature\":\"PRIVATE_LARGE_CALL_SIGNATURE\"}]},\"finishReason\":\"STOP\"}]}";
  for (bool const stream : {false, true})
    check_replay(provider.parse_response({.status_code = 200, .headers = {}, .body = stream ? "data: " + large_call + "\n\n" : large_call}, stream), "",
                 large_args, true);

  auto const oversized_call = provider.parse_response(
      {.status_code = 200,
       .headers = {},
       .body = "{\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{\"id\":\"large_call\",\"name\":\"write_file\",\"args\":" + large_args +
               "},\"thoughtSignature\":\"" + std::string(64U * 1024U, 'x') + "\"}]},\"finishReason\":\"STOP\"}]}"},
      false);
  check_replay(oversized_call, "", large_args, false);

  ava::provider::ProviderRequest request{.provider_id = "gemini",
                                         .model_id = "gemini-2.5-pro",
                                         .system_prompt = "",
                                         .messages = {{.role = "assistant",
                                                       .content = "",
                                                       .content_parts = {{.type = ava::provider::ContentPartType::Reasoning,
                                                                          .reasoning_format = "gemini",
                                                                          .reasoning_native_item_json = "{\"type\":\"reasoning\",\"parts\":[],\"padding\":\"" +
                                                                                                        std::string(64U * 1024U, 'x') + "\"}"}}}},
                                         .tools_json = {}};
  auto const rejected = provider.build_request(request, "gemini-key");
  expect(!rejected && rejected.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Gemini validates the bounded private replay envelope again at the outbound request boundary");
}

void test_gemini_default_model_signature_history_roundtrip()
{
  auto const registry = ava::config::builtin_model_registry();
  auto const model = ava::config::find_model(registry, "gemini", "gemini-2.5-pro");
  if (!ava::tests::require(model.has_value(), "Gemini default-model replay fixture uses the real builtin model"))
    return;
  agent_loop_test::AgentLoopFixture fixture("gemini-default");
  auto const& workspace = fixture.workspace;
  auto& store = fixture.store;
  ava::tests::write_test_file(workspace / "note.txt", "signed tool content");
  auto options_for = [&] {
    auto options = fixture.options({.provider_id = model->provider_id,
                                    .model_id = model->model_id,
                                    .system_prompt = "system",
                                    .stream = true,
                                    .supports_tools = model->supports_tools.value_or(false),
                                    .supports_streaming = model->supports_streaming.value_or(false),
                                    .input_modalities = model->input_modalities,
                                    .max_output_tokens = std::nullopt,
                                    .reasoning = std::nullopt,
                                    .pricing = std::nullopt,
                                    .api_family = model->api_family,
                                    .reasoning_format = model->reasoning_format,
                                    .compatibility_quirks = model->compatibility_quirks});
    options.access_token = "gemini-key";
    return options;
  };
  ava::provider::GeminiProvider const provider("https://gemini.example.test");
  ava::tests::FakeTransport transport({{.status_code = 200,
                                        .headers = {},
                                        .body = "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Inspect the note.\",\"thought\":true},"
                                                "{\"functionCall\":{\"id\":\"call_default\",\"name\":\"read_file\",\"args\":{\"path\":\"note.txt\"}},"
                                                "\"thoughtSignature\":\"PRIVATE_DEFAULT_SIGNATURE\"}]},\"finishReason\":\"STOP\"}]}\n\n"},
                                       {.status_code = 200,
                                        .headers = {},
                                        .body = "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Read it.\"}]},\"finishReason\":\"STOP\"}]}\n\n"}});
  {
    // Release the first runtime's exclusive session lease before reopening it
    // with a fresh loop and fresh append/read authorities.
    ava::agent::AgentLoop loop(options_for());
    auto const result = loop.run_turn("Read note.txt", store, provider, transport);
    if (!ava::tests::require(result.has_value(), result ? "Gemini signed tool continuation runs" : result.error().format()))
      return;
    ava::tests::expect_all("Gemini builtin model completes a signed tool continuation using runtime provenance defaults",
                           {{"final text", result->final_text == "Read it."},
                            {"one tool call", result->tool_calls == 1},
                            {"two provider iterations", result->provider_iterations == 2}});
    ava::tests::expect_eq(transport.requests().size(), std::size_t{2}, "Gemini builtin model makes both initial and history-backed continuation requests");
    if (transport.requests().size() != 2)
      return;
    auto const& continuation = transport.requests()[1].body;
    expect(continuation.find(
               R"({"functionCall":{"id":"call_default","name":"read_file","args":{"path":"note.txt"}},"thoughtSignature":"PRIVATE_DEFAULT_SIGNATURE"})") !=
                   std::string::npos &&
               continuation.find("\"functionResponse\"") != std::string::npos && continuation.find("signed tool content") != std::string::npos,
           "Gemini builtin-model continuation replays the matching signature after real assistant persistence, tool dispatch, and history reconstruction");
  }

  auto restarted = ava::session::SessionStore::open(workspace, store.session_id(), fixture.root / "sessions");
  expect(restarted.has_value(), "Gemini signed default-model history reopens from disk");
  if (!restarted)
    return;
  ava::tests::FakeTransport resumed_transport(
      {{.status_code = 200,
        .headers = {},
        .body = "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Remembered.\"}]},\"finishReason\":\"STOP\"}]}\n\n"}});
  ava::agent::AgentLoop resumed_loop(options_for());
  auto const resumed = resumed_loop.run_turn("What did you read?", *restarted, provider, resumed_transport);
  expect(resumed && resumed->final_text == "Remembered." && resumed_transport.requests().size() == 1 &&
             resumed_transport.requests().front().body.find("\"thoughtSignature\":\"PRIVATE_DEFAULT_SIGNATURE\"") != std::string::npos &&
             resumed_transport.requests().front().body.find("signed tool content") != std::string::npos,
         "Gemini builtin-model signatures survive a disk history round trip and remain available to a fresh agent loop");
}

void test_gemini_stream_parser_reuse()
{
  for (bool const explicit_done : {false, true})
  {
    ava::provider::GeminiStreamParser parser;
    std::string const signed_response =
        "data: "
        R"({"candidates":[{"content":{"parts":[{"text":"Think.","thought":true,"thoughtSignature":"PRIVATE_REUSE_SIGNATURE"},{"functionCall":{"name":"read_file","args":{}}}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":7}})"
        "\n\n";
    auto first = parser.append(signed_response + (explicit_done ? "data: [DONE]\n\n" : ""));
    auto first_final = parser.finish();
    expect(first && first_final, "Gemini reusable parser finishes signed responses with or without an explicit terminal record");
    if (!first || !first_final)
    {
      return;
    }
    first->insert(first->end(), first_final->begin(), first_final->end());
    auto const first_turn = ava::agent::parse_assistant_turn(*first, {});
    expect(first_turn && first_turn->tool_calls.size() == 1 && first->back().type == ava::provider::StreamEventType::Done && first->back().usage &&
               first->back().usage->input_tokens == 7 && first->back().finish_reason == ava::provider::ProviderFinishReason::Completed &&
               std::ranges::any_of(first_turn->reasoning_blocks,
                                   [](auto const& block) { return block.native_item_json.find("PRIVATE_REUSE_SIGNATURE") != std::string::npos; }),
           "Gemini parser reuse fixture retains usage, completion, and the private signed envelope");
    if (!first_turn || first_turn->tool_calls.size() != 1)
    {
      return;
    }

    auto second = parser.append("data: {\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{\"name\":\"read_file\",\"args\":{}}}]}}]}\n\n");
    auto second_final = parser.finish();
    expect(second && second->size() == 3 && second_final && second_final->size() == 1 && second_final->front().type == ava::provider::StreamEventType::Done &&
               !second_final->front().usage && !second_final->front().finish_reason,
           "Gemini finish resets per-response usage, completion, and reasoning replay state before parser reuse");
    if (!second || !second_final)
    {
      return;
    }
    second->insert(second->end(), second_final->begin(), second_final->end());
    auto const unfinished_turn = ava::agent::parse_assistant_turn(*second, {});
    expect(!unfinished_turn && unfinished_turn.error().category() == ava::core::ErrorCategory::Provider,
           "Gemini reset does not invent a finish reason for an incomplete response accepted by the SSE parser");

    auto complete = parser.append(
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{\"name\":\"read_file\",\"args\":{}}}]},\"finishReason\":\"MAX_TOKENS\"}]}\n\n");
    auto complete_final = parser.finish();
    expect(complete && complete->size() == 3 && complete_final && complete_final->size() == 1 &&
               complete_final->front().type == ava::provider::StreamEventType::Done &&
               complete_final->front().finish_reason == ava::provider::ProviderFinishReason::MaxTokens && !complete_final->front().usage,
           "Gemini reused parser accepts a complete unsigned response with its own terminal reason and no stale usage");
    if (!complete || !complete_final || second->empty())
    {
      return;
    }
    complete->insert(complete->end(), complete_final->begin(), complete_final->end());
    auto const second_turn = ava::agent::parse_assistant_turn(*complete, {});
    expect(second_turn && second_turn->tool_calls.size() == 1 && second_turn->reasoning_blocks.empty() &&
               second_turn->tool_calls.front().id != first_turn->tool_calls.front().id && second_turn->tool_calls.front().id != second->front().tool_call_id,
           "Gemini parser reuse retains its fallback-ID sequence without carrying signed parts into a later response");
  }
}

void test_gemini_stream_parser_error_reset()
{
  ava::provider::GeminiStreamParser parser;
  constexpr std::string_view response = "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"recovered\"}]}}]}\n\n";
  auto const malformed = parser.append("data: not-json\n\n");
  auto const ignored = parser.append(response);
  auto const failed_final = parser.finish();
  expect(malformed && malformed->size() == 1 && malformed->front().type == ava::provider::StreamEventType::Error && ignored && ignored->empty() &&
             failed_final && failed_final->empty(),
         "Gemini SSE errors remain latched for the whole response without a successful terminal event");

  auto const recovered = parser.append(response);
  auto const recovered_final = parser.finish();
  expect(recovered && recovered->size() == 1 && recovered->front().type == ava::provider::StreamEventType::TextDelta &&
             recovered->front().text == "recovered" && recovered_final && recovered_final->size() == 1 &&
             recovered_final->front().type == ava::provider::StreamEventType::Done,
         "Gemini finish clears the shared parse-state error before accepting a later response");
}

}  // namespace

void run_provider_gemini_tests()
{
  ava::tests::HermeticTestEnvironment environment("gemini-suite-");
  test_gemini_profiles_and_registry();
  test_gemini_provider_contract();
  test_gemini_requests_disable_redirects();
  test_gemini_response_parsing();
  test_gemini_sse_parsing_and_fake_transport();
  test_gemini_function_call_ids_across_stream_records();
  test_gemini_non_streaming_function_call_ids();
  test_gemini_thought_text_is_reasoning();
  test_gemini_thought_stream_closes_once();
  test_gemini_signed_parts_survive_history_replay();
  test_gemini_native_replay_metadata_limits();
  test_gemini_default_model_signature_history_roundtrip();
  test_gemini_stream_parser_reuse();
  test_gemini_stream_parser_error_reset();
}
