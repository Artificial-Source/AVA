#pragma once

#include "ava/observability/run_observer.h"
#include "ava/session/session_store.h"
#include "ava/core/strict_json.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include "debug.h"

namespace ava::session {

struct SessionStore::EphemeralState
{
  explicit EphemeralState(std::filesystem::path root) : root_dir(std::move(root)) { }

  EphemeralState(EphemeralState const&) = delete;
  EphemeralState& operator=(EphemeralState const&) = delete;

  ~EphemeralState()
  {
    std::error_code remove_error;
    std::filesystem::remove_all(root_dir, remove_error);
  }

  std::filesystem::path root_dir;
  // Serializes mutations across all copied stores/append targets. Keep this
  // distinct from entries_mutex so readers can take a short snapshot without
  // blocking a complete append preflight and write transaction.
  mutable std::mutex mutation_mutex;
  mutable std::mutex entries_mutex;
  std::vector<SessionEntry> entries;
  // Monotonic write generation for this in-memory store, analogous to
  // append_epoch_for_path for persistent files: bumped on every durable append
  // so independent ephemeral targets detect each other's advances.
  std::atomic<std::uint64_t> append_epoch{0};

  // Owns the complete in-memory session transcript; never generate debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct SessionStore::ObservationAttachment
{
  mutable std::mutex mutex;
  std::shared_ptr<ava::observability::RunObservation> observation;
  std::shared_ptr<ava::observability::TraceContext const> context;
  std::uint64_t generation = 0;
  std::atomic_bool enabled = false;
  std::atomic_bool fail_next_attachment_for_test = false;

  // Holds live observer state and trace context; never generate debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

namespace detail {

enum class AppendCommitState
{
  NotStarted,
  PartialOrUnknown,
  CommittedToLeasedInode,
};

[[nodiscard]] std::string_view append_commit_state_text(AppendCommitState state);
[[nodiscard]] bool has_append_commit_state(ava::core::Error const& error);
[[nodiscard]] ava::core::Error with_append_commit_state(ava::core::Error error, AppendCommitState state, std::filesystem::path const& path);

class ScopedFd
{
 public:
  explicit ScopedFd(int fd = -1) noexcept;
  ScopedFd(ScopedFd const&) = delete;
  ScopedFd& operator=(ScopedFd const&) = delete;
  ScopedFd(ScopedFd&& other) noexcept;
  ScopedFd& operator=(ScopedFd&& other) noexcept;
  ~ScopedFd();

  [[nodiscard]] int get() const noexcept;
  [[nodiscard]] int release() noexcept;
  [[nodiscard]] int close_checked() noexcept;
  void reset(int fd = -1) noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  int fd_ = -1;
};

class SessionTraceScope
{
 public:
  SessionTraceScope(std::shared_ptr<ava::observability::RunObservation> observation, std::shared_ptr<ava::observability::TraceContext const> context,
                    ava::observability::TraceEventType attempt, ava::observability::TraceEventType result, std::optional<EntryType> entry_type,
                    bool ephemeral) noexcept;
  ~SessionTraceScope() noexcept;

  void succeed(std::size_t entry_count = 0) noexcept;

  // Holds live observer state and trace context; never generate debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  std::shared_ptr<ava::observability::RunObservation> observation_;
  std::shared_ptr<ava::observability::TraceContext const> context_;
  ava::observability::TraceEventType result_;
  std::optional<EntryType> entry_type_;
  bool ephemeral_ = false;
  std::size_t entry_count_ = 0;
  ava::observability::TraceOutcome outcome_ = ava::observability::TraceOutcome::Error;
};

[[nodiscard]] std::string project_key(std::filesystem::path const& workspace_dir);
[[nodiscard]] std::mutex& append_mutex_for_path(std::filesystem::path const& path);
// Per-path monotonic write generation shared by all append targets on one file.
[[nodiscard]] std::atomic<std::uint64_t>& append_epoch_for_path(std::filesystem::path const& path);
[[nodiscard]] ava::core::Error path_io_error(std::string message, std::filesystem::path const& path, int error_number = 0);
[[nodiscard]] std::filesystem::path anchored_child_diagnostic_path(int parent_fd, std::string_view name, std::filesystem::path const& fallback_parent);
[[nodiscard]] ava::core::Error append_authority_error(std::string message, std::filesystem::path const& path);
[[nodiscard]] ava::core::Result<std::pair<int, off_t>> open_regular_snapshot(std::filesystem::path const& path, std::size_t max_file_bytes);
[[nodiscard]] ava::core::VoidResult validate_read_limits(SessionReadLimits const& limits);
[[nodiscard]] ava::core::Error strict_session_record_error(ava::core::StrictJsonStatus status, std::filesystem::path const& path, bool final_unterminated);
[[nodiscard]] ava::core::Result<SessionEntry> parse_strict_session_record(std::string_view line, std::filesystem::path const& path, bool final_unterminated);
[[nodiscard]] bool same_file_identity(struct stat const& left, struct stat const& right);
[[nodiscard]] bool same_directory_identity(struct stat const& left, struct stat const& right);

}  // namespace detail
}  // namespace ava::session
