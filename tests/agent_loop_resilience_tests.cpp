#include "sys.h"
#include "tests/support/agent_loop_test_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/app/session_run_controller.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/mode.h"
#include "ava/session/assistant_output.h"
#include "ava/session/session_store.h"
#include "ava/session/validation.h"
#include "ava/provider/anthropic_provider.h"
#include "ava/provider/gemini_provider.h"
#include "ava/provider/openai_compatible_provider.h"
#include "ava/provider/openai_provider.h"
#include "ava/provider/provider.h"
#include "ava/core/json.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>
#include <sys/stat.h>

namespace {

ava::http::HttpResponse sse_response(std::string const& body)
{
  return ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = body};
}

class CallbackTransport final : public ava::http::Transport
{
 public:
  CallbackTransport(std::vector<ava::http::HttpResponse> responses, std::function<void()> after_send)
      : responses_(std::move(responses)), after_send_(std::move(after_send))
  {
  }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override
  {
    requests_.push_back(request);
    if (responses_.empty())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "callback transport has no response"));
    }
    auto response = responses_.front();
    responses_.erase(responses_.begin());
    if (after_send_)
      after_send_();
    return response;
  }

  [[nodiscard]] std::vector<ava::http::HttpRequest> const& requests() const noexcept { return requests_; }

 private:
  std::vector<ava::http::HttpResponse> responses_;
  std::function<void()> after_send_;
  std::vector<ava::http::HttpRequest> requests_;
};

class SequencedProviderTransport final : public ava::http::Transport
{
 public:
  SequencedProviderTransport(std::vector<ava::http::HttpResponse> responses, bool streaming) : responses_(std::move(responses)), streaming_(streaming) { }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const& request) override
  {
    requests_.push_back(request);
    return take_response();
  }

  [[nodiscard]] bool supports_streaming() const noexcept override { return streaming_; }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send_streaming(ava::http::HttpRequest const& request, BodyChunkSink on_body_chunk,
                                                                          CancelCallback cancel_requested) override
  {
    requests_.push_back(request);
    if (cancel_requested && cancel_requested())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "sequenced provider stream canceled"));
    auto response = take_response();
    if (!response)
      return std::unexpected(std::move(response.error()));
    if (response->status_code >= 200 && response->status_code < 300 && on_body_chunk)
    {
      auto const midpoint = response->body.size() / 2;
      if (auto first = on_body_chunk(std::string_view(response->body).substr(0, midpoint)); !first)
        return std::unexpected(std::move(first.error()));
      if (auto second = on_body_chunk(std::string_view(response->body).substr(midpoint)); !second)
        return std::unexpected(std::move(second.error()));
    }
    return response;
  }

  [[nodiscard]] std::vector<ava::http::HttpRequest> const& requests() const noexcept { return requests_; }

 private:
  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> take_response()
  {
    if (responses_.empty())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "sequenced provider transport has no response"));
    auto response = std::move(responses_.front());
    responses_.erase(responses_.begin());
    return response;
  }

  std::vector<ava::http::HttpResponse> responses_;
  bool streaming_ = false;
  std::vector<ava::http::HttpRequest> requests_;
};

std::string chat_stream_tool_call(std::string_view call_id, std::string_view name, std::string_view arguments, std::string_view finish_reason)
{
  return "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"" + ava::core::json::escape(call_id) + "\",\"function\":{\"name\":\"" +
         ava::core::json::escape(name) + "\",\"arguments\":\"" + ava::core::json::escape(arguments) +
         "\"}}]}}]}\n\n"
         "data: {\"choices\":[{\"finish_reason\":\"" +
         ava::core::json::escape(finish_reason) + "\"}]}\n\ndata: [DONE]\n\n";
}

std::string anthropic_stream_tool_call(std::string_view call_id, std::string_view name, std::string_view arguments, std::string_view stop_reason)
{
  return "event: content_block_start\n"
         "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"" +
         ava::core::json::escape(call_id) + "\",\"name\":\"" + ava::core::json::escape(name) +
         "\",\"input\":{}}}\n\n"
         "event: content_block_delta\n"
         "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"" +
         ava::core::json::escape(arguments) +
         "\"}}\n\n"
         "event: content_block_stop\n"
         "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
         "event: message_delta\n"
         "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"" +
         ava::core::json::escape(stop_reason) +
         "\"}}\n\n"
         "event: message_stop\n"
         "data: {\"type\":\"message_stop\"}\n\n";
}

void test_agent_loop_cancellation_boundaries()
{
  ava::provider::OpenAIProvider const provider("https://api.example.test");

  {
    auto const root = create_empty_root("agent-cancel-before-turn-start");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "cancel-before-turn-start"});
    ava::tests::FakeTransport transport(
        {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"should not send\"}\n\n"
                      "data: [DONE]\n\n")});
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .cancel_requested = [] { return true; },
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("cancel now", store, provider, transport);
    auto entries = store.load();
    bool saw_user_message = false;
    bool saw_cancel = false;
    if (entries)
    {
      for (auto const& entry : *entries)
      {
        saw_user_message = saw_user_message || entry.type == ava::session::EntryType::UserMessage;
        saw_cancel = saw_cancel || (entry.type == ava::session::EntryType::Cancel && entry.data_json.find("before_turn_start") != std::string::npos);
      }
    }
    expect(!result && result.error().message() == "agent loop canceled" && transport.requests().empty() && entries && saw_cancel && !saw_user_message,
           "agent loop cancellation before turn start avoids persisting the user message");
  }

  {
    auto const root = create_empty_root("agent-cancel-before-provider");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "cancel-before-provider"});
    ava::tests::FakeTransport transport(
        {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"should not send\"}\n\n"
                      "data: [DONE]\n\n")});
    int cancel_checks = 0;
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .cancel_requested =
            [&cancel_checks] {
              ++cancel_checks;
              return cancel_checks >= 2;
            },
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("cancel before provider", store, provider, transport);
    auto entries = store.load();
    bool const saw_cancel = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                              return entry.type == ava::session::EntryType::Cancel && entry.data_json.find("before_provider_call") != std::string::npos;
                            });
    bool const saw_user_message =
        entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::UserMessage; });
    expect(!result && result.error().message() == "agent loop canceled" && transport.requests().empty() && saw_cancel && saw_user_message,
           "agent loop cancellation before provider call avoids transport send and records cancel boundary");
  }

  {
    auto const root = create_empty_root("agent-cancel-before-tool");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    {
      std::ofstream file(workspace / "note.txt", std::ios::binary | std::ios::trunc);
      file << "must not read";
    }
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "cancel-before-tool"});
    bool cancel = false;
    CallbackTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_read\",\"name\":\"read_file\"}\n\n"
                                              "data: "
                                              "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_read\",\"delta\":\"{\\\"path\\\":"
                                              "\\\"note.txt\\\"}\"}\n\n"
                                              "data: [DONE]\n\n")},
                                [&cancel] { cancel = true; });
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .cancel_requested = [&cancel] { return cancel; },
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("read then cancel", store, provider, transport);
    auto entries = store.load();
    bool saw_tool_entry = false;
    bool saw_v4_output = false;
    bool saw_cancel = false;
    if (entries)
    {
      for (auto const& entry : *entries)
      {
        saw_tool_entry = saw_tool_entry || entry.type == ava::session::EntryType::ToolCall || entry.type == ava::session::EntryType::ToolResult;
        saw_v4_output =
            saw_v4_output || entry.type == ava::session::EntryType::AssistantOutputItem || entry.type == ava::session::EntryType::AssistantTurnCommit;
        saw_cancel = saw_cancel || (entry.type == ava::session::EntryType::Cancel && (entry.data_json.find("before_tool_dispatch") != std::string::npos ||
                                                                                      entry.data_json.find("during_provider_request") != std::string::npos ||
                                                                                      entry.data_json.find("after_provider_call") != std::string::npos));
      }
    }
    expect(!result && result.error().message() == "agent loop canceled" && transport.requests().size() == 1 && entries && saw_cancel && !saw_tool_entry &&
               !saw_v4_output,
           "cancellation before the assistant batch writes no v4 staging or commit records");
  }

  {
    auto const root = create_empty_root("agent-cancel-during-bash-tool");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
           "agent bash cancellation workspace is owner-only for sealed planning");
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "cancel-during-bash-tool"});
    ava::tests::FakeTransport transport(
        {sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_bash\",\"name\":\"bash\"}\n\n"
                      "data: "
                      "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_bash\",\"delta\":\"{\\\"command\\\":"
                      "\\\"sleep 2\\\",\\\"timeout_ms\\\":5000}\"}\n\n"
                      "data: [DONE]\n\n")});
    bool bash_started = false;
    int bash_cancel_checks = 0;
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .anchor_set = command_anchors_for_test(workspace, store.session_path().parent_path() / "spill"),
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .on_tool_event =
            [&bash_started](auto const& entry) {
              if (entry.status == ava::agent::ToolTimelineStatus::Running && entry.name == "bash")
              {
                bash_started = true;
              }
            },
        .permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          return ava::permissions::PermissionResolution::Allow;
        },
        .cancel_requested =
            [&bash_started, &bash_cancel_checks] {
              if (!bash_started)
                return false;
              ++bash_cancel_checks;
              return bash_cancel_checks >= 3;
            },
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("sleep then cancel", store, provider, transport);
    auto entries = store.load();
    bool const saw_canceled_tool_result = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                                            return entry.type == ava::session::EntryType::ToolResult &&
                                                   entry.data_json.find("\"status\":\"canceled\"") != std::string::npos &&
                                                   entry.data_json.find("\"canceled\":true") != std::string::npos;
                                          });
    bool const saw_after_tool_cancel =
        entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
          return entry.type == ava::session::EntryType::Cancel && entry.data_json.find("after_tool_dispatch") != std::string::npos;
        });
    expect(
        !result && result.error().message() == "agent loop canceled" && transport.requests().size() == 1 && saw_canceled_tool_result && saw_after_tool_cancel,
        "agent loop propagates cancellation into bash tools and persists a canceled tool result");
  }

  {
    auto const root = create_empty_root("agent-cancel-after-committed-before-first-tool");
    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "cancel-after-commit"});
    ava::tests::FakeTransport transport({sse_response(
        "data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_unstarted\",\"name\":\"read_file\"}\n\n"
        "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_unstarted\",\"delta\":\"{\\\"path\\\":\\\"never-read.txt\\\"}\"}\n\n"
        "data: [DONE]\n\n")});
    bool cancel = false;
    std::size_t tool_events = 0;
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .on_tool_event = [&tool_events](ava::agent::ToolTimelineEntry const&) { ++tool_events; },
        .cancel_requested = [&cancel] { return cancel; },
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
        .on_phase = [&cancel](ava::agent::RunPhase phase) -> ava::core::VoidResult {
          if (phase == ava::agent::RunPhase::PreparingTools)
            cancel = true;
          return {};
        },
    });
    auto result = loop.run_turn("cancel after assistant commit", store, provider, transport);
    auto entries = store.load();
    std::size_t functions = 0;
    std::size_t bound_canceled_results = 0;
    if (entries)
    {
      auto projection = ava::session::classify_assistant_output(*entries);
      for (auto const& turn : projection.turns)
        for (auto const& item : turn.items) functions += std::holds_alternative<ava::session::AssistantOutputFunctionCall>(item.item.payload);
      for (auto const& entry : *entries)
        bound_canceled_results += entry.type == ava::session::EntryType::ToolResult && entry.data_json.find("call_unstarted") != std::string::npos &&
                                  entry.data_json.find("\"status\":\"canceled\"") != std::string::npos &&
                                  entry.data_json.find("execution_outcome_unknown") != std::string::npos;
    }
    auto validation = entries ? ava::session::validate_session_replay(*entries) : ava::session::SessionReplayValidation{};
    expect(!result && result.error().message() == "agent loop canceled" && entries && functions == 1 && bound_canceled_results == 1 && tool_events == 0 &&
               validation.ok(),
           "cancellation after a v4 commit but before first dispatch closes its exact binding without executing the tool");
  }
}

void test_agent_loop_error_paths_and_bounds()
{
  ava::provider::OpenAIProvider const provider("https://api.example.test");

  {
    auto const root = create_empty_root("agent-provider-error");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "provider-error"});
    ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.error\",\"error\":{\"message\":\"bad request\"}}\n\n")});
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("hi", store, provider, transport);
    expect(!result && result.error().message().find("provider stream error") != std::string::npos, "agent loop returns provider error events");
  }

  {
    auto const root = create_empty_root("agent-empty-transport");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "empty-transport"});
    ava::tests::FakeTransport transport({});
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("hi", store, provider, transport);
    expect(!result && result.error().message().find("fake transport has no response") != std::string::npos, "agent loop returns transport failures");
  }

  {
    auto const root = create_empty_root("agent-empty-response");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "empty-response"});
    ava::tests::FakeTransport transport({sse_response("")});
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("hi", store, provider, transport);
    expect(!result && result.error().message().find("empty") != std::string::npos, "agent loop returns empty provider responses");
  }

  {
    auto const root = create_empty_root("agent-event-bound");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "event-bound"});
    ava::tests::FakeTransport transport(
        {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"a\"}\n\n"
                      "data: [DONE]\n\n")});
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .max_provider_events = 1,
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("hi", store, provider, transport);
    expect(!result && result.error().message().find("event limit") != std::string::npos, "agent loop enforces provider event bounds");
  }

  {
    auto const root = create_empty_root("agent-text-bound");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "text-bound"});
    ava::tests::FakeTransport transport(
        {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"hello\"}\n\n"
                      "data: [DONE]\n\n")});
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .max_assistant_text_bytes = 3,
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("hi", store, provider, transport);
    expect(!result && result.error().message().find("text byte limit") != std::string::npos, "agent loop enforces assistant text byte bounds");
  }

  {
    auto const root = create_empty_root("agent-arg-bound");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "arg-bound"});
    ava::tests::FakeTransport transport(
        {sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_1\",\"name\":\"write_file\"}\n\n"
                      "data: "
                      "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_1\",\"delta\":\"{\\\"path\\\":"
                      "\\\"parser-limit-must-not-exist.txt\\\",\\\"content\\\":\\\"forbidden\\\"}\"}\n\n"
                      "data: [DONE]\n\n")});
    int permission_requests = 0;
    int tool_events = 0;
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .max_tool_argument_bytes = 5,
        .tool_execution = ava::agent::ToolExecutionOptions{.require_explicit_file_permissions = true},
        .on_tool_event = [&tool_events](ava::agent::ToolTimelineEntry const&) { ++tool_events; },
        .permission_resolver =
            [&permission_requests](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          ++permission_requests;
          return ava::permissions::PermissionResolution::Allow;
        },
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("hi", store, provider, transport);
    auto entries = store.load();
    bool const has_tool_lifecycle = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                                      return entry.type == ava::session::EntryType::AssistantOutputItem ||
                                             entry.type == ava::session::EntryType::AssistantTurnCommit || entry.type == ava::session::EntryType::ToolResult;
                                    });
    expect(!result && result.error().message().find("argument byte limit") != std::string::npos && permission_requests == 0 && tool_events == 0 && entries &&
               !has_tool_lifecycle && !std::filesystem::exists(workspace / "parser-limit-must-not-exist.txt"),
           "agent loop parser limits fail closed before persistence, permission, dispatch, or side effects");
  }

  {
    auto const root = create_empty_root("agent-control-call-id");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "control-call-id"});
    ava::tests::FakeTransport transport(
        {sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_\\u0001bad\",\"name\":\"read_file\"}\n\n"
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
    auto result = loop.run_turn("bad id", store, provider, transport);
    auto entries = store.load();
    bool saw_tool_entry = false;
    if (entries)
    {
      saw_tool_entry = std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
        return entry.type == ava::session::EntryType::ToolCall || entry.type == ava::session::EntryType::ToolResult;
      });
    }
    expect(!result && result.error().message().find("control byte") != std::string::npos && entries && !saw_tool_entry,
           "agent loop rejects provider tool call ids with control bytes before session or timeline use");
  }

  {
    auto const root = create_empty_root("agent-long-call-id");

    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "long-call-id"});
    std::string const long_call_id(300, 'a');
    ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"" + long_call_id +
                                                      "\",\"name\":\"read_file\"}\n\n"
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
    auto result = loop.run_turn("long id", store, provider, transport);
    expect(!result && result.error().message().find("too long") != std::string::npos, "agent loop rejects overlong provider tool call ids");
  }
}

void test_agent_loop_truncated_provider_output_never_dispatches_tools()
{
  constexpr std::string_view sentinel_name = "truncated-call-must-not-run.txt";
  std::string const write_arguments = "{\"path\":\"" + std::string(sentinel_name) + "\",\"content\":\"forbidden provider side effect\"}";
  std::string const bash_arguments = "{\"command\":\"touch " + std::string(sentinel_name) + "\"}";
  std::string const malformed_bash_arguments = "{\"command\":\"touch " + std::string(sentinel_name) + "\"";

  ava::provider::OpenAICompatibleProvider const compatible_provider(ava::provider::OpenAICompatibleProviderOptions{
      .base_url = "https://compatible.example.test", .provider_name = "Compatible test", .require_credential = false});
  ava::provider::OpenAIProvider const openai_provider("https://openai.example.test");
  ava::provider::AnthropicProvider const anthropic_provider("https://anthropic.example.test");
  ava::provider::GeminiProvider const gemini_provider("https://gemini.example.test");

  std::string const compatible_final_stream =
      "data: {\"choices\":[{\"delta\":{\"content\":\"continued\"}}]}\n\n"
      "data: {\"choices\":[{\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
  std::string const openai_final_stream =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"continued\"}\n\n"
      "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n";
  std::string const anthropic_final_stream =
      "event: content_block_start\n"
      "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
      "event: content_block_delta\n"
      "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"continued\"}}\n\n"
      "event: content_block_stop\n"
      "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
      "event: message_delta\n"
      "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
      "event: message_stop\n"
      "data: {\"type\":\"message_stop\"}\n\n";
  std::string const gemini_final_stream =
      "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"continued\"}]},\"finishReason\":\"STOP\"}]}\n\n"
      "data: [DONE]\n\n";

  struct TruncationCase
  {
    std::string name;
    ava::provider::Provider const* provider = nullptr;
    std::string provider_id;
    std::string model_id;
    std::string api_family;
    bool stream = false;
    std::string truncated_body;
    std::string final_body;
    std::vector<std::string> call_ids;
  };

  std::vector<TruncationCase> const cases{
      TruncationCase{
          .name = "openai-compatible-buffered",
          .provider = &compatible_provider,
          .provider_id = "compatible",
          .model_id = "compatible-test",
          .api_family = "openai_chat_completions",
          .stream = false,
          .truncated_body = "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"chat_write\",\"function\":{\"name\":\"write_file\","
                            "\"arguments\":\"" +
                            ava::core::json::escape(write_arguments) + "\"}},{\"id\":\"chat_bash\",\"function\":{\"name\":\"bash\",\"arguments\":\"" +
                            ava::core::json::escape(bash_arguments) + "\"}}]},\"finish_reason\":\"length\"}]}",
          .final_body = R"({"choices":[{"message":{"content":"continued"},"finish_reason":"stop"}]})",
          .call_ids = {"chat_write", "chat_bash"},
      },
      TruncationCase{.name = "openai-compatible-streaming",
                     .provider = &compatible_provider,
                     .provider_id = "compatible",
                     .model_id = "compatible-test",
                     .api_family = "openai_chat_completions",
                     .stream = true,
                     .truncated_body = chat_stream_tool_call("chat_partial_bash", "bash", malformed_bash_arguments, "length"),
                     .final_body = compatible_final_stream,
                     .call_ids = {"chat_partial_bash"}},
      TruncationCase{
          .name = "openai-responses-buffered",
          .provider = &openai_provider,
          .provider_id = "openai",
          .model_id = "gpt-test",
          .api_family = "openai_responses",
          .stream = false,
          .truncated_body = "{\"status\":\"incomplete\",\"incomplete_details\":{\"reason\":\"max_tokens\"},\"output\":[{\"id\":\"fc_write\","
                            "\"type\":\"function_call\",\"call_id\":\"responses_write\",\"name\":\"write_file\",\"arguments\":\"" +
                            ava::core::json::escape(write_arguments) + "\"}]}",
          .final_body = R"({"status":"completed","output_text":"continued"})",
          .call_ids = {"responses_write"},
      },
      TruncationCase{
          .name = "openai-responses-streaming",
          .provider = &openai_provider,
          .provider_id = "openai",
          .model_id = "gpt-test",
          .api_family = "openai_responses",
          .stream = true,
          .truncated_body = agent_loop_test::tool_call_sse("responses_partial_bash", "bash", malformed_bash_arguments) +
                            "data: {\"type\":\"response.incomplete\",\"response\":{\"status\":\"incomplete\","
                            "\"incomplete_details\":{\"reason\":\"max_output_tokens\"}}}\n\n",
          .final_body = openai_final_stream,
          .call_ids = {"responses_partial_bash"},
      },
      TruncationCase{.name = "anthropic-buffered",
                     .provider = &anthropic_provider,
                     .provider_id = "anthropic",
                     .model_id = "claude-test",
                     .api_family = "anthropic_messages",
                     .stream = false,
                     .truncated_body = "{\"content\":[{\"type\":\"tool_use\",\"id\":\"anthropic_write\",\"name\":\"write_file\",\"input\":" + write_arguments +
                                       "}],\"stop_reason\":\"max_tokens\"}",
                     .final_body = R"({"content":[{"type":"text","text":"continued"}],"stop_reason":"end_turn"})",
                     .call_ids = {"anthropic_write"}},
      TruncationCase{.name = "anthropic-streaming",
                     .provider = &anthropic_provider,
                     .provider_id = "anthropic",
                     .model_id = "claude-test",
                     .api_family = "anthropic_messages",
                     .stream = true,
                     .truncated_body = anthropic_stream_tool_call("anthropic_partial_bash", "bash", malformed_bash_arguments, "max_tokens"),
                     .final_body = anthropic_final_stream,
                     .call_ids = {"anthropic_partial_bash"}},
      TruncationCase{.name = "gemini-buffered",
                     .provider = &gemini_provider,
                     .provider_id = "gemini",
                     .model_id = "gemini-test",
                     .api_family = "gemini_generate_content",
                     .stream = false,
                     .truncated_body = "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"functionCall\":{\"id\":\"gemini_write\","
                                       "\"name\":\"write_file\",\"args\":" +
                                       write_arguments + "}}]},\"finishReason\":\"MAX_TOKENS\"}]}",
                     .final_body = R"({"candidates":[{"content":{"role":"model","parts":[{"text":"continued"}]},"finishReason":"STOP"}]})",
                     .call_ids = {"gemini_write"}},
      TruncationCase{.name = "gemini-streaming",
                     .provider = &gemini_provider,
                     .provider_id = "gemini",
                     .model_id = "gemini-test",
                     .api_family = "gemini_generate_content",
                     .stream = true,
                     .truncated_body = "data: {\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":[{\"functionCall\":{\"id\":\"gemini_bash\","
                                       "\"name\":\"bash\",\"args\":" +
                                       bash_arguments + "}}]},\"finishReason\":\"MAX_TOKENS\"}]}\n\ndata: [DONE]\n\n",
                     .final_body = gemini_final_stream,
                     .call_ids = {"gemini_bash"}},
  };

  for (auto const& test_case : cases)
  {
    auto const root = create_empty_root("agent-truncated-provider-" + test_case.name);
    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0, test_case.name + " secures the command fixture roots");
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "truncated-provider"});
    SequencedProviderTransport transport({sse_response(test_case.truncated_body), sse_response(test_case.final_body)}, test_case.stream);
    int permission_requests = 0;
    int deny_preflights = 0;
    std::vector<ava::agent::ToolTimelineEntry> tool_events;
    auto model = agent_loop_test::model_invocation_options("system prompt", test_case.provider_id, test_case.model_id);
    model.api_family = test_case.api_family;
    model.stream = test_case.stream;
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .anchor_set = command_anchors_for_test(workspace, store.session_path().parent_path() / "spill"),
        .mode = ava::agent::Mode::Build,
        .model = std::move(model),
        .access_token = "token",
        .tool_execution = ava::agent::ToolExecutionOptions{.require_explicit_file_permissions = true},
        .on_tool_event = [&tool_events](ava::agent::ToolTimelineEntry const& event) { tool_events.push_back(event); },
        .permission_resolver =
            [&permission_requests](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          ++permission_requests;
          return ava::permissions::PermissionResolution::Allow;
        },
        .auto_allow_deny_preflight =
            [&deny_preflights](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          ++deny_preflights;
          return ava::permissions::PermissionResolution::Allow;
        },
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });

    auto result = loop.run_turn("do not run truncated calls", store, *test_case.provider, transport);
    auto entries = store.load();
    auto const projection = entries ? ava::session::classify_assistant_output(*entries) : ava::session::AssistantOutputProjection{};
    std::vector<std::string> persisted_call_ids;
    std::vector<std::string> persisted_call_names;
    std::vector<std::string> function_entry_ids;
    bool truncated_commit = false;
    for (auto const& turn : projection.turns)
    {
      bool turn_has_function = false;
      for (auto const& item : turn.items)
      {
        if (auto const* function = std::get_if<ava::session::AssistantOutputFunctionCall>(&item.item.payload))
        {
          turn_has_function = true;
          persisted_call_ids.push_back(function->call_id);
          persisted_call_names.push_back(function->name);
          function_entry_ids.push_back(item.entry_id);
        }
      }
      if (turn_has_function)
        truncated_commit = turn.commit.finish_reason == "max_tokens";
    }

    std::vector<ava::session::SessionEntry const*> tool_results;
    std::size_t permission_entries = 0;
    if (entries)
    {
      for (auto const& entry : *entries)
      {
        if (entry.type == ava::session::EntryType::ToolResult)
          tool_results.push_back(&entry);
        permission_entries += entry.type == ava::session::EntryType::PermissionDecision;
      }
    }
    bool exact_results = tool_results.size() == test_case.call_ids.size() && function_entry_ids.size() == test_case.call_ids.size() &&
                         persisted_call_names.size() == test_case.call_ids.size();
    for (std::size_t index = 0; exact_results && index < tool_results.size(); ++index)
    {
      auto const& entry = *tool_results[index];
      auto const structured = ava::core::json::object_field(entry.data_json, "structured_result");
      auto const error = structured ? ava::core::json::object_field(*structured, "error") : std::nullopt;
      auto const result_json = ava::core::json::string_field(entry.data_json, "result");
      auto const message = error ? ava::core::json::string_field(*error, "message") : std::nullopt;
      exact_results = ava::core::json::string_field(entry.data_json, "call_id") == test_case.call_ids[index] &&
                      ava::core::json::string_field(entry.data_json, "name") == persisted_call_names[index] &&
                      ava::core::json::string_field(entry.data_json, "assistant_output_entry_id") == function_entry_ids[index] &&
                      ava::core::json::string_field(entry.data_json, "status") == "error" && error &&
                      ava::core::json::string_field(*error, "code") == "provider_output_truncated" && message && message->size() <= 256 && result_json &&
                      result_json->find("\"retryable\":false") != std::string::npos;
    }
    auto const validation = entries ? ava::session::validate_session_replay(*entries) : ava::session::SessionReplayValidation{};
    bool continuation_has_results = transport.requests().size() == 2 && transport.requests()[1].body.find("provider_output_truncated") != std::string::npos;
    for (auto const& call_id : test_case.call_ids)
      continuation_has_results = continuation_has_results && transport.requests()[1].body.find(call_id) != std::string::npos;

    expect(result && result->outcome == ava::core::RuntimeTerminalOutcome::Completed && result->final_text == "continued" && result->provider_iterations == 2 &&
               result->tool_iterations == 1 && result->tool_calls == 0 && result->tool_timeline.empty() && entries && projection.turns.size() == 2 &&
               truncated_commit && persisted_call_ids == test_case.call_ids && exact_results && validation.ok() && permission_requests == 0 &&
               deny_preflights == 0 && permission_entries == 0 && tool_events.empty() && !std::filesystem::exists(workspace / sentinel_name) &&
               continuation_has_results,
           test_case.name + " binds non-retryable truncation results and continues without dispatch, permission, or side effects");
  }

  {
    auto const root = create_empty_root("agent-max-tokens-without-calls");
    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "max-tokens-no-calls"});
    ava::tests::FakeTransport transport({sse_response(R"({"choices":[{"message":{"content":"partial answer"},"finish_reason":"length"}]})")});
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model = ava::agent::ModelInvocationOptions{.provider_id = "compatible",
                                                    .model_id = "compatible-test",
                                                    .system_prompt = "system prompt",
                                                    .stream = false,
                                                    .api_family = "openai_chat_completions"},
        .access_token = "token",
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("remain terminal", store, compatible_provider, transport);
    expect(result && result->outcome == ava::core::RuntimeTerminalOutcome::MaxTokens && result->final_text == "partial answer" &&
               result->provider_iterations == 1 && result->tool_iterations == 0 && result->tool_calls == 0 && transport.requests().size() == 1,
           "max-tokens output without tool calls remains terminal");
  }

  {
    auto const root = create_empty_root("agent-truncated-provider-iteration-bound");
    auto const workspace = root / "workspace";
    std::filesystem::create_directories(workspace);
    ava::session::SessionStore store(
        ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "truncated-provider-bound"});
    ava::tests::FakeTransport transport(
        {sse_response(chat_stream_tool_call("bounded_call", "write_file", write_arguments, "length")), sse_response(compatible_final_stream)});
    ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
        .workspace_dir = workspace,
        .mode = ava::agent::Mode::Build,
        .model =
            ava::agent::ModelInvocationOptions{
                .provider_id = "compatible", .model_id = "compatible-test", .system_prompt = "system prompt", .api_family = "openai_chat_completions"},
        .access_token = "token",
        .max_tool_iterations = 1,
        .append_entry = append_route_for_test(store),
        .append_batch = append_batch_route_for_test(store),
        .session_read_authority = read_authority_for_test(store),
    });
    auto result = loop.run_turn("respect the iteration cap", store, compatible_provider, transport);
    auto entries = store.load();
    bool const has_truncated_result =
        entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
          return entry.type == ava::session::EntryType::ToolResult && entry.data_json.find("provider_output_truncated") != std::string::npos;
        });
    expect(result && result->outcome == ava::core::RuntimeTerminalOutcome::MaxTurnRequests && result->provider_iterations == 1 &&
               result->tool_iterations == 1 && result->tool_calls == 0 && transport.requests().size() == 1 && has_truncated_result,
           "a truncated tool-call batch consumes one existing tool-iteration slot before another provider request");
  }
}

void test_agent_loop_unknown_incomplete_reason_remains_fail_closed()
{
  auto const root = create_empty_root("agent-unknown-incomplete-reason");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "unknown-incomplete"});
  ava::provider::OpenAIProvider const provider("https://openai.example.test");
  ava::tests::FakeTransport transport({sse_response(
      R"({"status":"incomplete","incomplete_details":{"reason":"future_limit"},"output":[{"id":"fc_unknown","type":"function_call","call_id":"unknown_incomplete_call","name":"write_file","arguments":"{\"path\":\"must-not-exist.txt\",\"content\":\"forbidden\"}"}]})")});
  int permission_requests = 0;
  int tool_events = 0;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model =
          ava::agent::ModelInvocationOptions{
              .provider_id = "openai", .model_id = "gpt-test", .system_prompt = "system prompt", .stream = false, .api_family = "openai_responses"},
      .access_token = "token",
      .tool_execution = ava::agent::ToolExecutionOptions{.require_explicit_file_permissions = true},
      .on_tool_event = [&tool_events](ava::agent::ToolTimelineEntry const&) { ++tool_events; },
      .permission_resolver =
          [&permission_requests](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++permission_requests;
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("unknown incomplete output", store, provider, transport);
  auto entries = store.load();
  bool const has_tool_lifecycle = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                                    return entry.type == ava::session::EntryType::AssistantOutputItem ||
                                           entry.type == ava::session::EntryType::AssistantTurnCommit || entry.type == ava::session::EntryType::ToolResult;
                                  });
  expect(!result && result.error().category() == ava::core::ErrorCategory::Provider && permission_requests == 0 && tool_events == 0 && entries &&
             !has_tool_lifecycle && !std::filesystem::exists(workspace / "must-not-exist.txt"),
         "an unknown incomplete reason with an actionable call remains fail closed before persistence, permission, or dispatch");
}

void test_child_auto_compacts_after_large_tool_result()
{
  auto const root = create_empty_root("agent-child-auto-compaction");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  {
    std::ofstream large(workspace / "large.txt", std::ios::binary | std::ios::trunc);
    large << std::string(4096, 'x');
  }
  auto store_result = ava::session::SessionStore::create_ephemeral(workspace);
  expect(store_result.has_value(), "child compaction fixture creates an ephemeral child session");
  if (!store_result)
    return;
  auto store = std::move(*store_result);
  auto target_result = ava::session::SessionAppendTarget::create_ephemeral(store);
  expect(target_result.has_value(), "child compaction fixture creates an owned append target");
  if (!target_result)
    return;
  auto target = *target_result;
  auto read_authority = target->read_authority();
  if (!read_authority)
  {
    expect(false, "child compaction fixture derives read authority");
    return;
  }
  ava::session::CompactionConfig config;
  config.auto_threshold_tokens = 50;
  config.auto_threshold_tokens_explicit = true;
  config.keep_recent_tokens = 200;
  config.provider_id = "openai";
  config.model_id = "gpt-test";
  auto const read_call =
      std::string("data: {\"type\":\"response.function_call.added\",\"call_id\":\"large_read\",\"name\":\"read_file\"}\n\n") +
      "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"large_read\",\"delta\":\"{\\\"path\\\":\\\"large.txt\\\"}\"}\n\n" +
      "data: {\"type\":\"response.function_call.done\",\"call_id\":\"large_read\"}\n\n"
      "data: [DONE]\n\n";
  ava::tests::FakeTransport transport(
      {sse_response(read_call),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"# Goal\\nPreserve findings\\n# Constraints / Preferences\\nNone noted.\\n# "
                    "Decisions\\nNone noted.\\n# Files Read or Modified\\nlarge.txt read\\n# Unresolved Tasks\\nReport findings\\n# Next "
                    "Steps\\nFinish\"}\n\ndata: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"child finished from compacted findings\"}\n\ndata: [DONE]\n\n")});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .child_execution = true,
      .child_compaction_blueprint = ava::agent::ChildContextCompactionBlueprint{.config = config, .context_window_tokens = 4096},
      .child_compaction_binding =
          ava::agent::ChildContextCompactionBinding{.blueprint = {.config = config, .context_window_tokens = 4096}, .append_target = target},
      .append_entry = [target](ava::session::SessionEntry entry) { return target->append(entry); },
      .append_batch = [target](std::vector<ava::session::SessionEntry> entries) { return target->append_batch(std::move(entries)); },
      .session_read_authority = std::move(*read_authority),
  });
  auto result = loop.run_turn("read the large file and report", store, provider, transport);
  auto entries = store.load();
  auto const compactions =
      entries ? std::ranges::count_if(*entries, [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::Compaction; }) : 0;
  expect(result && result->final_text == "child finished from compacted findings" && result->tool_iterations == 1 && compactions == 1 &&
             transport.requests().size() == 3 && transport.requests()[1].body.find("large.txt") != std::string::npos &&
             transport.requests()[2].body.find("Preserve findings") != std::string::npos,
         "a large child tool result crosses the snapshotted threshold, writes one child checkpoint, retains findings, and preserves its tool budget");
}

void test_agent_loop_max_iteration_guard()
{
  auto const root = create_empty_root("agent-max");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "max"});
  auto tool_sse = [](std::string_view call_id) {
    return "data: {\"type\":\"response.function_call.added\",\"call_id\":\"" + std::string(call_id) +
           "\",\"name\":\"glob\"}\n\n"
           "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"" +
           std::string(call_id) +
           "\",\"delta\":\"{\\\"pattern\\\":\\\"**/*\\\"}\"}\n\n"
           "data: {\"type\":\"response.function_call.done\",\"call_id\":\"" +
           std::string(call_id) +
           "\"}\n\n"
           "data: [DONE]\n\n";
  };
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(tool_sse("call_glob_1")), sse_response(tool_sse("call_glob_2"))});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .max_tool_iterations = 2,
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("loop", store, provider, transport);
  expect(result && result->outcome == ava::core::RuntimeTerminalOutcome::MaxTurnRequests && result->provider_iterations == 2 && result->tool_iterations == 2 &&
             result->tool_calls == 2,
         "agent loop reports repeated tool use as the max-turn-requests terminal outcome");
  auto entries = store.load();
  expect(entries && std::ranges::none_of(*entries, [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::Error; }),
         "max-turn-requests is a terminal outcome rather than a persisted internal error");

  ava::session::SessionStore child_store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "max-child"});
  auto const two_tools =
      std::string("data: {\"type\":\"response.function_call.added\",\"call_id\":\"child_glob_1\",\"name\":\"glob\"}\n\n") +
      "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"child_glob_1\",\"delta\":\"{\\\"pattern\\\":\\\"**/*\\\"}\"}\n\n" +
      "data: {\"type\":\"response.function_call.done\",\"call_id\":\"child_glob_1\"}\n\n" +
      "data: {\"type\":\"response.function_call.added\",\"call_id\":\"child_glob_2\",\"name\":\"glob\"}\n\n" +
      "data: {\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"child_glob_2\",\"delta\":\"{\\\"pattern\\\":\\\"**/*.cpp\\\"}\"}\n\n" +
      "data: {\"type\":\"response.function_call.done\",\"call_id\":\"child_glob_2\"}\n\n"
      "data: [DONE]\n\n";
  auto const noncompliant_wrap_up = std::string("data: {\"type\":\"response.output_text.delta\",\"delta\":\"findings and unfinished work\"}\n\n") +
                                    "data: {\"type\":\"response.function_call.added\",\"call_id\":\"forbidden_write\",\"name\":\"write_file\"}\n\n" +
                                    "data: "
                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"forbidden_write\",\"delta\":\"{\\\"path\\\":"
                                    "\\\"forbidden.txt\\\",\\\"content\\\":\\\"no\\\"}\"}\n\n" +
                                    "data: {\"type\":\"response.function_call.done\",\"call_id\":\"forbidden_write\"}\n\n"
                                    "data: [DONE]\n\n";
  ava::tests::FakeTransport child_transport({sse_response(two_tools), sse_response(noncompliant_wrap_up)});
  int permission_requests = 0;
  ava::agent::AgentLoop child_loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .max_tool_iterations = 1,
      .child_execution = true,
      .permission_resolver = [&permission_requests](auto const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++permission_requests;
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(child_store),
      .append_batch = append_batch_route_for_test(child_store),
      .session_read_authority = read_authority_for_test(child_store),
  });
  auto child_result = child_loop.run_turn("use one tool round", child_store, provider, child_transport);
  auto child_entries = child_store.load();
  bool const settled_without_execution =
      child_entries && std::ranges::any_of(*child_entries, [](ava::session::SessionEntry const& entry) {
        return entry.type == ava::session::EntryType::ToolResult && entry.data_json.find("child_tool_limit_reached") != std::string::npos;
      });
  expect(child_result && child_result->outcome == ava::core::RuntimeTerminalOutcome::MaxTurnRequests && child_result->provider_iterations == 2 &&
             child_result->tool_iterations == 1 && child_result->tool_calls == 2 && child_result->final_text == "findings and unfinished work" &&
             permission_requests == 0 && !std::filesystem::exists(workspace / "forbidden.txt") && settled_without_execution &&
             child_transport.requests().size() == 2 && child_transport.requests()[1].body.find("\"tools\":[]") != std::string::npos &&
             child_transport.requests()[1].body.find("Do not call tools") != std::string::npos,
         "a child limit counts a multi-tool round once, then makes one bounded tool-free summary request and settles noncompliant calls without execution");
}

void test_child_wrap_up_omits_anthropic_thinking_when_output_clamped()
{
  auto const root = create_empty_root("agent-child-wrap-up-anthropic-thinking");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "child-wrap-thinking"});
  ava::provider::AnthropicProvider const provider("https://anthropic.example.test");
  auto const wrap_up =
      std::string("event: content_block_start\n") +
      "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n" + "event: content_block_delta\n" +
      "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"findings\"}}\n\n" + "event: content_block_stop\n" +
      "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n" + "event: message_delta\n" +
      "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n" + "event: message_stop\n" + "data: {\"type\":\"message_stop\"}\n\n";
  ava::tests::FakeTransport transport(
      {sse_response(anthropic_stream_tool_call("toolu_wrap_1", "glob", R"({"pattern":"**/*"})", "tool_use")), sse_response(wrap_up)});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "anthropic",
                                                  .model_id = "claude-sonnet-4-5",
                                                  .system_prompt = "system prompt",
                                                  .max_output_tokens = 8192,
                                                  .reasoning = ava::provider::ProviderReasoningOptions{.type = "enabled", .budget_tokens = 4096},
                                                  .api_family = "anthropic_messages",
                                                  .reasoning_format = "anthropic_thinking"},
      .access_token = "token",
      .max_tool_iterations = 1,
      .child_execution = true,
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("use one tool round", store, provider, transport);
  auto entries = store.load();
  auto const validation = entries ? ava::session::validate_session_replay(*entries) : ava::session::SessionReplayValidation{};
  bool const bound_tool = transport.requests().size() == 2 &&
                          transport.requests()[1].body.find(R"("type":"tool_use","id":"toolu_wrap_1","name":"glob")") != std::string::npos &&
                          transport.requests()[1].body.find(R"("type":"tool_result","tool_use_id":"toolu_wrap_1")") != std::string::npos;
  expect(result && result->outcome == ava::core::RuntimeTerminalOutcome::MaxTurnRequests && result->final_text == "findings" &&
             result->provider_iterations == 2 && result->tool_iterations == 1 && result->tool_calls == 1 && validation.ok() && bound_tool &&
             transport.requests()[0].body.find(R"("max_tokens":8192)") != std::string::npos &&
             transport.requests()[0].body.find(R"("thinking":{"type":"enabled","budget_tokens":4096})") != std::string::npos &&
             transport.requests()[0].body.find(R"("tools":[)") != std::string::npos &&
             transport.requests()[1].body.find(R"("max_tokens":2048)") != std::string::npos &&
             transport.requests()[1].body.find(R"("thinking":{"type":"enabled")") == std::string::npos &&
             transport.requests()[1].body.find(R"("tools":)") == std::string::npos &&
             transport.requests()[1].body.find("Do not call tools") != std::string::npos,
         "child wrap-up clamps Anthropic output to 2048 without thinking or tools after one enabled-thinking tool round");
}

}  // namespace

void run_agent_loop_resilience_tests()
{
  test_agent_loop_cancellation_boundaries();
  test_agent_loop_error_paths_and_bounds();
  test_agent_loop_truncated_provider_output_never_dispatches_tools();
  test_agent_loop_unknown_incomplete_reason_remains_fail_closed();
  test_child_auto_compacts_after_large_tool_result();
  test_agent_loop_max_iteration_guard();
  test_child_wrap_up_omits_anthropic_thinking_when_output_clamped();
}
