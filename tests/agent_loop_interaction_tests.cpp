#include "sys.h"
#include "tests/agent_loop_test_declarations.h"
#include "tests/support/agent_loop_test_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/stream_bridge.h"
#include "ava/session/assistant_output.h"
#include "ava/session/session_store.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/result.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <sys/stat.h>

using agent_loop_test::sse_response;
using agent_loop_test::tool_call_sse;

namespace {

class OverflowOnceProvider final : public ava::provider::Provider
{
 public:
  explicit OverflowOnceProvider(std::string base_url) : delegate_(std::move(base_url)) { }

  [[nodiscard]] ava::core::Result<ava::http::HttpRequest> build_request(ava::provider::ProviderRequest const& request,
                                                                        std::string_view access_token) const override
  {
    ++build_calls_;
    if (build_calls_ == 1)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Provider, "context window exceeds token limit"));
    }
    return delegate_.build_request(request, access_token);
  }

  [[nodiscard]] std::unique_ptr<ava::provider::StreamParser> create_stream_parser() const override { return delegate_.create_stream_parser(); }

  [[nodiscard]] ava::core::Result<std::vector<ava::provider::StreamEvent>> parse_response(ava::http::HttpResponse const& response, bool stream) const override
  {
    return delegate_.parse_response(response, stream);
  }

 private:
  ava::provider::OpenAIProvider delegate_;
  mutable int build_calls_ = 0;
};

}  // namespace

void test_agent_loop_permission_resolver_threads_to_tools()
{
  auto const root = create_empty_root("agent-permission-resolver");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "agent command permission workspace is owner-only for sealed planning");
  auto const outside_path = root / "outside.txt";
  {
    std::ofstream file(outside_path, std::ios::binary | std::ios::trunc);
    file << "outside via agent";
  }
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "resolver"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_outside\",\"name\":\"read_file\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_outside\",\"delta\":\"{"
                                                    "\\\"path\\\":\\\"" +
                                                    ava::core::json::escape(outside_path.generic_string()) +
                                                    "\\\"}\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"used resolver\"}\n\n"
                                                    "data: [DONE]\n\n")});
  int prompts = 0;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver =
          [&prompts, &outside_path](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++prompts;
        expect(prompt.target_path == outside_path, "agent loop resolver sees tool target path");
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("read outside", store, provider, transport);
  expect(result && result->final_text == "used resolver" && prompts == 1 && result->tool_timeline.size() == 1 &&
             result->tool_timeline.front().status == ava::agent::ToolTimelineStatus::Success,
         "agent loop threads permission resolver into tool dispatcher");
  expect(transport.requests().size() == 2 && transport.requests()[1].body.find("outside via agent") != std::string::npos,
         "agent loop continuation includes resolver-approved tool result");
  auto resolver_entries = store.load();
  auto resolver_audits = resolver_entries ? permission_entries(*resolver_entries) : std::vector<ava::session::SessionEntry>{};
  auto const resolver_permission_request_id =
      resolver_audits.size() >= 2 ? ava::core::json::string_field(resolver_audits[0].data_json, "permission_request_id").value_or("") : "";
  expect(resolver_audits.size() == 2 && resolver_permission_request_id.starts_with("permreq_") &&
             ava::core::json::string_field(resolver_audits[0].data_json, "action") == "ask" &&
             ava::core::json::string_field(resolver_audits[0].data_json, "resolution_source") == "policy" &&
             ava::core::json::string_field(resolver_audits[1].data_json, "permission_request_id") == resolver_permission_request_id &&
             ava::core::json::string_field(resolver_audits[1].data_json, "resolution") == "allow" &&
             ava::core::json::string_field(resolver_audits[1].data_json, "resolution_source") == "resolver",
         "agent loop persists linked ask and resolver permission audit entries");
  auto const resolver_structured_permission_ids =
      result && !result->tool_timeline.empty()
          ? ava::core::json::strings_in_array_field(result->tool_timeline.front().structured_result_json, "permission_request_ids")
          : std::vector<std::string>{};
  expect(resolver_structured_permission_ids.size() == 1 && resolver_structured_permission_ids[0] == resolver_permission_request_id,
         "agent loop links structured tool result to permission audit request id");
  expect(result && !result->tool_timeline.empty() && result->tool_timeline.front().permission_request_ids.size() == 1 &&
             result->tool_timeline.front().permission_request_ids[0] == resolver_permission_request_id,
         "agent loop exposes permission request ids on tool timeline entries");

  {
    auto const bash_root = create_empty_root("agent-bash-ask-allow");

    auto const bash_workspace = bash_root / "workspace";
    std::filesystem::create_directories(bash_workspace);
    expect(::chmod(bash_root.c_str(), S_IRWXU) == 0 && ::chmod(bash_workspace.c_str(), S_IRWXU) == 0,
           "agent bash allow workspace is owner-only for sealed planning");
    ava::session::SessionStore bash_store(
        ava::session::SessionStoreOptions{.root_dir = bash_root / "sessions", .workspace_dir = bash_workspace, .session_id = "bash-allow"});
    ava::tests::FakeTransport bash_transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_bash\",\"name\":\"bash\"}\n\n"
                                                           "data: "
                                                           "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_bash\",\"delta\":\"{"
                                                           "\\\"command\\\":\\\"true\\\"}\"}\n\n"
                                                           "data: [DONE]\n\n"),
                                              sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"bash allowed\"}\n\n"
                                                           "data: [DONE]\n\n")});
    int bash_allow_prompts = 0;
    ava::agent::AgentLoop bash_loop(ava::agent::AgentLoopOptions{
        .workspace_dir = bash_workspace,
        .anchor_set = command_anchors_for_test(bash_workspace, bash_store.session_path().parent_path() / "spill"),
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .permission_resolver =
            [&bash_allow_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          ++bash_allow_prompts;
          expect(prompt.operation == ava::permissions::Operation::RunCommand, "agent bash allow resolver sees run command");
          expect(prompt.command == "true", "agent bash allow resolver sees command text");
          return ava::permissions::PermissionResolution::Allow;
        },
        .append_entry = append_route_for_test(bash_store),
        .append_batch = append_batch_route_for_test(bash_store),
        .session_read_authority = read_authority_for_test(bash_store),
    });
    auto bash_result = bash_loop.run_turn("run true", bash_store, provider, bash_transport);
    expect(bash_result && bash_result->final_text == "bash allowed" && bash_allow_prompts == 1 && bash_result->tool_timeline.size() == 1 &&
               bash_result->tool_timeline.front().status == ava::agent::ToolTimelineStatus::Success,
           "agent loop allows bash Ask decisions when resolver allows once");
  }

  {
    auto const bash_root = create_empty_root("agent-bash-ask-deny");

    auto const bash_workspace = bash_root / "workspace";
    std::filesystem::create_directories(bash_workspace);
    expect(::chmod(bash_root.c_str(), S_IRWXU) == 0 && ::chmod(bash_workspace.c_str(), S_IRWXU) == 0,
           "agent bash deny workspace is owner-only for sealed planning");
    ava::session::SessionStore bash_store(
        ava::session::SessionStoreOptions{.root_dir = bash_root / "sessions", .workspace_dir = bash_workspace, .session_id = "bash-deny"});
    ava::tests::FakeTransport bash_transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_bash\",\"name\":\"bash\"}\n\n"
                                                           "data: "
                                                           "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_bash\",\"delta\":\"{"
                                                           "\\\"command\\\":\\\"printf AGENT_DENIED_COMMAND_SECRET_SENTINEL\\\"}\"}\n\n"
                                                           "data: [DONE]\n\n"),
                                              sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"bash denied\"}\n\n"
                                                           "data: [DONE]\n\n")});
    int bash_deny_prompts = 0;
    std::vector<ava::provider::StreamEvent> public_bash_stream_events;
    ava::agent::AgentLoop bash_loop(ava::agent::AgentLoopOptions{
        .workspace_dir = bash_workspace,
        .anchor_set = command_anchors_for_test(bash_workspace, bash_store.session_path().parent_path() / "spill"),
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .on_stream_event = [&public_bash_stream_events](ava::provider::StreamEvent const& event) -> ava::core::VoidResult {
          public_bash_stream_events.push_back(event);
          return {};
        },
        .permission_resolver =
            [&bash_deny_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          ++bash_deny_prompts;
          expect(prompt.operation == ava::permissions::Operation::RunCommand, "agent bash deny resolver sees run command");
          expect(prompt.command.find("AGENT_DENIED_COMMAND_SECRET_SENTINEL") != std::string::npos,
                 "agent bash deny resolver retains the exact command only in its local prompt");
          return ava::permissions::PermissionResolution::Deny;
        },
        .append_entry = append_route_for_test(bash_store),
        .append_batch = append_batch_route_for_test(bash_store),
        .session_read_authority = read_authority_for_test(bash_store),
    });
    auto bash_result = bash_loop.run_turn("run true", bash_store, provider, bash_transport);
    expect(bash_result && bash_result->final_text == "bash denied" && bash_deny_prompts == 1 && bash_result->tool_timeline.size() == 1 &&
               bash_result->tool_timeline.front().status == ava::agent::ToolTimelineStatus::Error,
           "agent loop records denied bash Ask decisions as failed tool results and continues");
    auto bash_entries = bash_store.load();
    auto bash_audits = bash_entries ? permission_entries(*bash_entries) : std::vector<ava::session::SessionEntry>{};
    auto const serialized_denial =
        bash_result && !bash_result->tool_timeline.empty() ? bash_result->tool_timeline.front().structured_result_json : std::string{};
    auto const continuation_request = bash_transport.requests().size() > 1 ? bash_transport.requests()[1].body : std::string{};
    auto const durable_secret_absent = bash_entries && std::ranges::all_of(*bash_entries, [](ava::session::SessionEntry const& entry) {
                                         return entry.data_json.find("AGENT_DENIED_COMMAND_SECRET_SENTINEL") == std::string::npos;
                                       });
    auto const public_stream_secret_absent = std::ranges::all_of(public_bash_stream_events, [](ava::provider::StreamEvent const& event) {
      return event.text.find("AGENT_DENIED_COMMAND_SECRET_SENTINEL") == std::string::npos;
    });
    expect(bash_audits.size() == 2 && ava::core::json::string_field(bash_audits[1].data_json, "command") == "<redacted one-shot command>" &&
               ava::core::json::string_field(bash_audits[1].data_json, "resolution") == "deny" &&
               ava::core::json::string_field(bash_audits[1].data_json, "resolution_source") == "resolver" && durable_secret_absent &&
               public_stream_secret_absent && serialized_denial.find("AGENT_DENIED_COMMAND_SECRET_SENTINEL") == std::string::npos &&
               continuation_request.find("AGENT_DENIED_COMMAND_SECRET_SENTINEL") == std::string::npos,
           "agent loop redacts denied command arguments from durable audits, public stream events, serialized tool errors, and continuation payloads");
  }

  {
    auto const bash_root = create_empty_root("agent-bash-ask-fail");

    auto const bash_workspace = bash_root / "workspace";
    std::filesystem::create_directories(bash_workspace);
    expect(::chmod(bash_root.c_str(), S_IRWXU) == 0 && ::chmod(bash_workspace.c_str(), S_IRWXU) == 0,
           "agent bash resolver failure workspace is owner-only for sealed planning");
    ava::session::SessionStore bash_store(
        ava::session::SessionStoreOptions{.root_dir = bash_root / "sessions", .workspace_dir = bash_workspace, .session_id = "bash-fail"});
    ava::tests::FakeTransport bash_transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_bash\",\"name\":\"bash\"}\n\n"
                                                           "data: "
                                                           "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_bash\",\"delta\":\"{"
                                                           "\\\"command\\\":\\\"true\\\"}\"}\n\n"
                                                           "data: [DONE]\n\n"),
                                              sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"bash resolver failed\"}\n\n"
                                                           "data: [DONE]\n\n")});
    int bash_fail_prompts = 0;
    ava::agent::AgentLoop bash_loop(ava::agent::AgentLoopOptions{
        .workspace_dir = bash_workspace,
        .anchor_set = command_anchors_for_test(bash_workspace, bash_store.session_path().parent_path() / "spill"),
        .mode = ava::agent::Mode::Build,
        .model = agent_loop_test::model_invocation_options(),
        .access_token = "token",
        .permission_resolver =
            [&bash_fail_prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
          ++bash_fail_prompts;
          expect(prompt.operation == ava::permissions::Operation::RunCommand, "agent bash fail resolver sees run command");
          return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "resolver failed"));
        },
        .append_entry = append_route_for_test(bash_store),
        .append_batch = append_batch_route_for_test(bash_store),
        .session_read_authority = read_authority_for_test(bash_store),
    });
    auto bash_result = bash_loop.run_turn("run true", bash_store, provider, bash_transport);
    expect(bash_result && bash_result->final_text == "bash resolver failed" && bash_fail_prompts == 1 && bash_result->tool_timeline.size() == 1 &&
               bash_result->tool_timeline.front().status == ava::agent::ToolTimelineStatus::Error && bash_transport.requests().size() == 2,
           "agent loop records failed bash Ask resolver as failed tool result and continues");
  }
}

void test_agent_loop_question_resolver_threads_to_tools()
{
  auto const root = create_empty_root("agent-question-resolver");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "question-resolver"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_question\",\"name\":\"question\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_question\",\"delta\":\"{"
                                                    "\\\"question\\\":\\\"Pick one?\\\",\\\"options\\\":[\\\"A\\\",\\\"B\\\"]}\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"question answered\"}\n\n"
                                                    "data: [DONE]\n\n")});
  int prompts = 0;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .question_resolver = [&prompts](ava::agent::QuestionPrompt const& prompt) -> ava::core::Result<ava::agent::QuestionAnswer> {
        ++prompts;
        expect(prompt.question == "Pick one?" && prompt.options.size() == 2, "agent loop question resolver receives provider prompt");
        return ava::agent::QuestionAnswer{.selected_options = {"B"}, .custom_text = ""};
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("ask", store, provider, transport);
  expect(result && result->final_text == "question answered" && prompts == 1 && result->tool_timeline.size() == 1 &&
             result->tool_timeline.front().status == ava::agent::ToolTimelineStatus::Success,
         "agent loop threads question resolver into tool dispatcher");
  expect(transport.requests().size() == 2 && transport.requests()[1].body.find("\\\"selected_options\\\":[\\\"B\\\"]") != std::string::npos,
         "agent loop continuation includes serialized question answer");
}

void test_agent_loop_non_stream_response()
{
  auto const root = create_empty_root("agent-non-stream");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "nonstream"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"status\":\"completed\",\"output_text\":\"plain response with data: literal\"}"}});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "openai", .model_id = "gpt-5.5", .system_prompt = "system prompt", .stream = false},
      .access_token = "token",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("hi", store, provider, transport);
  expect(result && result->final_text == "plain response with data: literal", "agent loop parses non-stream response without sniffing data text");
  expect(!transport.requests().empty() && transport.requests()[0].body.find("\"stream\":false") != std::string::npos,
         "agent loop passes explicit non-stream request expectation");
}

void test_agent_loop_non_stream_error_prevents_tool_dispatch()
{
  auto const root = create_empty_root("agent-nonstream-provider-error");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "nonstream-provider-error"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{.status_code = 200,
                               .headers = {},
                               .body = "{\"output\":[{\"id\":\"fc_first\",\"type\":\"function_call\",\"call_id\":\"call_duplicate\","
                                       "\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"README.md\\\"}\"},{\"id\":\"fc_second\","
                                       "\"type\":\"function_call\",\"call_id\":\"call_duplicate\",\"name\":\"read_file\","
                                       "\"arguments\":\"{\\\"path\\\":\\\"README.md\\\"}\"}]}"}});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "openai", .model_id = "gpt-5.5", .system_prompt = "system prompt", .stream = false},
      .access_token = "token",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("read", store, provider, transport);
  auto entries = store.load();
  expect(!result && result.error().category() == ava::core::ErrorCategory::Provider && entries &&
             std::none_of(entries->begin(), entries->end(),
                          [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::ToolCall; }),
         "a non-stream OpenAI parser Error prevents dispatch of an earlier otherwise valid tool call");
}

void test_stream_bridge_redacts_untrusted_provider_error_event()
{
  std::vector<ava::provider::StreamEvent> published;
  ava::agent::AgentLoopOptions options;
  options.on_stream_event = [&published](ava::provider::StreamEvent const& event) -> ava::core::VoidResult {
    published.push_back(event);
    return {};
  };
  ava::provider::StreamEvent untrusted_error;
  untrusted_error.type = ava::provider::StreamEventType::Error;
  untrusted_error.error_message = "STREAM_BRIDGE_PROVIDER_CANARY";
  auto result = ava::agent::publish_stream_event(options, untrusted_error);
  auto parsed = ava::agent::parse_assistant_turn({untrusted_error}, {});
  expect(result && published.size() == 1 && published[0].error_message == "Provider streaming error" &&
             published[0].error_message.find("STREAM_BRIDGE_PROVIDER_CANARY") == std::string::npos && !parsed &&
             parsed.error().message() == "provider stream error" && parsed.error().format().find("STREAM_BRIDGE_PROVIDER_CANARY") == std::string::npos,
         "the stream bridge and assistant-turn parser redact arbitrary provider Error payloads before public or session handling");
}

void test_agent_loop_invalid_utf8_function_arguments_prevent_dispatch()
{
  auto const root = create_empty_root("agent-invalid-utf8-function-arguments");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "invalid-utf8-function-arguments"});
  std::string response =
      "{\"output\":[{\"id\":\"fc_invalid_utf8\",\"type\":\"function_call\",\"call_id\":\"call_invalid_utf8\",\"name\":\"read_file\",\"arguments\":\"{"
      "\\\"path\\\":\\\"";
  response.push_back(static_cast<char>(0xFF));
  response += "\\\"}\"}]}";
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = std::move(response)}});
  int permission_resolver_calls = 0;
  int tool_events = 0;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = ava::agent::ModelInvocationOptions{.provider_id = "openai", .model_id = "gpt-5.5", .system_prompt = "system prompt", .stream = false},
      .access_token = "token",
      .on_tool_event = [&tool_events](ava::agent::ToolTimelineEntry const&) { ++tool_events; },
      .permission_resolver =
          [&permission_resolver_calls](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++permission_resolver_calls;
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("read", store, provider, transport);
  auto entries = store.load();
  expect(!result && result.error().category() == ava::core::ErrorCategory::Provider && permission_resolver_calls == 0 && tool_events == 0 && entries &&
             std::none_of(entries->begin(), entries->end(),
                          [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::ToolCall; }),
         "invalid UTF-8 OpenAI function arguments are rejected before permission evaluation or tool dispatch");
}

void test_agent_loop_stream_unended_documented_function_prevents_dispatch()
{
  auto const root = create_empty_root("agent-stream-unended-documented-function");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "stream-unended-documented-function"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response("data: {\"type\":\"response.output_item.added\",\"item\":{\"id\":\"fc_open\",\"type\":\"function_call\","
                    "\"call_id\":\"call_open\",\"name\":\"read_file\",\"arguments\":\"\"}}\n\n"
                    "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("read", store, provider, transport);
  auto entries = store.load();
  expect(!result && result.error().category() == ava::core::ErrorCategory::Provider && entries &&
             std::none_of(entries->begin(), entries->end(),
                          [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::ToolCall; }),
         "an unfinished documented streaming function item fails before agent tool dispatch");
}

void test_agent_loop_stream_post_terminal_function_prevents_dispatch()
{
  auto const root = create_empty_root("agent-stream-post-terminal-function");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "stream-post-terminal-function"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response("data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n"
                    "data: {\"type\":\"response.output_item.done\",\"item\":{\"id\":\"fc_late\",\"type\":\"function_call\","
                    "\"call_id\":\"call_late\",\"name\":\"list_directory\",\"arguments\":\"{}\"}}\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("list", store, provider, transport);
  auto entries = store.load();
  expect(!result && result.error().category() == ava::core::ErrorCategory::Provider && entries &&
             std::none_of(entries->begin(), entries->end(),
                          [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::ToolCall; }),
         "a documented function item after the terminal response boundary fails before agent tool dispatch");
}

void test_agent_loop_compaction_status_metadata()
{
  auto const root = create_empty_root("agent-compaction-status");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "compaction-status"});
  auto appended =
      append_session_entry_for_test(store, ava::session::SessionEntry{.id = "entry_compaction_status",
                                                                      .parent_id = "",
                                                                      .type = ava::session::EntryType::Compaction,
                                                                      .timestamp = ava::session::now_timestamp(),
                                                                      .data_json = "{\"summary\":\"older context\",\"history_projection\":\"portable-v1\"}"});
  expect(appended.has_value(), "agent loop compaction metadata test seeds compaction entry");
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"after compaction\"}\n\n"
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
  auto result = loop.run_turn("continue", store, provider, transport);
  expect(result && result->used_compacted_context && result->initial_context_messages == 2 && result->outcome == ava::core::RuntimeTerminalOutcome::Completed,
         "agent loop status metadata reports compacted initial provider context");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("Compacted prior conversation summary") != std::string::npos,
         "agent loop sends compacted context in initial provider request");
}

void test_agent_loop_invokes_compaction_callback()
{
  auto const root = create_empty_root("agent-invokes-compaction-callback");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "invokes-compaction-callback"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"compaction callback completed\"}\n\n"
                    "data: [DONE]\n\n")});
  bool compact_callback_observed = false;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .compact_context = [&compact_callback_observed](ava::session::SessionReadAuthority, std::string_view, std::vector<std::string> const&)
                               -> ava::core::Result<bool> {
        compact_callback_observed = true;
        return false;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("continue", store, provider, transport);
  expect(result && result->final_text == "compaction callback completed" && compact_callback_observed,
         "agent loop invokes the context compaction callback");
}

void test_agent_loop_two_provider_tool_turn_phase_and_persistence_order()
{
  auto const root = create_empty_root("agent-two-provider-tool-turn-order");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  {
    std::ofstream file(workspace / "note.txt", std::ios::binary | std::ios::trunc);
    file << "safe tool content";
  }
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "two-provider-tool-turn-order"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(tool_call_sse("call_read", "read_file", R"({"path":"note.txt"})") + "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"final answer\"}\n\n"
                                                    "data: [DONE]\n\n")});
  std::vector<ava::agent::RunPhase> phase_transitions;
  std::vector<std::string> ordering;
  std::size_t assistant_commit_count = 0;
  std::size_t tool_event_count = 0;
  auto append_entry = append_route_for_test(store);
  auto append_batch = append_batch_route_for_test(store);
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .on_tool_event =
          [&ordering, &tool_event_count](ava::agent::ToolTimelineEntry const&) {
            if (tool_event_count++ == 0)
              ordering.push_back("first_tool_event");
          },
      .append_entry = std::move(append_entry),
      .append_batch =
          [&append_batch, &assistant_commit_count, &ordering](std::vector<ava::session::SessionEntry> entries) {
            if (std::ranges::any_of(entries,
                                    [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::AssistantTurnCommit; }))
            {
              ++assistant_commit_count;
              ordering.push_back(assistant_commit_count == 1 ? "tool_assistant_commit" : "terminal_assistant_commit");
            }
            return append_batch(std::move(entries));
          },
      .session_read_authority = read_authority_for_test(store),
      .on_phase = [&ordering, &phase_transitions](ava::agent::RunPhase phase) -> ava::core::VoidResult {
        if (phase_transitions.empty() || phase_transitions.back() != phase)
          phase_transitions.push_back(phase);
        if (phase == ava::agent::RunPhase::Completing)
          ordering.push_back("completing");
        return {};
      },
  });

  auto result = loop.run_turn("read note", store, provider, transport);
  auto entries = store.load();
  auto const projection = entries ? ava::session::classify_assistant_output(*entries) : ava::session::AssistantOutputProjection{};
  bool first_commit_contains_tool = false;
  bool terminal_commit_contains_text = false;
  if (projection.turns.size() == 2)
  {
    for (auto const& item : projection.turns[0].items)
    {
      if (auto const* function = std::get_if<ava::session::AssistantOutputFunctionCall>(&item.item.payload))
        first_commit_contains_tool = function->call_id == "call_read";
    }
    for (auto const& item : projection.turns[1].items)
    {
      if (auto const* text = std::get_if<ava::session::AssistantOutputText>(&item.item.payload))
        terminal_commit_contains_text = text->text == "final answer";
    }
  }
  auto const tool_assistant_commit = std::ranges::find(ordering, "tool_assistant_commit");
  auto const first_tool_event = std::ranges::find(ordering, "first_tool_event");
  auto const completing = std::ranges::find(ordering, "completing");
  auto const terminal_assistant_commit = std::ranges::find(ordering, "terminal_assistant_commit");
  expect(result && result->final_text == "final answer" && result->provider_iterations == 2 && result->tool_calls == 1 &&
             phase_transitions == std::vector<ava::agent::RunPhase>({ava::agent::RunPhase::BuildingContext, ava::agent::RunPhase::AwaitingProvider,
                                                                     ava::agent::RunPhase::PersistingAssistant, ava::agent::RunPhase::PreparingTools,
                                                                     ava::agent::RunPhase::ExecutingTools, ava::agent::RunPhase::SettlingTools,
                                                                     ava::agent::RunPhase::AwaitingProvider, ava::agent::RunPhase::Completing}),
         "agent loop emits the exact successful two-provider tool-turn phase transitions");
  expect(entries && assistant_commit_count == 2 && first_commit_contains_tool && terminal_commit_contains_text && tool_assistant_commit != ordering.end() &&
             first_tool_event != ordering.end() && completing != ordering.end() && terminal_assistant_commit != ordering.end() &&
             tool_assistant_commit < first_tool_event && completing < terminal_assistant_commit,
         "agent loop commits the tool assistant before its first event and the terminal assistant after Completing");
}

void test_agent_loop_replays_steering_after_mid_turn_auto_compaction()
{
  auto const root = create_empty_root("agent-steering-compaction-replay");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "steering-replay"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\n\n"
                    "data: [DONE]\n\n")});
  int compact_calls = 0;
  bool steering_taken = false;
  auto append_lease = ava::session::SessionLease::create_and_acquire(store.session_path());
  expect(append_lease.has_value(), "steering compaction fixture acquires its append lease");
  if (!append_lease)
    return;
  auto append_target = ava::session::SessionAppendTarget::create_persistent(store, *append_lease, ava::session::SessionReadLimits{});
  expect(append_target.has_value(), "steering compaction fixture creates its append target");
  if (!append_target)
    return;
  auto read_authority = (*append_target)->read_authority();
  expect(read_authority.has_value(), "steering compaction fixture creates its read authority");
  if (!read_authority)
    return;
  auto append_batch = [target = *append_target](std::vector<ava::session::SessionEntry> entries) { return target->append_batch(std::move(entries)); };
  auto append_route = [target = std::move(*append_target)](ava::session::SessionEntry entry) { return target->append(entry); };
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .take_steering_messages = [&steering_taken]() -> ava::core::Result<std::vector<std::string>> {
        if (steering_taken)
          return std::vector<std::string>{};
        steering_taken = true;
        return std::vector<std::string>{"mid-turn steering"};
      },
      .compact_context = [&compact_calls, &append_lease, &store](ava::session::SessionReadAuthority, std::string_view trigger,
                                                                 std::vector<std::string> const&) -> ava::core::Result<bool> {
        ++compact_calls;
        if (trigger == "auto" && compact_calls == 2)
        {
          auto appended = store.append(*append_lease, ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                 .parent_id = "",
                                                                                 .type = ava::session::EntryType::Compaction,
                                                                                 .timestamp = ava::session::now_timestamp(),
                                                                                 .data_json = "{\"summary\":\"mid turn\"}"});
          if (!appended)
            return std::unexpected(std::move(appended.error()));
          return true;
        }
        return false;
      },
      .append_entry = append_route,
      .append_batch = append_batch,
      .session_read_authority = std::move(*read_authority),
  });

  auto result = loop.run_turn("initial prompt", store, provider, transport);
  expect(result && result->final_text == "ok", "agent loop succeeds after mid-turn auto compaction");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("initial prompt") != std::string::npos &&
             transport.requests()[0].body.find("mid-turn steering") != std::string::npos,
         "mid-turn auto compaction replays both the initial prompt and consumed steering messages");
}

void test_agent_loop_context_overflow_retry_skips_duplicate_auto_compaction()
{
  auto const root = create_empty_root("agent-overflow-skip-auto");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "overflow-skip-auto"});
  OverflowOnceProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"retry ok\"}\n\n"
                    "data: [DONE]\n\n")});
  bool overflow_compacted = false;
  std::vector<std::string> triggers;
  auto append_lease = ava::session::SessionLease::create_and_acquire(store.session_path());
  expect(append_lease.has_value(), "overflow compaction fixture acquires its append lease");
  if (!append_lease)
    return;
  auto append_target = ava::session::SessionAppendTarget::create_persistent(store, *append_lease, ava::session::SessionReadLimits{});
  expect(append_target.has_value(), "overflow compaction fixture creates its append target");
  if (!append_target)
    return;
  auto read_authority = (*append_target)->read_authority();
  expect(read_authority.has_value(), "overflow compaction fixture creates its read authority");
  if (!read_authority)
    return;
  auto append_batch = [target = *append_target](std::vector<ava::session::SessionEntry> entries) { return target->append_batch(std::move(entries)); };
  auto append_route = [target = std::move(*append_target)](ava::session::SessionEntry entry) { return target->append(entry); };
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .compact_context = [&append_lease, &overflow_compacted, &triggers, &store](ava::session::SessionReadAuthority, std::string_view trigger,
                                                                                 std::vector<std::string> const&) -> ava::core::Result<bool> {
        triggers.push_back(std::string(trigger));
        if (trigger == "context_overflow")
        {
          overflow_compacted = true;
          auto appended =
              store.append(*append_lease, ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                     .parent_id = "",
                                                                     .type = ava::session::EntryType::Compaction,
                                                                     .timestamp = ava::session::now_timestamp(),
                                                                     .data_json = "{\"summary\":\"overflow summary\",\"history_projection\":\"portable-v1\"}"});
          if (!appended)
            return std::unexpected(std::move(appended.error()));
          return true;
        }
        if (trigger == "auto" && overflow_compacted)
        {
          auto appended =
              store.append(*append_lease, ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                     .parent_id = "",
                                                                     .type = ava::session::EntryType::Compaction,
                                                                     .timestamp = ava::session::now_timestamp(),
                                                                     .data_json = "{\"summary\":\"duplicate\",\"history_projection\":\"portable-v1\"}"});
          if (!appended)
            return std::unexpected(std::move(appended.error()));
          return true;
        }
        return false;
      },
      .append_entry = append_route,
      .append_batch = append_batch,
      .session_read_authority = *read_authority,
  });

  auto result = loop.run_turn("overflow prompt", store, provider, transport);
  auto entries = store.load();
  auto const compactions = entries ? static_cast<std::size_t>(std::ranges::count_if(
                                         *entries, [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::Compaction; }))
                                   : 0;
  auto const user_messages = entries
                                 ? static_cast<std::size_t>(std::ranges::count_if(
                                       *entries, [](ava::session::SessionEntry const& entry) { return entry.type == ava::session::EntryType::UserMessage; }))
                                 : 0;
  expect(result && result->final_text == "retry ok", "context overflow retry succeeds after compaction");
  expect(triggers == std::vector<std::string>({"auto", "auto", "context_overflow"}), "context overflow retry skips immediate duplicate auto compaction");
  expect(entries && compactions == 1 && user_messages == 2, "context overflow retry appends one compaction and replays the active prompt once");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("overflow summary") != std::string::npos &&
             transport.requests()[0].body.find("overflow prompt") != std::string::npos,
         "context overflow retry rebuilds context from the overflow compaction boundary");
  OverflowOnceProvider const canceled_provider("https://api.example.test");
  ava::tests::FakeTransport unused_transport({});
  ava::agent::AgentLoop canceled_loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .compact_context = [](auto, std::string_view trigger, auto const&) -> ava::core::Result<bool> {
        if (trigger == "context_overflow")
          return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "summary interrupted", ava::core::ErrorCode::Canceled));
        return false;
      },
      .append_entry = append_route,
      .append_batch = append_batch,
      .session_read_authority = *read_authority});
  auto canceled = canceled_loop.run_turn("cancel summary", store, canceled_provider, unused_transport);
  expect(!canceled && canceled.error().code() == ava::core::ErrorCode::Canceled && canceled.error().message() == "summary interrupted" &&
             unused_transport.requests().empty(),
         "overflow compaction passes typed cancellation through without sanitizing it into a provider failure");
}
