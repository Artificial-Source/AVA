#include "sys.h"
#include "tests/agent_loop_test_declarations.h"
#include "tests/support/agent_loop_test_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/app/command_jobs.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/subagent_inspector.h"
#include "ava/session/assistant_output.h"
#include "ava/session/session_store.h"
#include "ava/provider/openai_provider.h"
#include "ava/lsp/lsp_client.h"
#include "ava/core/result.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

using agent_loop_test::BlockingBackgroundTransport;
using agent_loop_test::BlockingSequenceTransport;
using agent_loop_test::SharedFakeTransport;
using agent_loop_test::sse_response;
using agent_loop_test::tool_call_sse;
using agent_loop_test::TraceCollector;

namespace {

class NoopDiagnosticsProvider final : public ava::lsp::DiagnosticsProvider
{
 public:
  [[nodiscard]] ava::core::Result<std::vector<ava::lsp::Diagnostic>> diagnostics(std::filesystem::path const&, ava::lsp::CancelCallback = nullptr) override
  {
    return std::vector<ava::lsp::Diagnostic>{};
  }
};

}  // namespace

void test_agent_loop_background_task_starts_child_session()
{
  auto const root = create_empty_root("agent-task-background");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const session_root = root / "sessions";
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "parent-bg"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(), "background start fixture creates coordinator");
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  auto trace_collector = std::make_shared<TraceCollector>();
  auto observation = std::make_shared<ava::observability::RunObservation>(trace_collector);
  auto background_state = std::make_shared<BlockingBackgroundTransport::State>();
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_task\",\"name\":\"task\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_task\",\"delta\":\"{"
                                                    "\\\"description\\\":\\\"Explore async\\\",\\\"prompt\\\":\\\"Return background child.\\\","
                                                    "\\\"subagent_type\\\":\\\"general\\\",\\\"background\\\":true}\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"queued\"}\n\n"
                                                    "data: [DONE]\n\n")});
  auto parent_append = append_route_for_test(store);
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .tool_resources =
          ava::agent::ToolResourceOptions{
              .lsp_diagnostics_provider = std::make_shared<NoopDiagnosticsProvider>(),
          },
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .background_provider_factory = []() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
        std::unique_ptr<ava::provider::Provider> provider = std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test");
        return provider;
      },
      .background_transport_factory = [background_state]() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
        std::unique_ptr<ava::http::Transport> transport = std::make_unique<BlockingBackgroundTransport>(
            background_state, sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"background child\"}\n\n"
                                           "data: [DONE]\n\n"));
        return transport;
      },
      .subagent_coordinator = coordinator,
      .append_entry = parent_append,
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
      .observation = observation});

  auto result = loop.run_turn("delegate background", store, provider, transport);
  expect(result && result->final_text == "queued" && result->tool_calls == 1 && transport.requests().size() == 2,
         "agent loop starts background task and continues parent turn immediately");
  if (transport.requests().size() == 2)
  {
    expect(transport.requests()[1].body.find("\\\"state\\\":\\\"running\\\"") != std::string::npos, "parent continuation receives running task state");
    expect(transport.requests()[1].body.find("\\\"job_id\\\":\\\"job_") != std::string::npos, "parent continuation receives coordinator job id");
  }

  auto running_jobs = coordinator->list(store.session_id());
  expect(running_jobs.size() == 1 && running_jobs.front().job.execution == ava::agent::SubagentExecutionState::Running &&
             running_jobs.front().job.identity.child_session_id.starts_with("session_"),
         "background task appears as running in the coordinator");
  expect(background_state->wait_for_request(std::chrono::milliseconds(1000)), "background child reaches provider transport while registered");
  auto background_requests = background_state->requests_snapshot();
  bool const saw_background_request_without_lsp = !background_requests.empty() &&
                                                  background_requests.front().body.find("\"name\":\"lsp_diagnostics\"") == std::string::npos &&
                                                  background_requests.front().body.find("\"name\":\"lsp_workspace_symbols\"") == std::string::npos;

  bool foreground_resume_blocked = false;
  if (!running_jobs.empty())
  {
    ava::session::SessionStore competing_parent(
        ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "parent-bg-contender"});
    auto const resume_arguments = std::string("{\\\"description\\\":\\\"Resume running child\\\",\\\"prompt\\\":\\\"Compete.\\\",") +
                                  "\\\"subagent_type\\\":\\\"general\\\",\\\"task_id\\\":\\\"" + running_jobs.front().job.identity.child_session_id + "\\\"}";
    ava::tests::FakeTransport competing_transport(
        {sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_compete\",\"name\":\"task\"}\n\n"
                      "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_compete\",\"delta\":\"" +
                      resume_arguments +
                      "\"}\n\n"
                      "data: [DONE]\n\n"),
         sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"resume was blocked\"}\n\n"
                      "data: [DONE]\n\n")});
    ava::agent::AgentLoop competing_loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          return ava::permissions::PermissionResolution::Allow;
        },
        .append_entry = append_route_for_test(competing_parent),
        .append_batch = append_batch_route_for_test(competing_parent),
        .session_read_authority = read_authority_for_test(competing_parent),
    });
    auto competing_result = competing_loop.run_turn("resume running child", competing_parent, provider, competing_transport);
    foreground_resume_blocked = competing_result && competing_result->final_text == "resume was blocked" && competing_transport.requests().size() == 2 &&
                                competing_transport.requests()[1].body.find("already owned") != std::string::npos;
  }
  expect(foreground_resume_blocked, "foreground task_id resume fails while the background child owns its session lease");

  background_state->release_success();
  ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot> completed =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing background job"));
  if (!running_jobs.empty())
  {
    completed = coordinator->wait(store.session_id(), running_jobs.front().job.identity.job_id, std::chrono::milliseconds(1000));
  }
  expect(completed && completed->job.execution == ava::agent::SubagentExecutionState::Completed && completed->job.summary == "background child",
         "background task transitions to completed in the coordinator");
  auto result_snapshot =
      completed
          ? coordinator->result(store.session_id(), completed->job.identity.job_id)
          : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing")));
  expect(
      result_snapshot && result_snapshot->job.execution == ava::agent::SubagentExecutionState::Completed && result_snapshot->job.summary == "background child",
      "background task result is queryable through the coordinator");
  bool saw_background_answer = false;
  if (completed)
  {
    auto child_store = ava::session::SessionStore::open(workspace, completed->job.identity.child_session_id, session_root);
    if (child_store)
    {
      auto child_entries = child_store->load();
      if (child_entries)
      {
        auto const projection = ava::session::classify_assistant_output(*child_entries);
        for (auto const& turn : projection.turns)
          for (auto const& item : turn.items)
            if (auto const* text = std::get_if<ava::session::AssistantOutputText>(&item.item.payload))
              saw_background_answer = saw_background_answer || text->text == "background child";
      }
    }
  }
  expect(saw_background_request_without_lsp, "background subagents do not inherit the parent LSP provider");
  expect(saw_background_answer, "background subagents write completion to the child session");
  std::lock_guard trace_lock(trace_collector->mutex);
  auto trace = ava::observability::validate_and_score_trace(trace_collector->events);
  std::map<std::string, unsigned> starts, terminals;
  bool child_parent_correlation = false;
  for (auto const& event : trace_collector->events)
  {
    starts[event.run_id] += event.type == ava::observability::TraceEventType::AgentRunStart;
    terminals[event.run_id] += event.type == ava::observability::TraceEventType::AgentRunTerminal;
    child_parent_correlation =
        child_parent_correlation || (!event.parent_run_id.empty() && event.parent_session_id == "parent-bg" && event.session_id != event.parent_session_id);
  }
  expect(trace.valid && starts.size() == 2 && starts == terminals && child_parent_correlation,
         "observed background task has separate parent/child lifecycles and typed parent correlation");
}

void test_agent_loop_background_resume_preserves_history_and_owner_authority()
{
  auto const root = create_empty_root("agent-task-background-resume-owner");
  auto const workspace = root / "workspace";
  auto const session_root = root / "sessions";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore owner(ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "background-owner"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(), "background resume fixture creates coordinator");
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  auto child_responses = std::make_shared<std::vector<ava::http::HttpResponse>>();
  child_responses->push_back(sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"first durable child answer\"}\n\ndata: [DONE]\n\n"));
  child_responses->push_back(sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"second durable child answer\"}\n\ndata: [DONE]\n\n"));
  auto child_requests = std::make_shared<std::vector<ava::http::HttpRequest>>();
  auto child_mutex = std::make_shared<std::mutex>();

  auto make_options = [&](ava::session::SessionStore& parent) {
    return ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          return ava::permissions::PermissionResolution::Allow;
        },
        .background_provider_factory = []() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
          return std::unique_ptr<ava::provider::Provider>(std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test"));
        },
        .background_transport_factory = [child_responses, child_requests, child_mutex]() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
          return std::unique_ptr<ava::http::Transport>(std::make_unique<SharedFakeTransport>(child_responses, child_requests, child_mutex));
        },
        .subagent_coordinator = coordinator,
        .append_entry = append_route_for_test(parent),
        .append_batch = append_batch_route_for_test(parent),
        .session_read_authority = read_authority_for_test(parent),
    };
  };

  ava::tests::FakeTransport launch_transport(
      {sse_response(tool_call_sse("call_launch", "task", R"({"description":"launch","prompt":"first prompt","subagent_type":"general","background":true})") +
                    "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"launched\"}\n\ndata: [DONE]\n\n")});
  ava::agent::AgentLoop owner_loop(make_options(owner));
  auto launch_result = owner_loop.run_turn("launch child", owner, provider, launch_transport);
  auto jobs = coordinator->list(owner.session_id());
  ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot> first =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing launched job"));
  if (!jobs.empty())
    first = coordinator->wait(owner.session_id(), jobs.front().job.identity.job_id, std::chrono::milliseconds(1000));
  expect(launch_result && first && first->job.execution == ava::agent::SubagentExecutionState::Completed,
         "background child completes before durable task_id resume");
  if (!first)
    return;

  auto child_store = ava::session::SessionStore::open(workspace, first->job.identity.child_session_id, session_root);
  expect(child_store.has_value(), "background resume reopens completed child session");
  if (!child_store)
    return;
  auto const child_path = child_store->session_path();
  {
    std::ofstream torn(child_path, std::ios::binary | std::ios::app);
    torn << "{\"version\":3,\"id\":\"wrong-owner-must-not-recover";
  }
  auto read_bytes = [&]() {
    std::ifstream file(child_path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  };
  auto const bytes_before_wrong_owner = read_bytes();

  ava::session::SessionStore stranger(
      ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "background-stranger"});
  auto const resume_json = std::string(R"({"description":"resume","prompt":"second prompt","subagent_type":"general","background":true,"task_id":")") +
                           first->job.identity.child_session_id + "\"}";
  ava::tests::FakeTransport stranger_transport(
      {sse_response(tool_call_sse("call_wrong_owner", "task", resume_json) + "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"wrong owner rejected\"}\n\ndata: [DONE]\n\n")});
  ava::agent::AgentLoop stranger_loop(make_options(stranger));
  auto stranger_result = stranger_loop.run_turn("try foreign resume", stranger, provider, stranger_transport);
  std::size_t child_request_count_after_wrong_owner = 0;
  {
    std::lock_guard lock(*child_mutex);
    child_request_count_after_wrong_owner = child_requests->size();
  }
  expect(stranger_result && stranger_transport.requests().size() == 2 &&
             stranger_transport.requests()[1].body.find("not owned by the current parent session") != std::string::npos &&
             read_bytes() == bytes_before_wrong_owner && child_request_count_after_wrong_owner == 1,
         "wrong-parent background task_id resume rejects before child recovery, mutation, provider creation, or execution");

  ava::tests::FakeTransport resume_transport({sse_response(tool_call_sse("call_owner_resume", "task", resume_json) + "data: [DONE]\n\n"),
                                              sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"resume queued\"}\n\ndata: [DONE]\n\n")});
  auto resume_result = owner_loop.run_turn("resume my child", owner, provider, resume_transport);
  auto resumed_jobs = coordinator->list(owner.session_id());
  ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot> second =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing resumed job"));
  if (resumed_jobs.size() == 2)
  {
    auto const& resumed = resumed_jobs.front().job.identity.job_id == first->job.identity.job_id ? resumed_jobs.back() : resumed_jobs.front();
    second = coordinator->wait(owner.session_id(), resumed.job.identity.job_id, std::chrono::milliseconds(1000));
  }
  std::vector<ava::http::HttpRequest> requests;
  {
    std::lock_guard lock(*child_mutex);
    requests = *child_requests;
  }
  auto child_entries = child_store->load();
  bool saw_both_answers = child_entries.has_value();
  if (child_entries)
    saw_both_answers =
        std::ranges::any_of(*child_entries, [](auto const& entry) { return entry.data_json.find("first durable child answer") != std::string::npos; }) &&
        std::ranges::any_of(*child_entries, [](auto const& entry) { return entry.data_json.find("second durable child answer") != std::string::npos; });
  expect(resume_result && second && second->job.execution == ava::agent::SubagentExecutionState::Completed &&
             second->job.identity.job_id != first->job.identity.job_id && second->job.identity.child_session_id == first->job.identity.child_session_id &&
             requests.size() == 2 && requests.back().body.find("first durable child answer") != std::string::npos &&
             requests.back().body.find("second prompt") != std::string::npos && read_bytes().find("wrong-owner-must-not-recover") == std::string::npos &&
             saw_both_answers,
         "owned background task_id resume recovers the torn tail, creates a new job, and retains prior child history");
}

void test_agent_loop_dispatcher_steering_reaches_blocked_child_fifo_once()
{
  auto const root = create_empty_root("agent-task-background-steering");
  auto const workspace = root / "workspace";
  auto const session_root = root / "sessions";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore parent(ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "steering-parent"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(), "dispatcher steering fixture creates coordinator");
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  auto child_state = std::make_shared<BlockingSequenceTransport::State>();
  std::vector<ava::http::HttpResponse> child_responses{
      sse_response(tool_call_sse("call_child_glob", "glob", R"({"pattern":"*"})") + "data: [DONE]\n\n"),
      sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"steered child summary\"}\n\ndata: [DONE]\n\n")};

  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .background_provider_factory = []() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
        return std::unique_ptr<ava::provider::Provider>(std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test"));
      },
      .background_transport_factory = [child_state, child_responses]() mutable -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
        return std::unique_ptr<ava::http::Transport>(std::make_unique<BlockingSequenceTransport>(child_state, std::move(child_responses)));
      },
      .subagent_coordinator = coordinator,
      .append_entry = append_route_for_test(parent),
      .append_batch = append_batch_route_for_test(parent),
      .session_read_authority = read_authority_for_test(parent),
  });
  ava::tests::FakeTransport launch_transport(
      {sse_response(
           tool_call_sse("call_task_steer", "task",
                         R"({"description":"steer target","prompt":"wait for guidance","subagent_type":"general","background":true,"max_tool_iterations":1})") +
           "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"child blocked\"}\n\ndata: [DONE]\n\n")});
  auto launch = loop.run_turn("launch steering target", parent, provider, launch_transport);
  expect(launch && child_state->wait_for_requests(1, std::chrono::milliseconds(1000)),
         "real background task reaches a blocked child provider request before steering");
  auto jobs = coordinator->list(parent.session_id());
  if (jobs.empty())
  {
    expect(false, "blocked steering child is published in coordinator");
    child_state->release_success();
    return;
  }
  auto const job_id = jobs.front().job.identity.job_id;
  auto const first_steer = std::string(R"({"action":"steer","job_id":")") + job_id + R"(","message":"FIRST_STEER_SENTINEL"})";
  auto const second_steer = std::string(R"({"action":"steer","job_id":")") + job_id + R"(","message":"SECOND_STEER_SENTINEL"})";
  ava::tests::FakeTransport steer_transport(
      {sse_response(tool_call_sse("call_steer_first", "job", first_steer) + tool_call_sse("call_steer_second", "job", second_steer) + "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"both steering messages queued\"}\n\ndata: [DONE]\n\n")});
  auto steered = loop.run_turn("steer the child twice", parent, provider, steer_transport);
  child_state->release_success();
  auto completed = coordinator->wait(parent.session_id(), job_id, std::chrono::milliseconds(1000));
  auto requests = child_state->requests_snapshot();

  auto child_store = ava::session::SessionStore::open(workspace, jobs.front().job.identity.child_session_id, session_root);
  auto child_entries =
      child_store
          ? child_store->load()
          : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing child")));
  std::size_t first_child_occurrences = 0;
  std::size_t second_child_occurrences = 0;
  if (child_entries)
    for (auto const& entry : *child_entries)
      if (entry.type == ava::session::EntryType::UserMessage)
      {
        first_child_occurrences += entry.data_json.find("FIRST_STEER_SENTINEL") != std::string::npos;
        second_child_occurrences += entry.data_json.find("SECOND_STEER_SENTINEL") != std::string::npos;
      }
  auto parent_entries = parent.load();
  bool parent_retained_first = false;
  bool parent_retained_second = false;
  if (parent_entries)
    for (auto const& entry : *parent_entries)
    {
      parent_retained_first = parent_retained_first || entry.data_json.find("FIRST_STEER_SENTINEL") != std::string::npos;
      parent_retained_second = parent_retained_second || entry.data_json.find("SECOND_STEER_SENTINEL") != std::string::npos;
    }
  auto const fifo_injection = requests.size() == 2 && requests.back().body.find("FIRST_STEER_SENTINEL") < requests.back().body.find("SECOND_STEER_SENTINEL") &&
                              requests.back().body.find("SECOND_STEER_SENTINEL") != std::string::npos;
  expect(steered && steered->tool_calls == 2 && completed && completed->job.execution == ava::agent::SubagentExecutionState::Completed &&
             completed->job.summary == "steered child summary" && fifo_injection && first_child_occurrences == 1 && second_child_occurrences == 1 &&
             parent_retained_first && parent_retained_second,
         "model job steer dispatch injects FIFO messages exactly once at the next child provider boundary and preserves parent tool arguments");
}

void test_agent_loop_cancel_during_child_compaction_writes_no_checkpoint()
{
  auto const root = create_empty_root("agent-task-child-compaction-cancel");
  auto const workspace = root / "workspace";
  auto const session_root = root / "sessions";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore parent(
      ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "compaction-cancel-parent"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(), "child compaction cancellation fixture creates coordinator");
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  auto child_state = std::make_shared<BlockingSequenceTransport::State>();
  ava::session::CompactionConfig config;
  config.auto_threshold_tokens = 1;
  config.auto_threshold_tokens_explicit = true;
  config.keep_recent_tokens = 1;
  config.provider_id = "openai";
  config.model_id = "gpt-5.5";
  std::vector<ava::http::HttpResponse> child_responses{
      sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"must never checkpoint\"}\n\ndata: [DONE]\n\n")};
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .child_compaction_blueprint = ava::agent::ChildContextCompactionBlueprint{.config = config, .context_window_tokens = 4096},
      .background_provider_factory = []() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
        return std::unique_ptr<ava::provider::Provider>(std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test"));
      },
      .background_transport_factory = [child_state, child_responses]() mutable -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
        return std::unique_ptr<ava::http::Transport>(std::make_unique<BlockingSequenceTransport>(child_state, std::move(child_responses)));
      },
      .subagent_coordinator = coordinator,
      .append_entry = append_route_for_test(parent),
      .append_batch = append_batch_route_for_test(parent),
      .session_read_authority = read_authority_for_test(parent),
  });
  ava::tests::FakeTransport parent_transport(
      {sse_response(
           tool_call_sse("call_compaction_cancel_task", "task",
                         R"({"description":"cancel summarizer","prompt":"long enough child prompt to compact","subagent_type":"general","background":true})") +
           "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"summarizer running\"}\n\ndata: [DONE]\n\n")});
  auto launched = loop.run_turn("launch child that compacts", parent, provider, parent_transport);
  auto jobs = coordinator->list(parent.session_id());
  bool const summary_request_blocked = child_state->wait_for_requests(1, std::chrono::milliseconds(1000));
  if (jobs.empty())
  {
    expect(false, "compacting child is published for cancellation");
    child_state->release_success();
    return;
  }
  auto const canceled = coordinator->cancel(parent.session_id(), jobs.front().job.identity.job_id);
  auto const cancel_observed = child_state->wait_for_cancel(std::chrono::milliseconds(1000));
  auto terminal = coordinator->wait(parent.session_id(), jobs.front().job.identity.job_id, std::chrono::milliseconds(1000));
  auto child_store = ava::session::SessionStore::open(workspace, jobs.front().job.identity.child_session_id, session_root);
  auto child_entries =
      child_store
          ? child_store->load()
          : ava::core::Result<std::vector<ava::session::SessionEntry>>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing child")));
  bool const has_checkpoint =
      child_entries && std::ranges::any_of(*child_entries, [](auto const& entry) { return entry.type == ava::session::EntryType::Compaction; });
  auto requests = child_state->requests_snapshot();
  expect(launched && canceled && summary_request_blocked && cancel_observed && terminal &&
             terminal->job.execution == ava::agent::SubagentExecutionState::Canceled && requests.size() == 1 &&
             requests.front().body.find("summary") != std::string::npos && !has_checkpoint,
         "cancellation while child summarization is blocked terminates the worker without appending a compaction checkpoint");
}

void test_agent_loop_background_task_failure_records_parent_and_child_errors()
{
  auto const root = create_empty_root("agent-task-background-failure");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const session_root = root / "sessions";
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "parent-bg-fail"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto background_responses = std::make_shared<std::vector<ava::http::HttpResponse>>();
  auto background_requests = std::make_shared<std::vector<ava::http::HttpRequest>>();
  auto background_mutex = std::make_shared<std::mutex>();
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(), "background failure fixture creates coordinator");
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_task\",\"name\":\"task\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_task\",\"delta\":\"{"
                                                    "\\\"description\\\":\\\"Fail async\\\",\\\"prompt\\\":\\\"This background request will fail.\\\","
                                                    "\\\"subagent_type\\\":\\\"general\\\",\\\"background\\\":true}\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"queued failure\"}\n\n"
                                                    "data: [DONE]\n\n")});
  auto parent_append = append_route_for_test(store);
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .background_provider_factory = []() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
        std::unique_ptr<ava::provider::Provider> provider = std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test");
        return provider;
      },
      .background_transport_factory = [background_responses, background_requests,
                                       background_mutex]() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
        std::unique_ptr<ava::http::Transport> transport = std::make_unique<SharedFakeTransport>(background_responses, background_requests, background_mutex);
        return transport;
      },
      .subagent_coordinator = coordinator,
      .append_entry = parent_append,
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("delegate failing background", store, provider, transport);
  expect(result && result->final_text == "queued failure" && result->tool_calls == 1, "agent loop can queue a background task that later fails");

  auto jobs = coordinator->list(store.session_id());
  expect(jobs.size() == 1, "failed background task is registered with the coordinator");
  ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot> failed =
      std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing background job"));
  if (!jobs.empty())
  {
    failed = coordinator->wait(store.session_id(), jobs.front().job.identity.job_id, std::chrono::milliseconds(1000));
  }
  expect(failed && failed->job.execution == ava::agent::SubagentExecutionState::Failed && failed->job.error && *failed->job.error == "subagent job failed" &&
             failed->job.error_category && *failed->job.error_category == "provider",
         "failed background task is marked failed with sanitized coordinator public error state");
  auto result_snapshot =
      failed ? coordinator->result(store.session_id(), failed->job.identity.job_id)
             : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing")));
  expect(result_snapshot && result_snapshot->job.execution == ava::agent::SubagentExecutionState::Failed && result_snapshot->job.error &&
             *result_snapshot->job.error == "subagent job failed",
         "failed background task result remains queryable through the coordinator");

  bool saw_background_request = false;
  {
    std::lock_guard lock(*background_mutex);
    saw_background_request = !background_requests->empty();
  }
  auto parent_entries = store.load();
  bool const saw_parent_error = parent_entries && std::ranges::any_of(*parent_entries, [](ava::session::SessionEntry const& entry) {
                                  return entry.type == ava::session::EntryType::Error &&
                                         entry.data_json.find("fake transport has no response") != std::string::npos &&
                                         entry.data_json.find("background_task_id") != std::string::npos;
                                });
  bool saw_child_error = false;
  if (failed)
  {
    auto child_store = ava::session::SessionStore::open(workspace, failed->job.identity.child_session_id, session_root);
    if (child_store)
    {
      auto child_entries = child_store->load();
      saw_child_error = child_entries && std::ranges::any_of(*child_entries, [](ava::session::SessionEntry const& entry) {
                          return entry.type == ava::session::EntryType::Error && entry.data_json.find("subagent job failed") != std::string::npos &&
                                 entry.data_json.find("fake transport has no response") == std::string::npos;
                        });
    }
  }
  expect(saw_background_request, "background failure test reaches the child provider transport");
  expect(!saw_parent_error, "background task failures are not appended directly into the parent session");
  expect(saw_child_error, "background task failures are persisted in the child session with bounded sanitized content");
}

void test_agent_loop_background_task_cancel_requests_child_cancellation()
{
  auto const root = create_empty_root("agent-task-background-cancel");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const session_root = root / "sessions";
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "parent-bg-cancel"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(), "background cancel fixture creates coordinator");
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  auto background_state = std::make_shared<BlockingBackgroundTransport::State>();
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_task\",\"name\":\"task\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_task\",\"delta\":\"{"
                                                    "\\\"description\\\":\\\"Cancel async\\\",\\\"prompt\\\":\\\"Wait until canceled.\\\","
                                                    "\\\"subagent_type\\\":\\\"general\\\",\\\"background\\\":true}\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"queued cancel\"}\n\n"
                                                    "data: [DONE]\n\n")});
  auto parent_append = append_route_for_test(store);
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .background_provider_factory = []() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
        std::unique_ptr<ava::provider::Provider> provider = std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test");
        return provider;
      },
      .background_transport_factory = [background_state]() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
        std::unique_ptr<ava::http::Transport> transport = std::make_unique<BlockingBackgroundTransport>(
            background_state, sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"should not complete\"}\n\n"
                                           "data: [DONE]\n\n"));
        return transport;
      },
      .subagent_coordinator = coordinator,
      .append_entry = parent_append,
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("delegate cancelable background", store, provider, transport);
  expect(result && result->final_text == "queued cancel" && result->tool_calls == 1, "agent loop can queue a cancelable background task");
  auto jobs = coordinator->list(store.session_id());
  expect(jobs.size() == 1 && jobs.front().job.execution == ava::agent::SubagentExecutionState::Running, "cancelable background task is running in coordinator");
  expect(background_state->wait_for_request(std::chrono::milliseconds(1000)), "cancel test background child reaches provider transport");
  if (!jobs.empty())
  {
    auto canceled = coordinator->cancel(store.session_id(), jobs.front().job.identity.job_id);
    background_state->notify();
    expect(canceled && canceled->job.cancel_requested, "background coordinator cancel requests stop");
    expect(background_state->wait_for_cancel(std::chrono::milliseconds(1000)), "background child transport observes cancellation");
    auto final = coordinator->wait(store.session_id(), jobs.front().job.identity.job_id, std::chrono::milliseconds(1000));
    expect(final && final->job.execution == ava::agent::SubagentExecutionState::Canceled, "background coordinator marks canceled child jobs canceled");
    expect(final && !final->job.error, "background coordinator canceled job snapshots do not carry failure errors");
    auto result_snapshot =
        final ? coordinator->result(store.session_id(), final->job.identity.job_id)
              : ava::core::Result<ava::agent::SubagentCoordinatorJobSnapshot>(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "missing")));
    expect(result_snapshot && result_snapshot->job.execution == ava::agent::SubagentExecutionState::Canceled,
           "canceled background task result remains queryable through the coordinator");

    bool saw_child_cancel = false;
    if (final)
    {
      auto child_store = ava::session::SessionStore::open(workspace, final->job.identity.child_session_id, session_root);
      if (child_store)
      {
        auto child_entries = child_store->load();
        saw_child_cancel = child_entries && std::ranges::any_of(*child_entries, [](ava::session::SessionEntry const& entry) {
                             return entry.type == ava::session::EntryType::Cancel && entry.data_json.find("cancel_requested") != std::string::npos;
                           });
      }
    }
    expect(saw_child_cancel, "canceled background child records cancellation in its child session");
  }
}

void test_agent_loop_background_task_requires_coordinator()
{
  auto const root = create_empty_root("agent-task-background-no-coordinator");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "no-coordinator"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_task\",\"name\":\"task\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_task\",\"delta\":\"{"
                                                    "\\\"description\\\":\\\"No coordinator\\\",\\\"prompt\\\":\\\"Try background.\\\","
                                                    "\\\"subagent_type\\\":\\\"general\\\",\\\"background\\\":true}\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"handled\"}\n\n"
                                                    "data: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .background_provider_factory = []() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
        std::unique_ptr<ava::provider::Provider> provider = std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test");
        return provider;
      },
      .background_transport_factory = []() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
        std::unique_ptr<ava::http::Transport> transport = std::make_unique<ava::tests::FakeTransport>(std::vector<ava::http::HttpResponse>{});
        return transport;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("delegate unavailable background", store, provider, transport);
  expect(result && result->final_text == "handled" && transport.requests().size() == 2,
         "agent loop continues after unavailable background coordinator tool error");
  if (transport.requests().size() == 2)
  {
    expect(transport.requests()[1].body.find("background task subagents are unavailable") != std::string::npos,
           "background task requires an explicit coordinator owner");
  }
}

void test_agent_loop_coordinator_start_failure_rolls_back_child()
{
  auto const root = temp_root() / "agent-task-background-start-failure";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  ava::agent::SubagentCoordinatorOptions coordinator_options;
  coordinator_options.registry_options.thread_start_preflight = []() -> ava::core::VoidResult {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "injected thread start failure"));
  };
  auto coordinator = ava::agent::SubagentCoordinator::create(std::move(coordinator_options));
  expect(coordinator.has_value(), "start-failure rollback fixture creates coordinator");
  if (!coordinator)
    return;
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const session_root = root / "sessions";
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "parent-start-failure"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_task\",\"name\":\"task\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_task\",\"delta\":\"{"
                                                    "\\\"description\\\":\\\"Reject start\\\",\\\"prompt\\\":\\\"Never run.\\\","
                                                    "\\\"subagent_type\\\":\\\"general\\\",\\\"background\\\":true}\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"start rejected\"}\n\n"
                                                    "data: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .background_provider_factory = []() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
        std::unique_ptr<ava::provider::Provider> provider = std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test");
        return provider;
      },
      .background_transport_factory = []() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
        std::unique_ptr<ava::http::Transport> transport = std::make_unique<ava::tests::FakeTransport>(std::vector<ava::http::HttpResponse>{});
        return transport;
      },
      .subagent_coordinator = *coordinator,
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("delegate rejected background", store, provider, transport);
  expect(result && result->final_text == "start rejected" && transport.requests().size() == 2,
         "parent continues after coordinator rejects background publication");
  bool saw_failure = transport.requests().size() == 2 && transport.requests()[1].body.find("injected thread start failure") != std::string::npos;
  std::size_t session_files = 0;
  if (std::filesystem::exists(session_root))
    for (auto const& entry : std::filesystem::recursive_directory_iterator(session_root))
      session_files += entry.is_regular_file() && entry.path().extension() == ".jsonl";
  expect(saw_failure, "coordinator start failure is returned to the parent tool continuation");
  expect(session_files == 1, "proven-unpublished start failure rolls back the newly created child session file");
  expect((*coordinator)->list("parent-start-failure").empty(), "start failure prevents live worker publication");
}

void test_agent_loop_background_task_publishes_inspection_source()
{
  auto const root = create_empty_root("agent-task-background-inspect");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto const session_root = root / "sessions";
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = session_root, .workspace_dir = workspace, .session_id = "parent-inspect"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  auto coordinator_result = ava::agent::SubagentCoordinator::create();
  expect(coordinator_result.has_value(), "background inspect fixture creates coordinator");
  if (!coordinator_result)
    return;
  auto coordinator = *coordinator_result;
  auto background_state = std::make_shared<BlockingBackgroundTransport::State>();
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_task\",\"name\":\"task\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_task\",\"delta\":\"{"
                                                    "\\\"description\\\":\\\"Inspect child\\\",\\\"prompt\\\":\\\"Say hello from child.\\\","
                                                    "\\\"subagent_type\\\":\\\"general\\\",\\\"background\\\":true}\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"queued\"}\n\n"
                                                    "data: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolution::Allow;
      },
      .background_provider_factory = []() -> ava::core::Result<std::unique_ptr<ava::provider::Provider>> {
        std::unique_ptr<ava::provider::Provider> provider = std::make_unique<ava::provider::OpenAIProvider>("https://api.example.test");
        return provider;
      },
      .background_transport_factory = [background_state]() -> ava::core::Result<std::unique_ptr<ava::http::Transport>> {
        std::unique_ptr<ava::http::Transport> transport = std::make_unique<BlockingBackgroundTransport>(
            background_state, sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"child hello\"}\n\n"
                                           "data: [DONE]\n\n"));
        return transport;
      },
      .subagent_coordinator = coordinator,
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("delegate background inspect", store, provider, transport);
  expect(result && result->final_text == "queued", "background inspect parent turn completes");
  auto jobs = coordinator->list(store.session_id());
  expect(jobs.size() == 1, "background inspect job is published");
  if (jobs.empty())
    return;

  expect(background_state->wait_for_request(std::chrono::milliseconds(1000)), "background child reaches provider");

  // While the child is running, the coordinated source must already expose the
  // committed user prompt prefix without leaking session paths through inspect.
  auto live = coordinator->inspect(store.session_id(), jobs.front().job.identity.job_id);
  expect(live && !(*live)->unavailable && !(*live)->terminal && !(*live)->messages.empty(),
         "coordinated background task publishes an inspection source before completion");
  if (live && !(*live)->messages.empty())
  {
    bool saw_prompt = false;
    for (auto const& message : (*live)->messages)
    {
      if (message.role == ava::agent::SubagentLiveMessageRole::User && message.text.find("Say hello from child.") != std::string::npos)
        saw_prompt = true;
    }
    expect(saw_prompt, "live inspect projects the committed child user prompt prefix");
  }

  background_state->release_success();
  auto terminal = coordinator->wait(store.session_id(), jobs.front().job.identity.job_id, std::chrono::seconds(2));
  expect(terminal && !terminal->timed_out && terminal->job.execution == ava::agent::SubagentExecutionState::Completed,
         "background inspect child reaches terminal completion");
  auto frozen = coordinator->inspect(store.session_id(), jobs.front().job.identity.job_id);
  expect(frozen && (*frozen)->terminal && !(*frozen)->unavailable, "terminal background child freezes a path-free inspection frame");
  if (frozen)
  {
    bool saw_assistant = false;
    for (auto const& message : (*frozen)->messages)
      if (message.role == ava::agent::SubagentLiveMessageRole::Assistant && message.text.find("child hello") != std::string::npos)
        saw_assistant = true;
    expect(saw_assistant, "frozen frame includes committed assistant transcript text only");
  }
}
