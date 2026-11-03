// Adapts synchronized plugin transport to session-owned TUI surfaces and timers.
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
class RuntimePluginUiCoordinatorState;
class TuiEventLoop;

// TUI-call-owned summary consumed by ActiveRun to request paint or cooperative stop.
// It carries no borrowed state and preserves independent redraw and expiry evidence.
struct TuiPluginUiPollResult
{
  // TUI poll reports projection mutation so its caller can request a fresh frame.
  bool changed = false;
  // One poll consumes each transport expiry report; ActiveRun then requests stop.
  bool deadline_expired = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// TUI input arbitration result; callers own painting and run-stop publication.
enum class TuiPluginUiInputResult
{
  Unhandled, // No plugin modal owns the key; ordinary routing may continue.
  Handled, // Modal consumed the key without requiring projection repaint.
  Redraw, // Selection/surface changed; the caller must request its next frame.
  RequestStop, // Plugin waits were canceled; ActiveRun must also stop backend work.
};

// Pure submission check: only a canonical direct /plugin run can receive a UI endpoint.
[[nodiscard]] bool is_direct_foreground_plugin_run_submission(std::string_view submitted);

// Session-owned TUI projection/timer owner. Workers call only cancel_active and
// deadline_reached here, or weak endpoints into the private synchronized state.
// The projection and attached loop outlive this stable owner. Shutdown closes
// state admission before releasing its token; in-flight calls pin transport only.
class RuntimePluginUiCoordinator final
{
 public:
  // TUI-thread owner; the borrowed projection outlives this coordinator.
  explicit RuntimePluginUiCoordinator(RuntimePresentationState& presentation);
  // TUI thread: close transport and clear surfaces before its members are destroyed.
  // Retained endpoints may survive; their weak locks can only reach closed transport.
  ~RuntimePluginUiCoordinator();

  // One TUI owner controls the deadline registration and projection for shared transport.
  RuntimePluginUiCoordinator(RuntimePluginUiCoordinator const&) = delete;
  // Replacing transport would invalidate timer targets and current endpoint identity.
  RuntimePluginUiCoordinator& operator=(RuntimePluginUiCoordinator const&) = delete;

  // Attach once on the TUI thread before admission opens. The loop outlives
  // this owner; worker endpoints receive only its weak wake capability.
  void attach_loop(TuiEventLoop& loop);

  // TUI thread: admit only a canonical direct plugin command with invocation identity.
  // Returned endpoints bind this generation and deadline but retain no projection owner.
  [[nodiscard]] std::optional<TuiPluginUiEndpoint> begin_submission(std::string_view submitted, std::string invocation_id,
                                                                    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  // TUI thread: validate geometry, consume a finite transport batch, then retire the
  // timer if admission ended. Host conflicts reject modal projection, not dock updates.
  [[nodiscard]] TuiPluginUiPollResult poll(bool host_modal_conflict, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
  // Main-thread geometry guard shared by resize, poll/render, and input dispatch.
  // Unfittable plugin surfaces invalidate their binding, resolve any modal as
  // Cancel, and are removed from the snapshot.
  [[nodiscard]] bool cancel_unfittable_surfaces();
  // TUI thread: revalidate fit before using buffered navigation/answer input. Modal
  // resolution transfers a reply to transport; callers handle the returned paint/stop work.
  [[nodiscard]] TuiPluginUiInputResult handle_input(InputEvent const& event);
  // Either thread: synchronized deadline observation for cooperative backend cancellation.
  // Calling does not mutate projection or dispatch the TUI timer.
  [[nodiscard]] bool deadline_reached(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

  // Either thread: close admission and release waits without allocation. A flag
  // asks the next TUI poll to clear surfaces; this call never touches projection.
  void cancel_active();
  // Main-thread terminal outcomes clear the dedicated ephemeral snapshot before
  // the worker future is joined or application state is applied.
  void finish_submission();
  // TUI thread: cancel deadline, permanently close transport, release runtime identity
  // and remove ephemeral surfaces before destruction.
  void shutdown();

  // One-shot deterministic seams for enqueue/poll ordering tests. Production
  // callers never set them; enqueue hooks run after releasing the state lock.
  void set_after_enqueue_for_test(std::function<void()> hook);
  // TUI test setup: replace the one-shot callback observing a detached poll batch;
  // it runs outside the state lock so tests can arrange cancellation races.
  void set_after_queue_swap_for_test(std::function<void(std::size_t)> hook);

  // Plugin requests and the borrowed projection contain arbitrary user/plugin
  // text; generated printing must not expose those content-bearing aggregates.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Session borrow accessed only by TUI methods; endpoint lambdas never capture it.
  RuntimePresentationState& presentation_;
  // Worker endpoint calls retain shared synchronized state until their modal wait
  // ends. Endpoints themselves hold weak references and cannot extend admission.
  std::shared_ptr<RuntimePluginUiCoordinatorState> state_;
  // Coordinator's shared identity is exposed weakly in endpoints. Reset at shutdown
  // invalidates lifetime checks without extending this owner's projection lifetime.
  std::shared_ptr<void const> runtime_token_;
  // TUI-only attachment state selects registered expiry versus explicit-clock fixture polling.
  bool loop_attached_ = false;
  // TUI registration targets the retained state; cancel before shutdown releases state.
  TuiTimer deadline_timer_;
};

}  // namespace ava::tui
