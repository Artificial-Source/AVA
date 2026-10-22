#include "sys.h"
#include "tests/agent_loop_test_declarations.h"
#include "tests/support/agent_loop_test_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "ava/agent/agent_loop.h"
#include "ava/session/assistant_output.h"
#include "ava/session/session_store.h"
#include "ava/session/validation.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/json.h"
#include "ava/core/result.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using agent_loop_test::sse_response;
using agent_loop_test::tool_call_sse;

namespace {
class GatedReadAccess final : public ava::tools::ExactFileAccess
{
 public:
  explicit GatedReadAccess(std::size_t count) : released_(count, false), delegate_(std::make_shared<agent_loop_test::ChildSchedulingWitness>()) { }
  bool supports_read_text_file() const noexcept override { return true; }
  bool supports_write_text_file() const noexcept override { return false; }
  ava::core::Result<std::string> read_text_file(std::filesystem::path const& path, ava::tools::ToolIoCancelCallback cancel) const override
  {
    auto const index = static_cast<std::size_t>(std::stoul(path.stem().string()));
    {
      std::unique_lock lock(mutex_);
      ++active_;
      ++started_;
      max_active_ = std::max(max_active_, active_);
      changed_.notify_all();
      changed_.wait(lock, [&] { return release_all_ || released_.at(index); });
    }
    auto result = delegate_.read_text_file(path, std::move(cancel));
    {
      std::lock_guard lock(mutex_);
      --active_;
      changed_.notify_all();
    }
    return result;
  }
  ava::core::VoidResult write_text_file(std::filesystem::path const&, std::string_view, ava::tools::ToolIoCancelCallback) const override
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "worker-cap fixture is read-only"));
  }
  bool wait_started(std::size_t count) const
  {
    std::unique_lock lock(mutex_);
    return changed_.wait_until(lock, ava::tests::now_plus_seconds(5), [&] { return started_ >= count; });
  }
  void release(std::size_t index)
  {
    std::lock_guard lock(mutex_);
    released_.at(index) = true;
    changed_.notify_all();
  }
  void release_all()
  {
    std::lock_guard lock(mutex_);
    release_all_ = true;
    changed_.notify_all();
  }
  std::size_t max_active() const
  {
    std::lock_guard lock(mutex_);
    return max_active_;
  }
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::vector<bool> released_;
  bool release_all_ = false;
  mutable std::size_t started_ = 0;
  mutable std::size_t active_ = 0;
  mutable std::size_t max_active_ = 0;
  agent_loop_test::ChildSchedulingExactFileAccess delegate_;
};

struct ReadReleaseGuard
{
  std::shared_ptr<GatedReadAccess> access;
  ~ReadReleaseGuard() { access->release_all(); }
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
}  // namespace

void test_agent_loop_parallel_read_search_enforces_worker_caps()
{
  for (std::size_t configured : {0U, 2U})
  {
    auto const expected_cap = std::max<std::size_t>(1, configured);
    auto const label = "AgentLoop worker cap " + std::to_string(configured);
    agent_loop_test::AgentLoopFixture fixture("agent-worker-cap");
    auto access = std::make_shared<GatedReadAccess>(5);
    std::string calls;
    for (std::size_t index = 0; index < 5; ++index)
    {
      auto const file = std::to_string(index) + ".txt";
      ava::tests::write_test_file(fixture.workspace / file, "read-effect-" + std::to_string(index));
      calls += tool_call_sse("cap_" + std::to_string(index), "read_file", "{\"path\":\"" + file + "\"}");
    }
    ava::tests::FakeTransport transport(
        {sse_response(calls + "data: [DONE]\n\n"), sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\ndata: [DONE]\n\n")});
    auto options = fixture.options();
    options.parallel_read_search_tools = true;
    options.parallel_read_search_max_workers = configured;
    options.tool_execution.exact_file_access = access;
    ava::agent::AgentLoop loop(std::move(options));
    auto running = std::async(std::launch::async, [&] { return loop.run_turn("read all", fixture.store, fixture.provider, transport); });
    ReadReleaseGuard release{access};  // Must release before the future can wait in its destructor.
    bool const saturated = access->wait_started(expected_cap);
    expect(saturated, label + ": fills the configured number of blocked read slots");
    if (saturated)
    {
      ava::tests::expect_eq(access->max_active(), expected_cap, label + ": initial blocked batch respects the cap");
      for (std::size_t index = 0; index < 5 - expected_cap; ++index)
      {
        access->release(index);
        expect(access->wait_started(expected_cap + index + 1), label + ": starts a replacement only after a read is released");
      }
    }
    access->release_all();
    auto result = running.get();
    ava::tests::expect_eq(access->max_active(), expected_cap, label + ": maximum observed active reads equals the effective cap");
    if (!ava::tests::require(result && result->tool_timeline.size() == 5, label + ": all five reads complete"))
      continue;
    for (std::size_t index = 0; index < result->tool_timeline.size(); ++index)
      ava::tests::expect_all(label + ": read " + std::to_string(index),
                             {{"successful read", result->tool_timeline[index].status == ava::agent::ToolTimelineStatus::Success},
                              {"provider-order result", result->tool_timeline[index].call_id == "cap_" + std::to_string(index)}});
  }
}

void test_agent_loop_multiple_tools_and_denied_continuation()
{
  auto const root = create_empty_root("agent-multi-tools");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  {
    std::ofstream one(workspace / "one.txt", std::ios::binary | std::ios::trunc);
    one << "one";
    std::ofstream two(workspace / "two.txt", std::ios::binary | std::ios::trunc);
    two << "two";
  }
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "multi"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_1\",\"name\":\"read_file\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_1\",\"delta\":\"{\\\"path\\\":"
                                                    "\\\"one.txt\\\"}\"}\n\n"
                                                    "data: {\"type\":\"response.function_call.done\",\"call_id\":\"call_1\"}\n\n"
                                                    "data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_2\",\"name\":\"read_file\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_2\",\"delta\":\"{\\\"path\\\":"
                                                    "\\\"two.txt\\\"}\"}\n\n"
                                                    "data: {\"type\":\"response.function_call.done\",\"call_id\":\"call_2\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\n"
                                                    "data: [DONE]\n\n")});
  std::vector<ava::agent::ToolTimelineEntry> tool_events;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .on_tool_event = [&tool_events](auto const& entry) { tool_events.push_back(entry); },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("read both", store, provider, transport);
  expect(result && result->tool_calls == 2 && result->final_text == "done", "agent loop handles multiple tool calls before continuation");
  expect(result && result->tool_timeline.size() == 2 && result->tool_timeline[0].call_id == "call_1" && result->tool_timeline[0].name == "read_file" &&
             result->tool_timeline[0].status == ava::agent::ToolTimelineStatus::Success &&
             result->tool_timeline[0].argument_summary.find("path=one.txt") != std::string::npos && result->tool_timeline[0].output_lines &&
             *result->tool_timeline[0].output_lines == 1 &&
             result->tool_timeline[0].structured_result_json.find("\"call_id\":\"call_1\"") != std::string::npos &&
             result->tool_timeline[1].call_id == "call_2" && result->tool_timeline[1].name == "read_file" &&
             result->tool_timeline[1].status == ava::agent::ToolTimelineStatus::Success &&
             result->tool_timeline[1].argument_summary.find("path=two.txt") != std::string::npos && result->tool_timeline[1].output_lines &&
             *result->tool_timeline[1].output_lines == 1 && result->tool_timeline[1].structured_result_json.find("\"call_id\":\"call_2\"") != std::string::npos,
         "agent loop preserves provider-order timeline metadata for multiple tool calls");
  expect(tool_events.size() == 4 && tool_events[0].call_id == "call_1" && tool_events[0].status == ava::agent::ToolTimelineStatus::Running &&
             tool_events[1].call_id == "call_1" && tool_events[1].status == ava::agent::ToolTimelineStatus::Success && tool_events[2].call_id == "call_2" &&
             tool_events[2].status == ava::agent::ToolTimelineStatus::Running && tool_events[3].call_id == "call_2" &&
             tool_events[3].status == ava::agent::ToolTimelineStatus::Success,
         "agent loop publishes running and completed tool events in provider order");

  auto entries = store.load();
  auto const projection = entries ? ava::session::classify_assistant_output(*entries) : ava::session::AssistantOutputProjection{};
  auto const committed_call_ids = entries ? agent_loop_test::committed_function_call_ids(*entries) : std::vector<std::string>{};
  std::vector<std::string> bound_result_ids;
  std::string final_text;
  for (auto const& turn : projection.turns)
  {
    for (auto const& item : turn.items)
    {
      if (auto const* text = std::get_if<ava::session::AssistantOutputText>(&item.item.payload))
        final_text = text->text;
    }
  }
  if (entries)
    for (auto const& entry : *entries)
      if (entry.type == ava::session::EntryType::ToolResult && ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id"))
        bound_result_ids.push_back(ava::core::json::string_field(entry.data_json, "call_id").value_or(""));
  expect(entries && projection.turns.size() == 2 && committed_call_ids == std::vector<std::string>({"call_1", "call_2"}) &&
             bound_result_ids == std::vector<std::string>({"call_1", "call_2"}) && final_text == "done",
         "agent loop commits ordered multi-tool functions, binds results, and stores the continuation as v4 turns");

  auto const denied_root = create_empty_root("agent-denied-continuation");
  auto const denied_workspace = denied_root / "workspace";
  std::filesystem::create_directories(denied_workspace);
  ava::session::SessionStore denied_store(
      ava::session::SessionStoreOptions{.root_dir = denied_root / "sessions", .workspace_dir = denied_workspace, .session_id = "denied"});
  ava::tests::FakeTransport denied_transport(
      {sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_write\",\"name\":\"write_file\"}\n\n"
                    "data: "
                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_write\",\"delta\":\"{\\\"path\\\":"
                    "\\\"src/new.cpp\\\",\\\"content\\\":\\\"bad\\\"}\"}\n\n"
                    "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"permission explained\"}\n\n"
                    "data: [DONE]\n\n")});
  ava::agent::AgentLoop denied_loop(ava::agent::AgentLoopOptions{
      .workspace_dir = denied_workspace,
      .mode = ava::agent::Mode::Plan,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .openai_oauth = true,
      .openai_account_id = "acct_123",
      .append_entry = append_route_for_test(denied_store),
      .append_batch = append_batch_route_for_test(denied_store),
      .session_read_authority = read_authority_for_test(denied_store),
  });
  auto denied_result = denied_loop.run_turn("write source", denied_store, provider, denied_transport);
  expect(denied_result && denied_result->final_text == "permission explained" && denied_result->provider_iterations == 2,
         "agent loop continues after permission-denied tool results");
  expect(denied_result && denied_result->tool_timeline.size() == 1 && denied_result->tool_timeline.front().status == ava::agent::ToolTimelineStatus::Error &&
             denied_result->tool_timeline.front().argument_summary.find("content=3 bytes") != std::string::npos &&
             denied_result->tool_timeline.front().argument_summary.find("bad") == std::string::npos &&
             denied_result->tool_timeline.front().result_summary.find("error:") == 0,
         "agent loop marks denied tool results as safe error timeline entries");
  expect(denied_transport.requests().size() == 2 && denied_transport.requests()[1].body.find("permission_denied") != std::string::npos,
         "permission-denied tool result is framed into continuation context");
}

void test_agent_loop_parallel_read_search_preserves_provider_order_and_replay()
{
  auto const root = create_empty_root("agent-parallel-read-search");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace / "a");
  std::filesystem::create_directories(workspace / "b");
  for (int index = 0; index < 500; ++index)
  {
    std::ofstream a(workspace / "a" / ("file_" + std::to_string(index) + ".txt"), std::ios::binary | std::ios::trunc);
    a << "alpha " << index;
    std::ofstream b(workspace / "b" / ("file_" + std::to_string(index) + ".txt"), std::ios::binary | std::ios::trunc);
    b << "bravo " << index;
  }

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "parallel-order"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(tool_call_sse("glob_a", "glob", R"({"pattern":"a/*.txt"})") +
                                                    tool_call_sse("glob_b", "glob", R"({"pattern":"b/*.txt"})") + "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\n"
                                                    "data: [DONE]\n\n")});
  std::vector<ava::agent::ToolTimelineEntry> tool_events;
  std::vector<ava::agent::ToolProgressEntry> progress_events;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{.workspace_dir = workspace,
                                                          .mode = ava::agent::Mode::Build,
                                                          .model = ava::agent::ModelInvocationOptions{.provider_id = "openai",
                                                                                                      .model_id = "gpt-5.5",
                                                                                                      .system_prompt = "system prompt",
                                                                                                      .api_family = "openai_responses",
                                                                                                      .reasoning_format = "openai_responses"},
                                                          .access_token = "token",
                                                          .on_tool_event = [&tool_events](auto const& entry) { tool_events.push_back(entry); },
                                                          .on_tool_progress = [&progress_events](auto const& entry) -> ava::core::VoidResult {
                                                            progress_events.push_back(entry);
                                                            return {};
                                                          },
                                                          .append_entry = append_route_for_test(store),
                                                          .append_batch = append_batch_route_for_test(store),
                                                          .session_read_authority = read_authority_for_test(store),
                                                          .parallel_read_search_tools = true,
                                                          .parallel_read_search_max_workers = 2});
  auto result = loop.run_turn("glob both", store, provider, transport);
  expect(result && result->final_text == "done" && result->tool_calls == 2 && result->provider_iterations == 2,
         "parallel read/search opt-in completes provider continuation");
  expect(result && result->tool_timeline.size() == 2 && result->tool_timeline[0].call_id == "glob_a" &&
             result->tool_timeline[0].status == ava::agent::ToolTimelineStatus::Success && result->tool_timeline[0].total_matches &&
             *result->tool_timeline[0].total_matches == 500 && result->tool_timeline[1].call_id == "glob_b" &&
             result->tool_timeline[1].status == ava::agent::ToolTimelineStatus::Success && result->tool_timeline[1].total_matches &&
             *result->tool_timeline[1].total_matches == 500,
         "parallel read/search timeline remains in provider order");
  expect(tool_events.size() == 4 && tool_events[0].call_id == "glob_a" && tool_events[0].status == ava::agent::ToolTimelineStatus::Running &&
             tool_events[1].call_id == "glob_a" && tool_events[1].status == ava::agent::ToolTimelineStatus::Success && tool_events[2].call_id == "glob_b" &&
             tool_events[2].status == ava::agent::ToolTimelineStatus::Running && tool_events[3].call_id == "glob_b" &&
             tool_events[3].status == ava::agent::ToolTimelineStatus::Success,
         "parallel read/search publishes running and final tool events in provider order");

  std::size_t glob_a_progress = 0;
  std::size_t glob_b_progress = 0;
  bool saw_glob_b_progress = false;
  bool glob_a_after_glob_b = false;
  for (auto const& event : progress_events)
  {
    if (event.call_id == "glob_b")
      saw_glob_b_progress = true;
    if (event.call_id == "glob_a")
    {
      ++glob_a_progress;
      glob_a_after_glob_b = glob_a_after_glob_b || saw_glob_b_progress;
    }
    if (event.call_id == "glob_b")
      ++glob_b_progress;
  }
  expect(glob_a_progress >= 2 && glob_b_progress >= 2 && !glob_a_after_glob_b, "parallel read/search buffers progress and publishes it by provider slot");

  auto entries = store.load();
  auto audits = entries ? permission_entries(*entries) : std::vector<ava::session::SessionEntry>{};
  auto const audit_id_0 = audits.size() >= 1 ? ava::core::json::string_field(audits[0].data_json, "permission_request_id").value_or("") : "";
  auto const audit_id_1 = audits.size() >= 2 ? ava::core::json::string_field(audits[1].data_json, "permission_request_id").value_or("") : "";
  expect(audits.size() == 2 && audit_id_0.starts_with("permreq_") && audit_id_1.starts_with("permreq_") && audit_id_0 != audit_id_1 &&
             ava::core::json::string_field(audits[0].data_json, "operation") == "search" &&
             ava::core::json::string_field(audits[1].data_json, "operation") == "search",
         "parallel read/search commits unique buffered permission audit entries in provider order");

  auto const projection = entries ? ava::session::classify_assistant_output(*entries) : ava::session::AssistantOutputProjection{};
  auto const committed_calls = entries ? agent_loop_test::committed_function_call_ids(*entries) : std::vector<std::string>{};
  std::vector<std::string> bound_results;
  if (entries)
    for (auto const& entry : *entries)
      if (entry.type == ava::session::EntryType::ToolResult && ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id"))
        bound_results.push_back(ava::core::json::string_field(entry.data_json, "call_id").value_or(""));
  expect(entries && projection.turns.size() == 2 && committed_calls == std::vector<std::string>({"glob_a", "glob_b"}) &&
             bound_results == std::vector<std::string>({"glob_a", "glob_b"}),
         "parallel read/search commits provider-ordered v4 functions and exact bound results");
  auto const continuation = transport.requests().size() >= 2 ? transport.requests()[1].body : std::string{};
  auto const continuation_a = continuation.find("glob_a");
  auto const continuation_b = continuation.find("glob_b");
  expect(transport.requests().size() == 2 && continuation_a != std::string::npos && continuation_b != std::string::npos && continuation_a < continuation_b,
         "parallel read/search continuation replay keeps provider-order tool results");
}

void test_agent_loop_parallel_read_search_zero_max_workers_completes_turn()
{
  auto const root = create_empty_root("agent-parallel-zero-workers");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  {
    std::ofstream one(workspace / "one.txt", std::ios::binary | std::ios::trunc);
    one << "one";
    std::ofstream two(workspace / "two.txt", std::ios::binary | std::ios::trunc);
    two << "two";
  }

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "parallel-zero-workers"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(tool_call_sse("call_1", "read_file", R"({"path":"one.txt"})") +
                                                    tool_call_sse("call_2", "read_file", R"({"path":"two.txt"})") + "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\n"
                                                    "data: [DONE]\n\n")});
  std::vector<ava::agent::ToolTimelineEntry> tool_events;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{.workspace_dir = workspace,
                                                          .mode = ava::agent::Mode::Build,
                                                          .model = agent_loop_test::model_invocation_options(),
                                                          .access_token = "token",
                                                          .on_tool_event = [&tool_events](auto const& entry) { tool_events.push_back(entry); },
                                                          .append_entry = append_route_for_test(store),
                                                          .append_batch = append_batch_route_for_test(store),
                                                          .session_read_authority = read_authority_for_test(store),
                                                          .parallel_read_search_tools = true,
                                                          .parallel_read_search_max_workers = 0});
  auto result = loop.run_turn("read both with zero worker cap", store, provider, transport);
  expect(result && result->final_text == "done" && result->tool_calls == 2 && result->provider_iterations == 2,
         "parallel read/search accepts a zero worker setting and completes the turn");
  expect(tool_events.size() == 4 && tool_events[0].call_id == "call_1" && tool_events[1].call_id == "call_1" &&
             tool_events[1].status == ava::agent::ToolTimelineStatus::Success && tool_events[2].call_id == "call_2" && tool_events[3].call_id == "call_2" &&
             tool_events[3].status == ava::agent::ToolTimelineStatus::Success,
         "a zero worker setting preserves provider-order tool event commits");
}

void test_agent_loop_parallel_read_search_falls_back_for_ask_preflight()
{
  auto const root = create_empty_root("agent-parallel-ask-fallback");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const outside_path = root / "outside.txt";
  {
    std::ofstream outside(outside_path, std::ios::binary | std::ios::trunc);
    outside << "outside content";
    std::ofstream inside(workspace / "inside.txt", std::ios::binary | std::ios::trunc);
    inside << "inside content";
  }

  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "ask-fallback"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(tool_call_sse("outside_read", "read_file", "{\"path\":\"" + ava::core::json::escape(outside_path.generic_string()) + "\"}") +
                    tool_call_sse("inside_read", "read_file", R"({"path":"inside.txt"})") + "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\n"
                    "data: [DONE]\n\n")});
  auto const main_thread = std::this_thread::get_id();
  struct ResolverObservation
  {
    std::filesystem::path target = {};
    std::thread::id thread = {};
    bool history_loaded = false;
    bool function_committed = false;
  };
  std::mutex resolver_mutex;
  std::vector<ResolverObservation> resolver_observations;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .anchor_set = command_anchors_for_test(workspace, store.session_path().parent_path() / "spill"),
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [&](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        auto entries = store.load();
        auto const ids = entries ? agent_loop_test::committed_function_call_ids(*entries) : std::vector<std::string>{};
        std::lock_guard lock(resolver_mutex);
        resolver_observations.push_back(ResolverObservation{.target = prompt.target_path,
                                                            .thread = std::this_thread::get_id(),
                                                            .history_loaded = entries.has_value(),
                                                            .function_committed = std::ranges::find(ids, "outside_read") != ids.end()});
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
      .parallel_read_search_tools = true,
      .parallel_read_search_max_workers = 2});
  auto result = loop.run_turn("read outside and inside", store, provider, transport);
  std::vector<ResolverObservation> observations;
  {
    std::lock_guard lock(resolver_mutex);
    observations = resolver_observations;
  }
  if (!ava::tests::require(observations.size() == 1, "Ask fallback invokes exactly one resolver"))
    return;
  ava::tests::expect_all("Ask fallback stays on the run thread after committing the exact function",
                         {{"turn completes", result && result->final_text == "done"},
                          {"run-thread resolver", observations.front().thread == main_thread},
                          {"outside target", observations.front().target == outside_path},
                          {"history loaded in resolver", observations.front().history_loaded},
                          {"function committed before resolver", observations.front().function_committed}});

  auto entries = store.load();
  auto const functions = entries ? agent_loop_test::committed_function_call_ids(*entries) : std::vector<std::string>{};
  std::vector<std::string> bound_results;
  if (entries)
    for (auto const& entry : *entries)
      if (entry.type == ava::session::EntryType::ToolResult && ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id"))
        bound_results.push_back(ava::core::json::string_field(entry.data_json, "call_id").value_or(""));
  expect(entries && functions == std::vector<std::string>({"outside_read", "inside_read"}) &&
             bound_results == std::vector<std::string>({"outside_read", "inside_read"}),
         "Ask fallback and later parallel-ready read retain committed function and result order");
}

void test_agent_loop_parallel_read_search_active_cancellation_stops_unstarted_slots()
{
  auto const root = create_empty_root("agent-parallel-active-cancel");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  for (int index = 1; index <= 4; ++index)
  {
    std::ofstream file(workspace / ("file_" + std::to_string(index) + ".txt"), std::ios::binary | std::ios::trunc);
    file << "file " << index;
  }

  struct ActiveCancelState
  {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread::id main_thread;
    std::vector<std::thread::id> worker_threads;
    bool cancel_requested = false;
    bool timed_out = false;

    bool operator()()
    {
      auto const thread_id = std::this_thread::get_id();
      std::unique_lock lock(mutex);
      if (thread_id == main_thread)
      {
        return cancel_requested;
      }

      if (std::ranges::find(worker_threads, thread_id) == worker_threads.end())
      {
        worker_threads.push_back(thread_id);
      }
      if (worker_threads.size() >= 2)
      {
        cancel_requested = true;
        changed.notify_all();
      }
      if (!changed.wait_for(lock, std::chrono::seconds(5), [&] { return cancel_requested || worker_threads.size() >= 2; }))
      {
        timed_out = true;
        cancel_requested = true;
        changed.notify_all();
      }
      return cancel_requested;
    }
  } cancel_state;
  cancel_state.main_thread = std::this_thread::get_id();

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "parallel-active-cancel"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(tool_call_sse("call_1", "read_file", R"({"path":"file_1.txt"})") + tool_call_sse("call_2", "read_file", R"({"path":"file_2.txt"})") +
                    tool_call_sse("call_3", "read_file", R"({"path":"file_3.txt"})") + tool_call_sse("call_4", "read_file", R"({"path":"file_4.txt"})") +
                    tool_call_sse("call_bash", "bash", R"({"command":"true"})") + "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"should not continue\"}\n\n"
                    "data: [DONE]\n\n")});
  std::atomic<int> resolver_calls = 0;
  std::vector<ava::agent::ToolTimelineEntry> tool_events;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .on_tool_event = [&tool_events](auto const& entry) { tool_events.push_back(entry); },
      .permission_resolver = [&resolver_calls](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++resolver_calls;
        return ava::permissions::PermissionResolution::Allow;
      },
      .cancel_requested = [&cancel_state] { return cancel_state(); },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
      .parallel_read_search_tools = true,
      .parallel_read_search_max_workers = 2});
  auto result = loop.run_turn("read four then cancel during active epoch", store, provider, transport);

  std::vector<std::thread::id> worker_threads;
  bool cancel_timed_out = false;
  {
    std::lock_guard lock(cancel_state.mutex);
    worker_threads = cancel_state.worker_threads;
    cancel_timed_out = cancel_state.timed_out;
  }
  expect(!cancel_timed_out, "active parallel cancellation test releases workers through condition variables without timing out");
  expect(worker_threads.size() == 2, "active parallel cancellation stops unstarted later read/search workers after the capped active batch");
  expect(!result && result.error().code() == ava::core::ErrorCode::Canceled,
         "active parallel read/search cancellation returns the agent-loop cancellation error");
  expect(resolver_calls.load() == 0, "active parallel cancellation does not call live permission resolvers from workers");
  expect(tool_events.size() == 4 && tool_events[0].call_id == "call_1" && tool_events[0].status == ava::agent::ToolTimelineStatus::Running &&
             tool_events[1].call_id == "call_1" && tool_events[1].status == ava::agent::ToolTimelineStatus::Canceled && tool_events[2].call_id == "call_2" &&
             tool_events[2].status == ava::agent::ToolTimelineStatus::Running && tool_events[3].call_id == "call_2" &&
             tool_events[3].status == ava::agent::ToolTimelineStatus::Canceled,
         "active parallel cancellation commits launched canceled slots in provider order only");

  auto entries = store.load();
  auto const committed_calls = entries ? agent_loop_test::committed_function_call_ids(*entries) : std::vector<std::string>{};
  std::vector<std::string> bound_results;
  bool saw_cancel_boundary = false;
  if (entries)
    for (auto const& entry : *entries)
    {
      saw_cancel_boundary = saw_cancel_boundary || entry.type == ava::session::EntryType::Cancel;
      if (entry.type == ava::session::EntryType::ToolResult && ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id"))
        bound_results.push_back(ava::core::json::string_field(entry.data_json, "call_id").value_or(""));
    }
  auto validation = entries ? ava::session::validate_session_replay(*entries) : ava::session::SessionReplayValidation{};
  expect(entries && committed_calls == std::vector<std::string>({"call_1", "call_2", "call_3", "call_4", "call_bash"}) &&
             bound_results == std::vector<std::string>({"call_1", "call_2", "call_3", "call_4", "call_bash"}) && saw_cancel_boundary &&
             transport.requests().size() == 1 && validation.ok(),
         "active parallel cancellation closes every committed binding without dispatching unlaunched slots");
}

void test_agent_loop_parallel_read_search_cancellation_stops_later_barrier()
{
  auto const root = create_empty_root("agent-parallel-cancel");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  {
    std::ofstream one(workspace / "one.txt", std::ios::binary | std::ios::trunc);
    one << "one";
    std::ofstream two(workspace / "two.txt", std::ios::binary | std::ios::trunc);
    two << "two";
  }

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "parallel-cancel"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(tool_call_sse("call_1", "read_file", R"({"path":"one.txt"})") + tool_call_sse("call_2", "read_file", R"({"path":"two.txt"})") +
                    tool_call_sse("call_bash", "bash", R"({"command":"true"})") + "data: [DONE]\n\n")});
  bool cancel_after_first_tool = false;
  std::vector<ava::agent::ToolTimelineEntry> tool_events;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{.workspace_dir = workspace,
                                                          .mode = ava::agent::Mode::Build,
                                                          .model = agent_loop_test::model_invocation_options(),
                                                          .access_token = "token",
                                                          .on_tool_event =
                                                              [&](ava::agent::ToolTimelineEntry const& entry) {
                                                                tool_events.push_back(entry);
                                                                if (entry.call_id == "call_1" && entry.status == ava::agent::ToolTimelineStatus::Success)
                                                                {
                                                                  cancel_after_first_tool = true;
                                                                }
                                                              },
                                                          .cancel_requested = [&cancel_after_first_tool] { return cancel_after_first_tool; },
                                                          .append_entry = append_route_for_test(store),
                                                          .append_batch = append_batch_route_for_test(store),
                                                          .session_read_authority = read_authority_for_test(store),
                                                          .parallel_read_search_tools = true,
                                                          .parallel_read_search_max_workers = 2});
  auto result = loop.run_turn("read both then cancel before bash", store, provider, transport);
  expect(!result && result.error().code() == ava::core::ErrorCode::Canceled, "parallel read/search reports cancellation after committing the completed epoch");
  expect(tool_events.size() == 4 && tool_events[0].call_id == "call_1" && tool_events[0].status == ava::agent::ToolTimelineStatus::Running &&
             tool_events[1].call_id == "call_1" && tool_events[1].status == ava::agent::ToolTimelineStatus::Success && tool_events[2].call_id == "call_2" &&
             tool_events[2].status == ava::agent::ToolTimelineStatus::Running && tool_events[3].call_id == "call_2" &&
             tool_events[3].status == ava::agent::ToolTimelineStatus::Success,
         "parallel cancellation commits completed read/search outcomes in provider order and does not publish later barrier events");

  auto entries = store.load();
  auto const committed_calls = entries ? agent_loop_test::committed_function_call_ids(*entries) : std::vector<std::string>{};
  std::vector<std::string> bound_results;
  if (entries)
    for (auto const& entry : *entries)
      if (entry.type == ava::session::EntryType::ToolResult && ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id"))
        bound_results.push_back(ava::core::json::string_field(entry.data_json, "call_id").value_or(""));
  auto validation = entries ? ava::session::validate_session_replay(*entries) : ava::session::SessionReplayValidation{};
  expect(entries && committed_calls == std::vector<std::string>({"call_1", "call_2", "call_bash"}) &&
             bound_results == std::vector<std::string>({"call_1", "call_2", "call_bash"}) && transport.requests().size() == 1 && validation.ok(),
         "parallel cancellation retains the committed tool turn, closes its later barrier, and never executes it");
}

void test_agent_loop_cancellation_stops_later_sequential_tools()
{
  auto const root = create_empty_root("agent-multi-tools-cancel");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  {
    std::ofstream one(workspace / "one.txt", std::ios::binary | std::ios::trunc);
    one << "one";
    std::ofstream two(workspace / "two.txt", std::ios::binary | std::ios::trunc);
    two << "two";
  }
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "multi-cancel"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_1\",\"name\":\"read_file\"}\n\n"
                    "data: "
                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_1\",\"delta\":\"{\\\"path\\\":"
                    "\\\"one.txt\\\"}\"}\n\n"
                    "data: {\"type\":\"response.function_call.done\",\"call_id\":\"call_1\"}\n\n"
                    "data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_2\",\"name\":\"read_file\"}\n\n"
                    "data: "
                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_2\",\"delta\":\"{\\\"path\\\":"
                    "\\\"two.txt\\\"}\"}\n\n"
                    "data: {\"type\":\"response.function_call.done\",\"call_id\":\"call_2\"}\n\n"
                    "data: [DONE]\n\n")});
  bool cancel_after_first_tool = false;
  std::vector<ava::agent::ToolTimelineEntry> tool_events;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .on_tool_event =
          [&](ava::agent::ToolTimelineEntry const& entry) {
            tool_events.push_back(entry);
            if (entry.call_id == "call_1" && entry.status == ava::agent::ToolTimelineStatus::Success)
            {
              cancel_after_first_tool = true;
            }
          },
      .cancel_requested = [&cancel_after_first_tool] { return cancel_after_first_tool; },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("read both but cancel after first", store, provider, transport);
  expect(!result && result.error().code() == ava::core::ErrorCode::Canceled, "agent loop reports typed cancellation after the first sequential tool dispatch");
  expect(tool_events.size() == 2 && tool_events[0].call_id == "call_1" && tool_events[0].status == ava::agent::ToolTimelineStatus::Running &&
             tool_events[1].call_id == "call_1" && tool_events[1].status == ava::agent::ToolTimelineStatus::Success,
         "agent loop does not publish events for later provider tool calls after cancellation");

  auto entries = store.load();
  auto const committed_calls = entries ? agent_loop_test::committed_function_call_ids(*entries) : std::vector<std::string>{};
  std::vector<std::string> bound_results;
  if (entries)
    for (auto const& entry : *entries)
      if (entry.type == ava::session::EntryType::ToolResult && ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id"))
        bound_results.push_back(ava::core::json::string_field(entry.data_json, "call_id").value_or(""));
  auto validation = entries ? ava::session::validate_session_replay(*entries) : ava::session::SessionReplayValidation{};
  expect(entries && committed_calls == std::vector<std::string>({"call_1", "call_2"}) && bound_results == std::vector<std::string>({"call_1", "call_2"}) &&
             validation.ok(),
         "sequential cancellation retains the committed turn, stops dispatch, and closes its later binding");
}

void test_agent_loop_tool_delta_dedupes_and_rejects_empty_tool_ids()
{
  ava::provider::OpenAIProvider const provider("https://api.example.test");

  {
    auto const root = create_empty_root("agent-delta-before-start");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    {
      std::ofstream file(workspace / "note.txt", std::ios::binary | std::ios::trunc);
      file << "dedupe content";
    }
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "delta-before-start"});
    ava::tests::FakeTransport transport({sse_response("data: "
                                                      "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_1\",\"delta\":\"{\\\"path\\\":"
                                                      "\\\"note.txt\\\"}\"}\n\n"
                                                      "data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_1\",\"name\":\"read_file\"}\n\n"
                                                      "data: {\"type\":\"response.function_call.done\",\"call_id\":\"call_1\"}\n\n"
                                                      "data: [DONE]\n\n"),
                                         sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\n"
                                                      "data: [DONE]\n\n")});
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("read note", store, provider, transport);
    expect(result && result->tool_calls == 1 && result->tool_timeline.size() == 1 &&
               result->tool_timeline.front().status == ava::agent::ToolTimelineStatus::Success && result->tool_timeline.front().name == "read_file",
           "agent loop deduplicates tool deltas that arrive before tool start events");

    auto entries = store.load();
    auto const functions = entries ? agent_loop_test::committed_function_call_ids(*entries).size() : 0;
    std::size_t bound_results = 0;
    if (entries)
      for (auto const& entry : *entries)
        bound_results += entry.type == ava::session::EntryType::ToolResult && ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id");
    expect(entries && functions == 1 && bound_results == 1, "same-iteration start/delta/end fragments merge into one committed v4 function and bound result");
  }

  {
    auto const root = create_empty_root("agent-cross-iteration-duplicate-call-id");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    {
      std::ofstream file(workspace / "note.txt", std::ios::binary | std::ios::trunc);
      file << "duplicate id content";
    }
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "duplicate-call-id"});
    auto const repeated_call = sse_response(
        "data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_reused\",\"name\":\"read_file\"}\n\n"
        "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_reused\",\"delta\":\"{\\\"path\\\":\\\"note.txt\\\"}\"}\n\n"
        "data: {\"type\":\"response.function_call.done\",\"call_id\":\"call_reused\"}\n\n"
        "data: [DONE]\n\n");
    ava::tests::FakeTransport transport({repeated_call, repeated_call});
    std::vector<ava::agent::ToolTimelineEntry> tool_events;
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .on_tool_event = [&tool_events](auto const& event) { tool_events.push_back(event); },
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("reuse a call id", store, provider, transport);
    auto entries = store.load();
    auto const projection = entries ? ava::session::classify_assistant_output(*entries) : ava::session::AssistantOutputProjection{};
    std::size_t functions = 0;
    std::size_t bound_results = 0;
    for (auto const& turn : projection.turns)
      for (auto const& item : turn.items) functions += std::holds_alternative<ava::session::AssistantOutputFunctionCall>(item.item.payload);
    if (entries)
      for (auto const& entry : *entries)
        bound_results += entry.type == ava::session::EntryType::ToolResult && ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id");
    auto validation = entries ? ava::session::validate_session_replay(*entries) : ava::session::SessionReplayValidation{};
    expect(!result && result.error().category() == ava::core::ErrorCategory::Provider && result.error().message().find("reused") != std::string::npos &&
               result.error().format().find("call_reused") != std::string::npos && functions == 1 && bound_results == 1 && tool_events.size() == 2 &&
               validation.ok(),
           "cross-iteration provider call-id reuse is rejected before a duplicate lifecycle, dispatch, or session record");
  }

  {
    auto const root = create_empty_root("agent-cross-prompt-duplicate-call-id");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    {
      std::ofstream file(workspace / "note.txt", std::ios::binary | std::ios::trunc);
      file << "persistent duplicate id content";
    }
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "cross-prompt-duplicate"});
    auto const tool_call = sse_response(
        "data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_persistent\",\"name\":\"read_file\"}\n\n"
        "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_persistent\",\"delta\":\"{\\\"path\\\":\\\"note.txt\\\"}\"}\n\n"
        "data: {\"type\":\"response.function_call.done\",\"call_id\":\"call_persistent\"}\n\n"
        "data: [DONE]\n\n");
    ava::tests::FakeTransport transport({tool_call,
                                         sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"first done\"}\n\n"
                                                      "data: [DONE]\n\n"),
                                         tool_call});
    std::vector<ava::provider::StreamEvent> stream_events;
    auto options = ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .on_stream_event = [&stream_events](auto const& event) -> ava::core::VoidResult {
          stream_events.push_back(event);
          return {};
        },
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    };
    ava::agent::AgentLoop first_loop(options);
    auto first = first_loop.run_turn("first prompt", store, provider, transport);
    auto const events_after_first = stream_events.size();
    ava::agent::AgentLoop second_loop(std::move(options));
    auto second = second_loop.run_turn("second prompt", store, provider, transport);
    auto entries = store.load();
    auto const functions = entries ? agent_loop_test::committed_function_call_ids(*entries).size() : 0;
    std::size_t bound_results = 0;
    if (entries)
      for (auto const& entry : *entries)
        bound_results += entry.type == ava::session::EntryType::ToolResult && ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id");
    auto validation = entries ? ava::session::validate_session_replay(*entries) : ava::session::SessionReplayValidation{};
    expect(first && !second && second.error().category() == ava::core::ErrorCategory::Provider &&
               second.error().message().find("persistent session") != std::string::npos &&
               second.error().format().find("call_persistent") != std::string::npos && functions == 1 && bound_results == 1 &&
               stream_events.size() == events_after_first && validation.ok(),
           "cross-prompt provider call-id reuse is rejected before duplicate updates, persistence, or dispatch");
  }

  {
    auto const root = create_empty_root("agent-empty-call-id");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "empty-call-id"});
    ava::tests::FakeTransport transport(
        {sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"\",\"name\":\"read_file\"}\n\n"
                      "data: [DONE]\n\n")});
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("read missing-id", store, provider, transport);
    auto entries = store.load();
    bool saw_tool_entry = false;
    if (entries)
    {
      for (auto const& entry : *entries)
      {
        saw_tool_entry = saw_tool_entry || entry.type == ava::session::EntryType::ToolCall || entry.type == ava::session::EntryType::ToolResult;
      }
    }
    expect(!result && result.error().message().find("empty") != std::string::npos && entries && !saw_tool_entry,
           "agent loop rejects empty provider tool call ids before session or timeline use");
  }
}
