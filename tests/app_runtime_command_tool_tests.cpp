#include "sys.h"
#include "tests/app_runtime_test_declarations.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/runtime_event_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/app/commands.h"
#include "ava/app/interactive.h"
#include "ava/app/runtime/Session.h"
#include "ava/agent/agent_loop.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/event_state.h"
#include "ava/session/compaction.h"
#include "ava/session/record.h"
#include "ava/core/result.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_runtime_tests {

using namespace ava::tests;

// Exercise tool command dispatch while allowing each command and callback to acquire its own session access.
//
// unlocked_session must be unlocked on entry and supplies session state; workspace identifies the fixture project.
void app_command_dispatcher_tool_part(ava::app::runtime::session_ts& unlocked_session, std::filesystem::path const& workspace)
{
  std::vector<ava::event::RuntimeEvent> command_tool_events;
  auto glob = ava::app::run_command(
      unlocked_session, ava::app::CommandRequest{.command = "/glob **/*.cpp", .event_sink = [&command_tool_events](ava::event::RuntimeEvent const& event) {
                                             command_tool_events.push_back(event);
                                             return ava::core::VoidResult{};
                                           }});
  expect(glob && glob->handled && !glob->output.empty() && glob->output[0].find("src/main.cpp") != std::string::npos,
         "command dispatcher /glob runs existing safe file search command");
  expect(glob && glob->tool_timeline.size() == 2 && glob->tool_timeline[0].status == ava::agent::ToolTimelineStatus::Running &&
             glob->tool_timeline[1].status == ava::agent::ToolTimelineStatus::Success &&
             glob->tool_timeline[1].structured_result_json.find("\"status\":\"success\"") != std::string::npos && glob->tool_timeline[1].total_matches,
         "command dispatcher records running and completed timeline entries with structured result metadata");
  auto const* glob_result = command_tool_events.size() == 2 ? ava::tests::runtime_event_as<ava::event::ToolResultEvent>(command_tool_events[1]) : nullptr;
  expect(glob_result && !glob_result->payload.structured_result_json.empty() &&
             glob_result->payload.structured_result_json.find("\"tool\":\"glob\"") != std::string::npos && glob_result->payload.total_matches > 0,
         "command dispatcher emits structured tool result runtime events");

  auto const fallback_timeline = ava::app::tool_timeline_for_tui({
      {.status = ava::agent::ToolTimelineStatus::Running,
       .call_id = "running-only",
       .name = "read",
       .argument_summary = "pending.txt",
       .arguments_json = "{\"path\":\"pending.txt\"}"},
      {.status = ava::agent::ToolTimelineStatus::Running,
       .call_id = "success",
       .name = "write",
       .argument_summary = "src/main.cpp",
       .arguments_json = "{\"path\":\"src/main.cpp\"}"},
      {.status = ava::agent::ToolTimelineStatus::Running,
       .call_id = "error",
       .name = "bash",
       .argument_summary = "git push origin main",
       .arguments_json = "{\"command\":\"git push origin main\"}"},
      {.status = ava::agent::ToolTimelineStatus::Success, .call_id = "success", .name = "write", .result_summary = "wrote file"},
      {.status = ava::agent::ToolTimelineStatus::Error, .call_id = "error", .name = "bash", .result_summary = "permission denied"},
      {.status = ava::agent::ToolTimelineStatus::Running,
       .call_id = "argument-conflict",
       .name = "write",
       .argument_summary = "first running summary",
       .arguments_json = "{\"path\":\"first-running.json\"}"},
      {.status = ava::agent::ToolTimelineStatus::Running,
       .call_id = "argument-conflict",
       .name = "write",
       .argument_summary = "later running summary",
       .arguments_json = "{\"path\":\"later-running.json\"}"},
      {.status = ava::agent::ToolTimelineStatus::Success,
       .call_id = "argument-conflict",
       .name = "write",
       .argument_summary = "settled summary",
       .arguments_json = "{\"path\":\"settled.json\"}"},
      {.status = ava::agent::ToolTimelineStatus::Running, .call_id = "argument-independent", .name = "write", .argument_summary = "first summary only"},
      {.status = ava::agent::ToolTimelineStatus::Running,
       .call_id = "argument-independent",
       .name = "write",
       .argument_summary = "later summary",
       .arguments_json = "{\"path\":\"first-json-for-field\"}"},
      {.status = ava::agent::ToolTimelineStatus::Success,
       .call_id = "argument-independent",
       .name = "write",
       .argument_summary = "settled independent summary",
       .arguments_json = "{\"path\":\"settled-independent.json\"}"},
      {.status = ava::agent::ToolTimelineStatus::Running, .name = "empty-start", .argument_summary = "first empty id"},
      {.status = ava::agent::ToolTimelineStatus::Success, .name = "empty-result", .result_summary = "second empty id"},
  });
  expect(fallback_timeline.size() == 7 && fallback_timeline[0].call_id == "running-only" &&
             fallback_timeline[0].status == ava::tui::ToolTimelineStatus::Running && fallback_timeline[1].call_id == "success" &&
             fallback_timeline[1].status == ava::tui::ToolTimelineStatus::Success && fallback_timeline[1].argument_summary == "src/main.cpp" &&
             fallback_timeline[1].arguments_json == "{\"path\":\"src/main.cpp\"}" && fallback_timeline[2].call_id == "error" &&
             fallback_timeline[2].status == ava::tui::ToolTimelineStatus::Error && fallback_timeline[2].argument_summary == "git push origin main" &&
             fallback_timeline[2].arguments_json == "{\"command\":\"git push origin main\"}" && fallback_timeline[3].call_id == "argument-conflict" &&
             fallback_timeline[3].argument_summary == "first running summary" && fallback_timeline[3].arguments_json == "{\"path\":\"first-running.json\"}" &&
             fallback_timeline[4].call_id == "argument-independent" && fallback_timeline[4].argument_summary == "first summary only" &&
             fallback_timeline[4].arguments_json == "{\"path\":\"first-json-for-field\"}" && fallback_timeline[5].call_id.empty() &&
             fallback_timeline[5].name == "empty-start" && fallback_timeline[6].call_id.empty() && fallback_timeline[6].name == "empty-result",
         "fallback TUI timeline retains first nonempty running arguments per field across later running and settled replacements while keeping empty ids "
         "separate");

  std::vector<ava::event::RuntimeEvent> write_tool_events;
  auto write = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/write src/main.cpp int changed() { return 1; }",
                                                                          .event_sink = [&write_tool_events](ava::event::RuntimeEvent const& event) {
                                                                            write_tool_events.push_back(event);
                                                                            return ava::core::VoidResult{};
                                                                          }});
  auto const* write_result = write_tool_events.size() == 2 ? ava::tests::runtime_event_as<ava::event::ToolResultEvent>(write_tool_events[1]) : nullptr;
  expect(write && write->handled && write->tool_timeline.size() == 2 && write->tool_timeline[1].status == ava::agent::ToolTimelineStatus::Success &&
             write->tool_timeline[1].diff.find("-int main()") != std::string::npos &&
             write->tool_timeline[1].diff.find("+int changed()") != std::string::npos && write_result &&
             write_result->payload.diff.find("+int changed()") != std::string::npos &&
             std::ranges::any_of(write_result->payload.changed_paths, [](std::string const& path) { return path.ends_with("src/main.cpp"); }),
         "command dispatcher /write forwards successful mutation diffs and changed paths into tool events");
  if (write && write_tool_events.size() == 2)
  {
    auto tui_timeline = ava::app::tool_timeline_for_tui(write->tool_timeline);
    ava::tui::TuiEventState event_state;
    for (auto const& event : write_tool_events) ava::tui::apply_runtime_event(event_state, event);
    auto event_transcript = ava::tui::event_state_transcript_snapshot(event_state);
    std::vector<ava::tui::TranscriptItem> timeline_transcript;
    if (!tui_timeline.empty())
      timeline_transcript.push_back(ava::tui::TranscriptItem{.tool = tui_timeline.back()});
    auto const timeline_lines = ava::tui::detail::render_transcript_lines(timeline_transcript, 100, false, true, false);
    auto const event_lines = ava::tui::detail::render_transcript_lines(event_transcript, 100, false, true, false);
    auto join_lines = [](std::vector<std::string> const& lines) {
      std::string joined;
      for (auto const& line : lines) joined += line + '\n';
      return joined;
    };
    auto const timeline_card = join_lines(timeline_lines);
    auto const event_card = join_lines(event_lines);
    auto count_text = [](std::string_view text, std::string_view needle) {
      std::size_t count = 0;
      for (auto position = text.find(needle); position != std::string_view::npos; position = text.find(needle, position + needle.size())) ++count;
      return count;
    };
    auto const absolute_target = (workspace / "src/main.cpp").generic_string();
    expect(tui_timeline.size() == 1 && tui_timeline.front().status == ava::tui::ToolTimelineStatus::Success &&
               tui_timeline.front().argument_summary == "src/main.cpp" && timeline_lines.size() <= 2 && event_lines.size() <= 2 &&
               timeline_card.find("src/main.cpp · wrote 27 bytes") != std::string::npos &&
               event_card.find("src/main.cpp · wrote 27 bytes") != std::string::npos && count_text(timeline_card, "src/main.cpp") == 1 &&
               count_text(event_card, "src/main.cpp") == 1 && timeline_card.find(absolute_target) == std::string::npos &&
               event_card.find(absolute_target) == std::string::npos,
           "real /write command source timeline retains two lifecycle entries while fallback TUI conversion matches the live settled card: timeline=" +
               timeline_card + " event=" + event_card);
  }

  std::size_t compact_generator_calls = 0;
  auto compact_generator = [&](std::vector<ava::session::SessionEntry> const& entries, ava::session::CompactionConfig const& config,
                               std::string_view instructions, std::size_t estimated_tokens) -> ava::core::Result<std::string> {
    ++compact_generator_calls;
    static_cast<void>(instructions);
    auto const portable_entries = std::ranges::all_of(entries, [](auto const& entry) {
      return entry.type == ava::session::EntryType::UserMessage || entry.type == ava::session::EntryType::AssistantMessage;
    });
    expect(portable_entries && config.max_summary_bytes > 0,
           "command dispatcher /compact passes only portable request projection records to summary generator call " + std::to_string(compact_generator_calls));
    static_cast<void>(estimated_tokens);
    return std::string(
        "# Goal\nKeep key facts\n# Constraints / Preferences\nNone noted.\n# Decisions\nNone noted.\n"
        "# Files Read or Modified\nsrc/main.cpp\n# Unresolved Tasks\nNone noted.\n# Next Steps\nContinue.");
  };
  auto compact =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/compact Keep key facts", .compaction_summary_generator = compact_generator});
  expect(compact && compact->handled && !compact->output.empty() && compact->output[0].find("compaction summary recorded") != std::string::npos,
         "command dispatcher /compact records generated compaction summary");
  auto compact_empty = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/compact", .compaction_summary_generator = compact_generator});
  expect(compact_empty && compact_empty->handled && !compact_empty->output.empty() &&
             compact_empty->output[0].find("compaction summary recorded") != std::string::npos,
         "command dispatcher /compact without instructions records generated compaction summary");
  auto compact_trailing =
      ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/compact ", .compaction_summary_generator = compact_generator});
  expect(compact_trailing && compact_trailing->handled && !compact_trailing->output.empty() &&
             compact_trailing->output[0].find("compaction summary recorded") != std::string::npos,
         "command dispatcher /compact with trailing space records generated compaction summary");
  expect(compact_generator_calls == 3, "command dispatcher /compact invokes the summary generator once per command");

  auto entries = ava::app::runtime::session_ts::rat(unlocked_session)->store.load();
  expect(entries && std::ranges::any_of(*entries,
                                        [](ava::session::SessionEntry const& entry) {
                                          return entry.type == ava::session::EntryType::Compaction &&
                                                 entry.data_json.find("Keep key facts") != std::string::npos &&
                                                 entry.data_json.find("\"summary_unavailable\":false") != std::string::npos;
                                        }),
         "command dispatcher /compact persists generated summary and instructions");

  auto const compactions_before_stale = entries ? count_compaction_entries(*entries) : 0;
  bool introduced_manual_stale_snapshot = false;
  std::size_t manual_stale_generator_calls = 0;
  auto stale_compact = ava::app::run_command(
      unlocked_session,
      ava::app::CommandRequest{.command = "/compact stale snapshot",
                                .compaction_summary_generator = [&unlocked_session, &manual_stale_generator_calls, &introduced_manual_stale_snapshot](
                                                                   std::vector<ava::session::SessionEntry> const&, ava::session::CompactionConfig const&,
                                                                   std::string_view, std::size_t) -> ava::core::Result<std::string> {
                                 ++manual_stale_generator_calls;
                                 if (!introduced_manual_stale_snapshot)
                                 {
                                   introduced_manual_stale_snapshot = true;
                                    static_cast<void>(ava::app::runtime::session_ts::wat(unlocked_session)->append_owned(
                                        ava::session::SessionEntry{.id = "entry_manual_compact_concurrent_change",
                                                                                          .parent_id = "",
                                                                                          .type = ava::session::EntryType::UserMessage,
                                                                                          .timestamp = ava::session::now_timestamp(),
                                                                   .data_json = "{\"text\":\"manual compact concurrent change\"}"}));
                                 }
                                 return std::string(
                                     "# Goal\nStale\n# Constraints / Preferences\nNone noted.\n# Decisions\nNone noted.\n"
                                     "# Files Read or Modified\nNone noted.\n# Unresolved Tasks\nNone noted.\n# Next Steps\nContinue.");
                               }});
  entries = ava::app::runtime::session_ts::rat(unlocked_session)->store.load();
  expect(stale_compact && stale_compact->handled && !stale_compact->output.empty() &&
             stale_compact->output[0].find("compaction summary recorded") != std::string::npos,
         "command dispatcher /compact retries one stale snapshot and records a fresh summary");
  expect(manual_stale_generator_calls == 2, "manual /compact regenerates summary after a stale snapshot");
  expect(
      entries && count_compaction_entries(*entries) == compactions_before_stale + 1 &&
          std::ranges::any_of(
              *entries, [](ava::session::SessionEntry const& entry) { return entry.data_json.find("manual compact concurrent change") != std::string::npos; }),
      "manual /compact stale snapshot preserves concurrent changes and appends one retried compaction");
}

}  // namespace ava::tests::app_runtime_tests
