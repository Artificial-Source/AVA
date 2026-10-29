#include "sys.h"
#include "ava/diagnostics/artifact_store.h"
#include "ava/diagnostics/runtime_diagnostics.h"
#include "ava/diagnostics/safe_failure.h"
#include "ava/core/AnchorOpen.h"
#include "ava/core/hex.h"
#include "ava/core/saturating.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#include "debug.h"

namespace ava::diagnostics {
namespace {

constexpr std::size_t kTraceMaxBytes = 10U * 1024U * 1024U;
constexpr std::size_t kTraceMaxEvents = 10'000;
constexpr std::size_t kMaxAliasesPerIdentityClass = 20'000;
// Summaries get 10% of the 10,000-record budget, leaving 9,000 lifecycle slots.
// Their opaque identities and two numeric fields keep each below 1 KiB, so
// 1,000 summaries cannot exhaust the byte budget or the 1,024-record queue.
constexpr std::size_t kTraceMaxDeltaSegments = kTraceMaxEvents / 10;

struct ProductionTraceCounters
{
  std::atomic<std::uint64_t> runtime_starts = 0;
  std::atomic<std::uint64_t> provider_requests = 0;
  std::atomic<std::uint64_t> provider_failures = 0;
  std::atomic<std::uint64_t> session_failures = 0;
  std::atomic<std::uint64_t> plugin_failures = 0;
  std::atomic<std::uint64_t> mcp_failures = 0;
  std::atomic<std::uint64_t> delta_summaries_refused = 0;
};

std::string random_token()
{
  std::array<unsigned char, 16> bytes{};
  std::size_t offset = 0;
  while (offset < bytes.size())
  {
    auto const count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    offset += static_cast<std::size_t>(count);
  }
  if (offset != bytes.size())
  {
    static std::atomic<std::uint64_t> sequence{0};
    auto value = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^ (static_cast<std::uint64_t>(::getpid()) << 32U) ^
                 sequence.fetch_add(1, std::memory_order_relaxed);
    for (auto& byte : bytes)
    {
      value ^= value << 13U;
      value ^= value >> 7U;
      value ^= value << 17U;
      byte = static_cast<unsigned char>(value & 0xffU);
    }
  }
  return ava::core::hex_encode(bytes);
}

bool decimal_value(std::string_view value) noexcept
{
  if (value.empty() || value.size() > 20)
    return false;
  std::size_t index = value.front() == '-' ? 1U : 0U;
  if (index == value.size())
    return false;
  for (; index < value.size(); ++index)
    if (value[index] < '0' || value[index] > '9')
      return false;
  return true;
}

bool boolean_value(std::string_view value) noexcept
{
  return value == "true" || value == "false";
}

bool safe_numeric_field(std::string_view key) noexcept
{
  constexpr std::array values{"text_bytes",   "arguments_bytes", "result_bytes", "output_bytes", "request_bytes", "status_code",
                              "next_attempt", "max_attempts",    "delay_ms",     "entry_count",  "delta_count"};
  for (auto const value : values)
    if (key == value)
      return true;
  return false;
}

bool safe_boolean_field(std::string_view key) noexcept
{
  constexpr std::array values{"usage_present", "streaming", "ephemeral", "containment_applied"};
  for (auto const value : values)
    if (key == value)
      return true;
  return false;
}

std::int64_t now_seconds() noexcept
{
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

ComponentClass component_for(RuntimeFailureClass failure_class) noexcept
{
  switch (failure_class)
  {
    case RuntimeFailureClass::Configuration:
      return ComponentClass::Configuration;
    case RuntimeFailureClass::Provider:
      return ComponentClass::Provider;
    case RuntimeFailureClass::Session:
      return ComponentClass::Session;
    case RuntimeFailureClass::Tool:
      return ComponentClass::Tool;
    case RuntimeFailureClass::Runtime:
      return ComponentClass::Runtime;
  }
  return ComponentClass::Runtime;
}

ava::observability::QueuedJsonlObserverOptions production_observer_options(std::filesystem::path path, int trace_fd)
{
  ava::observability::QueuedJsonlObserverOptions options;
  options.path = std::move(path);
  options.initial_fd = trace_fd;
  options.max_events = kTraceMaxEvents;
  options.max_bytes = kTraceMaxBytes;
  options.max_event_bytes = 16U * 1024U;
  return options;
}

class PrivateTraceObserver final : public ava::observability::RunObserver
{
 public:
  PrivateTraceObserver(std::filesystem::path path, int trace_fd, std::shared_ptr<ProductionTraceCounters> counters)
      : writer_(production_observer_options(std::move(path), trace_fd)), counters_(std::move(counters))
  {
  }

  void on_event(ava::observability::TraceEvent const& event) override
  {
    std::lock_guard lock(mutex_);
    account(event);
    if (closed_)
    {
      refused_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (is_delta(event))
    {
      if (pending_ && !same_segment(*pending_, event))
        flush_delta();
      if (!pending_)
      {
        // Retain only identity and closed metadata, never the delta's content.
        pending_ = event;
        pending_->fields.clear();
      }
      delta_count_ = ava::core::saturating_add(delta_count_, std::uint64_t{1});
      for (auto const& field : event.fields)
      {
        if (field.provenance != ava::observability::FieldProvenance::PublicMetadata)
          continue;
        if (field.key != "text_bytes")
          continue;
        std::uint64_t bytes = 0;
        auto const parsed = std::from_chars(field.value.data(), field.value.data() + field.value.size(), bytes);
        if (parsed.ec == std::errc{} && parsed.ptr == field.value.data() + field.value.size())
          delta_bytes_ = ava::core::saturating_add(delta_bytes_, bytes);
      }
      return;
    }
    flush_delta();
    admit(event);
  }

  void close() noexcept
  {
    try
    {
      std::lock_guard lock(mutex_);
      if (!closed_)
      {
        closed_ = true;
        flush_delta();
      }
    }
    catch (...)
    {
      admission_failures_.fetch_add(1, std::memory_order_relaxed);
    }
    // Allocation or summary failure must never skip the writer's drain/join.
    writer_.close();
  }

  [[nodiscard]] ava::observability::QueuedJsonlObserverCounters writer_counters() const noexcept
  {
    auto result = writer_.counters();
    result.dropped = ava::core::saturating_add(result.dropped, refused_.load(std::memory_order_relaxed));
    result.failures = ava::core::saturating_add(result.failures, admission_failures_.load(std::memory_order_relaxed));
    return result;
  }

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  using AliasMap = std::unordered_map<std::string, std::string>;

  static bool is_delta(ava::observability::TraceEvent const& event) noexcept
  {
    using ava::observability::TraceOutcome;
    return event.type == ava::observability::TraceEventType::ProviderStreamEvent &&
           (event.outcome == TraceOutcome::TextDelta || event.outcome == TraceOutcome::ReasoningDelta || event.outcome == TraceOutcome::ToolCallDelta);
  }

  static bool same_segment(ava::observability::TraceEvent const& left, ava::observability::TraceEvent const& right) noexcept
  {
    return left.outcome == right.outcome && left.phase == right.phase && left.run_id == right.run_id && left.turn_id == right.turn_id &&
           left.call_id == right.call_id && left.session_id == right.session_id && left.provider_id == right.provider_id &&
           left.parent_run_id == right.parent_run_id && left.parent_turn_id == right.parent_turn_id && left.parent_session_id == right.parent_session_id;
  }

  void flush_delta()
  {
    if (!pending_)
      return;
    auto segment = std::move(*pending_);
    pending_.reset();
    auto const count = std::exchange(delta_count_, 0);
    auto const bytes = std::exchange(delta_bytes_, 0);
    if (delta_segments_ >= kTraceMaxDeltaSegments)
    {
      counters_->delta_summaries_refused.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    ++delta_segments_;
    segment.fields = {{.key = "delta_count", .value = std::to_string(count)}, {.key = "text_bytes", .value = std::to_string(bytes)}};
    admit(segment);
  }

  static bool has_alias_capacity(AliasMap const& aliases, std::string const& source, std::string const& parent = {})
  {
    std::size_t needed = !source.empty() && !aliases.contains(source) ? 1 : 0;
    if (!parent.empty() && parent != source && !aliases.contains(parent))
      ++needed;
    return needed <= kMaxAliasesPerIdentityClass - aliases.size();
  }

  void admit(ava::observability::TraceEvent const& event)
  {
    auto const health = writer_.counters();
    // No alias insertion, sanitization or serialization once output is saturated.
    if (health.written >= kTraceMaxEvents || health.bytes_written >= kTraceMaxBytes || health.dropped != 0 || health.failures != 0 ||
        !has_alias_capacity(run_aliases_, event.run_id, event.parent_run_id) || !has_alias_capacity(turn_aliases_, event.turn_id, event.parent_turn_id) ||
        !has_alias_capacity(session_aliases_, event.session_id, event.parent_session_id) || !has_alias_capacity(call_aliases_, event.call_id) ||
        !has_alias_capacity(provider_aliases_, event.provider_id))
    {
      refused_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    writer_.on_event(sanitize(event));
  }

  std::string alias(AliasMap& aliases, std::string_view domain, std::string const& source)
  {
    if (source.empty())
      return {};
    if (auto const found = aliases.find(source); found != aliases.end())
      return found->second;
    auto value = std::string(domain) + '-' + random_token();
    aliases.emplace(source, value);
    return value;
  }

  ava::observability::TraceEvent sanitize(ava::observability::TraceEvent const& source)
  {
    ava::observability::TraceEvent safe{.schema_version = source.schema_version,
                                        .sequence = source.sequence,
                                        .timestamp_ms = source.timestamp_ms,
                                        .type = source.type,
                                        .run_id = alias(run_aliases_, "run", source.run_id),
                                        .turn_id = alias(turn_aliases_, "turn", source.turn_id),
                                        .call_id = alias(call_aliases_, "call", source.call_id),
                                        .session_id = alias(session_aliases_, "session", source.session_id),
                                        .provider_id = alias(provider_aliases_, "provider", source.provider_id),
                                        .parent_run_id = alias(run_aliases_, "run", source.parent_run_id),
                                        .parent_turn_id = alias(turn_aliases_, "turn", source.parent_turn_id),
                                        .parent_session_id = alias(session_aliases_, "session", source.parent_session_id),
                                        .phase = source.phase,
                                        .outcome = source.outcome,
                                        .fields = {}};
    safe.fields.reserve(source.fields.size());
    for (auto const& field : source.fields)
    {
      if (field.provenance != ava::observability::FieldProvenance::PublicMetadata)
        continue;
      if (safe_numeric_field(field.key) && decimal_value(field.value))
        safe.fields.push_back({.key = field.key, .value = field.value});
      else if (safe_boolean_field(field.key) && boolean_value(field.value))
        safe.fields.push_back({.key = field.key, .value = field.value});
    }
    return safe;
  }

  void account(ava::observability::TraceEvent const& event)
  {
    using ava::observability::TraceEventType;
    using ava::observability::TraceOutcome;
    if (event.type == TraceEventType::AgentRunStart)
      counters_->runtime_starts.fetch_add(1, std::memory_order_relaxed);
    if (event.type == TraceEventType::TransportRequestResult)
    {
      counters_->provider_requests.fetch_add(1, std::memory_order_relaxed);
      if (event.outcome == TraceOutcome::Error || event.outcome == TraceOutcome::ProviderError)
        counters_->provider_failures.fetch_add(1, std::memory_order_relaxed);
    }
    else if (event.type == TraceEventType::ProviderStreamEvent && (event.outcome == TraceOutcome::Error || event.outcome == TraceOutcome::ProviderError))
    {
      counters_->provider_failures.fetch_add(1, std::memory_order_relaxed);
    }
    if ((event.type == TraceEventType::SessionAppendResult || event.type == TraceEventType::SessionLoadResult) &&
        (event.outcome == TraceOutcome::Error || event.outcome == TraceOutcome::SessionError || event.outcome == TraceOutcome::Failed))
    {
      counters_->session_failures.fetch_add(1, std::memory_order_relaxed);
    }

    if (event.type == TraceEventType::ToolDispatchStart && !event.call_id.empty())
    {
      for (auto const& field : event.fields)
      {
        if (field.key != "tool_name" || field.provenance != ava::observability::FieldProvenance::PublicMetadata)
          continue;
        auto component = external_tool_component(field.value);
        if (component)
        {
          if (tool_components_.size() < kMaxAliasesPerIdentityClass)
            tool_components_.insert_or_assign(event.call_id, *component);
        }
        break;
      }
    }
    if (event.type == TraceEventType::ToolDispatchResult && !event.call_id.empty())
    {
      std::optional<ComponentClass> component;
      {
        auto const found = tool_components_.find(event.call_id);
        if (found != tool_components_.end())
        {
          component = found->second;
          tool_components_.erase(found);
        }
      }
      if (event.outcome != TraceOutcome::Error)
        return;
      if (component == ComponentClass::Mcp)
        counters_->mcp_failures.fetch_add(1, std::memory_order_relaxed);
      else if (component == ComponentClass::Plugin)
        counters_->plugin_failures.fetch_add(1, std::memory_order_relaxed);
    }
  }

  ava::observability::QueuedJsonlRunObserver writer_;
  std::shared_ptr<ProductionTraceCounters> counters_;
  std::mutex mutex_;
  bool closed_ = false;
  std::optional<ava::observability::TraceEvent> pending_;
  std::uint64_t delta_count_ = 0;
  std::uint64_t delta_bytes_ = 0;
  std::size_t delta_segments_ = 0;
  std::atomic<std::uint64_t> refused_ = 0;
  std::atomic<std::uint64_t> admission_failures_ = 0;
  AliasMap run_aliases_;
  AliasMap turn_aliases_;
  AliasMap call_aliases_;
  AliasMap session_aliases_;
  AliasMap provider_aliases_;
  std::unordered_map<std::string, ComponentClass> tool_components_;
};

}  // namespace

TraceWriterHealth trace_writer_health_from_counters(ava::observability::QueuedJsonlObserverCounters const& counters) noexcept
{
  auto const maximum = std::numeric_limits<std::uint64_t>::max();
  auto const events_dropped = ava::core::saturating_add(counters.dropped, counters.queue_dropped);
  auto const bytes_written = counters.bytes_written > static_cast<std::size_t>(maximum) ? maximum : static_cast<std::uint64_t>(counters.bytes_written);
  return {.complete = true,
          .events_written = counters.written,
          .events_dropped = events_dropped,
          .writer_failures = counters.failures,
          .bytes_written = bytes_written};
}

struct RuntimeDiagnostics::State
{
  std::mutex mutex;
  std::shared_ptr<ava::core::AnchorSet> anchor_set;
  std::filesystem::path trace_path;
  std::shared_ptr<ProductionTraceCounters> counters;
  std::shared_ptr<PrivateTraceObserver> observer;
  std::shared_ptr<ava::observability::RunObservation> observation;
  bool trace_requested = false;
  bool closed = false;
  std::once_flag close_once;
};

RuntimeDiagnostics::RuntimeDiagnostics(ava::config::XdgPaths paths) : paths_(std::move(paths)), state_(std::make_unique<State>())
{
}

RuntimeDiagnostics::~RuntimeDiagnostics() noexcept
{
  close();
}

ava::core::Result<std::shared_ptr<RuntimeDiagnostics>> RuntimeDiagnostics::create(ava::config::XdgPaths paths, bool trace_enabled) noexcept
{
  try
  {
    auto diagnostics = std::make_shared<RuntimeDiagnostics>(std::move(paths));
    diagnostics->state_->trace_requested = trace_enabled;
    return diagnostics;
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "private runtime diagnostics initialization failed"));
  }
}

ava::core::Result<std::shared_ptr<RuntimeDiagnostics>> RuntimeDiagnostics::create(ava::config::XdgPaths paths,
                                                                                 std::shared_ptr<ava::core::AnchorSet> anchor_set,
                                                                                 bool trace_enabled) noexcept
{
  auto diagnostics = create(std::move(paths), trace_enabled);
  if (!diagnostics)
    return diagnostics;
  if (auto bound = (*diagnostics)->bind_anchor_set(std::move(anchor_set)); !bound)
    return std::unexpected(std::move(bound.error()));
  return diagnostics;
}

ava::core::VoidResult RuntimeDiagnostics::bind_anchor_set(std::shared_ptr<ava::core::AnchorSet> anchor_set) noexcept
{
  try
  {
    if (!anchor_set)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "runtime diagnostics requires shared storage anchors"));
    auto state_anchor = anchor_set->find_anchor(paths_.ava_state_dir);
    if (!state_anchor || !state_anchor->relative().empty() || state_anchor->anchor().root != paths_.ava_state_dir.lexically_normal())
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Configuration, "runtime diagnostics state anchor is unavailable"));

    std::lock_guard lock(state_->mutex);
    if (state_->closed)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "runtime diagnostics is closed"));
    if (state_->anchor_set)
      return {};
    state_->anchor_set = std::move(anchor_set);
    if (state_->trace_requested)
    {
      if (auto initialized = initialize_trace(); !initialized)
      {
        state_->anchor_set.reset();
        return initialized;
      }
    }
    return {};
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "private runtime diagnostics initialization failed"));
  }
}

ava::core::VoidResult RuntimeDiagnostics::initialize_trace() noexcept
{
  try
  {
    Dout(dc::runtime, "operation=trace_initialization state=start");
    auto prepared = prepare_trace_artifact(paths_, *state_->anchor_set);
    if (prepared.status != ArtifactWriteStatus::Success || !prepared.descriptor || prepared.descriptor->fd() < 0)
    {
      Dout(dc::runtime, "operation=trace_initialization state=failed status=" << to_string(prepared.status));
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "private runtime diagnostics storage is unavailable"));
    }
    state_->trace_path = std::move(prepared.path);
    state_->counters = std::make_shared<ProductionTraceCounters>();
    state_->observer = std::make_shared<PrivateTraceObserver>(state_->trace_path, prepared.descriptor->fd(), state_->counters);
    state_->observation = std::make_shared<ava::observability::RunObservation>(state_->observer);
    Dout(dc::runtime, "operation=trace_initialization state=ready");
    return {};
  }
  catch (...)
  {
    Dout(dc::runtime, "operation=trace_initialization state=failed status=io_failure");
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "private runtime diagnostics storage is unavailable"));
  }
}

bool RuntimeDiagnostics::trace_enabled() const noexcept
{
  try
  {
    if (!state_)
      return false;
    std::lock_guard lock(state_->mutex);
    return state_->observation && state_->observation->enabled();
  }
  catch (...)
  {
    return false;
  }
}

std::shared_ptr<ava::observability::RunObservation> RuntimeDiagnostics::observation() const noexcept
{
  try
  {
    if (!state_)
      return nullptr;
    std::lock_guard lock(state_->mutex);
    return state_->observation;
  }
  catch (...)
  {
    return nullptr;
  }
}

std::optional<std::filesystem::path> RuntimeDiagnostics::trace_path() const noexcept
{
  try
  {
    if (!state_)
      return std::nullopt;
    std::lock_guard lock(state_->mutex);
    if (!state_->observation || !state_->observation->enabled())
      return std::nullopt;
    return state_->trace_path;
  }
  catch (...)
  {
    return std::nullopt;
  }
}

void RuntimeDiagnostics::record_terminal_failure(RuntimeFailureClass failure_class, ava::core::Error const& error) noexcept
{
  if (error.category() == ava::core::ErrorCategory::InvalidArgument)
    return;
  try
  {
    std::shared_ptr<ava::core::AnchorSet> anchor_set;
    {
      std::lock_guard lock(state_->mutex);
      anchor_set = state_->anchor_set;
    }
    if (!anchor_set)
      return;
    auto const record =
        LastFailureRecord{.recorded_at = now_seconds(), .failure = safe_failure_from_error(component_for(failure_class), error), .occurrences = 1};
    auto const status = write_last_failure_record(paths_, *anchor_set, record);
    Dout(dc::runtime, "operation=last_failure_write status=" << to_string(status) << " category=" << ava::core::to_string(error.category()));
    static_cast<void>(status);
  }
  catch (...)
  {
    Dout(dc::runtime, "operation=last_failure_write status=io_failure category=" << ava::core::to_string(error.category()));
  }
}

void RuntimeDiagnostics::close() noexcept
{
  if (!state_)
    return;
  try
  {
    std::shared_ptr<ava::core::AnchorSet> anchor_set;
    std::shared_ptr<PrivateTraceObserver> observer;
    std::shared_ptr<ProductionTraceCounters> counters;
    {
      std::lock_guard lock(state_->mutex);
      state_->closed = true;
      anchor_set = state_->anchor_set;
      observer = state_->observer;
      counters = state_->counters;
    }
    // Drain before call_once, snapshot allocation or logging can throw.
    if (observer)
      observer->close();
    if (!anchor_set || !observer || !counters)
      return;
    std::call_once(state_->close_once, [&] {
      try
      {
        auto const writer = observer->writer_counters();
        auto writer_health = trace_writer_health_from_counters(writer);
        writer_health.delta_summaries_refused = counters->delta_summaries_refused.load(std::memory_order_relaxed);
        TraceCounterSnapshot const snapshot{.captured_at = now_seconds(),
                                            .runtime_starts = counters->runtime_starts.load(std::memory_order_relaxed),
                                            .provider_requests = counters->provider_requests.load(std::memory_order_relaxed),
                                            .provider_failures = counters->provider_failures.load(std::memory_order_relaxed),
                                            .session_failures = counters->session_failures.load(std::memory_order_relaxed),
                                            .plugin_failures = counters->plugin_failures.load(std::memory_order_relaxed),
                                            .mcp_failures = counters->mcp_failures.load(std::memory_order_relaxed),
                                            .writer_health = writer_health};
        auto const status = write_trace_counter_snapshot(paths_, *anchor_set, snapshot);
        Dout(dc::runtime, "operation=trace_close state=done status=" << to_string(status) << " written=" << writer_health.events_written << " dropped="
                                                                     << writer_health.events_dropped << " failures=" << writer_health.writer_failures
                                                                     << " queue_failure_observations=" << writer.queue_failures);
        static_cast<void>(writer);
        static_cast<void>(status);
      }
      catch (...)
      {
        // A throwing callable leaves close_once unset. Swallow failures here so
        // later closes cannot publish the contribution again if logging throws.
      }
    });
  }
  catch (...)
  {
    // Mutex acquisition and call_once itself can still throw during best-effort shutdown.
  }
}

}  // namespace ava::diagnostics
