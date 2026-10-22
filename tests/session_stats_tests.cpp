#include "sys.h"
#include "tests/session_test_declarations.h"
#include "tests/support/test_harness.h"
#include "ava/session/Stats.h"

#include <climits>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace session_tests {
void test_session_stats_helper()
{
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{
          .id = "start_1", .parent_id = "", .type = ava::session::EntryType::SessionStart, .timestamp = "2026-04-29T00:00:00Z", .data_json = "{}"},
      ava::session::SessionEntry{.id = "user_1",
                                 .parent_id = "start_1",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-29T00:00:01Z",
                                 .data_json = "{\"usage\":{\"input_tokens\":3,\"output_tokens\":2,"
                                              "\"reasoning_tokens\":1,\"cache_read_tokens\":2,"
                                              "\"total_tokens\":5,\"cost_usd\":0.001,"
                                              "\"source\":\"provider\"}}"},
      ava::session::SessionEntry{.id = "user_1_replay",
                                 .parent_id = "start_1",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-29T00:00:01Z",
                                 .data_json = "{\"text\":\"hidden replay\",\"internal_replay\":true,"
                                              "\"replay_of\":\"user_1\",\"reason\":\"test\"}"},
      ava::session::SessionEntry{.id = "assistant_1",
                                 .parent_id = "user_1",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-04-29T00:00:02Z",
                                 .data_json = "{\"usage\":{\"input_tokens\":1,\"output_tokens\":4,"
                                              "\"cache_write_tokens\":3,\"total_tokens\":5,"
                                              "\"estimated_input_bytes\":10,\"estimated_output_bytes\":20,"
                                              "\"estimated_total_bytes\":30,"
                                              "\"cost_usd\":0.0025,\"estimated\":true}}"},
      ava::session::SessionEntry{.id = "reasoning_1",
                                 .parent_id = "assistant_1",
                                 .type = ava::session::EntryType::ReasoningBlock,
                                 .timestamp = "2026-04-29T00:00:02Z",
                                 .data_json = "{\"provider\":\"anthropic\",\"model\":\"claude\","
                                              "\"format\":\"anthropic_thinking\",\"text\":\"visible\","
                                              "\"signature\":\"secret-signature\"}"},
      ava::session::SessionEntry{.id = "reasoning_change_1",
                                 .parent_id = "reasoning_1",
                                 .type = ava::session::EntryType::ReasoningChange,
                                 .timestamp = "2026-04-29T00:00:02Z",
                                 .data_json = "{\"level\":\"high\"}"},
      ava::session::SessionEntry{.id = "mode_1",
                                 .parent_id = "reasoning_change_1",
                                 .type = ava::session::EntryType::ModeChange,
                                 .timestamp = "2026-04-29T00:00:03Z",
                                 .data_json = "{\"mode\":\"plan\"}"},
      ava::session::SessionEntry{.id = "compact_1",
                                 .parent_id = "mode_1",
                                 .type = ava::session::EntryType::Compaction,
                                 .timestamp = "2026-04-29T00:00:04Z",
                                 .data_json = "{\"summary\":\"prior\"}"},
      ava::session::SessionEntry{
          .id = "cancel_1", .parent_id = "compact_1", .type = ava::session::EntryType::Cancel, .timestamp = "2026-04-29T00:00:05Z", .data_json = "{}"},
      ava::session::SessionEntry{
          .id = "error_1", .parent_id = "cancel_1", .type = ava::session::EntryType::Error, .timestamp = "2026-04-29T00:00:06Z", .data_json = "{}"},
  };

  auto const stats = ava::session::compute_session_stats(entries);
  expect(stats->entry_count == entries.size() && stats->first_timestamp == "2026-04-29T00:00:00Z" && stats->last_timestamp == "2026-04-29T00:00:06Z",
         "session stats helper reports entry count and timestamps");
  expect(stats->counts.session_start == 1 && stats->counts.user_message == 1 && stats->counts.assistant_message == 1 && stats->counts.reasoning_block == 1 &&
             stats->counts.reasoning_change == 1 && stats->counts.mode_change == 1 && stats->counts.compaction == 1 && stats->counts.cancel == 1 &&
             stats->counts.error == 1,
         "session stats helper reports current counts without durable internal replay user messages");
  expect(stats->input_tokens && *stats->input_tokens == 3 && stats->output_tokens && *stats->output_tokens == 2 && stats->total_tokens &&
             *stats->total_tokens == 5,
         "session stats helper aggregates exact token fields only from provider usage");
  expect(stats->reasoning_tokens && *stats->reasoning_tokens == 1 && stats->cache_read_tokens && *stats->cache_read_tokens == 2 && !stats->cache_write_tokens,
         "session stats helper keeps estimated cache token fields out of exact totals");
  expect(stats->estimated_input_bytes && *stats->estimated_input_bytes == 10 && stats->estimated_output_bytes && *stats->estimated_output_bytes == 20 &&
             stats->estimated_total_bytes && *stats->estimated_total_bytes == 30,
         "session stats helper aggregates separate estimated byte totals");
  expect(stats->total_cost_usd && *stats->total_cost_usd > 0.0009L && *stats->total_cost_usd < 0.0011L,
         "session stats helper aggregates exact cost fields without estimated fallback cost");
  expect(stats->exact_usage_entries == 1 && stats->estimated_usage_entries == 1, "session stats helper counts exact and estimated usage entries");

  auto const empty_stats = ava::session::compute_session_stats({});
  expect(!empty_stats->input_tokens && !empty_stats->total_cost_usd,
         "session stats helper leaves token and cost totals absent when no entry JSON supplies them");
}

void test_session_stats_saturates_large_usage_and_costs()
{
  std::ostringstream maximum_cost;
  maximum_cost << std::scientific << std::setprecision(std::numeric_limits<long double>::max_digits10) << std::numeric_limits<long double>::max();
  auto make_usage_entry = [&](std::string id, std::string parent, long long input_tokens, long long total_tokens, std::string cost) {
    return ava::session::SessionEntry{.id = std::move(id),
                                      .parent_id = std::move(parent),
                                      .type = ava::session::EntryType::AssistantMessage,
                                      .timestamp = "2026-04-29T00:00:00Z",
                                      .data_json = "{\"usage\":{\"input_tokens\":" + std::to_string(input_tokens) + ",\"total_tokens\":" +
                                                   std::to_string(total_tokens) + ",\"cost_usd\":" + cost + ",\"source\":\"provider\"}}"};
  };
  std::vector<ava::session::SessionEntry> const entries = {
      make_usage_entry("large_usage_1", "", LLONG_MAX - 2, LLONG_MAX - 1, maximum_cost.str()),
      make_usage_entry("large_usage_2", "large_usage_1", 9, 7, "1.0"),
  };
  auto const stats = ava::session::compute_session_stats(entries);
  expect(stats && stats->input_tokens && *stats->input_tokens == LLONG_MAX && stats->total_tokens && *stats->total_tokens == LLONG_MAX &&
             stats->known_cost_usd && std::isfinite(*stats->known_cost_usd) && *stats->known_cost_usd == std::numeric_limits<long double>::max(),
         "session stats saturates near-LLONG_MAX token totals and overflowing finite cost aggregates");
}

void test_session_stats_omits_incomplete_cost_total()
{
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "assistant_priced",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"usage\":{\"input_tokens\":3,\"output_tokens\":2,"
                                              "\"total_tokens\":5,\"cost_usd\":0.001,"
                                              "\"source\":\"provider\"}}"},
      ava::session::SessionEntry{.id = "assistant_unpriced",
                                 .parent_id = "assistant_priced",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-04-29T00:00:01Z",
                                 .data_json = "{\"usage\":{\"input_tokens\":4,\"cache_read_tokens\":4,"
                                              "\"total_tokens\":4,\"source\":\"provider\"}}"},
  };

  auto const stats = ava::session::compute_session_stats(entries);
  expect(stats->exact_usage_entries == 2 && stats->estimated_usage_entries == 0, "session stats counts mixed exact usage entries");
  expect(stats->known_cost_usd && *stats->known_cost_usd > 0.0009L && *stats->known_cost_usd < 0.0011L,
         "session stats preserves the known portion of incomplete cost totals");
  expect(!stats->total_cost_usd && !stats->cost_complete && stats->unknown_cost_entries == 1,
         "session stats omits total cost when exact billable usage has unknown cost");
}

void test_session_stats_flags_legacy_assistant_tokens_without_cost()
{
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "assistant_legacy_tokens",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"text\":\"legacy\",\"input_tokens\":7,"
                                              "\"output_tokens\":3,\"total_tokens\":10}"},
  };

  auto const stats = ava::session::compute_session_stats(entries);
  expect(stats->input_tokens && *stats->input_tokens == 7 && stats->output_tokens && *stats->output_tokens == 3 && stats->total_tokens &&
             *stats->total_tokens == 10,
         "session stats still aggregates legacy top-level assistant token totals");
  expect(stats->exact_usage_entries == 0 && stats->estimated_usage_entries == 0, "legacy top-level assistant token stats do not masquerade as usage objects");
  expect(!stats->total_cost_usd && !stats->cost_complete && stats->unknown_cost_entries == 1,
         "legacy top-level assistant tokens without cost make cost stats incomplete");
}

void test_session_stats_projects_mixed_v3_v4_history()
{
  using ava::session::EntryType;
  using ava::session::SessionEntry;
  std::vector<SessionEntry> const entries = {
      SessionEntry{
          .id = "legacy_assistant",
          .parent_id = "",
          .type = EntryType::AssistantMessage,
          .timestamp = "2026-07-18T00:00:00Z",
          .data_json = "{\"text\":\"legacy\",\"usage\":{\"input_tokens\":1,\"output_tokens\":2,\"total_tokens\":3,\"source\":\"provider\",\"cost_usd\":0.1}}",
          .version = 3},
      SessionEntry{.id = "v4_text",
                   .parent_id = "legacy_assistant",
                   .type = EntryType::AssistantOutputItem,
                   .timestamp = "2026-07-18T00:00:01Z",
                   .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"turn_stats\",\"sequence\":0,\"kind\":\"text\",\"text\":\"one\",\"assistant_"
                                "phase\":\"commentary\"}"},
      SessionEntry{.id = "v4_reasoning",
                   .parent_id = "v4_text",
                   .type = EntryType::AssistantOutputItem,
                   .timestamp = "2026-07-18T00:00:02Z",
                   .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"turn_stats\",\"sequence\":1,\"kind\":\"reasoning\",\"text\":\"think\","
                                "\"format\":\"openai_responses\",\"redacted\":false}"},
      SessionEntry{.id = "v4_function",
                   .parent_id = "v4_reasoning",
                   .type = EntryType::AssistantOutputItem,
                   .timestamp = "2026-07-18T00:00:03Z",
                   .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"turn_stats\",\"sequence\":2,\"kind\":\"function_call\",\"call_id\":\"call_"
                                "stats\",\"name\":\"read_file\",\"arguments\":\"{}\"}"},
      SessionEntry{.id = "v4_commit",
                   .parent_id = "v4_function",
                   .type = EntryType::AssistantTurnCommit,
                   .timestamp = "2026-07-18T00:00:04Z",
                   .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"turn_stats\",\"item_count\":3,\"provider\":\"openai\",\"model\":\"gpt-5.5\","
                                "\"finish_reason\":\"tool_calls\",\"usage\":{\"input_tokens\":5,\"output_tokens\":6,\"reasoning_tokens\":4,\"total_tokens\":11,"
                                "\"source\":\"provider\",\"cost_usd\":0.2}}"},
      SessionEntry{.id = "v4_incomplete",
                   .parent_id = "v4_commit",
                   .type = EntryType::AssistantOutputItem,
                   .timestamp = "2026-07-18T00:00:05Z",
                   .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"turn_incomplete\",\"sequence\":0,\"kind\":\"text\",\"text\":\"must stay "
                                "invisible\",\"assistant_phase\":\"final_answer\"}"},
  };

  auto const stats = ava::session::compute_session_stats(entries);
  expect(stats && stats->entry_count == 4 && stats->first_timestamp == "2026-07-18T00:00:00Z" && stats->last_timestamp == "2026-07-18T00:00:04Z" &&
             stats->counts.assistant_message == 2 && stats->counts.reasoning_block == 1 && stats->counts.tool_call == 1 && stats->input_tokens &&
             *stats->input_tokens == 6 && stats->output_tokens && *stats->output_tokens == 8 && stats->reasoning_tokens && *stats->reasoning_tokens == 4 &&
             stats->total_tokens && *stats->total_tokens == 14 && stats->exact_usage_entries == 2 && stats->estimated_usage_entries == 0 &&
             stats->cost_complete && stats->total_cost_usd && *stats->total_cost_usd > 0.299L && *stats->total_cost_usd < 0.301L,
         "session stats project mixed v3/v4 committed turns once and exclude incomplete v4 staging");

  auto malformed_v4 = entries[1];
  malformed_v4.data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"turn_stats\",\"sequence\":0,\"kind\":\"text\"}";
  auto malformed_stats = ava::session::compute_session_stats({malformed_v4});
  expect(!malformed_stats && malformed_stats.error().message().find("malformed assistant-output") != std::string::npos,
         "session stats fail closed on malformed v4 classifier diagnostics instead of silently omitting them");
}

}  // namespace session_tests
