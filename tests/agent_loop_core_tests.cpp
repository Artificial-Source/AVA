#include "sys.h"
#include "tests/agent_loop_test_declarations.h"
#include "tests/support/agent_loop_test_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/usage_accounting.h"
#include "ava/config/model_config.h"
#include "ava/session/assistant_output.h"
#include "ava/session/session_store.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/result.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using agent_loop_test::sse_response;

void test_usage_accounting_saturates_without_signed_overflow()
{
  ava::provider::TokenUsage ordinary_usage{};
  ordinary_usage.input_tokens = 17;
  ordinary_usage.output_tokens = 25;
  auto const ordinary = ava::agent::with_total_tokens(ordinary_usage);
  ava::provider::TokenUsage saturated_usage{};
  saturated_usage.input_tokens = LLONG_MAX - 3;
  saturated_usage.output_tokens = 10;
  auto const saturated = ava::agent::with_total_tokens(saturated_usage);
  ava::provider::TokenUsage aggregate_usage{};
  aggregate_usage.input_tokens = LLONG_MAX - 2;
  aggregate_usage.output_tokens = 10;
  aggregate_usage.estimated_total_bytes = LLONG_MAX - 1;
  std::optional<ava::provider::TokenUsage> aggregate = aggregate_usage;
  ava::provider::TokenUsage increment{};
  increment.input_tokens = 9;
  increment.output_tokens = 5;
  increment.estimated_total_bytes = 7;
  ava::agent::accumulate_usage(aggregate, increment);
  expect(ordinary.total_tokens && *ordinary.total_tokens == 42 && saturated.total_tokens && *saturated.total_tokens == LLONG_MAX && aggregate &&
             aggregate->input_tokens && *aggregate->input_tokens == LLONG_MAX && aggregate->output_tokens && *aggregate->output_tokens == 15 &&
             aggregate->estimated_total_bytes && *aggregate->estimated_total_bytes == LLONG_MAX,
         "usage accounting preserves ordinary sums and saturates every near-LLONG_MAX aggregate without signed overflow");
}

void test_agent_loop_text_only_turn()
{
  constexpr std::string_view system_prompt_marker = "AVA-ISSUE-54-AGENT-LOOP-SYSTEM-PROMPT";
  auto const root = create_empty_root("agent-text");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "text"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello user\"}\n\n"
                    "data: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "openai",
                                                  .model_id = "gpt-5.5",
                                                  .system_prompt = std::string(system_prompt_marker),
                                                  .api_family = "openai_responses",
                                                  .reasoning_format = "openai_responses"},
      .access_token = "token",
      .openai_oauth = true,
      .openai_account_id = "acct_123",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("hi", store, provider, transport);
  expect(result && result->final_text == "hello user" && result->tool_calls == 0 && result->initial_context_messages == 1 && !result->used_compacted_context &&
             result->tool_iterations == 0 && result->outcome == ava::core::RuntimeTerminalOutcome::Completed,
         "agent loop returns text-only provider response with status metadata");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("read_file") != std::string::npos,
         "agent loop includes tool schemas in provider request");
  expect(transport.requests().size() == 1 && ava::tests::count_occurrences(transport.requests()[0].body, system_prompt_marker) == 1,
         "agent loop sends the effective system prompt to the provider exactly once");
  expect(transport.requests().size() == 1 &&
             transport.requests()[0].body.find("\"instructions\":\"" + std::string(system_prompt_marker) + "\"") != std::string::npos,
         "agent loop places the effective system prompt in the OpenAI instructions field");
  expect(transport.requests().size() == 1 && transport.requests()[0].url == "https://chatgpt.com/backend-api/codex/responses" &&
             transport.requests()[0].headers.at("ChatGPT-Account-Id") == "acct_123",
         "agent loop routes OpenAI OAuth turns through delegated endpoint");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("\"store\":false") != std::string::npos,
         "agent loop disables response storage for OpenAI OAuth turns");
  auto entries = store.load();
  auto const projection = entries ? ava::session::classify_assistant_output(*entries) : ava::session::AssistantOutputProjection{};
  expect(entries && entries->size() == 3 && (*entries)[0].type == ava::session::EntryType::UserMessage && projection.turns.size() == 1 &&
             projection.turns.front().items.size() == 1 && projection.turns.front().commit.api_family == "openai_responses" &&
             projection.turns.front().commit.reasoning_format == "openai_responses" &&
             std::get<ava::session::AssistantOutputText>(projection.turns.front().items.front().item.payload).text == "hello user",
         "agent loop persists text-only provider output with explicit source API/reasoning provenance as one committed v4 assistant turn");
}

void test_agent_loop_uses_established_session_read_limits()
{
  auto const root = create_empty_root("agent-established-read-limits");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "established-read-limits"});

  for (std::size_t index = 0; index < 10; ++index)
  {
    auto appended = append_session_entry_for_test(store, ava::session::SessionEntry{.id = "large-history-" + std::to_string(index),
                                                                                    .parent_id = "",
                                                                                    .type = ava::session::EntryType::Error,
                                                                                    .timestamp = ava::session::now_timestamp(),
                                                                                    .data_json = "{\"message\":\"" + std::string(900U * 1024U, 'x') + "\"}"});
    expect(appended.has_value(), "large legacy-unbounded agent history fixture appends");
    if (!appended)
      return;
  }

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport legacy_transport(
      {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"legacy reads succeed\"}\n\ndata: [DONE]\n\n")});
  ava::agent::AgentLoop legacy_loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto legacy_result = legacy_loop.run_turn("read large legacy history", store, provider, legacy_transport);

  ava::tests::FakeTransport bounded_transport({});
  ava::agent::AgentLoop bounded_loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
      .session_read_limits = ava::session::SessionReadLimits{.max_file_bytes = 8U * 1024U * 1024U, .max_line_bytes = 1024U * 1024U, .max_entries = 64},
  });
  auto bounded_result = bounded_loop.run_turn("bounded read must fail", store, provider, bounded_transport);
  ava::tests::expect_all("AgentLoop preserves legacy history reads and enforces the explicit file-byte policy",
                         {{"legacy turn completes", legacy_result.has_value()},
                          {"one legacy request", legacy_transport.requests().size() == 1},
                          {"bounded session failure", !bounded_result && bounded_result.error().category() == ava::core::ErrorCategory::Session},
                          {"file-byte limit context", !bounded_result && std::ranges::any_of(bounded_result.error().context(),
                                                                                             [](auto const& field) { return field.key == "max_file_bytes"; })},
                          {"no bounded provider request", bounded_transport.requests().empty()}});
}

void test_agent_loop_authority_policy_applies_between_provider_iterations()
{
  auto const root = create_empty_root("agent-authority-policy-between-iterations");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  {
    std::ofstream file(workspace / "note.txt", std::ios::binary | std::ios::trunc);
    file << "tool content";
  }

  auto store = ava::session::SessionStore::create_ephemeral(workspace);
  auto const limits = ava::session::SessionReadLimits{.max_file_bytes = 4096, .max_line_bytes = 2048, .max_entries = 1};
  auto target = store ? ava::session::SessionAppendTarget::create_ephemeral(*store, limits)
                      : ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>>(std::unexpected(store.error()));
  auto authority = target ? (*target)->read_authority() : ava::core::Result<ava::session::SessionReadAuthority>(std::unexpected(target.error()));
  if (!store || !target || !authority)
  {
    expect(false, "policy-bound provider-iteration fixture creates its ephemeral append and read authorities");
    return;
  }

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response("data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"fc_policy\",\"type\":\"function_call\","
                    "\"call_id\":\"call_policy\",\"name\":\"read_file\",\"arguments\":\"\"}}\n\n"
                    "data: {\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_policy\",\"delta\":\"{\\\"path\\\":\\\"note.txt\\\"}\"}\n\n"
                    "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"fc_policy\",\"type\":\"function_call\","
                    "\"call_id\":\"call_policy\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"note.txt\\\"}\"}}\n\n"
                    "data: {\"type\":\"response.completed\",\"response\":{}}\n\n")});
  auto append_target = *target;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .append_entry = [append_target](ava::session::SessionEntry const& entry) { return append_target->append(entry); },
      .append_batch = [append_target](std::vector<ava::session::SessionEntry> entries) { return append_target->append_batch(std::move(entries)); },
      .session_read_authority = std::move(*authority),
  });
  auto result = loop.run_turn("read note", *store, provider, transport);
  auto const grown_history = store->load();
  ava::tests::expect_all("bound read authority rejects grown history before the second provider iteration",
                         {{"typed session failure", !result && result.error().category() == ava::core::ErrorCategory::Session},
                          // Ephemeral bounded reads expose no numeric context or
                          // distinguishing error code for their entry-count limit.
                          {"entry-count policy rejected", !result && result.error().message().find("entry count") != std::string::npos},
                          {"history grew beyond the bound", grown_history && grown_history->size() > limits.max_entries},
                          {"only first provider request", transport.requests().size() == 1}});
}

void test_agent_loop_model_capability_gating()
{
  auto const root = create_empty_root("agent-capabilities");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "capabilities"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"status\":\"completed\",\"output_text\":\"plain\"}"}});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "openai",
                                                  .model_id = "text-only-model",
                                                  .system_prompt = "system prompt",
                                                  .stream = true,
                                                  .supports_tools = false,
                                                  .supports_streaming = false},
      .access_token = "token",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("hi", store, provider, transport);
  expect(result && result->final_text == "plain", "agent loop accepts text-only model response");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("read_file") == std::string::npos &&
             transport.requests()[0].body.find("write_file") == std::string::npos,
         "agent loop omits tool definitions for models without tool support");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("\"stream\":false") != std::string::npos,
         "agent loop disables streaming for models without streaming support");
}

void test_agent_loop_rejects_store_without_append_routes()
{
  auto const root = create_empty_root("agent-missing-append-route");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "missing-route"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace, .mode = ava::agent::Mode::Build, .model = agent_loop_test::model_invocation_options(), .access_token = "token"});
  auto result = loop.run_turn("hi", store, provider, transport);
  ava::tests::expect_all("missing append routes fail before provider work or session mutation",
                         {{"invalid configuration", !result && result.error().category() == ava::core::ErrorCategory::InvalidArgument},
                          {"no provider request", transport.requests().empty()},
                          {"no session file", !std::filesystem::exists(store.session_path())}});
}

void test_agent_loop_rejects_ephemeral_store_missing_entry_or_batch_route()
{
  auto const root = create_empty_root("agent-ephemeral-missing-append-route");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto store = ava::session::SessionStore::create_ephemeral(workspace);
  expect(store.has_value(), "ephemeral missing-route fixture creates a store");
  if (!store)
    return;
  auto target = ava::session::SessionAppendTarget::create_ephemeral(*store);
  expect(target.has_value(), "ephemeral missing-route fixture creates an append target");
  if (!target)
    return;
  auto authority = (*target)->read_authority();
  expect(authority.has_value(), "ephemeral missing-route fixture creates a read authority");
  if (!authority)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport missing_entry_transport({});
  auto append_target = *target;
  ava::agent::AgentLoop missing_entry(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .append_batch = [append_target](std::vector<ava::session::SessionEntry> entries) { return append_target->append_batch(std::move(entries)); },
      .session_read_authority = *authority,
  });
  auto missing_entry_result = missing_entry.run_turn("hi", *store, provider, missing_entry_transport);
  auto const after_missing_entry = store->load();
  ava::tests::expect_all(
      "missing ephemeral entry route fails before effects",
      {{"invalid configuration", !missing_entry_result && missing_entry_result.error().category() == ava::core::ErrorCategory::InvalidArgument},
       {"no provider request", missing_entry_transport.requests().empty()},
       {"empty history", after_missing_entry && after_missing_entry->empty()}});

  ava::tests::FakeTransport missing_batch_transport({});
  ava::agent::AgentLoop missing_batch(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .append_entry = [append_target](ava::session::SessionEntry const& entry) { return append_target->append(entry); },
      .session_read_authority = *authority,
  });
  auto missing_batch_result = missing_batch.run_turn("hi", *store, provider, missing_batch_transport);
  auto const after_missing_batch = store->load();
  ava::tests::expect_all(
      "missing ephemeral batch route fails before effects",
      {{"invalid configuration", !missing_batch_result && missing_batch_result.error().category() == ava::core::ErrorCategory::InvalidArgument},
       {"no provider request", missing_batch_transport.requests().empty()},
       {"empty history", after_missing_batch && after_missing_batch->empty()}});
}

void test_agent_loop_rejects_replaced_history_before_provider_use()
{
  auto const root = create_empty_root("agent-history-path-replacement");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "history-replacement"});

  auto replacement = ava::session::serialize_session_entry_line(ava::session::SessionEntry{.id = "replacement",
                                                                                           .parent_id = "",
                                                                                           .type = ava::session::EntryType::UserMessage,
                                                                                           .timestamp = "2026-05-08T00:00:00Z",
                                                                                           .data_json = "{\"text\":\"REPLACEMENT_CANARY\"}"});
  expect(replacement.has_value(), "agent replacement fixture serializes a valid session record");
  if (!replacement)
    return;
  bool replaced = false;
  store.set_after_lease_bound_read_for_test([&] {
    if (replaced)
      return;
    replaced = true;
    std::filesystem::rename(store.session_path(), store.session_path().string() + ".parked");
    std::ofstream file(store.session_path(), std::ios::binary | std::ios::trunc);
    file << *replacement << '\n';
  });

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"must not run\"}\n\ndata: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("do not mix history", store, provider, transport);
  store.set_after_lease_bound_read_for_test({});
  auto pathname_entries = store.load();
  expect(replaced && !result && transport.requests().empty() && pathname_entries && pathname_entries->size() == 1 &&
             pathname_entries->front().data_json.find("REPLACEMENT_CANARY") != std::string::npos,
         "provider history fails closed after authority binding when the live session pathname is replaced and never consumes replacement content");
}

void test_agent_loop_image_attachment_load_failure_records_error()
{
  auto const root = create_empty_root("agent-image-load-failure");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "image-load-failure"});
  expect(
      append_session_entry_for_test(
          store,
          ava::session::SessionEntry{
              .id = "prior_image",
              .parent_id = "",
              .type = ava::session::EntryType::UserMessage,
              .timestamp = "2026-05-08T00:00:00Z",
              .data_json =
                  R"({"text":"look","attachments":[{"id":"img_missing","type":"image","mime_type":"image/png","byte_size":5,"sha256":"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824","storage_path":"attachments/missing.png"}]})"})
          .has_value(),
      "agent loop image load failure test appends prior image message");

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"should not call\"}\n\n"
                    "data: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model =
          ava::agent::ModelInvocationOptions{
              .provider_id = "openai", .model_id = "gpt-image", .system_prompt = "system prompt", .input_modalities = {"text", "image"}},
      .access_token = "token",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("continue", store, provider, transport);
  expect(!result && result.error().message().find("image attachment") != std::string::npos, "agent loop returns attachment load errors before provider calls");
  expect(transport.requests().empty(), "agent loop does not call provider when attachment loading fails");
  auto entries = store.load();
  expect(entries && std::ranges::any_of(*entries,
                                        [](ava::session::SessionEntry const& entry) {
                                          return entry.type == ava::session::EntryType::Error && entry.data_json.find("image attachment") != std::string::npos;
                                        }),
         "agent loop records image attachment load errors in the session");
}

void test_agent_loop_usage_and_cost_persistence()
{
  auto const root = create_empty_root("agent-usage");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::config::ModelPricing const pricing{.input_per_million = 10.0L,
                                          .output_per_million = 20.0L,
                                          .cache_read_per_million = std::nullopt,
                                          .cache_write_per_million = std::nullopt,
                                          .reasoning_per_million = std::nullopt};

  ava::session::SessionStore exact_store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "exact"});
  ava::tests::FakeTransport exact_transport(
      {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"priced\"}\n\n"
                    "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":1000,"
                    "\"output_tokens\":2000,\"total_tokens\":3000}}}\n\n")});
  ava::agent::AgentLoop exact_loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "openai", .model_id = "gpt-5.5", .system_prompt = "system prompt", .pricing = pricing},
      .access_token = "token",
      .append_entry = append_route_for_test(exact_store),
      .append_batch = append_batch_route_for_test(exact_store),
      .session_read_authority = read_authority_for_test(exact_store)});
  auto exact_result = exact_loop.run_turn("hi", exact_store, provider, exact_transport);
  expect(exact_result && exact_result->usage && !exact_result->usage->estimated && exact_result->cost_usd && *exact_result->cost_usd > 0.049L &&
             *exact_result->cost_usd < 0.051L,
         "agent loop calculates cost from provider usage when pricing is known");
  auto exact_entries = exact_store.load();
  auto const exact_projection = exact_entries ? ava::session::classify_assistant_output(*exact_entries) : ava::session::AssistantOutputProjection{};
  expect(exact_entries && exact_projection.turns.size() == 1 && exact_projection.turns.front().commit.usage_json &&
             exact_projection.turns.front().commit.usage_json->find("\"source\":\"provider\"") != std::string::npos &&
             exact_projection.turns.front().commit.usage_json->find("\"cost_usd\":0.05") != std::string::npos,
         "agent loop persists exact provider usage and known cost on the committed v4 assistant turn");

  ava::session::SessionStore unknown_price_store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "unknown-price"});
  ava::tests::FakeTransport unknown_price_transport(
      {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"unknown\"}\n\n"
                    "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":1,"
                    "\"output_tokens\":1,\"total_tokens\":2}}}\n\n")});
  ava::agent::AgentLoop unknown_price_loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "openai", .model_id = "unknown-model", .system_prompt = "system prompt"},
      .access_token = "token",
      .append_entry = append_route_for_test(unknown_price_store),
      .append_batch = append_batch_route_for_test(unknown_price_store),
      .session_read_authority = read_authority_for_test(unknown_price_store),
  });
  auto unknown_price_result = unknown_price_loop.run_turn("hi", unknown_price_store, provider, unknown_price_transport);
  auto unknown_price_entries = unknown_price_store.load();
  auto const unknown_price_projection =
      unknown_price_entries ? ava::session::classify_assistant_output(*unknown_price_entries) : ava::session::AssistantOutputProjection{};
  expect(unknown_price_result && unknown_price_projection.turns.size() == 1 && unknown_price_projection.turns.front().commit.usage_json &&
             unknown_price_projection.turns.front().commit.usage_json->find("cost_usd") == std::string::npos,
         "agent loop does not persist fake cost when model pricing is unknown");

  ava::session::SessionStore estimated_store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "estimated"});
  ava::tests::FakeTransport estimated_transport(
      {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"estimated\"}\n\n"
                    "data: [DONE]\n\n")});
  ava::agent::AgentLoop estimated_loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "openai", .model_id = "gpt-5.5", .system_prompt = "system prompt", .pricing = pricing},
      .access_token = "token",
      .append_entry = append_route_for_test(estimated_store),
      .append_batch = append_batch_route_for_test(estimated_store),
      .session_read_authority = read_authority_for_test(estimated_store)});
  auto estimated_result = estimated_loop.run_turn("hi", estimated_store, provider, estimated_transport);
  auto estimated_entries = estimated_store.load();
  auto const estimated_projection = estimated_entries ? ava::session::classify_assistant_output(*estimated_entries) : ava::session::AssistantOutputProjection{};
  auto const estimated_usage = estimated_projection.turns.empty() ? std::optional<std::string>{} : estimated_projection.turns.front().commit.usage_json;
  expect(estimated_result && estimated_result->usage && estimated_result->usage->estimated && estimated_usage &&
             estimated_usage->find("\"source\":\"estimated\"") != std::string::npos &&
             estimated_usage->find("\"estimation_method\":\"byte_count\"") != std::string::npos &&
             estimated_usage->find("\"estimated_input_bytes\":") != std::string::npos && estimated_usage->find("cost_usd") == std::string::npos,
         "agent loop estimates byte usage without persisting fake cost when provider usage is unavailable");
}

void test_agent_loop_tool_turn_and_continuation()
{
  auto const root = create_empty_root("agent-tool");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  {
    std::ofstream file(workspace / "note.txt", std::ios::binary | std::ios::trunc);
    file << "tool content";
  }
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "tool"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::config::ModelPricing const pricing{.input_per_million = 10.0L,
                                          .output_per_million = 20.0L,
                                          .cache_read_per_million = std::nullopt,
                                          .cache_write_per_million = std::nullopt,
                                          .reasoning_per_million = std::nullopt};
  auto const private_reasoning_item_json =
      R"({"id":"rs_tool","type":"reasoning","summary":[{"type":"summary_text","text":"inspect note"}],"status":"completed","encrypted_content":"ciphertext-tool"})";
  ava::tests::FakeTransport transport(
      {sse_response(std::string("data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"rs_tool\",\"type\":\"reasoning\"}}\n\n") +
                    "data: {\"type\":\"response.output_item.done\",\"item\":" + private_reasoning_item_json + "}\n\n" +
                    "data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"fc_1\",\"type\":\"function_call\","
                    "\"call_id\":\"call_1\",\"name\":\"read_file\",\"arguments\":\"\"}}\n\n"
                    "data: "
                    "{\"type\":\"response.function_call_arguments.delta\",\"item_id\":\"fc_1\",\"delta\":\"{\\\"path\\\":"
                    "\\\"note.txt\\\"}\"}\n\n"
                    "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"fc_1\",\"type\":\"function_call\","
                    "\"call_id\":\"call_1\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"note.txt\\\"}\"}}\n\n"
                    "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":10,"
                    "\"output_tokens\":2,\"total_tokens\":12}}}\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"read it\"}\n\n"
                    "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":5,"
                    "\"output_tokens\":3,\"total_tokens\":8}}}\n\n")});
  std::vector<ava::agent::ToolTimelineEntry> tool_events;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{.workspace_dir = workspace,
                                                          .mode = ava::agent::Mode::Build,
                                                          .model = ava::agent::ModelInvocationOptions{.provider_id = "openai",
                                                                                                      .model_id = "gpt-5.5",
                                                                                                      .system_prompt = "system prompt",
                                                                                                      .pricing = pricing,
                                                                                                      .api_family = "openai_responses",
                                                                                                      .reasoning_format = "openai_responses"},
                                                          .access_token = "token",
                                                          .on_tool_event = [&tool_events](auto const& entry) { tool_events.push_back(entry); },
                                                          .append_entry = append_route_for_test(store),
                                                          .append_batch = append_batch_route_for_test(store),
                                                          .session_read_authority = read_authority_for_test(store)});
  auto result = loop.run_turn("read note", store, provider, transport);
  expect(result && result->final_text == "read it" && result->tool_calls == 1 && result->provider_iterations == 2 && result->initial_context_messages == 1 &&
             result->tool_iterations == 1 && result->outcome == ava::core::RuntimeTerminalOutcome::Completed,
         "agent loop runs one sequential tool call then continues to final answer with status metadata");
  expect(transport.requests().size() == 2 && transport.requests()[1].body.find("tool content") != std::string::npos &&
             transport.requests()[1].body.find(private_reasoning_item_json) != std::string::npos &&
             transport.requests()[1].body.find(R"({"type":"function_call","id":"fc_1","call_id":"call_1","name":"read_file",)") != std::string::npos &&
             transport.requests()[1].body.find(R"({"type":"function_call_output","call_id":"call_1",)") != std::string::npos &&
             transport.requests()[1].body.find(private_reasoning_item_json) <
                 transport.requests()[1].body.find(R"({"type":"function_call","id":"fc_1","call_id":"call_1",)") &&
             transport.requests()[1].body.find(R"({"type":"function_call","id":"fc_1","call_id":"call_1",)") <
                 transport.requests()[1].body.find(R"({"type":"function_call_output","call_id":"call_1",)") &&
             transport.requests()[1].body.find("Tool call requested by assistant") == std::string::npos &&
             transport.requests()[1].body.find("Tool result data only") == std::string::npos,
         "agent loop continues OpenAI reasoning tool turns with exact native reasoning before function replay");
  expect(result && result->tool_timeline.size() == 1 && result->tool_timeline.front().status == ava::agent::ToolTimelineStatus::Success &&
             result->tool_timeline.front().name == "read_file" && result->tool_timeline.front().argument_summary.find("path=note.txt") != std::string::npos &&
             result->tool_timeline.front().argument_summary.find('{') == std::string::npos &&
             result->tool_timeline.front().result_summary.find("tool content") == std::string::npos &&
             result->tool_timeline.front().result_summary.find("lines") != std::string::npos && result->tool_timeline.front().output_lines &&
             *result->tool_timeline.front().output_lines == 1 &&
             result->tool_timeline.front().structured_result_json.find("\"status\":\"success\"") != std::string::npos &&
             result->tool_timeline.front().content_type == "application/json",
         "agent loop returns safe compact tool timeline summaries and structured result metadata");
  expect(tool_events.size() == 2 && tool_events.front().status == ava::agent::ToolTimelineStatus::Running &&
             tool_events.back().status == ava::agent::ToolTimelineStatus::Success,
         "agent loop publishes running and completed tool timeline events");
  expect(result && result->usage && !result->usage->estimated && result->usage->input_tokens == 15 && result->usage->output_tokens == 5 &&
             result->usage->total_tokens == 20 && result->cost_usd && *result->cost_usd > 0.00024L && *result->cost_usd < 0.00026L,
         "agent loop accumulates usage and cost across provider iterations");

  auto entries = store.load();
  expect(entries.has_value(), "agent tool turn session loads");
  if (!entries)
    return;
  auto const projection = ava::session::classify_assistant_output(*entries);
  bool saw_bound_function = false;
  bool saw_private_reasoning_item = false;
  bool saw_final_text = false;
  bool saw_bound_tool_result = false;
  for (auto const& turn : projection.turns)
  {
    for (auto const& item : turn.items)
    {
      if (auto const* function = std::get_if<ava::session::AssistantOutputFunctionCall>(&item.item.payload))
        saw_bound_function = saw_bound_function || (function->call_id == "call_1" && item.item.provider_item_id == std::optional<std::string>{"fc_1"});
      if (auto const* reasoning = std::get_if<ava::session::AssistantOutputReasoning>(&item.item.payload))
        saw_private_reasoning_item =
            saw_private_reasoning_item ||
            (reasoning->native_item_json && reasoning->native_item_json->find("\"encrypted_content\":\"ciphertext-tool\"") != std::string::npos);
      if (auto const* text = std::get_if<ava::session::AssistantOutputText>(&item.item.payload))
        saw_final_text = saw_final_text || text->text == "read it";
    }
  }
  for (auto const& entry : *entries)
  {
    saw_bound_tool_result = saw_bound_tool_result ||
                            (entry.type == ava::session::EntryType::ToolResult && entry.data_json.find("\"assistant_output_entry_id\":") != std::string::npos &&
                             entry.data_json.find("\"structured_result\":{\"schema_version\":1") != std::string::npos);
  }
  expect(saw_bound_function && saw_bound_tool_result && saw_private_reasoning_item && saw_final_text,
         "agent loop persists private reasoning, exact function identity, bound result, and final text in committed v4 turns");
}
