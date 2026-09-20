#include "sys.h"
#include "tests/agent_loop_test_declarations.h"
#include "tests/support/agent_loop_test_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/app/command_jobs.h"
#include "ava/agent/agent_loop.h"
#include "ava/session/assistant_output.h"
#include "ava/session/session_store.h"
#include "ava/session/validation.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/json.h"
#include "ava/core/result.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>
#include <sys/stat.h>

using agent_loop_test::BlockingBackgroundTransport;
using agent_loop_test::SharedFakeTransport;
using agent_loop_test::sse_response;
using agent_loop_test::tool_call_sse;
using agent_loop_test::TraceCollector;

namespace {
std::vector<ava::agent::SubagentCoordinatorJobSnapshot> wait_for_published_running_job(ava::agent::SubagentCoordinator const& coordinator,
                                                                                       std::string const& parent_session_id, std::size_t expected_total_count)
{
  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (true)
  {
    auto jobs = coordinator.list(parent_session_id);
    // Transport request readiness does not guarantee coordinator publication.
    if (jobs.size() == expected_total_count &&
        std::ranges::any_of(jobs, [](auto const& snapshot) { return snapshot.job.execution == ava::agent::SubagentExecutionState::Running; }))
      return jobs;
    if (std::chrono::steady_clock::now() >= deadline)
      return jobs;
    std::this_thread::yield();
  }
}
} // namespace

void test_agent_loop_private_task_launch_follows_public_running_and_stays_private()
{
  auto const root = create_empty_root("agent-private-task-launch-order");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "parent-private-launch"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(tool_call_sse("call_private_launch", "task", R"({"description":"private launch","prompt":"return child","subagent_type":"general"})") +
                    "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"child done\"}\n\ndata: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent done\"}\n\ndata: [DONE]\n\n")});
  auto const display = ava::agent::SubagentLaunchDisplay::normalized("MODEL_SENTINEL_PRIVATE", std::string_view("LEVEL_SENTINEL"));
  std::vector<std::string> order;
  std::vector<ava::agent::SubagentLaunchNotification> launches;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .on_tool_event =
          [&order](ava::agent::ToolTimelineEntry const& entry) {
            if (entry.status == ava::agent::ToolTimelineStatus::Running)
              order.emplace_back("public_running");
          },
      .subagent_launch = {.display = display,
                          .request_id = "request-private",
                          .correlation_id = "correlation-private",
                          .sink =
                              [&order, &launches](ava::agent::SubagentLaunchNotification const& launch) {
                                order.emplace_back("private_launch");
                                launches.push_back(launch);
                              }},
      .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("delegate", store, provider, transport);
  auto entries = store.load();
  bool session_private = entries.has_value();
  if (entries)
    for (auto const& entry : *entries)
      session_private =
          session_private && entry.data_json.find("MODEL_SENTINEL_PRIVATE") == std::string::npos && entry.data_json.find("LEVEL_SENTINEL") == std::string::npos;
  bool timeline_private = result.has_value();
  if (result)
    for (auto const& entry : result->tool_timeline)
      timeline_private = timeline_private && entry.result_json.find("MODEL_SENTINEL_PRIVATE") == std::string::npos &&
                         entry.result_json.find("LEVEL_SENTINEL") == std::string::npos &&
                         entry.structured_result_json.find("MODEL_SENTINEL_PRIVATE") == std::string::npos;
  expect(result && result->final_text == "parent done" && order.size() >= 2 && order[0] == "public_running" && order[1] == "private_launch" &&
             launches.size() == 1 && launches.front().tool_call_id == "call_private_launch" && launches.front().request_id == "request-private" &&
             launches.front().correlation_id == "correlation-private" && launches.front().display == display,
         "ordinary public task Running publication precedes one exact private launch callback on the parent execution thread");
  expect(session_private && timeline_private,
         "private launch model/reasoning sentinels never enter task JSON/XML timeline results or persisted session serialization");
}

void test_agent_loop_task_subagent_runs_child_session()
{
  auto const root = create_empty_root("agent-task-subagent");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const session_root = root / "sessions";
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "parent"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_task\",\"name\":\"task\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_task\",\"delta\":\"{"
                                                    "\\\"description\\\":\\\"Check docs\\\",\\\"prompt\\\":\\\"Return child result only.\\\","
                                                    "\\\"subagent_type\\\":\\\"general\\\",\\\"task_id\\\":\\\"\\\",\\\"command\\\":\\\"\\\","
                                                    "\\\"background\\\":false}\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"child result\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent saw task\"}\n\n"
                                                    "data: [DONE]\n\n")});
  int resolver_prompts = 0;
  auto trace_collector = std::make_shared<TraceCollector>();
  auto observation = std::make_shared<ava::observability::RunObservation>(trace_collector);
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{.workspace_dir = workspace,
                                                          .mode = ava::agent::Mode::Build,
                                                          .model = agent_loop_test::model_invocation_options(),
                                                          .access_token = "token",
                                                          .permission_resolver = [&resolver_prompts](ava::permissions::PermissionPrompt const&)
                                                              -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
                                                            ++resolver_prompts;
                                                            return ava::permissions::PermissionResolution::Allow;
                                                          },
                                                          .append_entry = append_route_for_test(store),
                                                          .append_batch = append_batch_route_for_test(store),
                                                          .session_read_authority = read_authority_for_test(store),
                                                          .observation = observation});

  auto result = loop.run_turn("delegate", store, provider, transport);
  expect(result && result->final_text == "parent saw task" && result->tool_calls == 1 && result->provider_iterations == 2 && resolver_prompts == 0,
         "agent loop launches foreground task subagents without a resolver prompt and continues the parent turn");
  expect(transport.requests().size() == 3, "task subagent uses parent-child-parent provider request order");
  if (transport.requests().size() == 3)
  {
    expect(transport.requests()[0].body.find("\"name\":\"task\"") != std::string::npos, "parent provider request exposes the task tool schema");
    expect(transport.requests()[1].body.find("Return child result only.") != std::string::npos, "child provider request receives the delegated prompt");
    expect(transport.requests()[1].body.find("\"name\":\"task\"") == std::string::npos &&
               transport.requests()[1].body.find("\"name\":\"job\"") == std::string::npos,
           "child provider request hides recursive task and job tool access");
    expect(transport.requests()[2].body.find("child result") != std::string::npos, "parent continuation receives child task result context");
  }

  auto entries = store.load();
  expect(entries.has_value(), "task parent session loads");
  bool saw_task_call = false;
  bool saw_task_result = false;
  bool saw_task_permission = false;
  if (entries)
  {
    auto const projection = ava::session::classify_assistant_output(*entries);
    for (auto const& turn : projection.turns)
    {
      for (auto const& item : turn.items)
      {
        if (auto const* function = std::get_if<ava::session::AssistantOutputFunctionCall>(&item.item.payload))
          saw_task_call = saw_task_call || function->name == "task";
      }
    }
    for (auto const& entry : *entries)
    {
      saw_task_result =
          saw_task_result ||
          (entry.type == ava::session::EntryType::ToolResult && entry.data_json.find("\\\"tool\\\":\\\"task\\\"") != std::string::npos &&
           entry.data_json.find("child result") != std::string::npos && entry.data_json.find("\"assistant_output_entry_id\":") != std::string::npos);
      saw_task_permission = saw_task_permission ||
                            (entry.type == ava::session::EntryType::PermissionDecision && entry.data_json.find("\"operation\":\"task\"") != std::string::npos &&
                             entry.data_json.find("\"resolution\":\"allow\"") != std::string::npos &&
                             entry.data_json.find("\"resolution_source\":\"policy\"") != std::string::npos);
    }
  }
  expect(saw_task_call && saw_task_result && saw_task_permission,
         "prompt-free task launch persists committed task function, bound result, and audited policy Allow");
  auto const parent_validation = entries ? ava::session::validate_session_replay(*entries) : ava::session::SessionReplayValidation{};
  expect(entries && parent_validation.ok(), "task parent session passes strict replay validation");
  auto summaries = ava::session::SessionStore::list_sessions(workspace, session_root);
  expect(summaries && summaries->size() == 2, "task subagent creates a persisted child session beside the parent");
  bool saw_child_metadata = false;
  bool saw_child_prompt = false;
  bool saw_child_answer = false;
  if (summaries)
  {
    for (auto const& summary : *summaries)
    {
      if (summary.session_id == store.session_id())
        continue;
      auto child_store = ava::session::SessionStore::open(workspace, summary.session_id, session_root);
      if (!child_store)
        continue;
      auto child_entries = child_store->load();
      if (!child_entries)
        continue;
      for (auto const& entry : *child_entries)
      {
        saw_child_metadata = saw_child_metadata || (entry.type == ava::session::EntryType::SessionMetadata &&
                                                    entry.data_json.find("\"parent_session_id\":\"parent\"") != std::string::npos &&
                                                    entry.data_json.find("@general subagent") != std::string::npos);
        saw_child_prompt =
            saw_child_prompt || (entry.type == ava::session::EntryType::UserMessage && entry.data_json.find("Return child result only.") != std::string::npos);
        if (entry.type == ava::session::EntryType::AssistantTurnCommit)
        {
          auto const projection = ava::session::classify_assistant_output(*child_entries);
          for (auto const& turn : projection.turns)
            for (auto const& item : turn.items)
              if (auto const* text = std::get_if<ava::session::AssistantOutputText>(&item.item.payload))
                saw_child_answer = saw_child_answer || text->text == "child result";
        }
      }
    }
  }
  expect(saw_child_metadata && saw_child_prompt && saw_child_answer, "task child session records parent linkage, delegated prompt, and child answer");
  std::lock_guard trace_lock(trace_collector->mutex);
  auto trace = ava::observability::validate_and_score_trace(trace_collector->events);
  std::map<std::string, unsigned> starts, terminals;
  bool child_parent_correlation = false;
  for (auto const& event : trace_collector->events)
  {
    starts[event.run_id] += event.type == ava::observability::TraceEventType::AgentRunStart;
    terminals[event.run_id] += event.type == ava::observability::TraceEventType::AgentRunTerminal;
    child_parent_correlation =
        child_parent_correlation || (!event.parent_run_id.empty() && event.parent_session_id == "parent" && event.session_id != event.parent_session_id);
  }
  expect(trace.valid && starts.size() == 2 && starts == terminals && child_parent_correlation,
         "observed foreground task has separate parent/child lifecycles, fresh child session IDs, and typed parent correlation");
}

void test_agent_loop_task_subagent_limit_precedence_exceeds_parent_budget()
{
  auto const root = create_empty_root("agent-task-child-limit-precedence");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::provider::OpenAIProvider const provider("https://api.example.test");

  auto run_case = [&](std::string const& session_id, std::optional<std::size_t> task_override, std::size_t expected_rounds) {
    ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = session_id});
    std::string task_arguments = R"({"description":"exercise child budget","prompt":"keep using glob","subagent_type":"general"})";
    if (task_override)
      task_arguments.insert(task_arguments.size() - 1, ",\"max_tool_iterations\":" + std::to_string(*task_override));

    std::vector<ava::http::HttpResponse> responses;
    responses.push_back(sse_response(tool_call_sse("call_task", "task", task_arguments) + "data: [DONE]\n\n"));
    for (std::size_t index = 0; index < expected_rounds; ++index)
      responses.push_back(sse_response(tool_call_sse("call_glob_" + std::to_string(index), "glob", R"({"pattern":"*"})") + "data: [DONE]\n\n"));
    responses.push_back(sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"bounded child wrap-up\"}\n\ndata: [DONE]\n\n"));
    ava::tests::FakeTransport transport(std::move(responses));

    auto definitions = ava::agent::builtin_subagents();
    for (auto& definition : definitions)
      if (definition.name == "general")
        definition.max_tool_iterations = 7;
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .max_tool_iterations = 1,
        .subagents = std::move(definitions),
        .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          return ava::permissions::PermissionResolution::Allow;
        },
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });

    auto result = loop.run_turn("delegate bounded work", store, provider, transport);
    bool const tool_free_wrap = transport.requests().size() == expected_rounds + 2 &&
                                transport.requests().back().body.find("\"tools\":[]") != std::string::npos &&
                                transport.requests().back().body.find("Do not call tools") != std::string::npos;
    auto entries = store.load();
    bool persisted_count = false;
    if (entries)
      for (auto const& entry : *entries)
        persisted_count = persisted_count || (entry.type == ava::session::EntryType::ToolResult &&
                                              entry.data_json.find("\\\"tool_iterations\\\":" + std::to_string(expected_rounds)) != std::string::npos);
    return result && result->outcome == ava::core::RuntimeTerminalOutcome::MaxTurnRequests && result->tool_iterations == 1 && tool_free_wrap && persisted_count;
  };

  expect(run_case("parent-override-eight", 8, 8),
         "task max_tool_iterations override permits eight child tool rounds plus one tool-free wrap-up while parent maximum remains one");
  expect(run_case("parent-definition-seven", std::nullopt, 7),
         "subagent definition max_tool_iterations is the default when the task omits an override and is independent of the parent budget");
}

void test_agent_loop_task_spawned_child_compaction_isolated_and_resumable()
{
  auto const root = create_empty_root("agent-task-child-compaction-wiring");
  auto const workspace = root / "workspace";
  auto const session_root = root / "sessions";
  std::filesystem::create_directories(workspace);
  {
    std::ofstream large(workspace / "large.txt", std::ios::binary | std::ios::trunc);
    large << std::string(8192, 'x');
  }
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::session::CompactionConfig config;
  config.auto_threshold_tokens = 500;
  config.auto_threshold_tokens_explicit = true;
  config.keep_recent_tokens = 50;
  config.provider_id = "openai";
  config.model_id = "gpt-5.5";
  int parent_compactor_calls = 0;
  ava::session::SessionStore parent(ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "compaction-parent"});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .compact_context = [&parent_compactor_calls](ava::session::SessionReadAuthority, std::string_view,
                                                   std::vector<std::string> const&) -> ava::core::Result<bool> {
        ++parent_compactor_calls;
        return false;
      },
      .child_compaction_blueprint = ava::agent::ChildContextCompactionBlueprint{.config = config, .context_window_tokens = 4096},
      .append_entry = append_route_for_test(parent),
      .append_batch = append_batch_route_for_test(parent),
      .session_read_authority = read_authority_for_test(parent),
  });
  auto const read_call = tool_call_sse("call_large_read", "read_file", R"({"path":"large.txt"})") + "data: [DONE]\n\n";
  ava::tests::FakeTransport first_transport(
      {sse_response(tool_call_sse("call_compacting_task", "task",
                                  R"({"description":"compact child","prompt":"read the large file","subagent_type":"general","max_tool_iterations":1})") +
                    "data: [DONE]\n\n"),
       sse_response(read_call),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"# Goal\\nPreserve child findings CHECKPOINT_SENTINEL\\n# Constraints / "
                    "Preferences\\nNone\\n# Decisions\\nNone\\n# Files Read or Modified\\nlarge.txt\\n# Unresolved Tasks\\nWrap up\\n# Next "
                    "Steps\\nReport\"}\n\ndata: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"child wrap after one tool\"}\n\ndata: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent received compacted child\"}\n\ndata: [DONE]\n\n")});
  auto first_result = loop.run_turn("delegate compacting child", parent, provider, first_transport);

  auto sessions = ava::session::SessionStore::list_sessions(workspace, session_root);
  std::optional<std::string> child_id;
  if (sessions)
    for (auto const& summary : *sessions)
      if (summary.session_id != parent.session_id())
        child_id = summary.session_id;
  auto child_store = child_id
                         ? ava::session::SessionStore::open(workspace, *child_id, session_root)
                         : ava::core::Result<ava::session::SessionStore>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing child")));
  auto child_entries =
      child_store
          ? child_store->load()
          : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing child")));
  auto parent_entries = parent.load();
  auto const child_checkpoints =
      child_entries ? std::ranges::count_if(*child_entries, [](auto const& entry) { return entry.type == ava::session::EntryType::Compaction; }) : 0;
  auto const parent_checkpoints =
      parent_entries ? std::ranges::count_if(*parent_entries, [](auto const& entry) { return entry.type == ava::session::EntryType::Compaction; }) : 0;
  bool const compacted_request_shape = first_transport.requests().size() == 5 && first_transport.requests()[2].body.find("large.txt") != std::string::npos &&
                                       first_transport.requests()[3].body.find("CHECKPOINT_SENTINEL") != std::string::npos &&
                                       first_transport.requests()[3].body.find("\"tools\":[]") != std::string::npos;
  expect(first_result && first_result->final_text == "parent received compacted child" && child_checkpoints == 1 && parent_checkpoints == 0 &&
             parent_compactor_calls == 2 && compacted_request_shape,
         "real task spawn binds automatic compaction to the child only, clears the parent callback, and preserves the one-round tool ceiling through wrap-up");

  if (child_id)
  {
    auto const resume_json =
        std::string(R"({"description":"resume compacted child","prompt":"continue after checkpoint","subagent_type":"general","task_id":")") + *child_id +
        "\"}";
    ava::tests::FakeTransport resume_transport(
        {sse_response(tool_call_sse("call_resume_compacted", "task", resume_json) + "data: [DONE]\n\n"),
         sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"resumed from child checkpoint\"}\n\ndata: [DONE]\n\n"),
         sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent saw resumed child\"}\n\ndata: [DONE]\n\n")});
    auto resumed = loop.run_turn("resume compacted child", parent, provider, resume_transport);
    expect(resumed && resume_transport.requests().size() == 3 && resume_transport.requests()[1].body.find("CHECKPOINT_SENTINEL") != std::string::npos &&
               resume_transport.requests()[1].body.find("continue after checkpoint") != std::string::npos,
           "task_id resume restores the child-owned compaction checkpoint into the next child provider request");
  }

  ava::session::CompactionConfig disabled = config;
  disabled.auto_threshold_tokens = 0;
  disabled.auto_threshold_tokens_explicit = true;
  ava::session::SessionStore disabled_parent(
      ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "disabled-compaction-parent"});
  ava::tests::FakeTransport disabled_transport(
      {sse_response(tool_call_sse(
                        "call_disabled_task", "task",
                        R"({"description":"disabled compaction","prompt":"read large without compaction","subagent_type":"general","max_tool_iterations":2})") +
                    "data: [DONE]\n\n"),
       sse_response(tool_call_sse("call_disabled_read", "read_file", R"({"path":"large.txt"})") + "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"child without checkpoint\"}\n\ndata: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent disabled done\"}\n\ndata: [DONE]\n\n")});
  ava::agent::AgentLoop disabled_loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .child_compaction_blueprint = ava::agent::ChildContextCompactionBlueprint{.config = disabled, .context_window_tokens = 4096},
      .append_entry = append_route_for_test(disabled_parent),
      .append_batch = append_batch_route_for_test(disabled_parent),
      .session_read_authority = read_authority_for_test(disabled_parent),
  });
  auto disabled_result = disabled_loop.run_turn("delegate without compaction", disabled_parent, provider, disabled_transport);
  auto all_sessions = ava::session::SessionStore::list_sessions(workspace, session_root);
  bool disabled_child_has_checkpoint = false;
  if (all_sessions)
    for (auto const& summary : *all_sessions)
      if (summary.session_id != parent.session_id() && summary.session_id != disabled_parent.session_id() && (!child_id || summary.session_id != *child_id))
        if (auto store = ava::session::SessionStore::open(workspace, summary.session_id, session_root))
          if (auto entries = store->load())
            disabled_child_has_checkpoint = disabled_child_has_checkpoint ||
                                            std::ranges::any_of(*entries, [](auto const& entry) { return entry.type == ava::session::EntryType::Compaction; });
  expect(
      disabled_result && disabled_result->final_text == "parent disabled done" && disabled_transport.requests().size() == 4 && !disabled_child_has_checkpoint,
      "explicit child auto_threshold_tokens zero disables task-spawned child summarization and checkpoint writes");
}

void test_agent_loop_foreground_task_child_uses_parent_permission_resolver()
{
  auto const root = create_empty_root("agent-task-child-permission");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "foreground child permission fixture keeps sealed planning roots owner-only");
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "parent-child-permission"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(tool_call_sse("call_task", "task",
                                  R"({"description":"check child permission","prompt":"Run the requested verification.","subagent_type":"general"})") +
                    "data: [DONE]\n\n"),
       sse_response(tool_call_sse("call_child_bash", "bash", R"({"command":"true"})") + "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"child handled permission\"}\n\n"
                    "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent continued\"}\n\n"
                    "data: [DONE]\n\n")});
  std::vector<ava::permissions::PermissionPrompt> prompts;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .anchor_set = command_anchors_for_test(workspace, store.session_path().parent_path() / "spill"),
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        prompts.push_back(prompt);
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("delegate with child permission", store, provider, transport);
  expect(result && result->final_text == "parent continued" && transport.requests().size() == 4 && prompts.size() == 1 &&
             prompts.front().operation == ava::permissions::Operation::RunCommand && prompts.front().tool_name == "bash" && prompts.front().command == "true",
         "foreground child Ask operations use the inherited parent resolver while task launch itself remains auto-allowed");
}

void test_agent_loop_child_rejects_unadvertised_task_and_job_calls()
{
  auto const root = temp_root() / "agent-child-rejects-job-controls";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "simulated-child"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(tool_call_sse("call_nested_task", "task", R"({"description":"nested","prompt":"must not run","subagent_type":"general"})") +
                    "data: [DONE]\n\n"),
       sse_response(tool_call_sse("call_nested_job", "job", R"({"action":"list"})") + "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"child controls rejected\"}\n\ndata: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "openai", .model_id = "gpt-5.5", .system_prompt = "child system prompt"},
      .access_token = "token",
      .tool_visibility = {.excluded_tools = {"task", "job", "todowrite"}},
      .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
      .trace_context =
          {.run_id = {}, .turn_id = {}, .session_id = {}, .provider_id = {}, .parent_run_id = {}, .parent_turn_id = {}, .parent_session_id = "parent-session"},
  });
  auto result = loop.run_turn("malicious child tool calls", store, provider, transport);
  auto const requests = transport.requests();
  bool const schemas_hidden = requests.size() == 3 && requests.front().body.find("\"name\":\"task\"") == std::string::npos &&
                              requests.front().body.find("\"name\":\"job\"") == std::string::npos;
  expect(result && result->final_text == "child controls rejected" && result->tool_calls == 2 && requests.size() == 3,
         "child malicious task/job calls return bounded tool errors and the child continues");
  expect(requests.size() == 3 && requests[1].body.find("unknown tool") != std::string::npos, "child malicious task call cannot start a recursive subagent");
  expect(requests.size() == 3 && requests[2].body.find("unknown tool") != std::string::npos, "child malicious job call cannot reach a coordinator");
  expect(schemas_hidden, "child provider schema hides both task and job controls");
}

void test_agent_loop_coordinated_foreground_uses_fresh_worker_and_preserves_result_accounting()
{
  auto const root = temp_root() / "agent-task-coordinated-foreground";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  std::filesystem::permissions(root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "parent-coordinated"});
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(), coordinator_result ? "coordinated foreground fixture creates coordinator"
                                                            : "coordinated foreground fixture creates coordinator: " + coordinator_result.error().format());
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  ava::provider::OpenAIProvider const parent_provider("https://api.example.test");
  ava::tests::FakeTransport parent_transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_task\",\"name\":\"task\"}\n\n"
                                                           "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_task\",\"delta\":\"{"
                                                           "\\\"description\\\":\\\"Fresh child\\\",\\\"prompt\\\":\\\"Return fresh child result.\\\","
                                                           "\\\"subagent_type\\\":\\\"general\\\"}\"}\n\n"
                                                           "data: [DONE]\n\n"),
                                              sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent received fresh child\"}\n\n"
                                                           "data: [DONE]\n\n")});
  auto const full_child_summary = std::string(17U * 1024U, 'x') + "FULL_FOREGROUND_TAIL";
  auto child_responses = std::make_shared<std::vector<ava::http::HttpResponse>>(std::initializer_list<ava::http::HttpResponse>{
      sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"" + full_child_summary + "\"}\n\ndata: [DONE]\n\n")});
  auto child_requests = std::make_shared<std::vector<ava::http::HttpRequest>>();
  auto child_mutex = std::make_shared<std::mutex>();
  auto resume_state = std::make_shared<BlockingBackgroundTransport::State>();
  auto provider_creations = std::make_shared<std::atomic<unsigned>>(0);
  auto transport_creations = std::make_shared<std::atomic<unsigned>>(0);
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .background_provider_factory = [provider_creations]() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
        provider_creations->fetch_add(1, std::memory_order_relaxed);
        std::unique_ptr<ava::provider::Provider> provider = std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test");
        return provider;
      },
      .background_transport_factory = [child_responses, child_requests, child_mutex, resume_state,
                                       transport_creations]() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
        auto const creation = transport_creations->fetch_add(1, std::memory_order_relaxed) + 1;
        if (creation == 1)
        {
          std::unique_ptr<ava::http::Transport> transport = std::make_unique<SharedFakeTransport>(child_responses, child_requests, child_mutex);
          return transport;
        }
        std::unique_ptr<ava::http::Transport> transport = std::make_unique<BlockingBackgroundTransport>(
            resume_state, sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"resumed child result\"}\n\n"
                                       "data: [DONE]\n\n"));
        return transport;
      },
      .subagent_coordinator = coordinator,
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("delegate through coordinator", store, parent_provider, parent_transport);
  auto jobs = coordinator->list(store.session_id());
  expect(result && result->final_text == "parent received fresh child" && parent_transport.requests().size() == 2 && jobs.size() == 1,
         "coordinated foreground child returns synchronously without using the parent transport");
  expect(provider_creations->load(std::memory_order_relaxed) == 1 && transport_creations->load(std::memory_order_relaxed) == 1 && child_requests->size() == 1,
         "coordinated foreground owns exactly one fresh provider and transport worker");
  if (child_requests->size() == 1)
    expect(child_requests->front().body.find("\"name\":\"task\"") == std::string::npos &&
               child_requests->front().body.find("\"name\":\"job\"") == std::string::npos,
           "coordinated child provider request cannot expose recursive task/job execution");
  expect(jobs.size() == 1 && jobs.front().job.execution == ava::agent::SubagentExecutionState::Completed &&
             jobs.front().job.delivery == ava::agent::SubagentDeliveryState::Direct && jobs.front().job.summary &&
             jobs.front().job.summary->size() == 16U * 1024U && jobs.front().job.summary_truncated && jobs.front().job.provider_iterations == 1 &&
             jobs.front().job.tool_calls == 0 && jobs.front().job.tool_iterations == 0,
         "coordinated foreground durably bounds summary text while preserving direct accounting metadata");
  auto parent_entries = store.load();
  bool const persisted_full_result =
      parent_entries && std::ranges::any_of(*parent_entries, [](ava::session::SessionEntry const& entry) {
        return entry.type == ava::session::EntryType::ToolResult && entry.data_json.find("FULL_FOREGROUND_TAIL") != std::string::npos;
      });
  expect(persisted_full_result && parent_transport.requests().size() == 2 &&
             parent_transport.requests()[1].body.find("\\\"provider_iterations\\\":1") != std::string::npos,
         "foreground task result preserves the exact untruncated final text and accounting before normal provider-context limiting");

  if (jobs.empty())
    return;
  auto const task_id = jobs.front().job.identity.task_id;
  ava::tests::FakeTransport resume_parent_transport(
      {sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_resume\",\"name\":\"task\"}\n\n"
                    "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_resume\",\"delta\":\"{"
                    "\\\"description\\\":\\\"Resume fresh child\\\",\\\"prompt\\\":\\\"Continue and wait.\\\","
                    "\\\"subagent_type\\\":\\\"general\\\",\\\"task_id\\\":\\\"" +
                    task_id +
                    "\\\"}\"}\n\n"
                    "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent received resumed promotion\"}\n\n"
                    "data: [DONE]\n\n")});
  auto resumed_parent =
      std::async(std::launch::async, [&] { return loop.run_turn("resume coordinated child", store, parent_provider, resume_parent_transport); });
  expect(resume_state->wait_for_request(std::chrono::milliseconds(1000)), "completed child session starts a later foreground worker with the same task_id");
  auto resumed_jobs = wait_for_published_running_job(*coordinator, store.session_id(), 2);
  auto resumed_job =
      std::ranges::find_if(resumed_jobs, [](auto const& snapshot) { return snapshot.job.execution == ava::agent::SubagentExecutionState::Running; });
  expect(resumed_jobs.size() == 2 && resumed_job != resumed_jobs.end() && resumed_job->job.identity.task_id == task_id &&
             resumed_job->job.identity.child_session_id == jobs.front().job.identity.child_session_id &&
             resumed_job->job.identity.job_id != jobs.front().job.identity.job_id &&
             resumed_job->job.identity.delivery_id != jobs.front().job.identity.delivery_id,
         "foreground resume reuses child identity sequentially with fresh job and delivery identities");
  if (resumed_job == resumed_jobs.end())
  {
    resume_state->release_success();
    static_cast<void>(resumed_parent.get());
    return;
  }
  auto const resumed_job_id = resumed_job->job.identity.job_id;
  auto promoted_resume = coordinator->promote(store.session_id(), resumed_job_id);
  bool const resumed_parent_woke = resumed_parent.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  if (!resumed_parent_woke)
    resume_state->release_success();
  auto resumed_result = resumed_parent.get();
  expect(promoted_resume && promoted_resume->job.was_promoted && resumed_parent_woke && resumed_result &&
             resumed_result->final_text == "parent received resumed promotion",
         "a resumed foreground run can be promoted and wakes the parent without restarting");
  resume_state->release_success();
  auto resumed_terminal = coordinator->wait(store.session_id(), resumed_job_id, std::chrono::seconds(1));
  expect(resumed_terminal && resumed_terminal->job.execution == ava::agent::SubagentExecutionState::Completed &&
             resumed_terminal->job.delivery == ava::agent::SubagentDeliveryState::Pending && provider_creations->load(std::memory_order_relaxed) == 2 &&
             transport_creations->load(std::memory_order_relaxed) == 2,
         "promoted resumed worker completes once with pending delivery and fresh provider/transport ownership");
}

void test_agent_loop_foreground_promotion_wakes_parent_without_restarting_child()
{
  auto const root = temp_root() / "agent-task-foreground-promotion";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  std::filesystem::permissions(root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "parent-promote"});
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(),
         coordinator_result ? "promotion fixture creates coordinator" : "promotion fixture creates coordinator: " + coordinator_result.error().format());
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  auto child_state = std::make_shared<BlockingBackgroundTransport::State>();
  auto provider_creations = std::make_shared<std::atomic<unsigned>>(0);
  auto transport_creations = std::make_shared<std::atomic<unsigned>>(0);
  ava::provider::OpenAIProvider const parent_provider("https://api.example.test");
  ava::tests::FakeTransport parent_transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_task\",\"name\":\"task\"}\n\n"
                                                           "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_task\",\"delta\":\"{"
                                                           "\\\"description\\\":\\\"Promote child\\\",\\\"prompt\\\":\\\"Wait for promotion.\\\","
                                                           "\\\"subagent_type\\\":\\\"general\\\"}\"}\n\n"
                                                           "data: [DONE]\n\n"),
                                              sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent resumed after promotion\"}\n\n"
                                                           "data: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .background_provider_factory = [provider_creations]() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
        provider_creations->fetch_add(1, std::memory_order_relaxed);
        std::unique_ptr<ava::provider::Provider> provider = std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test");
        return provider;
      },
      .background_transport_factory = [child_state, transport_creations]() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
        transport_creations->fetch_add(1, std::memory_order_relaxed);
        std::unique_ptr<ava::http::Transport> transport = std::make_unique<BlockingBackgroundTransport>(
            child_state, sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"late child result\"}\n\n"
                                      "data: [DONE]\n\n"));
        return transport;
      },
      .subagent_coordinator = coordinator,
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto parent = std::async(std::launch::async, [&] { return loop.run_turn("delegate then promote", store, parent_provider, parent_transport); });
  expect(child_state->wait_for_request(std::chrono::milliseconds(1000)), "foreground promotion child reaches its fresh transport");
  auto jobs = wait_for_published_running_job(*coordinator, store.session_id(), 1);
  expect(jobs.size() == 1, "foreground promotion publishes one stable job before waiting");
  ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot> promoted =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing promotion job"));
  ava::core::Result<ava::app::CommandResult> active_command =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing active promotion command"));
  if (!jobs.empty())
  {
    active_command = ava::app::run_jobs_command(coordinator, store.session_id(), "promote " + jobs.front().job.identity.job_id, true);
    promoted = coordinator->snapshot(store.session_id(), jobs.front().job.identity.job_id);
  }
  expect(active_command && !active_command->output.empty() && promoted && promoted->job.was_promoted &&
             promoted->job.execution == ava::agent::SubagentExecutionState::Running,
         "out-of-band active-run /jobs promote durably changes mode while preserving the running worker");
  bool const parent_woke = parent.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
  if (!parent_woke)
    child_state->release_success();
  auto parent_result = parent.get();
  expect(parent_woke && parent_result && parent_result->final_text == "parent resumed after promotion",
         "live foreground /jobs promote wakes the parent tool call without a restart or modal boundary");
  expect(provider_creations->load(std::memory_order_relaxed) == 1 && transport_creations->load(std::memory_order_relaxed) == 1,
         "promotion never restarts or replaces the child worker");
  if (parent_transport.requests().size() == 2 && promoted)
    expect(parent_transport.requests()[1].body.find(promoted->job.identity.job_id) != std::string::npos &&
               parent_transport.requests()[1].body.find("promoted") != std::string::npos,
           "promoted task result returns the same running job identity to the parent");
  child_state->release_success();
  if (promoted)
  {
    auto completed = coordinator->wait(store.session_id(), promoted->job.identity.job_id, std::chrono::seconds(1));
    expect(completed && completed->job.execution == ava::agent::SubagentExecutionState::Completed &&
               completed->job.delivery == ava::agent::SubagentDeliveryState::Pending && completed->job.summary == "late child result",
           "promoted worker continues unchanged and atomically records pending delivery at completion");
  }
}

void test_agent_loop_promoted_failure_persists_sanitized_child_error()
{
  auto const root = temp_root() / "agent-task-promoted-failure";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  std::filesystem::permissions(root, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  auto const session_root = root / "sessions";
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "parent-promoted-failure"});
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  if (!coordinator_result)
  {
    expect(false, "promoted failure fixture creates coordinator");
    return;
  }
  auto coordinator = *coordinator_result;
  auto child_state = std::make_shared<BlockingBackgroundTransport::State>();
  ava::provider::OpenAIProvider const parent_provider("https://api.example.test");
  ava::tests::FakeTransport parent_transport(
      {sse_response(tool_call_sse("call_promoted_failure", "task",
                                  R"({"description":"Promoted failure","prompt":"Fail after promotion.","subagent_type":"general","mode":"foreground"})") +
                    "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent saw promotion\"}\n\n"
                    "data: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .background_provider_factory = []() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
        std::unique_ptr<ava::provider::Provider> provider = std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test");
        return provider;
      },
      .background_transport_factory = [child_state]() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
        auto secret_body = std::string("{\"error\":{\"message\":\"credential=promoted-secret command=curl --token promoted-secret\"}}");
        std::unique_ptr<ava::http::Transport> transport = std::make_unique<BlockingBackgroundTransport>(
            child_state, ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = std::move(secret_body)});
        return transport;
      },
      .subagent_coordinator = coordinator,
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto parent = std::async(std::launch::async, [&] { return loop.run_turn("delegate promoted failure", store, parent_provider, parent_transport); });
  expect(child_state->wait_for_request(std::chrono::milliseconds(1000)), "promoted failure child reaches transport");
  auto jobs = wait_for_published_running_job(*coordinator, store.session_id(), 1);
  if (jobs.empty())
  {
    child_state->release_success();
    static_cast<void>(parent.get());
    expect(false, "promoted failure publishes job");
    return;
  }
  auto promoted = coordinator->promote(store.session_id(), jobs.front().job.identity.job_id);
  expect(promoted && promoted->job.was_promoted, "promoted failure switches the running foreground job to background delivery");
  auto parent_result = parent.get();
  expect(parent_result && parent_result->final_text == "parent saw promotion", "promoted failure releases the parent before child terminal failure");
  child_state->release_success();
  auto failed = coordinator->wait(store.session_id(), jobs.front().job.identity.job_id, std::chrono::seconds(1));
  auto child_store = ava::session::SessionStore::open(workspace, jobs.front().job.identity.child_session_id, session_root);
  auto child_entries = child_store ? child_store->load()
                                   : ava::core::Result<std::vector<ava::session::SessionEntry>>(
                                         std::unexpected(ava::core::Error(ava::core::ErrorCategory::NotFound, "child unavailable")));
  bool const safe_error = child_entries && std::ranges::any_of(*child_entries, [](auto const& entry) {
                            return entry.type == ava::session::EntryType::Error && entry.data_json.find("subagent job failed") != std::string::npos &&
                                   entry.data_json.find("promoted-secret") == std::string::npos && entry.data_json.find("curl --token") == std::string::npos;
                          });
  expect(failed && failed->job.execution == ava::agent::SubagentExecutionState::Failed && failed->job.delivery == ava::agent::SubagentDeliveryState::Pending &&
             failed->job.error == "subagent job failed" && safe_error,
         "a promoted child failure persists one sanitized bounded child error and safe coordinator result");
}

void test_agent_loop_task_subagent_propagates_authority_roots_to_foreground_and_background_children()
{
  auto run_case = [](bool background) {
    auto const root = create_empty_root(background ? "agent-task-authority-background" : "agent-task-authority-foreground");
    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
           "task authority-root fixture keeps sealed planning roots owner-only");
    ava::session::SessionStore store(ava::session::SessionStoreOptions{
        .root_dir = root / "sessions", .workspace_dir = workspace, .session_id = background ? "task-authority-bg" : "task-authority-fg"});
    ava::provider::OpenAIProvider const provider("https://api.example.test");
    auto const task_arguments = std::string(R"({"description":"authority child","prompt":"run child bash","subagent_type":"general","background":)") +
                                (background ? "true}" : "false}");
    auto const child_bash = tool_call_sse("call_child_bash", "bash", R"({"command":"ls"})") + "data: [DONE]\n\n";
    int task_prompts = 0;
    auto collector = std::make_shared<TraceCollector>();
    auto observation = std::make_shared<ava::observability::RunObservation>(collector);
    std::shared_ptr<ava::agent::SubagentCoordinator> coordinator;
    std::shared_ptr<std::vector<ava::http::HttpResponse>> background_responses;
    std::shared_ptr<std::vector<ava::http::HttpRequest>> background_requests;
    std::shared_ptr<std::mutex> background_mutex;
    ava::tests::FakeTransport transport(
        background ? std::vector<ava::http::HttpResponse>{sse_response(tool_call_sse("call_task", "task", task_arguments) + "data: [DONE]\n\n"),
                                                          sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent queued\"}\n\n"
                                                                       "data: [DONE]\n\n")}
                   : std::vector<ava::http::HttpResponse>{sse_response(tool_call_sse("call_task", "task", task_arguments) + "data: [DONE]\n\n"),
                                                          sse_response(child_bash),
                                                          sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"child denied\"}\n\n"
                                                                       "data: [DONE]\n\n"),
                                                          sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent continued\"}\n\n"
                                                                       "data: [DONE]\n\n")});
    if (background)
    {
      auto coordinator_result = ava::agent::SubagentCoordinator::create();
      expect(coordinator_result.has_value(), "task authority-root background fixture creates coordinator");
      if (!coordinator_result)
        return;
      coordinator = *coordinator_result;
      background_responses = std::make_shared<std::vector<ava::http::HttpResponse>>(std::vector<ava::http::HttpResponse>{
          sse_response(child_bash), sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"child denied\"}\n\n"
                                                 "data: [DONE]\n\n")});
      background_requests = std::make_shared<std::vector<ava::http::HttpRequest>>();
      background_mutex = std::make_shared<std::mutex>();
    }

    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .anchor_set = command_anchors_for_test(workspace, store.session_path().parent_path() / "spill"),
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .tool_execution =
            ava::agent::ToolExecutionOptions{
                .ava_authority_roots = {workspace},
            },
        .permission_resolver = [&task_prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          ++task_prompts;
          return ava::permissions::PermissionResolution::Allow;
        },
        .background_provider_factory = background ? []() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
          std::unique_ptr<ava::provider::Provider> child = std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test");
          return child;
        }
        : decltype(ava::agent::AgentLoopOptions{}.background_provider_factory){},
        .background_transport_factory =
            background ? [background_responses, background_requests, background_mutex]() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
          std::unique_ptr<ava::http::Transport> child = std::make_unique<SharedFakeTransport>(background_responses, background_requests, background_mutex);
          return child;
        }
        : decltype(ava::agent::AgentLoopOptions{}.background_transport_factory){},
        .subagent_coordinator = coordinator,
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
        .observation = observation,
    });
    auto result = loop.run_turn("delegate authority child", store, provider, transport);

    bool child_completed = !background;
    std::vector<ava::http::HttpRequest> child_requests;
    if (background)
    {
      auto jobs = coordinator->list(store.session_id());
      if (!jobs.empty())
      {
        auto completed = coordinator->wait(store.session_id(), jobs.front().job.identity.job_id, std::chrono::milliseconds(1000));
        child_completed = completed && completed->job.execution == ava::agent::SubagentExecutionState::Completed && completed->job.summary == "child denied";
      }
      std::lock_guard lock(*background_mutex);
      child_requests = *background_requests;
    }
    bool process_started = false;
    {
      std::lock_guard lock(collector->mutex);
      process_started = std::ranges::any_of(
          collector->events, [](ava::observability::TraceEvent const& event) { return event.type == ava::observability::TraceEventType::ProcessStart; });
    }
    auto const child_error_propagated =
        background ? child_requests.size() == 2 && child_requests[1].body.find("must not overlap with any AVA authority root") != std::string::npos
                   : transport.requests().size() == 4 && transport.requests()[2].body.find("must not overlap with any AVA authority root") != std::string::npos;
    expect(result && task_prompts == 0 && child_completed && child_error_propagated && !process_started,
           background
               ? "background child copies AVA authority roots before its AgentLoop starts and blocks overlapping model commands without a launch prompt"
               : "foreground child copies AVA authority roots before its AgentLoop starts and blocks overlapping model commands without a launch prompt");
  };

  run_case(false);
  run_case(true);
}

void test_agent_loop_task_subagent_recovers_torn_child_before_resume()
{
  auto const root = create_empty_root("agent-task-subagent-torn-resume");

  auto const workspace = root / "workspace";
  auto const session_root = root / "sessions";
  std::filesystem::create_directories(workspace);

  auto child = ava::session::SessionStore::create(workspace, session_root);
  expect(child.has_value(), "torn child resume test creates a child session");
  if (!child)
    return;
  auto metadata = append_session_metadata_for_test(
      *child, ava::session::SessionMetadataUpdate{.name = "resumable child", .parent_session_id = "parent-resume", .actor = "subagent"});
  expect(metadata.has_value(), "torn child resume test seeds child metadata");
  if (!metadata)
    return;
  auto const child_id = child->session_id();
  auto const child_path = child->session_path();
  auto const valid_child_bytes = [&] {
    std::ifstream file(child_path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }();
  {
    std::ofstream file(child_path, std::ios::binary | std::ios::app);
    file << "{\"version\":3,\"id\":\"torn-child";
  }

  ava::session::SessionStore parent(ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "parent-resume"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto const task_arguments = std::string("{\\\"description\\\":\\\"Resume child\\\",\\\"prompt\\\":\\\"Continue child.\\\",") +
                              "\\\"subagent_type\\\":\\\"general\\\",\\\"task_id\\\":\\\"" + child_id + "\\\"}";
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_resume\",\"name\":\"task\"}\n\n"
                                                    "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_resume\",\"delta\":\"" +
                                                    task_arguments +
                                                    "\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"resumed child answer\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"parent resumed child\"}\n\n"
                                                    "data: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(parent),
      .append_batch = append_batch_route_for_test(parent),
      .session_read_authority = read_authority_for_test(parent),
  });

  auto result = loop.run_turn("resume torn child", parent, provider, transport);
  std::ifstream repaired_file(child_path, std::ios::binary);
  std::string repaired_bytes{std::istreambuf_iterator<char>(repaired_file), std::istreambuf_iterator<char>()};
  auto child_entries = child->load();
  bool quarantine_found = false;
  auto const quarantine_prefix = child_path.filename().string() + ".torn-tail.";
  std::error_code iter_error;
  for (std::filesystem::directory_iterator iterator(child_path.parent_path(), iter_error), end; !iter_error && iterator != end; iterator.increment(iter_error))
  {
    quarantine_found = quarantine_found || iterator->path().filename().string().starts_with(quarantine_prefix);
  }
  expect(result && result->final_text == "parent resumed child" && transport.requests().size() == 3 && child_entries && child_entries->size() >= 3 &&
             repaired_bytes.starts_with(valid_child_bytes) && repaired_bytes.find("torn-child") == std::string::npos && quarantine_found,
         "foreground task_id resume owns and recovers a torn child before loading and running it");
}

void test_subagent_config_loads_project_definitions()
{
  auto const root = create_empty_root("subagent-config");

  auto const workspace = root / "workspace";
  auto const global_agent_dir = root / "global-agents";
  auto const agent_dir = workspace / ".ava" / "agents";
  std::filesystem::create_directories(global_agent_dir);
  std::filesystem::create_directories(agent_dir);
  {
    std::ofstream file(global_agent_dir / "coder.md", std::ios::binary | std::ios::trunc);
    file << "---\n"
            "name: coder\n"
            "description: Global primary coder.\n"
            "mode: primary\n"
            "---\n"
            "GLOBAL PRIMARY INSTRUCTIONS";
  }
  {
    // Create the lexically later file first so this fixture proves discovery sorts paths rather than relying on filesystem iteration order.
    std::ofstream file(global_agent_dir / "ordered-z.md", std::ios::binary | std::ios::trunc);
    file << "---\n"
            "name: ordered\n"
            "description: Lexically later primary.\n"
            "mode: primary\n"
            "---\n"
            "LEXICALLY LATER PRIMARY";
  }
  {
    std::ofstream file(global_agent_dir / "ordered-a.md", std::ios::binary | std::ios::trunc);
    file << "---\n"
            "name: ordered\n"
            "description: Lexically earlier primary.\n"
            "mode: primary\n"
            "---\n"
            "LEXICALLY EARLIER PRIMARY";
  }
  {
    std::ofstream file(global_agent_dir / "broken-primary.md", std::ios::binary | std::ios::trunc);
    file << "---\n"
            "description: Broken primary.\n"
            "mode: primary\n"
            "tools: unrestricted\n"
            "---\n"
            "BROKEN PRIMARY";
  }
  {
    std::ofstream file(agent_dir / "reviewer.md", std::ios::binary | std::ios::trunc);
    file << "---\n"
            "name: reviewer\n"
            "description: Review implementation details.\n"
            "tools: read-only\n"
            "max_tool_iterations: 14\n"
            "---\n"
            "Inspect files and return concise review findings.";
  }
  {
    std::ofstream file(agent_dir / "general.md", std::ios::binary | std::ios::trunc);
    file << "---\n"
            "description: Attempt to override builtin.\n"
            "---\n"
            "Should be ignored.";
  }
  {
    std::ofstream file(agent_dir / "coder.md", std::ios::binary | std::ios::trunc);
    file << "---\n"
            "name: coder\n"
            "description: Project coder for both catalogs.\n"
            "mode: all\n"
            "tools: read-only\n"
            "---\n"
            "PROJECT PRIMARY INSTRUCTIONS";
  }
  std::array<std::string_view, 5> const invalid_limits{"true", "1.5", "0", "-1", "18446744073709551616"};
  for (std::size_t index = 0; index < invalid_limits.size(); ++index)
  {
    std::ofstream file(agent_dir / ("invalid-limit-" + std::to_string(index) + ".md"), std::ios::binary | std::ios::trunc);
    file << "---\nname: invalid-limit-" << index << "\ndescription: Invalid limit.\nmax_tool_iterations: " << invalid_limits[index] << "\n---\nINVALID";
  }
  {
    std::ofstream file(agent_dir / "invalid-limit-duplicate.md", std::ios::binary | std::ios::trunc);
    file << "---\nname: invalid-limit-duplicate\ndescription: Invalid duplicate limit.\nmax_tool_iterations: 2\nmax_tool_iterations: 3\n---\nINVALID";
  }
  {
    std::ofstream file(agent_dir / "primary-only.md", std::ios::binary | std::ios::trunc);
    file << "---\n"
            "description: Primary only.\n"
            "mode: primary\n"
            "---\n"
            "PRIMARY ONLY";
  }

  auto loaded = ava::agent::load_subagents(ava::agent::SubagentLoadOptions{
      .workspace_root = workspace, .global_agent_dirs = {global_agent_dir}, .project_agent_dirs = {agent_dir}, .include_project_agents = true});
  auto const* reviewer = ava::agent::find_subagent(loaded.subagents, "reviewer");
  auto const* general = ava::agent::find_subagent(loaded.subagents, "general");
  expect(reviewer && reviewer->description == "Review implementation details." && reviewer->tool_preset == ava::agent::SubagentToolPreset::ReadOnly &&
             reviewer->max_tool_iterations == 14 && reviewer->system_prompt.find("Inspect files") != std::string::npos &&
             reviewer->provenance == ava::agent::SubagentDefinitionProvenance::Project,
         "subagent config loads project-defined read-only subagents with explicit project provenance");
  expect(general && general->provenance == ava::agent::SubagentDefinitionProvenance::Builtin,
         "subagent config keeps builtin subagents from project override with explicit builtin provenance");
  auto ordered_primary = ava::agent::resolve_primary_agent(loaded, "ordered");
  expect(ordered_primary && ordered_primary->system_prompt.find("LEXICALLY LATER") != std::string::npos,
         "same-root duplicate definitions resolve in deterministic lexical path order");
  auto coder_primary = ava::agent::resolve_primary_agent(loaded, "coder");
  expect(coder_primary && coder_primary->system_prompt.find("PROJECT PRIMARY") != std::string::npos &&
             coder_primary->tool_preset == ava::agent::SubagentToolPreset::ReadOnly &&
             coder_primary->provenance == ava::agent::SubagentDefinitionProvenance::Project,
         "project mode-all definitions override global primary definitions with explicit project provenance");
  if (coder_primary)
  {
    std::ostringstream diagnostic;
    coder_primary->print_on(diagnostic);
    expect(diagnostic.str().find("provenance:project") != std::string::npos && diagnostic.str().find("coder") == std::string::npos &&
               diagnostic.str().find("PROJECT PRIMARY") == std::string::npos && diagnostic.str().find(agent_dir.string()) == std::string::npos,
           "primary-definition diagnostics expose typed provenance without names, prompt content, or source paths");
  }
  bool invalid_limits_rejected = true;
  for (std::size_t index = 0; index < invalid_limits.size(); ++index)
    invalid_limits_rejected = invalid_limits_rejected && ava::agent::find_subagent(loaded.subagents, "invalid-limit-" + std::to_string(index)) == nullptr;
  invalid_limits_rejected = invalid_limits_rejected && ava::agent::find_subagent(loaded.subagents, "invalid-limit-duplicate") == nullptr;
  expect(invalid_limits_rejected, "agent definitions strictly reject boolean, fractional, zero, negative, overflowing, and duplicate tool limits");
  expect(ava::agent::find_subagent(loaded.subagents, "coder") != nullptr, "mode-all definitions are task-subagent visible");
  expect(ava::agent::find_subagent(loaded.subagents, "primary-only") == nullptr, "primary-mode definitions are not task-subagent visible");
  expect(!ava::agent::resolve_primary_agent(loaded, "reviewer"), "subagent-mode definitions are not primary selectable");
  auto broken_primary = ava::agent::resolve_primary_agent(loaded, "broken-primary");
  expect(!broken_primary && broken_primary.error().format().find("failed validation") != std::string::npos,
         "malformed selected primary definitions fail closed with a sanitized validation error");
  expect(std::ranges::any_of(
             loaded.diagnostics,
             [](ava::agent::SubagentDiagnostic const& diagnostic) { return diagnostic.message.find("collides with a builtin") != std::string::npos; }),
         "subagent config reports builtin-name collisions");

  auto untrusted = ava::agent::load_subagents(ava::agent::SubagentLoadOptions{
      .workspace_root = workspace, .global_agent_dirs = {global_agent_dir}, .project_agent_dirs = {agent_dir}, .include_project_agents = false});
  expect(ava::agent::find_subagent(untrusted.subagents, "reviewer") == nullptr, "project subagents are gated by project resource trust");
  auto global_coder = ava::agent::resolve_primary_agent(untrusted, "coder");
  expect(global_coder && global_coder->system_prompt.find("GLOBAL PRIMARY") != std::string::npos &&
             global_coder->provenance == ava::agent::SubagentDefinitionProvenance::Global,
         "global primary definitions load without project trust with explicit global provenance");
  expect(!ava::agent::resolve_primary_agent(untrusted, "primary-only"), "project-only primary definitions fail closed without trust");

  auto narrowed_default = ava::agent::narrow_tool_visibility_to_read_only({});
  expect(narrowed_default.mode == ava::agent::ToolVisibilityMode::Default &&
             narrowed_default.included_tools == std::vector<std::string>({"read_file", "list_directory", "glob", "grep"}),
         "primary read-only preset narrows default visibility to canonical read tools");
  auto narrowed_include = ava::agent::narrow_tool_visibility_to_read_only(
      ava::agent::ToolVisibilityOptions{.included_tools = {"read", "write_file", "grep"}, .excluded_tools = {"grep"}});
  expect(narrowed_include.mode == ava::agent::ToolVisibilityMode::Default && narrowed_include.included_tools == std::vector<std::string>({"read", "grep"}) &&
             narrowed_include.excluded_tools == std::vector<std::string>({"grep"}),
         "primary read-only preset intersects aliases and preserves explicit exclusions");
  auto narrowed_mutation_only = ava::agent::narrow_tool_visibility_to_read_only(ava::agent::ToolVisibilityOptions{.included_tools = {"write_file"}});
  expect(narrowed_mutation_only.mode == ava::agent::ToolVisibilityMode::NoTools && narrowed_mutation_only.included_tools.empty(),
         "primary read-only preset fails closed when an explicit include has no read-only tools");
  auto narrowed_no_builtins =
      ava::agent::narrow_tool_visibility_to_read_only(ava::agent::ToolVisibilityOptions{.mode = ava::agent::ToolVisibilityMode::NoBuiltinTools});
  expect(narrowed_no_builtins.mode == ava::agent::ToolVisibilityMode::NoTools,
         "primary read-only preset cannot widen no-builtin visibility through an include list");
}

void test_agent_loop_custom_subagent_definition_controls_prompt_and_tools()
{
  auto const root = create_empty_root("agent-task-custom-subagent");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const session_root = root / "sessions";
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "parent-custom"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_task\",\"name\":\"task\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_task\",\"delta\":\"{"
                                                    "\\\"description\\\":\\\"Review docs\\\",\\\"prompt\\\":\\\"Return review result.\\\","
                                                    "\\\"subagent_type\\\":\\\"reviewer\\\"}\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"review result\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"custom done\"}\n\n"
                                                    "data: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .subagents = {ava::agent::SubagentDefinition{.name = "reviewer",
                                                   .description = "Read-only reviewer",
                                                   .system_prompt = "CUSTOM REVIEWER ROLE",
                                                   .tool_preset = ava::agent::SubagentToolPreset::ReadOnly}},
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("delegate custom", store, provider, transport);
  expect(result && result->final_text == "custom done" && transport.requests().size() == 3, "agent loop runs configured custom subagents");
  if (transport.requests().size() == 3)
  {
    expect(transport.requests()[1].body.find("CUSTOM REVIEWER ROLE") != std::string::npos, "custom subagent system prompt is appended to child request");
    expect(transport.requests()[1].body.find("\"name\":\"read_file\"") != std::string::npos, "read-only custom subagents retain read tools");
    expect(transport.requests()[1].body.find("\"name\":\"bash\"") == std::string::npos &&
               transport.requests()[1].body.find("\"name\":\"write_file\"") == std::string::npos &&
               transport.requests()[1].body.find("\"name\":\"task\"") == std::string::npos,
           "read-only custom subagents hide mutation, shell, and recursive task tools");
  }
}
