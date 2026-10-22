#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/diagnostics/artifact_store.h"
#include "ava/diagnostics/records.h"
#include "ava/diagnostics/safe_failure.h"
#include "ava/agent/message_builder.h"
#include "ava/session/logical_projection.h"
#include "ava/session/portable_sanitization.h"
#include "ava/core/json.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "debug.h"

namespace {

void test_closed_failure_serializers()
{
  constexpr std::array components{ava::diagnostics::ComponentClass::Mcp, ava::diagnostics::ComponentClass::Plugin};
  constexpr std::array categories{ava::diagnostics::FailureCategory::Configuration, ava::diagnostics::FailureCategory::Transport,
                                  ava::diagnostics::FailureCategory::Authorization, ava::diagnostics::FailureCategory::Protocol,
                                  ava::diagnostics::FailureCategory::Cancellation,  ava::diagnostics::FailureCategory::Internal};
  constexpr std::array codes{ava::diagnostics::FailureCode::InvalidRequest,  ava::diagnostics::FailureCode::IoFailure,
                             ava::diagnostics::FailureCode::NotFound,        ava::diagnostics::FailureCode::PermissionDenied,
                             ava::diagnostics::FailureCode::ExternalFailure, ava::diagnostics::FailureCode::Canceled,
                             ava::diagnostics::FailureCode::InternalFailure};
  constexpr std::array retryability{ava::diagnostics::Retryability::Never, ava::diagnostics::Retryability::AfterUserAction,
                                    ava::diagnostics::Retryability::Transient};
  constexpr std::array hints{ava::diagnostics::RecoveryHint::VerifyRequest, ava::diagnostics::RecoveryHint::VerifyConfiguration,
                             ava::diagnostics::RecoveryHint::VerifyPermissions, ava::diagnostics::RecoveryHint::RetryOperation,
                             ava::diagnostics::RecoveryHint::ContactSupport};

  for (auto const value : components) expect(!ava::diagnostics::to_string(value).empty(), "SafeFailure component enum has a fixed serializer");
  for (auto const value : categories) expect(!ava::diagnostics::to_string(value).empty(), "SafeFailure category enum has a fixed serializer");
  for (auto const value : codes) expect(!ava::diagnostics::to_string(value).empty(), "SafeFailure code enum has a fixed serializer");
  for (auto const value : retryability) expect(!ava::diagnostics::to_string(value).empty(), "SafeFailure retryability enum has a fixed serializer");
  for (auto const value : hints) expect(!ava::diagnostics::to_string(value).empty(), "SafeFailure recovery enum has a fixed serializer");

  auto const failure = ava::diagnostics::external_failure(ava::diagnostics::ComponentClass::Mcp);
  auto const json = ava::diagnostics::serialize_safe_failure_json(failure);
  auto const human = ava::diagnostics::serialize_safe_failure_human(failure);
  expect(ava::core::json::is_valid_object(json) && json.find("\"component\":\"mcp\"") != std::string::npos &&
             json.find("\"code\":\"external_failure\"") != std::string::npos,
         "SafeFailure JSON serializer emits only the stable closed representation");
  expect(human == "MCP integration operation failed [external_failure]. Verify the integration configuration before trying again.",
         "SafeFailure human serializer is stable and fixed");
}

void test_core_error_adapter_discards_raw_text()
{
  constexpr std::string_view canary = "CANARY_DIAGNOSTIC_SECRET_74a9";
  ava::core::Error error(ava::core::ErrorCategory::Io, std::string(canary));
  error.with_context("path", std::string(canary));
  auto const failure = ava::diagnostics::safe_failure_from_error(ava::diagnostics::ComponentClass::Plugin, error);
  auto const json = ava::diagnostics::serialize_safe_failure_json(failure);
  auto const human = ava::diagnostics::serialize_safe_failure_human(failure);
  expect(json.find(canary) == std::string::npos && human.find(canary) == std::string::npos,
         "SafeFailure adapter never serializes Error message or context text");
  expect(failure.code == ava::diagnostics::FailureCode::IoFailure && failure.retryability == ava::diagnostics::Retryability::Transient,
         "SafeFailure adapter classifies only the core ErrorCategory");

  ava::core::Error configuration_error(ava::core::ErrorCategory::Configuration, std::string(canary));
  configuration_error.with_context("config_path", std::string(canary));
  auto const configuration_failure =
      ava::diagnostics::safe_failure_from_error(ava::diagnostics::ComponentClass::Configuration, configuration_error);
  auto const configuration_json = ava::diagnostics::serialize_safe_failure_json(configuration_failure);
  expect(configuration_failure.category == ava::diagnostics::FailureCategory::Configuration &&
             configuration_failure.code == ava::diagnostics::FailureCode::InvalidRequest && configuration_json.find(canary) == std::string::npos,
         "SafeFailure handles Configuration explicitly without serializing message or context text");
}

void test_external_tool_identity_boundary()
{
  auto const mcp = ava::diagnostics::external_tool_component("mcp_demo_echo");
  auto const plugin = ava::diagnostics::external_tool_component("plugin_demo_echo");
  expect(mcp == ava::diagnostics::ComponentClass::Mcp && plugin == ava::diagnostics::ComponentClass::Plugin,
         "reserved external-tool prefixes identify historical integration results");
  expect(!ava::diagnostics::external_tool_component("bash") && !ava::diagnostics::external_tool_component("xmcp_demo"),
         "unrelated built-in tool names are outside the fail-closed integration boundary");
}

void test_historical_external_failure_projection_is_safe()
{
  constexpr std::string_view canary = "CANARY_HISTORICAL_EXTERNAL_FAILURE_93c1";
  ava::session::SessionEntry call{.id = "entry_call",
                                  .parent_id = "",
                                  .type = ava::session::EntryType::ToolCall,
                                  .timestamp = "2026-01-01T00:00:00Z",
                                  .data_json = "{\"call_id\":\"call_external\",\"name\":\"mcp_demo_echo\",\"arguments\":\"{}\"}",
                                  .version = 3};
  ava::session::SessionEntry result{
      .id = "entry_result",
      .parent_id = "entry_call",
      .type = ava::session::EntryType::ToolResult,
      .timestamp = "2026-01-01T00:00:01Z",
      .data_json = "{\"call_id\":\"call_external\",\"name\":\"mcp_demo_echo\",\"success\":false,\"status\":\"error\",\"result\":\"" + std::string(canary) +
                   "\",\"structured_result\":{\"schema_version\":1,\"call_id\":\"call_external\",\"tool\":\"mcp_demo_echo\",\"status\":\"error\","
                   "\"ok\":false,\"summary\":\"" +
                   std::string(canary) + "\",\"content_type\":\"text/plain\",\"content\":\"" + std::string(canary) +
                   "\",\"error\":{\"category\":\"tool\",\"message\":\"" + std::string(canary) + "\"},\"truncated\":false}}",
      .version = 3};
  std::vector<ava::session::SessionEntry> const entries{call, result};

  auto const replay = ava::agent::build_provider_messages_from_entries(entries);
  std::string replay_text;
  if (replay)
  {
    for (auto const& message : *replay)
    {
      replay_text += message.content;
      for (auto const& part : message.content_parts) replay_text += part.text;
    }
  }
  expect(replay && replay_text.find(canary) == std::string::npos && replay_text.find("external_failure") != std::string::npos,
         "provider replay replaces historical failed MCP content with SafeFailure");

  auto const public_projection = ava::session::project_logical_session_history(entries);
  auto const portable_projection = ava::session::project_portable_session_history(entries);
  auto const direct_portable = ava::session::sanitize_session_entry_for_portable_jsonl_export(result);
  expect(public_projection && public_projection->back().data_json.find(canary) == std::string::npos &&
             public_projection->back().data_json.find("external_failure") != std::string::npos,
         "public session projection removes historical failed MCP content");
  expect(portable_projection && portable_projection->back().data_json.find(canary) == std::string::npos &&
             portable_projection->back().data_json.find("external_failure") != std::string::npos && direct_portable.data_json.find(canary) == std::string::npos,
         "portable projection and direct portable sanitization remove historical failed MCP content");

  auto plugin_call = call;
  auto plugin_result = result;
  auto replace_tool_name = [](std::string& text) {
    constexpr std::string_view old_name = "mcp_demo_echo";
    constexpr std::string_view new_name = "plugin_demo_echo";
    for (auto offset = text.find(old_name); offset != std::string::npos; offset = text.find(old_name, offset + new_name.size()))
      text.replace(offset, old_name.size(), new_name);
  };
  replace_tool_name(plugin_call.data_json);
  replace_tool_name(plugin_result.data_json);
  std::vector<ava::session::SessionEntry> const plugin_entries{plugin_call, plugin_result};
  auto const plugin_replay = ava::agent::build_provider_messages_from_entries(plugin_entries);
  auto const plugin_public = ava::session::project_logical_session_history(plugin_entries);
  auto const plugin_portable = ava::session::project_portable_session_history(plugin_entries);
  std::string plugin_replay_text;
  if (plugin_replay)
  {
    for (auto const& message : *plugin_replay)
    {
      plugin_replay_text += message.content;
      for (auto const& part : message.content_parts) plugin_replay_text += part.text;
    }
  }
  expect(plugin_replay && plugin_public && plugin_portable && plugin_replay_text.find(canary) == std::string::npos &&
             plugin_public->back().data_json.find(canary) == std::string::npos && plugin_portable->back().data_json.find(canary) == std::string::npos,
         "provider replay and public/portable projections remove historical failed plugin content");

  auto successful = result;
  successful.data_json =
      "{\"call_id\":\"call_external\",\"name\":\"plugin_demo_echo\",\"success\":true,\"status\":\"success\",\"result\":\"" + std::string(canary) + "\"}";
  auto const preserved = ava::session::sanitize_session_entry_for_portable_jsonl_export(successful);
  expect(preserved.data_json.find(canary) != std::string::npos, "successful external-tool content remains unchanged by compatibility sanitization");
}

std::filesystem::path unique_diagnostic_root(std::string_view name)
{
  static std::atomic<unsigned long long> sequence{0};
  auto const root = temp_root() / (std::string(name) + "_" + std::to_string(static_cast<unsigned long long>(::getpid())) + "_" +
                                   std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "state");
  static_cast<void>(::chmod(root.c_str(), 0700));
  static_cast<void>(::chmod((root / "state").c_str(), 0700));
  return root;
}

ava::config::XdgPaths diagnostic_paths(std::filesystem::path const& root)
{
  ava::config::XdgPaths paths;
  paths.state_home = root / "state";
  paths.ava_state_dir = paths.state_home / "ava";
  return paths;
}

std::shared_ptr<ava::core::AnchorSet> diagnostic_anchors(ava::config::XdgPaths const& paths)
{
  std::filesystem::create_directories(paths.ava_state_dir);
  static_cast<void>(::chmod(paths.ava_state_dir.c_str(), 0700));
  auto anchors = ava::core::AnchorSet::open({paths.ava_state_dir});
  expect(anchors.has_value(), anchors ? "diagnostic anchors open" : anchors.error().format());
  return anchors ? std::move(*anchors) : nullptr;
}

void write_test_file(std::filesystem::path const& path, std::string_view body)
{
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(body.data(), static_cast<std::streamsize>(body.size()));
  file.close();
  static_cast<void>(::chmod(path.c_str(), 0600));
}

ava::diagnostics::DoctorReport sample_doctor_report()
{
  return ava::diagnostics::DoctorReport{.checks = {{.kind = ava::diagnostics::DoctorCheckKind::VersionPlatform,
                                                    .status = ava::diagnostics::DoctorStatus::Pass,
                                                    .code = ava::diagnostics::DoctorCode::Ready,
                                                    .items = 1}}};
}

void test_phase2_typed_serialization_and_strict_parsing()
{
  auto const report = sample_doctor_report();
  auto const report_json = ava::diagnostics::serialize_doctor_report_json(report);
  auto parsed_report = ava::diagnostics::parse_doctor_report_json(report_json);
  expect(parsed_report && parsed_report->checks.size() == 1 && !parsed_report->has_failures(), "doctor report has a strict typed round trip");
  auto duplicate_report = report_json;
  duplicate_report.insert(1, "\"schema_version\":1,");
  expect(!ava::diagnostics::parse_doctor_report_json(duplicate_report), "doctor report rejects duplicate object keys");

  ava::diagnostics::LastFailureRecord const failure{.recorded_at = 42,
                                                    .failure = {.component = ava::diagnostics::ComponentClass::Runtime,
                                                                .category = ava::diagnostics::FailureCategory::Internal,
                                                                .code = ava::diagnostics::FailureCode::InternalFailure,
                                                                .retryability = ava::diagnostics::Retryability::Never,
                                                                .recovery_hint = ava::diagnostics::RecoveryHint::ContactSupport},
                                                    .occurrences = 3};
  auto const failure_json = ava::diagnostics::serialize_last_failure_record(failure);
  auto parsed_failure = ava::diagnostics::parse_last_failure_record(failure_json);
  expect(parsed_failure && parsed_failure->failure.component == ava::diagnostics::ComponentClass::Runtime && parsed_failure->occurrences == 3,
         "last-failure record has a strict typed round trip");
  auto duplicate_failure = failure_json;
  duplicate_failure.insert(1, "\"recorded_at\":99,");
  expect(!ava::diagnostics::parse_last_failure_record(duplicate_failure), "last-failure record rejects duplicate keys");
  expect(!ava::diagnostics::parse_last_failure_record(std::string(65 * 1024, 'x')), "last-failure parser rejects oversized input");

  constexpr std::string_view legacy_counters =
      "{\"schema_version\":1,\"captured_at\":84,\"runtime_starts\":2,\"provider_requests\":4,\"provider_failures\":1,"
      "\"session_failures\":3,\"plugin_failures\":5,\"mcp_failures\":6}";
  auto legacy_parsed = ava::diagnostics::parse_trace_counter_snapshot(legacy_counters);
  auto const upgraded_counters = legacy_parsed ? ava::diagnostics::serialize_trace_counter_snapshot(*legacy_parsed) : std::string{};
  auto upgraded_parsed = ava::diagnostics::parse_trace_counter_snapshot(upgraded_counters);
  expect(legacy_parsed && !legacy_parsed->writer_health.complete && legacy_parsed->writer_health.events_written == 0 && upgraded_parsed &&
             upgraded_counters.find("\"writer_health\"") != std::string::npos && !upgraded_parsed->writer_health.complete,
         "exact legacy v1 trace counters parse with incomplete zero health and round-trip to the new strict form");

  ava::diagnostics::TraceCounterSnapshot const counters{
      .captured_at = 84,
      .runtime_starts = 2,
      .provider_requests = 4,
      .provider_failures = 1,
      .session_failures = 3,
      .plugin_failures = 5,
      .mcp_failures = 6,
      .writer_health = {.complete = true, .events_written = 7, .events_dropped = 8, .writer_failures = 9, .bytes_written = 10}};
  auto parsed_counters = ava::diagnostics::parse_trace_counter_snapshot(ava::diagnostics::serialize_trace_counter_snapshot(counters));
  expect(parsed_counters && parsed_counters->provider_requests == 4 && parsed_counters->mcp_failures == 6 && parsed_counters->writer_health.complete &&
             parsed_counters->writer_health.events_dropped == 8,
         "trace-counter snapshot has a strict typed round trip");
  auto summary_counters = counters;
  summary_counters.writer_health.delta_summaries_refused = 23;
  auto const summary_round_trip = ava::diagnostics::parse_trace_counter_snapshot(ava::diagnostics::serialize_trace_counter_snapshot(summary_counters));
  expect(summary_round_trip && summary_round_trip->writer_health.delta_summaries_refused == 23 && summary_round_trip->writer_health.events_dropped == 8,
         "summary refusals round-trip independently of trace event loss");
  auto const legacy_health = ava::diagnostics::parse_trace_counter_snapshot(
      "{\"schema_version\":1,\"captured_at\":1,\"runtime_starts\":1,\"provider_requests\":1,\"provider_failures\":1,"
      "\"session_failures\":1,\"plugin_failures\":1,\"mcp_failures\":1,\"writer_health\":{"
      "\"complete\":true,\"events_written\":1,\"events_dropped\":1,\"writer_failures\":1,\"bytes_written\":1}}");
  expect(
      legacy_health && legacy_health->writer_health.delta_summaries_refused == 0 && legacy_parsed && legacy_parsed->writer_health.delta_summaries_refused == 0,
      "both older counter formats load with zero summary refusals");
  expect(!ava::diagnostics::parse_trace_counter_snapshot(
             "{\"schema_version\":1,\"captured_at\":1,\"runtime_starts\":1,\"provider_requests\":1,\"provider_failures\":1,"
             "\"session_failures\":1,\"plugin_failures\":1,\"mcp_failures\":1,\"writer_health\":{"
             "\"complete\":true,\"events_written\":1,\"events_dropped\":1,\"writer_failures\":1,\"bytes_written\":1,"
             "\"delta_summaries_refused\":-1}}"),
         "summary refusal counters retain strict unsigned validation");
  expect(!ava::diagnostics::parse_trace_counter_snapshot(
             "{\"schema_version\":1,\"captured_at\":1,\"runtime_starts\":1,\"provider_requests\":1,\"provider_failures\":1,"
             "\"session_failures\":1,\"plugin_failures\":1,\"mcp_failures\":1,\"writer_health\":{"
             "\"complete\":true,\"events_written\":1,\"events_dropped\":1,\"writer_failures\":1,\"bytes_written\":1,\"extra\":1}}") &&
             !ava::diagnostics::parse_trace_counter_snapshot(
                 "{\"schema_version\":1,\"captured_at\":1,\"runtime_starts\":1,\"provider_requests\":1,\"provider_failures\":1,"
                 "\"session_failures\":1,\"plugin_failures\":1,\"mcp_failures\":1,\"writer_health\":{"
                 "\"complete\":true,\"complete\":false,\"events_written\":1,\"events_dropped\":1,\"writer_failures\":1,\"bytes_written\":1}}") &&
             !ava::diagnostics::parse_trace_counter_snapshot(
                 "{\"schema_version\":1,\"captured_at\":1,\"runtime_starts\":1,\"provider_requests\":1,\"provider_failures\":1,"
                 "\"session_failures\":1,\"plugin_failures\":1,\"mcp_failures\":1,\"writer_health\":{"
                 "\"complete\":1,\"events_written\":1,\"events_dropped\":1,\"writer_failures\":1,\"bytes_written\":1}}"),
         "trace-counter parser rejects extra, duplicate, and malformed writer-health fields");

  ava::diagnostics::SupportArtifact const support{.generated_at = 100,
                                                  .doctor = report,
                                                  .trace = {.state = ava::diagnostics::StoredRecordState::Present, .record = counters},
                                                  .last_failure = {.state = ava::diagnostics::StoredRecordState::Present, .record = failure}};
  auto const support_json = ava::diagnostics::serialize_support_artifact(support);
  expect(ava::core::json::is_valid_object(support_json) && support_json.find("\"trace\":") != std::string::npos &&
             support_json.find("\"last_failure\":") != std::string::npos && support_json.find("CANARY") == std::string::npos,
         "support serializer emits the fixed typed schema only");
}

void test_private_artifact_storage_and_atomic_replacement()
{
  auto const root = unique_diagnostic_root("diagnostic_store_atomic");
  auto const paths = diagnostic_paths(root);
  auto const anchors = diagnostic_anchors(paths);
  ava::diagnostics::LastFailureRecord first{
      .recorded_at = 1, .failure = ava::diagnostics::external_failure(ava::diagnostics::ComponentClass::Provider), .occurrences = 1};
  ava::diagnostics::LastFailureRecord second{
      .recorded_at = 2, .failure = ava::diagnostics::canceled_failure(ava::diagnostics::ComponentClass::Runtime), .occurrences = 2};
  expect(ava::diagnostics::write_last_failure_record(paths, *anchors, first) == ava::diagnostics::ArtifactWriteStatus::Success,
         "private storage creates the diagnostic tree");
  expect(ava::diagnostics::write_last_failure_record(paths, *anchors, second) == ava::diagnostics::ArtifactWriteStatus::Success,
         "last-failure update atomically replaces a valid record");
  auto const read = ava::diagnostics::read_last_failure_record(paths, *anchors);
  expect(read.state == ava::diagnostics::StoredRecordState::Present && read.record && read.record->recorded_at == 2 && read.record->occurrences == 2,
         "last-failure reader observes the complete replacement");
  ava::diagnostics::TraceCounterSnapshot const counters{.captured_at = 3, .runtime_starts = 4, .provider_requests = 5, .writer_health = {}};
  expect(ava::diagnostics::write_trace_counter_snapshot(paths, *anchors, counters) == ava::diagnostics::ArtifactWriteStatus::Success,
         "trace counters use the private typed storage boundary");
  auto const counters_read = ava::diagnostics::read_trace_counter_snapshot(paths, *anchors);
  expect(counters_read.state == ava::diagnostics::StoredRecordState::Present && counters_read.record && counters_read.record->runtime_starts == 4 &&
             counters_read.record->provider_requests == 5,
         "trace-counter reader observes a complete typed record");

  struct stat directory_metadata{};
  struct stat file_metadata{};
  struct stat lock_metadata{};
  auto const diagnostics_dir = paths.ava_state_dir / "diagnostics";
  auto const record_file = diagnostics_dir / "last-failure-v1.json";
  auto const lock_file = diagnostics_dir / "trace-counters-v1.lock";
  expect(::stat(diagnostics_dir.c_str(), &directory_metadata) == 0 && (directory_metadata.st_mode & 0777) == 0700,
         "diagnostic directories are exact mode 0700");
  expect(::stat(record_file.c_str(), &file_metadata) == 0 && (file_metadata.st_mode & 0777) == 0600 && file_metadata.st_nlink == 1 &&
             file_metadata.st_uid == ::geteuid() && file_metadata.st_gid == ::getegid(),
         "diagnostic files are owner-owned, exact mode 0600, and singly linked");
  expect(::stat(lock_file.c_str(), &lock_metadata) == 0 && S_ISREG(lock_metadata.st_mode) && (lock_metadata.st_mode & 0777) == 0600 &&
             lock_metadata.st_nlink == 1 && lock_metadata.st_uid == ::geteuid() && lock_metadata.st_gid == ::getegid(),
         "trace-counter lock is a fixed owner-only singly-linked regular file");
  bool temporary_found = false;
  for (auto const& entry : std::filesystem::directory_iterator(diagnostics_dir))
    temporary_found = temporary_found || entry.path().filename().string().starts_with(".diagnostic-tmp-");
  expect(!temporary_found, "successful atomic replacement leaves no temporary artifact");
  std::filesystem::remove_all(root);
}

void test_trace_counter_writes_aggregate_sequentially_concurrently_and_saturate()
{
  auto const sequential_root = unique_diagnostic_root("diagnostic_counter_aggregate");
  auto const sequential_paths = diagnostic_paths(sequential_root);
  auto const sequential_anchors = diagnostic_anchors(sequential_paths);
  ava::diagnostics::TraceCounterSnapshot const first{
      .captured_at = 10,
      .runtime_starts = 1,
      .provider_requests = 2,
      .provider_failures = 3,
      .session_failures = 4,
      .plugin_failures = 5,
      .mcp_failures = 6,
      .writer_health = {.complete = true, .events_written = 7, .events_dropped = 8, .writer_failures = 9, .bytes_written = 10}};
  ava::diagnostics::TraceCounterSnapshot const second{
      .captured_at = 20,
      .runtime_starts = 10,
      .provider_requests = 20,
      .provider_failures = 30,
      .session_failures = 40,
      .plugin_failures = 50,
      .mcp_failures = 60,
      .writer_health = {.complete = true, .events_written = 70, .events_dropped = 80, .writer_failures = 90, .bytes_written = 100}};
  expect(ava::diagnostics::write_trace_counter_snapshot(sequential_paths, *sequential_anchors, first) == ava::diagnostics::ArtifactWriteStatus::Success &&
             ava::diagnostics::write_trace_counter_snapshot(sequential_paths, *sequential_anchors, second) == ava::diagnostics::ArtifactWriteStatus::Success,
         "sequential trace-counter contributions merge successfully");
  auto sequential = ava::diagnostics::read_trace_counter_snapshot(sequential_paths, *sequential_anchors);
  expect(sequential.record && sequential.record->captured_at == 20 && sequential.record->runtime_starts == 11 && sequential.record->provider_requests == 22 &&
             sequential.record->provider_failures == 33 && sequential.record->session_failures == 44 && sequential.record->plugin_failures == 55 &&
             sequential.record->mcp_failures == 66 && sequential.record->writer_health.complete && sequential.record->writer_health.events_written == 77 &&
             sequential.record->writer_health.events_dropped == 88 && sequential.record->writer_health.writer_failures == 99 &&
             sequential.record->writer_health.bytes_written == 110,
         "sequential aggregation sums every outcome and health counter and keeps the latest timestamp");

  auto const maximum = std::numeric_limits<std::uint64_t>::max();
  ava::diagnostics::TraceCounterSnapshot const saturating{
      .captured_at = 15,
      .runtime_starts = maximum,
      .provider_requests = maximum,
      .provider_failures = maximum,
      .session_failures = maximum,
      .plugin_failures = maximum,
      .mcp_failures = maximum,
      .writer_health = {.complete = true, .events_written = maximum, .events_dropped = maximum, .writer_failures = maximum, .bytes_written = maximum}};
  expect(ava::diagnostics::write_trace_counter_snapshot(sequential_paths, *sequential_anchors, saturating) == ava::diagnostics::ArtifactWriteStatus::Success,
         "trace-counter saturation contribution is accepted");
  auto saturated = ava::diagnostics::read_trace_counter_snapshot(sequential_paths, *sequential_anchors);
  expect(saturated.record && saturated.record->captured_at == 20 && saturated.record->runtime_starts == maximum &&
             saturated.record->provider_requests == maximum && saturated.record->provider_failures == maximum &&
             saturated.record->session_failures == maximum && saturated.record->plugin_failures == maximum && saturated.record->mcp_failures == maximum &&
             saturated.record->writer_health.complete && saturated.record->writer_health.events_written == maximum &&
             saturated.record->writer_health.events_dropped == maximum && saturated.record->writer_health.writer_failures == maximum &&
             saturated.record->writer_health.bytes_written == maximum,
         "all aggregate numeric fields saturate and timestamps never move backward");
  ava::diagnostics::TraceCounterSnapshot const incomplete{.captured_at = 21, .writer_health = {}};
  expect(ava::diagnostics::write_trace_counter_snapshot(sequential_paths, *sequential_anchors, incomplete) == ava::diagnostics::ArtifactWriteStatus::Success &&
             ava::diagnostics::read_trace_counter_snapshot(sequential_paths, *sequential_anchors).record &&
             !ava::diagnostics::read_trace_counter_snapshot(sequential_paths, *sequential_anchors).record->writer_health.complete,
         "aggregate writer completeness uses logical AND");
  std::filesystem::remove_all(sequential_root);

  auto const concurrent_root = unique_diagnostic_root("diagnostic_counter_concurrent");
  auto const concurrent_paths = diagnostic_paths(concurrent_root);
  auto const concurrent_anchors = diagnostic_anchors(concurrent_paths);
  constexpr unsigned kProcesses = 8;
  std::array<pid_t, kProcesses> children{};
  for (unsigned index = 0; index < kProcesses; ++index)
  {
    auto const child = ::fork();
    expect(child >= 0, "concurrent trace-counter child starts");
    if (child == 0)
    {
      auto const value = static_cast<std::uint64_t>(index + 1);
      ava::diagnostics::TraceCounterSnapshot const contribution{
          .captured_at = static_cast<std::int64_t>(value),
          .runtime_starts = value,
          .provider_requests = 2 * value,
          .provider_failures = 3 * value,
          .session_failures = 4 * value,
          .plugin_failures = 5 * value,
          .mcp_failures = 6 * value,
          .writer_health = {
              .complete = true, .events_written = 7 * value, .events_dropped = 8 * value, .writer_failures = 9 * value, .bytes_written = 10 * value}};
      auto const status = ava::diagnostics::write_trace_counter_snapshot(concurrent_paths, *concurrent_anchors, contribution);
      ::_exit(status == ava::diagnostics::ArtifactWriteStatus::Success ? 0 : 1);
    }
    children[index] = child;
  }
  bool children_succeeded = true;
  for (auto const child : children)
  {
    int status = 0;
    children_succeeded = children_succeeded && ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  }
  auto concurrent = ava::diagnostics::read_trace_counter_snapshot(concurrent_paths, *concurrent_anchors);
  expect(children_succeeded && concurrent.record && concurrent.record->captured_at == 8 && concurrent.record->runtime_starts == 36 &&
             concurrent.record->provider_requests == 72 && concurrent.record->provider_failures == 108 && concurrent.record->session_failures == 144 &&
             concurrent.record->plugin_failures == 180 && concurrent.record->mcp_failures == 216 && concurrent.record->writer_health.complete &&
             concurrent.record->writer_health.events_written == 252 && concurrent.record->writer_health.events_dropped == 288 &&
             concurrent.record->writer_health.writer_failures == 324 && concurrent.record->writer_health.bytes_written == 360,
         "blocking interprocess lock preserves every concurrent process contribution exactly once");
  std::filesystem::remove_all(concurrent_root);
}

void test_trace_counter_lock_rejects_unsafe_targets_without_erasing_evidence()
{
  for (std::string const fixture : {"symlink", "fifo", "hardlink", "mode"})
  {
    auto const root = unique_diagnostic_root("diagnostic_counter_lock_" + fixture);
    auto const paths = diagnostic_paths(root);
  auto const anchors = diagnostic_anchors(paths);
    ava::diagnostics::TraceCounterSnapshot const baseline{
        .captured_at = 1,
        .runtime_starts = 1,
        .writer_health = {.complete = true, .events_written = 1, .events_dropped = 0, .writer_failures = 0, .bytes_written = 10}};
    expect(ava::diagnostics::write_trace_counter_snapshot(paths, *anchors, baseline) == ava::diagnostics::ArtifactWriteStatus::Success,
           "unsafe-lock fixture begins with counter evidence");
    auto const diagnostics = paths.ava_state_dir / "diagnostics";
    auto const lock = diagnostics / "trace-counters-v1.lock";
    auto const evidence = diagnostics / "trace-counters-v1.json";
    auto const original = [&] {
      std::ifstream input(evidence, std::ios::binary);
      return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }();
    std::filesystem::remove(lock);
    if (fixture == "symlink")
    {
      write_test_file(diagnostics / "lock-target", "lock");
      std::filesystem::create_symlink("lock-target", lock);
    }
    else if (fixture == "fifo")
    {
      expect(::mkfifo(lock.c_str(), 0600) == 0, "unsafe counter-lock FIFO fixture is created");
    }
    else if (fixture == "hardlink")
    {
      write_test_file(diagnostics / "lock-target", "lock");
      std::filesystem::create_hard_link(diagnostics / "lock-target", lock);
    }
    else
    {
      write_test_file(lock, "lock");
      static_cast<void>(::chmod(lock.c_str(), 0644));
    }
    auto const started = std::chrono::steady_clock::now();
    auto const status = ava::diagnostics::write_trace_counter_snapshot(paths, *anchors, baseline);
    auto const elapsed = std::chrono::steady_clock::now() - started;
    std::ifstream input(evidence, std::ios::binary);
    std::string const after((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    bool temporary_found = false;
    for (auto const& entry : std::filesystem::directory_iterator(diagnostics))
      temporary_found = temporary_found || entry.path().filename().string().starts_with(".diagnostic-tmp-");
    expect(status == ava::diagnostics::ArtifactWriteStatus::UnsafeStorage && elapsed < std::chrono::seconds(1) && after == original && !temporary_found,
           "unsafe counter-lock " + fixture + " fails nonblockingly without erasing evidence or leaving a partial temporary");
    std::filesystem::remove_all(root);
  }
}

void test_private_artifact_reader_rejects_unsafe_and_malformed_files()
{
  auto const root = unique_diagnostic_root("diagnostic_store_reject");
  auto const paths = diagnostic_paths(root);
  auto const anchors = diagnostic_anchors(paths);
  ava::diagnostics::LastFailureRecord const record{.recorded_at = 1, .failure = ava::diagnostics::external_failure(ava::diagnostics::ComponentClass::Storage)};
  expect(ava::diagnostics::write_last_failure_record(paths, *anchors, record) == ava::diagnostics::ArtifactWriteStatus::Success,
         "unsafe-record fixtures begin with a valid private record");
  auto const file = paths.ava_state_dir / "diagnostics" / "last-failure-v1.json";
  auto const sibling = paths.ava_state_dir / "diagnostics" / "sibling";

  static_cast<void>(::chmod(file.c_str(), 0644));
  expect(ava::diagnostics::read_last_failure_record(paths, *anchors).state == ava::diagnostics::StoredRecordState::Unsafe,
         "diagnostic reader rejects wrong file permissions");
  std::filesystem::remove(file);
  write_test_file(sibling, "{}\n");
  std::filesystem::create_symlink(sibling.filename(), file);
  expect(ava::diagnostics::read_last_failure_record(paths, *anchors).state == ava::diagnostics::StoredRecordState::Unsafe, "diagnostic reader rejects symbolic links");
  std::filesystem::remove(file);
  expect(::mkfifo(file.c_str(), 0600) == 0, "FIFO fixture is created");
  expect(ava::diagnostics::read_last_failure_record(paths, *anchors).state == ava::diagnostics::StoredRecordState::Unsafe,
         "diagnostic reader rejects FIFOs without blocking");
  std::filesystem::remove(file);
  write_test_file(file, ava::diagnostics::serialize_last_failure_record(record));
  auto const hardlink = paths.ava_state_dir / "diagnostics" / "hardlink";
  std::filesystem::create_hard_link(file, hardlink);
  expect(ava::diagnostics::read_last_failure_record(paths, *anchors).state == ava::diagnostics::StoredRecordState::Unsafe,
         "diagnostic reader rejects multiply linked files");
  std::filesystem::remove(hardlink);

  write_test_file(file, "{\"schema_version\":1,\"schema_version\":1}");
  expect(ava::diagnostics::read_last_failure_record(paths, *anchors).state == ava::diagnostics::StoredRecordState::Malformed,
         "diagnostic reader reports duplicate-key records as malformed");
  write_test_file(file, std::string(65 * 1024, 'x'));
  expect(ava::diagnostics::read_last_failure_record(paths, *anchors).state == ava::diagnostics::StoredRecordState::Malformed,
         "diagnostic reader reports oversized records as malformed");

  std::filesystem::remove(file);
  expect(ava::diagnostics::read_last_failure_record(paths, *anchors).state == ava::diagnostics::StoredRecordState::Absent,
         "missing optional diagnostic record is absent");
  std::filesystem::remove_all(root);
}

void test_support_publication_is_unique_private_and_concurrent()
{
  auto const root = unique_diagnostic_root("diagnostic_support_publish");
  auto const paths = diagnostic_paths(root);
  auto const anchors = diagnostic_anchors(paths);
  ava::diagnostics::SupportArtifact const artifact{.generated_at = 123, .doctor = sample_doctor_report(), .trace = {}, .last_failure = {}};
  constexpr std::size_t publications = 8;
  std::array<ava::diagnostics::ArtifactWriteStatus, publications> statuses{};
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < publications; ++index)
  {
    threads.emplace_back([&, index] { statuses[index] = ava::diagnostics::publish_support_artifact(paths, *anchors, artifact).status; });
  }
  for (auto& thread : threads) thread.join();
  expect(std::ranges::all_of(statuses, [](auto status) { return status == ava::diagnostics::ArtifactWriteStatus::Success; }),
         "concurrent support publications all complete");

  auto const support_dir = paths.ava_state_dir / "support";
  std::size_t published = 0;
  bool safe = true;
  bool partial = false;
  for (auto const& entry : std::filesystem::directory_iterator(support_dir))
  {
    auto const name = entry.path().filename().string();
    partial = partial || name.starts_with(".support-tmp-");
    if (!name.starts_with("ava-support-v1-") || !name.ends_with(".json"))
      continue;
    ++published;
    struct stat metadata{};
    safe = safe && ::stat(entry.path().c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode) && (metadata.st_mode & 0777) == 0600 && metadata.st_nlink == 1 &&
           metadata.st_uid == ::geteuid() && metadata.st_gid == ::getegid();
    std::ifstream input(entry.path(), std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    safe = safe && ava::core::json::is_valid_object(body);
  }
  expect(published == publications && safe, "support publication is unique, valid, owner-only, and no-replace");
  expect(!partial, "support publication never leaves a partial published artifact");

  static_cast<void>(::chmod(support_dir.c_str(), 0755));
  expect(ava::diagnostics::publish_support_artifact(paths, *anchors, artifact).status == ava::diagnostics::ArtifactWriteStatus::UnsafeStorage,
         "support publication rejects an unsafe existing directory");
  std::size_t after_failed_publication = 0;
  for (auto const& entry : std::filesystem::directory_iterator(support_dir))
    after_failed_publication += entry.path().filename().string().starts_with("ava-support-v1-") ? 1U : 0U;
  expect(after_failed_publication == publications, "failed support publication exposes no partial or extra artifact");
  std::filesystem::remove_all(root);
}

void test_private_artifact_storage_uses_logical_anchors_and_rejects_unsafe_roots()
{
  auto const root = unique_diagnostic_root("diagnostic_store_anchor");
  auto const paths = diagnostic_paths(root);
  std::filesystem::create_directories(root / "replacement");
  static_cast<void>(::chmod((root / "replacement").c_str(), 0700));
  std::filesystem::create_directory_symlink(root / "replacement", paths.ava_state_dir);
  auto anchors = ava::core::AnchorSet::open({paths.ava_state_dir});
  expect(anchors.has_value(), anchors ? "logical diagnostic state anchor opens" : anchors.error().format());
  ava::diagnostics::TraceCounterSnapshot const snapshot{.captured_at = 1, .writer_health = {}};
  expect(anchors && ava::diagnostics::write_trace_counter_snapshot(paths, **anchors, snapshot) == ava::diagnostics::ArtifactWriteStatus::Success &&
             std::filesystem::exists(root / "replacement" / "diagnostics" / "trace-counters-v1.json"),
         "artifact writer preserves a trusted logical state-root identity through its configured anchor");

  std::filesystem::remove(paths.ava_state_dir);
  std::filesystem::create_directory(paths.ava_state_dir);
  static_cast<void>(::chmod(paths.ava_state_dir.c_str(), 0755));
  auto unsafe_anchors = ava::core::AnchorSet::open({paths.ava_state_dir});
  expect(unsafe_anchors &&
             ava::diagnostics::write_trace_counter_snapshot(paths, **unsafe_anchors, snapshot) ==
                 ava::diagnostics::ArtifactWriteStatus::UnsafeStorage,
         "artifact writer rejects wrong private-directory permissions at the anchored state root");
  std::filesystem::remove_all(root);
}

void test_malformed_counters_recover_and_quarantine_is_bounded()
{
  auto const root = unique_diagnostic_root("diagnostic_counter_recovery");
  auto const paths = diagnostic_paths(root);
  auto const anchors = diagnostic_anchors(paths);
  auto const directory = paths.ava_state_dir / "diagnostics";
  ava::diagnostics::TraceCounterSnapshot const contribution{.captured_at = 1, .runtime_starts = 3, .writer_health = {.complete = true}};
  expect(ava::diagnostics::write_trace_counter_snapshot(paths, *anchors, contribution) == ava::diagnostics::ArtifactWriteStatus::Success,
         "counter recovery fixture starts");
  auto const counters = directory / "trace-counters-v1.json";
  for (auto const& corrupt : {std::string("{broken"), std::string(65 * 1024, 'x')})
  {
    write_test_file(counters, corrupt);
    expect(ava::diagnostics::write_trace_counter_snapshot(paths, *anchors, contribution) == ava::diagnostics::ArtifactWriteStatus::Success,
           "verified-private malformed counters are quarantined and replaced");
    auto const recovered = ava::diagnostics::read_trace_counter_snapshot(paths, *anchors);
    expect(recovered.record && recovered.record->runtime_starts == 3, "corruption starts a fresh aggregate");
    auto const quarantine = directory / "trace-counters-v1.corrupt";
    expect(std::filesystem::exists(quarantine) && std::filesystem::file_size(quarantine) <= 64 * 1024,
           "a single bounded quarantine preserves malformed evidence");
    auto const status = ava::diagnostics::write_trace_counter_snapshot(paths, *anchors, contribution);
    auto const resumed = ava::diagnostics::read_trace_counter_snapshot(paths, *anchors);
    expect(status == ava::diagnostics::ArtifactWriteStatus::Success && resumed.record && resumed.record->runtime_starts == 6,
           "counter aggregation resumes after recovery");
  }
  write_test_file(counters, "{unsafe");
  static_cast<void>(::chmod(counters.c_str(), 0644));
  expect(ava::diagnostics::write_trace_counter_snapshot(paths, *anchors, contribution) == ava::diagnostics::ArtifactWriteStatus::UnsafeStorage &&
             std::filesystem::file_size(counters) == 7,
         "recovery never replaces unsafe counters");
  std::filesystem::remove_all(root);
}

void test_last_failure_counts_consecutive_classifications()
{
  auto const root = unique_diagnostic_root("diagnostic_failure_counts");
  auto const paths = diagnostic_paths(root);
  auto const anchors = diagnostic_anchors(paths);
  ava::diagnostics::LastFailureRecord record{.recorded_at = 1, .failure = ava::diagnostics::external_failure(ava::diagnostics::ComponentClass::Mcp)};
  expect(ava::diagnostics::write_last_failure_record(paths, *anchors, record) == ava::diagnostics::ArtifactWriteStatus::Success, "first failure is persisted");
  record.recorded_at = 2;
  expect(ava::diagnostics::write_last_failure_record(paths, *anchors, record) == ava::diagnostics::ArtifactWriteStatus::Success,
         "matching failure is persisted");
  auto stored = ava::diagnostics::read_last_failure_record(paths, *anchors);
  expect(stored.record && stored.record->occurrences == 2 && stored.record->recorded_at == 2,
         "consecutive identical safe classifications increment occurrences and refresh the timestamp");
  constexpr unsigned writers = 8;
  std::array<ava::diagnostics::ArtifactWriteStatus, writers> statuses{};
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < writers; ++i)
    threads.emplace_back([&, i] { statuses[i] = ava::diagnostics::write_last_failure_record(paths, *anchors, record); });
  for (auto& thread : threads)
    thread.join();
  stored = ava::diagnostics::read_last_failure_record(paths, *anchors);
  expect(std::ranges::all_of(statuses, [](auto status) { return status == ava::diagnostics::ArtifactWriteStatus::Success; }) && stored.record &&
             stored.record->occurrences == 10,
         "locked matching failure updates lose no concurrent occurrences");
  record.failure = ava::diagnostics::external_failure(ava::diagnostics::ComponentClass::Plugin);
  expect(ava::diagnostics::write_last_failure_record(paths, *anchors, record) == ava::diagnostics::ArtifactWriteStatus::Success,
         "different failure is persisted");
  stored = ava::diagnostics::read_last_failure_record(paths, *anchors);
  expect(stored.record && stored.record->occurrences == 1, "a different safe classification resets the consecutive count");
  record.occurrences = std::numeric_limits<std::uint64_t>::max();
  expect(ava::diagnostics::write_last_failure_record(paths, *anchors, record) == ava::diagnostics::ArtifactWriteStatus::Success,
         "large matching contribution is persisted");
  stored = ava::diagnostics::read_last_failure_record(paths, *anchors);
  expect(stored.record && stored.record->occurrences == std::numeric_limits<std::uint64_t>::max(), "failure occurrences saturate without wrapping");
  std::filesystem::remove_all(root);
}

void test_artifact_retention_prunes_oldest_and_stale_temporaries()
{
  auto const root = unique_diagnostic_root("diagnostic_retention");
  auto const paths = diagnostic_paths(root);
  auto const anchors = diagnostic_anchors(paths);
  auto initial = ava::diagnostics::prepare_trace_artifact(paths, *anchors);
  expect(initial.status == ava::diagnostics::ArtifactWriteStatus::Success, "retention trace directory starts");
  initial.descriptor.reset();
  std::filesystem::remove(initial.path);
  auto const directory = paths.ava_state_dir / "diagnostics" / "traces";
  auto const old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
  for (unsigned i = 0; i < 32; ++i)
  {
    auto const path = directory / ("trace-v1-fixture-" + std::to_string(i) + ".jsonl");
    write_test_file(path, "{}\n");
    std::filesystem::last_write_time(path, old + std::chrono::seconds(i));
  }
  auto const stale = paths.ava_state_dir / "diagnostics" / ".diagnostic-tmp-stale";
  auto const fresh = directory / ".diagnostic-tmp-fresh";
  write_test_file(stale, "orphan");
  write_test_file(fresh, "fresh");
  std::filesystem::last_write_time(stale, old);
  auto const active_temp = directory / ".diagnostic-tmp-active";
  write_test_file(active_temp, "active");
  std::filesystem::last_write_time(active_temp, old);
  int const active_fd = ::open(active_temp.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  expect(active_fd >= 0 && ::flock(active_fd, LOCK_EX | LOCK_NB) == 0, "stale active temporary is lifetime-locked");
  auto const unsafe_temp = directory / ".diagnostic-tmp-unsafe";
  std::filesystem::create_symlink(stale, unsafe_temp);
  auto next = ava::diagnostics::prepare_trace_artifact(paths, *anchors);
  expect(next.status == ava::diagnostics::ArtifactWriteStatus::Success && !std::filesystem::exists(directory / "trace-v1-fixture-0.jsonl") &&
             std::filesystem::exists(directory / "trace-v1-fixture-1.jsonl") && !std::filesystem::exists(stale) && std::filesystem::exists(fresh),
         "trace admission prunes the oldest at the count cap and sweeps only stale temporaries");
  expect(std::filesystem::exists(active_temp) && std::filesystem::is_symlink(unsafe_temp), "orphan cleanup preserves active and unsafe temporary entries");
  if (active_fd >= 0)
    static_cast<void>(::close(active_fd));
  next.descriptor.reset();
  // Sparse fixtures exercise byte retention without allocating or writing 100 MiB.
  for (auto const& entry : std::filesystem::directory_iterator(directory))
    if (entry.path().extension() == ".jsonl")
      std::filesystem::remove(entry.path());
  for (unsigned i = 0; i < 10; ++i)
  {
    auto const path = directory / ("trace-v1-large-" + std::to_string(i) + ".jsonl");
    write_test_file(path, "");
    std::filesystem::resize_file(path, 10U * 1024U * 1024U);
    std::filesystem::last_write_time(path, old + std::chrono::seconds(i));
  }
  next = ava::diagnostics::prepare_trace_artifact(paths, *anchors);
  expect(next.status == ava::diagnostics::ArtifactWriteStatus::Success && !std::filesystem::exists(directory / "trace-v1-large-0.jsonl") &&
             std::filesystem::exists(directory / "trace-v1-large-1.jsonl"),
         "trace admission reserves one full trace and removes the oldest to respect the byte cap");
  next.descriptor.reset();

  ava::diagnostics::SupportArtifact const artifact{.generated_at = 1, .doctor = sample_doctor_report(), .trace = {}, .last_failure = {}};
  auto publication = ava::diagnostics::publish_support_artifact(paths, *anchors, artifact);
  expect(publication.status == ava::diagnostics::ArtifactWriteStatus::Success, "retention support directory starts");
  std::filesystem::remove(publication.path);
  auto const support = paths.ava_state_dir / "support";
  for (unsigned i = 0; i < 32; ++i)
  {
    auto const path = support / ("ava-support-v1-fixture-" + std::to_string(i) + ".json");
    write_test_file(path, "{}");
    std::filesystem::last_write_time(path, old + std::chrono::seconds(i));
  }
  auto const support_stale = support / ".support-tmp-stale";
  write_test_file(support_stale, "orphan");
  std::filesystem::last_write_time(support_stale, old);
  publication = ava::diagnostics::publish_support_artifact(paths, *anchors, artifact);
  expect(publication.status == ava::diagnostics::ArtifactWriteStatus::Success && !std::filesystem::exists(support / "ava-support-v1-fixture-0.json") &&
             std::filesystem::exists(support / "ava-support-v1-fixture-1.json") && !std::filesystem::exists(support_stale),
         "support retention prunes oldest exports and stale support temporaries");
  for (auto const& entry : std::filesystem::directory_iterator(support))
    if (entry.path().extension() == ".json")
      std::filesystem::remove(entry.path());
  for (unsigned i = 0; i < 16; ++i)
  {
    auto const path = support / ("ava-support-v1-large-" + std::to_string(i) + ".json");
    write_test_file(path, "");
    std::filesystem::resize_file(path, 64U * 1024U);
    std::filesystem::last_write_time(path, old + std::chrono::seconds(i));
  }
  publication = ava::diagnostics::publish_support_artifact(paths, *anchors, artifact);
  expect(publication.status == ava::diagnostics::ArtifactWriteStatus::Success && !std::filesystem::exists(support / "ava-support-v1-large-0.json") &&
             std::filesystem::exists(support / "ava-support-v1-large-1.json"),
         "support retention also enforces its byte cap");
  std::filesystem::remove_all(root);
}

void test_retention_preserves_active_and_unsafe_artifacts()
{
  auto const root = unique_diagnostic_root("diagnostic_retention_active");
  auto const paths = diagnostic_paths(root);
  auto const anchors = diagnostic_anchors(paths);
  std::vector<ava::diagnostics::TraceArtifactPreparation> active;
  for (unsigned i = 0; i < 10; ++i)
    active.push_back(ava::diagnostics::prepare_trace_artifact(paths, *anchors));
  auto refused = ava::diagnostics::prepare_trace_artifact(paths, *anchors);
  expect(std::ranges::all_of(
             active, [](auto const& item) { return item.status == ava::diagnostics::ArtifactWriteStatus::Success && std::filesystem::exists(item.path); }) &&
             refused.status == ava::diagnostics::ArtifactWriteStatus::StorageUnavailable,
         "active traces reserve their maximum growth and cannot be pruned to admit another writer");
  auto const first = active.front().path;
  active.front().descriptor.reset();
  auto admitted = ava::diagnostics::prepare_trace_artifact(paths, *anchors);
  expect(admitted.status == ava::diagnostics::ArtifactWriteStatus::Success && std::filesystem::exists(first),
         "closed empty trace no longer reserves future growth");
  for (auto& trace : active)
    trace.descriptor.reset();
  admitted.descriptor.reset();
  static_cast<void>(::chmod(first.c_str(), 0644));
  auto const unsafe = first.parent_path() / "trace-v1-unsafe.jsonl";
  write_test_file(root / "outside", "keep");
  std::filesystem::create_symlink(root / "outside", unsafe);
  refused = ava::diagnostics::prepare_trace_artifact(paths, *anchors);
  struct stat shared_metadata{};
  expect(refused.status == ava::diagnostics::ArtifactWriteStatus::Success && std::filesystem::is_symlink(unsafe) &&
             std::filesystem::file_size(root / "outside") == 4 && ::stat(first.c_str(), &shared_metadata) == 0 && (shared_metadata.st_mode & 07777) == 0644,
         "shared traces and unsafe siblings remain untouched without blocking new trace admission");
  ava::diagnostics::SupportArtifact const artifact{.generated_at = 1, .doctor = sample_doctor_report(), .trace = {}, .last_failure = {}};
  auto const shared_export = ava::diagnostics::publish_support_artifact(paths, *anchors, artifact);
  expect(shared_export.status == ava::diagnostics::ArtifactWriteStatus::Success, "shared export fixture starts");
  static_cast<void>(::chmod(shared_export.path.c_str(), 0644));
  auto const unsafe_export = shared_export.path.parent_path() / "ava-support-v1-unsafe.json";
  std::filesystem::create_symlink(root / "outside", unsafe_export);
  auto const next_export = ava::diagnostics::publish_support_artifact(paths, *anchors, artifact);
  expect(next_export.status == ava::diagnostics::ArtifactWriteStatus::Success && std::filesystem::is_symlink(unsafe_export) &&
             ::stat(shared_export.path.c_str(), &shared_metadata) == 0 && (shared_metadata.st_mode & 07777) == 0644,
         "shared exports and unsafe siblings remain untouched without blocking new support publication");
  std::filesystem::remove_all(root);
}

void test_summary_refusal_counter_merges_without_event_loss()
{
  auto const root = unique_diagnostic_root("diagnostic_summary_refusal_merge");
  auto const paths = diagnostic_paths(root);
  auto const anchors = diagnostic_anchors(paths);
  ava::diagnostics::TraceCounterSnapshot contribution{.captured_at = 1, .writer_health = {.complete = true, .delta_summaries_refused = 7}};
  expect(ava::diagnostics::write_trace_counter_snapshot(paths, *anchors, contribution) == ava::diagnostics::ArtifactWriteStatus::Success &&
             ava::diagnostics::write_trace_counter_snapshot(paths, *anchors, contribution) == ava::diagnostics::ArtifactWriteStatus::Success,
         "summary refusal contributions persist");
  auto stored = ava::diagnostics::read_trace_counter_snapshot(paths, *anchors);
  expect(stored.record && stored.record->writer_health.delta_summaries_refused == 14 && stored.record->writer_health.events_dropped == 0,
         "summary refusals merge independently without reporting lifecycle loss");
  contribution.writer_health.delta_summaries_refused = std::numeric_limits<std::uint64_t>::max();
  expect(ava::diagnostics::write_trace_counter_snapshot(paths, *anchors, contribution) == ava::diagnostics::ArtifactWriteStatus::Success,
         "saturating summary refusal contribution persists");
  stored = ava::diagnostics::read_trace_counter_snapshot(paths, *anchors);
  expect(stored.record && stored.record->writer_health.delta_summaries_refused == std::numeric_limits<std::uint64_t>::max() &&
             stored.record->writer_health.events_dropped == 0,
         "summary refusal totals saturate without affecting event loss");
  std::filesystem::remove_all(root);
}

}  // namespace

void run_diagnostics_tests()
{
  test_closed_failure_serializers();
  test_core_error_adapter_discards_raw_text();
  test_external_tool_identity_boundary();
  test_historical_external_failure_projection_is_safe();
  test_phase2_typed_serialization_and_strict_parsing();
  test_private_artifact_storage_and_atomic_replacement();
  test_trace_counter_writes_aggregate_sequentially_concurrently_and_saturate();
  test_trace_counter_lock_rejects_unsafe_targets_without_erasing_evidence();
  test_private_artifact_reader_rejects_unsafe_and_malformed_files();
  test_support_publication_is_unique_private_and_concurrent();
  test_private_artifact_storage_uses_logical_anchors_and_rejects_unsafe_roots();
  test_malformed_counters_recover_and_quarantine_is_bounded();
  test_last_failure_counts_consecutive_classifications();
  test_artifact_retention_prunes_oldest_and_stale_temporaries();
  test_retention_preserves_active_and_unsafe_artifacts();
  test_summary_refusal_counter_merges_without_event_loss();
}
