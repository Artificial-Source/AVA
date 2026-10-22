#include "sys.h"
#include "tests/app_rpc_test_cases.h"
#include "tests/support/app_rpc_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/app/rpc/input.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/permissions/permission.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/error.h"
#include "ava/core/result.h"
#include "ava/core/thread.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <ios>
#include <istream>
#include <memory>
#include <mutex>
#include <ostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ava::tests::app_rpc_test {

namespace {

class TerminalPublicationStreamBuf final : public std::streambuf
{
 public:
  explicit TerminalPublicationStreamBuf(std::string terminal_marker, bool fail_terminal = false)
      : terminal_marker_(std::move(terminal_marker)), fail_terminal_(fail_terminal)
  {
  }

  bool wait_until_terminal(std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return terminal_reached_; });
  }

  bool wait_contains(std::string_view value, std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return text_.find(value) != std::string::npos; });
  }

  bool wait_for_responses(std::string_view id, std::size_t count, std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] {
      std::size_t found = 0;
      std::size_t begin = 0;
      for (auto end = text_.find('\n', begin); end != std::string::npos; end = text_.find('\n', begin))
      {
        auto const record = std::string_view(text_).substr(begin, end - begin + 1);
        if (rpc_response_line(record, id))
          ++found;
        begin = end + 1;
      }
      return found >= count;
    });
  }

  void release_terminal()
  {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

  std::string str() const
  {
    std::lock_guard lock(mutex_);
    return text_;
  }

 protected:
  int overflow(int ch) override
  {
    if (ch == traits_type::eof())
      return traits_type::not_eof(ch);
    {
      std::lock_guard lock(mutex_);
      text_.push_back(static_cast<char>(ch));
    }
    cv_.notify_all();
    return ch;
  }

  std::streamsize xsputn(char const* data, std::streamsize count) override
  {
    {
      std::lock_guard lock(mutex_);
      text_.append(data, static_cast<std::size_t>(count));
    }
    cv_.notify_all();
    return count;
  }

  int sync() override
  {
    std::unique_lock lock(mutex_);
    if (!terminal_handled_ && text_.find(terminal_marker_) != std::string::npos)
    {
      terminal_handled_ = true;
      terminal_reached_ = true;
      cv_.notify_all();
      if (fail_terminal_)
        return -1;
      cv_.wait(lock, [&] { return released_; });
    }
    return 0;
  }

 private:
  std::string terminal_marker_;
  bool fail_terminal_ = false;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::string text_;
  bool terminal_handled_ = false;
  bool terminal_reached_ = false;
  bool released_ = false;
};

class PausingTerminalCallbackLineReader final : public ava::app::rpc::RpcLineReader
{
 public:
  explicit PausingTerminalCallbackLineReader(std::istream& input, ava::app::rpc::RpcInputWake wake) : input_(input, std::move(wake)) { }

  ava::core::Result<bool> read_line(std::string& line, ava::app::rpc::RpcInputTerminalCallback const& on_terminal) override
  {
    return input_.read_line(line, [this, &on_terminal](ava::app::rpc::RpcInputTerminalOutcome outcome) {
      if (on_terminal)
        on_terminal(outcome);
      {
        std::lock_guard lock(mutex_);
        terminal_callback_observed_ = true;
      }
      cv_.notify_all();
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return terminal_callback_released_; });
    });
  }

  void cancel() noexcept override { input_.cancel(); }

  bool wait_until_terminal_callback(std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return terminal_callback_observed_; });
  }

  void release_terminal_callback()
  {
    {
      std::lock_guard lock(mutex_);
      terminal_callback_released_ = true;
    }
    cv_.notify_all();
  }

 private:
  ava::app::rpc::StreamRpcLineReader input_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool terminal_callback_observed_ = false;
  bool terminal_callback_released_ = false;
};

}  // namespace

void test_app_rpc_terminal_publication_gates_prompt_id_reuse()
{
  auto const root = create_empty_root("app-rpc-prompt-publication-gate");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC prompt publication gate test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(final_text_sse("first terminal")), sse_response(final_text_sse("reused terminal"))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  TerminalPublicationStreamBuf output_buffer("\"id\":\"same\",\"type\":\"response\"");
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"same\",\"type\":\"prompt\",\"message\":\"first\"}\n");
  bool const terminal_observable = output_buffer.wait_until_terminal(std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"same\",\"type\":\"prompt\",\"message\":\"reuse after response\"}\n");
  output_buffer.release_terminal();
  bool const reused = output_buffer.wait_for_responses("same", 2, std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result && terminal_observable && reused && transport.requests().size() == 2,
         "RPC prompt response publication gates admission and permits immediate sequential id reuse");
  expect(jsonl.find("RPC request id is already outstanding") == std::string::npos && count_substrings(jsonl, "\"id\":\"same\",\"type\":\"response\"") == 2,
         "RPC sequential prompt id reuse is not misclassified as a pipelined duplicate");
}

void test_app_rpc_parent_terminal_precedes_queued_follow_up_start()
{
  auto const root = create_empty_root("app-rpc-parent-follow-up-publication");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  auto const outside_path = root / "outside.txt";
  std::filesystem::create_directories(workspace);
  write_app_test_file(outside_path, "parent follow-up publication");
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC parent/follow-up publication test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({sse_response(read_file_call_sse(outside_path.generic_string())), sse_response(final_text_sse("parent terminal")),
                                       sse_response(final_text_sse("child terminal"))});
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  TerminalPublicationStreamBuf output_buffer("\"id\":\"parent\",\"type\":\"response\"");
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
  });

  input_buffer.push("{\"id\":\"parent\",\"type\":\"prompt\",\"message\":\"parent\"}\n");
  bool const permission_requested = output_buffer.wait_contains("\"name\":\"permission_requested\"", std::chrono::seconds(2));
  auto const resolver_id = rpc_string_field_from_output(output_buffer.str(), "resolver_request_id");
  input_buffer.push("{\"id\":\"child\",\"type\":\"follow_up\",\"message\":\"child\"}\n");
  input_buffer.push("{\"id\":\"allow\",\"type\":\"permission_reply\",\"request_id\":\"" + resolver_id.value_or("") +
                    "\",\"correlation_id\":\"parent\",\"decision\":\"allow\"}\n");
  bool const parent_observable = output_buffer.wait_until_terminal(std::chrono::seconds(2));
  bool const child_not_started_during_parent_flush = output_buffer.str().find("\"name\":\"follow_up_started\"") == std::string::npos;
  output_buffer.release_terminal();
  bool const child_completed = output_buffer.wait_for_responses("child", 1, std::chrono::seconds(2));
  input_buffer.close();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  auto const parent_response = jsonl.find("\"id\":\"parent\",\"type\":\"response\"");
  auto const child_started = jsonl.find("\"name\":\"follow_up_started\"");
  auto const child_response = jsonl.find("\"id\":\"child\",\"type\":\"response\"");
  expect(result && permission_requested && resolver_id && parent_observable && child_not_started_during_parent_flush && child_completed,
         "RPC queued child remains unpublished while the parent terminal response flush is blocked");
  expect(parent_response < child_started && child_started < child_response,
         "RPC parent response remains ordered before follow_up_started and the queued child response");
}

void test_app_rpc_eof_during_blocked_parent_publication_skips_follow_up()
{
  auto const root = create_empty_root("app-rpc-eof-during-parent-publication");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC blocked-publication EOF test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  BlockingResponseTransport transport(sse_response(final_text_sse("parent terminal")));
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  PausingTerminalCallbackLineReader input(in, [&] noexcept { input_buffer.close(); });
  TerminalPublicationStreamBuf output_buffer("\"id\":\"parent\",\"type\":\"response\"");
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto rpc_thread = ava::core::JoinThread::create(
      "rpc_thread", [&] { result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, transport, runtime_options, input, out); });

  input_buffer.push("{\"id\":\"parent\",\"type\":\"prompt\",\"message\":\"parent\"}\n");
  bool const parent_requested = transport.wait_for_request(std::chrono::seconds(2));
  input_buffer.push("{\"id\":\"child\",\"type\":\"follow_up\",\"message\":\"must not run\"}\n");
  bool const child_queued = output_buffer.wait_contains("\"name\":\"follow_up_queued\"", std::chrono::seconds(2));
  transport.release();
  bool const parent_publication_blocked = output_buffer.wait_until_terminal(std::chrono::seconds(2));
  input_buffer.close();
  bool const terminal_state_published = input.wait_until_terminal_callback(std::chrono::seconds(2));
  output_buffer.release_terminal();
  bool const child_skipped_while_reader_paused = output_buffer.wait_contains("\"name\":\"follow_up_skipped\"", std::chrono::seconds(2));
  input.release_terminal_callback();
  rpc_thread.join();

  auto const jsonl = output_buffer.str();
  expect(result && parent_requested && child_queued && parent_publication_blocked && terminal_state_published && child_skipped_while_reader_paused,
         "RPC EOF terminal state is published before the parent response flush is released while the reader remains paused");
  expect(transport.requests().size() == 1 && jsonl.find("\"name\":\"follow_up_started\"") == std::string::npos &&
             jsonl.find("\"id\":\"child\",\"type\":\"response\",\"success\":true") == std::string::npos,
         "RPC EOF during parent publication cannot start or run the queued child");
  auto const parent_response = jsonl.find("\"id\":\"parent\",\"type\":\"response\"");
  auto const child_skipped = jsonl.find("\"name\":\"follow_up_skipped\"");
  auto const child_error = jsonl.find("\"id\":\"child\",\"type\":\"response\",\"success\":false");
  expect(jsonl.find("\"request_id\":\"child\"") != std::string::npos && child_skipped != std::string::npos &&
             jsonl.find("\"reason\":\"canceled\"") != std::string::npos && child_error != std::string::npos && parent_response < child_skipped &&
             child_skipped < child_error,
         "RPC EOF publishes the parent response before the queued child skipped event and canceled response once output becomes writable");
}

void test_app_rpc_terminal_publication_gates_direct_and_compaction_runs()
{
  {
    auto const root = create_empty_root("app-rpc-direct-publication-gate");

    auto const workspace = root / "workspace";
    auto const paths = app_test_paths(root);
    std::filesystem::create_directories(workspace);
    ava::app::runtime::OpenContext open_context;
    open_context.workspace_dir = workspace;
    open_context.current_dir = workspace;
    open_context.paths = paths;
    auto session = ava::app::runtime::Session::open(open_context);
    expect(session.has_value(), "RPC direct publication gate test opens runtime session");
    if (session)
    {
      ava::provider::OpenAIProvider const provider("https://api.example.test");
      ava::tests::FakeTransport transport({sse_response(final_text_sse("prompt after direct"))});
      ava::app::runtime::RunOptions runtime_options;
      runtime_options.access_token = "token";
      runtime_options.permission_resolver = [](ava::permissions::PermissionPrompt const&) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
        return ava::permissions::PermissionResolutionDecision{ava::permissions::PermissionResolution::Allow, "publication gate test"};
      };
      BlockingInputBuf input_buffer;
      std::istream in(&input_buffer);
      TerminalPublicationStreamBuf output_buffer("\"id\":\"direct\",\"type\":\"response\"");
      std::ostream out(&output_buffer);
      ava::core::VoidResult result;
      ava::app::runtime::session_ts unlocked_session(std::move(*session));
      ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
        result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
      });
      input_buffer.push("{\"id\":\"direct\",\"type\":\"run_bash\",\"command\":\"printf direct\"}\n");
      bool const terminal_observable = output_buffer.wait_until_terminal(std::chrono::seconds(2));
      input_buffer.push("{\"id\":\"next\",\"type\":\"prompt\",\"message\":\"after direct\"}\n");
      output_buffer.release_terminal();
      bool const next_completed = output_buffer.wait_for_responses("next", 1, std::chrono::seconds(2));
      input_buffer.close();
      rpc_thread.join();
      auto const jsonl = output_buffer.str();
      expect(result && terminal_observable && next_completed && jsonl.find("\"code\":\"active_run\"") == std::string::npos,
             "RPC direct terminal publication settles active state before admitting an immediately submitted prompt");
    }
  }

  {
    auto const root = create_empty_root("app-rpc-compaction-publication-gate");

    auto const workspace = root / "workspace";
    auto const paths = app_test_paths(root);
    std::filesystem::create_directories(workspace);
    ava::app::runtime::OpenContext open_context;
    open_context.workspace_dir = workspace;
    open_context.current_dir = workspace;
    open_context.paths = paths;
    auto session = ava::app::runtime::Session::open(open_context);
    expect(session.has_value(), "RPC compaction publication gate test opens runtime session");
    if (session)
    {
      ava::provider::OpenAIProvider const provider("https://api.example.test");
      ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "{\"error\":{\"message\":\"first\"}}"},
                                           ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = "{\"error\":{\"message\":\"second\"}}"}});
      ava::app::runtime::RunOptions runtime_options;
      runtime_options.access_token = "token";
      BlockingInputBuf input_buffer;
      std::istream in(&input_buffer);
      TerminalPublicationStreamBuf output_buffer("\"id\":\"compact\",\"type\":\"response\"");
      std::ostream out(&output_buffer);
      ava::core::VoidResult result;
      ava::app::runtime::session_ts unlocked_session(std::move(*session));
      ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
        result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, [&] noexcept { input_buffer.close(); });
      });
      input_buffer.push("{\"id\":\"compact\",\"type\":\"compact\"}\n");
      bool const terminal_observable = output_buffer.wait_until_terminal(std::chrono::seconds(2));
      input_buffer.push("{\"id\":\"compact\",\"type\":\"compact\"}\n");
      output_buffer.release_terminal();
      bool const second_completed = output_buffer.wait_for_responses("compact", 2, std::chrono::seconds(2));
      input_buffer.close();
      rpc_thread.join();
      auto const jsonl = output_buffer.str();
      expect(result && terminal_observable && second_completed, "RPC compaction terminal publication completes both observable sequential responses");
      expect(transport.requests().size() == 2 && jsonl.find("RPC request id is already outstanding") == std::string::npos,
             "RPC compaction terminal publication permits immediate sequential id reuse after observable response");
    }
  }
}

void test_app_rpc_worker_output_failure_wakes_blocked_input()
{
  auto const root = create_empty_root("app-rpc-output-failure-wake");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC output-failure wake test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  BlockingResponseTransport transport(sse_response(final_text_sse("terminal write fails")));
  ava::app::runtime::RunOptions runtime_options;
  runtime_options.access_token = "token";
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  TerminalPublicationStreamBuf output_buffer("\"id\":\"p1\",\"type\":\"response\"", true);
  std::ostream out(&output_buffer);
  ava::core::VoidResult result;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  ava::core::JoinThread rpc_thread = ava::core::JoinThread::create("rpc_thread", [&] {
    result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, transport, runtime_options, in, out,
                                    [&] noexcept { input_buffer.close(); });
  });
  input_buffer.push("{\"id\":\"p1\",\"type\":\"prompt\",\"message\":\"fail output\"}\n");
  bool const request_started = transport.wait_for_request(std::chrono::seconds(2));
  bool const input_blocked = input_buffer.wait_until_blocked(std::chrono::seconds(2));
  transport.release();
  bool const exited_without_more_input = input_buffer.wait_until_eof_observed(std::chrono::seconds(2));
  // Rescue the reader independently so a missing wake reports a local failure.
  input_buffer.close();
  rpc_thread.join();

  expect_all("public std::istream RPC loop wakes after worker output failure without another stdin record",
             {{"provider requested", request_started}, {"reader blocked", input_blocked}, {"worker woke input", exited_without_more_input}});
  expect(!result && result.error().category() == ava::core::ErrorCategory::Io, "RPC output-failure wake returns the worker output I/O category after joining");
}

void test_app_rpc_mode_forwards_nonstdin_wake()
{
  auto const root = create_empty_root("app-rpc-mode-stream-wake");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::app::RpcModeOptions options;
  options.open_context.workspace_dir = workspace;
  options.open_context.current_dir = workspace;
  options.open_context.paths = app_test_paths(root);
  options.open_context.offline = true;
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto process_scope = ava::process::ProcessScopeV1::application(supervisor);
  expect(static_cast<bool>(process_scope), "RPC mode wake test creates explicit process authority");
  if (!process_scope)
    return;
  options.open_context.application_process_scope = *process_scope;
  BlockingInputBuf input_buffer;
  std::istream in(&input_buffer);
  std::ostringstream out;
  out.setstate(std::ios::badbit);
  std::ostringstream err;
  bool wake_called = false;
  input_buffer.push("{\"id\":\"protocol\",\"type\":\"get_protocol\"}\n");
  auto const status = ava::app::run_rpc_mode(options, in, out, err, [&] noexcept {
    wake_called = true;
    input_buffer.close();
  });

  expect(status == 1 && wake_called && err.str().find("failed to write RPC JSONL record") != std::string::npos,
         "run_rpc_mode forwards an explicit wake callback for a non-std::cin stream on output failure");
}

}  // namespace ava::tests::app_rpc_test
