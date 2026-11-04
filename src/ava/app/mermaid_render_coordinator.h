// Owns application Mermaid worker execution and publishes bounded completion payloads before consumer notification.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ava::app {

struct MermaidDisplaySettings;

inline constexpr std::size_t kMaxMermaidSourceBytes = 64 * 1024;
inline constexpr auto kMermaidRenderDeadline = std::chrono::seconds(2);
inline constexpr std::size_t kMaxMermaidOutputBytes = 256 * 1024;
inline constexpr std::size_t kMaxMermaidOutputLines = 4096;
inline constexpr std::size_t kMaxMermaidOutputLineBytes = 8 * 1024;
// Caps all undrained identities across queued, in-flight, and completed requests.
inline constexpr std::size_t kMaxMermaidQueuedRequests = 32;
inline constexpr std::size_t kMaxMermaidCacheEntries = 128;
inline constexpr std::size_t kMaxMermaidAcceptedCacheBytes = 4 * 1024 * 1024;

// The caller owns epoch assignment. A changed helper configuration must use a
// new epoch; reusing an epoch with different values is rejected fail-closed.
struct MermaidRenderConfiguration
{
  std::uint64_t epoch = 0;
  bool enabled = false;
  std::vector<ava::core::NoLogString> argv;

  // argv may contain sensitive local arguments and must not enter debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] MermaidRenderConfiguration mermaid_render_configuration_from_display_settings(MermaidDisplaySettings const& settings, std::uint64_t epoch);

struct MermaidRenderRequest
{
  std::uint64_t identity = 0;
  std::uint64_t config_epoch = 0;
  ava::core::NoLogString source;

  // Mermaid source is user payload and must not enter debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

enum class MermaidEnqueueResult : std::uint8_t
{
  Queued,
  AttachedToExisting,
  CompletedFromCache,
  CompletedFallback,
  QueueFull,
  StaleEpoch,
};

enum class MermaidRenderOutcome : std::uint8_t
{
  Accepted,
  Disabled,
  SourceTooLarge,
  MissingHelper,
  LaunchFailed,
  Timeout,
  Canceled,
  Signaled,
  NonzeroExit,
  OutputOverflow,
  UnsafeOutput,
};

struct MermaidRenderCompletion
{
  std::uint64_t identity = 0;
  std::uint64_t config_epoch = 0;
  MermaidRenderOutcome outcome = MermaidRenderOutcome::LaunchFailed;
  ava::core::NoLogString text;
  bool from_cache = false;

  // Accepted text is helper output derived from user payload and is never logged.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct MermaidRenderCoordinatorStats
{
  std::uint64_t config_epoch = 0;
  std::size_t queued_requests = 0;
  std::size_t pending_identities = 0;
  std::size_t completion_count = 0;
  std::size_t cache_entries = 0;
  std::size_t accepted_cache_bytes = 0;
  bool in_flight = false;
  bool shutting_down = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Application-owned single-worker renderer; frontend consumption uses completion
// notifications, and queue methods remain separate from helper execution.
// Application-owned worker and synchronized request/result queues. Public queue operations may run from caller
// threads but perform no helper I/O; mutex acquisition may wait. Only the worker owns helper execution. The
// application serializes shutdown/destruction, stops external calls, and joins the worker before Impl storage
// is destroyed.
class MermaidRenderCoordinator final
{
 public:
  // Caller thread: validate configuration and construct wake resources before starting the owned worker;
  // failure returns an error without a live coordinator.
  [[nodiscard]] static ava::core::Result<std::unique_ptr<MermaidRenderCoordinator>> create(MermaidRenderConfiguration configuration = {});
  // Owning application thread after external callers retire: shutdown and join before queue, callback, or wake
  // resources are destroyed.
  ~MermaidRenderCoordinator();

  // A single owner controls worker lifetime and queue/configuration authority.
  MermaidRenderCoordinator(MermaidRenderCoordinator const&) = delete;
  // Prevent replacement of a running worker's callback target and synchronization state.
  MermaidRenderCoordinator& operator=(MermaidRenderCoordinator const&) = delete;

  // Caller thread: under queue lock, replace a changed epoch and retire old queue/results/cache; wake active
  // work to observe cancellation.
  [[nodiscard]] ava::core::VoidResult reconfigure(MermaidRenderConfiguration configuration);
  // Caller thread: admit identity under queue mutex, deduplicate source/cache work, and publish immediate
  // fallback/cache completions before notifying.
  [[nodiscard]] MermaidEnqueueResult enqueue(MermaidRenderRequest request);
  // Caller thread: remove one current-epoch waiter and publish cancellation; helper stops only when no
  // identities still need it.
  [[nodiscard]] bool cancel(std::uint64_t identity, std::uint64_t config_epoch);
  // Consumer thread: atomically take owned completions under queue lock, releasing their admission capacity
  // before further enqueue.
  [[nodiscard]] std::vector<MermaidRenderCompletion> take_completions();
  // Any thread: replace registration under the queue mutex, then notify existing
  // work after unlocking. Empty unregisters; shutdown rejects later registration.
  // Notifications may run concurrently on caller/worker threads and may query or
  // drain this owner. Captures must survive already copied calls after unregister.
  // A callback must not destroy this owner or call shutdown: it may be running
  // on the worker that shutdown must join. Callback exceptions are contained.
  void set_completion_notifier(std::function<void()> notifier);
  // Any caller thread while owner lives: copy bounded counters under the queue mutex without exposing
  // source/output.
  [[nodiscard]] MermaidRenderCoordinatorStats stats() const;

  // Non-worker caller: bounded wait for active helper work or shutdown; false also covers timeout and no active
  // work.
  [[nodiscard]] bool wait_for_in_flight(std::chrono::milliseconds timeout) const;
  // Non-worker caller: wait until active and queued work are empty; undrained completions may still retain
  // admission capacity.
  [[nodiscard]] bool wait_until_idle(std::chrono::milliseconds timeout) const;
  // Owning non-worker thread, serialized with other shutdown calls: stop admission, cancel active work, release
  // the queue lock, then join. External callers must retire before destruction.
  void shutdown() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Private heap-stable worker target owning mutex, queues, callback, cache, and wake resources; destroyed only
  // after join.
  struct Impl;
  // Construction thread: take sole ownership of a fully formed but not-yet-started implementation.
  explicit MermaidRenderCoordinator(std::unique_ptr<Impl> impl);

  // Exclusive stable worker state; public destructor joins before this member releases synchronization and
  // payload storage.
  std::unique_ptr<Impl> impl_;
};

}  // namespace ava::app
