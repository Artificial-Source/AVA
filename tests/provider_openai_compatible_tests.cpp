#include "sys.h"
#include "tests/provider_openai_test_suite.h"
#include "tests/support/provider_parser_budget_tests.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/provider/openai_compatible_provider.h"
#include "ava/provider/provider.h"
#include "ava/provider/registry.h"
#include "ava/core/error.h"

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tests::provider_openai_suite {

void test_openai_compatible_provider_contract()
{
  constexpr std::string_view system_prompt_marker = "AVA-ISSUE-54-COMPAT-SYSTEM-PROMPT";
  ava::provider::OpenAICompatibleProvider const provider(ava::provider::OpenAICompatibleProviderOptions{.base_url = "https://compat.example.test/api",
                                                                                                        .chat_completions_path = "/v1/chat/completions",
                                                                                                        .provider_name = "Compat",
                                                                                                        .reasoning_format = "reasoning_content",
                                                                                                        .user_agent = "CompatAgent/1.0",
                                                                                                        .default_temperature = 1.0,
                                                                                                        .preserve_reasoning_content = true,
                                                                                                        .include_stream_usage = true});
  auto const request = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "kimi",
          .model_id = "kimi-k2-thinking",
          .system_prompt = std::string(system_prompt_marker),
          .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"},
                       ava::provider::ChatMessage{
                           .role = "assistant",
                           .content = "fallback answer",
                           .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Reasoning,
                                                                        .text = "prior reasoning",
                                                                        .reasoning_format = "reasoning_content"},
                                             ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text, .text = "answer text"},
                                             ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                        .text = "",
                                                                        .tool_call_id = "call_1",
                                                                        .tool_name = "read_file",
                                                                        .input_json = "{\"path\":\"README.md\"}"}}},
                       ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback tool output",
                                                  .content_parts = {ava::provider::ContentPart{
                                                      .type = ava::provider::ContentPartType::ToolResult, .text = "tool output", .tool_call_id = "call_1"}}}},
          .tools_json = {"{\"type\":\"function\",\"name\":\"read_file\",\"description\":\"Read\","
                         "\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
                         "\"required\":[\"path\"]}}"},
          .stream = true,
          .max_output_tokens = 16000,
          .reasoning = ava::provider::ProviderReasoningOptions{.type = "enabled", .budget_tokens = 4096, .display = "summarized"}},
      "compat-token");
  expect(request.has_value(), "OpenAI-compatible request builds");
  if (request)
  {
    expect(request->url == "https://compat.example.test/api/v1/chat/completions", "OpenAI-compatible request targets chat completions endpoint");
    expect(request->headers.at("Authorization") == "Bearer compat-token" && request->headers.at("User-Agent") == "CompatAgent/1.0",
           "OpenAI-compatible request includes bearer auth and provider user agent");
    expect(request->body.find("\"model\":\"kimi-k2-thinking\"") != std::string::npos && request->body.find("\"temperature\":1") != std::string::npos &&
               request->body.find("\"stream_options\":{\"include_usage\":true}") != std::string::npos &&
               request->body.find("\"thinking\":{\"type\":\"enabled\",\"budget_tokens\":4096") != std::string::npos &&
               request->body.find("\"display\":\"summarized\"") != std::string::npos && request->body.find("\"keep\":\"all\"") != std::string::npos,
           "OpenAI-compatible request includes model, fixed temperature, stream usage, and Kimi thinking option");
    expect(ava::tests::count_occurrences(request->body, system_prompt_marker) == 1,
           "OpenAI-compatible request serializes the effective system prompt exactly once");
    expect(request->body.find("\"messages\":[{\"role\":\"system\",\"content\":\"" + std::string(system_prompt_marker) + "\"") != std::string::npos,
           "OpenAI-compatible request leads messages with the system-role system prompt");
    expect(request->body.find("\"reasoning_content\":\"prior reasoning\"") != std::string::npos,
           "OpenAI-compatible request preserves visible reasoning_content for compatible replay");
    expect(request->body.find("\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\"") != std::string::npos &&
               request->body.find("\"role\":\"tool\",\"tool_call_id\":\"call_1\",\"content\":\"tool output\"") != std::string::npos,
           "OpenAI-compatible request serializes native tool_use/tool_result history");
    expect(request->body.find("fallback tool output") == std::string::npos,
           "OpenAI-compatible request does not insert fallback user text before native tool results");
    expect(request->body.find("\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"read_file\"") != std::string::npos,
           "OpenAI-compatible request converts Responses-style tool schemas to chat-completions tools");
    expect(request->body.find("\"max_tokens\":16000") != std::string::npos && request->body.find("\"max_completion_tokens\"") == std::string::npos,
           "OpenAI-compatible request defaults positive max_output_tokens to max_tokens without the Z.AI quirk");
  }

  auto const image_request = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "moonshot",
          .model_id = "kimi-k2.6",
          .system_prompt = "",
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
      "compat-token");
  expect(image_request && image_request->body.find(R"({"type":"text","text":"describe this"})") != std::string::npos &&
             image_request->body.find(R"({"type":"image_url","image_url":{"url":"data:image/png;base64,aGk="}})") != std::string::npos,
         "OpenAI-compatible request serializes verified image content parts");

  auto const missing_image_bytes = provider.build_request(
      ava::provider::ProviderRequest{.provider_id = "moonshot",
                                     .model_id = "kimi-k2.6",
                                     .system_prompt = "",
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
      "compat-token");
  expect(!missing_image_bytes && missing_image_bytes.error().message().find("verified attachment bytes") != std::string::npos,
         "OpenAI-compatible request rejects image content without verified attachment bytes");

  auto const invalid_image_bytes = provider.build_request(
      ava::provider::ProviderRequest{.provider_id = "moonshot",
                                     .model_id = "kimi-k2.6",
                                     .system_prompt = "",
                                     .messages = {ava::provider::ChatMessage{.role = "user",
                                                                             .content = "fallback image metadata",
                                                                             .content_parts = {ava::provider::ContentPart{
                                                                                 .type = ava::provider::ContentPartType::Image,
                                                                                 .attachment_id = "img_1",
                                                                                 .mime_type = "image/png",
                                                                                 .storage_path = "attachments/img_1.png",
                                                                                 .sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                                                                 .byte_size = 3,
                                                                                 .data_base64 = "not base64"}}}},
                                     .tools_json = {},
                                     .stream = false},
      "compat-token");
  expect(!invalid_image_bytes && invalid_image_bytes.error().message().find("verified attachment bytes") != std::string::npos,
         "OpenAI-compatible request rejects invalid image base64 payloads");

  auto const image_error_context = [](ava::core::Error const& error, std::string_view key) {
    for (auto const& item : error.context())
    {
      if (item.key == key)
        return item.value;
    }
    return std::string{};
  };
  auto const indexed_image_request = [&provider](std::string role, std::string mime_type, std::string data_base64) {
    return provider.build_request(
        ava::provider::ProviderRequest{
            .provider_id = "moonshot",
            .model_id = "kimi-k2.6",
            .system_prompt = "",
            .messages = {ava::provider::ChatMessage{.role = "user", .content = "first message"},
                         ava::provider::ChatMessage{
                             .role = std::move(role),
                             .content = "fallback image metadata",
                             .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text, .text = "describe this"},
                                               ava::provider::ContentPart{.type = ava::provider::ContentPartType::Image,
                                                                          .attachment_id = "img_indexed",
                                                                          .mime_type = std::move(mime_type),
                                                                          .storage_path = "attachments/img_indexed.png",
                                                                          .sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                                                          .byte_size = 3,
                                                                          .data_base64 = std::move(data_base64)}}}},
            .tools_json = {},
            .stream = false},
        "compat-token");
  };
  auto const non_user_image = indexed_image_request("assistant", "image/tiff", "not base64");
  expect(!non_user_image && non_user_image.error().category() == ava::core::ErrorCategory::InvalidArgument &&
             non_user_image.error().message() == "Compat image content requires user role" &&
             image_error_context(non_user_image.error(), "message_index") == "1" && image_error_context(non_user_image.error(), "content_part_index") == "1",
         "OpenAI-compatible image validation rejects non-user roles first with the provider prefix and nonzero part indexes");
  auto const unsupported_mime_image = indexed_image_request("user", "image/tiff", "not base64");
  expect(!unsupported_mime_image && unsupported_mime_image.error().category() == ava::core::ErrorCategory::InvalidArgument &&
             unsupported_mime_image.error().message() == "Compat image MIME type is not supported" &&
             image_error_context(unsupported_mime_image.error(), "message_index") == "1" &&
             image_error_context(unsupported_mime_image.error(), "content_part_index") == "1",
         "OpenAI-compatible image validation rejects unsupported MIME types before base64 checks with nonzero part indexes");
  auto const empty_bytes_image = indexed_image_request("user", "image/png", "");
  expect(!empty_bytes_image && empty_bytes_image.error().category() == ava::core::ErrorCategory::InvalidArgument &&
             empty_bytes_image.error().message() == "Compat image content requires verified attachment bytes" &&
             image_error_context(empty_bytes_image.error(), "message_index") == "1" &&
             image_error_context(empty_bytes_image.error(), "content_part_index") == "1",
         "OpenAI-compatible image validation rejects empty base64 payloads with the provider prefix and nonzero part indexes");
  auto const invalid_indexed_image_bytes = indexed_image_request("user", "image/png", "not base64");
  expect(!invalid_indexed_image_bytes && invalid_indexed_image_bytes.error().category() == ava::core::ErrorCategory::InvalidArgument &&
             invalid_indexed_image_bytes.error().message() == "Compat image content requires verified attachment bytes" &&
             image_error_context(invalid_indexed_image_bytes.error(), "message_index") == "1" &&
             image_error_context(invalid_indexed_image_bytes.error(), "content_part_index") == "1",
         "OpenAI-compatible image validation rejects invalid base64 payloads with the provider prefix and nonzero part indexes");

  auto const invalid_tool = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "moonshot", .model_id = "kimi-k2.6", .system_prompt = "", .messages = {}, .tools_json = {"{\"type\":\"function\"}"}},
      "compat-token");
  expect(!invalid_tool && invalid_tool.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "OpenAI-compatible request rejects function tools without names");

  auto const invalid_wrapped_tool = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "moonshot", .model_id = "kimi-k2.6", .system_prompt = "", .messages = {}, .tools_json = {"{\"type\":\"function\",\"function\":{}}"}},
      "compat-token");
  expect(!invalid_wrapped_tool && invalid_wrapped_tool.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "OpenAI-compatible request rejects wrapped tools without function names");

  auto const strict_tool = provider.build_request(ava::provider::ProviderRequest{.provider_id = "moonshot",
                                                                                 .model_id = "kimi-k2.6",
                                                                                 .system_prompt = "",
                                                                                 .messages = {},
                                                                                 .tools_json = {"{\"type\":\"function\",\"name\":\"strict_tool\","
                                                                                                "\"description\":\"uses strict mode\",\"strict\":true,"
                                                                                                "\"parameters\":{\"type\":\"object\"}}"}},
                                                  "compat-token");
  expect(strict_tool && strict_tool->body.find("\"strict\":true") != std::string::npos, "OpenAI-compatible request preserves strict tool schemas");

  auto const invalid_parameters = provider.build_request(ava::provider::ProviderRequest{.provider_id = "moonshot",
                                                                                        .model_id = "kimi-k2.6",
                                                                                        .system_prompt = "",
                                                                                        .messages = {},
                                                                                        .tools_json = {"{\"type\":\"function\",\"name\":\"bad_params\","
                                                                                                       "\"parameters\":{\"type\":\"object\""}},
                                                         "compat-token");
  expect(!invalid_parameters && invalid_parameters.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "OpenAI-compatible request rejects malformed tool parameter JSON");

  ava::provider::OpenAICompatibleProvider const kimi_default_reasoning_provider(ava::provider::OpenAICompatibleProviderOptions{
      .base_url = "https://compat.example.test", .provider_name = "Kimi", .reasoning_format = "reasoning_content", .preserve_reasoning_content = true});
  auto const default_reasoning_request = kimi_default_reasoning_provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "kimi",
          .model_id = "kimi-k2-thinking",
          .system_prompt = "",
          .messages = {ava::provider::ChatMessage{
              .role = "assistant",
              .content = "answer",
              .content_parts = {ava::provider::ContentPart{
                  .type = ava::provider::ContentPartType::Reasoning, .text = "preserved thinking", .reasoning_format = "reasoning_content"}}}},
          .tools_json = {}},
      "compat-token");
  expect(default_reasoning_request && default_reasoning_request->body.find("\"reasoning_content\":\"preserved thinking\"") != std::string::npos &&
             default_reasoning_request->body.find("\"thinking\"") == std::string::npos,
         "OpenAI-compatible preserved reasoning replay does not request reasoning after clear/default state");

  ava::provider::OpenAICompatibleProvider const no_preserve_provider(ava::provider::OpenAICompatibleProviderOptions{
      .base_url = "https://compat.example.test", .provider_name = "Compat", .reasoning_format = "reasoning_content"});
  auto const no_preserve_request = no_preserve_provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "moonshot",
          .model_id = "kimi-k2.6",
          .system_prompt = "",
          .messages = {ava::provider::ChatMessage{
              .role = "assistant",
              .content = "answer",
              .content_parts = {ava::provider::ContentPart{
                  .type = ava::provider::ContentPartType::Reasoning, .text = "private compatible reasoning", .reasoning_format = "reasoning_content"}}}},
          .tools_json = {}},
      "compat-token");
  expect(no_preserve_request && no_preserve_request->body.find("private compatible reasoning") == std::string::npos,
         "OpenAI-compatible request only replays reasoning_content when explicitly enabled");
}

void test_openai_compatible_parsing()
{
  ava::provider::OpenAICompatibleProvider const budget_provider({});
  ava::tests::test_provider_parser_budgets(
      budget_provider, [](std::string const& text) { return R"({"choices":[{"message":{"content":")" + text + R"("}}]})"; },
      [](std::string const& text) { return R"(data: {"choices":[{"delta":{"content":")" + text + R"("}}]})"; },
      {R"({"choices":)" + ava::tests::provider_budget_array("{}") + "}",
       R"({"choices":[{"message":{"tool_calls":)" + ava::tests::provider_budget_array(R"({"function":{"name":"read_file","arguments":"{}"}})") +
           R"(},"delta":{"tool_calls":)" + ava::tests::provider_budget_array(R"({"function":{"name":"read_file","arguments":"{}"}})") + "}}]}"},
      R"(data: {"choices":[]})");
  ava::tests::test_provider_tool_fragment_budget(
      budget_provider, "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"budget_call\",\"function\":{\"name\":\"read_file\"}}]}}]}\n\n",
      [](std::string const& text) { return R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":")" + text + R"("}}]}}]})"; },
      "data: [DONE]\n\n");
  for (auto const index : {"-1", "2147483648", "4294967296", "9223372036854775807"})
  {
    auto const invalid_index =
        ava::provider::parse_openai_compatible_sse(std::string(R"(data: {"choices":[{"delta":{"tool_calls":[{"index":)") + index +
                                                   R"(,"id":"invalid","function":{"name":"read_file","arguments":"{}"}}]}}]})" + "\n\ndata: [DONE]\n\n");
    expect(invalid_index && invalid_index->size() == 1 && invalid_index->front().type == ava::provider::StreamEventType::Error,
           "OpenAI-compatible SSE rejects invalid tool indices before narrowing or publishing tool calls");
  }

  auto stream = ava::provider::parse_openai_compatible_sse(
      "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"plan\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
      "\"function\":{\"name\":\"read_file\",\"arguments\":\"{}\"}}]}}]}\n\n"
      "data: {\"choices\":[{\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":11,"
      "\"completion_tokens\":7,\"total_tokens\":18,\"prompt_tokens_details\":{\"cached_tokens\":3},"
      "\"completion_tokens_details\":{\"reasoning_tokens\":2}}}\n\n"
      "data: [DONE]\n\n");
  expect(stream.has_value(), "OpenAI-compatible SSE parses");
  if (stream)
  {
    expect(stream->size() == 8, "OpenAI-compatible SSE emits reasoning, text, tool, and done events");
    expect((*stream)[0].type == ava::provider::StreamEventType::ReasoningStart && (*stream)[0].reasoning_format == "reasoning_content",
           "OpenAI-compatible SSE starts reasoning_content block");
    expect((*stream)[1].type == ava::provider::StreamEventType::ReasoningDelta && (*stream)[1].text == "plan",
           "OpenAI-compatible SSE emits reasoning_content delta");
    expect((*stream)[2].type == ava::provider::StreamEventType::ReasoningEnd, "OpenAI-compatible SSE closes reasoning before answer text");
    expect((*stream)[3].type == ava::provider::StreamEventType::TextDelta && (*stream)[3].text == "answer", "OpenAI-compatible SSE emits answer text");
    expect((*stream)[4].type == ava::provider::StreamEventType::ToolCallStart && (*stream)[4].tool_call_id == "call_1" && (*stream)[4].tool_name == "read_file",
           "OpenAI-compatible SSE emits tool call start");
    expect((*stream)[7].type == ava::provider::StreamEventType::Done && (*stream)[7].usage && (*stream)[7].usage->input_tokens == 11 &&
               (*stream)[7].usage->output_tokens == 7 && (*stream)[7].usage->reasoning_tokens == 2 && (*stream)[7].usage->cache_read_tokens == 3 &&
               (*stream)[7].finish_reason == ava::provider::ProviderFinishReason::ToolCalls,
           "OpenAI-compatible SSE done carries usage and normalized tool stop reason");
  }

  ava::provider::OpenAICompatibleProvider const moonshot(
      ava::provider::OpenAICompatibleProviderOptions{.base_url = "https://moonshot.example.test", .provider_name = "Moonshot"});
  auto const non_stream = moonshot.parse_response(ava::http::HttpResponse{.status_code = 200,
                                                                          .headers = {},
                                                                          .body = "{\"choices\":[{\"message\":{\"reasoning_content\":\"think\","
                                                                                  "\"content\":\"done\"},\"finish_reason\":\"stop\"}],"
                                                                                  "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":6,"
                                                                                  "\"total_tokens\":11,\"cached_tokens\":1}}"},
                                                  false);
  expect(non_stream && non_stream->size() == 5 && (*non_stream)[0].type == ava::provider::StreamEventType::ReasoningStart &&
             (*non_stream)[1].type == ava::provider::StreamEventType::ReasoningDelta && (*non_stream)[1].text == "think" &&
             (*non_stream)[3].type == ava::provider::StreamEventType::TextDelta && (*non_stream)[3].text == "done" &&
             (*non_stream)[4].type == ava::provider::StreamEventType::Done && (*non_stream)[4].usage && (*non_stream)[4].usage->cache_read_tokens == 1 &&
             (*non_stream)[4].finish_reason == ava::provider::ProviderFinishReason::Completed,
         "OpenAI-compatible non-stream response parses reasoning_content, text, usage, and stop reason");

  auto const reasoning_only_length =
      moonshot.parse_response(ava::http::HttpResponse{.status_code = 200,
                                                      .headers = {},
                                                      .body = "{\"choices\":[{\"message\":{\"reasoning_content\":\"budget used\","
                                                              "\"content\":\"\"},\"finish_reason\":\"length\"}],"
                                                              "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":32,"
                                                              "\"completion_tokens_details\":{\"reasoning_tokens\":32}}}"},
                              false);
  expect(reasoning_only_length && reasoning_only_length->size() == 4 && (*reasoning_only_length)[0].type == ava::provider::StreamEventType::ReasoningStart &&
             (*reasoning_only_length)[1].type == ava::provider::StreamEventType::ReasoningDelta &&
             (*reasoning_only_length)[2].type == ava::provider::StreamEventType::ReasoningEnd &&
             (*reasoning_only_length)[3].type == ava::provider::StreamEventType::Done && (*reasoning_only_length)[3].usage &&
             (*reasoning_only_length)[3].usage->reasoning_tokens == 32 &&
             (*reasoning_only_length)[3].finish_reason == ava::provider::ProviderFinishReason::MaxTokens,
         "OpenAI-compatible non-stream parser preserves reasoning-only length responses as terminal provider turns");

  auto const filtered = moonshot.parse_response(ava::http::HttpResponse{.status_code = 200,
                                                                        .headers = {},
                                                                        .body = "{\"choices\":[{\"finish_reason\":\"content_filter\"}],"
                                                                                "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":0}}"},
                                                false);
  expect(filtered && filtered->size() == 1 && (*filtered)[0].type == ava::provider::StreamEventType::Done &&
             (*filtered)[0].finish_reason == ava::provider::ProviderFinishReason::Refusal,
         "OpenAI-compatible non-stream parser treats filtered empty responses as completed provider turns");
  auto const empty_stop = moonshot.parse_response(ava::http::HttpResponse{.status_code = 200,
                                                                          .headers = {},
                                                                          .body = "{\"choices\":[{\"message\":{\"content\":\"\"},"
                                                                                  "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":2,"
                                                                                  "\"completion_tokens\":0,\"total_tokens\":2}}"},
                                                  false);
  expect(empty_stop && empty_stop->size() == 1 && (*empty_stop)[0].type == ava::provider::StreamEventType::Done && (*empty_stop)[0].usage &&
             (*empty_stop)[0].usage->input_tokens == 2 && (*empty_stop)[0].usage->output_tokens == 0 &&
             (*empty_stop)[0].finish_reason == ava::provider::ProviderFinishReason::Completed,
         "OpenAI-compatible non-stream parser accepts empty completed output with usage");

  auto const unicode_text = std::string("rocket ") + "\xF0\x9F\x9A\x80" + " bad " + "\xEF\xBF\xBD";
  auto const unicode = moonshot.parse_response(ava::http::HttpResponse{.status_code = 200,
                                                                       .headers = {},
                                                                       .body = "{\"choices\":[{\"message\":{\"content\":"
                                                                               "\"rocket \\ud83d\\ude80 bad \\ud800\"},\"finish_reason\":\"stop\"}]}"},
                                               false);
  expect(unicode && unicode->size() == 2 && (*unicode)[0].type == ava::provider::StreamEventType::TextDelta && (*unicode)[0].text == unicode_text &&
             (*unicode)[1].type == ava::provider::StreamEventType::Done,
         "OpenAI-compatible non-stream parser preserves surrogate pairs and replaces dangling surrogates");

  auto const unknown_finish = moonshot.parse_response(ava::http::HttpResponse{.status_code = 200,
                                                                              .headers = {},
                                                                              .body = "{\"choices\":[{\"message\":{\"content\":\"done\"},"
                                                                                      "\"finish_reason\":\"provider_custom\"}]}"},
                                                      false);
  expect(unknown_finish && unknown_finish->size() == 2 && (*unknown_finish)[1].type == ava::provider::StreamEventType::Done &&
             (*unknown_finish)[1].finish_reason == ava::provider::ProviderFinishReason::Error,
         "OpenAI-compatible non-stream parser preserves unknown finish reasons");

  auto const non_stream_tool = moonshot.parse_response(ava::http::HttpResponse{.status_code = 200,
                                                                               .headers = {},
                                                                               .body = "{\"choices\":[{\"message\":{\"tool_calls\":[{"
                                                                                       "\"id\":\"call_9\",\"function\":{\"name\":\"read_file\","
                                                                                       "\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}"},
                                                       false);
  expect(non_stream_tool && non_stream_tool->size() == 4 && (*non_stream_tool)[0].type == ava::provider::StreamEventType::ToolCallStart &&
             (*non_stream_tool)[0].tool_call_id == "call_9" && (*non_stream_tool)[0].tool_name == "read_file" &&
             (*non_stream_tool)[1].type == ava::provider::StreamEventType::ToolCallDelta &&
             (*non_stream_tool)[2].type == ava::provider::StreamEventType::ToolCallEnd && (*non_stream_tool)[3].type == ava::provider::StreamEventType::Done &&
             (*non_stream_tool)[3].finish_reason == ava::provider::ProviderFinishReason::ToolCalls,
         "OpenAI-compatible non-stream parser emits tool call events");

  ava::http::HttpResponse const missing_id_response{
      .status_code = 200,
      .headers = {},
      .body = R"({"choices":[{"message":{"tool_calls":[{"function":{"name":"read_file","arguments":"{}"}}]},"finish_reason":"tool_calls"}]})"};
  auto const first_missing_id = moonshot.parse_response(missing_id_response, false);
  auto const second_missing_id = moonshot.parse_response(missing_id_response, false);
  auto const first_fallback = first_missing_id && first_missing_id->size() >= 3 ? (*first_missing_id)[0].tool_call_id : std::string{};
  auto const second_fallback = second_missing_id && second_missing_id->size() >= 3 ? (*second_missing_id)[0].tool_call_id : std::string{};
  expect(!first_fallback.empty() && !second_fallback.empty() && first_fallback != second_fallback && (*first_missing_id)[1].tool_call_id == first_fallback &&
             (*first_missing_id)[2].tool_call_id == first_fallback,
         "OpenAI-compatible non-stream missing tool-call IDs use distinct per-response fallbacks");

  auto first_parser = moonshot.create_stream_parser();
  auto second_parser = moonshot.create_stream_parser();
  auto first_stream_fragment =
      first_parser->append("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"name\":\"grep\",\"arguments\":\"{\"}}]}}]}\n\n");
  auto merged_stream_fragment =
      first_parser->append("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"}\"}}]}}]}\n\n");
  auto distinct_stream =
      second_parser->append("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"name\":\"grep\",\"arguments\":\"{}\"}}]}}]}\n\n");
  auto const first_stream_id = first_stream_fragment && first_stream_fragment->size() == 2 ? (*first_stream_fragment)[0].tool_call_id : std::string{};
  auto const second_stream_id = distinct_stream && distinct_stream->size() == 2 ? (*distinct_stream)[0].tool_call_id : std::string{};
  expect(!first_stream_id.empty() && !second_stream_id.empty() && first_stream_id != second_stream_id && merged_stream_fragment &&
             merged_stream_fragment->size() == 1 && (*first_stream_fragment)[1].tool_call_id == first_stream_id &&
             (*merged_stream_fragment)[0].tool_call_id == first_stream_id,
         "OpenAI-compatible stream parsers use distinct fallbacks while same-parser index fragments retain one ID");

  auto const malformed = moonshot.parse_response(ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"choices\":[]}"}, false);
  expect(!malformed && malformed.error().category() == ava::core::ErrorCategory::Provider, "OpenAI-compatible non-stream parser rejects missing messages");

  auto const http_error = moonshot.parse_response(
      ava::http::HttpResponse{
          .status_code = 500,
          .headers = {},
          .body =
              R"({"error":{"message":"bad","reasoning_content":"COMPAT_HTTP_REASONING_CANARY","thinking":"COMPAT_HTTP_THINKING_CANARY","api_key":"COMPAT_HTTP_KEY_CANARY","unknown":{"private":"COMPAT_HTTP_NESTED_CANARY"}},"private":"COMPAT_HTTP_OUTER_CANARY"})"},
      false);
  auto const http_error_text = http_error ? std::string{} : http_error.error().format();
  expect(!http_error && http_error_text.find("provider_message") == std::string::npos && http_error_text.find("body_snippet") == std::string::npos &&
             http_error_text.find("COMPAT_HTTP_REASONING_CANARY") == std::string::npos &&
             http_error_text.find("COMPAT_HTTP_THINKING_CANARY") == std::string::npos && http_error_text.find("COMPAT_HTTP_KEY_CANARY") == std::string::npos &&
             http_error_text.find("COMPAT_HTTP_NESTED_CANARY") == std::string::npos && http_error_text.find("COMPAT_HTTP_OUTER_CANARY") == std::string::npos,
         "OpenAI-compatible HTTP errors expose only the allowlisted provider message");

  auto sse_error = ava::provider::parse_openai_compatible_sse(
      "data: {\"error\":{\"message\":\"{\\\"reasoning_content\\\":\\\"secret stream reasoning\\\","
      "\\\"api_key\\\":\\\"secret-stream-key\\\"}\"}}\n\n"
      ": OPENROUTER PROCESSING\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"must not appear\"}}]}\n\n"
      "data: [DONE]\n\n");
  expect(sse_error && sse_error->size() == 1 && (*sse_error)[0].type == ava::provider::StreamEventType::Error &&
             (*sse_error)[0].error_message == "OpenAI-compatible provider reported a streaming error" &&
             (*sse_error)[0].error_message.find("secret stream reasoning") == std::string::npos &&
             (*sse_error)[0].error_message.find("secret-stream-key") == std::string::npos &&
             (*sse_error)[0].error_message.find("must not appear") == std::string::npos,
         "OpenAI-compatible SSE errors reject provider payloads embedded inside message strings and terminate parsing");
  auto sse_error_after_open_state = ava::provider::parse_openai_compatible_sse(
      "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"plan\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_err\","
      "\"function\":{\"name\":\"grep\",\"arguments\":\"{}\"}}]}}]}\n\n"
      "data: {\"error\":{\"message\":\"provider failed\"}}\n\n"
      "data: [DONE]\n\n");
  expect(sse_error_after_open_state && sse_error_after_open_state->size() == 7 &&
             (*sse_error_after_open_state)[0].type == ava::provider::StreamEventType::ReasoningStart &&
             (*sse_error_after_open_state)[1].type == ava::provider::StreamEventType::ReasoningDelta &&
             (*sse_error_after_open_state)[2].type == ava::provider::StreamEventType::ToolCallStart &&
             (*sse_error_after_open_state)[3].type == ava::provider::StreamEventType::ToolCallDelta &&
             (*sse_error_after_open_state)[4].type == ava::provider::StreamEventType::ToolCallEnd &&
             (*sse_error_after_open_state)[5].type == ava::provider::StreamEventType::ReasoningEnd &&
             (*sse_error_after_open_state)[6].type == ava::provider::StreamEventType::Error,
         "OpenAI-compatible SSE provider errors close open lifecycle state before the terminal error");
  auto openrouter_keepalive = ava::provider::parse_openai_compatible_sse(
      ": OPENROUTER PROCESSING\n\n"
      "event: ping\n\n"
      "data: {\"choices\":[{\"delta\":{\"content\":\"routed\"}}]}\n\n"
      ": keep-alive\n\n"
      "data: [DONE]\n\n");
  expect(openrouter_keepalive && openrouter_keepalive->size() == 2 && (*openrouter_keepalive)[0].type == ava::provider::StreamEventType::TextDelta &&
             (*openrouter_keepalive)[0].text == "routed" && (*openrouter_keepalive)[1].type == ava::provider::StreamEventType::Done,
         "OpenAI-compatible SSE parser ignores OpenRouter comment and keepalive lines");
  auto malformed_compatible = ava::provider::parse_openai_compatible_sse("data: {not-json}\n\n");
  expect(malformed_compatible && malformed_compatible->size() == 1 && (*malformed_compatible)[0].type == ava::provider::StreamEventType::Error,
         "OpenAI-compatible parser does not add truncation error after malformed SSE data");

  ava::provider::OpenAICompatibleProvider const parser_provider(
      ava::provider::OpenAICompatibleProviderOptions{.base_url = "https://compat.example.test", .reasoning_format = "custom_reasoning"});
  auto parser = parser_provider.create_stream_parser();
  auto part_one = parser->append("data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"pl");
  expect(part_one && part_one->empty(), "OpenAI-compatible parser buffers partial SSE lines");
  auto part_two = parser->append(
      "an\"}}]}\n\n"
      "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
      "\"id\":\"call_2\",\"function\":{\"name\":\"grep\","
      "\"arguments\":\"{}\"}}]}}]}\n\n");
  expect(part_two && part_two->size() == 4 && (*part_two)[0].type == ava::provider::StreamEventType::ReasoningStart &&
             (*part_two)[0].reasoning_format == "custom_reasoning" && (*part_two)[1].type == ava::provider::StreamEventType::ReasoningDelta &&
             (*part_two)[2].type == ava::provider::StreamEventType::ToolCallStart && (*part_two)[2].tool_call_id == "call_2" &&
             (*part_two)[3].type == ava::provider::StreamEventType::ToolCallDelta,
         "OpenAI-compatible incremental parser preserves reasoning and tool state across chunks");
  auto flushed = parser->finish();
  expect(flushed && flushed->size() == 3 && (*flushed)[0].type == ava::provider::StreamEventType::ToolCallEnd &&
             (*flushed)[1].type == ava::provider::StreamEventType::ReasoningEnd && (*flushed)[2].type == ava::provider::StreamEventType::Error,
         "OpenAI-compatible parser flushes open tool calls and reasoning before reporting truncated streams");
  auto second_finish = parser->finish();
  expect(second_finish && second_finish->empty(), "OpenAI-compatible parser finish resets terminal state");

  auto const error =
      moonshot.parse_response(ava::http::HttpResponse{.status_code = 400, .headers = {}, .body = "Your request exceeded model token limit"}, true);
  expect(!error && error.error().format().find("provider_error_kind: context_overflow") != std::string::npos,
         "OpenAI-compatible HTTP errors reuse normalized context-overflow classification");
}

void test_builtin_openai_compatible_provider_contracts()
{
  ScopedEnvVar deepseek_base("DEEPSEEK_BASE_URL", "https://deepseek.override.test/");
  ScopedEnvVar kimi_base("KIMI_BASE_URL", "https://kimi.override.test/coding/");
  ScopedEnvVar moonshot_base("MOONSHOT_BASE_URL", "https://moonshot.override.test/api");
  ScopedEnvVar openrouter_base("OPENROUTER_BASE_URL", "https://openrouter.override.test/router/");
  ScopedEnvVar zai_base("ZAI_BASE_URL", "https://zai.override.test/paas/v4/");
  ScopedEnvVar zai_cn_base("ZAI_CODING_CN_BASE_URL", "https://zai-cn.override.test/paas/v4");
  auto registry = ava::provider::builtin_provider_registry();

  auto deepseek = registry.create("deepseek");
  expect(deepseek.has_value() && *deepseek, "builtin registry creates DeepSeek compatible provider");
  if (!deepseek || !*deepseek)
    return;
  auto const deepseek_request =
      (*deepseek)->build_request(ava::provider::ProviderRequest{.provider_id = "deepseek",
                                                                .model_id = "deepseek-v4-flash",
                                                                .system_prompt = "system",
                                                                .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
                                                                .tools_json = {},
                                                                .stream = true,
                                                                .max_output_tokens = 4096,
                                                                .reasoning = ava::provider::ProviderReasoningOptions{.type = "xhigh"}},
                                 "deepseek-token");
  expect(deepseek_request.has_value(), "builtin DeepSeek request builds");
  if (deepseek_request)
  {
    expect(deepseek_request->url == "https://deepseek.override.test/chat/completions",
           "builtin DeepSeek request honors env base URL and chat-completions path");
    expect(deepseek_request->headers.at("Authorization") == "Bearer deepseek-token" &&
               deepseek_request->body.find("\"model\":\"deepseek-v4-flash\"") != std::string::npos &&
               deepseek_request->body.find("\"reasoning_effort\":\"max\"") != std::string::npos &&
               deepseek_request->body.find("\"stream_options\":{\"include_usage\":true}") != std::string::npos &&
               deepseek_request->body.find("\"thinking\"") == std::string::npos,
           "builtin DeepSeek request uses bearer auth, stream usage, and string reasoning_effort without thinking controls");
    expect(deepseek_request->body.find("\"max_tokens\":4096") != std::string::npos &&
               deepseek_request->body.find("\"max_completion_tokens\"") == std::string::npos,
           "builtin DeepSeek request keeps chat-completions max_tokens rather than Z.AI max_completion_tokens");
  }

  auto kimi = registry.create("kimi");
  expect(kimi.has_value() && *kimi, "builtin registry creates Kimi compatible provider");
  if (!kimi || !*kimi)
    return;
  auto const kimi_request = (*kimi)->build_request(
      ava::provider::ProviderRequest{
          .provider_id = "kimi",
          .model_id = "kimi-k2-thinking",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"},
                       ava::provider::ChatMessage{
                           .role = "assistant",
                           .content = "fallback answer",
                           .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Reasoning,
                                                                        .text = "prior kimi reasoning",
                                                                        .reasoning_format = "reasoning_content"},
                                             ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text, .text = "visible answer"}}}},
          .tools_json = {},
          .stream = true,
          .reasoning = ava::provider::ProviderReasoningOptions{.type = "enabled"}},
      "kimi-token");
  expect(kimi_request.has_value(), "builtin Kimi request builds");
  if (kimi_request)
  {
    expect(kimi_request->url == "https://kimi.override.test/coding/v1/chat/completions", "builtin Kimi request honors env base URL and chat-completions path");
    expect(kimi_request->headers.at("Authorization") == "Bearer kimi-token" && kimi_request->headers.at("User-Agent") == "KimiCLI/1.5",
           "builtin Kimi request includes bearer auth and Kimi user agent");
    expect(kimi_request->body.find("\"model\":\"kimi-k2-thinking\"") != std::string::npos &&
               kimi_request->body.find("\"temperature\":1") != std::string::npos &&
               kimi_request->body.find("\"stream_options\":{\"include_usage\":true}") != std::string::npos &&
               kimi_request->body.find("\"thinking\":{\"type\":\"enabled\",\"keep\":\"all\"}") != std::string::npos &&
               kimi_request->body.find("\"reasoning_content\":\"prior kimi reasoning\"") != std::string::npos,
           "builtin Kimi request includes fixed temperature, stream usage, thinking keep-all, and replayed reasoning");
  }

  auto moonshot = registry.create("moonshot");
  expect(moonshot.has_value() && *moonshot, "builtin registry creates Moonshot compatible provider");
  if (!moonshot || !*moonshot)
    return;
  auto const moonshot_request = (*moonshot)->build_request(
      ava::provider::ProviderRequest{
          .provider_id = "moonshot",
          .model_id = "kimi-k2.6",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{
              .role = "assistant",
              .content = "moonshot fallback",
              .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Reasoning,
                                                           .text = "private moonshot reasoning",
                                                           .reasoning_format = "reasoning_content"},
                                ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text, .text = "moonshot answer"}}}},
          .tools_json = {},
          .stream = true,
          .reasoning = ava::provider::ProviderReasoningOptions{.type = "enabled"}},
      "moonshot-token");
  expect(moonshot_request.has_value(), "builtin Moonshot request builds");
  if (moonshot_request)
  {
    expect(moonshot_request->url == "https://moonshot.override.test/api/v1/chat/completions",
           "builtin Moonshot request honors env base URL and chat-completions path");
    expect(moonshot_request->headers.at("Authorization") == "Bearer moonshot-token", "builtin Moonshot request includes bearer auth");
    expect(moonshot_request->body.find("\"model\":\"kimi-k2.6\"") != std::string::npos &&
               moonshot_request->body.find("\"stream_options\":{\"include_usage\":true}") != std::string::npos &&
               moonshot_request->body.find("\"thinking\":{\"type\":\"enabled\"}") != std::string::npos,
           "builtin Moonshot request includes model, stream usage, and thinking controls");
    expect(moonshot_request->body.find("private moonshot reasoning") == std::string::npos &&
               moonshot_request->body.find("\"reasoning_content\"") == std::string::npos &&
               moonshot_request->body.find("\"keep\":\"all\"") == std::string::npos,
           "builtin Moonshot request does not replay private reasoning_content or request keep-all");
  }

  auto zai = registry.create("zai");
  expect(zai.has_value() && *zai, "builtin registry creates Z.AI compatible provider");
  if (!zai || !*zai)
    return;
  auto const zai_enabled = (*zai)->build_request(
      ava::provider::ProviderRequest{
          .provider_id = "zai",
          .model_id = "glm-4.7",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"},
                       ava::provider::ChatMessage{
                           .role = "assistant",
                           .content = "fallback",
                           .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Reasoning,
                                                                        .text = "prior zai reasoning",
                                                                        .reasoning_format = "reasoning_content"},
                                             ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text, .text = "visible"}}}},
          .tools_json = {"{\"name\":\"search\",\"description\":\"q\",\"parameters\":{\"type\":\"object\"}}"},
          .stream = true,
          .max_output_tokens = 8192,
          .reasoning = ava::provider::ProviderReasoningOptions{.type = "enabled"},
          .compatibility_quirks = {"zai", "openai_compatible", "reasoning_content", "preserve_reasoning_content", "max_completion_tokens", "tool_stream"}},
      "zai-token");
  expect(zai_enabled.has_value(), "builtin Z.AI enabled-thinking request builds");
  if (zai_enabled)
  {
    expect(zai_enabled->url == "https://zai.override.test/paas/v4/chat/completions", "builtin Z.AI request honors env base URL and chat-completions path");
    expect(zai_enabled->headers.at("Authorization") == "Bearer zai-token", "builtin Z.AI request includes bearer auth");
    expect(zai_enabled->body.find("\"model\":\"glm-4.7\"") != std::string::npos &&
               zai_enabled->body.find("\"thinking\":{\"type\":\"enabled\",\"clear_thinking\":false}") != std::string::npos &&
               zai_enabled->body.find("\"reasoning_content\":\"prior zai reasoning\"") != std::string::npos &&
               zai_enabled->body.find("\"tool_stream\":true") != std::string::npos &&
               zai_enabled->body.find("\"stream_options\":{\"include_usage\":true}") != std::string::npos &&
               zai_enabled->body.find("\"reasoning_effort\"") == std::string::npos && zai_enabled->body.find("\"keep\":\"all\"") == std::string::npos,
           "builtin Z.AI enabled request uses clear_thinking, replayed reasoning, tool_stream, and no keep-all/effort");
    expect(zai_enabled->body.find("\"max_completion_tokens\":8192") != std::string::npos && zai_enabled->body.find("\"max_tokens\"") == std::string::npos,
           "builtin Z.AI request serializes positive max_output_tokens as max_completion_tokens");
  }

  auto const zai_disabled = (*zai)->build_request(
      ava::provider::ProviderRequest{
          .provider_id = "zai",
          .model_id = "glm-4.7",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
          .tools_json = {},
          .stream = true,
          .compatibility_quirks = {"zai", "openai_compatible", "reasoning_content", "preserve_reasoning_content", "max_completion_tokens", "tool_stream"}},
      "zai-token");
  expect(zai_disabled.has_value(), "builtin Z.AI disabled-thinking request builds");
  if (zai_disabled)
  {
    expect(zai_disabled->body.find("\"thinking\":{\"type\":\"disabled\"}") != std::string::npos &&
               zai_disabled->body.find("\"tool_stream\"") == std::string::npos && zai_disabled->body.find("\"reasoning_effort\"") == std::string::npos,
           "builtin Z.AI disabled request omits tool_stream and effort while sending thinking disabled");
  }

  auto const zai_no_tool_stream = (*zai)->build_request(
      ava::provider::ProviderRequest{
          .provider_id = "zai",
          .model_id = "glm-4.5-air",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
          .tools_json = {"{\"name\":\"search\",\"description\":\"q\",\"parameters\":{\"type\":\"object\"}}"},
          .stream = true,
          .reasoning = ava::provider::ProviderReasoningOptions{.type = "enabled"},
          .compatibility_quirks = {"zai", "openai_compatible", "reasoning_content", "preserve_reasoning_content", "max_completion_tokens"}},
      "zai-token");
  expect(zai_no_tool_stream.has_value(), "builtin Z.AI non-tool-stream model request builds");
  if (zai_no_tool_stream)
  {
    expect(zai_no_tool_stream->body.find("\"tool_stream\"") == std::string::npos, "builtin Z.AI non-tool-stream model never sends tool_stream even with tools");
  }

  auto const zai_glm52_minimal = (*zai)->build_request(
      ava::provider::ProviderRequest{.provider_id = "zai",
                                     .model_id = "glm-5.2",
                                     .system_prompt = "system",
                                     .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
                                     .tools_json = {},
                                     .stream = true,
                                     .reasoning = ava::provider::ProviderReasoningOptions{.type = "minimal"},
                                     .compatibility_quirks = {"zai", "openai_compatible", "reasoning_content", "preserve_reasoning_content",
                                                              "max_completion_tokens", "tool_stream", "zai_reasoning_effort"}},
      "zai-token");
  expect(zai_glm52_minimal.has_value(), "builtin Z.AI glm-5.2 minimal request builds");
  if (zai_glm52_minimal)
  {
    expect(zai_glm52_minimal->body.find("\"thinking\":{\"type\":\"enabled\",\"clear_thinking\":false}") != std::string::npos &&
               zai_glm52_minimal->body.find("\"reasoning_effort\"") == std::string::npos,
           "builtin Z.AI glm-5.2 minimal enables thinking without reasoning_effort");
  }

  auto const zai_glm52_high = (*zai)->build_request(
      ava::provider::ProviderRequest{.provider_id = "zai",
                                     .model_id = "glm-5.2",
                                     .system_prompt = "system",
                                     .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
                                     .tools_json = {},
                                     .stream = true,
                                     .reasoning = ava::provider::ProviderReasoningOptions{.type = "high"},
                                     .compatibility_quirks = {"zai", "openai_compatible", "reasoning_content", "preserve_reasoning_content",
                                                              "max_completion_tokens", "tool_stream", "zai_reasoning_effort"}},
      "zai-token");
  expect(zai_glm52_high.has_value(), "builtin Z.AI glm-5.2 high request builds");
  if (zai_glm52_high)
  {
    expect(zai_glm52_high->body.find("\"thinking\":{\"type\":\"enabled\",\"clear_thinking\":false}") != std::string::npos &&
               zai_glm52_high->body.find("\"reasoning_effort\":\"high\"") != std::string::npos,
           "builtin Z.AI glm-5.2 high maps to reasoning_effort high");
  }

  auto const zai_glm52_max = (*zai)->build_request(
      ava::provider::ProviderRequest{.provider_id = "zai",
                                     .model_id = "glm-5.2",
                                     .system_prompt = "system",
                                     .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
                                     .tools_json = {},
                                     .stream = true,
                                     .reasoning = ava::provider::ProviderReasoningOptions{.type = "max"},
                                     .compatibility_quirks = {"zai", "openai_compatible", "reasoning_content", "preserve_reasoning_content",
                                                              "max_completion_tokens", "tool_stream", "zai_reasoning_effort"}},
      "zai-token");
  expect(zai_glm52_max.has_value(), "builtin Z.AI glm-5.2 max request builds");
  if (zai_glm52_max)
  {
    expect(zai_glm52_max->body.find("\"reasoning_effort\":\"max\"") != std::string::npos, "builtin Z.AI glm-5.2 xhigh mapping yields reasoning_effort max");
  }

  auto zai_cn = registry.create("zai-coding-cn");
  expect(zai_cn.has_value() && *zai_cn, "builtin registry creates Z.AI Coding CN compatible provider");
  if (!zai_cn || !*zai_cn)
    return;
  auto const zai_cn_request = (*zai_cn)->build_request(
      ava::provider::ProviderRequest{
          .provider_id = "zai-coding-cn",
          .model_id = "glm-5.1",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
          .tools_json = {},
          .stream = true,
          .max_output_tokens = 2048,
          .reasoning = ava::provider::ProviderReasoningOptions{.type = "enabled"},
          .compatibility_quirks = {"zai", "openai_compatible", "reasoning_content", "preserve_reasoning_content", "max_completion_tokens", "tool_stream"}},
      "zai-cn-token");
  expect(zai_cn_request.has_value(), "builtin Z.AI Coding CN request builds");
  if (zai_cn_request)
  {
    expect(zai_cn_request->url == "https://zai-cn.override.test/paas/v4/chat/completions",
           "builtin Z.AI Coding CN request honors env base URL and chat-completions path");
    expect(zai_cn_request->headers.at("Authorization") == "Bearer zai-cn-token" &&
               zai_cn_request->body.find("\"thinking\":{\"type\":\"enabled\",\"clear_thinking\":false}") != std::string::npos,
           "builtin Z.AI Coding CN request includes bearer auth and Z.AI thinking controls");
    expect(zai_cn_request->body.find("\"max_completion_tokens\":2048") != std::string::npos && zai_cn_request->body.find("\"max_tokens\"") == std::string::npos,
           "builtin Z.AI Coding CN request serializes positive max_output_tokens as max_completion_tokens");
  }

  auto openrouter = registry.create("openrouter");
  expect(openrouter.has_value() && *openrouter, "builtin registry creates OpenRouter compatible provider");
  if (!openrouter || !*openrouter)
    return;
  auto const openrouter_request =
      (*openrouter)
          ->build_request(ava::provider::ProviderRequest{.provider_id = "openrouter",
                                                         .model_id = "moonshotai/kimi-k2.6",
                                                         .system_prompt = "system",
                                                         .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
                                                         .tools_json = {},
                                                         .stream = true},
                          "openrouter-token");
  expect(openrouter_request.has_value(), "builtin OpenRouter request builds");
  if (openrouter_request)
  {
    expect(openrouter_request->url == "https://openrouter.override.test/router/v1/chat/completions",
           "builtin OpenRouter request honors env base URL and chat-completions path");
    expect(openrouter_request->headers.at("Authorization") == "Bearer openrouter-token" &&
               openrouter_request->body.find("\"model\":\"moonshotai/kimi-k2.6\"") != std::string::npos &&
               openrouter_request->body.find("\"stream_options\":{\"include_usage\":true}") != std::string::npos,
           "builtin OpenRouter request includes bearer auth, model, and stream usage");
  }

  struct CompatibleErrorCase
  {
    std::string provider_id;
    std::string label;
    int status_code = 0;
    std::map<std::string, std::string> headers;
    std::string body;
    ava::provider::ProviderErrorKind expected_kind = ava::provider::ProviderErrorKind::Unknown;
    std::string required_context;
  };

  std::vector<CompatibleErrorCase> const error_cases = {
      CompatibleErrorCase{"deepseek",
                          "DeepSeek authentication error",
                          401,
                          {},
                          "{\"error\":{\"message\":\"invalid api key\"}}",
                          ava::provider::ProviderErrorKind::Authentication,
                          ""},
      CompatibleErrorCase{
          "deepseek", "DeepSeek context-overflow error", 400, {}, "maximum context length exceeded", ava::provider::ProviderErrorKind::ContextOverflow, ""},
      CompatibleErrorCase{
          "kimi", "Kimi authentication error", 401, {}, "{\"error\":{\"message\":\"invalid api key\"}}", ava::provider::ProviderErrorKind::Authentication, ""},
      CompatibleErrorCase{"kimi", "Kimi context-overflow error", 400, {}, "Input token length too long", ava::provider::ProviderErrorKind::ContextOverflow, ""},
      CompatibleErrorCase{"moonshot",
                          "Moonshot rate-limit error",
                          429,
                          {{"Retry-After", "4"}},
                          "{\"error\":{\"message\":\"rate limit\"}}",
                          ava::provider::ProviderErrorKind::RateLimited,
                          "retry_after: 4"},
      CompatibleErrorCase{"moonshot",
                          "Moonshot context-overflow error",
                          400,
                          {},
                          "Your request exceeded model token limit",
                          ava::provider::ProviderErrorKind::ContextOverflow,
                          ""},
      CompatibleErrorCase{"openrouter",
                          "OpenRouter transient error",
                          503,
                          {},
                          "{\"error\":{\"message\":\"upstream overloaded\"}}",
                          ava::provider::ProviderErrorKind::Transient,
                          ""},
      CompatibleErrorCase{"openrouter",
                          "OpenRouter invalid-request error",
                          422,
                          {},
                          "{\"error\":{\"message\":\"invalid model\"}}",
                          ava::provider::ProviderErrorKind::InvalidRequest,
                          ""},
      CompatibleErrorCase{"openrouter",
                          "OpenRouter insufficient-credits error",
                          402,
                          {},
                          "{\"error\":{\"message\":\"insufficient credits\"}}",
                          ava::provider::ProviderErrorKind::Quota,
                          ""},
      CompatibleErrorCase{
          "zai", "Z.AI authentication error", 401, {}, "{\"error\":{\"message\":\"invalid api key\"}}", ava::provider::ProviderErrorKind::Authentication, ""},
      CompatibleErrorCase{"zai-coding-cn",
                          "Z.AI Coding CN rate-limit error",
                          429,
                          {{"Retry-After", "3"}},
                          "{\"error\":{\"message\":\"rate limit\"}}",
                          ava::provider::ProviderErrorKind::RateLimited,
                          "retry_after: 3"},
      CompatibleErrorCase{"openrouter",
                          "OpenRouter context-overflow error",
                          400,
                          {},
                          "{\"error\":{\"message\":\"This model's maximum context length is 8192 tokens\"}}",
                          ava::provider::ProviderErrorKind::ContextOverflow,
                          ""},
  };

  for (auto const& error_case : error_cases)
  {
    auto compatible = registry.create(error_case.provider_id);
    expect(compatible.has_value() && *compatible, error_case.label + " provider is registered");
    if (!compatible || !*compatible)
      continue;
    auto parsed =
        (*compatible)
            ->parse_response(ava::http::HttpResponse{.status_code = error_case.status_code, .headers = error_case.headers, .body = error_case.body}, false);
    expect(!parsed && parsed.error().format().find("provider_error_kind: " + ava::provider::to_string(error_case.expected_kind)) != std::string::npos,
           error_case.label + " is normalized with the expected compatible provider error kind");
    if (!parsed && !error_case.required_context.empty())
    {
      expect(parsed.error().format().find(error_case.required_context) != std::string::npos, error_case.label + " preserves expected error context");
    }
    if (!parsed && error_case.expected_kind == ava::provider::ProviderErrorKind::ContextOverflow)
    {
      expect(ava::provider::is_context_overflow_error(parsed.error()), error_case.label + " is detectable as a context-overflow provider error");
    }
  }
}

}  // namespace ava::tests::provider_openai_suite
