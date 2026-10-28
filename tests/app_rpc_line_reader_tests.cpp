#include "sys.h"
#include "tests/app_rpc_test_cases.h"
#include "tests/support/app_rpc_test_support.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/app/rpc/input.h"
#include "ava/app/rpc/protocol.h"
#include "ava/app/rpc_mode.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
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
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace ava::tests::app_rpc_test {

namespace {

class WakeBadInputBuf final : public std::streambuf
{
 public:
  bool wait_until_blocked(std::chrono::milliseconds timeout)
  {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return blocked_; });
  }

  void wake() noexcept
  {
    {
      std::lock_guard lock(mutex_);
      woken_ = true;
    }
    cv_.notify_all();
  }

 protected:
  int underflow() override
  {
    std::unique_lock lock(mutex_);
    blocked_ = true;
    cv_.notify_all();
    cv_.wait(lock, [&] { return woken_; });
    throw std::runtime_error("wake-induced stream failure");
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool blocked_ = false;
  bool woken_ = false;
};

}  // namespace

void test_app_rpc_posix_line_reader_wake_eof_and_fd_lifetime()
{
  using ava::app::rpc::RpcInputTerminalOutcome;

  expect(!ava::app::rpc::make_posix_rpc_line_reader(-1), "RPC POSIX line reader rejects an invalid descriptor instead of waiting forever on its wake pipe");

  auto fd_count = [] {
    std::error_code error;
    std::size_t count = 0;
    for (auto const& entry : std::filesystem::directory_iterator("/proc/self/fd", error))
    {
      static_cast<void>(entry);
      ++count;
    }
    return error ? std::optional<std::size_t>{} : std::optional<std::size_t>{count};
  };
  auto const before = fd_count();
  bool framing_ok = true;
  bool races_ok = true;
  for (int iteration = 0; iteration < 32; ++iteration)
  {
    int fds[2] = {-1, -1};
    if (pipe(fds) != 0)
    {
      races_ok = false;
      break;
    }
    auto reader = ava::app::rpc::make_posix_rpc_line_reader(fds[0]);
    if (!reader)
    {
      close(fds[0]);
      close(fds[1]);
      races_ok = false;
      break;
    }
    if (iteration == 0)
    {
      std::string const records = "one\r\ntwo";
      auto const bytes_written = write(fds[1], records.data(), records.size());
      close(fds[1]);
      std::vector<RpcInputTerminalOutcome> outcomes;
      std::string line;
      auto first = (*reader)->read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); });
      framing_ok = bytes_written == static_cast<ssize_t>(records.size()) && first && *first && line == "one\r";
      auto second = (*reader)->read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); });
      framing_ok = framing_ok && second && *second && line == "two";
      auto eof = (*reader)->read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); });
      framing_ok = framing_ok && eof && !*eof &&
                   outcomes == std::vector<RpcInputTerminalOutcome>{RpcInputTerminalOutcome::EofWithFinalRecord, RpcInputTerminalOutcome::Eof};
    }
    else
    {
      ava::core::Result<bool> read_result = true;
      std::vector<RpcInputTerminalOutcome> outcomes;
      std::string line;
      ava::core::JoinThread reading = ava::core::JoinThread::create(
          "reading", [&] { read_result = (*reader)->read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); }); });
      (*reader)->cancel();
      close(fds[1]);
      reading.join();
      races_ok = races_ok && read_result && !*read_result && outcomes == std::vector<RpcInputTerminalOutcome>{RpcInputTerminalOutcome::Canceled};
    }
    close(fds[0]);
  }

  int error_fds[2] = {-1, -1};
  bool error_outcome_ok = false;
  if (pipe(error_fds) == 0)
  {
    auto reader = ava::app::rpc::make_posix_rpc_line_reader(error_fds[0]);
    if (reader)
    {
      close(error_fds[0]);
      std::vector<RpcInputTerminalOutcome> outcomes;
      std::string line;
      auto error = (*reader)->read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); });
      error_outcome_ok = !error && error.error().category() == ava::core::ErrorCategory::Io &&
                         outcomes == std::vector<RpcInputTerminalOutcome>{RpcInputTerminalOutcome::Error};
    }
    else
    {
      close(error_fds[0]);
    }
    close(error_fds[1]);
  }
  auto const after = fd_count();
  ava::tests::expect_all("RPC POSIX line reader framing and terminal outcomes",
                         {{"framing", framing_ok}, {"wake/EOF classification", races_ok}, {"I/O error category", error_outcome_ok}});
  expect(!before || !after || *before == *after, "RPC POSIX line reader wake pipes do not leak file descriptors");
}

void test_app_rpc_stream_line_reader_terminal_outcomes()
{
  using ava::app::rpc::RpcInputTerminalOutcome;

  std::vector<RpcInputTerminalOutcome> outcomes;
  std::string line;
  std::istringstream empty_input;
  ava::app::rpc::StreamRpcLineReader empty_reader(empty_input, ava::app::rpc::RpcInputWake{});
  auto empty_eof = empty_reader.read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); });

  std::istringstream final_input("final record");
  ava::app::rpc::StreamRpcLineReader final_reader(final_input, ava::app::rpc::RpcInputWake{});
  auto final_record = final_reader.read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); });
  bool const final_record_read = final_record && *final_record && line == "final record";
  auto final_eof = final_reader.read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); });

  std::istringstream newline_input("complete\n");
  ava::app::rpc::StreamRpcLineReader newline_reader(newline_input, ava::app::rpc::RpcInputWake{});
  auto complete_record = newline_reader.read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); });
  bool const normal_record_silent = complete_record && *complete_record && line == "complete" && outcomes.size() == 3;

  BlockingInputBuf canceled_input_buffer;
  canceled_input_buffer.push("partial final record");
  std::istream canceled_input(&canceled_input_buffer);
  ava::app::rpc::StreamRpcLineReader canceled_reader(canceled_input, [&] noexcept { canceled_input_buffer.close(); });
  ava::core::Result<bool> canceled = true;
  auto canceled_read = ava::core::JoinThread::create(
      "canceled_read", [&] { canceled = canceled_reader.read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); }); });
  bool const canceled_while_partial = canceled_input_buffer.wait_until_blocked(std::chrono::seconds(2));
  canceled_reader.cancel();
  canceled_read.join();

  std::istringstream error_input("unread");
  error_input.setstate(std::ios::badbit);
  ava::app::rpc::StreamRpcLineReader error_reader(error_input, ava::app::rpc::RpcInputWake{});
  auto read_error = error_reader.read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); });

  ava::tests::expect_all(
      "RPC stream reader synchronously classifies terminal outcomes",
      {{"empty EOF", empty_eof && !*empty_eof},
       {"final record", final_record_read},
       {"final EOF", final_eof && !*final_eof},
       {"ordinary record is nonterminal", normal_record_silent},
       {"partial cancellation", canceled_while_partial},
       {"canceled read", canceled && !*canceled},
       {"I/O error", !read_error && read_error.error().category() == ava::core::ErrorCategory::Io},
       {"ordered terminal callbacks",
        outcomes == std::vector<RpcInputTerminalOutcome>{RpcInputTerminalOutcome::Eof, RpcInputTerminalOutcome::EofWithFinalRecord,
                                                         RpcInputTerminalOutcome::Eof, RpcInputTerminalOutcome::Canceled, RpcInputTerminalOutcome::Error}}});

  std::istringstream throwing_eof_input("final");
  throwing_eof_input.exceptions(std::ios::badbit | std::ios::failbit);
  ava::app::rpc::StreamRpcLineReader throwing_eof_reader(throwing_eof_input);
  auto throwing_final = throwing_eof_reader.read_line(line);
  expect(throwing_final && *throwing_final && line == "final", "RPC stream reader preserves the final record when EOF throws");
  auto throwing_eof = throwing_eof_reader.read_line(line);
  expect(throwing_eof && !*throwing_eof, "RPC stream reader contains exception-enabled EOF");

  std::istringstream throwing_bad_input;
  throwing_bad_input.exceptions(std::ios::badbit);
  throwing_bad_input.setstate(std::ios::failbit);
  ava::app::rpc::StreamRpcLineReader throwing_bad_reader(throwing_bad_input);
  auto throwing_bad = throwing_bad_reader.read_line(line);
  expect(!throwing_bad && throwing_bad.error().category() == ava::core::ErrorCategory::Io, "RPC stream reader reports failed input through Result");
}

void test_app_rpc_stream_reader_wake_badbit_is_canceled()
{
  using ava::app::rpc::RpcInputTerminalOutcome;

  WakeBadInputBuf input_buffer;
  std::istream in(&input_buffer);
  in.exceptions(std::ios::badbit);
  ava::app::rpc::StreamRpcLineReader reader(in, [&] noexcept { input_buffer.wake(); });
  std::vector<RpcInputTerminalOutcome> outcomes;
  std::string line;
  ava::core::Result<bool> result = true;
  ava::core::JoinThread reader_thread = ava::core::JoinThread::create(
      "reader_thread", [&] { result = reader.read_line(line, [&outcomes](RpcInputTerminalOutcome outcome) { outcomes.push_back(outcome); }); });
  bool const blocked = input_buffer.wait_until_blocked(std::chrono::seconds(2));
  reader.cancel();
  reader_thread.join();

  ava::tests::expect_all("RPC stream reader classifies wake-induced badbit as cancellation",
                         {{"blocked before wake", blocked},
                          {"wake sets badbit", in.bad()},
                          {"no record", result && !*result},
                          {"canceled callback", outcomes == std::vector<RpcInputTerminalOutcome>{RpcInputTerminalOutcome::Canceled}}});
}

void test_app_rpc_unterminated_final_command_executes()
{
  auto const root = create_empty_root("app-rpc-unterminated-final-command");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC unterminated final command test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  ava::app::runtime::RunOptions runtime_options;
  std::istringstream in("{\"id\":\"protocol\",\"type\":\"get_protocol\"}");
  std::ostringstream out;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, ava::app::rpc::RpcInputWake{});
  auto const jsonl = out.str();

  expect(result && jsonl.find("\"id\":\"protocol\",\"type\":\"response\",\"success\":true") != std::string::npos,
         "RPC loop executes an unterminated final command before observing EOF closure");
}

void test_app_rpc_newline_terminated_oversized_line_recovers()
{
  auto const root = create_empty_root("app-rpc-oversized-line-recovery");

  auto const workspace = root / "workspace";
  auto const paths = app_test_paths(root);
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext open_context;
  open_context.workspace_dir = workspace;
  open_context.current_dir = workspace;
  open_context.paths = paths;
  auto session = ava::app::runtime::Session::open(open_context);
  expect(session.has_value(), "RPC oversized-line recovery test opens runtime session");
  if (!session)
    return;

  ava::provider::OpenAIProvider const provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  ava::app::runtime::RunOptions runtime_options;
  std::string input(ava::app::rpc::kMaxRpcLineBytes + 1, 'x');
  input += "\n{\"id\":\"protocol\",\"type\":\"get_protocol\"}\n";
  std::istringstream in(std::move(input));
  std::ostringstream out;
  ava::app::runtime::session_ts unlocked_session(std::move(*session));
  auto result = ava::app::run_rpc_loop(unlocked_session, open_context, provider, transport, runtime_options, in, out, ava::app::rpc::RpcInputWake{});
  auto const jsonl = out.str();

  auto const rejected = rpc_response_line(jsonl, "").value_or("");
  auto const recovered = rpc_response_line(jsonl, "protocol").value_or("");
  expect_all("RPC newline-terminated oversized line is recoverable and the next command executes",
             {{"loop completes", result.has_value()},
              {"one rejected record", count_substrings(jsonl, "\"id\":\"\",\"type\":\"response\",\"success\":false") == 1},
              {"invalid_request code", rejected.find("\"code\":\"invalid_request\"") != std::string_view::npos},
              {"following command succeeds", recovered.find("\"success\":true") != std::string_view::npos}});
}

}  // namespace ava::tests::app_rpc_test
