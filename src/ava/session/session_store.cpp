#include "sys.h"
#include "ava/observability/run_observer.h"
#include "ava/session/record.h"
#include "ava/session/session_store.h"
#include "ava/session/session_store_internal.h"
#include "ava/core/json.h"
#include "ava/core/strict_json.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>
#include "debug.h"

namespace ava::session {

namespace detail {

ScopedFd::ScopedFd(int fd) noexcept : fd_(fd)
{
}

ScopedFd::ScopedFd(ScopedFd&& other) noexcept : fd_(std::exchange(other.fd_, -1))
{
}

ScopedFd& ScopedFd::operator=(ScopedFd&& other) noexcept
{
  if (this != &other)
  {
    reset();
    fd_ = std::exchange(other.fd_, -1);
  }
  return *this;
}

ScopedFd::~ScopedFd()
{
  reset();
}

int ScopedFd::get() const noexcept
{
  return fd_;
}

int ScopedFd::release() noexcept
{
  return std::exchange(fd_, -1);
}

int ScopedFd::close_checked() noexcept
{
  if (fd_ < 0)
    return 0;
  int const fd = std::exchange(fd_, -1);
  return ::close(fd) == 0 ? 0 : errno;
}

void ScopedFd::reset(int fd) noexcept
{
  if (fd_ >= 0)
    static_cast<void>(::close(fd_));
  fd_ = fd;
}

std::string project_key(std::filesystem::path const& workspace_dir)
{
  auto const normalized = std::filesystem::absolute(workspace_dir).lexically_normal().string();
  std::uint64_t hash = 14695981039346656037ULL;
  for (unsigned char const ch : normalized)
  {
    hash ^= ch;
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << std::hex << hash;
  return out.str();
}

std::mutex& append_mutex_for_path(std::filesystem::path const& path)
{
  static std::mutex registry_mutex;
  static std::map<std::filesystem::path, std::shared_ptr<std::mutex>> append_mutexes;

  auto const key = std::filesystem::absolute(path).lexically_normal();
  std::lock_guard lock(registry_mutex);
  auto& mutex = append_mutexes[key];
  if (!mutex)
  {
    mutex = std::make_shared<std::mutex>();
  }
  return *mutex;
}

// Monotonic per-path write generation shared by all SessionAppendTarget
// instances (and any other writer) on the same session file. A target records
// the generation it has folded into its cached assistant_output_state_ and, on
// its next append, reloads from storage only when the generation advanced --
// i.e. only when some other writer committed to the file. The single-target
// runtime append path therefore stays O(1) while independent targets on one
// path still observe each other's commits.
std::atomic<std::uint64_t>& append_epoch_for_path(std::filesystem::path const& path)
{
  static std::mutex registry_mutex;
  static std::map<std::filesystem::path, std::shared_ptr<std::atomic<std::uint64_t>>> append_epochs;

  auto const key = std::filesystem::absolute(path).lexically_normal();
  std::lock_guard lock(registry_mutex);
  auto& epoch = append_epochs[key];
  if (!epoch)
  {
    epoch = std::make_shared<std::atomic<std::uint64_t>>(0);
  }
  return *epoch;
}

ava::core::Error path_io_error(std::string message, std::filesystem::path const& path, int error_number)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Io, std::move(message));
  error.with_context("path", path.string());
  if (error_number != 0)
    error.with_context("cause", std::strerror(error_number));
  return error;
}

ava::core::Error append_authority_error(std::string message, std::filesystem::path const& path)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, std::move(message));
  error.with_context("path", path.string());
  return error;
}

ava::core::VoidResult validate_read_limits(SessionReadLimits const& limits)
{
  if (limits.max_file_bytes == 0 || limits.max_line_bytes == 0 || limits.max_entries == 0 || limits.max_line_bytes > limits.max_file_bytes ||
      limits.max_line_bytes > kMaxSessionLineBytes)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session bounded read limits are invalid"));
  return {};
}

ava::core::Error strict_session_record_error(ava::core::StrictJsonStatus status, std::filesystem::path const& path, bool final_unterminated)
{
  std::string message;
  switch (status)
  {
    case ava::core::StrictJsonStatus::Invalid:
      message = final_unterminated ? "unterminated session suffix is invalid JSON" : "newline-terminated session record is invalid JSON";
      break;
    case ava::core::StrictJsonStatus::NestingTooDeep:
      message = "session record JSON nesting exceeds limit";
      break;
    case ava::core::StrictJsonStatus::DuplicateObjectKey:
      message = "session record JSON contains duplicate object member names";
      break;
    case ava::core::StrictJsonStatus::Valid:
      message = "session record JSON validation failed";
      break;
  }
  auto error = ava::core::Error(ava::core::ErrorCategory::Session, std::move(message));
  error.with_context("path", path.string());
  error.with_context("record_framing", final_unterminated ? "final_unterminated" : "newline_terminated");
  error.with_context("max_nesting_depth", std::to_string(ava::core::json::kMaxNestingDepth));
  return error;
}

ava::core::Result<SessionEntry> parse_strict_session_record(std::string_view line, std::filesystem::path const& path, bool final_unterminated)
{
  auto const strict_status = ava::core::validate_strict_json(line, ava::core::json::kMaxNestingDepth);
  if (strict_status != ava::core::StrictJsonStatus::Valid)
    return std::unexpected(strict_session_record_error(strict_status, path, final_unterminated));
  if (line.ends_with('\r'))
    line.remove_suffix(1);
  auto entry = parse_session_entry_line(line, path);
  if (!entry)
  {
    auto error = std::move(entry.error());
    error.with_context("record_framing", final_unterminated ? "final_unterminated" : "newline_terminated");
    return std::unexpected(std::move(error));
  }
  return entry;
}

bool same_file_identity(struct stat const& left, struct stat const& right)
{
  return S_ISREG(left.st_mode) && S_ISREG(right.st_mode) && left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool same_directory_identity(struct stat const& left, struct stat const& right)
{
  return S_ISDIR(left.st_mode) && S_ISDIR(right.st_mode) && left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

std::filesystem::path anchored_child_diagnostic_path(int parent_fd, std::string_view name, std::filesystem::path const& fallback_parent)
{
  std::error_code link_error;
  auto parent = std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(parent_fd), link_error);
  if (link_error || parent.empty())
    parent = fallback_parent;
  return parent / std::string(name);
}

SessionTraceScope::SessionTraceScope(std::shared_ptr<ava::observability::RunObservation> observation,
                                     std::shared_ptr<ava::observability::TraceContext const> context, ava::observability::TraceEventType attempt,
                                     ava::observability::TraceEventType result, std::optional<EntryType> entry_type, bool ephemeral) noexcept
    : observation_(std::move(observation)), context_(std::move(context)), result_(result), entry_type_(entry_type), ephemeral_(ephemeral)
{
  observation_->emit(attempt, *context_, [this](auto& event) {
    event.phase = ava::observability::TracePhase::Session;
    if (entry_type_)
      event.fields = {{.key = "entry_type", .value = to_string(*entry_type_)}, {.key = "ephemeral", .value = ephemeral_ ? "true" : "false"}};
    else
      event.fields = {{.key = "ephemeral", .value = ephemeral_ ? "true" : "false"}};
  });
}

SessionTraceScope::~SessionTraceScope() noexcept
{
  observation_->emit(result_, *context_, [this](auto& event) {
    event.phase = ava::observability::TracePhase::Session;
    event.outcome = outcome_;
    if (result_ == ava::observability::TraceEventType::SessionLoadResult)
      event.fields = {{.key = "ephemeral", .value = ephemeral_ ? "true" : "false"}, {.key = "entry_count", .value = std::to_string(entry_count_)}};
    else
      event.fields = {{.key = "entry_type", .value = to_string(*entry_type_)}, {.key = "ephemeral", .value = ephemeral_ ? "true" : "false"}};
  });
}

void SessionTraceScope::succeed(std::size_t entry_count) noexcept
{
  outcome_ = ava::observability::TraceOutcome::Success;
  entry_count_ = entry_count;
}

}  // namespace detail

void SessionLease::swap(SessionLease& other)
{
  std::swap(fd_, other.fd_);
  canonical_path_.swap(other.canonical_path_);
  std::swap(created_, other.created_);
}

using detail::project_key;

SessionStore::SessionStore(SessionStoreOptions options) : options_(std::move(options)), observation_attachment_(std::make_shared<ObservationAttachment>())
{
}

SessionStore::SessionStore(SessionStoreOptions options, std::shared_ptr<EphemeralState> ephemeral_state)
    : options_(std::move(options)), ephemeral_state_(std::move(ephemeral_state)), observation_attachment_(std::make_shared<ObservationAttachment>())
{
}

std::string const& SessionStore::session_id() const noexcept
{
  return options_.session_id;
}

std::filesystem::path SessionStore::session_path() const
{
  return options_.root_dir / project_key(options_.workspace_dir) / (options_.session_id + ".jsonl");
}

bool SessionStore::is_ephemeral() const noexcept
{
  return static_cast<bool>(ephemeral_state_);
}

std::uint64_t SessionStore::set_run_observation(std::shared_ptr<ava::observability::RunObservation> const& observation,
                                                ava::observability::TraceContext const& context) noexcept
{
  // Disabled observation has no attachment state and does not take its lock.
  if (!observation || !observation->enabled())
    return 0;
  try
  {
    if (observation_attachment_->fail_next_attachment_for_test.exchange(false, std::memory_order_relaxed))
      throw std::bad_alloc();
    // Finish every allocation before acquiring the lock so a failure leaves
    // the live attachment untouched. Immutable shared context also makes the
    // append/load snapshot and SessionTraceScope construction no-throw.
    auto prepared_context = std::make_shared<ava::observability::TraceContext>(context);
    if (prepared_context->session_id.empty())
      prepared_context->session_id = session_id();
    auto prepared_observation = observation;

    std::lock_guard lock(observation_attachment_->mutex);
    observation_attachment_->observation = std::move(prepared_observation);
    observation_attachment_->context = std::move(prepared_context);
    observation_attachment_->enabled.store(true, std::memory_order_release);
    auto generation = observation_attachment_->generation + 1;
    if (generation == 0)
      generation = 1;
    observation_attachment_->generation = generation;
    return generation;
  }
  catch (...)
  {
    observation->account_external_failure();
    return 0;
  }
}

void SessionStore::fail_next_run_observation_attachment_for_test() noexcept
{
  observation_attachment_->fail_next_attachment_for_test.store(true, std::memory_order_relaxed);
}

void SessionStore::clear_run_observation(std::uint64_t generation) noexcept
{
  if (generation == 0)
    return;
  try
  {
    std::lock_guard lock(observation_attachment_->mutex);
    if (observation_attachment_->generation == generation)
    {
      observation_attachment_->enabled.store(false, std::memory_order_release);
      observation_attachment_->observation.reset();
      observation_attachment_->context = {};
    }
  }
  catch (...)
  {
    // A cleanup failure is observer-only and must never terminate a run.
  }
}

SessionStore SessionStore::detached_copy_for_background_persistence() const
{
  return SessionStore(options_, ephemeral_state_);
}

void SessionStore::set_before_append_identity_check_for_test(std::function<void()> hook)
{
  before_append_identity_check_for_test_ = std::move(hook);
}

void SessionStore::set_after_append_write_for_test(std::function<void()> hook)
{
  after_append_write_for_test_ = std::move(hook);
}

void SessionStore::set_after_lease_bound_read_for_test(std::function<void()> hook)
{
  after_lease_bound_read_for_test_ = std::move(hook);
}

void SessionStore::fail_persistent_append_target_allocation_for_test(bool fail) noexcept
{
  fail_persistent_append_target_allocation_for_test_ = fail;
}

void SessionStore::fail_persistent_read_authority_allocation_for_test(bool fail) noexcept
{
  fail_persistent_read_authority_allocation_for_test_ = fail;
}

void SessionStore::set_append_write_for_test(std::function<ssize_t(int, std::string_view)> hook)
{
  append_write_for_test_ = std::move(hook);
}

void SessionStore::set_before_created_file_rollback_detach_for_test(std::function<void()> hook)
{
  before_created_file_rollback_detach_for_test_ = std::move(hook);
}

void SessionStore::set_after_created_file_rollback_detach_for_test(std::function<void()> hook)
{
  after_created_file_rollback_detach_for_test_ = std::move(hook);
}

}  // namespace ava::session
