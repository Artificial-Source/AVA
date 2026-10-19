#include "sys.h"
#include "tests/session_test_declarations.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/session/assistant_output.h"
#include "ava/session/export.h"
#include "ava/session/logical_projection.h"
#include "ava/session/record.h"
#include "ava/session/Stats.h"
#include "ava/session/validation.h"
#include "ava/session/validation_fields.h"
#include "ava/core/json.h"
#include "ava/core/result.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace session_tests {
void test_assistant_output_v4_session_schema_and_replay()
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

  auto make_entry = [](std::string id, EntryType type, std::string data_json, long long version = 4) {
    return SessionEntry{
        .id = std::move(id), .parent_id = "", .type = type, .timestamp = "2026-07-18T00:00:00Z", .data_json = std::move(data_json), .version = version};
  };
  auto make_item = [&](std::string id, AssistantOutputItem item) {
    auto data = ava::session::serialize_assistant_output_item_data_json(item);
    expect(data.has_value(), "v4 assistant output item codec serializes a strict item variant");
    return make_entry(std::move(id), EntryType::AssistantOutputItem, data ? *data : "{}");
  };
  auto make_text = [&](std::string id, std::string turn_id, std::size_t sequence, std::string provider_item_id, std::size_t output_index,
                       std::string text = "text") {
    return make_item(std::move(id),
                     AssistantOutputItem{.assistant_turn_id = std::move(turn_id),
                                         .sequence = sequence,
                                         .kind = AssistantOutputItemKind::Text,
                                         .provider_item_id = std::move(provider_item_id),
                                         .provider_output_index = output_index,
                                         .payload = AssistantOutputText{.text = std::move(text), .assistant_phase = AssistantOutputTextPhase::Commentary}});
  };
  auto make_reasoning = [&](std::string id, std::string turn_id, std::size_t sequence, std::string provider_item_id, std::size_t output_index) {
    return make_item(std::move(id), AssistantOutputItem{.assistant_turn_id = std::move(turn_id),
                                                        .sequence = sequence,
                                                        .kind = AssistantOutputItemKind::Reasoning,
                                                        .provider_item_id = std::move(provider_item_id),
                                                        .provider_output_index = output_index,
                                                        .payload = AssistantOutputReasoning{
                                                            .text = "reasoning",
                                                            .format = "openai_responses",
                                                            .redacted = false,
                                                            .signature = "PRIVATE_SIGNATURE_CANARY",
                                                            .redacted_data = "PRIVATE_REDACTED_CANARY",
                                                            .native_item_json = "{\"id\":\"rs_schema\",\"type\":\"reasoning\",\"summary\":[]}"}});
  };
  auto make_function = [&](std::string id, std::string turn_id, std::size_t sequence, std::string provider_item_id, std::size_t output_index,
                           std::string call_id = "call_schema", std::string name = "read_file") {
    return make_item(std::move(id),
                     AssistantOutputItem{.assistant_turn_id = std::move(turn_id),
                                         .sequence = sequence,
                                         .kind = AssistantOutputItemKind::FunctionCall,
                                         .provider_item_id = std::move(provider_item_id),
                                         .provider_output_index = output_index,
                                         .payload = AssistantOutputFunctionCall{
                                             .call_id = std::move(call_id), .name = std::move(name), .arguments_json = "{\"path\":\"note.txt\"}"}});
  };
  auto make_commit = [&](std::string id, std::string turn_id, std::size_t item_count, std::string finish_reason = "completed") {
    auto data = ava::session::serialize_assistant_turn_commit_data_json(AssistantTurnCommit{
        .assistant_turn_id = std::move(turn_id),
        .item_count = item_count,
        .provider = "openai",
        .model = "gpt-5.5",
        .finish_reason = std::move(finish_reason),
        .usage_json = "{\"input_tokens\":1,\"output_tokens\":2,\"total_tokens\":3,\"estimated\":false,\"source\":\"provider\",\"cost_usd\":0.01}"});
    expect(data.has_value(), "v4 assistant turn commit codec serializes valid bounded usage metadata");
    return make_entry(std::move(id), EntryType::AssistantTurnCommit, data ? *data : "{}");
  };

  auto const text = make_text("out_text", "turn_codec", 0, "msg_codec", 0);
  auto const reasoning = make_reasoning("out_reasoning", "turn_codec", 1, "rs_codec", 1);
  auto const function = make_function("out_function", "turn_codec", 2, "fc_codec", 2);
  auto const parsed_text = ava::session::parse_assistant_output_item(text);
  auto const parsed_reasoning = ava::session::parse_assistant_output_item(reasoning);
  auto const parsed_function = ava::session::parse_assistant_output_item(function);
  expect(parsed_text && parsed_reasoning && parsed_function && std::holds_alternative<AssistantOutputText>(parsed_text->payload) &&
             std::holds_alternative<AssistantOutputReasoning>(parsed_reasoning->payload) &&
             std::holds_alternative<AssistantOutputFunctionCall>(parsed_function->payload),
         "v4 codecs round-trip text, reasoning, and function_call item variants");
  if (parsed_reasoning)
  {
    auto const* private_reasoning = std::get_if<AssistantOutputReasoning>(&parsed_reasoning->payload);
    expect(private_reasoning && private_reasoning->signature == "PRIVATE_SIGNATURE_CANARY" && private_reasoning->redacted_data == "PRIVATE_REDACTED_CANARY",
           "v4 reasoning codec preserves private-field canaries for later dedicated sanitization work");
  }

  auto portable_reasoning = *parsed_reasoning;
  auto* portable_reasoning_payload = std::get_if<AssistantOutputReasoning>(&portable_reasoning.payload);
  if (portable_reasoning_payload)
  {
    portable_reasoning_payload->signature.reset();
    portable_reasoning_payload->redacted_data.reset();
    portable_reasoning_payload->native_item_json.reset();
    portable_reasoning_payload->private_replay_metadata_omitted = true;
  }
  auto portable_reasoning_data = ava::session::serialize_assistant_output_item_data_json(portable_reasoning);
  auto portable_reasoning_entry = make_entry("portable_reasoning", EntryType::AssistantOutputItem, portable_reasoning_data.value_or("{}"));
  auto contradictory_portable_reasoning = portable_reasoning_entry;
  contradictory_portable_reasoning.data_json.pop_back();
  contradictory_portable_reasoning.data_json += ",\"signature\":\"PRIVATE_CONTRADICTION\"}";
  auto false_portable_marker = portable_reasoning_entry;
  false_portable_marker.data_json.replace(false_portable_marker.data_json.find("\"private_replay_metadata_omitted\":true"),
                                          std::string("\"private_replay_metadata_omitted\":true").size(), "\"private_replay_metadata_omitted\":false");
  expect(portable_reasoning_data && ava::session::parse_assistant_output_item(portable_reasoning_entry) &&
             !ava::session::parse_assistant_output_item(contradictory_portable_reasoning) && !ava::session::parse_assistant_output_item(false_portable_marker),
         "v4 portable reasoning marker is canonical, strict, and forbids retained private replay fields");

  auto line = ava::session::serialize_session_entry_line(text);
  auto round_trip = line ? ava::session::parse_session_entry_line(*line, "v4-round-trip.jsonl") : ava::core::Result<SessionEntry>{};
  auto legacy_v3_item = text;
  legacy_v3_item.version = 3;
  auto v3_item_line = line;
  if (v3_item_line)
    v3_item_line->replace(v3_item_line->find("\"version\":4"), std::string("\"version\":4").size(), "\"version\":3");
  expect(line && round_trip && round_trip->type == EntryType::AssistantOutputItem && round_trip->version == 4 &&
             !ava::session::serialize_session_entry_line(legacy_v3_item) &&
             (!v3_item_line || !ava::session::parse_session_entry_line(*v3_item_line, "v3-output-item.jsonl")),
         "v4 assistant output entry type round-trips only at version 4 or newer");

  auto incompatible_text = text;
  incompatible_text.data_json.pop_back();
  incompatible_text.data_json += ",\"format\":\"not-text\"}";
  auto duplicate_key = text;
  duplicate_key.data_json.pop_back();
  duplicate_key.data_json += ",\"sequence\":0}";
  auto invalid_arguments = function;
  invalid_arguments.data_json =
      "{\"schema_version\":1,\"assistant_turn_id\":\"turn_codec\",\"sequence\":2,\"kind\":\"function_call\","
      "\"call_id\":\"call_schema\",\"name\":\"read_file\",\"arguments\":\"[]\"}";
  auto invalid_native = reasoning;
  invalid_native.data_json =
      "{\"schema_version\":1,\"assistant_turn_id\":\"turn_codec\",\"sequence\":1,\"kind\":\"reasoning\","
      "\"text\":\"reasoning\",\"format\":\"openai_responses\",\"redacted\":false,\"native_item_json\":\"{}\"}";
  auto invalid_commit = make_commit("commit_invalid", "turn_codec", 3, "completed");
  invalid_commit.data_json =
      "{\"schema_version\":1,\"assistant_turn_id\":\"turn_codec\",\"item_count\":3,\"provider\":\"openai\","
      "\"model\":\"gpt-5.5\",\"finish_reason\":\"not_normalized\"}";
  auto maximum_usage_commit = make_commit("commit_max_usage", "turn_max_usage", 0);
  maximum_usage_commit.data_json =
      "{\"schema_version\":1,\"assistant_turn_id\":\"turn_max_usage\",\"item_count\":0,\"provider\":\"openai\",\"model\":\"gpt-5.5\","
      "\"finish_reason\":\"completed\",\"usage\":{\"input_tokens\":9223372036854775807,\"total_tokens\":9223372036854775807}}";
  auto overflowing_usage_commit = maximum_usage_commit;
  overflowing_usage_commit.id = "commit_overflowing_usage";
  overflowing_usage_commit.data_json.replace(overflowing_usage_commit.data_json.find("9223372036854775807"), std::string("9223372036854775807").size(),
                                             "9223372036854775808");
  auto leading_zero_sequence = text;
  leading_zero_sequence.data_json.replace(leading_zero_sequence.data_json.find("\"sequence\":0"), std::string("\"sequence\":0").size(), "\"sequence\":00");
  auto over_limit_sequence = text;
  over_limit_sequence.data_json.replace(over_limit_sequence.data_json.find("\"sequence\":0"), std::string("\"sequence\":0").size(), "\"sequence\":4096");
  auto over_limit_provider_index = text;
  over_limit_provider_index.data_json.replace(over_limit_provider_index.data_json.find("\"provider_output_index\":0"),
                                              std::string("\"provider_output_index\":0").size(), "\"provider_output_index\":4096");
  auto empty_reasoning = reasoning;
  empty_reasoning.data_json =
      "{\"schema_version\":1,\"assistant_turn_id\":\"turn_codec\",\"sequence\":1,\"kind\":\"reasoning\","
      "\"text\":\"\",\"format\":\"openai_responses\",\"redacted\":false}";
  auto const invalid_item_validation = ava::session::validate_session_replay({incompatible_text});
  auto const invalid_commit_validation = ava::session::validate_session_replay({invalid_commit});
  expect(ava::session::kCurrentAssistantOutputSchemaVersion == 1 && ava::session::to_string(static_cast<AssistantOutputItemKind>(999)) == "unknown" &&
             !ava::session::parse_assistant_output_item(incompatible_text) && !ava::session::parse_assistant_output_item(duplicate_key) &&
             !ava::session::parse_assistant_output_item(invalid_arguments) && !ava::session::parse_assistant_output_item(invalid_native) &&
             !ava::session::parse_assistant_output_item(leading_zero_sequence) && !ava::session::parse_assistant_output_item(over_limit_sequence) &&
             !ava::session::parse_assistant_output_item(over_limit_provider_index) && !ava::session::parse_assistant_output_item(empty_reasoning) &&
             !ava::session::parse_assistant_turn_commit(invalid_commit) && ava::session::parse_assistant_turn_commit(maximum_usage_commit) &&
             !ava::session::parse_assistant_turn_commit(overflowing_usage_commit) &&
             ava::tests::session_replay_has_issue(invalid_item_validation, ava::session::SessionReplayIssueKind::InvalidAssistantOutputItem) &&
             ava::tests::session_replay_has_issue(invalid_commit_validation, ava::session::SessionReplayIssueKind::InvalidAssistantTurnCommit),
         "v4 codecs and replay diagnostics reject duplicate keys, incompatible variants, invalid arguments/native reasoning, and commits");

  auto nested_object = [](std::size_t depth) {
    std::string json;
    for (std::size_t index = 0; index < depth; ++index) json += "{\"x\":";
    json += '0';
    json.append(depth, '}');
    return json;
  };
  auto function_at_depth = [&](std::size_t depth) {
    auto candidate = function;
    candidate.data_json =
        "{\"schema_version\":1,\"assistant_turn_id\":\"turn_codec\",\"sequence\":2,\"kind\":\"function_call\","
        "\"call_id\":\"call_schema\",\"name\":\"read_file\",\"arguments\":\"" +
        ava::core::json::escape(nested_object(depth)) + "\"}";
    return ava::session::parse_assistant_output_item(candidate).has_value();
  };
  auto reasoning_at_depth = [&](std::size_t depth) {
    auto candidate = reasoning;
    auto const native_item = std::string("{\"id\":\"rs_depth\",\"type\":\"reasoning\",\"summary\":[],\"unknown\":") + nested_object(depth - 1) + "}";
    candidate.data_json =
        "{\"schema_version\":1,\"assistant_turn_id\":\"turn_codec\",\"sequence\":1,\"kind\":\"reasoning\","
        "\"text\":\"reasoning\",\"format\":\"openai_responses\",\"redacted\":false,\"native_item_json\":\"" +
        ava::core::json::escape(native_item) + "\"}";
    return ava::session::parse_assistant_output_item(candidate).has_value();
  };
  expect(function_at_depth(ava::core::json::kMaxNestingDepth - 1) && function_at_depth(ava::core::json::kMaxNestingDepth) &&
             !function_at_depth(ava::core::json::kMaxNestingDepth + 1) && reasoning_at_depth(ava::core::json::kMaxNestingDepth - 1) &&
             reasoning_at_depth(ava::core::json::kMaxNestingDepth) && !reasoning_at_depth(ava::core::json::kMaxNestingDepth + 1),
         "v4 function arguments and native reasoning enforce the shared JSON nesting boundary");

  auto zero_commit = make_commit("commit_zero", "turn_zero", 0);
  auto commit = make_commit("commit_codec", "turn_codec", 3);
  auto const complete = ava::session::classify_assistant_output({text, reasoning, function, commit, zero_commit});
  auto const projected_stats = ava::session::compute_session_stats({text, reasoning, function, commit, zero_commit});
  expect(complete.diagnostics.empty() && complete.turns.size() == 2 && complete.turns[0].start_index == 0 && complete.turns[0].commit_index == 3 &&
             complete.turns[0].items.size() == 3 && complete.turns[1].items.empty() && complete.find_turn_by_commit_index(3) == &complete.turns[0] &&
             complete.find_item_by_output_entry_id("out_function") != nullptr && projected_stats->entry_count == 4 &&
             projected_stats->counts.assistant_message == 2 && projected_stats->counts.reasoning_block == 1 && projected_stats->counts.tool_call == 1 &&
             projected_stats->input_tokens && *projected_stats->input_tokens == 2 && projected_stats->known_cost_usd &&
             *projected_stats->known_cost_usd > 0.019L,
         "v4 classifier projects committed turns into legacy-equivalent stats without counting physical staging records");

  for (std::size_t prefix_size = 1; prefix_size <= 4; ++prefix_size)
  {
    std::vector<SessionEntry> prefix{text, reasoning, function, commit};
    prefix.resize(prefix_size);
    auto const projection = ava::session::classify_assistant_output(prefix);
    bool const complete_prefix = prefix_size == 4;
    expect((complete_prefix && projection.turns.size() == 1 && projection.diagnostics.empty()) ||
               (!complete_prefix && projection.turns.empty() && projection.diagnostics.size() == 1 &&
                projection.diagnostics.front().kind == ava::session::AssistantOutputDiagnosticKind::IncompleteAssistantTurn &&
                projection.diagnostics.front().severity == ava::session::AssistantOutputDiagnosticSeverity::Warning),
           "every complete-line prefix keeps staged assistant output invisible until its trailing commit");
  }

  auto sparse = reasoning;
  auto sparse_item = ava::session::parse_assistant_output_item(sparse);
  if (sparse_item)
  {
    sparse_item->sequence = 2;
    sparse.data_json = ava::session::serialize_assistant_output_item_data_json(*sparse_item).value_or("{}");
  }
  auto bad_count = make_commit("commit_bad_count", "turn_codec", 1);
  auto mismatched_turn = make_commit("commit_mismatch", "other_turn", 2);
  auto unrelated = make_entry("user_interleave", EntryType::UserMessage, "{\"text\":\"interleaved\"}");
  auto has_malformed = [](ava::session::AssistantOutputProjection const& projection) {
    return std::ranges::any_of(projection.diagnostics,
                               [](auto const& diagnostic) { return diagnostic.kind == ava::session::AssistantOutputDiagnosticKind::MalformedAssistantTurn; });
  };
  auto const final_sparse_projection = ava::session::classify_assistant_output({text, sparse});
  auto const final_invalid_projection = ava::session::classify_assistant_output({incompatible_text});
  expect(has_malformed(ava::session::classify_assistant_output({text, sparse, make_commit("commit_sparse", "turn_codec", 2)})) &&
             has_malformed(final_sparse_projection) && has_malformed(final_invalid_projection) &&
             std::ranges::none_of(
                 final_invalid_projection.diagnostics,
                 [](auto const& diagnostic) { return diagnostic.kind == ava::session::AssistantOutputDiagnosticKind::IncompleteAssistantTurn; }) &&
             std::ranges::none_of(
                 final_sparse_projection.diagnostics,
                 [](auto const& diagnostic) { return diagnostic.kind == ava::session::AssistantOutputDiagnosticKind::IncompleteAssistantTurn; }) &&
             has_malformed(ava::session::classify_assistant_output({text, reasoning, bad_count})) &&
             has_malformed(ava::session::classify_assistant_output({text, reasoning, mismatched_turn})) &&
             has_malformed(ava::session::classify_assistant_output({text, unrelated})),
         "v4 classifier rejects sparse sequences, count/turn mismatches, and unrelated interleaving");

  std::vector<SessionEntry> over_limit_staging;
  over_limit_staging.reserve(ava::session::kMaxAssistantOutputItemsPerTurn + 1);
  for (std::size_t index = 0; index <= ava::session::kMaxAssistantOutputItemsPerTurn; ++index)
  {
    over_limit_staging.push_back(
        make_item("out_over_limit_" + std::to_string(index),
                  AssistantOutputItem{.assistant_turn_id = "turn_over_limit",
                                      .sequence = index % ava::session::kMaxAssistantOutputItemsPerTurn,
                                      .kind = AssistantOutputItemKind::Text,
                                      .provider_item_id = std::nullopt,
                                      .provider_output_index = std::nullopt,
                                      .payload = AssistantOutputText{.text = "x", .assistant_phase = AssistantOutputTextPhase::Commentary}}));
  }
  auto const over_limit_projection = ava::session::classify_assistant_output(over_limit_staging);
  expect(
      std::ranges::any_of(over_limit_projection.diagnostics,
                          [](auto const& diagnostic) {
                            return diagnostic.severity == ava::session::AssistantOutputDiagnosticSeverity::Error &&
                                   diagnostic.message == "staged assistant turn exceeds 4096 output items";
                          }) &&
          std::ranges::none_of(over_limit_projection.diagnostics,
                               [](auto const& diagnostic) { return diagnostic.kind == ava::session::AssistantOutputDiagnosticKind::IncompleteAssistantTurn; }),
      "v4 classifier treats staging beyond 4096 items as an error rather than an incomplete suffix");

  auto duplicate_turn = make_commit("commit_duplicate_turn", "turn_codec", 0);
  auto duplicate_item_id = reasoning;
  auto duplicate_item_id_payload = ava::session::parse_assistant_output_item(duplicate_item_id);
  if (duplicate_item_id_payload)
  {
    duplicate_item_id_payload->provider_item_id = "msg_codec";
    duplicate_item_id.data_json = ava::session::serialize_assistant_output_item_data_json(*duplicate_item_id_payload).value_or("{}");
  }
  auto duplicate_item_index = reasoning;
  auto duplicate_item_index_payload = ava::session::parse_assistant_output_item(duplicate_item_index);
  if (duplicate_item_index_payload)
  {
    duplicate_item_index_payload->provider_output_index = 0;
    duplicate_item_index.data_json = ava::session::serialize_assistant_output_item_data_json(*duplicate_item_index_payload).value_or("{}");
  }
  auto const final_duplicate_id_projection = ava::session::classify_assistant_output({text, duplicate_item_id});
  auto const final_duplicate_index_projection = ava::session::classify_assistant_output({text, duplicate_item_index});
  expect(
      has_malformed(ava::session::classify_assistant_output({text, reasoning, function, commit, duplicate_turn})) &&
          has_malformed(ava::session::classify_assistant_output({text, duplicate_item_id, make_commit("commit_duplicate_id", "turn_codec", 2)})) &&
          has_malformed(ava::session::classify_assistant_output({text, duplicate_item_index, make_commit("commit_duplicate_index", "turn_codec", 2)})) &&
          has_malformed(final_duplicate_id_projection) && has_malformed(final_duplicate_index_projection) &&
          std::ranges::none_of(
              final_duplicate_id_projection.diagnostics,
              [](auto const& diagnostic) { return diagnostic.kind == ava::session::AssistantOutputDiagnosticKind::IncompleteAssistantTurn; }) &&
          std::ranges::none_of(final_duplicate_index_projection.diagnostics,
                               [](auto const& diagnostic) { return diagnostic.kind == ava::session::AssistantOutputDiagnosticKind::IncompleteAssistantTurn; }),
      "v4 classifier rejects duplicate committed and final-staged provider item IDs or indexes");

  auto const reused_committed_turn_item = make_text("out_reused_committed_turn", "turn_codec", 0, "msg_reused_committed_turn", 0);
  auto const reused_committed_turn_projection = ava::session::classify_assistant_output({text, reasoning, function, commit, reused_committed_turn_item});
  auto const reused_committed_turn_replay = ava::session::validate_session_replay({text, reasoning, function, commit, reused_committed_turn_item});
  expect(has_malformed(reused_committed_turn_projection) &&
             std::ranges::none_of(
                 reused_committed_turn_projection.diagnostics,
                 [](auto const& diagnostic) { return diagnostic.kind == ava::session::AssistantOutputDiagnosticKind::IncompleteAssistantTurn; }) &&
             ava::tests::session_replay_has_issue(reused_committed_turn_replay, ava::session::SessionReplayIssueKind::MalformedAssistantTurn),
         "a final staged v4 item cannot reuse a committed assistant turn id as an incomplete warning");

  auto legacy_user = make_entry("legacy_user", EntryType::UserMessage, "{\"text\":\"legacy\"}", 3);
  auto const mixed = ava::session::validate_session_replay({legacy_user, zero_commit});
  expect(mixed.ok() && mixed.issues.empty(), "mixed v3 and v4 histories remain replay-valid");

  auto bound_function = make_function("out_bound_function", "turn_bound", 0, "fc_bound", 0, "call_bound", "read_file");
  auto bound_commit = make_commit("commit_bound", "turn_bound", 1, "tool_calls");
  auto bound_result = make_entry("result_bound", EntryType::ToolResult,
                                 "{\"assistant_output_entry_id\":\"out_bound_function\",\"call_id\":\"call_bound\","
                                 "\"name\":\"read_file\",\"success\":true,\"result\":\"ok\"}");
  auto const exact_binding = ava::session::validate_session_replay({bound_function, bound_commit, bound_result});
  auto missing_binding = bound_result;
  missing_binding.data_json = "{\"call_id\":\"call_bound\",\"name\":\"read_file\",\"success\":true,\"result\":\"ok\"}";
  auto wrong_binding = bound_result;
  wrong_binding.data_json = "{\"assistant_output_entry_id\":\"out_text\",\"call_id\":\"call_bound\",\"name\":\"read_file\",\"success\":true,\"result\":\"ok\"}";
  auto wrong_name_binding = bound_result;
  wrong_name_binding.data_json =
      "{\"assistant_output_entry_id\":\"out_bound_function\",\"call_id\":\"call_bound\",\"name\":\"bash\",\"success\":true,\"result\":\"ok\"}";
  auto const missing_binding_validation = ava::session::validate_session_replay({bound_function, bound_commit, missing_binding});
  auto const wrong_binding_validation = ava::session::validate_session_replay({bound_function, bound_commit, wrong_binding});
  auto const wrong_name_binding_validation = ava::session::validate_session_replay({bound_function, bound_commit, wrong_name_binding});
  auto const duplicate_result_validation = ava::session::validate_session_replay({bound_function, bound_commit, bound_result, bound_result});
  auto const uncommitted_function = ava::session::validate_session_replay({bound_function});
  auto const unresolved_function = ava::session::validate_session_replay({bound_function, bound_commit});
  auto const lenient_missing_binding = ava::session::validate_session_replay(
      {bound_function, bound_commit, missing_binding}, ava::session::SessionReplayValidationOptions{.require_tool_result_pairing = false});
  auto const result_before_commit = ava::session::validate_session_replay({bound_function, bound_result, bound_commit});
  auto const lenient_legacy_result = ava::session::validate_session_replay(
      {make_entry("legacy_result", EntryType::ToolResult, "{\"call_id\":\"legacy\",\"name\":\"read_file\",\"success\":true,\"result\":\"ok\"}", 3)},
      ava::session::SessionReplayValidationOptions{.require_tool_result_pairing = false});
  auto v3_private_binding = bound_result;
  v3_private_binding.id = "v3_private_binding";
  v3_private_binding.version = 3;
  auto const v3_private_binding_validation = ava::session::validate_session_replay({v3_private_binding});
  auto const mixed_private_projection = ava::session::project_logical_session_history({bound_function, bound_commit, v3_private_binding});
  expect(exact_binding.ok() &&
             ava::tests::session_replay_has_issue(missing_binding_validation, ava::session::SessionReplayIssueKind::ToolResultOutputItemMismatch) &&
             ava::tests::session_replay_has_issue(wrong_binding_validation, ava::session::SessionReplayIssueKind::ToolResultOutputItemMismatch) &&
             ava::tests::session_replay_has_issue(wrong_name_binding_validation, ava::session::SessionReplayIssueKind::ToolResultOutputItemMismatch) &&
             ava::tests::session_replay_has_issue(duplicate_result_validation, ava::session::SessionReplayIssueKind::DuplicateToolResult) &&
             uncommitted_function.ok() &&
             ava::tests::session_replay_has_issue(uncommitted_function, ava::session::SessionReplayIssueKind::IncompleteAssistantTurn) &&
             ava::tests::session_replay_has_issue(unresolved_function, ava::session::SessionReplayIssueKind::UnresolvedToolCall) &&
             ava::tests::session_replay_has_issue(lenient_missing_binding, ava::session::SessionReplayIssueKind::ToolResultOutputItemMismatch) &&
             ava::tests::session_replay_has_issue(result_before_commit, ava::session::SessionReplayIssueKind::ToolResultOutputItemMismatch) &&
             lenient_legacy_result.ok() &&
             ava::tests::session_replay_has_issue(v3_private_binding_validation, ava::session::SessionReplayIssueKind::ToolResultOutputItemMismatch) &&
             mixed_private_projection && mixed_private_projection->back().data_json.find("assistant_output_entry_id") == std::string::npos,
         "v4 tool results require exact committed bindings while every public projection strips v3/v4 private bindings");

  auto permission_audit = make_entry("permission_audit", EntryType::PermissionDecision,
                                     "{\"operation\":\"read\",\"mode\":\"build\",\"tool_name\":\"read_file\",\"action\":\"allow\",\"reason\":\"test\","
                                     "\"permission_request_id\":\"permission-window\",\"resolution\":\"allow\",\"resolution_source\":\"policy\"}");
  auto const allowed_window = ava::session::validate_session_replay(
      {bound_function, bound_commit, make_entry("window_error", EntryType::Error, "{\"message\":\"audit\"}"), permission_audit, bound_result});
  auto const after_user = ava::session::validate_session_replay(
      {bound_function, bound_commit, make_entry("window_user", EntryType::UserMessage, "{\"text\":\"next\"}"), bound_result});
  auto const after_assistant = ava::session::validate_session_replay(
      {bound_function, bound_commit, make_entry("window_assistant", EntryType::AssistantMessage, "{\"text\":\"next\"}"), bound_result});
  auto const after_output_item = ava::session::validate_session_replay(
      {bound_function, bound_commit, make_text("window_output_item", "window-next-turn", 0, "window-msg", 0), bound_result});
  auto const after_turn_commit =
      ava::session::validate_session_replay({bound_function, bound_commit, make_commit("window_turn_commit", "window-next-turn", 0), bound_result});
  auto const after_compaction = ava::session::validate_session_replay(
      {bound_function, bound_commit, make_entry("window_compaction", EntryType::Compaction, "{\"summary\":\"next\"}"), bound_result});
  expect(allowed_window.ok() && ava::tests::session_replay_has_issue(after_user, ava::session::SessionReplayIssueKind::ToolResultOutputItemMismatch) &&
             ava::tests::session_replay_has_issue(after_assistant, ava::session::SessionReplayIssueKind::ToolResultOutputItemMismatch) &&
             ava::tests::session_replay_has_issue(after_output_item, ava::session::SessionReplayIssueKind::ToolResultOutputItemMismatch) &&
             ava::tests::session_replay_has_issue(after_turn_commit, ava::session::SessionReplayIssueKind::ToolResultOutputItemMismatch) &&
             ava::tests::session_replay_has_issue(after_compaction, ava::session::SessionReplayIssueKind::ToolResultOutputItemMismatch),
         "v4 tool results remain valid across bookkeeping but must stay in their committed turn's immediate post-commit window");

  auto reconciliation_rejects = [](std::vector<SessionEntry> const& entries) { return !ava::session::find_unresolved_committed_function_calls(entries); };
  auto const next_unresolved_function =
      make_function("out_current_unresolved", "turn_current_unresolved", 0, "fc_current_unresolved", 0, "call_current_unresolved", "read_file");
  auto const next_unresolved_commit = make_commit("commit_current_unresolved", "turn_current_unresolved", 1, "tool_calls");
  auto const closed_then_current_unresolved = ava::session::find_unresolved_committed_function_calls(
      {bound_function, bound_commit, make_entry("closed_window_user", EntryType::UserMessage, "{\"text\":\"later\"}"), next_unresolved_function,
       next_unresolved_commit});
  auto missing_output_binding = bound_result;
  missing_output_binding.id = "missing-output-binding";
  missing_output_binding.data_json =
      "{\"assistant_output_entry_id\":\"unknown-output\",\"call_id\":\"call_bound\",\"name\":\"read_file\",\"success\":true,\"result\":\"ok\"}";
  auto empty_output_binding = missing_output_binding;
  empty_output_binding.id = "empty-output-binding";
  empty_output_binding.data_json = "{\"assistant_output_entry_id\":\"\",\"call_id\":\"call_bound\",\"name\":\"read_file\",\"success\":true,\"result\":\"ok\"}";
  auto wrong_type_output_binding = missing_output_binding;
  wrong_type_output_binding.id = "wrong-type-output-binding";
  wrong_type_output_binding.data_json =
      "{\"assistant_output_entry_id\":false,\"call_id\":\"call_bound\",\"name\":\"read_file\",\"success\":true,\"result\":\"ok\"}";
  auto nonfunction_output = make_text("nonfunction-output", "nonfunction-turn", 0, "nonfunction-msg", 0);
  auto nonfunction_commit = make_commit("nonfunction-commit", "nonfunction-turn", 1);
  auto nonfunction_binding = missing_output_binding;
  nonfunction_binding.id = "nonfunction-binding";
  nonfunction_binding.data_json =
      "{\"assistant_output_entry_id\":\"nonfunction-output\",\"call_id\":\"call_bound\",\"name\":\"read_file\",\"success\":true,\"result\":\"ok\"}";
  auto out_of_window_binding = bound_result;
  out_of_window_binding.id = "out-of-window-binding";
  auto missing_binding_for_function = bound_result;
  missing_binding_for_function.id = "missing-binding-for-function";
  missing_binding_for_function.data_json = "{\"call_id\":\"call_bound\",\"name\":\"read_file\",\"success\":true,\"result\":\"ok\"}";
  auto wrong_call_binding = bound_result;
  wrong_call_binding.id = "wrong-call-binding";
  wrong_call_binding.data_json =
      "{\"assistant_output_entry_id\":\"out_bound_function\",\"call_id\":\"other\",\"name\":\"read_file\",\"success\":true,\"result\":\"ok\"}";
  auto future_bound_function = make_function("future-bound-function", "future-bound-turn", 0, "future-fc", 0, "future-call", "read_file");
  auto future_bound_commit = make_commit("future-bound-commit", "future-bound-turn", 1, "tool_calls");
  auto future_binding = make_entry("future-binding", EntryType::ToolResult,
                                   "{\"assistant_output_entry_id\":\"future-bound-function\",\"call_id\":\"future-call\",\"name\":\"read_file\","
                                   "\"success\":true,\"result\":\"ok\"}");
  auto duplicate_bound_result = bound_result;
  duplicate_bound_result.id = "duplicate-bound-result";
  auto exact_reconciled = ava::session::find_unresolved_committed_function_calls({bound_function, bound_commit, bound_result});
  expect(exact_reconciled && exact_reconciled->empty() && !closed_then_current_unresolved &&
             closed_then_current_unresolved.error().message().find("active EOF tool-result window") != std::string::npos &&
             reconciliation_rejects({bound_function, bound_commit, missing_output_binding}) &&
             reconciliation_rejects({bound_function, bound_commit, empty_output_binding}) &&
             reconciliation_rejects({bound_function, bound_commit, wrong_type_output_binding}) &&
             reconciliation_rejects({bound_function, bound_commit, nonfunction_output, nonfunction_commit, nonfunction_binding}) &&
             reconciliation_rejects({bound_function, bound_commit, make_entry("window-user-for-reconcile", EntryType::UserMessage, "{\"text\":\"next\"}"),
                                     out_of_window_binding}) &&
             reconciliation_rejects({bound_function, bound_commit, missing_binding_for_function}) &&
             reconciliation_rejects({bound_function, bound_commit, wrong_call_binding}) &&
             reconciliation_rejects({future_binding, future_bound_function, future_bound_commit}) &&
             reconciliation_rejects({bound_function, bound_commit, bound_result, duplicate_bound_result}),
         "reconciliation rejects closed unresolved windows before synthetic append, including histories with a later current call");

  auto const compaction_after_unresolved = ava::session::validate_session_replay(
      {bound_function, bound_commit, make_entry("compact_pending", EntryType::Compaction, "{\"summary\":\"pending tool\"}")});
  expect(ava::tests::session_replay_has_issue(compaction_after_unresolved, ava::session::SessionReplayIssueKind::CompactionWithUnresolvedToolCall),
         "committed v4 function calls participate in compaction unresolved-call boundaries");

  auto v3_native_reasoning =
      make_entry("v3_native_reasoning", EntryType::ReasoningBlock,
                 "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\",\"text\":\"reasoned\",\"redacted\":false,"
                 "\"native_item_json\":\"{\\\"type\\\":\\\"reasoning\\\",\\\"summary\\\":[]}\"}",
                 3);
  auto const v3_native_validation = ava::session::validate_session_replay({v3_native_reasoning});
  auto const public_markdown = ava::session::format_session_markdown({reasoning, commit});
  expect(ava::tests::session_replay_has_issue(v3_native_validation, ava::session::SessionReplayIssueKind::InvalidReasoningEntry) &&
             public_markdown.find("PRIVATE_SIGNATURE_CANARY") == std::string::npos && public_markdown.find("PRIVATE_REDACTED_CANARY") == std::string::npos,
         "native OpenAI reasoning remains strict from v3 and private v4 physical records stay out of public export");

  auto future_item = text;
  future_item.version = 5;
  auto future_line = line;
  if (future_line)
  {
    auto future = *future_line;
    future.replace(future.find("\"version\":4"), std::string("\"version\":4").size(), "\"version\":5");
    auto const future_projection = ava::session::classify_assistant_output({future_item});
    expect(!ava::session::parse_session_entry_line(future, "future-v4.jsonl") && future_projection.turns.empty() && !future_projection.diagnostics.empty() &&
               future_projection.diagnostics.front().kind == ava::session::AssistantOutputDiagnosticKind::InvalidAssistantOutputItem,
           "record and v4 classifier reject future entry versions after the v4 bump");
  }
}

void test_session_replay_validation()
{
  std::vector<std::string_view> const current_resolution_sources{"client_cancel", "hard_scope", "session_grant", "session_config", "client"};
  expect(std::ranges::all_of(current_resolution_sources, ava::session::valid_resolution_source),
         "session validation accepts current protocol-neutral permission resolution sources");
  std::vector<std::string_view> const legacy_acp_resolution_sources{"acp_client_cancel", "acp_hard_policy", "acp_session_grant",
                                                                    "acp_session_mcp",   "acp_client",      "acp_client_error"};
  expect(
      std::ranges::all_of(legacy_acp_resolution_sources, ava::session::valid_resolution_source) && !ava::session::valid_resolution_source("acp_unknown_source"),
      "session validation accepts only known legacy ACP permission source aliases for read compatibility");

  std::vector<ava::session::SessionEntry> const valid_entries = {
      ava::session::SessionEntry{.id = "start",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::SessionStart,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"mode\":\"build\",\"provider\":\"openai\","
                                              "\"model\":\"gpt-5.5\",\"prompt_override\":false,"
                                              "\"context_sources\":0}"},
      ava::session::SessionEntry{.id = "user",
                                 .parent_id = "start",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-29T00:00:01Z",
                                 .data_json = "{\"text\":\"read note\"}"},
      ava::session::SessionEntry{.id = "assistant",
                                 .parent_id = "user",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-04-29T00:00:02Z",
                                 .data_json = "{\"text\":\"\",\"tool_calls\":1}"},
      ava::session::SessionEntry{.id = "tool_call",
                                 .parent_id = "assistant",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-04-29T00:00:03Z",
                                 .data_json = "{\"call_id\":\"call_read\",\"name\":\"read_file\","
                                              "\"arguments\":\"{\\\"path\\\":\\\"note.txt\\\"}\"}"},
      ava::session::SessionEntry{.id = "tool_result",
                                 .parent_id = "tool_call",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-29T00:00:04Z",
                                 .data_json = "{\"call_id\":\"call_read\",\"name\":\"read_file\",\"success\":true,"
                                              "\"status\":\"success\",\"result\":\"note contents\","
                                              "\"structured_result\":{\"schema_version\":1,\"call_id\":\"call_read\","
                                              "\"tool\":\"read_file\",\"status\":\"success\",\"ok\":true,"
                                              "\"content_type\":\"text/plain\",\"content\":\"note contents\"}}"},
  };

  auto const valid =
      ava::session::validate_session_replay(valid_entries, ava::session::SessionReplayValidationOptions{.require_structured_tool_results = true});
  expect(valid.ok() && valid.issues.empty(), "session replay validator accepts paired structured tool history");

  auto nested_structured_value = [](std::size_t depth) {
    std::string value;
    value.reserve(depth * 6 + 1);
    for (std::size_t index = 0; index < depth; ++index) value += "{\"x\":";
    value += '0';
    value.append(depth, '}');
    return value;
  };
  auto over_depth_structured_entries = valid_entries;
  over_depth_structured_entries.back().data_json =
      "{\"call_id\":\"call_read\",\"name\":\"read_file\",\"success\":true,\"status\":\"success\",\"result\":\"note contents\","
      "\"structured_result\":{\"schema_version\":1,\"call_id\":\"call_read\",\"tool\":\"read_file\",\"status\":\"success\",\"ok\":true,"
      "\"content_type\":\"application/json\",\"content\":" +
      nested_structured_value(ava::core::json::kMaxNestingDepth + 1) + "}}";
  auto const over_depth_structured = ava::session::validate_session_replay(
      over_depth_structured_entries, ava::session::SessionReplayValidationOptions{.require_structured_tool_results = true});
  expect(!over_depth_structured.ok() &&
             ava::tests::session_replay_has_issue(over_depth_structured, ava::session::SessionReplayIssueKind::InvalidStructuredToolResult) &&
             std::ranges::any_of(over_depth_structured.issues, [](auto const& issue) { return issue.message.find("not valid JSON") != std::string::npos; }),
         "session replay rejects over-depth structured tool results through the shared JSON nesting boundary");

  auto unsupported_version_entries = valid_entries;
  unsupported_version_entries[1].version = ava::session::kCurrentSessionEntryVersion + 1;
  auto const unsupported_version = ava::session::validate_session_replay(unsupported_version_entries);
  expect(!unsupported_version.ok() && ava::tests::session_replay_has_issue(unsupported_version, ava::session::SessionReplayIssueKind::UnsupportedEntryVersion),
         "session replay validator flags unsupported in-memory entry versions");

  std::vector<ava::session::SessionEntry> const duplicate_entry_entries = {
      valid_entries[0],
      valid_entries[0],
  };
  auto const duplicate_entry = ava::session::validate_session_replay(duplicate_entry_entries);
  expect(!duplicate_entry.ok() && ava::tests::session_replay_has_issue(duplicate_entry, ava::session::SessionReplayIssueKind::DuplicateEntryId),
         "session replay validator flags duplicate entry ids");

  std::vector<ava::session::SessionEntry> const unknown_parent_entries = {
      ava::session::SessionEntry{.id = "child",
                                 .parent_id = "missing_parent",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"text\":\"orphan\"}"},
  };
  auto const unknown_parent = ava::session::validate_session_replay(unknown_parent_entries);
  expect(!unknown_parent.ok() && ava::tests::session_replay_has_issue(unknown_parent, ava::session::SessionReplayIssueKind::UnknownParentId),
         "session replay validator flags parent ids that do not reference earlier entries");

  std::vector<ava::session::SessionEntry> const result_without_call_entries = {
      ava::session::SessionEntry{.id = "tool_result",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"call_id\":\"call_missing\",\"name\":\"read_file\","
                                              "\"success\":true,\"result\":\"orphan\"}"},
  };
  auto const result_without_call = ava::session::validate_session_replay(result_without_call_entries);
  expect(!result_without_call.ok() && ava::tests::session_replay_has_issue(result_without_call, ava::session::SessionReplayIssueKind::ToolResultWithoutCall),
         "session replay validator flags tool results without earlier tool calls");

  auto mismatch_entries = valid_entries;
  mismatch_entries.back().data_json = "{\"call_id\":\"call_read\",\"name\":\"bash\",\"success\":true,\"result\":\"wrong tool\"}";
  auto const mismatch = ava::session::validate_session_replay(mismatch_entries);
  expect(!mismatch.ok() && ava::tests::session_replay_has_issue(mismatch, ava::session::SessionReplayIssueKind::ToolResultToolMismatch),
         "session replay validator flags tool result name mismatches");

  auto unresolved_entries = valid_entries;
  unresolved_entries.pop_back();
  auto const unresolved = ava::session::validate_session_replay(unresolved_entries);
  expect(!unresolved.ok() && ava::tests::session_replay_has_issue(unresolved, ava::session::SessionReplayIssueKind::UnresolvedToolCall),
         "session replay validator flags unresolved tool calls");

  auto missing_structured_entries = valid_entries;
  missing_structured_entries.back().data_json =
      "{\"call_id\":\"call_read\",\"name\":\"read_file\",\"success\":true,\"status\":\"success\","
      "\"result\":\"legacy result\"}";
  auto const missing_structured =
      ava::session::validate_session_replay(missing_structured_entries, ava::session::SessionReplayValidationOptions{.require_structured_tool_results = true});
  expect(!missing_structured.ok() && ava::tests::session_replay_has_issue(missing_structured, ava::session::SessionReplayIssueKind::MissingStructuredToolResult),
         "session replay validator can require structured tool result payloads");

  auto structured_mismatch_entries = valid_entries;
  structured_mismatch_entries.back().data_json =
      "{\"call_id\":\"call_read\",\"name\":\"read_file\",\"success\":true,\"status\":\"success\","
      "\"result\":\"note contents\",\"structured_result\":{\"schema_version\":1,"
      "\"call_id\":\"call_other\",\"tool\":\"read_file\",\"status\":\"success\",\"ok\":true,"
      "\"content_type\":\"text/plain\",\"content\":\"note contents\"}}";
  auto const structured_mismatch =
      ava::session::validate_session_replay(structured_mismatch_entries, ava::session::SessionReplayValidationOptions{.require_structured_tool_results = true});
  expect(
      !structured_mismatch.ok() && ava::tests::session_replay_has_issue(structured_mismatch, ava::session::SessionReplayIssueKind::StructuredToolResultMismatch),
      "session replay validator flags structured result call/tool/status mismatches");

  auto missing_schema_entries = valid_entries;
  missing_schema_entries.back().data_json =
      "{\"call_id\":\"call_read\",\"name\":\"read_file\",\"success\":true,\"status\":\"success\","
      "\"result\":\"note contents\",\"structured_result\":{\"call_id\":\"call_read\","
      "\"tool\":\"read_file\",\"status\":\"success\",\"ok\":true,"
      "\"content_type\":\"text/plain\",\"content\":\"note contents\"}}";
  auto const missing_schema =
      ava::session::validate_session_replay(missing_schema_entries, ava::session::SessionReplayValidationOptions{.require_structured_tool_results = true});
  expect(!missing_schema.ok() && ava::tests::session_replay_has_issue(missing_schema, ava::session::SessionReplayIssueKind::InvalidStructuredToolResult),
         "session replay validator requires structured result schema versions");

  auto missing_ok_entries = valid_entries;
  missing_ok_entries.back().data_json =
      "{\"call_id\":\"call_read\",\"name\":\"read_file\",\"success\":true,\"status\":\"success\","
      "\"result\":\"note contents\",\"structured_result\":{\"schema_version\":1,\"call_id\":\"call_read\","
      "\"tool\":\"read_file\",\"status\":\"success\","
      "\"content_type\":\"text/plain\",\"content\":\"note contents\"}}";
  auto const missing_ok =
      ava::session::validate_session_replay(missing_ok_entries, ava::session::SessionReplayValidationOptions{.require_structured_tool_results = true});
  expect(!missing_ok.ok() && ava::tests::session_replay_has_issue(missing_ok, ava::session::SessionReplayIssueKind::InvalidStructuredToolResult),
         "session replay validator requires structured result ok flags");

  auto ok_mismatch_entries = valid_entries;
  ok_mismatch_entries.back().data_json =
      "{\"call_id\":\"call_read\",\"name\":\"read_file\",\"success\":true,\"status\":\"success\","
      "\"result\":\"note contents\",\"structured_result\":{\"schema_version\":1,\"call_id\":\"call_read\","
      "\"tool\":\"read_file\",\"status\":\"success\",\"ok\":false,"
      "\"content_type\":\"text/plain\",\"content\":\"note contents\"}}";
  auto const ok_mismatch =
      ava::session::validate_session_replay(ok_mismatch_entries, ava::session::SessionReplayValidationOptions{.require_structured_tool_results = true});
  expect(!ok_mismatch.ok() && ava::tests::session_replay_has_issue(ok_mismatch, ava::session::SessionReplayIssueKind::StructuredToolResultMismatch),
         "session replay validator requires structured result ok to match success status");

  auto missing_content_entries = valid_entries;
  missing_content_entries.back().data_json =
      "{\"call_id\":\"call_read\",\"name\":\"read_file\",\"success\":true,\"status\":\"success\","
      "\"result\":\"note contents\",\"structured_result\":{\"schema_version\":1,\"call_id\":\"call_read\","
      "\"tool\":\"read_file\",\"status\":\"success\",\"ok\":true,"
      "\"content_type\":\"text/plain\"}}";
  auto const missing_content =
      ava::session::validate_session_replay(missing_content_entries, ava::session::SessionReplayValidationOptions{.require_structured_tool_results = true});
  expect(!missing_content.ok() && ava::tests::session_replay_has_issue(missing_content, ava::session::SessionReplayIssueKind::InvalidStructuredToolResult),
         "session replay validator requires structured result content data");

  auto invalid_metadata_entries = valid_entries;
  invalid_metadata_entries.back().data_json =
      "{\"call_id\":\"call_read\",\"name\":\"read_file\",\"success\":true,\"status\":\"success\","
      "\"result\":\"note contents\",\"structured_result\":{\"schema_version\":1,\"call_id\":\"call_read\","
      "\"tool\":\"read_file\",\"status\":\"success\",\"ok\":true,"
      "\"content_type\":\"text/plain\",\"content\":\"note contents\","
      "\"changed_paths\":[\"note.txt\",\"note.txt\"]}}";
  auto const invalid_metadata =
      ava::session::validate_session_replay(invalid_metadata_entries, ava::session::SessionReplayValidationOptions{.require_structured_tool_results = true});
  expect(!invalid_metadata.ok() && ava::tests::session_replay_has_issue(invalid_metadata, ava::session::SessionReplayIssueKind::InvalidStructuredToolResult),
         "session replay validator rejects malformed structured result metadata arrays");

  auto failed_missing_error_entries = valid_entries;
  failed_missing_error_entries.back().data_json =
      "{\"call_id\":\"call_read\",\"name\":\"read_file\",\"success\":false,\"status\":\"error\","
      "\"result\":\"failed\",\"structured_result\":{\"schema_version\":1,\"call_id\":\"call_read\","
      "\"tool\":\"read_file\",\"status\":\"error\",\"ok\":false,"
      "\"content_type\":\"text/plain\",\"content\":\"failed\"}}";
  auto const failed_missing_error = ava::session::validate_session_replay(
      failed_missing_error_entries, ava::session::SessionReplayValidationOptions{.require_structured_tool_results = true});
  expect(!failed_missing_error.ok() &&
             ava::tests::session_replay_has_issue(failed_missing_error, ava::session::SessionReplayIssueKind::InvalidStructuredToolResult),
         "session replay validator requires failed structured results to carry error details");

  std::vector<ava::session::SessionEntry> const valid_permission_entries = {
      ava::session::SessionEntry{.id = "permission_policy_allow",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"permission_request_id\":\"permreq_read\","
                                              "\"operation\":\"read\",\"mode\":\"build\","
                                              "\"tool_name\":\"read_file\",\"action\":\"allow\","
                                              "\"reason\":\"allowed by default workspace policy\","
                                              "\"risk\":\"low\",\"target_path\":\"note.txt\",\"resolution\":\"allow\","
                                              "\"resolution_source\":\"policy\"}"},
      ava::session::SessionEntry{.id = "permission_ask",
                                 .parent_id = "permission_policy_allow",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-29T00:00:01Z",
                                 .data_json = "{\"permission_request_id\":\"permreq_edit\","
                                              "\"operation\":\"edit\",\"mode\":\"build\","
                                              "\"tool_name\":\"write_file\",\"action\":\"ask\","
                                              "\"reason\":\"target is outside the workspace\","
                                              "\"risk\":\"high\",\"target_path\":\"/tmp/outside.txt\","
                                              "\"resolution_source\":\"policy\"}"},
      ava::session::SessionEntry{.id = "permission_resolution",
                                 .parent_id = "permission_ask",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-29T00:00:02Z",
                                 .data_json = "{\"permission_request_id\":\"permreq_edit\","
                                              "\"operation\":\"edit\",\"mode\":\"build\","
                                              "\"tool_name\":\"write_file\",\"action\":\"ask\","
                                              "\"reason\":\"target is outside the workspace\","
                                              "\"risk\":\"high\",\"target_path\":\"/tmp/outside.txt\","
                                              "\"resolution\":\"deny\",\"resolution_source\":\"resolver\"}"},
      ava::session::SessionEntry{.id = "permission_lsp_launch",
                                 .parent_id = "permission_resolution",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-29T00:00:03Z",
                                 .data_json = "{\"permission_request_id\":\"permreq_lsp\","
                                              "\"operation\":\"lsp.server.launch\",\"mode\":\"build\","
                                              "\"tool_name\":\"lsp_server_launch\",\"action\":\"allow\","
                                              "\"reason\":\"LSP server launch requires explicit approval\","
                                              "\"risk\":\"high\",\"command\":\"[\\\"clangd\\\"]\","
                                              "\"resolution\":\"allow\",\"resolution_source\":\"policy\"}"},
      ava::session::SessionEntry{.id = "permission_mcp_resource",
                                 .parent_id = "permission_lsp_launch",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-29T00:00:04Z",
                                 .data_json = "{\"permission_request_id\":\"permreq_mcp_resource\","
                                              "\"operation\":\"mcp.resource.read\",\"mode\":\"build\","
                                              "\"tool_name\":\"mcp_demo_resource\",\"action\":\"allow\","
                                              "\"reason\":\"MCP resource read requires permission\","
                                              "\"risk\":\"medium\",\"command\":\"demo:file:///workspace/notes.md\","
                                              "\"resolution\":\"allow\",\"resolution_source\":\"policy\"}"},
  };
  auto const valid_permission = ava::session::validate_session_replay(valid_permission_entries);
  expect(valid_permission.ok() && valid_permission.issues.empty(), "session replay validator accepts complete permission audit decisions");

  std::vector<ava::session::SessionEntry> const valid_session_grant_entries = {
      ava::session::SessionEntry{.id = "permission_ask",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-29T00:00:01Z",
                                 .data_json = "{\"permission_request_id\":\"permreq_granted\","
                                              "\"operation\":\"read\",\"mode\":\"build\","
                                              "\"tool_name\":\"read_file\",\"action\":\"ask\","
                                              "\"reason\":\"target is outside the workspace\","
                                              "\"risk\":\"high\",\"target_path\":\"/tmp/outside.txt\","
                                              "\"resolution_source\":\"policy\"}"},
      ava::session::SessionEntry{.id = "permission_granted",
                                 .parent_id = "permission_ask",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-29T00:00:02Z",
                                 .data_json = "{\"permission_request_id\":\"permreq_granted\","
                                              "\"operation\":\"read\",\"mode\":\"build\","
                                              "\"tool_name\":\"read_file\",\"action\":\"ask\","
                                              "\"reason\":\"target is outside the workspace\","
                                              "\"risk\":\"high\",\"target_path\":\"/tmp/outside.txt\","
                                              "\"resolution\":\"allow\","
                                              "\"resolution_source\":\"session_grant\"}"},
  };
  auto const valid_session_grant = ava::session::validate_session_replay(valid_session_grant_entries);
  expect(valid_session_grant.ok() && valid_session_grant.issues.empty(), "session replay validator accepts permission outcomes resolved by session grants");

  std::vector<ava::session::SessionEntry> const invalid_permission_entries = {
      ava::session::SessionEntry{.id = "permission_invalid",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"operation\":\"not-real\",\"mode\":\"build\","
                                              "\"tool_name\":\"read_file\",\"action\":\"allow\","
                                              "\"reason\":\"bad\",\"resolution\":\"allow\","
                                              "\"resolution_source\":\"policy\"}"},
  };
  auto const invalid_permission = ava::session::validate_session_replay(invalid_permission_entries);
  expect(!invalid_permission.ok() && ava::tests::session_replay_has_issue(invalid_permission, ava::session::SessionReplayIssueKind::InvalidPermissionDecision),
         "session replay validator flags malformed permission audit decisions");

  std::vector<ava::session::SessionEntry> const invalid_risk_entries = {
      ava::session::SessionEntry{.id = "permission_invalid_risk",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"permission_request_id\":\"permreq_bad\","
                                              "\"operation\":\"read\",\"mode\":\"build\","
                                              "\"tool_name\":\"read_file\",\"action\":\"allow\","
                                              "\"reason\":\"bad\",\"risk\":\"extreme\","
                                              "\"resolution\":\"allow\",\"resolution_source\":\"policy\"}"},
  };
  auto const invalid_risk = ava::session::validate_session_replay(invalid_risk_entries);
  expect(!invalid_risk.ok() && ava::tests::session_replay_has_issue(invalid_risk, ava::session::SessionReplayIssueKind::InvalidPermissionDecision),
         "session replay validator flags malformed permission risk values");

  std::vector<ava::session::SessionEntry> const resolution_without_ask_entries = {
      ava::session::SessionEntry{.id = "permission_resolution_without_ask",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"operation\":\"edit\",\"mode\":\"build\","
                                              "\"tool_name\":\"write_file\",\"action\":\"ask\","
                                              "\"reason\":\"target is outside the workspace\","
                                              "\"target_path\":\"/tmp/outside.txt\","
                                              "\"resolution\":\"allow\",\"resolution_source\":\"resolver\"}"},
  };
  auto const resolution_without_ask = ava::session::validate_session_replay(resolution_without_ask_entries);
  expect(!resolution_without_ask.ok() &&
             ava::tests::session_replay_has_issue(resolution_without_ask, ava::session::SessionReplayIssueKind::PermissionResolutionWithoutAsk),
         "session replay validator flags resolver outcomes without earlier ask prompts");

  auto mismatched_permission_id_entries = valid_permission_entries;
  mismatched_permission_id_entries.back().data_json =
      "{\"permission_request_id\":\"permreq_other\",\"operation\":\"edit\",\"mode\":\"build\","
      "\"tool_name\":\"write_file\",\"action\":\"ask\","
      "\"reason\":\"target is outside the workspace\","
      "\"risk\":\"high\",\"target_path\":\"/tmp/outside.txt\","
      "\"resolution\":\"deny\",\"resolution_source\":\"resolver\"}";
  auto const mismatched_permission_id = ava::session::validate_session_replay(mismatched_permission_id_entries);
  expect(!mismatched_permission_id.ok() &&
             ava::tests::session_replay_has_issue(mismatched_permission_id, ava::session::SessionReplayIssueKind::PermissionResolutionWithoutAsk),
         "session replay validator pairs permission resolver outcomes by stable request id when present");

  std::vector<ava::session::SessionEntry> const unresolved_permission_entries = {
      ava::session::SessionEntry{.id = "permission_unresolved",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"operation\":\"network.fetch\",\"mode\":\"build\","
                                              "\"tool_name\":\"webfetch\",\"action\":\"ask\","
                                              "\"reason\":\"network fetch requires explicit approval\","
                                              "\"command\":\"https://example.com\","
                                              "\"resolution_source\":\"policy\"}"},
  };
  auto const unresolved_permission = ava::session::validate_session_replay(unresolved_permission_entries);
  expect(!unresolved_permission.ok() &&
             ava::tests::session_replay_has_issue(unresolved_permission, ava::session::SessionReplayIssueKind::UnresolvedPermissionPrompt),
         "session replay validator flags ask permission prompts without outcomes");

  auto valid_compaction_entries = valid_entries;
  valid_compaction_entries.push_back(ava::session::SessionEntry{.id = "compaction",
                                                                .parent_id = "tool_result",
                                                                .type = ava::session::EntryType::Compaction,
                                                                .timestamp = "2026-04-29T00:00:05Z",
                                                                .data_json =
                                                                    "{\"trigger\":\"manual\",\"status\":\"recorded\",\"summary_unavailable\":false,"
                                                                    "\"summary\":\"read_file returned note contents\","
                                                                    "\"instructions\":\"keep the file result\",\"model\":\"gpt-5.5\","
                                                                    "\"threshold_tokens\":100,\"estimated_tokens\":125,"
                                                                    "\"keep_recent_tokens\":64,\"keep_recent_messages\":4,\"max_summary_bytes\":65536}"});
  auto const valid_compaction =
      ava::session::validate_session_replay(valid_compaction_entries, ava::session::SessionReplayValidationOptions{.require_structured_tool_results = true});
  expect(valid_compaction.ok() && valid_compaction.issues.empty(), "session replay validator accepts compaction after resolved tool state");

  auto invalid_compaction_entries = valid_entries;
  invalid_compaction_entries.push_back(ava::session::SessionEntry{.id = "compaction_invalid",
                                                                  .parent_id = "tool_result",
                                                                  .type = ava::session::EntryType::Compaction,
                                                                  .timestamp = "2026-04-29T00:00:05Z",
                                                                  .data_json = "{\"status\":\"recorded\","
                                                                               "\"summary_unavailable\":false,"
                                                                               "\"summary\":\"\"}"});
  auto const invalid_compaction = ava::session::validate_session_replay(invalid_compaction_entries);
  expect(!invalid_compaction.ok() && ava::tests::session_replay_has_issue(invalid_compaction, ava::session::SessionReplayIssueKind::InvalidCompactionEntry),
         "session replay validator flags compaction entries without durable summaries");

  auto malformed_compaction_metadata_entries = valid_entries;
  malformed_compaction_metadata_entries.push_back(ava::session::SessionEntry{.id = "compaction_malformed_metadata",
                                                                             .parent_id = "tool_result",
                                                                             .type = ava::session::EntryType::Compaction,
                                                                             .timestamp = "2026-04-29T00:00:05Z",
                                                                             .data_json = "{\"status\":\"recorded\",\"summary\":\"durable summary\","
                                                                                          "\"summary_unavailable\":false,\"threshold_tokens\":1.5}"});
  auto const malformed_compaction_metadata = ava::session::validate_session_replay(malformed_compaction_metadata_entries);
  expect(!malformed_compaction_metadata.ok() &&
             ava::tests::session_replay_has_issue(malformed_compaction_metadata, ava::session::SessionReplayIssueKind::InvalidCompactionEntry),
         "session replay validator flags non-integer compaction token metadata");

  std::vector<std::string> const malformed_additive_compaction_metadata = {R"({"summary":"durable","provider":""})",
                                                                           R"({"summary":"durable","reason":"other"})",
                                                                           R"({"summary":"durable","threshold_type":"bytes"})",
                                                                           R"({"summary":"durable","configured_threshold_tokens":1.5})",
                                                                           R"({"summary":"durable","configured_threshold_percent":1e2})",
                                                                           R"({"summary":"durable","active_pre_compaction_tokens":false})",
                                                                           R"({"summary":"durable","retained_tokens":-1})",
                                                                           R"({"summary":"durable","post_compaction_estimated_tokens":null})",
                                                                           R"({"summary":"durable","keep_recent_turns":"2"})",
                                                                           R"({"summary":"durable","recent_context_omitted":0})",
                                                                           R"({"summary":"durable","overflow_retry_outcome":7})",
                                                                           R"({"summary":"durable","overflow_retry_outcome":"other"})"};
  bool rejects_malformed_additive_metadata = true;
  for (std::size_t index = 0; index < malformed_additive_compaction_metadata.size(); ++index)
  {
    auto malformed = valid_entries;
    malformed.push_back(ava::session::SessionEntry{.id = "compaction_additive_" + std::to_string(index),
                                                   .parent_id = "tool_result",
                                                   .type = ava::session::EntryType::Compaction,
                                                   .timestamp = "2026-04-29T00:00:05Z",
                                                   .data_json = malformed_additive_compaction_metadata[index]});
    auto const validation = ava::session::validate_session_replay(malformed);
    rejects_malformed_additive_metadata = rejects_malformed_additive_metadata && !validation.ok() &&
                                          ava::tests::session_replay_has_issue(validation, ava::session::SessionReplayIssueKind::InvalidCompactionEntry);
  }
  expect(rejects_malformed_additive_metadata, "additive compaction metadata remains optional but is strictly typed and enum-validated whenever present");

  auto unresolved_tool_compaction_entries = valid_entries;
  unresolved_tool_compaction_entries.pop_back();
  unresolved_tool_compaction_entries.push_back(ava::session::SessionEntry{.id = "compaction_before_tool_result",
                                                                          .parent_id = "tool_call",
                                                                          .type = ava::session::EntryType::Compaction,
                                                                          .timestamp = "2026-04-29T00:00:04Z",
                                                                          .data_json = "{\"summary\":\"tool call still pending\"}"});
  auto const unresolved_tool_compaction = ava::session::validate_session_replay(unresolved_tool_compaction_entries);
  expect(!unresolved_tool_compaction.ok() &&
             ava::tests::session_replay_has_issue(unresolved_tool_compaction, ava::session::SessionReplayIssueKind::CompactionWithUnresolvedToolCall),
         "session replay validator flags compaction before unresolved tool results");

  std::vector<ava::session::SessionEntry> const unresolved_permission_compaction_entries = {
      unresolved_permission_entries[0],
      ava::session::SessionEntry{.id = "compaction_before_permission_resolution",
                                 .parent_id = "permission_unresolved",
                                 .type = ava::session::EntryType::Compaction,
                                 .timestamp = "2026-04-29T00:00:01Z",
                                 .data_json = "{\"summary\":\"permission prompt still pending\"}"},
  };
  auto const unresolved_permission_compaction = ava::session::validate_session_replay(unresolved_permission_compaction_entries);
  expect(
      !unresolved_permission_compaction.ok() &&
          ava::tests::session_replay_has_issue(unresolved_permission_compaction, ava::session::SessionReplayIssueKind::CompactionWithUnresolvedPermissionPrompt),
      "session replay validator flags compaction before unresolved permission decisions");

  std::vector<ava::session::SessionEntry> const valid_model_reasoning_entries = {
      ava::session::SessionEntry{.id = "model_start",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::SessionStart,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"mode\":\"build\",\"provider\":\"openai\","
                                              "\"model\":\"gpt-5.5\",\"prompt_override\":false,"
                                              "\"context_sources\":0,\"supports_reasoning\":true}"},
      ava::session::SessionEntry{.id = "model_change",
                                 .parent_id = "model_start",
                                 .type = ava::session::EntryType::ModelChange,
                                 .timestamp = "2026-04-29T00:00:01Z",
                                 .data_json = "{\"previous_provider\":\"openai\","
                                              "\"previous_model\":\"gpt-5.5\","
                                              "\"provider\":\"kimi\",\"model\":\"kimi-k2-thinking\","
                                              "\"supports_reasoning\":true,\"max_output_tokens\":8192}"},
      ava::session::SessionEntry{.id = "reasoning_change",
                                 .parent_id = "model_change",
                                 .type = ava::session::EntryType::ReasoningChange,
                                 .timestamp = "2026-04-29T00:00:02Z",
                                 .data_json = "{\"provider\":\"kimi\",\"model\":\"kimi-k2-thinking\","
                                              "\"format\":\"reasoning_content\",\"enabled\":true,"
                                              "\"level\":\"enabled\"}"},
      ava::session::SessionEntry{.id = "reasoning_block",
                                 .parent_id = "reasoning_change",
                                 .type = ava::session::EntryType::ReasoningBlock,
                                 .timestamp = "2026-04-29T00:00:03Z",
                                 .data_json = "{\"provider\":\"kimi\",\"model\":\"kimi-k2-thinking\","
                                              "\"format\":\"reasoning_content\",\"text\":\"reasoned\","
                                              "\"redacted\":false}"},
  };
  auto const valid_model_reasoning = ava::session::validate_session_replay(valid_model_reasoning_entries);
  expect(valid_model_reasoning.ok() && valid_model_reasoning.issues.empty(), "session replay validator accepts durable model and reasoning metadata");

  auto valid_native_reasoning_item_entries = valid_model_reasoning_entries;
  valid_native_reasoning_item_entries[0].data_json =
      "{\"mode\":\"build\",\"provider\":\"anthropic\",\"model\":\"claude-test\",\"prompt_override\":false,"
      "\"context_sources\":0,\"supports_reasoning\":true}";
  valid_native_reasoning_item_entries[1].data_json =
      "{\"previous_provider\":\"anthropic\",\"previous_model\":\"claude-test\",\"provider\":\"openai\",\"model\":\"gpt-5.5\","
      "\"supports_reasoning\":true,\"max_output_tokens\":8192}";
  valid_native_reasoning_item_entries[2].data_json =
      "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\",\"enabled\":true,\"level\":\"high\"}";
  valid_native_reasoning_item_entries[3].data_json =
      "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\",\"text\":\"reasoned\",\"redacted\":false,"
      "\"native_item_json\":\"{\\\"id\\\":\\\"rs_session\\\",\\\"type\\\":\\\"reasoning\\\",\\\"summary\\\":[],\\\"encrypted_content\\\":\\\"cipher-"
      "session\\\"}\"}";
  auto const valid_native_reasoning_item = ava::session::validate_session_replay(valid_native_reasoning_item_entries);
  expect(valid_native_reasoning_item.ok(), "session replay validator accepts optional private native reasoning item objects");

  auto invalid_native_reasoning_item_entries = valid_native_reasoning_item_entries;
  invalid_native_reasoning_item_entries[3].data_json =
      "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\",\"text\":\"reasoned\",\"redacted\":false,"
      "\"native_item_json\":\"{\\\"type\\\":\\\"message\\\"}\"}";
  auto const invalid_native_reasoning_item = ava::session::validate_session_replay(invalid_native_reasoning_item_entries);
  expect(!invalid_native_reasoning_item.ok() &&
             ava::tests::session_replay_has_issue(invalid_native_reasoning_item, ava::session::SessionReplayIssueKind::InvalidReasoningEntry),
         "session replay validator rejects private native items that are not reasoning objects");

  auto missing_native_reasoning_id_entries = valid_native_reasoning_item_entries;
  missing_native_reasoning_id_entries[3].data_json =
      "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\",\"text\":\"reasoned\",\"redacted\":false,"
      "\"native_item_json\":\"{\\\"type\\\":\\\"reasoning\\\",\\\"summary\\\":[]}\"}";
  auto const missing_native_reasoning_id = ava::session::validate_session_replay(missing_native_reasoning_id_entries);
  expect(!missing_native_reasoning_id.ok() &&
             ava::tests::session_replay_has_issue(missing_native_reasoning_id, ava::session::SessionReplayIssueKind::InvalidReasoningEntry),
         "current-version sessions reject native reasoning metadata without an id");

  auto empty_native_reasoning_id_entries = valid_native_reasoning_item_entries;
  empty_native_reasoning_id_entries[3].data_json =
      "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\",\"text\":\"reasoned\",\"redacted\":false,"
      "\"native_item_json\":\"{\\\"id\\\":\\\"\\\",\\\"type\\\":\\\"reasoning\\\",\\\"summary\\\":[]}\"}";
  auto const empty_native_reasoning_id = ava::session::validate_session_replay(empty_native_reasoning_id_entries);
  expect(!empty_native_reasoning_id.ok() &&
             ava::tests::session_replay_has_issue(empty_native_reasoning_id, ava::session::SessionReplayIssueKind::InvalidReasoningEntry),
         "current-version sessions reject native reasoning metadata with an empty id");

  auto missing_native_reasoning_summary_entries = valid_native_reasoning_item_entries;
  missing_native_reasoning_summary_entries[3].data_json =
      "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\",\"text\":\"reasoned\",\"redacted\":false,"
      "\"native_item_json\":\"{\\\"id\\\":\\\"rs_missing_summary\\\",\\\"type\\\":\\\"reasoning\\\"}\"}";
  auto const missing_native_reasoning_summary = ava::session::validate_session_replay(missing_native_reasoning_summary_entries);
  expect(!missing_native_reasoning_summary.ok() &&
             ava::tests::session_replay_has_issue(missing_native_reasoning_summary, ava::session::SessionReplayIssueKind::InvalidReasoningEntry),
         "current-version sessions reject native reasoning metadata without a summary array");

  auto scalar_native_reasoning_summary_entries = valid_native_reasoning_item_entries;
  scalar_native_reasoning_summary_entries[3].data_json =
      "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\",\"text\":\"reasoned\",\"redacted\":false,"
      "\"native_item_json\":\"{\\\"id\\\":\\\"rs_scalar_summary\\\",\\\"type\\\":\\\"reasoning\\\",\\\"summary\\\":[\\\"not-an-object\\\"]}\"}";
  auto const scalar_native_reasoning_summary = ava::session::validate_session_replay(scalar_native_reasoning_summary_entries);
  expect(!scalar_native_reasoning_summary.ok() &&
             ava::tests::session_replay_has_issue(scalar_native_reasoning_summary, ava::session::SessionReplayIssueKind::InvalidReasoningEntry),
         "current-version sessions reject native reasoning summary arrays with scalar items");

  auto malformed_native_reasoning_summary_entries = valid_native_reasoning_item_entries;
  malformed_native_reasoning_summary_entries[3].data_json =
      "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\",\"text\":\"reasoned\",\"redacted\":false,"
      "\"native_item_json\":\"{\\\"id\\\":\\\"rs_malformed_summary\\\",\\\"type\\\":\\\"reasoning\\\",\\\"summary\\\":[{\\\"type\\\":\\\"summary_text\\\"}]}"
      "\"}";
  auto const malformed_native_reasoning_summary = ava::session::validate_session_replay(malformed_native_reasoning_summary_entries);
  expect(!malformed_native_reasoning_summary.ok() &&
             ava::tests::session_replay_has_issue(malformed_native_reasoning_summary, ava::session::SessionReplayIssueKind::InvalidReasoningEntry),
         "current-version sessions reject native reasoning summary items without summary_text text");

  auto oversized_native_reasoning_item_entries = valid_native_reasoning_item_entries;
  auto const oversized_native_item =
      std::string("{\"id\":\"rs_oversized\",\"type\":\"reasoning\",\"summary\":[],\"opaque\":\"") + std::string(64U * 1024U, 'x') + "\"}";
  oversized_native_reasoning_item_entries[3].data_json =
      "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\",\"text\":\"reasoned\",\"redacted\":false,"
      "\"native_item_json\":\"" +
      ava::core::json::escape(oversized_native_item) + "\"}";
  auto const oversized_native_reasoning_item = ava::session::validate_session_replay(oversized_native_reasoning_item_entries);
  expect(!oversized_native_reasoning_item.ok() &&
             ava::tests::session_replay_has_issue(oversized_native_reasoning_item, ava::session::SessionReplayIssueKind::InvalidReasoningEntry),
         "current-version sessions reject native reasoning metadata beyond the provider-private byte bound");

  auto legacy_native_reasoning_item_entries = missing_native_reasoning_summary_entries;
  legacy_native_reasoning_item_entries[3].version = 0;
  auto const legacy_native_reasoning_item = ava::session::validate_session_replay(legacy_native_reasoning_item_entries);
  expect(legacy_native_reasoning_item.ok(),
         "legacy sessions retain readable fallback compatibility when explicitly present native reasoning metadata is malformed");

  std::vector<ava::session::SessionEntry> const invalid_model_start_entries = {
      ava::session::SessionEntry{.id = "bad_start",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::SessionStart,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"mode\":\"build\",\"model\":\"gpt-5.5\"}"},
  };
  auto const invalid_model_start = ava::session::validate_session_replay(invalid_model_start_entries);
  expect(!invalid_model_start.ok() && ava::tests::session_replay_has_issue(invalid_model_start, ava::session::SessionReplayIssueKind::InvalidModelEntry),
         "session replay validator flags session_start entries without provider/model metadata");

  auto invalid_model_change_entries = valid_model_reasoning_entries;
  invalid_model_change_entries[1].data_json =
      "{\"previous_provider\":\"anthropic\",\"previous_model\":\"claude\","
      "\"provider\":\"kimi\",\"model\":\"kimi-k2-thinking\"}";
  auto const invalid_model_change = ava::session::validate_session_replay(invalid_model_change_entries);
  expect(!invalid_model_change.ok() && ava::tests::session_replay_has_issue(invalid_model_change, ava::session::SessionReplayIssueKind::InvalidModelEntry),
         "session replay validator flags model_change entries whose previous model does not match active state");

  auto invalid_reasoning_change_entries = valid_model_reasoning_entries;
  invalid_reasoning_change_entries[2].data_json = "{\"provider\":\"kimi\",\"model\":\"kimi-k2-thinking\",\"enabled\":true}";
  auto const invalid_reasoning_change = ava::session::validate_session_replay(invalid_reasoning_change_entries);
  expect(!invalid_reasoning_change.ok() &&
             ava::tests::session_replay_has_issue(invalid_reasoning_change, ava::session::SessionReplayIssueKind::InvalidReasoningEntry),
         "session replay validator flags enabled reasoning_change entries without a level");

  auto mismatched_reasoning_change_entries = valid_model_reasoning_entries;
  mismatched_reasoning_change_entries[2].data_json = "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"enabled\":true,\"level\":\"low\"}";
  auto const mismatched_reasoning_change = ava::session::validate_session_replay(mismatched_reasoning_change_entries);
  expect(!mismatched_reasoning_change.ok() &&
             ava::tests::session_replay_has_issue(mismatched_reasoning_change, ava::session::SessionReplayIssueKind::InvalidReasoningEntry),
         "session replay validator flags reasoning_change entries for the wrong active model");

  auto invalid_reasoning_block_entries = valid_model_reasoning_entries;
  invalid_reasoning_block_entries[3].data_json =
      "{\"provider\":\"kimi\",\"model\":\"kimi-k2-thinking\",\"format\":\"reasoning_content\","
      "\"redacted\":false}";
  auto const invalid_reasoning_block = ava::session::validate_session_replay(invalid_reasoning_block_entries);
  expect(!invalid_reasoning_block.ok() &&
             ava::tests::session_replay_has_issue(invalid_reasoning_block, ava::session::SessionReplayIssueKind::InvalidReasoningEntry),
         "session replay validator flags reasoning_block entries without replayable content");

  auto mismatched_reasoning_block_entries = valid_model_reasoning_entries;
  mismatched_reasoning_block_entries[3].data_json =
      "{\"provider\":\"deepseek\",\"model\":\"deepseek-reasoner\",\"format\":\"reasoning_content\","
      "\"text\":\"contradictory\",\"redacted\":false}";
  auto const mismatched_reasoning_block = ava::session::validate_session_replay(mismatched_reasoning_block_entries);
  expect(!mismatched_reasoning_block.ok() &&
             ava::tests::session_replay_has_issue(mismatched_reasoning_block, ava::session::SessionReplayIssueKind::InvalidReasoningEntry),
         "session replay validator rejects reasoning_block provider/model metadata that contradicts the active model");

  auto legacy_reasoning_without_active_model = valid_model_reasoning_entries[3];
  legacy_reasoning_without_active_model.parent_id.clear();
  auto const unavailable_active_model = ava::session::validate_session_replay({legacy_reasoning_without_active_model});
  expect(unavailable_active_model.ok(), "session replay validator preserves legacy reasoning_block validation when no active model is available");

  expect(ava::session::to_string(ava::session::SessionReplayIssueKind::InvalidCompactionEntry) == "invalid_compaction_entry",
         "session replay issue kind names include compaction validation failures");
  expect(ava::session::to_string(ava::session::SessionReplayIssueKind::UnsupportedEntryVersion) == "unsupported_entry_version",
         "session replay issue kind names include entry version validation failures");
  expect(ava::session::to_string(ava::session::SessionReplayIssueKind::InvalidReasoningEntry) == "invalid_reasoning_entry",
         "session replay issue kind names include reasoning validation failures");
  expect(ava::session::to_string(ava::session::SessionReplayIssueKind::InvalidSessionMetadataEntry) == "invalid_session_metadata_entry" &&
             ava::session::to_string(ava::session::SessionReplayIssueKind::InvalidBranchSummaryEntry) == "invalid_branch_summary_entry",
         "session replay issue kind names include tree metadata validation failures");
}

}  // namespace session_tests
