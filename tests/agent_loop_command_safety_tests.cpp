#include "sys.h"
#include "tests/agent_loop_test_declarations.h"
#include "tests/support/agent_loop_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/agent_loop_session.h"
#include "ava/agent/assistant_turn.h"
#include "ava/session/session_store.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/json.h"
#include "ava/core/result.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <sys/stat.h>

using agent_loop_test::sse_response;
using agent_loop_test::tool_call_sse;
using agent_loop_test::TraceCollector;

void test_agent_loop_model_auto_allow_deny_preflight_blocks_auto_allow_without_process()
{
  auto const test_root = temp_root();
  expect(::chmod(test_root.c_str(), S_IRWXU) == 0, "model command deny preflight test secures its test-root ancestor");
  auto const root = test_root / "agent-model-command-preflight-deny";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "model command deny preflight fixture keeps sealed planning roots owner-only");

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "model-preflight-deny"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(tool_call_sse("call_auto_allow", "bash", R"({"command":"ls"})") + "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"persistent deny handled\"}\n\n"
                                                    "data: [DONE]\n\n")});
  auto collector = std::make_shared<TraceCollector>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector);
  int interactive_prompts = 0;
  int deny_preflights = 0;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .anchor_set = command_anchors_for_test(workspace, store.session_path().parent_path() / "spill"),
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver =
          [&interactive_prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++interactive_prompts;
        return ava::permissions::PermissionResolution::Allow;
      },
      .auto_allow_deny_preflight =
          [&deny_preflights](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++deny_preflights;
        expect(prompt.operation == ava::permissions::Operation::RunCommand && prompt.command == "ls" && prompt.command_metadata &&
                   prompt.command_metadata->level == ava::command::CommandLevel::Standard &&
                   prompt.command_metadata->containment_status == ava::permissions::CommandContainmentStatus::NotRequired,
               "model command deny preflight receives the auto-Allow standard command's sealed metadata");
        ava::permissions::PermissionResolutionDecision denied(ava::permissions::PermissionResolution::Deny, "external persistent Deny");
        denied.resolution_source = "persistent_rule";
        denied.rule_id = "rule_model_deny";
        denied.authoritative = true;
        return denied;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
      .observation = observation,
  });

  auto result = loop.run_turn("run inspection", store, provider, transport);
  bool process_started = false;
  {
    std::lock_guard lock(collector->mutex);
    process_started = std::ranges::any_of(
        collector->events, [](ava::observability::TraceEvent const& event) { return event.type == ava::observability::TraceEventType::ProcessStart; });
  }
  expect(result && result->final_text == "persistent deny handled" && deny_preflights == 1 && interactive_prompts == 0 && result->tool_timeline.size() == 1 &&
             result->tool_timeline.front().status == ava::agent::ToolTimelineStatus::Error && !process_started,
         "an authoritative model-command Deny preflight overrides Standard auto-Allow before prompt or process side effects");
}

void test_agent_loop_model_command_rejects_authority_workspace_before_permission_or_process()
{
  auto const test_root = temp_root();
  expect(::chmod(test_root.c_str(), S_IRWXU) == 0, "model authority-root test secures its test-root ancestor");
  auto const root = test_root / "agent-model-authority-root";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "ava-authority";
  std::filesystem::create_directories(workspace);
  expect(::chmod(root.c_str(), S_IRWXU) == 0 && ::chmod(workspace.c_str(), S_IRWXU) == 0,
         "model authority-root fixture keeps sealed planning roots owner-only");

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "model-authority-root"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(tool_call_sse("call_authority", "bash", R"({"command":"ls"})") + "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"authority rejected\"}\n\n"
                                                    "data: [DONE]\n\n")});
  auto collector = std::make_shared<TraceCollector>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector);
  int prompts = 0;
  int preflights = 0;
  std::vector<std::filesystem::path> duplicate_roots(65, workspace);
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .anchor_set = command_anchors_for_test(workspace, store.session_path().parent_path() / "spill"),
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .tool_execution =
          ava::agent::ToolExecutionOptions{
              .ava_authority_roots = std::move(duplicate_roots),
          },
      .permission_resolver = [&prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++prompts;
        return ava::permissions::PermissionResolution::Allow;
      },
      .auto_allow_deny_preflight =
          [&preflights](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++preflights;
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
      .observation = observation,
  });

  auto result = loop.run_turn("inspect authority", store, provider, transport);
  bool process_started = false;
  {
    std::lock_guard lock(collector->mutex);
    process_started = std::ranges::any_of(
        collector->events, [](ava::observability::TraceEvent const& event) { return event.type == ava::observability::TraceEventType::ProcessStart; });
  }
  auto const detail = result && !result->tool_timeline.empty() ? result->tool_timeline.front().result_summary : std::string{};
  expect(result && result->final_text == "authority rejected" && prompts == 0 && preflights == 0 && !process_started &&
             detail.find("must not overlap with any AVA authority root") != std::string::npos,
         "model ToolContexts deduplicate bounded AVA authority roots and reject overlapping workspaces before prompts or processes");
}

void test_agent_loop_truncates_tool_context()
{
  auto const root = create_empty_root("agent-tool-truncate");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  {
    std::ofstream large(workspace / "large.txt", std::ios::binary | std::ios::trunc);
    large << std::string(12 * 1024, 'x');
  }
  ava::session::SessionStore store(ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "truncate"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response("data: {\"type\":\"response.function_call.added\",\"call_id\":\"call_large\",\"name\":\"read_file\"}\n\n"
                                                    "data: "
                                                    "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call_large\",\"delta\":\"{\\\"path\\\":"
                                                    "\\\"large.txt\\\"}\"}\n\n"
                                                    "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\n\n"
                                                    "data: [DONE]\n\n")});
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .max_tool_result_context_bytes = 8 * 1024,
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });
  auto result = loop.run_turn("read large", store, provider, transport);
  expect(result && transport.requests().size() == 2 && transport.requests()[1].body.find("tool result content truncated") != std::string::npos,
         "agent loop truncates native tool results before OpenAI continuation");
}

void test_unresolved_committed_function_recovery_never_partially_appends_after_a_closed_window()
{
  auto store = ava::session::SessionStore::create_ephemeral(create_empty_root("unresolved-committed-function-recovery"));
  auto target = store ? ava::session::SessionAppendTarget::create_ephemeral(*store)
                      : ava::core::Result<std::shared_ptr<ava::session::SessionAppendTarget>>(std::unexpected(store.error()));
  if (!store || !target)
    return;

  auto function_turn = [](std::string call_id) {
    ava::agent::ParsedAssistantTurn turn;
    turn.ordered_items.push_back(ava::agent::OrderedAssistantItem{
        .sequence = 0,
        .item = ava::agent::AssistantFunctionCallItem{
            .metadata = {}, .tool_call = ava::agent::ProviderToolCall{.id = std::move(call_id), .name = "read_file", .arguments_json = "{}"}}});
    turn.finish_reason = ava::provider::ProviderFinishReason::ToolCalls;
    return turn;
  };
  auto append_batch = [append_target = *target](std::vector<ava::session::SessionEntry> entries) { return append_target->append_batch(std::move(entries)); };
  auto first = ava::agent::append_assistant_turn(append_batch, function_turn("closed_call"), "openai", "gpt-5.5", {}, std::nullopt);
  auto boundary = first ? (*target)->append(ava::session::SessionEntry{.id = "closed_window_user",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::UserMessage,
                                                                       .timestamp = ava::session::now_timestamp(),
                                                                       .data_json = "{\"text\":\"later turn\"}"})
                        : ava::core::VoidResult(std::unexpected(first.error()));
  auto current = boundary ? ava::agent::append_assistant_turn(append_batch, function_turn("current_call"), "openai", "gpt-5.5", {}, std::nullopt)
                          : ava::core::Result<ava::agent::PersistedAssistantTurn>(std::unexpected(boundary.error()));
  auto authority = current ? (*target)->read_authority() : ava::core::Result<ava::session::SessionReadAuthority>(std::unexpected(current.error()));
  std::size_t synthetic_append_attempts = 0;
  ava::agent::SessionAppendSink sink = [append_target = *target, &synthetic_append_attempts](ava::session::SessionEntry entry) {
    ++synthetic_append_attempts;
    return append_target->append(std::move(entry));
  };
  auto const before = store->load();
  auto reconciled = authority
                        ? ava::agent::reconcile_unresolved_committed_function_calls(*authority, sink, ava::session::legacy_unbounded_session_read_limits())
                        : ava::core::VoidResult(std::unexpected(authority.error()));
  auto const after = store->load();
  expect(first && boundary && current && authority && !reconciled && reconciled.error().message().find("active EOF tool-result window") != std::string::npos &&
             synthetic_append_attempts == 0 && before && after && after->size() == before->size() &&
             std::ranges::equal(
                 *before, *after, {}, [](auto const& entry) { return entry.id; }, [](auto const& entry) { return entry.id; }),
         "closed unresolved v4 calls reject the whole recovery batch before any synthetic result can mutate a later current call");
}

namespace {

ava::agent::ToolTimelineStatus timeline_status(ava::agent::AgentLoopResult const& result, std::string const& call_id)
{
  auto const entry = std::ranges::find(result.tool_timeline, call_id, &ava::agent::ToolTimelineEntry::call_id);
  return entry == result.tool_timeline.end() ? ava::agent::ToolTimelineStatus::Running : entry->status;
}

}  // namespace

void test_agent_loop_secure_workspace_refuses_in_workspace_symlink_escape()
{
  auto const root = create_empty_root("agent-secure-workspace-symlink-escape");
  auto const workspace = root / "workspace";
  auto const outside = root / "outside";
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(outside);
  ava::tests::write_app_test_file(outside / "secret.txt", "LINKED-OUTSIDE-SECRET-7f3a\n");
  ava::tests::write_app_test_file(outside / "listing-marker-7f3a.txt", "listed\n");
  auto const approved_path = outside / "approved.txt";
  ava::tests::write_app_test_file(approved_path, "APPROVED-OUTSIDE-READ-7f3a\n");
  // A committed in-workspace link to an outside directory, like docs/keys -> ~/.ssh.
  std::error_code link_error;
  std::filesystem::create_directory_symlink(outside, workspace / "link", link_error);
  expect(!link_error, "secure workspace escape fixture creates a committed escaping directory symlink");
  if (link_error)
    return;

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "secure-symlink-escape"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport(
      {sse_response(tool_call_sse("call_link_read", "read_file", R"({"path":"link/secret.txt"})") +
                    tool_call_sse("call_link_list", "list_directory", R"({"path":"link"})") +
                    tool_call_sse("call_link_write", "write_file", R"({"path":"link/planted.txt","content":"planted through link"})") +
                    tool_call_sse("call_outside_read", "read_file", "{\"path\":\"" + ava::core::json::escape(approved_path.generic_string()) + "\"}") +
                    "data: [DONE]\n\n"),
       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"escape handled\"}\n\n"
                    "data: [DONE]\n\n")});
  std::vector<std::filesystem::path> prompted_targets;
  // No ToolResourceOptions/ToolExecutionOptions: the ordinary (TUI/print/RPC)
  // dispatcher built by AgentTurnExecutor::initialize_tools.
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .anchor_set = command_anchors_for_test(workspace, store.session_path().parent_path() / "spill"),
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver =
          [&prompted_targets](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        prompted_targets.push_back(prompt.target_path);
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("use the linked directory", store, provider, transport);
  auto const continuation = transport.requests().size() == 2 ? transport.requests()[1].body : std::string{};
  expect(result && result->final_text == "escape handled" && result->tool_timeline.size() == 4 &&
             timeline_status(*result, "call_link_read") == ava::agent::ToolTimelineStatus::Error &&
             timeline_status(*result, "call_link_list") == ava::agent::ToolTimelineStatus::Error &&
             timeline_status(*result, "call_link_write") == ava::agent::ToolTimelineStatus::Error,
         "print-style dispatcher refuses read, list, and write through an in-workspace symlink that escapes the workspace");
  expect(continuation.find("LINKED-OUTSIDE-SECRET-7f3a") == std::string::npos && continuation.find("listing-marker-7f3a") == std::string::npos &&
             !std::filesystem::exists(outside / "planted.txt"),
         "in-workspace symlink escapes leak no outside bytes or names and plant no outside file even when every prompt is approved");
  expect(prompted_targets.size() == 1 && prompted_targets.front() == approved_path,
         "in-workspace symlink escapes fail before any permission prompt; only the explicit outside read asks");
  expect(result && timeline_status(*result, "call_outside_read") == ava::agent::ToolTimelineStatus::Success &&
             continuation.find("APPROVED-OUTSIDE-READ-7f3a") != std::string::npos,
         "an approved explicit outside-workspace read still succeeds under the descriptor-anchored workspace");
}

void test_agent_loop_secure_workspace_read_survives_ancestor_swap_after_permission()
{
  auto const root = create_empty_root("agent-secure-workspace-ancestor-swap");
  auto const workspace = root / "workspace";
  auto const outside = root / "outside";
  std::filesystem::create_directories(workspace / "sub");
  std::filesystem::create_directories(outside);
  ava::tests::write_app_test_file(workspace / "sub" / "note.txt", "INSIDE-NOTE-41c2\n");
  ava::tests::write_app_test_file(outside / "note.txt", "SWAPPED-OUTSIDE-SECRET-41c2\n");

  ava::session::SessionStore store(
      ava::session::SessionStoreOptions{.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "secure-ancestor-swap"});
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(tool_call_sse("call_swapped_read", "read_file", R"({"path":"sub/note.txt"})") + "data: [DONE]\n\n"),
                                       sse_response("data: {\"type\":\"response.output_text.delta\",\"delta\":\"swap handled\"}\n\n"
                                                    "data: [DONE]\n\n")});
  int prompts = 0;
  int preflights = 0;
  bool swapped = false;
  ava::agent::AgentLoop loop(ava::agent::AgentLoopOptions{
      .workspace_dir = workspace,
      .anchor_set = command_anchors_for_test(workspace, store.session_path().parent_path() / "spill"),
      .mode = ava::agent::Mode::Build,
      .model = agent_loop_test::model_invocation_options(),
      .access_token = "token",
      .permission_resolver = [&prompts](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++prompts;
        return ava::permissions::PermissionResolution::Allow;
      },
      // The in-workspace read is policy-allowed; this deny preflight runs after
      // the permission identity was resolved, so it is the last moment before
      // the file open. Swap the ancestor directory for an outside symlink there.
      .auto_allow_deny_preflight = [&](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        ++preflights;
        if (prompt.operation == ava::permissions::Operation::ReadFile && !swapped)
        {
          std::error_code rename_error;
          std::filesystem::rename(workspace / "sub", workspace / "sub-original", rename_error);
          std::error_code link_error;
          if (!rename_error)
            std::filesystem::create_directory_symlink(outside, workspace / "sub", link_error);
          swapped = !rename_error && !link_error;
        }
        return ava::permissions::PermissionResolution::Allow;
      },
      .append_entry = append_route_for_test(store),
      .append_batch = append_batch_route_for_test(store),
      .session_read_authority = read_authority_for_test(store),
  });

  auto result = loop.run_turn("read the note", store, provider, transport);
  auto const continuation = transport.requests().size() == 2 ? transport.requests()[1].body : std::string{};
  expect(swapped && preflights == 1 && prompts == 0, "ancestor swap fixture replaces the checked parent directory after the permission decision");
  expect(result && result->final_text == "swap handled" && result->tool_timeline.size() == 1 &&
             timeline_status(*result, "call_swapped_read") == ava::agent::ToolTimelineStatus::Error &&
             continuation.find("SWAPPED-OUTSIDE-SECRET-41c2") == std::string::npos,
         "print-style dispatcher cannot be redirected outside the workspace by an ancestor swap after the permission check");
}
