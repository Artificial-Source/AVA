#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
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

// Application-owned single-worker renderer. enqueue(), cancel(), and
// take_completions() do no helper I/O and are suitable for a frontend poll loop.
class MermaidRenderCoordinator final
{
 public:
  [[nodiscard]] static ava::core::Result<std::unique_ptr<MermaidRenderCoordinator>> create(MermaidRenderConfiguration configuration = {});
  ~MermaidRenderCoordinator();

  MermaidRenderCoordinator(MermaidRenderCoordinator const&) = delete;
  MermaidRenderCoordinator& operator=(MermaidRenderCoordinator const&) = delete;

  [[nodiscard]] ava::core::VoidResult reconfigure(MermaidRenderConfiguration configuration);
  [[nodiscard]] MermaidEnqueueResult enqueue(MermaidRenderRequest request);
  [[nodiscard]] bool cancel(std::uint64_t identity, std::uint64_t config_epoch);
  [[nodiscard]] std::vector<MermaidRenderCompletion> take_completions();
  [[nodiscard]] MermaidRenderCoordinatorStats stats() const;

  [[nodiscard]] bool wait_for_in_flight(std::chrono::milliseconds timeout) const;
  [[nodiscard]] bool wait_until_idle(std::chrono::milliseconds timeout) const;
  void shutdown() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct Impl;
  explicit MermaidRenderCoordinator(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace ava::app
