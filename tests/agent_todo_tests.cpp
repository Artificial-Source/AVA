#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/agent/todo.h"
#include "ava/agent/tool_dispatch_todo.h"
#include "ava/agent/tool_registration.h"
#include "ava/agent/tool_summaries.h"
#include "ava/tools/file_tools.h"
#include "ava/session/session_store.h"
#include "ava/core/mode.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ava::session::EntryType;
using ava::session::SessionEntry;

ava::tools::ToolContext test_context(std::filesystem::path const& workspace)
{
  return ava::tools::ToolContext{.workspace_dir = workspace, .mode = ava::core::Mode::Build, .session_id = "session_todo"};
}

ava::agent::ProviderToolCall todo_call(std::string_view arguments)
{
  return ava::agent::ProviderToolCall{.id = "call_todo", .name = "todowrite", .arguments_json = std::string(arguments)};
}

SessionEntry tool_result_entry(std::string id, std::string data_json)
{
  return SessionEntry{
      .id = std::move(id), .parent_id = "", .type = EntryType::ToolResult, .timestamp = "2026-07-24T00:00:00Z", .data_json = std::move(data_json)};
}

void test_todowrite_registry_and_success_snapshot()
{
  auto const& registry = ava::agent::builtin_tool_registry();
  auto const* meta = registry.find("todowrite");
  if (!ava::tests::require(meta != nullptr, "todowrite registry entry exists"))
    return;
  ava::tests::expect_all("todowrite is registered as a synchronous user-owned builtin", {{"permission category", meta->metadata.permission_category == "user"},
                                                                                         {"execution mode", meta->metadata.execution_mode == "synchronous"}});
  expect(meta->metadata.schema_json.find("\"name\":\"todowrite\"") != std::string::npos, "todowrite schema advertises the tool name");

  auto const root = create_empty_root("todowrite-success");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto result = ava::agent::todowrite_result(
      test_context(workspace),
      todo_call(
          R"({"todos":[{"id":"setup","content":"Prepare workspace","status":"completed"},{"id":"impl","content":"Implement feature","status":"in_progress"},{"id":"verify","content":"Run tests","status":"pending"}]})"));
  expect(result.success && result.result_text.find("\"ok\":true") != std::string::npos &&
             result.result_text.find("\"schema_version\":1") != std::string::npos && result.result_text.find("\"in_progress\":1") != std::string::npos &&
             result.payload.summary == "1/3 completed",
         "successful todowrite returns normalized full-list JSON with counts");

  auto parsed = ava::agent::parse_todowrite_result_text(result.result_text);
  expect(parsed && parsed->todos.size() == 3 && parsed->counts.pending == 1 && parsed->counts.in_progress == 1 && parsed->counts.completed == 1,
         "normalized todowrite success result round-trips through the strict parser");

  auto summary = ava::agent::summarize_tool_arguments(todo_call(R"({"todos":[{"id":"a","content":"A","status":"pending"}]})"));
  expect(summary == "1 item", "todowrite argument summary is concise");
  auto clear_summary = ava::agent::summarize_tool_arguments(todo_call(R"({"todos":[]})"));
  expect(clear_summary == "clear", "empty todowrite argument summary says clear");
}

void test_todowrite_validation_bounds()
{
  auto const root = create_empty_root("todowrite-validate");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto ctx = test_context(workspace);

  auto unknown = ava::agent::todowrite_result(ctx, todo_call(R"({"todos":[],"extra":true})"));
  expect(!unknown.success && unknown.result_text.find("unknown field") != std::string::npos, "todowrite rejects unknown top-level fields");

  auto dup_members = ava::agent::todowrite_result(ctx, todo_call(R"({"todos":[],"todos":[]})"));
  expect(!dup_members.success && dup_members.result_text.find("duplicate") != std::string::npos, "todowrite rejects duplicate JSON members");

  auto bad_id = ava::agent::todowrite_result(ctx, todo_call(R"({"todos":[{"id":"bad id!","content":"x","status":"pending"}]})"));
  ava::tests::expect_all("todowrite rejects non-semantic ids",
                         {{"failed result", !bad_id.success},
                          {"invalid argument", bad_id.payload.error_category == "invalid_argument"},
                          {"ASCII id rule", bad_id.payload.error_message == "todowrite todo id must be 1..32 ASCII bytes matching [A-Za-z0-9_-]"}});

  // Contract is explicit ASCII [A-Za-z0-9_-]; non-ASCII letters/bytes must fail independently of locale.
  auto utf8_letter_id = ava::agent::todowrite_result(ctx, todo_call(R"({"todos":[{"id":"caf\u00e9","content":"x","status":"pending"}]})"));
  ava::tests::expect_all("todowrite rejects non-ASCII UTF-8 letters in todo ids",
                         {{"failed result", !utf8_letter_id.success},
                          {"invalid argument", utf8_letter_id.payload.error_category == "invalid_argument"},
                          {"ASCII id rule", utf8_letter_id.payload.error_message == "todowrite todo id must be 1..32 ASCII bytes matching [A-Za-z0-9_-]"}});

  auto ascii_ok = ava::agent::todowrite_result(ctx, todo_call(R"({"todos":[{"id":"A-z_09","content":"x","status":"pending"}]})"));
  expect(ascii_ok.success, "todowrite still accepts valid ASCII semantic ids");

  auto empty_content = ava::agent::todowrite_result(ctx, todo_call(R"({"todos":[{"id":"a","content":"","status":"pending"}]})"));
  expect(!empty_content.success, "todowrite rejects empty content");

  auto control = ava::agent::todowrite_result(ctx, todo_call("{\"todos\":[{\"id\":\"a\",\"content\":\"bad\\nline\",\"status\":\"pending\"}]}"));
  expect(!control.success && control.result_text.find("control") != std::string::npos, "todowrite rejects control text in content");

  auto bad_status = ava::agent::todowrite_result(ctx, todo_call(R"({"todos":[{"id":"a","content":"x","status":"cancelled"}]})"));
  expect(!bad_status.success, "todowrite rejects unknown status values");

  auto two_progress = ava::agent::todowrite_result(
      ctx, todo_call(R"({"todos":[{"id":"a","content":"one","status":"in_progress"},{"id":"b","content":"two","status":"in_progress"}]})"));
  expect(!two_progress.success && two_progress.result_text.find("in_progress") != std::string::npos, "todowrite allows at most one in_progress item");

  auto dup_ids = ava::agent::todowrite_result(
      ctx, todo_call(R"({"todos":[{"id":"a","content":"one","status":"pending"},{"id":"a","content":"two","status":"pending"}]})"));
  expect(!dup_ids.success && dup_ids.result_text.find("unique") != std::string::npos, "todowrite rejects duplicate ids");

  std::string many = {"{\"todos\":["};
  for (int i = 0; i < 51; ++i)
  {
    if (i)
      many += ',';
    many += "{\"id\":\"i" + std::to_string(i) + "\",\"content\":\"c\",\"status\":\"pending\"}";
  }
  many += "]}";
  auto too_many = ava::agent::todowrite_result(ctx, todo_call(many));
  expect(!too_many.success && too_many.result_text.find("50") != std::string::npos, "todowrite rejects more than 50 items");

  auto clear = ava::agent::todowrite_result(ctx, todo_call(R"({"todos":[]})"));
  expect(clear.success && clear.result_text.find("\"todos\":[]") != std::string::npos && clear.payload.summary == "todos cleared",
         "empty array clears the todo list");

  auto zero_progress = ava::agent::todowrite_result(
      ctx, todo_call(R"({"todos":[{"id":"a","content":"one","status":"pending"},{"id":"b","content":"two","status":"completed"}]})"));
  expect(zero_progress.success && zero_progress.result_text.find("\"in_progress\":0") != std::string::npos, "todowrite allows zero in_progress items");
}

void test_todowrite_session_projection()
{
  std::vector<SessionEntry> entries;
  entries.push_back(tool_result_entry("e1", R"({"call_id":"c1","name":"todowrite","success":false,"status":"error","result":"{\"ok\":false}"})"));
  entries.push_back(tool_result_entry(
      "e2",
      R"({"call_id":"c2","name":"todowrite","success":true,"status":"success","result":"{\"schema_version\":1,\"tool\":\"todowrite\",\"ok\":true,\"todos\":[{\"id\":\"a\",\"content\":\"First\",\"status\":\"pending\"}],\"counts\":{\"total\":1,\"pending\":1,\"in_progress\":0,\"completed\":0}}"})"));
  entries.push_back(tool_result_entry("e3", R"({"call_id":"c3","name":"read_file","success":true,"status":"success","result":"{}"})"));
  entries.push_back(tool_result_entry(
      "e4",
      R"({"call_id":"c4","name":"todowrite","success":true,"status":"success","result":"{\"schema_version\":1,\"tool\":\"todowrite\",\"ok\":true,\"todos\":[{\"id\":\"a\",\"content\":\"First\",\"status\":\"completed\"},{\"id\":\"b\",\"content\":\"Second\",\"status\":\"in_progress\"}],\"counts\":{\"total\":2,\"pending\":0,\"in_progress\":1,\"completed\":1}}"})"));

  auto latest = ava::agent::latest_committed_todowrite_snapshot(entries);
  expect(latest && latest->todos.size() == 2 && latest->todos[0].status == ava::agent::TodoStatus::Completed &&
             latest->todos[1].status == ava::agent::TodoStatus::InProgress,
         "latest successful committed todowrite snapshot wins over earlier and failed results");

  entries.push_back(tool_result_entry(
      "e5",
      R"({"call_id":"c5","name":"todowrite","success":true,"status":"success","result":"{\"schema_version\":1,\"tool\":\"todowrite\",\"ok\":true,\"todos\":[],\"counts\":{\"total\":0,\"pending\":0,\"in_progress\":0,\"completed\":0}}"})"));
  latest = ava::agent::latest_committed_todowrite_snapshot(entries);
  expect(latest && latest->todos.empty(), "empty successful todowrite snapshot clears projected todos");

  entries.push_back(entries[3]);
  entries.push_back(tool_result_entry("e6", R"({"call_id":"c6","name":"todowrite","success":true,"status":"success","result":"{\"ok\":true,\"todos\":[}"})"));
  latest = ava::agent::latest_committed_todowrite_snapshot(entries);
  if (!ava::tests::require(latest && latest->todos.size() == 2, "malformed later todowrite success preserves the non-empty committed snapshot"))
    return;
  ava::tests::expect_all("malformed later todowrite success leaves both prior items unchanged",
                         {{"first id", latest->todos[0].id == "a"},
                          {"first content", latest->todos[0].content == "First"},
                          {"first status", latest->todos[0].status == ava::agent::TodoStatus::Completed},
                          {"second id", latest->todos[1].id == "b"},
                          {"second content", latest->todos[1].content == "Second"},
                          {"second status", latest->todos[1].status == ava::agent::TodoStatus::InProgress}});
}

void test_todowrite_result_parser_rejects_invalid_snapshots()
{
  std::string const prefix = R"({"schema_version":1,"tool":"todowrite","ok":true,"todos":[])";
  std::vector<std::pair<std::string, std::string>> const invalid{
      {"malformed JSON", "{"},
      {"non-object", "[]"},
      {"unknown field", prefix + R"(,"extra":true})"},
      {"duplicate field", prefix + R"(,"ok":true})"},
      {"missing ok", R"({"schema_version":1,"tool":"todowrite","todos":[]})"},
      {"false ok", R"({"schema_version":1,"tool":"todowrite","ok":false,"todos":[]})"},
      {"non-boolean ok", R"({"schema_version":1,"tool":"todowrite","ok":1,"todos":[]})"},
      {"missing tool", R"({"schema_version":1,"ok":true,"todos":[]})"},
      {"wrong tool", R"({"schema_version":1,"tool":"read_file","ok":true,"todos":[]})"},
      {"non-string tool", R"({"schema_version":1,"tool":1,"ok":true,"todos":[]})"},
      {"missing version", R"({"tool":"todowrite","ok":true,"todos":[]})"},
      {"unsupported version", R"({"schema_version":2,"tool":"todowrite","ok":true,"todos":[]})"},
      {"non-integer version", R"({"schema_version":1.5,"tool":"todowrite","ok":true,"todos":[]})"},
      {"missing todos", R"({"schema_version":1,"tool":"todowrite","ok":true})"},
      {"non-array todos", R"({"schema_version":1,"tool":"todowrite","ok":true,"todos":{}})"},
      {"non-object counts", prefix + R"(,"counts":[]})"},
      {"unknown count", prefix + R"(,"counts":{"total":0,"pending":0,"in_progress":0,"completed":0,"extra":0}})"},
      {"missing count", prefix + R"(,"counts":{"pending":0,"in_progress":0,"completed":0}})"},
      {"negative count", prefix + R"(,"counts":{"total":-1,"pending":0,"in_progress":0,"completed":0}})"},
      {"non-integer count", prefix + R"(,"counts":{"total":0.5,"pending":0,"in_progress":0,"completed":0}})"},
      {"total mismatch", prefix + R"(,"counts":{"total":1,"pending":0,"in_progress":0,"completed":0}})"},
      {"pending mismatch", prefix + R"(,"counts":{"total":0,"pending":1,"in_progress":0,"completed":0}})"},
      {"in-progress mismatch", prefix + R"(,"counts":{"total":0,"pending":0,"in_progress":1,"completed":0}})"},
      {"completed mismatch", prefix + R"(,"counts":{"total":0,"pending":0,"in_progress":0,"completed":1}})"}};
  for (auto const& [label, json] : invalid)
  {
    auto result = ava::agent::parse_todowrite_result_text(json);
    ava::tests::expect_all("strict todowrite result parser rejects " + label,
                           {{"rejected", !result}, {"invalid argument", !result && result.error().category() == ava::core::ErrorCategory::InvalidArgument}});
  }
  auto without_counts = ava::agent::parse_todowrite_result_text(prefix + "}");
  auto with_counts = ava::agent::parse_todowrite_result_text(prefix + R"(,"counts":{"total":0,"pending":0,"in_progress":0,"completed":0}})");
  ava::tests::expect_all("strict todowrite result parser accepts valid snapshots with optional counts",
                         {{"without counts", without_counts.has_value()}, {"with matching counts", with_counts.has_value()}});
}

}  // namespace

void run_agent_todo_tests()
{
  test_todowrite_registry_and_success_snapshot();
  test_todowrite_validation_bounds();
  test_todowrite_session_projection();
  test_todowrite_result_parser_rejects_invalid_snapshots();
}
