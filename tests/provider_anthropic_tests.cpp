#include "sys.h"
#include "tests/support/fake_transport.h"
#include "tests/support/provider_parser_budget_tests.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/agent/agent_loop.h"
#include "ava/config/auth.h"
#include "ava/config/xdg_paths.h"
#include "ava/session/assistant_output.h"
#include "ava/session/session_store.h"
#include "ava/provider/anthropic_provider.h"
#include "ava/provider/provider_utils.h"
#include "ava/provider/registry.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

ava::http::HttpResponse sse_response(std::string body)
{
  return ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = std::move(body)};
}

void test_json_object_validator()
{
  expect(ava::provider::is_valid_json_object(R"({})"), "JSON validator accepts empty objects");
  expect(ava::provider::is_valid_json_object(R"({"path":"note.txt","nested":{"enabled":true},"list":[1,-2.5e+3,null,false]})"),
         "JSON validator accepts nested object values");
  expect(ava::provider::is_valid_json_object(R"({"escaped":"quote \" slash \\ unicode \u20ac"})"),
         "JSON validator accepts escaped strings and unicode escapes");
  expect(!ava::provider::is_valid_json_object(""), "JSON validator rejects empty input");
  expect(!ava::provider::is_valid_json_object(R"([])"), "JSON validator rejects non-object roots");
  expect(!ava::provider::is_valid_json_object(R"({"path":})"), "JSON validator rejects missing values");
  expect(!ava::provider::is_valid_json_object(R"({"path":"note.txt",})"), "JSON validator rejects trailing commas");
  expect(!ava::provider::is_valid_json_object(R"({"n":01})"), "JSON validator rejects leading-zero numbers");
  expect(!ava::provider::is_valid_json_object(R"({"bad":"\x"})"), "JSON validator rejects invalid escapes");
  expect(!ava::provider::is_valid_json_object(std::string(R"({"path":"note.txt"})") + '\f'), "JSON validator rejects non-JSON whitespace");
}

void test_anthropic_provider_contract()
{
  constexpr std::string_view system_prompt_marker = "AVA-ISSUE-54-ANTHROPIC-SYSTEM-PROMPT";
  ava::provider::AnthropicProvider const provider("https://anthropic.example.test/");
  auto const request = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = std::string(system_prompt_marker),
          .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}, ava::provider::ChatMessage{.role = "assistant", .content = "hi"}},
          .tools_json =
              {R"({"type":"function","name":"read_file","description":"Read","parameters":{"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}})"},
          .stream = true,
          .max_output_tokens = 64'000},
      "anthropic-key");
  expect(request.has_value(), "Anthropic request builds with API key");
  if (request)
  {
    expect(request->method == "POST" && request->url == "https://anthropic.example.test/v1/messages",
           "Anthropic request targets messages endpoint with trimmed base URL");
    expect(request->headers.at("x-api-key") == "anthropic-key", "Anthropic API-key request uses x-api-key header");
    expect(request->headers.at("anthropic-version") == "2023-06-01", "Anthropic request carries API version");
    expect(request->headers.at("Accept") == "text/event-stream" && !request->follow_redirects,
           "Anthropic streaming request asks for SSE and does not follow authenticated redirects");
    expect(request->body.find("\"model\":\"claude-sonnet-4-5\"") != std::string::npos, "Anthropic request includes model id");
    expect(request->body.find("\"max_tokens\":64000") != std::string::npos, "Anthropic request uses model max output token metadata when supplied");
    expect(ava::tests::count_occurrences(request->body, system_prompt_marker) == 1, "Anthropic request serializes the effective system prompt exactly once");
    expect(request->body.find("\"system\":\"" + std::string(system_prompt_marker) + "\"") != std::string::npos,
           "Anthropic request places the effective system prompt in the top-level system field");
    expect(request->body.find("\"stream\":true") != std::string::npos, "Anthropic request preserves stream flag");
    expect(request->body.find("\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}") != std::string::npos, "Anthropic request includes messages");
    expect(request->body.find("\"input_schema\":{\"type\":\"object\"") != std::string::npos, "Anthropic request maps OpenAI-style parameters to input_schema");
    expect(request->body.find("\"parameters\"") == std::string::npos, "Anthropic request does not send OpenAI parameters key");
  }

  auto const auth_request = ava::provider::ProviderRequest{.provider_id = "anthropic",
                                                           .model_id = "claude-sonnet-4-5",
                                                           .system_prompt = "system",
                                                           .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
                                                           .tools_json = {},
                                                           .stream = false};
  auto const api_auth_request = provider.build_request(
      auth_request, ava::provider::ProviderAuthContext{.access_token = "api-context-key", .credential_type = "api_key", .account_id = ""});
  expect(api_auth_request && api_auth_request->headers.at("x-api-key") == "api-context-key" &&
             api_auth_request->headers.find("Authorization") == api_auth_request->headers.end() &&
             api_auth_request->headers.find("anthropic-beta") == api_auth_request->headers.end(),
         "Anthropic API-key auth context keeps x-api-key behavior without OAuth beta marker");

  auto const oauth_auth_request = provider.build_request(
      auth_request, ava::provider::ProviderAuthContext{.access_token = "oauth-context-token", .credential_type = "oauth", .account_id = "acct"});
  expect(oauth_auth_request && oauth_auth_request->headers.find("x-api-key") == oauth_auth_request->headers.end() &&
             oauth_auth_request->headers.at("Authorization") == "Bearer oauth-context-token" &&
             oauth_auth_request->headers.at("anthropic-beta") == "oauth-2025-04-20",
         "Anthropic OAuth auth context uses bearer authorization and OAuth beta marker");

  ava::http::HttpRequest existing_beta{.method = "POST",
                                       .url = "https://anthropic.example.test/v1/messages",
                                       .headers = {{"x-api-key", "old-key"}, {"anthropic-beta", "prompt-caching-2024-07-31, oauth-2025-04-20"}},
                                       .body = "{}",
                                       .timeout_ms = 60000,
                                       .follow_redirects = false,
                                       .include_response_headers = false,
                                       .resolve_hosts = {}};
  auto applied = provider.apply_auth_options(
      existing_beta, ava::provider::ProviderAuthContext{.access_token = "oauth-context-token", .credential_type = "oauth", .account_id = "acct"});
  auto const& beta = existing_beta.headers.at("anthropic-beta");
  expect(applied && existing_beta.headers.find("x-api-key") == existing_beta.headers.end() &&
             existing_beta.headers.at("Authorization") == "Bearer oauth-context-token" && beta.find("prompt-caching-2024-07-31") != std::string::npos &&
             beta.find("oauth-2025-04-20") == beta.rfind("oauth-2025-04-20"),
         "Anthropic OAuth auth preserves existing beta features without duplicating OAuth marker");

  auto const empty_system = provider.build_request(ava::provider::ProviderRequest{.provider_id = "anthropic",
                                                                                  .model_id = "claude-sonnet-4-5",
                                                                                  .system_prompt = "",
                                                                                  .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"}},
                                                                                  .tools_json = {},
                                                                                  .stream = true},
                                                   "anthropic-key");
  expect(empty_system && empty_system->body.find(",,") == std::string::npos && empty_system->body.find("\"system\"") == std::string::npos &&
             empty_system->body.find("\"messages\"") != std::string::npos,
         "Anthropic request omits empty system prompt without malformed separators");

  auto const collapsed =
      provider.build_request(ava::provider::ProviderRequest{.provider_id = "anthropic",
                                                            .model_id = "claude-sonnet-4-5",
                                                            .system_prompt = "system",
                                                            .messages = {ava::provider::ChatMessage{.role = "assistant", .content = "first"},
                                                                         ava::provider::ChatMessage{.role = "assistant", .content = "second"},
                                                                         ava::provider::ChatMessage{.role = "user", .content = "third"},
                                                                         ava::provider::ChatMessage{.role = "user", .content = "fourth"}},
                                                            .tools_json = {},
                                                            .stream = false},
                             "anthropic-key");
  expect(collapsed && collapsed->body.find("first\\n\\nsecond") != std::string::npos && collapsed->body.find("third\\n\\nfourth") != std::string::npos,
         "Anthropic request collapses consecutive same-role messages for role alternation");

  auto const cached_collapse = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback cached prefix",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text,
                                                                                               .text = "cached prefix",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .cache_control_ttl = "1h"}}},
                       ava::provider::ChatMessage{.role = "user", .content = "uncached suffix"}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(cached_collapse.has_value(), "Anthropic request builds when collapsing cache-controlled text");
  if (cached_collapse)
  {
    expect(cached_collapse->body.find(R"({"type":"text","text":"cached prefix","cache_control":{"type":"ephemeral","ttl":"1h"}})") != std::string::npos,
           "Anthropic collapse preserves the cached text block");
    expect(cached_collapse->body.find(R"({"type":"text","text":"\n\n"})") != std::string::npos &&
               cached_collapse->body.find(R"({"type":"text","text":"uncached suffix"})") != std::string::npos,
           "Anthropic collapse keeps uncached suffix outside the cached block");
    expect(cached_collapse->body.find("cached prefix\\n\\nuncached suffix") == std::string::npos,
           "Anthropic collapse does not merge uncached text into a cache-controlled block");
  }

  auto const reasoning_and_cache = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "cache system",
          .messages = {ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback cached text",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text,
                                                                                               .text = "cached text",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .cache_control_ttl = "5m"}}}},
          .tools_json = {},
          .stream = false,
          .max_output_tokens = 4096,
          .reasoning = ava::provider::ProviderReasoningOptions{.type = "enabled", .budget_tokens = 1024, .display = "summarized"},
          .system_prompt_cache_ttl = "1h"},
      "anthropic-key");
  expect(reasoning_and_cache.has_value(), "Anthropic request builds with thinking and cache controls");
  if (reasoning_and_cache)
  {
    expect(reasoning_and_cache->body.find(R"("system":[{"type":"text","text":"cache system","cache_control":{"type":"ephemeral","ttl":"1h"}}])") !=
               std::string::npos,
           "Anthropic request serializes system prompt cache control");
    expect(reasoning_and_cache->body.find(R"({"type":"text","text":"cached text","cache_control":{"type":"ephemeral","ttl":"5m"}})") != std::string::npos,
           "Anthropic request serializes content block cache control");
    expect(reasoning_and_cache->body.find(R"("thinking":{"type":"enabled","budget_tokens":1024,"display":"summarized"})") != std::string::npos,
           "Anthropic request serializes thinking controls");
  }

  auto const invalid_budget =
      provider.build_request(ava::provider::ProviderRequest{.provider_id = "anthropic",
                                                            .model_id = "claude-sonnet-4-5",
                                                            .system_prompt = "system",
                                                            .messages = {},
                                                            .tools_json = {},
                                                            .stream = false,
                                                            .max_output_tokens = 1024,
                                                            .reasoning = ava::provider::ProviderReasoningOptions{.type = "enabled", .budget_tokens = 1024}},
                             "anthropic-key");
  expect(!invalid_budget && invalid_budget.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects thinking budgets at or above max_tokens");

  auto const too_small_budget =
      provider.build_request(ava::provider::ProviderRequest{.provider_id = "anthropic",
                                                            .model_id = "claude-sonnet-4-5",
                                                            .system_prompt = "system",
                                                            .messages = {},
                                                            .tools_json = {},
                                                            .stream = false,
                                                            .max_output_tokens = 4096,
                                                            .reasoning = ava::provider::ProviderReasoningOptions{.type = "enabled", .budget_tokens = 1023}},
                             "anthropic-key");
  expect(!too_small_budget && too_small_budget.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects thinking budgets below provider minimum");

  auto const adaptive_with_budget =
      provider.build_request(ava::provider::ProviderRequest{.provider_id = "anthropic",
                                                            .model_id = "claude-sonnet-4-5",
                                                            .system_prompt = "system",
                                                            .messages = {},
                                                            .tools_json = {},
                                                            .stream = false,
                                                            .max_output_tokens = 4096,
                                                            .reasoning = ava::provider::ProviderReasoningOptions{.type = "adaptive", .budget_tokens = 1024}},
                             "anthropic-key");
  expect(!adaptive_with_budget && adaptive_with_budget.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects adaptive thinking with a fixed budget");

  auto const unsupported_adaptive =
      provider.build_request(ava::provider::ProviderRequest{.provider_id = "anthropic",
                                                            .model_id = "claude-sonnet-4-5",
                                                            .system_prompt = "system",
                                                            .messages = {},
                                                            .tools_json = {},
                                                            .stream = false,
                                                            .reasoning = ava::provider::ProviderReasoningOptions{.type = "adaptive"}},
                             "anthropic-key");
  expect(!unsupported_adaptive && unsupported_adaptive.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects adaptive thinking for Sonnet 4.5");

  auto const invalid_cache_ttl = provider.build_request(ava::provider::ProviderRequest{.provider_id = "anthropic",
                                                                                       .model_id = "claude-sonnet-4-5",
                                                                                       .system_prompt = "system",
                                                                                       .messages = {},
                                                                                       .tools_json = {},
                                                                                       .stream = false,
                                                                                       .system_prompt_cache_ttl = "24h"},
                                                        "anthropic-key");
  expect(!invalid_cache_ttl && invalid_cache_ttl.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects unsupported cache-control ttl values");

  auto const invalid_cache_order = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text,
                                                                                               .text = "later long cache",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .cache_control_ttl = "1h"}}}},
          .tools_json = {},
          .stream = false,
          .system_prompt_cache_ttl = "5m"},
      "anthropic-key");
  expect(!invalid_cache_order && invalid_cache_order.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects 1h cache-control after a 5m breakpoint");

  auto const empty_system_cache = provider.build_request(ava::provider::ProviderRequest{.provider_id = "anthropic",
                                                                                        .model_id = "claude-sonnet-4-5",
                                                                                        .system_prompt = "",
                                                                                        .messages = {},
                                                                                        .tools_json = {},
                                                                                        .stream = false,
                                                                                        .system_prompt_cache_ttl = "5m"},
                                                         "anthropic-key");
  expect(!empty_system_cache && empty_system_cache.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects cache-control on empty system prompts");

  auto const empty_text_cache = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text,
                                                                                               .text = "",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .cache_control_ttl = "5m"}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(!empty_text_cache && empty_text_cache.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects cache-control on empty text blocks");

  auto const too_many_cache_breakpoints = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text,
                                                                                               .text = "one",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .cache_control_ttl = "1h"},
                                                                    ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text,
                                                                                               .text = "two",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .cache_control_ttl = "1h"},
                                                                    ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text,
                                                                                               .text = "three",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .cache_control_ttl = "1h"},
                                                                    ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text,
                                                                                               .text = "four",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .cache_control_ttl = "1h"}}}},
          .tools_json = {},
          .stream = false,
          .system_prompt_cache_ttl = "1h"},
      "anthropic-key");
  expect(!too_many_cache_breakpoints && too_many_cache_breakpoints.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects more than four cache-control breakpoints");

  auto const missing_token = provider.build_request(
      ava::provider::ProviderRequest{.provider_id = "anthropic", .model_id = "claude-sonnet-4-5", .system_prompt = "system", .messages = {}, .tools_json = {}},
      "");
  expect(!missing_token && missing_token.error().category() == ava::core::ErrorCategory::PermissionDenied, "Anthropic request rejects empty token");
}

void test_anthropic_native_content_parts_request()
{
  ava::provider::AnthropicProvider const provider("https://anthropic.example.test");
  auto const request = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"},
                       ava::provider::ChatMessage{.role = "assistant",
                                                  .content = "fallback tool call text",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                                               .text = "",
                                                                                               .tool_call_id = "toolu_1",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = R"({"path":"note.txt"})",
                                                                                               .is_error = false}}},
                       ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback tool result text",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolResult,
                                                                                               .text = "tool content",
                                                                                               .tool_call_id = "toolu_1",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "",
                                                                                               .is_error = true}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(request.has_value(), "Anthropic request builds with native content parts");
  if (!request)
    return;
  expect(request->body.find(R"({"type":"tool_use","id":"toolu_1","name":"read_file","input":{"path":"note.txt"}})") != std::string::npos,
         "Anthropic request serializes native tool_use content blocks");
  expect(request->body.find(R"({"type":"tool_result","tool_use_id":"toolu_1","content":"tool content","is_error":true})") != std::string::npos,
         "Anthropic request serializes native tool_result content blocks with error metadata");
  expect(request->body.find("fallback tool call text") == std::string::npos && request->body.find("fallback tool result text") == std::string::npos,
         "Anthropic native content parts are canonical over fallback text");

  auto const reasoning_request = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user", .content = "hello"},
                       ava::provider::ChatMessage{.role = "assistant",
                                                  .content = "fallback reasoning text",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Reasoning,
                                                                                               .text = "visible reasoning",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .reasoning_format = "anthropic_thinking",
                                                                                               .reasoning_signature = "sig-1",
                                                                                               .reasoning_redacted_data = "",
                                                                                               .redacted = false},
                                                                    ava::provider::ContentPart{.type = ava::provider::ContentPartType::Reasoning,
                                                                                               .text = "",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .reasoning_format = "anthropic_thinking",
                                                                                               .reasoning_signature = "",
                                                                                               .reasoning_redacted_data = "opaque-redacted",
                                                                                               .redacted = true}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(reasoning_request.has_value(), "Anthropic request builds with reasoning content parts");
  if (reasoning_request)
  {
    expect(reasoning_request->body.find(R"({"type":"thinking","thinking":"visible reasoning","signature":"sig-1"})") != std::string::npos,
           "Anthropic request serializes thinking content blocks with signatures");
    expect(reasoning_request->body.find(R"({"type":"redacted_thinking","data":"opaque-redacted"})") != std::string::npos,
           "Anthropic request serializes redacted thinking content blocks without visible text");
    expect(reasoning_request->body.find("fallback reasoning text") == std::string::npos, "Anthropic reasoning content parts are canonical over fallback text");
  }

  auto const wrong_reasoning_role = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Reasoning,
                                                                                               .text = "user reasoning",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(!wrong_reasoning_role && wrong_reasoning_role.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects reasoning content outside assistant role");

  auto const reasoning_cache_control = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "assistant",
                                                  .content = "fallback",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Reasoning,
                                                                                               .text = "cached reasoning",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .cache_control_ttl = "5m"}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(!reasoning_cache_control && reasoning_cache_control.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects cache-control on reasoning content blocks");

  auto const wrong_reasoning_format = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "assistant",
                                                  .content = "fallback",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Reasoning,
                                                                                               .text = "foreign reasoning",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .reasoning_format = "reasoning_content"}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(!wrong_reasoning_format && wrong_reasoning_format.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects non-Anthropic reasoning replay formats");

  auto const cached_tool_request = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "assistant",
                                                  .content = "fallback tool call text",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                                               .text = "",
                                                                                               .tool_call_id = "toolu_cached",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = R"({"path":"note.txt"})",
                                                                                               .is_error = false,
                                                                                               .cache_control_ttl = "1h"}}},
                       ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback tool result text",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolResult,
                                                                                               .text = "cached tool content",
                                                                                               .tool_call_id = "toolu_cached",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "",
                                                                                               .is_error = false,
                                                                                               .cache_control_ttl = "1h"}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(cached_tool_request.has_value(), "Anthropic request accepts cache-control on native tool blocks");
  if (cached_tool_request)
  {
    expect(cached_tool_request->body.find(
               R"({"type":"tool_use","id":"toolu_cached","name":"read_file","input":{"path":"note.txt"},"cache_control":{"type":"ephemeral","ttl":"1h"}})") !=
               std::string::npos,
           "Anthropic request serializes tool_use cache control");
    expect(cached_tool_request->body.find(
               R"({"type":"tool_result","tool_use_id":"toolu_cached","content":"cached tool content","cache_control":{"type":"ephemeral","ttl":"1h"}})") !=
               std::string::npos,
           "Anthropic request serializes tool_result cache control");
  }

  auto const invalid_input = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "assistant",
                                                  .content = "fallback",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                                               .text = "",
                                                                                               .tool_call_id = "toolu_bad",
                                                                                               .tool_name = "bash",
                                                                                               .input_json = R"({"cmd":})",
                                                                                               .is_error = false}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(!invalid_input && invalid_input.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects invalid native tool_use input before serialization");

  auto const image_input = provider.build_request(
      ava::provider::ProviderRequest{.provider_id = "anthropic",
                                     .model_id = "claude-sonnet-4-5",
                                     .system_prompt = "system",
                                     .messages = {ava::provider::ChatMessage{.role = "user",
                                                                             .content = "fallback image metadata",
                                                                             .content_parts = {ava::provider::ContentPart{
                                                                                 .type = ava::provider::ContentPartType::Image,
                                                                                 .attachment_id = "img_1",
                                                                                 .mime_type = "image/png",
                                                                                 .storage_path = "attachments/img_1.png",
                                                                                 .sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                                                                 .byte_size = 128}}}},
                                     .tools_json = {},
                                     .stream = false},
      "anthropic-key");
  expect(!image_input && image_input.error().message().find("verified attachment bytes") != std::string::npos,
         "Anthropic request rejects image content without verified attachment bytes");

  auto const serialized_image_input = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
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
      "anthropic-key");
  expect(serialized_image_input && serialized_image_input->body.find(R"({"type":"text","text":"describe this"})") != std::string::npos &&
             serialized_image_input->body.find(R"({"type":"image","source":{"type":"base64","media_type":"image/png","data":"aGk="}})") != std::string::npos,
         "Anthropic request serializes verified image content parts");

  auto const oversized_image_input = provider.build_request(
      ava::provider::ProviderRequest{.provider_id = "anthropic",
                                     .model_id = "claude-sonnet-4-5",
                                     .system_prompt = "system",
                                     .messages = {ava::provider::ChatMessage{.role = "user",
                                                                             .content = "fallback image metadata",
                                                                             .content_parts = {ava::provider::ContentPart{
                                                                                 .type = ava::provider::ContentPartType::Image,
                                                                                 .attachment_id = "img_big",
                                                                                 .mime_type = "image/png",
                                                                                 .storage_path = "attachments/img_big.png",
                                                                                 .sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                                                                                 .byte_size = 6 * 1024 * 1024,
                                                                                 .data_base64 = "aGk="}}}},
                                     .tools_json = {},
                                     .stream = false},
      "anthropic-key");
  expect(!oversized_image_input && oversized_image_input.error().message().find("byte size") != std::string::npos,
         "Anthropic request enforces provider image byte-size limits before serialization");

  auto const invalid_base64_image_input = provider.build_request(
      ava::provider::ProviderRequest{.provider_id = "anthropic",
                                     .model_id = "claude-sonnet-4-5",
                                     .system_prompt = "system",
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
      "anthropic-key");
  expect(!invalid_base64_image_input && invalid_base64_image_input.error().message().find("verified attachment bytes") != std::string::npos,
         "Anthropic request rejects invalid image base64 payloads");

  auto const wrong_role = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                                               .text = "",
                                                                                               .tool_call_id = "toolu_wrong_role",
                                                                                               .tool_name = "bash",
                                                                                               .input_json = "{}",
                                                                                               .is_error = false}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(!wrong_role && wrong_role.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects role-incompatible native content parts");

  auto const dangling_tool_use = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "assistant",
                                                  .content = "fallback",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                                               .text = "",
                                                                                               .tool_call_id = "toolu_dangling",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "{}",
                                                                                               .is_error = false}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(!dangling_tool_use && dangling_tool_use.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects native tool_use blocks without matching tool_result");

  auto const dangling_tool_result = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolResult,
                                                                                               .text = "tool content",
                                                                                               .tool_call_id = "toolu_missing",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "",
                                                                                               .is_error = false}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(!dangling_tool_result && dangling_tool_result.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects native tool_result blocks without a preceding tool_use");

  auto const duplicate_tool_use = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "assistant",
                                                  .content = "fallback",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                                               .text = "",
                                                                                               .tool_call_id = "toolu_dup",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "{}",
                                                                                               .is_error = false},
                                                                    ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                                               .text = "",
                                                                                               .tool_call_id = "toolu_dup",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "{}",
                                                                                               .is_error = false}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(!duplicate_tool_use && duplicate_tool_use.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects duplicate native tool_use ids");

  auto const text_before_tool_result = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "assistant",
                                                  .content = "fallback tool call",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                                               .text = "",
                                                                                               .tool_call_id = "toolu_text_first",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "{}",
                                                                                               .is_error = false}}},
                       ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback result",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::Text,
                                                                                               .text = "ordinary text before result",
                                                                                               .tool_call_id = "",
                                                                                               .tool_name = "",
                                                                                               .input_json = "",
                                                                                               .is_error = false},
                                                                    ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolResult,
                                                                                               .text = "tool content",
                                                                                               .tool_call_id = "toolu_text_first",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "",
                                                                                               .is_error = false}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(!text_before_tool_result && text_before_tool_result.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects ordinary user text before matching native tool_result");

  auto const reversed_tool_results = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic",
          .model_id = "claude-sonnet-4-5",
          .system_prompt = "system",
          .messages = {ava::provider::ChatMessage{.role = "assistant",
                                                  .content = "fallback tool calls",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                                               .text = "",
                                                                                               .tool_call_id = "toolu_a",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "{}",
                                                                                               .is_error = false},
                                                                    ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolUse,
                                                                                               .text = "",
                                                                                               .tool_call_id = "toolu_b",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "{}",
                                                                                               .is_error = false}}},
                       ava::provider::ChatMessage{.role = "user",
                                                  .content = "fallback results",
                                                  .content_parts = {ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolResult,
                                                                                               .text = "b result",
                                                                                               .tool_call_id = "toolu_b",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "",
                                                                                               .is_error = false},
                                                                    ava::provider::ContentPart{.type = ava::provider::ContentPartType::ToolResult,
                                                                                               .text = "a result",
                                                                                               .tool_call_id = "toolu_a",
                                                                                               .tool_name = "read_file",
                                                                                               .input_json = "",
                                                                                               .is_error = false}}}},
          .tools_json = {},
          .stream = false},
      "anthropic-key");
  expect(!reversed_tool_results && reversed_tool_results.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "Anthropic request rejects native tool_results that do not follow tool_use order");
}

void test_anthropic_parsing()
{
  ava::provider::AnthropicProvider const budget_provider;
  ava::tests::test_provider_parser_budgets(
      budget_provider, [](std::string const& text) { return R"({"content":[{"type":"text","text":")" + text + R"("}]})"; },
      [](std::string const& text) { return R"(data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":")" + text + R"("}})"; },
      {R"({"type":"message_start","message":{"content":)" + ava::tests::provider_budget_array(R"({"type":"text","text":"x"})") + R"(},"content":)" +
       ava::tests::provider_budget_array(R"({"type":"text","text":"x"})") + "}"},
      R"(data: {"type":"ping"})");
  ava::tests::test_provider_tool_fragment_budget(
      budget_provider,
      "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"budget_call\",\"name\":\"read_file\"}}\n\n",
      [](std::string const& text) {
        return R"(data: {"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":")" + text + R"("}})";
      },
      "data: {\"type\":\"content_block_stop\",\"index\":0}\n\ndata: {\"type\":\"message_stop\"}\n\n");
  for (bool const redacted : {false, true})
  {
    ava::provider::AnthropicStreamParser opaque_parser;
    bool prefix_accepted = true;
    for (int index = 0; index < 4; ++index)
    {
      auto const events = opaque_parser.append("data: {\"type\":\"content_block_start\",\"index\":" + std::to_string(index) + ",\"content_block\":" +
                                               (redacted ? R"({"type":"redacted_thinking","data":")" : R"({"type":"thinking","signature":")") +
                                               std::string(64U * 1024U, 'x') + "\"}}\n\n");
      prefix_accepted = prefix_accepted && events && events->size() == 1 && events->front().type == ava::provider::StreamEventType::ReasoningStart;
    }
    auto const overflow = opaque_parser.append(std::string("data: {\"type\":\"content_block_start\",\"index\":4,\"content_block\":") +
                                               (redacted ? R"({"type":"redacted_thinking","data":")" : R"({"type":"thinking","signature":")") + "x\"}}\n\n");
    auto const ignored = opaque_parser.append("data: {\"type\":\"message_stop\"}\n\n");
    auto const final = opaque_parser.finish();
    expect(prefix_accepted && overflow && overflow->size() == 1 && overflow->front().type == ava::provider::StreamEventType::Error &&
               overflow->front().error_message == "Anthropic response parser limit exceeded" && ignored && ignored->empty() && final && final->empty(),
           "Anthropic charges opaque reasoning across open blocks before retaining un-emitted signatures or redacted bytes");
  }
  std::string const sse =
      "event: message_start\n"
      "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":10,"
      "\"cache_read_input_tokens\":2}}}\n\n"
      "event: content_block_start\n"
      "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
      "event: content_block_delta\n"
      "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
      "event: content_block_start\n"
      "data: "
      "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\","
      "\"name\":\"read_file\",\"input\":{}}}\n\n"
      "event: content_block_delta\n"
      "data: "
      "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{"
      "\\\"path\\\":\"}}\n\n"
      "event: content_block_delta\n"
      "data: "
      "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":"
      "\"\\\"README.md\\\"}\"}}\n\n"
      "event: content_block_stop\n"
      "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
      "event: message_delta\n"
      "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":7}}\n\n"
      "event: message_stop\n"
      "data: {\"type\":\"message_stop\"}\n\n";
  auto events = ava::provider::parse_anthropic_sse(sse);
  expect(events.has_value(), "Anthropic SSE parses");
  if (events)
  {
    expect(events->size() == 6, "Anthropic SSE produces text, tool, and done events");
    expect((*events)[0].type == ava::provider::StreamEventType::TextDelta && (*events)[0].text == "hi", "Anthropic SSE text delta parses");
    expect(
        (*events)[1].type == ava::provider::StreamEventType::ToolCallStart && (*events)[1].tool_call_id == "toolu_1" && (*events)[1].tool_name == "read_file",
        "Anthropic SSE tool start parses");
    expect((*events)[2].type == ava::provider::StreamEventType::ToolCallDelta && (*events)[2].tool_call_id == "toolu_1" &&
               (*events)[2].text.find("path") != std::string::npos && (*events)[3].type == ava::provider::StreamEventType::ToolCallDelta &&
               (*events)[3].tool_call_id == "toolu_1" && (*events)[3].text.find("README.md") != std::string::npos,
           "Anthropic SSE tool input deltas parse");
    expect((*events)[4].type == ava::provider::StreamEventType::ToolCallEnd && (*events)[4].tool_call_id == "toolu_1", "Anthropic SSE tool stop parses");
    expect((*events)[5].type == ava::provider::StreamEventType::Done && (*events)[5].usage && (*events)[5].usage->input_tokens == 12 &&
               (*events)[5].usage->output_tokens == 7 && (*events)[5].usage->cache_read_tokens == 2 && (*events)[5].usage->total_tokens == 19 &&
               (*events)[5].finish_reason == ava::provider::ProviderFinishReason::ToolCalls,
           "Anthropic SSE usage and stop reason accumulate from message_delta");
  }

  std::string const reasoning_sse =
      "event: content_block_start\n"
      "data: "
      "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
      "event: content_block_delta\n"
      "data: "
      "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"plan\"}}\n\n"
      "event: content_block_delta\n"
      "data: "
      "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"sig-\"}}"
      "\n\n"
      "event: content_block_delta\n"
      "data: "
      "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"1\"}}\n\n"
      "event: content_block_stop\n"
      "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
      "event: content_block_start\n"
      "data: "
      "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"redacted_thinking\",\"data\":"
      "\"opaque-redacted\"}}\n\n"
      "event: content_block_stop\n"
      "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
      "event: message_stop\n"
      "data: {\"type\":\"message_stop\"}\n\n";
  auto reasoning_events = ava::provider::parse_anthropic_sse(reasoning_sse);
  expect(reasoning_events.has_value(), "Anthropic SSE parses reasoning blocks");
  if (reasoning_events)
  {
    expect(reasoning_events->size() == 6, "Anthropic SSE produces reasoning lifecycle and done events");
    expect((*reasoning_events)[0].type == ava::provider::StreamEventType::ReasoningStart && (*reasoning_events)[0].reasoning_format == "anthropic_thinking",
           "Anthropic SSE thinking block start becomes reasoning_start");
    expect((*reasoning_events)[1].type == ava::provider::StreamEventType::ReasoningDelta && (*reasoning_events)[1].text == "plan",
           "Anthropic SSE thinking_delta becomes reasoning_delta");
    expect((*reasoning_events)[2].type == ava::provider::StreamEventType::ReasoningEnd && (*reasoning_events)[2].reasoning_signature == "sig-1" &&
               !(*reasoning_events)[2].redacted,
           "Anthropic SSE signature_delta is preserved on reasoning_end");
    expect((*reasoning_events)[3].type == ava::provider::StreamEventType::ReasoningStart && (*reasoning_events)[3].redacted,
           "Anthropic SSE redacted_thinking emits a redacted reasoning_start");
    expect((*reasoning_events)[4].type == ava::provider::StreamEventType::ReasoningEnd && (*reasoning_events)[4].redacted &&
               (*reasoning_events)[4].reasoning_redacted_data == "opaque-redacted" && (*reasoning_events)[4].text.empty(),
           "Anthropic SSE redacted_thinking keeps opaque data provider-private");
    expect((*reasoning_events)[5].type == ava::provider::StreamEventType::Done, "Anthropic SSE reasoning stream ends with done event");
  }

  std::string const oversized_signature(70 * 1024, 's');
  auto oversized_initial_signature = ava::provider::parse_anthropic_sse(
      "event: content_block_start\n"
      "data: "
      "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\","
      "\"signature\":\"" +
      oversized_signature + "\"}}\n\n");
  expect(oversized_initial_signature && oversized_initial_signature->size() == 1 &&
             (*oversized_initial_signature)[0].type == ava::provider::StreamEventType::Error &&
             (*oversized_initial_signature)[0].error_message.find("signature exceeded") != std::string::npos,
         "Anthropic SSE parser rejects oversized initial thinking signatures");

  auto oversized_signature_events = ava::provider::parse_anthropic_sse(
      "event: content_block_start\n"
      "data: "
      "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
      "event: content_block_delta\n"
      "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"" +
      oversized_signature + "\"}}\n\n");
  expect(oversized_signature_events && oversized_signature_events->size() == 2 &&
             (*oversized_signature_events)[1].type == ava::provider::StreamEventType::Error &&
             (*oversized_signature_events)[1].error_message.find("signature exceeded") != std::string::npos,
         "Anthropic SSE parser rejects oversized thinking signatures");

  ava::provider::AnthropicStreamParser parser;
  auto first_chunk = parser.append("event: content_block_delta\n");
  auto second_chunk = parser.append("data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"split\"}}\n");
  auto third_chunk = parser.append("\n");
  expect(first_chunk && first_chunk->empty() && second_chunk && second_chunk->empty() && third_chunk && third_chunk->size() == 1 &&
             (*third_chunk)[0].text == "split",
         "Anthropic incremental SSE parser buffers split frames");

  auto truncated = parser.finish();
  expect(truncated && truncated->size() == 1 && (*truncated)[0].type == ava::provider::StreamEventType::Error &&
             (*truncated)[0].error_message.find("message_stop") != std::string::npos,
         "Anthropic incremental SSE parser reports truncated streams without message_stop");

  auto malformed = ava::provider::parse_anthropic_sse("data: not-json\n\n");
  expect(malformed && malformed->size() == 1 && (*malformed)[0].type == ava::provider::StreamEventType::Error,
         "Anthropic malformed SSE data produces a provider error event");
  for (auto const terminal : {R"({"type":"message_stop"})", R"({"type":"error"})", "not-json"})
  {
    ava::provider::AnthropicStreamParser terminal_parser;
    auto const first = terminal_parser.append(std::string("data: ") + terminal + "\n\n");
    auto const trailing = terminal_parser.append(
        "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"after terminal\"}}\n\n"
        "data: {\"type\":\"message_stop\"}\n\n");
    auto const final = terminal_parser.finish();
    expect(first && first->size() == 1 && trailing && trailing->empty() && final && final->empty(),
           "Anthropic SSE ignores every record after completion or the first parser error");
  }
  auto private_error = ava::provider::parse_anthropic_sse(
      "data: {\"type\":\"error\",\"error\":{\"message\":\"event: error\\ndata: ANTHROPIC_SSE_MESSAGE_CANARY\","
      "\"signature\":\"ANTHROPIC_SSE_SIGNATURE_CANARY\",\"native\":{\"reasoning\":\"ANTHROPIC_SSE_REASONING_CANARY\"}},"
      "\"unknown\":\"ANTHROPIC_SSE_OUTER_CANARY\"}\n\n");
  expect(private_error && private_error->size() == 1 && (*private_error)[0].type == ava::provider::StreamEventType::Error &&
             (*private_error)[0].error_message == "Anthropic provider reported a streaming error" &&
             (*private_error)[0].error_message.find("ANTHROPIC_SSE_MESSAGE_CANARY") == std::string::npos &&
             (*private_error)[0].error_message.find("ANTHROPIC_SSE_SIGNATURE_CANARY") == std::string::npos &&
             (*private_error)[0].error_message.find("ANTHROPIC_SSE_REASONING_CANARY") == std::string::npos &&
             (*private_error)[0].error_message.find("ANTHROPIC_SSE_OUTER_CANARY") == std::string::npos,
         "Anthropic SSE error diagnostics expose only a normalized provider message and reject embedded event-stream payloads");

  auto non_stream = ava::provider::parse_anthropic_response(ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body =
          R"({"content":[{"type":"text","text":"hello"},{"type":"tool_use","id":"toolu_2","name":"grep","input":{"pattern":"needle"}}],"stop_reason":"end_turn","usage":{"input_tokens":3,"output_tokens":4,"cache_creation_input_tokens":1}})"});
  expect(non_stream && non_stream->size() == 5 && (*non_stream)[0].text == "hello" && (*non_stream)[1].tool_name == "grep" &&
             (*non_stream)[2].text.find("needle") != std::string::npos && (*non_stream)[4].usage && (*non_stream)[4].usage->input_tokens == 4 &&
             (*non_stream)[4].usage->total_tokens == 8 && (*non_stream)[4].usage->cache_write_tokens == 1 &&
             (*non_stream)[4].finish_reason == ava::provider::ProviderFinishReason::Completed,
         "Anthropic non-stream response parses text, tools, usage, and stop reason");
  auto saturated_usage = ava::provider::parse_anthropic_response(ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body =
          R"({"content":[{"type":"text","text":"bounded"}],"stop_reason":"end_turn","usage":{"input_tokens":9223372036854775807,"output_tokens":9,"cache_read_input_tokens":7,"cache_creation_input_tokens":5}})"});
  expect(saturated_usage && saturated_usage->back().usage && saturated_usage->back().usage->input_tokens == 9223372036854775807LL &&
             saturated_usage->back().usage->total_tokens == 9223372036854775807LL,
         "Anthropic usage parsing saturates near-LLONG_MAX input and total token arithmetic");

  auto non_stream_refusal = ava::provider::parse_anthropic_response(ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = R"({"content":[{"type":"refusal","refusal":"I can't help with that."}],"stop_reason":"refusal","usage":{"input_tokens":1,"output_tokens":2}})"});
  expect(non_stream_refusal && non_stream_refusal->size() == 2 && (*non_stream_refusal)[0].text.find("help") != std::string::npos &&
             (*non_stream_refusal)[1].type == ava::provider::StreamEventType::Done &&
             (*non_stream_refusal)[1].finish_reason == ava::provider::ProviderFinishReason::Refusal,
         "Anthropic non-stream response parses refusal content and stop reason");

  auto non_stream_refusal_details = ava::provider::parse_anthropic_response(ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body =
          R"({"content":[],"stop_reason":"refusal","stop_details":{"type":"refusal","explanation":"Refusal from stop details."},"usage":{"input_tokens":1,"output_tokens":2}})"});
  expect(non_stream_refusal_details && non_stream_refusal_details->size() == 2 &&
             (*non_stream_refusal_details)[0].text.find("stop details") != std::string::npos &&
             (*non_stream_refusal_details)[1].type == ava::provider::StreamEventType::Done &&
             (*non_stream_refusal_details)[1].finish_reason == ava::provider::ProviderFinishReason::Refusal,
         "Anthropic non-stream response accepts refusal stop_details without content blocks");

  auto non_stream_refusal_empty_details = ava::provider::parse_anthropic_response(ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = R"({"content":[],"stop_reason":"refusal","stop_details":{"type":"refusal","explanation":null},"usage":{"input_tokens":1,"output_tokens":2}})"});
  expect(non_stream_refusal_empty_details && non_stream_refusal_empty_details->size() == 1 &&
             (*non_stream_refusal_empty_details)[0].type == ava::provider::StreamEventType::Done &&
             (*non_stream_refusal_empty_details)[0].finish_reason == ava::provider::ProviderFinishReason::Refusal,
         "Anthropic non-stream response accepts refusal stop_details with nullable explanation");

  auto streaming_refusal_details = ava::provider::parse_anthropic_sse(
      "event: message_delta\n"
      "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"refusal\",\"stop_details\":{"
      "\"type\":\"refusal\",\"explanation\":\"Streaming refusal details.\"}}}\n\n"
      "event: message_stop\n"
      "data: {\"type\":\"message_stop\"}\n\n");
  expect(streaming_refusal_details && streaming_refusal_details->size() == 2 &&
             (*streaming_refusal_details)[0].text.find("Streaming refusal") != std::string::npos &&
             (*streaming_refusal_details)[1].type == ava::provider::StreamEventType::Done &&
             (*streaming_refusal_details)[1].finish_reason == ava::provider::ProviderFinishReason::Refusal,
         "Anthropic SSE emits refusal stop_details as visible text");

  auto pause_turn = ava::provider::parse_anthropic_sse(
      "event: message_delta\n"
      "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"pause_turn\"}}\n\n"
      "event: message_stop\n"
      "data: {\"type\":\"message_stop\"}\n\n");
  expect(pause_turn && pause_turn->size() == 1 && (*pause_turn)[0].type == ava::provider::StreamEventType::Done &&
             (*pause_turn)[0].finish_reason == ava::provider::ProviderFinishReason::Error,
         "Anthropic SSE preserves pause_turn stop reason");

  auto non_stream_reasoning = ava::provider::parse_anthropic_response(ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body =
          R"({"content":[{"type":"thinking","thinking":"non-stream plan","signature":"sig-ns"},{"type":"redacted_thinking","data":"ns-redacted"},{"type":"text","text":"answer"}],"usage":{"input_tokens":3,"output_tokens":4}})"});
  expect(non_stream_reasoning && non_stream_reasoning->size() == 7 && (*non_stream_reasoning)[0].type == ava::provider::StreamEventType::ReasoningStart &&
             (*non_stream_reasoning)[1].type == ava::provider::StreamEventType::ReasoningDelta && (*non_stream_reasoning)[1].text == "non-stream plan" &&
             (*non_stream_reasoning)[2].type == ava::provider::StreamEventType::ReasoningEnd && (*non_stream_reasoning)[2].reasoning_signature == "sig-ns" &&
             (*non_stream_reasoning)[4].type == ava::provider::StreamEventType::ReasoningEnd && (*non_stream_reasoning)[4].redacted &&
             (*non_stream_reasoning)[4].reasoning_redacted_data == "ns-redacted" &&
             (*non_stream_reasoning)[5].type == ava::provider::StreamEventType::TextDelta &&
             (*non_stream_reasoning)[6].type == ava::provider::StreamEventType::Done,
         "Anthropic non-stream response parses thinking and redacted thinking blocks");

  std::string const oversized_redacted_data(70 * 1024, 'r');
  auto oversized_redacted = ava::provider::parse_anthropic_response(ava::http::HttpResponse{
      .status_code = 200, .headers = {}, .body = R"({"content":[{"type":"redacted_thinking","data":")" + oversized_redacted_data + R"("}]})"});
  expect(!oversized_redacted, "Anthropic non-stream response rejects oversized redacted thinking payloads");

  auto malformed_success = ava::provider::parse_anthropic_response(ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = R"({"content":[]})"});
  expect(!malformed_success, "Anthropic non-stream success without content is rejected");

  auto http_error = ava::provider::parse_anthropic_sse_response(ava::http::HttpResponse{
      .status_code = 529,
      .headers = {},
      .body =
          R"({"error":{"type":"overloaded_error","message":"Overloaded","api_key":"ANTHROPIC_HTTP_KEY_CANARY","signature":"ANTHROPIC_HTTP_SIGNATURE_CANARY","redacted_data":"ANTHROPIC_HTTP_REDACTED_CANARY","data":"ANTHROPIC_HTTP_DATA_CANARY","thinking":"ANTHROPIC_HTTP_THINKING_CANARY","unknown":{"private":"ANTHROPIC_HTTP_NESTED_CANARY"}},"unknown":"ANTHROPIC_HTTP_OUTER_CANARY"})"});
  auto const error_text = http_error ? std::string{} : http_error.error().format();
  expect(!http_error && error_text.find("provider_error_kind: transient") != std::string::npos && error_text.find("provider_message") == std::string::npos &&
             error_text.find("body_snippet") == std::string::npos && error_text.find("ANTHROPIC_HTTP_KEY_CANARY") == std::string::npos &&
             error_text.find("ANTHROPIC_HTTP_SIGNATURE_CANARY") == std::string::npos &&
             error_text.find("ANTHROPIC_HTTP_REDACTED_CANARY") == std::string::npos && error_text.find("ANTHROPIC_HTTP_DATA_CANARY") == std::string::npos &&
             error_text.find("ANTHROPIC_HTTP_THINKING_CANARY") == std::string::npos && error_text.find("ANTHROPIC_HTTP_NESTED_CANARY") == std::string::npos &&
             error_text.find("ANTHROPIC_HTTP_OUTER_CANARY") == std::string::npos,
         "Anthropic HTTP errors carry only fixed local status and classification diagnostics");
}

void test_anthropic_registry_and_env_auth()
{
  auto registry = ava::provider::builtin_provider_registry();
  expect(registry.contains("anthropic"), "builtin provider registry contains Anthropic");
  auto provider = registry.create("anthropic");
  expect(provider.has_value() && *provider, "builtin provider registry creates Anthropic provider");

  auto const root = create_empty_root("anthropic-auth");

  auto const config_home = root / "config";
  auto const state_home = root / "state";
  auto const data_home = root / "data";
  auto const ava_config = config_home / "ava";
  auto const ava_state = state_home / "ava";
  auto const paths = ava::config::XdgPaths{.config_home = config_home,
                                           .state_home = state_home,
                                           .data_home = data_home,
                                           .ava_config_dir = ava_config,
                                           .ava_state_dir = ava_state,
                                           .auth_file = ava_config / "auth.json",
                                           .compaction_file = ava_config / "compaction.json",
                                           .global_agents_file = ava_config / "AGENTS.md",
                                           .models_file = ava_config / "models.json",
                                           .providers_file = ava_config / "providers.json",
                                           .prompts_dir = ava_config / "prompts",
                                           .sessions_dir = ava_state / "sessions"};
  ava::tests::FakeTransport transport({});
  ScopedEnvVar oauth_token("ANTHROPIC_OAUTH_TOKEN", "");
  ScopedEnvVar auth_token("ANTHROPIC_AUTH_TOKEN", "");
  ScopedEnvVar api_key("ANTHROPIC_API_KEY", "api-key-value");
  auto api_credential = ava::config::provider_credential_for_request(paths, "anthropic", transport);
  expect(api_credential && *api_credential && (*api_credential)->access_token == "api-key-value" && (*api_credential)->credential_type == "api_key" &&
             (*api_credential)->source == "env:ANTHROPIC_API_KEY",
         "Anthropic API key is discovered from environment");
}

void test_anthropic_agent_tool_loop_native_replay()
{
  auto const root = create_empty_root("anthropic-tool-loop");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::tests::write_test_file(workspace / "note.txt", "tool content");

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "anthropic-tool"});
  ava::provider::AnthropicProvider const provider("https://anthropic.example.test");
  ava::tests::FakeTransport transport({sse_response("event: content_block_start\n"
                                                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                                                    "\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"read_file\",\"input\":{}}}\n\n"
                                                    "event: content_block_delta\n"
                                                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                                                    "\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"note.txt\\\"}\"}}\n\n"
                                                    "event: content_block_stop\n"
                                                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                                                    "event: message_stop\n"
                                                    "data: {\"type\":\"message_stop\"}\n\n"),
                                       sse_response("event: content_block_start\n"
                                                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                                                    "\"type\":\"text\",\"text\":\"\"}}\n\n"
                                                    "event: content_block_delta\n"
                                                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                                                    "\"type\":\"text_delta\",\"text\":\"read it\"}}\n\n"
                                                    "event: message_stop\n"
                                                    "data: {\"type\":\"message_stop\"}\n\n")});

  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "anthropic",
                                                  .model_id = "claude-sonnet-4-5",
                                                  .system_prompt = "system prompt",
                                                  .api_family = "anthropic_messages",
                                                  .reasoning_format = "anthropic_thinking"},
      .access_token = "anthropic-key",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("read note", store, provider, transport);
  expect(result && result->final_text == "read it" && result->tool_calls == 1 && result->provider_iterations == 2,
         "Anthropic agent loop runs one native tool call then continues to final answer");
  expect(transport.requests().size() == 2, "Anthropic tool loop makes initial and continuation requests");
  if (transport.requests().size() != 2)
    return;
  auto const& continuation = transport.requests()[1].body;
  expect(continuation.find(R"("type":"tool_use","id":"toolu_1","name":"read_file")") != std::string::npos &&
             continuation.find(R"("input":{"path":"note.txt"})") != std::string::npos,
         "Anthropic continuation replays native tool_use block");
  expect(continuation.find(R"("type":"tool_result","tool_use_id":"toolu_1")") != std::string::npos && continuation.find("tool content") != std::string::npos,
         "Anthropic continuation sends native tool_result block with tool output");
  expect(continuation.find("Tool call requested by assistant") == std::string::npos && continuation.find("Tool result data only") == std::string::npos,
         "Anthropic continuation omits fallback tool replay text when native content parts are available");
}

void test_anthropic_agent_reasoning_native_replay()
{
  auto const root = create_empty_root("anthropic-reasoning-replay");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "anthropic-reasoning"});
  ava::provider::AnthropicProvider const provider("https://anthropic.example.test");
  ava::tests::FakeTransport transport({sse_response("event: content_block_start\n"
                                                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                                                    "\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
                                                    "event: content_block_delta\n"
                                                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                                                    "\"type\":\"thinking_delta\",\"thinking\":\"visible plan\"}}\n\n"
                                                    "event: content_block_delta\n"
                                                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                                                    "\"type\":\"signature_delta\",\"signature\":\"sig-1\"}}\n\n"
                                                    "event: content_block_stop\n"
                                                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                                                    "event: content_block_start\n"
                                                    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{"
                                                    "\"type\":\"text\",\"text\":\"\"}}\n\n"
                                                    "event: content_block_delta\n"
                                                    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{"
                                                    "\"type\":\"text_delta\",\"text\":\"first answer\"}}\n\n"
                                                    "event: message_stop\n"
                                                    "data: {\"type\":\"message_stop\"}\n\n"),
                                       sse_response("event: content_block_start\n"
                                                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                                                    "\"type\":\"text\",\"text\":\"\"}}\n\n"
                                                    "event: content_block_delta\n"
                                                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                                                    "\"type\":\"text_delta\",\"text\":\"second answer\"}}\n\n"
                                                    "event: message_stop\n"
                                                    "data: {\"type\":\"message_stop\"}\n\n")});

  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "anthropic",
                                                  .model_id = "claude-sonnet-4-5",
                                                  .system_prompt = "system prompt",
                                                  .api_family = "anthropic_messages",
                                                  .reasoning_format = "anthropic_thinking"},
      .access_token = "anthropic-key",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto first = loop.run_turn("first", store, provider, transport);
  expect(first && first->final_text == "first answer", "Anthropic agent loop stores first reasoning response");
  auto second = loop.run_turn("second", store, provider, transport);
  expect(second && second->final_text == "second answer", "Anthropic agent loop continues after reasoning response");
  expect(transport.requests().size() == 2, "Anthropic reasoning replay test makes two provider requests");
  if (transport.requests().size() != 2)
    return;
  auto const& continuation = transport.requests()[1].body;
  expect(continuation.find(R"("type":"thinking","thinking":"visible plan","signature":"sig-1")") != std::string::npos,
         "Anthropic continuation replays native thinking block with signature");
  expect(continuation.find(R"("type":"text","text":"first answer")") != std::string::npos, "Anthropic continuation keeps assistant text after thinking block");

  auto entries = store.load();
  expect(entries.has_value(), "Anthropic reasoning replay session loads");
  if (!entries)
    return;
  auto const projection = ava::session::classify_assistant_output(*entries);
  bool saw_reasoning = false;
  for (auto const& turn : projection.turns)
  {
    for (auto const& item : turn.items)
    {
      if (auto const* reasoning = std::get_if<ava::session::AssistantOutputReasoning>(&item.item.payload))
        saw_reasoning = saw_reasoning || (reasoning->text == "visible plan" && reasoning->signature == std::optional<std::string>{"sig-1"});
    }
  }
  expect(saw_reasoning, "Anthropic reasoning is persisted in its committed v4 turn for replay");
}

void test_anthropic_agent_redacted_reasoning_native_replay()
{
  auto const root = create_empty_root("anthropic-redacted-reasoning-replay");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "anthropic-redacted-reasoning"});
  ava::provider::AnthropicProvider const provider("https://anthropic.example.test");
  ava::tests::FakeTransport transport({sse_response("event: content_block_start\n"
                                                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                                                    "\"type\":\"redacted_thinking\",\"data\":\"opaque-redacted\"}}\n\n"
                                                    "event: content_block_stop\n"
                                                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                                                    "event: content_block_start\n"
                                                    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{"
                                                    "\"type\":\"text\",\"text\":\"\"}}\n\n"
                                                    "event: content_block_delta\n"
                                                    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{"
                                                    "\"type\":\"text_delta\",\"text\":\"first answer\"}}\n\n"
                                                    "event: message_stop\n"
                                                    "data: {\"type\":\"message_stop\"}\n\n"),
                                       sse_response("event: content_block_start\n"
                                                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                                                    "\"type\":\"text\",\"text\":\"\"}}\n\n"
                                                    "event: content_block_delta\n"
                                                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                                                    "\"type\":\"text_delta\",\"text\":\"second answer\"}}\n\n"
                                                    "event: message_stop\n"
                                                    "data: {\"type\":\"message_stop\"}\n\n")});

  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "anthropic",
                                                  .model_id = "claude-sonnet-4-5",
                                                  .system_prompt = "system prompt",
                                                  .api_family = "anthropic_messages",
                                                  .reasoning_format = "anthropic_thinking"},
      .access_token = "anthropic-key",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto first = loop.run_turn("first", store, provider, transport);
  expect(first && first->final_text == "first answer", "Anthropic agent loop stores redacted reasoning response");
  auto second = loop.run_turn("second", store, provider, transport);
  expect(second && second->final_text == "second answer", "Anthropic agent loop continues after redacted reasoning");
  expect(transport.requests().size() == 2, "Anthropic redacted reasoning replay test makes two provider requests");
  if (transport.requests().size() != 2)
    return;
  auto const& continuation = transport.requests()[1].body;
  expect(continuation.find(R"({"type":"redacted_thinking","data":"opaque-redacted"})") != std::string::npos,
         "Anthropic continuation replays native redacted thinking block");
  expect(continuation.find(R"("thinking":"opaque-redacted")") == std::string::npos && continuation.find(R"("text":"opaque-redacted")") == std::string::npos,
         "Anthropic continuation does not expose redacted thinking data as visible text");
}

void test_anthropic_agent_non_stream_reasoning_events()
{
  auto const root = create_empty_root("anthropic-non-stream-reasoning-events");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "anthropic-non-stream-reasoning"});
  ava::provider::AnthropicProvider const provider("https://anthropic.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body =
          R"({"content":[{"type":"thinking","thinking":"non-stream visible","signature":"sig-ns"},{"type":"text","text":"answer"}],"stop_reason":"end_turn"})"}});

  std::vector<ava::provider::StreamEvent> published_events;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "anthropic",
                                                  .model_id = "claude-sonnet-4-5",
                                                  .system_prompt = "system prompt",
                                                  .stream = false,
                                                  .api_family = "anthropic_messages",
                                                  .reasoning_format = "anthropic_thinking"},
      .access_token = "anthropic-key",
      .on_stream_event = [&](ava::provider::StreamEvent const& event) -> ava::core::VoidResult {
        published_events.push_back(event);
        return {};
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("first", store, provider, transport);
  expect(result && result->final_text == "answer" && result->outcome == ava::core::RuntimeTerminalOutcome::Completed,
         "Anthropic non-stream reasoning turn completes with normalized stop reason");
  auto const reasoning_end = std::find_if(published_events.begin(), published_events.end(),
                                          [](auto const& event) { return event.type == ava::provider::StreamEventType::ReasoningEnd; });
  expect(std::find_if(published_events.begin(), published_events.end(),
                      [](auto const& event) { return event.type == ava::provider::StreamEventType::ReasoningStart; }) != published_events.end() &&
             std::find_if(published_events.begin(), published_events.end(),
                          [](auto const& event) { return event.type == ava::provider::StreamEventType::ReasoningDelta; }) != published_events.end() &&
             reasoning_end != published_events.end(),
         "Anthropic non-stream reasoning events are published through the agent loop");
  expect(reasoning_end != published_events.end() && reasoning_end->reasoning_signature.empty() && reasoning_end->reasoning_redacted_data.empty() &&
             reasoning_end->reasoning_signature_present,
         "Anthropic stream callbacks receive sanitized reasoning provider-private state");
}

void test_anthropic_agent_multi_tool_native_replay()
{
  auto const root = create_empty_root("anthropic-multi-tool-loop");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::tests::write_test_file(workspace / "one.txt", "first content");
  ava::tests::write_test_file(workspace / "two.txt", "second content");

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "anthropic-multi-tool"});
  ava::provider::AnthropicProvider const provider("https://anthropic.example.test");
  ava::tests::FakeTransport transport({sse_response("event: content_block_start\n"
                                                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                                                    "\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"read_file\",\"input\":{}}}\n\n"
                                                    "event: content_block_delta\n"
                                                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                                                    "\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"one.txt\\\"}\"}}\n\n"
                                                    "event: content_block_stop\n"
                                                    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                                                    "event: content_block_start\n"
                                                    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{"
                                                    "\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"read_file\",\"input\":{}}}\n\n"
                                                    "event: content_block_delta\n"
                                                    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{"
                                                    "\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"two.txt\\\"}\"}}\n\n"
                                                    "event: content_block_stop\n"
                                                    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
                                                    "event: message_stop\n"
                                                    "data: {\"type\":\"message_stop\"}\n\n"),
                                       sse_response("event: content_block_start\n"
                                                    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                                                    "\"type\":\"text\",\"text\":\"\"}}\n\n"
                                                    "event: content_block_delta\n"
                                                    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                                                    "\"type\":\"text_delta\",\"text\":\"read both\"}}\n\n"
                                                    "event: message_stop\n"
                                                    "data: {\"type\":\"message_stop\"}\n\n")});

  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "anthropic",
                                                  .model_id = "claude-sonnet-4-5",
                                                  .system_prompt = "system prompt",
                                                  .api_family = "anthropic_messages",
                                                  .reasoning_format = "anthropic_thinking"},
      .access_token = "anthropic-key",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("read both notes", store, provider, transport);
  expect(result && result->final_text == "read both" && result->tool_calls == 2 && result->provider_iterations == 2,
         "Anthropic agent loop runs multiple native tool calls then continues");
  expect(transport.requests().size() == 2, "Anthropic multi-tool loop makes initial and continuation requests");
  if (transport.requests().size() != 2)
    return;
  auto const& continuation = transport.requests()[1].body;
  expect(continuation.find(R"("type":"tool_use","id":"toolu_1","name":"read_file")") != std::string::npos &&
             continuation.find(R"("input":{"path":"one.txt"})") != std::string::npos &&
             continuation.find(R"("type":"tool_result","tool_use_id":"toolu_1")") != std::string::npos &&
             continuation.find("first content") != std::string::npos,
         "Anthropic continuation replays first native tool pair");
  expect(continuation.find(R"("type":"tool_use","id":"toolu_2","name":"read_file")") != std::string::npos &&
             continuation.find(R"("input":{"path":"two.txt"})") != std::string::npos &&
             continuation.find(R"("type":"tool_result","tool_use_id":"toolu_2")") != std::string::npos &&
             continuation.find("second content") != std::string::npos,
         "Anthropic continuation replays second native tool pair");
  auto const first_use = continuation.find(R"("type":"tool_use","id":"toolu_1")");
  auto const second_use = continuation.find(R"("type":"tool_use","id":"toolu_2")");
  auto const first_result = continuation.find(R"("type":"tool_result","tool_use_id":"toolu_1")");
  auto const second_result = continuation.find(R"("type":"tool_result","tool_use_id":"toolu_2")");
  expect(first_use != std::string::npos && second_use != std::string::npos && first_result != std::string::npos && second_result != std::string::npos &&
             first_use < second_use && second_use < first_result && first_result < second_result,
         "Anthropic continuation batches parallel tool_use blocks before matching tool_result blocks");
}

void test_anthropic_agent_non_stream_tool_loop_native_replay()
{
  auto const root = create_empty_root("anthropic-non-stream-tool-loop");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::tests::write_test_file(workspace / "note.txt", "non-stream content");

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "anthropic-non-stream-tool"});
  ava::provider::AnthropicProvider const provider("https://anthropic.example.test");
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{
           .status_code = 200, .headers = {}, .body = R"({"content":[{"type":"tool_use","id":"toolu_1","name":"read_file","input":{"path":"note.txt"}}]})"},
       ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = R"({"content":[{"type":"text","text":"read non-stream"}]})"}});

  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "anthropic",
                                                  .model_id = "claude-sonnet-4-5",
                                                  .system_prompt = "system prompt",
                                                  .stream = false,
                                                  .api_family = "anthropic_messages",
                                                  .reasoning_format = "anthropic_thinking"},
      .access_token = "anthropic-key",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("read note", store, provider, transport);
  expect(result && result->final_text == "read non-stream" && result->tool_calls == 1 && result->provider_iterations == 2,
         "Anthropic non-stream agent loop runs native tool call then continues");
  expect(transport.requests().size() == 2, "Anthropic non-stream tool loop makes initial and continuation requests");
  if (transport.requests().size() != 2)
    return;
  auto const& continuation = transport.requests()[1].body;
  expect(continuation.find(R"("stream":false)") != std::string::npos, "Anthropic non-stream continuation preserves stream=false");
  expect(continuation.find(R"("type":"tool_use","id":"toolu_1","name":"read_file")") != std::string::npos &&
             continuation.find(R"("type":"tool_result","tool_use_id":"toolu_1")") != std::string::npos &&
             continuation.find("non-stream content") != std::string::npos,
         "Anthropic non-stream continuation replays native tool result");
}

}  // namespace

void run_provider_anthropic_tests()
{
  ava::tests::HermeticTestEnvironment environment("anthropic-suite-");
  test_json_object_validator();
  test_anthropic_provider_contract();
  test_anthropic_native_content_parts_request();
  test_anthropic_parsing();
  test_anthropic_registry_and_env_auth();
  test_anthropic_agent_tool_loop_native_replay();
  test_anthropic_agent_reasoning_native_replay();
  test_anthropic_agent_redacted_reasoning_native_replay();
  test_anthropic_agent_non_stream_reasoning_events();
  test_anthropic_agent_multi_tool_native_replay();
  test_anthropic_agent_non_stream_tool_loop_native_replay();
}
