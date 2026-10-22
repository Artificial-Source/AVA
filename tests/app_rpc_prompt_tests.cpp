#include "sys.h"
#include "tests/app_rpc_test_cases.h"
#include "tests/support/app_rpc_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/app/rpc/session_operators.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/config/auth.h"
#include "ava/config/model_config.h"
#include "ava/session/record.h"
#include "ava/provider/catalog.h"
#include "ava/provider/openai_compatible_provider.h"
#include "ava/provider/openai_provider.h"
#include "ava/provider/provider_utils.h"
#include "ava/core/json.h"
#include "ava/core/result.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <istream>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <sys/stat.h>

namespace ava::tests::app_rpc_test {

void test_app_rpc_prompt_with_fake_transport_streams_events()
{
  auto const root = create_empty_root("app-rpc-prompt");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC prompt test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"rpc answer\"}\n\n"
              "data: [DONE]\n\n",
  }});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"hello rpc\"}\n");
  bool const completed = output_buffer.wait_contains("rpc answer", std::chrono::seconds(2));
  bool const terminal = wait_for_rpc_response(output_buffer, "p1", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();
  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC prompt loop completes successfully");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("hello rpc") != std::string::npos,
         "RPC prompt sends command message through shared runtime");
  auto const response = rpc_response_line(jsonl, "p1").value_or("");
  expect_all("RPC prompt streams runtime envelopes and ends with its successful response",
             {{"session start", jsonl.find("\"name\":\"session_start\"") != std::string::npos},
              {"assistant event", jsonl.find("\"name\":\"assistant_message\"") != std::string::npos},
              {"correlation", jsonl.find("\"request_id\":\"p1\"") != std::string::npos},
              {"streamed answer", completed},
              {"terminal response", terminal},
              {"success", response.find("\"success\":true") != std::string_view::npos},
              {"final answer", response.find("rpc answer") != std::string_view::npos}});
}

void test_app_rpc_offline_allows_local_protocol_and_rejects_prompt_before_provider_request()
{
  auto const root = create_empty_root("app-rpc-offline");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  open_context.offline = true;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC offline test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.offline = true;
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"proto\",\"type\":\"get_protocol\"}\n");
  bool const protocol_completed = wait_for_rpc_response(output_buffer, "proto", std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"p-offline\",\"type\":\"prompt\",\"message\":\"hello offline rpc\"}\n");
  bool const prompt_failed = wait_for_rpc_response(output_buffer, "p-offline", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC offline loop completes successfully after prompt rejection");
  auto const protocol = rpc_response_line(jsonl, "proto").value_or("");
  auto const rejected = rpc_response_line(jsonl, "p-offline").value_or("");
  expect(protocol_completed && protocol.find("\"success\":true") != std::string_view::npos, "RPC offline mode still serves local protocol commands");
  expect_all("RPC offline mode rejects the prompt with a machine-readable error",
             {{"terminal response", prompt_failed},
              {"failed", rejected.find("\"success\":false") != std::string_view::npos},
              {"permission code", rejected.find("\"code\":\"permission_denied\"") != std::string_view::npos}});
  expect(transport.requests().empty(), "RPC offline prompt rejection avoids provider transport requests");
}

void test_app_rpc_prompt_imports_image_attachments()
{
  auto const root = create_empty_root("app-rpc-prompt-image-attachment");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto const image_path = workspace / "screen.png";
  write_test_file(image_path, session_test_tiny_png_bytes());

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC image prompt test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"rpc image answer\"}\n\n"
              "data: [DONE]\n\n",
  }});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"p-img\",\"type\":\"prompt\",\"message\":\"describe\",\"attachments\":[\"" + ava::core::json::escape(image_path.string()) +
                    "\"]}\n");
  bool const completed = wait_for_rpc_response(output_buffer, "p-img", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();
  auto const jsonl = output_buffer.str();
  auto entries = ava::app::runtime::session_ts::rat(unlocked_session)->store.load();
  expect(result.has_value(), "RPC image prompt loop completes successfully");
  auto const response = rpc_response_line(jsonl, "p-img").value_or("");
  expect(completed && response.find("\"success\":true") != std::string_view::npos, "RPC image prompt returns its successful prompt response");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("\"type\":\"input_image\"") != std::string::npos &&
             transport.requests()[0].body.find("data:image/png;base64,") != std::string::npos,
         "RPC image prompt imports local image paths into provider image payloads");
  auto const persisted_metadata = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                                    return entry.type == ava::session::EntryType::UserMessage && entry.data_json.find("\"attachments\"") != std::string::npos &&
                                           entry.data_json.find("\"storage_path\":\"attachments/") != std::string::npos &&
                                           entry.data_json.find("data_base64") == std::string::npos;
                                  });
  expect(persisted_metadata, "RPC image prompt persists session-owned attachment metadata without raw image bytes");
}

void test_app_rpc_prompt_imports_inline_image_uploads()
{
  auto const root = create_empty_root("app-rpc-prompt-inline-image-upload");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC inline image upload prompt test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"rpc upload answer\"}\n\n"
              "data: [DONE]\n\n",
  }});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"p-upload\",\"type\":\"prompt\",\"message\":\"describe\",\"images\":[{\"type\":\"image\",\"data\":\"" +
                    ava::provider::base64_encode(session_test_tiny_png_bytes()) + "\",\"mimeType\":\"image/png\"}]}\n");
  bool const completed = wait_for_rpc_response(output_buffer, "p-upload", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();
  auto const jsonl = output_buffer.str();
  auto entries = ava::app::runtime::session_ts::rat(unlocked_session)->store.load();
  expect(result.has_value(), "RPC inline image upload prompt loop completes successfully");
  auto const response = rpc_response_line(jsonl, "p-upload").value_or("");
  expect(completed && response.find("\"success\":true") != std::string_view::npos, "RPC inline image upload returns its successful prompt response");
  expect(transport.requests().size() == 1 && transport.requests()[0].body.find("\"type\":\"input_image\"") != std::string::npos &&
             transport.requests()[0].body.find("data:image/png;base64,") != std::string::npos,
         "RPC inline image uploads are imported into provider image payloads");
  auto const persisted_metadata = entries && std::ranges::any_of(*entries, [](ava::session::SessionEntry const& entry) {
                                    return entry.type == ava::session::EntryType::UserMessage && entry.data_json.find("\"attachments\"") != std::string::npos &&
                                           entry.data_json.find("\"mime_type\":\"image/png\"") != std::string::npos &&
                                           entry.data_json.find("\"storage_path\":\"attachments/") != std::string::npos &&
                                           entry.data_json.find("data_base64") == std::string::npos;
                                  });
  expect(persisted_metadata, "RPC inline image upload persists metadata without raw image bytes");
}

void test_app_rpc_prompt_rejects_inline_image_upload_mime_mismatch()
{
  auto const root = create_empty_root("app-rpc-prompt-inline-image-upload-mismatch");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC inline image MIME mismatch test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  std::istringstream in("{\"id\":\"p-upload-bad\",\"type\":\"prompt\",\"message\":\"describe\",\"images\":[{\"type\":\"image\",\"data\":\"" +
                        ava::provider::base64_encode(session_test_tiny_png_bytes()) + "\",\"mimeType\":\"image/jpeg\"}]}\n");
  std::ostringstream out;

  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, ava::app::rpc::RpcInputWake{});
  auto const jsonl = out.str();
  expect(result.has_value(), "RPC inline image MIME mismatch loop completes after error response");
  expect(jsonl.find("\"id\":\"p-upload-bad\"") != std::string::npos && jsonl.find("\"success\":false") != std::string::npos &&
             jsonl.find("image attachment MIME type does not match detected bytes") != std::string::npos,
         "RPC inline image upload MIME mismatches are machine-readable error responses");
  expect(transport.requests().empty(), "RPC inline image MIME mismatch avoids dispatching a provider request");
}

void test_app_rpc_prompt_streams_provider_deltas_before_final_response()
{
  auto const root = create_empty_root("app-rpc-prompt-streaming");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC streaming prompt test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ChunkedStreamingTransport transport({"data: {\"type\":\"response.output_text.delta\",\"delta\":\"rpc \"}\n\n",
                                       "data: {\"type\":\"response.output_text.delta\",\"delta\":\"stream\"}\n\n", "data: [DONE]\n\n"});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"hello rpc stream\"}\n");
  bool const completed = output_buffer.wait_contains("\"success\":true", std::chrono::seconds(2));
  expect(wait_for_rpc_response(output_buffer, "p1", std::chrono::seconds(2)), "RPC streaming prompt reaches its complete terminal response before EOF");
  input_buffer.close();
  rpc_thread.join();
  auto const jsonl = output_buffer.str();
  auto const update_position = jsonl.find("\"name\":\"message_update\"");
  auto const final_position = jsonl.find("\"name\":\"assistant_message\"");
  auto const response_position = jsonl.find("\"type\":\"response\"");
  expect(result.has_value(), "RPC streaming prompt loop completes successfully");
  auto const response = rpc_response_line(jsonl, "p1").value_or("");
  expect(response.find("\"success\":true") != std::string_view::npos, "RPC streamed prompt finishes with its successful terminal response");
  expect(update_position != std::string::npos && final_position != std::string::npos && completed && response_position != std::string::npos &&
             update_position < final_position && final_position < response_position && jsonl.find("rpc stream") != std::string::npos,
         "RPC prompt emits live provider deltas before final assistant event and command response");
}

void test_app_rpc_prompt_retry_transport_cancellation_is_canceled_event()
{
  auto const root = create_empty_root("app-rpc-prompt-retry-canceled");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC retry-cancel prompt test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "{\"error\":{\"message\":\"retry later\"}}"},
                                       ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "{\"error\":{\"message\":\"retry later\"}}"},
                                       ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "{\"error\":{\"message\":\"retry later\"}}"}});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  runtime_options.enable_transport_retries = true;

  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"p-cancel\",\"type\":\"prompt\",\"message\":\"cancel during retry\"}\n");
  bool const retry_started = output_buffer.wait_contains("\"name\":\"retry\"", std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"cancel\",\"type\":\"cancel\"}\n");
  bool const completed = wait_for_rpc_response(output_buffer, "p-cancel", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();
  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC retry-cancel prompt loop returns control after canceled run");
  expect(retry_started, "RPC retry-cancel prompt reaches retry backoff before cancellation");
  expect(completed, "RPC retry-cancel prompt produces a response before input closes");
  expect(!transport.requests().empty() && transport.requests().size() <= 2,
         "RPC retry-cancel prompt dispatches a bounded number of provider requests before cancellation");
  expect(jsonl.find("\"name\":\"canceled\"") != std::string::npos && jsonl.find("\"payload_type\":\"cancellation\"") != std::string::npos &&
             jsonl.find("\"name\":\"error\"") == std::string::npos,
         "RPC retry transport cancellation emits terminal canceled envelope instead of an error envelope");
}

void test_app_rpc_prompt_after_idle_cancel_clears_cancel_flag()
{
  auto const root = create_empty_root("app-rpc-prompt-after-idle-cancel");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC idle-cancel prompt test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"after cancel\"}\n\n"
              "data: [DONE]\n\n",
  }});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"cancel-idle\",\"type\":\"cancel\"}\n");
  bool const canceled = wait_for_rpc_response(output_buffer, "cancel-idle", std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"p-after\",\"type\":\"prompt\",\"message\":\"run after idle cancel\"}\n");
  bool const completed = wait_for_rpc_response(output_buffer, "p-after", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();
  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC idle-cancel prompt loop completes successfully");
  expect(canceled, "RPC idle cancel writes a response");
  auto const response = rpc_response_line(jsonl, "p-after").value_or("");
  expect_all("RPC prompt after idle cancel clears the latched flag and runs normally",
             {{"completed", completed},
              {"provider request", transport.requests().size() == 1},
              {"success", response.find("\"success\":true") != std::string_view::npos},
              {"answer", response.find("after cancel") != std::string_view::npos}});
}

void test_app_rpc_prompt_refreshes_expired_oauth_before_provider_request()
{
  auto const root = create_empty_root("app-rpc-oauth-refresh");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  auto stored = ava::config::store_openai_credential(paths, ava::config::OpenAICredential{.type = ava::config::OpenAICredentialType::OAuth,
                                                                                          .access_token = "expired-rpc-access",
                                                                                          .refresh_token = "rpc-refresh",
                                                                                          .expires_at = 100,
                                                                                          .account_id = "acct_old",
                                                                                          .source_path = {}});
  expect(stored.has_value(), "RPC OAuth refresh test stores expired credential");

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC OAuth refresh test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
                                           .status_code = 200,
                                           .headers = {},
                                           .body = "{\"access_token\":\"rpc-refreshed-access\","
                                                   "\"refresh_token\":\"rpc-rotated-refresh\","
                                                   "\"expires_in\":3600,\"account_id\":\"acct_rpc\"}",
                                       },
                                       ava::http::HttpResponse{
                                           .status_code = 200,
                                           .headers = {},
                                           .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":"
                                                   "\"rpc refreshed answer\"}\n\n"
                                                   "data: [DONE]\n\n",
                                       }});
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out,
                                    [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"hello refreshed rpc\"}\n");
  bool const completed = output_buffer.wait_contains("\"success\":true", std::chrono::seconds(2));
  expect(wait_for_rpc_response(output_buffer, "p1", std::chrono::seconds(2)), "RPC OAuth prompt reaches its complete terminal response before EOF");
  input_buffer.close();
  rpc_thread.join();
  auto const jsonl = output_buffer.str();
  expect(result.has_value(), "RPC prompt with expired OAuth completes after refresh");
  auto const response = rpc_response_line(jsonl, "p1").value_or("");
  expect_all(
      "RPC OAuth prompt returns its correlated successful terminal answer",
      {{"success", response.find("\"success\":true") != std::string_view::npos}, {"answer", response.find("rpc refreshed answer") != std::string_view::npos}});
  expect(transport.requests().size() == 2 && transport.requests()[0].url == "https://auth.openai.com/oauth/token" &&
             transport.requests()[1].headers.at("Authorization") == "Bearer rpc-refreshed-access" &&
             transport.requests()[1].headers.at("ChatGPT-Account-Id") == "acct_rpc",
         "RPC prompt refreshes OAuth before sending provider request");
  expect(completed && jsonl.find("rpc refreshed answer") != std::string::npos && jsonl.find("\"success\":true") != std::string::npos,
         "RPC prompt returns refreshed OAuth provider response");
  auto persisted = ava::config::load_openai_credential(paths);
  expect(persisted && persisted->has_value() && (*persisted)->access_token == "rpc-refreshed-access" && (*persisted)->refresh_token == "rpc-rotated-refresh",
         "RPC OAuth preflight persists refreshed credential before provider startup");
}

void test_app_rpc_prompt_uses_pinned_catalog_credential_policy()
{
  auto const root = create_empty_root("app-rpc-catalog-creds");
  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);

  {
    std::ofstream file(paths.providers_file, std::ios::binary | std::ios::trunc);
    file << R"JSON({
  "version": 1,
  "providers": [
    {
      "id": "safe-provider",
      "display_name": "Safe Provider",
      "protocol": "openai_chat_completions",
      "base_url": "http://127.0.0.1:11434",
      "auth": "api_key",
      "api_key_env": "SAFE_PROVIDER_KEY"
    },
    {
      "id": "none-provider",
      "display_name": "None Provider",
      "protocol": "openai_chat_completions",
      "base_url": "http://127.0.0.1:11435",
      "auth": "none"
    }
  ]
})JSON";
  }
  ::chmod(paths.providers_file.c_str(), 0600);

  auto catalog = ava::provider::ProviderCatalog::build(paths);
  expect(catalog.has_value(), catalog ? "RPC catalog credential test builds providers.json catalog" : catalog.error().format());
  if (!catalog)
    return;

  // Derived-name trap for safe-provider is SAFE_PROVIDER_API_KEY; configured env is SAFE_PROVIDER_KEY.
  ScopedEnvVar trap_safe("SAFE_PROVIDER_API_KEY", "trap-derived-secret");
  ScopedEnvVar intended_safe("SAFE_PROVIDER_KEY", "intended-safe-secret");
  ScopedEnvVar trap_none("NONE_PROVIDER_API_KEY", "trap-none-secret");
  ScopedEnvVar trap_generic("OPENAI_API_KEY", "trap-openai-secret");

  ava::tests::FakeTransport auth_transport({});

  // ensure_prompt_runtime_options is the RPC credential boundary used by the prompt worker.
  {
    auto prepared = ava::app::rpc::ensure_prompt_runtime_options(paths, "safe-provider", {}, auth_transport, "prompt", *catalog);
    expect(prepared && prepared->access_token == "intended-safe-secret" && prepared->credential_type == "api_key",
           "RPC ensure_prompt_runtime_options uses catalog api_key_env, not derived-name trap");
  }

  {
    auto stored = ava::config::store_provider_credential(
        paths, ava::config::ProviderCredential{
                   .provider_id = "safe-provider", .access_token = "stored-safe-secret", .credential_type = "api_key", .account_id = "", .source = "test"});
    expect(stored.has_value(), "RPC catalog credential test stores provider-scoped API key");
    auto prepared = ava::app::rpc::ensure_prompt_runtime_options(paths, "safe-provider", {}, auth_transport, "prompt", *catalog);
    expect(prepared && prepared->access_token == "stored-safe-secret" && prepared->credential_type == "api_key",
           "RPC ensure_prompt_runtime_options prefers stored key over configured env and trap env");
    std::error_code ec;
    std::filesystem::remove(paths.auth_file, ec);
  }

  {
    auto prepared = ava::app::rpc::ensure_prompt_runtime_options(paths, "none-provider", {}, auth_transport, "prompt", *catalog);
    expect(prepared && prepared->credential_type == "none" && prepared->access_token.empty(),
           "RPC ensure_prompt_runtime_options honors auth:none without consuming trap env secrets");
  }

  // Full RPC prompt path: empty runtime token forces catalog-bound resolution; Authorization must be intended secret.
  ava::config::ModelInfo model;
  model.provider_id = "safe-provider";
  model.model_id = "safe-model";
  model.display_name = "Safe Model";
  model.api_family = "openai_chat_completions";
  model.supports_tools = true;
  model.supports_streaming = true;

  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.mode = ava::agent::Mode::Build;
  open_context.paths = paths;
  open_context.provider_catalog = *catalog;
  open_context.default_model_override = model;
  open_context.pin_model_override = true;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), session ? "RPC catalog credential prompt opens session on safe-provider" : session.error().format());
  if (!session)
    return;

  ava::provider::OpenAICompatibleProvider const provider(ava::provider::OpenAICompatibleProviderOptions{
      .base_url = "http://127.0.0.1:11434",
      .chat_completions_path = "/v1/chat/completions",
      .endpoint = "http://127.0.0.1:11434/v1/chat/completions",
      .provider_name = "Safe Provider",
      .follow_redirects = false,
  });
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200,
      .headers = {},
      .body = "data: {\"id\":\"c\",\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"safe ok\"}}]}\n\n"
              "data: [DONE]\n\n",
  }});
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  ThreadSafeStringBuf output_buffer;
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  std::jthread rpc_thread([&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, ava::app::runtime::RunOptions{}, in, out,
                                    [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"p-safe\",\"type\":\"prompt\",\"message\":\"hello safe\"}\n");
  bool const completed = wait_for_rpc_response(output_buffer, "p-safe", std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();
  auto const jsonl = output_buffer.str();
  auto const response = rpc_response_line(jsonl, "p-safe").value_or("");
  expect_all("RPC catalog-credential prompt returns its successful terminal answer",
             {{"success", response.find("\"success\":true") != std::string_view::npos}, {"answer", response.find("safe ok") != std::string_view::npos}});
  expect(result.has_value(), "RPC prompt with catalog credential policy completes");
  expect(completed && transport.requests().size() == 1, "RPC prompt issues one provider request under catalog credentials");
  if (!transport.requests().empty())
  {
    auto const auth = transport.requests()[0].headers.find("Authorization");
    expect(auth != transport.requests()[0].headers.end() && auth->second == "Bearer intended-safe-secret",
           "RPC prompt Authorization uses SAFE_PROVIDER_KEY, not derived-name trap secret");
    expect(transport.requests()[0].headers.find("Authorization") != transport.requests()[0].headers.end() &&
               transport.requests()[0].headers.at("Authorization").find("trap-") == std::string::npos,
           "RPC prompt never sends trap env secret material");
  }
}

}  // namespace ava::tests::app_rpc_test
