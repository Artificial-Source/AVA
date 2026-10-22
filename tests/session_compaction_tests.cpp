#include "sys.h"
#include "tests/session_test_declarations.h"
#include "tests/support/test_harness.h"
#include "ava/agent/message_builder.h"
#include "ava/config/xdg_paths.h"
#include "ava/session/assistant_output.h"
#include "ava/session/compaction.h"
#include "ava/session/export.h"
#include "ava/session/logical_projection.h"
#include "ava/session/record.h"
#include "ava/session/session_metadata.h"
#include "ava/session/validation.h"
#include "ava/provider/anthropic_provider.h"
#include "ava/provider/provider.h"
#include "ava/core/error.h"
#include "ava/core/json.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace session_tests {
void test_compaction_snapshot_matcher_is_exact()
{
  std::vector<ava::session::SessionEntry> const snapshot = {{.id = "snapshot-first",
                                                             .parent_id = "",
                                                             .type = ava::session::EntryType::UserMessage,
                                                             .timestamp = "2026-08-17T00:00:00Z",
                                                             .data_json = "{\"text\":\"first\"}",
                                                             .version = 3},
                                                            {.id = "snapshot-second",
                                                             .parent_id = "snapshot-first",
                                                             .type = ava::session::EntryType::AssistantMessage,
                                                             .timestamp = "2026-08-17T00:00:01Z",
                                                             .data_json = "{\"text\":\"second\"}",
                                                             .version = 3},
                                                            {.id = "snapshot-third",
                                                             .parent_id = "snapshot-second",
                                                             .type = ava::session::EntryType::UserMessage,
                                                             .timestamp = "2026-08-17T00:00:02Z",
                                                             .data_json = "{\"text\":\"third\"}",
                                                             .version = 3}};
  expect(ava::session::compaction_snapshot_matches(snapshot, snapshot), "compaction snapshot matcher accepts an exact ordered history");

  auto expect_field_mismatch = [&](auto mutate, std::string_view field) {
    auto changed = snapshot;
    mutate(changed.front());
    expect(!ava::session::compaction_snapshot_matches(snapshot, changed), "compaction snapshot matcher compares " + std::string(field));
  };
  expect_field_mismatch([](auto& entry) { entry.id += "-changed"; }, "id");
  expect_field_mismatch([](auto& entry) { entry.parent_id += "-changed"; }, "parent_id");
  expect_field_mismatch([](auto& entry) { entry.type = ava::session::EntryType::AssistantMessage; }, "type");
  expect_field_mismatch([](auto& entry) { entry.timestamp = "2026-08-17T00:00:01Z"; }, "timestamp");
  expect_field_mismatch([](auto& entry) { entry.data_json = "{\"text\":\"changed\"}"; }, "data_json");
  expect_field_mismatch([](auto& entry) { entry.version = 4; }, "version");
  auto reordered = snapshot;
  std::swap(reordered[0], reordered[1]);
  expect(!ava::session::compaction_snapshot_matches(snapshot, reordered), "compaction snapshot matcher rejects reordered earlier entries");
  auto shortened = snapshot;
  shortened.pop_back();
  expect(!ava::session::compaction_snapshot_matches(snapshot, shortened), "compaction snapshot matcher rejects truncated history");
  auto ordinary_suffix = snapshot;
  ordinary_suffix.push_back({.id = "snapshot-extra",
                             .parent_id = snapshot.back().id,
                             .type = ava::session::EntryType::UserMessage,
                             .timestamp = "2026-08-17T00:00:03Z",
                             .data_json = "{\"text\":\"extra\"}",
                             .version = 3});
  expect(!ava::session::compaction_snapshot_matches(snapshot, ordinary_suffix), "compaction snapshot matcher rejects ordinary trailing history");

  auto generated = ava::session::make_session_metadata_entry(ava::session::SessionMetadataUpdate{.actor = "auto-title", .generated_title = "Generated title"},
                                                             snapshot.back().id);
  auto manual = ava::session::make_session_metadata_entry(ava::session::SessionMetadataUpdate{.name = "Manual title", .actor = "manual"}, snapshot.back().id);
  if (ava::tests::require(generated.has_value() && manual.has_value(), "compaction snapshot matcher metadata fixtures serialize"))
  {
    auto auto_title_suffix = snapshot;
    auto_title_suffix.push_back(*generated);
    auto manual_suffix = snapshot;
    manual_suffix.push_back(*manual);
    auto context_metadata_suffix = auto_title_suffix;
    context_metadata_suffix.back().data_json = "{\"actor\":\"auto-title\",\"generated_title\":\"Generated title\",\"source_session_id\":\"other\"}";
    expect(ava::session::compaction_snapshot_matches(snapshot, auto_title_suffix),
           "compaction snapshot matcher accepts only the validated trailing auto-title exception");
    ava::tests::expect_all("compaction snapshot matcher rejects context-affecting metadata suffixes",
                           {{"manual", !ava::session::compaction_snapshot_matches(snapshot, manual_suffix)},
                            {"context metadata", !ava::session::compaction_snapshot_matches(snapshot, context_metadata_suffix)}});
    auto second_title = *generated;
    second_title.id = "second-auto-title";
    second_title.parent_id = generated->id;
    auto_title_suffix.push_back(std::move(second_title));
    expect(ava::session::compaction_snapshot_matches(snapshot, auto_title_suffix),
           "compaction snapshot matcher accepts multiple context-neutral trailing automatic titles");
    auto_title_suffix.front().data_json = "{\"text\":\"changed earlier entry\"}";
    expect(!ava::session::compaction_snapshot_matches(snapshot, auto_title_suffix), "automatic-title suffixes do not excuse a changed earlier snapshot entry");
  }
}

void test_session_compaction_entry_round_trip()
{
  auto const root = create_empty_root("compaction-round-trip");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "compact"});
  auto config = ava::session::default_compaction_config();
  config.auto_threshold_tokens = 1234;
  config.keep_recent_tokens = 321;
  config.keep_recent_messages = 6;
  config.model_id = "gpt-5.5-mini";
  config.max_summary_bytes = 1024;

  auto appended = append_manual_compaction_for_test(store, ava::session::ManualCompactionRequest{.summary = "Prior work summary",
                                                                                                 .instructions = "Keep the recent plan.",
                                                                                                 .config = config,
                                                                                                 .estimated_tokens = 1300,
                                                                                                 .threshold_tokens = 0,
                                                                                                 .trigger = "manual",
                                                                                                 .recent_context = ""});
  expect(appended.has_value(), "manual compaction entry appends");

  auto loaded = store.load();
  expect(loaded && loaded->size() == 1 && (*loaded)[0].type == ava::session::EntryType::Compaction, "compaction entry type round trips through session store");
  if (loaded && !loaded->empty())
  {
    expect(ava::core::json::string_field((*loaded)[0].data_json, "summary") == "Prior work summary", "compaction summary round trips");
    expect(ava::core::json::string_field((*loaded)[0].data_json, "instructions") == "Keep the recent plan.", "compaction instructions round trips");
    expect(ava::core::json::string_field((*loaded)[0].data_json, "model") == "gpt-5.5-mini", "compaction model metadata round trips");
    expect(ava::core::json::integer_field((*loaded)[0].data_json, "threshold_tokens") == 1234, "compaction threshold metadata round trips");
  }

  ava::session::SessionStore unavailable_store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "compact-unavailable"});
  auto unavailable = append_manual_compaction_for_test(
      unavailable_store,
      ava::session::ManualCompactionRequest{
          .summary = "", .instructions = "", .config = config, .estimated_tokens = 0, .threshold_tokens = 0, .trigger = "manual", .recent_context = ""});
  auto unavailable_loaded = unavailable_store.load();
  std::optional<std::string> unavailable_summary;
  if (unavailable_loaded && !unavailable_loaded->empty())
  {
    unavailable_summary = ava::core::json::string_field((*unavailable_loaded)[0].data_json, "summary");
  }
  expect(unavailable && unavailable_summary && unavailable_summary->find("unavailable") != std::string::npos,
         "manual compaction records deterministic unavailable summary when no provider summary exists");

  auto tiny_config = config;
  tiny_config.max_summary_bytes = 1;
  ava::session::SessionStore tiny_unavailable_store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "compact-tiny-unavailable"});
  auto tiny_unavailable = append_manual_compaction_for_test(
      tiny_unavailable_store,
      ava::session::ManualCompactionRequest{
          .summary = "", .instructions = "", .config = tiny_config, .estimated_tokens = 0, .threshold_tokens = 0, .trigger = "manual", .recent_context = ""});
  auto tiny_loaded = tiny_unavailable_store.load();
  std::optional<std::string> tiny_summary;
  if (tiny_loaded && !tiny_loaded->empty())
  {
    tiny_summary = ava::core::json::string_field((*tiny_loaded)[0].data_json, "summary");
  }
  expect(tiny_unavailable && tiny_summary && tiny_summary->find("unavailable") != std::string::npos,
         "empty manual compaction succeeds with tiny summary limit");

  ava::session::SessionStore oversized_summary_store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "compact-oversized-summary"});
  auto oversized_summary = append_manual_compaction_for_test(
      oversized_summary_store,
      ava::session::ManualCompactionRequest{
          .summary = "xx", .instructions = "", .config = tiny_config, .estimated_tokens = 0, .threshold_tokens = 0, .trigger = "manual", .recent_context = ""});
  expect(!oversized_summary && oversized_summary.error().category() == ava::core::ErrorCategory::InvalidArgument,
         "manual compaction rejects oversized user summary with tiny summary limit");

  auto direct_entry = ava::session::make_manual_compaction_entry(ava::session::ManualCompactionRequest{.summary = "direct compatibility summary",
                                                                                                       .instructions = "",
                                                                                                       .config = ava::session::default_compaction_config(),
                                                                                                       .estimated_tokens = 0,
                                                                                                       .threshold_tokens = 0,
                                                                                                       .retained_tokens = 0,
                                                                                                       .trigger = "manual",
                                                                                                       .recent_context = "",
                                                                                                       .recent_context_omitted = false});
  bool direct_round_trip = false;
  if (direct_entry)
  {
    auto direct_line = ava::session::serialize_session_entry_line(*direct_entry);
    if (direct_line)
    {
      auto direct_parsed = ava::session::parse_session_entry_line(*direct_line, "direct-compaction.jsonl");
      if (direct_parsed)
      {
        auto const direct_validation = ava::session::validate_session_replay({*direct_parsed});
        direct_round_trip = direct_validation.ok() && ava::core::json::string_field(direct_parsed->data_json, "model").value_or("") == "gpt-5.5" &&
                            !ava::core::json::field_value_start(direct_parsed->data_json, "provider");
      }
    }
  }
  expect(direct_round_trip, "direct default compaction entry retains a non-empty compatibility model and round-trips through replay validation");

  std::vector<ava::session::SessionEntry> legacy_entries;
  for (long long version = 0; version <= ava::session::kCurrentSessionEntryVersion; ++version)
  {
    auto const version_field = version == 0 ? std::string{} : "\"version\":" + std::to_string(version) + ",";
    auto const line = "{" + version_field + "\"id\":\"legacy_compaction_" + std::to_string(version) +
                      "\",\"parent_id\":\"\",\"type\":\"compaction\",\"timestamp\":\"2026-04-29T00:00:00Z\",\"data\":{"
                      "\"trigger\":\"manual\",\"status\":\"recorded\",\"summary_unavailable\":false,"
                      "\"summary\":\"legacy summary\",\"instructions\":\"\",\"model\":\"gpt-5.5\","
                      "\"threshold_tokens\":100,\"estimated_tokens\":125,\"keep_recent_tokens\":64,"
                      "\"keep_recent_messages\":4,\"max_summary_bytes\":65536,\"recent_context\":\"\"}}";
    auto parsed = ava::session::parse_session_entry_line(line, "legacy-import.jsonl");
    if (parsed)
      legacy_entries.push_back(std::move(*parsed));
  }
  auto const legacy_validation = ava::session::validate_session_replay(legacy_entries);
  expect(legacy_entries.size() == static_cast<std::size_t>(ava::session::kCurrentSessionEntryVersion + 1) && legacy_validation.ok(),
         "literal legacy v0-v4 compaction records remain import/replay compatible without additive metadata");
}

void test_session_markdown_export()
{
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "start_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::SessionStart,
                                 .timestamp = "2026-04-29T00:00:00Z",
                                 .data_json = "{\"mode\":\"build\",\"provider\":\"openai\",\"model\":\"gpt-5.5\"}"},
      ava::session::SessionEntry{.id = "user_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-29T00:00:01Z",
                                 .data_json = "{\"text\":\"Hello AVA\"}"},
      ava::session::SessionEntry{.id = "user_1_replay",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-29T00:00:01Z",
                                 .data_json = "{\"text\":\"Hello AVA\",\"internal_replay\":true,"
                                              "\"replay_of\":\"user_1\",\"reason\":\"test\"}"},
      ava::session::SessionEntry{.id = "assistant_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-04-29T00:00:02Z",
                                 .data_json = "{\"text\":\"Hello human\",\"tool_calls\":1,"
                                              "\"usage\":{\"input_tokens\":10,\"output_tokens\":5,"
                                              "\"total_tokens\":15,\"source\":\"provider\"}}"},
      ava::session::SessionEntry{.id = "unsafe_html_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-29T00:00:02Z",
                                 .data_json = R"json({"text":"<script>alert('x')</script> & raw"})json"},
      ava::session::SessionEntry{.id = "reasoning_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ReasoningBlock,
                                 .timestamp = "2026-04-29T00:00:02Z",
                                 .data_json = "{\"provider\":\"anthropic\",\"model\":\"claude-sonnet-4-5\","
                                              "\"format\":\"anthropic_thinking\","
                                              "\"text\":\"visible reasoning summary\","
                                              "\"signature\":\"super-secret-signature\","
                                              "\"native_item_json\":\"{\\\"id\\\":\\\"rs_export\\\",\\\"type\\\":\\\"reasoning\\\",\\\"summary\\\":[],"
                                              "\\\"encrypted_content\\\":\\\"export-private-cipher\\\"}\"}"},
      ava::session::SessionEntry{.id = "reasoning_change_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ReasoningChange,
                                 .timestamp = "2026-04-29T00:00:02Z",
                                 .data_json = "{\"provider\":\"anthropic\",\"model\":\"claude-sonnet-4-5\","
                                              "\"format\":\"anthropic_thinking\",\"enabled\":true,"
                                              "\"level\":\"enabled\",\"budget_tokens\":4096,"
                                              "\"display\":\"summarized\"}"},
      ava::session::SessionEntry{.id = "tool_call_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-04-29T00:00:03Z",
                                 .data_json = "{\"call_id\":\"call_1\",\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"README.md\\\"}\"}"},
      ava::session::SessionEntry{.id = "tool_result_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-29T00:00:04Z",
                                 .data_json = "{\"call_id\":\"call_1\",\"name\":\"read_file\",\"success\":true,"
                                              "\"result\":\"tool output\"}"},
      ava::session::SessionEntry{.id = "compact_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::Compaction,
                                 .timestamp = "2026-04-29T00:00:05Z",
                                 .data_json = "{\"trigger\":\"manual\",\"status\":\"recorded\","
                                              "\"summary_unavailable\":false,\"summary\":\"Prior summary\","
                                              "\"instructions\":\"Keep this constraint\",\"model\":\"gpt-5.5-mini\","
                                              "\"threshold_tokens\":100,\"estimated_tokens\":125,"
                                              "\"keep_recent_tokens\":64,\"keep_recent_messages\":4}"},
      ava::session::SessionEntry{.id = "mode_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ModeChange,
                                 .timestamp = "2026-04-29T00:00:06Z",
                                 .data_json = "{\"mode\":\"plan\"}"},
      ava::session::SessionEntry{
          .id = "error_1",
          .parent_id = "",
          .type = ava::session::EntryType::Error,
          .timestamp = "2026-04-29T00:00:07Z",
          .data_json = "{\"category\":\"provider\",\"message\":\"SESSION_PROVIDER_MESSAGE_CANARY\",\"details\":\"SESSION_PROVIDER_DETAILS_CANARY\"}"},
      ava::session::SessionEntry{.id = "backticks_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-29T00:00:08Z",
                                 .data_json = R"json({"text":"before ``` after ```` done"})json"},
      ava::session::SessionEntry{.id = "control_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-29T00:00:09Z",
                                 .data_json = R"json({"text":"first\nsecond\tindent\u0000\u001B\u007F\r"})json"},
  };

  auto const basic = ava::session::format_session_markdown(entries);
  auto const physical_error = std::ranges::find_if(entries, [](ava::session::SessionEntry const& entry) { return entry.id == "error_1"; });
  expect(physical_error != entries.end() && physical_error->data_json.find("SESSION_PROVIDER_MESSAGE_CANARY") != std::string::npos &&
             physical_error->data_json.find("SESSION_PROVIDER_DETAILS_CANARY") != std::string::npos,
         "public session projections leave the local physical error record unchanged");
  expect(basic.find("# AVA Session Export") != std::string::npos, "markdown export has deterministic title");
  expect(basic.find("## User") != std::string::npos && basic.find("Hello AVA") != std::string::npos, "markdown export renders user messages");
  expect(basic.find("internal_replay") == std::string::npos && basic.find("replay_of") == std::string::npos,
         "markdown export hides internal replay user messages");
  expect(basic.find("## Assistant") != std::string::npos && basic.find("Hello human") != std::string::npos, "markdown export renders assistant messages");
  expect(basic.find("## Reasoning") != std::string::npos && basic.find("visible reasoning summary") != std::string::npos &&
             basic.find("Signature present") != std::string::npos && basic.find("super-secret-signature") == std::string::npos &&
             basic.find("export-private-cipher") == std::string::npos,
         "markdown export renders reasoning blocks without leaking provider-private replay metadata");
  expect(basic.find("## Reasoning Change") != std::string::npos && basic.find("Budget tokens") != std::string::npos && basic.find("4096") != std::string::npos,
         "markdown export renders reasoning-change budget tokens when present");
  expect(basic.find("Usage:") != std::string::npos && basic.find("input_tokens") != std::string::npos, "markdown export renders assistant usage when present");
  expect(basic.find("## Tool Call") == std::string::npos && basic.find("README.md") == std::string::npos, "markdown export omits tool details by default");
  expect(basic.find("## Compaction") != std::string::npos && basic.find("Prior summary") != std::string::npos &&
             basic.find("Keep this constraint") != std::string::npos,
         "markdown export renders compactions by default");
  expect(basic.find("## Mode Change") != std::string::npos && basic.find("## Error") != std::string::npos &&
             basic.find(ava::session::kPublicSessionErrorOmission) != std::string::npos && basic.find("SESSION_PROVIDER_MESSAGE_CANARY") == std::string::npos &&
             basic.find("SESSION_PROVIDER_DETAILS_CANARY") == std::string::npos,
         "markdown export renders only a fixed omission for historical provider error diagnostics");
  expect(basic.find("Metadata:") == std::string::npos && basic.find("\"id\":\"user_1\"") == std::string::npos, "markdown export omits metadata by default");
  expect(basic.find("`````text\nbefore ``` after ```` done\n`````") != std::string::npos, "markdown export expands fences around backtick content");
  std::string const escaped_control_markdown = std::string("first\nsecond\tindent") + "\\u0000\\u001B\\u007F\\u000D";
  expect(basic.find(escaped_control_markdown) != std::string::npos, "markdown export escapes decoded fenced control bytes while preserving newlines and tabs");
  expect(basic.find('\0') == std::string::npos && basic.find('\x1B') == std::string::npos && basic.find('\x7F') == std::string::npos &&
             basic.find('\r') == std::string::npos,
         "markdown export does not emit raw NUL, escape, DEL, or carriage return bytes");

  auto const with_tools = ava::session::format_session_markdown(entries, ava::session::ExportOptions{.include_tool_details = true, .include_metadata = false});
  expect(with_tools.find("## Tool Call") != std::string::npos && with_tools.find("README.md") != std::string::npos &&
             with_tools.find("## Tool Result") != std::string::npos && with_tools.find("tool output") != std::string::npos,
         "markdown export includes tool calls and results when requested");

  auto const without_compactions = ava::session::format_session_markdown(
      entries, ava::session::ExportOptions{.include_tool_details = false, .include_metadata = false, .include_compactions = false});
  expect(without_compactions.find("## Compaction") == std::string::npos && without_compactions.find("Prior summary") == std::string::npos,
         "markdown export can omit compaction entries");

  auto const with_metadata = ava::session::format_session_markdown(
      entries, ava::session::ExportOptions{.include_tool_details = false, .include_metadata = true, .include_compactions = true});
  expect(with_metadata.find("Metadata:") != std::string::npos && with_metadata.find("\"id\":\"user_1\"") != std::string::npos &&
             with_metadata.find("Estimated tokens") != std::string::npos && with_metadata.find("gpt-5.5-mini") != std::string::npos,
         "markdown export includes entry and compaction metadata when requested");

  auto const html = ava::session::format_session_html(entries);
  expect(html.find("<!doctype html>") != std::string::npos && html.find("<title>AVA Session Export</title>") != std::string::npos &&
             html.find("<pre>") != std::string::npos && html.find("# AVA Session Export") != std::string::npos,
         "html export wraps the session markdown in a self-contained document");
  expect(html.find("&lt;script&gt;alert(&#39;x&#39;)&lt;/script&gt; &amp; raw") != std::string::npos && html.find("<script>alert") == std::string::npos,
         "html export escapes user and model text instead of emitting executable markup");
}

void test_session_portable_jsonl_sanitizer()
{
  auto const redacted_with_private = ava::session::sanitize_session_entry_for_portable_jsonl_export(
      ava::session::SessionEntry{.id = "portable_redacted_private",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ReasoningBlock,
                                 .timestamp = "2026-05-11T00:00:00Z",
                                 .data_json = "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\","
                                              "\"text\":\"redacted secret text\",\"redacted\":true,\"signature\":\"secret-signature\","
                                              "\"native_item_json\":\"{\\\"id\\\":\\\"rs_portable\\\",\\\"type\\\":\\\"reasoning\\\",\\\"summary\\\":[]}\","
                                              "\"redacted_data\":\"secret-redacted-data\",\"unknown_provider_canary\":\"must-not-export\"}"});
  expect(redacted_with_private.data_json.find("redacted secret text") == std::string::npos &&
             redacted_with_private.data_json.find("secret-signature") == std::string::npos &&
             redacted_with_private.data_json.find("secret-redacted-data") == std::string::npos &&
             redacted_with_private.data_json.find("must-not-export") == std::string::npos &&
             redacted_with_private.data_json.find("[Provider-private reasoning metadata omitted from portable export.]") != std::string::npos &&
             redacted_with_private.data_json.find("\"redacted\":true") != std::string::npos &&
             redacted_with_private.data_json.find("\"native_item_json\":true") != std::string::npos &&
             redacted_with_private.data_json.find("\"signature\":true") != std::string::npos &&
             redacted_with_private.data_json.find("\"redacted_data\":true") != std::string::npos,
         "portable JSONL rebuilds redacted reasoning with a neutral placeholder and private-field presence metadata");

  auto const redacted_without_private = ava::session::sanitize_session_entry_for_portable_jsonl_export(
      ava::session::SessionEntry{.id = "portable_redacted_plain",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ReasoningBlock,
                                 .timestamp = "2026-05-11T00:00:01Z",
                                 .data_json = "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\","
                                              "\"text\":\"redacted text without known private fields\",\"redacted\":true,"
                                              "\"unknown_provider_canary\":\"must-not-export\"}"});
  expect(redacted_without_private.data_json.find("redacted text without known private fields") == std::string::npos &&
             redacted_without_private.data_json.find("must-not-export") == std::string::npos &&
             redacted_without_private.data_json.find("[Provider-private reasoning metadata omitted from portable export.]") != std::string::npos &&
             redacted_without_private.data_json.find("\"native_item_json\":false") != std::string::npos &&
             redacted_without_private.data_json.find("\"signature\":false") != std::string::npos &&
             redacted_without_private.data_json.find("\"redacted_data\":false") != std::string::npos,
         "portable JSONL redacts reasoning text and additive canaries even without known private fields");

  auto const portable_error = ava::session::sanitize_session_entry_for_portable_jsonl_export(ava::session::SessionEntry{
      .id = "portable_provider_error",
      .parent_id = "",
      .type = ava::session::EntryType::Error,
      .timestamp = "2026-05-11T00:00:02Z",
      .data_json = "{\"category\":\"provider\",\"message\":\"PORTABLE_PROVIDER_MESSAGE_CANARY\",\"details\":\"PORTABLE_PROVIDER_DETAILS_CANARY\"}"});
  expect(portable_error.data_json.find("PORTABLE_PROVIDER_MESSAGE_CANARY") == std::string::npos &&
             portable_error.data_json.find("PORTABLE_PROVIDER_DETAILS_CANARY") == std::string::npos &&
             portable_error.data_json.find(ava::session::kPublicSessionErrorOmission) != std::string::npos,
         "portable JSONL exports replace historical Error diagnostics with a fixed omission");

  auto const visible_without_private = ava::session::sanitize_session_entry_for_portable_jsonl_export(
      ava::session::SessionEntry{.id = "portable_visible_plain",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ReasoningBlock,
                                 .timestamp = "2026-05-11T00:00:02Z",
                                 .data_json = "{\"provider\":\"openai\",\"model\":\"gpt-5.5\",\"format\":\"openai_responses\","
                                              "\"text\":\"visible safe summary\",\"redacted\":false,\"unknown_provider_canary\":\"must-not-export\"}"});
  expect(visible_without_private.data_json.find("visible safe summary") != std::string::npos &&
             visible_without_private.data_json.find("must-not-export") == std::string::npos,
         "portable JSONL retains only visible non-redacted reasoning text from its explicit allowlist");
}

void test_compaction_config_and_thresholds()
{
  auto const root = create_empty_root("compaction-config");

  ScopedEnvVar home("HOME", (root / "home").string());
  ScopedEnvVar config_home("XDG_CONFIG_HOME", (root / "config").string());
  ScopedEnvVar state_home("XDG_STATE_HOME", (root / "state").string());
  ScopedEnvVar data_home("XDG_DATA_HOME", (root / "data").string());
  auto const paths = ava::config::xdg_paths();

  expect(paths.compaction_file == root / "config" / "ava" / "compaction.json", "compaction config path follows XDG config home");
  auto missing = ava::session::load_compaction_config(paths);
  expect(missing && missing->model_id == "gpt-5.5" && !missing->model_explicit && missing->provider_id.empty() && !missing->provider_explicit &&
             missing->auto_threshold_tokens == 0 && missing->auto_threshold_percent == 80 && missing->keep_recent_turns == 2 &&
             missing->keep_recent_tokens == 20'000,
         "missing compaction config keeps a direct-API compatibility model while runtime resolution remains active-model based");
  if (!ava::tests::require(missing.has_value(), "default compaction configuration loads before threshold calculation"))
    return;
  auto const fallback_threshold = ava::session::effective_auto_threshold_tokens(*missing, std::nullopt);
  expect(fallback_threshold > 0, "missing compaction config uses a nonzero effective auto-compaction threshold without model metadata");

  std::filesystem::create_directories(paths.ava_config_dir);
  {
    std::ofstream file(paths.compaction_file, std::ios::binary | std::ios::trunc);
    file << "{\"auto_threshold_tokens\":4096,\"keep_recent_tokens\":512,"
            "\"keep_recent_messages\":7,\"compaction_model\":\"gpt-5.5-compact\","
            "\"max_summary_bytes\":2048}";
  }
  auto loaded = ava::session::load_compaction_config(paths);
  expect(loaded && loaded->auto_threshold_tokens == 4096 && loaded->keep_recent_tokens == 512 && loaded->keep_recent_messages == 7 &&
             loaded->model_id == "gpt-5.5-compact" && loaded->max_summary_bytes == 2048,
         "compaction config parses token budgets and model id from XDG file");

  expect(ava::session::estimate_tokens("") == 0 && ava::session::estimate_tokens("abcd") == 1 && ava::session::estimate_tokens("abcde") == 2,
         "compaction token estimate uses deterministic chars over four heuristic");
  std::vector<ava::session::SessionEntry> const entries = {ava::session::SessionEntry{.id = "u",
                                                                                      .parent_id = "",
                                                                                      .type = ava::session::EntryType::UserMessage,
                                                                                      .timestamp = "2026-04-27T00:00:00Z",
                                                                                      .data_json = "{\"text\":\"abcdefgh\"}"},
                                                           ava::session::SessionEntry{.id = "ignored",
                                                                                      .parent_id = "",
                                                                                      .type = ava::session::EntryType::ModeChange,
                                                                                      .timestamp = "2026-04-27T00:00:00Z",
                                                                                      .data_json = "{\"mode\":\"build\"}"}};
  auto config = ava::session::default_compaction_config();
  auto estimated = ava::session::estimate_session_tokens(entries);
  config.auto_threshold_tokens = estimated.value_or(0);
  auto decision = ava::session::should_auto_compact(entries, config);
  expect(estimated && decision && decision->should_compact && decision->estimated_tokens == config.auto_threshold_tokens,
         "auto compaction triggers when estimated tokens reach threshold");
  config.auto_threshold_tokens = decision ? decision->estimated_tokens + 1 : 1;
  auto below_threshold = ava::session::should_auto_compact(entries, config);
  expect(below_threshold && !below_threshold->should_compact, "auto compaction does not trigger below threshold");
  config.auto_threshold_tokens = 0;
  auto disabled = ava::session::should_auto_compact(entries, config);
  expect(disabled && !disabled->should_compact, "auto compaction threshold zero disables automatic compaction");
  config.auto_threshold_tokens_explicit = false;
  std::vector<ava::session::SessionEntry> const fallback_entries = {
      ava::session::SessionEntry{.id = "fallback_big",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-27T00:00:00Z",
                                 .data_json = "{\"text\":\"" + std::string(fallback_threshold * 4, 'x') + "\"}"}};
  auto fallback_decision = ava::session::should_auto_compact(fallback_entries, config, std::nullopt);
  expect(fallback_decision && fallback_decision->should_compact, "default auto compaction can trigger when model context-window metadata is absent");
  config.auto_threshold_tokens_explicit = true;
  auto explicitly_disabled = ava::session::should_auto_compact(fallback_entries, config, std::nullopt);
  expect(explicitly_disabled && !explicitly_disabled->should_compact, "explicit auto_threshold_tokens zero remains disabled without model metadata");

  auto percent = ava::session::parse_compaction_config(R"({"auto_threshold_percent":75,"keep_recent_turns":3,"model":"summary-model"})");
  expect(percent && percent->auto_threshold_percent_explicit && percent->auto_threshold_percent == 75 && percent->keep_recent_turns_explicit &&
             percent->keep_recent_turns == 3 && percent->model_explicit && percent->model_id == "summary-model" &&
             ava::session::effective_auto_threshold_tokens(*percent, 200'000) == 150'000,
         "compaction config parses percentage, turn, and same-provider model selection");
  auto cross_provider = ava::session::parse_compaction_config(R"({"provider":"anthropic","model":"claude-sonnet-4-5"})");
  expect(cross_provider && cross_provider->provider_explicit && cross_provider->model_explicit && cross_provider->provider_id == "anthropic",
         "compaction config parses explicit cross-provider selection");

  std::vector<std::string> const invalid_configs = {R"({"model":7})",
                                                    R"({"provider":false})",
                                                    R"({"provider":"openai"})",
                                                    R"({"auto_threshold_tokens":"80000"})",
                                                    R"({"auto_threshold_percent":0})",
                                                    R"({"auto_threshold_percent":96})",
                                                    R"({"auto_threshold_tokens":1,"auto_threshold_percent":80})",
                                                    R"({"keep_recent_tokens":true})",
                                                    R"({"keep_recent_turns":"2"})",
                                                    R"({"keep_recent_messages":[]})",
                                                    R"({"keep_recent_turns":2,"keep_recent_messages":6})",
                                                    R"({"max_summary_bytes":"16384"})",
                                                    R"({"max_summary_bytes":1048577})"};
  expect(std::ranges::all_of(invalid_configs, [](auto const& content) { return !ava::session::parse_compaction_config(content); }),
         "compaction config rejects wrong known-field types, invalid ranges, and ambiguous selectors");

  std::vector<std::string_view> const numeric_fields = {"auto_threshold_tokens", "auto_threshold_percent", "keep_recent_tokens",
                                                        "keep_recent_turns",     "keep_recent_messages",   "max_summary_bytes"};
  bool strict_numeric_tokens = true;
  for (auto const field : numeric_fields)
  {
    strict_numeric_tokens = strict_numeric_tokens && !ava::session::parse_compaction_config("{\"" + std::string(field) + "\":80.9}") &&
                            !ava::session::parse_compaction_config("{\"" + std::string(field) + "\":1e5}");
  }
  for (std::string_view value : {"\"80\"", "true", "false", "null", "-1", "184467440737095516160"})
    strict_numeric_tokens = strict_numeric_tokens && !ava::session::parse_compaction_config("{\"auto_threshold_tokens\":" + std::string(value) + "}");
  expect(strict_numeric_tokens,
         "all compaction numeric fields require complete non-negative integer tokens and reject fractions, exponents, other JSON types, and overflow");

  std::vector<ava::session::SessionEntry> const compacted_entries = {ava::session::SessionEntry{.id = "old",
                                                                                                .parent_id = "",
                                                                                                .type = ava::session::EntryType::UserMessage,
                                                                                                .timestamp = "2026-04-27T00:00:00Z",
                                                                                                .data_json = "{\"text\":\"OLD_PHYSICAL_HISTORY\"}"},
                                                                     ava::session::SessionEntry{.id = "boundary",
                                                                                                .parent_id = "",
                                                                                                .type = ava::session::EntryType::Compaction,
                                                                                                .timestamp = "2026-04-27T00:00:01Z",
                                                                                                .data_json = "{\"summary\":\"ACTIVE_SUMMARY\"}"},
                                                                     ava::session::SessionEntry{.id = "new",
                                                                                                .parent_id = "",
                                                                                                .type = ava::session::EntryType::UserMessage,
                                                                                                .timestamp = "2026-04-27T00:00:02Z",
                                                                                                .data_json = "{\"text\":\"NEW_ACTIVE_HISTORY\"}"}};
  auto active = ava::session::project_active_compaction_context(compacted_entries);
  auto active_tokens = ava::session::estimate_active_context_tokens(compacted_entries);
  expect(active && active->size() == 2 && active->front().id == "boundary" && active->back().id == "new" && active_tokens &&
             *active_tokens < ava::session::estimate_session_tokens(compacted_entries).value_or(0),
         "active compaction projection excludes physical history replaced by the latest valid boundary");
}

void test_compaction_context_reconstruction()
{
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "old_user",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-27T00:00:00Z",
                                 .data_json = "{\"text\":\"old raw user\"}"},
      ava::session::SessionEntry{.id = "old_assistant",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-04-27T00:00:01Z",
                                 .data_json = "{\"text\":\"old raw assistant\"}"},
      ava::session::SessionEntry{.id = "old_tool",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-27T00:00:02Z",
                                 .data_json = "{\"call_id\":\"call_old\",\"name\":\"read_file\",\"result\":\"old raw tool\"}"},
      ava::session::SessionEntry{.id = "compact_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::Compaction,
                                 .timestamp = "2026-04-27T00:00:03Z",
                                 .data_json = "{\"summary\":\"first summary\",\"instructions\":\"ignored later\",\"history_projection\":\"portable-v1\"}"},
      ava::session::SessionEntry{.id = "middle_user",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-27T00:00:04Z",
                                 .data_json = "{\"text\":\"middle raw user\"}"},
      ava::session::SessionEntry{.id = "compact_2",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::Compaction,
                                 .timestamp = "2026-04-27T00:00:05Z",
                                 .data_json = "{\"summary\":\"latest summary\",\"instructions\":\"carry this\",\"history_projection\":\"portable-v1\"}"},
      ava::session::SessionEntry{.id = "new_user",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-27T00:00:06Z",
                                 .data_json = "{\"text\":\"new user\"}"},
      ava::session::SessionEntry{.id = "new_user_replay",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::UserMessage,
                                 .timestamp = "2026-04-27T00:00:06Z",
                                 .data_json = "{\"text\":\"new user\",\"internal_replay\":true,"
                                              "\"replay_of\":\"new_user\",\"reason\":\"test\"}"},
      ava::session::SessionEntry{.id = "new_assistant",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-04-27T00:00:07Z",
                                 .data_json = "{\"text\":\"new assistant\"}"},
      ava::session::SessionEntry{.id = "new_tool_call",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-04-27T00:00:08Z",
                                 .data_json = "{\"call_id\":\"call_read\",\"name\":\"read_file\","
                                              "\"arguments\":\"{\\\"path\\\":\\\"note.txt\\\"}\"}"},
      ava::session::SessionEntry{.id = "new_tool_result",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-27T00:00:09Z",
                                 .data_json = "{\"call_id\":\"call_read\",\"name\":\"read_file\","
                                              "\"result\":\"note contents\"}"}};

  auto messages = ava::agent::build_provider_messages_from_entries(
      entries, ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = "test",
                                                                                         .model_id = "test-tools",
                                                                                         .api_family = "test_api",
                                                                                         .reasoning_format = "",
                                                                                         .supports_tools = true,
                                                                                         .supports_images = false}});
  expect(messages && messages->size() == 6, "compacted context reconstructs summary plus post-compaction turns");
  if (!messages)
    return;
  expect((*messages)[0].role == "user" && (*messages)[0].content.find("latest summary") != std::string::npos &&
             (*messages)[0].content.find("carry this") != std::string::npos,
         "latest compaction summary becomes provider-visible context");
  std::string const joined =
      (*messages)[0].content + (*messages)[1].content + (*messages)[2].content + (*messages)[3].content + (*messages)[4].content + (*messages)[5].content;
  expect(joined.find("old raw user") == std::string::npos && joined.find("old raw assistant") == std::string::npos &&
             joined.find("old raw tool") == std::string::npos && joined.find("middle raw user") == std::string::npos &&
             joined.find("first summary") == std::string::npos,
         "context reconstruction omits raw messages and tool results before latest compaction");
  expect((*messages)[1].role == "user" && (*messages)[1].content == "new user" && (*messages)[2].role == "user" && (*messages)[2].content == "new user" &&
             (*messages)[3].role == "assistant" && (*messages)[3].content == "new assistant",
         "post-compaction entries include internal replays as normal provider messages");
  expect((*messages)[4].role == "assistant" && (*messages)[4].content.find("Tool call (read_file)") != std::string::npos &&
             (*messages)[4].content.find("note.txt") != std::string::npos && (*messages)[5].role == "user" &&
             (*messages)[5].content.find("note contents") != std::string::npos,
         "post-compaction portable context includes labelled tool-call metadata before tool result data");
  expect((*messages)[4].content_parts.size() == 1 && (*messages)[4].content_parts[0].type == ava::provider::ContentPartType::ToolUse &&
             (*messages)[4].content_parts[0].tool_call_id == "ava_history_tool_1" && (*messages)[4].content_parts[0].tool_name == "read_file" &&
             (*messages)[4].content_parts[0].input_json.find("note.txt") != std::string::npos && (*messages)[5].content_parts.size() == 1 &&
             (*messages)[5].content_parts[0].type == ava::provider::ContentPartType::ToolResult &&
             (*messages)[5].content_parts[0].tool_call_id == "ava_history_tool_1" && (*messages)[5].content_parts[0].tool_name == "read_file" &&
             (*messages)[5].content_parts[0].text == "note contents" && !(*messages)[5].content_parts[0].is_error,
         "portable tool-call and tool-result entries carry request-local native parts without persisted call ids");
}

void test_tool_content_parts_reconstruction()
{
  std::string const long_result(80, 'r');
  std::vector<ava::session::SessionEntry> const entries = {ava::session::SessionEntry{.id = "tool_call",
                                                                                      .parent_id = "",
                                                                                      .type = ava::session::EntryType::ToolCall,
                                                                                      .timestamp = "2026-04-27T00:00:00Z",
                                                                                      .data_json = "{\"call_id\":\"call_failed\",\"name\":\"bash\","
                                                                                                   "\"arguments\":\"{\\\"cmd\\\":\\\"false\\\"}\"}"},
                                                           ava::session::SessionEntry{.id = "tool_result",
                                                                                      .parent_id = "",
                                                                                      .type = ava::session::EntryType::ToolResult,
                                                                                      .timestamp = "2026-04-27T00:00:01Z",
                                                                                      .data_json = "{\"call_id\":\"call_failed\",\"name\":\"bash\","
                                                                                                   "\"success\":false,\"result\":\"" +
                                                                                                   long_result + "\"}"}};

  auto messages = ava::agent::build_provider_messages_from_entries(
      entries, ava::agent::MessageBuildOptions{.max_tool_result_context_bytes = 48,
                                               .target = ava::agent::HistoryReplayTarget{.provider_id = "test",
                                                                                         .model_id = "test-tools",
                                                                                         .api_family = "test_api",
                                                                                         .reasoning_format = "",
                                                                                         .supports_tools = true,
                                                                                         .supports_images = false}});
  expect(messages && messages->size() == 2, "tool entries reconstruct as provider messages");
  if (!messages || messages->size() != 2)
    return;

  expect((*messages)[0].role == "assistant" && (*messages)[0].content.find("Tool call (bash)") != std::string::npos &&
             (*messages)[0].content_parts.size() == 1 && (*messages)[0].content_parts[0].type == ava::provider::ContentPartType::ToolUse &&
             (*messages)[0].content_parts[0].tool_call_id == "ava_history_tool_1" && (*messages)[0].content_parts[0].tool_name == "bash" &&
             (*messages)[0].content_parts[0].input_json.find("false") != std::string::npos,
         "portable tool-call entry reconstructs an assistant tool-use part with a request-local id");
  expect((*messages)[1].role == "user" && !(*messages)[1].content.empty() && (*messages)[1].content_parts.size() == 1 &&
             (*messages)[1].content_parts[0].type == ava::provider::ContentPartType::ToolResult &&
             (*messages)[1].content_parts[0].tool_call_id == "ava_history_tool_1" && (*messages)[1].content_parts[0].tool_name == "bash" &&
             (*messages)[1].content_parts[0].is_error && (*messages)[1].content_parts[0].text.size() == 48 &&
             (*messages)[1].content_parts[0].text.find("[AVA: tool result content truncated]") != std::string::npos,
         "portable failed tool-result entry preserves bounded error metadata without its persisted call id");

  std::vector<ava::session::SessionEntry> const permission_entries = {
      ava::session::SessionEntry{.id = "permission_tool_call",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-04-27T00:00:00Z",
                                 .data_json = "{\"call_id\":\"call_permission\",\"name\":\"read_file\","
                                              "\"arguments\":\"{}\"}"},
      ava::session::SessionEntry{.id = "permission_decision",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::PermissionDecision,
                                 .timestamp = "2026-04-27T00:00:01Z",
                                 .data_json = "{\"resolution\":\"allow\"}"},
      ava::session::SessionEntry{.id = "permission_tool_result",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-27T00:00:02Z",
                                 .data_json = "{\"call_id\":\"call_permission\",\"name\":\"read_file\","
                                              "\"success\":true,\"result\":\"permission result\"}"}};
  auto permission_messages = ava::agent::build_provider_messages_from_entries(
      permission_entries, ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = "test",
                                                                                                    .model_id = "test-tools",
                                                                                                    .api_family = "test_api",
                                                                                                    .reasoning_format = "",
                                                                                                    .supports_tools = true,
                                                                                                    .supports_images = false}});
  expect(permission_messages && permission_messages->size() == 2, "permission decisions are internal metadata during provider replay");
  if (!permission_messages || permission_messages->size() != 2)
    return;
  expect((*permission_messages)[0].content_parts.size() == 1 && (*permission_messages)[1].content_parts.size() == 1,
         "native tool replay allows internal permission metadata between tool call and result");

  std::vector<ava::session::SessionEntry> const paired_batch_entries = {
      ava::session::SessionEntry{.id = "batch_assistant",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-04-27T00:00:00Z",
                                 .data_json = "{\"text\":\"\",\"tool_calls\":2}"},
      ava::session::SessionEntry{.id = "batch_call_first",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-04-27T00:00:01Z",
                                 .data_json = "{\"call_id\":\"call_batch_first\",\"name\":\"read_file\","
                                              "\"arguments\":\"{\\\"path\\\":\\\"first.txt\\\"}\"}"},
      ava::session::SessionEntry{.id = "batch_result_first",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-27T00:00:02Z",
                                 .data_json = "{\"call_id\":\"call_batch_first\",\"name\":\"read_file\","
                                              "\"success\":true,\"result\":\"first result\"}"},
      ava::session::SessionEntry{.id = "batch_call_second",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-04-27T00:00:03Z",
                                 .data_json = "{\"call_id\":\"call_batch_second\",\"name\":\"read_file\","
                                              "\"arguments\":\"{\\\"path\\\":\\\"second.txt\\\"}\"}"},
      ava::session::SessionEntry{.id = "batch_result_second",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-27T00:00:04Z",
                                 .data_json = "{\"call_id\":\"call_batch_second\",\"name\":\"read_file\","
                                              "\"success\":true,\"result\":\"second result\"}"}};
  auto paired_batch_messages = ava::agent::build_provider_messages_from_entries(
      paired_batch_entries, ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = "test",
                                                                                                      .model_id = "test-tools",
                                                                                                      .api_family = "test_api",
                                                                                                      .reasoning_format = "",
                                                                                                      .supports_tools = true,
                                                                                                      .supports_images = false}});
  expect(paired_batch_messages && paired_batch_messages->size() == 2,
         "provider-order portable multi-tool pairs replay as one request-local tool-use/tool-result batch");
  if (!paired_batch_messages || paired_batch_messages->size() != 2)
    return;
  expect((*paired_batch_messages)[0].role == "assistant" && (*paired_batch_messages)[0].content_parts.size() == 2 &&
             (*paired_batch_messages)[0].content_parts[0].type == ava::provider::ContentPartType::ToolUse &&
             (*paired_batch_messages)[0].content_parts[0].tool_call_id == "ava_history_tool_1" &&
             (*paired_batch_messages)[0].content_parts[0].tool_name == "read_file" &&
             (*paired_batch_messages)[0].content_parts[1].type == ava::provider::ContentPartType::ToolUse &&
             (*paired_batch_messages)[0].content_parts[1].tool_call_id == "ava_history_tool_2" &&
             (*paired_batch_messages)[0].content_parts[1].tool_name == "read_file" && (*paired_batch_messages)[1].role == "user" &&
             (*paired_batch_messages)[1].content_parts.size() == 2 &&
             (*paired_batch_messages)[1].content_parts[0].type == ava::provider::ContentPartType::ToolResult &&
             (*paired_batch_messages)[1].content_parts[0].tool_call_id == "ava_history_tool_1" &&
             (*paired_batch_messages)[1].content_parts[1].type == ava::provider::ContentPartType::ToolResult &&
             (*paired_batch_messages)[1].content_parts[1].tool_call_id == "ava_history_tool_2" &&
             (*paired_batch_messages)[1].content_parts[0].text == "first result" && (*paired_batch_messages)[1].content_parts[1].text == "second result",
         "portable multi-tool replay preserves provider order while replacing persisted call ids");

  auto const content_parts_empty = [](std::vector<ava::provider::ChatMessage> const& built_messages) {
    for (auto const& message : built_messages)
    {
      if (!message.content_parts.empty())
        return false;
    }
    return true;
  };

  std::vector<ava::session::SessionEntry> const detached_batch_entries = {paired_batch_entries[0], paired_batch_entries[1], paired_batch_entries[3],
                                                                          paired_batch_entries[2], paired_batch_entries[4]};
  auto detached_batch_messages = ava::agent::build_provider_messages_from_entries(detached_batch_entries);
  expect(detached_batch_messages && detached_batch_messages->size() == 5 && content_parts_empty(*detached_batch_messages),
         "native multi-tool replay requires contiguous call/result pairs and falls back when results are detached from their calls");

  std::vector<ava::session::SessionEntry> const reordered_batch_entries = {paired_batch_entries[0], paired_batch_entries[1], paired_batch_entries[4],
                                                                           paired_batch_entries[3], paired_batch_entries[2]};
  auto reordered_batch_messages = ava::agent::build_provider_messages_from_entries(reordered_batch_entries);
  expect(reordered_batch_messages && reordered_batch_messages->size() == 5 && content_parts_empty(*reordered_batch_messages),
         "native multi-tool replay rejects reordered tool results instead of attaching them to the wrong calls");

  constexpr std::string_view truncation_marker = "\n[AVA: tool result content truncated]";
  std::string const euro = std::string("\xE2") + "\x82" + "\xAC";
  std::string const utf8_result = "abc" + euro + std::string(80, 'x');
  std::vector<ava::session::SessionEntry> const utf8_entries = {ava::session::SessionEntry{.id = "utf8_tool_call",
                                                                                           .parent_id = "",
                                                                                           .type = ava::session::EntryType::ToolCall,
                                                                                           .timestamp = "2026-04-27T00:00:00Z",
                                                                                           .data_json = "{\"call_id\":\"call_utf8\",\"name\":\"bash\","
                                                                                                        "\"arguments\":\"{}\"}"},
                                                                ava::session::SessionEntry{.id = "utf8_tool_result",
                                                                                           .parent_id = "",
                                                                                           .type = ava::session::EntryType::ToolResult,
                                                                                           .timestamp = "2026-04-27T00:00:01Z",
                                                                                           .data_json = "{\"call_id\":\"call_utf8\",\"name\":\"bash\","
                                                                                                        "\"success\":true,\"result\":\"" +
                                                                                                        utf8_result + "\"}"}};
  auto utf8_messages = ava::agent::build_provider_messages_from_entries(
      utf8_entries, ava::agent::MessageBuildOptions{.max_tool_result_context_bytes = truncation_marker.size() + 4,
                                                    .target = ava::agent::HistoryReplayTarget{.provider_id = "test",
                                                                                              .model_id = "test-tools",
                                                                                              .api_family = "test_api",
                                                                                              .reasoning_format = "",
                                                                                              .supports_tools = true,
                                                                                              .supports_images = false}});
  expect(utf8_messages && utf8_messages->size() == 2, "utf8 tool entries reconstruct as provider messages");
  if (!utf8_messages || utf8_messages->size() != 2)
    return;
  expect((*utf8_messages)[1].content_parts[0].text.rfind("abc\n[AVA: tool result content truncated]", 0) == 0 &&
             (*utf8_messages)[1].content_parts[0].text.find(euro) == std::string::npos,
         "native tool-result truncation avoids splitting utf8 code points");

  std::vector<ava::session::SessionEntry> const malformed_success_entries = {
      ava::session::SessionEntry{.id = "malformed_success_call",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-04-27T00:00:00Z",
                                 .data_json = "{\"call_id\":\"call_success_prefix\",\"name\":\"bash\","
                                              "\"arguments\":\"{}\"}"},
      ava::session::SessionEntry{.id = "malformed_success_result",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-27T00:00:01Z",
                                 .data_json = "{\"call_id\":\"call_success_prefix\",\"name\":\"bash\","
                                              "\"success\":falsefoo,\"result\":\"bool result\"}"}};
  auto malformed_success_messages = ava::agent::build_provider_messages_from_entries(malformed_success_entries);
  expect(malformed_success_messages && malformed_success_messages->size() == 2, "malformed bool tool entries reconstruct as provider messages");
  if (!malformed_success_messages || malformed_success_messages->size() != 2)
    return;
  expect((*malformed_success_messages)[1].content_parts.empty() && (*malformed_success_messages)[1].content.find("bool result") != std::string::npos,
         "malformed success bool prefixes remain bounded text instead of native error metadata");

  std::vector<ava::session::SessionEntry> const malformed_entries = {ava::session::SessionEntry{.id = "malformed_tool_call",
                                                                                                .parent_id = "",
                                                                                                .type = ava::session::EntryType::ToolCall,
                                                                                                .timestamp = "2026-04-27T00:00:00Z",
                                                                                                .data_json = "{\"call_id\":\"call_bad\",\"name\":\"bash\","
                                                                                                             "\"arguments\":\"{\\\"cmd\\\":}\"}"},
                                                                     ava::session::SessionEntry{.id = "malformed_tool_result",
                                                                                                .parent_id = "",
                                                                                                .type = ava::session::EntryType::ToolResult,
                                                                                                .timestamp = "2026-04-27T00:00:01Z",
                                                                                                .data_json = "{\"call_id\":\"call_bad\",\"name\":\"bash\","
                                                                                                             "\"success\":true,\"result\":\"still visible\"}"}};
  auto malformed_messages = ava::agent::build_provider_messages_from_entries(malformed_entries);
  expect(malformed_messages && malformed_messages->size() == 2, "malformed tool entries still reconstruct as fallback provider messages");
  if (!malformed_messages || malformed_messages->size() != 2)
    return;
  expect((*malformed_messages)[0].content.find("cmd") != std::string::npos && (*malformed_messages)[0].content_parts.empty() &&
             (*malformed_messages)[1].content.find("still visible") != std::string::npos && (*malformed_messages)[1].content_parts.empty(),
         "malformed native tool-use replay falls back to text-only without dangling native tool-result");

  std::vector<ava::session::SessionEntry> const malformed_id_entries = {
      ava::session::SessionEntry{.id = "malformed_id_tool_call",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-04-27T00:00:00Z",
                                 .data_json = "{\"call_id\":\"call\\nbad\",\"name\":\"bash\","
                                              "\"arguments\":\"{}\"}"},
      ava::session::SessionEntry{.id = "malformed_id_tool_result",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-27T00:00:01Z",
                                 .data_json = "{\"call_id\":\"call\\nbad\",\"name\":\"bash\","
                                              "\"success\":true,\"result\":\"still visible\"}"}};
  auto malformed_id_messages = ava::agent::build_provider_messages_from_entries(malformed_id_entries);
  expect(malformed_id_messages && malformed_id_messages->size() == 2, "malformed tool id entries still reconstruct as fallback provider messages");
  if (!malformed_id_messages || malformed_id_messages->size() != 2)
    return;
  expect((*malformed_id_messages)[0].content_parts.empty() && (*malformed_id_messages)[1].content_parts.empty(),
         "malformed native tool ids fall back to text-only replay");

  std::vector<ava::session::SessionEntry> const duplicate_batch_id_entries = {
      ava::session::SessionEntry{.id = "duplicate_batch_assistant",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-04-27T00:00:00Z",
                                 .data_json = "{\"text\":\"\",\"tool_calls\":2}"},
      ava::session::SessionEntry{.id = "duplicate_batch_tool_call_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-04-27T00:00:01Z",
                                 .data_json = "{\"call_id\":\"call_duplicate_batch\",\"name\":\"bash\","
                                              "\"arguments\":\"{}\"}"},
      ava::session::SessionEntry{.id = "duplicate_batch_tool_result_1",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-27T00:00:02Z",
                                 .data_json = "{\"call_id\":\"call_duplicate_batch\",\"name\":\"bash\","
                                              "\"success\":true,\"result\":\"first result\"}"},
      ava::session::SessionEntry{.id = "duplicate_batch_tool_call_2",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolCall,
                                 .timestamp = "2026-04-27T00:00:03Z",
                                 .data_json = "{\"call_id\":\"call_duplicate_batch\",\"name\":\"bash\","
                                              "\"arguments\":\"{}\"}"},
      ava::session::SessionEntry{.id = "duplicate_batch_tool_result_2",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ToolResult,
                                 .timestamp = "2026-04-27T00:00:04Z",
                                 .data_json = "{\"call_id\":\"call_duplicate_batch\",\"name\":\"bash\","
                                              "\"success\":true,\"result\":\"second result\"}"}};
  auto duplicate_batch_id_messages = ava::agent::build_provider_messages_from_entries(duplicate_batch_id_entries);
  expect(duplicate_batch_id_messages && duplicate_batch_id_messages->size() == 5, "duplicate same-turn tool ids reconstruct as fallback provider messages");
  if (!duplicate_batch_id_messages || duplicate_batch_id_messages->size() != 5)
    return;
  expect((*duplicate_batch_id_messages)[1].content_parts.empty() && (*duplicate_batch_id_messages)[2].content_parts.empty() &&
             (*duplicate_batch_id_messages)[3].content_parts.empty() && (*duplicate_batch_id_messages)[4].content_parts.empty(),
         "duplicate same-turn native tool ids fall back to text-only replay");

  std::vector<ava::session::SessionEntry> const reused_id_entries = {ava::session::SessionEntry{.id = "valid_tool_call",
                                                                                                .parent_id = "",
                                                                                                .type = ava::session::EntryType::ToolCall,
                                                                                                .timestamp = "2026-04-27T00:00:00Z",
                                                                                                .data_json = "{\"call_id\":\"call_reused\",\"name\":\"bash\","
                                                                                                             "\"arguments\":\"{}\"}"},
                                                                     ava::session::SessionEntry{.id = "valid_tool_result",
                                                                                                .parent_id = "",
                                                                                                .type = ava::session::EntryType::ToolResult,
                                                                                                .timestamp = "2026-04-27T00:00:01Z",
                                                                                                .data_json = "{\"call_id\":\"call_reused\",\"name\":\"bash\","
                                                                                                             "\"success\":true,\"result\":\"first result\"}"},
                                                                     ava::session::SessionEntry{.id = "valid_reused_tool_call",
                                                                                                .parent_id = "",
                                                                                                .type = ava::session::EntryType::ToolCall,
                                                                                                .timestamp = "2026-04-27T00:00:02Z",
                                                                                                .data_json = "{\"call_id\":\"call_reused\",\"name\":\"bash\","
                                                                                                             "\"arguments\":\"{}\"}"},
                                                                     ava::session::SessionEntry{.id = "valid_reused_tool_result",
                                                                                                .parent_id = "",
                                                                                                .type = ava::session::EntryType::ToolResult,
                                                                                                .timestamp = "2026-04-27T00:00:03Z",
                                                                                                .data_json = "{\"call_id\":\"call_reused\",\"name\":\"bash\","
                                                                                                             "\"success\":true,\"result\":\"second result\"}"}};
  auto reused_id_messages = ava::agent::build_provider_messages_from_entries(
      reused_id_entries, ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = "test",
                                                                                                   .model_id = "test-tools",
                                                                                                   .api_family = "test_api",
                                                                                                   .reasoning_format = "",
                                                                                                   .supports_tools = true,
                                                                                                   .supports_images = false}});
  expect(reused_id_messages && reused_id_messages->size() == 4, "reused tool ids reconstruct as provider messages");
  if (!reused_id_messages || reused_id_messages->size() != 4)
    return;
  expect((*reused_id_messages)[0].content_parts.size() == 1 && (*reused_id_messages)[1].content_parts.size() == 1 &&
             (*reused_id_messages)[2].content_parts.size() == 1 && (*reused_id_messages)[3].content_parts.size() == 1 &&
             (*reused_id_messages)[0].content_parts[0].tool_call_id == "ava_history_tool_1" &&
             (*reused_id_messages)[1].content_parts[0].tool_call_id == "ava_history_tool_1" &&
             (*reused_id_messages)[2].content_parts[0].tool_call_id == "ava_history_tool_2" &&
             (*reused_id_messages)[3].content_parts[0].tool_call_id == "ava_history_tool_2",
         "portable repeated source tool ids receive distinct deterministic request-local pair ids");

  std::vector<ava::session::SessionEntry> const interrupted_entries = {ava::session::SessionEntry{.id = "interrupted_tool_call",
                                                                                                  .parent_id = "",
                                                                                                  .type = ava::session::EntryType::ToolCall,
                                                                                                  .timestamp = "2026-04-27T00:00:00Z",
                                                                                                  .data_json = "{\"call_id\":\"call_late\",\"name\":\"bash\","
                                                                                                               "\"arguments\":\"{}\"}"},
                                                                       ava::session::SessionEntry{.id = "intervening_user",
                                                                                                  .parent_id = "",
                                                                                                  .type = ava::session::EntryType::UserMessage,
                                                                                                  .timestamp = "2026-04-27T00:00:01Z",
                                                                                                  .data_json = "{\"text\":\"intervening user\"}"},
                                                                       ava::session::SessionEntry{.id = "late_tool_result",
                                                                                                  .parent_id = "",
                                                                                                  .type = ava::session::EntryType::ToolResult,
                                                                                                  .timestamp = "2026-04-27T00:00:02Z",
                                                                                                  .data_json = "{\"call_id\":\"call_late\",\"name\":\"bash\","
                                                                                                               "\"success\":true,\"result\":\"late result\"}"}};
  auto interrupted_messages = ava::agent::build_provider_messages_from_entries(interrupted_entries);
  expect(interrupted_messages && interrupted_messages->size() == 3, "interrupted tool entries still reconstruct as provider messages");
  if (!interrupted_messages || interrupted_messages->size() != 3)
    return;
  expect((*interrupted_messages)[0].content_parts.empty() && (*interrupted_messages)[2].content_parts.empty(),
         "non-contiguous tool-use/result history falls back to text-only native replay");
}

void test_portable_omitted_reasoning_is_dropped_from_provider_replay()
{
  std::vector<ava::session::SessionEntry> const entries = {
      ava::session::SessionEntry{.id = "portable_reasoning",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::ReasoningBlock,
                                 .timestamp = "2026-05-11T00:00:00Z",
                                 .data_json = "{\"provider\":\"anthropic\",\"model\":\"claude-sonnet-4-5\",\"format\":\"anthropic_thinking\","
                                              "\"text\":\"[Provider-private reasoning metadata omitted from portable export.]\",\"redacted\":true,"
                                              "\"private_replay_metadata_omitted\":{\"native_item_json\":false,\"signature\":false,\"redacted_data\":true}}"},
      ava::session::SessionEntry{.id = "portable_answer",
                                 .parent_id = "",
                                 .type = ava::session::EntryType::AssistantMessage,
                                 .timestamp = "2026-05-11T00:00:01Z",
                                 .data_json = "{\"text\":\"answer after portable import\",\"tool_calls\":0}"}};

  auto messages = ava::agent::build_provider_messages_from_entries(entries);
  expect(messages && messages->size() == 1 && (*messages)[0].content_parts.size() == 1 &&
             (*messages)[0].content_parts[0].type == ava::provider::ContentPartType::Text && !(*messages)[0].content_parts[0].redacted &&
             (*messages)[0].content_parts[0].text == "answer after portable import",
         "portable private-replay omission markers are dropped while visible assistant answers remain");
  if (!messages)
    return;

  ava::provider::AnthropicProvider const provider("https://anthropic.example.test");
  auto request = provider.build_request(
      ava::provider::ProviderRequest{
          .provider_id = "anthropic", .model_id = "claude-sonnet-4-5", .system_prompt = "system", .messages = *messages, .tools_json = {}, .stream = false},
      "anthropic-key");
  expect(request && request->body.find("metadata omitted") == std::string::npos && request->body.find("redacted_thinking") == std::string::npos &&
             request->body.find("answer after portable import") != std::string::npos,
         "portable imported Anthropic reasoning is omitted entirely while visible answer text remains");

  auto const v4_reasoning_data = ava::session::serialize_assistant_output_item_data_json(
      ava::session::AssistantOutputItem{.assistant_turn_id = "portable_v4_reasoning_turn",
                                        .sequence = 0,
                                        .kind = ava::session::AssistantOutputItemKind::Reasoning,
                                        .provider_item_id = "PRIVATE_PORTABLE_V4_ITEM",
                                        .provider_output_index = 0,
                                        .payload = ava::session::AssistantOutputReasoning{.text = "PRIVATE_REDACTED_V4_TEXT",
                                                                                          .format = "anthropic_thinking",
                                                                                          .redacted = true,
                                                                                          .signature = "PRIVATE_REDACTED_V4_SIGNATURE",
                                                                                          .redacted_data = "PRIVATE_REDACTED_V4_DATA",
                                                                                          .native_item_json = "{\"type\":\"reasoning\"}"}});
  auto const v4_commit_data =
      ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{.assistant_turn_id = "portable_v4_reasoning_turn",
                                                                                                .item_count = 1,
                                                                                                .provider = "anthropic",
                                                                                                .model = "claude-sonnet-4-5",
                                                                                                .finish_reason = "completed",
                                                                                                .usage_json = std::nullopt});
  std::vector<ava::session::SessionEntry> const private_v4_entries = {{.id = "portable_v4_reasoning",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::AssistantOutputItem,
                                                                       .timestamp = "2026-07-18T00:00:00Z",
                                                                       .data_json = v4_reasoning_data.value_or("{}")},
                                                                      {.id = "portable_v4_commit",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::AssistantTurnCommit,
                                                                       .timestamp = "2026-07-18T00:00:01Z",
                                                                       .data_json = v4_commit_data.value_or("{}")}};
  auto portable_v4_entries = ava::session::project_portable_session_history(private_v4_entries);
  std::optional<std::vector<ava::provider::ChatMessage>> portable_v4_messages;
  if (portable_v4_entries)
  {
    auto built_messages = ava::agent::build_provider_messages_from_entries(*portable_v4_entries);
    if (built_messages)
      portable_v4_messages = std::move(*built_messages);
  }
  expect(portable_v4_entries && portable_v4_entries->front().data_json.find("private_replay_metadata_omitted\":true") != std::string::npos &&
             portable_v4_entries->front().data_json.find("PRIVATE_REDACTED_V4_SIGNATURE") == std::string::npos && portable_v4_messages &&
             portable_v4_messages->empty(),
         "portable v4 signed/redacted reasoning is dropped entirely for every provider request projection");
}

}  // namespace session_tests
