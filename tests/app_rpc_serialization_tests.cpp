#include "sys.h"
#include "tests/app_rpc_test_cases.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/command/command.h"
#include "ava/event/events.h"
#include "ava/app/rpc/catalog.h"
#include "ava/app/rpc/output.h"
#include "ava/app/rpc/protocol.h"
#include "ava/app/rpc/resolvers.h"
#include "ava/app/rpc/serialization.h"
#include "ava/app/rpc/serialization_json.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/question.h"
#include "ava/permissions/permission.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/error.h"
#include "ava/core/json.h"
#include "ava/core/result.h"

#include <algorithm>
#include <expected>
#include <filesystem>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef AVA_RPC_V1_GOLDEN_DIR
#define AVA_RPC_V1_GOLDEN_DIR ""
#endif

namespace ava::tests::app_rpc_test {

void test_app_rpc_prompt_payload_serialization()
{
  auto const permission_json =
      ava::app::rpc::permission_request_payload_json("permission_1", ava::permissions::PermissionPrompt{.permission_request_id = "permreq_1",
                                                                                                        .operation = ava::permissions::Operation::EditFile,
                                                                                                        .mode = ava::agent::Mode::Build,
                                                                                                        .workspace_dir = "/workspace",
                                                                                                        .target_path = "/workspace/src/main.cpp",
                                                                                                        .command = "",
                                                                                                        .tool_name = "edit_file",
                                                                                                        .reason = "needs approval",
                                                                                                        .risk = ava::permissions::PermissionRisk::High,
                                                                                                        .diff_preview = "--- a\n+++ b\n-old\n+new",
                                                                                                        .diff_truncated = true});
  expect(permission_json.find("\"operation\":\"edit\"") != std::string::npos &&
             permission_json.find("\"permission_request_id\":\"permreq_1\"") != std::string::npos &&
             permission_json.find("\"target_path\":\"/workspace/src/main.cpp\"") != std::string::npos &&
             permission_json.find("\"risk\":\"high\"") != std::string::npos &&
             permission_json.find("\"diff_preview\":\"--- a\\n+++ b\\n-old\\n+new\"") != std::string::npos &&
             permission_json.find("\"diff_truncated\":true") != std::string::npos,
         "RPC permission request payload preserves semantic operation, target, risk, reason, and diff preview data");

  // The additive optional command_metadata block carries the sealed command
  // plan identity so RPC clients can display exact execution context.
  ava::permissions::CommandPermissionMetadata command_metadata;
  command_metadata.level = ava::command::CommandLevel::Critical;
  command_metadata.family = ava::command::CommandFamily::InterpreterInline;
  command_metadata.fingerprint = "sha256:ava-command-plan-v3:test-fingerprint";
  command_metadata.execution_domain = ava::command::CommandExecutionDomain::RawShell;
  command_metadata.resolved_executable = "/usr/bin/env";
  command_metadata.executable_origin = ava::command::ExecutableOrigin::System;
  command_metadata.cwd = "/workspace";
  command_metadata.containment_available = false;
  command_metadata.containment_status = ava::permissions::CommandContainmentStatus::Unavailable;
  command_metadata.backend_maximum_scope = ava::command::InteractiveScope::Once;
  command_metadata.environment_profile_id = "ava-local-bash-prompt-v2";
  command_metadata.environment_digest = "abc123";
  command_metadata.executor_identity_verified = false;
  auto const command_permission_json =
      ava::app::rpc::permission_request_payload_json("permission_cmd", ava::permissions::PermissionPrompt{.permission_request_id = "permreq_cmd",
                                                                                                          .operation = ava::permissions::Operation::RunCommand,
                                                                                                          .mode = ava::agent::Mode::Build,
                                                                                                          .workspace_dir = "/workspace",
                                                                                                          .target_path = {},
                                                                                                          .command = "python3 -c 'print(1)'",
                                                                                                          .tool_name = "bash",
                                                                                                          .reason = "sealed command requires approval",
                                                                                                          .risk = ava::permissions::PermissionRisk::Critical,
                                                                                                          .command_metadata = command_metadata});
  expect(command_permission_json.find("\"command_metadata\":{") != std::string::npos &&
             command_permission_json.find("\"level\":\"critical\"") != std::string::npos &&
             command_permission_json.find("\"family\":\"interpreter_inline\"") != std::string::npos &&
             command_permission_json.find("\"fingerprint\":\"sha256:ava-command-plan-v3:test-fingerprint\"") != std::string::npos &&
             command_permission_json.find("\"execution_domain\":\"raw_shell\"") != std::string::npos &&
             command_permission_json.find("\"resolved_executable\":\"/usr/bin/env\"") != std::string::npos &&
             command_permission_json.find("\"origin\":\"system\"") != std::string::npos &&
             command_permission_json.find("\"cwd\":\"/workspace\"") != std::string::npos &&
             command_permission_json.find("\"containment_available\":false") != std::string::npos &&
             command_permission_json.find("\"containment_status\":\"unavailable\"") != std::string::npos &&
             command_permission_json.find("\"backend_maximum_scope\":\"once\"") != std::string::npos &&
             command_permission_json.find("\"environment_profile_id\":\"ava-local-bash-prompt-v2\"") != std::string::npos &&
             command_permission_json.find("\"environment_digest\":\"abc123\"") != std::string::npos &&
             command_permission_json.find("\"executor_identity_verified\":false") != std::string::npos,
         "RPC permission request payload includes additive optional command_metadata fields for sealed commands");

  // The command_recipe_key is additive in session grant serialization: it is
  // omitted when empty and present when a sealed command plan is bound.
  ava::app::rpc::PendingResolverState grant_pending_state;
  grant_pending_state.permission_session_grants.push_back(ava::app::rpc::PermissionSessionGrant{.grant_id = "permgrant_1",
                                                                                                .permission_request_id = "permreq_1",
                                                                                                .session_id = "session_1",
                                                                                                .operation = ava::permissions::Operation::RunCommand,
                                                                                                .mode = ava::agent::Mode::Build,
                                                                                                .tool_name = "bash",
                                                                                                .target_path = {},
                                                                                                .command = "true",
                                                                                                .command_recipe_key = {},
                                                                                                .command_recipe_display = {},
                                                                                                .reason = "one-shot",
                                                                                                .risk = ava::permissions::PermissionRisk::Critical});
  auto const grants_json_no_recipe = ava::app::rpc::permission_session_grants_result_json(grant_pending_state);
  expect(grants_json_no_recipe.find("\"command_recipe_key\"") == std::string::npos,
         "RPC session grant serialization omits command_recipe_key when no sealed plan is bound");
  grant_pending_state.permission_session_grants.front().command_recipe_key =
      "sha256:ava-command-workspace-recipe-v1:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
  grant_pending_state.permission_session_grants.front().command_recipe_display = "ctest";
  auto const grants_json_with_recipe = ava::app::rpc::permission_session_grants_result_json(grant_pending_state);
  expect(grants_json_with_recipe.find(
             "\"command_recipe_key\":\"sha256:ava-command-workspace-recipe-v1:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\"") !=
             std::string::npos,
         "RPC session grant serialization includes command_recipe_key when a sealed plan is bound");

  auto const question_json = ava::app::rpc::question_request_payload_json(
      "question_1", ava::agent::QuestionPrompt{.header = "Choose",
                                               .question = "Pick providers",
                                               .options = {ava::agent::QuestionOption{.value = "openai", .label = "OpenAI"}},
                                               .multiple = true,
                                               .allow_custom = true,
                                               .secret = true,
                                               .modal = true,
                                               .searchable = true});
  expect(question_json.find("\"options\":[{\"value\":\"openai\",\"label\":\"OpenAI\"}]") != std::string::npos &&
             question_json.find("\"multiple\":true") != std::string::npos && question_json.find("\"allow_custom\":true") != std::string::npos &&
             question_json.find("\"secret\":true") != std::string::npos && question_json.find("\"modal\":true") != std::string::npos &&
             question_json.find("\"searchable\":true") != std::string::npos,
         "RPC question request payload preserves options, selection metadata, and local prompt flags");

  auto const permission_reply_json = ava::app::rpc::permission_reply_payload_json("permission_1", "deny", std::optional<std::string>{"not approved"});
  expect(permission_reply_json.find("\"resolver_request_id\":\"permission_1\"") != std::string::npos &&
             permission_reply_json.find("\"decision\":\"deny\"") != std::string::npos &&
             permission_reply_json.find("\"reason\":\"not approved\"") != std::string::npos,
         "RPC permission reply payload preserves client-supplied resolution reasons");

  auto const question_reply_json = ava::app::rpc::question_reply_payload_json(
      "question_1", std::optional<std::string>{"custom"}, std::nullopt, std::optional<std::vector<std::string>>{std::vector<std::string>{"alpha", "beta"}});
  expect(question_reply_json.find("\"resolver_request_id\":\"question_1\"") != std::string::npos &&
             question_reply_json.find("\"answer\":\"custom\"") != std::string::npos &&
             question_reply_json.find("\"selected_options\":[\"alpha\",\"beta\"]") != std::string::npos,
         "RPC question reply payload preserves multiple selections and custom text");

  ava::agent::ToolTimelineEntry tool_entry;
  tool_entry.status = ava::agent::ToolTimelineStatus::Success;
  tool_entry.call_id = "call_prompt";
  tool_entry.name = "read_file";
  tool_entry.result_summary = "read ok";
  tool_entry.result_json = "{\"ok\":true,\"path\":\"README.md\"}";
  tool_entry.structured_result_json =
      "{\"schema_version\":1,\"call_id\":\"call_prompt\",\"tool\":\"read_file\","
      "\"status\":\"success\",\"ok\":true,\"content_type\":\"application/json\","
      "\"content\":{\"ok\":true,\"path\":\"README.md\"}}";
  tool_entry.content_type = "application/json";
  tool_entry.output_lines = 1;
  ava::agent::AgentLoopResult prompt_result;
  prompt_result.final_text = "done";
  prompt_result.outcome = ava::core::RuntimeTerminalOutcome::Completed;
  prompt_result.tool_calls = 1;
  prompt_result.tool_timeline.push_back(tool_entry);
  auto const prompt_json = ava::app::rpc::prompt_result_json("session_1", prompt_result);
  expect(prompt_json.find("\"tool_timeline\"") != std::string::npos && prompt_json.find("\"structured_result\":{\"schema_version\":1") != std::string::npos &&
             prompt_json.find("\"tool\":\"read_file\"") != std::string::npos && prompt_json.find("\"output_lines\":1") != std::string::npos,
         "RPC prompt results expose structured tool timeline metadata");

  auto const cancel_json = ava::app::rpc::cancel_requested_payload_json(true, 2, 1, "prompt_1");
  expect(cancel_json == "{\"active_run\":true,\"cleared_steer\":2,\"cleared_follow_up\":1,\"active_request_id\":\"prompt_1\"}",
         "RPC cancel request payload preserves active-run and cleared-queue counters");
}

void test_app_rpc_prompt_result_tool_timeline_golden_payloads()
{
  std::string const success_structured =
      "{\"schema_version\":1,\"call_id\":\"call_success\",\"tool\":\"write_file\",\"status\":\"success\","
      "\"ok\":true,\"content_type\":\"application/json\",\"content\":{\"ok\":true,\"path\":\"note.txt\"},"
      "\"changed_paths\":[\"note.txt\"],\"permission_request_ids\":[\"permreq_write\"]}";
  ava::agent::ToolTimelineEntry success_entry;
  success_entry.status = ava::agent::ToolTimelineStatus::Success;
  success_entry.call_id = "call_success";
  success_entry.name = "write_file";
  success_entry.result_summary = "wrote note";
  success_entry.arguments_json = "{\"path\":\"note.txt\"}";
  success_entry.result_json = "{\"ok\":true,\"path\":\"note.txt\"}";
  success_entry.structured_result_json = success_structured;
  success_entry.content_type = "application/json";
  success_entry.diff = "--- note.txt\n+++ note.txt\n-old\n+new";
  success_entry.changed_paths = {"note.txt"};
  success_entry.permission_request_ids = {"permreq_write"};
  success_entry.spill_path = "spill/tool.txt";
  success_entry.diff_truncated = true;
  success_entry.truncated = true;
  success_entry.byte_limited = true;
  success_entry.line_limited = true;
  success_entry.spill_truncated = true;
  success_entry.output_bytes = 128;
  success_entry.total_bytes = 512;
  success_entry.output_lines = 2;
  success_entry.total_lines = 5;
  success_entry.start_line = 1;
  success_entry.end_line = 2;
  success_entry.next_offset_line = 3;
  success_entry.omitted_bytes = 384;
  success_entry.omitted_lines = 3;

  std::string const denied_structured =
      "{\"schema_version\":1,\"call_id\":\"call_denied\",\"tool\":\"write_file\",\"status\":\"error\","
      "\"ok\":false,\"summary\":\"permission denied\",\"content_type\":\"text/plain\","
      "\"content\":\"permission denied\",\"error\":{\"category\":\"permission\",\"code\":\"permission_denied\","
      "\"message\":\"permission denied\",\"details\":\"resolution: deny\"}}";
  ava::agent::ToolTimelineEntry denied_entry;
  denied_entry.status = ava::agent::ToolTimelineStatus::Error;
  denied_entry.call_id = "call_denied";
  denied_entry.name = "write_file";
  denied_entry.result_summary = "permission denied";
  denied_entry.structured_result_json = denied_structured;
  denied_entry.content_type = "text/plain";
  denied_entry.error_category = "permission";
  denied_entry.error_code = "permission_denied";
  denied_entry.error_message = "permission denied";
  denied_entry.error_details = "resolution: deny";

  std::string const canceled_structured =
      "{\"schema_version\":1,\"call_id\":\"call_canceled\",\"tool\":\"glob\",\"status\":\"canceled\","
      "\"ok\":false,\"summary\":\"tool canceled\",\"content_type\":\"text/plain\","
      "\"content\":\"tool canceled\",\"error\":{\"category\":\"canceled\",\"code\":\"tool_canceled\","
      "\"message\":\"tool call canceled\",\"details\":\"cancel requested\"}}";
  ava::agent::ToolTimelineEntry canceled_entry;
  canceled_entry.status = ava::agent::ToolTimelineStatus::Canceled;
  canceled_entry.call_id = "call_canceled";
  canceled_entry.name = "glob";
  canceled_entry.result_summary = "tool canceled";
  canceled_entry.structured_result_json = canceled_structured;
  canceled_entry.content_type = "text/plain";
  canceled_entry.error_category = "canceled";
  canceled_entry.error_code = "tool_canceled";
  canceled_entry.error_message = "tool call canceled";
  canceled_entry.error_details = "cancel requested";

  ava::agent::AgentLoopResult prompt_result;
  prompt_result.final_text = "done";
  prompt_result.outcome = ava::core::RuntimeTerminalOutcome::Completed;
  prompt_result.provider_iterations = 2;
  prompt_result.tool_calls = 3;
  prompt_result.tool_timeline = {success_entry, denied_entry, canceled_entry};

  auto const success_entry_json = std::string{"{\"status\":\"success\",\"call_id\":\"call_success\",\"tool\":\"write_file\","} +
                                  "\"text\":\"wrote note\",\"result_summary\":\"wrote note\",\"args\":{\"path\":\"note.txt\"}," +
                                  "\"result\":{\"ok\":true,\"path\":\"note.txt\"},\"structured_result\":" + success_structured +
                                  ",\"content_type\":\"application/json\",\"diff\":\"--- note.txt\\n+++ note.txt\\n-old\\n+new\"," +
                                  "\"changed_paths\":[\"note.txt\"],\"permission_request_ids\":[\"permreq_write\"]," +
                                  "\"spill_path\":\"spill/tool.txt\",\"diff_truncated\":true,\"truncated\":true,"
                                  "\"byte_limited\":true,\"line_limited\":true,\"spill_truncated\":true,\"output_bytes\":128,"
                                  "\"total_bytes\":512,\"output_lines\":2,\"total_lines\":5,\"start_line\":1,\"end_line\":2,"
                                  "\"next_offset_line\":3,\"omitted_bytes\":384,\"omitted_lines\":3}";
  auto const denied_entry_json = std::string{"{\"status\":\"error\",\"call_id\":\"call_denied\",\"tool\":\"write_file\","} +
                                 "\"text\":\"permission denied\",\"result_summary\":\"permission denied\",\"structured_result\":" + denied_structured +
                                 ",\"content_type\":\"text/plain\",\"category\":\"permission\",\"error_code\":\"permission_denied\"," +
                                 "\"message\":\"permission denied\",\"details\":\"resolution: deny\"}";
  auto const canceled_entry_json = std::string{"{\"status\":\"canceled\",\"call_id\":\"call_canceled\",\"tool\":\"glob\","} +
                                   "\"text\":\"tool canceled\",\"result_summary\":\"tool canceled\",\"structured_result\":" + canceled_structured +
                                   ",\"content_type\":\"text/plain\",\"category\":\"canceled\",\"error_code\":\"tool_canceled\"," +
                                   "\"message\":\"tool call canceled\",\"details\":\"cancel requested\"}";
  auto const expected_json = std::string{"{\"session_id\":\"session_rpc\",\"final_text\":\"done\",\"stop_reason\":\"completed\","} +
                             "\"provider_iterations\":2,\"tool_calls\":3,\"tool_timeline\":[" + success_entry_json + "," + denied_entry_json + "," +
                             canceled_entry_json + "]}";

  auto const prompt_json = ava::app::rpc::prompt_result_json("session_rpc", prompt_result);
  expect(prompt_json == expected_json,
         "RPC prompt result golden locks success, denied/error, canceled, diff, changed path, permission id, and spill tool timeline shapes");
}

void test_app_rpc_parsing_and_response_serialization()
{
  auto command = ava::app::parse_rpc_command_line("{\"id\":\"1\",\"type\":\"prompt\",\"message\":\"hello\\nava\",\"instructions\":\"keep\"}");
  expect(command && command->id == "1" && command->type == "prompt" && command->message && *command->message == "hello\nava" && command->instructions &&
             *command->instructions == "keep",
         "RPC parser extracts string envelope fields and unescapes JSON strings");

  auto image_prompt = ava::app::parse_rpc_command_line(R"JSON({"id":"img","type":"prompt","message":"look","attachments":["screen.png"]})JSON");
  expect(image_prompt && image_prompt->attachments && image_prompt->attachments->size() == 1 && (*image_prompt->attachments)[0] == "screen.png",
         "RPC parser preserves prompt image attachment path arrays");

  auto uploaded_image_prompt = ava::app::parse_rpc_command_line(
      R"JSON({"id":"img-upload","type":"prompt","message":"look","images":[{"type":"image","data":"QUJD","mimeType":"image/png"}]})JSON");
  expect(uploaded_image_prompt && uploaded_image_prompt->images && uploaded_image_prompt->images->size() == 1 &&
             (*uploaded_image_prompt->images)[0].type == "image" && (*uploaded_image_prompt->images)[0].data_base64 == "QUJD" &&
             (*uploaded_image_prompt->images)[0].mime_type == "image/png",
         "RPC parser preserves Pi-style inline image upload objects");

  auto invalid_uploaded_image = ava::app::parse_rpc_command_line(R"JSON({"id":"img-upload-bad","type":"prompt","message":"look","images":["bad"]})JSON");
  expect(!invalid_uploaded_image && invalid_uploaded_image.error().message() == "RPC images must be an array of image objects",
         "RPC parser rejects non-object inline image upload entries");

  auto missing_upload_data = ava::app::parse_rpc_command_line(
      R"JSON({"id":"img-upload-missing","type":"prompt","message":"look","images":[{"type":"image","mimeType":"image/png"}]})JSON");
  expect(!missing_upload_data && missing_upload_data.error().message() == "RPC images entries require data",
         "RPC parser rejects inline image uploads without data");

  auto invalid_upload_type = ava::app::parse_rpc_command_line(
      R"JSON({"id":"img-upload-type","type":"prompt","message":"look","images":[{"type":"file","data":"QUJD","mimeType":"image/png"}]})JSON");
  expect(!invalid_upload_type && invalid_upload_type.error().message() == "RPC images entries must have type image",
         "RPC parser rejects inline image upload entries with non-image type");

  auto invalid_image_prompt = ava::app::parse_rpc_command_line(R"JSON({"id":"img-bad","type":"prompt","message":"look","attachments":["ok.png",2]})JSON");
  expect(!invalid_image_prompt && invalid_image_prompt.error().message() == "RPC attachments must be an array of strings",
         "RPC parser rejects non-string prompt attachment path entries");

  auto empty_image_prompt = ava::app::parse_rpc_command_line(R"JSON({"id":"img-empty","type":"prompt","message":"look","attachments":[""]})JSON");
  expect(!empty_image_prompt && empty_image_prompt.error().message() == "RPC attachments entries must be non-empty",
         "RPC parser rejects empty prompt attachment paths");

  auto reply = ava::app::parse_rpc_command_line(R"JSON({"id":"reply","type":"permission_reply","request_id":"permission_1",)JSON"
                                                R"JSON("correlation_id":"prompt_1","decision":"deny","reason":"not approved for this run"})JSON");
  expect(reply && reply->reason && *reply->reason == "not approved for this run", "RPC parser preserves optional permission reply reasons");

  auto question_reply = ava::app::parse_rpc_command_line(R"JSON({"id":"question","type":"question_reply","request_id":"question_1",)JSON"
                                                         R"JSON("correlation_id":"prompt_1","selected_options":["alpha","beta"],"answer":"custom"})JSON");
  expect(question_reply && question_reply->selected_options && question_reply->selected_options->size() == 2 &&
             (*question_reply->selected_options)[0] == "alpha" && (*question_reply->selected_options)[1] == "beta",
         "RPC parser preserves selected_options arrays for question replies");

  auto branch = ava::app::parse_rpc_command_line(R"JSON({"id":"fork","type":"fork_session","branch_from_entry_id":"entry_1"})JSON");
  expect(branch && branch->branch_from_entry_id && *branch->branch_from_entry_id == "entry_1",
         "RPC parser preserves branch_from_entry_id for session fork commands");

  auto empty_branch = ava::app::parse_rpc_command_line(R"JSON({"id":"fork","type":"fork_session","branch_from_entry_id":""})JSON");
  expect(!empty_branch && empty_branch.error().message() == "fork_session branch_from_entry_id must be non-empty when provided",
         "RPC parser rejects empty explicit branch_from_entry_id for session fork commands");

  auto non_string_session = ava::app::parse_rpc_command_line(R"JSON({"id":"summary","type":"summarize_branch","session_id":123})JSON");
  expect(!non_string_session && non_string_session.error().message() == "RPC session_id must be a string",
         "RPC parser rejects non-string session selectors for session mutation commands");

  auto non_string_session_name = ava::app::parse_rpc_command_line(R"JSON({"id":"fork","type":"fork_session","session_name":123})JSON");
  expect(!non_string_session_name && non_string_session_name.error().message() == "RPC session_name must be a string",
         "RPC parser rejects non-string session names for session mutation commands");

  auto branch_summary = ava::app::parse_rpc_command_line(R"JSON({"id":"summary","type":"summarize_branch","branch_root_entry_id":"entry_1",)JSON"
                                                         R"JSON("branch_tip_entry_id":"entry_2","summary":"line one\nline two",)JSON"
                                                         R"JSON("provider":"openai","model":"gpt-test","reason":"user requested"})JSON");
  expect(branch_summary && branch_summary->branch_root_entry_id && *branch_summary->branch_root_entry_id == "entry_1" && branch_summary->branch_tip_entry_id &&
             *branch_summary->branch_tip_entry_id == "entry_2" && branch_summary->summary && *branch_summary->summary == "line one\nline two" &&
             branch_summary->provider && *branch_summary->provider == "openai" && branch_summary->model && *branch_summary->model == "gpt-test" &&
             branch_summary->reason && *branch_summary->reason == "user requested",
         "RPC parser preserves summarize_branch source range, summary text, and provenance fields");

  auto invalid_branch_summary = ava::app::parse_rpc_command_line(R"JSON({"id":"summary","type":"summarize_branch","summary":"bad\u001b"})JSON");
  expect(!invalid_branch_summary && invalid_branch_summary.error().message() == "RPC text field contains invalid character",
         "RPC parser rejects control bytes in branch summary text");

  auto plugin_command_object_args = ava::app::parse_rpc_command_line(
      R"JSON({"id":"plugin","type":"run_plugin_command","plugin_id":"com.example.tool","name":"status","arguments":{"verbose":true}})JSON");
  expect(plugin_command_object_args && plugin_command_object_args->arguments && *plugin_command_object_args->arguments == R"JSON({"verbose":true})JSON",
         "RPC parser preserves object-shaped plugin command arguments");

  auto plugin_command_string_args = ava::app::parse_rpc_command_line(
      R"JSON({"id":"plugin","type":"run_plugin_command","plugin_id":"com.example.tool","name":"status","arguments":"{\"verbose\":true}"})JSON");
  expect(plugin_command_string_args && plugin_command_string_args->arguments && *plugin_command_string_args->arguments == R"JSON({"verbose":true})JSON",
         "RPC parser accepts legacy stringified JSON-object plugin command arguments");

  auto invalid_plugin_command_args = ava::app::parse_rpc_command_line(
      R"JSON({"id":"plugin","type":"run_plugin_command","plugin_id":"com.example.tool","name":"status","arguments":"not json"})JSON");
  expect(!invalid_plugin_command_args && invalid_plugin_command_args.error().message() == "RPC string arguments must contain a JSON object",
         "RPC parser rejects string plugin command arguments that are not JSON objects");

  auto non_object_plugin_command_args =
      ava::app::parse_rpc_command_line(R"JSON({"id":"plugin","type":"run_plugin_command","plugin_id":"com.example.tool","name":"status","arguments":123})JSON");
  expect(!non_object_plugin_command_args && non_object_plugin_command_args.error().message() == "RPC arguments must be an object",
         "RPC parser rejects non-object plugin command arguments instead of silently defaulting");

  auto wait_job = ava::app::parse_rpc_command_line(R"JSON({"id":"wait-job","type":"wait_job","job_id":"job_123","timeout_ms":45000})JSON");
  expect(wait_job && wait_job->job_id == "job_123" && wait_job->timeout_ms == 45000, "RPC parser preserves strict job wait fields for handler clamping");
  auto invalid_job_id = ava::app::parse_rpc_command_line(R"JSON({"id":"job","type":"get_job","job_id":42})JSON");
  expect(!invalid_job_id && invalid_job_id.error().message() == "RPC job_id must be a string", "RPC parser rejects wrong-typed job identifiers");
  auto invalid_job_timeout = ava::app::parse_rpc_command_line(R"JSON({"id":"job","type":"wait_job","job_id":"job_123","timeout_ms":"1"})JSON");
  expect(!invalid_job_timeout && invalid_job_timeout.error().message() == "RPC timeout_ms must be an integer",
         "RPC parser rejects wrong-typed job wait timeouts");

  auto direct_run_command = ava::app::parse_rpc_command_line(R"JSON({"id":"cmd","type":"run_command","command":"printf rpc-direct"})JSON");
  expect(direct_run_command && direct_run_command->command && *direct_run_command->command == "printf rpc-direct",
         "RPC parser preserves direct command execution payloads");

  auto invalid_direct_run_command = ava::app::parse_rpc_command_line(R"JSON({"id":"cmd","type":"run_command","command":123})JSON");
  expect(!invalid_direct_run_command && invalid_direct_run_command.error().message() == "RPC command must be a string",
         "RPC parser rejects non-string direct command execution payloads");

  auto malformed_with_string_prefix = ava::app::parse_rpc_command_line(R"JSON({"id":"bad","type":"summarize_branch","session_id":"session_a" garbage})JSON");
  expect(!malformed_with_string_prefix && malformed_with_string_prefix.error().message() == "malformed RPC JSON object",
         "RPC parser rejects malformed JSON even when a string prefix is parseable");

  auto clone_with_branch_from = ava::app::parse_rpc_command_line(R"JSON({"id":"clone","type":"clone_session","branch_from_entry_id":"entry_1"})JSON");
  expect(!clone_with_branch_from && clone_with_branch_from.error().message() == "clone_session does not support branch_from_entry_id",
         "RPC parser rejects misleading branch_from_entry_id on clone_session");

  auto unrelated_with_command_specific_fields = ava::app::parse_rpc_command_line(
      "{\"id\":\"prompt\",\"type\":\"prompt\",\"message\":\"hello\","
      "\"labels\":[\"ok\",2],\"branch_from_entry_id\":\"bad\\u001b\",\"grant_id\":\"bad grant\"}");
  expect(unrelated_with_command_specific_fields && unrelated_with_command_specific_fields->type == "prompt",
         "RPC parser ignores command-specific session and grant fields on unrelated commands");

  auto invalid_grant_revoke = ava::app::parse_rpc_command_line(R"JSON({"id":"revoke","type":"permission_grant_revoke","grant_id":"bad grant"})JSON");
  expect(!invalid_grant_revoke && invalid_grant_revoke.error().message() == "RPC identifier contains invalid character",
         "RPC parser validates grant_id only for permission_grant_revoke");

  auto invalid_selected_options = ava::app::parse_rpc_command_line(R"JSON({"id":"bad","type":"question_reply","selected_options":["ok",2]})JSON");
  expect(!invalid_selected_options && invalid_selected_options.error().message() == "RPC selected_options must be an array of strings",
         "RPC parser rejects non-string selected_options entries");

  auto oversized_reason = ava::app::parse_rpc_command_line("{\"id\":\"reply\",\"type\":\"permission_reply\",\"reason\":\"" +
                                                           std::string(ava::app::rpc::kMaxRpcReasonBytes + 1, 'x') + "\"}");
  expect(!oversized_reason && oversized_reason.error().message() == "RPC text field is too long",
         "RPC parser rejects oversized text fields before emitting resolver events");

  auto control_reason = ava::app::parse_rpc_command_line(R"JSON({"id":"reply","type":"permission_reply","reason":"bad\u001b"})JSON");
  expect(!control_reason && control_reason.error().message() == "RPC text field contains invalid character",
         "RPC parser rejects control bytes in free-text resolver reasons");

  auto malformed = ava::app::parse_rpc_command_line("{\"id\":\"bad\",\"type\":\"prompt\"");
  expect(!malformed && malformed.error().category() == ava::core::ErrorCategory::InvalidArgument, "RPC parser rejects malformed JSON object lines");

  auto oversized_id = ava::app::parse_rpc_command_line("{\"id\":\"" + std::string(257, 'x') + "\",\"type\":\"prompt\"}");
  expect(!oversized_id && oversized_id.error().message() == "RPC identifier is too long", "RPC parser rejects oversized request identifiers before queueing");

  auto const success = ava::app::serialize_rpc_success_jsonl("a\"b", "{\"value\":1}");
  expect(success == "{\"id\":\"a\\\"b\",\"type\":\"response\",\"success\":true,\"result\":{\"value\":1}}\n",
         "RPC success response serializes deterministic JSONL with escaped id");

  auto const error = ava::app::serialize_rpc_error_jsonl("e1", ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "bad \"request\""));
  expect(error.find("\"success\":false") != std::string::npos && error.find("bad \\\"request\\\"") != std::string::npos && error.ends_with('\n'),
         "RPC error response serializes JSONL error details");
}

void test_app_rpc_identifier_validation()
{
  auto allowed = ava::app::parse_rpc_command_line(R"JSON({"id":"rpc.1","type":"set_model","request_id":"req_1","correlation_id":"corr-1",)JSON"
                                                  R"JSON("provider":"openai","model":"openai/gpt-5.5","plugin_id":"com.example.rpc",)JSON"
                                                  R"JSON("name":"demo-server_1","server_id":"demo-server_1"})JSON");
  expect(allowed && allowed->model && *allowed->model == "openai/gpt-5.5" && allowed->plugin_id && *allowed->plugin_id == "com.example.rpc" &&
             allowed->server_id && *allowed->server_id == "demo-server_1",
         "RPC parser allows practical dotted, dashed, underscored, and slash-delimited identifiers");

  auto control = ava::app::parse_rpc_command_line(R"JSON({"id":"bad\u001f","type":"prompt"})JSON");
  expect(!control && control.error().message() == "RPC identifier contains invalid character", "RPC parser rejects escaped control bytes in identifiers");

  std::string del_line = R"JSON({"id":"bad)JSON";
  del_line.push_back(static_cast<char>(0x7F));
  del_line += R"JSON(","type":"prompt"})JSON";
  auto del = ava::app::parse_rpc_command_line(del_line);
  expect(!del && del.error().message() == "RPC identifier contains invalid character", "RPC parser rejects DEL bytes in identifiers");

  auto whitespace = ava::app::parse_rpc_command_line(R"JSON({"id":"bad id","type":"prompt"})JSON");
  expect(!whitespace && whitespace.error().message() == "RPC identifier contains invalid character", "RPC parser rejects ASCII whitespace in identifiers");

  auto metachar = ava::app::parse_rpc_command_line(R"JSON({"id":"ok","type":"inspect_plugin","plugin_id":"bad;id"})JSON");
  expect(!metachar && metachar.error().message() == "RPC identifier contains invalid character",
         "RPC parser rejects command-ambiguous metacharacters in slash-command identifiers");

  auto path = ava::app::parse_rpc_command_line(R"JSON({"id":"path-ok","type":"validate_plugin","path":"./plugins/bad; path.json"})JSON");
  expect(path && path->path && *path->path == "./plugins/bad; path.json", "RPC parser leaves validate_plugin path validation to the plugin path handler");

  std::string escaped_control_path = R"JSON({"id":"path-bad","type":"install_plugin","path":"./plugins/)JSON";
  escaped_control_path += "\\u001f";
  escaped_control_path += R"JSON(bad"})JSON";
  auto control_path = ava::app::parse_rpc_command_line(escaped_control_path);
  expect(!control_path && control_path.error().message() == "RPC text field contains invalid character", "RPC parser rejects control bytes in path fields");
}

void test_app_rpc_production_catalog_and_golden_contract()
{
  auto const golden = std::filesystem::path(AVA_RPC_V1_GOLDEN_DIR);
  auto const manifest = app_read_binary_file(golden / "manifest.json");
  auto const manifest_commands = ava::core::json::strings_in_array_field(manifest, "request_types");
  auto const manifest_events = ava::core::json::strings_in_array_field(manifest, "event_names");
  auto const manifest_errors = ava::core::json::strings_in_array_field(manifest, "stable_error_codes");
  auto as_set = [](auto const& values) { return std::set<std::string>(values.begin(), values.end()); };
  auto const production_commands = as_set(ava::app::rpc::rpc_command_types());
  auto const production_events = as_set(ava::app::rpc::rpc_event_names());
  auto const production_errors = as_set(ava::app::rpc::rpc_stable_error_codes());
  expect(production_commands == as_set(manifest_commands) && production_events == as_set(manifest_events) && production_errors == as_set(manifest_errors) &&
             production_commands.size() == ava::app::rpc::rpc_command_types().size() && production_events.size() == ava::app::rpc::rpc_event_names().size() &&
             production_errors.size() == ava::app::rpc::rpc_stable_error_codes().size(),
         "RPC production command/event/error catalogs match the manifest bidirectionally without duplicate production entries");
  expect(ava::core::json::integer_field(manifest, "protocol_version") == ava::app::rpc::kRpcProtocolVersions.protocol &&
             ava::core::json::integer_field(manifest, "event_schema_version") == ava::app::rpc::kRpcProtocolVersions.event_schema,
         "RPC production protocol and event versions match the manifest");

  bool all_catalog_commands_parse = true;
  for (auto const type : ava::app::rpc::rpc_command_types())
  {
    auto parsed = ava::app::parse_rpc_command_line("{\"id\":\"catalog\",\"type\":\"" + std::string(type) + "\"}");
    all_catalog_commands_parse = all_catalog_commands_parse && parsed && parsed->type == type;
  }
  expect(all_catalog_commands_parse, "RPC parser recognizes every production-catalog command type before command-specific required-field dispatch");

  std::string actual_errors;
  actual_errors += ava::app::serialize_rpc_error_jsonl("bad", ava::app::rpc::invalid_rpc("RPC message must be a string"));
  actual_errors += ava::app::serialize_rpc_error_jsonl("busy", ava::app::rpc::active_run_reject_error("prompt"));
  actual_errors += ava::app::serialize_rpc_error_jsonl("cancelled", ava::app::rpc::canceled_error());
  actual_errors += ava::app::serialize_rpc_error_jsonl("follow", ava::app::rpc::skipped_follow_up_error("canceled"));
  expect(actual_errors == app_read_binary_file(golden / "errors.jsonl"),
         "RPC normative error golden is exact production serialization including category/code/details");

  std::string actual_envelopes;
  actual_envelopes += ava::app::serialize_rpc_success_jsonl("req-1", "{\"protocol_version\":1}");
  actual_envelopes += ava::app::serialize_rpc_error_jsonl("bad-1", ava::app::rpc::invalid_rpc("malformed RPC JSON object"));
  ava::event::MessagePayload event_payload;
  event_payload.text = "hello";
  event_payload.status = "streaming";
  auto event = ava::event::RuntimeEvent{{.timestamp = "2026-07-12T00:00:00Z", .session_id = "session-1"},
                                        ava::event::MessageUpdateEvent{.payload = std::move(event_payload)}};
  ava::event::EventEnvelopeContext event_context;
  event_context.event_id = "event-1";
  event_context.request_id = "prompt-1";
  event_context.correlation_id = "prompt-1";
  actual_envelopes += ava::event::serialize_event_envelope_jsonl(ava::event::to_event_envelope(event, event_context));
  expect(actual_envelopes == app_read_binary_file(golden / "envelopes.jsonl"),
         "RPC response/event envelope golden is exact deterministic production serialization");

  auto const wire = app_read_binary_file(golden / "wire.jsonl");
  auto const first_newline = wire.find('\n');
  auto parsed_wire = ava::app::parse_rpc_command_line(wire.substr(0, first_newline));
  auto const actual_wire_response = ava::app::serialize_rpc_success_jsonl("protocol", ava::app::rpc::rpc_protocol_result_json());
  expect(parsed_wire && parsed_wire->type == "get_protocol" && wire.substr(first_newline + 1) == actual_wire_response,
         "RPC get_protocol golden request uses the production parser and its response uses the production serializer");
}

void test_app_rpc_contract_validation_regressions()
{
  std::vector<std::string> const malformed_recognized_fields = {
      R"JSON({"id":"scope","type":"permission_rule_add","action":"allow","operation":"read","scope":7,"reason":"x"})JSON",
      R"JSON({"id":"mode","type":"permission_rule_add","action":"allow","operation":"read","mode":false,"reason":"x"})JSON",
      R"JSON({"id":"provider","type":"set_model","provider":{},"model":"gpt-5.5"})JSON",
      R"JSON({"id":"display","type":"set_reasoning","reasoning_level":"low","reasoning_display":[]})JSON",
      R"JSON({"id":"compact","type":"compact","instructions":9})JSON",
      R"JSON({"id":"invoke","type":"invoke_command","name":"help","command_arguments":{}})JSON",
      R"JSON({"id":"question-answer","type":"question_reply","request_id":"q","correlation_id":"p","answer":1})JSON",
      R"JSON({"id":"question-selected","type":"question_reply","request_id":"q","correlation_id":"p","selected":true})JSON",
  };
  for (auto const& input : malformed_recognized_fields)
  {
    auto parsed = ava::app::parse_rpc_command_line(input);
    expect(!parsed && parsed.error().message().find("must be a string") != std::string::npos,
           "RPC parser rejects wrong-typed fields recognized by the selected command");
  }

  auto unrelated =
      ava::app::parse_rpc_command_line(R"JSON({"id":"state","type":"get_state","scope":7,"provider":{},"reasoning_display":[],"command_arguments":{}})JSON");
  expect(unrelated.has_value(), "RPC parser ignores malformed fields that are unrelated to the selected command");

  std::string invalid_utf8 = R"JSON({"id":"bad-utf8","type":"prompt","message":")JSON";
  invalid_utf8.push_back(static_cast<char>(0xC3));
  invalid_utf8 += R"JSON("})JSON";
  auto invalid = ava::app::parse_rpc_command_line(invalid_utf8);
  expect(!invalid && invalid.error().message() == "RPC request is not valid UTF-8", "RPC parser validates UTF-8 before JSON extraction");

  auto const invalid_response = ava::app::serialize_rpc_error_jsonl("", invalid.error());
  expect(invalid_response.find("\"code\":\"invalid_request\"") != std::string::npos &&
             std::ranges::all_of(invalid_response, [](char ch) { return static_cast<unsigned char>(ch) < 0x80U; }),
         "invalid UTF-8 produces an ASCII-safe recoverable error with stable code");

  ava::event::ToolPayload invalid_tool_payload;
  invalid_tool_payload.args_json = "{\"path\":\"";
  invalid_tool_payload.args_json.push_back(static_cast<char>(0xFF));
  invalid_tool_payload.args_json += "\"}";
  auto invalid_event = ava::event::RuntimeEvent{{}, ava::event::ToolStartEvent{.payload = invalid_tool_payload}};
  auto const invalid_payload_json = ava::event::serialize_payload_json(invalid_tool_payload);
  auto const invalid_envelope = ava::event::to_event_envelope(invalid_event);
  auto const serialized_invalid_event = ava::event::serialize_event_envelope_json(invalid_envelope);
  expect(invalid_payload_json.find("\"args\":{") == std::string::npos && invalid_payload_json.find("\"args_json\":") != std::string::npos &&
             invalid_envelope.payload_json == invalid_payload_json && ava::core::json::is_valid_utf8(serialized_invalid_event) &&
             ava::core::json::is_valid_object(serialized_invalid_event),
         "typed RPC event payload serialization rejects invalid UTF-8 argument JSON and preserves the envelope fallback path");

  auto active = ava::app::rpc::active_run_reject_error("prompt");
  auto canceled = ava::app::rpc::canceled_error();
  auto skipped = ava::app::rpc::skipped_follow_up_error("canceled");
  expect(ava::app::serialize_rpc_error_jsonl("a", active).find("\"code\":\"active_run\"") != std::string::npos &&
             ava::app::serialize_rpc_error_jsonl("c", canceled).find("\"code\":\"canceled\"") != std::string::npos &&
             ava::app::serialize_rpc_error_jsonl("f", skipped).find("\"code\":\"follow_up_skipped\"") != std::string::npos,
         "RPC stable error codes cover active, canceled, and skipped follow-up branches");

  auto runtime_canceled = ava::core::Error(ava::core::ErrorCategory::Unknown, "agent loop canceled", ava::core::ErrorCode::Canceled);
  runtime_canceled.with_context("boundary", "after_tool_dispatch");
  auto const runtime_canceled_json = ava::app::serialize_rpc_error_jsonl("runtime-canceled", runtime_canceled);
  expect(runtime_canceled_json ==
             "{\"id\":\"runtime-canceled\",\"type\":\"response\",\"success\":false,\"error\":{\"category\":\"unknown\",\"code\":\"canceled\","
             "\"message\":\"agent loop canceled\",\"details\":\"unknown: agent loop canceled\\n  boundary: after_tool_dispatch\"}}\n",
         "typed cancellation preserves the existing RPC response byte for byte");
  auto new_wording = ava::core::Error(ava::core::ErrorCategory::Unknown, "operation stopped", ava::core::ErrorCode::Canceled);
  expect(ava::app::serialize_rpc_error_jsonl("new", new_wording).find("\"code\":\"canceled\"") != std::string::npos &&
             ava::app::serialize_rpc_error_jsonl("negative", ava::core::Error(ava::core::ErrorCategory::Unknown, "not canceled"))
                     .find("\"code\":\"internal_error\"") != std::string::npos &&
             ava::app::serialize_rpc_error_jsonl("untyped", ava::core::Error(ava::core::ErrorCategory::Unknown, "agent loop canceled"))
                     .find("\"code\":\"internal_error\"") != std::string::npos,
         "RPC cancellation uses identity rather than English wording");
  auto const independent_unknown =
      ava::app::serialize_rpc_error_jsonl("provider-unknown", ava::core::Error(ava::core::ErrorCategory::Unknown, "provider returned an unknown failure"));
  expect(runtime_canceled_json.find("\"category\":\"unknown\",\"code\":\"canceled\",\"message\":\"agent loop canceled\"") != std::string::npos &&
             runtime_canceled_json.find("\"details\":\"unknown: agent loop canceled\\n  boundary: after_tool_dispatch\"") != std::string::npos &&
             independent_unknown.find("\"code\":\"internal_error\"") != std::string::npos,
         "RPC boundary classifies known runtime cancellation semantics without changing details or unrelated unknown errors");
}

void test_app_rpc_utf8_recovery_and_framing()
{
  std::istringstream framed(
      "{\"id\":\"one\",\"type\":\"get_state\"}\r\n"
      "{\"id\":\"two\",\"type\":\"get_state\"}");
  std::string line;
  auto first = ava::app::rpc::read_rpc_line_bounded(framed, line);
  expect(first && *first && line.ends_with('\r'), "RPC framing uses LF and retains one CR for the parser to strip");
  auto first_command = ava::app::parse_rpc_command_line(line);
  auto second = ava::app::rpc::read_rpc_line_bounded(framed, line);
  auto second_command = second && *second ? ava::app::parse_rpc_command_line(line)
                                          : ava::core::Result<ava::app::RpcCommand>{std::unexpected(ava::app::rpc::invalid_rpc("missing"))};
  auto eof = ava::app::rpc::read_rpc_line_bounded(framed, line);
  expect(first_command && first_command->id == "one" && second_command && second_command->id == "two" && eof && !*eof,
         "RPC framing accepts CRLF and processes an unterminated final JSON record before clean EOF");

  auto const root = create_empty_root("app-rpc-invalid-utf8-recovery");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto unlocked_session_result = ava::app::runtime::Session::open(open_context);
  expect(unlocked_session_result.has_value(), "RPC invalid UTF-8 recovery test opens runtime session");
  if (!unlocked_session_result)
    return;
  ava::app::runtime::session_ts& unlocked_session(*unlocked_session_result);

  std::string const replacement = "\xEF\xBF\xBD";
  std::string invalid_component = "bad";
  invalid_component.push_back(static_cast<char>(0xFF));
  std::string invalid_state;
  {
    SCOPED_CRITICAL_AREA_W(session_w, unlocked_session);
    session_w->invocation_inputs().workspace_dir = std::filesystem::path(invalid_component);
    session_w->invocation_inputs().current_dir = std::filesystem::path(invalid_component);
    invalid_state = session_w->state_result_json_1(false);
  }
  auto const invalid_path = ava::app::rpc::string_field_json("path", invalid_component);
  ava::app::CommandResult invalid_output_result;
  invalid_output_result.handled = true;
  invalid_output_result.output = {invalid_component};
  auto const invalid_output = ava::app::rpc::command_result_json(invalid_output_result);
  expect(invalid_state.find("\"workspace_dir\":\"bad" + replacement + "\"") != std::string::npos &&
             invalid_state.find("\"current_dir\":\"bad" + replacement + "\"") != std::string::npos && invalid_path == "\"path\":\"bad" + replacement + "\"" &&
             invalid_output.find("\"output\":[\"bad" + replacement + "\"]") != std::string::npos &&
             invalid_output.find("\"text\":\"bad" + replacement + "\"") != std::string::npos,
         "RPC string serializers replace exact invalid workspace, path, output-array, and output-text bytes with U+FFFD");

  std::ostringstream boundary_stream;
  ava::app::rpc::output_ts boundary_output(boundary_stream, [] { });
  std::string raw_invalid_record = "{\"raw\":\"";
  raw_invalid_record.push_back(static_cast<char>(0xFF));
  raw_invalid_record += "\"}\n";
  auto boundary_written = ava::app::rpc::Output::write_record(boundary_output, raw_invalid_record);
  expect(boundary_written && boundary_stream.str() == "{\"raw\":\"" + replacement + "\"}\n" && ava::core::json::is_valid_utf8(boundary_stream.str()),
         "RPC sole output boundary validates and repairs the complete record before writing");

  std::string input = R"JSON({"id":"bad","type":"prompt","message":")JSON";
  input.push_back(static_cast<char>(0xFF));
  input += "\"}\n{\"id\":\"state-after\",\"type\":\"get_state\"}\n";
  std::istringstream in(input);
  std::ostringstream out;
  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  auto result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, {}, in, out, ava::app::rpc::RpcInputWake{});
  auto const jsonl = out.str();
  expect(result && jsonl.find("\"id\":\"\",\"type\":\"response\",\"success\":false") != std::string::npos &&
             jsonl.find("RPC request is not valid UTF-8") != std::string::npos && jsonl.find("\"id\":\"state-after\"") != std::string::npos &&
             ava::core::json::is_valid_utf8(jsonl),
         "RPC loop recovers after invalid UTF-8 with an empty-id valid UTF-8 JSON response");
}

}  // namespace ava::tests::app_rpc_test
