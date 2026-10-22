#include "sys.h"
#include "ava/core/thread.h"
#include "ava/observability/run_observer.h"
#include "ava/core/json.h"
#include "ava/core/fd_io.h"
#include "ava/core/saturating.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::observability {
namespace {
struct RunObserverCallbackFrame
{
  RunObserverCallbackFrame* previous = nullptr;
};

thread_local RunObserverCallbackFrame* active_run_observer_callback = nullptr;

class RunObserverCallbackScope final
{
 public:
  RunObserverCallbackScope() noexcept : frame_{.previous = active_run_observer_callback} { active_run_observer_callback = &frame_; }
  ~RunObserverCallbackScope() { active_run_observer_callback = frame_.previous; }
  RunObserverCallbackScope(RunObserverCallbackScope const&) = delete;
  RunObserverCallbackScope& operator=(RunObserverCallbackScope const&) = delete;

 private:
  RunObserverCallbackFrame frame_;
};

constexpr std::size_t kMaxIdentifierBytes = 256, kMaxMetadataKeyBytes = 128, kMaxMetadataValueBytes = 1024;
constexpr std::string_view kFieldsTruncatedKey = "fields_truncated";
std::string bounded(std::string_view value, std::size_t limit)
{
  return value.size() <= limit ? std::string(value) : (limit ? std::string(value.substr(0, limit)) + "…" : std::string{});
}
std::string quoted(std::string_view value, std::size_t limit)
{
  return "\"" + ava::core::json::escape(bounded(value, limit)) + "\"";
}
std::string canonical_key(std::string_view key)
{
  std::string out;
  for (unsigned char ch : key)
    if (std::isalnum(ch))
      out += static_cast<char>(std::tolower(ch));
  return out;
}
bool sensitive_key(std::string_view key)
{
  auto k = canonical_key(key);
  return k == "authorization" || k == "proxyauthorization" || k == "apikey" || k == "xapikey" || k == "token" || k == "accesstoken" || k == "refreshtoken" ||
         k == "idtoken" || k == "authtoken" || k == "secret" || k == "clientsecret" || k == "password" || k == "openaiapikey" || k == "anthropicapikey" ||
         k == "geminiapikey" || k == "googleapikey" || k == "deepseekapikey" || k == "openrouterapikey" || k == "moonshotapikey" || k == "kimiapikey" ||
         k == "azureopenaiapikey" || k == "awsaccesskeyid" || k == "awssecretaccesskey" || k == "awssessiontoken";
}
std::string redacted(TraceField const& field)
{
  switch (field.provenance)
  {
    case FieldProvenance::Secret:
    case FieldProvenance::AuthorizationHeader:
    case FieldProvenance::Environment:
      return "[redacted]";
    case FieldProvenance::Content:
      return "[omitted]";
    case FieldProvenance::Path:
      return "[path-omitted]";
    case FieldProvenance::PublicMetadata:
      return sensitive_key(field.key) ? "[redacted]" : bounded(field.value, kMaxMetadataValueBytes);
  }
  return "[omitted]";
}
void append_field(std::string& out, std::string_view key, std::string_view value, bool& first)
{
  if (!first)
    out += ',';
  first = false;
  out += quoted(key, kMaxMetadataKeyBytes);
  out += ':';
  out += quoted(value, kMaxMetadataValueBytes);
}
bool append_if_fits(std::string& out, std::string_view key, std::string_view value, bool& first, std::size_t max, bool reserve_truncation_marker = false)
{
  auto candidate = out;
  auto candidate_first = first;
  append_field(candidate, key, value, candidate_first);
  if (reserve_truncation_marker)
  {
    auto marked = candidate;
    auto marked_first = candidate_first;
    append_field(marked, kFieldsTruncatedKey, "true", marked_first);
    if (marked.size() + 2 > max)
      return false;
  }
  else if (candidate.size() + 2 > max)
  {
    return false;
  }
  out = std::move(candidate);
  first = candidate_first;
  return true;
}
std::string fallback(std::size_t max)
{
  static constexpr std::string_view full = "{\"schema_version\":1,\"type\":\"trace_event_oversize\",\"fields\":{\"fields_truncated\":\"true\"}}",
                                    marker = "{\"fields_truncated\":\"true\"}", short_form = "{\"schema_version\":1}";
  if (full.size() <= max)
    return std::string(full);
  if (marker.size() <= max)
    return std::string(marker);
  if (short_form.size() <= max)
    return std::string(short_form);
  return "{}";
}

void verify_directory(int fd)
{
  struct stat st{};
  if (fstat(fd, &st) != 0)
    throw std::system_error(errno, std::generic_category(), "inspect trace directory");
  if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & (S_IRWXG | S_IRWXO)))
    throw std::runtime_error("trace directory must be an owner-only directory");
}
int stable_root(std::filesystem::path const& path)
{
  return open(path.is_absolute() ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}
int open_trace_directory(std::filesystem::path const& path)
{
  int fd = stable_root(path);
  if (fd < 0)
    throw std::system_error(errno, std::generic_category(), "open stable trace root");
  std::filesystem::path relative = path.is_absolute() ? path.relative_path() : path;
  try
  {
    for (auto const& part : relative)
    {
      auto const name = part.string();
      if (name.empty() || name == ".")
        continue;
      if (name == "..")
        throw std::runtime_error("trace path may not contain parent traversal");
      bool created = false;
      int next = openat(fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (next < 0 && errno == ENOENT)
      {
        if (mkdirat(fd, name.c_str(), S_IRWXU) == 0)
        {
          created = true;
        }
        else if (errno == EEXIST)
        {
          // A concurrent creator won the race. Its directory is not ours to
          // chmod: descriptor validation below decides whether it is safe.
          created = false;
        }
        else
        {
          throw std::system_error(errno, std::generic_category(), "create trace directory");
        }
        next = openat(fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      }
      if (next < 0)
        throw std::system_error(errno, std::generic_category(), "open trace directory component");
      if (created)
      {
        if (fchmod(next, S_IRWXU) != 0)
        {
          auto error = errno;
          close(next);
          throw std::system_error(error, std::generic_category(), "secure new trace directory");
        }
      }
      close(fd);
      fd = next;
    }
    verify_directory(fd);
    return fd;
  }
  catch (...)
  {
    ::close(fd);
    throw;
  }
}
void verify_trace_file(int fd)
{
  struct stat st{};
  if (fstat(fd, &st) != 0)
    throw std::system_error(errno, std::generic_category(), "inspect trace artifact");
  if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & (S_IRWXG | S_IRWXO)) || st.st_nlink != 1)
    throw std::runtime_error("trace artifact must be a private unlinked regular file");
}

template <class Enum, class ToString>
std::optional<Enum> parse_enum(std::string_view value, std::initializer_list<Enum> values, ToString to_text)
{
  for (auto candidate : values)
    if (to_text(candidate) == value)
      return candidate;
  return std::nullopt;
}
}  // namespace

std::string_view to_string(TraceEventType v) noexcept
{
  switch (v)
  {
    case TraceEventType::AgentRunStart:
      return "agent.run_start";
    case TraceEventType::AgentRunTerminal:
      return "agent.run_terminal";
    case TraceEventType::TransportRequestResult:
      return "transport.request_result";
    case TraceEventType::TransportAttemptResult:
      return "transport.attempt_result";
    case TraceEventType::TransportRetry:
      return "transport.retry";
    case TraceEventType::SessionAppendAttempt:
      return "session.append_attempt";
    case TraceEventType::SessionAppendResult:
      return "session.append_result";
    case TraceEventType::SessionLoadAttempt:
      return "session.load_attempt";
    case TraceEventType::SessionLoadResult:
      return "session.load_result";
    case TraceEventType::ProviderStreamEvent:
      return "provider.stream_event";
    case TraceEventType::ToolDispatchStart:
      return "tool.dispatch_start";
    case TraceEventType::ToolDispatchResult:
      return "tool.dispatch_result";
    case TraceEventType::ProcessStart:
      return "process.start";
    case TraceEventType::ProcessResult:
      return "process.result";
  }
  return "unknown";
}
std::string_view to_string(TracePhase v) noexcept
{
  switch (v)
  {
    case TracePhase::None:
      return "";
    case TracePhase::Run:
      return "run";
    case TracePhase::Transport:
      return "transport";
    case TracePhase::Session:
      return "session";
    case TracePhase::Provider:
      return "provider";
    case TracePhase::Tool:
      return "tool";
    case TracePhase::Process:
      return "process";
  }
  return "";
}
std::string_view to_string(TraceOutcome v) noexcept
{
  switch (v)
  {
    case TraceOutcome::None:
      return "";
    case TraceOutcome::Started:
      return "started";
    case TraceOutcome::Completed:
      return "completed";
    case TraceOutcome::Success:
      return "success";
    case TraceOutcome::Error:
      return "error";
    case TraceOutcome::Canceled:
      return "canceled";
    case TraceOutcome::ProviderError:
      return "provider_error";
    case TraceOutcome::ToolError:
      return "tool_error";
    case TraceOutcome::SessionError:
      return "session_error";
    case TraceOutcome::Failed:
      return "failed";
    case TraceOutcome::Retrying:
      return "retrying";
    case TraceOutcome::TextDelta:
      return "text_delta";
    case TraceOutcome::ToolCallStart:
      return "tool_call_start";
    case TraceOutcome::ToolCallDelta:
      return "tool_call_delta";
    case TraceOutcome::ToolCallEnd:
      return "tool_call_end";
    case TraceOutcome::ReasoningStart:
      return "reasoning_start";
    case TraceOutcome::ReasoningDelta:
      return "reasoning_delta";
    case TraceOutcome::ReasoningEnd:
      return "reasoning_end";
    case TraceOutcome::Done:
      return "done";
  }
  return "";
}
std::int64_t SystemClock::now_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string CounterIdGenerator::next(std::string_view prefix)
{
  return std::string(prefix) + "-" + std::to_string(next_.fetch_add(1, std::memory_order_relaxed) + 1);
}
RunObservation::RunObservation(std::shared_ptr<RunObserver> observer, std::shared_ptr<Clock> clock, std::shared_ptr<IdGenerator> ids)
{
  if (!observer)
    return;
  state_ = std::make_shared<State>();
  state_->observer = std::move(observer);
  state_->clock = clock ? std::move(clock) : std::make_shared<SystemClock>();
  state_->ids = ids ? std::move(ids) : std::make_shared<CounterIdGenerator>();
}
bool RunObservation::enabled() const noexcept
{
  return state_ && state_->observer;
}
bool in_run_observer_callback() noexcept
{
  return active_run_observer_callback != nullptr;
}
void RunObservation::invoke_observer(TraceEvent const& event) const
{
  RunObserverCallbackScope callback_scope;
  state_->observer->on_event(event);
}
void RunObservation::account_failure() const noexcept
{
  if (state_)
    state_->callback_failures.fetch_add(1, std::memory_order_relaxed);
}
void RunObservation::account_external_failure() const noexcept
{
  account_failure();
}
std::string RunObservation::next_id(std::string_view prefix) const noexcept
{
  if (!enabled())
    return {};
  try
  {
    return state_->ids->next(prefix);
  }
  catch (...)
  {
    account_failure();
    return {};
  }
}
TraceEvent RunObservation::make_event(TraceEventType type, TraceContext const& c) const
{
  TraceEvent e;
  e.type = type;
  e.run_id = bounded(c.run_id, kMaxIdentifierBytes);
  e.turn_id = bounded(c.turn_id, kMaxIdentifierBytes);
  e.session_id = bounded(c.session_id, kMaxIdentifierBytes);
  e.provider_id = bounded(c.provider_id, kMaxIdentifierBytes);
  e.parent_run_id = bounded(c.parent_run_id, kMaxIdentifierBytes);
  e.parent_turn_id = bounded(c.parent_turn_id, kMaxIdentifierBytes);
  e.parent_session_id = bounded(c.parent_session_id, kMaxIdentifierBytes);
  e.timestamp_ms = state_->clock->now_ms();
  e.sequence = state_->sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  return e;
}
ObserverCounters RunObservation::counters() const noexcept
{
  return state_ ? ObserverCounters{state_->emitted.load(std::memory_order_relaxed), state_->callback_failures.load(std::memory_order_relaxed)}
                : ObserverCounters{};
}
std::optional<TraceEvent> parse_canonical_json(std::string_view json)
{
  if (!ava::core::json::is_valid_object(json))
    return std::nullopt;
  auto const schema_version = ava::core::json::integer_field(json, "schema_version");
  auto const sequence = ava::core::json::integer_field(json, "sequence");
  auto const timestamp_ms = ava::core::json::integer_field(json, "timestamp_ms");
  auto const type = ava::core::json::string_field(json, "type");
  auto const run_id = ava::core::json::string_field(json, "run_id");
  auto const turn_id = ava::core::json::string_field(json, "turn_id");
  auto const call_id = ava::core::json::string_field(json, "call_id");
  auto const session_id = ava::core::json::string_field(json, "session_id");
  auto const provider_id = ava::core::json::string_field(json, "provider_id");
  auto const parent_run_id = ava::core::json::string_field(json, "parent_run_id");
  auto const parent_turn_id = ava::core::json::string_field(json, "parent_turn_id");
  auto const parent_session_id = ava::core::json::string_field(json, "parent_session_id");
  auto const phase = ava::core::json::string_field(json, "phase");
  auto const outcome = ava::core::json::string_field(json, "outcome");
  auto const fields = ava::core::json::object_field(json, "fields");
  if (!schema_version || *schema_version != kTraceSchemaVersion || !sequence || *sequence < 0 || !timestamp_ms || !type || !run_id || !turn_id || !call_id ||
      !session_id || !provider_id || !parent_run_id || !parent_turn_id || !parent_session_id || !phase || !outcome || !fields ||
      !ava::core::json::is_valid_object(*fields))
    return std::nullopt;
  auto event_type = parse_enum<TraceEventType>(
      *type,
      {TraceEventType::AgentRunStart, TraceEventType::AgentRunTerminal, TraceEventType::TransportRequestResult, TraceEventType::TransportAttemptResult,
       TraceEventType::TransportRetry, TraceEventType::SessionAppendAttempt, TraceEventType::SessionAppendResult, TraceEventType::SessionLoadAttempt,
       TraceEventType::SessionLoadResult, TraceEventType::ProviderStreamEvent, TraceEventType::ToolDispatchStart, TraceEventType::ToolDispatchResult,
       TraceEventType::ProcessStart, TraceEventType::ProcessResult},
      [](TraceEventType value) { return to_string(value); });
  auto event_phase = parse_enum<TracePhase>(
      *phase, {TracePhase::None, TracePhase::Run, TracePhase::Transport, TracePhase::Session, TracePhase::Provider, TracePhase::Tool, TracePhase::Process},
      [](TracePhase value) { return to_string(value); });
  auto event_outcome = parse_enum<TraceOutcome>(
      *outcome,
      {TraceOutcome::None, TraceOutcome::Started, TraceOutcome::Completed, TraceOutcome::Success, TraceOutcome::Error, TraceOutcome::Canceled,
       TraceOutcome::ProviderError, TraceOutcome::ToolError, TraceOutcome::SessionError, TraceOutcome::Failed, TraceOutcome::Retrying, TraceOutcome::TextDelta,
       TraceOutcome::ToolCallStart, TraceOutcome::ToolCallDelta, TraceOutcome::ToolCallEnd, TraceOutcome::ReasoningStart, TraceOutcome::ReasoningDelta,
       TraceOutcome::ReasoningEnd, TraceOutcome::Done},
      [](TraceOutcome value) { return to_string(value); });
  if (!event_type || !event_phase || !event_outcome)
    return std::nullopt;
  return TraceEvent{.schema_version = static_cast<int>(*schema_version),
                    .sequence = static_cast<std::uint64_t>(*sequence),
                    .timestamp_ms = *timestamp_ms,
                    .type = *event_type,
                    .run_id = *run_id,
                    .turn_id = *turn_id,
                    .call_id = *call_id,
                    .session_id = *session_id,
                    .provider_id = *provider_id,
                    .parent_run_id = *parent_run_id,
                    .parent_turn_id = *parent_turn_id,
                    .parent_session_id = *parent_session_id,
                    .phase = *event_phase,
                    .outcome = *event_outcome,
                    .fields = {}};
}

std::string to_string(FieldProvenance p)
{
  switch (p)
  {
    case FieldProvenance::PublicMetadata:
      return "metadata";
    case FieldProvenance::Path:
      return "path";
    case FieldProvenance::Content:
      return "content";
    case FieldProvenance::Secret:
      return "secret";
    case FieldProvenance::AuthorizationHeader:
      return "authorization_header";
    case FieldProvenance::Environment:
      return "environment";
  }
  return "unknown";
}
std::string canonical_json(TraceEvent const& e, std::size_t max)
{
  std::string out = "{\"schema_version\":" + std::to_string(kTraceSchemaVersion) + ",\"sequence\":" + std::to_string(e.sequence) +
                    ",\"timestamp_ms\":" + std::to_string(e.timestamp_ms) + ",\"type\":" + quoted(to_string(e.type), 128) +
                    ",\"run_id\":" + quoted(e.run_id, kMaxIdentifierBytes) + ",\"turn_id\":" + quoted(e.turn_id, kMaxIdentifierBytes) +
                    ",\"call_id\":" + quoted(e.call_id, kMaxIdentifierBytes) + ",\"session_id\":" + quoted(e.session_id, kMaxIdentifierBytes) +
                    ",\"provider_id\":" + quoted(e.provider_id, kMaxIdentifierBytes) + ",\"parent_run_id\":" + quoted(e.parent_run_id, kMaxIdentifierBytes) +
                    ",\"parent_turn_id\":" + quoted(e.parent_turn_id, kMaxIdentifierBytes) +
                    ",\"parent_session_id\":" + quoted(e.parent_session_id, kMaxIdentifierBytes) + ",\"phase\":" + quoted(to_string(e.phase), 128) +
                    ",\"outcome\":" + quoted(to_string(e.outcome), 128) + ",\"fields\":{";
  if (out.size() + 2 > max)
    return fallback(max);
  auto fields = e.fields;
  std::ranges::sort(fields, [](auto const& a, auto const& b) {
    return std::tuple(bounded(a.key, kMaxMetadataKeyBytes), a.provenance, a.value) < std::tuple(bounded(b.key, kMaxMetadataKeyBytes), b.provenance, b.value);
  });
  std::vector<std::pair<std::string, std::string>> serializable_fields;
  for (std::size_t i = 0; i < fields.size();)
  {
    auto key = bounded(fields[i].key, kMaxMetadataKeyBytes);
    auto end = std::ranges::find_if(fields.begin() + static_cast<std::ptrdiff_t>(i + 1), fields.end(),
                                    [&](auto const& f) { return bounded(f.key, kMaxMetadataKeyBytes) != key; });
    if (key != kFieldsTruncatedKey)
    {
      auto duplicate = end != fields.begin() + static_cast<std::ptrdiff_t>(i + 1);
      serializable_fields.emplace_back(std::move(key), duplicate ? "[duplicate-omitted]" : redacted(fields[i]));
    }
    // This key is reserved for the serializer's own truncation marker.
    // Omitting all caller values prevents spoofing and duplicate JSON keys.
    i = static_cast<std::size_t>(end - fields.begin());
  }

  bool first = true, truncated = false;
  for (std::size_t i = 0; i < serializable_fields.size(); ++i)
  {
    auto const& [key, value] = serializable_fields[i];
    // Every non-final field leaves room for the marker needed if a later field does not fit.
    if (!append_if_fits(out, key, value, first, max, i + 1 < serializable_fields.size()))
    {
      truncated = true;
      break;
    }
  }
  if (truncated && !append_if_fits(out, kFieldsTruncatedKey, "true", first, max))
    return fallback(max);
  out += "}}";
  return out.size() <= max ? out : fallback(max);
}
JsonlRunObserver::JsonlRunObserver(JsonlObserverOptions options) : options_(std::move(options))
{
  if (options_.path.empty() || options_.path.parent_path().empty())
    throw std::invalid_argument("trace path must include parent directory");
  options_.max_events = std::max<std::size_t>(1, options_.max_events);
  options_.max_event_bytes = std::min(options_.max_event_bytes, options_.max_bytes);
  if (options_.initial_fd >= 0)
  {
    int const fd = ::fcntl(options_.initial_fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0)
      throw std::system_error(errno, std::generic_category(), "duplicate trace artifact descriptor");
    try
    {
      verify_trace_file(fd);
      int const flags = ::fcntl(fd, F_GETFL);
      if (flags < 0 || (flags & O_ACCMODE) == O_RDONLY || (flags & O_APPEND) == 0)
        throw std::invalid_argument("trace artifact descriptor must be append-writable");
      if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
        throw std::system_error(errno, std::generic_category(), "lock trace artifact");
      fd_ = fd;
    }
    catch (...)
    {
      ::close(fd);
      throw;
    }
  }
}
JsonlRunObserver::~JsonlRunObserver() noexcept
{
  close();
}
void JsonlRunObserver::ensure_open()
{
  if (fd_ >= 0)
    return;
  int dir = open_trace_directory(options_.path.parent_path());
  try
  {
    auto name = options_.path.filename().string();
    int fd = openat(dir, name.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, S_IRUSR | S_IWUSR);
    if (fd < 0 && errno == EEXIST)
      fd = openat(dir, name.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
      throw std::system_error(errno, std::generic_category(), "open trace artifact");
    try
    {
      verify_trace_file(fd);
      if (flock(fd, LOCK_EX | LOCK_NB) != 0)
        throw std::system_error(errno, std::generic_category(), "lock trace artifact");
    }
    catch (...)
    {
      ::close(fd);
      throw;
    }
    ::close(dir);
    fd_ = fd;
  }
  catch (...)
  {
    ::close(dir);
    throw;
  }
}
void JsonlRunObserver::write_record(std::string record)
{
  std::lock_guard lock(mutex_);
  if (closed_ || written_ >= options_.max_events || record.size() > options_.max_event_bytes || record.size() + 1 > options_.max_bytes ||
      bytes_written_ > options_.max_bytes - record.size() - 1)
  {
    ++dropped_;
    return;
  }
  off_t record_start = -1;
  try
  {
    ensure_open();
    record += '\n';
    record_start = ::lseek(fd_, 0, SEEK_END);
    if (record_start < 0)
      throw std::system_error(errno, std::generic_category(), "locate trace record rollback boundary");
    if (auto const error = ava::core::write_all(fd_, record); error != 0)
    {
      // The lifetime lock excludes other writers, so the append offset records
      // exactly the bytes accepted before failure, including rolled-back bytes.
      auto const record_end = ::lseek(fd_, 0, SEEK_CUR);
      if (record_end > record_start)
        bytes_written_ += static_cast<std::size_t>(record_end - record_start);
      throw std::system_error(error, std::generic_category(), "write trace record");
    }
    bytes_written_ += record.size();
    ++written_;
  }
  catch (...)
  {
    ++failures_;
    if (record_start >= 0)
    {
      // The lifetime file lock excludes other trace writers during rollback.
      static_cast<void>(ava::core::restart_on_eintr([&] { return ::ftruncate(fd_, record_start); }));
    }
    close_locked();
    throw;
  }
}
void JsonlRunObserver::on_event(TraceEvent const& event)
{
  write_record(canonical_json(event, options_.max_event_bytes));
}
void JsonlRunObserver::close() noexcept
{
  std::lock_guard lock(mutex_);
  close_locked();
}
void JsonlRunObserver::close_locked() noexcept
{
  if (closed_)
    return;
  closed_ = true;
  if (fd_ >= 0)
  {
    ::close(fd_);
    fd_ = -1;
  }
}
JsonlObserverCounters JsonlRunObserver::counters() const noexcept
{
  std::lock_guard lock(mutex_);
  return {written_, dropped_, failures_, bytes_written_};
}
bool queue_has_capacity(std::size_t queued_events, std::size_t queued_bytes, std::size_t record_bytes, std::size_t max_queue_events,
                        std::size_t max_queue_bytes) noexcept
{
  return queued_events < max_queue_events && record_bytes <= max_queue_bytes && queued_bytes <= max_queue_bytes - record_bytes;
}

QueuedJsonlRunObserver::QueuedJsonlRunObserver(QueuedJsonlObserverOptions options)
    : options_(std::move(options)),
      writer_(JsonlObserverOptions{.path = options_.path,
                                   .max_events = options_.max_events,
                                   .max_bytes = options_.max_bytes,
                                   .max_event_bytes = options_.max_event_bytes,
                                   .initial_fd = options_.initial_fd})
{
  options_.max_queue_events = std::max<std::size_t>(1, options_.max_queue_events);
  options_.max_queue_bytes = std::max(options_.max_event_bytes, options_.max_queue_bytes);
  worker_ = ava::core::JoinThread::create("run_observer", [this](std::stop_token stop) { run(stop); });
}
QueuedJsonlRunObserver::~QueuedJsonlRunObserver() noexcept
{
  close();
}
void QueuedJsonlRunObserver::on_event(TraceEvent const& event)
{
  auto record = canonical_json(event, options_.max_event_bytes);
  // This mutex covers only the bounded deque bookkeeping; the writer releases
  // it before opening or writing the file. Contention therefore cannot drop a
  // lifecycle record.
  std::unique_lock lock(mutex_);
  if (closing_ || !queue_has_capacity(queue_.size(), queue_bytes_, record.size(), options_.max_queue_events, options_.max_queue_bytes))
  {
    queue_dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  queue_bytes_ += record.size();
  queue_.push_back(std::move(record));
  high_water_bytes_ = std::max(high_water_bytes_, queue_bytes_);
  high_water_events_ = std::max(high_water_events_, queue_.size());
  lock.unlock();
  wake_.notify_one();
}
void QueuedJsonlRunObserver::run(std::stop_token stop) noexcept
{
  while (true)
  {
    std::string record;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, stop, [this] { return !queue_.empty() || closing_; });
      if (queue_.empty() && (stop.stop_requested() || closing_))
        return;
      record = std::move(queue_.front());
      queue_bytes_ -= record.size();
      queue_.pop_front();
    }
    try
    {
      writer_.write_record(std::move(record));
    }
    catch (...)
    {
      std::lock_guard lock(mutex_);
      queue_failures_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}
void QueuedJsonlRunObserver::close() noexcept
{
  std::call_once(close_once_, [this] {
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable())
      worker_.join();
    writer_.close();
  });
}
QueuedJsonlObserverCounters QueuedJsonlRunObserver::counters() const noexcept
{
  auto base = writer_.counters();
  std::lock_guard lock(mutex_);
  return {{base.written,
           base.dropped,
           base.failures,
           base.bytes_written},
          queue_dropped_.load(std::memory_order_relaxed),
          queue_failures_.load(std::memory_order_relaxed),
          queue_bytes_,
          high_water_bytes_,
          high_water_events_};
}

TraceValidationResult validate_and_score_trace(std::span<TraceEvent const> events, TraceAccounting accounting, TraceFixturePolicy const* policy)
{
  TraceValidationResult result;
  result.accounted_failures = accounting.callback_failures;
  result.accounted_drops = accounting.dropped_events;
  auto const losses = ava::core::saturating_add(accounting.callback_failures, accounting.dropped_events);
  result.incomplete_coverage = losses != 0;
  auto missing_boundary = [&](std::string message) {
    result.incomplete_coverage = true;
    auto& issues = losses != 0 ? result.coverage_gaps : result.errors;
    issues.push_back(std::move(message));
  };
  result.score = static_cast<unsigned>(losses >= 100 ? 0 : 100 - losses);
  struct RunState
  {
    bool started = false;
    bool terminal = false;
    bool event_before_start = false;
  };
  std::map<std::string, RunState> runs;
  std::uint64_t previous_sequence = 0;
  for (auto const& event : events)
  {
    if (event.sequence == 0 || event.sequence <= previous_sequence)
      result.errors.push_back("sequence must be unique and strictly monotonic");
    previous_sequence = event.sequence;
    if (event.run_id.empty())
    {
      result.errors.push_back("event is missing run_id");
      continue;
    }
    auto& run = runs[event.run_id];
    if (run.terminal)
      result.errors.push_back("event occurred after run terminal");
    if (event.type == TraceEventType::AgentRunStart)
    {
      if (run.started)
        result.errors.push_back("run has duplicate start");
      run.started = true;
      continue;
    }
    if (!run.started)
      run.event_before_start = true;
    if (event.type == TraceEventType::AgentRunTerminal)
    {
      if (event.outcome != TraceOutcome::Completed && event.outcome != TraceOutcome::Canceled && event.outcome != TraceOutcome::ProviderError &&
          event.outcome != TraceOutcome::ToolError && event.outcome != TraceOutcome::SessionError && event.outcome != TraceOutcome::Failed)
        result.errors.push_back("run terminal has invalid outcome");
      run.terminal = true;
    }
  }
  for (auto const& [run_id, state] : runs)
  {
    if (state.event_before_start)
    {
      if (state.started)
        result.errors.push_back("event occurred before run start");
      else
        missing_boundary("run is missing start");
    }
    if (state.started && !state.terminal)
      missing_boundary("run is missing terminal outcome");
  }

  if (policy)
  {
    unsigned satisfied = 0;
    auto has_boundary = [&](TraceRequiredBoundary boundary) {
      switch (boundary)
      {
        case TraceRequiredBoundary::RunLifecycle:
          return !runs.empty() && std::ranges::all_of(runs, [](auto const& run) { return run.second.started && run.second.terminal; });
        case TraceRequiredBoundary::TextDelta:
          return std::ranges::any_of(events, [](auto const& event) { return event.outcome == TraceOutcome::TextDelta; });
        case TraceRequiredBoundary::ToolDispatch:
          return std::ranges::any_of(
              events, [](auto const& event) { return event.type == TraceEventType::ToolDispatchStart || event.type == TraceEventType::ToolDispatchResult; });
        case TraceRequiredBoundary::ProviderErrorTerminal:
          return std::ranges::any_of(
              events, [](auto const& event) { return event.type == TraceEventType::AgentRunTerminal && event.outcome == TraceOutcome::ProviderError; });
        case TraceRequiredBoundary::CanceledTerminal:
          return std::ranges::any_of(
              events, [](auto const& event) { return event.type == TraceEventType::AgentRunTerminal && event.outcome == TraceOutcome::Canceled; });
      }
      return false;
    };
    for (auto const boundary : policy->required_boundaries)
    {
      if (has_boundary(boundary))
        satisfied += boundary == TraceRequiredBoundary::RunLifecycle ? policy->lifecycle_weight : policy->required_boundary_weight;
      else
        missing_boundary("fixture " + policy->fixture_id + " is missing a required trace boundary");
    }
    if (policy->denominator == 0)
    {
      result.errors.push_back("fixture " + policy->fixture_id + " has a zero score denominator");
      result.score = 0;
    }
    else
    {
      auto const weighted = std::min(policy->denominator, satisfied);
      result.score = static_cast<unsigned>((100ULL * weighted) / policy->denominator);
      result.score = losses >= result.score ? 0 : result.score - static_cast<unsigned>(losses);
    }
  }
  result.valid = result.errors.empty();
  return result;
}
}  // namespace ava::observability
