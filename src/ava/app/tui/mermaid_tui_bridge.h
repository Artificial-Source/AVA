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

// Application-owned synchronized adapter retained by shared callback captures in TuiMermaidRenderBridge.
// Callers may run on application/TUI threads; mutex protects settings, epoch, and
// shutdown. The coordinator is never replaced, and this shared bridge outlives
// every exported callback. Notifications run outside both locks and may query
// the bridge. Final destruction joins the worker after callback owners retire;
// a worker notification must never destroy the bridge or request its own join.
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
  // TUI thread: nullopt requests retry after contention or allocation failure.
  // Reserve bounded output storage before removing results; once taken, owned
  // completion text moves into that storage without allocation or lost identities.
  [[nodiscard]] std::optional<std::vector<ava::tui::TuiMermaidRenderCompletion>> drain() noexcept;
  // TUI setup/teardown: registration can synchronously notify a backlog outside
  // both locks. Copied callbacks may run concurrently and outlive unregister;
  // they must not borrow UI owners or destroy/join their own rendering worker.
  void set_completion_notifier(std::function<void()> notifier);

  // Serializes settings/epoch and admission across application/TUI callbacks.
  // Release before any coordinator call that may invoke a notifier or join.
  mutable std::mutex mutex_;
  // Exclusive stable worker owner, never replaced or reset during bridge life.
  // Exported callbacks retain the bridge while borrowing this pointer unlocked;
  // owning-thread shutdown joins before destruction releases it.
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
