#pragma once

#include "TuiTimer.h"
#include "ava/debug/print_members_on.h"
#include "ava/tui/runtime_plugin_ui.h"
#include "ava/tui/terminal.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace ava::tui {

class RuntimePresentationState;

struct ComposerSnapshot;
struct RuntimePluginUiCoordinatorState;
class TuiEventLoop;

struct TuiPluginUiPollResult
{
  bool changed = false;
  bool deadline_expired = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

enum class TuiPluginUiInputResult
{
  Unhandled,
  Handled,
  Redraw,
  RequestStop,
};

[[nodiscard]] bool is_direct_foreground_plugin_run_submission(std::string_view submitted);

class RuntimePluginUiCoordinator final
{
 public:
  // TUI-thread owner; the borrowed projection outlives this coordinator.
  explicit RuntimePluginUiCoordinator(RuntimePresentationState& presentation);
  ~RuntimePluginUiCoordinator();

  RuntimePluginUiCoordinator(RuntimePluginUiCoordinator const&) = delete;
  RuntimePluginUiCoordinator& operator=(RuntimePluginUiCoordinator const&) = delete;

  // Attach once on the TUI thread before admission opens. The loop outlives
  // this owner; worker endpoints receive only its weak wake capability.
  void attach_loop(TuiEventLoop& loop);

  [[nodiscard]] std::optional<TuiPluginUiEndpoint> begin_submission(std::string_view submitted, std::string invocation_id,
                                                                    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  [[nodiscard]] TuiPluginUiPollResult poll(bool host_modal_conflict, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  // Main-thread geometry guard shared by resize, poll/render, and input dispatch.
  // Unfittable plugin surfaces invalidate their binding, resolve any modal as
  // Cancel, and are removed from the snapshot.
  [[nodiscard]] bool cancel_unfittable_surfaces();
  [[nodiscard]] TuiPluginUiInputResult handle_input(InputEvent const& event);
  [[nodiscard]] bool deadline_reached(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

  // Worker-safe cancellation queues a clear and resolves any synchronous modal.
  void cancel_active();
  // Main-thread terminal outcomes clear the dedicated ephemeral snapshot before
  // the worker future is joined or application state is applied.
  void finish_submission();
  void shutdown();

  // One-shot deterministic seams for enqueue/poll ordering tests. Production
  // callers never set them; enqueue hooks run after releasing the state lock.
  void set_after_enqueue_for_test(std::function<void()> hook);
  void set_after_queue_swap_for_test(std::function<void(std::size_t)> hook);

  // Plugin requests and the borrowed projection contain arbitrary user/plugin
  // text; generated printing must not expose those content-bearing aggregates.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  [[nodiscard]] bool clear_surface(std::optional<TuiPluginUiBinding> const& binding = std::nullopt);
  void expire_submission(std::chrono::steady_clock::time_point now);

  RuntimePresentationState& presentation_;
  // Worker endpoint calls retain shared synchronized state until their modal wait
  // ends. Endpoints themselves hold weak references and cannot extend admission.
  std::shared_ptr<RuntimePluginUiCoordinatorState> state_;
  // Shared identity token lets copied endpoints observe this runtime lifetime.
  std::shared_ptr<void const> runtime_token_;
  bool loop_attached_ = false;
  TuiTimer deadline_timer_;
};

}  // namespace ava::tui
