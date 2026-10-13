#include "sys.h"
#include "tests/app_rpc_test_cases.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/app/project_trust.h"
#include "ava/app/rpc/runtime_navigation.h"
#include "ava/app/rpc/serialization.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/session/assistant_output.h"
#include "ava/session/record.h"
#include "ava/session/session_metadata.h"
#include "ava/permissions/permission.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/ids.h"
#include "ava/core/json.h"
#include "ava/core/result.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <istream>
#include <optional>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_rpc_test {

void test_app_rpc_model_commands()
{
  auto const root = create_empty_root("app-rpc-model-commands");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC model command test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in(
      "{\"id\":\"list\",\"type\":\"list_models\"}\n"
      "{\"id\":\"set\",\"type\":\"set_model\",\"provider\":\"anthropic\","
      "\"model\":\"claude-sonnet-4-5\"}\n"
      "{\"id\":\"state\",\"type\":\"get_state\"}\n"
      "{\"id\":\"stats\",\"type\":\"get_session_stats\"}\n"
      "{\"id\":\"cycle\",\"type\":\"cycle_model\"}\n");
  std::ostringstream out;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto result =
      ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});
  auto const jsonl = out.str();
  expect(result.has_value(), "RPC model command loop completes successfully");
  expect(jsonl.find("\"id\":\"list\"") != std::string::npos && jsonl.find("\"models\"") != std::string::npos &&
             jsonl.find("claude-sonnet-4-5") != std::string::npos,
         "RPC list_models returns configured model catalog");
  expect(jsonl.find("\"id\":\"set\"") != std::string::npos && jsonl.find("\"provider\":\"anthropic\"") != std::string::npos &&
             jsonl.find("\"model\":\"claude-sonnet-4-5\"") != std::string::npos,
         "RPC set_model returns updated Anthropic state");
  expect(jsonl.find("\"id\":\"state\"") != std::string::npos && jsonl.find("\"model\":\"claude-sonnet-4-5\"") != std::string::npos,
         "RPC get_state reflects selected model after set_model");
  expect(jsonl.find("\"id\":\"stats\"") != std::string::npos && jsonl.find("\"model_change\":1") != std::string::npos,
         "RPC get_session_stats reports model_change count");
  expect(jsonl.find("\"id\":\"cycle\"") != std::string::npos && jsonl.find("\"provider\":\"deepseek\"") != std::string::npos,
         "RPC cycle_model advances to the next configured provider model");
  SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);
  expect(session_w->model_selection_r()->model.provider_id == "deepseek", "RPC cycle_model updates active session model");
  auto previous = ava::app::rpc::previous_runtime_model(session_w);
  expect(previous && previous->provider_id == "anthropic" && previous->model_id == "claude-sonnet-4-5",
         "runtime previous model helper returns the configured predecessor for TUI reverse cycling");
  session_w->model_selection().change([](auto& model_selection){
    model_selection.scoped_model_cycle = std::vector<std::string>{"anthropic/claude-sonnet-4-5", "openai/gpt-5.5"};
  });
  auto scoped_next = ava::app::rpc::next_runtime_model(session_w);
  expect(scoped_next && scoped_next->provider_id == "anthropic" && scoped_next->model_id == "claude-sonnet-4-5",
         "runtime next model helper starts at the first scoped model when current model is outside the scoped cycle");
  if (scoped_next)
    session_w->model_selection().change([&scoped_next](auto& model_selection){
    model_selection.model = *scoped_next;
  });
  auto scoped_previous = ava::app::rpc::previous_runtime_model(session_w);
  expect(scoped_previous && scoped_previous->provider_id == "openai" && scoped_previous->model_id == "gpt-5.5",
         "runtime previous model helper wraps within the session-scoped model cycle");
  session_w->model_selection().change([](auto& model_selection){
    model_selection.scoped_model_cycle = std::vector<std::string>{};
  });
  auto empty_scoped_next = ava::app::rpc::next_runtime_model(session_w);
  expect(!empty_scoped_next && empty_scoped_next.error().message().find("enabled for cycling") != std::string::npos,
         "runtime model cycling fails visibly when the session-scoped model cycle is empty");
}

void test_app_rpc_reasoning_commands()
{
  auto const root = create_empty_root("app-rpc-reasoning-commands");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC reasoning command test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in(
      "{\"id\":\"set\",\"type\":\"set_reasoning\",\"reasoning_level\":\"medium\"}\n"
      "{\"id\":\"state\",\"type\":\"get_state\"}\n"
      "{\"id\":\"invalid\",\"type\":\"set_reasoning\",\"reasoning_level\":\"ultra\"}\n"
      "{\"id\":\"set-off\",\"type\":\"set_reasoning\",\"reasoning_level\":\" off \"}\n"
      "{\"id\":\"state-off\",\"type\":\"get_state\"}\n"
      "{\"id\":\"clear\",\"type\":\"clear_reasoning\"}\n");
  std::ostringstream out;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto result =
      ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
  auto const jsonl = out.str();
  expect(result.has_value(), "RPC reasoning command loop completes successfully");
  expect(jsonl.find("\"id\":\"set\"") != std::string::npos && jsonl.find("\"reasoning_enabled\":true") != std::string::npos &&
             jsonl.find("\"reasoning_level\":\"medium\"") != std::string::npos,
         "RPC set_reasoning returns enabled reasoning state");
  expect(jsonl.find("\"id\":\"state\"") != std::string::npos && jsonl.find("\"reasoning_level\":\"medium\"") != std::string::npos,
         "RPC get_state reflects selected reasoning");
  expect(jsonl.find("\"id\":\"invalid\"") != std::string::npos && jsonl.find("reasoning level is not supported") != std::string::npos,
         "RPC set_reasoning reports invalid reasoning levels");
  expect(jsonl.find("\"id\":\"set-off\"") != std::string::npos && jsonl.find("\"id\":\"state-off\"") != std::string::npos &&
             jsonl.rfind("\"reasoning_enabled\":false") != std::string::npos,
         "RPC set_reasoning off clears reasoning state instead of storing an active off selection");
  expect(jsonl.find("\"id\":\"clear\"") != std::string::npos && jsonl.rfind("\"reasoning_enabled\":false") != std::string::npos,
         "RPC clear_reasoning disables reasoning state");
  expect(!session_r->model_selection_r()->reasoning, "RPC clear_reasoning updates active session state");

  auto entries = session_r->store.load();
  expect(entries.has_value(), "RPC reasoning command test reloads entries");
  if (entries)
  {
    auto const reasoning_changes = std::ranges::count_if(*entries, [](auto const& entry) { return entry.type == ava::session::EntryType::ReasoningChange; });
    expect(reasoning_changes == 2, "RPC reasoning commands persist set and clear reasoning_change entries");
  }
}

void test_app_rpc_reasoning_model_serialization_exposes_resolved_maps()
{
  auto const root = create_empty_root("app-rpc-reasoning-model-serialization");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC reasoning serialization test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in(
      "{\"id\":\"models\",\"type\":\"list_models\"}\n"
      "{\"id\":\"model\",\"type\":\"set_model\",\"provider\":\"deepseek\",\"model\":\"deepseek-v4-flash\"}\n"
      "{\"id\":\"set\",\"type\":\"set_reasoning\",\"reasoning_level\":\"xhigh\"}\n"
      "{\"id\":\"state\",\"type\":\"get_state\"}\n");
  std::ostringstream out;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto result =
      ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
  auto const jsonl = out.str();

  expect(result.has_value(), "RPC reasoning serialization loop completes successfully");
  expect(jsonl.find("\"id\":\"models\"") != std::string::npos && jsonl.find("\"raw_reasoning_levels\"") != std::string::npos &&
             jsonl.find("\"reasoning_level_map\":{") != std::string::npos && jsonl.find("\"xhigh\":\"max\"") != std::string::npos &&
             jsonl.find("\"minimal\":null") != std::string::npos,
         "RPC list_models exposes raw levels plus resolved reasoning map policy");
  expect(jsonl.find("\"id\":\"set\"") != std::string::npos && jsonl.find("\"reasoning_level\":\"xhigh\"") != std::string::npos &&
             jsonl.find("\"reasoning_provider_level\":\"max\"") != std::string::npos && jsonl.find("\"id\":\"state\"") != std::string::npos,
         "RPC get_state exposes provider-level reasoning rewrites when they differ from user-facing levels");

  auto entries = session_r->store.load();
  auto const persisted_provider_level = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                                          return entry.type == ava::session::EntryType::ReasoningChange &&
                                                 entry.data_json.find("\"level\":\"xhigh\"") != std::string::npos &&
                                                 entry.data_json.find("\"provider_level\":\"max\"") != std::string::npos;
                                        });
  expect(persisted_provider_level, "RPC reasoning changes persist provider-level rewrites for replay diagnostics");
}

void test_app_rpc_protocol_version_and_session_commands()
{
  auto const root = create_empty_root("app-rpc-protocol-session");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC protocol/session test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  ava::session::SessionMetadataUpdate metadata_update;
  metadata_update.name = "Stats audit";
  metadata_update.actor = "test";

  CRITICAL_AREA_BEGIN_W(session);

  auto initial_id = session_w->store.session_id();
  auto appended_metadata = session_w->append_metadata_1(std::move(metadata_update));

  auto appended_user = session_w->append_owned(ava::session::SessionEntry{.id = "entry_user",
                                                                        .parent_id = "",
                                                                        .type = ava::session::EntryType::UserMessage,
                                                                        .timestamp = ava::session::now_timestamp(),
                                                                        .data_json = "{\"text\":\"hello\"}"});
  auto appended_internal_replay = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                   .parent_id = "",
                                                                                   .type = ava::session::EntryType::UserMessage,
                                                                                   .timestamp = ava::session::now_timestamp(),
                                                                                   .data_json = "{\"text\":\"hidden rpc replay\","
                                                                                                "\"internal_replay\":true,"
                                                                                                "\"replay_of\":\"entry_user\","
                                                                                                "\"reason\":\"test\"}"});
  auto appended_assistant = session_w->append_owned(ava::session::SessionEntry{.id = "entry_assistant",
                                                                             .parent_id = "",
                                                                             .type = ava::session::EntryType::AssistantMessage,
                                                                             .timestamp = ava::session::now_timestamp(),
                                                                             .data_json = "{\"text\":\"answer\",\"usage\":{\"input_tokens\":1,"
                                                                                          "\"output_tokens\":1,\"total_tokens\":2,"
                                                                                          "\"cost_usd\":0.001,"
                                                                                          "\"source\":\"provider\"}}"});
  auto appended_unpriced_assistant = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                      .parent_id = "",
                                                                                      .type = ava::session::EntryType::AssistantMessage,
                                                                                      .timestamp = ava::session::now_timestamp(),
                                                                                      .data_json = "{\"text\":\"unknown cost\",\"usage\":{"
                                                                                                   "\"input_tokens\":1,\"cache_read_tokens\":1,"
                                                                                                   "\"total_tokens\":1,"
                                                                                                   "\"source\":\"provider\"}}"});
  auto appended_reasoning = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                             .parent_id = "",
                                                                             .type = ava::session::EntryType::ReasoningBlock,
                                                                             .timestamp = ava::session::now_timestamp(),
                                                                             .data_json = "{\"provider\":\"openai\","
                                                                                          "\"model\":\"gpt-5.5\","
                                                                                          "\"format\":\"openai_responses\","
                                                                                          "\"text\":\"visible reasoning\","
                                                                                          "\"signature\":\"rpc-secret-signature\"}"});
  auto appended_redacted_reasoning = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                      .parent_id = "",
                                                                                      .type = ava::session::EntryType::ReasoningBlock,
                                                                                      .timestamp = ava::session::now_timestamp(),
                                                                                      .data_json = "{\"provider\":\"openai\","
                                                                                                   "\"model\":\"gpt-5.5\","
                                                                                                   "\"format\":\"openai_responses\","
                                                                                                   "\"text\":\"hidden redacted rpc reasoning\","
                                                                                                   "\"signature\":\"rpc-redacted-secret-signature\","
                                                                                                   "\"redacted\": true }"});
  auto appended_mode = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                        .parent_id = "",
                                                                        .type = ava::session::EntryType::ModeChange,
                                                                        .timestamp = ava::session::now_timestamp(),
                                                                        .data_json = "{\"mode\":\"build\"}"});
  auto appended_compaction = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                              .parent_id = "",
                                                                              .type = ava::session::EntryType::Compaction,
                                                                              .timestamp = ava::session::now_timestamp(),
                                                                              .data_json = "{\"summary\":\"prior\"}"});
  auto appended_cancel = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                          .parent_id = "",
                                                                          .type = ava::session::EntryType::Cancel,
                                                                          .timestamp = ava::session::now_timestamp(),
                                                                          .data_json = "{}"});
  auto appended_branch_summary = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                  .parent_id = "",
                                                                                  .type = ava::session::EntryType::BranchSummary,
                                                                                  .timestamp = ava::session::now_timestamp(),
                                                                                  .data_json = "{\"schema_version\":1,\"source_session_id\":\"" + initial_id +
                                                                                               "\",\"branch_root_entry_id\":\"entry_user\"," +
                                                                                               "\"branch_tip_entry_id\":\"entry_assistant\","
                                                                                               "\"summary\":\"branch summary\",\"provider\":\"openai\"," +
                                                                                               "\"model\":\"gpt-test\",\"reason\":\"test\"}"});
  auto v4_text_data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
      .assistant_turn_id = "turn_rpc_projection",
      .sequence = 0,
      .kind = ava::session::AssistantOutputItemKind::Text,
      .provider_item_id = "PRIVATE_RPC_ITEM",
      .provider_output_index = 0,
      .payload = ava::session::AssistantOutputText{.text = "rpc v4 commentary ", .assistant_phase = ava::session::AssistantOutputTextPhase::Commentary}});
  auto v4_reasoning_data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
      .assistant_turn_id = "turn_rpc_projection",
      .sequence = 1,
      .kind = ava::session::AssistantOutputItemKind::Reasoning,
      .provider_item_id = std::nullopt,
      .provider_output_index = std::nullopt,
      .payload = ava::session::AssistantOutputReasoning{.text = "rpc v4 reasoning",
                                                        .format = "openai_responses",
                                                        .redacted = false,
                                                        .signature = "PRIVATE_RPC_SIGNATURE",
                                                        .redacted_data = std::nullopt,
                                                        .native_item_json = "{\"id\":\"PRIVATE_RPC_NATIVE\",\"type\":\"reasoning\",\"summary\":[]}"}});
  auto v4_function_data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
      .assistant_turn_id = "turn_rpc_projection",
      .sequence = 2,
      .kind = ava::session::AssistantOutputItemKind::FunctionCall,
      .provider_item_id = std::nullopt,
      .provider_output_index = std::nullopt,
      .payload =
          ava::session::AssistantOutputFunctionCall{.call_id = "call_rpc_projection", .name = "read_file", .arguments_json = "{\"path\":\"README.md\"}"}});
  auto v4_commit_data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{.assistant_turn_id = "turn_rpc_projection",
                                                                                                                  .item_count = 3,
                                                                                                                  .provider = "openai",
                                                                                                                  .model = "gpt-5.5",
                                                                                                                  .finish_reason = "tool_calls",
                                                                                                                  .usage_json = std::nullopt});
  auto appended_v4_text = v4_text_data && session_w->append_owned(ava::session::SessionEntry{.id = "rpc_v4_text",
                                                                                           .parent_id = "",
                                                                                           .type = ava::session::EntryType::AssistantOutputItem,
                                                                                           .timestamp = ava::session::now_timestamp(),
                                                                                           .data_json = *v4_text_data});
  auto appended_v4_reasoning = v4_reasoning_data && session_w->append_owned(ava::session::SessionEntry{.id = "rpc_v4_reasoning",
                                                                                                     .parent_id = "",
                                                                                                     .type = ava::session::EntryType::AssistantOutputItem,
                                                                                                     .timestamp = ava::session::now_timestamp(),
                                                                                                     .data_json = *v4_reasoning_data});
  auto appended_v4_function = v4_function_data && session_w->append_owned(ava::session::SessionEntry{.id = "rpc_v4_function",
                                                                                                   .parent_id = "",
                                                                                                   .type = ava::session::EntryType::AssistantOutputItem,
                                                                                                   .timestamp = ava::session::now_timestamp(),
                                                                                                   .data_json = *v4_function_data});
  auto appended_v4_commit = v4_commit_data && session_w->append_owned(ava::session::SessionEntry{.id = "rpc_v4_commit",
                                                                                               .parent_id = "",
                                                                                               .type = ava::session::EntryType::AssistantTurnCommit,
                                                                                               .timestamp = ava::session::now_timestamp(),
                                                                                               .data_json = *v4_commit_data});
  auto appended_v4_result =
      session_w->append_owned(ava::session::SessionEntry{.id = "rpc_v4_result",
                                                       .parent_id = "",
                                                       .type = ava::session::EntryType::ToolResult,
                                                       .timestamp = ava::session::now_timestamp(),
                                                       .data_json = "{\"assistant_output_entry_id\":\"rpc_v4_function\",\"call_id\":\"call_rpc_projection\","
                                                                    "\"name\":\"read_file\",\"success\":true,\"result\":\"rpc tool result\"}"});
  expect(appended_metadata.has_value() && appended_user.has_value() && appended_internal_replay.has_value() && appended_assistant.has_value() &&
             appended_unpriced_assistant.has_value() && appended_reasoning.has_value() && appended_redacted_reasoning.has_value() &&
             appended_mode.has_value() && appended_compaction.has_value() && appended_cancel.has_value() && appended_branch_summary.has_value() &&
             appended_v4_text && appended_v4_reasoning && appended_v4_function && appended_v4_commit && appended_v4_result.has_value(),
         "RPC protocol/session test appends legacy and committed v4 message history");

  CRITICAL_AREA_END_W(session);

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in(
      "{\"id\":\"proto\",\"type\":\"get_protocol\",\"protocol_version\":1}\n"
      "{\"id\":\"messages\",\"type\":\"get_messages\"}\n"
      "{\"id\":\"stats\",\"type\":\"get_session_stats\"}\n"
      "{\"id\":\"validate\",\"type\":\"validate_session\"}\n"
      "{\"id\":\"new\",\"type\":\"new_session\"}\n"
      "{\"id\":\"switch\",\"type\":\"switch_session\",\"session_id\":\"" +
      initial_id + "\"}\n");
  std::ostringstream out;
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  auto result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, ava::app::rpc::RpcInputWake{});
  auto const jsonl = out.str();
  auto const current_entry_version = std::string("\"version\":") + std::to_string(ava::session::kCurrentSessionEntryVersion);
  expect(result.has_value(), "RPC protocol/session loop completes successfully");
  expect(jsonl.find("\"id\":\"proto\"") != std::string::npos && jsonl.find("\"protocol_version\":1") != std::string::npos &&
             jsonl.find("\"supported_protocol_versions\":[1]") != std::string::npos &&
             jsonl.find("\"session_entry_version\":" + std::to_string(ava::session::kCurrentSessionEntryVersion)) != std::string::npos &&
             jsonl.find("\"supported_session_entry_versions\":[0,1,2,3,4]") != std::string::npos &&
             jsonl.find("\"capabilities\":[\"direct_bash_rpc\",\"job_controls\"]") != std::string::npos &&
             jsonl.find("\"direct_command_types\":[\"run_bash\",\"run_command\",\"list_jobs\"") != std::string::npos,
         "RPC get_protocol reports supported protocol, session entry versions, and direct job capabilities");
  expect(jsonl.find("\"id\":\"messages\"") != std::string::npos && jsonl.find("\"messages\"") != std::string::npos &&
             jsonl.find(current_entry_version) != std::string::npos && jsonl.find("hello") != std::string::npos && jsonl.find("answer") != std::string::npos &&
             jsonl.find("visible reasoning") != std::string::npos && jsonl.find("hidden redacted rpc reasoning") == std::string::npos &&
             jsonl.find("rpc v4 commentary ") != std::string::npos && jsonl.find("rpc v4 reasoning") != std::string::npos &&
             jsonl.find("call_rpc_projection") != std::string::npos && jsonl.find("rpc tool result") != std::string::npos &&
             jsonl.find("\"ordered_output\":[{\"sequence\":0,\"kind\":\"text\",\"text\":\"rpc v4 commentary \",\"assistant_phase\":\"commentary\"}") !=
                 std::string::npos &&
             jsonl.find("\"sequence\":1,\"kind\":\"reasoning\"") != std::string::npos &&
             jsonl.find("\"sequence\":2,\"kind\":\"function_call\"") != std::string::npos && jsonl.find("\"signature_present\":true") != std::string::npos &&
             jsonl.find("hidden rpc replay") == std::string::npos && jsonl.find("rpc-secret-signature") == std::string::npos &&
             jsonl.find("rpc-redacted-secret-signature") == std::string::npos && jsonl.find("PRIVATE_RPC_ITEM") == std::string::npos &&
             jsonl.find("PRIVATE_RPC_SIGNATURE") == std::string::npos && jsonl.find("PRIVATE_RPC_NATIVE") == std::string::npos &&
             jsonl.find("assistant_output_entry_id") == std::string::npos,
         "RPC get_messages returns projected committed v4 content without private replay metadata or exact bindings");
  expect(jsonl.find("\"id\":\"stats\"") != std::string::npos && jsonl.find("\"entry_count\":16") != std::string::npos &&
             jsonl.find("\"session_metadata\":1") != std::string::npos && jsonl.find("\"user_message\":1") != std::string::npos &&
             jsonl.find("\"assistant_message\":3") != std::string::npos && jsonl.find("\"reasoning_block\":3") != std::string::npos &&
             jsonl.find("\"tool_call\":1") != std::string::npos && jsonl.find("\"tool_result\":1") != std::string::npos &&
             jsonl.find("\"mode_change\":1") != std::string::npos && jsonl.find("\"compaction\":1") != std::string::npos &&
             jsonl.find("\"branch_summary\":1") != std::string::npos && jsonl.find("\"cancel\":1") != std::string::npos,
         "RPC get_session_stats returns session counters");
  expect(jsonl.find("\"known_cost_usd\":0.001") != std::string::npos && jsonl.find("\"cost_complete\":false") != std::string::npos &&
             jsonl.find("\"unknown_cost_entries\":1") != std::string::npos && jsonl.find("\"total_cost_usd\"") == std::string::npos,
         "RPC get_session_stats omits incomplete total cost and reports known cost metadata");
  expect(jsonl.find("\"id\":\"validate\"") != std::string::npos && jsonl.find("\"ok\":true") != std::string::npos &&
             jsonl.find("\"error_count\":0") != std::string::npos,
         "RPC validate_session reports a clean replay audit for the active session");
  expect(jsonl.find("\"id\":\"new\"") != std::string::npos && jsonl.find("\"created\":true") != std::string::npos,
         "RPC new_session creates and switches to a new active session");
  SCOPED_CRITICAL_AREA_R(session_r, unlocked_session);
  expect(session_r->store.session_id() == initial_id && jsonl.find("\"id\":\"switch\"") != std::string::npos,
         "RPC switch_session switches back to the requested session");
}

void test_app_rpc_messages_keep_v1_payloads_when_ordered_output_does_not_fit()
{
  auto const root = create_empty_root("app-rpc-messages-ordered-output-caps");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  std::filesystem::permissions(root, std::filesystem::perms::owner_all);

  auto open_session = [&](std::string_view name) {
    ava::app::runtime::OpenContext options;
    options.workspace_dir = workspace / std::string(name);
    options.current_dir = options.workspace_dir;
    options.paths = app_test_paths(root / std::string(name));
    std::filesystem::create_directories(options.workspace_dir);
    return ava::app::runtime::Session::open(options);
  };
  auto append_v4_text_turn = [](ava::app::runtime::session_ts& unlocked_session, std::size_t index, std::string text) {
    auto const turn_id = "rpc_cap_turn_" + std::to_string(index);
    auto const item_id = "rpc_cap_item_" + std::to_string(index);
    auto item_data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
        .assistant_turn_id = turn_id,
        .sequence = 0,
        .kind = ava::session::AssistantOutputItemKind::Text,
        .provider_item_id = "PRIVATE_RPC_CAP_ITEM_" + std::to_string(index),
        .provider_output_index = 0,
        .payload = ava::session::AssistantOutputText{.text = std::move(text), .assistant_phase = ava::session::AssistantOutputTextPhase::Commentary}});
    auto commit_data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{
        .assistant_turn_id = turn_id, .item_count = 1, .provider = "openai", .model = "gpt-5.5", .finish_reason = "completed", .usage_json = std::nullopt});
    if (!item_data || !commit_data)
      return false;
    SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);
    auto item = session_w->append_owned(ava::session::SessionEntry{.id = item_id,
                                                                .parent_id = "",
                                                                .type = ava::session::EntryType::AssistantOutputItem,
                                                                .timestamp = ava::session::now_timestamp(),
                                                                .data_json = std::move(*item_data)});
    auto commit = session_w->append_owned(ava::session::SessionEntry{.id = "rpc_cap_commit_" + std::to_string(index),
                                                                  .parent_id = item_id,
                                                                  .type = ava::session::EntryType::AssistantTurnCommit,
                                                                  .timestamp = ava::session::now_timestamp(),
                                                                  .data_json = std::move(*commit_data)});
    return item.has_value() && commit.has_value();
  };

  auto single_session_result = open_session("single");
  bool appended_single = false;
  std::optional<std::string> single_json;
  if (single_session_result)
  {
    appended_single = append_v4_text_turn(*single_session_result, 0, "RPC_SINGLE_" + std::string(6'000, 's'));
    if (appended_single)
    {
      auto serialized = ava::app::runtime::session_ts::wat(*single_session_result)->messages_result_json();
      if (serialized)
        single_json = std::move(*serialized);
    }
  }
  expect(single_json && single_json->find("RPC_SINGLE_") != std::string::npos && single_json->find("\"tool_calls\":0") != std::string::npos &&
             single_json->find("\"ordered_output\"") == std::string::npos && single_json->find("\"message_count\":1") != std::string::npos &&
             single_json->find("\"ordered_output_truncated\":true") != std::string::npos && single_json->find("PRIVATE_RPC_CAP_ITEM") == std::string::npos,
         "RPC keeps a 5--8 KiB v1 assistant payload when additive ordered output exceeds its per-entry cap");

  auto near_cap_session_result = open_session("near-cap");
  bool appended_near_cap = false;
  std::optional<std::string> near_cap_json;
  if (near_cap_session_result)
  {
    appended_near_cap = true;
    for (std::size_t index = 0; appended_near_cap && index < 190; ++index)
      appended_near_cap = append_v4_text_turn(*near_cap_session_result, index, "RPC_CAP_" + std::to_string(index) + "_" + std::string(6'000, 'n'));
    if (appended_near_cap)
    {
      auto serialized = ava::app::runtime::session_ts::wat(*near_cap_session_result)->messages_result_json();
      if (serialized)
        near_cap_json = std::move(*serialized);
    }
  }
  auto const near_cap_count = near_cap_json ? ava::core::json::integer_field(*near_cap_json, "message_count") : std::nullopt;
  expect(near_cap_json && near_cap_json->size() <= ava::app::rpc::kMaxRpcMessagesResponseBytes && near_cap_count && *near_cap_count > 150 &&
             near_cap_json->find("RPC_CAP_150_") != std::string::npos && near_cap_json->find("\"original_bytes\"") == std::string::npos &&
             near_cap_json->find("\"ordered_output_truncated\":true") != std::string::npos && near_cap_json->find("PRIVATE_RPC_CAP_ITEM") == std::string::npos,
         "near-cap RPC histories retain their legacy message count and data while reporting omitted ordered detail");
}

void test_app_rpc_protocol_version_and_resolver_reply_errors()
{
  auto const root = create_empty_root("app-rpc-protocol-errors");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC protocol error test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::string input =
      "{\"id\":\"bad-version\",\"type\":\"get_state\",\"protocol_version\":999}\n"
      "{\"id\":\"reply-missing\",\"type\":\"permission_reply\"}\n"
      "{\"id\":\"bad-version-type\",\"type\":\"get_state\",\"protocol_version\":\"1\"}\n"
      "{\"id\":\"oversized-reply\",\"type\":\"permission_reply\",\"request_id\":\"" +
      std::string(257, 'r') +
      "\",\"correlation_id\":\"p1\",\"decision\":\"allow\"}\n"
      "{\"id\":\"reply\",\"type\":\"question_reply\",\"request_id\":\"question_1\",\"correlation_id\":\"p1\","
      "\"answer\":\"ok\"}\n"
      "{\"id\":\"state\",\"type\":\"get_state\",\"protocol_version\":1}\n";
  std::istringstream in(input);
  std::ostringstream out;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto result =
      ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});
  auto const jsonl = out.str();
  expect(result.has_value(), "RPC protocol error loop recovers after unsupported commands");
  expect(jsonl.find("unsupported RPC protocol version") != std::string::npos && jsonl.find("RPC protocol_version must be an integer") != std::string::npos &&
             jsonl.find("permission_reply requires request_id") != std::string::npos && jsonl.find("\"id\":\"oversized-reply\"") != std::string::npos &&
             jsonl.find("RPC identifier is too long") != std::string::npos &&
             jsonl.find("RPC resolver reply has no matching pending request") != std::string::npos && jsonl.find("\"id\":\"state\"") != std::string::npos &&
             jsonl.find("\"success\":true") != std::string::npos,
         "RPC version and resolver reply errors are in-band and recoverable");
}

void test_app_rpc_mcp_command_responses()
{
  expect(!std::string_view(AVA_FAKE_MCP_SERVER_PATH).empty(), "RPC MCP command test has fake server path");
  if (std::string_view(AVA_FAKE_MCP_SERVER_PATH).empty())
    return;

  auto const root = create_empty_root("app-rpc-mcp-commands");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  write_app_test_file(workspace / ".ava" / "mcp.json", app_test_mcp_config_json("demo", "Demo MCP", AVA_FAKE_MCP_SERVER_PATH));
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(),
         trusted ? "RPC MCP command test trusts project config" : "RPC MCP command test trusts project config: " + trusted.error().format());

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto application_scope = ava::process::ProcessScopeV1::application(std::make_shared<ava::process::Supervisor>());
  expect(application_scope.has_value(), "RPC MCP fixture admits test-owned application process authority");
  if (!application_scope)
    return;
  open_context.application_process_scope = *application_scope;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC MCP command test opens runtime session");
  if (!session)
    return;

  std::vector<ava::permissions::PermissionPrompt> prompts;
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.permission_resolver =
      [&prompts](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    prompts.push_back(prompt);
    return ava::permissions::PermissionResolution::Allow;
  };

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  std::istringstream in(
      "{\"id\":\"mcp-list\",\"type\":\"list_mcp_servers\"}\n"
      "{\"id\":\"mcp-inspect\",\"type\":\"inspect_mcp_server\",\"server_id\":\"demo\"}\n"
      "{\"id\":\"mcp-tools\",\"type\":\"list_mcp_tools\",\"server_id\":\"demo\"}\n"
      "{\"id\":\"mcp-restart\",\"type\":\"restart_mcp_server\",\"server_id\":\"demo\"}\n");
  std::ostringstream out;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, ava::app::rpc::RpcInputWake{});
  auto const jsonl = out.str();

  auto const has_id = [&jsonl](std::string_view id) { return jsonl.find("\"id\":\"" + std::string(id) + "\"") != std::string::npos; };
  expect(result.has_value(), "RPC MCP command loop completes successfully");
  expect(has_id("mcp-list") && has_id("mcp-inspect") && has_id("mcp-tools") && has_id("mcp-restart"), "RPC MCP command responses include all request ids");
  expect(jsonl.find("MCP servers:") != std::string::npos && jsonl.find("Demo MCP") != std::string::npos && jsonl.find("MCP server demo") != std::string::npos &&
             jsonl.find("MCP tools for demo") != std::string::npos && jsonl.find("fake-mcp") != std::string::npos && jsonl.find("echo") != std::string::npos &&
             jsonl.find("mcp_demo_echo") != std::string::npos && jsonl.find("next discovery or tool call will launch a fresh process") != std::string::npos,
         "RPC MCP command responses expose list, inspect, tools, and restart output");

  auto const has_launch_prompt = std::ranges::any_of(
      prompts, [](auto const& prompt) { return prompt.operation == ava::permissions::Operation::McpServerLaunch && prompt.tool_name == "mcp_tools"; });
  auto const has_connect_prompt = std::ranges::any_of(prompts, [](auto const& prompt) {
    return prompt.operation == ava::permissions::Operation::McpServerConnect && prompt.tool_name == "mcp_tools" && prompt.command == "demo";
  });
  expect(has_launch_prompt && has_connect_prompt, "RPC list_mcp_tools requests MCP launch and connect permissions before allowing discovery");
}

void test_app_rpc_command_responses_for_context_compact_export()
{
  auto const root = create_empty_root("app-rpc-commands");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  {
    std::ofstream file(workspace / "AGENTS.md", std::ios::binary | std::ios::trunc);
    file << "rpc command context\n";
  }
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.rpc" / "plugin.json", app_test_plugin_manifest_json("com.example.rpc", "RPC Plugin"));
  write_app_test_file(workspace / ".ava" / "plugins" / "com.example.rpcbad" / "plugin.json", "{not-json");
  auto const rpc_install_source = root / "rpc-install-source";
  write_app_test_file(rpc_install_source / "plugin.json", app_test_plugin_manifest_json("com.example.rpcinstall", "RPC Installed Plugin"));
  auto trusted = ava::app::set_project_trust_decision(paths, workspace, true);
  expect(trusted.has_value(),
         trusted ? "RPC command test trusts project plugin resources" : "RPC command test trusts project plugin resources: " + trusted.error().format());

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC command test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  std::string const rpc_summary =
      "# Goal\nRemember RPC facts\n# Constraints / Preferences\nNone noted.\n# Decisions\nNone noted.\n"
      "# Files Read or Modified\nNone noted.\n# Unresolved Tasks\nNone noted.\n# Next Steps\nContinue.";
  ava::tests::FakeTransport transport(
      {ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "{\"output_text\":\"" + ava::core::json::escape(rpc_summary) + "\"}"}});
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  runtime_options.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    return ava::permissions::PermissionResolutionDecision{ava::permissions::PermissionResolution::Allow, "test export write"};
  };
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push(std::string("{\"id\":\"plugins\",\"type\":\"list_plugins\"}\n") +
                    "{\"id\":\"plugin-enable\",\"type\":\"enable_plugin\",\"plugin_id\":\"com.example.rpc\"}\n"
                    "{\"id\":\"plugin-inspect\",\"type\":\"inspect_plugin\",\"plugin_id\":\"com.example.rpc\"}\n"
                    "{\"id\":\"plugin-validate\",\"type\":\"validate_plugin\",\"path\":\".ava/plugins/com.example.rpc/plugin.json\"}\n" +
                    "{\"id\":\"plugin-install\",\"type\":\"install_plugin\",\"path\":\"" + ava::core::json::escape(rpc_install_source.generic_string()) +
                    "\"}\n"
                    "{\"id\":\"plugin-remove\",\"type\":\"remove_plugin\",\"plugin_id\":\"com.example.rpcinstall\"}\n"
                    "{\"id\":\"plugin-failures\",\"type\":\"plugin_failures\"}\n"
                    "{\"id\":\"ctx\",\"type\":\"context\"}\n"
                    "{\"id\":\"read\",\"type\":\"invoke_command\",\"name\":\"read\",\"command_arguments\":\"AGENTS.md\"}\n"
                    "{\"id\":\"cmp\",\"type\":\"compact\",\"instructions\":\"remember rpc facts\"}\n");
  bool const compacted = output_buffer.wait_contains("compaction summary recorded", std::chrono::seconds(2));
  input_buffer.push(
      "{\"id\":\"exp\",\"type\":\"export\"}\n"
      "{\"id\":\"exp-html\",\"type\":\"export_html\"}\n"
      "{\"id\":\"exp-html-file\",\"type\":\"export_html\",\"outputPath\":\"rpc-session.html\"}\n");
  bool const exported = output_buffer.wait_contains("\"id\":\"exp-html-file\"", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();
  auto const jsonl = output_buffer.str();
  std::ifstream exported_html_file(workspace / "rpc-session.html", std::ios::binary);
  std::ostringstream exported_html_text;
  exported_html_text << exported_html_file.rdbuf();
  expect(result.has_value() && compacted && exported, "RPC context/compact/export loop completes successfully");
  expect(jsonl.find("\"id\":\"plugins\"") != std::string::npos && jsonl.find("com.example.rpc") != std::string::npos &&
             jsonl.find("\"id\":\"plugin-enable\"") != std::string::npos && jsonl.find("No plugin process was started") != std::string::npos &&
             jsonl.find("\"id\":\"plugin-inspect\"") != std::string::npos && jsonl.find("status: enabled") != std::string::npos &&
             jsonl.find("\"id\":\"plugin-validate\"") != std::string::npos && jsonl.find("Valid plugin manifest") != std::string::npos &&
             jsonl.find("\"id\":\"plugin-install\"") != std::string::npos &&
             jsonl.find("Installed global plugin com.example.rpcinstall") != std::string::npos && jsonl.find("\"id\":\"plugin-remove\"") != std::string::npos &&
             jsonl.find("Removed global plugin com.example.rpcinstall") != std::string::npos &&
             !std::filesystem::exists(paths.ava_config_dir / "plugins" / "com.example.rpcinstall") &&
             jsonl.find("\"id\":\"plugin-failures\"") != std::string::npos && jsonl.find("com.example.rpcbad") != std::string::npos &&
             jsonl.find("\"id\":\"ctx\"") != std::string::npos && jsonl.find("AGENTS.md") != std::string::npos &&
             jsonl.find("\"id\":\"read\"") != std::string::npos && jsonl.find("\"tool_timeline\"") != std::string::npos &&
             jsonl.find("\"structured_result\":{\"schema_version\":1") != std::string::npos && jsonl.find("\"tool\":\"read\"") != std::string::npos &&
             jsonl.find("\"id\":\"cmp\"") != std::string::npos && jsonl.find("\"name\":\"compaction_start\"") != std::string::npos &&
             jsonl.find("\"name\":\"compaction_end\"") != std::string::npos && jsonl.find("compaction summary recorded") != std::string::npos &&
             jsonl.find("\"id\":\"exp\"") != std::string::npos && jsonl.find("# AVA Session Export") != std::string::npos &&
             jsonl.find("remember rpc facts") != std::string::npos && jsonl.find("\"id\":\"exp-html\"") != std::string::npos &&
             jsonl.find("<!doctype html>") != std::string::npos && jsonl.find("\"id\":\"exp-html-file\"") != std::string::npos &&
             jsonl.find("format: html") != std::string::npos && exported_html_text.str().find("<!doctype html>") != std::string::npos &&
             exported_html_text.str().find("# AVA Session Export") != std::string::npos,
         "RPC command responses expose dispatcher output plus Pi-style export_html records");
}

}  // namespace ava::tests::app_rpc_test
