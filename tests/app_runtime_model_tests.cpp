#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/app/commands.h"
#include "ava/app/frontend/FrontEnd.h"
#include "ava/app/frontend/UnlockedData.h"
#include "ava/app/reasoning_controls.h"
#include "ava/app/rpc/input.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/RunOptions.h"
#include "ava/app/runtime/OpenContext.h"
#include "ava/app/runtime/Session.h"
#include "ava/app/runtime_model.h"
#include "ava/app/runtime_reasoning.h"
#include "ava/agent/message_builder.h"
#include "ava/config/model_config.h"
#include "ava/http/transport.h"
#include "ava/session/assistant_output.h"
#include "ava/session/attachments.h"
#include "ava/session/record.h"
#include "ava/session/session_store.h"
#include "ava/session/validation.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/error.h"
#include "ava/core/ids.h"
#include "ava/core/result.h"

#include <algorithm>
#include <concepts>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#ifndef AVA_FAKE_MCP_SERVER_PATH
#define AVA_FAKE_MCP_SERVER_PATH ""
#endif

namespace ava::tests::app_runtime_tests {

using namespace ava::tests;

namespace {

class CallbackFrontEnd final : public ava::app::frontend::FrontEnd
{
 public:
  std::function<void(ava::app::frontend::DataListIndex)> on_data_changed;

  void initialize_frontend_state(app::runtime::SessionConstReadAccess const&) override { }

  void data_changed(ava::app::frontend::DataListIndex index) override
  {
    if (on_data_changed)
      on_data_changed(index);
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

} // namespace

void test_unlocked_data_access_and_notifications()
{
  using data_type = ava::app::frontend::UnlockedData<std::string>;
  static_assert(std::derived_from<data_type::wat, data_type::rat>);
  static_assert(std::derived_from<data_type::wat, data_type::crat>);
  static_assert(std::move_constructible<data_type::wat>);

  CallbackFrontEnd frontend;
  data_type data(&frontend, "seven");
  int notifications = 0;
  std::string observed_value;
  frontend.on_data_changed = [&](ava::app::frontend::DataListIndex) {
    ++notifications;
    data_type::crat data_r(std::as_const(data));
    observed_value = *data_r;
  };

  expect(notifications == 0, "constructing registered observable data does not emit a change notification");
  {
    data_type::rat data_r(data);
    expect(*data_r == "seven", "standard mutable-owner read access accepts an observable owner with a protected Unlocked base");
  }
  {
    data_type::crat data_cr(std::as_const(data));
    expect(*data_cr == "seven", "standard const-owner read access accepts an observable owner with a protected Unlocked base");
  }
  expect(notifications == 0, "read access guards do not emit change notifications");

  {
    data_type::wat data_w(data);
    auto read_rat = [](data_type::rat const& data_r) { return *data_r; };
    auto read_crat = [](data_type::crat const& data_r) { return *data_r; };
    expect(read_rat(data_w) == "seven" && read_crat(data_w) == "seven", "write access retains inherited rat and crat reference conversions");
  }
  expect(notifications == 1 && observed_value == "seven", "a write guard notifies after unlocking even when no value was modified");

  {
    data_type::wat first(data);
    data_type::wat moved(std::move(first));
    *moved = "nine";
  }
  expect(notifications == 2 && observed_value == "nine", "moving a write guard transfers its single pending notification");

  data.change([](std::string& value) { value = "eleven"; });
  expect(notifications == 3 && observed_value == "eleven", "change emits exactly one unlocked notification");

  data_type replacement("forty-two");
  data = std::move(replacement);
  expect(notifications == 4 && observed_value == "forty-two", "move assignment preserves destination registration and publishes the transferred value");
}

std::optional<std::string> app_error_context(ava::core::Error const& error, std::string_view key)
{
  auto const found = std::ranges::find_if(error.context(), [key](ava::core::ErrorContext const& context) { return context.key == key; });
  if (found == error.context().end())
    return std::nullopt;
  return found->value;
}

void test_request_projection_validates_committed_v4_history()
{
  auto const workspace = create_empty_root("runtime-model-v4-history");
  std::filesystem::create_directories(workspace);
  auto store = ava::session::SessionStore::create_ephemeral(workspace);
  expect(store.has_value(), "v4 model-switch test creates an ephemeral session store");
  if (!store)
    return;

  auto function_data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
      .assistant_turn_id = "turn_model_v4",
      .sequence = 0,
      .kind = ava::session::AssistantOutputItemKind::FunctionCall,
      .provider_item_id = std::nullopt,
      .provider_output_index = std::nullopt,
      .payload = ava::session::AssistantOutputFunctionCall{.call_id = "call_model_v4", .name = "read_file", .arguments_json = "{}"}});
  auto reasoning_data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
      .assistant_turn_id = "turn_model_v4",
      .sequence = 1,
      .kind = ava::session::AssistantOutputItemKind::Reasoning,
      .provider_item_id = std::nullopt,
      .provider_output_index = std::nullopt,
      .payload = ava::session::AssistantOutputReasoning{.text = "v4 reasoning",
                                                        .format = "openai_responses",
                                                        .redacted = false,
                                                        .signature = "private",
                                                        .redacted_data = std::nullopt,
                                                        .native_item_json = "{\"id\":\"rs_model_v4\",\"type\":\"reasoning\",\"summary\":[]}"}});
  auto commit_data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{.assistant_turn_id = "turn_model_v4",
                                                                                                               .item_count = 2,
                                                                                                               .provider = "openai",
                                                                                                               .model = "gpt-5.5",
                                                                                                               .finish_reason = "tool_calls",
                                                                                                               .usage_json = std::nullopt});
  auto append_target = ava::session::SessionAppendTarget::create_ephemeral(*store);
  auto appended_function = function_data && append_target
                               ? (*append_target)
                                     ->append(ava::session::SessionEntry{.id = "out_model_function",
                                                                         .parent_id = "",
                                                                         .type = ava::session::EntryType::AssistantOutputItem,
                                                                         .timestamp = "2026-07-18T00:00:00Z",
                                                                         .data_json = *function_data})
                               : ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "missing v4 append target")));
  auto appended_reasoning = reasoning_data && appended_function && append_target
                                ? (*append_target)
                                      ->append(ava::session::SessionEntry{.id = "out_model_reasoning",
                                                                          .parent_id = "",
                                                                          .type = ava::session::EntryType::AssistantOutputItem,
                                                                          .timestamp = "2026-07-18T00:00:01Z",
                                                                          .data_json = *reasoning_data})
                                : ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "v4 item append failed")));
  auto appended_commit = commit_data && appended_reasoning && append_target
                             ? (*append_target)
                                   ->append(ava::session::SessionEntry{.id = "commit_model_v4",
                                                                       .parent_id = "",
                                                                       .type = ava::session::EntryType::AssistantTurnCommit,
                                                                       .timestamp = "2026-07-18T00:00:02Z",
                                                                       .data_json = *commit_data})
                             : ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "v4 commit append failed")));
  auto entries = store->load();
  auto no_tools = entries ? ava::agent::build_provider_messages_from_entries(
                                *entries, ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = "openai",
                                                                                                                    .model_id = "no-tools",
                                                                                                                    .api_family = "openai_responses",
                                                                                                                    .reasoning_format = "openai_responses",
                                                                                                                    .supports_tools = false,
                                                                                                                    .supports_images = false}})
                          : ava::core::Result<std::vector<ava::provider::ChatMessage>>(std::unexpected(entries.error()));
  auto other_provider = entries
                            ? ava::agent::build_provider_messages_from_entries(
                                  *entries, ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = "anthropic",
                                                                                                                      .model_id = "other",
                                                                                                                      .api_family = "anthropic_messages",
                                                                                                                      .reasoning_format = "anthropic_thinking",
                                                                                                                      .supports_tools = true,
                                                                                                                      .supports_images = false}})
                            : ava::core::Result<std::vector<ava::provider::ChatMessage>>(std::unexpected(entries.error()));
  expect(appended_function && appended_reasoning && appended_commit && entries && !no_tools && !other_provider &&
             no_tools.error().format().find("no exact bound tool result") != std::string::npos &&
             other_provider.error().format().find("no exact bound tool result") != std::string::npos,
         "request projection validates unresolved committed v4 functions independently of model-switch acceptance");

  auto incomplete = ava::session::SessionStore::create_ephemeral(workspace / "incomplete");
  if (!incomplete || !function_data)
    return;
  auto incomplete_target = ava::session::SessionAppendTarget::create_ephemeral(*incomplete);
  auto appended_incomplete = incomplete_target ? (*incomplete_target)
                                                     ->append(ava::session::SessionEntry{.id = "out_model_incomplete",
                                                                                         .parent_id = "",
                                                                                         .type = ava::session::EntryType::AssistantOutputItem,
                                                                                         .timestamp = "2026-07-18T00:00:03Z",
                                                                                         .data_json = *function_data})
                                               : ava::core::VoidResult(std::unexpected(incomplete_target.error()));
  auto incomplete_entries = incomplete->load();
  auto projected_incomplete = incomplete_entries ? ava::agent::build_provider_messages_from_entries(*incomplete_entries)
                                                 : ava::core::Result<std::vector<ava::provider::ChatMessage>>(std::unexpected(incomplete_entries.error()));
  expect(appended_incomplete && incomplete_entries && projected_incomplete && projected_incomplete->empty(),
         "request projection strictly classifies but does not expose a structurally valid uncommitted v4 staging suffix");
}

void test_runtime_model_switch_accepts_committed_openai_responses_reasoning()
{
  auto const root = temp_root() / "runtime-model-openai-responses-replay";
  std::error_code remove_error;
  std::filesystem::remove_all(root, remove_error);
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(options);
  expect(unlocked_session_result.has_value(), "OpenAI Responses replay model-switch test opens a runtime session");
  if (!unlocked_session_result)
    return;

  auto reasoning_data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
      .assistant_turn_id = "turn_openai_replay",
      .sequence = 0,
      .kind = ava::session::AssistantOutputItemKind::Reasoning,
      .provider_item_id = "rs_openai_replay",
      .provider_output_index = 0,
      .payload = ava::session::AssistantOutputReasoning{
          .text = "inspect first",
          .format = "openai_responses",
          .redacted = false,
          .signature = "",
          .redacted_data = std::nullopt,
          .native_item_json =
              R"({"id":"rs_openai_replay","type":"reasoning","summary":[{"type":"summary_text","text":"inspect first"}],"status":"completed","encrypted_content":"cipher-replay"})"}});
  auto text_data = ava::session::serialize_assistant_output_item_data_json(ava::session::AssistantOutputItem{
      .assistant_turn_id = "turn_openai_replay",
      .sequence = 1,
      .kind = ava::session::AssistantOutputItemKind::Text,
      .provider_item_id = "msg_openai_replay",
      .provider_output_index = 1,
      .payload = ava::session::AssistantOutputText{.text = "done", .assistant_phase = ava::session::AssistantOutputTextPhase::FinalAnswer}});
  auto commit_data = ava::session::serialize_assistant_turn_commit_data_json(ava::session::AssistantTurnCommit{.assistant_turn_id = "turn_openai_replay",
                                                                                                               .item_count = 2,
                                                                                                               .provider = "openai",
                                                                                                               .model = "gpt-5.5",
                                                                                                               .finish_reason = "completed",
                                                                                                               .usage_json = std::nullopt});
  ava::core::VoidResult appended_user;
  ava::core::VoidResult appended_reasoning;
  ava::core::VoidResult appended_text;
  ava::core::VoidResult appended_commit;
  {
    ava::app::runtime::session_ts& unlocked_session = *unlocked_session_result;
    CRITICAL_AREA_BEGIN_W(session);
    appended_user = session_w->append_owned(ava::session::SessionEntry{.id = "user_openai_replay",
                                                                        .parent_id = "",
                                                                        .type = ava::session::EntryType::UserMessage,
                                                                        .timestamp = "2026-07-18T00:00:00Z",
                                                                        .data_json = R"({"text":"continue"})"});
    appended_reasoning = reasoning_data && appended_user
                             ? session_w->append_owned(ava::session::SessionEntry{.id = "reasoning_openai_replay",
                                                                                   .parent_id = "user_openai_replay",
                                                                                   .type = ava::session::EntryType::AssistantOutputItem,
                                                                                   .timestamp = "2026-07-18T00:00:01Z",
                                                                                   .data_json = *reasoning_data})
                             : ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "reasoning append failed")));
    appended_text = text_data && appended_reasoning
                        ? session_w->append_owned(ava::session::SessionEntry{.id = "text_openai_replay",
                                                                            .parent_id = "reasoning_openai_replay",
                                                                            .type = ava::session::EntryType::AssistantOutputItem,
                                                                            .timestamp = "2026-07-18T00:00:02Z",
                                                                            .data_json = *text_data})
                        : ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "text append failed")));
    appended_commit = commit_data && appended_text
                          ? session_w->append_owned(ava::session::SessionEntry{.id = "commit_openai_replay",
                                                                               .parent_id = "text_openai_replay",
                                                                               .type = ava::session::EntryType::AssistantTurnCommit,
                                                                               .timestamp = "2026-07-18T00:00:03Z",
                                                                               .data_json = *commit_data})
                          : ava::core::VoidResult(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "commit append failed")));
  }
  auto target = ava::app::resolve_runtime_model(paths, "openai", "gpt-5.6-sol");
  expect(reasoning_data.has_value(), "OpenAI Responses replay test serializes a valid native reasoning item");
  expect(text_data.has_value(), "OpenAI Responses replay test serializes the committed answer text");
  expect(commit_data.has_value(), "OpenAI Responses replay test serializes the production-shaped turn commit");
  expect(appended_user.has_value(), "OpenAI Responses replay test appends the user turn");
  expect(appended_reasoning.has_value(), "OpenAI Responses replay test appends native reasoning");
  expect(appended_text.has_value(), "OpenAI Responses replay test appends answer text");
  expect(appended_commit.has_value(), "OpenAI Responses replay test commits the assistant turn");
  expect(target.has_value(), "OpenAI Responses replay test resolves GPT-5.6 Sol");
  if (!appended_commit || !target)
    return;

  ava::core::Result<std::vector<ava::session::SessionEntry>> physical_entries;
  {
    SCOPED_CRITICAL_AREA_W(session_w, *unlocked_session_result);
    physical_entries = session_w->store.load();
  }
  auto project_for = [&](ava::config::ModelInfo const& model) {
    return physical_entries
               ? ava::agent::build_provider_messages_from_entries(
                     *physical_entries,
                     ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = model.provider_id,
                                                                                               .model_id = model.model_id,
                                                                                               .api_family = model.api_family,
                                                                                               .reasoning_format = model.reasoning_format,
                                                                                               .supports_tools = model.supports_tools.value_or(false),
                                                                                               .supports_images = false}})
               : ava::core::Result<std::vector<ava::provider::ChatMessage>>(std::unexpected(physical_entries.error()));
  };
  auto wrong_format = *target;
  wrong_format.reasoning_format = "anthropic_thinking";
  auto projected_wrong_format = project_for(wrong_format);
  auto other_provider = ava::app::resolve_runtime_model(paths, "anthropic", "claude-sonnet-4-5");
  auto projected_other_provider =
      other_provider ? project_for(*other_provider) : ava::core::Result<std::vector<ava::provider::ChatMessage>>(std::unexpected(other_provider.error()));
  auto portable_without_reasoning = [](auto const& projected) {
    return projected && std::ranges::any_of(*projected, [](auto const& message) { return message.content.find("done") != std::string::npos; }) &&
           std::ranges::none_of(*projected, [](auto const& message) {
             return std::ranges::any_of(message.content_parts, [](auto const& part) {
               return part.type == ava::provider::ContentPartType::Reasoning || !part.provider_item_id.empty();
             });
           });
  };
  expect(portable_without_reasoning(projected_wrong_format) && portable_without_reasoning(projected_other_provider),
         "foreign reasoning format and cross-provider targets receive visible answer text through portable request projection");

  {
    SCOPED_CRITICAL_AREA_W(session_w, *unlocked_session_result);
    auto controller = std::move(session_w->resources().run_controller);
    auto rejected_append_switch = session_w->switch_model(*target);
    session_w->resources().run_controller = std::move(controller);
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(!rejected_append_switch && model_selection_r->model.provider_id == "openai" && model_selection_r->model.model_id == "gpt-5.5",
             "a model_change append failure is reported truthfully and leaves active runtime model state unchanged");
    }

    auto switched = session_w->switch_model(*target);
    auto entries = session_w->store.load();
    auto const appended_model_change = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                                         return entry.type == ava::session::EntryType::ModelChange &&
                                                entry.data_json.find(R"("previous_model":"gpt-5.5")") != std::string::npos &&
                                                entry.data_json.find(R"("model":"gpt-5.6-sol")") != std::string::npos;
                                       });
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(switched && *switched && model_selection_r->model.provider_id == "openai" && model_selection_r->model.model_id == "gpt-5.6-sol" &&
                 appended_model_change,
             "committed GPT-5.5 OpenAI Responses native reasoning safely switches to GPT-5.6 Sol and appends model_change");
    }
  }
}

void test_app_runtime_model_switch_persists_and_reopens()
{
  auto const root = create_empty_root("app-runtime-model-switch");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(paths.ava_config_dir);
  {
    std::ofstream file(paths.models_file, std::ios::binary | std::ios::trunc);
    file << R"JSON({
      "default_provider":"openai",
      "default_model":"gpt-5.5",
      "scoped_model_cycle":["anthropic/claude-test","openai/gpt-5.5"],
      "models":[{
        "provider":"anthropic",
        "id":"claude-test",
        "name":"Claude Test",
        "family":"claude-test",
        "api_family":"anthropic_messages",
        "context_window_tokens":999,
        "max_output_tokens":123,
        "supports_tools":false,
        "supports_streaming":true,
        "supports_reasoning":false,
        "reports_usage":true,
        "input_modalities":["text"],
        "output_modalities":["text"],
        "compatibility_quirks":["test_quirk"]
      }]
    })JSON";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime model switch test opens runtime session");
  if (!unlocked_session_result)
    return;
  std::string session_id;
  {
    SCOPED_CRITICAL_AREA_W(session_w, *unlocked_session_result);
    ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
    expect(model_selection_r->scoped_model_cycle && model_selection_r->scoped_model_cycle->size() == 2 &&
               (*model_selection_r->scoped_model_cycle)[0] == "anthropic/claude-test" && (*model_selection_r->scoped_model_cycle)[1] == "openai/gpt-5.5",
           "runtime session restores persisted scoped model cycle");
    session_id = session_w->store.session_id();
  }

  auto model = ava::app::resolve_runtime_model(paths, "anthropic", "claude-test");
  expect(model.has_value(), "runtime resolves configured Anthropic model");
  if (!model)
    return;
  {
    SCOPED_CRITICAL_AREA_W(session_w, *unlocked_session_result);
    auto switched = session_w->switch_model(*model);
    expect(switched.has_value() && *switched, "runtime model switch reports a change");
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(model_selection_r->model.provider_id == "anthropic" && model_selection_r->model.model_id == "claude-test",
             "runtime model switch updates active session model");
    }

    auto entries = session_w->store.load();
    expect(entries.has_value(), "runtime model switch loads session entries");
    bool saw_model_change = false;
    if (entries)
    {
      for (auto const& entry : *entries)
      {
        saw_model_change = saw_model_change ||
                           (entry.type == ava::session::EntryType::ModelChange && entry.data_json.find("\"previous_provider\":\"openai\"") != std::string::npos &&
                            entry.data_json.find("\"provider\":\"anthropic\"") != std::string::npos);
      }
    }
    expect(saw_model_change, "runtime model switch appends model_change entry");

    auto appended_escaped_model_change = session_w->append_owned(ava::session::SessionEntry{
        .id = ava::core::make_id("entry"),
        .parent_id = "",
        .type = ava::session::EntryType::ModelChange,
        .timestamp = ava::session::now_timestamp(),
        .data_json =
            R"JSON({"previous_provider":"anthropic","previous_model":"claude-test","provider":"anthropic","model":"claude-test","display_name":"Claude Test","family":"claude-test","api_family":"anthropic_messages","input_modalities":["text"],"output_modalities":["text"],"reasoning_levels":[],"compatibility_quirks":["test_quirk","\uD83D\uDE00"],"context_window_tokens":999,"max_output_tokens":123,"supports_tools":false,"supports_streaming":true,"supports_reasoning":false,"reports_usage":true})JSON"});
    expect(appended_escaped_model_change.has_value(), "runtime model switch test seeds escaped unicode metadata");
  }

  ava::app::runtime::OpenContext reopen_context = open_context;
  std::error_code remove_error;
  std::filesystem::remove(paths.models_file, remove_error);
  auto same_process_contested = ava::app::runtime::Session::open(reopen_context, {.sessionless = false,
                                                                                .requested_session_id = session_id,
                                                                                .fork_session_id = std::nullopt,
                                                                                .initial_session_name = std::nullopt,
                                                                                .continue_last_session = false,
                                                                                .initial_reasoning_level = std::nullopt,
                                                                                .expected_original_cwd = std::nullopt});
  expect(!same_process_contested && same_process_contested.error().message().find("already owned") != std::string::npos,
         "protocol-neutral runtime ownership excludes a second same-process mode");
  pid_t const contender = fork();
  if (contender == 0)
  {
    auto contested = ava::app::runtime::Session::open(reopen_context, {.sessionless = false,
                                                                     .requested_session_id = session_id,
                                                                     .fork_session_id = std::nullopt,
                                                                     .initial_session_name = std::nullopt,
                                                                     .continue_last_session = false,
                                                                     .initial_reasoning_level = std::nullopt,
                                                                     .expected_original_cwd = std::nullopt});
    _exit(!contested && contested.error().message().find("already owned") != std::string::npos ? 0 : 1);
  }
  int contender_status = 0;
  if (contender > 0)
    static_cast<void>(waitpid(contender, &contender_status, 0));
  expect(contender > 0 && WIFEXITED(contender_status) && WEXITSTATUS(contender_status) == 0,
         "TUI/print/RPC-style runtime owners contend on the same cross-process session lease");
  unlocked_session_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "runtime owner released for reopen test"));
  auto unlocked_reopened_result = ava::app::runtime::Session::open(reopen_context, {.sessionless = false,
                                                                   .requested_session_id = session_id,
                                                                   .fork_session_id = std::nullopt,
                                                                   .initial_session_name = std::nullopt,
                                                                   .continue_last_session = false,
                                                                   .initial_reasoning_level = std::nullopt,
                                                                   .expected_original_cwd = std::nullopt});
  expect(unlocked_reopened_result.has_value(), "runtime releases its lease on normal lifetime end and reopens persisted session");
  if (unlocked_reopened_result)
  {
    SCOPED_CRITICAL_AREA_R(reopened_r, *unlocked_reopened_result);
    ava::app::runtime::ModelSelection_ts::crat model_selection_r = reopened_r->model_selection_r();
    expect(model_selection_r->model.provider_id == "anthropic" && model_selection_r->model.model_id == "claude-test",
           "runtime reopen restores latest persisted model_change");
    auto const resumed_unknown_launch_display =
        ava::agent::SubagentLaunchDisplay::normalized(ava::config::proven_configured_model_display_name(model_selection_r->model));
    expect(model_selection_r->model.display_name == "Claude Test" &&
               ava::config::proven_configured_model_display_name(model_selection_r->model).empty() &&
               resumed_unknown_launch_display.model_display_name().empty(),
           "resumed unknown model retains its public fallback label but is omitted from private launch display without process-local provenance");
    auto const emoji_quirk = std::string("\xF0\x9F\x98\x80");
    bool const restored_emoji_quirk = std::ranges::find(model_selection_r->model.compatibility_quirks, emoji_quirk) !=
                                      model_selection_r->model.compatibility_quirks.end();
    expect(restored_emoji_quirk, "runtime reopen decodes escaped supplementary-plane metadata");
  }
  if (unlocked_reopened_result)
  {
    ava::app::runtime::session_ts& unlocked_reopened(*unlocked_reopened_result);
    ava::provider::OpenAIProvider const provider("https://api.example.test");
    ava::tests::FakeTransport transport({});
    std::istringstream in("{\"id\":\"list\",\"type\":\"list_models\"}\n");
    std::ostringstream out;
    auto result =
        ava::app::run_rpc_loop(unlocked_reopened, reopen_context, provider, transport, ava::app::runtime::RunOptions{}, in, out, ava::app::rpc::RpcInputWake{});
    auto const jsonl = out.str();
    auto const restored_position = jsonl.find("\"model\":\"claude-test\"");
    expect(result.has_value() && restored_position != std::string::npos, "RPC list_models includes restored removed current model");
    expect(restored_position != std::string::npos && jsonl.find("\"selectable\":false", restored_position) != std::string::npos,
           "RPC list_models marks restored removed current model as not selectable");
    expect(restored_position != std::string::npos && jsonl.find("\"context_window_tokens\":999", restored_position) != std::string::npos &&
               jsonl.find("\"max_output_tokens\":123", restored_position) != std::string::npos &&
               jsonl.find("\"supports_streaming\":true", restored_position) != std::string::npos &&
               jsonl.find("\"supports_tools\":false", restored_position) != std::string::npos &&
               jsonl.find("\"reports_usage\":true", restored_position) != std::string::npos && jsonl.find("test_quirk", restored_position) != std::string::npos,
           "RPC list_models preserves capability metadata for restored removed models");
  }
}

void test_app_runtime_model_switch_projects_incompatible_history_at_request_time()
{
  auto const root = create_empty_root("app-runtime-model-switch-compatibility");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(paths.ava_config_dir);
  {
    std::ofstream file(paths.models_file, std::ios::binary | std::ios::trunc);
    file << R"JSON({
      "default_provider":"openai",
      "default_model":"gpt-5.5",
      "models":[{
        "provider":"openai",
        "id":"no-tools",
        "name":"No Tools",
        "family":"test",
        "api_family":"openai_responses",
        "supports_streaming":true,
        "input_modalities":["text"],
        "output_modalities":["text"]
      },{
        "provider":"anthropic",
        "id":"claude-replay",
        "name":"Claude Replay",
        "family":"claude-test",
        "api_family":"anthropic_messages",
        "supports_tools":true,
        "supports_streaming":true,
        "input_modalities":["text"],
        "output_modalities":["text"]
      },{
        "provider":"anthropic",
        "id":"claude-image",
        "name":"Claude Image",
        "family":"claude-test",
        "api_family":"anthropic_messages",
        "supports_tools":true,
        "supports_streaming":true,
        "input_modalities":["text","image"],
        "output_modalities":["text"]
      }]
    })JSON";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime model switch compatibility test opens runtime session");
  if (!unlocked_session_result)
    return;

  {
    SCOPED_CRITICAL_AREA_W(session_w, *unlocked_session_result);

    auto project_current_request = [&]() {
      auto physical = session_w->store.load();
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      bool const supports_images = std::ranges::find(model_selection_r->model.input_modalities, "image") != model_selection_r->model.input_modalities.end();
      return physical
                 ? ava::agent::build_provider_messages_from_entries(
                       *physical, ava::agent::MessageBuildOptions{.target = ava::agent::HistoryReplayTarget{.provider_id = model_selection_r->model.provider_id,
                                                                                                            .model_id = model_selection_r->model.model_id,
                                                                                                            .api_family = model_selection_r->model.api_family,
                                                                                                            .reasoning_format = model_selection_r->model.reasoning_format,
                                                                                                            .supports_tools =
                                                                                                                model_selection_r->model.supports_tools.value_or(false),
                                                                                                            .supports_images = supports_images}})
                 : ava::core::Result<std::vector<ava::provider::ChatMessage>>(std::unexpected(std::move(physical.error())));
    };

    auto appended_tool_call = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                 .parent_id = "",
                                                                                 .type = ava::session::EntryType::ToolCall,
                                                                                 .timestamp = ava::session::now_timestamp(),
                                                                                 .data_json = "{\"call_id\":\"call_1\","
                                                                                              "\"name\":\"read_file\","
                                                                                              "\"arguments\":{}}"});
    auto appended_tool_result = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                   .parent_id = "",
                                                                                   .type = ava::session::EntryType::ToolResult,
                                                                                   .timestamp = ava::session::now_timestamp(),
                                                                                   .data_json = "{\"call_id\":\"call_1\",\"content\":\"ok\"}"});
    expect(appended_tool_call.has_value() && appended_tool_result.has_value(), "model switch compatibility test seeds tool history");

    auto no_tools_model = ava::app::resolve_runtime_model(paths, "openai", "no-tools");
    expect(no_tools_model.has_value(), "runtime resolves no-tools model");
    if (!no_tools_model)
      return;
    auto switched_no_tools = session_w->switch_model(*no_tools_model);
    expect(switched_no_tools.has_value() && *switched_no_tools, "runtime switches immediately to a model without tool support after tool history");
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(model_selection_r->model.provider_id == "openai" && model_selection_r->model.model_id == "no-tools",
             "tool-history model switch updates active state without scanning history");
    }
    auto no_tools_request = project_current_request();
    expect(no_tools_request &&
               std::ranges::any_of(*no_tools_request, [](auto const& message) { return message.content.find("Tool call") != std::string::npos; }) &&
               std::ranges::all_of(*no_tools_request, [](auto const& message) { return message.content_parts.empty(); }),
           "the request immediately after a no-tools switch textualizes complete historical tool semantics");

    auto appended_reasoning = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                 .parent_id = "",
                                                                                 .type = ava::session::EntryType::ReasoningBlock,
                                                                                 .timestamp = ava::session::now_timestamp(),
                                                                                 .data_json = "{\"provider\":\"anthropic\","
                                                                                              "\"model\":\"claude-sonnet-4-5\","
                                                                                              "\"format\":\"anthropic_thinking\","
                                                                                              "\"text\":\"visible reasoning\","
                                                                                              "\"signature\":\"sig-1\"}"});
    expect(appended_reasoning.has_value(), "model switch compatibility test seeds reasoning history");

    auto anthropic_replay = ava::app::resolve_runtime_model(paths, "anthropic", "claude-replay");
    expect(anthropic_replay.has_value(), "runtime resolves Anthropic replay model");
    if (!anthropic_replay)
      return;
    auto switched_anthropic = session_w->switch_model(*anthropic_replay);
    expect(switched_anthropic.has_value() && *switched_anthropic, "runtime allows switch to Anthropic model that can replay Anthropic reasoning");
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(model_selection_r->model.provider_id == "anthropic" && model_selection_r->model.model_id == "claude-replay",
             "compatible reasoning switch updates active model");
    }

    auto kimi_model = ava::app::resolve_runtime_model(paths, "kimi", "kimi-k2-thinking");
    expect(kimi_model.has_value(), "runtime resolves Kimi model");
    if (!kimi_model)
      return;
    auto switched_reasoning = session_w->switch_model(*kimi_model);
    expect(switched_reasoning.has_value() && *switched_reasoning, "runtime switches immediately across incompatible reasoning providers");
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(model_selection_r->model.provider_id == "kimi" && model_selection_r->model.model_id == "kimi-k2-thinking",
             "cross-provider reasoning switch updates active state without scanning history");
    }
    auto cross_reasoning_request = project_current_request();
    expect(cross_reasoning_request && std::ranges::none_of(*cross_reasoning_request,
                                                            [](auto const& message) {
                                                              return std::ranges::any_of(message.content_parts, [](auto const& part) {
                                                                return part.type == ava::provider::ContentPartType::Reasoning;
                                                              });
                                                            }),
           "the request immediately after a cross-provider switch drops historical reasoning instead of blocking selection");

    auto appended_compaction = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                  .parent_id = "",
                                                                                  .type = ava::session::EntryType::Compaction,
                                                                                  .timestamp = ava::session::now_timestamp(),
                                                                                  .data_json = "{\"summary\":\"old history\"}"});
    expect(appended_compaction.has_value(), "model switch compatibility test seeds compaction boundary");
    auto switched_no_tools_after_compaction = session_w->switch_model(*no_tools_model);
    expect(switched_no_tools_after_compaction.has_value() && *switched_no_tools_after_compaction,
           "runtime ignores pre-compaction native history for switch compatibility");
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(model_selection_r->model.provider_id == "openai" && model_selection_r->model.model_id == "no-tools",
             "post-compaction switch updates active model");
    }

    auto appended_large_image = session_w->append_owned(ava::session::SessionEntry{
        .id = ava::core::make_id("entry"),
        .parent_id = "",
        .type = ava::session::EntryType::UserMessage,
        .timestamp = ava::session::now_timestamp(),
        .data_json =
            R"({"text":"large image","attachments":[{"id":"img_big","type":"image","mime_type":"image/png","byte_size":6291456,"sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","storage_path":"attachments/img_big.png"}]})"});
    expect(appended_large_image.has_value(), "model switch compatibility test seeds large image history");
    auto anthropic_image = ava::app::resolve_runtime_model(paths, "anthropic", "claude-image");
    expect(anthropic_image.has_value(), "runtime resolves Anthropic image model");
    if (!anthropic_image)
      return;
    auto switched_large_image = session_w->switch_model(*anthropic_image);
    expect(switched_large_image.has_value() && *switched_large_image,
           "runtime switches immediately despite historical images exceeding the target's provider-specific limit");
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(model_selection_r->model.provider_id == "anthropic" && model_selection_r->model.model_id == "claude-image",
             "image-history model switch updates active state without scanning history");
    }
    auto anthropic_image_request = project_current_request();
    expect(anthropic_image_request &&
               std::ranges::any_of(*anthropic_image_request,
                                   [](auto const& message) {
                                     return message.content.find("[historical image omitted: mime=image/png bytes=6291456]") != std::string::npos &&
                                            std::ranges::none_of(message.content_parts,
                                                                 [](auto const& part) { return part.type == ava::provider::ContentPartType::Image; });
                                   }),
           "the request immediately after an Anthropic switch replaces an oversized historical image with a safe placeholder");
    auto appended_post_image_compaction = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                             .parent_id = "",
                                                                                             .type = ava::session::EntryType::Compaction,
                                                                                             .timestamp = ava::session::now_timestamp(),
                                                                                             .data_json = "{\"summary\":\"image history compacted\"}"});
    expect(appended_post_image_compaction.has_value(), "model switch compatibility test clears image history with compaction");

    auto appended_deepseek_reasoning = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                         .parent_id = "",
                                                                                         .type = ava::session::EntryType::ReasoningBlock,
                                                                                         .timestamp = ava::session::now_timestamp(),
                                                                                         .data_json = "{\"provider\":\"deepseek\","
                                                                                                      "\"model\":\"deepseek-v4-flash\","
                                                                                                      "\"format\":\"reasoning_content\","
                                                                                                      "\"text\":\"display-only deepseek reasoning\"}"});
    expect(appended_deepseek_reasoning.has_value(), "model switch compatibility test seeds DeepSeek reasoning history");
    auto switched_deepseek_to_kimi = session_w->switch_model(*kimi_model);
    expect(switched_deepseek_to_kimi.has_value() && *switched_deepseek_to_kimi,
           "runtime switches immediately across providers that use the same reasoning format");
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(model_selection_r->model.provider_id == "kimi" && model_selection_r->model.model_id == "kimi-k2-thinking",
             "same-format cross-provider switch updates active state without replaying native reasoning");
    }

    auto appended_deepseek_compaction = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                           .parent_id = "",
                                                                                           .type = ava::session::EntryType::Compaction,
                                                                                           .timestamp = ava::session::now_timestamp(),
                                                                                           .data_json = "{\"summary\":\"deepseek reasoning compacted\"}"});
    expect(appended_deepseek_compaction.has_value(), "model switch compatibility test clears DeepSeek reasoning with compaction");

    auto appended_kimi_reasoning = session_w->append_owned(ava::session::SessionEntry{.id = ava::core::make_id("entry"),
                                                                                      .parent_id = "",
                                                                                      .type = ava::session::EntryType::ReasoningBlock,
                                                                                      .timestamp = ava::session::now_timestamp(),
                                                                                      .data_json = "{\"provider\":\"kimi\","
                                                                                                   "\"model\":\"kimi-k2-thinking\","
                                                                                                   "\"format\":\"reasoning_content\","
                                                                                                   "\"text\":\"compatible kimi reasoning\"}"});
    expect(appended_kimi_reasoning.has_value(), "model switch compatibility test seeds Kimi reasoning history");

    auto switched_kimi = session_w->switch_model(*kimi_model);
    expect(switched_kimi.has_value() && !*switched_kimi, "switching to the already-active Kimi model remains an accepted no-op");
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(model_selection_r->model.provider_id == "kimi" && model_selection_r->model.model_id == "kimi-k2-thinking",
             "accepted Kimi no-op preserves the active model");
    }

    auto moonshot_model = ava::app::resolve_runtime_model(paths, "moonshot", "kimi-k2.6");
    expect(moonshot_model.has_value(), "runtime resolves Moonshot model");
    if (!moonshot_model)
      return;
    auto switched_moonshot = session_w->switch_model(*moonshot_model);
    expect(switched_moonshot.has_value() && *switched_moonshot, "runtime switches immediately without a native-reasoning preservation quirk");
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(model_selection_r->model.provider_id == "moonshot" && model_selection_r->model.model_id == "kimi-k2.6",
             "Moonshot switch updates active state while request projection owns replay safety");
    }

    auto entries = session_w->store.load();
    expect(entries.has_value(), "model switch compatibility test reloads entries");
    if (entries)
    {
      auto const model_changes = std::ranges::count_if(*entries, [](auto const& entry) { return entry.type == ava::session::EntryType::ModelChange; });
      expect(model_changes == 7, "every effective immediate model switch appends one truthful model_change entry");
    }
  }
}

void test_app_runtime_reasoning_selection_persists_and_requests()
{
  ava::config::ModelInfo mapped_model;
  mapped_model.provider_id = "synthetic";
  mapped_model.model_id = "mapped-reasoning";
  mapped_model.supports_reasoning = true;
  mapped_model.reasoning_level_mappings = {{.level = "high", .provider_level = "provider-high"}};
  auto mapped = ava::app::reasoning_selection_for_level(mapped_model, "high");
  expect(mapped && mapped->level == "high" && mapped->provider_level == std::optional<std::string>("provider-high"),
         "runtime reasoning accepts an explicit mapping when support is true and the supported set includes off plus the mapped level");

  auto unsupported_model = mapped_model;
  unsupported_model.supports_reasoning = false;
  auto unsupported_mapping = ava::app::reasoning_selection_for_level(unsupported_model, "high");
  auto unsupported_resolved =
      ava::app::runtime::resolve_runtime_reasoning_selection(unsupported_model, ava::app::runtime::ReasoningSelection{.level = "high", .display = {}});
  expect(unsupported_mapping && unsupported_resolved && unsupported_resolved->provider_level == std::optional<std::string>("provider-high"),
         "explicit provider mappings remain valid lower-level metadata when the model does not expose a reasoning choice");

  auto single_level_model = mapped_model;
  single_level_model.model_id = "off-only-reasoning";
  single_level_model.reasoning_level_mappings.clear();
  auto single_level =
      ava::app::runtime::resolve_runtime_reasoning_selection(single_level_model, ava::app::runtime::ReasoningSelection{.level = "off", .display = {}});
  auto empty_level =
      ava::app::runtime::resolve_runtime_reasoning_selection(mapped_model, ava::app::runtime::ReasoningSelection{.level = " \t ", .display = {}});
  expect(single_level && single_level->level == "off" && !empty_level && empty_level.error().format().find("reasoning level is required") != std::string::npos,
         "the lower-level resolver accepts singleton provider metadata while still rejecting trim-empty selections");

  std::vector<ava::session::SessionEntry> persisted_reasoning_entries = {
      {.id = "reasoning_change_invalid_capability",
       .parent_id = "",
       .type = ava::session::EntryType::ReasoningChange,
       .timestamp = "2026-10-13T00:00:00Z",
       .data_json = R"JSON({"provider":"synthetic","model":"mapped-reasoning","enabled":true,"level":"high"})JSON"}};
  expect(!ava::app::runtime::latest_persisted_reasoning(persisted_reasoning_entries, unsupported_model),
         "persisted reasoning normalizes to absent when the current model does not expose a choice");
  auto singleton_persisted_entries = persisted_reasoning_entries;
  singleton_persisted_entries.front().data_json = R"JSON({"provider":"synthetic","model":"off-only-reasoning","enabled":true,"level":"off"})JSON";
  expect(!ava::app::runtime::latest_persisted_reasoning(singleton_persisted_entries, single_level_model),
         "persisted reasoning normalizes to absent for singleton level metadata without rejecting that metadata");

  auto const root = create_empty_root("app-runtime-reasoning-selection");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  std::filesystem::create_directories(paths.ava_config_dir);
  {
    std::ofstream file(paths.models_file, std::ios::binary | std::ios::trunc);
    file << R"JSON({
      "default_provider":"openai",
      "default_model":"gpt-5.5",
      "models":[{
        "provider":"openai",
        "id":"no-reasoning-levels",
        "name":"No Reasoning Levels",
        "family":"test",
        "api_family":"openai_responses",
        "supports_tools":true,
        "supports_streaming":true,
        "supports_reasoning":true,
        "input_modalities":["text"],
        "output_modalities":["text"]
      },{
        "provider":"anthropic",
        "id":"claude-default-max",
        "name":"Claude Default Max",
        "family":"claude",
        "api_family":"anthropic_messages",
        "supports_tools":true,
        "supports_streaming":true,
        "supports_reasoning":true,
        "reasoning_levels":["enabled"],
        "input_modalities":["text"],
        "output_modalities":["text"]
      },{
        "provider":"anthropic-proxy",
        "id":"claude-proxy",
        "name":"Claude Proxy",
        "family":"claude",
        "api_family":"anthropic_messages",
        "max_output_tokens":8192,
        "supports_tools":true,
        "supports_streaming":true,
        "supports_reasoning":true,
        "reasoning_levels":["enabled"],
        "input_modalities":["text"],
        "output_modalities":["text"]
      }]
    })JSON";
  }

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "runtime reasoning test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  std::string session_id;
  {
    CRITICAL_AREA_BEGIN_W(session);
    session_id = session_w->store.session_id();

    auto selected = session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = " low ", .budget_tokens = std::nullopt, .display = ""});
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(selected.has_value() && *selected && model_selection_r->reasoning && model_selection_r->reasoning->level == "low",
             "runtime reasoning selection validates, normalizes, and updates state");
    }

    auto duplicate = session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "low", .budget_tokens = std::nullopt, .display = ""});
    expect(duplicate.has_value() && !*duplicate, "runtime reasoning selection is idempotent when unchanged");

    auto invalid = session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "ultra", .budget_tokens = std::nullopt, .display = ""});
    expect(!invalid.has_value(), "runtime reasoning selection rejects unsupported model levels");
    CRITICAL_AREA_END_W(session);

    ava::provider::OpenAIProvider const provider("https://api.example.test");
    ava::tests::FakeTransport transport({ava::http::HttpResponse{
        .status_code = 200,
        .headers = {},
        .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"reasoned answer\"}\n\n"
                "data: [DONE]\n\n",
    }});
    ava::app::runtime::RunOptions run_options;
    run_options.access_token = "token";
    auto result = ava::app::run_prompt(unlocked_session, "use reasoning", provider, transport, run_options);
    expect(result && result->final_text == "reasoned answer", "runtime reasoning prompt completes");
    expect(transport.requests().size() == 1, "runtime reasoning test sends one provider request");
    if (!transport.requests().empty())
    {
      expect(transport.requests()[0].body.find("\"reasoning\"") != std::string::npos &&
                 transport.requests()[0].body.find("\"effort\":\"low\"") != std::string::npos &&
                 transport.requests()[0].body.find("\"summary\":\"auto\"") != std::string::npos,
             "runtime reasoning selection is sent to the provider request with visible summary request");
    }

    CRITICAL_AREA_CONTINUE_W(session);
    auto entries = session_w->store.load();
    expect(entries.has_value(), "runtime reasoning test reloads session entries");
    if (entries)
    {
      auto const reasoning_changes = std::ranges::count_if(*entries, [](auto const& entry) { return entry.type == ava::session::EntryType::ReasoningChange; });
      expect(reasoning_changes == 1, "runtime reasoning selection appends one durable reasoning_change entry");
    }

    ava::app::runtime::OpenContext reopen_context = open_context;
    auto reopened = ava::app::runtime::Session::open(reopen_context, {.sessionless = false,
                                                                     .requested_session_id = session_id,
                                                                     .fork_session_id = std::nullopt,
                                                                     .initial_session_name = std::nullopt,
                                                                     .continue_last_session = false,
                                                                     .initial_reasoning_level = std::nullopt,
                                                                     .expected_original_cwd = std::nullopt});
    expect(!reopened && reopened.error().message().find("already owned") != std::string::npos,
           "a second runtime cannot inspect reasoning by bypassing the active owner's lease");

    auto cleared = session_w->set_reasoning(std::nullopt);
    expect(cleared.has_value() && *cleared && !session_w->model_selection_r()->reasoning, "runtime reasoning selection can be cleared");

    auto reselected = session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "low", .budget_tokens = std::nullopt, .display = ""});
    expect(reselected.has_value() && *reselected, "runtime reasoning test re-enables reasoning before switch boundary");
    auto kimi_model = ava::app::resolve_runtime_model(paths, "kimi", "kimi-k2-thinking");
    auto openai_model = ava::app::resolve_runtime_model(paths, "openai", "gpt-5.5");
    expect(kimi_model.has_value() && openai_model.has_value(), "runtime reasoning test resolves switch boundary models");
    if (kimi_model && openai_model)
    {
      auto switched_away = session_w->switch_model(*kimi_model);
      expect(switched_away.has_value() && *switched_away, "runtime reasoning test switches to Kimi model");
      auto kimi_budget =
          session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "enabled", .budget_tokens = 1024, .display = "summarized"});
      expect(!kimi_budget.has_value() && kimi_budget.error().format().find("Kimi reasoning supports level only") != std::string::npos,
             "runtime reasoning selection rejects unsupported OpenAI-compatible budget/display controls");
      auto switched_back = session_w->switch_model(*openai_model);
      expect(switched_back.has_value() && *switched_back && !session_w->model_selection_r()->reasoning, "runtime model switches clear active reasoning selection");
      auto reopened_after_switch = ava::app::runtime::Session::open(reopen_context, {.sessionless = false,
                                                                                   .requested_session_id = session_id,
                                                                                   .fork_session_id = std::nullopt,
                                                                                   .initial_session_name = std::nullopt,
                                                                                   .continue_last_session = false,
                                                                                   .initial_reasoning_level = std::nullopt,
                                                                                   .expected_original_cwd = std::nullopt});
      expect(!reopened_after_switch && reopened_after_switch.error().message().find("already owned") != std::string::npos,
             "runtime model-change persistence remains exclusively owned until the active runtime ends");
    }

    auto no_levels_model = ava::app::resolve_runtime_model(paths, "openai", "no-reasoning-levels");
    expect(no_levels_model.has_value(), "runtime reasoning test resolves no-level custom model");
    if (no_levels_model)
    {
      auto switched = session_w->switch_model(*no_levels_model);
      expect(switched.has_value() && *switched, "runtime reasoning test switches to no-level custom model");
      auto no_level_selection =
          session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "low", .budget_tokens = std::nullopt, .display = ""});
      expect(no_level_selection && !*no_level_selection && !session_w->model_selection_r()->reasoning,
             "runtime reasoning selection normalizes to absent for models without a user-selectable reasoning level");
    }

    auto anthropic_default_max = ava::app::resolve_runtime_model(paths, "anthropic", "claude-default-max");
    expect(anthropic_default_max.has_value(), "runtime reasoning test resolves Anthropic default max model");
    if (anthropic_default_max)
    {
      auto switched = session_w->switch_model(*anthropic_default_max);
      expect(switched.has_value() && *switched, "runtime reasoning test switches to Anthropic default max model");
      auto over_budget =
          session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "enabled", .budget_tokens = 4096, .display = "summarized"});
      expect(!over_budget.has_value() && over_budget.error().format().find("reasoning budget must be below max output tokens") != std::string::npos,
             "runtime reasoning selection validates Anthropic budget against provider default max tokens");
    }

    auto proxy_registry = ava::config::load_model_registry(paths);
    expect(proxy_registry.has_value(), "runtime reasoning test loads registry for custom Anthropic-compatible model");
    auto anthropic_proxy = proxy_registry ? ava::config::find_model(*proxy_registry, "anthropic-proxy", "claude-proxy") : std::optional<ava::config::ModelInfo>{};
    expect(anthropic_proxy.has_value(), "runtime reasoning test finds custom Anthropic-compatible model");
    if (anthropic_proxy)
    {
      session_w->model_selection().change([&anthropic_proxy](auto& model_selection){
        model_selection.model = *anthropic_proxy;
        model_selection.reasoning.reset();
      });
      CRITICAL_AREA_END_W(session);
      auto cycled = ava::app::cycle_runtime_reasoning(unlocked_session);
      CRITICAL_AREA_CONTINUE_W(session);
      {
        ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
        expect(cycled.has_value() && model_selection_r->reasoning && model_selection_r->reasoning->level == "enabled" && model_selection_r->reasoning->budget_tokens &&
                   *model_selection_r->reasoning->budget_tokens == 4096,
               "runtime reasoning cycling uses API-family fallback profile for custom Anthropic-compatible models");
      }
      auto missing_budget =
          session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "enabled", .budget_tokens = std::nullopt, .display = ""});
      expect(!missing_budget.has_value() && missing_budget.error().format().find("Anthropic-proxy enabled reasoning requires budget_tokens") != std::string::npos,
             "runtime reasoning validation labels missing-budget errors with the custom provider id");
      auto too_large_budget = session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "enabled", .budget_tokens = 8192, .display = ""});
      expect(!too_large_budget.has_value() && too_large_budget.error().format().find("reasoning budget must be below max output tokens") != std::string::npos,
             "runtime reasoning validation applies fallback budget limits to custom providers");
    }

    session_w->model_selection().change([&mapped_model](auto& model_selection) {
      model_selection.model = mapped_model;
      model_selection.reasoning = ava::app::runtime::ReasoningSelection{.level = "high", .provider_level = "provider-high", .display = {}};
    });
    session_w->model_selection().change([&unsupported_model](auto& model_selection) { model_selection.model = unsupported_model; });
    auto normalized_false_mapping =
        session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "high", .budget_tokens = std::nullopt, .display = ""});
    expect(normalized_false_mapping && *normalized_false_mapping && !session_w->model_selection_r()->reasoning,
           "setting valid mapped metadata on a model without a choice succeeds by clearing non-canonical active reasoning");
    auto normalized_false_mapping_again =
        session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "high", .budget_tokens = std::nullopt, .display = ""});
    expect(normalized_false_mapping_again && !*normalized_false_mapping_again && !session_w->model_selection_r()->reasoning,
           "setting valid mapped metadata on an already canonical model remains absent without another write");

    session_w->model_selection().change([&single_level_model](auto& model_selection) { model_selection.model = single_level_model; });
    auto initial_off = session_w->apply_initial_reasoning_level("off");
    auto normalized_singleton = session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = "off", .budget_tokens = std::nullopt, .display = ""});
    auto empty_selection = session_w->set_reasoning(ava::app::runtime::ReasoningSelection{.level = " \t ", .budget_tokens = std::nullopt, .display = ""});
    expect(initial_off && normalized_singleton && !*normalized_singleton && !session_w->model_selection_r()->reasoning,
           "initial off and singleton selections succeed as canonical absent reasoning");
    expect(!empty_selection && empty_selection.error().format().find("reasoning level is required") != std::string::npos,
           "the session setter rejects trim-empty selections before canonical no-choice normalization");
  }
}

void test_app_runtime_branch_construction_failure_rolls_back_created_file()
{
  auto seed_source_attachment = [](ava::app::runtime::session_ts& unlocked_session) {
    SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);
    auto entries = session_w->store.load();
    if (!entries || entries->empty())
      return false;
    auto appended =
        session_w->append_owned(ava::session::SessionEntry{.id = "entry_rollback_attachment",
                                                        .parent_id = entries->back().id,
                                                        .type = ava::session::EntryType::UserMessage,
                                                        .timestamp = "2026-07-16T00:00:00Z",
                                                        .data_json = "{\"text\":\"attachment\",\"attachments\":[{\"id\":\"rollback_img\","
                                                                     "\"type\":\"image\",\"mime_type\":\"image/png\",\"byte_size\":5,"
                                                                     "\"sha256\":\"2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\","
                                                                     "\"storage_path\":\"attachments/rollback.txt\"}]}",
                                                        .version = 2});
    if (!appended)
      return false;
    auto const attachment = ava::session::attachment_storage_root(session_w->store) / "attachments" / "rollback.txt";
    write_app_test_file(attachment, "hello");
    return true;
  };

  {
    auto const root = create_empty_root("app-runtime-tui-branch-rollback");

    auto const workspace = root / "workspace";
    auto const paths = app_test_paths(root);
    std::filesystem::create_directories(workspace);

    ava::app::runtime::OpenContext options;
    options.workspace_dir = workspace;
    options.current_dir = workspace;
    options.paths = paths;
    auto unlocked_source_result = ava::app::runtime::Session::open(options);
    expect(unlocked_source_result.has_value(), "TUI rollback test opens an active source with a copyable attachment");
    if (!unlocked_source_result)
      return;
    std::string source_id;
    std::filesystem::path source_path;
    ava::core::Result<ava::app::CommandResult> forked;
    bool source_id_unchanged = false;
    ava::app::runtime::session_ts& unlocked_source = *unlocked_source_result;
    expect(seed_source_attachment(unlocked_source), "TUI rollback test seeds an active source with a copyable attachment");
    {
      SCOPED_CRITICAL_AREA_W(source_w, unlocked_source);
      source_id = source_w->store.session_id();
      source_path = source_w->store.session_path();
      std::filesystem::create_directories(paths.models_file);
    }
    forked = ava::app::run_command(unlocked_source, ava::app::CommandRequest{.command = "/fork rollback"});
    auto const created_id = forked ? std::optional<std::string>{} : app_error_context(forked.error(), "created_session_id");
    bool destination_jsonl_removed = false;
    bool destination_attachment_retained = false;
    if (created_id)
    {
      auto destination =
          ava::session::SessionStore(ava::session::SessionStoreOptions{.root_dir = paths.sessions_dir, .workspace_dir = workspace, .session_id = *created_id});
      auto const destination_attachment = ava::session::attachment_storage_root(destination) / "attachments" / "rollback.txt";
      destination_jsonl_removed = !std::filesystem::exists(destination.session_path());
      destination_attachment_retained = app_read_binary_file(destination_attachment) == "hello";
    }
    auto source_contender = ava::session::SessionLease::acquire(source_path);
    {
      SCOPED_CRITICAL_AREA_R(source_r, *unlocked_source_result);
      source_id_unchanged = source_r->store.session_id() == source_id;
    }
    expect(!forked && created_id && forked.error().message().find("rollback") == std::string::npos &&
               forked.error().format().find("rollback_attachment_disposition: preserved") != std::string::npos && destination_jsonl_removed &&
               destination_attachment_retained && source_id_unchanged && !source_contender &&
               source_contender.error().message().find("already owned") != std::string::npos,
           "TUI branch runtime-construction failure keeps the source active, removes only destination JSONL, and retains copied attachments with rollback "
           "context");
  }

  {
    auto const root = create_empty_root("app-runtime-startup-fork-rollback");

    auto const workspace = root / "workspace";
    auto const paths = app_test_paths(root);
    std::filesystem::create_directories(workspace);

    ava::app::runtime::OpenContext seed_options;
    seed_options.workspace_dir = workspace;
    seed_options.current_dir = workspace;
    seed_options.paths = paths;
    auto unlocked_source_result = ava::app::runtime::Session::open(seed_options);
    expect(unlocked_source_result.has_value(), "startup fork rollback test creates a source with a copyable attachment");
    if (!unlocked_source_result)
      return;
    std::string source_id;
    std::filesystem::path source_path;
    expect(seed_source_attachment(*unlocked_source_result), "startup fork rollback test seeds a source with a copyable attachment");
    {
      SCOPED_CRITICAL_AREA_W(source_w, *unlocked_source_result);
      source_id = source_w->store.session_id();
      source_path = source_w->store.session_path();
    }
    auto const source_bytes = app_read_binary_file(source_path);
    unlocked_source_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release startup fork rollback source"));

    auto fork_context = seed_options;
    auto forked = ava::app::runtime::Session::open(fork_context, {.sessionless = false,
                                                                .requested_session_id = std::nullopt,
                                                                .fork_session_id = source_id,
                                                                .initial_session_name = std::nullopt,
                                                                .continue_last_session = false,
                                                                .initial_reasoning_level = "minimal",
                                                                .expected_original_cwd = std::nullopt});
    auto const created_id = forked ? std::optional<std::string>{} : app_error_context(forked.error(), "created_session_id");
    bool destination_jsonl_removed = false;
    bool destination_attachment_retained = false;
    if (created_id)
    {
      auto destination =
          ava::session::SessionStore(ava::session::SessionStoreOptions{.root_dir = paths.sessions_dir, .workspace_dir = workspace, .session_id = *created_id});
      auto const destination_attachment = ava::session::attachment_storage_root(destination) / "attachments" / "rollback.txt";
      destination_jsonl_removed = !std::filesystem::exists(destination.session_path());
      destination_attachment_retained = app_read_binary_file(destination_attachment) == "hello";
    }
    auto source_store = ava::session::SessionStore::open(workspace, source_id, paths.sessions_dir);
    expect(!forked && created_id && forked.error().message().find("rollback") == std::string::npos &&
               forked.error().format().find("rollback_attachment_disposition: preserved") != std::string::npos && destination_jsonl_removed &&
               destination_attachment_retained && source_store && app_read_binary_file(source_path) == source_bytes,
           "startup fork construction failure removes only destination JSONL, retains copied attachments, reports rollback context, and preserves the source "
           "session");
  }
}

void test_app_runtime_initial_reasoning_level_option()
{
  auto const root = create_empty_root("app-runtime-initial-reasoning-level");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context, {.sessionless = false,
                                                               .requested_session_id = std::nullopt,
                                                               .fork_session_id = std::nullopt,
                                                               .initial_session_name = std::nullopt,
                                                               .continue_last_session = false,
                                                               .initial_reasoning_level = " high ",
                                                               .expected_original_cwd = std::nullopt});
  expect(unlocked_session_result.has_value(), "runtime startup applies initial reasoning level");
  if (!unlocked_session_result)
    return;
  std::string session_id;
  {
    SCOPED_CRITICAL_AREA_W(session_w, *unlocked_session_result);
    {
      ava::app::runtime::ModelSelection_ts::crat model_selection_r = session_w->model_selection_r();
      expect(model_selection_r->reasoning && model_selection_r->reasoning->level == "high", "runtime startup applies initial reasoning level");
    }
    session_id = session_w->store.session_id();

    auto entries = session_w->store.load();
    expect(entries.has_value(), "runtime startup reasoning test reloads session entries");
    if (entries)
    {
      auto const reasoning_changes = std::ranges::count_if(*entries, [](auto const& entry) { return entry.type == ava::session::EntryType::ReasoningChange; });
      expect(reasoning_changes == 1, "runtime startup reasoning appends one durable reasoning_change entry");
    }
  }

  unlocked_session_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "release runtime before startup reasoning reopen"));
  auto clear_context = open_context;
  auto unlocked_cleared_result = ava::app::runtime::Session::open(clear_context, {.sessionless = false,
                                                                  .requested_session_id = session_id,
                                                                  .fork_session_id = std::nullopt,
                                                                  .initial_session_name = std::nullopt,
                                                                  .continue_last_session = false,
                                                                  .initial_reasoning_level = "off",
                                                                  .expected_original_cwd = std::nullopt});
  expect(unlocked_cleared_result.has_value(), "runtime startup reasoning accepts off as clear_reasoning alias");
  if (unlocked_cleared_result)
  {
    SCOPED_CRITICAL_AREA_R(cleared_r, *unlocked_cleared_result);
    expect(!cleared_r->model_selection_r()->reasoning, "runtime startup reasoning accepts off as clear_reasoning alias");
    auto cleared_entries = cleared_r->store.load();
    expect(cleared_entries.has_value(), "runtime startup reasoning clear reloads entries");
    if (cleared_entries)
    {
      auto const reasoning_changes =
          std::ranges::count_if(*cleared_entries, [](auto const& entry) { return entry.type == ava::session::EntryType::ReasoningChange; });
      expect(reasoning_changes == 2, "runtime startup reasoning off appends a clear reasoning_change when clearing existing reasoning");
    }
  }

  auto invalid_context = open_context;
  auto invalid = ava::app::runtime::Session::open(invalid_context, {.sessionless = false,
                                                                  .requested_session_id = std::nullopt,
                                                                  .fork_session_id = std::nullopt,
                                                                  .initial_session_name = std::nullopt,
                                                                  .continue_last_session = false,
                                                                  .initial_reasoning_level = "minimal",
                                                                  .expected_original_cwd = std::nullopt});
  expect(!invalid.has_value() && invalid.error().format().find("option: --thinking") != std::string::npos &&
             invalid.error().format().find("supported_levels: off, low, medium, high, xhigh") != std::string::npos,
         "runtime startup reasoning reports clear --thinking validation errors");
}

}  // namespace ava::tests::app_runtime_tests
