#include "sys.h"
#include "ava/session/assistant_output.h"
#include "ava/session/compaction.h"
#include "ava/session/record.h"
#include "ava/session/session_store_internal.h"
#include "ava/core/json.h"
#include "ava/core/path.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ava::session {
namespace {

using ava::core::normalized_absolute_path;
using detail::append_authority_error;
using detail::append_commit_state_text;
using detail::append_epoch_for_path;
using detail::append_mutex_for_path;
using detail::AppendCommitState;
using detail::has_append_commit_state;
using detail::parse_strict_session_record;
using detail::path_io_error;
using detail::same_file_identity;
using detail::ScopedFd;
using detail::with_append_commit_state;

bool append_committed_to_leased_inode(ava::core::Error const& error)
{
  return std::ranges::any_of(error.context(), [](ava::core::ErrorContext const& item) {
    return item.key == "append_commit_state" && item.value == append_commit_state_text(AppendCommitState::CommittedToLeasedInode);
  });
}

bool append_partial_or_unknown(ava::core::Error const& error)
{
  return std::ranges::any_of(error.context(), [](ava::core::ErrorContext const& item) {
    return item.key == "append_commit_state" && item.value == append_commit_state_text(AppendCommitState::PartialOrUnknown);
  });
}

ava::core::Error append_target_recovery_required_error(std::filesystem::path const& path)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Session, "session append target requires explicit recovery after a partially persisted batch");
  error.with_context("recovery", "call SessionAppendTarget::recover before any later append or append_batch").with_context("path", path.string());
  return error;
}

ava::core::Error batch_partial_failure(ava::core::Error const& failure, std::filesystem::path const& path, std::size_t persisted_entries)
{
  ava::core::Error normalized(failure.category(), failure.message());
  for (auto const& context : failure.context())
  {
    if (context.key != "append_commit_state" && context.key != "recovery")
      normalized.with_context(context.key, context.value);
  }
  normalized = with_append_commit_state(std::move(normalized), AppendCommitState::PartialOrUnknown, path);
  normalized.with_context("batch_persisted_entries", std::to_string(persisted_entries));
  normalized.with_context(
      "staged_prefix_recovery",
      "a batch prefix may be durable; retain the append authority and call SessionAppendTarget::recover to repair torn bytes plus any valid incomplete "
      "assistant-output suffix before accepting another mutation");
  return normalized;
}

using BranchSummaryIdentity = std::array<std::string, 3>;

ava::core::Result<BranchSummaryIdentity> branch_summary_identity(SessionEntry const& entry)
{
  if (entry.type != EntryType::BranchSummary)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "conditional branch summary append requires a BranchSummary entry"));
  auto source = ava::core::json::string_field(entry.data_json, "source_session_id");
  auto root = ava::core::json::string_field(entry.data_json, "branch_root_entry_id");
  auto tip = ava::core::json::string_field(entry.data_json, "branch_tip_entry_id");
  if (!source || source->empty() || !root || root->empty() || !tip || tip->empty())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "conditional branch summary append requires a complete final identity"));
  }
  return BranchSummaryIdentity{std::move(*source), std::move(*root), std::move(*tip)};
}

bool matches_branch_summary_identity(SessionEntry const& entry, BranchSummaryIdentity const& identity)
{
  if (entry.type != EntryType::BranchSummary)
    return false;
  return ava::core::json::string_field(entry.data_json, "source_session_id") == std::optional<std::string>(identity[0]) &&
         ava::core::json::string_field(entry.data_json, "branch_root_entry_id") == std::optional<std::string>(identity[1]) &&
         ava::core::json::string_field(entry.data_json, "branch_tip_entry_id") == std::optional<std::string>(identity[2]);
}

ava::core::VoidResult validate_branch_summary_range(std::vector<SessionEntry> const& history, BranchSummaryIdentity const& identity)
{
  auto const root = std::ranges::find_if(history, [&](SessionEntry const& item) { return item.id == identity[1]; });
  auto const tip = std::ranges::find_if(history, [&](SessionEntry const& item) { return item.id == identity[2]; });
  if (root == history.end() || tip == history.end())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "branch summary source range changed before append"));
  if (std::distance(history.begin(), root) > std::distance(history.begin(), tip))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "branch summary root must not be after tip"));
  return {};
}

}  // namespace

struct SessionReadAuthority::State
{
  State(SessionStore store_in, std::optional<SessionLease> lease_in, SessionReadLimits limits_in)
      : store(std::move(store_in)), lease(std::move(lease_in)), limits(limits_in)
  {
  }

  SessionStore store;
  std::optional<SessionLease> lease;
  SessionReadLimits limits;
};

SessionReadAuthority::SessionReadAuthority(std::shared_ptr<State const> state) : state_(std::move(state))
{
}

ava::core::Result<SessionReadAuthority> SessionReadAuthority::create_persistent(SessionStore const& store, SessionLease const& lease, SessionReadLimits limits)
{
  if (store.is_ephemeral())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "persistent read authority requires a persistent store"));
  if (!lease.active())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "persistent read authority requires an active session lease"));

  try
  {
    // Complete every potentially throwing metadata copy before duplicating the
    // locked open-file description. The local SessionLease adopts the duplicate
    // immediately, so all later allocation failures close it through RAII.
    SessionStore authority_store = store;
    std::filesystem::path authority_path = normalized_absolute_path(store.session_path());
    std::filesystem::path authority_lease_path = lease.canonical_path_;
    bool const authority_created = lease.created_;
    bool const fail_allocation_for_test = store.fail_persistent_read_authority_allocation_for_test_;

    if (authority_path != authority_lease_path)
    {
      auto error = append_authority_error("persistent read authority lease does not exactly match the store", authority_path);
      error.with_context("lease_path", authority_lease_path.string());
      return std::unexpected(std::move(error));
    }
    auto const parent_path = authority_path.parent_path();
    auto const name = authority_path.filename().string();
    if (name.empty())
      return std::unexpected(path_io_error("persistent read authority has no basename", authority_path));

    ScopedFd parent_fd(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (parent_fd.get() < 0)
      return std::unexpected(path_io_error("failed to anchor persistent read authority directory", parent_path, errno));
    ScopedFd name_fd(::openat(parent_fd.get(), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
    if (name_fd.get() < 0)
      return std::unexpected(path_io_error("failed to inspect persistent read authority target", authority_path, errno));

    struct stat lease_status{};
    struct stat name_status{};
    struct stat named_status{};
    int const lease_error = fstat(lease.fd_, &lease_status) == 0 ? 0 : errno;
    int const name_error = fstat(name_fd.get(), &name_status) == 0 ? 0 : errno;
    int const named_error = fstatat(parent_fd.get(), name.c_str(), &named_status, AT_SYMLINK_NOFOLLOW) == 0 ? 0 : errno;
    int const name_close_error = name_fd.close_checked();
    int const parent_close_error = parent_fd.close_checked();
    if (lease_error != 0)
      return std::unexpected(path_io_error("failed to inspect persistent read authority lease", authority_path, lease_error));
    if (name_error != 0)
      return std::unexpected(path_io_error("failed to inspect persistent read authority name", authority_path, name_error));
    if (named_error != 0)
      return std::unexpected(path_io_error("failed to inspect persistent read authority publication", authority_path, named_error));
    if (name_close_error != 0)
      return std::unexpected(path_io_error("failed to close persistent read authority name descriptor", authority_path, name_close_error));
    if (parent_close_error != 0)
      return std::unexpected(path_io_error("failed to close persistent read authority directory", parent_path, parent_close_error));
    if (!same_file_identity(lease_status, name_status) || !same_file_identity(lease_status, named_status) || lease_status.st_nlink != 1 ||
        name_status.st_nlink != 1 || named_status.st_nlink != 1)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "persistent read authority does not identify one regular leased inode");
      error.with_context("path", authority_path.string());
      return std::unexpected(std::move(error));
    }

    int const duplicate_fd = fcntl(lease.fd_, F_DUPFD_CLOEXEC, 3);
    if (duplicate_fd < 0)
      return std::unexpected(path_io_error("failed to duplicate persistent read lease with CLOEXEC", authority_path, errno));
    SessionLease duplicated_lease(duplicate_fd, std::move(authority_lease_path), authority_created);
    if (fail_allocation_for_test)
      throw std::bad_alloc();
    auto state = std::make_shared<State>(std::move(authority_store), std::optional<SessionLease>(std::move(duplicated_lease)), limits);
    return SessionReadAuthority(std::move(state));
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to allocate persistent session read authority"));
  }
}

ava::core::Result<SessionReadAuthority> SessionReadAuthority::create_ephemeral(SessionStore const& store, SessionReadLimits limits)
{
  if (!store.is_ephemeral())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "ephemeral read authority requires an ephemeral store"));
  try
  {
    auto state = std::make_shared<State>(store, std::nullopt, limits);
    return SessionReadAuthority(std::move(state));
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to allocate ephemeral session read authority"));
  }
}

namespace {

std::uint64_t hash_tip(std::string_view id, std::string_view timestamp, std::size_t data_bytes) noexcept
{
  constexpr std::uint64_t kOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  std::uint64_t hash = kOffset;
  auto mix = [&](unsigned char byte) {
    hash ^= byte;
    hash *= kPrime;
  };
  for (char const ch : id) mix(static_cast<unsigned char>(ch));
  mix(0);
  for (char const ch : timestamp) mix(static_cast<unsigned char>(ch));
  mix(0);
  // Cast before shifting so 32-bit size_t never hits UB on shifts >= 32.
  auto const bytes = static_cast<std::uint64_t>(data_bytes);
  for (int shift = 0; shift < 64; shift += 8) mix(static_cast<unsigned char>((bytes >> shift) & 0xffU));
  return hash;
}

}  // namespace

std::string const& SessionReadAuthority::session_id() const noexcept
{
  static std::string const kEmpty;
  return state_ ? state_->store.session_id() : kEmpty;
}

bool SessionReadAuthority::is_ephemeral() const noexcept
{
  return state_ && !state_->lease.has_value();
}

bool SessionReadAuthority::active() const noexcept
{
  return static_cast<bool>(state_);
}

SessionReadLimits SessionReadAuthority::read_limits() const noexcept
{
  return state_ ? state_->limits : SessionReadLimits{};
}

SessionReadLimits SessionReadAuthority::clamp_limits(SessionReadLimits request) const noexcept
{
  SessionReadLimits const& policy = state_->limits;
  return SessionReadLimits{
      .max_file_bytes = std::min(policy.max_file_bytes, request.max_file_bytes),
      .max_line_bytes = std::min(policy.max_line_bytes, request.max_line_bytes),
      .max_entries = std::min(policy.max_entries, request.max_entries),
  };
}

ava::core::Result<SessionContentFingerprint> SessionReadAuthority::content_fingerprint() const
{
  if (!state_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session read authority is inactive"));

  SessionContentFingerprint fingerprint;
  if (!state_->lease)
  {
    // Append-only summary of the shared in-memory tip. This is not a durable
    // content identity and is insufficient for live inspection; coordinated
    // subagent inspection rejects ephemeral sources before publication.
    fingerprint.ephemeral = true;
    if (!state_->store.ephemeral_state_)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "ephemeral read authority is missing shared state"));
    std::lock_guard lock(state_->store.ephemeral_state_->entries_mutex);
    auto const& entries = state_->store.ephemeral_state_->entries;
    fingerprint.entry_count = entries.size();
    if (!entries.empty())
    {
      auto const& tip = entries.back();
      fingerprint.tip_hash = hash_tip(tip.id, tip.timestamp, tip.data_json.size());
    }
    return fingerprint;
  }

  if (!state_->lease->active())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "persistent read authority lease is inactive"));

  struct stat status{};
  if (fstat(state_->lease->fd_, &status) != 0)
    return std::unexpected(path_io_error("failed to fingerprint persistent read authority lease", state_->store.session_path(), errno));
  if (!S_ISREG(status.st_mode))
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Session, "persistent read authority lease is not a regular file"));

  fingerprint.ephemeral = false;
  fingerprint.dev = static_cast<std::uint64_t>(status.st_dev);
  fingerprint.ino = static_cast<std::uint64_t>(status.st_ino);
  fingerprint.size = status.st_size < 0 ? 0 : static_cast<std::uint64_t>(status.st_size);
#if defined(__linux__)
  fingerprint.mtime_sec = static_cast<std::int64_t>(status.st_mtim.tv_sec);
  fingerprint.mtime_nsec = static_cast<std::int64_t>(status.st_mtim.tv_nsec);
#else
  fingerprint.mtime_sec = static_cast<std::int64_t>(status.st_mtime);
  fingerprint.mtime_nsec = 0;
#endif
  return fingerprint;
}

ava::core::Result<std::vector<SessionEntry>> SessionReadAuthority::load() const
{
  if (!state_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session read authority is inactive"));
  return state_->lease ? state_->store.load_bounded(*state_->lease, state_->limits) : state_->store.load_bounded(state_->limits);
}

ava::core::Result<std::vector<SessionEntry>> SessionReadAuthority::load_bounded(SessionReadLimits limits, SessionCancelCallback cancel_requested) const
{
  if (!state_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session read authority is inactive"));
  auto const effective = clamp_limits(limits);
  return state_->lease ? state_->store.load_bounded(*state_->lease, effective, std::move(cancel_requested))
                       : state_->store.load_bounded(effective, std::move(cancel_requested));
}

ava::core::Result<SessionSummary> SessionReadAuthority::inspect_bounded(SessionReadLimits limits, SessionCancelCallback cancel_requested) const
{
  if (!state_)
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session read authority is inactive"));
  auto const effective = clamp_limits(limits);
  return state_->lease ? state_->store.inspect_bounded(*state_->lease, effective, std::move(cancel_requested))
                       : state_->store.inspect_bounded(effective, std::move(cancel_requested));
}

SessionAppendTarget::SessionAppendTarget(SessionStore store, std::optional<SessionLease> lease, AssistantOutputAppendState assistant_output_state,
                                         SessionReadLimits read_limits)
    : store_(std::move(store)), lease_(std::move(lease)), read_limits_(read_limits), assistant_output_state_(std::move(assistant_output_state))
{
}

std::atomic<std::uint64_t>& SessionAppendTarget::append_epoch_ref() const
{
  if (lease_)
    return append_epoch_for_path(lease_->canonical_path());
  return store_.ephemeral_state_->append_epoch;
}

ava::core::VoidResult SessionAppendTarget::refresh_state_if_stale()
{
  auto const current = append_epoch_ref().load(std::memory_order_acquire);
  if (current == last_seen_append_epoch_)
    return {};
  auto history = lease_ ? store_.load_bounded(*lease_, read_limits_) : store_.load_bounded(read_limits_);
  if (!history)
    return std::unexpected(std::move(history.error()));
  auto refreshed = AssistantOutputAppendState::from_validated_history(*history);
  if (!refreshed)
    return std::unexpected(std::move(refreshed.error()));
  assistant_output_state_ = std::move(*refreshed);
  last_seen_append_epoch_ = current;
  return {};
}

std::uint64_t SessionAppendTarget::advance_append_epoch()
{
  auto& epoch = append_epoch_ref();
  last_seen_append_epoch_ = epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
  return last_seen_append_epoch_;
}

void SessionAppendTarget::bump_append_epoch()
{
  append_epoch_ref().fetch_add(1, std::memory_order_acq_rel);
}

ava::core::Result<std::shared_ptr<SessionAppendTarget>> SessionAppendTarget::create_persistent(SessionStore const& store, SessionLease const& lease,
                                                                                               SessionReadLimits read_limits,
                                                                                               SessionCancelCallback cancel_requested)
{
  if (store.is_ephemeral())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "persistent append target requires a persistent store"));
  if (!lease.active())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "persistent append target requires an active session lease"));
  auto const path = normalized_absolute_path(store.session_path());
  if (path != lease.canonical_path())
  {
    auto error = append_authority_error("persistent append target lease does not exactly match the store", path);
    error.with_context("lease_path", lease.canonical_path().string());
    return std::unexpected(std::move(error));
  }
  auto const parent_path = path.parent_path();
  auto const name = path.filename().string();
  if (name.empty())
    return std::unexpected(path_io_error("persistent append target has no basename", path));
  ScopedFd parent_fd(::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (parent_fd.get() < 0)
    return std::unexpected(path_io_error("failed to anchor persistent append target directory", parent_path, errno));
  ScopedFd name_fd(::openat(parent_fd.get(), name.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC));
  if (name_fd.get() < 0)
    return std::unexpected(path_io_error("failed to inspect persistent append target", path, errno));
  struct stat lease_status{};
  struct stat name_status{};
  struct stat named_status{};
  int const lease_error = fstat(lease.fd_, &lease_status) == 0 ? 0 : errno;
  int const name_error = fstat(name_fd.get(), &name_status) == 0 ? 0 : errno;
  int const named_error = fstatat(parent_fd.get(), name.c_str(), &named_status, AT_SYMLINK_NOFOLLOW) == 0 ? 0 : errno;
  int const name_close_error = name_fd.close_checked();
  int const parent_close_error = parent_fd.close_checked();
  if (lease_error != 0)
    return std::unexpected(path_io_error("failed to inspect persistent append target lease", path, lease_error));
  if (name_error != 0)
    return std::unexpected(path_io_error("failed to inspect persistent append target name", path, name_error));
  if (named_error != 0)
    return std::unexpected(path_io_error("failed to inspect persistent append target publication", path, named_error));
  if (name_close_error != 0)
    return std::unexpected(path_io_error("failed to close persistent append target name descriptor", path, name_close_error));
  if (parent_close_error != 0)
    return std::unexpected(path_io_error("failed to close persistent append target directory", parent_path, parent_close_error));
  if (!same_file_identity(lease_status, name_status) || !same_file_identity(lease_status, named_status) || lease_status.st_nlink != 1 ||
      name_status.st_nlink != 1 || named_status.st_nlink != 1)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Session, "persistent append target does not identify one regular leased inode");
    error.with_context("path", path.string());
    return std::unexpected(std::move(error));
  }
  auto history = store.load_bounded(lease, read_limits, std::move(cancel_requested));
  if (!history)
    return std::unexpected(std::move(history.error()));
  auto assistant_output_state = AssistantOutputAppendState::from_validated_history(*history);
  if (!assistant_output_state)
    return std::unexpected(std::move(assistant_output_state.error()));

  struct PersistentTarget final : SessionAppendTarget
  {
    PersistentTarget(SessionStore store_in, SessionLease lease_in, AssistantOutputAppendState assistant_output_state_in, SessionReadLimits read_limits_in)
        : SessionAppendTarget(std::move(store_in), std::optional<SessionLease>(std::move(lease_in)), std::move(assistant_output_state_in), read_limits_in)
    {
    }
  };
  try
  {
    // Finish every potentially throwing copy before duplicating the locked open
    // file description. Once duplicated, local SessionLease RAII owns it across
    // all allocation and move failures.
    SessionStore target_store = store;
    std::filesystem::path target_canonical_path = lease.canonical_path_;
    bool const target_created = lease.created_;
    bool const fail_allocation_for_test = store.fail_persistent_append_target_allocation_for_test_;

    int const duplicate_fd = fcntl(lease.fd_, F_DUPFD_CLOEXEC, 3);
    if (duplicate_fd < 0)
      return std::unexpected(path_io_error("failed to duplicate persistent append lease with CLOEXEC", path, errno));
    SessionLease duplicated_lease(duplicate_fd, std::move(target_canonical_path), target_created);
    if (fail_allocation_for_test)
      throw std::bad_alloc();
    return std::make_shared<PersistentTarget>(std::move(target_store), std::move(duplicated_lease), std::move(*assistant_output_state), read_limits);
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to allocate persistent session append target"));
  }
}

ava::core::Result<std::shared_ptr<SessionAppendTarget>> SessionAppendTarget::create_ephemeral(SessionStore const& store, SessionReadLimits read_limits)
{
  if (!store.is_ephemeral())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "ephemeral append target requires an ephemeral store"));
  auto history = store.load_bounded(read_limits);
  if (!history)
    return std::unexpected(std::move(history.error()));
  auto assistant_output_state = AssistantOutputAppendState::from_validated_history(*history);
  if (!assistant_output_state)
    return std::unexpected(std::move(assistant_output_state.error()));
  struct EphemeralTarget final : SessionAppendTarget
  {
    EphemeralTarget(SessionStore store_in, AssistantOutputAppendState assistant_output_state_in, SessionReadLimits read_limits_in)
        : SessionAppendTarget(std::move(store_in), std::nullopt, std::move(assistant_output_state_in), read_limits_in)
    {
    }
  };
  try
  {
    return std::make_shared<EphemeralTarget>(store, std::move(*assistant_output_state), read_limits);
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to allocate ephemeral session append target"));
  }
}

static_assert(std::is_nothrow_move_assignable_v<AssistantOutputAppendState>);

ava::core::VoidResult SessionAppendTarget::append(SessionEntry const& entry)
{
  std::lock_guard lock(mutex_);
  if (recovery_required_)
    return std::unexpected(append_target_recovery_required_error(store_.session_path()));

  std::optional<std::unique_lock<std::mutex>> serialization_lock;
  if (lease_)
    serialization_lock.emplace(append_mutex_for_path(lease_->canonical_path()));
  else
    serialization_lock.emplace(store_.ephemeral_state_->mutation_mutex);

  // The append target keeps the live assistant-output state in the
  // assistant_output_state_ member, rebuilt only at creation (create_persistent
  // / create_ephemeral) and after a torn-write recovery (recover). Preflight
  // against that cache instead of reloading and re-projecting the whole history
  // on every append (which was O(n) per append, i.e. O(n^2) for a sequence).
  //
  // The per-path serialization lock acquired above is held across the stale
  // check, preflight, and write, so a second target built from an older
  // snapshot cannot advance the file underneath us. If another writer DID
  // advance the file since we last folded it into the cache, the shared append
  // epoch will differ from last_seen_append_epoch_ and refresh_state_if_stale
  // reloads from storage before the preflight. A partial/unknown write latches
  // recovery_required_ and bumps the epoch so other targets also reload.
  if (auto refreshed = refresh_state_if_stale(); !refreshed)
    return std::unexpected(std::move(refreshed.error()));
  AssistantOutputAppendState next_state;
  try
  {
    next_state = assistant_output_state_;
    if (auto preflight = next_state.apply_candidate(entry); !preflight)
      return std::unexpected(std::move(preflight.error()));
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to preflight assistant-output append state"));
  }

  auto appended = store_.append_impl(lease_ ? &*lease_ : nullptr, entry, true);
  if (!appended && lease_ && !has_append_commit_state(appended.error()))
    appended = std::unexpected(with_append_commit_state(std::move(appended.error()), AppendCommitState::NotStarted, store_.session_path()));
  if (!appended)
  {
    auto error = std::move(appended.error());
    if (append_partial_or_unknown(error))
    {
      recovery_required_ = true;
      bump_append_epoch();
    }
    else if (append_committed_to_leased_inode(error))
    {
      // The entry was durably committed even though a later step (e.g. close)
      // failed, so the cache advances to match storage and owns the new epoch.
      assistant_output_state_ = std::move(next_state);
      advance_append_epoch();
    }
    return std::unexpected(std::move(error));
  }
  assistant_output_state_ = std::move(next_state);
  advance_append_epoch();
  return {};
}

SessionAppendTarget::ConditionalAppendOutcome SessionAppendTarget::append_branch_summary_if_absent_classified(SessionEntry const& entry,
                                                                                                              SessionCancelCallback const& cancel_requested)
{
  auto rejected = [](ava::core::Error error) {
    return ConditionalAppendOutcome{.completion = ConditionalAppendCompletion::RejectedBeforeAppend, .result = std::nullopt, .error = std::move(error)};
  };
  auto append_failed = [](ava::core::Error error) {
    return ConditionalAppendOutcome{.completion = ConditionalAppendCompletion::AppendFailed, .result = std::nullopt, .error = std::move(error)};
  };
  try
  {
    std::lock_guard lock(mutex_);
    if (recovery_required_)
      return rejected(append_target_recovery_required_error(store_.session_path()));
    if (!lease_)
    {
      return rejected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "conditional branch summary append requires a persistent append target"));
    }
    auto identity = branch_summary_identity(entry);
    if (!identity)
      return rejected(std::move(identity.error()));
    if ((*identity)[0] != store_.session_id())
    {
      return rejected(
          ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "conditional branch summary source identity does not match the append target"));
    }

    std::unique_lock serialization_lock(append_mutex_for_path(lease_->canonical_path()));
    // Keep the callback alive through the read. It is checked once more below
    // while this per-path lock is still held.
    auto history = store_.load_bounded(*lease_, read_limits_, cancel_requested);
    if (!history)
      return rejected(std::move(history.error()));
    if (history->empty())
      return rejected(ava::core::Error(ava::core::ErrorCategory::Session, "source session has no entries before branch summary append"));
    auto rebuilt_state = AssistantOutputAppendState::from_validated_history(*history);
    if (!rebuilt_state)
      return rejected(std::move(rebuilt_state.error()));
    assistant_output_state_ = std::move(*rebuilt_state);
    last_seen_append_epoch_ = append_epoch_ref().load(std::memory_order_acquire);

    if (auto valid_range = validate_branch_summary_range(*history, *identity); !valid_range)
      return rejected(std::move(valid_range.error()));
    if (auto const existing = std::ranges::find_if(*history, [&](SessionEntry const& item) { return matches_branch_summary_identity(item, *identity); });
        existing != history->end())
    {
      auto const parent = std::ranges::find_if(*history, [&](SessionEntry const& item) { return item.id == entry.parent_id; });
      bool const exact_final_coverage =
          parent != history->end() && existing > parent &&
          std::ranges::all_of(std::next(parent), history->end(), [](SessionEntry const& item) { return item.type == EntryType::BranchSummary; });
      if (exact_final_coverage)
      {
        return ConditionalAppendOutcome{.completion = ConditionalAppendCompletion::Succeeded,
                                        .result = SessionConditionalAppendResult{.entry = *existing, .disposition = SessionAppendDisposition::Existing},
                                        .error = std::nullopt};
      }
    }
    if (entry.parent_id != history->back().id)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::Session, "source session changed before branch summary append");
      error.with_context("expected_parent_id", entry.parent_id).with_context("actual_parent_id", history->back().id);
      return rejected(std::move(error));
    }

    // Cancellation linearizes here: the exact source snapshot, range, duplicate,
    // and parent checks are complete under per-path serialization, but append
    // preflight and append_impl have not started. Cancellation after this check
    // may race with a committed append and is reported by the append result.
    if (cancel_requested && cancel_requested())
      return rejected(ava::core::Error(ava::core::ErrorCategory::Unknown, "conditional branch summary append canceled", ava::core::ErrorCode::Canceled));

    AssistantOutputAppendState next_state = assistant_output_state_;
    if (auto preflight = next_state.apply_candidate(entry); !preflight)
      return rejected(std::move(preflight.error()));

    auto appended = store_.append_impl(&*lease_, entry, true);
    if (!appended && !has_append_commit_state(appended.error()))
      appended = std::unexpected(with_append_commit_state(std::move(appended.error()), AppendCommitState::NotStarted, store_.session_path()));
    if (!appended)
    {
      auto error = std::move(appended.error());
      if (append_partial_or_unknown(error))
      {
        recovery_required_ = true;
        bump_append_epoch();
      }
      else if (append_committed_to_leased_inode(error))
      {
        assistant_output_state_ = std::move(next_state);
        advance_append_epoch();
      }
      return append_failed(std::move(error));
    }
    assistant_output_state_ = std::move(next_state);
    advance_append_epoch();
    return ConditionalAppendOutcome{.completion = ConditionalAppendCompletion::Succeeded,
                                    .result = SessionConditionalAppendResult{.entry = entry, .disposition = SessionAppendDisposition::Appended},
                                    .error = std::nullopt};
  }
  catch (std::exception const& exception)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "conditional branch summary append failed unexpectedly");
    error.with_context("cause", exception.what());
    return append_failed(with_append_commit_state(std::move(error), AppendCommitState::PartialOrUnknown, store_.session_path()));
  }
  catch (...)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "conditional branch summary append failed unexpectedly");
    return append_failed(with_append_commit_state(std::move(error), AppendCommitState::PartialOrUnknown, store_.session_path()));
  }
}

ava::core::Result<SessionConditionalAppendResult> SessionAppendTarget::append_branch_summary_if_absent(SessionEntry const& entry,
                                                                                                       SessionCancelCallback cancel_requested)
{
  try
  {
    auto outcome = append_branch_summary_if_absent_classified(entry, cancel_requested);
    if (outcome.completion == ConditionalAppendCompletion::Succeeded && outcome.result)
      return std::move(*outcome.result);
    if (outcome.error)
      return std::unexpected(std::move(*outcome.error));
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "conditional branch summary append produced no outcome"));
  }
  catch (std::exception const& exception)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "conditional branch summary append failed unexpectedly");
    error.with_context("cause", exception.what());
    return std::unexpected(with_append_commit_state(std::move(error), AppendCommitState::PartialOrUnknown, store_.session_path()));
  }
  catch (...)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "conditional branch summary append failed unexpectedly");
    return std::unexpected(with_append_commit_state(std::move(error), AppendCommitState::PartialOrUnknown, store_.session_path()));
  }
}

SessionAppendTarget::CompactionAppendOutcome SessionAppendTarget::append_compaction_if_snapshot_matches_classified(
    SessionEntry const& entry, std::vector<SessionEntry> const& expected, SessionCancelCallback const& cancel_requested)
{
  auto rejected = [](ava::core::Error error) {
    return CompactionAppendOutcome{.completion = ConditionalAppendCompletion::RejectedBeforeAppend, .result = std::nullopt, .error = std::move(error)};
  };
  auto append_failed = [](ava::core::Error error) {
    return CompactionAppendOutcome{.completion = ConditionalAppendCompletion::AppendFailed, .result = std::nullopt, .error = std::move(error)};
  };

  std::unique_lock target_lock(mutex_, std::defer_lock);
  std::optional<std::unique_lock<std::mutex>> serialization_lock;
  bool append_attempted = false;
  try
  {
    target_lock.lock();
    if (recovery_required_)
      return rejected(append_target_recovery_required_error(store_.session_path()));
    if (entry.type != EntryType::Compaction)
      return rejected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "conditional compaction append requires a Compaction entry"));

    // Lock order is target then destination. Keep both continuously through
    // authoritative reload, compare, preflight, append, and cache publication.
    if (lease_)
      serialization_lock.emplace(append_mutex_for_path(lease_->canonical_path()));
    else
      serialization_lock.emplace(store_.ephemeral_state_->mutation_mutex);

    auto history = lease_ ? store_.load_bounded(*lease_, read_limits_, cancel_requested) : store_.load_bounded(read_limits_, cancel_requested);
    if (!history)
      return rejected(std::move(history.error()));
    if (!compaction_snapshot_matches(expected, *history))
    {
      return CompactionAppendOutcome{
          .completion = ConditionalAppendCompletion::Succeeded, .result = SessionCompactionAppendResult::SnapshotMismatch, .error = std::nullopt};
    }

    // Cancellation linearizes after the exact compare and before append-state
    // rebuild/preflight. A later cancellation may race with a committed write
    // and is represented by append_impl's stable commit state.
    if (cancel_requested && cancel_requested())
      return rejected(ava::core::Error(ava::core::ErrorCategory::Unknown, "conditional compaction append canceled", ava::core::ErrorCode::Canceled));

    auto rebuilt_state = AssistantOutputAppendState::from_validated_history(*history);
    if (!rebuilt_state)
      return rejected(std::move(rebuilt_state.error()));
    AssistantOutputAppendState next_state = std::move(*rebuilt_state);
    if (auto preflight = next_state.apply_candidate(entry); !preflight)
      return rejected(std::move(preflight.error()));

    append_attempted = true;
    auto appended = store_.append_impl(lease_ ? &*lease_ : nullptr, entry, true);
    if (!appended && lease_ && !has_append_commit_state(appended.error()))
      appended = std::unexpected(with_append_commit_state(std::move(appended.error()), AppendCommitState::NotStarted, store_.session_path()));
    if (!appended)
    {
      auto error = std::move(appended.error());
      if (append_partial_or_unknown(error))
      {
        recovery_required_ = true;
        bump_append_epoch();
      }
      else if (append_committed_to_leased_inode(error))
      {
        assistant_output_state_ = std::move(next_state);
        advance_append_epoch();
      }
      return append_failed(std::move(error));
    }

    assistant_output_state_ = std::move(next_state);
    advance_append_epoch();
    return CompactionAppendOutcome{
        .completion = ConditionalAppendCompletion::Succeeded, .result = SessionCompactionAppendResult::Appended, .error = std::nullopt};
  }
  catch (std::exception const& exception)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "conditional compaction append failed unexpectedly");
    error.with_context("cause", exception.what());
    if (!append_attempted)
      return rejected(std::move(error));
    recovery_required_ = true;
    bump_append_epoch();
    return append_failed(with_append_commit_state(std::move(error), AppendCommitState::PartialOrUnknown, store_.session_path()));
  }
  catch (...)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::Unknown, "conditional compaction append failed unexpectedly");
    if (!append_attempted)
      return rejected(std::move(error));
    recovery_required_ = true;
    bump_append_epoch();
    return append_failed(with_append_commit_state(std::move(error), AppendCommitState::PartialOrUnknown, store_.session_path()));
  }
}

ava::core::Result<SessionCompactionAppendResult> SessionAppendTarget::append_compaction_if_snapshot_matches(SessionEntry const& entry,
                                                                                                            std::vector<SessionEntry> const& expected,
                                                                                                            SessionCancelCallback cancel_requested)
{
  auto outcome = append_compaction_if_snapshot_matches_classified(entry, expected, cancel_requested);
  if (outcome.completion == ConditionalAppendCompletion::Succeeded && outcome.result)
    return *outcome.result;
  if (outcome.error)
    return std::unexpected(std::move(*outcome.error));
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "conditional compaction append produced no outcome"));
}

ava::core::VoidResult SessionAppendTarget::append_batch(std::vector<SessionEntry> entries)
{
  std::lock_guard lock(mutex_);
  if (recovery_required_)
    return std::unexpected(append_target_recovery_required_error(store_.session_path()));
  if (entries.empty())
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session append batch must not be empty"));
  if (entries.size() > kMaxSessionAppendBatchEntries)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session append batch exceeds record limit");
    error.with_context("max_records", std::to_string(kMaxSessionAppendBatchEntries));
    return std::unexpected(std::move(error));
  }
  if (entries.back().type != EntryType::AssistantTurnCommit)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session append batch must end with one assistant_turn_commit"));
  }
  for (std::size_t index = 0; index + 1 < entries.size(); ++index)
  {
    if (entries[index].type != EntryType::AssistantOutputItem)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::InvalidArgument,
                                              "session append batch may contain only assistant_output_item records before its final commit"));
    }
  }

  std::size_t serialized_bytes = 0;
  for (auto const& entry : entries)
  {
    auto serialized = serialize_session_entry_line(entry);
    if (!serialized)
      return std::unexpected(std::move(serialized.error()));
    if (auto strict_entry = parse_strict_session_record(*serialized, store_.session_path(), false); !strict_entry)
      return std::unexpected(std::move(strict_entry.error()));
    if (serialized->size() >= kMaxSessionAppendBatchBytes || serialized_bytes >= kMaxSessionAppendBatchBytes ||
        serialized->size() > kMaxSessionAppendBatchBytes - serialized_bytes - 1)
    {
      auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session append batch exceeds serialized byte limit");
      error.with_context("max_bytes", std::to_string(kMaxSessionAppendBatchBytes));
      return std::unexpected(std::move(error));
    }
    serialized_bytes += serialized->size() + 1;
  }

  // The same persistent-path or shared ephemeral mutation lock spans the v4
  // preflight and every write. A second target constructed from an old snapshot
  // therefore cannot commit stale state after this target advances the session.
  std::optional<std::unique_lock<std::mutex>> serialization_lock;
  if (lease_)
    serialization_lock.emplace(append_mutex_for_path(lease_->canonical_path()));
  else
    serialization_lock.emplace(store_.ephemeral_state_->mutation_mutex);
  // As in append(), preflight against the cached assistant_output_state_
  // instead of reloading and re-projecting the whole history (which would be
  // O(n) per batch). Reload only when another writer advanced the shared
  // append epoch since we last folded it into the cache.
  if (auto refreshed = refresh_state_if_stale(); !refreshed)
    return std::unexpected(std::move(refreshed.error()));
  if (!assistant_output_state_.ready())
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::Session, "session append batch requires a ready assistant-output state with no staged transaction"));
  }

  AssistantOutputAppendState final_state;
  try
  {
    final_state = assistant_output_state_;
    for (auto const& entry : entries)
    {
      if (auto preflight = final_state.apply_candidate(entry); !preflight)
        return std::unexpected(std::move(preflight.error()));
    }
  }
  catch (...)
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Unknown, "failed to preflight assistant-output append batch state"));
  }
  if (!final_state.ready())
  {
    return std::unexpected(
        ava::core::Error(ava::core::ErrorCategory::Session, "session append batch must complete its assistant-output transaction before writing"));
  }

  std::size_t persisted_entries = 0;
  for (auto const& entry : entries)
  {
    auto appended = store_.append_impl(lease_ ? &*lease_ : nullptr, entry, true);
    if (!appended && lease_ && !has_append_commit_state(appended.error()))
      appended = std::unexpected(with_append_commit_state(std::move(appended.error()), AppendCommitState::NotStarted, store_.session_path()));
    if (!appended)
    {
      auto error = std::move(appended.error());
      bool const partial_or_unknown = append_partial_or_unknown(error);
      if (append_committed_to_leased_inode(error))
        ++persisted_entries;
      if (partial_or_unknown || persisted_entries != 0)
      {
        recovery_required_ = true;
        // Some batch records may be durable; advance the epoch so other targets
        // reload and see them. This target's cache is left stale on purpose
        // (recovery_required_ blocks further appends until recover() rebuilds).
        bump_append_epoch();
        error = batch_partial_failure(error, store_.session_path(), persisted_entries);
      }
      return std::unexpected(std::move(error));
    }
    ++persisted_entries;
    // Each durable entry advances the file; the epoch is finalized to our cache
    // only after the whole batch commits below.
    bump_append_epoch();
  }
  assistant_output_state_ = std::move(final_state);
  // The cache now reflects every durably written batch entry, so claim the
  // current epoch as the generation it represents.
  last_seen_append_epoch_ = append_epoch_ref().load(std::memory_order_acquire);
  return {};
}

ava::core::VoidResult SessionAppendTarget::recover()
{
  std::lock_guard lock(mutex_);
  std::optional<std::unique_lock<std::mutex>> ephemeral_mutation_lock;
  if (!lease_)
    ephemeral_mutation_lock.emplace(store_.ephemeral_state_->mutation_mutex);

  if (lease_)
  {
    auto recovered = store_.recover_torn_tail(*lease_, read_limits_);
    if (!recovered)
      return std::unexpected(std::move(recovered.error()));
    auto staged_recovery = store_.recover_incomplete_assistant_output_suffix(*lease_, read_limits_);
    if (!staged_recovery)
      return std::unexpected(std::move(staged_recovery.error()));
  }
  else
  {
    auto staged_recovery = store_.recover_incomplete_assistant_output_suffix_ephemeral_impl(read_limits_, nullptr, true);
    if (!staged_recovery)
      return std::unexpected(std::move(staged_recovery.error()));
  }
  auto history = lease_ ? store_.load_bounded(*lease_, read_limits_) : store_.load_bounded(read_limits_);
  if (!history)
    return std::unexpected(std::move(history.error()));
  auto rebuilt_state = AssistantOutputAppendState::from_validated_history(*history);
  if (!rebuilt_state)
    return std::unexpected(std::move(rebuilt_state.error()));
  assistant_output_state_ = std::move(*rebuilt_state);
  recovery_required_ = false;
  // Recovery may have truncated or completed the file, so advance the shared
  // epoch (forcing other targets to reload) and record that this target's
  // freshly rebuilt cache now owns the new generation.
  advance_append_epoch();
  return {};
}

ava::core::Result<SessionReadAuthority> SessionAppendTarget::read_authority() const
{
  return lease_ ? SessionReadAuthority::create_persistent(store_, *lease_, read_limits_) : SessionReadAuthority::create_ephemeral(store_, read_limits_);
}

bool SessionAppendTarget::is_ephemeral() const noexcept
{
  return !lease_.has_value();
}

}  // namespace ava::session
