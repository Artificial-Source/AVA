#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime_internal.h"
#include "ava/tui/tool_card_task_job.h"
#include "ava/tui/tool_cards.h"
#include "ava/core/json.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

void run_tui_tool_card_tests_part_1()
{
  tui_test_support::TuiFrontEndFixture frontend;
  auto const tool_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                     .name = "read_file",
                                                                                     .argument_summary = "path=note.txt\x1b[31m",
                                                                                     .result_summary = "read lines 1-12/12",
                                                                                     .lifecycle = ava::tui::ToolLifecycleState::Complete}}},
          .width = 80,
          .height = 10,
          .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(tool_card,
                             [](std::string const& line) {
                               auto visible = strip_sgr(line);
                               return visible.find("+ read_file") != std::string::npos && visible.find("read lines 1-12/12") != std::string::npos &&
                                      visible.find("complete") == std::string::npos;
                             }) &&
             std::ranges::any_of(tool_card,
                                 [](std::string const& line) {
                                   return line.find("\x1b[38;2;52;211;153m+") != std::string::npos &&
                                          line.find("\x1b[1m\x1b[38;2;77;158;246mread_file") != std::string::npos;
                                 }),
         "tui renders a compact successful tool row without redundant lifecycle text");

  auto plain_tool_row = [](ava::tui::ToolTimelineItem const& item) {
    auto const rows = ava::tui::detail::render_tool_card(item, 120, false);
    auto row = rows.empty() ? std::string{} : strip_sgr(rows.front());
    while (!row.empty() && row.back() == ' ')
      row.pop_back();
    return row;
  };
  auto running_outcome = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Running,
                                                    .name = "read_file",
                                                    .argument_summary = "path=note.txt",
                                                    .call_id = "raw-call-running",
                                                    .lifecycle = ava::tui::ToolLifecycleState::Progress};
  auto successful_outcome = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                       .name = "read_file",
                                                       .result_summary = "12 lines",
                                                       .result_json = "{\"duration_ms\":1200}",
                                                       .call_id = "raw-call-success",
                                                       .lifecycle = ava::tui::ToolLifecycleState::Complete};
  auto failed_outcome = ava::tui::ToolTimelineItem{
      .status = ava::tui::ToolTimelineStatus::Error, .name = "write_file", .call_id = "raw-call-failed", .lifecycle = ava::tui::ToolLifecycleState::Error};
  auto canceled_outcome = ava::tui::ToolTimelineItem{
      .status = ava::tui::ToolTimelineStatus::Canceled, .name = "task", .call_id = "raw-call-canceled", .lifecycle = ava::tui::ToolLifecycleState::Canceled};
  auto const running_row = plain_tool_row(running_outcome);
  auto const successful_row = plain_tool_row(successful_outcome);
  auto const failed_row = plain_tool_row(failed_outcome);
  auto const canceled_row = plain_tool_row(canceled_outcome);
  auto const painted_success = ava::tui::detail::render_tool_card(successful_outcome, 120, false);
  expect(painted_success.size() == 1 && visible_columns(painted_success.front()) == 120 && painted_success.front().starts_with("  \x1b[48;2;18;23;34m") &&
             painted_success.front().ends_with("\x1b[0m  ") && painted_success.front().find("\x1b[0m\x1b[48;2;18;23;34m") != std::string::npos &&
             painted_success.front().find("\x1b[48;2;26;31;46m") == std::string::npos,
         "tui tool cards paint an independently padded low-contrast surface inside exact two-cell screen margins and reapply it after resets");
  expect(running_row == "  │ ~ read_file · running · path=note.txt" && successful_row == "  │ + read_file · 12 lines · 1.2s" &&
             failed_row == "  │ x write_file · failed" && canceled_row == "  │ - task · canceled",
         "tui compact tool rows distinguish running, successful, failed, and canceled outcomes in plain text");
  expect(running_row.find("progress") == std::string::npos && successful_row.find("complete") == std::string::npos &&
             failed_row.find("raw-call-failed") == std::string::npos && canceled_row.find("raw-call-canceled") == std::string::npos,
         "tui compact tool rows omit low-level lifecycle words and raw call ids");
  expect(std::ranges::none_of(tool_card, [](std::string const& line) { return line.find("\x1b[31m") != std::string::npos; }),
         "tui tool card rendering removes untrusted raw sgr escape sequences");
  {
    ScopedEnvVar no_color_guard("NO_COLOR", "1");
    auto const plain_tool_frame =
        ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                  //.provider = "openai",
                                                                                  //.model = "gpt-5.5",
                                                                                  .session_id = "session_plain_tool",
                                                                                  .input = "",
                                                                                  .status = "ready",
                                                                                  .transcript = {ava::tui::TranscriptItem{.tool = successful_outcome}},
                                                                                  .width = 40,
                                                                                  .height = 10,
                                                                                  .tool_presentation = ava::tui::ToolPresentation::Compact});
    auto const plain_tool_line = std::ranges::find_if(plain_tool_frame, [](std::string const& line) { return line.find("+ read_file") != std::string::npos; });
    expect(plain_tool_line != plain_tool_frame.end() && plain_tool_line->starts_with("  │ + read_file") &&
               std::ranges::all_of(plain_tool_frame,
                                   [](std::string const& line) { return line.find('\x1b') == std::string::npos && visible_columns(line) <= 40; }),
           "tui plain-theme tool cards preserve spacing, prefixes, and labels without ANSI background sequences");
  }

  auto permission_tool_item = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Error,
                                                         .name = "bash",
                                                         .argument_summary = "command=git push origin main",
                                                         .result_summary = "permission denied",
                                                         .arguments_json = "{\"command\":\"git push origin main\"}",
                                                         .result_json = "{\"tool\":\"bash\",\"exit_code\":126}",
                                                         .call_id = "call_permission",
                                                         .lifecycle = ava::tui::ToolLifecycleState::Error,
                                                         .permission_request_ids = {"permreq_push"},
                                                         .permissions = {ava::tui::ToolPermissionAuditItem{.permission_request_id = "permreq_push",
                                                                                                           .resolver_request_id = "permission_1",
                                                                                                           .decision = "deny",
                                                                                                           .operation = "bash",
                                                                                                           .tool_name = "bash",
                                                                                                           .risk = "critical",
                                                                                                           .reason = "command permission denied\x1b[31m",
                                                                                                           .target = "",
                                                                                                           .command = "<redacted one-shot command>",
                                                                                                           .resolution_reason = "selected deny"}}};
  auto pending_permission_item = permission_tool_item;
  pending_permission_item.status = ava::tui::ToolTimelineStatus::Running;
  pending_permission_item.lifecycle = ava::tui::ToolLifecycleState::ExecutionStarted;
  pending_permission_item.result_summary.clear();
  pending_permission_item.result_json.clear();
  pending_permission_item.permissions.front().decision.clear();
  pending_permission_item.permissions.front().resolution_reason.clear();
  auto const pending_permission_rows = ava::tui::detail::render_tool_card(pending_permission_item, 120, false);
  auto const pending_permission_text = pending_permission_rows.empty() ? std::string{} : strip_sgr(pending_permission_rows.front());
  auto const pending_permission_expanded = ava::tui::detail::render_tool_card(pending_permission_item, 120, true);
  auto const pending_permission_expanded_text = tui_test_support::join_visible_lines(pending_permission_expanded);
  auto const pending_permission_copy = ava::tui::detail::tool_card_copy_text(pending_permission_item);
  expect(pending_permission_rows.size() == 1 && pending_permission_text.find("permission") == std::string::npos &&
             pending_permission_text.find("permreq_push") == std::string::npos && pending_permission_text.find("permission_1") == std::string::npos &&
             pending_permission_text.find("executing") == std::string::npos,
         "tui compact pending tools omit internal permission state and raw ids");
  expect(pending_permission_expanded_text.find("permission") == std::string::npos &&
             pending_permission_expanded_text.find("permreq_push") == std::string::npos &&
             pending_permission_expanded_text.find("permission_1") == std::string::npos && pending_permission_copy.find("permission") == std::string::npos &&
             pending_permission_copy.find("permreq_push") == std::string::npos && pending_permission_copy.find("permission_1") == std::string::npos,
         "tui expanded and copied pending tools omit internal permission state and identities");
  auto running_id_only_permission_item = pending_permission_item;
  running_id_only_permission_item.permissions.clear();
  auto settled_id_only_permission_item = running_id_only_permission_item;
  settled_id_only_permission_item.status = ava::tui::ToolTimelineStatus::Error;
  settled_id_only_permission_item.lifecycle = ava::tui::ToolLifecycleState::Error;
  auto settled_empty_decision_permission_item = pending_permission_item;
  settled_empty_decision_permission_item.status = ava::tui::ToolTimelineStatus::Error;
  settled_empty_decision_permission_item.lifecycle = ava::tui::ToolLifecycleState::Error;
  expect(plain_tool_row(running_id_only_permission_item).find("permission") == std::string::npos &&
             plain_tool_row(settled_id_only_permission_item).find("permission") == std::string::npos &&
             plain_tool_row(pending_permission_item).find("permission") == std::string::npos &&
             plain_tool_row(settled_empty_decision_permission_item).find("permreq_push") == std::string::npos,
         "tui hides unresolved and settled internal permission receipts from tool cards");
  auto allowed_permission_item = pending_permission_item;
  allowed_permission_item.permissions.front().decision = "allow";
  auto denied_permission_item = pending_permission_item;
  denied_permission_item.permissions.front().decision = "deny";
  expect(plain_tool_row(allowed_permission_item).find("permission allow") == std::string::npos &&
             plain_tool_row(denied_permission_item).find("command permission denied") != std::string::npos,
         "tui compact cards hide routine allows and retain a human-readable denial reason");

  auto const compact_permission_card =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = {ava::tui::TranscriptItem{.tool = permission_tool_item}},
                                                                                .width = 72,
                                                                                .height = 12,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(compact_permission_card,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("command permission denied") != std::string::npos;
                             }) &&
             std::ranges::none_of(compact_permission_card,
                                  [](std::string const& line) {
                                    auto const visible = strip_sgr(line);
                                    return visible.find("permreq_push") != std::string::npos || visible.find("permission_1") != std::string::npos ||
                                           visible.find("/permissions audit") != std::string::npos ||
                                           visible.find("/permissions diagnose") != std::string::npos || visible.find("output:") != std::string::npos;
                                  }) &&
             std::ranges::none_of(compact_permission_card, [](std::string const& line) { return line.find("\x1b[31m") != std::string::npos; }) &&
             std::ranges::all_of(compact_permission_card, [](std::string const& line) { return visible_columns(line) <= 72; }),
         "tui compact denied cards show a human reason while hiding internal audit ids and detail bodies");

  auto running_permission_item = permission_tool_item;
  running_permission_item.status = ava::tui::ToolTimelineStatus::Running;
  running_permission_item.lifecycle = ava::tui::ToolLifecycleState::ExecutionStarted;
  running_permission_item.result_json = "{\"output\":\"SECRET OUTPUT PREVIEW\"}";
  auto const running_permission_card = ava::tui::detail::render_tool_card(running_permission_item, 88, false);
  auto const running_permission_text = tui_test_support::join_visible_lines(running_permission_card);
  expect(running_permission_card.size() == 1 && running_permission_text.find("command permission denied") != std::string::npos &&
             running_permission_text.find("permreq_push") == std::string::npos && running_permission_text.find("permission_1") == std::string::npos &&
             running_permission_text.find("SECRET OUTPUT PREVIEW") == std::string::npos && running_permission_text.find("executing") == std::string::npos,
         "tui compact running denial keeps a human reason on one row without ids or output bodies");

  auto structured_permission_item = permission_tool_item;
  structured_permission_item.result_summary =
      "permission_denied: command requires permission\n  action: ask\n  request_id: permreq_push\n  inspect: /permissions audit show permreq_push";
  structured_permission_item.argument_summary = structured_permission_item.result_summary;
  auto const structured_permission_card = ava::tui::detail::render_tool_card(structured_permission_item, 88, false);
  auto const structured_permission_text = tui_test_support::join_visible_lines(structured_permission_card);
  expect(structured_permission_text.find("command permission denied") != std::string::npos &&
             structured_permission_text.find("permission_denied:") == std::string::npos &&
             structured_permission_text.find("permreq_push") == std::string::npos && structured_permission_text.find("/permissions audit") == std::string::npos,
         "tui compact permission cards suppress raw structured receipts while retaining a readable denial");

  structured_permission_item.arguments_json.clear();
  auto const& joined_tool_card_text = tui_test_support::join_visible_lines;
  auto const& count_text = tui_test_support::count_occurrences;
  auto const structured_permission_expanded = joined_tool_card_text(ava::tui::detail::render_tool_card(structured_permission_item, 57, true));
  auto const structured_permission_copy = ava::tui::detail::tool_card_copy_text(structured_permission_item);
  auto const structured_permission_only_copy = ava::tui::detail::tool_card_permission_copy_text(structured_permission_item);
  auto const curated_copied_permission_fields_once = [&](std::string_view text) {
    return count_text(text, "permission: deny · risk critical · id permreq_push") == 1 && count_text(text, "resolver permission_1") == 1 &&
           count_text(text, "reason command permission denied") == 1 && count_text(text, "resolution selected deny") == 1 &&
           count_text(text, "operation bash") == 1 && count_text(text, "tool bash") == 1 && count_text(text, "command <redacted one-shot command>") == 1 &&
           count_text(text, "inspect: /permissions audit show permreq_push") == 1 && count_text(text, "diagnose: /permissions diagnose permreq_push") == 1;
  };
  auto const excludes_raw_permission_dump = [](std::string_view text) {
    return text.find("permission_denied") == std::string_view::npos && text.find("action: ask") == std::string_view::npos &&
           text.find("request_id:") == std::string_view::npos;
  };
  expect(excludes_raw_permission_dump(structured_permission_expanded) && structured_permission_expanded.find("permreq_push") == std::string::npos &&
             structured_permission_expanded.find("permission_1") == std::string::npos,
         "tui expanded denied shell cards omit raw and curated routine audit receipts");
  expect(excludes_raw_permission_dump(structured_permission_copy) && structured_permission_copy.find("permreq_push") == std::string::npos &&
             structured_permission_copy.find("permission_1") == std::string::npos && structured_permission_copy.find("status: error") != std::string::npos,
         "tui general tool copy omits raw permission receipts and resolver identities");
  expect(curated_copied_permission_fields_once(structured_permission_only_copy),
         "tui standalone permission copy continues to expose one complete curated permission audit");

  auto unrelated_shell_failure = permission_tool_item;
  unrelated_shell_failure.argument_summary = "command=git push origin main";
  unrelated_shell_failure.result_summary = "remote rejected the push";
  unrelated_shell_failure.result_json.clear();
  auto const unrelated_shell_failure_expanded = joined_tool_card_text(ava::tui::detail::render_tool_card(unrelated_shell_failure, 120, true));
  auto const unrelated_shell_failure_copy = ava::tui::detail::tool_card_copy_text(unrelated_shell_failure);
  expect(unrelated_shell_failure_expanded.find("remote rejected the push") != std::string::npos &&
             unrelated_shell_failure_copy.find("shell status: remote rejected the push") != std::string::npos,
         "tui permission audit deduplication preserves unrelated single-line shell failure status");

  auto unrelated_multiline_shell_failure = permission_tool_item;
  unrelated_multiline_shell_failure.argument_summary = "command=git push origin main";
  unrelated_multiline_shell_failure.result_summary = "remote rejected the push\nfailure: remote changed";
  unrelated_multiline_shell_failure.result_json = R"({"tool":"bash","exit_code":1,"stderr":"fatal: remote rejected the push\nhint: fetch before retrying"})";
  auto const unrelated_multiline_shell_failure_expanded =
      joined_tool_card_text(ava::tui::detail::render_tool_card(unrelated_multiline_shell_failure, 120, true));
  auto const unrelated_multiline_shell_failure_copy = ava::tui::detail::tool_card_copy_text(unrelated_multiline_shell_failure);
  expect(unrelated_multiline_shell_failure_expanded.find("remote rejected the push") != std::string::npos &&
             unrelated_multiline_shell_failure_expanded.find("failure: remote changed") != std::string::npos &&
             unrelated_multiline_shell_failure_expanded.find("fatal: remote rejected the push") != std::string::npos &&
             unrelated_multiline_shell_failure_expanded.find("hint: fetch before retrying") != std::string::npos &&
             unrelated_multiline_shell_failure_copy.find("remote rejected the push") != std::string::npos &&
             unrelated_multiline_shell_failure_copy.find("failure: remote changed") != std::string::npos &&
             unrelated_multiline_shell_failure_copy.find("fatal: remote rejected the push") != std::string::npos &&
             unrelated_multiline_shell_failure_copy.find("hint: fetch before retrying") != std::string::npos,
         "tui permission audit deduplication preserves unrelated multiline shell failure and output");

  auto unrelated_permission_denied_shell_failure = permission_tool_item;
  unrelated_permission_denied_shell_failure.argument_summary = "command=git status";
  unrelated_permission_denied_shell_failure.result_summary = "test category permission_denied remains user-visible";
  unrelated_permission_denied_shell_failure.result_json =
      R"({"tool":"bash","exit_code":1,"category":"permission_denied","stderr":"test output: permission_denied\nunrelated tool output"})";
  auto const unrelated_permission_denied_shell_failure_expanded =
      joined_tool_card_text(ava::tui::detail::render_tool_card(unrelated_permission_denied_shell_failure, 120, true));
  auto const unrelated_permission_denied_shell_failure_copy = ava::tui::detail::tool_card_copy_text(unrelated_permission_denied_shell_failure);
  expect(unrelated_permission_denied_shell_failure_expanded.find("test category permission_denied remains user-visible") != std::string::npos &&
             unrelated_permission_denied_shell_failure_expanded.find("test output: permission_denied") != std::string::npos &&
             unrelated_permission_denied_shell_failure_expanded.find("unrelated tool output") != std::string::npos &&
             unrelated_permission_denied_shell_failure_copy.find("test category permission_denied remains user-visible") != std::string::npos &&
             unrelated_permission_denied_shell_failure_copy.find("test output: permission_denied") != std::string::npos &&
             unrelated_permission_denied_shell_failure_copy.find("unrelated tool output") != std::string::npos,
         "tui permission audit deduplication preserves unrelated permission_denied shell result and output without a linked request id");

  auto completed_write = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                    .name = "write_file",
                                                    .argument_summary = "path=src/cache.cpp",
                                                    .result_summary = "wrote 27 bytes",
                                                    .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                                    .changed_paths = {"src/cache.cpp"}};
  auto const write_without_target_result = ava::tui::detail::render_tool_card(completed_write, 88, false);
  completed_write.result_summary = "wrote 27 bytes to src/cache.cpp";
  auto const write_with_target_result = ava::tui::detail::render_tool_card(completed_write, 88, false);
  auto count_target = [](std::vector<std::string> const& lines) {
    std::size_t count = 0;
    for (auto const& line : lines)
    {
      auto visible = strip_sgr(line);
      for (auto offset = visible.find("src/cache.cpp"); offset != std::string::npos; offset = visible.find("src/cache.cpp", offset + 1))
        ++count;
    }
    return count;
  };
  expect(count_target(write_without_target_result) == 1 && count_target(write_with_target_result) == 1 && write_without_target_result.size() <= 2 &&
             write_with_target_result.size() <= 2,
         "tui completed write cards retain one workspace-relative target without duplicating a target already present in the result");

  auto absolute_path_item = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                       .name = "write_file",
                                                       .argument_summary = "path=/home/user/workspace/src/main.cpp",
                                                       .result_summary = "wrote 27 bytes",
                                                       .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                                       .changed_paths = {"/home/user/workspace/src/main.cpp"}};
  auto const absolute_path_card = ava::tui::detail::render_tool_card(absolute_path_item, 88, false);
  expect(std::ranges::any_of(absolute_path_card, [](std::string const& line) { return strip_sgr(line).find("wrote 27 bytes") != std::string::npos; }) &&
             std::ranges::none_of(absolute_path_card, [](std::string const& line) { return strip_sgr(line).find("raw-call") != std::string::npos; }),
         "tui compact tool cards retain the useful outcome without raw identities");

  auto changed_path_priority = completed_write;
  changed_path_priority.argument_summary = "path=stale/input.cpp";
  changed_path_priority.result_summary = "wrote 27 bytes";
  changed_path_priority.changed_paths = {"src/main.cpp"};
  auto const changed_path_priority_card = ava::tui::detail::render_tool_card(changed_path_priority, 88, false);
  expect(changed_path_priority_card.size() == 1 && strip_sgr(changed_path_priority_card.front()).find("src/main.cpp · wrote 27 bytes") != std::string::npos &&
             strip_sgr(changed_path_priority_card.front()).find("stale/input.cpp") == std::string::npos,
         "settled mutation cards prefer a safe relative changed path over stale argument summaries");

  auto same_basename_multi_path_item = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                  .name = "write_file",
                                                                  .argument_summary = "path=a.cpp",
                                                                  .result_summary = "wrote /workspace/pkg/a.cpp and a.cpp",
                                                                  .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                                                  .changed_paths = {"/workspace/pkg/a.cpp", "a.cpp"}};
  auto const same_basename_multi_path_text = tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(same_basename_multi_path_item, 120, true));
  expect(same_basename_multi_path_text.find("/workspace/pkg/a.cpp") != std::string::npos &&
             same_basename_multi_path_text.find("wrote /workspace/pkg/a.cpp and a.cpp") != std::string::npos,
         "tui multi-file cards retain distinct absolute and same-basename relative paths without cross-path aliasing");

  auto action_query_item = changed_path_priority;
  action_query_item.call_id = "call\nunsafe";
  auto const action_query_card = ava::tui::detail::render_tool_card(action_query_item, 120, true);
  auto const action_query_text = tui_test_support::join_visible_lines(action_query_card);

  expect(action_query_text.find("toggle: /tool") == std::string::npos && action_query_text.find("copy: /copy tool") == std::string::npos &&
             ava::tui::detail::tool_card_matches_copy_query(action_query_item, "src/main.cpp"),
         "expanded cards avoid noisy action command rows while preserving explicit query matching");
  action_query_item.call_id = "call-safe";
  auto const safe_call_action_text = tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(action_query_item, 120, true));
  expect(safe_call_action_text.find("call-safe") == std::string::npos && ava::tui::detail::tool_card_matches_copy_query(action_query_item, "call-safe"),
         "expanded cards keep supplied call identities out of ordinary presentation");
  auto parse_rendered_tool_action = [](std::string_view rendered) {
    constexpr std::string_view marker = "toggle: ";
    auto const start = rendered.find(marker);
    if (start == std::string_view::npos)
      return std::optional<std::string>{};
    auto command = rendered.substr(start + marker.size());
    if (auto const end = command.find('\n'); end != std::string_view::npos)
      command = command.substr(0, end);
    return ava::tui::parse_tui_tool_command_argument(command);
  };
  auto narrow_action_item = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                       .name = "write",
                                                       .result_summary = "wrote",
                                                       .call_id = "call-" + std::string(48, 'x'),
                                                       .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                                       .diff = "--- a/src/main.cpp\n+++ b/src/main.cpp"};
  auto const narrow_action_card = ava::tui::detail::render_tool_card(narrow_action_item, 40, true);
  auto const narrow_action_text = tui_test_support::join_visible_lines(narrow_action_card);
  auto const parsed_narrow_action = parse_rendered_tool_action(narrow_action_text);
  expect(narrow_action_text.find("toggle: /tool") == std::string::npos && narrow_action_text.find("copy: /copy tool") == std::string::npos &&
             !parsed_narrow_action && std::ranges::all_of(narrow_action_card, [](std::string const& row) { return visible_columns(row) <= 40; }),
         "narrow expanded cards remain bounded without action or identity rows");
  auto near_threshold_action_item = narrow_action_item;
  near_threshold_action_item.call_id = "call123";
  near_threshold_action_item.diff.clear();
  auto const near_threshold_action_card = ava::tui::detail::render_tool_card(near_threshold_action_item, 32, true);
  auto const near_threshold_action_text = tui_test_support::join_visible_lines(near_threshold_action_card);
  auto const parsed_near_threshold_action = parse_rendered_tool_action(near_threshold_action_text);
  expect(near_threshold_action_text.find("toggle: /tool") == std::string::npos && !parsed_near_threshold_action &&
             std::ranges::all_of(near_threshold_action_card, [](std::string const& row) { return visible_columns(row) == 32; }),
         "near-threshold expanded cards remain exactly width-bounded without identities");
  auto no_fit_action_item = narrow_action_item;
  no_fit_action_item.name = "impossibly-long-tool-name";
  no_fit_action_item.diff.clear();
  auto const no_fit_action_text = tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(no_fit_action_item, 32, true));
  bool narrow_widths_safe = true;
  for (std::size_t width = 0; width <= 4; ++width)
  {
    auto const rows = ava::tui::detail::render_tool_card(no_fit_action_item, width, true);
    narrow_widths_safe = narrow_widths_safe && std::ranges::all_of(rows, [width](std::string const& row) { return visible_columns(row) <= width; });
  }
  expect(
      no_fit_action_text.find("toggle: /tool") == std::string::npos && no_fit_action_text.find("copy: /copy tool") == std::string::npos && narrow_widths_safe,
      "tool action rows are omitted when no safe matching query fits every emitted command row and zero-to-four-column cards stay bounded");
  auto whitespace_id_item = action_query_item;
  whitespace_id_item.call_id = "   ";
  auto const whitespace_id_action_text = tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(whitespace_id_item, 120, true));

  auto edge_whitespace_item = action_query_item;
  edge_whitespace_item.call_id = " call-edge ";
  edge_whitespace_item.changed_paths = {" changed-edge.cpp "};
  auto const edge_whitespace_action_text = tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(edge_whitespace_item, 120, true));

  auto const parsed_whitespace_fallback = parse_rendered_tool_action(whitespace_id_action_text);
  auto const parsed_edge_fallback = parse_rendered_tool_action(edge_whitespace_action_text);
  expect(!parsed_whitespace_fallback && !parsed_edge_fallback && whitespace_id_action_text.find("toggle: /tool") == std::string::npos &&
             edge_whitespace_action_text.find("call-edge") == std::string::npos,
         "expanded cards omit whitespace and edge-whitespace identities along with action command rows");
  action_query_item.call_id = "\x1b[31munsafe";
  action_query_item.changed_paths = {"/absolute/not-safe"};
  action_query_item.name = "write\nunsafe";
  auto const unsafe_action_text = tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(action_query_item, 120, true));
  expect(unsafe_action_text.find("toggle: /tool") == std::string::npos && unsafe_action_text.find("copy: /copy tool") == std::string::npos,
         "tool action rows are omitted when no supplied id, relative path, or tool name is an exact safe query");

  permission_tool_item.details_visible = true;
  auto const expanded_permission_card =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = {ava::tui::TranscriptItem{.tool = permission_tool_item}},
                                                                                .width = 88,
                                                                                .height = 28,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(expanded_permission_card,
                             [](std::string const& line) { return strip_sgr(line).find("command permission denied") != std::string::npos; }) &&
             std::ranges::none_of(expanded_permission_card,
                                  [](std::string const& line) {
                                    auto const visible = strip_sgr(line);
                                    return visible.find("permreq_push") != std::string::npos || visible.find("permission_1") != std::string::npos ||
                                           visible.find("/permissions audit") != std::string::npos ||
                                           visible.find("/permissions diagnose") != std::string::npos;
                                  }) &&
             std::ranges::all_of(expanded_permission_card, [](std::string const& line) { return visible_columns(line) <= 88; }),
         "tui expanded denied cards keep a human reason while omitting internal receipts and resolver identities");

  {
    ScopedEnvVar no_color_permission_guard("NO_COLOR", "1");
    auto const plain_narrow_permission_card =
        ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                  //.provider = "openai",
                                                                                  //.model = "gpt-5.5",
                                                                                  .session_id = "session_test",
                                                                                  .input = "",
                                                                                  .status = "ready",
                                                                                  .transcript = {ava::tui::TranscriptItem{.tool = permission_tool_item}},
                                                                                  .width = 57,
                                                                                  .height = 24,
                                                                                  .tool_presentation = ava::tui::ToolPresentation::Expanded});
    auto plain_permission_text = std::string{};
    for (auto const& line : plain_narrow_permission_card)
    {
      plain_permission_text += line;
      plain_permission_text += '\n';
    }
    auto const plain_permission_accessible =
        std::ranges::all_of(plain_narrow_permission_card,
                            [](std::string const& line) { return line.find('\x1b') == std::string::npos && visible_columns(line) <= 57; }) &&
        plain_permission_text.find("x bash") != std::string::npos && plain_permission_text.find("command permission denied") != std::string::npos &&
        plain_permission_text.find("permreq_push") == std::string::npos && plain_permission_text.find("permission_1") == std::string::npos;
    expect(plain_permission_accessible, "tui plain narrow denied cards keep the human reason readable without audit identities");
  }

  auto const grouped_context_tools = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                                            .name = "glob",
                                                                                                            .argument_summary = "pattern=src/**/*.cpp",
                                                                                                            .result_summary = "12 files"}},
                                                ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                                            .name = "grep",
                                                                                                            .argument_summary = "pattern=TODO",
                                                                                                            .result_summary = "3 matches"}},
                                                ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                                            .name = "read_file",
                                                                                                            .argument_summary = "path=src/main.cpp",
                                                                                                            .result_summary = "read lines 1-40"}}},
                                 .width = 96,
                                 .height = 16,
                                 .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::count_if(grouped_context_tools,
                               [](std::string const& line) { return strip_sgr(line).find("context gathering · 3 tools") != std::string::npos; }) == 1 &&
             std::ranges::any_of(grouped_context_tools, [](std::string const& line) { return strip_sgr(line).find("glob") != std::string::npos; }) &&
             std::ranges::any_of(grouped_context_tools, [](std::string const& line) { return strip_sgr(line).find("read_file") != std::string::npos; }),
         "tui groups consecutive context-gathering tool cards with a single readable heading while keeping details");

  auto const empty_tool_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{
              .tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success, .name = "", .argument_summary = "", .result_summary = ""}}},
          .width = 40,
          .height = 8,
          .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(empty_tool_card,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("+ unknown") != std::string::npos && visible.find("unknown") != std::string::npos;
                             }) &&
             std::ranges::all_of(empty_tool_card, [](std::string const& line) { return visible_columns(line) <= 40; }),
         "tui renders empty tool-card fields with a safe fallback name");

  auto const& visible_text = tui_test_support::join_visible_lines;
  auto const webfetch_item = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                        .name = "webfetch",
                                                        .argument_summary = "url=https://example.test/docs\x1b[31m",
                                                        .result_summary = "fetched text/plain",
                                                        .result_json = "{\"content_type\":\"text/plain\",\"content\":\"alpha\\nbeta\\ngamma\"}",
                                                        .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                                        .truncated = true,
                                                        .output_lines = 2,
                                                        .total_lines = 3};
  auto const webfetch_card = ava::tui::detail::render_tool_card(webfetch_item, 72, true);
  auto const webfetch_text = visible_text(webfetch_card);
  auto const webfetch_copy = ava::tui::detail::tool_card_copy_text(webfetch_item);
  expect(webfetch_text.find("webfetch") != std::string::npos && webfetch_text.find("url=https://example.test/docs") != std::string::npos,
         "tui non-shell webfetch cards preserve the tool name and sanitized arguments");
  expect(webfetch_text.find("output:") != std::string::npos && webfetch_text.find("alpha") != std::string::npos,
         "tui non-shell webfetch cards render expanded content previews");
  expect(webfetch_copy.find("output:\n  alpha\n  beta\n  gamma") != std::string::npos &&
             webfetch_copy.find("truncation: truncated 2/3 lines") != std::string::npos && webfetch_copy.find("\x1b[") == std::string::npos,
         "tui non-shell webfetch copy text includes preview, truncation, and no ANSI styling");
  expect(std::ranges::none_of(webfetch_card, [](std::string const& line) { return line.find("\x1b[31m") != std::string::npos; }) &&
             std::ranges::all_of(webfetch_card, [](std::string const& line) { return visible_columns(line) <= 72; }),
         "tui non-shell webfetch cards remove raw escapes and stay within width");

  auto const non_shell_cards =
      std::vector<ava::tui::ToolTimelineItem>{ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Error,
                                                                         .name = "websearch",
                                                                         .argument_summary = "query=terminal renderer",
                                                                         .result_summary = "provider error",
                                                                         .result_json = "{\"error\":\"rate limited\"}",
                                                                         .lifecycle = ava::tui::ToolLifecycleState::Error},
                                              ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                         .name = "skill",
                                                                         .argument_summary = "name=frontend-review",
                                                                         .result_summary = "loaded skill instructions",
                                                                         .result_json = "{\"preview\":\"Use semantic landmarks\"}",
                                                                         .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                                                         .permission_request_ids = {"permreq_skill"}},
                                              ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                         .name = "question",
                                                                         .argument_summary = "choose deployment target",
                                                                         .result_summary = "answered: staging",
                                                                         .result_json = "{\"answer\":\"staging\"}",
                                                                         .lifecycle = ava::tui::ToolLifecycleState::Complete},
                                              ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                         .name = "lsp_diagnostics",
                                                                         .argument_summary = "path=src/main.cpp",
                                                                         .result_summary = "2 diagnostics",
                                                                         .result_json = "{\"output\":\"line 12 warning\\nline 18 error\"}",
                                                                         .lifecycle = ava::tui::ToolLifecycleState::Complete}};
  for (auto const& item : non_shell_cards)
  {
    auto const card = ava::tui::detail::render_tool_card(item, 36, true);
    auto const text = visible_text(card);
    auto const copy = ava::tui::detail::tool_card_copy_text(item);
    expect(text.find(item.name) != std::string::npos && text.find("shell status") == std::string::npos &&
               copy.find("tool: " + item.name) != std::string::npos && copy.find("shell status") == std::string::npos &&
               copy.find("\x1b[") == std::string::npos && std::ranges::all_of(card, [](std::string const& line) { return visible_columns(line) <= 36; }),
           "tui non-shell tool cards render and copy safely on narrow terminals for " + item.name);
  }
  auto const skill_permission_copy = ava::tui::detail::tool_card_permission_copy_text(non_shell_cards[1]);
  expect(skill_permission_copy.empty(), "tui permission copy text stays empty when only unresolved permission ids are present");
  auto const expanded_skill_card = ava::tui::detail::render_tool_card(non_shell_cards[1], 48, true);
  auto const expanded_skill_text = visible_text(expanded_skill_card);
  expect(expanded_skill_text.find("permission:") == std::string::npos && expanded_skill_text.find("permreq_skill") == std::string::npos,
         "tui expanded settled non-shell cards omit unresolved internal permission identities");

  {
    ScopedEnvVar no_color_tool_guard("NO_COLOR", "1");
    auto const plain_non_shell_card = ava::tui::render_composer(
        frontend.frontend(),
        ava::tui::ComposerSnapshot{.mode = "build",
                                   //.provider = "openai",
                                   //.model = "gpt-5.5",
                                   .session_id = "session_test",
                                   .input = "",
                                   .status = "ready",
                                   .transcript = {ava::tui::TranscriptItem{.tool = webfetch_item}, ava::tui::TranscriptItem{.tool = non_shell_cards[3]}},
                                   .width = 40,
                                   .height = 30,
                                   .tool_presentation = ava::tui::ToolPresentation::Expanded});
    auto const plain_text = visible_text(plain_non_shell_card);
    expect(std::ranges::all_of(plain_non_shell_card,
                               [](std::string const& line) { return line.find('\x1b') == std::string::npos && visible_columns(line) <= 40; }) &&
               plain_text.find("webfetch") != std::string::npos && plain_text.find("lsp_diagnostics") != std::string::npos &&
               plain_text.find("output:") != std::string::npos,
           "tui plain narrow non-shell tool cards keep names and output readable without color");
  }

  auto const running_error_cards = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Running,
                                                                                     .name = "bash",
                                                                                     .argument_summary = "command=build\x1b[31m now",
                                                                                     .result_summary = ""}},
                         ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Error,
                                                                                     .name = "write_file",
                                                                                     .argument_summary = std::string(120, 'a') + "\x1b[31m",
                                                                                     .result_summary = "error: denied\x1b[31m"}}},
          .width = 60,
          .height = 14,
          .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(running_error_cards,
                             [](std::string const& line) {
                               auto visible = strip_sgr(line);
                               return visible.find("~ bash") != std::string::npos && visible.find("bash") != std::string::npos &&
                                      visible.find("command=build?") != std::string::npos;
                             }) &&
             std::ranges::any_of(running_error_cards,
                                 [](std::string const& line) {
                                   auto visible = strip_sgr(line);
                                   return visible.find("x write_file") != std::string::npos && visible.find("write_file") != std::string::npos;
                                 }) &&
             std::ranges::all_of(running_error_cards, [](std::string const& line) { return visible_columns(line) <= 60; }),
         "tui renders running/error tool cards with sanitized truncated summaries");
  expect(std::ranges::none_of(running_error_cards, [](std::string const& line) { return line.find("\x1b[31m") != std::string::npos; }),
         "tui running/error tool cards remove untrusted raw sgr escape sequences");
  expect(std::ranges::any_of(running_error_cards, [](std::string const& line) { return line.find("\x1b[38;2;251;191;36m~") != std::string::npos; }) &&
             std::ranges::any_of(running_error_cards, [](std::string const& line) { return line.find("\x1b[38;2;248;113;113mx") != std::string::npos; }),
         "tui emits trusted sgr status colors for running and error tool cards");

  auto const canceled_tool_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Canceled,
                                                                                     .name = "bash",
                                                                                     .argument_summary = "command=sleep 30",
                                                                                     .result_summary = "stopped by user",
                                                                                     .arguments_json = "{\"command\":\"sleep 30\"}",
                                                                                     .result_json = "{\"tool\":\"bash\",\"canceled\":true}",
                                                                                     .lifecycle = ava::tui::ToolLifecycleState::Canceled}}},
          .width = 64,
          .height = 16,
          .tool_presentation = ava::tui::ToolPresentation::Expanded});
  expect(std::ranges::any_of(canceled_tool_card,
                             [](std::string const& line) {
                               auto visible = strip_sgr(line);
                               return visible.find("- bash") != std::string::npos && visible.find("bash") != std::string::npos &&
                                      visible.find("canceled") != std::string::npos;
                             }) &&
             std::ranges::any_of(canceled_tool_card,
                                 [](std::string const& line) { return strip_sgr(line).find("result: stopped by user") != std::string::npos; }) &&
             std::ranges::none_of(canceled_tool_card, [](std::string const& line) { return strip_sgr(line).find("x bash") != std::string::npos; }) &&
             std::ranges::all_of(canceled_tool_card, [](std::string const& line) { return visible_columns(line) <= 64; }),
         "tui renders canceled tool cards as a distinct non-error status");
  auto const canceled_copy_text = ava::tui::detail::tool_card_copy_text(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Canceled,
                                                                                                   .name = "bash",
                                                                                                   .argument_summary = "command=sleep 30",
                                                                                                   .result_summary = "stopped by user",
                                                                                                   .arguments_json = "{\"command\":\"sleep 30\"}",
                                                                                                   .result_json = "{\"tool\":\"bash\",\"canceled\":true}",
                                                                                                   .lifecycle = ava::tui::ToolLifecycleState::Canceled});
  expect(canceled_copy_text.find("status: canceled") != std::string::npos && canceled_copy_text.find("command: sleep 30") != std::string::npos &&
             canceled_copy_text.find("lifecycle:") == std::string::npos && canceled_copy_text.find("\x1b[") == std::string::npos,
         "tui copy text preserves canceled tool state and safe call text without internal lifecycle receipts");

  auto const detailed_tool_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                     .name = "bash",
                                                                                     .argument_summary = "command=cmake --build build",
                                                                                     .result_summary = "line one line two line three line four"}}},
          .width = 48,
          .height = 12,
          .tool_presentation = ava::tui::ToolPresentation::Expanded});
  expect(std::ranges::any_of(detailed_tool_card, [](std::string const& line) { return strip_sgr(line).find("$ cmake --build build") != std::string::npos; }) &&
             std::ranges::any_of(detailed_tool_card, [](std::string const& line) { return strip_sgr(line).find("bash · line one") != std::string::npos; }) &&
             std::ranges::none_of(detailed_tool_card, [](std::string const& line) { return strip_sgr(line).find("args:") != std::string::npos; }) &&
             std::ranges::all_of(detailed_tool_card, [](std::string const& line) { return visible_columns(line) <= 48; }),
         "tui expanded shell cards wrap a human call below a clipped primary without raw argument blocks");

  auto const copyable_tool_text = ava::tui::detail::tool_card_copy_text(
      ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Error,
                                 .name = "bash",
                                 .argument_summary = "command=git push origin main",
                                 .result_summary = "permission denied",
                                 .arguments_json = "{\"command\":\"git push origin main\"}",
                                 .result_json = "{\"tool\":\"bash\",\"exit_code\":126,\"stderr\":\"denied\\ntry again\"}",
                                 .lifecycle = ava::tui::ToolLifecycleState::Error,
                                 .permissions = {ava::tui::ToolPermissionAuditItem{.permission_request_id = "permreq_push",
                                                                                   .resolver_request_id = "permission_1",
                                                                                   .decision = "deny",
                                                                                   .operation = "bash",
                                                                                   .tool_name = "bash",
                                                                                   .risk = "high",
                                                                                   .reason = "command can change external state",
                                                                                   .command = "git push origin main"}},
                                 .diff = "--- a\n+++ b\n-old\n+new",
                                 .changed_paths = {"src/main.cpp", "\x1b[31msecret.txt"},
                                 .truncated = true,
                                 .output_lines = 2,
                                 .total_lines = 10,
                                 .spill_path = "/tmp/ava-spill/bash.txt"});
  expect(copyable_tool_text.find("tool: bash") != std::string::npos && copyable_tool_text.find("command: git push origin main") != std::string::npos &&
             copyable_tool_text.find("shell status: exit 126") != std::string::npos && copyable_tool_text.find("permission:") == std::string::npos &&
             copyable_tool_text.find("permreq_push") == std::string::npos && copyable_tool_text.find("permission_1") == std::string::npos &&
             copyable_tool_text.find("output:\n  denied\n  try again") != std::string::npos &&
             copyable_tool_text.find("truncation: truncated 2/10 lines") != std::string::npos &&
             copyable_tool_text.find("changed: src/main.cpp, ?[31msecret.txt") != std::string::npos &&
             copyable_tool_text.find("full output: /tmp/ava-spill/bash.txt") != std::string::npos &&
             copyable_tool_text.find("diff:\n  --- a\n  +++ b") != std::string::npos && copyable_tool_text.find("\x1b[") == std::string::npos,
         "tui exposes safe plain copy text without ANSI styling or internal audit receipts");

  auto const permission_copy_text = ava::tui::detail::tool_card_permission_copy_text(
      ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Error,
                                 .name = "bash",
                                 .permissions = {ava::tui::ToolPermissionAuditItem{.permission_request_id = "permreq_push",
                                                                                   .resolver_request_id = "permission_1",
                                                                                   .decision = "deny",
                                                                                   .operation = "bash",
                                                                                   .tool_name = "bash",
                                                                                   .risk = "high",
                                                                                   .reason = "command can change external state",
                                                                                   .command = "git push origin main"},
                                                 ava::tui::ToolPermissionAuditItem{.permission_request_id = "permreq_read",
                                                                                   .decision = "allow",
                                                                                   .operation = "read",
                                                                                   .tool_name = "read_file",
                                                                                   .risk = "low",
                                                                                   .target = "docs/core/usage.md"}}},
      "git push");
  auto const missing_permission_copy_text = ava::tui::detail::tool_card_permission_copy_text(
      ava::tui::ToolTimelineItem{
          .status = ava::tui::ToolTimelineStatus::Success,
          .name = "read",
          .permissions = {ava::tui::ToolPermissionAuditItem{
              .permission_request_id = "permreq_read", .decision = "allow", .operation = "read", .tool_name = "read_file", .target = "docs/core/usage.md"}}},
      "git push");
  expect(permission_copy_text.find("permission: deny") != std::string::npos && permission_copy_text.find("id permreq_push") != std::string::npos &&
             permission_copy_text.find("risk high") != std::string::npos &&
             permission_copy_text.find("reason command can change external state") != std::string::npos &&
             permission_copy_text.find("command git push origin main") != std::string::npos &&
             permission_copy_text.find("inspect: /permissions audit show permreq_push") != std::string::npos &&
             permission_copy_text.find("diagnose: /permissions diagnose permreq_push") != std::string::npos &&
             permission_copy_text.find("permreq_read") == std::string::npos && permission_copy_text.find("\x1b[") == std::string::npos &&
             missing_permission_copy_text.empty(),
         "tui exposes focused permission copy text with follow-up commands, query filtering, and no ANSI styling");

  auto const copyable_diff_text = ava::tui::detail::tool_card_diff_copy_text(ava::tui::ToolTimelineItem{
      .status = ava::tui::ToolTimelineStatus::Success, .name = "write", .diff = "--- note.txt\n+++ note.txt\n-\x1b[31mold\n+new", .diff_truncated = true});
  expect(copyable_diff_text.find("--- note.txt") != std::string::npos && copyable_diff_text.find("-old") != std::string::npos &&
             copyable_diff_text.find("+new") != std::string::npos && copyable_diff_text.find("[diff truncated]") != std::string::npos &&
             copyable_diff_text.find("\x1b[") == std::string::npos,
         "tui exposes focused plain copy text for latest tool diffs without ANSI styling");

  auto const queryable_tool_card =
      ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                 .name = "write",
                                 .argument_summary = "src/main.cpp",
                                 .result_summary = "wrote 27 bytes",
                                 .permissions = {ava::tui::ToolPermissionAuditItem{
                                     .permission_request_id = "permreq_write", .decision = "allow", .tool_name = "write_file", .reason = "workspace edit"}},
                                 .diff = "--- src/main.cpp\n+++ src/main.cpp\n-old\n+new",
                                 .changed_paths = {"src/main.cpp"}};
  expect(ava::tui::detail::tool_card_matches_copy_query(queryable_tool_card, "MAIN.CPP") &&
             ava::tui::detail::tool_card_matches_copy_query(queryable_tool_card, "workspace edit") &&
             ava::tui::detail::tool_card_matches_copy_query(queryable_tool_card, "+new") &&
             !ava::tui::detail::tool_card_matches_copy_query(queryable_tool_card, "package-lock"),
         "tui copy query matching finds visible tool, permission, changed-path, and diff context case-insensitively");

  auto const collapsed_override_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                                            .name = "bash",
                                                                                                            .argument_summary = "command=ctest",
                                                                                                            .result_summary = "ok",
                                                                                                            .details_visible = false}}},
                                 .width = 48,
                                 .height = 10,
                                 .tool_presentation = ava::tui::ToolPresentation::Expanded});
  expect(
      std::ranges::none_of(collapsed_override_card, [](std::string const& line) { return strip_sgr(line).find("args: command=ctest") != std::string::npos; }),
      "tui supports per-tool detail collapse even when the global details toggle is enabled");

  auto const expanded_override_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                                            .name = "grep",
                                                                                                            .argument_summary = "pattern=todo",
                                                                                                            .result_summary = "2 matches",
                                                                                                            .details_visible = true,
                                                                                                            .truncated = true,
                                                                                                            .visible_matches = 2,
                                                                                                            .total_matches = 10,
                                                                                                            .spill_path = "/tmp/ava-spill/grep.txt",
                                                                                                            .spill_truncated = true}}},
                                 .width = 72,
                                 .height = 12,
                                 .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(expanded_override_card,
                             [](std::string const& line) { return strip_sgr(line).find("truncation: truncated 2/10 matches") != std::string::npos; }) &&
             std::ranges::any_of(expanded_override_card,
                                 [](std::string const& line) { return strip_sgr(line).find("full output: /tmp/ava-spill/grep.txt") != std::string::npos; }) &&
             std::ranges::any_of(expanded_override_card, [](std::string const& line) { return strip_sgr(line).find("spill incomplete") != std::string::npos; }),
         "tui renders backend-provided truncation counts and separate truthful spill metadata only when present");

  auto const changed_paths_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{
              .tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                 .name = "apply_patch",
                                                 .argument_summary = "2 edits",
                                                 .result_summary = "updated files",
                                                 .details_visible = true,
                                                 .changed_paths = {"src/main.cpp", "tests/tui.cpp", "docs/mvp.md", "goals/ledger.md", "\x1b[31mhidden.txt"}}}},
          .width = 120,
          .height = 16,
          .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(changed_paths_card,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("changed:") != std::string::npos && visible.find("src/main.cpp") != std::string::npos &&
                                      visible.find("+1 more") != std::string::npos && visible.find("\x1b[") == std::string::npos;
                             }) &&
             std::ranges::all_of(changed_paths_card, [](std::string const& line) { return visible_columns(line) <= 120; }),
         "tui expanded tool cards render bounded changed-file summaries even without a diff");

  auto const inferred_changed_tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                .name = "write_file",
                                                                .argument_summary = "path=notes/output.txt",
                                                                .result_summary = "wrote 12 bytes",
                                                                .result_json = "{\"tool\":\"write_file\",\"ok\":true,\"path\":\"notes/output.txt\"}",
                                                                .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                                                .details_visible = true,
                                                                .diff = "--- notes/output.txt\n+++ notes/output.txt\n-old\n+new"};
  auto const inferred_changed_card =
      ava::tui::render_composer(frontend.frontend(), ava::tui::ComposerSnapshot{.mode = "build",
                                                                                //.provider = "openai",
                                                                                //.model = "gpt-5.5",
                                                                                .session_id = "session_test",
                                                                                .input = "",
                                                                                .status = "ready",
                                                                                .transcript = {ava::tui::TranscriptItem{.tool = inferred_changed_tool}},
                                                                                .width = 88,
                                                                                .height = 14,
                                                                                .tool_presentation = ava::tui::ToolPresentation::Compact});
  auto const inferred_changed_copy = ava::tui::detail::tool_card_copy_text(inferred_changed_tool);
  expect(std::ranges::any_of(inferred_changed_card,
                             [](std::string const& line) { return strip_sgr(line).find("changed: notes/output.txt") != std::string::npos; }) &&
             std::ranges::any_of(inferred_changed_card,
                                 [](std::string const& line) { return strip_sgr(line).find("diff notes/output.txt:") != std::string::npos; }) &&
             inferred_changed_copy.find("changed: notes/output.txt") != std::string::npos &&
             ava::tui::detail::tool_card_matches_copy_query(inferred_changed_tool, "output.txt"),
         "tui infers changed-file summaries from mutating tool result JSON when timeline metadata is sparse");

  auto const wide_diff_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                                            .name = "edit_file",
                                                                                                            .argument_summary = "path=note.txt",
                                                                                                            .result_summary = "wrote 9 bytes",
                                                                                                            .details_visible = true,
                                                                                                            .diff = "--- note.txt\n+++ note.txt\n-old\n+new",
                                                                                                            .diff_truncated = true}}},
                                 .width = 88,
                                 .height = 14,
                                 .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(wide_diff_card, [](std::string const& line) { return strip_sgr(line).find("diff:") != std::string::npos; }) &&
             std::ranges::any_of(wide_diff_card,
                                 [](std::string const& line) {
                                   return strip_sgr(line).find("+new") != std::string::npos && line.find("\x1b[38;2;52;211;153m") != std::string::npos;
                                 }) &&
             std::ranges::any_of(wide_diff_card, [](std::string const& line) { return strip_sgr(line).find("diff truncated") != std::string::npos; }),
         "tui renders backend-provided unified diff previews with mutation colors and truncation markers");

  auto const narrow_diff_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                     .name = "edit_file",
                                                                                     .argument_summary = "path=very/long/path/to/note.txt",
                                                                                     .result_summary = "wrote 9 bytes",
                                                                                     .details_visible = true,
                                                                                     .diff = "--- very/long/path/to/note.txt\n+++ very/long/path/to/"
                                                                                             "note.txt\n-old value\n+new value"}}},
          .width = 36,
          .height = 14,
          .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(narrow_diff_card, [](std::string const& line) { return strip_sgr(line).find("diff:") != std::string::npos; }) &&
             std::ranges::all_of(narrow_diff_card, [](std::string const& line) { return visible_columns(line) <= 36; }),
         "tui keeps backend-provided diff previews width-safe on narrow terminals");

  auto const bash_ux_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Error,
                                                                                     .name = "bash",
                                                                                     .argument_summary = "command=ctest --test-dir build",
                                                                                     .result_summary = "exit 1",
                                                                                     .arguments_json = "{\"command\":\"ctest --test-dir build\"}",
                                                                                     .result_json = "{\"tool\":\"bash\",\"exit_code\":1,"
                                                                                                    "\"duration_ms\":1530,\"total_lines\":4,"
                                                                                                    "\"output_lines\":4,\"output\":"
                                                                                                    "\"configure\\nbuild\\nfail\\nsummary\"}"}}},
          .width = 72,
          .height = 16,
          .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(bash_ux_card,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("bash") != std::string::npos && visible.find("executing") == std::string::npos &&
                                      visible.find("exit 1 · 1.5s") != std::string::npos;
                             }) &&
             std::ranges::none_of(bash_ux_card,
                                  [](std::string const& line) { return strip_sgr(line).find("command=ctest --test-dir build") != std::string::npos; }) &&
             std::ranges::none_of(bash_ux_card,
                                  [](std::string const& line) {
                                    auto const visible = strip_sgr(line);
                                    return visible.find("output:") != std::string::npos || visible.find("configure") != std::string::npos;
                                  }) &&
             std::ranges::all_of(bash_ux_card, [](std::string const& line) { return visible_columns(line) <= 72; }),
         "tui collapsed bash cards keep status and duration on one header without output preview bodies");

  auto const expanded_bash_ux_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                     .name = "bash",
                                                                                     .argument_summary = "command=cmake --build build",
                                                                                     .result_summary = "exit 0",
                                                                                     .arguments_json = "{\"command\":\"cmake --build build\"}",
                                                                                     .result_json = "{\"tool\":\"bash\",\"exit_code\":0,"
                                                                                                    "\"duration_ms\":250,\"total_lines\":3,"
                                                                                                    "\"output_lines\":3,\"output\":"
                                                                                                    "\"[1/2] compile\\n[2/2] link\\nok\"}"}}},
          .width = 80,
          .height = 18,
          .tool_presentation = ava::tui::ToolPresentation::Expanded});
  expect(
      std::ranges::any_of(expanded_bash_ux_card, [](std::string const& line) { return strip_sgr(line).find("$ cmake --build build") != std::string::npos; }) &&
          std::ranges::any_of(expanded_bash_ux_card, [](std::string const& line) { return strip_sgr(line).find("exit 0 · 250ms") != std::string::npos; }) &&
          std::ranges::any_of(expanded_bash_ux_card, [](std::string const& line) { return strip_sgr(line).find("output:") != std::string::npos; }) &&
          std::ranges::any_of(expanded_bash_ux_card, [](std::string const& line) { return strip_sgr(line).find("[2/2] link") != std::string::npos; }),
      "tui expanded bash cards show an accented human call, status, duration, and retained output");

  auto const quoted_bash_summary_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                     .name = "bash",
                                                                                     .argument_summary = "command=echo \"a, b\", timeout=1000",
                                                                                     .result_summary = "exit 0",
                                                                                     .details_visible = true}}},
          .width = 80,
          .height = 14,
          .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(quoted_bash_summary_card, [](std::string const& line) { return strip_sgr(line).find("$ echo \"a, b\"") != std::string::npos; }),
         "tui bash human calls do not split fallback summaries at comma-space inside shell quotes");

  auto const escaped_bash_summary_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{
          .mode = "build",
          //.provider = "openai",
          //.model = "gpt-5.5",
          .session_id = "session_test",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                     .name = "bash",
                                                                                     .argument_summary = "command=printf foo\\, bar, timeout=1000",
                                                                                     .result_summary = "exit 0",
                                                                                     .details_visible = true}}},
          .width = 80,
          .height = 14,
          .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(
      std::ranges::any_of(escaped_bash_summary_card, [](std::string const& line) { return strip_sgr(line).find("$ printf foo\\, bar") != std::string::npos; }),
      "tui bash human calls do not split fallback summaries at escaped comma-space");

  auto const running_bash_ux_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Running,
                                                                                                            .name = "bash",
                                                                                                            .argument_summary = "command=long-test",
                                                                                                            .arguments_json = "{\"command\":\"long-test\"}"}}},
                                 .width = 52,
                                 .height = 10,
                                 .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(running_bash_ux_card,
                             [](std::string const& line) { return strip_sgr(line).find("~ bash · running · command=long-test") != std::string::npos; }) &&
             std::ranges::none_of(running_bash_ux_card, [](std::string const& line) { return strip_sgr(line).find("Esc/Ctrl+C stop") != std::string::npos; }) &&
             std::ranges::all_of(running_bash_ux_card, [](std::string const& line) { return visible_columns(line) <= 52; }),
         "tui resting running bash cards remain one concise primary row");

  auto const rich_diff_card = ava::tui::render_composer(
      frontend.frontend(),
      ava::tui::ComposerSnapshot{.mode = "build",
                                 //.provider = "openai",
                                 //.model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{
                                     .tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                        .name = "edit_file",
                                                                        .argument_summary = "path=note.txt",
                                                                        .result_summary = "edited note.txt",
                                                                        .details_visible = true,
                                                                        .diff = "--- note.txt\n+++ note.txt\n@@ -1,2 +1,2 @@\n-old\n+new\n context",
                                                                        .changed_paths = {"note.txt"}}}},
                                 .width = 92,
                                 .height = 18,
                                 .tool_presentation = ava::tui::ToolPresentation::Compact});
  expect(std::ranges::any_of(rich_diff_card, [](std::string const& line) { return strip_sgr(line).find("diff note.txt:") != std::string::npos; }) &&
             std::ranges::any_of(rich_diff_card, [](std::string const& line) { return strip_sgr(line).find("hunk @@ -1,2 +1,2 @@") != std::string::npos; }) &&
             std::ranges::any_of(rich_diff_card,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("1") != std::string::npos && visible.find("-old") != std::string::npos;
                                 }) &&
             std::ranges::any_of(rich_diff_card,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("1") != std::string::npos && visible.find("+new") != std::string::npos;
                                 }),
         "tui diff cards render unified hunk boundaries with line-numbered added and removed rows");
}
namespace {
void test_tui_large_tool_output_preview_is_bounded()
{
  auto const small_output_card = ava::tui::detail::render_tool_card(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                               .name = "grep",
                                                                                               .result_summary = "4 matches",
                                                                                               .result_json = "{\"output\":\"one\\ntwo\\nthree\\nfour\"}",
                                                                                               .lifecycle = ava::tui::ToolLifecycleState::Complete},
                                                                    96, false);
  expect(small_output_card.size() == 1 &&
             std::ranges::none_of(small_output_card, [](std::string const& line) { return strip_sgr(line).find("output:") != std::string::npos; }),
         "tui Compact tool cards omit output preview bodies");

  auto const rich_output_card = ava::tui::detail::render_tool_card(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                              .name = "grep",
                                                                                              .argument_summary = "pattern=needle",
                                                                                              .result_summary = "4 matches",
                                                                                              .result_json = "{\"output\":\"one\\ntwo\\nthree\\nfour\"}",
                                                                                              .lifecycle = ava::tui::ToolLifecycleState::Complete},
                                                                   96, ava::tui::ToolPresentation::Rich);
  auto const rich_output_text = tui_test_support::join_visible_lines(rich_output_card);
  expect(ava::tui::ComposerSnapshot{}.tool_presentation == ava::tui::ToolPresentation::Rich && rich_output_text.find("pattern=needle") != std::string::npos &&
             rich_output_text.find("output:") != std::string::npos && rich_output_text.find("four") != std::string::npos,
         "tui defaults to Rich cards with human calls and useful bounded output");

  std::string directory_entries = "{\"entries\":[";
  for (std::size_t index = 1; index <= 205; ++index)
  {
    if (index > 1)
      directory_entries += ',';
    auto number = std::to_string(index);
    auto const name = index == 3 ? std::string("unsafe\\u001b[31m") : "entry-" + std::string(3 - number.size(), '0') + number;
    directory_entries += "{\"name\":\"" + name + "\",\"type\":\"" + (index == 2 ? "directory" : "file") + "\"}";
  }
  directory_entries += "],\"truncated\":true,\"total_entries\":205}";
  auto const directory_item = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                         .name = "list_directory",
                                                         .argument_summary = "path=.",
                                                         .result_summary = "205 entries (truncated)",
                                                         .result_json = std::move(directory_entries),
                                                         .lifecycle = ava::tui::ToolLifecycleState::Complete};
  auto const directory_compact = ava::tui::detail::render_tool_card(directory_item, 96, ava::tui::ToolPresentation::Compact);
  auto const directory_rich = tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(directory_item, 96, ava::tui::ToolPresentation::Rich));
  auto const directory_expanded =
      tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(directory_item, 96, ava::tui::ToolPresentation::Expanded));
  expect(directory_compact.size() == 1 && directory_rich.find("entry-001") != std::string::npos && directory_rich.find("entry-002/") != std::string::npos &&
             directory_rich.find("entry-020") != std::string::npos && directory_rich.find("entry-021") == std::string::npos &&
             directory_rich.find("185 lines hidden") != std::string::npos && directory_expanded.find("entry-200") != std::string::npos &&
             directory_expanded.find("entry-201") == std::string::npos && directory_expanded.find("5 lines hidden") != std::string::npos &&
             directory_rich.find("unsafe?[31m") != std::string::npos && directory_rich.find("arguments provided") == std::string::npos &&
             directory_rich.find("result: ok") == std::string::npos,
         "tui list_directory cards keep Compact to one row and render bounded structured directory entries without placeholders");

  auto output_preview_text = [](std::string result_json) {
    auto const lines = ava::tui::detail::render_tool_card(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                     .name = "bash",
                                                                                     .result_summary = "exit 0",
                                                                                     .result_json = std::move(result_json),
                                                                                     .lifecycle = ava::tui::ToolLifecycleState::Complete},
                                                          96, true);
    return tui_test_support::join_visible_lines(lines);
  };
  auto const empty_preview = output_preview_text("{\"output\":\"\"}");
  auto const unterminated_preview = output_preview_text("{\"output\":\"one\"}");
  auto const lf_preview = output_preview_text("{\"output\":\"one\\n\"}");
  auto const crlf_preview = output_preview_text("{\"output\":\"one\\r\\n\"}");
  expect(empty_preview.find("output:") == std::string::npos && unterminated_preview.find("output:") != std::string::npos &&
             lf_preview.find("output:") != std::string::npos && crlf_preview.find("output:") != std::string::npos &&
             tui_test_support::count_occurrences(unterminated_preview, "one") == 1 && tui_test_support::count_occurrences(lf_preview, "one") == 1 &&
             tui_test_support::count_occurrences(crlf_preview, "one") == 1,
         "tui output previews handle empty, unterminated, LF-terminated, and CRLF-terminated logical lines without a synthetic trailing row");

  std::string thousand_line_json = "{\"output\":\"";
  for (std::size_t line = 1; line <= 1000; ++line)
  {
    auto number = std::to_string(line);
    thousand_line_json += "shell line " + std::string(4 - number.size(), '0') + number;
    if (line != 1000)
      thousand_line_json += "\\n";
  }
  thousand_line_json += "\"}";
  auto thousand_line_item = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                       .name = "bash",
                                                       .result_summary = "exit 0",
                                                       .result_json = thousand_line_json,
                                                       .lifecycle = ava::tui::ToolLifecycleState::Complete};
  auto const thousand_expanded =
      tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(thousand_line_item, 96, ava::tui::ToolPresentation::Expanded));
  auto const thousand_rich = tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(thousand_line_item, 96, ava::tui::ToolPresentation::Rich));
  expect(thousand_expanded.find("shell line 0801") != std::string::npos && thousand_expanded.find("shell line 1000") != std::string::npos &&
             thousand_expanded.find("shell line 0800") == std::string::npos && thousand_expanded.find("800 lines hidden") != std::string::npos,
         "tui Expanded shell previews select the final 200 lines from the complete source and report the exact hidden count");
  expect(thousand_rich.find("shell line 0996") != std::string::npos && thousand_rich.find("shell line 1000") != std::string::npos &&
             thousand_rich.find("shell line 0995") == std::string::npos && thousand_rich.find("995 lines hidden") != std::string::npos,
         "tui Rich shell previews select the final 5 lines from the complete source and report the exact hidden count");

  auto rich_preview = [](std::string name, std::size_t count) {
    std::string result_json = "{\"output\":\"";
    for (std::size_t line = 1; line <= count; ++line)
    {
      auto number = std::to_string(line);
      result_json += name + " cap line " + std::string(3 - number.size(), '0') + number;
      if (line != count)
        result_json += "\\n";
    }
    result_json += "\"}";
    return tui_test_support::join_visible_lines(
        ava::tui::detail::render_tool_card(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                      .name = std::move(name),
                                                                      .result_json = std::move(result_json),
                                                                      .lifecycle = ava::tui::ToolLifecycleState::Complete},
                                           96, ava::tui::ToolPresentation::Rich));
  };
  auto const grep_rich = rich_preview("grep", 20);
  auto const find_rich = rich_preview("find", 25);
  auto const read_rich = rich_preview("read_file", 12);
  expect(grep_rich.find("grep cap line 001") != std::string::npos && grep_rich.find("grep cap line 015") != std::string::npos &&
             grep_rich.find("grep cap line 016") == std::string::npos && grep_rich.find("5 lines hidden") != std::string::npos &&
             find_rich.find("find cap line 001") != std::string::npos && find_rich.find("find cap line 020") != std::string::npos &&
             find_rich.find("find cap line 021") == std::string::npos && find_rich.find("5 lines hidden") != std::string::npos &&
             read_rich.find("read_file cap line 001") != std::string::npos && read_rich.find("read_file cap line 010") != std::string::npos &&
             read_rich.find("read_file cap line 011") == std::string::npos && read_rich.find("2 lines hidden") != std::string::npos,
         "tui Rich output previews use Pi-aligned grep, listing, and file caps with exact hidden counts");

  std::string retained_crlf_json = "{\"output\":\"";
  for (std::size_t line = 901; line <= 1000; ++line)
    retained_crlf_json += "retained " + std::to_string(line) + "\\r\\n";
  retained_crlf_json += "\"}";
  auto const retained_crlf =
      tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                                         .name = "bash",
                                                                                                         .result_summary = "exit 0",
                                                                                                         .result_json = std::move(retained_crlf_json),
                                                                                                         .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                                                                                         .total_lines = 1000,
                                                                                                         .omitted_lines = 900},
                                                                              96, ava::tui::ToolPresentation::Expanded));
  expect(retained_crlf.find("retained 901") != std::string::npos && retained_crlf.find("retained 1000") != std::string::npos &&
             retained_crlf.find("900 lines hidden") != std::string::npos && retained_crlf.find("1000 lines hidden") == std::string::npos,
         "tui shell previews combine CRLF/trailing-newline sources with authoritative total/omitted counts without double counting");

  auto huge_line = std::string(512 * 1024 - 28, 'x') + "FINAL-HUGE-LINE";
  auto const huge_line_card = ava::tui::detail::render_tool_card(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                            .name = "bash",
                                                                                            .result_summary = "exit 0",
                                                                                            .result_json = "{\"output\":\"" + huge_line + "\"}",
                                                                                            .lifecycle = ava::tui::ToolLifecycleState::Complete},
                                                                 40, ava::tui::ToolPresentation::Expanded);
  auto const huge_line_text = tui_test_support::join_visible_lines(huge_line_card);
  expect(huge_line_card.size() <= 203, "tui huge single-line shell previews cap materialized visual rows");
  expect(huge_line_text.find("FINAL-HUGE-LINE") != std::string::npos, "tui huge single-line tail previews retain the exact final content");
  expect(huge_line_text.find("more output hidden") != std::string::npos,
         "tui huge single-line shell previews disclose conservative hidden output instead of a false exact count");
  expect(std::ranges::all_of(huge_line_card, [](std::string const& line) { return visible_columns(line) <= 40; }),
         "tui huge single-line shell preview rows remain width bounded");

  std::string seq_output_json = "{\"output\":\"";
  for (std::size_t line = 19801; line <= 20000; ++line)
    seq_output_json += std::to_string(line) + "\\n";
  seq_output_json += "\"}";
  auto const seq_preview_lines = ava::tui::detail::render_tool_card(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                               .name = "bash",
                                                                                               .result_summary = "exit 0",
                                                                                               .result_json = std::move(seq_output_json),
                                                                                               .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                                                                               .truncated = true,
                                                                                               .output_lines = 200,
                                                                                               .total_lines = 20000,
                                                                                               .omitted_lines = 19800},
                                                                    96, true);
  auto const seq_preview_text = tui_test_support::join_visible_lines(seq_preview_lines);

  expect(seq_preview_text.find("output:") != std::string::npos && seq_preview_text.find("19800 lines hidden") != std::string::npos &&
             seq_preview_text.find("20000") != std::string::npos && seq_preview_text.find("20001") == std::string::npos,
         "tui expanded seq-like output uses its defensive cap and authoritative hidden-line metadata after a final LF");

  constexpr auto total_lines = std::size_t{20000};
  std::string output_json = "{\"output\":\"";
  output_json.reserve(total_lines * 28);
  for (std::size_t index = 0; index < total_lines; ++index)
  {
    if (index > 0)
      output_json += "\\n";
    output_json += "large output line ";
    output_json += std::to_string(index);
  }
  output_json += "\"}";

  auto item = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                         .name = "grep",
                                         .argument_summary = "pattern=large",
                                         .result_summary = "20000 matches",
                                         .result_json = std::move(output_json),
                                         .lifecycle = ava::tui::ToolLifecycleState::Complete,
                                         .total_lines = total_lines};

  auto const start = std::chrono::steady_clock::now();
  auto const compact = ava::tui::detail::render_tool_card(item, 96, false);
  item.details_visible = true;
  auto const expanded = ava::tui::detail::render_tool_card(item, 96, true);
  auto const elapsed = std::chrono::steady_clock::now() - start;

  auto const compact_text = tui_test_support::join_visible_lines(compact);
  auto const expanded_text = tui_test_support::join_visible_lines(expanded);

  expect(compact.size() == 1 && compact_text.find("output:") == std::string::npos && compact_text.find("large output line") == std::string::npos,
         "tui collapsed tool cards never render large output preview bodies");
  expect(expanded_text.find("output:") != std::string::npos && expanded_text.find("large output line 0") != std::string::npos &&
             expanded_text.find("large output line 199") != std::string::npos && expanded_text.find("large output line 200") == std::string::npos &&
             expanded_text.find("large output line 19999") == std::string::npos && expanded.size() <= 205 &&
             std::ranges::all_of(expanded, [](std::string const& line) { return visible_columns(line) <= 96; }),
         "tui Expanded output remains defensively capped for large tool output");
  expect(elapsed < std::chrono::seconds(2), "tui large tool-output preview avoids pathological redraw cost");
}

void test_tui_f5_progressive_tool_details()
{
  tui_test_support::TuiFrontEndFixture frontend;
  auto const& joined = tui_test_support::join_plain_lines;
  using tui_test_support::plain_lines;
  auto row_text = [](ava::tui::ToolTimelineItem item) {
    auto const lines = ava::tui::detail::render_tool_card(item, 120, false);
    auto row = lines.empty() ? std::string{} : strip_sgr(lines.front());
    while (!row.empty() && row.back() == ' ')
      row.pop_back();
    return row;
  };
  for (auto const lifecycle :
       {ava::tui::ToolLifecycleState::ProviderAnnounced, ava::tui::ToolLifecycleState::ArgumentsStreaming, ava::tui::ToolLifecycleState::ArgumentsComplete})
  {
    auto const text = row_text(ava::tui::ToolTimelineItem{
        .status = ava::tui::ToolTimelineStatus::Running, .name = "read_file", .argument_summary = "path=src/pending.cpp", .lifecycle = lifecycle});
    expect(text.find("pending") != std::string::npos && text.find("running") == std::string::npos, "tui F5 pre-execution tool lifecycle is plainly pending");
  }
  for (auto const lifecycle : {ava::tui::ToolLifecycleState::ExecutionStarted, ava::tui::ToolLifecycleState::Progress})
  {
    auto const text = row_text(ava::tui::ToolTimelineItem{
        .status = ava::tui::ToolTimelineStatus::Running, .name = "read_file", .argument_summary = "path=src/running.cpp", .lifecycle = lifecycle});
    expect(text.find("running") != std::string::npos && text.find("pending") == std::string::npos, "tui F5 executing tool lifecycle is plainly running");
  }
  expect(row_text(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                             .name = "read_file",
                                             .result_summary = "12 lines",
                                             .lifecycle = ava::tui::ToolLifecycleState::Complete}) == "  │ + read_file · 12 lines" &&
             row_text(ava::tui::ToolTimelineItem{
                 .status = ava::tui::ToolTimelineStatus::Error, .name = "write_file", .lifecycle = ava::tui::ToolLifecycleState::Error}) ==
                 "  │ x write_file · failed" &&
             row_text(ava::tui::ToolTimelineItem{
                 .status = ava::tui::ToolTimelineStatus::Canceled, .name = "task", .lifecycle = ava::tui::ToolLifecycleState::Canceled}) ==
                 "  │ - task · canceled",
         "tui F5 settled cards preserve distinct success, error, and canceled markers without redundant lifecycle words");
  auto const compact_identity_row = row_text(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                        .name = "read_file",
                                                                        .result_summary = "call_private",
                                                                        .call_id = "call_private",
                                                                        .lifecycle = ava::tui::ToolLifecycleState::Complete});
  expect(compact_identity_row.find("call_private") == std::string::npos,
         "tui F5 compact summaries suppress explicit tool identifiers even when echoed by a result");

  auto detail_item = ava::tui::ToolTimelineItem{
      .status = ava::tui::ToolTimelineStatus::Error,
      .name = "edit_file",
      .argument_summary = "path=src/detail.cpp",
      .result_summary = "permission denied",
      .arguments_json = "{\"path\":\"src/detail.cpp\",\"edits\":[{}]}",
      .result_json = "{\"output\":\"permission denied\\nretained diagnostic\"}",
      .call_id = "call_detail",
      .request_id = "request_detail",
      .correlation_id = "correlation_detail",
      .lifecycle = ava::tui::ToolLifecycleState::Error,
      .permissions = {ava::tui::ToolPermissionAuditItem{
          .permission_request_id = "perm_detail", .resolver_request_id = "resolver_detail", .decision = "deny", .reason = "write was rejected"}},
      .diff = "--- src/detail.cpp\n+++ src/detail.cpp\n-old\n+new",
      .changed_paths = {"src/detail.cpp", "tests/hidden.cpp"},
      .truncated = true,
      .output_bytes = 40,
      .total_bytes = 400,
      .next_offset_line = 9,
      .omitted_bytes = 360,
      .spill_path = "/tmp/ava-spill/detail.txt",
      .spill_truncated = true};
  auto const collapsed = plain_lines(ava::tui::detail::render_tool_card(detail_item, 120, false));
  auto const expanded = plain_lines(ava::tui::detail::render_tool_card(detail_item, 160, true));
  auto const expanded_text = joined(expanded);
  auto const copy = ava::tui::detail::tool_card_copy_text(detail_item);
  expect(joined(collapsed).find("call_detail") == std::string::npos && joined(collapsed).find("resolver_detail") == std::string::npos &&
             joined(collapsed).find("write was rejected") != std::string::npos,
         "tui F5 compact denied cards retain a human reason without internal identities");
  expect(expanded_text.find("src/detail.cpp · 1 edit") != std::string::npos &&
             expanded_text.find("changed: src/detail.cpp, tests/hidden.cpp") != std::string::npos &&
             expanded_text.find("truncation: truncated 40/400 bytes; next offset 9; omitted 360 bytes") != std::string::npos &&
             expanded_text.find("full output: /tmp/ava-spill/detail.txt") != std::string::npos &&
             expanded_text.find("spill incomplete: true") != std::string::npos && expanded_text.find("-old") != std::string::npos &&
             expanded_text.find("+new") != std::string::npos,
         "tui F5 expanded cards retain human calls, changed paths, truncation, spill, and diff details");
  expect(expanded_text.find("call_detail") == std::string::npos && expanded_text.find("request_detail") == std::string::npos &&
             expanded_text.find("correlation_detail") == std::string::npos && expanded_text.find("perm_detail") == std::string::npos &&
             expanded_text.find("resolver_detail") == std::string::npos && expanded_text.find("toggle: /tool") == std::string::npos,
         "tui F5 expanded cards omit internal identities, routine audit receipts, and noisy action commands");
  expect(copy.find("call_detail") == std::string::npos && copy.find("request_detail") == std::string::npos &&
             copy.find("correlation_detail") == std::string::npos && copy.find("perm_detail") == std::string::npos &&
             copy.find("resolver_detail") == std::string::npos && copy.find("full output: /tmp/ava-spill/detail.txt") != std::string::npos &&
             copy.find('\x1b') == std::string::npos,
         "tui F5 copied tool details retain useful output metadata without internal identities or control bytes");

  auto const exact_payload = ava::tui::detail::render_tool_card(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                           .name = "payload_tool",
                                                                                           .result_summary = "exact payload",
                                                                                           .result_json = "{\"output\":\"exact payload\"}",
                                                                                           .lifecycle = ava::tui::ToolLifecycleState::Complete},
                                                                120, ava::tui::ToolPresentation::Rich);
  auto const distinct_payload = ava::tui::detail::render_tool_card(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                              .name = "payload_tool",
                                                                                              .result_summary = "a b",
                                                                                              .result_json = "{\"output\":\"a\\nb\"}",
                                                                                              .lifecycle = ava::tui::ToolLifecycleState::Complete},
                                                                   120, ava::tui::ToolPresentation::Rich);
  expect(joined(plain_lines(exact_payload)).find("output:") == std::string::npos && joined(plain_lines(distinct_payload)).find("output:") != std::string::npos,
         "tui F5 Rich cards suppress exact duplicate output while preserving structurally distinct output");

  std::vector<ava::tui::TranscriptItem> toggle_transcript{
      ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                  .name = "read_file",
                                                                  .call_id = "older",
                                                                  .lifecycle = ava::tui::ToolLifecycleState::Complete}},
      ava::tui::TranscriptItem{.label = "ava", .text = "between"},
      ava::tui::TranscriptItem{
          .tool = ava::tui::ToolTimelineItem{
              .status = ava::tui::ToolTimelineStatus::Success, .name = "read_file", .call_id = "latest", .lifecycle = ava::tui::ToolLifecycleState::Complete}}};
  auto const original_size = toggle_transcript.size();
  auto toggled = ava::tui::toggle_latest_matching_tool_details(toggle_transcript, "read_file", false);
  auto toggled_back = ava::tui::toggle_latest_matching_tool_details(toggle_transcript, "latest", false);
  expect(toggled && *toggled == 2 && toggled_back && *toggled_back == 2 && toggle_transcript.size() == original_size && toggle_transcript[0].tool &&
             !toggle_transcript[0].tool->details_visible && toggle_transcript[2].tool && toggle_transcript[2].tool->details_visible == false,
         "tui F5 exact tool routing toggles the latest matching original card in place without duplicating transcript items");

  auto hit_snapshot = ava::tui::ComposerSnapshot{
      .mode = "build",
      //.provider = "openai",
      //.model = "gpt-5.5",
      .session_id = "session_hit",
      .input = "draft",
      .status = "ready",
      .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "intro"},
                     ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                 .name = "glob",
                                                                                 .result_summary = "4 files",
                                                                                 .call_id = "glob_hit",
                                                                                 .lifecycle = ava::tui::ToolLifecycleState::Complete}},
                     ava::tui::TranscriptItem{.tool = detail_item}, ava::tui::TranscriptItem{.label = "ava", .text = "tail"}},
      .width = 160,
      .height = 48,
      .sidebar = ava::tui::SidebarSnapshot{.session_id = "session_hit"}};
  auto const hit_frame = plain_lines(ava::tui::render_composer(frontend.frontend(), hit_snapshot));
  auto const intro_line = std::ranges::find_if(hit_frame, [](std::string const& line) { return line.find("intro") != std::string::npos; });
  auto const glob_line = std::ranges::find_if(hit_frame, [](std::string const& line) { return line.find("+ glob") != std::string::npos; });
  auto const detail_line = std::ranges::find_if(hit_frame, [](std::string const& line) { return line.find("x edit_file") != std::string::npos; });
  auto const heading_line = std::ranges::find_if(hit_frame, [](std::string const& line) { return line.find("context gathering") != std::string::npos; });
  auto const intro_row = intro_line == hit_frame.end() ? 0 : static_cast<std::size_t>(intro_line - hit_frame.begin()) + 1;
  auto const glob_row = glob_line == hit_frame.end() ? 0 : static_cast<std::size_t>(glob_line - hit_frame.begin()) + 1;
  auto const detail_row = detail_line == hit_frame.end() ? 0 : static_cast<std::size_t>(detail_line - hit_frame.begin()) + 1;
  auto const heading_row = heading_line == hit_frame.end() ? 0 : static_cast<std::size_t>(heading_line - hit_frame.begin()) + 1;
  auto const hit_canvas = ava::tui::composer_canvas_layout(hit_snapshot);
  expect(hit_canvas.content_width == 120 && hit_canvas.left == 20 &&
             ava::tui::detail::transcript_tool_card_header_for_screen_position(hit_snapshot, glob_row, 23) == 1 &&
             ava::tui::detail::transcript_tool_card_header_for_screen_position(hit_snapshot, detail_row, 138) == 2 &&
             !ava::tui::detail::transcript_tool_card_header_for_screen_position(hit_snapshot, intro_row, 24) &&
             !ava::tui::detail::transcript_tool_card_header_for_screen_position(hit_snapshot, heading_row, 24) &&
             !ava::tui::detail::transcript_tool_card_header_for_screen_position(hit_snapshot, detail_row, 22) &&
             !ava::tui::detail::transcript_tool_card_header_for_screen_position(hit_snapshot, detail_row, 139) &&
             !ava::tui::detail::transcript_tool_card_header_for_screen_position(hit_snapshot, hit_snapshot.height, 24),
         "tui F5 tool header hit testing uses centered transcript geometry and rejects headings, both two-cell card margins, and composer rows");

  hit_snapshot.transcript[2].tool->details_visible = true;
  auto expanded_hit_frame = plain_lines(ava::tui::render_composer(frontend.frontend(), hit_snapshot));
  auto const expanded_detail_line =
      std::ranges::find_if(expanded_hit_frame, [](std::string const& line) { return line.find("x edit_file") != std::string::npos; });
  auto const expanded_detail_row =
      expanded_detail_line == expanded_hit_frame.end() ? 0 : static_cast<std::size_t>(expanded_detail_line - expanded_hit_frame.begin()) + 1;
  expect(ava::tui::detail::transcript_tool_card_header_for_screen_position(hit_snapshot, expanded_detail_row, 28) == 2 &&
             !ava::tui::detail::transcript_tool_card_header_for_screen_position(hit_snapshot, expanded_detail_row + 1, 28),
         "tui F5 per-card expansion hit testing toggles only the original header and rejects payload rows");
  hit_snapshot.tool_presentation = ava::tui::ToolPresentation::Expanded;
  expect(ava::tui::detail::transcript_tool_card_header_for_screen_position(hit_snapshot, glob_row, 28) == 1,
         "tui F5 header hit testing shares global expansion geometry");

  auto short_clipped = hit_snapshot;
  short_clipped.width = 100;
  short_clipped.height = 12;
  short_clipped.sidebar.reset();
  short_clipped.transcript = {ava::tui::TranscriptItem{.tool = detail_item}};
  short_clipped.transcript.front().tool->details_visible = true;
  bool clipped_header_hit = false;
  for (std::size_t row = 1; row <= short_clipped.height; ++row)
  {
    clipped_header_hit = clipped_header_hit || ava::tui::detail::transcript_tool_card_header_for_screen_position(short_clipped, row, 8).has_value();
  }
  expect(!clipped_header_hit, "tui F5 short viewports reject visible expanded payload rows when the original card header is clipped offscreen");

  auto detached = hit_snapshot;
  detached.width = 80;
  detached.height = 24;
  detached.tool_presentation = ava::tui::ToolPresentation::Compact;
  detached.sidebar.reset();
  detached.transcript.insert(detached.transcript.begin(), 30, ava::tui::TranscriptItem{.label = "you", .text = "older transcript line"});
  detached.transcript.insert(detached.transcript.end(), 30, ava::tui::TranscriptItem{.label = "ava", .text = "newer transcript line"});
  auto const detached_layout = ava::tui::detail::render_transcript_layout(detached.transcript, ava::tui::composer_main_width(detached), false, true, false);
  auto const detached_tool = std::ranges::find(detached_layout.message_item_indices, std::size_t{31});
  auto const detached_position = static_cast<std::size_t>(detached_tool - detached_layout.message_item_indices.begin());
  auto const max_scroll = ava::tui::composer_max_transcript_scroll_offset(detached, detached.width, detached.height);
  auto const desired_start = detached_layout.content_starts[detached_position] - 1;
  detached.transcript_scroll_offset = max_scroll - std::min(max_scroll, desired_start);
  expect(!ava::tui::detail::transcript_tool_card_header_for_screen_position(detached, 1, 4) &&
             ava::tui::detail::transcript_tool_card_header_for_screen_position(detached, 2, 4) == 31,
         "tui F5 detached transcript hit testing rejects the prose-to-tool spacer and maps the anchored visible tool header without a banner row");

  for (auto const& [width, height] : {std::pair{std::size_t{160}, std::size_t{48}}, std::pair{std::size_t{120}, std::size_t{36}},
                                      std::pair{std::size_t{80}, std::size_t{24}}, std::pair{std::size_t{100}, std::size_t{12}}})
  {
    auto bounded = hit_snapshot;
    bounded.width = width;
    bounded.height = height;
    bounded.sidebar.reset();
    auto const frame = ava::tui::render_composer(frontend.frontend(), bounded);
    expect(frame.size() == height && std::ranges::all_of(frame, [width](std::string const& line) { return visible_columns(line) <= width; }),
           "tui F5 progressive tool details stay within requested frame dimensions");
  }
  {
    ScopedEnvVar no_color("NO_COLOR", "1");
    auto plain = hit_snapshot;
    plain.width = 100;
    plain.height = 12;
    plain.sidebar.reset();
    auto const frame = ava::tui::render_composer(frontend.frontend(), plain);
    expect(frame.size() == 12 &&
               std::ranges::all_of(frame, [](std::string const& line) { return line.find('\x1b') == std::string::npos && visible_columns(line) <= 100; }),
           "tui F5 progressive tool details retain compact NO_COLOR bounds");
  }
}

bool line_has_intraline_emphasis(std::string const& line)
{
  // Changed middle is marked with bold + underline while the role color remains.
  return line.find("\x1b[1m") != std::string::npos && line.find("\x1b[4m") != std::string::npos;
}

std::string find_plain_diff_line(std::vector<std::string> const& lines, std::string_view needle)
{
  for (auto const& line : lines)
  {
    auto const plain = strip_sgr(line);
    if (plain.find(needle) != std::string::npos)
      return plain;
  }
  return {};
}

std::string find_styled_diff_line(std::vector<std::string> const& lines, std::string_view needle)
{
  for (auto const& line : lines)
  {
    if (strip_sgr(line).find(needle) != std::string::npos)
      return line;
  }
  return {};
}

void test_tui_diff_intraline_emphasis()
{
  tui_test_support::TuiFrontEndFixture frontend;
  auto const one_to_one =
      ava::tui::detail::render_unified_diff_body("--- note.txt\n+++ note.txt\n@@ -1,1 +1,1 @@\n-hello world\n+hello earth\n", false, 120, "", 20);
  auto const removed = find_styled_diff_line(one_to_one, "-hello world");
  auto const added = find_styled_diff_line(one_to_one, "+hello earth");
  expect(!removed.empty() && !added.empty() && line_has_intraline_emphasis(removed) && line_has_intraline_emphasis(added) &&
             removed.find("\x1b[38;2;248;113;113m") != std::string::npos && added.find("\x1b[38;2;52;211;153m") != std::string::npos &&
             strip_sgr(removed).find("1") != std::string::npos && strip_sgr(added).find("1") != std::string::npos &&
             strip_sgr(removed).find("-hello world") != std::string::npos && strip_sgr(added).find("+hello earth") != std::string::npos,
         "tui 1:1 replacement pairs emphasize the changed middle while keeping role colors and gutters");

  auto const multi =
      ava::tui::detail::render_unified_diff_body("--- m.txt\n+++ m.txt\n@@ -1,2 +1,2 @@\n-alpha one\n-beta two\n+alpha ONE\n+beta TWO\n", false, 120, "", 20);
  expect(line_has_intraline_emphasis(find_styled_diff_line(multi, "-alpha one")) && line_has_intraline_emphasis(find_styled_diff_line(multi, "+alpha ONE")) &&
             line_has_intraline_emphasis(find_styled_diff_line(multi, "-beta two")) && line_has_intraline_emphasis(find_styled_diff_line(multi, "+beta TWO")),
         "tui equal multi-line replacement runs pair by ordinal for intra-line emphasis");

  auto const unequal = ava::tui::detail::render_unified_diff_body("--- u.txt\n+++ u.txt\n@@ -1,2 +1,1 @@\n-first\n-second\n+only\n", false, 120, "", 20);
  expect(!line_has_intraline_emphasis(find_styled_diff_line(unequal, "-first")) && !line_has_intraline_emphasis(find_styled_diff_line(unequal, "-second")) &&
             !line_has_intraline_emphasis(find_styled_diff_line(unequal, "+only")),
         "tui unequal replacement runs keep whole-line diff styling without intra-line pairs");

  // "café 🌍 end" vs "café 🌎 end" — shared UTF-8/emoji prefix and suffix.
  auto const utf8 = ava::tui::detail::render_unified_diff_body("--- e.txt\n+++ e.txt\n@@ -1 +1 @@\n-café 🌍 end\n+café 🌎 end\n", false, 120, "", 20);
  auto const utf8_removed = find_styled_diff_line(utf8, "café 🌍 end");
  auto const utf8_added = find_styled_diff_line(utf8, "café 🌎 end");
  expect(line_has_intraline_emphasis(utf8_removed) && line_has_intraline_emphasis(utf8_added) &&
             strip_sgr(utf8_removed).find("café 🌍 end") != std::string::npos && strip_sgr(utf8_added).find("café 🌎 end") != std::string::npos,
         "tui intra-line prefix/suffix matching respects UTF-8 and emoji cluster boundaries");

  // Both sides carry a visible non-empty middle (punctuation vs spaces).
  auto const whitespace_both = ava::tui::detail::render_unified_diff_body("--- w.txt\n+++ w.txt\n@@ -1 +1 @@\n-a-b\n+a b\n", false, 120, "", 20);
  auto const ws_both_removed = find_styled_diff_line(whitespace_both, "-a-b");
  auto const ws_both_added = find_styled_diff_line(whitespace_both, "+a b");
  expect(line_has_intraline_emphasis(ws_both_removed) && line_has_intraline_emphasis(ws_both_added) &&
             strip_sgr(ws_both_removed).find("-a-b") != std::string::npos && strip_sgr(ws_both_added).find("+a b") != std::string::npos,
         "tui whitespace/punctuation replacement middles stay emphasizeable on both sides");

  // Extra spaces only on one side: emphasize the visible middle, never fabricate glyphs.
  auto const whitespace_one = ava::tui::detail::render_unified_diff_body("--- w2.txt\n+++ w2.txt\n@@ -1 +1 @@\n-a b\n+a  b\n", false, 120, "", 20);
  auto const ws_one_removed = find_styled_diff_line(whitespace_one, "-a b");
  auto const ws_one_added = find_styled_diff_line(whitespace_one, "+a  b");
  expect(!ws_one_removed.empty() && !ws_one_added.empty() && line_has_intraline_emphasis(ws_one_added) &&
             strip_sgr(ws_one_removed).find("-a b") != std::string::npos && strip_sgr(ws_one_added).find("+a  b") != std::string::npos &&
             strip_sgr(ws_one_added).find("+a   b") == std::string::npos,
         "tui one-sided whitespace-only middles emphasize only visible spans without fabricated glyphs");

  auto const wrapped = ava::tui::detail::render_unified_diff_body(
      "--- long.txt\n+++ long.txt\n@@ -1 +1 @@\n-prefix CHANGED_OLD trailing material that forces width wrapping in the diff body\n"
      "+prefix CHANGED_NEW trailing material that forces width wrapping in the diff body\n",
      false, 48, "  ", 20);
  expect(std::ranges::all_of(wrapped, [](std::string const& line) { return visible_columns(line) <= 48; }) &&
             std::ranges::any_of(wrapped, [](std::string const& line) { return strip_sgr(line).find("prefix") != std::string::npos; }),
         "tui long intra-line diff rows remain width-safe under existing wrapping");

  auto const numbered = ava::tui::detail::render_unified_diff_body("--- n.txt\n+++ n.txt\n@@ -10,1 +10,1 @@\n-value old\n+value new\n", false, 120, "", 20);
  auto const numbered_removed = find_plain_diff_line(numbered, "-value old");
  auto const numbered_added = find_plain_diff_line(numbered, "+value new");
  expect(numbered_removed.find("10") != std::string::npos && numbered_added.find("10") != std::string::npos &&
             line_has_intraline_emphasis(find_styled_diff_line(numbered, "-value old")),
         "tui intra-line emphasis preserves existing line-number gutters");

  // Exact plain-text parity: stripped markup matches a no-emphasis structural reading.
  auto const parity_source = "--- p.txt\n+++ p.txt\n@@ -1,2 +1,2 @@\n-keep change_me tail\n context\n+keep CHANGE_ME tail\n context\n";
  auto const parity = ava::tui::detail::render_unified_diff_body(parity_source, false, 120, "|", 20);
  expect(find_plain_diff_line(parity, "-keep change_me tail").find("|") != std::string::npos &&
             find_plain_diff_line(parity, "+keep CHANGE_ME tail").find("keep CHANGE_ME tail") != std::string::npos &&
             find_plain_diff_line(parity, "context").find("context") != std::string::npos &&
             std::ranges::none_of(parity,
                                  [](std::string const& line) {
                                    auto const plain = strip_sgr(line);
                                    return plain.find("\x1b") != std::string::npos;
                                  }),
         "tui stripped intra-line diff markup preserves exact plain-text content and prefixes");

  // Cap/truncation markers remain unchanged when the body exceeds max_lines.
  std::string many_lines = "--- t.txt\n+++ t.txt\n@@ -1,12 +1,12 @@\n";
  for (int i = 0; i < 12; ++i)
  {
    many_lines += "-old" + std::to_string(i) + "\n";
    many_lines += "+new" + std::to_string(i) + "\n";
  }
  auto const capped = ava::tui::detail::render_unified_diff_body(many_lines, true, 100, "", 6);
  expect(capped.size() == 6 &&
             std::ranges::any_of(capped, [](std::string const& line) { return strip_sgr(line).find("diff lines hidden") != std::string::npos; }) &&
             std::ranges::any_of(capped, [](std::string const& line) { return strip_sgr(line).find("[diff truncated]") != std::string::npos; }),
         "tui intra-line emphasis leaves existing diff row caps and truncation markers unchanged");

  {
    ScopedEnvVar no_color("NO_COLOR", "1");
    auto const plain_card = ava::tui::render_composer(
        frontend.frontend(),
        ava::tui::ComposerSnapshot{.mode = "build",
                                   //.provider = "openai",
                                   //.model = "gpt-5.5",
                                   .session_id = "session_test",
                                   .input = "",
                                   .status = "ready",
                                   .transcript = {ava::tui::TranscriptItem{
                                       .tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                          .name = "edit_file",
                                                                          .argument_summary = "path=note.txt",
                                                                          .result_summary = "edited",
                                                                          .details_visible = true,
                                                                          .diff = "--- note.txt\n+++ note.txt\n@@ -1 +1 @@\n-hello world\n+hello earth\n",
                                                                          .changed_paths = {"note.txt"}}}},
                                   .width = 100,
                                   .height = 18,
                                   .tool_presentation = ava::tui::ToolPresentation::Compact});
    expect(std::ranges::all_of(plain_card, [](std::string const& line) { return line.find('\x1b') == std::string::npos; }) &&
               std::ranges::any_of(plain_card, [](std::string const& line) { return line.find("-hello world") != std::string::npos; }) &&
               std::ranges::any_of(plain_card, [](std::string const& line) { return line.find("+hello earth") != std::string::npos; }),
           "tui NO_COLOR/plain fallback strips intra-line emphasis and retains readable diff text");
  }
}

void test_tui_task_job_tool_card_presentation()
{
  using tui_test_support::join_visible_lines;
  using tui_test_support::plain_lines;

  // Frozen production task serializer field set plus model-facing XML in content.
  // Production task result_text fields (tool_dispatch_task.cpp):
  // tool, ok, task_id, subagent_type, description, session_path, state, optional job_id,
  // stop_reason, provider_iterations, tool_calls, tool_iterations, task_result, content.
  // No duration_ms/elapsed_ms/runtime_ms.
  constexpr std::string_view kTaskId = "task_7f3a91c2e8b04d15";
  constexpr std::string_view kJobId = "job_c0ffee1234abcd56";
  constexpr std::string_view kSessionPath = "/home/user/.local/share/ava/sessions/child_explore_auth.jsonl";
  constexpr std::string_view kPrompt = "Inspect authentication entry points and report handler names only.";
  constexpr std::string_view kDescription = "scan auth handlers";
  constexpr std::string_view kTaskResult = "Found 3 auth handlers in src/auth.";
  constexpr std::string_view kCodeLikeResult = "use std::vector<int> when a < b && tag != \"</div>\"";
  auto const xml_content = std::string("<task id=\"") + std::string(kTaskId) + "\" job_id=\"" + std::string(kJobId) +
                           "\" state=\"completed\"><summary>Subagent task completed: " + std::string(kDescription) + "</summary><task_result>" +
                           std::string(kTaskResult) + "</task_result></task>";
  auto const j = [](std::string_view value) { return ava::core::json::escape(value); };

  auto const task_arguments = std::string("{\"description\":\"") + j(kDescription) + "\",\"prompt\":\"" + j(kPrompt) +
                              "\",\"subagent_type\":\"explore\",\"mode\":\"foreground\",\"task_id\":\"" + j(kTaskId) + "\"}";
  auto const task_result_json = std::string("{\"tool\":\"task\",\"ok\":true,\"task_id\":\"") + j(kTaskId) +
                                "\",\"subagent_type\":\"explore\",\"description\":\"" + j(kDescription) + "\",\"session_path\":\"" + j(kSessionPath) +
                                "\",\"state\":\"completed\",\"job_id\":\"" + j(kJobId) +
                                "\",\"stop_reason\":\"completed\",\"provider_iterations\":2,\"tool_calls\":4,\"tool_iterations\":4,\"task_result\":\"" +
                                j(kTaskResult) + "\",\"content\":\"" + j(xml_content) + "\"}";

  auto completed_task = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                   .name = "task",
                                                   .argument_summary = "arguments provided",
                                                   .result_summary = "ok",
                                                   .arguments_json = task_arguments,
                                                   .result_json = task_result_json,
                                                   .call_id = "call_task_completed",
                                                   .lifecycle = ava::tui::ToolLifecycleState::Complete};

  auto running_fg = completed_task;
  running_fg.status = ava::tui::ToolTimelineStatus::Running;
  running_fg.lifecycle = ava::tui::ToolLifecycleState::Progress;
  running_fg.result_summary.clear();
  running_fg.result_json.clear();

  auto background_started = completed_task;
  background_started.arguments_json =
      std::string("{\"description\":\"") + j(kDescription) + "\",\"prompt\":\"" + j(kPrompt) + "\",\"subagent_type\":\"explore\",\"mode\":\"background\"}";
  auto const bg_xml = std::string("<task id=\"") + std::string(kTaskId) + "\" job_id=\"" + std::string(kJobId) +
                      "\" state=\"running\"><summary>Background subagent task started: " + std::string(kDescription) +
                      "</summary><task_result></task_result></task>";
  background_started.result_json =
      std::string("{\"tool\":\"task\",\"ok\":true,\"task_id\":\"") + j(kTaskId) + "\",\"subagent_type\":\"explore\",\"description\":\"" + j(kDescription) +
      "\",\"session_path\":\"" + j(kSessionPath) + "\",\"state\":\"running\",\"job_id\":\"" + j(kJobId) +
      "\",\"stop_reason\":\"\",\"provider_iterations\":0,\"tool_calls\":0,\"tool_iterations\":0,\"task_result\":\"\",\"content\":\"" + j(bg_xml) + "\"}";

  auto canceled_task = completed_task;
  canceled_task.status = ava::tui::ToolTimelineStatus::Canceled;
  canceled_task.lifecycle = ava::tui::ToolLifecycleState::Canceled;
  canceled_task.result_json = std::string("{\"tool\":\"task\",\"ok\":false,\"task_id\":\"") + j(kTaskId) +
                              "\",\"subagent_type\":\"explore\",\"description\":\"" + j(kDescription) + "\",\"session_path\":\"" + j(kSessionPath) +
                              "\",\"state\":\"canceled\"}";

  auto error_task = completed_task;
  error_task.status = ava::tui::ToolTimelineStatus::Error;
  error_task.lifecycle = ava::tui::ToolLifecycleState::Error;
  error_task.result_summary = "task failed";
  error_task.result_json = std::string("{\"tool\":\"task\",\"ok\":false,\"error\":{\"message\":\"boom\",\"task_id\":\"") + j(kTaskId) +
                           "\"},\"description\":\"" + j(kDescription) + "\",\"subagent_type\":\"explore\"}";

  auto malformed_task = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                   .name = "task",
                                                   .argument_summary = "arguments provided",
                                                   .result_summary = xml_content,
                                                   .arguments_json = "{not-json",
                                                   .result_json = "<task id=\"raw\">not json</task>",
                                                   .call_id = "call_task_malformed",
                                                   .lifecycle = ava::tui::ToolLifecycleState::Complete};

  auto unicode_task = completed_task;
  unicode_task.arguments_json =
      "{\"description\":\"caf\\u00e9 \\ud83c\\udf0d auth\\u0007\",\"prompt\":\"secret prompt path=/tmp/x\",\"subagent_type\":\"explore\"}";
  unicode_task.result_json =
      std::string("{\"tool\":\"task\",\"ok\":true,\"task_id\":\"") + j(kTaskId) +
      "\",\"subagent_type\":\"explore\",\"description\":\"caf\\u00e9 \\ud83c\\udf0d auth\\u0007\",\"session_path\":\"" + j(kSessionPath) +
      "\",\"state\":\"completed\",\"tool_calls\":1,\"task_result\":\"ok \\u0001 plain \\u4e2d\\u6587\",\"content\":\"" + j(xml_content) + "\"}";

  // Description and body ending near multi-byte cluster boundaries for UTF-8-safe caps.
  auto cluster_boundary_task = completed_task;
  {
    std::string desc_prefix(150, 'D');
    // emoji + combining-ish CJK near the 160-byte description cap
    std::string desc = desc_prefix + "🌍中文";
    std::string body_prefix(4090, 'B');
    std::string body = body_prefix + "🌍中";
    cluster_boundary_task.arguments_json =
        std::string("{\"description\":\"") + j(desc) + "\",\"prompt\":\"" + j(kPrompt) + "\",\"subagent_type\":\"explore\",\"mode\":\"foreground\"}";
    cluster_boundary_task.result_json = std::string("{\"tool\":\"task\",\"ok\":true,\"task_id\":\"") + j(kTaskId) +
                                        "\",\"subagent_type\":\"explore\",\"description\":\"" + j(desc) + "\",\"session_path\":\"" + j(kSessionPath) +
                                        "\",\"state\":\"completed\",\"tool_calls\":1,\"task_result\":\"" + j(body) + "\",\"content\":\"" + j(xml_content) +
                                        "\"}";
  }

  auto code_like_task = completed_task;
  code_like_task.result_json = std::string("{\"tool\":\"task\",\"ok\":true,\"task_id\":\"") + j(kTaskId) +
                               "\",\"subagent_type\":\"explore\",\"description\":\"" + j(kDescription) + "\",\"session_path\":\"" + j(kSessionPath) +
                               "\",\"state\":\"completed\",\"tool_calls\":1,\"task_result\":\"" + j(kCodeLikeResult) + "\",\"content\":\"" + j(xml_content) +
                               "\"}";

  auto long_result_task = completed_task;
  {
    std::string long_body(6000, 'R');
    long_result_task.result_json = std::string("{\"tool\":\"task\",\"ok\":true,\"task_id\":\"") + j(kTaskId) +
                                   "\",\"subagent_type\":\"explore\",\"description\":\"" + j(kDescription) + "\",\"session_path\":\"" + j(kSessionPath) +
                                   "\",\"state\":\"completed\",\"tool_calls\":2,\"task_result\":\"" + j(long_body) + "\",\"content\":\"" + j(xml_content) +
                                   "\"}";
  }

  auto duplicate_description_task = completed_task;
  duplicate_description_task.argument_summary = std::string("description=") + std::string(kDescription);
  duplicate_description_task.result_summary = std::string("Subagent task completed: ") + std::string(kDescription);

  auto spoofed_task_name = completed_task;
  spoofed_task_name.name = "TASK";

  auto job_status_running =
      ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                 .name = "job",
                                 .argument_summary = "arguments provided",
                                 .result_summary = "ok",
                                 .arguments_json = std::string("{\"action\":\"status\",\"job_id\":\"") + j(kJobId) + "\"}",
                                 .result_json = std::string("{\"schema_version\":1,\"job_id\":\"") + j(kJobId) + "\",\"task_id\":\"" + j(kTaskId) +
                                                "\",\"parent_session_id\":\"parent_sess\",\"child_session_id\":\"child_sess\",\"delivery_id\":\"deliv_1\","
                                                "\"mode\":\"background\",\"state\":\"running\",\"delivery_state\":\"none\",\"was_promoted\":false,"
                                                "\"cancel_requested\":false,\"timed_out\":false,\"started_at\":\"t0\",\"updated_at\":\"t1\","
                                                "\"delivery_attempts\":0,\"summary_truncated\":false,\"error_truncated\":false,\"stop_reason_truncated\":false,"
                                                "\"provider_iterations\":1,\"tool_calls\":2,\"tool_iterations\":2}",
                                 .call_id = "call_job_status",
                                 .lifecycle = ava::tui::ToolLifecycleState::Complete};

  auto job_result_completed = job_status_running;
  job_result_completed.arguments_json = std::string("{\"action\":\"result\",\"job_id\":\"") + j(kJobId) + "\"}";
  job_result_completed.result_json = std::string("{\"schema_version\":1,\"job_id\":\"") + j(kJobId) + "\",\"task_id\":\"" + j(kTaskId) +
                                     "\",\"parent_session_id\":\"parent_sess\",\"child_session_id\":\"child_sess\",\"delivery_id\":\"deliv_1\","
                                     "\"mode\":\"background\",\"state\":\"completed\",\"delivery_state\":\"pending\",\"was_promoted\":true,"
                                     "\"cancel_requested\":false,\"timed_out\":false,\"started_at\":\"t0\",\"updated_at\":\"t2\","
                                     "\"delivery_attempts\":0,\"summary_truncated\":false,\"error_truncated\":false,\"stop_reason_truncated\":false,"
                                     "\"provider_iterations\":3,\"tool_calls\":5,\"tool_iterations\":5,"
                                     "\"result\":{\"status\":\"completed\",\"summary\":\"auth scan finished\"}}";

  auto job_list = job_status_running;
  job_list.arguments_json = "{\"action\":\"list\"}";
  job_list.result_json = std::string("{\"schema_version\":1,\"jobs\":[{\"schema_version\":1,\"job_id\":\"") + j(kJobId) + "\",\"task_id\":\"" + j(kTaskId) +
                         "\",\"mode\":\"background\",\"state\":\"running\",\"was_promoted\":false,\"tool_calls\":1}],"
                         "\"total_jobs\":1,\"truncated\":false}";

  auto running_job_list = job_list;
  running_job_list.status = ava::tui::ToolTimelineStatus::Running;
  running_job_list.lifecycle = ava::tui::ToolLifecycleState::ExecutionStarted;
  running_job_list.result_summary.clear();
  running_job_list.result_json.clear();

  auto running_job_status = running_job_list;
  running_job_status.arguments_json = std::string("{\"action\":\"status\",\"job_id\":\"") + j(kJobId) + "\"}";
  auto running_job_wait = running_job_list;
  running_job_wait.arguments_json = std::string("{\"action\":\"wait\",\"job_id\":\"") + j(kJobId) + "\",\"timeout_ms\":1000}";

  auto job_status_starting = job_status_running;
  job_status_starting.result_json = std::string("{\"schema_version\":1,\"job_id\":\"") + j(kJobId) + "\",\"state\":\"starting\"}";
  auto job_wait_timed_out = job_status_running;
  job_wait_timed_out.arguments_json = std::string("{\"action\":\"wait\",\"job_id\":\"") + j(kJobId) + "\",\"timeout_ms\":1000}";
  job_wait_timed_out.result_json = std::string("{\"schema_version\":1,\"job_id\":\"") + j(kJobId) + "\",\"state\":\"running\",\"timed_out\":true}";

  auto job_status_completed = job_status_running;
  job_status_completed.result_json = std::string("{\"schema_version\":1,\"job_id\":\"") + j(kJobId) + "\",\"state\":\"completed\"}";
  auto job_wait_failed = job_wait_timed_out;
  job_wait_failed.result_json = std::string("{\"schema_version\":1,\"job_id\":\"") + j(kJobId) + "\",\"state\":\"failed\",\"timed_out\":false}";
  auto job_status_canceled = job_status_running;
  job_status_canceled.result_json = std::string("{\"schema_version\":1,\"job_id\":\"") + j(kJobId) + "\",\"state\":\"canceled\"}";
  auto job_status_unknown = job_status_running;
  job_status_unknown.result_json = std::string("{\"schema_version\":1,\"job_id\":\"") + j(kJobId) + "\",\"state\":\"queued\"}";
  auto job_unknown_action = job_status_running;
  job_unknown_action.arguments_json = "{\"action\":\"inspect\"}";
  auto job_cancel = job_status_running;
  job_cancel.arguments_json = std::string("{\"action\":\"cancel\",\"job_id\":\"") + j(kJobId) + "\"}";
  job_cancel.result_json = std::string("{\"schema_version\":1,\"job_id\":\"") + j(kJobId) + "\",\"state\":\"running\",\"cancel_requested\":true}";
  auto canceled_job_poll = job_status_running;
  canceled_job_poll.status = ava::tui::ToolTimelineStatus::Canceled;
  canceled_job_poll.lifecycle = ava::tui::ToolLifecycleState::Canceled;
  auto malformed_job_list = job_list;
  malformed_job_list.result_json = "{\"total_jobs\":1}";

  auto job_failed = job_status_running;
  job_failed.status = ava::tui::ToolTimelineStatus::Error;
  job_failed.lifecycle = ava::tui::ToolLifecycleState::Error;
  job_failed.arguments_json = std::string("{\"action\":\"result\",\"job_id\":\"") + j(kJobId) + "\"}";
  job_failed.result_json = "{\"tool\":\"job\",\"ok\":false,\"error\":{\"category\":\"tool\",\"code\":\"job_error\",\"message\":\"missing\"}}";

  auto job_malformed = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                  .name = "job",
                                                  .argument_summary = "arguments provided",
                                                  .result_summary = std::string(kJobId),
                                                  .arguments_json = "not-json",
                                                  .result_json = "{bad",
                                                  .call_id = "call_job_bad",
                                                  .lifecycle = ava::tui::ToolLifecycleState::Complete};

  auto const forbidden = std::vector<std::string_view>{kTaskId,
                                                       kJobId,
                                                       kSessionPath,
                                                       kPrompt,
                                                       "session_path",
                                                       "task_id",
                                                       "job_id",
                                                       "schema_version",
                                                       "parent_session_id",
                                                       "child_session_id",
                                                       "delivery_id",
                                                       "<task ",
                                                       "</task>",
                                                       "<summary>",
                                                       "arguments provided",
                                                       "call_task_completed",
                                                       "call_job_status",
                                                       "/home/user",
                                                       "secret prompt",
                                                       "duration_ms",
                                                       "2.4s",
                                                       "2400ms"};

  auto assert_no_forbidden = [&](std::string const& text, std::string_view label) {
    for (auto const needle : forbidden)
    {
      expect(text.find(needle) == std::string::npos,
             std::string("tui task/job ") + std::string(label) + " must not contain forbidden token: " + std::string(needle));
    }
  };

  auto render_all = [](ava::tui::ToolTimelineItem const& item, ava::tui::ToolPresentation presentation) {
    std::vector<std::string> combined;
    for (auto const width : {std::size_t{32}, std::size_t{40}, std::size_t{80}, std::size_t{120}})
    {
      auto lines = ava::tui::detail::render_tool_card(item, width, presentation);
      expect(std::ranges::all_of(lines, [&](std::string const& line) { return visible_columns(line) <= width; }),
             "tui task/job cards stay within requested width");
      for (auto const& line : lines)
      {
        auto const plain = strip_sgr(line);
        expect(ava::core::json::is_valid_utf8(plain), "tui task/job rendered lines remain valid UTF-8");
      }
      combined.insert(combined.end(), lines.begin(), lines.end());
    }
    return combined;
  };

  auto check_item = [&](ava::tui::ToolTimelineItem const& item, std::string_view label, auto const& header_check) {
    for (auto const presentation : {ava::tui::ToolPresentation::Compact, ava::tui::ToolPresentation::Rich, ava::tui::ToolPresentation::Expanded})
    {
      auto const lines = render_all(item, presentation);
      auto const visible = join_visible_lines(lines);
      auto const plain = tui_test_support::join_plain_lines(plain_lines(lines));
      assert_no_forbidden(visible, std::string(label) + " render");
      assert_no_forbidden(plain, std::string(label) + " plain render");
      if (presentation == ava::tui::ToolPresentation::Compact)
      {
        expect(lines.size() == 4, std::string("tui task/job compact cards stay one line per width for ") + std::string(label));
      }
      if (presentation == ava::tui::ToolPresentation::Rich && item.name == "task")
      {
        expect(lines.size() == 4, std::string("tui rich task cards stay single-line for ") + std::string(label));
        expect(plain.find("result:") == std::string::npos, std::string("tui rich task cards omit task_result body for ") + std::string(label));
      }
      header_check(plain, presentation);
    }

    // Default collapsed/Rich copy and search must not include or match task_result.
    auto const default_copy = ava::tui::detail::tool_card_copy_text(item);
    assert_no_forbidden(default_copy, std::string(label) + " default copy");
    expect(ava::core::json::is_valid_utf8(default_copy), std::string("tui task/job default copy is valid UTF-8 for ") + std::string(label));
    if (item.name == "task" && !std::string(kTaskResult).empty())
    {
      expect(default_copy.find(std::string(kTaskResult)) == std::string::npos && default_copy.find("task_result:") == std::string::npos &&
                 !ava::tui::detail::tool_card_matches_copy_query(item, std::string(kTaskResult)),
             std::string("tui default task copy/search omit task_result for ") + std::string(label));
    }

    expect(!ava::tui::detail::tool_card_matches_copy_query(item, std::string(kPrompt)),
           std::string("tui task/job search must not match prompt for ") + std::string(label));
    expect(!ava::tui::detail::tool_card_matches_copy_query(item, std::string(kTaskId)),
           std::string("tui task/job search must not match task_id for ") + std::string(label));
    expect(!ava::tui::detail::tool_card_matches_copy_query(item, std::string(kJobId)),
           std::string("tui task/job search must not match job_id for ") + std::string(label));
    expect(!ava::tui::detail::tool_card_matches_copy_query(item, std::string(kSessionPath)),
           std::string("tui task/job search must not match session_path for ") + std::string(label));
    expect(!ava::tui::detail::tool_card_matches_copy_query(item, "<task"),
           std::string("tui task/job search must not match XML content wrapper for ") + std::string(label));
    expect(!ava::tui::detail::tool_card_matches_copy_query(item, "schema_version"),
           std::string("tui task/job search must not match schema keys for ") + std::string(label));
  };

  check_item(running_fg, "running foreground task", [&](std::string const& plain, ava::tui::ToolPresentation) {
    expect(plain.find("Explore") != std::string::npos && plain.find(std::string(kDescription)) != std::string::npos &&
               plain.find("running") != std::string::npos && plain.find("~") != std::string::npos,
           "tui running foreground task cards show human type, description, and running");
    expect(plain.find("running in background") == std::string::npos, "tui foreground running task is not labeled background");
  });

  check_item(background_started, "background started task", [&](std::string const& plain, ava::tui::ToolPresentation) {
    expect(plain.find("Explore") != std::string::npos && plain.find(std::string(kDescription)) != std::string::npos &&
               plain.find("running in background") != std::string::npos && plain.find("+") != std::string::npos,
           "tui background task start cards show human type, description, and background running");
  });

  check_item(completed_task, "completed task", [&](std::string const& plain, ava::tui::ToolPresentation presentation) {
    expect(
        plain.find("Explore") != std::string::npos && plain.find(std::string(kDescription)) != std::string::npos && plain.find("4 tools") != std::string::npos,
        "tui completed task cards show type, description, and tool count");
    expect(plain.find("2.4s") == std::string::npos && plain.find("duration") == std::string::npos, "tui completed task cards do not invent duration fields");
    for (auto const& line : plain_lines(ava::tui::detail::render_tool_card(completed_task, 120, presentation)))
    {
      auto const pos = line.find(std::string(kDescription));
      if (pos == std::string::npos)
        continue;
      expect(line.find(std::string(kDescription), pos + 1) == std::string::npos, "tui completed task card lines do not duplicate description");
    }
    if (presentation == ava::tui::ToolPresentation::Expanded)
    {
      expect(plain.find(std::string(kTaskResult)) != std::string::npos, "tui expanded completed task may show plain task_result");
    }
    else if (presentation == ava::tui::ToolPresentation::Rich)
    {
      expect(plain.find(std::string(kTaskResult)) == std::string::npos, "tui rich completed task omits task_result body");
    }
  });

  check_item(canceled_task, "canceled task", [&](std::string const& plain, ava::tui::ToolPresentation) {
    expect(plain.find("Explore") != std::string::npos && plain.find("canceled") != std::string::npos && plain.find("-") != std::string::npos,
           "tui canceled task cards retain truthful canceled status with human type");
  });

  check_item(error_task, "failed task", [&](std::string const& plain, ava::tui::ToolPresentation) {
    expect(plain.find("Explore") != std::string::npos && plain.find("failed") != std::string::npos && plain.find("x") != std::string::npos,
           "tui failed task cards retain truthful failed status with human type");
  });

  check_item(malformed_task, "malformed task", [&](std::string const& plain, ava::tui::ToolPresentation) {
    expect(plain.find("task") != std::string::npos, "tui malformed task falls back to safe generic task card");
    expect(plain.find("arguments provided") == std::string::npos && plain.find("{not-json") == std::string::npos,
           "tui malformed task never falls back to raw JSON or generic argument placeholders");
  });

  check_item(unicode_task, "unicode task", [&](std::string const& plain, ava::tui::ToolPresentation presentation) {
    expect(plain.find("Explore") != std::string::npos, "tui unicode task keeps humanized type");
    expect(plain.find('\x07') == std::string::npos && plain.find('\x01') == std::string::npos, "tui unicode/control task cards strip control bytes");
    if (presentation == ava::tui::ToolPresentation::Expanded)
    {
      expect(plain.find("plain") != std::string::npos, "tui expanded unicode task_result keeps sanitized plain text");
    }
  });

  check_item(long_result_task, "long result task", [&](std::string const& plain, ava::tui::ToolPresentation presentation) {
    if (presentation == ava::tui::ToolPresentation::Expanded)
    {
      expect(plain.find(std::string(5000, 'R')) == std::string::npos, "tui expanded task_result is bounded for long bodies");
      expect(plain.find("result:") != std::string::npos, "tui expanded long task still exposes a bounded result body");
    }
  });

  check_item(duplicate_description_task, "duplicate description task", [&](std::string const& plain, ava::tui::ToolPresentation presentation) {
    (void)plain;
    for (auto const& line : plain_lines(ava::tui::detail::render_tool_card(duplicate_description_task, 120, presentation)))
    {
      auto const pos = line.find(std::string(kDescription));
      if (pos == std::string::npos)
        continue;
      expect(line.find(std::string(kDescription), pos + 1) == std::string::npos,
             "tui task cards do not duplicate description on one line when summaries echo it");
    }
  });

  auto assert_quiet_poll = [&](ava::tui::ToolTimelineItem const& item, std::string_view label) {
    expect(ava::tui::detail::task_job_card_is_quiet_poll(item), std::string("tui recognizes quiet job poll: ") + std::string(label));
    expect(ava::tui::detail::render_tool_card(item, 120, ava::tui::ToolPresentation::Expanded).empty(),
           std::string("tui quiet job poll renders no transcript rows: ") + std::string(label));
    expect(!ava::tui::detail::tool_card_matches_copy_query(item, {}) && !ava::tui::detail::tool_card_matches_copy_query(item, "job") &&
               !ava::tui::detail::tool_card_matches_copy_query(item, "running") && ava::tui::detail::tool_card_copy_text(item).empty() &&
               ava::tui::detail::task_job_card_copy_text(item, ava::tui::detail::task_job_card_presentation(item)).empty(),
           std::string("tui quiet job poll is not a search, copy, or details-toggle candidate: ") + std::string(label));
  };
  assert_quiet_poll(running_job_list, "running list start");
  assert_quiet_poll(running_job_status, "running status start");
  assert_quiet_poll(running_job_wait, "running wait start");
  assert_quiet_poll(job_list, "successful list");
  assert_quiet_poll(job_status_starting, "starting status result");
  assert_quiet_poll(job_status_running, "running status result");
  assert_quiet_poll(job_wait_timed_out, "timed-out running wait result");

  auto assert_visible_job = [&](ava::tui::ToolTimelineItem const& item, std::string_view label) {
    expect(!ava::tui::detail::task_job_card_is_quiet_poll(item), std::string("tui retains visible job outcome: ") + std::string(label));
    expect(!ava::tui::detail::render_tool_card(item, 120, ava::tui::ToolPresentation::Compact).empty() &&
               ava::tui::detail::tool_card_matches_copy_query(item, {}) && !ava::tui::detail::tool_card_copy_text(item).empty(),
           std::string("tui visible job outcome remains renderable, copyable, and toggleable: ") + std::string(label));
  };
  assert_visible_job(malformed_job_list, "malformed list result");
  assert_visible_job(job_malformed, "malformed payload");
  assert_visible_job(job_failed, "error result");
  assert_visible_job(canceled_job_poll, "canceled status poll");
  assert_visible_job(job_status_completed, "completed status");
  assert_visible_job(job_wait_failed, "failed wait");
  assert_visible_job(job_status_canceled, "terminal canceled status");
  assert_visible_job(job_status_unknown, "unknown state");
  assert_visible_job(job_unknown_action, "unknown action");
  assert_visible_job(job_result_completed, "result retrieval");
  assert_visible_job(job_cancel, "cancel request");

  check_item(job_result_completed, "job result completed", [&](std::string const& plain, ava::tui::ToolPresentation) {
    expect(plain.find("job") != std::string::npos && plain.find("result") != std::string::npos && plain.find("completed") != std::string::npos &&
               plain.find("promoted") != std::string::npos && plain.find("auth scan finished") != std::string::npos,
           "tui job result cards show action, state, promotion, and stable summary");
  });

  check_item(job_failed, "job failed", [&](std::string const& plain, ava::tui::ToolPresentation) {
    expect(plain.find("job") != std::string::npos && plain.find("failed") != std::string::npos, "tui failed job cards retain truthful failed status");
  });

  check_item(job_malformed, "job malformed", [&](std::string const& plain, ava::tui::ToolPresentation) {
    expect(plain.find("job") != std::string::npos, "tui malformed job falls back to safe generic job card");
    expect(plain.find("not-json") == std::string::npos, "tui malformed job never falls back to raw JSON");
  });

  // TJ-01: explicit details_visible expansion enables task_result in copy/search.
  auto explicit_expanded = completed_task;
  explicit_expanded.details_visible = true;
  auto const expanded_copy = ava::tui::detail::tool_card_copy_text(explicit_expanded);
  assert_no_forbidden(expanded_copy, "explicit expanded copy");
  expect(expanded_copy.find(std::string(kTaskResult)) != std::string::npos && expanded_copy.find("task_result:") != std::string::npos &&
             expanded_copy.find(std::string(kDescription)) != std::string::npos && ava::core::json::is_valid_utf8(expanded_copy),
         "tui explicitly expanded task copy includes allowlisted description and plain task_result only");
  expect(ava::tui::detail::tool_card_matches_copy_query(explicit_expanded, std::string(kTaskResult)) &&
             ava::tui::detail::tool_card_matches_copy_query(completed_task, std::string(kDescription)) &&
             ava::tui::detail::tool_card_matches_copy_query(completed_task, "Explore") &&
             !ava::tui::detail::tool_card_matches_copy_query(completed_task, std::string(kTaskResult)) &&
             ava::tui::detail::tool_card_matches_copy_query(job_result_completed, "auth scan finished"),
         "tui task/job search matches allowlisted fields and task_result only when explicitly expanded");

  // Inherited Expanded render still shows body without details_visible, but copy stays closed.
  auto inherited_expanded_render = plain_lines(ava::tui::detail::render_tool_card(completed_task, 120, ava::tui::ToolPresentation::Expanded));
  auto const inherited_plain = tui_test_support::join_plain_lines(inherited_expanded_render);
  expect(inherited_plain.find(std::string(kTaskResult)) != std::string::npos &&
             ava::tui::detail::tool_card_copy_text(completed_task).find(std::string(kTaskResult)) == std::string::npos &&
             !ava::tui::detail::tool_card_matches_copy_query(completed_task, std::string(kTaskResult)),
         "tui inherited Expanded render may show task_result while default copy/search stay closed");

  // TJ-04: code-like task_result preserved under explicit expansion; content XML still forbidden.
  auto code_expanded = code_like_task;
  code_expanded.details_visible = true;
  auto const code_copy = ava::tui::detail::tool_card_copy_text(code_expanded);
  auto const code_expanded_plain =
      tui_test_support::join_plain_lines(plain_lines(ava::tui::detail::render_tool_card(code_expanded, 120, ava::tui::ToolPresentation::Expanded)));
  expect(code_copy.find("std::vector<int>") != std::string::npos && code_copy.find("a < b") != std::string::npos &&
             code_expanded_plain.find("std::vector<int>") != std::string::npos && code_expanded_plain.find("a < b") != std::string::npos &&
             code_copy.find(std::string(kTaskId)) == std::string::npos && code_copy.find("<task id=") == std::string::npos &&
             !ava::tui::detail::tool_card_matches_copy_query(code_expanded, std::string(xml_content)),
         "tui expanded task_result preserves code-like angle brackets while content XML stays unselected");

  // TJ-03: UTF-8/cluster-safe truncation near description and expanded-body boundaries.
  auto cluster_expanded = cluster_boundary_task;
  cluster_expanded.details_visible = true;
  auto const cluster_copy = ava::tui::detail::tool_card_copy_text(cluster_expanded);
  auto const cluster_header =
      tui_test_support::join_plain_lines(plain_lines(ava::tui::detail::render_tool_card(cluster_boundary_task, 120, ava::tui::ToolPresentation::Compact)));
  auto const cluster_body =
      tui_test_support::join_plain_lines(plain_lines(ava::tui::detail::render_tool_card(cluster_expanded, 120, ava::tui::ToolPresentation::Expanded)));
  expect(ava::core::json::is_valid_utf8(cluster_copy) && ava::core::json::is_valid_utf8(cluster_header) && ava::core::json::is_valid_utf8(cluster_body) &&
             cluster_copy.find("\xFF") == std::string::npos &&
             std::ranges::all_of(ava::tui::detail::render_tool_card(cluster_expanded, 80, ava::tui::ToolPresentation::Expanded),
                                 [](std::string const& line) { return visible_columns(line) <= 80 && ava::core::json::is_valid_utf8(strip_sgr(line)); }),
         "tui task description/body caps preserve valid UTF-8 clusters under width and copy");

  auto launch_task = completed_task;
  launch_task.subagent_launch_display = ava::agent::SubagentLaunchDisplay::normalized("GPT-5.6 Terra", std::string_view("high"));
  for (auto const presentation : {ava::tui::ToolPresentation::Compact, ava::tui::ToolPresentation::Rich, ava::tui::ToolPresentation::Expanded})
  {
    auto const launch_lines = ava::tui::detail::render_tool_card(launch_task, 80, presentation);
    auto const launch_screen = tui_test_support::join_visible_lines(launch_lines);
    expect(launch_screen.find("launch: GPT-5.6 Terra · thinking high") != std::string::npos,
           "live task cards render a dedicated launch row in every presentation");
  }
  auto const narrow_launch_lines = ava::tui::detail::render_tool_card(launch_task, 32, ava::tui::ToolPresentation::Compact);
  auto const narrow_launch_screen = tui_test_support::join_visible_lines(narrow_launch_lines);
  auto narrow_launch_compact = narrow_launch_screen;
  std::erase_if(narrow_launch_compact, [](unsigned char ch) { return ch == ' ' || ch == '\n'; });
  expect(narrow_launch_lines.size() > 2 && std::ranges::all_of(narrow_launch_lines, [](auto const& line) { return visible_columns(line) <= 32; }) &&
             narrow_launch_compact.find("launch:GPT-5.6Terra·thinkinghigh") != std::string::npos,
         "narrow live task launch rows wrap within the requested width");

  auto reasoning_only_task = completed_task;
  reasoning_only_task.subagent_launch_display = ava::agent::SubagentLaunchDisplay::normalized("", std::string_view("high"));
  auto const reasoning_only_screen =
      tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(reasoning_only_task, 80, ava::tui::ToolPresentation::Compact));
  expect(reasoning_only_screen.find("launch: thinking high") != std::string::npos && reasoning_only_screen.find("launch: ·") == std::string::npos,
         "task launch rows show exact reasoning without inferring a missing model name");

  auto const launch_presentation = ava::tui::detail::task_job_card_presentation(launch_task);
  auto const launch_copy = ava::tui::detail::tool_card_copy_text(launch_task);
  expect(launch_copy.find("GPT-5.6 Terra") == std::string::npos && launch_copy.find("thinking high") == std::string::npos &&
             launch_presentation.primary.find("GPT-5.6 Terra") == std::string::npos &&
             launch_presentation.searchable_text.find("GPT-5.6 Terra") == std::string::npos &&
             launch_presentation.expanded_detail.find("GPT-5.6 Terra") == std::string::npos &&
             !ava::tui::detail::tool_card_matches_copy_query(launch_task, "GPT-5.6 Terra") &&
             !ava::tui::detail::tool_card_matches_copy_query(launch_task, "thinking high"),
         "task launch metadata is absent from copy, primary, details, and matching corpora");

  auto historical_task = completed_task;
  auto const historical_screen =
      tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(historical_task, 80, ava::tui::ToolPresentation::Expanded));
  auto unrelated_tool = completed_task;
  unrelated_tool.name = "plugin_task";
  unrelated_tool.subagent_launch_display = launch_task.subagent_launch_display;
  auto job_with_metadata = job_result_completed;
  job_with_metadata.subagent_launch_display = launch_task.subagent_launch_display;
  expect(historical_screen.find("launch:") == std::string::npos &&
             tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(unrelated_tool, 80, ava::tui::ToolPresentation::Expanded))
                     .find("GPT-5.6 Terra") == std::string::npos &&
             tui_test_support::join_visible_lines(ava::tui::detail::render_tool_card(job_with_metadata, 80, ava::tui::ToolPresentation::Expanded))
                     .find("GPT-5.6 Terra") == std::string::npos,
         "historical tasks, arbitrary tools, and job polling cards never consume task launch metadata");

  // Exact reserved names only — case spoofs keep generic tool behavior (may show raw summaries).
  spoofed_task_name.subagent_launch_display = launch_task.subagent_launch_display;
  auto const spoof_row = plain_lines(ava::tui::detail::render_tool_card(spoofed_task_name, 120, ava::tui::ToolPresentation::Compact));
  expect(!spoof_row.empty() && strip_sgr(spoof_row.front()).find("TASK") != std::string::npos &&
             tui_test_support::join_plain_lines(spoof_row).find("GPT-5.6 Terra") == std::string::npos,
         "tui case-spoofed task names do not enter the reserved task presentation adapter");
}
}  // namespace

void test_tui_todowrite_tool_card_checklist()
{
  auto const success_json =
      R"({"schema_version":1,"tool":"todowrite","ok":true,"todos":[{"id":"a","content":"First","status":"completed"},{"id":"b","content":"Second","status":"in_progress"},{"id":"c","content":"Third","status":"pending"}],"counts":{"total":3,"pending":1,"in_progress":1,"completed":1}})";
  auto const card = ava::tui::detail::render_tool_card(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success,
                                                                                  .name = "todowrite",
                                                                                  .result_summary = "1/3 completed",
                                                                                  .result_json = success_json,
                                                                                  .lifecycle = ava::tui::ToolLifecycleState::Complete},
                                                       100, ava::tui::ToolPresentation::Rich, false);
  expect(std::ranges::any_of(card,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("todowrite") != std::string::npos && visible.find("1/3 completed") != std::string::npos;
                             }) &&
             std::ranges::any_of(card, [](std::string const& line) { return strip_sgr(line).find("#a First") != std::string::npos; }) &&
             std::ranges::any_of(card, [](std::string const& line) { return strip_sgr(line).find("#b Second") != std::string::npos; }) &&
             std::ranges::none_of(card, [](std::string const& line) { return strip_sgr(line).find("schema_version") != std::string::npos; }),
         "todowrite success cards render a human checklist instead of raw JSON");

  auto const clear_card = ava::tui::detail::render_tool_card(
      ava::tui::ToolTimelineItem{
          .status = ava::tui::ToolTimelineStatus::Success,
          .name = "todowrite",
          .result_summary = "todos cleared",
          .result_json = R"({"schema_version":1,"tool":"todowrite","ok":true,"todos":[],"counts":{"total":0,"pending":0,"in_progress":0,"completed":0}})",
          .lifecycle = ava::tui::ToolLifecycleState::Complete},
      80, ava::tui::ToolPresentation::Rich, false);
  expect(std::ranges::any_of(clear_card, [](std::string const& line) { return strip_sgr(line).find("todos cleared") != std::string::npos; }),
         "todowrite clear cards say todos cleared");

  auto const error_card = ava::tui::detail::render_tool_card(ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Error,
                                                                                        .name = "todowrite",
                                                                                        .result_summary = "todo id is invalid",
                                                                                        .result_json = R"({"schema_version":1,"tool":"todowrite","ok":false})",
                                                                                        .lifecycle = ava::tui::ToolLifecycleState::Error},
                                                             80, ava::tui::ToolPresentation::Compact, false);
  expect(std::ranges::any_of(error_card,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("x todowrite") != std::string::npos && visible.find("todo id is invalid") != std::string::npos;
                             }),
         "todowrite error cards remain ordinary error cards");
}

void run_tui_tool_card_detail_tests()
{
  test_tui_large_tool_output_preview_is_bounded();
  test_tui_f5_progressive_tool_details();
  test_tui_diff_intraline_emphasis();
  test_tui_task_job_tool_card_presentation();
  test_tui_todowrite_tool_card_checklist();
}
