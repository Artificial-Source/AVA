#include "sys.h"
#include "tests/support/agent_loop_test_support.h"
#include "tests/support/test_timeout.h"
#include "ava/http/transport.h"
#include "ava/session/assistant_output.h"
#include "ava/core/json.h"

#include <algorithm>
#include <expected>
#include <fstream>
#include <optional>
#include <ranges>
#include <sstream>
#include <utility>

namespace agent_loop_test {

void TraceCollector::on_event(ava::observability::TraceEvent const& event)
{
  std::lock_guard lock(mutex);
  events.push_back(event);
}

ava::http::HttpResponse sse_response(std::string const& body)
{
  return ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = body};
}

std::string tool_call_sse(std::string_view id, std::string_view name, std::string_view arguments_json)
{
  return "data: {\"type\":\"response.function_call.added\",\"call_id\":\"" + ava::core::json::escape(id) + "\",\"name\":\"" + ava::core::json::escape(name) +
         "\"}\n\n" + "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"" + ava::core::json::escape(id) + "\",\"delta\":\"" +
         ava::core::json::escape(arguments_json) + "\"}\n\n" + "data: {\"type\":\"response.function_call.done\",\"call_id\":\"" + ava::core::json::escape(id) +
         "\"}\n\n";
}

ava::agent::ModelInvocationOptions model_invocation_options(std::string system_prompt, std::string provider_id, std::string model_id)
{
  return ava::agent::ModelInvocationOptions{.provider_id = std::move(provider_id), .model_id = std::move(model_id), .system_prompt = std::move(system_prompt)};
}
AgentLoopFixture::AgentLoopFixture(std::string_view name, ava::agent::Mode initial_mode)
    : root(ScopedTestDirectory::create_unique(temp_root(), name)),
      workspace(root / "workspace"),
      store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = std::string(name)}),
      mode(initial_mode)
{
  std::filesystem::create_directories(workspace);
}

ava::agent::AgentLoopOptions AgentLoopFixture::options(ava::agent::ModelInvocationOptions model) const
{
  ava::agent::AgentLoopOptions options{};
  options.workspace_dir = workspace;
  options.mode = mode;
  options.model = std::move(model);
  options.access_token = "token";
  options.append_entry = append_route_for_test(store);
  options.append_batch = append_batch_route_for_test(store);
  options.session_read_authority = read_authority_for_test(store);
  return options;
}

std::vector<std::string> committed_function_call_ids(std::vector<ava::session::SessionEntry> const& entries)
{
  std::vector<std::string> ids;
  auto const projection = ava::session::classify_assistant_output(entries);
  for (auto const& turn : projection.turns)
    for (auto const& item : turn.items)
      if (auto const* function = std::get_if<ava::session::AssistantOutputFunctionCall>(&item.item.payload))
        ids.push_back(function->call_id);
  return ids;
}

std::vector<std::string> bound_tool_result_call_ids(std::vector<ava::session::SessionEntry> const& entries)
{
  std::vector<std::string> ids;
  for (auto const& entry : entries)
    if (entry.type == ava::session::EntryType::ToolResult && ava::core::json::field_value_start(entry.data_json, "assistant_output_entry_id"))
      ids.push_back(ava::core::json::string_field(entry.data_json, "call_id").value_or(""));
  return ids;
}
std::vector<ava::agent::SubagentCoordinatorJobSnapshot> wait_for_published_running_job(ava::agent::SubagentCoordinator const& coordinator,
                                                                                       std::string const& parent_session_id, std::size_t expected_total_count,
                                                                                       std::chrono::milliseconds timeout)
{
  auto const deadline = ava::tests::now_plus_seconds(timeout);
  while (true)
  {
    auto jobs = coordinator.list(parent_session_id);
    // Transport request readiness does not guarantee coordinator publication.
    if (jobs.size() == expected_total_count &&
        std::ranges::any_of(jobs, [](auto const& snapshot) { return snapshot.job.execution == ava::agent::SubagentExecutionState::Running; }))
      return jobs;
    if (std::chrono::steady_clock::now() >= deadline)
      return jobs;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void ChildSchedulingWitness::record_trace(ava::observability::TraceEvent const& event)
{
  std::optional<ChildSchedulingEvent::Kind> kind;
  if (event.type == ava::observability::TraceEventType::AgentRunStart)
    kind = ChildSchedulingEvent::Kind::RunStart;
  else if (event.type == ava::observability::TraceEventType::ToolDispatchStart)
    kind = ChildSchedulingEvent::Kind::ToolStart;
  if (!kind)
    return;
  std::string tool_name;
  for (auto const& field : event.fields)
    if (field.key == "tool_name")
      tool_name = field.value;
  std::lock_guard lock(mutex_);
  events_.push_back(ChildSchedulingEvent{.kind = *kind,
                                         .session_id = event.session_id,
                                         .parent_session_id = event.parent_session_id,
                                         .tool_name = std::move(tool_name),
                                         .file_name = {},
                                         .thread = std::this_thread::get_id(),
                                         .order = next_order_++});
}

void ChildSchedulingWitness::record_file_read(std::filesystem::path const& absolute_path)
{
  std::lock_guard lock(mutex_);
  events_.push_back(ChildSchedulingEvent{.kind = ChildSchedulingEvent::Kind::FileRead,
                                         .session_id = {},
                                         .parent_session_id = {},
                                         .tool_name = {},
                                         .file_name = absolute_path.filename().string(),
                                         .thread = std::this_thread::get_id(),
                                         .order = next_order_++});
}

std::vector<ChildSchedulingEvent> ChildSchedulingWitness::snapshot() const
{
  std::lock_guard lock(mutex_);
  return events_;
}

ChildSchedulingObserver::ChildSchedulingObserver(std::shared_ptr<ChildSchedulingWitness> witness) : witness_(std::move(witness))
{
}

void ChildSchedulingObserver::on_event(ava::observability::TraceEvent const& event)
{
  witness_->record_trace(event);
}

ChildSchedulingExactFileAccess::ChildSchedulingExactFileAccess(std::shared_ptr<ChildSchedulingWitness> witness) : witness_(std::move(witness))
{
}

ava::core::Result<std::string> ChildSchedulingExactFileAccess::read_text_file(std::filesystem::path const& absolute_path,
                                                                              ava::tools::ToolIoCancelCallback) const
{
  witness_->record_file_read(absolute_path);
  std::ifstream file(absolute_path, std::ios::binary);
  if (!file)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::NotFound, "child scheduling fixture file is missing"));
  std::ostringstream content;
  content << file.rdbuf();
  return content.str();
}

ava::core::VoidResult ChildSchedulingExactFileAccess::write_text_file(std::filesystem::path const&, std::string_view, ava::tools::ToolIoCancelCallback) const
{
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::PermissionDenied, "child scheduling fixture is read-only"));
}

SharedFakeTransport::SharedFakeTransport(std::shared_ptr<std::vector<ava::http::HttpResponse>> responses,
                                         std::shared_ptr<std::vector<ava::http::HttpRequest>> requests, std::shared_ptr<std::mutex> mutex)
    : responses_(std::move(responses)), requests_(std::move(requests)), mutex_(std::move(mutex))
{
}

ava::core::Result<ava::http::HttpResponse> SharedFakeTransport::send(ava::http::HttpRequest const& request)
{
  std::lock_guard lock(*mutex_);
  requests_->push_back(request);
  if (responses_->empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "fake transport has no response"));
  }
  auto response = responses_->front();
  responses_->erase(responses_->begin());
  return response;
}

void BlockingSequenceTransport::State::release_success()
{
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  changed.notify_all();
}

bool BlockingSequenceTransport::State::wait_for_requests(std::size_t count, std::chrono::milliseconds timeout)
{
  std::unique_lock lock(mutex);
  return changed.wait_until(lock, ava::tests::now_plus_seconds(timeout), [&] { return requests.size() >= count; });
}

bool BlockingSequenceTransport::State::wait_for_cancel(std::chrono::milliseconds timeout)
{
  std::unique_lock lock(mutex);
  return changed.wait_until(lock, ava::tests::now_plus_seconds(timeout), [&] { return cancel_observed; });
}

std::vector<ava::http::HttpRequest> BlockingSequenceTransport::State::requests_snapshot()
{
  std::lock_guard lock(mutex);
  return requests;
}

BlockingSequenceTransport::BlockingSequenceTransport(std::shared_ptr<State> state, std::vector<ava::http::HttpResponse> responses)
    : state_(std::move(state)), responses_(std::move(responses))
{
}

ava::core::Result<ava::http::HttpResponse> BlockingSequenceTransport::send(ava::http::HttpRequest const& request)
{
  return send(request, nullptr);
}

ava::core::Result<ava::http::HttpResponse> BlockingSequenceTransport::send(ava::http::HttpRequest const& request, CancelCallback cancel_requested)
{
  std::unique_lock lock(state_->mutex);
  auto const index = state_->requests.size();
  state_->requests.push_back(request);
  state_->changed.notify_all();
  while (index == state_->blocked_request_index && !state_->release && !(cancel_requested && cancel_requested()))
    state_->changed.wait_for(lock, std::chrono::milliseconds(1));
  if (cancel_requested && cancel_requested())
  {
    state_->cancel_observed = true;
    state_->changed.notify_all();
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "transport request canceled"));
  }
  if (index >= responses_.size())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "blocking sequence transport has no response"));
  return responses_[index];
}

void BlockingBackgroundTransport::State::release_success()
{
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  changed.notify_all();
}

void BlockingBackgroundTransport::State::notify()
{
  changed.notify_all();
}

bool BlockingBackgroundTransport::State::wait_for_request(std::chrono::milliseconds timeout)
{
  std::unique_lock lock(mutex);
  return changed.wait_until(lock, ava::tests::now_plus_seconds(timeout), [&] { return request_seen; });
}

bool BlockingBackgroundTransport::State::wait_for_cancel(std::chrono::milliseconds timeout)
{
  std::unique_lock lock(mutex);
  return changed.wait_until(lock, ava::tests::now_plus_seconds(timeout), [&] { return cancel_observed; });
}

std::vector<ava::http::HttpRequest> BlockingBackgroundTransport::State::requests_snapshot()
{
  std::lock_guard lock(mutex);
  return requests;
}

BlockingBackgroundTransport::BlockingBackgroundTransport(std::shared_ptr<State> state, ava::http::HttpResponse response)
    : state_(std::move(state)), response_(std::move(response))
{
}

ava::core::Result<ava::http::HttpResponse> BlockingBackgroundTransport::send(ava::http::HttpRequest const& request)
{
  return send(request, nullptr);
}

ava::core::Result<ava::http::HttpResponse> BlockingBackgroundTransport::send(ava::http::HttpRequest const& request, CancelCallback cancel_requested)
{
  std::unique_lock lock(state_->mutex);
  state_->requests.push_back(request);
  state_->request_seen = true;
  state_->changed.notify_all();
  state_->changed.wait(lock, [&] { return state_->release || (cancel_requested && cancel_requested()); });
  if (cancel_requested && cancel_requested())
  {
    state_->cancel_observed = true;
    state_->changed.notify_all();
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "transport request canceled"));
  }
  return response_;
}

}  // namespace agent_loop_test
