#include "sys.h"
#include "tests/support/process_test_support.h"
#include "tests/support/session_test_support.h"
#include "tests/support/test_env.h"
#include "tests/support/test_harness.h"
#include "ava/process/environment.h"
#include "ava/process/scope.h"
#include "ava/process/supervisor.h"
#include "ava/process/supervisor_test_support.h"
#include "ava/plugin/discovery.h"
#include "ava/plugin/runner.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef AVA_FAKE_PLUGIN_CHILD_PATH
#define AVA_FAKE_PLUGIN_CHILD_PATH ""
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct TestAuthority
{
  std::shared_ptr<ava::process::Supervisor> supervisor;
  ava::process::ProcessScopeV1 application;
  ava::process::ProcessScopeV1 session;
  ava::process::ProcessScopeV1 run;
};

std::optional<TestAuthority> make_authority()
{
  auto supervisor = std::make_shared<ava::process::Supervisor>();
  auto application = ava::process::ProcessScopeV1::application(supervisor);
  if (!application)
  {
    expect(false, "plugin process test application scope: " + application.error().format());
    return std::nullopt;
  }
  auto session = application->session();
  if (!session)
  {
    expect(false, "plugin process test session scope: " + session.error().format());
    return std::nullopt;
  }
  auto run = session->run();
  if (!run)
  {
    expect(false, "plugin process test run scope: " + run.error().format());
    return std::nullopt;
  }
  return TestAuthority{.supervisor = std::move(supervisor), .application = std::move(*application), .session = std::move(*session), .run = std::move(*run)};
}

ScopedTestDirectory test_root(std::string_view label)
{
  return ScopedTestDirectory::create_unique(temp_root(), "plugin-process-" + std::string(label) + "-");
}

void write_text(std::filesystem::path const& path, std::string_view text)
{
  std::filesystem::create_directories(path.parent_path());
  ava::tests::write_test_file(path, text);
}

std::string read_text(std::filesystem::path const& path)
{
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string first_line(std::filesystem::path const& path)
{
  auto text = read_text(path);
  auto const newline = text.find('\n');
  if (newline != std::string::npos)
    text.resize(newline);
  return text;
}

void copy_executable(std::filesystem::path const& destination)
{
  std::filesystem::create_directories(destination.parent_path());
  std::filesystem::copy_file(AVA_FAKE_PLUGIN_CHILD_PATH, destination, std::filesystem::copy_options::overwrite_existing);
  std::filesystem::permissions(destination, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
}

bool wait_for_path(std::filesystem::path const& path, std::chrono::milliseconds timeout = 2s)
{
  auto const deadline = ava::tests::now_plus_seconds(timeout);
  while (Clock::now() < deadline)
  {
    if (std::filesystem::exists(path))
      return true;
    std::this_thread::sleep_for(10ms);
  }
  return std::filesystem::exists(path);
}

// The fake child creates its marker before flushing the PID record, so path
// existence alone can race the write. A complete marker is one newline-terminated
// decimal PID line.
bool pid_marker_complete(std::filesystem::path const& path)
{
  auto const text = read_text(path);
  if (text.size() < 2 || text.back() != '\n')
    return false;
  return std::string_view(text).substr(0, text.size() - 1).find_first_not_of("0123456789") == std::string_view::npos;
}

bool wait_for_pid_marker(std::filesystem::path const& path, std::chrono::milliseconds timeout = 2s)
{
  auto const deadline = ava::tests::now_plus_seconds(timeout);
  while (Clock::now() < deadline)
  {
    if (pid_marker_complete(path))
      return true;
    std::this_thread::sleep_for(10ms);
  }
  return pid_marker_complete(path);
}

pid_t process_identity(std::string_view identity)
{
  pid_t pid = -1;
  auto const parsed = std::from_chars(identity.data(), identity.data() + identity.size(), pid);
  return parsed.ec == std::errc{} && parsed.ptr == identity.data() + identity.size() && pid > 0 ? pid : -1;
}

ava::plugin::PluginManifest fake_manifest(std::filesystem::path const& directory, std::string scenario, std::filesystem::path const& marker = {})
{
  std::filesystem::create_directories(directory);
  ava::plugin::PluginManifest manifest;
  manifest.schema_version = 1;
  manifest.id = "com.example.process";
  manifest.name = "Process fixture";
  manifest.version = "1.0.0";
  manifest.api_version = "ava.plugin.v1";
  manifest.entrypoint.command = AVA_FAKE_PLUGIN_CHILD_PATH;
  manifest.entrypoint.args.push_back(std::move(scenario));
  if (!marker.empty())
    manifest.entrypoint.args.push_back(marker.string());
  manifest.path = directory / "plugin.json";
  manifest.directory = directory;
  return manifest;
}

ava::plugin::PluginRunnerOptions options_for(std::filesystem::path const& workspace, ava::process::ProcessScopeV1 const& scope,
                                             std::chrono::milliseconds startup_timeout = 500ms, std::chrono::milliseconds request_timeout = 500ms)
{
  return ava::plugin::PluginRunnerOptions{.workspace_dir = workspace,
                                          .startup_timeout = startup_timeout,
                                          .request_timeout = request_timeout,
                                          .max_record_bytes = 64 * 1024,
                                          .max_stderr_bytes = 64 * 1024,
                                          .process_scope = scope};
}

std::vector<ava::process::ProcessSnapshotRecordV1> plugin_records(ava::process::Supervisor const& supervisor)
{
  auto snapshot = supervisor.snapshot();
  std::vector<ava::process::ProcessSnapshotRecordV1> records;
  for (auto const& record : snapshot.records)
  {
    if (record.role == ava::process::ProcessRoleV1::Plugin)
      records.push_back(record);
  }
  return records;
}

bool completely_settled(ava::process::ProcessSnapshotRecordV1 const& record)
{
  return record.state == ava::process::ProcessStateV1::Finished && record.cleanup == ava::process::CleanupStateV1::Complete && record.settlement_count == 1;
}

bool wait_for_plugin_settlement(ava::process::Supervisor& supervisor)
{
  auto const deadline = ava::tests::now_plus_seconds(10);
  while (Clock::now() < deadline)
  {
    auto const cycle = ava::process::testing::SupervisorTestAccess::monitor_cycle(supervisor);
    auto const records = plugin_records(supervisor);
    if (records.size() == 1 && completely_settled(records.front()))
      return true;
    static_cast<void>(ava::process::testing::SupervisorTestAccess::wait_for_monitor_cycle(supervisor, cycle, deadline));
  }
  auto const records = plugin_records(supervisor);
  return records.size() == 1 && completely_settled(records.front());
}

ava::core::VoidResult shutdown_for_test(ava::plugin::PluginProcess& process)
{
  return process.shutdown(2s);
}

void finish_supervisor(TestAuthority& authority)
{
  authority.supervisor->stop_accepting();
  auto const result = authority.supervisor->shutdown(ava::tests::now_plus_seconds(2s));
  expect(result.complete && result.incomplete_count == 0 && authority.supervisor->snapshot().live_records == 0,
         "plugin process test supervisor finishes with no live records");
}

class TestLatch final
{
 public:
  ~TestLatch() { release(); }

  void arrive_and_wait()
  {
    std::unique_lock lock(mutex_);
    reached_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_; });
  }

  bool wait_until(Clock::time_point deadline)
  {
    std::unique_lock lock(mutex_);
    return changed_.wait_until(lock, deadline, [&] { return reached_; });
  }

  void release() noexcept
  {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool reached_ = false;
  bool released_ = false;
};

void test_compatibility_environment_bare_command_and_idempotence()
{
  auto authority = make_authority();
  if (!ava::tests::require(authority.has_value(), "plugin fixture creates authority"))
    return;
  auto const root = test_root("compatibility");
  auto const workspace = root / "workspace";
  auto const plugin_dir = root / "plugin";
  std::filesystem::create_directories(workspace);

  auto process = ava::plugin::PluginProcess::start(fake_manifest(plugin_dir, "normal"), options_for(workspace, authority->run));
  expect(process && (*process)->initialization().api_version == "ava.plugin.v1" && (*process)->initialization().plugin_version == "1.2.3",
         process ? "supervised plugin initialize remains protocol-compatible" : "supervised plugin initialize: " + process.error().format());
  if (process)
  {
    auto called = (*process)->call_tool("tool", "{}", "compat");
    expect(called && called->ok && called->content == "compatible", "supervised plugin call remains JSONL-compatible");
    auto first = shutdown_for_test(**process);
    auto second = shutdown_for_test(**process);
    expect(first && second, "plugin shutdown is successful and idempotent");
  }
  auto records = plugin_records(*authority->supervisor);
  expect(records.size() == 1 && completely_settled(records.front()), "compatibility plugin record settles exactly once with complete cleanup");

  ScopedEnvVar home("HOME", "CANARY_PLUGIN_HOME");
  ScopedEnvVar proxy("HTTPS_PROXY", "CANARY_PLUGIN_PROXY");
  ScopedEnvVar credential("OPENAI_API_KEY", "CANARY_PLUGIN_CREDENTIAL");
  ScopedEnvVar loader("LD_PRELOAD", "CANARY_PLUGIN_LOADER");
  ScopedEnvVar agent("SSH_AUTH_SOCK", "CANARY_PLUGIN_AGENT");
  ScopedEnvVar private_token("AVA_PRIVATE_TOKEN", "CANARY_PLUGIN_AVA");
  auto environment_authority = make_authority();
  if (!ava::tests::require(environment_authority.has_value(), "plugin fixture creates environment_authority"))
    return;
  auto const environment_file = root / "environment.txt";
  auto environment_process =
      ava::plugin::PluginProcess::start(fake_manifest(plugin_dir, "environment", environment_file), options_for(workspace, environment_authority->run));
  expect(environment_process.has_value(),
         environment_process ? "plugin exact environment fixture initializes" : "plugin exact environment fixture: " + environment_process.error().format());
  if (environment_process)
  {
    auto called = (*environment_process)->call_tool("tool", "{}", "compat");
    auto shutdown = shutdown_for_test(**environment_process);
    expect(called && shutdown, "plugin exact environment fixture completes");
  }
  auto const child_environment = read_text(environment_file);
  auto const expected_environment =
      "PATH=" + std::string(ava::process::kTrustedEnvironmentPathV1) + "\nLANG=C.UTF-8\nLC_ALL=C.UTF-8\nPWD=" + plugin_dir.string() + "\n";
  expect(child_environment == expected_environment, "plugin receives only the exact minimal environment in canonical order");
  expect(std::string(std::getenv("HOME")) == "CANARY_PLUGIN_HOME" && std::string(std::getenv("HTTPS_PROXY")) == "CANARY_PLUGIN_PROXY" &&
             std::string(std::getenv("OPENAI_API_KEY")) == "CANARY_PLUGIN_CREDENTIAL" && std::string(std::getenv("LD_PRELOAD")) == "CANARY_PLUGIN_LOADER" &&
             std::string(std::getenv("SSH_AUTH_SOCK")) == "CANARY_PLUGIN_AGENT" && std::string(std::getenv("AVA_PRIVATE_TOKEN")) == "CANARY_PLUGIN_AVA",
         "plugin launch does not mutate its parent environment");
  finish_supervisor(*environment_authority);

  auto bare_authority = make_authority();
  if (!ava::tests::require(bare_authority.has_value(), "plugin fixture creates bare_authority"))
    return;
  auto bare = fake_manifest(plugin_dir, "normal");
  bare.entrypoint.command = "sh";
  bare.entrypoint.args = {"-c",
                          "IFS= read -r init; printf '%s\\n' '{\"id\":\"ava_1\",\"type\":\"initialized\",\"api_version\":\"ava.plugin.v1\","
                          "\"plugin_version\":\"1.0\",\"contributions\":{}}'; IFS= read -r request; printf '%s\\n' "
                          "'{\"id\":\"ava_tool_compat\",\"type\":\"tool.result\",\"ok\":true,\"content\":\"bare\"}'; while IFS= read -r line; do :; done"};
  auto bare_process = ava::plugin::PluginProcess::start(std::move(bare), options_for(workspace, bare_authority->run));
  auto bare_call = bare_process ? (*bare_process)->call_tool("tool", "{}", "compat")
                                : ava::core::Result<ava::plugin::PluginToolCallResult>(std::unexpected(bare_process.error()));
  auto bare_shutdown = bare_process ? shutdown_for_test(**bare_process) : ava::core::VoidResult(std::unexpected(bare_process.error()));
  expect(bare_call && bare_call->content == "bare" && bare_shutdown, "bare plugin command resolves through the exact trusted PATH");
  finish_supervisor(*bare_authority);
  finish_supervisor(*authority);
}

void test_entrypoint_path_forms_preserve_lexical_resolution_and_argv0()
{
  auto authority = make_authority();
  if (!ava::tests::require(authority.has_value(), "plugin fixture creates authority"))
    return;
  auto const root = test_root("entrypoint-forms");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);

  struct Form
  {
    std::string label;
    std::string command;
    std::filesystem::path executable;
  };
  auto const absolute = std::filesystem::path(AVA_FAKE_PLUGIN_CHILD_PATH);
  std::vector<Form> const forms{
      {.label = "absolute", .command = absolute.string(), .executable = absolute},
      {.label = "dot-relative", .command = "./plugin-child", .executable = root / "dot-relative" / "plugin-child"},
      {.label = "nested-relative", .command = "bin/plugin-child", .executable = root / "nested-relative" / "bin" / "plugin-child"},
      {.label = "parent-relative", .command = "../shared/plugin-child", .executable = root / "shared" / "plugin-child"},
  };

  for (auto const& form : forms)
  {
    auto const plugin_dir = root / form.label;
    auto const marker = root / (form.label + ".argv0");
    if (form.label != "absolute")
      copy_executable(form.executable);
    auto manifest = fake_manifest(plugin_dir, "argv0", marker);
    manifest.entrypoint.command = form.command;
    auto process = ava::plugin::PluginProcess::start(std::move(manifest), options_for(workspace, authority->run));
    auto shutdown = process ? shutdown_for_test(**process) : ava::core::VoidResult(std::unexpected(process.error()));
    expect(process && shutdown && first_line(marker) == form.command,
           form.label + " plugin entrypoint preserves the manifest spelling as argv[0] while resolving only slash-containing relatives against cwd");
  }

  auto records = plugin_records(*authority->supervisor);
  expect(records.size() == forms.size() && std::ranges::all_of(records, completely_settled),
         "absolute, ./, nested, and existing ../ plugin entrypoint forms settle under supervision without a shell");
  finish_supervisor(*authority);
}

void test_prelaunch_authority_cancel_and_discovery_are_process_free()
{
  auto authority = make_authority();
  if (!ava::tests::require(authority.has_value(), "plugin fixture creates authority"))
    return;
  auto const root = test_root("prelaunch");
  auto const workspace = root / "workspace";
  auto const plugin_dir = workspace / ".ava" / "plugins" / "com.example.discovery";
  auto const marker = root / "should-not-exist";
  std::filesystem::create_directories(workspace);
  write_text(plugin_dir / "plugin.json",
             "{\"schema_version\":1,\"id\":\"com.example.discovery\",\"name\":\"Discovery\",\"version\":\"1\","
             "\"api_version\":\"ava.plugin.v1\",\"entrypoint\":{\"command\":\"/bin/sh\",\"args\":[\"-c\",\"touch should-not-run\"]}}\n");
  auto discovered = ava::plugin::discover_plugins({.global_plugins_dir = root / "global", .project_plugins_dir = workspace / ".ava" / "plugins"});
  auto initial = authority->supervisor->snapshot();
  expect(discovered && discovered->size() == 1 && !initial.monitor_started && initial.live_records == 0 && initial.records.empty(),
         "plugin discovery and basic scope startup create no process or monitor");

  auto missing_options = options_for(workspace, authority->run);
  missing_options.process_scope.reset();
  auto missing = ava::plugin::PluginProcess::start(fake_manifest(root / "missing", "normal", marker), missing_options);
  auto after_missing = authority->supervisor->snapshot();
  expect(!missing && missing.error().message().find("authority is required") != std::string::npos && !after_missing.monitor_started &&
             after_missing.live_records == 0 && after_missing.records.empty() && !std::filesystem::exists(marker),
         "missing plugin scope fails before reservation and spawn");

  auto operation = authority->run.operation();
  expect(operation.has_value(), "plugin derivation-failure fixture creates an already-terminal owner");
  if (operation)
  {
    auto invalid_parent = ava::plugin::PluginProcess::start(fake_manifest(root / "invalid-parent", "normal", marker), options_for(workspace, *operation));
    auto after_invalid_parent = authority->supervisor->snapshot();
    expect(!invalid_parent && invalid_parent.error().message().find("derive plugin operation") != std::string::npos && !after_invalid_parent.monitor_started &&
               after_invalid_parent.live_records == 0 && after_invalid_parent.records.empty(),
           "plugin operation-scope derivation failure is actionable and precedes reservation");
  }

  auto invalid_record_max = options_for(workspace, authority->run);
  invalid_record_max.max_record_bytes = ava::plugin::kPluginRunnerMaxRecordBytes + 1;
  invalid_record_max.process_scope.reset();
  auto over_record = ava::plugin::PluginProcess::start(fake_manifest(root / "over-record", "argv0", marker), invalid_record_max);
  auto invalid_record_zero = options_for(workspace, authority->run);
  invalid_record_zero.max_record_bytes = 0;
  invalid_record_zero.process_scope.reset();
  auto zero_record = ava::plugin::PluginProcess::start(fake_manifest(root / "zero-record", "argv0", marker), invalid_record_zero);
  auto invalid_stderr_max = options_for(workspace, authority->run);
  invalid_stderr_max.max_stderr_bytes = ava::plugin::kPluginRunnerMaxStderrBytes + 1;
  invalid_stderr_max.process_scope.reset();
  auto over_stderr = ava::plugin::PluginProcess::start(fake_manifest(root / "over-stderr", "argv0", marker), invalid_stderr_max);
  auto invalid_stderr_zero = options_for(workspace, authority->run);
  invalid_stderr_zero.max_stderr_bytes = 0;
  invalid_stderr_zero.process_scope.reset();
  auto zero_stderr = ava::plugin::PluginProcess::start(fake_manifest(root / "zero-stderr", "argv0", marker), invalid_stderr_zero);
  auto after_invalid_limits = authority->supervisor->snapshot();
  auto invalid_limit_error = [](auto const& result) { return !result && result.error().message().find("byte limits are out of bounds") != std::string::npos; };
  expect(invalid_limit_error(over_record) && invalid_limit_error(zero_record) && invalid_limit_error(over_stderr) && invalid_limit_error(zero_stderr) &&
             !after_invalid_limits.monitor_started && after_invalid_limits.live_records == 0 && after_invalid_limits.records.empty(),
         "zero and over-hard-max record and stderr limits fail before process-scope validation or reservation");

  auto canceled =
      ava::plugin::PluginProcess::start(fake_manifest(root / "canceled", "argv0", marker), options_for(workspace, authority->run), [] { return true; });
  auto after_cancel = authority->supervisor->snapshot();
  expect(!canceled && canceled.error().code() == ava::core::ErrorCode::Canceled && canceled.error().message().find("canceled") != std::string::npos &&
             !after_cancel.monitor_started && after_cancel.live_records == 0 && after_cancel.records.empty() && !std::filesystem::exists(marker),
         "pre-canceled plugin startup creates no child or process record");

  std::atomic_size_t cancellation_observations = 0;
  auto reserved_cancel = ava::plugin::PluginProcess::start(fake_manifest(root / "reserved-cancel", "argv0", marker), options_for(workspace, authority->run),
                                                           [&] { return cancellation_observations.fetch_add(1) + 1 >= 3; });
  auto after_reserved_cancel = authority->supervisor->snapshot();
  auto reserved_records = plugin_records(*authority->supervisor);
  expect(!reserved_cancel && reserved_cancel.error().message().find("plugin startup canceled") != std::string::npos &&
             reserved_cancel.error().format().find("canceled: true") != std::string::npos &&
             reserved_cancel.error().format().find("reason: canceled") != std::string::npos && cancellation_observations == 3 &&
             !after_reserved_cancel.monitor_started && after_reserved_cancel.live_records == 0 && reserved_records.size() == 1 &&
             reserved_records.front().state == ava::process::ProcessStateV1::Finished &&
             reserved_records.front().reason == ava::process::TerminationReasonV1::Canceled &&
             reserved_records.front().cleanup == ava::process::CleanupStateV1::NotRequired && reserved_records.front().settlement_count == 1 &&
             !std::filesystem::exists(marker),
         "cancellation after Plugin reservation settles Canceled exactly once without a child or monitor");
  finish_supervisor(*authority);
}

void test_failure_reason_mapping_and_output_accounting()
{
  auto run_case = [](std::string_view label, std::string scenario, std::chrono::milliseconds startup_timeout, std::chrono::milliseconds request_timeout,
                     std::size_t record_limit, std::size_t stderr_limit, ava::process::TerminationReasonV1 expected_reason, bool call_after_start,
                     bool cancel_call, bool expect_start_success) {
    auto authority = make_authority();
    if (!ava::tests::require(authority.has_value(), "plugin fixture creates authority"))
      return;
    auto const root = test_root(label);
    auto const workspace = root / "workspace";
    auto const marker = root / "marker";
    std::filesystem::create_directories(workspace);
    auto options = options_for(workspace, authority->run, startup_timeout, request_timeout);
    options.max_record_bytes = record_limit;
    options.max_stderr_bytes = stderr_limit;
    auto process = ava::plugin::PluginProcess::start(fake_manifest(root / "plugin", std::move(scenario), marker), options);
    expect(static_cast<bool>(process) == expect_start_success, std::string(label) + " reaches its expected startup boundary");
    if (process && call_after_start)
    {
      auto called = (*process)->call_tool(
          "tool", "{}", "compat", cancel_call ? ava::plugin::CancelCallback([marker] { return wait_for_path(marker, 10ms); }) : ava::plugin::CancelCallback{});
      expect(!called, std::string(label) + " returns a bounded call failure");
      auto shutdown = (*process)->shutdown(100ms);
      expect(shutdown.has_value(), std::string(label) + " failure cleanup remains explicitly observable as complete");
    }
    auto records = plugin_records(*authority->supervisor);
    expect(records.size() == 1 && completely_settled(records.front()) && records.front().reason == expected_reason,
           std::string(label) + " commits the expected first termination reason and settles once");
    if (label == "oversized-stdout")
      expect(records.front().stdout_truncated && records.front().stdout_bytes >= 8192, "oversized plugin stdout is fully accounted and marked truncated");
    finish_supervisor(*authority);
  };

  run_case("startup-timeout", "startup-hang", 50ms, 500ms, 64 * 1024, 64 * 1024, ava::process::TerminationReasonV1::DeadlineExpired, false, false, false);
  run_case("request-timeout", "request-hang", 500ms, 50ms, 64 * 1024, 64 * 1024, ava::process::TerminationReasonV1::DeadlineExpired, true, false, true);
  run_case("request-cancel", "request-hang", 500ms, 2s, 64 * 1024, 64 * 1024, ava::process::TerminationReasonV1::Canceled, true, true, true);
  run_case("malformed-framing", "malformed", 500ms, 500ms, 64 * 1024, 64 * 1024, ava::process::TerminationReasonV1::ProtocolFailure, true, false, true);
  run_case("oversized-stdout", "oversized-initialize", 500ms, 500ms, 256, 64 * 1024, ava::process::TerminationReasonV1::OutputLimit, false, false, false);

  auto stderr_authority = make_authority();
  if (!ava::tests::require(stderr_authority.has_value(), "plugin fixture creates stderr_authority"))
    return;
  auto const root = test_root("oversized-stderr");
  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  auto options = options_for(workspace, stderr_authority->run);
  options.max_stderr_bytes = 64;
  auto process = ava::plugin::PluginProcess::start(fake_manifest(root / "plugin", "stderr-large"), options);
  expect(process && (*process)->stderr_truncated() && (*process)->stderr_tail().size() == 64, "oversized plugin stderr keeps only its bounded tail");
  if (process)
  {
    auto shutdown = shutdown_for_test(**process);
    expect(shutdown.has_value(), "stderr truncation does not change protocol success");
  }
  auto records = plugin_records(*stderr_authority->supervisor);
  expect(records.size() == 1 && completely_settled(records.front()) && records.front().stderr_truncated && records.front().stderr_bytes >= 8192,
         "every stderr byte is supervisor-accounted and truncation is recorded");
  finish_supervisor(*stderr_authority);
}

void test_outbound_and_queued_protocol_memory_bounds()
{
  auto outbound_authority = make_authority();
  if (!ava::tests::require(outbound_authority.has_value(), "plugin fixture creates outbound_authority"))
    return;
  auto const outbound_root = test_root("outbound-limit");
  auto const outbound_marker = outbound_root / "request-observed";
  std::filesystem::create_directories(outbound_root / "workspace");
  auto outbound_options = options_for(outbound_root / "workspace", outbound_authority->run);
  outbound_options.max_record_bytes = 512;
  auto outbound_process = ava::plugin::PluginProcess::start(fake_manifest(outbound_root / "plugin", "request-marker", outbound_marker), outbound_options);
  std::string oversized_arguments = "{\"value\":\"" + std::string(1024, 'x') + "\"}";
  auto outbound_call = outbound_process ? (*outbound_process)->call_tool("tool", oversized_arguments, "oversized")
                                        : ava::core::Result<ava::plugin::PluginToolCallResult>(std::unexpected(outbound_process.error()));
  auto outbound_shutdown = outbound_process ? (*outbound_process)->shutdown() : ava::core::VoidResult(std::unexpected(outbound_process.error()));
  auto outbound_records = plugin_records(*outbound_authority->supervisor);
  expect(!outbound_call && outbound_call.error().format().find("output_limit: true") != std::string::npos && outbound_shutdown &&
             !std::filesystem::exists(outbound_marker) && outbound_records.size() == 1 && completely_settled(outbound_records.front()) &&
             outbound_records.front().reason == ava::process::TerminationReasonV1::OutputLimit,
         "oversized outbound JSONL is rejected before frame allocation or write and settles OutputLimit once");
  finish_supervisor(*outbound_authority);

  auto flood_authority = make_authority();
  if (!ava::tests::require(flood_authority.has_value(), "plugin fixture creates flood_authority"))
    return;
  auto const flood_root = test_root("short-line-flood");
  std::filesystem::create_directories(flood_root / "workspace");
  auto flood_options = options_for(flood_root / "workspace", flood_authority->run, 500ms, 2s);
  flood_options.max_record_bytes = ava::plugin::kPluginRunnerMaxRecordBytes;
  auto flood_process = ava::plugin::PluginProcess::start(fake_manifest(flood_root / "plugin", "blocked-stdin-short-line-flood"), flood_options);
  std::string blocked_arguments = "{\"value\":\"" + std::string(512 * 1024, 'x') + "\"}";
  auto flood_call = flood_process ? (*flood_process)->call_tool("tool", blocked_arguments, "flood")
                                  : ava::core::Result<ava::plugin::PluginToolCallResult>(std::unexpected(flood_process.error()));
  auto flood_shutdown = flood_process ? (*flood_process)->shutdown() : ava::core::VoidResult(std::unexpected(flood_process.error()));
  auto flood_records = plugin_records(*flood_authority->supervisor);
  expect(!flood_call && flood_call.error().message().find("queued record count") != std::string::npos &&
             flood_call.error().format().find("output_limit: true") != std::string::npos && flood_shutdown && flood_records.size() == 1 &&
             completely_settled(flood_records.front()) && flood_records.front().reason == ava::process::TerminationReasonV1::OutputLimit &&
             flood_records.front().stdout_bytes > 0 && flood_records.front().stdout_truncated,
         "short-line stdout flood while stdin is blocked hits bounded queued-record OutputLimit with complete accounting and settlement");
  finish_supervisor(*flood_authority);

  auto byte_authority = make_authority();
  if (!ava::tests::require(byte_authority.has_value(), "plugin fixture creates byte_authority"))
    return;
  auto const byte_root = test_root("queued-byte-flood");
  std::filesystem::create_directories(byte_root / "workspace");
  auto byte_options = options_for(byte_root / "workspace", byte_authority->run, 500ms, 2s);
  byte_options.max_record_bytes = 512;
  auto byte_process = ava::plugin::PluginProcess::start(fake_manifest(byte_root / "plugin", "queued-byte-flood"), byte_options);
  auto byte_call = byte_process ? (*byte_process)->call_tool("tool", "{}", "byte-flood")
                                : ava::core::Result<ava::plugin::PluginToolCallResult>(std::unexpected(byte_process.error()));
  auto byte_shutdown = byte_process ? (*byte_process)->shutdown() : ava::core::VoidResult(std::unexpected(byte_process.error()));
  auto byte_records = plugin_records(*byte_authority->supervisor);
  expect(!byte_call && byte_call.error().message().find("queued bytes") != std::string::npos &&
             byte_call.error().format().find("output_limit: true") != std::string::npos && byte_shutdown && byte_records.size() == 1 &&
             completely_settled(byte_records.front()) && byte_records.front().reason == ava::process::TerminationReasonV1::OutputLimit &&
             byte_records.front().stdout_truncated,
         "complete in-bound records stay individually valid while the overflow-safe queued-byte multiplier still settles OutputLimit");
  finish_supervisor(*byte_authority);
}

void test_single_startup_budget_spans_launch_and_initialize()
{
  auto authority = make_authority();
  if (!ava::tests::require(authority.has_value(), "single startup-budget fixture creates an authority"))
    return;
  auto const root = test_root("single-startup-budget");
  auto const marker = root / "initialize-observed";
  std::filesystem::create_directories(root / "workspace");
  bool gate_reached = false;
  // Time is the stimulus, not a readiness guess: launch consumes 1200ms and the
  // fake consumes another 1200ms only after receiving initialize. Each phase
  // fits a fresh 2000ms budget with 800ms slack, but their 2400ms sum cannot fit
  // one budget. Scheduling delay cannot make that sum fit; no elapsed ceiling
  // is asserted. The marker proves expiry occurred in the second phase.
  ava::process::testing::SupervisorTestAccess::set_after_gate_release_hook(*authority->supervisor, [&] {
    gate_reached = true;
    std::this_thread::sleep_for(1200ms);
  });
  auto process =
      ava::plugin::PluginProcess::start(fake_manifest(root / "plugin", "initialize-delay", marker), options_for(root / "workspace", authority->run, 2s, 500ms));
  ava::process::testing::SupervisorTestAccess::clear_after_gate_release_hook(*authority->supervisor);
  auto cleanup = process ? (*process)->shutdown() : ava::core::VoidResult{};
  auto const records = plugin_records(*authority->supervisor);
  ava::tests::expect_all(
      "launch and initialization share one startup budget: " + (process ? std::string("unexpected startup success") : process.error().format()),
      {{"launch gate reached", gate_reached},
       {"initialize received", first_line(marker) == "initialize-observed"},
       {"startup rejected", !process},
       {"unexpected success cleaned", cleanup.has_value()},
       {"one record", records.size() == 1},
       {"complete settlement", records.size() == 1 && completely_settled(records.front())},
       {"deadline first reason", records.size() == 1 && records.front().reason == ava::process::TerminationReasonV1::DeadlineExpired}});
  finish_supervisor(*authority);
}

void test_spawn_cancellation_before_gate_release()
{
  auto cancel_authority = make_authority();
  if (!ava::tests::require(cancel_authority.has_value(), "plugin fixture creates cancel_authority"))
    return;
  auto const cancel_root = test_root("spawn-cancel");
  auto const exec_marker = cancel_root / "exec-observed";
  std::filesystem::create_directories(cancel_root / "workspace");
  auto latch = std::make_shared<TestLatch>();
  std::atomic_bool canceled = false;
  ava::process::testing::SupervisorTestAccess::set_after_fork_before_release_hook(*cancel_authority->supervisor, [latch] { latch->arrive_and_wait(); });
  auto manifest = fake_manifest(cancel_root / "plugin", "argv0", exec_marker);
  auto runner_options = options_for(cancel_root / "workspace", cancel_authority->run, 1s, 500ms);
  auto launch = std::async(std::launch::async, [manifest = std::move(manifest), runner_options = std::move(runner_options), &canceled]() mutable {
    return ava::plugin::PluginProcess::start(std::move(manifest), std::move(runner_options), [&] { return canceled.load(); });
  });
  bool const reached_gate = latch->wait_until(ava::tests::now_plus_seconds(2s));
  canceled = true;
  latch->release();
  bool const completed = launch.wait_until(ava::tests::now_plus_seconds(10)) == std::future_status::ready;
  auto canceled_process = launch.get();
  ava::process::testing::SupervisorTestAccess::clear_after_fork_before_release_hook(*cancel_authority->supervisor);
  auto cancel_snapshot = cancel_authority->supervisor->snapshot();
  auto cancel_records = plugin_records(*cancel_authority->supervisor);
  expect(reached_gate && completed && !canceled_process && canceled_process.error().message().find("plugin startup canceled") != std::string::npos &&
             canceled_process.error().format().find("canceled: true") != std::string::npos &&
             canceled_process.error().format().find("reason: canceled") != std::string::npos && !std::filesystem::exists(exec_marker) &&
             cancel_snapshot.live_records == 0 && cancel_records.size() == 1 && completely_settled(cancel_records.front()) &&
             cancel_records.front().reason == ava::process::TerminationReasonV1::Canceled,
         "parent cancellation during common spawn commits Canceled before gate release and settles completely without exec side effects");
  finish_supervisor(*cancel_authority);
}

void test_exec_confirmation_cancellation_classification()
{
  auto authority = make_authority();
  if (!ava::tests::require(authority.has_value(), "plugin fixture creates authority"))
    return;
  auto const root = test_root("exec-confirmation-cancel");
  auto const pid_marker = root / "leader";
  std::filesystem::create_directories(root / "workspace");
  std::atomic_bool canceled = false;
  std::atomic_bool marker_ready_in_hook = false;
  // The exec'd child publishes its real PID while spawn still owns exec confirmation;
  // only after the complete marker record is readable does cancellation become
  // observable. Cancellation still flips on readiness failure so cleanup stays bounded.
  ava::process::testing::SupervisorTestAccess::set_after_gate_release_hook(*authority->supervisor, [&] {
    marker_ready_in_hook = wait_for_pid_marker(pid_marker);
    canceled = true;
  });
  auto process = ava::plugin::PluginProcess::start(fake_manifest(root / "plugin", "live-destructor", pid_marker),
                                                   options_for(root / "workspace", authority->run, 2s, 500ms), [&] { return canceled.load(); });
  ava::process::testing::SupervisorTestAccess::clear_after_gate_release_hook(*authority->supervisor);
  auto const identity = first_line(pid_marker);
  auto const format = process ? std::string{} : process.error().format();
  auto snapshot = authority->supervisor->snapshot();
  auto records = plugin_records(*authority->supervisor);
  bool const group_reaped = ava::tests::wait_for_process_absent(process_identity(identity), ava::tests::now_plus_seconds(10));
  expect(marker_ready_in_hook.load(), "exec-confirmation cancellation hook observes a complete child PID marker before canceling");
  expect(!process && process.error().message().find("plugin startup canceled") != std::string::npos && format.find("canceled: true") != std::string::npos &&
             format.find("reason: canceled") != std::string::npos,
         "cancellation after exec but before confirmation is classified as plugin startup cancellation: " + format);
  expect(records.size() == 1 && completely_settled(records.front()) && records.front().reason == ava::process::TerminationReasonV1::Canceled &&
             snapshot.live_records == 0,
         "exec-confirmation cancellation settles Canceled exactly once with complete cleanup");
  expect(group_reaped, "exec-confirmation cancellation leaves no live plugin process or group member");
  finish_supervisor(*authority);

  auto stop_authority = make_authority();
  if (!ava::tests::require(stop_authority.has_value(), "plugin fixture creates stop_authority"))
    return;
  auto const stop_root = test_root("owner-stop-before-late-cancel");
  auto const stop_marker = stop_root / "leader";
  std::filesystem::create_directories(stop_root / "workspace");
  std::atomic_bool cancel = false;
  std::atomic_size_t cancel_true_polls = 0;
  std::atomic_bool stop_committed = false;
  // A synchronous owner stop commits the first termination cause once the exec'd
  // child is up; only then does the cancel callback become observable, while spawn
  // still owns exec confirmation and the launch wrapper has not classified anything.
  ava::process::testing::SupervisorTestAccess::set_after_gate_release_hook(*stop_authority->supervisor, [&] {
    if (wait_for_pid_marker(stop_marker))
    {
      auto stopped = stop_authority->supervisor->request_stop(stop_authority->run.owner_prefix(), ava::process::TerminationReasonV1::OwnerShutdown,
                                                              ava::tests::now_plus_seconds(2s));
      stop_committed = stopped && stopped->matched == 1;
    }
    cancel = true;
  });
  auto failed = ava::plugin::PluginProcess::start(fake_manifest(stop_root / "plugin", "live-destructor", stop_marker),
                                                  options_for(stop_root / "workspace", stop_authority->run, 2s, 500ms), [&] {
                                                    bool const value = cancel.load();
                                                    if (value)
                                                      cancel_true_polls.fetch_add(1);
                                                    return value;
                                                  });
  ava::process::testing::SupervisorTestAccess::clear_after_gate_release_hook(*stop_authority->supervisor);
  auto const stop_identity = first_line(stop_marker);
  auto const stop_format = failed ? std::string{} : failed.error().format();
  auto stop_records = plugin_records(*stop_authority->supervisor);
  bool const stop_reaped = ava::tests::wait_for_process_absent(process_identity(stop_identity), ava::tests::now_plus_seconds(10));
  expect(stop_committed && cancel_true_polls > 0, "owner shutdown commits as the first cause and the cancel callback turns true before launch returns");
  expect(!failed && failed.error().message().find("canceled") == std::string::npos && stop_format.find("canceled: true") == std::string::npos &&
             stop_format.find("reason: canceled") == std::string::npos && stop_format.find("reason: owner_shutdown") != std::string::npos,
         "a committed owner shutdown keeps its structured first cause when cancellation arrives before launch returns: " + stop_format);
  expect(stop_records.size() == 1 && completely_settled(stop_records.front()) &&
             stop_records.front().reason == ava::process::TerminationReasonV1::OwnerShutdown && stop_authority->supervisor->snapshot().live_records == 0 &&
             stop_reaped,
         "owner shutdown first cause settles completely once with the child reaped despite late cancellation");
  finish_supervisor(*stop_authority);
}

void test_eof_term_refusal_and_natural_descendant_cleanup()
{
  auto eof_authority = make_authority();
  if (!ava::tests::require(eof_authority.has_value(), "plugin fixture creates eof_authority"))
    return;
  auto const eof_root = test_root("endpoint-eof");
  std::filesystem::create_directories(eof_root / "workspace");
  auto eof_process = ava::plugin::PluginProcess::start(fake_manifest(eof_root / "plugin", "endpoint-eof", eof_root / "leader"),
                                                       options_for(eof_root / "workspace", eof_authority->run));
  expect(eof_process.has_value(), "endpoint EOF fixture initializes before leader exit");
  if (eof_process)
  {
    bool const marker_ready = wait_for_pid_marker(eof_root / "leader");
    bool const absent =
        marker_ready && ava::tests::wait_for_process_absent(process_identity(first_line(eof_root / "leader")), ava::tests::now_plus_seconds(10));
    bool const settled = absent && wait_for_plugin_settlement(*eof_authority->supervisor);
    if (!ava::tests::require(settled, "natural EOF is settled before the competing tool call"))
      return;
    auto called = (*eof_process)->call_tool("tool", "{}", "compat");
    auto shutdown = (*eof_process)->shutdown();
    expect(!called && shutdown, "endpoint EOF is reported without losing complete cleanup");
  }
  auto eof_records = plugin_records(*eof_authority->supervisor);
  expect(eof_records.size() == 1 && completely_settled(eof_records.front()) && eof_records.front().reason == ava::process::TerminationReasonV1::NaturalExit,
         "natural leader EOF wins the first-reason race when already observed");
  finish_supervisor(*eof_authority);

  auto refusal_authority = make_authority();
  if (!ava::tests::require(refusal_authority.has_value(), "plugin fixture creates refusal_authority"))
    return;
  auto const refusal_root = test_root("term-refusal");
  std::filesystem::create_directories(refusal_root / "workspace");
  auto refusal_process = ava::plugin::PluginProcess::start(fake_manifest(refusal_root / "plugin", "shutdown-term-refusal"),
                                                           options_for(refusal_root / "workspace", refusal_authority->run));
  auto refusal_shutdown = refusal_process ? (*refusal_process)->shutdown(2s) : ava::core::VoidResult(std::unexpected(refusal_process.error()));
  auto refusal_records = plugin_records(*refusal_authority->supervisor);
  ava::tests::expect_all(
      "ready TERM-refusing plugin is killed and completely settled by owner shutdown",
      {{"initialized", refusal_process.has_value()},
       {"shutdown", refusal_shutdown.has_value()},
       {"one record", refusal_records.size() == 1},
       {"settled", refusal_records.size() == 1 && completely_settled(refusal_records.front())},
       {"owner shutdown", refusal_records.size() == 1 && refusal_records.front().reason == ava::process::TerminationReasonV1::OwnerShutdown},
       {"SIGKILL observed", refusal_records.size() == 1 && refusal_records.front().has_signal_number && refusal_records.front().signal_number == SIGKILL}});
  finish_supervisor(*refusal_authority);

  auto descendant_authority = make_authority();
  if (!ava::tests::require(descendant_authority.has_value(), "plugin fixture creates descendant_authority"))
    return;
  auto const descendant_root = test_root("leader-first-descendant");
  auto const descendant_marker = descendant_root / "descendant";
  std::filesystem::create_directories(descendant_root / "workspace");
  auto descendant_process = ava::plugin::PluginProcess::start(fake_manifest(descendant_root / "plugin", "descendant", descendant_marker),
                                                              options_for(descendant_root / "workspace", descendant_authority->run, 500ms, 2s));
  expect(descendant_process.has_value(), "leader-first descendant fixture initializes");
  if (descendant_process)
  {
    bool const marker_ready = wait_for_pid_marker(descendant_marker);
    bool const settled = marker_ready && wait_for_plugin_settlement(*descendant_authority->supervisor);
    if (!ava::tests::require(settled, "natural leader-first descendant cleanup settles before the competing tool call"))
      return;
    auto called = (*descendant_process)->call_tool("tool", "{}", "compat");
    auto shutdown = (*descendant_process)->shutdown();
    expect(!called && shutdown, "natural leader-first exit remains a bounded protocol failure to the caller");
  }
  auto const descendant_identity = read_text(descendant_marker);
  auto const trimmed_identity = descendant_identity.empty() ? std::string{} : descendant_identity.substr(0, descendant_identity.find('\n'));
  auto descendant_records = plugin_records(*descendant_authority->supervisor);
  expect(descendant_records.size() == 1 && completely_settled(descendant_records.front()) &&
             descendant_records.front().reason == ava::process::TerminationReasonV1::NaturalExit &&
             ava::tests::wait_for_process_absent(process_identity(trimmed_identity), ava::tests::now_plus_seconds(10)),
         "Supervisor leaves no live same-group TERM-refusing descendant after natural leader exit");
  finish_supervisor(*descendant_authority);
}

void test_live_destructor_and_late_cleanup_error_settlement()
{
  auto destructor_authority = make_authority();
  if (!ava::tests::require(destructor_authority.has_value(), "plugin fixture creates destructor_authority"))
    return;
  auto const destructor_root = test_root("live-destructor");
  auto const destructor_marker = destructor_root / "leader";
  std::filesystem::create_directories(destructor_root / "workspace");
  auto process = ava::plugin::PluginProcess::start(fake_manifest(destructor_root / "plugin", "live-destructor", destructor_marker),
                                                   options_for(destructor_root / "workspace", destructor_authority->run));
  bool const marker_ready = wait_for_pid_marker(destructor_marker);
  auto const identity = marker_ready ? first_line(destructor_marker) : std::string{};
  bool const live_before = ava::tests::process_is_live(process_identity(identity));
  if (process)
    process->reset();
  auto records = plugin_records(*destructor_authority->supervisor);
  expect(marker_ready && live_before && ava::tests::wait_for_process_absent(process_identity(identity), ava::tests::now_plus_seconds(10)) &&
             records.size() == 1 && completely_settled(records.front()) && records.front().reason == ava::process::TerminationReasonV1::OwnerShutdown &&
             destructor_authority->supervisor->snapshot().live_records == 0,
         "destroying a live plugin requests owner shutdown and leaves no runnable child or unsettled authority");
  finish_supervisor(*destructor_authority);

  auto late_authority = make_authority();
  if (!ava::tests::require(late_authority.has_value(), "plugin fixture creates late_authority"))
    return;
  auto const late_root = test_root("late-settlement");
  auto const late_marker = late_root / "leader";
  std::filesystem::create_directories(late_root / "workspace");
  auto late_process = ava::plugin::PluginProcess::start(fake_manifest(late_root / "plugin", "live-destructor", late_marker),
                                                        options_for(late_root / "workspace", late_authority->run));
  if (!late_process || !wait_for_path(late_marker))
  {
    expect(false, "late-settlement fixture starts a live plugin");
    finish_supervisor(*late_authority);
    return;
  }
  auto const late_identity = first_line(late_marker);
  auto monitor_latch = std::make_shared<TestLatch>();
  ava::process::testing::SupervisorTestAccess::set_after_poll_snapshot_hook(*late_authority->supervisor, [monitor_latch] { monitor_latch->arrive_and_wait(); });
  bool const monitor_blocked = monitor_latch->wait_until(ava::tests::now_plus_seconds(2s));
  auto explicit_shutdown = std::async(std::launch::async, [&] { return (*late_process)->shutdown(0ms); });
  bool const error_returned = explicit_shutdown.wait_for(3s) == std::future_status::ready;
  if (!error_returned)
  {
    ava::process::testing::SupervisorTestAccess::clear_after_poll_snapshot_hook(*late_authority->supervisor);
    monitor_latch->release();
  }
  auto shutdown_result = explicit_shutdown.get();
  bool const surfaced_error = !shutdown_result && shutdown_result.error().message().find("cleanup did not settle") != std::string::npos;
  bool const still_live_after_error = ava::tests::process_is_live(process_identity(late_identity));
  ava::process::testing::SupervisorTestAccess::clear_after_poll_snapshot_hook(*late_authority->supervisor);
  monitor_latch->release();
  late_process->reset();
  auto late_snapshot = late_authority->supervisor->snapshot();
  auto late_records = plugin_records(*late_authority->supervisor);
  bool const absent_after_destructor = ava::tests::wait_for_process_absent(process_identity(late_identity), ava::tests::now_plus_seconds(10));
  bool const late_record_settled =
      late_records.size() == 1 && completely_settled(late_records.front()) && late_records.front().reason == ava::process::TerminationReasonV1::OwnerShutdown;
  expect(monitor_blocked && error_returned && surfaced_error && still_live_after_error && absent_after_destructor && late_record_settled &&
             late_authority->supervisor->snapshot().live_records == 0,
         "explicit shutdown surfaces a bounded cleanup error while destruction retries late Supervisor settlement without a runnable-child gap: monitor=" +
             std::to_string(monitor_blocked) + " returned=" + std::to_string(error_returned) + " surfaced=" + std::to_string(surfaced_error) +
             " live=" + std::to_string(still_live_after_error) + " absent=" + std::to_string(absent_after_destructor) +
             " records=" + std::to_string(late_records.size()) + " live_records=" + std::to_string(late_snapshot.live_records) +
             (late_records.empty()
                  ? std::string{}
                  : " state=" + std::to_string(static_cast<int>(late_records.front().state)) +
                        " reason=" + std::to_string(static_cast<int>(late_records.front().reason.value_or(ava::process::TerminationReasonV1::LaunchFailed))) +
                        " cleanup=" + std::to_string(static_cast<int>(late_records.front().cleanup)) +
                        " settlements=" + std::to_string(late_records.front().settlement_count)) +
             " shutdown=" + (shutdown_result ? std::string("success") : shutdown_result.error().format()));
  finish_supervisor(*late_authority);
}

void test_owner_prefix_isolation_for_concurrent_operations()
{
  auto authority = make_authority();
  if (!ava::tests::require(authority.has_value(), "plugin fixture creates authority"))
    return;
  auto run_a = authority->session.run();
  auto run_b = authority->session.run();
  if (!run_a || !run_b)
  {
    expect(false, "concurrent plugin run scopes derive");
    return;
  }
  auto const root = test_root("owner-isolation");
  auto const workspace = root / "workspace";
  auto const marker_a = root / "a.started";
  auto const marker_b = root / "b.started";
  std::filesystem::create_directories(workspace);
  auto process_a = ava::plugin::PluginProcess::start(fake_manifest(root / "plugin-a", "request-hang", marker_a), options_for(workspace, *run_a, 500ms, 5s));
  auto process_b = ava::plugin::PluginProcess::start(fake_manifest(root / "plugin-b", "request-hang", marker_b), options_for(workspace, *run_b, 500ms, 5s));
  expect(process_a && process_b, "concurrent plugin operations initialize under distinct run prefixes");
  if (!process_a || !process_b)
    return;

  auto call_a = std::async(std::launch::async, [&] { return (*process_a)->call_tool("tool", "{}", "compat"); });
  auto call_b = std::async(std::launch::async, [&] { return (*process_b)->call_tool("tool", "{}", "compat"); });
  expect(wait_for_path(marker_a) && wait_for_path(marker_b), "both concurrent plugin operations publish their request boundary");

  auto stopped_a =
      authority->supervisor->request_stop(run_a->owner_prefix(), ava::process::TerminationReasonV1::OwnerShutdown, ava::tests::now_plus_seconds(2s));
  expect(stopped_a && stopped_a->matched == 1, "one run-prefix stop matches only its plugin operation");
  expect(call_a.wait_for(2s) == std::future_status::ready, "stopped plugin operation settles completely");
  auto mid_snapshot = authority->supervisor->snapshot();
  auto const running_b = std::ranges::any_of(mid_snapshot.records, [](auto const& record) {
    return record.role == ava::process::ProcessRoleV1::Plugin && record.state != ava::process::ProcessStateV1::Finished;
  });
  expect(running_b && call_b.wait_for(50ms) == std::future_status::timeout, "sibling plugin operation remains live after isolated prefix stop");

  auto stopped_b =
      authority->supervisor->request_stop(run_b->owner_prefix(), ava::process::TerminationReasonV1::OwnerShutdown, ava::tests::now_plus_seconds(2s));
  expect(stopped_b && stopped_b->matched == 1 && call_b.wait_for(2s) == std::future_status::ready, "second prefix independently settles its operation");
  static_cast<void>(call_a.get());
  static_cast<void>(call_b.get());
  auto shutdown_a = (*process_a)->shutdown();
  auto shutdown_b = (*process_b)->shutdown();
  auto records = plugin_records(*authority->supervisor);
  expect(shutdown_a && shutdown_b && records.size() == 2 && std::ranges::all_of(records, completely_settled) &&
             std::ranges::all_of(records, [](auto const& record) { return record.reason == ava::process::TerminationReasonV1::OwnerShutdown; }) &&
             authority->supervisor->snapshot().live_records == 0,
         "concurrent plugin operations finish once with complete isolated cleanup");
  finish_supervisor(*authority);
}

}  // namespace

void run_plugin_runner_process_tests()
{
  ava::tests::HermeticTestEnvironment environment("plugin-process-");
  if (ava::process::platform_support_v1() != ava::process::PlatformSupportV1::Posix)
  {
    ava::tests::request_skip("plugin Supervisor runner is unsupported on this platform");
    return;
  }
  expect(std::filesystem::is_regular_file(AVA_FAKE_PLUGIN_CHILD_PATH), "plugin process fake fixture is available");
  test_compatibility_environment_bare_command_and_idempotence();
  test_entrypoint_path_forms_preserve_lexical_resolution_and_argv0();
  test_prelaunch_authority_cancel_and_discovery_are_process_free();
  test_failure_reason_mapping_and_output_accounting();
  test_outbound_and_queued_protocol_memory_bounds();
  test_single_startup_budget_spans_launch_and_initialize();
  test_spawn_cancellation_before_gate_release();
  test_exec_confirmation_cancellation_classification();
  test_eof_term_refusal_and_natural_descendant_cleanup();
  test_live_destructor_and_late_cleanup_error_settlement();
  test_owner_prefix_isolation_for_concurrent_operations();
}
