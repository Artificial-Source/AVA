// Adapts application-owned Mermaid rendering into payload callbacks and completion notification for the TUI.
#pragma once

#include "ava/app/mermaid_render_coordinator.h"
#include "ava/app/tui/tui_display_settings.h"
#include "ava/tui/runtime.h"
#include "ava/core/result.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace ava::app {

struct MermaidTuiPresentationConfiguration
{
  std::uint64_t epoch = 0;
  bool enabled = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Application-owned adapter between validated display settings/process
// coordination and the TUI's process-neutral callback DTOs.
// Application-owned synchronized adapter retained by shared callback captures in TuiMermaidRenderBridge.
// Callers may run on application/TUI threads; mutex protects settings, coordinator lifetime, epoch, and
// shutdown. Bridge lock precedes coordinator lock, so notification must never reenter either. Final destruction
// waits for worker shutdown after all callback owners release their shares.
class MermaidTuiBridge final : public std::enable_shared_from_this<MermaidTuiBridge>
{
 public:
  // Application setup thread: construct a coordinator at epoch one; shared ownership is required because
  // exported callbacks keep the adapter alive. Coordinator creation failure throws core::Error.
  [[nodiscard]] static std::shared_ptr<MermaidTuiBridge> create(MermaidDisplaySettings const& settings);
  // Preserve one synchronized application adapter and epoch authority for all exported callbacks.
  MermaidTuiBridge(MermaidTuiBridge const&) = delete;
  // Do not replace coordinator ownership beneath callbacks holding this adapter alive.
  MermaidTuiBridge& operator=(MermaidTuiBridge const&) = delete;
  // Final owning thread after callback references retire: idempotently shut down and join application worker
  // before member teardown.
  ~MermaidTuiBridge();

  // Application settings thread: under bridge lock, validate/reconfigure coordinator before publishing a new
  // epoch/settings pair.
  [[nodiscard]] ava::core::VoidResult apply(MermaidDisplaySettings const& settings);
  // Caller thread: briefly lock to copy coherent enabled/epoch facts; returned value retains no mutable bridge
  // state.
  [[nodiscard]] MermaidTuiPresentationConfiguration presentation_configuration() const noexcept;
  // Application/TUI setup: create callbacks that each retain shared adapter lifetime and copy initial
  // configuration; no UI owner is captured.
  [[nodiscard]] ava::tui::TuiMermaidRenderBridge tui_bridge();
  // Owning application thread: serialize teardown with callbacks, mark admission closed, and join coordinator
  // work while adapter resources remain alive.
  void shutdown() noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Construction thread: take coordinator ownership and initial settings before publishing shared callback
  // handles.
  MermaidTuiBridge(std::unique_ptr<MermaidRenderCoordinator> coordinator, MermaidDisplaySettings settings, std::uint64_t epoch);

  // TUI thread: try bridge lock and return Busy on contention; validate epoch/enabled before forwarding owned
  // payload under coordinator serialization.
  [[nodiscard]] ava::tui::TuiMermaidEnqueueResult enqueue(ava::tui::TuiMermaidRenderRequest request) noexcept;
  // TUI thread: best-effort try-lock cancellation; false may leave a completion pending, so caller must still
  // drain retired identities.
  [[nodiscard]] bool cancel(std::uint64_t identity, std::uint64_t config_epoch) noexcept;
  // TUI thread: nullopt preserves lock contention as Busy for bounded retries.
  // TUI thread: coordinator exceptions become an empty result. Projection allocation is outside that catch;
  // allocation failure terminates under this noexcept API.
  [[nodiscard]] std::optional<std::vector<ava::tui::TuiMermaidRenderCompletion>> drain() noexcept;
  // TUI setup/teardown only. The notifier may run on the rendering worker and
  // must not borrow UI owners; registration is serialized with shutdown.
  void set_completion_notifier(std::function<void()> notifier);

  // Serializes settings/epoch and coordinator lifetime across application/TUI callbacks; acquire before
  // coordinator mutex, never from notification callback.
  mutable std::mutex mutex_;
  // Exclusive application worker owner, accessed under bridge mutex; shutdown joins before destruction releases
  // it.
  std::unique_ptr<MermaidRenderCoordinator> coordinator_;
  // Last successfully applied settings under bridge mutex; equality avoids unnecessary epoch invalidation.
  MermaidDisplaySettings settings_;
  // Bridge-mutex-protected configuration identity advanced only after successful reconfiguration; exhaustion
  // rejects further changes.
  std::uint64_t epoch_ = 0;
  // Bridge-mutex-protected one-way admission latch; settings/admission calls reject after shutdown begins.
  bool shutdown_ = false;
};

}  // namespace ava::app
