#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/observability/trace_context.h"
#include "ava/core/thread.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ava::observability {

inline constexpr int kTraceSchemaVersion = 1;

enum class TraceEventType
{
  AgentRunStart,
  AgentRunTerminal,
  TransportRequestResult,
  TransportAttemptResult,
  TransportRetry,
  SessionAppendAttempt,
  SessionAppendResult,
  SessionLoadAttempt,
  SessionLoadResult,
  ProviderStreamEvent,
  ToolDispatchStart,
  ToolDispatchResult,
  ProcessStart,
  ProcessResult
};
enum class TracePhase
{
  None,
  Run,
  Transport,
  Session,
  Provider,
  Tool,
  Process
};
enum class TraceOutcome
{
  None,
  Started,
  Completed,
  Success,
  Error,
  Canceled,
  ProviderError,
  ToolError,
  SessionError,
  Failed,
  Retrying,
  TextDelta,
  ToolCallStart,
  ToolCallDelta,
  ToolCallEnd,
  ReasoningStart,
  ReasoningDelta,
  ReasoningEnd,
  Done
};
enum class FieldProvenance
{
  PublicMetadata,
  Path,
  Content,
  Secret,
  AuthorizationHeader,
  Environment
};

[[nodiscard]] std::string_view to_string(TraceEventType value) noexcept;
[[nodiscard]] std::string_view to_string(TracePhase value) noexcept;
[[nodiscard]] std::string_view to_string(TraceOutcome value) noexcept;

struct TraceField
{
  std::string key;
  std::string value;
  FieldProvenance provenance = FieldProvenance::PublicMetadata;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
struct TraceEvent
{
  int schema_version = kTraceSchemaVersion;
  std::uint64_t sequence = 0;
  std::int64_t timestamp_ms = 0;
  TraceEventType type = TraceEventType::AgentRunStart;
  std::string run_id;
  std::string turn_id;
  std::string call_id;
  std::string session_id;
  std::string provider_id;
  std::string parent_run_id;
  std::string parent_turn_id;
  std::string parent_session_id;
  TracePhase phase = TracePhase::None;
  TraceOutcome outcome = TraceOutcome::None;
  std::vector<TraceField> fields;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

class Clock
{
 public:
  virtual ~Clock() = default;
  [[nodiscard]] virtual std::int64_t now_ms() = 0;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
class IdGenerator
{
 public:
  virtual ~IdGenerator() = default;
  [[nodiscard]] virtual std::string next(std::string_view prefix) = 0;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
class SystemClock final : public Clock
{
 public:
  [[nodiscard]] std::int64_t now_ms() override;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
class CounterIdGenerator final : public IdGenerator
{
 public:
  explicit CounterIdGenerator(std::uint64_t initial = 0) : next_(initial) { }
  [[nodiscard]] std::string next(std::string_view prefix) override;

 private:
  std::atomic<std::uint64_t> next_;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ObserverCounters
{
  std::uint64_t emitted = 0;
  std::uint64_t callback_failures = 0;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
class RunObserver
{
 public:
  virtual void on_event(TraceEvent const& event) = 0;
  virtual ~RunObserver() = default;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// True only while the current thread is inside a user RunObserver callback.
// Nested observation restores the outer callback context on return.
[[nodiscard]] bool in_run_observer_callback() noexcept;

// The sole production emission boundary. Factories, clocks, IDs, enrichment,
// serialization and callbacks are all isolated here; observation can never
// alter the observed operation, including from noexcept destructors.
class RunObservation final
{
 public:
  RunObservation() = default;
  RunObservation(std::shared_ptr<RunObserver> observer, std::shared_ptr<Clock> clock = std::make_shared<SystemClock>(),
                 std::shared_ptr<IdGenerator> ids = std::make_shared<CounterIdGenerator>());
  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] std::string next_id(std::string_view prefix) const noexcept;
  // Use for failures in setup paths that occur before emit can own them.
  void account_external_failure() const noexcept;
  template <class Enricher>
  void emit(TraceEventType type, TraceContext const& context, Enricher&& enrich) const noexcept
  {
    if (!enabled())
      return;
    try
    {
      // Sequence assignment and delivery form one ordered critical section.
      // recursive_mutex permits an observer to emit a nested diagnostic event
      // without deadlocking itself; other producers cannot overtake it.
      std::lock_guard<std::recursive_mutex> lock(state_->emit_mutex);
      TraceEvent event = make_event(type, context);
      std::forward<Enricher>(enrich)(event);
      invoke_observer(event);
      state_->emitted.fetch_add(1, std::memory_order_relaxed);
    }
    catch (...)
    {
      account_failure();
    }
  }
  void emit(TraceEventType type, TraceContext const& context) const noexcept
  {
    emit(type, context, [](TraceEvent&) { });
  }
  [[nodiscard]] ObserverCounters counters() const noexcept;

 private:
  struct State
  {
    std::shared_ptr<RunObserver> observer;
    std::shared_ptr<Clock> clock;
    std::shared_ptr<IdGenerator> ids;
    std::recursive_mutex emit_mutex;
    std::atomic<std::uint64_t> sequence = 0;
    std::atomic<std::uint64_t> emitted = 0;
    std::atomic<std::uint64_t> callback_failures = 0;
    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };
  [[nodiscard]] TraceEvent make_event(TraceEventType type, TraceContext const& context) const;
  void invoke_observer(TraceEvent const& event) const;
  void account_failure() const noexcept;
  std::shared_ptr<State> state_;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct JsonlObserverOptions
{
  std::filesystem::path path;
  std::size_t max_events = 10'000;
  std::size_t max_bytes = 10 * 1024 * 1024;
  std::size_t max_event_bytes = 16 * 1024;
  // Optional borrowed descriptor. The observer duplicates and verifies it in
  // its constructor, so asynchronous writers never need to reopen by path.
  int initial_fd = -1;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
struct JsonlObserverCounters
{
  std::uint64_t written = 0;
  std::uint64_t dropped = 0;
  std::uint64_t failures = 0;
  std::size_t bytes_written = 0;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Deterministic synchronous JSONL mode, retained for tests and small local runs.
// It owns one descriptor and nonblocking lifetime lock for its entire lifetime.
class JsonlRunObserver final : public RunObserver
{
 public:
  explicit JsonlRunObserver(JsonlObserverOptions options);
  ~JsonlRunObserver() noexcept override;
  JsonlRunObserver(JsonlRunObserver const&) = delete;
  JsonlRunObserver& operator=(JsonlRunObserver const&) = delete;
  void on_event(TraceEvent const& event) override;
  void close() noexcept;
  [[nodiscard]] JsonlObserverCounters counters() const noexcept;

 private:
  friend class QueuedJsonlRunObserver;
  void write_record(std::string record);
  void ensure_open();
  JsonlObserverOptions options_;
  mutable std::mutex mutex_;
  bool closed_ = false;
  int fd_ = -1;
  std::uint64_t written_ = 0, dropped_ = 0, failures_ = 0;
  std::size_t bytes_written_ = 0;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct QueuedJsonlObserverOptions : JsonlObserverOptions
{
  std::size_t max_queue_events = 1024;
  std::size_t max_queue_bytes = 1024 * 1024;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Pure capacity check for the serialized-item queue. It deliberately does not
// depend on scheduling or I/O, so admission is deterministic at its bounds.
[[nodiscard]] bool queue_has_capacity(std::size_t queued_events, std::size_t queued_bytes, std::size_t record_bytes, std::size_t max_queue_events,
                                      std::size_t max_queue_bytes) noexcept;
struct QueuedJsonlObserverCounters : JsonlObserverCounters
{
  std::uint64_t queue_dropped = 0;
  std::uint64_t queue_failures = 0;
  std::size_t queue_bytes = 0;
  std::size_t queue_high_water_bytes = 0;
  std::size_t queue_high_water_events = 0;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Production writer. on_event only serializes one bounded record and takes a
// brief queue mutex; it never waits for I/O. It drops only while closing or
// when queue_has_capacity rejects the configured item/byte limits. Normal close drains
// accepted records then joins its owned jthread. A regular-file write already
// in progress is an OS-uninterruptible residual shutdown limit; no thread is
// detached and no memory is left unbounded.
class QueuedJsonlRunObserver final : public RunObserver
{
 public:
  explicit QueuedJsonlRunObserver(QueuedJsonlObserverOptions options);
  ~QueuedJsonlRunObserver() noexcept override;
  QueuedJsonlRunObserver(QueuedJsonlRunObserver const&) = delete;
  QueuedJsonlRunObserver& operator=(QueuedJsonlRunObserver const&) = delete;
  void on_event(TraceEvent const& event) override;
  void close() noexcept;
  [[nodiscard]] QueuedJsonlObserverCounters counters() const noexcept;

 private:
  void run(std::stop_token stop) noexcept;
  QueuedJsonlObserverOptions options_;
  JsonlRunObserver writer_;
  std::once_flag close_once_;
  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  std::deque<std::string> queue_;
  std::size_t queue_bytes_ = 0, high_water_bytes_ = 0, high_water_events_ = 0;
  std::atomic<std::uint64_t> queue_dropped_ = 0;
  std::atomic<std::uint64_t> queue_failures_ = 0;
  bool closing_ = false;
  ava::core::JoinThread worker_;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TraceAccounting
{
  std::uint64_t callback_failures = 0;
  std::uint64_t dropped_events = 0;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
struct TraceValidationResult
{
  bool valid = true;
  std::vector<std::string> errors;
  std::uint64_t accounted_failures = 0;
  std::uint64_t accounted_drops = 0;
  unsigned score = 100;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};
enum class TraceRequiredBoundary
{
  RunLifecycle,
  TextDelta,
  ToolDispatch,
  ProviderErrorTerminal,
  CanceledTerminal,
};
struct TraceFixturePolicy
{
  std::string fixture_id;
  unsigned denominator = 0;
  unsigned lifecycle_weight = 0;
  unsigned required_boundary_weight = 0;
  std::vector<TraceRequiredBoundary> required_boundaries;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Deterministic evaluation seam: product code supplies ordered trace records
// plus separately-accounted observer losses. Losses lower score but do not
// masquerade as lifecycle violations.
[[nodiscard]] TraceValidationResult validate_and_score_trace(std::span<TraceEvent const> events, TraceAccounting accounting = {},
                                                             TraceFixturePolicy const* policy = nullptr);
[[nodiscard]] std::string canonical_json(TraceEvent const& event, std::size_t max_event_bytes = 16 * 1024);
// Parses the schema-v1 fields needed by the deterministic scorer. Fields are
// retained as an opaque validated JSON object because scoring only consumes
// lifecycle metadata.
[[nodiscard]] std::optional<TraceEvent> parse_canonical_json(std::string_view json);
[[nodiscard]] std::string to_string(FieldProvenance provenance);

}  // namespace ava::observability
