#include "sys.h"
#include "tests/acp_test_declarations.h"
#include "tests/support/acp_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/event/RuntimeEvent.h"
#include "ava/app/acp/codec.h"
#include "ava/app/acp/content.h"
#include "ava/app/acp/envelope_intent.h"
#include "ava/app/acp/permission.h"
#include "ava/app/acp/session_update.h"
#include "ava/app/acp/transport.h"
#include "ava/agent/mode.h"
#include "ava/permissions/permission.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/UniqueFd.h"
#include "ava/core/json.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include <unistd.h>

using namespace std::chrono_literals;
using ava::app::acp::JsonRpcId;
using namespace acp_test;

void test_acp_prompt_content_capabilities_and_strict_validation()
{
  using namespace ava::app::acp;
  auto valid = decode_prompt_content(
      R"([{"type":"text","text":"inspect"},{"type":"image","data":"iVBORw0KGgo=","mimeType":"image/png","uri":null},{"type":"resource_link","name":"docs","uri":"https://example.test/docs"}])");
  expect(valid && valid->images.size() == 1 && valid->images.front().bytes.size() == 8 && valid->text.find("not fetched") != std::string::npos,
         "ACP prompt decoder accepts bounded canonical image data and reference-only resource links");

  auto defaulted_optional = decode_prompt_content(
      R"([{"type":"text","text":"defaults","annotations":7,"_meta":[]},{"type":"resource_link","name":"docs","uri":"https://example.test","description":7,"mimeType":{},"title":[],"size":-2},{"type":"image","data":"iVBORw0KGgo=","mimeType":"image/png","uri":{}}])");
  expect(defaulted_optional && defaulted_optional->images.size() == 1 && defaulted_optional->text.find("defaults") != std::string::npos &&
             defaulted_optional->text.find("not fetched") != std::string::npos,
         "ACP prompt decoder defaults malformed optional content fields locally while preserving valid required siblings");

  for (auto const& prompt : {
           R"([{"type":"image","data":"iVBORw0KGgo", "mimeType":"image/png"}])",
           R"([{"type":"image","data":"iVBORw0KGgo=","mimeType":"image/svg+xml"}])",
           R"([{"type":"audio","data":"AAAA","mimeType":"audio/wav"}])",
           R"([{"type":"resource","resource":{"uri":"file:///secret","text":"x"}}])",
           R"([{"type":"future","value":"ignored?"}])",
           R"([{"type":"resource_link","name":"x","uri":7}])",
       })
  {
    auto rejected = decode_prompt_content(prompt);
    expect(!rejected && rejected.error().code == -32602,
           "ACP prompt decoder rejects malformed base64/MIME/discriminators and unsupported rich content with -32602");
  }

  auto blocks = [](std::size_t count, std::string const& block) {
    std::string json = "[";
    for (std::size_t index = 0; index < count; ++index)
    {
      if (index != 0)
        json += ',';
      json += block;
    }
    return json + ']';
  };
  auto image_block = [](std::size_t bytes) {
    return std::string(R"({"type":"image","mimeType":"image/png","data":")") + ava::provider::base64_encode(std::string(bytes, 'x')) + R"("})";
  };
  auto boundary = [](std::string const& accepted, std::string const& rejected, std::string const& name) {
    auto at_limit = decode_prompt_content(accepted);
    auto over_limit = decode_prompt_content(rejected);
    ava::tests::expect_all(
        "ACP prompt " + name + " boundary",
        {{"accepts exact limit", at_limit.has_value()}, {"rejects one over limit with invalid params", !over_limit && over_limit.error().code == -32602}});
  };
  boundary(blocks(kMaxAcpPromptBlocks, R"({"type":"text","text":"x"})"), blocks(kMaxAcpPromptBlocks + 1, R"({"type":"text","text":"x"})"), "block count");
  auto const text_prefix = R"([{"type":"text","text":")" + std::string(kMaxAcpPromptTextBytes / 2, 'x') + R"("},{"type":"text","text":")";
  boundary(text_prefix + std::string(kMaxAcpPromptTextBytes / 2 - 1, 'x') + R"("}])", text_prefix + std::string(kMaxAcpPromptTextBytes / 2, 'x') + R"("}])",
           "aggregate text bytes including the separator");
  boundary(blocks(kMaxAcpPromptImages, image_block(1)), blocks(kMaxAcpPromptImages + 1, image_block(1)), "image count");
  boundary("[" + image_block(kMaxAcpPromptImageBytes) + "]", "[" + image_block(kMaxAcpPromptImageBytes + 1) + "]", "per-image bytes");
  std::string aggregate = blocks(3, image_block(kMaxAcpPromptImageBytes));
  aggregate.pop_back();
  auto const remaining = kMaxAcpPromptImageTotalBytes - 3 * kMaxAcpPromptImageBytes;
  boundary(aggregate + "," + image_block(remaining) + "]", aggregate + "," + image_block(remaining + 1) + "]", "aggregate image bytes");

  auto image_capabilities = initialize_result_json("1", true);
  auto text_capabilities = initialize_result_json("1", false);
  expect(image_capabilities && image_capabilities->find(R"("image":true)") != std::string::npos && text_capabilities &&
             text_capabilities->find(R"("image":false)") != std::string::npos && image_capabilities->find(R"("loadSession":false)") != std::string::npos &&
             image_capabilities->find(R"("audio":false)") != std::string::npos && image_capabilities->find(R"("embeddedContext":false)") != std::string::npos,
         "ACP initialization derives image support while leaving exact rich-history loading unadvertised");
}

void test_acp_typed_session_update_mapper_ordering_and_limits()
{
  using namespace ava::app::acp;
  using namespace ava::event;
  auto root = std::filesystem::path("/workspace");
  RuntimeSessionUpdateMapper mapper(RuntimeSessionUpdateMapperOptions{.workspace_root = root, .message_id = "message_1"});
  auto message_update = [](std::string value) {
    MessagePayload payload;
    payload.text = std::move(value);
    return RuntimeEvent{{}, MessageUpdateEvent{.payload = std::move(payload)}};
  };
  auto reasoning_delta = [](std::string value) {
    ReasoningPayload payload;
    payload.text = std::move(value);
    return RuntimeEvent{{}, ReasoningDeltaEvent{.payload = std::move(payload)}};
  };
  auto text = message_update("hello");
  MessagePayload final_payload;
  final_payload.text = "hello";
  auto final = RuntimeEvent{{}, AssistantMessageEvent{.payload = std::move(final_payload)}};
  auto thought = reasoning_delta("considering");
  ToolPayload start_payload;
  start_payload.text = "src/a.cpp";
  start_payload.call_id = "call_1";
  start_payload.tool = "write_file";
  start_payload.args_json = R"({"path":"src/a.cpp"})";
  auto start = RuntimeEvent{{}, ToolStartEvent{.payload = std::move(start_payload)}};
  ToolPayload progress_payload;
  progress_payload.text = "writing";
  progress_payload.call_id = "call_1";
  progress_payload.tool = "write_file";
  progress_payload.status = "running";
  auto progress = RuntimeEvent{{}, ToolProgressEvent{.payload = std::move(progress_payload)}};
  ToolPayload result_payload;
  result_payload.call_id = "call_1";
  result_payload.tool = "write_file";
  result_payload.result_json = R"({"ok":true})";
  result_payload.status = "success";
  result_payload.changed_paths = {"src/a.cpp"};
  auto result = RuntimeEvent{{}, ToolResultEvent{.payload = std::move(result_payload)}};
  auto text_update = mapper.map_and_encode(text);
  auto duplicate = mapper.map_and_encode(final);
  auto thought_update = mapper.map_and_encode(thought);
  auto start_update = mapper.map_and_encode(start);
  auto progress_update = mapper.map_and_encode(progress);
  auto result_update = mapper.map_and_encode(result);
  auto has = [](auto const& update, std::string_view field) { return update && *update && (*update)->find(field) != std::string::npos; };
  ava::tests::expect_all("typed ACP mapper emits the requested text and thought content",
                         {{"text type", has(text_update, R"("sessionUpdate":"agent_message_chunk")")},
                          {"text content", has(text_update, R"("text":"hello")")},
                          {"thought type", has(thought_update, R"("sessionUpdate":"agent_thought_chunk")")},
                          {"thought content", has(thought_update, R"("text":"considering")")}});
  ava::tests::expect_all("typed ACP mapper retains one tool identity across its ordered lifecycle",
                         {{"start type", has(start_update, R"("sessionUpdate":"tool_call")")},
                          {"start id", has(start_update, R"("toolCallId":"call_1")")},
                          {"start kind", has(start_update, R"("kind":"edit")")},
                          {"start status", has(start_update, R"("status":"pending")")},
                          {"start location", has(start_update, "/workspace/src/a.cpp")},
                          {"progress type", has(progress_update, R"("sessionUpdate":"tool_call_update")")},
                          {"progress id", has(progress_update, R"("toolCallId":"call_1")")},
                          {"progress status", has(progress_update, R"("status":"in_progress")")},
                          {"result type", has(result_update, R"("sessionUpdate":"tool_call_update")")},
                          {"result id", has(result_update, R"("toolCallId":"call_1")")},
                          {"result status", has(result_update, R"("status":"completed")")}});
  expect(duplicate && !*duplicate, "typed ACP mapper suppresses a final message already emitted as a text delta");
  RuntimeSessionUpdateMapper ordered(RuntimeSessionUpdateMapperOptions{.workspace_root = root, .message_id = "ordered"});
  std::vector<std::string> ordered_updates;
  for (auto const* event : {&text, &thought, &start, &progress, &result})
  {
    auto batch = ordered.map_coalesced_and_encode(*event);
    if (!ava::tests::require(batch.has_value(), "ordered mapper accepts each text/thought/tool event"))
      return;
    ordered_updates.insert(ordered_updates.end(), std::make_move_iterator(batch->begin()), std::make_move_iterator(batch->end()));
  }
  auto ordered_tail = ordered.flush_coalesced();
  if (!ava::tests::require(ordered_tail.has_value(), "ordered mapper flushes its final pending content"))
    return;
  ordered_updates.insert(ordered_updates.end(), std::make_move_iterator(ordered_tail->begin()), std::make_move_iterator(ordered_tail->end()));
  std::vector<std::string> ordered_kinds;
  for (auto const& update : ordered_updates)
    ordered_kinds.push_back(ava::core::json::string_field(update, "sessionUpdate").value_or(""));
  expect(ordered_kinds == std::vector<std::string>({"agent_message_chunk", "agent_thought_chunk", "tool_call", "tool_call_update", "tool_call_update"}),
         "coalesced ACP mapper flushes text and thought before the tool lifecycle, in input order");

  RuntimeSessionUpdateMapper bounded(
      RuntimeSessionUpdateMapperOptions{.workspace_root = root, .message_id = "bounded", .max_updates = 2, .max_encoded_bytes = 4096});
  auto bounded_one = message_update("1");
  auto bounded_two = reasoning_delta("2");
  ToolPayload bounded_three_payload;
  bounded_three_payload.text = "3";
  bounded_three_payload.call_id = "call";
  bounded_three_payload.tool = "bash";
  auto bounded_three = RuntimeEvent{{}, ToolProgressEvent{.payload = std::move(bounded_three_payload)}};
  auto one = bounded.map_and_encode(bounded_one);
  auto two = bounded.map_and_encode(bounded_two);
  auto saturated = bounded.map_and_encode(bounded_three);
  expect(one && *one && two && *two && !saturated && saturated.error().message().find("budget") != std::string::npos,
         "ACP mapper fails closed when the per-prompt update budget saturates");

  RuntimeSessionUpdateMapper coalesced(
      RuntimeSessionUpdateMapperOptions{.workspace_root = root, .message_id = "coalesced", .max_updates = 8, .max_encoded_bytes = 64 * 1024});
  std::vector<std::string> coalesced_updates;
  bool coalesce_failed = false;
  for (std::size_t index = 0; index < 5'000; ++index)
  {
    auto delta = message_update("x");
    auto batch = coalesced.map_coalesced_and_encode(delta);
    if (!batch)
      coalesce_failed = true;
    else
      coalesced_updates.insert(coalesced_updates.end(), std::make_move_iterator(batch->begin()), std::make_move_iterator(batch->end()));
  }
  auto done = RuntimeEvent{{}, CompletionEvent{}};
  auto flushed = coalesced.map_coalesced_and_encode(done);
  if (flushed)
    coalesced_updates.insert(coalesced_updates.end(), std::make_move_iterator(flushed->begin()), std::make_move_iterator(flushed->end()));
  std::size_t coalesced_text_bytes = 0;
  for (auto const& update : coalesced_updates)
  {
    auto content = ava::core::json::object_field(update, "content");
    if (content)
      coalesced_text_bytes += ava::core::json::string_field(*content, "text").value_or("").size();
  }
  expect(!coalesce_failed && flushed && coalesced_updates.size() == 5 && coalesced_text_bytes == 5'000 && coalesced.update_count() == 5,
         "ACP mapper coalesces provider-fragmented adjacent text into bounded live chunks before applying update-count and byte budgets");

  RuntimeSessionUpdateMapper unicode_mapper(
      RuntimeSessionUpdateMapperOptions{.workspace_root = root, .message_id = "unicode", .max_updates = 4, .max_encoded_bytes = 16 * 1024});
  auto unicode_text = std::string(kMaxStreamContentChunkBytes - 1, 'a') + "€x";
  auto unicode_delta = message_update(unicode_text);
  auto unicode_batch = unicode_mapper.map_coalesced_and_encode(unicode_delta);
  auto unicode_flush = unicode_mapper.flush_coalesced();
  std::string reconstructed;
  std::size_t unicode_updates = 0;
  if (unicode_batch && unicode_flush)
  {
    unicode_batch->insert(unicode_batch->end(), std::make_move_iterator(unicode_flush->begin()), std::make_move_iterator(unicode_flush->end()));
    unicode_updates = unicode_batch->size();
    for (auto const& update : *unicode_batch)
      if (auto content = ava::core::json::object_field(update, "content"))
        reconstructed += ava::core::json::string_field(*content, "text").value_or("");
  }
  expect(unicode_batch && unicode_flush && unicode_updates == 2 && reconstructed == unicode_text,
         "ACP stream coalescing preserves a multibyte UTF-8 code point that crosses the byte chunk boundary");
}

void test_acp_permission_dtos_show_bounded_actions()
{
  using namespace ava::app::acp;
  ava::permissions::PermissionPrompt prompt;
  prompt.permission_request_id = "perm_1";
  prompt.tool_call_id = "call_1";
  prompt.operation = ava::permissions::Operation::RunCommand;
  prompt.mode = ava::agent::Mode::Build;
  prompt.workspace_dir = "/workspace";
  prompt.target_path = "/workspace";
  prompt.command = "secret-command --token value";
  prompt.tool_name = "bash";
  prompt.reason = "command approval";
  prompt.risk = ava::permissions::PermissionRisk::High;
  auto params = encode_permission_request_params("session_1", prompt, "/workspace");
  expect(params && params->find(R"("sessionId":"session_1")") != std::string::npos && params->find(R"("toolCallId":"call_1")") != std::string::npos &&
             params->find(R"("kind":"execute")") != std::string::npos && params->find(R"("optionId":"allow_once")") != std::string::npos &&
             params->find("secret-command --token value") != std::string::npos && params->find("exact request for this session") != std::string::npos,
         "ACP permission request links identity and exposes the bounded exact command before offering a session grant");

  prompt.command.assign(7U * 1024U, 'x');
  auto truncated_command = encode_permission_request_params("session_1", prompt, "/workspace");
  expect(truncated_command && truncated_command->find("command display truncated") != std::string::npos &&
             truncated_command->find(R"("optionId":"allow_once")") != std::string::npos &&
             truncated_command->find(R"("optionId":"allow_always")") == std::string::npos &&
             truncated_command->find(R"("optionId":"reject_always")") == std::string::npos && !permission_request_offers_session_decisions(prompt),
         "ACP hides session-wide decisions when a command cannot be displayed completely within the bounded permission request");

  prompt.operation = ava::permissions::Operation::EditFile;
  prompt.tool_name = "edit_file";
  prompt.command.clear();
  prompt.target_path = "/workspace/src/main.cpp";
  prompt.diff_preview = "--- a/src/main.cpp\n+++ b/src/main.cpp\n@@\n-old\n+new";
  auto edit_params = encode_permission_request_params("session_1", prompt, "/workspace");
  expect(edit_params && edit_params->find("Proposed file diff") != std::string::npos && edit_params->find("+new") != std::string::npos &&
             edit_params->find(R"("optionId":"allow_once")") != std::string::npos && edit_params->find(R"("optionId":"reject_once")") != std::string::npos &&
             edit_params->find(R"("optionId":"allow_always")") == std::string::npos && edit_params->find(R"("optionId":"reject_always")") == std::string::npos,
         "ACP file-mutation permission shows a bounded diff and offers only one-shot decisions");

  auto selected = decode_permission_response(R"({"outcome":{"outcome":"selected","optionId":"allow_always"}})");
  auto cancelled = decode_permission_response(R"({"outcome":{"outcome":"cancelled"}})");
  expect(selected && *selected == AcpPermissionSelection::AllowAlways && cancelled && *cancelled == AcpPermissionSelection::Cancelled,
         "ACP permission response DTO strictly parses pinned Selected and Cancelled outcomes");
  auto selected_with_invalid_response_meta = decode_permission_response(R"({"_meta":[],"outcome":{"outcome":"selected","optionId":"allow_once"}})");
  auto selected_with_invalid_outcome_meta = decode_permission_response(R"({"outcome":{"outcome":"selected","optionId":"reject_once","_meta":"invalid"}})");
  expect(selected_with_invalid_response_meta && *selected_with_invalid_response_meta == AcpPermissionSelection::AllowOnce &&
             selected_with_invalid_outcome_meta && *selected_with_invalid_outcome_meta == AcpPermissionSelection::RejectOnce,
         "ACP permission responses default malformed optional _meta fields locally as required by the pinned schema");
  for (auto const& malformed : {R"({"outcome":{"outcome":"selected","optionId":"wildcard"}})", R"({"outcome":{"outcome":"cancelled","optionId":"allow_once"}})",
                                R"({"outcome":"selected"})", R"({"outcome":{"outcome":"future"}})"})
  {
    auto rejected = decode_permission_response(malformed);
    expect(!rejected && rejected.error().code == -32602, "ACP permission response parser rejects invalid outcomes and unoffered option ids");
  }
}

void test_acp_codec_envelopes_and_ids()
{
  using namespace ava::app::acp;
  auto integer = decode_message(R"({"jsonrpc":"2.0","id":7,"method":"x","params":{}})");
  auto string = decode_message(R"({"jsonrpc":"2.0","id":"7","method":"x","params":[]})");
  expect(integer && std::holds_alternative<Request>(*integer) && std::get<std::int64_t>(std::get<Request>(*integer).id) == 7,
         "ACP codec preserves integer request ids");
  expect(string && std::holds_alternative<Request>(*string) && std::get<std::string>(std::get<Request>(*string).id) == "7",
         "ACP codec preserves string request ids");

  auto null_request = decode_message(R"({"jsonrpc":"2.0","id":null,"method":"x"})");
  expect(null_request && std::holds_alternative<Request>(*null_request) && std::holds_alternative<NullJsonRpcId>(std::get<Request>(*null_request).id),
         "ACP codec preserves a present null request id instead of treating it as a notification");
  auto null_params = decode_message(R"({"jsonrpc":"2.0","id":8,"method":"session/list","params":null})");
  expect(null_params && std::holds_alternative<Request>(*null_params) && !std::get<Request>(*null_params).params_json,
         "ACP codec accepts params:null and normalizes it to absent parameters");
  auto notification = decode_message(R"({"jsonrpc":"2.0","method":"session/cancel","params":{"sessionId":"s"}})");
  auto response = decode_message(R"({"jsonrpc":"2.0","id":7,"result":{"ok":true}})");
  auto error = decode_message(R"({"jsonrpc":"2.0","id":"7","error":{"code":-32800,"message":"cancelled","data":{"safe":true}}})");
  expect(notification && std::holds_alternative<Notification>(*notification), "ACP codec routes notifications");
  expect(response && std::holds_alternative<Response>(*response), "ACP codec routes result responses");
  expect(error && std::holds_alternative<ErrorResponse>(*error) && std::get<ErrorResponse>(*error).error.data_json.has_value(),
         "ACP codec routes typed error responses and preserves data");

  auto null_success = encode_success(JsonRpcId(NullJsonRpcId{}), R"({"ok":true})");
  auto null_error = encode_error(JsonRpcId(NullJsonRpcId{}), -32603, "failed");
  expect(null_success && null_success->find("\"id\":null") != std::string::npos && null_error && null_error->find("\"id\":null") != std::string::npos,
         "ACP success and error serialization preserve explicit null ids");

  for (auto const& bad :
       {R"({"jsonrpc":"2.0","id":1.5,"method":"x"})", R"({"jsonrpc":"2.0","id":true,"method":"x"})", R"({"jsonrpc":"2.0","id":{},"method":"x"})"})
  {
    auto decoded = decode_message(bad);
    expect(!decoded && decoded.error().code == -32600, "ACP codec rejects unsupported JSON-RPC id types");
  }
  auto wrong_version = decode_message(R"({"jsonrpc":"1.0","id":1,"method":"x"})");
  auto wrong_params = decode_message(R"({"jsonrpc":"2.0","id":1,"method":"x","params":"bad"})");
  auto exclusive = decode_message(R"({"jsonrpc":"2.0","id":1,"result":{},"error":{"code":1,"message":"x"}})");
  auto id_only = decode_message(R"({"jsonrpc":"2.0","id":1})");
  auto null_id_only = decode_message(R"({"jsonrpc":"2.0","id":null})");
  auto malformed_error = decode_message(R"({"jsonrpc":"2.0","id":1,"error":{"code":"bad","message":1}})");
  auto batch = decode_message(R"([{"jsonrpc":"2.0","id":1,"method":"x"}])");
  expect(!wrong_version && wrong_version.error().code == -32600, "ACP codec requires jsonrpc 2.0");
  expect(!wrong_params && wrong_params.error().code == -32600, "ACP codec requires structured params");
  expect(!exclusive && exclusive.error().code == -32600 && exclusive.error().intent == EnvelopeIntent::Response && exclusive.error().suppress_response,
         "ACP codec classifies malformed response envelopes before reply policy");
  expect(!id_only && id_only.error().intent == EnvelopeIntent::Response && id_only.error().suppress_response && !null_id_only &&
             null_id_only.error().intent == EnvelopeIntent::Response && null_id_only.error().suppress_response && !malformed_error &&
             malformed_error.error().intent == EnvelopeIntent::Response && malformed_error.error().suppress_response,
         "ACP codec treats every id-bearing methodless object as response intent, including null ids and malformed response bodies");
  expect(!batch && batch.error().code == -32600, "ACP codec explicitly rejects batches");

  for (auto const& duplicate : {R"({"jsonrpc":"2.0","id":1,"id":2,"method":"x"})", R"({"jsonrpc":"2.0","id":1,"method":"x","\u006dethod":"y"})",
                                R"({"jsonrpc":"2.0","id":1,"method":"x","params":{"future":1,"future":2}})"})
  {
    auto rejected = decode_message(duplicate);
    expect(!rejected && rejected.error().code == -32600, "ACP rejects duplicate member names, including escaped-equivalent and nested keys");
  }
  auto additive = decode_message(R"({"jsonrpc":"2.0","id":1,"method":"x","future":{"unique":true}})");
  expect(additive && std::holds_alternative<Request>(*additive), "ACP continues accepting unique additive fields");

  auto escaped_response = scan_envelope_intent(R"({"payload":"escaped quote: \" and fake braces: {[","i\u0064":null,"result":[{"method":"nested"}]})");
  auto trailing_request = scan_envelope_intent(R"({"params":{"nested":[{"id":99,"method":"nested"}]},"m\u0065thod":"top-level","id":7})");
  auto crossed_response = scan_envelope_intent(R"({"id":8,"result":[{"x":0],"method":"not-reliably-top-level"})");
  expect(escaped_response.intent == EnvelopeIntent::Response && trailing_request.intent == EnvelopeIntent::Request &&
             loop_safe_oversized_intent(crossed_response) == EnvelopeIntent::Response,
         "streaming envelope scan handles escaped keys and strings, ignores nested envelope names, and stays loop-safe on mismatched nesting");
}

void test_acp_codec_initialize_meta_additive_and_malformed_fields()
{
  using namespace ava::app::acp;
  auto decoded = decode_message(
      R"({"jsonrpc":"2.0","id":"init","method":"initialize","params":{"protocolVersion":1,"clientCapabilities":{"fs":{"readTextFile":true,"future":1,"_meta":{"fs":1}},"terminal":true,"session":{"configOptions":{"boolean":{"_meta":{"boolean":1}},"_meta":{"config":1}},"_meta":{"session":1}},"_meta":{"cap":1}},"clientInfo":{"name":"client","version":"2","title":"Client","_meta":{"i":2}},"_meta":{"root":3},"futureField":{"anything":true}}})");
  expect(decoded && std::holds_alternative<Request>(*decoded), "ACP initialize envelope parses");
  if (!decoded || !std::holds_alternative<Request>(*decoded))
    return;
  auto initialize = decode_initialize_params(std::get<Request>(*decoded));
  expect(initialize && initialize->protocol_version == 1 && initialize->client_capabilities.read_text_file && initialize->client_capabilities.terminal &&
             initialize->client_capabilities.boolean_config_options && initialize->client_info && initialize->meta_json && initialize->client_info->meta_json &&
             initialize->client_capabilities.meta_json && initialize->client_capabilities.fs_meta_json && initialize->client_capabilities.session_meta_json &&
             initialize->client_capabilities.config_options_meta_json && initialize->client_capabilities.boolean_config_meta_json,
         "ACP initialize accepts additive fields and preserves all recognized _meta objects");

  for (auto const& params : {R"({})", R"({"protocolVersion":"1"})", R"({"protocolVersion":-1})", R"({"protocolVersion":65536})"})
  {
    Request request{.id = std::int64_t(1), .method = "initialize", .params_json = std::string(params)};
    auto malformed = decode_initialize_params(request);
    expect(!malformed && malformed.error().code == -32602, "ACP initialize keeps required protocolVersion strict");
  }

  auto decode_params = [](std::string params) {
    return decode_initialize_params(Request{.id = std::int64_t(1), .method = "initialize", .params_json = std::move(params)});
  };
  auto malformed_capabilities = decode_params(R"({"protocolVersion":1,"clientCapabilities":[]})");
  expect(malformed_capabilities && !malformed_capabilities->client_capabilities.read_text_file &&
             !malformed_capabilities->client_capabilities.write_text_file && !malformed_capabilities->client_capabilities.terminal &&
             !malformed_capabilities->client_capabilities.boolean_config_options,
         "malformed clientCapabilities defaults to the all-false capability object");

  auto field_local = decode_params(
      R"({"protocolVersion":1,"clientCapabilities":{"fs":{"readTextFile":true,"writeTextFile":"bad","_meta":[]},"terminal":"bad","session":{"configOptions":{"boolean":[],"_meta":[]},"_meta":[]},"_meta":[]},"clientInfo":{"name":"client","version":"2","title":7,"_meta":[]},"_meta":[]})");
  expect(field_local && field_local->client_capabilities.read_text_file && !field_local->client_capabilities.write_text_file &&
             !field_local->client_capabilities.terminal && !field_local->client_capabilities.boolean_config_options &&
             !field_local->client_capabilities.meta_json && !field_local->client_capabilities.fs_meta_json &&
             !field_local->client_capabilities.session_meta_json && !field_local->client_capabilities.config_options_meta_json && field_local->client_info &&
             field_local->client_info->name == "client" && field_local->client_info->version == "2" && !field_local->client_info->title &&
             !field_local->client_info->meta_json && !field_local->meta_json,
         "initialize defaults malformed optional fields locally while preserving valid required and sibling fields");

  auto malformed_fs = decode_params(R"({"protocolVersion":1,"clientCapabilities":{"fs":[],"terminal":true,"session":{"configOptions":{"boolean":{}}}}})");
  expect(malformed_fs && !malformed_fs->client_capabilities.read_text_file && !malformed_fs->client_capabilities.write_text_file &&
             malformed_fs->client_capabilities.terminal && malformed_fs->client_capabilities.boolean_config_options,
         "malformed fs defaults both filesystem flags without discarding valid terminal or session siblings");

  auto write_sibling = decode_params(
      R"({"protocolVersion":1,"clientCapabilities":{"fs":{"readTextFile":{},"writeTextFile":true},"session":[]},"clientInfo":{"name":"x","version":2}})");
  expect(write_sibling && !write_sibling->client_capabilities.read_text_file && write_sibling->client_capabilities.write_text_file &&
             !write_sibling->client_capabilities.boolean_config_options && !write_sibling->client_info,
         "malformed individual filesystem booleans and optional capability/clientInfo objects default absent without erasing valid siblings");

  auto malformed_config = decode_params(R"({"protocolVersion":1,"clientCapabilities":{"session":{"configOptions":[]}},"clientInfo":"bad"})");
  expect(malformed_config && !malformed_config->client_capabilities.boolean_config_options && !malformed_config->client_info,
         "malformed optional config capabilities and clientInfo default absent");

  auto result = encode_initialize_result(std::string("id"), "1.0.0", true);
  expect(result && result->find("\"protocolVersion\":1") != std::string::npos,
         "ACP initialize result codec remains testable independently of the M2 public conformance gate");
}

void test_acp_codec_utf8_depth_size_and_serialization()
{
  using namespace ava::app::acp;
  std::string invalid = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"";
  invalid.push_back(static_cast<char>(0xFF));
  invalid += "\"}";
  auto invalid_utf8 = decode_message(invalid);
  expect(!invalid_utf8 && invalid_utf8.error().code == -32700, "invalid inbound UTF-8 produces a parse error");
  if (!ava::tests::require(!invalid_utf8, "invalid UTF-8 fixture produces a decoder error"))
    return;
  auto encoded_error = encode_error(std::nullopt, invalid_utf8.error().code, invalid_utf8.error().message);
  expect(encoded_error && ava::core::json::is_valid_utf8(*encoded_error), "ACP serializes the decoder's invalid UTF-8 error as valid UTF-8");
  auto hostile_error = encode_error(std::nullopt, -32700, std::string("invalid byte: ") + static_cast<char>(0xFF));
  expect(hostile_error && ava::core::json::is_valid_utf8(*hostile_error), "ACP error serialization sanitizes invalid bytes in an error message");

  std::string deep = R"({"jsonrpc":"2.0","id":1,"method":"x","params":)";
  deep.append(kMaxNestingDepth + 1, '[');
  deep.append(kMaxNestingDepth + 1, ']');
  deep += '}';
  auto too_deep = decode_message(deep);
  expect(!too_deep && too_deep.error().code == -32700, "ACP codec bounds nesting depth");

  std::string deep_value(kMaxNestingDepth + 1, '[');
  deep_value += '0';
  deep_value.append(kMaxNestingDepth + 1, ']');
  auto deep_integer_response = decode_message(R"({"jsonrpc":"2.0","id":41,"result":)" + deep_value + '}');
  auto deep_null_response = decode_message(R"({"jsonrpc":"2.0","result":)" + deep_value + R"(,"id":null})");
  expect(!deep_integer_response && deep_integer_response.error().intent == EnvelopeIntent::Response && deep_integer_response.error().suppress_response &&
             !deep_null_response && deep_null_response.error().intent == EnvelopeIntent::Response && deep_null_response.error().suppress_response,
         "ACP codec establishes integer and null response intent before depth rejection, including an id after the deep value");

  auto too_large = decode_message(std::string(kMaxRecordBytes + 1, ' '));
  expect(!too_large && too_large.error().code == -32700, "ACP codec bounds record bytes before parsing");

  std::string long_method(kMaxMethodBytes + 1, 'x');
  auto long_record = encode_request(std::int64_t(1), long_method, std::nullopt);
  expect(!long_record, "ACP serialization bounds method length");
  auto malformed_result = encode_success(std::int64_t(1), "{bad");
  expect(!malformed_result, "ACP serialization rejects malformed raw JSON at the codec boundary");
  std::string oversized_string = R"({"jsonrpc":"2.0","id":1,"method":"x","params":{"value":")";
  oversized_string.append(kMaxStringBytes + 1, 'x');
  oversized_string += R"("}})";
  auto too_long = decode_message(oversized_string);
  expect(!too_long && too_long.error().code == -32700, "ACP codec bounds individual JSON strings");

  std::string collection = R"({"jsonrpc":"2.0","id":1,"method":"x","params":[)";
  for (std::size_t index = 0; index <= kMaxCollectionItems; ++index)
  {
    if (index != 0)
      collection += ',';
    collection += "0";
  }
  collection += "]}";
  auto too_many = decode_message(collection);
  expect(!too_many && too_many.error().code == -32700, "ACP codec bounds JSON collection sizes");
}

void test_acp_transport_lf_crlf_and_final_record()
{
  using namespace ava::app::acp;
  int input[2] = {-1, -1};
  int output[2] = {-1, -1};
  bool const opened = pipe(input) == 0 && pipe(output) == 0;
  ava::core::UniqueFd input_read(input[0]);
  ava::core::UniqueFd input_write(input[1]);
  ava::core::UniqueFd output_read(output[0]);
  ava::core::UniqueFd output_write(output[1]);
  if (!ava::tests::require(opened, "ACP framing test creates transport pipes"))
    return;
  auto transport = make_fd_record_transport(input[0], output[1]);
  if (!ava::tests::require(transport.has_value(), "ACP framing test creates fd transport"))
    return;
  std::string const records = "{}\r\n[]\n{\"final\":true}";
  auto const count = write(input[1], records.data(), records.size());
  input_write.reset();
  auto crlf = (*transport)->read_record();
  auto lf = (*transport)->read_record();
  auto final = (*transport)->read_record();
  auto eof = (*transport)->read_record();
  ava::tests::expect_all("ACP transport accepts CRLF, LF, and a bounded unterminated final record",
                         {{"fixture bytes written", count == static_cast<ssize_t>(records.size())},
                          {"CRLF status", crlf.status == ReadRecordStatus::Record},
                          {"CRLF payload", crlf.record == "{}"},
                          {"LF status", lf.status == ReadRecordStatus::Record},
                          {"LF payload", lf.record == "[]"},
                          {"final status", final.status == ReadRecordStatus::Record},
                          {"final payload", final.record == R"({"final":true})"},
                          {"EOF after final record", eof.status == ReadRecordStatus::EndOfFile}});
  transport->reset();
}
