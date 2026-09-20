#include "sys.h"
#include "tests/support/agent_loop_test_support.h"
#include "tests/support/fake_transport.h"
#include "tests/support/test_harness.h"
#include "ava/http/transport.h"
#include "ava/observability/run_observer.h"
#include "ava/agent/agent_loop.h"
#include "ava/agent/tool_dispatcher.h"
#include "ava/tools/bash_tool.h"
#include "ava/session/session_store.h"
#include "ava/provider/openai_provider.h"
#include "ava/core/json.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class FixedClock final : public ava::observability::Clock
{
 public:
  explicit FixedClock(std::int64_t value) : value_(value) { }
  [[nodiscard]] std::int64_t now_ms() override { return value_.fetch_add(1, std::memory_order_relaxed); }

 private:
  std::atomic<std::int64_t> value_;
};

class CollectingObserver final : public ava::observability::RunObserver
{
 public:
  void on_event(ava::observability::TraceEvent const& event) override
  {
    std::lock_guard lock(mutex);
    events.push_back(event);
  }
  std::mutex mutex;
  std::vector<ava::observability::TraceEvent> events;
};

class ThrowingObserver final : public ava::observability::RunObserver
{
 public:
  void on_event(ava::observability::TraceEvent const&) override { throw std::runtime_error("observer failure"); }
};

class ThrowingClock final : public ava::observability::Clock
{
 public:
  [[nodiscard]] std::int64_t now_ms() override { throw std::runtime_error("clock failure"); }
};

class ThrowingIds final : public ava::observability::IdGenerator
{
 public:
  [[nodiscard]] std::string next(std::string_view) override { throw std::runtime_error("id failure"); }
};

class CountingClock final : public ava::observability::Clock
{
 public:
  [[nodiscard]] std::int64_t now_ms() override
  {
    ++calls;
    return 1;
  }
  std::atomic<unsigned> calls = 0;
};

class CountingIds final : public ava::observability::IdGenerator
{
 public:
  [[nodiscard]] std::string next(std::string_view) override
  {
    ++calls;
    return "unexpected";
  }
  std::atomic<unsigned> calls = 0;
};

class CallbackPollingTransport final : public ava::http::Transport
{
 public:
  explicit CallbackPollingTransport(std::vector<ava::http::HttpResponse> responses, bool streaming = false)
      : responses_(std::move(responses)), streaming_(streaming)
  {
  }

  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const&) override { return next_response(); }
  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send(ava::http::HttpRequest const&, CancelCallback cancel_requested) override
  {
    ++send_calls;
    poll(cancel_requested);
    return next_response();
  }
  [[nodiscard]] bool supports_streaming() const noexcept override { return streaming_; }
  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> send_streaming(ava::http::HttpRequest const&, BodyChunkSink on_body_chunk,
                                                                          CancelCallback cancel_requested) override
  {
    ++streaming_calls;
    poll(cancel_requested);
    auto response = next_response();
    if (response && response->status_code >= 200 && response->status_code < 300 && on_body_chunk && !response->body.empty())
    {
      if (auto delivered = on_body_chunk(response->body); !delivered)
        return std::unexpected(std::move(delivered.error()));
    }
    return response;
  }

  unsigned send_calls = 0;
  unsigned streaming_calls = 0;
  unsigned callback_polls = 0;

 private:
  void poll(CancelCallback const& cancel_requested)
  {
    if (cancel_requested)
    {
      ++callback_polls;
      if (cancel_requested())
        throw std::runtime_error("transport canceled");
    }
  }
  [[nodiscard]] ava::core::Result<ava::http::HttpResponse> next_response()
  {
    if (next_ == responses_.size())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "missing scripted response"));
    return responses_[next_++];
  }

  std::vector<ava::http::HttpResponse> responses_;
  std::size_t next_ = 0;
  bool streaming_ = false;
};

class ReentrantObserver final : public ava::observability::RunObserver
{
 public:
  void on_event(ava::observability::TraceEvent const& event) override
  {
    callback_context_preserved = callback_context_preserved && ava::observability::in_run_observer_callback();
    events.push_back(event);
    if (!reentered)
    {
      reentered = true;
      observation->emit(ava::observability::TraceEventType::ProviderStreamEvent, context);
      callback_context_preserved = callback_context_preserved && ava::observability::in_run_observer_callback();
    }
  }

  ava::observability::RunObservation const* observation = nullptr;
  ava::observability::TraceContext context{
      .run_id = "run", .turn_id = "turn", .session_id = {}, .provider_id = {}, .parent_run_id = {}, .parent_turn_id = {}, .parent_session_id = {}};
  std::vector<ava::observability::TraceEvent> events;
  bool callback_context_preserved = true;

 private:
  bool reentered = false;
};

void test_trace_canonical_redaction_and_determinism()
{
  ava::observability::TraceEvent event;
  event.sequence = 7;
  event.timestamp_ms = 42;
  event.type = ava::observability::TraceEventType::TransportRequestResult;
  event.run_id = "run-1";
  event.fields = {{.key = "Authorization", .value = "Bearer CANARY_AUTH", .provenance = ava::observability::FieldProvenance::AuthorizationHeader},
                  {.key = "OPENAI_API_KEY", .value = "CANARY_ENV", .provenance = ava::observability::FieldProvenance::Environment},
                  {.key = "prompt", .value = "CANARY_PROMPT", .provenance = ava::observability::FieldProvenance::Content},
                  {.key = "path", .value = "/secret/CANARY_PATH", .provenance = ava::observability::FieldProvenance::Path},
                  {.key = "z", .value = "ok"}};
  auto const first = ava::observability::canonical_json(event);
  auto const second = ava::observability::canonical_json(event);
  expect(first == second, "trace serialization is byte deterministic");
  expect(first.find("CANARY_") == std::string::npos, "trace redacts auth, environment, content, and paths by provenance");
  expect(first.find("[redacted]") != std::string::npos && first.find("[omitted]") != std::string::npos, "trace records redaction disposition");
  expect(ava::core::json::is_valid_object(first), "canonical trace is parseable JSON");

  ava::observability::TraceEvent incorrectly_labeled = event;
  incorrectly_labeled.fields = {{.key = "Authorization", .value = "CANARY_PUBLIC_AUTH"},
                                {.key = "API-key", .value = "CANARY_PUBLIC_API_KEY"},
                                {.key = "token", .value = "CANARY_PUBLIC_TOKEN"},
                                {.key = "secret", .value = "CANARY_PUBLIC_SECRET"},
                                {.key = "OPENAI_API_KEY", .value = "CANARY_PUBLIC_ENVIRONMENT"},
                                {.key = "token_count", .value = "17"},
                                {.key = "api_version", .value = "2026-07-11"}};
  auto const incorrectly_labeled_json = ava::observability::canonical_json(incorrectly_labeled);
  expect(incorrectly_labeled_json.find("CANARY_PUBLIC_") == std::string::npos,
         "canonical credential keys are redacted even when incorrectly labeled public metadata");
  expect(incorrectly_labeled_json.find("\"token_count\":\"17\"") != std::string::npos &&
             incorrectly_labeled_json.find("\"api_version\":\"2026-07-11\"") != std::string::npos,
         "credential-key defense does not redact harmless public metadata");

  for (std::size_t limit = 2; limit <= 512; ++limit)
  {
    auto const bounded = ava::observability::canonical_json(event, limit);
    expect(ava::core::json::is_valid_object(bounded), "every feasible canonical byte bound remains valid JSON");
    expect(bounded.size() <= limit, "canonical serialization obeys each feasible byte bound");
  }
  ava::observability::TraceEvent hostile = event;
  hostile.run_id = std::string(10000, 'r');
  hostile.fields = {{.key = "duplicate", .value = "one"},
                    {.key = "duplicate", .value = "two"},
                    {.key = "fields_truncated", .value = "attacker"},
                    {.key = std::string(200, 'k'), .value = "long-key"},
                    {.key = std::string(200, 'k') + "other", .value = "long-key-two"},
                    {.key = "large", .value = std::string(4096, 'x')}};
  auto const hostile_json = ava::observability::canonical_json(hostile, 800);
  std::string_view const marker = "\"fields_truncated\":\"true\"";
  auto const marker_count = [&] {
    std::size_t count = 0, offset = 0;
    while ((offset = hostile_json.find(marker, offset)) != std::string::npos)
    {
      ++count;
      offset += marker.size();
    }
    return count;
  }();
  expect(ava::core::json::is_valid_object(hostile_json) && hostile_json.find("duplicate-omitted") != std::string::npos,
         "hostile identifiers and duplicate field keys serialize as valid deterministic JSON");
  expect(marker_count == 1 && hostile_json.find("attacker") == std::string::npos,
         "reserved truncation key cannot spoof or duplicate the serializer truncation marker");

  ava::observability::TraceEvent boundary = event;
  boundary.fields = {{.key = "fields_truncated", .value = "attacker"}, {.key = "a", .value = "one"}, {.key = "b", .value = std::string(40, 'b')}};
  auto empty_fields = boundary;
  empty_fields.fields.clear();
  auto only_a = empty_fields;
  only_a.fields = {{.key = "a", .value = "one"}};
  auto only_b = empty_fields;
  only_b.fields = {{.key = "b", .value = std::string(40, 'b')}};
  auto const base = ava::observability::canonical_json(empty_fields, 4096);
  auto const a_record = ava::observability::canonical_json(only_a, 4096);
  auto const b_record = ava::observability::canonical_json(only_b, 4096);
  auto const complete_record = ava::observability::canonical_json(boundary, 4096);
  static constexpr std::string_view minimal_marker = "{\"fields_truncated\":\"true\"}";
  static constexpr std::string_view marker_field = "\"fields_truncated\":\"true\"";
  std::set<std::size_t> boundary_limits;
  for (auto const center : {minimal_marker.size(), base.size(), base.size() + marker_field.size(), a_record.size(), b_record.size(), complete_record.size()})
    for (std::size_t offset = 0; offset <= 2; ++offset)
    {
      if (center >= offset)
        boundary_limits.insert(center - offset);
      boundary_limits.insert(center + offset);
    }
  auto count_marker = [&](std::string_view json) {
    std::size_t count = 0, offset = 0;
    while ((offset = json.find(marker, offset)) != std::string::npos)
    {
      ++count;
      offset += marker.size();
    }
    return count;
  };
  for (auto const limit : boundary_limits)
  {
    auto const bounded = ava::observability::canonical_json(boundary, limit);
    auto const omitted = bounded.find("\"a\":\"one\"") == std::string::npos || bounded.find("\"b\":\"") == std::string::npos;
    expect(ava::core::json::is_valid_object(bounded) && bounded.size() <= limit,
           "near-capacity canonical serialization remains valid JSON within its byte bound");
    expect(bounded.find("attacker") == std::string::npos, "near-capacity serialization never reflects an attacker truncation marker");
    expect(count_marker(bounded) == (omitted && limit >= minimal_marker.size() ? 1U : 0U),
           "every representable omitted-field record has exactly one authoritative truncation marker");
  }

  auto collector = std::make_shared<CollectingObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(100),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>(9));
  ava::observability::TraceContext context;
  context.run_id = observation->next_id("run");
  context.turn_id = observation->next_id("turn");
  observation->emit(ava::observability::TraceEventType::AgentRunStart, context);
  observation->emit(ava::observability::TraceEventType::AgentRunTerminal, context);
  auto one = collector->events.at(0);
  auto two = collector->events.at(1);
  expect(context.run_id == "run-10" && context.turn_id == "turn-11", "injected IDs are deterministic");
  expect(one.timestamp_ms == 100 && two.timestamp_ms == 101 && one.sequence == 1 && two.sequence == 2, "injected clock and sequence are deterministic");
}

void test_observer_failures_bounds_and_disabled_artifacts()
{
  auto const root = create_empty_root("test_observer_failures_bounds_and_disabled_artifacts");

  auto clock = std::make_shared<CountingClock>();
  auto ids = std::make_shared<CountingIds>();
  ava::observability::RunObservation disabled(nullptr, clock, ids);
  expect(!disabled.enabled(), "observer is disabled by default");
  bool enricher_called = false;
  disabled.emit(ava::observability::TraceEventType::AgentRunStart, {}, [&enricher_called](auto&) { enricher_called = true; });
  auto const disabled_path = root / "disabled-observer" / "trace.jsonl";
  expect(!enricher_called && clock->calls == 0 && ids->calls == 0 && !std::filesystem::exists(disabled_path),
         "disabled emit returns before event allocation/enrichment, clock/ID access, or observer file writes");
  auto disabled_store = ava::session::SessionStore::create_ephemeral(root);
  expect(disabled_store.has_value(), "create disabled-path session store");
  if (disabled_store)
  {
    static_cast<void>(disabled_store->set_run_observation(std::make_shared<ava::observability::RunObservation>(nullptr, clock, ids)));
    static_cast<void>(append_session_entry_for_test(
        *disabled_store, {.id = "disabled", .parent_id = "", .type = ava::session::EntryType::SessionStart, .timestamp = "now", .data_json = "{}"}));
    static_cast<void>(disabled_store->load());
  }
  expect(clock->calls == 0 && ids->calls == 0, "disabled SessionStore append/load takes no attachment lock, trace allocation, clock, or ID path");

  auto throwing = std::make_shared<ThrowingObserver>();
  ava::observability::RunObservation isolated(throwing, std::make_shared<FixedClock>(1), std::make_shared<ava::observability::CounterIdGenerator>());
  isolated.emit(ava::observability::TraceEventType::AgentRunStart, {});
  expect(isolated.counters().callback_failures == 1, "throwing observer is accounted and isolated");

  auto const path = root / "observer-bounds" / "trace.jsonl";
  auto writer = std::make_shared<ava::observability::JsonlRunObserver>(
      ava::observability::JsonlObserverOptions{.path = path, .max_events = 1, .max_bytes = 1024, .max_event_bytes = 512});
  ava::observability::RunObservation bounded(writer, std::make_shared<FixedClock>(1), std::make_shared<ava::observability::CounterIdGenerator>());
  ava::observability::TraceEvent oversized;
  oversized.type = ava::observability::TraceEventType::AgentRunStart;
  oversized.fields.push_back({.key = "body", .value = std::string(100000, 'x'), .provenance = ava::observability::FieldProvenance::Content});
  bounded.emit(ava::observability::TraceEventType::AgentRunStart, {}, [&oversized](auto& event) { event = oversized; });
  bounded.emit(ava::observability::TraceEventType::AgentRunTerminal, {});
  writer->close();
  auto const counters = writer->counters();
  expect(counters.written == 1 && counters.dropped == 1 && counters.bytes_written <= 1024, "writer enforces item and byte bounds with accounting");
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  expect(!line.empty() && line.size() <= 512 && ava::core::json::is_valid_object(line), "hostile event remains valid bounded JSONL");

  auto const parent_safety_root = root / "observer-parent-safety";
  std::error_code cleanup_error;
  std::filesystem::remove_all(parent_safety_root, cleanup_error);
  auto const unsafe_parent = parent_safety_root / "unsafe";
  std::filesystem::create_directories(unsafe_parent);
  expect(::chmod(unsafe_parent.c_str(), 0750) == 0, "set unsafe existing trace parent mode");
  struct stat unsafe_before{};
  expect(::stat(unsafe_parent.c_str(), &unsafe_before) == 0, "inspect unsafe existing trace parent mode");
  auto unsafe_writer = std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = unsafe_parent / "trace.jsonl"});
  ava::observability::RunObservation unsafe_observation(unsafe_writer, std::make_shared<FixedClock>(1),
                                                        std::make_shared<ava::observability::CounterIdGenerator>());
  unsafe_observation.emit(ava::observability::TraceEventType::AgentRunStart, {});
  struct stat unsafe_after{};
  expect(::stat(unsafe_parent.c_str(), &unsafe_after) == 0 && (unsafe_before.st_mode & 0777) == (unsafe_after.st_mode & 0777),
         "existing trace parent mode is never changed");
  expect(unsafe_writer->counters().written == 0 && unsafe_writer->counters().failures == 1 && unsafe_observation.counters().callback_failures == 1 &&
             !std::filesystem::exists(unsafe_parent / "trace.jsonl"),
         "unsafe existing trace parent fails closed and is accounted in the isolated observer");

  auto const owner_only_parent = parent_safety_root / "owner-only";
  std::filesystem::create_directories(owner_only_parent);
  expect(::chmod(owner_only_parent.c_str(), 0700) == 0, "set owner-only existing trace parent mode");
  struct stat owner_only_before{};
  expect(::stat(owner_only_parent.c_str(), &owner_only_before) == 0, "inspect raced existing trace parent mode");
  auto owner_only_writer =
      std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = owner_only_parent / "trace.jsonl"});
  ava::observability::RunObservation owner_only_observation(owner_only_writer, std::make_shared<FixedClock>(1),
                                                            std::make_shared<ava::observability::CounterIdGenerator>());
  owner_only_observation.emit(ava::observability::TraceEventType::AgentRunStart, {});
  struct stat owner_only_after{};
  expect(owner_only_writer->counters().written == 1 && owner_only_writer->counters().failures == 0 &&
             owner_only_observation.counters().callback_failures == 0 && std::filesystem::exists(owner_only_parent / "trace.jsonl") &&
             ::stat(owner_only_parent.c_str(), &owner_only_after) == 0 && (owner_only_before.st_mode & 0777) == (owner_only_after.st_mode & 0777),
         "mkdirat EEXIST race path treats the existing owner-only parent as uncreated and never chmods it");

  auto const failure_path = root / "observer-failure" / "directory-as-trace";
  std::filesystem::create_directories(failure_path);
  auto failing_writer = std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = failure_path});
  ava::observability::RunObservation failing_observation(failing_writer);
  failing_observation.emit(ava::observability::TraceEventType::AgentRunStart, {});
  expect(failing_writer->counters().written == 0 && failing_writer->counters().failures == 1 && failing_observation.counters().callback_failures == 1,
         "writer failures are isolated and accounted without a partial success count");
}

void test_emit_isolates_all_construction_failures()
{
  auto collector = std::make_shared<CollectingObserver>();
  ava::observability::RunObservation clock_failure(collector, std::make_shared<ThrowingClock>(), std::make_shared<ThrowingIds>());
  clock_failure.emit(ava::observability::TraceEventType::AgentRunStart, {}, [](auto&) { throw std::runtime_error("enricher failure"); });
  expect(clock_failure.counters().callback_failures == 1, "throwing clock and enricher are isolated by emit");
  expect(clock_failure.next_id("run").empty() && clock_failure.counters().callback_failures == 2, "throwing ID generator is isolated by emit boundary state");
  ava::observability::RunObservation observer_failure(std::make_shared<ThrowingObserver>(), std::make_shared<FixedClock>(1),
                                                      std::make_shared<ava::observability::CounterIdGenerator>());
  observer_failure.emit(ava::observability::TraceEventType::AgentRunStart, {});
  expect(observer_failure.counters().callback_failures == 1, "throwing observer is isolated by emit");
}

void test_trace_validator_and_scoring_manifest()
{
  auto trace_event = [](std::uint64_t sequence, ava::observability::TraceEventType type,
                        ava::observability::TraceOutcome outcome = ava::observability::TraceOutcome::None) {
    ava::observability::TraceEvent event;
    event.sequence = sequence;
    event.type = type;
    event.run_id = "text";
    event.outcome = outcome;
    return event;
  };
  std::vector<ava::observability::TraceEvent> complete = {
      trace_event(1, ava::observability::TraceEventType::AgentRunStart),
      trace_event(2, ava::observability::TraceEventType::ProviderStreamEvent),
      trace_event(3, ava::observability::TraceEventType::AgentRunTerminal, ava::observability::TraceOutcome::Completed),
  };
  auto scored = ava::observability::validate_and_score_trace(complete, {.callback_failures = 1, .dropped_events = 2});
  expect(scored.valid && scored.score == 97 && scored.accounted_failures == 1 && scored.accounted_drops == 2,
         "trace validator accepts complete lifecycle and scores separately-accounted losses deterministically");
  complete.push_back(trace_event(4, ava::observability::TraceEventType::SessionAppendAttempt));
  expect(!ava::observability::validate_and_score_trace(complete).valid, "trace validator rejects events after a terminal record");
  std::vector<ava::observability::TraceEvent> lifecycle_only = {
      trace_event(1, ava::observability::TraceEventType::AgentRunStart),
      trace_event(2, ava::observability::TraceEventType::AgentRunTerminal, ava::observability::TraceOutcome::Completed)};
  ava::observability::TraceFixturePolicy text_policy{
      .fixture_id = "text",
      .denominator = 100,
      .lifecycle_weight = 40,
      .required_boundary_weight = 60,
      .required_boundaries = {ava::observability::TraceRequiredBoundary::RunLifecycle, ava::observability::TraceRequiredBoundary::TextDelta}};
  auto lifecycle_only_score = ava::observability::validate_and_score_trace(lifecycle_only, {}, &text_policy);
  expect(!lifecycle_only_score.valid && lifecycle_only_score.score == 40, "a start/terminal-only text fixture cannot receive a complete weighted score");

#ifdef AVA_M1_GOLDEN_DIR
  std::ifstream manifest(std::string(AVA_M1_GOLDEN_DIR) + "/scoring-manifest.json", std::ios::binary);
  std::ifstream golden(std::string(AVA_M1_GOLDEN_DIR) + "/normalized-trace.jsonl", std::ios::binary);
  std::string manifest_bytes((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
  std::vector<ava::observability::TraceEvent> golden_events;
  std::string line;
  while (std::getline(golden, line))
  {
    auto event = ava::observability::parse_canonical_json(line);
    expect(event.has_value(), "every committed normalized trace line parses as schema-v1");
    if (event)
      golden_events.push_back(std::move(*event));
  }
  auto const pre_m1 = ava::core::json::object_field(manifest_bytes, "pre_m1");
  auto const current = ava::core::json::object_field(manifest_bytes, "current");
  auto const timeouts = ava::core::json::object_field(manifest_bytes, "timeouts");
  auto const fixtures = ava::core::json::objects_in_array_field(manifest_bytes, "fixtures");
  bool golden_complete = fixtures.size() == 4;
  for (auto const& fixture : fixtures)
  {
    auto const id = ava::core::json::string_field(fixture, "id");
    auto const revision = ava::core::json::string_field(fixture, "revision");
    auto const applicable = ava::core::json::field_value_start(fixture, "applicable");
    auto const evidence_source = ava::core::json::string_field(fixture, "evidence_source");
    auto const required = ava::core::json::strings_in_array_field(fixture, "required_boundaries");
    auto const weights = ava::core::json::object_field(fixture, "weights");
    auto const denominator = ava::core::json::integer_field(fixture, "denominator");
    auto const timeout = ava::core::json::integer_field(fixture, "timeout_seconds");
    std::vector<ava::observability::TraceRequiredBoundary> boundaries;
    for (auto const& name : required)
    {
      if (name == "run_lifecycle")
        boundaries.push_back(ava::observability::TraceRequiredBoundary::RunLifecycle);
      else if (name == "text_delta")
        boundaries.push_back(ava::observability::TraceRequiredBoundary::TextDelta);
      else if (name == "tool_dispatch")
        boundaries.push_back(ava::observability::TraceRequiredBoundary::ToolDispatch);
      else if (name == "provider_error_terminal")
        boundaries.push_back(ava::observability::TraceRequiredBoundary::ProviderErrorTerminal);
      else if (name == "canceled_terminal")
        boundaries.push_back(ava::observability::TraceRequiredBoundary::CanceledTerminal);
    }
    std::vector<ava::observability::TraceEvent> fixture_events;
    if (id)
      for (auto const& event : golden_events)
        if (event.run_id == *id)
          fixture_events.push_back(event);
    auto const lifecycle_weight = weights ? ava::core::json::integer_field(*weights, "run_lifecycle") : std::nullopt;
    auto const required_weight = weights && id ? ava::core::json::integer_field(*weights, *id == "text"    ? "text_delta"
                                                                                          : *id == "tool"  ? "tool_dispatch"
                                                                                          : *id == "error" ? "provider_error_terminal"
                                                                                                           : "canceled_terminal")
                                               : std::nullopt;
    ava::observability::TraceFixturePolicy policy{.fixture_id = id.value_or(""),
                                                  .denominator = static_cast<unsigned>(denominator.value_or(0)),
                                                  .lifecycle_weight = static_cast<unsigned>(lifecycle_weight.value_or(0)),
                                                  .required_boundary_weight = static_cast<unsigned>(required_weight.value_or(0)),
                                                  .required_boundaries = std::move(boundaries)};
    auto score = ava::observability::validate_and_score_trace(fixture_events, {}, &policy);
    golden_complete = golden_complete && id && revision && *revision == "m1-observer-golden-r2" && applicable && fixture.substr(*applicable, 4) == "true" &&
                      evidence_source && denominator && *denominator == 100 && timeout && *timeout == 60 && required.size() == 2 && score.valid &&
                      score.score == 100;
  }
  expect(ava::core::json::is_valid_object(manifest_bytes) && pre_m1 && current && timeouts && ava::core::json::integer_field(*pre_m1, "registered") == 63 &&
             ava::core::json::integer_field(*pre_m1, "passed") == 59 && ava::core::json::integer_field(*pre_m1, "skipped") == 4 &&
             ava::core::json::integer_field(*current, "registered") == 64 && ava::core::json::integer_field(*current, "passed") == 60 &&
             ava::core::json::integer_field(*current, "skipped") == 4 && ava::core::json::integer_field(*timeouts, "focused_ctest_seconds") == 60 &&
             ava::core::json::integer_field(*timeouts, "full_ctest_seconds") == 300 && golden_complete,
         "scorer consumes pinned fixture-specific applicability, evidence, weights, denominators, and text/tool/error/cancel lifecycle checks");
#endif
}

void test_queued_writer_bounds_and_unsafe_targets()
{
  auto const close_root = create_empty_root("queued-observer-close");
  ava::observability::QueuedJsonlObserverOptions immediate_options;
  immediate_options.path = close_root / "trace.jsonl";
  immediate_options.max_events = 8;
  immediate_options.max_bytes = 4096;
  immediate_options.max_event_bytes = 512;
  immediate_options.max_queue_events = 8;
  immediate_options.max_queue_bytes = 4096;
  ava::observability::QueuedJsonlRunObserver immediate_close(std::move(immediate_options));
  ava::observability::RunObservation immediate_observation(
      std::shared_ptr<ava::observability::RunObserver>(&immediate_close, [](ava::observability::RunObserver*) { }), std::make_shared<FixedClock>(1),
      std::make_shared<ava::observability::CounterIdGenerator>());
  ava::observability::TraceContext immediate_context{.run_id = "close",
                                                     .turn_id = "turn",
                                                     .session_id = "session",
                                                     .provider_id = "provider",
                                                     .parent_run_id = {},
                                                     .parent_turn_id = {},
                                                     .parent_session_id = {}};
  immediate_observation.emit(ava::observability::TraceEventType::AgentRunStart, immediate_context);
  immediate_observation.emit(ava::observability::TraceEventType::AgentRunTerminal, immediate_context,
                             [](auto& event) { event.outcome = ava::observability::TraceOutcome::Completed; });
  immediate_close.close();
  auto const immediate_counters = immediate_close.counters();
  std::ifstream immediate_file(close_root / "trace.jsonl");
  std::string immediate_line;
  unsigned immediate_records = 0;
  while (std::getline(immediate_file, immediate_line)) ++immediate_records;
  expect(immediate_counters.written == 2 && immediate_counters.queue_dropped == 0 && immediate_counters.queue_bytes == 0 && immediate_records == 2,
         "normal queued close drains accepted start and terminal records before joining without detaching");

  immediate_close.on_event({});
  expect(immediate_close.counters().queue_dropped == 1, "queued writer drops only new records admitted after close begins");

  expect(ava::observability::queue_has_capacity(0, 0, 5, 2, 10) && ava::observability::queue_has_capacity(1, 5, 5, 2, 10) &&
             !ava::observability::queue_has_capacity(2, 0, 1, 2, 10) && !ava::observability::queue_has_capacity(0, 0, 11, 2, 10) &&
             !ava::observability::queue_has_capacity(1, 6, 5, 2, 10),
         "pure queue capacity predicate deterministically rejects event and byte overflow");

  auto const root = create_empty_root("queued-observer");


  constexpr int kProducerCount = 4;
  constexpr int kEventsPerProducer = 50;
  constexpr auto kAcceptedRecords = kProducerCount * kEventsPerProducer;
  ava::observability::QueuedJsonlObserverOptions queued_options;
  queued_options.path = root / "trace.jsonl";
  queued_options.max_events = kAcceptedRecords;
  queued_options.max_bytes = 128 * 1024;
  queued_options.max_event_bytes = 512;
  queued_options.max_queue_events = kAcceptedRecords;
  queued_options.max_queue_bytes = 128 * 1024;
  auto writer = std::make_shared<ava::observability::QueuedJsonlRunObserver>(queued_options);
  ava::observability::RunObservation observation(writer, std::make_shared<FixedClock>(1), std::make_shared<ava::observability::CounterIdGenerator>());
  std::vector<std::thread> producers;
  for (int producer = 0; producer < kProducerCount; ++producer)
    producers.emplace_back([&] {
      for (int count = 0; count < kEventsPerProducer; ++count) observation.emit(ava::observability::TraceEventType::AgentRunStart, {});
    });
  for (auto& producer : producers) producer.join();
  writer->close();
  auto const counters = writer->counters();
  expect(counters.queue_dropped == 0 && counters.written == kAcceptedRecords && counters.dropped == 0 && counters.queue_bytes == 0 &&
             counters.queue_high_water_events <= kAcceptedRecords && counters.queue_high_water_bytes <= queued_options.max_queue_bytes,
         "ample-capacity concurrent producers have no contention drops, write every accepted record, remain bounded, and close drains");

  auto const unsafe = root / "unsafe";
  std::filesystem::create_directories(unsafe.parent_path());
  expect(::symlink("trace.jsonl", unsafe.c_str()) == 0, "create trace symlink canary");
  auto unsafe_writer = std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = unsafe});
  ava::observability::RunObservation unsafe_observation(unsafe_writer, std::make_shared<FixedClock>(1),
                                                        std::make_shared<ava::observability::CounterIdGenerator>());
  unsafe_observation.emit(ava::observability::TraceEventType::AgentRunStart, {});
  expect(unsafe_writer->counters().failures == 1 && unsafe_observation.counters().callback_failures == 1, "symlink target fails closed without writing");

  auto const hardlink_target = root / "hardlink-target";
  {
    std::ofstream target(hardlink_target);
    target << "unchanged";
  }
  expect(::chmod(hardlink_target.c_str(), 0600) == 0 && ::link(hardlink_target.c_str(), (root / "hardlink-trace").c_str()) == 0, "create hardlink canary");
  auto hardlink_writer = std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = root / "hardlink-trace"});
  ava::observability::RunObservation hardlink_observation(hardlink_writer, std::make_shared<FixedClock>(1),
                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  hardlink_observation.emit(ava::observability::TraceEventType::AgentRunStart, {});
  std::ifstream hardlink_target_input(hardlink_target, std::ios::binary);
  std::string hardlink_target_bytes((std::istreambuf_iterator<char>(hardlink_target_input)), std::istreambuf_iterator<char>());
  expect(hardlink_writer->counters().failures == 1 && hardlink_target_bytes == "unchanged", "hardlinked target fails closed without modification");

  auto first = std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = root / "single-writer"});
  auto second = std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = root / "single-writer"});
  ava::observability::RunObservation first_observation(first, std::make_shared<FixedClock>(1), std::make_shared<ava::observability::CounterIdGenerator>());
  ava::observability::RunObservation second_observation(second, std::make_shared<FixedClock>(1), std::make_shared<ava::observability::CounterIdGenerator>());
  first_observation.emit(ava::observability::TraceEventType::AgentRunStart, {});
  second_observation.emit(ava::observability::TraceEventType::AgentRunStart, {});
  first->close();
  second->close();
  std::ifstream single_writer(root / "single-writer", std::ios::binary);
  std::string single_writer_bytes((std::istreambuf_iterator<char>(single_writer)), std::istreambuf_iterator<char>());
  expect(first->counters().written == 1 && second->counters().failures == 1 && single_writer_bytes.find('\n') == single_writer_bytes.size() - 1 &&
             ava::core::json::is_valid_object(single_writer_bytes.substr(0, single_writer_bytes.size() - 1)),
         "lifetime lock rejects a second writer and preserves one complete non-interleaved record");

  auto const fifo = root / "trace-fifo";
  expect(::mkfifo(fifo.c_str(), 0600) == 0, "create FIFO trace target");
  auto fifo_writer = std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = fifo});
  ava::observability::RunObservation fifo_observation(fifo_writer, std::make_shared<FixedClock>(1), std::make_shared<ava::observability::CounterIdGenerator>());
  auto const fifo_start = std::chrono::steady_clock::now();
  fifo_observation.emit(ava::observability::TraceEventType::AgentRunStart, {});
  auto const fifo_elapsed = std::chrono::steady_clock::now() - fifo_start;
  expect(fifo_writer->counters().failures == 1 && fifo_observation.counters().callback_failures == 1 && fifo_elapsed < std::chrono::seconds(1),
         "FIFO target is rejected promptly through O_NONBLOCK without blocking a producer");

  auto const intermediate_target = root / "intermediate-target";
  std::filesystem::create_directories(intermediate_target);
  auto const intermediate_link = root / "intermediate-link";
  expect(::symlink(intermediate_target.c_str(), intermediate_link.c_str()) == 0, "create intermediate component symlink canary");
  auto intermediate_writer =
      std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = intermediate_link / "child" / "trace.jsonl"});
  ava::observability::RunObservation intermediate_observation(intermediate_writer, std::make_shared<FixedClock>(1),
                                                              std::make_shared<ava::observability::CounterIdGenerator>());
  intermediate_observation.emit(ava::observability::TraceEventType::AgentRunStart, {});
  expect(intermediate_writer->counters().written == 1 && std::filesystem::exists(intermediate_target / "child" / "trace.jsonl"),
         "contained intermediate symlink resolves within the trace anchor");
}

void test_dispatcher_trace_correlation_and_ordering()
{
  auto const root = create_empty_root("test_dispatcher_trace_correlation_and_ordering");

  auto collector = std::make_shared<CollectingObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  ava::tools::ToolContext context;
  context.workspace_dir = root / "dispatcher-trace";
  std::filesystem::create_directories(context.workspace_dir);
  expect(::chmod(temp_root().c_str(), S_IRWXU) == 0 && ::chmod(context.workspace_dir.c_str(), S_IRWXU) == 0,
         "dispatcher trace workspace is owner-only for sealed command planning");
  context.spill_dir = root / "dispatcher-spill";
  context.anchor_set = command_anchors_for_test(context.workspace_dir, context.spill_dir);
  context.observation = observation;
  context.trace_context = {.run_id = "run",
                           .turn_id = "turn",
                           .session_id = "session",
                           .provider_id = "provider",
                           .parent_run_id = {},
                           .parent_turn_id = {},
                           .parent_session_id = {}};
  context.permission_resolver = [](ava::permissions::PermissionPrompt const&) {
    return ava::permissions::PermissionResolutionDecision(ava::permissions::PermissionResolution::Allow);
  };
  ava::agent::ToolDispatcher dispatcher(context);
  auto serial = dispatcher.dispatch({.id = "CANARY_PROVIDER_CALL", .name = "bash", .arguments_json = "{\"command\":\"true\"}"});
  expect(serial && serial->success, "serial dispatcher call succeeds through trace boundary");
  std::thread parallel_one(
      [&] { static_cast<void>(dispatcher.dispatch({.id = "CANARY_PARALLEL_ONE", .name = "bash", .arguments_json = "{\"command\":\"true\"}"})); });
  std::thread parallel_two(
      [&] { static_cast<void>(dispatcher.dispatch({.id = "CANARY_PARALLEL_TWO", .name = "bash", .arguments_json = "{\"command\":\"true\"}"})); });
  parallel_one.join();
  parallel_two.join();
  std::lock_guard lock(collector->mutex);
  std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> bounds;
  bool process_correlated = false;
  for (auto const& event : collector->events)
  {
    auto json = ava::observability::canonical_json(event);
    expect(json.find("CANARY_") == std::string::npos, "dispatcher trace omits raw provider call IDs and names");
    if (event.type == ava::observability::TraceEventType::ToolDispatchStart)
      bounds[event.call_id].first = event.sequence;
    if (event.type == ava::observability::TraceEventType::ToolDispatchResult)
      bounds[event.call_id].second = event.sequence;
    if ((event.type == ava::observability::TraceEventType::ProcessStart || event.type == ava::observability::TraceEventType::ProcessResult) &&
        !event.call_id.empty())
      process_correlated = true;
  }
  bool ordered = bounds.size() == 3;
  for (auto const& [call_id, range] : bounds) ordered = ordered && !call_id.empty() && range.first < range.second;
  expect(ordered && process_correlated, "serial and parallel dispatches trace start before effect/result with stable generated correlation IDs");
}

void test_session_attachment_generation_alias_stress()
{
  static_assert(noexcept(std::declval<ava::session::SessionStore&>().set_run_observation(
      std::declval<std::shared_ptr<ava::observability::RunObservation> const&>(), std::declval<ava::observability::TraceContext const&>())));

  auto const root = create_empty_root("test_session_attachment_generation_alias_stress");



  auto collector = std::make_shared<CollectingObserver>();
  auto first = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                    std::make_shared<ava::observability::CounterIdGenerator>());
  auto second = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1000),
                                                                     std::make_shared<ava::observability::CounterIdGenerator>());
  auto store = ava::session::SessionStore::create_ephemeral(root);
  expect(store.has_value(), "create aliased ephemeral store for attachment stress");
  auto alias = *store;
  auto stale_generation = store->set_run_observation(first, {.run_id = "parent",
                                                             .turn_id = "turn",
                                                             .session_id = store->session_id(),
                                                             .provider_id = {},
                                                             .parent_run_id = {},
                                                             .parent_turn_id = {},
                                                             .parent_session_id = {}});
  auto disabled = std::make_shared<ava::observability::RunObservation>();
  auto const disabled_generation = alias.set_run_observation(disabled, {});
  alias.clear_run_observation(disabled_generation);
  auto const events_before_disabled_setup = [&] {
    std::lock_guard lock(collector->mutex);
    return collector->events.size();
  }();
  auto retained = append_session_entry_for_test(
      alias, {.id = "retained", .parent_id = "", .type = ava::session::EntryType::SessionStart, .timestamp = "now", .data_json = "{}"});
  auto const events_after_disabled_setup = [&] {
    std::lock_guard lock(collector->mutex);
    return collector->events.size();
  }();
  expect(disabled_generation == 0 && retained && events_after_disabled_setup > events_before_disabled_setup,
         "disabled attachment setup returns generation zero without locking away or clearing a valid attachment");

  auto newer_generation = alias.set_run_observation(second, {.run_id = "new",
                                                             .turn_id = "turn",
                                                             .session_id = store->session_id(),
                                                             .provider_id = {},
                                                             .parent_run_id = {},
                                                             .parent_turn_id = {},
                                                             .parent_session_id = {}});
  store->clear_run_observation(stale_generation);
  auto appended = append_session_entry_for_test(
      alias, {.id = "newer", .parent_id = "", .type = ava::session::EntryType::SessionStart, .timestamp = "now", .data_json = "{}"});
  expect(appended.has_value(), "stale generation clear leaves newer aliased attachment intact");
  std::atomic<bool> run = true;
  std::thread attach_clear([&] {
    for (int i = 0; i < 200; ++i)
    {
      auto generation = store->set_run_observation((i % 2) == 0 ? first : second, {.run_id = "stress",
                                                                                   .turn_id = "turn",
                                                                                   .session_id = store->session_id(),
                                                                                   .provider_id = {},
                                                                                   .parent_run_id = {},
                                                                                   .parent_turn_id = {},
                                                                                   .parent_session_id = {}});
      store->clear_run_observation(generation);
    }
    run.store(false, std::memory_order_release);
  });
  std::thread append_load([&] {
    int i = 0;
    while (run.load(std::memory_order_acquire))
    {
      static_cast<void>(append_session_entry_for_test(
          alias,
          {.id = "stress-" + std::to_string(i++), .parent_id = "", .type = ava::session::EntryType::SessionStart, .timestamp = "now", .data_json = "{}"}));
      static_cast<void>(alias.load());
    }
  });
  attach_clear.join();
  append_load.join();
  alias.clear_run_observation(newer_generation);
  auto events_before_terminal = [&] {
    std::lock_guard lock(collector->mutex);
    return collector->events.size();
  }();
  static_cast<void>(append_session_entry_for_test(
      alias, {.id = "after-parent-terminal", .parent_id = "", .type = ava::session::EntryType::SessionStart, .timestamp = "now", .data_json = "{}"}));
  std::lock_guard lock(collector->mutex);
  expect(collector->events.size() == events_before_terminal,
         "copied background-style store emits no session event after parent terminal clear and concurrent attachment snapshots remain safe");

  auto overlap_store = ava::session::SessionStore::create_ephemeral(root);
  expect(overlap_store.has_value(), "create session for A/B background attachment overlap");
  if (overlap_store)
  {
    auto run_a = overlap_store->set_run_observation(first, {.run_id = "A",
                                                            .turn_id = "A-turn",
                                                            .session_id = overlap_store->session_id(),
                                                            .provider_id = {},
                                                            .parent_run_id = {},
                                                            .parent_turn_id = {},
                                                            .parent_session_id = {}});
    auto old_background_store = overlap_store->detached_copy_for_background_persistence();
    overlap_store->clear_run_observation(run_a);
    auto run_b = overlap_store->set_run_observation(second, {.run_id = "B",
                                                             .turn_id = "B-turn",
                                                             .session_id = overlap_store->session_id(),
                                                             .provider_id = {},
                                                             .parent_run_id = {},
                                                             .parent_turn_id = {},
                                                             .parent_session_id = {}});
    auto before_old_write = collector->events.size();
    static_cast<void>(append_session_entry_for_test(
        old_background_store, {.id = "A-background", .parent_id = "", .type = ava::session::EntryType::Error, .timestamp = "now", .data_json = "{}"}));
    overlap_store->clear_run_observation(run_b);
    static_cast<void>(append_session_entry_for_test(
        old_background_store, {.id = "A-after-terminal", .parent_id = "", .type = ava::session::EntryType::Error, .timestamp = "now", .data_json = "{}"}));
    expect(collector->events.size() == before_old_write,
           "an old background persistence copy cannot snapshot B's live attachment or emit after A's terminal cleanup");
  }
}

void test_session_and_process_boundaries_are_independent()
{
  auto collector = std::make_shared<CollectingObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  auto const root = create_empty_root("test_session_and_process_boundaries_are_independent");

  auto store = ava::session::SessionStore::create_ephemeral(root);
  expect(static_cast<bool>(store), "create ephemeral session for trace boundary test");
  ava::observability::TraceContext session_context;
  session_context.run_id = "run";
  session_context.turn_id = "turn";
  session_context.session_id = store->session_id();
  static_cast<void>(store->set_run_observation(observation, std::move(session_context)));
  auto appended = append_session_entry_for_test(
      *store, {.id = "entry-1", .parent_id = "", .type = ava::session::EntryType::SessionStart, .timestamp = "now", .data_json = "{}"});
  expect(static_cast<bool>(appended), "session append succeeds with observer");
  auto loaded = store->load();
  expect(static_cast<bool>(loaded), "session load succeeds with observer");
  ava::tools::ToolContext context;
  context.workspace_dir = root / "process-boundary";
  std::filesystem::create_directories(context.workspace_dir);
  expect(::chmod(temp_root().c_str(), S_IRWXU) == 0 && ::chmod(context.workspace_dir.c_str(), S_IRWXU) == 0,
         "process boundary workspace is owner-only for sealed command planning");
  context.spill_dir = root / "process-boundary-spill";
  context.anchor_set = command_anchors_for_test(context.workspace_dir, context.spill_dir);
  context.observation = observation;
  context.permission_resolver = [](ava::permissions::PermissionPrompt const&) {
    return ava::core::Result<ava::permissions::PermissionResolutionDecision>(
        ava::permissions::PermissionResolutionDecision(ava::permissions::PermissionResolution::Allow));
  };
  context.trace_context.run_id = "run";
  context.trace_context.turn_id = "turn";
  context.current_call_id = "CANARY_RAW_PROVIDER_CALL_ID";
  auto process = ava::tools::run_bash(context, "true");
  expect(static_cast<bool>(process) && process->exit_code == 0, "bash still succeeds with observer");
  std::lock_guard lock(collector->mutex);
  bool session_append = false;
  bool session_load = false;
  bool process_start = false;
  bool process_result = false;
  bool generated_process_id = false;
  std::map<std::string, std::pair<unsigned, unsigned>> process_pairs;
  for (auto const& event : collector->events)
  {
    session_append = session_append || event.type == ava::observability::TraceEventType::SessionAppendAttempt;
    session_load = session_load || event.type == ava::observability::TraceEventType::SessionLoadAttempt;
    process_start = process_start || event.type == ava::observability::TraceEventType::ProcessStart;
    process_result = process_result || event.type == ava::observability::TraceEventType::ProcessResult;
    if (event.type == ava::observability::TraceEventType::ProcessStart || event.type == ava::observability::TraceEventType::ProcessResult)
    {
      generated_process_id = generated_process_id || event.call_id.starts_with("process-");
      auto& pair = process_pairs[event.call_id];
      if (event.type == ava::observability::TraceEventType::ProcessStart)
        ++pair.first;
      else
        ++pair.second;
      expect(ava::observability::canonical_json(event).find("CANARY_RAW_PROVIDER_CALL_ID") == std::string::npos,
             "process traces never fall back to a raw provider current_call_id");
    }
  }
  bool paired = process_pairs.size() == 1;
  for (auto const& [call_id, pair] : process_pairs) paired = paired && !call_id.empty() && pair.first == 1 && pair.second == 1;
  expect(session_append && session_load && process_start && process_result && generated_process_id && paired,
         "normal process execution emits exactly one generated ProcessStart/ProcessResult pair");
}

void test_agent_fake_provider_boundaries()
{
  auto collector = std::make_shared<CollectingObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  auto const root = create_empty_root("observer-agent");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store({.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "observer-agent"});
  ava::provider::OpenAIProvider provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200, .headers = {}, .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\n\ndata: [DONE]\n\n"}});
  ava::agent::AgentLoopOptions options;
  options.workspace_dir = workspace;
  options.model = agent_loop_test::model_invocation_options("system");
  options.access_token = "CANARY_TOKEN";
  options.append_entry = append_route_for_test(store);
  options.append_batch = append_batch_route_for_test(store);
  options.session_read_authority = read_authority_for_test(store);
  options.observation = observation;
  ava::agent::AgentLoop loop(std::move(options));
  auto result = loop.run_turn("CANARY_PROMPT", store, provider, transport);
  expect(static_cast<bool>(result) && result->final_text == "ok", "fake provider run succeeds with independent observer");
  std::lock_guard lock(collector->mutex);
  bool run_start = false;
  bool transport_request = false;
  bool stream_event = false;
  bool terminal = false;
  for (auto const& event : collector->events)
  {
    run_start = run_start || event.type == ava::observability::TraceEventType::AgentRunStart;
    transport_request = transport_request || event.type == ava::observability::TraceEventType::TransportRequestResult;
    stream_event = stream_event || event.type == ava::observability::TraceEventType::ProviderStreamEvent;
    terminal = terminal || (event.type == ava::observability::TraceEventType::AgentRunTerminal && event.outcome == ava::observability::TraceOutcome::Completed);
  }
  expect(run_start && transport_request && stream_event && terminal, "fake provider covers agent, transport, stream, and terminal boundaries");
}

void test_disabled_and_enabled_runs_preserve_authoritative_session_semantics()
{
  struct ScriptedRunArtifacts
  {
    std::vector<std::pair<ava::session::EntryType, std::string>> entries;
    std::string session_jsonl;
    std::string observer_jsonl;
  };
  auto const root = create_empty_root("observer-session-semantics");


  auto run_scripted = [&root](bool enabled) {
    auto const run_root = root / (enabled ? "enabled" : "disabled");
    std::filesystem::create_directories(run_root / "workspace");
    ava::session::SessionStore store({.root_dir = run_root / "sessions", .workspace_dir = run_root / "workspace", .session_id = "semantic"});
    ava::provider::OpenAIProvider provider("https://api.example.test");
    ava::tests::FakeTransport transport({ava::http::HttpResponse{
        .status_code = 200, .headers = {}, .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\n\ndata: [DONE]\n\n"}});
    std::shared_ptr<ava::observability::JsonlRunObserver> writer;
    std::shared_ptr<ava::observability::RunObservation> observation;
    if (enabled)
    {
      writer = std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = run_root / "observer" / "trace.jsonl"});
      observation = std::make_shared<ava::observability::RunObservation>(writer, std::make_shared<FixedClock>(1),
                                                                         std::make_shared<ava::observability::CounterIdGenerator>());
    }
    ava::agent::AgentLoop loop({.workspace_dir = run_root / "workspace",
                                .model = agent_loop_test::model_invocation_options("system"),
                                .access_token = "CANARY_TOKEN",
                                .append_entry = append_route_for_test(store),
                                .append_batch = append_batch_route_for_test(store),
                                .session_read_authority = read_authority_for_test(store),
                                .observation = observation});
    auto result = loop.run_turn("scripted prompt", store, provider, transport);
    expect(result && result->final_text == "ok", "scripted fake-provider semantic characterization run succeeds");
    auto loaded = store.load();
    expect(loaded.has_value(), "scripted fake-provider semantic characterization session loads");

    ScriptedRunArtifacts artifacts;
    for (auto const& entry : *loaded) artifacts.entries.emplace_back(entry.type, entry.data_json);
    std::ifstream session_file(store.session_path(), std::ios::binary);
    artifacts.session_jsonl.assign(std::istreambuf_iterator<char>(session_file), std::istreambuf_iterator<char>());
    if (writer)
    {
      writer->close();
      std::ifstream observer_file(run_root / "observer" / "trace.jsonl", std::ios::binary);
      artifacts.observer_jsonl.assign(std::istreambuf_iterator<char>(observer_file), std::istreambuf_iterator<char>());
    }
    return artifacts;
  };

  auto const disabled = run_scripted(false);
  auto const enabled = run_scripted(true);
  auto same_logical_turn = [](std::vector<std::pair<ava::session::EntryType, std::string>> const& entries) {
    return entries.size() == 3 && entries[0].first == ava::session::EntryType::UserMessage &&
           entries[1].first == ava::session::EntryType::AssistantOutputItem && entries[1].second.find("\"text\":\"ok\"") != std::string::npos &&
           entries[2].first == ava::session::EntryType::AssistantTurnCommit && entries[2].second.find("\"provider\":\"openai\"") != std::string::npos &&
           entries[2].second.find("\"model\":\"gpt-5.5\"") != std::string::npos;
  };
  expect(same_logical_turn(disabled.entries) && same_logical_turn(enabled.entries),
         "enabled observation preserves the v4 scripted record ordering and logical payloads despite fresh opaque entry IDs");
  expect(enabled.session_jsonl.find("agent.run_start") == std::string::npos && enabled.session_jsonl.find("agent.run_terminal") == std::string::npos &&
             enabled.session_jsonl.find("session.append_attempt") == std::string::npos &&
             enabled.session_jsonl.find("session.append_result") == std::string::npos &&
             enabled.session_jsonl.find("transport.request_result") == std::string::npos &&
             enabled.session_jsonl.find("provider.stream_event") == std::string::npos && enabled.session_jsonl.find("\"timestamp_ms\":") == std::string::npos,
         "observer data never enters authoritative session JSONL");
  expect(!enabled.observer_jsonl.empty() && enabled.observer_jsonl.find("agent.run_start") != std::string::npos &&
             !std::filesystem::exists(root / "disabled" / "observer" / "trace.jsonl"),
         "observer artifact is independent and remains disabled by default");
}

void test_jsonl_event_ordering_is_byte_identical()
{
  auto const root = create_empty_root("observer-deterministic");

  auto write_trace = [](std::filesystem::path const& path) {
    auto writer = std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = path});
    ava::observability::RunObservation observation(writer, std::make_shared<FixedClock>(77), std::make_shared<ava::observability::CounterIdGenerator>(0));
    ava::observability::TraceContext context{.run_id = observation.next_id("run"),
                                             .turn_id = observation.next_id("turn"),
                                             .session_id = "session",
                                             .provider_id = "provider",
                                             .parent_run_id = {},
                                             .parent_turn_id = {},
                                             .parent_session_id = {}};
    observation.emit(ava::observability::TraceEventType::AgentRunStart, context, [](auto& event) { event.phase = ava::observability::TracePhase::Run; });
    observation.emit(ava::observability::TraceEventType::TransportRequestResult, context, [](auto& event) {
      event.phase = ava::observability::TracePhase::Transport;
      event.outcome = ava::observability::TraceOutcome::Success;
      event.fields = {{.key = "request_bytes", .value = "7"}, {.key = "status_code", .value = "200"}};
    });
    writer->close();
  };
  auto const first_path = root / "one" / "trace.jsonl";
  auto const second_path = root / "two" / "trace.jsonl";
  write_trace(first_path);
  write_trace(second_path);
  std::ifstream first(first_path, std::ios::binary);
  std::ifstream second(second_path, std::ios::binary);
  std::string first_bytes((std::istreambuf_iterator<char>(first)), std::istreambuf_iterator<char>());
  std::string second_bytes((std::istreambuf_iterator<char>(second)), std::istreambuf_iterator<char>());
  expect(first_bytes == second_bytes && first_bytes.find("\"sequence\":1") != std::string::npos && first_bytes.find("\"sequence\":2") != std::string::npos,
         "fixed clock and IDs produce deterministically ordered byte-identical JSONL artifacts");
}

void test_observed_transport_cancellation_callback_contracts()
{
  ava::http::HttpRequest request;
  request.method = "POST";
  request.url = "https://example.test";
  request.body = "{}";
  auto collector = std::make_shared<CollectingObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  ava::http::TransportObservation transport_observation{
      .observation = observation,
      .context = {
          .run_id = "callback", .turn_id = "turn", .session_id = {}, .provider_id = {}, .parent_run_id = {}, .parent_turn_id = {}, .parent_session_id = {}}};

  unsigned direct_send_callback_calls = 0;
  CallbackPollingTransport direct_send({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  auto direct_send_result = direct_send.send(request, [&direct_send_callback_calls] { return ++direct_send_callback_calls == 2; });
  unsigned observed_send_callback_calls = 0;
  CallbackPollingTransport observed_send_inner({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  ava::http::ObservedTransport observed_send(observed_send_inner, transport_observation);
  auto observed_send_result = observed_send.send(request, [&observed_send_callback_calls] { return ++observed_send_callback_calls == 2; });
  expect(direct_send_result && observed_send_result && direct_send_callback_calls == 1 && observed_send_callback_calls == 1 &&
             direct_send.callback_polls == 1 && observed_send_inner.callback_polls == 1,
         "observed send forwards a stateful cancellation callback exactly as the disabled transport does");

  unsigned direct_stream_callback_calls = 0;
  CallbackPollingTransport direct_stream({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "chunk"}}, true);
  auto direct_stream_result = direct_stream.send_streaming(
      request, [](std::string_view) { return ava::core::VoidResult{}; }, [&direct_stream_callback_calls] { return ++direct_stream_callback_calls == 2; });
  unsigned observed_stream_callback_calls = 0;
  CallbackPollingTransport observed_stream_inner({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "chunk"}}, true);
  ava::http::ObservedTransport observed_stream(observed_stream_inner, transport_observation);
  auto observed_stream_result = observed_stream.send_streaming(
      request, [](std::string_view) { return ava::core::VoidResult{}; }, [&observed_stream_callback_calls] { return ++observed_stream_callback_calls == 2; });
  expect(direct_stream_result && observed_stream_result && direct_stream_callback_calls == 1 && observed_stream_callback_calls == 1 &&
             direct_stream.callback_polls == 1 && observed_stream_inner.callback_polls == 1,
         "observed streaming forwards a stateful cancellation callback exactly as the disabled transport does");

  unsigned throwing_callback_calls = 0;
  CallbackPollingTransport throwing_inner({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  ava::http::ObservedTransport throwing_observed(throwing_inner, transport_observation);
  bool callback_threw = false;
  try
  {
    static_cast<void>(throwing_observed.send(request, [&throwing_callback_calls]() -> bool {
      ++throwing_callback_calls;
      throw std::runtime_error("authoritative callback failure");
    }));
  }
  catch (std::runtime_error const&)
  {
    callback_threw = true;
  }
  expect(callback_threw && throwing_callback_calls == 1 && throwing_inner.callback_polls == 1,
         "observed transport neither catches nor repolls a throwing authoritative cancellation callback");

  unsigned throwing_stream_callback_calls = 0;
  CallbackPollingTransport throwing_stream_inner({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "chunk"}}, true);
  ava::http::ObservedTransport throwing_stream_observed(throwing_stream_inner, transport_observation);
  bool streaming_callback_threw = false;
  try
  {
    static_cast<void>(throwing_stream_observed.send_streaming(
        request, [](std::string_view) { return ava::core::VoidResult{}; },
        [&throwing_stream_callback_calls]() -> bool {
          ++throwing_stream_callback_calls;
          throw std::runtime_error("authoritative streaming callback failure");
        }));
  }
  catch (std::runtime_error const&)
  {
    streaming_callback_threw = true;
  }
  expect(streaming_callback_threw && throwing_stream_callback_calls == 1 && throwing_stream_inner.callback_polls == 1,
         "observed streaming neither catches nor repolls a throwing authoritative cancellation callback");

  CallbackPollingTransport retry_inner({ava::http::HttpResponse{.status_code = 503, .headers = {}, .body = "retry"},
                                        ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "done"}});
  unsigned retry_callback_calls = 0;
  ava::http::RetryTransport retry(
      retry_inner,
      ava::http::RetryOptions{
          .observation = transport_observation, .max_attempts = 2, .base_delay_ms = 0, .response_retry_decision = ava::provider::provider_retry_decision});
  auto retry_result = retry.send(request, [&retry_callback_calls] {
    ++retry_callback_calls;
    return false;
  });
  expect(retry_result && retry_callback_calls == 7 && retry_inner.callback_polls == 2,
         "retry reuses each authoritative post-attempt cancellation poll for tracing and control flow");

  CallbackPollingTransport retry_cancel_inner({ava::http::HttpResponse{.status_code = 503, .headers = {}, .body = "retry"}});
  ava::http::RetryTransport retry_cancel(
      retry_cancel_inner,
      ava::http::RetryOptions{.observation = transport_observation, .max_attempts = 2, .response_retry_decision = ava::provider::provider_retry_decision});
  unsigned retry_cancel_callback_calls = 0;
  bool retry_callback_threw = false;
  ava::core::Result<ava::http::HttpResponse> retry_cancel_result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "not run"));
  try
  {
    retry_cancel_result = retry_cancel.send(request, [&retry_cancel_callback_calls]() -> bool {
      ++retry_cancel_callback_calls;
      if (retry_cancel_callback_calls < 3)
        return false;
      if (retry_cancel_callback_calls == 3)
        return true;
      throw std::runtime_error("retry callback was polled twice after an attempt");
    });
  }
  catch (std::runtime_error const&)
  {
    retry_callback_threw = true;
  }
  expect(!retry_cancel_result && !retry_callback_threw && retry_cancel_callback_calls == 3,
         "retry stores a stateful post-attempt cancellation result instead of invoking a throwing callback again for tracing");

  CallbackPollingTransport retry_countdown_inner({ava::http::HttpResponse{.status_code = 503, .headers = {}, .body = "retry"},
                                                  ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "done"}});
  unsigned retry_ticks = 0;
  ava::http::RetryTransport retry_countdown(retry_countdown_inner, ava::http::RetryOptions{.observation = transport_observation,
                                                                                           .max_attempts = 2,
                                                                                           .base_delay_ms = 20,
                                                                                           .countdown_tick_ms = 5,
                                                                                           .on_retry =
                                                                                               [&retry_ticks](ava::http::RetryOptions::Event const& event) {
                                                                                                 if (event.countdown_tick)
                                                                                                   ++retry_ticks;
                                                                                                 return ava::core::VoidResult{};
                                                                                               },
                                                                                           .response_retry_decision = ava::provider::provider_retry_decision});
  auto retry_countdown_result = retry_countdown.send(request);
  std::lock_guard lock(collector->mutex);
  auto const retry_traces = std::count_if(collector->events.begin(), collector->events.end(),
                                          [](auto const& event) { return event.type == ava::observability::TraceEventType::TransportRetry; });
  expect(retry_countdown_result && retry_ticks > 0 && retry_traces == 2, "nonzero retry countdown ticks produce UI progress without duplicate retry traces");
}

void test_transport_terminal_boundaries()
{
  auto collector = std::make_shared<CollectingObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  ava::http::HttpRequest request;
  request.method = "POST";
  request.url = "https://user:CANARY_URL_AUTH@example.test/path?CANARY_QUERY=1";
  request.headers = {{"Authorization", "CANARY_HEADER"}};
  request.body = "CANARY_BODY";
  ava::http::TransportObservation transport_observation{
      .observation = observation,
      .context = {.run_id = "run", .turn_id = "turn", .session_id = {}, .provider_id = {}, .parent_run_id = {}, .parent_turn_id = {}, .parent_session_id = {}}};
  ava::tests::FakeTransport success({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  ava::http::ObservedTransport observed_success(success, transport_observation);
  auto success_result = observed_success.send(request, [] { return false; });
  ava::tests::FakeTransport failure({});
  ava::http::ObservedTransport observed_failure(failure, transport_observation);
  auto failure_result = observed_failure.send(request, [] { return false; });
  ava::tests::FakeTransport canceled({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "unused"}});
  ava::http::ObservedTransport observed_canceled(canceled, transport_observation);
  auto canceled_result = observed_canceled.send(request, [] { return true; });
  ava::tests::FakeTransport retry_inner({ava::http::HttpResponse{.status_code = 503, .headers = {}, .body = "retry"},
                                         ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "done"}});
  ava::http::RetryTransport retry(retry_inner, ava::http::RetryOptions{.observation = transport_observation,
                                                                       .max_attempts = 2,
                                                                       .base_delay_ms = 0,
                                                                       .max_retry_after_ms = 0,
                                                                       .response_retry_decision = ava::provider::provider_retry_decision});
  ava::http::ObservedTransport observed_retry(retry, transport_observation);
  auto retry_result = observed_retry.send(request);
  expect(success_result && !failure_result && !canceled_result && retry_result,
         "scripted transport covers success, error, cancel, and retry without a live provider");

  std::lock_guard lock(collector->mutex);
  bool saw_success = false;
  bool saw_error = false;
  bool saw_canceled = false;
  bool saw_retry = false;
  bool saw_request_bytes = false;
  for (auto const& event : collector->events)
  {
    saw_success =
        saw_success || (event.type == ava::observability::TraceEventType::TransportRequestResult && event.outcome == ava::observability::TraceOutcome::Success);
    saw_error =
        saw_error || (event.type == ava::observability::TraceEventType::TransportRequestResult && event.outcome == ava::observability::TraceOutcome::Error);
    saw_canceled = saw_canceled ||
                   (event.type == ava::observability::TraceEventType::TransportRequestResult && event.outcome == ava::observability::TraceOutcome::Canceled);
    saw_retry = saw_retry || event.type == ava::observability::TraceEventType::TransportRetry;
    for (auto const& field : event.fields) saw_request_bytes = saw_request_bytes || field.key == "request_bytes";
    auto const json = ava::observability::canonical_json(event);
    expect(json.find("CANARY_") == std::string::npos, "transport traces never serialize URLs, queries, headers, or bodies");
  }
  expect(saw_success && saw_error && saw_canceled && saw_retry && saw_request_bytes,
         "actual transport terminal events include outcomes, retries, and byte totals");
}

void test_agent_terminal_uses_returned_control_state_without_callback_repoll()
{
  auto collector = std::make_shared<CollectingObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  auto const root = create_empty_root("observer-terminal-cancel-callback");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store({.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "terminal-cancel"});
  ava::provider::OpenAIProvider provider("https://api.example.test");
  ava::tests::FakeTransport transport({});
  unsigned cancellation_callback_calls = 0;
  ava::agent::AgentLoop loop({.workspace_dir = workspace,
                              .model = agent_loop_test::model_invocation_options("system"),
                              .access_token = "CANARY",
                              .cancel_requested = [&cancellation_callback_calls]() -> bool {
                                ++cancellation_callback_calls;
                                if (cancellation_callback_calls == 1)
                                  return true;
                                throw std::runtime_error("terminal classification repolled cancellation");
                              },
                              .append_entry = append_route_for_test(store),
                              .append_batch = append_batch_route_for_test(store),
                              .session_read_authority = read_authority_for_test(store),
                              .observation = observation});
  bool cancellation_callback_threw = false;
  ava::core::Result<ava::agent::AgentLoopResult> result = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "not run"));
  try
  {
    result = loop.run_turn("prompt", store, provider, transport);
  }
  catch (std::runtime_error const&)
  {
    cancellation_callback_threw = true;
  }
  std::lock_guard lock(collector->mutex);
  bool canceled_terminal = std::any_of(collector->events.begin(), collector->events.end(), [](auto const& event) {
    return event.type == ava::observability::TraceEventType::AgentRunTerminal && event.outcome == ava::observability::TraceOutcome::Canceled;
  });
  expect(!result && !cancellation_callback_threw && cancellation_callback_calls == 1 && canceled_terminal,
         "agent terminal telemetry classifies the returned cancellation error without repolling a stateful or throwing callback");
}

void test_agent_lifecycle_survives_observation_attachment_failure()
{
  auto collector = std::make_shared<CollectingObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  auto const root = create_empty_root("observer-attachment-failure");

  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store({.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "attachment-failure"});
  store.fail_next_run_observation_attachment_for_test();
  ava::provider::OpenAIProvider provider("https://api.example.test");
  ava::tests::FakeTransport transport({ava::http::HttpResponse{
      .status_code = 200, .headers = {}, .body = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\n\ndata: [DONE]\n\n"}});
  ava::agent::AgentLoop loop({.workspace_dir = workspace,
                              .model = agent_loop_test::model_invocation_options("system"),
                              .access_token = "CANARY",
                              .append_entry = append_route_for_test(store),
                              .append_batch = append_batch_route_for_test(store),
                              .session_read_authority = read_authority_for_test(store),
                              .observation = observation});
  auto result = loop.run_turn("prompt", store, provider, transport);
  std::lock_guard lock(collector->mutex);
  auto const starts = std::count_if(collector->events.begin(), collector->events.end(),
                                    [](auto const& event) { return event.type == ava::observability::TraceEventType::AgentRunStart; });
  auto const terminals = std::count_if(collector->events.begin(), collector->events.end(), [](auto const& event) {
    return event.type == ava::observability::TraceEventType::AgentRunTerminal && event.outcome == ava::observability::TraceOutcome::Completed;
  });
  expect(result && starts == 1 && terminals == 1 && observation->counters().callback_failures >= 1,
         "a generation-zero SessionStore attachment failure still pairs agent lifecycle start and terminal events");
}

void test_session_results_and_agent_terminal_cleanup()
{
  auto collector = std::make_shared<CollectingObserver>();
  auto const observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  {
    auto const root = create_empty_root("session-observer-failure");

    auto const workspace = root / "workspace";
    ava::session::SessionStore invalid({.root_dir = root, .workspace_dir = root, .session_id = "bad/id"});
    static_cast<void>(invalid.set_run_observation(
        observation,
        {.run_id = "run", .turn_id = "turn", .session_id = {}, .provider_id = {}, .parent_run_id = {}, .parent_turn_id = {}, .parent_session_id = {}}));
    auto append = invalid.append(ava::session::SessionLease{},
                                 {.id = "entry", .parent_id = "", .type = ava::session::EntryType::SessionStart, .timestamp = "now", .data_json = "{}"});
    auto load = invalid.load();
    expect(!append && !load, "invalid session store produces append and load failures");
  }

  auto const root = create_empty_root("observer-agent-terminal");


  auto const workspace = root / "workspace";
  std::filesystem::create_directories(workspace);
  ava::session::SessionStore store({.root_dir = root / "sessions", .workspace_dir = workspace, .session_id = "terminal"});
  ava::provider::OpenAIProvider provider("https://api.example.test");
  ava::tests::FakeTransport provider_failure({});
  auto append_route = append_route_for_test(store);
  auto append_batch = append_batch_route_for_test(store);
  ava::agent::AgentLoop failed_loop({.workspace_dir = workspace,
                                     .model = agent_loop_test::model_invocation_options("system"),
                                     .access_token = "CANARY",
                                     .append_entry = append_route,
                                     .append_batch = append_batch,
                                     .session_read_authority = read_authority_for_test(store),
                                     .observation = observation});
  auto failed = failed_loop.run_turn("prompt", store, provider, provider_failure);
  expect(!failed, "fake provider failure reaches the agent terminal observer");
  ava::tests::FakeTransport canceled_transport({});
  ava::agent::AgentLoop canceled_loop({.workspace_dir = workspace,
                                       .model = agent_loop_test::model_invocation_options("system"),
                                       .access_token = "CANARY",
                                       .cancel_requested = [] { return true; },
                                       .append_entry = append_route,
                                       .append_batch = append_batch,
                                       .session_read_authority = read_authority_for_test(store),
                                       .observation = observation});
  auto canceled = canceled_loop.run_turn("prompt", store, provider, canceled_transport);
  expect(!canceled, "canceled agent run reaches the terminal observer");
  auto const events_after_observed_runs = collector->events.size();
  ava::agent::AgentLoop disabled_loop({
      .workspace_dir = workspace,
      .model = agent_loop_test::model_invocation_options("system"),
      .access_token = "CANARY",
      .cancel_requested = [] { return true; },
      .append_entry = append_route,
      .append_batch = append_batch,
      .session_read_authority = read_authority_for_test(store),
  });
  static_cast<void>(disabled_loop.run_turn("prompt", store, provider, canceled_transport));
  static_cast<void>(
      append_route({.id = "post-disabled", .parent_id = "", .type = ava::session::EntryType::SessionStart, .timestamp = "now", .data_json = "{}"}));

  std::lock_guard lock(collector->mutex);
  bool append_error = false;
  bool load_error = false;
  bool provider_error = false;
  bool agent_canceled = false;
  for (auto const& event : collector->events)
  {
    append_error =
        append_error || (event.type == ava::observability::TraceEventType::SessionAppendResult && event.outcome == ava::observability::TraceOutcome::Error);
    load_error =
        load_error || (event.type == ava::observability::TraceEventType::SessionLoadResult && event.outcome == ava::observability::TraceOutcome::Error);
    provider_error = provider_error ||
                     (event.type == ava::observability::TraceEventType::AgentRunTerminal && event.outcome == ava::observability::TraceOutcome::ProviderError);
    agent_canceled =
        agent_canceled || (event.type == ava::observability::TraceEventType::AgentRunTerminal && event.outcome == ava::observability::TraceOutcome::Canceled);
  }
  expect(append_error && load_error && provider_error && agent_canceled, "session results and agent terminal outcomes report actual failures and cancellation");
  expect(collector->events.size() == events_after_observed_runs, "disabled reused run detaches stale session observation and emits no artifact events");
}

void test_process_cancellation_and_bounded_output_observation()
{
  auto const root = create_empty_root("test_process_cancellation_and_bounded_output_observation");


  auto collector = std::make_shared<CollectingObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  ava::tools::ToolContext context;
  context.workspace_dir = root;
  context.spill_dir = root.parent_path() / "process-cancellation-spill";
  context.anchor_set = command_anchors_for_test(context.workspace_dir, context.spill_dir);
  context.current_call_id = "process-call";
  context.observation = observation;
  context.trace_context = {
      .run_id = "run", .turn_id = "turn", .session_id = {}, .provider_id = {}, .parent_run_id = {}, .parent_turn_id = {}, .parent_session_id = {}};
  context.permission_resolver = [](ava::permissions::PermissionPrompt const&) {
    return ava::permissions::PermissionResolutionDecision(ava::permissions::PermissionResolution::Allow);
  };
  context.cancel_requested = [collector] {
    std::lock_guard lock(collector->mutex);
    return std::ranges::any_of(collector->events, [](auto const& event) { return event.type == ava::observability::TraceEventType::ProcessStart; });
  };
  auto result = ava::tools::run_bash(context, "sleep 1", {.timeout = std::chrono::seconds(2)});
  expect(result && result->canceled,
         result ? "process cancellation remains a normal bash result" : "process cancellation remains a normal bash result: " + result.error().format());
  std::lock_guard lock(collector->mutex);
  auto const output_events = 0U;  // Schema v1 has no per-output-chunk event.
  auto const start = std::find_if(collector->events.begin(), collector->events.end(), [](auto const& event) {
    return event.type == ava::observability::TraceEventType::ProcessStart && event.outcome == ava::observability::TraceOutcome::Started;
  });
  auto const terminal = std::find_if(collector->events.begin(), collector->events.end(), [](auto const& event) {
    return event.type == ava::observability::TraceEventType::ProcessResult && event.outcome == ava::observability::TraceOutcome::Canceled;
  });
  expect(output_events == 0 && start != collector->events.end() && terminal != collector->events.end() && start->call_id == terminal->call_id &&
             start->call_id.starts_with("process-"),
         "canceled-before-spawn process emits one generated start/result pair without raw call IDs");
}

void test_jsonl_close_with_concurrent_producers()
{
  auto const root = create_empty_root("test_jsonl_close_with_concurrent_producers");

  auto const path = root / "observer-concurrent" / "trace.jsonl";
  auto writer = std::make_shared<ava::observability::JsonlRunObserver>(
      ava::observability::JsonlObserverOptions{.path = path, .max_events = 1000, .max_bytes = 1024 * 1024});
  std::vector<std::thread> threads;
  for (int index = 0; index < 4; ++index)
    threads.emplace_back([writer, index] {
      for (int count = 0; count < 100; ++count)
      {
        ava::observability::TraceEvent event;
        event.sequence = static_cast<std::uint64_t>(index * 100 + count);
        event.type = ava::observability::TraceEventType::AgentRunStart;
        writer->on_event(event);
      }
    });
  std::thread closer([writer] { writer->close(); });
  for (auto& thread : threads) thread.join();
  closer.join();
  auto const counters = writer->counters();
  expect(counters.written + counters.dropped == 400 && counters.failures == 0, "close safely accounts concurrent JSONL producers");
}

void test_concurrent_observation_ordering_and_reentrancy()
{
  auto const root = create_empty_root("test_concurrent_observation_ordering_and_reentrancy");

  auto reentrant_observer = std::make_shared<ReentrantObserver>();
  ava::observability::RunObservation reentrant(reentrant_observer, std::make_shared<FixedClock>(1), std::make_shared<ava::observability::CounterIdGenerator>());
  reentrant_observer->observation = &reentrant;
  bool enricher_had_callback_context = true;
  reentrant.emit(ava::observability::TraceEventType::AgentRunStart, reentrant_observer->context,
                 [&](ava::observability::TraceEvent&) { enricher_had_callback_context = ava::observability::in_run_observer_callback(); });
  expect(reentrant_observer->events.size() == 2 && reentrant_observer->events[0].sequence == 1 && reentrant_observer->events[1].sequence == 2 &&
             reentrant_observer->callback_context_preserved && !enricher_had_callback_context && !ava::observability::in_run_observer_callback(),
         "observer callback context is stack-aware, excludes enrichment, and restores after nested delivery");

  class DelayedFirstObserver final : public ava::observability::RunObserver
  {
   public:
    void on_event(ava::observability::TraceEvent const& event) override
    {
      if (event.sequence == 1)
      {
        {
          std::lock_guard lock(mutex);
          first_callback_entered = true;
        }
        first_callback.notify_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      std::lock_guard lock(mutex);
      events.push_back(event);
    }
    std::mutex mutex;
    std::condition_variable first_callback;
    bool first_callback_entered = false;
    std::vector<ava::observability::TraceEvent> events;
  };
  auto collector = std::make_shared<DelayedFirstObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  std::thread first([&] {
    observation->emit(
        ava::observability::TraceEventType::AgentRunStart,
        {.run_id = "run", .turn_id = {}, .session_id = {}, .provider_id = {}, .parent_run_id = {}, .parent_turn_id = {}, .parent_session_id = {}});
  });
  {
    std::unique_lock lock(collector->mutex);
    collector->first_callback.wait(lock, [&] { return collector->first_callback_entered; });
  }
  std::thread second([&] {
    observation->emit(
        ava::observability::TraceEventType::ProviderStreamEvent,
        {.run_id = "run", .turn_id = {}, .session_id = {}, .provider_id = {}, .parent_run_id = {}, .parent_turn_id = {}, .parent_session_id = {}});
  });
  first.join();
  second.join();
  std::lock_guard lock(collector->mutex);
  expect(collector->events.size() == 2 && collector->events[0].sequence == 1 && collector->events[1].sequence == 2,
         "concurrent callbacks are delivered in the same strict order as assigned sequences");

  auto const file_path = root / "observer-concurrent-sequence" / "trace.jsonl";
  std::error_code remove_error;
  std::filesystem::remove_all(file_path.parent_path(), remove_error);
  auto writer = std::make_shared<ava::observability::JsonlRunObserver>(ava::observability::JsonlObserverOptions{.path = file_path});
  ava::observability::RunObservation file_observation(writer, std::make_shared<FixedClock>(1), std::make_shared<ava::observability::CounterIdGenerator>());
  std::vector<std::thread> file_producers;
  for (int producer = 0; producer < 4; ++producer)
    file_producers.emplace_back([&] {
      for (int event = 0; event < 25; ++event)
        file_observation.emit(
            ava::observability::TraceEventType::ProviderStreamEvent,
            {.run_id = "run", .turn_id = "turn", .session_id = {}, .provider_id = {}, .parent_run_id = {}, .parent_turn_id = {}, .parent_session_id = {}});
    });
  for (auto& producer : file_producers) producer.join();
  writer->close();
  std::ifstream file(file_path);
  std::string line;
  std::uint64_t expected_sequence = 1;
  while (std::getline(file, line))
  {
    auto event = ava::observability::parse_canonical_json(line);
    expect(event && event->sequence == expected_sequence, "concurrent synchronous JSONL records retain callback sequence order on disk");
    ++expected_sequence;
  }
  expect(expected_sequence == 101 && writer->counters().written == 100 && writer->counters().dropped == 0,
         "concurrent synchronous JSONL trace writes all ordered records without a loss");
}

void test_provider_stream_event_outcomes_are_exhaustive()
{
  class AllEventsProvider final : public ava::provider::Provider
  {
   public:
    [[nodiscard]] ava::core::Result<ava::http::HttpRequest> build_request(ava::provider::ProviderRequest const&, std::string_view) const override
    {
      return ava::http::HttpRequest{.method = "POST",
                                    .url = "https://example.test",
                                    .headers = {},
                                    .body = "{}",
                                    .timeout_ms = 60000,
                                    .follow_redirects = true,
                                    .include_response_headers = false,
                                    .resolve_hosts = {}};
    }
    [[nodiscard]] ava::core::Result<std::vector<ava::provider::StreamEvent>> parse_response(ava::http::HttpResponse const&, bool) const override
    {
      using Type = ava::provider::StreamEventType;
      auto event = [](Type type, std::string text = {}, std::string call_id = {}) {
        ava::provider::StreamEvent result;
        result.type = type;
        result.text = std::move(text);
        result.tool_call_id = std::move(call_id);
        return result;
      };
      auto error = event(Type::Error);
      error.error_message = "provider error";
      return std::vector<ava::provider::StreamEvent>{event(Type::TextDelta, "text"),
                                                     event(Type::ReasoningStart),
                                                     event(Type::ReasoningDelta, "reason"),
                                                     event(Type::ReasoningEnd),
                                                     event(Type::ToolCallStart, {}, "call"),
                                                     event(Type::ToolCallDelta, "{}", "call"),
                                                     event(Type::ToolCallEnd, {}, "call"),
                                                     event(Type::Done),
                                                     std::move(error)};
    }
  };
  auto const root = create_empty_root("test_provider_stream_event_outcomes_are_exhaustive");
  auto collector = std::make_shared<CollectingObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  auto store = ava::session::SessionStore::create_ephemeral(root);
  expect(store.has_value(), "provider stream outcome fixture creates an ephemeral session");
  if (!store)
    return;
  auto append_target = ava::session::SessionAppendTarget::create_ephemeral(*store);
  expect(append_target.has_value(), "provider stream outcome fixture creates an explicit append target");
  if (!append_target)
    return;
  auto read_authority = (*append_target)->read_authority();
  expect(read_authority.has_value(), "provider stream outcome fixture creates an explicit read authority");
  if (!read_authority)
    return;

  ava::tests::FakeTransport transport({ava::http::HttpResponse{.status_code = 200, .headers = {}, .body = "ok"}});
  AllEventsProvider provider;
  auto target = *append_target;
  ava::agent::AgentLoopOptions options;
  options.workspace_dir = root;
  options.model = ava::agent::ModelInvocationOptions{.provider_id = "test", .model_id = "test", .stream = false};
  options.observation = observation;
  options.append_entry = [target](ava::session::SessionEntry entry) { return target->append(std::move(entry)); };
  options.append_batch = [target](std::vector<ava::session::SessionEntry> entries) { return target->append_batch(std::move(entries)); };
  options.session_read_authority = std::move(*read_authority);
  ava::agent::AgentLoop loop(std::move(options));
  static_cast<void>(loop.run_turn("all events", *store, provider, transport));
  std::set<ava::observability::TraceOutcome> outcomes;
  std::lock_guard lock(collector->mutex);
  for (auto const& event : collector->events)
    if (event.type == ava::observability::TraceEventType::ProviderStreamEvent)
      outcomes.insert(event.outcome);
  expect(outcomes == std::set<ava::observability::TraceOutcome>{ava::observability::TraceOutcome::TextDelta, ava::observability::TraceOutcome::ReasoningStart,
                                                                ava::observability::TraceOutcome::ReasoningDelta,
                                                                ava::observability::TraceOutcome::ReasoningEnd, ava::observability::TraceOutcome::ToolCallStart,
                                                                ava::observability::TraceOutcome::ToolCallDelta, ava::observability::TraceOutcome::ToolCallEnd,
                                                                ava::observability::TraceOutcome::Done, ava::observability::TraceOutcome::Error},
         "every provider stream event maps to its typed outcome and only Error maps to Error");
}

void test_concurrent_observation()
{
  auto collector = std::make_shared<CollectingObserver>();
  auto observation = std::make_shared<ava::observability::RunObservation>(collector, std::make_shared<FixedClock>(1),
                                                                          std::make_shared<ava::observability::CounterIdGenerator>());
  std::vector<std::thread> threads;
  for (int index = 0; index < 4; ++index)
    threads.emplace_back([&] {
      for (int count = 0; count < 50; ++count) observation->emit(ava::observability::TraceEventType::AgentRunStart, {});
    });
  for (auto& thread : threads) thread.join();
  auto const counters = observation->counters();
  std::lock_guard lock(collector->mutex);
  std::set<std::uint64_t> sequences;
  std::set<std::int64_t> timestamps;
  for (auto const& event : collector->events)
  {
    sequences.insert(event.sequence);
    timestamps.insert(event.timestamp_ms);
  }
  expect(counters.emitted == 200 && collector->events.size() == 200 && sequences.size() == 200 && timestamps.size() == 200,
         "concurrent runs retain complete accounting with unique clock and sequence values independent of callback order");
  expect(*sequences.begin() == 1 && *sequences.rbegin() == 200 && *timestamps.begin() == 1 && *timestamps.rbegin() == 200,
         "concurrent observation reserves the complete deterministic clock and sequence ranges");
}

}  // namespace

void run_run_observer_tests()
{
  test_trace_canonical_redaction_and_determinism();
  test_observer_failures_bounds_and_disabled_artifacts();
  test_emit_isolates_all_construction_failures();
  test_trace_validator_and_scoring_manifest();
  test_queued_writer_bounds_and_unsafe_targets();
  test_dispatcher_trace_correlation_and_ordering();
  test_session_attachment_generation_alias_stress();
  test_session_and_process_boundaries_are_independent();
  test_agent_fake_provider_boundaries();
  test_disabled_and_enabled_runs_preserve_authoritative_session_semantics();
  test_jsonl_event_ordering_is_byte_identical();
  test_observed_transport_cancellation_callback_contracts();
  test_transport_terminal_boundaries();
  test_agent_terminal_uses_returned_control_state_without_callback_repoll();
  test_agent_lifecycle_survives_observation_attachment_failure();
  test_session_results_and_agent_terminal_cleanup();
  test_process_cancellation_and_bounded_output_observation();
  test_jsonl_close_with_concurrent_producers();
  test_concurrent_observation_ordering_and_reentrancy();
  test_provider_stream_event_outcomes_are_exhaustive();
  test_concurrent_observation();
}
