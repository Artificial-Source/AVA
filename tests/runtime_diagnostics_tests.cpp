#include "sys.h"
#include "tests/support/app_runtime_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_harness.h"
#include "ava/diagnostics/artifact_store.h"
#include "ava/diagnostics/runtime_diagnostics.h"
#include "ava/http/transport.h"
#include "ava/app/runtime.h"
#include "ava/app/runtime/Session.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/json.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include "debug.h"

namespace {

using namespace ava::tests;

std::filesystem::path diagnostics_root(std::string_view name)
{
  static std::atomic<unsigned long long> sequence{0};
  auto const root = temp_root() / (std::string(name) + "_" + std::to_string(static_cast<unsigned long long>(::getpid())) + "_" +
                                   std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  static_cast<void>(::chmod(root.c_str(), 0700));
  return root;
}

ava::config::XdgPaths private_paths(std::filesystem::path const& root)
{
  ava::config::XdgPaths paths;
  paths.state_home = root / "state";
  paths.ava_state_dir = paths.state_home / "ava";
  paths.sessions_dir = paths.ava_state_dir / "sessions";
  return paths;
}

std::string read_all(std::filesystem::path const& path)
{
  return ava::tests::read_session_test_binary_file(path);
}

std::vector<std::string> lines(std::string const& text)
{
  std::vector<std::string> result;
  std::istringstream input(text);
  for (std::string line; std::getline(input, line);) result.push_back(std::move(line));
  return result;
}

void make_private_directories(std::initializer_list<std::filesystem::path> directories)
{
  for (auto const& directory : directories)
  {
    std::filesystem::create_directories(directory);
    static_cast<void>(::chmod(directory.c_str(), 0700));
  }
}

std::shared_ptr<ava::core::AnchorSet> private_anchors(ava::config::XdgPaths const& paths)
{
  make_private_directories({paths.state_home, paths.ava_state_dir});
  auto anchors = ava::core::AnchorSet::open({paths.ava_state_dir});
  expect(anchors.has_value(), anchors ? "runtime diagnostic anchors open" : anchors.error().format());
  return anchors ? std::move(*anchors) : nullptr;
}

void test_disabled_runtime_diagnostics_is_artifact_free()
{
  auto const root = diagnostics_root("runtime-diagnostics-disabled");
  auto const paths = private_paths(root);
  {
    ava::diagnostics::RuntimeDiagnostics diagnostics(paths);
    expect(!diagnostics.trace_enabled() && !diagnostics.observation() && !diagnostics.trace_path(),
           "disabled runtime diagnostics has no production observation");
  }
  expect(!std::filesystem::exists(paths.state_home), "disabled runtime diagnostics performs no filesystem access");
  std::filesystem::remove_all(root);
}

void test_private_trace_sanitization_limits_and_counters()
{
  constexpr std::string_view session_canary = "SESSION_CANARY_PRIVATE_4091";
  constexpr std::string_view provider_canary = "PROVIDER_CANARY_PRIVATE_4092";
  constexpr std::string_view tool_canary = "TOOL_CANARY_PRIVATE_4093";
  constexpr std::string_view path_canary = "/tmp/PATH_CANARY_PRIVATE_4094";
  constexpr std::string_view command_canary = "rm -rf COMMAND_CANARY_PRIVATE_4095";
  constexpr std::string_view url_canary = "https://URL_CANARY_PRIVATE_4096.invalid";
  constexpr std::string_view secret_canary = "SECRET_CANARY_PRIVATE_4097";

  auto const root = diagnostics_root("runtime-diagnostics-trace");
  auto const paths = private_paths(root);
  auto const anchors = private_anchors(paths);
  auto created = ava::diagnostics::RuntimeDiagnostics::create(paths, anchors, true);
  expect(created && (*created)->trace_enabled() && (*created)->trace_path(), "explicit tracing securely initializes a production observation");
  if (!created)
    return;
  auto const trace_path_value = (*created)->trace_path();
  if (!trace_path_value)
    return;
  auto const trace_path = *trace_path_value;
  auto observation = (*created)->observation();
  ava::observability::TraceContext first{.run_id = "run-source-canary",
                                         .turn_id = "turn-source-canary",
                                         .session_id = std::string(session_canary),
                                         .provider_id = std::string(provider_canary),
                                         .parent_run_id = "parent-run-source-canary",
                                         .parent_turn_id = "parent-turn-source-canary",
                                         .parent_session_id = "parent-session-source-canary"};
  observation->emit(ava::observability::TraceEventType::TransportRequestResult, first, [&](auto& event) {
    event.call_id = std::string(tool_canary);
    event.phase = ava::observability::TracePhase::Transport;
    event.outcome = ava::observability::TraceOutcome::Success;
    event.fields = {{.key = "request_bytes", .value = "42"},
                    {.key = "streaming", .value = "true"},
                    {.key = "path", .value = std::string(path_canary), .provenance = ava::observability::FieldProvenance::Path},
                    {.key = "command", .value = std::string(command_canary)},
                    {.key = "url", .value = std::string(url_canary)},
                    {.key = "secret", .value = std::string(secret_canary), .provenance = ava::observability::FieldProvenance::Secret},
                    {.key = "status_code", .value = "200-not-safe"}};
    event.fields.push_back({.key = "output_bytes", .value = "987654321012345678", .provenance = ava::observability::FieldProvenance::Secret});
    event.fields.push_back({.key = "ephemeral", .value = "true", .provenance = ava::observability::FieldProvenance::Content});
  });
  observation->emit(ava::observability::TraceEventType::TransportRequestResult, first, [&](auto& event) {
    event.call_id = std::string(tool_canary);
    event.phase = ava::observability::TracePhase::Transport;
    event.outcome = ava::observability::TraceOutcome::Error;
    event.fields = {{.key = "status_code", .value = "503"}};
  });
  auto second = first;
  second.run_id = "different-run-source";
  observation->emit(ava::observability::TraceEventType::TransportRequestResult, second);
  for (std::size_t index = 0; index < 10'005; ++index) observation->emit(ava::observability::TraceEventType::AgentRunStart, first);

  (*created)->close();
  auto const trace = read_all(trace_path);
  auto const records = lines(trace);
  bool const canaries_absent = trace.find(session_canary) == std::string::npos && trace.find(provider_canary) == std::string::npos &&
                               trace.find(tool_canary) == std::string::npos && trace.find(path_canary) == std::string::npos &&
                               trace.find(command_canary) == std::string::npos && trace.find(url_canary) == std::string::npos &&
                               trace.find(secret_canary) == std::string::npos && trace.find("run-source-canary") == std::string::npos;
  expect(canaries_absent, "production trace omits raw identities, paths, commands, URLs, secrets, and tool identifiers");
  expect(records.size() <= 10'000 && trace.size() <= 10U * 1024U * 1024U, "production trace enforces the existing event and byte limits");
  expect(records.size() >= 3 && trace.find("\"request_bytes\":\"42\"") != std::string::npos && trace.find("\"streaming\":\"true\"") != std::string::npos &&
             trace.find("200-not-safe") == std::string::npos,
         "production trace retains only allowlisted proven numeric and boolean metadata");
  expect(trace.find("987654321012345678") == std::string::npos && trace.find("\"ephemeral\"") == std::string::npos,
         "production trace never reclassifies secret or content fields as public metadata");
  if (records.size() >= 3)
  {
    auto const first_run = ava::core::json::string_field(records[0], "run_id");
    auto const repeated_run = ava::core::json::string_field(records[1], "run_id");
    auto const different_run = ava::core::json::string_field(records[2], "run_id");
    auto const first_session = ava::core::json::string_field(records[0], "session_id");
    auto const repeated_session = ava::core::json::string_field(records[1], "session_id");
    expect(first_run && repeated_run && different_run && *first_run == *repeated_run && *first_run != *different_run && *first_run != "run-source-canary",
           "production identity aliases are stable per source and differ across sources");
    expect(first_session && repeated_session && *first_session == *repeated_session && *first_session != session_canary,
           "session aliases are stable and opaque");
  }

  auto const snapshot = ava::diagnostics::read_trace_counter_snapshot(paths, *anchors);
  expect(snapshot.state == ava::diagnostics::StoredRecordState::Present && snapshot.record && snapshot.record->runtime_starts == 10'005 &&
             snapshot.record->provider_requests == 3 && snapshot.record->provider_failures == 1 && snapshot.record->writer_health.complete &&
             snapshot.record->writer_health.events_written == records.size() && snapshot.record->writer_health.events_dropped == 10'008 - records.size() &&
             snapshot.record->writer_health.writer_failures == 0 && snapshot.record->writer_health.bytes_written == trace.size(),
         "trace close persists outcome counters and complete drained writer health under queue pressure");
  struct stat trace_metadata{};
  expect(::stat(trace_path.c_str(), &trace_metadata) == 0 && S_ISREG(trace_metadata.st_mode) && (trace_metadata.st_mode & 07777) == 0600 &&
             trace_metadata.st_nlink == 1 && trace_metadata.st_uid == ::geteuid(),
         "production trace is one owner-only singly-linked regular file");
  std::filesystem::remove_all(root);
}

void test_trace_writer_retains_anchored_inode_after_path_replacement()
{
  auto const root = diagnostics_root("runtime-diagnostics-trace-inode");
  auto const paths = private_paths(root);
  auto const anchors = private_anchors(paths);
  auto created = ava::diagnostics::RuntimeDiagnostics::create(paths, anchors, true);
  expect(created && (*created)->trace_path(), "anchored trace writer starts");
  if (!created || !(*created)->trace_path())
    return;

  auto const published = *(*created)->trace_path();
  auto retained = published;
  retained += ".retained";
  std::filesystem::rename(published, retained);
  {
    std::ofstream replacement(published, std::ios::binary | std::ios::trunc);
    replacement << "replacement-must-remain";
  }
  static_cast<void>(::chmod(published.c_str(), 0600));
  ava::observability::TraceContext context;
  (*created)->observation()->emit(ava::observability::TraceEventType::TransportRequestResult, context,
                                  [](auto& event) { event.fields = {{.key = "request_bytes", .value = "7"}}; });
  (*created)->close();
  auto const retained_body = read_all(retained);
  auto const replacement_body = read_all(published);
  expect(retained_body.find("\"request_bytes\":\"7\"") != std::string::npos && replacement_body == "replacement-must-remain",
         "asynchronous trace writes stay bound to the pre-opened inode and never reopen a replacement pathname");
  std::filesystem::remove_all(root);
}

void test_concurrent_traces_are_distinct_and_counters_are_cumulative()
{
  auto const root = diagnostics_root("runtime-diagnostics-concurrent");
  auto const paths = private_paths(root);
  auto const anchors = private_anchors(paths);
  std::array<std::shared_ptr<ava::diagnostics::RuntimeDiagnostics>, 2> diagnostics;
  std::array<bool, 2> succeeded{};
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < diagnostics.size(); ++index)
  {
    threads.emplace_back([&, index] {
      auto created = ava::diagnostics::RuntimeDiagnostics::create(paths, anchors, true);
      succeeded[index] = created.has_value();
      if (created)
        diagnostics[index] = std::move(*created);
    });
  }
  for (auto& thread : threads) thread.join();
  auto first = diagnostics[0] ? diagnostics[0]->trace_path() : std::nullopt;
  auto second = diagnostics[1] ? diagnostics[1]->trace_path() : std::nullopt;
  expect(succeeded[0] && succeeded[1] && first && second && *first != *second, "concurrent traced runtimes receive independent opaque files");
  if (!diagnostics[0] || !diagnostics[1] || !first || !second)
  {
    std::filesystem::remove_all(root);
    return;
  }

  ava::observability::TraceContext context;
  diagnostics[0]->observation()->emit(ava::observability::TraceEventType::AgentRunStart, context);
  diagnostics[0]->observation()->emit(ava::observability::TraceEventType::TransportRequestResult, context,
                                      [](auto& event) { event.outcome = ava::observability::TraceOutcome::Success; });
  diagnostics[1]->observation()->emit(ava::observability::TraceEventType::AgentRunStart, context);
  diagnostics[1]->observation()->emit(ava::observability::TraceEventType::AgentRunStart, context);
  diagnostics[1]->observation()->emit(ava::observability::TraceEventType::TransportRequestResult, context,
                                      [](auto& event) { event.outcome = ava::observability::TraceOutcome::Error; });
  threads.clear();
  for (auto& item : diagnostics) threads.emplace_back([item] { item->close(); });
  for (auto& thread : threads) thread.join();

  auto const first_trace = read_all(*first);
  auto const second_trace = read_all(*second);
  auto const snapshot = ava::diagnostics::read_trace_counter_snapshot(paths, *anchors);
  expect(lines(first_trace).size() == 2 && lines(second_trace).size() == 3 && snapshot.record && snapshot.record->runtime_starts == 3 &&
             snapshot.record->provider_requests == 2 && snapshot.record->provider_failures == 1 && snapshot.record->writer_health.complete &&
             snapshot.record->writer_health.events_written == 5 && snapshot.record->writer_health.events_dropped == 0 &&
             snapshot.record->writer_health.writer_failures == 0 && snapshot.record->writer_health.bytes_written == first_trace.size() + second_trace.size(),
         "concurrent runtime closes retain distinct traces and cumulatively preserve both different contributions");
  std::filesystem::remove_all(root);
}

void test_writer_health_conversion_does_not_double_count_failures()
{
  ava::observability::QueuedJsonlObserverCounters counters;
  counters.written = 2;
  counters.dropped = 3;
  counters.failures = 1;
  counters.bytes_written = 40;
  counters.queue_dropped = 4;
  counters.queue_failures = 1;
  auto const health = ava::diagnostics::trace_writer_health_from_counters(counters);
  expect(health.complete && health.events_written == 2 && health.events_dropped == 7 && health.writer_failures == 1 && health.bytes_written == 40,
         "one failed queued record maps to one persisted writer failure rather than writer and queue observations being summed");
}

void test_trace_initialization_rejects_unsafe_paths_nonblocking()
{
  for (std::string const fixture : {"symlink", "fifo", "hardlink", "permissions"})
  {
    auto const root = diagnostics_root("runtime-diagnostics-unsafe-" + fixture);
    auto const paths = private_paths(root);
    auto const diagnostics_dir = paths.ava_state_dir / "diagnostics";
    make_private_directories({paths.state_home, paths.ava_state_dir, diagnostics_dir});
    auto const traces = diagnostics_dir / "traces";
    if (fixture == "symlink")
    {
      auto const target = root / "target";
      make_private_directories({target});
      std::filesystem::create_directory_symlink(target, traces);
    }
    else if (fixture == "fifo")
    {
      expect(::mkfifo(traces.c_str(), 0600) == 0, "unsafe trace FIFO fixture is created");
    }
    else if (fixture == "hardlink")
    {
      auto const target = root / "target-file";
      std::ofstream(target) << "fixture";
      static_cast<void>(::chmod(target.c_str(), 0600));
      std::filesystem::create_hard_link(target, traces);
    }
    else
    {
      std::filesystem::create_directory(traces);
      static_cast<void>(::chmod(traces.c_str(), 0755));
    }
    auto anchors = ava::core::AnchorSet::open({paths.ava_state_dir});
    auto created = anchors ? ava::diagnostics::RuntimeDiagnostics::create(paths, *anchors, true)
                           : ava::core::Result<std::shared_ptr<ava::diagnostics::RuntimeDiagnostics>>(std::unexpected(anchors.error()));
    expect(!created, "explicit trace rejects unsafe " + fixture + " storage without following or blocking");
    std::filesystem::remove_all(root);
  }
}

void test_last_failure_is_typed_and_message_free()
{
  constexpr std::string_view canary = "RAW_FAILURE_CANARY_PRIVATE_5101";
  auto const root = diagnostics_root("runtime-diagnostics-last-failure");
  auto const paths = private_paths(root);
  auto const anchors = private_anchors(paths);
  ava::diagnostics::RuntimeDiagnostics diagnostics(paths);
  expect(diagnostics.bind_anchor_set(anchors).has_value(), "last-failure diagnostics bind the configured state anchor");
  ava::core::Error provider_error(ava::core::ErrorCategory::Provider, std::string(canary));
  provider_error.with_context("url", std::string(canary));
  diagnostics.record_terminal_failure(ava::diagnostics::RuntimeFailureClass::Provider, provider_error);
  auto const record_path = paths.ava_state_dir / "diagnostics" / "last-failure-v1.json";
  auto const first_body = read_all(record_path);
  auto const first = ava::diagnostics::read_last_failure_record(paths, *anchors);
  expect(first.state == ava::diagnostics::StoredRecordState::Present && first.record &&
             first.record->failure.component == ava::diagnostics::ComponentClass::Provider && first_body.find(canary) == std::string::npos,
         "last failure stores only the stable terminal class and ErrorCategory mapping");
  diagnostics.record_terminal_failure(ava::diagnostics::RuntimeFailureClass::Runtime,
                                      ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "user input must not replace the record"));
  expect(read_all(record_path) == first_body, "user-invalid input does not overwrite the latest terminal failure");
  std::filesystem::remove_all(root);
}

class CollectingObserver final : public ava::observability::RunObserver
{
 public:
  void on_event(ava::observability::TraceEvent const&) override
  {
    std::lock_guard lock(mutex_);
    ++events_;
  }
  [[nodiscard]] std::size_t events() const
  {
    std::lock_guard lock(mutex_);
    return events_;
  }
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  mutable std::mutex mutex_;
  std::size_t events_ = 0;
};

ava::core::Result<ava::app::runtime::session_ts> open_diagnostic_session(std::filesystem::path const& root,
                                                                         std::shared_ptr<ava::diagnostics::RuntimeDiagnostics> diagnostics)
{
  auto const paths = app_test_paths(root);
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::app::runtime::OpenContext options;
  options.workspace_dir = workspace;
  options.current_dir = workspace;
  options.paths = paths;
  options.diagnostics = std::move(diagnostics);
  auto unlocked_session_result = ava::app::runtime::Session::open(options, {.sessionless = true,
                                                                            .requested_session_id = std::nullopt,
                                                                            .fork_session_id = std::nullopt,
                                                                            .initial_session_name = std::nullopt,
                                                                            .continue_last_session = false,
                                                                            .initial_reasoning_level = std::nullopt,
                                                                            .expected_original_cwd = std::nullopt});
  if (!unlocked_session_result)
    return std::unexpected(std::move(unlocked_session_result.error()));
  return unlocked_session_result;
}

void test_runtime_failure_boundaries_and_observation_precedence()
{
  constexpr std::string_view provider_canary = "PROVIDER_BODY_CANARY_PRIVATE_6201";
  auto const provider = ava::provider::OpenAIProvider("https://api.example.test");

  auto const canceled_root = diagnostics_root("runtime-diagnostics-canceled");
  auto const canceled_paths = app_test_paths(canceled_root);
  auto canceled_diagnostics = ava::diagnostics::RuntimeDiagnostics::create(canceled_paths, false);
  auto unlocked_canceled_session_result = canceled_diagnostics
                                              ? open_diagnostic_session(canceled_root, *canceled_diagnostics)
                                              : ava::core::Result<ava::app::runtime::session_ts>(std::unexpected(canceled_diagnostics.error()));
  ava::tests::FakeTransport canceled_transport({});
  ava::app::runtime::RunOptions canceled_options;
  canceled_options.access_token = "token";
  canceled_options.cancel_requested = [] { return true; };
  bool runtime_cancellation_does_not_create_a_last_failure_artifact = false;
  if (unlocked_canceled_session_result)
  {
    auto canceled = ava::app::run_prompt(*unlocked_canceled_session_result, "cancel", provider, canceled_transport, canceled_options);
    SCOPED_CRITICAL_AREA_R(canceled_session_r, *unlocked_canceled_session_result);
    runtime_cancellation_does_not_create_a_last_failure_artifact =
        !canceled &&
        ava::diagnostics::read_last_failure_record(canceled_paths, *canceled_session_r->anchor_set()).state == ava::diagnostics::StoredRecordState::Absent;
  }
  expect(runtime_cancellation_does_not_create_a_last_failure_artifact, "runtime cancellation does not create a last-failure artifact");

  auto const provider_root = diagnostics_root("runtime-diagnostics-provider-failure");
  auto const provider_paths = app_test_paths(provider_root);
  auto provider_diagnostics = ava::diagnostics::RuntimeDiagnostics::create(provider_paths, false);
  auto unlocked_provider_session_result = provider_diagnostics
                                              ? open_diagnostic_session(provider_root, *provider_diagnostics)
                                              : ava::core::Result<ava::app::runtime::session_ts>(std::unexpected(provider_diagnostics.error()));
  ava::tests::FakeTransport provider_transport({ava::http::HttpResponse{.status_code = 500, .headers = {}, .body = std::string(provider_canary)}});
  ava::app::runtime::RunOptions provider_options;
  provider_options.access_token = "token";
  bool central_runtime_provider_failure_persists_a_strict_message_free_record = false;
  if (unlocked_provider_session_result)
  {
    auto provider_result = ava::app::run_prompt(*unlocked_provider_session_result, "fail", provider, provider_transport, provider_options);
    if (!provider_result)
    {
      SCOPED_CRITICAL_AREA_R(provider_session_r, *unlocked_provider_session_result);
      auto provider_failure = ava::diagnostics::read_last_failure_record(provider_paths, *provider_session_r->anchor_set());
      std::string const provider_body = read_all(provider_paths.ava_state_dir / "diagnostics" / "last-failure-v1.json");
      central_runtime_provider_failure_persists_a_strict_message_free_record =
          provider_failure.record && provider_failure.record->failure.component == ava::diagnostics::ComponentClass::Provider &&
          provider_body.find(provider_canary) == std::string::npos;
    }
  }
  expect(central_runtime_provider_failure_persists_a_strict_message_free_record, "central runtime provider failure persists a strict message-free record");

  auto const tool_root = diagnostics_root("runtime-diagnostics-tool-failure");
  auto const tool_paths = app_test_paths(tool_root);
  auto tool_diagnostics = ava::diagnostics::RuntimeDiagnostics::create(tool_paths, true);
  auto unlocked_tool_session_result = tool_diagnostics ? open_diagnostic_session(tool_root, *tool_diagnostics)
                                                       : ava::core::Result<ava::app::runtime::session_ts>(std::unexpected(tool_diagnostics.error()));
  auto supplied_observer = std::make_shared<CollectingObserver>();
  ava::app::runtime::RunOptions tool_options;
  tool_options.access_token = "token";
  tool_options.observation = std::make_shared<ava::observability::RunObservation>(supplied_observer);
  tool_options.event_sink = [](ava::event::RuntimeEvent const&) -> ava::core::VoidResult {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "TOOL_FAILURE_CANARY_PRIVATE_6202"));
  };
  ava::tests::FakeTransport tool_transport({});
  bool central_runtime_tool_failure_persists_the_typed_tool_terminal_class = false;
  if (unlocked_tool_session_result)
  {
    auto tool_result = ava::app::run_prompt(*unlocked_tool_session_result, "tool failure", provider, tool_transport, tool_options);
    if (!tool_result)
    {
      SCOPED_CRITICAL_AREA_R(tool_session_r, *unlocked_tool_session_result);
      auto tool_failure = ava::diagnostics::read_last_failure_record(tool_paths, *tool_session_r->anchor_set());
      central_runtime_tool_failure_persists_the_typed_tool_terminal_class =
          tool_failure.record && tool_failure.record->failure.component == ava::diagnostics::ComponentClass::Tool;
    }
  }
  expect(central_runtime_tool_failure_persists_the_typed_tool_terminal_class, "central runtime tool failure persists the typed tool terminal class");
  expect(supplied_observer->events() == 0, "enabled production diagnostics overrides an explicitly supplied programmatic observation");

  auto const session_root = diagnostics_root("runtime-diagnostics-session-failure");
  auto const session_paths = app_test_paths(session_root);
  auto session_diagnostics = ava::diagnostics::RuntimeDiagnostics::create(session_paths, false);
  auto unlocked_failed_session_result = session_diagnostics ? open_diagnostic_session(session_root, *session_diagnostics)
                                                            : ava::core::Result<ava::app::runtime::session_ts>(std::unexpected(session_diagnostics.error()));
  ava::app::runtime::RunOptions session_options;
  session_options.access_token = "token";
  session_options.event_sink = [](ava::event::RuntimeEvent const&) -> ava::core::VoidResult {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "SESSION_FAILURE_CANARY_PRIVATE_6203"));
  };
  ava::tests::FakeTransport session_transport({});
  bool central_runtime_persistence_failure_persists_the_typed_session_terminal_class = false;
  if (unlocked_failed_session_result)
  {
    auto session_result = ava::app::run_prompt(*unlocked_failed_session_result, "session failure", provider, session_transport, session_options);
    if (!session_result)
    {
      SCOPED_CRITICAL_AREA_R(failed_session_r, *unlocked_failed_session_result);
      auto session_failure = ava::diagnostics::read_last_failure_record(session_paths, *failed_session_r->anchor_set());
      central_runtime_persistence_failure_persists_the_typed_session_terminal_class =
          session_failure.record && session_failure.record->failure.component == ava::diagnostics::ComponentClass::Session;
    }
  }
  expect(central_runtime_persistence_failure_persists_the_typed_session_terminal_class,
         "central runtime persistence failure persists the typed session terminal class");

  auto const preserved = read_all(provider_paths.ava_state_dir / "diagnostics" / "last-failure-v1.json");
  auto disabled_observer = std::make_shared<CollectingObserver>();
  provider_options.observation = std::make_shared<ava::observability::RunObservation>(disabled_observer);
  ava::tests::FakeTransport success_transport({sse_response(final_text_sse("ok"))});
  bool subsequent_successful_runtime_does_not_overwrite_the_latest_failure = false;
  if (unlocked_provider_session_result)
  {
    auto success = ava::app::run_prompt(*unlocked_provider_session_result, "success", provider, success_transport, provider_options);
    if (success)
      subsequent_successful_runtime_does_not_overwrite_the_latest_failure =
          read_all(provider_paths.ava_state_dir / "diagnostics" / "last-failure-v1.json") == preserved;
  }
  expect(subsequent_successful_runtime_does_not_overwrite_the_latest_failure, "a subsequent successful runtime does not overwrite the latest failure");
  expect(disabled_observer->events() > 0, "disabled production diagnostics preserves an explicitly supplied programmatic observation");

  if (tool_diagnostics)
    (*tool_diagnostics)->close();
  std::filesystem::remove_all(canceled_root);
  std::filesystem::remove_all(provider_root);
  std::filesystem::remove_all(tool_root);
  std::filesystem::remove_all(session_root);
}

void test_long_delta_stream_preserves_boundaries_and_close_flush()
{
  using namespace ava::observability;
  auto const root = diagnostics_root("runtime-diagnostics-deltas");
  auto const paths = private_paths(root);
  auto const anchors = private_anchors(paths);
  auto diagnostics = ava::diagnostics::RuntimeDiagnostics::create(paths, anchors, true);
  expect(diagnostics.has_value(), "delta aggregation diagnostics starts");
  if (!diagnostics)
    return;
  auto const path = *(*diagnostics)->trace_path();
  auto observation = (*diagnostics)->observation();
  TraceContext context{
      .run_id = "long-run", .turn_id = {}, .session_id = {}, .provider_id = {}, .parent_run_id = {}, .parent_turn_id = {}, .parent_session_id = {}};
  observation->emit(TraceEventType::AgentRunStart, context);
  auto delta = [&](TraceOutcome outcome) {
    observation->emit(TraceEventType::ProviderStreamEvent, context, [outcome](auto& event) {
      event.phase = TracePhase::Provider;
      event.outcome = outcome;
      event.fields = {{.key = "text_bytes", .value = "3"}};
      event.fields.push_back({.key = "text_bytes", .value = "987654321", .provenance = FieldProvenance::Secret});
    });
  };
  for (unsigned i = 0; i < 25'000; ++i)
    delta(TraceOutcome::TextDelta);
  observation->emit(TraceEventType::ToolDispatchStart, context);
  observation->emit(TraceEventType::ToolDispatchResult, context);
  observation->emit(TraceEventType::AgentRunTerminal, context);
  delta(TraceOutcome::ReasoningDelta);
  (*diagnostics)->close();
  (*diagnostics)->close();
  auto const trace = read_all(path);
  auto const records = lines(trace);
  expect(records.size() == 6 && records[1].find("\"delta_count\":\"25000\"") != std::string::npos &&
             records[1].find("\"text_bytes\":\"75000\"") != std::string::npos && records[2].find("tool.dispatch_start") != std::string::npos &&
             records[3].find("tool.dispatch_result") != std::string::npos && records[4].find("agent.run_terminal") != std::string::npos &&
             records[5].find("\"delta_count\":\"1\"") != std::string::npos,
         "long stream becomes one ordered summary, preserving tool/terminal events and flushing the final segment on close");
  auto const snapshot = ava::diagnostics::read_trace_counter_snapshot(paths, *anchors);
  expect(snapshot.record && snapshot.record->writer_health.events_written == 6 && snapshot.record->writer_health.events_dropped == 0,
         "aggregation is not counted as loss and repeated close does not double-count");
  std::filesystem::remove_all(root);
}

void test_external_tool_attribution_survives_completed_dispatches()
{
  using namespace ava::observability;
  auto const root = diagnostics_root("runtime-diagnostics-attribution");
  auto const paths = private_paths(root);
  auto const anchors = private_anchors(paths);
  auto diagnostics = ava::diagnostics::RuntimeDiagnostics::create(paths, anchors, true);
  expect(diagnostics.has_value(), "attribution diagnostics starts");
  if (!diagnostics)
    return;
  auto observation = (*diagnostics)->observation();
  TraceContext context;
  for (unsigned i = 0; i < 20'002; ++i)
  {
    auto const call = std::to_string(i);
    observation->emit(TraceEventType::ToolDispatchStart, context, [&](auto& event) {
      event.call_id = call;
      event.fields = {{.key = "tool_name", .value = i == 20'001 ? "plugin_demo_echo" : "mcp_demo_echo"}};
    });
    observation->emit(TraceEventType::ToolDispatchResult, context, [&](auto& event) {
      event.call_id = call;
      event.outcome = i < 20'000 ? (i % 2 == 0 ? TraceOutcome::Success : TraceOutcome::Canceled) : TraceOutcome::Error;
    });
  }
  (*diagnostics)->close();
  auto const snapshot = ava::diagnostics::read_trace_counter_snapshot(paths, *anchors);
  expect(snapshot.record && snapshot.record->mcp_failures == 1 && snapshot.record->plugin_failures == 1,
         "successful and canceled dispatches release attribution capacity even after trace admission stops");
  std::filesystem::remove_all(root);
}

void test_delta_segment_budget_reserves_lifecycle_capacity()
{
  using namespace ava::observability;
  auto const root = diagnostics_root("runtime-diagnostics-segments");
  auto const paths = private_paths(root);
  auto const anchors = private_anchors(paths);
  auto diagnostics = ava::diagnostics::RuntimeDiagnostics::create(paths, anchors, true);
  expect(diagnostics.has_value(), "segmented stream diagnostics starts");
  if (!diagnostics)
    return;
  auto const path = *(*diagnostics)->trace_path();
  auto observation = (*diagnostics)->observation();
  TraceContext context{
      .run_id = "segmented-run", .turn_id = {}, .session_id = {}, .provider_id = {}, .parent_run_id = {}, .parent_turn_id = {}, .parent_session_id = {}};
  observation->emit(TraceEventType::AgentRunStart, context);
  for (unsigned i = 0; i < 20'000; ++i)
    observation->emit(TraceEventType::ProviderStreamEvent, context, [i](auto& event) {
      event.outcome = i % 2 == 0 ? TraceOutcome::TextDelta : TraceOutcome::ReasoningDelta;
      event.fields = {{.key = "text_bytes", .value = "1"}};
    });
  observation->emit(TraceEventType::ToolDispatchStart, context);
  observation->emit(TraceEventType::ToolDispatchResult, context);
  observation->emit(TraceEventType::AgentRunTerminal, context);
  (*diagnostics)->close();
  auto const records = lines(read_all(path));
  auto const snapshot = ava::diagnostics::read_trace_counter_snapshot(paths, *anchors);
  expect(records.size() == 1'004 && records.back().find("agent.run_terminal") != std::string::npos &&
             records[1'001].find("tool.dispatch_start") != std::string::npos && records[1'002].find("tool.dispatch_result") != std::string::npos &&
             snapshot.record && snapshot.record->writer_health.events_written == 1'004 && snapshot.record->writer_health.events_dropped == 0 &&
             snapshot.record->writer_health.delta_summaries_refused == 19'000,
         "delta summaries use only their reserved budget and intentional refusals are distinct from lifecycle event loss");
  std::filesystem::remove_all(root);
}

}  // namespace

void run_runtime_diagnostics_tests()
{
  test_disabled_runtime_diagnostics_is_artifact_free();
  test_private_trace_sanitization_limits_and_counters();
  test_trace_writer_retains_anchored_inode_after_path_replacement();
  test_concurrent_traces_are_distinct_and_counters_are_cumulative();
  test_writer_health_conversion_does_not_double_count_failures();
  test_trace_initialization_rejects_unsafe_paths_nonblocking();
  test_last_failure_is_typed_and_message_free();
  test_runtime_failure_boundaries_and_observation_precedence();
  test_long_delta_stream_preserves_boundaries_and_close_flush();
  test_external_tool_attribution_survives_completed_dispatches();
  test_delta_segment_budget_reserves_lifecycle_capacity();
}
