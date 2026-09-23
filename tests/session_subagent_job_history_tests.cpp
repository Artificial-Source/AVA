#include "sys.h"
#include "tests/session_test_declarations.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/agent/message_builder.h"
#include "ava/session/subagent_job_history.h"
#include "ava/session/validation.h"

#include <string>
#include <vector>

namespace session_tests {
namespace {

ava::session::SubagentJobHistoryRecord start_record(std::string job_id = "job_one", std::string parent = "session_parent")
{
  ava::session::SubagentJobHistoryRecord record;
  record.phase = ava::session::SubagentJobHistoryPhase::Start;
  record.job_id = std::move(job_id);
  record.task_id = "session_child";
  record.parent_session_id = std::move(parent);
  record.child_session_id = "session_child";
  record.delivery_id = "delivery_one";
  record.mode = ava::session::SubagentJobHistoryMode::Background;
  record.execution = ava::session::SubagentJobHistoryExecution::Starting;
  record.started_at = "2026-05-01T00:00:00Z";
  record.updated_at = "2026-05-01T00:00:00Z";
  return record;
}

ava::session::SubagentJobHistoryRecord terminal_record(std::string job_id = "job_one", std::string parent = "session_parent")
{
  auto record = start_record(std::move(job_id), std::move(parent));
  record.phase = ava::session::SubagentJobHistoryPhase::Terminal;
  record.execution = ava::session::SubagentJobHistoryExecution::Completed;
  record.updated_at = "2026-05-01T00:00:01Z";
  record.terminal_at = "2026-05-01T00:00:01Z";
  record.summary = "bounded summary";
  record.tool_calls = 2;
  return record;
}

}  // namespace

void test_subagent_job_history_round_trip_malformed_bounds_and_legacy()
{
  auto start = ava::session::make_subagent_job_history_entry(start_record());
  auto terminal = ava::session::make_subagent_job_history_entry(terminal_record());
  expect(start && terminal, "subagent job history entries serialize");
  if (!start || !terminal)
    return;

  auto parsed_start = ava::session::parse_subagent_job_history_entry(*start);
  auto parsed_terminal = ava::session::parse_subagent_job_history_entry(*terminal);
  expect(parsed_start && *parsed_start && (*parsed_start)->phase == ava::session::SubagentJobHistoryPhase::Start && (*parsed_start)->job_id == "job_one",
         "start history round-trips");
  expect(parsed_terminal && *parsed_terminal && (*parsed_terminal)->summary == "bounded summary" && (*parsed_terminal)->tool_calls == 2,
         "terminal history round-trips bounded summary");

  auto failed_record = terminal_record("job_failed");
  failed_record.execution = ava::session::SubagentJobHistoryExecution::Failed;
  failed_record.summary = std::nullopt;
  failed_record.error = "subagent job failed";
  auto canceled_record = terminal_record("job_canceled");
  canceled_record.execution = ava::session::SubagentJobHistoryExecution::Canceled;
  canceled_record.summary = std::nullopt;
  auto failed = ava::session::make_subagent_job_history_entry(failed_record);
  auto canceled = ava::session::make_subagent_job_history_entry(canceled_record);
  expect(failed && canceled && ava::session::validate_session_replay({*failed, *canceled}).ok(), "failed and canceled terminal history records validate");

  auto const start_validation = ava::session::validate_session_replay({*start});
  auto const terminal_validation = ava::session::validate_session_replay({*start, *terminal});
  expect(start_validation.ok() && terminal_validation.ok(), "valid subagent_job history entries replay-validate");

  ava::session::SessionEntry legacy{.id = "entry_legacy",
                                    .parent_id = "",
                                    .type = ava::session::EntryType::SessionMetadata,
                                    .timestamp = "2026-04-27T00:00:00Z",
                                    .data_json = "{\"schema_version\":1,\"name\":\"Investigate auth flow\"}"};
  expect(ava::session::validate_session_replay({legacy}).ok() && !ava::session::session_metadata_has_subagent_job_history(legacy),
         "legacy session_metadata without subagent_job remains valid");

  auto malformed = *start;
  malformed.data_json = "{\"schema_version\":1,\"subagent_job\":{\"schema_version\":1,\"phase\":\"start\"}}";
  auto const malformed_validation = ava::session::validate_session_replay({malformed});
  expect(!malformed_validation.ok() &&
             ava::tests::session_replay_has_issue(malformed_validation, ava::session::SessionReplayIssueKind::InvalidSessionMetadataEntry),
         "missing identity fields fail closed");

  auto bad_enum = *start;
  bad_enum.data_json =
      "{\"schema_version\":1,\"subagent_job\":{\"schema_version\":1,\"phase\":\"start\",\"job_id\":\"job_one\",\"task_id\":\"session_child\","
      "\"parent_session_id\":\"session_parent\",\"child_session_id\":\"session_child\",\"delivery_id\":\"delivery_one\",\"mode\":\"sideways\","
      "\"execution\":\"starting\",\"started_at\":\"2026-05-01T00:00:00Z\",\"updated_at\":\"2026-05-01T00:00:00Z\"}}";
  expect(!ava::session::validate_session_replay({bad_enum}).ok(), "invalid mode enum fails closed");

  auto oversized = start_record();
  oversized.job_id = std::string(97, 'a');
  expect(!ava::session::make_subagent_job_history_entry(oversized), "oversized job_id is rejected");

  auto path_id = start_record();
  path_id.job_id = "job/../escape";
  expect(!ava::session::make_subagent_job_history_entry(path_id), "path-like job_id is rejected");

  auto huge_summary = terminal_record();
  huge_summary.summary = std::string(ava::session::kMaxSubagentJobHistorySummaryBytes + 1, 'x');
  expect(!ava::session::make_subagent_job_history_entry(huge_summary), "oversized summary is rejected");

  auto non_object = *start;
  non_object.data_json = "{\"schema_version\":1,\"subagent_job\":\"nope\"}";
  expect(!ava::session::validate_session_replay({non_object}).ok(), "non-object subagent_job fails closed");

  auto future = *start;
  future.data_json =
      "{\"schema_version\":1,\"subagent_job\":{\"schema_version\":2,\"phase\":\"start\",\"job_id\":\"job_one\",\"task_id\":\"session_child\","
      "\"parent_session_id\":\"session_parent\",\"child_session_id\":\"session_child\",\"delivery_id\":\"delivery_one\",\"mode\":\"background\","
      "\"execution\":\"starting\",\"started_at\":\"2026-05-01T00:00:00Z\",\"updated_at\":\"2026-05-01T00:00:00Z\"}}";
  expect(!ava::session::validate_session_replay({future}).ok(), "nested schema_version 2 fails closed");
}

void test_subagent_job_history_projection_fork_filter_and_unmatched_start()
{
  auto start = ava::session::make_subagent_job_history_entry(start_record("job_open"));
  auto done_start = ava::session::make_subagent_job_history_entry(start_record("job_done"));
  auto done_terminal = ava::session::make_subagent_job_history_entry(terminal_record("job_done"));
  auto other = ava::session::make_subagent_job_history_entry(terminal_record("job_other", "session_fork"));
  expect(start && done_start && done_terminal && other, "projection fixtures serialize");
  if (!start || !done_start || !done_terminal || !other)
    return;

  auto projected = ava::session::project_subagent_job_history("session_parent", {*start, *done_start, *done_terminal, *other});
  expect(projected.size() == 2 && projected[0].unmatched_start && projected[0].record.job_id == "job_open" &&
             projected[0].record.execution == ava::session::SubagentJobHistoryExecution::Interrupted && !projected[1].unmatched_start &&
             projected[1].record.job_id == "job_done" && projected[1].record.summary == "bounded summary",
         "unmatched starts become interrupted/unknown and foreign parent ids are ignored");

  auto fork_view = ava::session::project_subagent_job_history("session_fork", {*start, *done_terminal, *other});
  expect(fork_view.size() == 1 && fork_view[0].record.job_id == "job_other", "forks do not inherit control of another parent identity");

  std::vector<ava::session::SessionEntry> many;
  for (int index = 0; index < 70; ++index)
  {
    auto entry = ava::session::make_subagent_job_history_entry(terminal_record("job_" + std::to_string(index)));
    expect(static_cast<bool>(entry), "bounded projection fixture serializes");
    if (entry)
      many.push_back(std::move(*entry));
  }
  auto bounded = ava::session::project_subagent_job_history("session_parent", many);
  expect(
      bounded.size() == ava::session::kMaxProjectedSubagentJobHistory && bounded.front().record.job_id == "job_6" && bounded.back().record.job_id == "job_69",
      "display projection keeps only the latest 64 jobs");
}

void test_subagent_job_history_is_excluded_from_provider_replay()
{
  auto terminal = ava::session::make_subagent_job_history_entry(terminal_record());
  expect(static_cast<bool>(terminal), "provider exclusion fixture serializes");
  if (!terminal)
    return;
  std::vector<ava::session::SessionEntry> entries = {
      ava::session::SessionEntry{.id = "entry_user",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-05-01T00:00:00Z",
                                 .data_json = "{\"text\":\"hello\"}"},
      std::move(*terminal),
      ava::session::SessionEntry{.id = "entry_assistant",
                                 .parent_id = "entry_user",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-05-01T00:00:02Z",
                                 .data_json = "{\"text\":\"ok\"}"},
  };
  auto messages = ava::agent::build_provider_messages_from_entries(entries);
  expect(messages.has_value(), messages ? "job history is not injected into provider messages" : messages.error().format());
  if (!messages)
    return;
  std::string joined;
  for (auto const& message : *messages)
  {
    joined += message.content;
    for (auto const& part : message.content_parts)
      joined += part.text;
  }
  expect(joined.find("bounded summary") == std::string::npos && joined.find("job_one") == std::string::npos && joined.find("hello") != std::string::npos,
         "provider replay keeps user/assistant text and omits job history fields");
}

}  // namespace session_tests
