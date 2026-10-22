#include "sys.h"
#include "tests/app_runtime_test_declarations.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/test_harness.h"
#include "ava/app/command_sessions.h"
#include "ava/app/commands.h"
#include "ava/app/runtime/Session.h"
#include "ava/agent/agent_loop.h"
#include "ava/session/assistant_output.h"
#include "ava/session/export.h"
#include "ava/session/record.h"
#include "ava/session/session_store.h"
#include "ava/session/stats.h"
#include "ava/session/validation.h"

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>
#include <sys/stat.h>
#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_runtime_tests {

using namespace ava::tests;

// Exercise session export, import, statistics, and lifecycle commands.
//
// unlocked_session supplies the runtime session and must be unlocked on entry; workspace supplies the fixture paths. The function mutates the session's
// store, active imported session, and appended test records while exercising command dispatch.
void app_command_dispatcher_session_part(ava::app::runtime::session_ts& unlocked_session, std::filesystem::path const& workspace)
{
  auto session_id = [&] { return ava::app::runtime::session_ts::rat(unlocked_session)->store.session_id(); };
  auto session_path = [&] { return ava::app::runtime::session_ts::rat(unlocked_session)->store.session_path(); };
  auto load_entries = [&] { return ava::app::runtime::session_ts::rat(unlocked_session)->store.load(); };
  auto append_owned = [&](ava::session::SessionEntry entry) { return ava::app::runtime::session_ts::wat(unlocked_session)->append_owned(std::move(entry)); };
  auto seeded_compaction = ava::session::make_manual_compaction_entry({.summary = "Session export fixture summary",
                                                                       .instructions = "Export the explicit checkpoint",
                                                                       .config = ava::session::default_compaction_config(),
                                                                       .estimated_tokens = 0,
                                                                       .threshold_tokens = 0,
                                                                       .retained_tokens = 0,
                                                                       .trigger = "manual",
                                                                       .recent_context = {},
                                                                       .recent_context_omitted = false});
  if (!require(seeded_compaction.has_value(), "session command fixture creates its own compaction checkpoint"))
    return;
  auto checkpoint_appended = append_owned(std::move(*seeded_compaction));
  if (!require(checkpoint_appended.has_value(), "session command fixture appends its own compaction checkpoint"))
    return;

  auto exported = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/export"});
  expect(exported && exported->handled && !exported->output.empty() && exported->output[0].find("# AVA Session Export") != std::string::npos &&
             exported->output[0].find("## Compaction") != std::string::npos,
         "command dispatcher /export returns markdown for loaded session entries");
  auto exported_html = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/export html"});
  expect(exported_html && exported_html->handled && !exported_html->output.empty() && exported_html->output[0].find("<!doctype html>") != std::string::npos &&
             exported_html->output[0].find("<title>AVA Session Export</title>") != std::string::npos &&
             exported_html->output[0].find("# AVA Session Export") != std::string::npos,
         "command dispatcher /export html returns a self-contained HTML session export");
  auto exported_html_file = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/export session-export.html"});
  auto const exported_html_path = workspace / "session-export.html";
  std::ifstream exported_html_input(exported_html_path, std::ios::binary);
  std::ostringstream exported_html_file_text;
  exported_html_file_text << exported_html_input.rdbuf();
  expect(exported_html_file && exported_html_file->handled && !exported_html_file->output.empty() &&
             exported_html_file->output[0].find("format: html") != std::string::npos && exported_html_file->tool_timeline.size() == 2 &&
             exported_html_file->tool_timeline[1].status == ava::agent::ToolTimelineStatus::Success &&
             exported_html_file->tool_timeline[1].structured_result_json.find("\"tool\":\"export\"") != std::string::npos &&
             exported_html_file_text.str().find("<!doctype html>") != std::string::npos &&
             exported_html_file_text.str().find("# AVA Session Export") != std::string::npos,
         "command dispatcher /export <file.html> writes Pi-style HTML through command-side tool metadata");
  auto exported_markdown_file = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/export markdown session-export.md"});
  auto const exported_markdown_path = workspace / "session-export.md";
  std::ifstream exported_markdown_input(exported_markdown_path, std::ios::binary);
  std::ostringstream exported_markdown_file_text;
  exported_markdown_file_text << exported_markdown_input.rdbuf();
  expect(exported_markdown_file && exported_markdown_file->handled && !exported_markdown_file->output.empty() &&
             exported_markdown_file->output[0].find("format: markdown") != std::string::npos &&
             exported_markdown_file_text.str().find("# AVA Session Export") != std::string::npos &&
             exported_markdown_file_text.str().find("<!doctype html>") == std::string::npos,
         "command dispatcher /export markdown <path> keeps explicit Markdown file export available");
  auto exported_jsonl_file = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/export jsonl session-export.jsonl"});
  auto const exported_jsonl_path = workspace / "session-export.jsonl";
  std::ifstream exported_jsonl_input(exported_jsonl_path, std::ios::binary);
  std::ostringstream exported_jsonl_file_text;
  exported_jsonl_file_text << exported_jsonl_input.rdbuf();
  expect(exported_jsonl_file && exported_jsonl_file->handled && !exported_jsonl_file->output.empty() &&
             exported_jsonl_file->output[0].find("format: jsonl") != std::string::npos &&
             exported_jsonl_file_text.str().find("\"type\":\"session_start\"") != std::string::npos,
         "command dispatcher /export jsonl <path> writes a raw AVA session JSONL archive");

  auto const pre_failed_import_session_id = session_id();
  auto const empty_import_path = workspace / "empty-import.jsonl";
  {
    std::ofstream empty_import(empty_import_path, std::ios::binary | std::ios::trunc);
  }
  auto empty_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import empty-import.jsonl --confirm"});
  expect(empty_import && empty_import->handled && !empty_import->output.empty() &&
             empty_import->output[0].find("session import file has no entries") != std::string::npos && session_id() == pre_failed_import_session_id,
         "command dispatcher /import rejects empty archives without switching sessions");
  auto const malformed_import_path = workspace / "malformed-import.jsonl";
  write_app_test_file(malformed_import_path, "not json\n");
  auto malformed_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import malformed-import.jsonl --confirm"});
  expect(malformed_import && malformed_import->handled && !malformed_import->output.empty() &&
             malformed_import->output[0].find("malformed session entry") != std::string::npos && session_id() == pre_failed_import_session_id,
         "command dispatcher /import surfaces malformed JSONL parse errors without switching sessions");
  auto const pi_import_path = workspace / "pi-import.jsonl";
  write_app_test_file(pi_import_path,
                      "{\"type\":\"session\",\"version\":3,\"id\":\"d703a1a9-1b7b-4fb1-b512-c9738b1fe617\","
                      "\"timestamp\":\"2025-11-20T23:33:50.805Z\",\"cwd\":\"/tmp/pi-project\"}\n"
                      "{\"type\":\"message\",\"id\":\"a1b2c3d4\",\"parentId\":null,"
                      "\"timestamp\":\"2025-11-20T23:33:51.000Z\",\"message\":{\"role\":\"user\",\"content\":\"Hello\"}}\n");
  auto pi_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import pi-import.jsonl --confirm"});
  expect(pi_import && pi_import->handled && !pi_import->output.empty() &&
             pi_import->output[0].find("Pi session import is not supported yet") != std::string::npos &&
             pi_import->output[0].find("session entry data must be a JSON object") == std::string::npos && session_id() == pre_failed_import_session_id,
         "command dispatcher /import rejects Pi JSONL with a specific unsupported-format error");
  auto const pi_legacy_import_path = workspace / "pi-legacy-import.jsonl";
  write_app_test_file(pi_legacy_import_path,
                      "{\"type\":\"session\",\"id\":\"legacy-pi-session\",\"timestamp\":\"2025-11-20T23:33:50.805Z\","
                      "\"cwd\":\"/tmp/pi-project\",\"provider\":\"anthropic\",\"modelId\":\"claude-sonnet-4-5\"}\n");
  auto pi_legacy_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import pi-legacy-import.jsonl --confirm"});
  expect(pi_legacy_import && pi_legacy_import->handled && !pi_legacy_import->output.empty() &&
             pi_legacy_import->output[0].find("Pi session import is not supported yet") != std::string::npos &&
             pi_legacy_import->output[0].find("session entry data must be a JSON object") == std::string::npos && session_id() == pre_failed_import_session_id,
         "command dispatcher /import rejects legacy Pi JSONL headers with the same specific unsupported-format error");
  auto const future_import_path = workspace / "future-import.jsonl";
  write_app_test_file(future_import_path,
                      "{\"version\":99,\"id\":\"entry_future_start\",\"parent_id\":\"\",\"type\":\"session_start\","
                      "\"timestamp\":\"2026-05-02T00:00:00Z\",\"data\":{\"mode\":\"build\"}}\n");
  auto future_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import future-import.jsonl --confirm"});
  expect(future_import && future_import->handled && !future_import->output.empty() &&
             future_import->output[0].find("unsupported session entry version") != std::string::npos && session_id() == pre_failed_import_session_id,
         "command dispatcher /import rejects future session entry versions without switching sessions");
  auto const import_symlink_path = workspace / "symlink-import.jsonl";
  std::error_code symlink_error;
  std::filesystem::create_symlink(empty_import_path.filename(), import_symlink_path, symlink_error);
  if (!require(!symlink_error, "create import rejection symlink fixture: " + symlink_error.message()))
    return;
  if (!symlink_error)
  {
    auto symlink_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import symlink-import.jsonl --confirm"});
    expect(symlink_import && symlink_import->handled && !symlink_import->output.empty() &&
               symlink_import->output[0].find("session import path must be a regular file and not a symlink") != std::string::npos &&
               session_id() == pre_failed_import_session_id,
           "command dispatcher /import rejects symlink archives without switching sessions");
  }

  auto const fifo_import_path = workspace / "fifo-import.jsonl";
  int const fifo_created = ::mkfifo(fifo_import_path.c_str(), 0600);
  if (!require(fifo_created == 0, "create import rejection FIFO fixture: " + std::error_code(errno, std::generic_category()).message()))
    return;
  if (fifo_created == 0)
  {
    auto fifo_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import fifo-import.jsonl --confirm"});
    expect(fifo_import && fifo_import->handled && !fifo_import->output.empty() &&
               fifo_import->output[0].find("session import path must be a regular file") != std::string::npos && session_id() == pre_failed_import_session_id,
           "command dispatcher /import rejects a FIFO through its nonblocking opened descriptor");
  }

  auto const anchored_import_path = workspace / "anchored-import.jsonl";
  auto const anchored_displaced_path = workspace / "anchored-import.original.jsonl";
  auto anchored_line =
      ava::session::serialize_session_entry_line(ava::session::SessionEntry{.id = "entry_anchored_start",
                                                                            .parent_id = "",
                                                                            .type = ava::session::EntryType::SessionStart,
                                                                            .timestamp = "2026-05-02T00:00:00Z",
                                                                            .data_json = "{\"mode\":\"build\",\"provider\":\"openai\",\"model\":\"gpt-5.5\"}"});
  if (anchored_line)
    write_app_test_file(anchored_import_path, *anchored_line + "\n");
  bool import_name_replaced_after_open = false;
  ava::app::set_after_session_import_open_for_test([&] {
    std::filesystem::rename(anchored_import_path, anchored_displaced_path);
    write_app_test_file(anchored_import_path, "IMPORT_REPLACEMENT_CANARY\n");
    import_name_replaced_after_open = true;
  });
  auto anchored_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import anchored-import.jsonl"});
  ava::app::set_after_session_import_open_for_test({});
  expect(anchored_line && import_name_replaced_after_open && anchored_import && anchored_import->handled && !anchored_import->output.empty() &&
             anchored_import->output[0].find("session import is ready") != std::string::npos &&
             anchored_import->output[0].find("entries: 1") != std::string::npos &&
             app_read_binary_file(anchored_import_path).find("IMPORT_REPLACEMENT_CANARY") != std::string::npos &&
             app_read_binary_file(anchored_displaced_path) == *anchored_line + "\n",
         "command dispatcher /import reads only the descriptor opened before a final-component replacement");

  auto const oversized_file_import_path = workspace / "oversized-file-import.jsonl";
  {
    std::ofstream oversized_file(oversized_file_import_path, std::ios::binary | std::ios::trunc);
  }
  std::error_code resize_error;
  std::filesystem::resize_file(oversized_file_import_path, ava::app::kMaxSessionImportFileBytes + 1, resize_error);
  auto oversized_file_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import oversized-file-import.jsonl"});
  expect(!resize_error && oversized_file_import && oversized_file_import->handled && !oversized_file_import->output.empty() &&
             oversized_file_import->output[0].find("session import file exceeds byte limit") != std::string::npos &&
             oversized_file_import->output[0].find("reduce or split") != std::string::npos,
         "command dispatcher /import enforces its explicit file-byte cap with actionable remediation");

  auto const oversized_line_import_path = workspace / "oversized-line-import.jsonl";
  write_app_test_file(oversized_line_import_path, std::string(ava::app::kMaxSessionImportLineBytes, 'x'));
  auto oversized_line_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import oversized-line-import.jsonl"});
  expect(oversized_line_import && oversized_line_import->handled && !oversized_line_import->output.empty() &&
             oversized_line_import->output[0].find("session import line exceeds byte limit") != std::string::npos &&
             oversized_line_import->output[0].find("oversized JSONL record") != std::string::npos,
         "command dispatcher /import enforces its explicit per-line cap before JSON parsing");

  auto const excessive_entries_import_path = workspace / "excessive-entries-import.jsonl";
  {
    std::ofstream excessive_entries(excessive_entries_import_path, std::ios::binary | std::ios::trunc);
    for (std::size_t index = 0; index <= ava::app::kMaxSessionImportEntries; ++index)
    {
      excessive_entries << "{\"version\":3,\"id\":\"entry_import_cap_" << index << "\",\"parent_id\":\"\",\"type\":\""
                        << (index == 0 ? "session_start" : "user_message") << "\",\"timestamp\":\"2026-05-02T00:00:00Z\",\"data\":{} }\n";
    }
  }
  auto excessive_entries_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import excessive-entries-import.jsonl"});
  expect(excessive_entries_import && excessive_entries_import->handled && !excessive_entries_import->output.empty() &&
             excessive_entries_import->output[0].find("session import entry count exceeds limit") != std::string::npos &&
             excessive_entries_import->output[0].find("split the JSONL history") != std::string::npos,
         "command dispatcher /import enforces its explicit entry-count cap before replay validation");

  auto const incomplete_v4_import_path = workspace / "incomplete-v4-import.jsonl";
  auto incomplete_v4_line = ava::session::serialize_session_entry_line(
      ava::session::SessionEntry{.id = "entry_import_incomplete_v4",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantOutputItem,
                                 .timestamp = "2026-07-18T00:00:00Z",
                                 .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"turn_import_incomplete\",\"sequence\":0,\"kind\":\"text\","
                                              "\"text\":\"staged\",\"assistant_phase\":\"commentary\"}"});
  expect(incomplete_v4_line.has_value(), "test serializes an incomplete v4 import fixture");
  if (incomplete_v4_line)
    write_app_test_file(incomplete_v4_import_path, *incomplete_v4_line + "\n");
  auto incomplete_v4_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import incomplete-v4-import.jsonl --confirm"});
  expect(
      incomplete_v4_import && incomplete_v4_import->handled && !incomplete_v4_import->output.empty() &&
          incomplete_v4_import->output[0].find("session import has incomplete final assistant turn; recover the source under its lease") != std::string::npos &&
          session_id() == pre_failed_import_session_id,
      "command dispatcher /import rejects incomplete final v4 output before copying or switching sessions");

  auto const out_of_window_import_path = workspace / "out-of-window-v4-import.jsonl";
  auto import_function_data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
      .assistant_turn_id = "turn_import_window",
      .sequence = 0,
      .kind = ava::session::AssistantOutputItemKind::FunctionCall,
      .provider_item_id = "fc_import_window",
      .provider_output_index = 0,
      .payload = ava::session::AssistantOutputFunctionCall{.call_id = "call_import_window", .name = "read_file", .arguments_json = "{}"}});
  auto import_commit_data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{.assistant_turn_id = "turn_import_window",
                                                                                                                      .item_count = 1,
                                                                                                                      .provider = "openai",
                                                                                                                      .model = "gpt-5.5",
                                                                                                                      .finish_reason = "tool_calls",
                                                                                                                      .usage_json = std::nullopt});
  std::vector<ava::session::SessionEntry> const out_of_window_import_entries = {
      {.id = "entry_import_window_function",
       .parent_id = "",
       .type = ava::session::EntryType::AssistantOutputItem,
       .timestamp = "2026-07-18T00:00:00Z",
       .data_json = import_function_data.value_or("{}")},
      {.id = "entry_import_window_commit",
       .parent_id = "",
       .type = ava::session::EntryType::AssistantTurnCommit,
       .timestamp = "2026-07-18T00:00:01Z",
       .data_json = import_commit_data.value_or("{}")},
      {.id = "entry_import_window_user",
       .parent_id = "",
       .type = ava::session::EntryType::UserMessage,
       .timestamp = "2026-07-18T00:00:02Z",
       .data_json = "{\"text\":\"later\"}"},
      {.id = "entry_import_window_result",
       .parent_id = "",
       .type = ava::session::EntryType::ToolResult,
       .timestamp = "2026-07-18T00:00:03Z",
       .data_json = "{\"assistant_output_entry_id\":\"entry_import_window_function\",\"call_id\":\"call_import_window\",\"name\":\"read_file\","
                    "\"success\":true,\"result\":\"late\"}"}};
  {
    std::ofstream import_file(out_of_window_import_path, std::ios::binary | std::ios::trunc);
    for (auto const& entry : out_of_window_import_entries)
    {
      auto line = ava::session::serialize_session_entry_line(entry);
      expect(line.has_value(), "test serializes a v4 out-of-window import fixture entry");
      if (line)
        import_file << *line << '\n';
    }
  }
  auto out_of_window_import = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import out-of-window-v4-import.jsonl --confirm"});
  expect(out_of_window_import && out_of_window_import->handled && !out_of_window_import->output.empty() &&
             out_of_window_import->output[0].find("session import failed validation") != std::string::npos &&
             out_of_window_import->output[0].find("immediate post-commit result window") != std::string::npos && session_id() == pre_failed_import_session_id,
         "command dispatcher /import rejects a v4 result after its user-message window boundary before copying or switching sessions");

  auto const valid_import_path = workspace / "valid-import.jsonl";
  {
    std::ofstream valid_import(valid_import_path, std::ios::binary | std::ios::trunc);
    valid_import << "{\"id\":\"entry_import_start\",\"parent_id\":\"\",\"type\":\"session_start\","
                 << "\"timestamp\":\"2026-05-02T00:00:00Z\",\"data\":{\"mode\":\"build\",\"provider\":\"openai\",\"model\":\"gpt-5.5\"}}\n";
    auto line = ava::session::serialize_session_entry_line(ava::session::SessionEntry{.id = "entry_import_user",
                                                                                      .parent_id = "",
                                                                                      .type = ava::session::EntryType::UserMessage,
                                                                                      .timestamp = "2026-05-02T00:00:01Z",
                                                                                      .data_json = "{\"text\":\"import me\"}"});
    expect(line.has_value(), "test can serialize valid session import entries");
    if (line)
      valid_import << *line << '\n';
  }
  auto const pre_import_session_id = session_id();
  auto import_preview = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import valid-import.jsonl"});
  expect(import_preview && import_preview->handled && !import_preview->output.empty() &&
             import_preview->output[0].find("re-run with --confirm") != std::string::npos,
         "command dispatcher /import previews validated AVA JSONL archives before switching sessions");
  auto imported = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/import valid-import.jsonl --confirm"});
  auto imported_entries = load_entries();
  expect(imported && imported->handled && !imported->output.empty() && imported->output[0].find("imported session") != std::string::npos && imported_entries &&
             imported_entries->size() >= 1 && session_id() != pre_import_session_id && (*imported_entries)[0].version == 0 &&
             std::ranges::any_of(*imported_entries, [](auto const& entry) { return entry.version == ava::session::kCurrentSessionEntryVersion; }),
         "command dispatcher /import preserves canonical v0 records alongside current-version records");
  auto import_contender = ava::session::SessionLease::acquire(session_path());
  expect(!import_contender && import_contender.error().message().find("already owned") != std::string::npos,
         "confirmed /import retains its destination lease through append and runtime handoff");

  auto seeded_stats_usage = append_owned(ava::session::SessionEntry{.id = "entry_slash_stats_usage",
                                                                    .parent_id = "",
                                                                    .type = ava::session::EntryType::AssistantMessage,
                                                                    .timestamp = "2026-05-02T00:00:00Z",
                                                                    .data_json = "{\"text\":\"usage\",\"usage\":{\"input_tokens\":12,\"output_tokens\":7,"
                                                                                 "\"total_tokens\":19,\"cost_usd\":0.0015}}"});
  expect(seeded_stats_usage.has_value(), "command dispatcher /stats test seeds usage metadata");
  auto stats = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/stats"});
  expect(stats && stats->handled && !stats->output.empty() && stats->output[0].find("Session stats") != std::string::npos &&
             stats->output[0].find("tokens: input=12 output=7 total=19") != std::string::npos &&
             stats->output[0].find("cost: $0.001500") != std::string::npos && stats->output[0].find("compactions ") != std::string::npos &&
             stats->output[0].find("path:") == std::string::npos && stats->output[0].find("export: /export   resume: ava --session ") != std::string::npos,
         "command dispatcher /stats renders compact session counts, usage, cost, and hints");
  auto status = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/status"});
  expect(status && status->handled && !status->output.empty() && status->output[0] == stats->output[0],
         "command dispatcher /status aliases the backend-backed session stats surface");

  auto appended_v4_for_export = append_owned(
      ava::session::SessionEntry{.id = "entry_export_v4_private",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantOutputItem,
                                 .timestamp = "2026-07-18T00:00:00Z",
                                 .data_json = "{\"schema_version\":1,\"assistant_turn_id\":\"turn_export\",\"sequence\":0,\"kind\":\"reasoning\",\"text\":\"\","
                                              "\"format\":\"openai_responses\",\"redacted\":true,\"signature\":\"V4_EXPORT_PRIVATE_CANARY\"}"});
  auto projected_jsonl_export = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/export jsonl"});
  auto const projected_jsonl = projected_jsonl_export && !projected_jsonl_export->output.empty() ? projected_jsonl_export->output.front() : std::string{};
  auto markdown_after_v4 = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/export markdown"});
  auto html_after_v4 = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/export html"});
  expect(appended_v4_for_export && projected_jsonl_export && projected_jsonl_export->handled && !projected_jsonl_export->output.empty() &&
             !projected_jsonl.empty() && projected_jsonl.find("assistant_output_item") == std::string::npos &&
             projected_jsonl.find("V4_EXPORT_PRIVATE_CANARY") == std::string::npos && markdown_after_v4 && !markdown_after_v4->output.empty() &&
             markdown_after_v4->output.front().find("V4_EXPORT_PRIVATE_CANARY") == std::string::npos && html_after_v4 && !html_after_v4->output.empty() &&
             html_after_v4->output.front().find("V4_EXPORT_PRIVATE_CANARY") == std::string::npos,
         "portable JSONL and transcript exports omit only a valid final incomplete v4 staging suffix");

  auto quit = ava::app::run_command(unlocked_session, ava::app::CommandRequest{.command = "/quit"});
  expect(quit && quit->handled && quit->quit, "command dispatcher /quit requests shell exit");
}

}  // namespace ava::tests::app_runtime_tests
