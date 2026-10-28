#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/result.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ava::agent {

enum class BackgroundJobState
{
  Running,
  Completed,
  Failed,
  Canceled,
};

[[nodiscard]] std::string to_string(BackgroundJobState state);

struct BackgroundJobSnapshot
{
  std::string job_id = {};
  std::string title = {};
  std::string description = {};
  std::string subagent_type = {};
  std::string child_session_id = {};
  std::filesystem::path child_session_path = {};
  BackgroundJobState state = BackgroundJobState::Running;
  std::string created_at = {};
  std::optional<std::string> completed_at = std::nullopt;
  bool cancel_requested = false;
  std::string final_text = {};
  bool description_truncated = false;
  bool final_text_truncated = false;
  std::string stop_reason = {};
  std::optional<std::string> error = std::nullopt;
  std::size_t provider_iterations = 0;
  std::size_t tool_calls = 0;
  std::size_t tool_iterations = 0;
  bool timed_out = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct BackgroundJobStartOptions
{
  // Coordinators supply their process-local identity before publication.
  // Standalone registry callers may leave this empty to retain generated IDs.
  std::string job_id = {};
  std::string title = {};
  std::string description = {};
  std::string subagent_type = {};
  std::string child_session_id = {};
  std::filesystem::path child_session_path = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct BackgroundJobContext
{
  std::string job_id = {};
  std::stop_token stop_token;

  // BackgroundJobContext carries a std::stop_token, which has no debug
  // ostream representation, so this type opts out of print_members.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct BackgroundJobCompletion
{
  BackgroundJobState state = BackgroundJobState::Completed;
  std::string final_text;
  std::string stop_reason;
  std::optional<ava::core::Error> error = std::nullopt;
  std::size_t provider_iterations = 0;
  std::size_t tool_calls = 0;
  std::size_t tool_iterations = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using BackgroundJobWorker = std::function<BackgroundJobCompletion(BackgroundJobContext const&)>;

struct BackgroundJobRegistryOptions
{
  std::size_t max_running_jobs = 8;
  std::size_t max_retained_finished_jobs = 64;
  std::size_t max_description_bytes = 8 * 1024;
  std::size_t max_final_text_bytes = 64 * 1024;
  // Deterministic test seam for the otherwise platform-dependent jthread
  // construction failure path. Production leaves this empty.
  std::function<ava::core::VoidResult()> thread_start_preflight = nullptr;
  // Deterministic test seam that waits for a started worker to publish its
  // terminal registry state before start() returns. Production leaves it off.
  bool wait_for_terminal_before_start_returns = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class BackgroundJobRegistry final
{
 public:
  explicit BackgroundJobRegistry(BackgroundJobRegistryOptions options = {});
  ~BackgroundJobRegistry();

  BackgroundJobRegistry(BackgroundJobRegistry const&) = delete;
  BackgroundJobRegistry& operator=(BackgroundJobRegistry const&) = delete;
  BackgroundJobRegistry(BackgroundJobRegistry&&) = delete;
  BackgroundJobRegistry& operator=(BackgroundJobRegistry&&) = delete;

  [[nodiscard]] ava::core::Result<BackgroundJobSnapshot> start(BackgroundJobStartOptions options, BackgroundJobWorker worker);
  [[nodiscard]] std::vector<BackgroundJobSnapshot> snapshot() const;
  [[nodiscard]] ava::core::Result<BackgroundJobSnapshot> snapshot(std::string_view job_id) const;
  [[nodiscard]] ava::core::Result<BackgroundJobSnapshot> cancel(std::string_view job_id);
  [[nodiscard]] ava::core::Result<BackgroundJobSnapshot> wait(std::string_view job_id, std::chrono::milliseconds timeout);
  // Coordinator-only timeout shape; standalone wait() retains its historical
  // timeout error contract.
  [[nodiscard]] ava::core::Result<BackgroundJobSnapshot> wait_snapshot(std::string_view job_id, std::chrono::milliseconds timeout);
  std::size_t join_finished();
  void request_stop_all();
  void shutdown();

 private:
  struct JobRecord;

  [[nodiscard]] std::shared_ptr<JobRecord> find_record_locked(std::string_view job_id) const;
  [[nodiscard]] std::size_t running_job_count_locked() const;
  // Prunes only registry-owned execution records. Coordinator snapshots live
  // in independently owned JobState objects and are never affected.
  void prune_retained_finished_jobs_locked();
  void complete(std::string const& job_id, BackgroundJobCompletion completion);
  void join_all();

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  BackgroundJobRegistryOptions options_;
  bool accepting_ = true;
  // Waiters and joiners retain records after unlocking the map or pruning it.
  // Workers borrow the registry; shutdown joins them before member destruction.
  std::unordered_map<std::string, std::shared_ptr<JobRecord>> jobs_;

  // BackgroundJobRegistry owns synchronization primitives (mutex, condition
  // variable) and an opaque JobRecord map that have no debug ostream form, so
  // it opts out of print_members.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::agent
