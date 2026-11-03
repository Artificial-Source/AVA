// Owns the TUI workspace selector and inspector projection; backend jobs retain their application owners.
#pragma once

#include "TuiTimer.h"
#include "ava/debug/print_members_on.h"
#include "ava/tui/runtime.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace ava::tui {

class RuntimePresentationState;

class SelectorHost;
class RuntimeRenderer;
class TuiEventLoop;

// Owned TUI-thread dispatch result returned to session routing; no borrowed data survives the synchronous
// handler.
struct RuntimeSubagentWorkspaceInputResult
{
  // Tell the dispatcher that workspace focus consumed this key even when its view did not change.
  bool handled = false;
  // Ask session rendering to repaint the updated workspace or selector projection.
  bool changed = false;
  // Request terminal feedback at the caller without giving this result terminal ownership.
  bool beep = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Session-owned TUI-thread selector/inspector presentation for application-owned jobs. Frontend, projection,
// selector host, attached renderer, and loop outlive it. Discovery runs only while open; close/destruction
// cancels it before releasing view data and never stops backend jobs.
class RuntimeSubagentWorkspaceController final
{
 public:
  // TUI-thread monotonic clock shared with discovery timer scheduling.
  using Clock = std::chrono::steady_clock;
  // While a workspace surface is open, refresh snapshots every 150 ms; closed mode leaves no discovery wake.
  static constexpr auto kPollInterval = std::chrono::milliseconds(150);

  // Frontend, projection and selector host outlive this TUI-owned presentation.
  // Destruction clears only workspace presentation; backend jobs keep their owner.
  RuntimeSubagentWorkspaceController(TuiFrontEnd const& frontend, RuntimePresentationState& presentation, SelectorHost& selectors);
  // TUI thread: close presentation and timer while borrowed host/projection still live.
  ~RuntimeSubagentWorkspaceController();
  // Keep one owner of workspace projection and discovery registration.
  RuntimeSubagentWorkspaceController(RuntimeSubagentWorkspaceController const&) = delete;
  // Do not replace lifetime-bound dependencies or duplicate the selector checkpoint.
  RuntimeSubagentWorkspaceController& operator=(RuntimeSubagentWorkspaceController const&) = delete;

  // Loop and renderer outlive this stable owner; attached polling only runs from
  // its member timer and requests invalidation without painting. Attach the
  // renderer's loop mode before attaching this feature.
  void attach_loop(TuiEventLoop& loop, RuntimeRenderer& renderer);
  // TUI thread: consume a failure latched by timer-driven invalidation so session control handles it once.
  [[nodiscard]] bool take_render_failure() noexcept;
  // TUI thread: initialize a fresh job list or unavailable placeholder, publish it, and start discovery
  // cadence.
  [[nodiscard]] bool open_selector(Clock::time_point now = Clock::now());
  // TUI thread while active: route selector/workspace actions and return changed/beep feedback; backend
  // callbacks are synchronous borrowed calls.
  [[nodiscard]] RuntimeSubagentWorkspaceInputResult handle_input(InputEvent const& event, Clock::time_point now = Clock::now());
  // Close presentation and polling admission while the borrowed host/snapshot live.
  void close();

  // TUI thread: report whether discovery and workspace-specific input routing remain admitted.
  [[nodiscard]] bool active() const noexcept;
  // TUI thread: distinguish the job chooser from inspector mode before routing list input.
  [[nodiscard]] bool selector_active() const noexcept;
  // TUI thread: distinguish the inspector surface for modal input precedence.
  [[nodiscard]] bool workspace_active() const noexcept;
  // TUI thread: borrow selected job identity until close/cycle/open mutates it or this controller dies.
  [[nodiscard]] std::string_view active_job_id() const noexcept;
  // TUI thread: copy last accepted inspector generation for conditional refresh; reset when selected job
  // changes.
  [[nodiscard]] std::optional<std::uint64_t> known_generation() const noexcept;

  // Workspace messages and selector checkpoints contain arbitrary user text;
  // the borrowed projection/renderer must not become generated debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI-thread exclusive surface state; publish ensures selector and inspector are never displayed together.
  enum class Mode
  {
    // No workspace input or periodic discovery is admitted.
    Closed,
    // SelectorHost owns the visible list's current query and selection.
    Selector,
    // Inspector owns visible messages while selector_ retains a return checkpoint.
    Workspace,
  };

  // TUI thread: preserve selected job identity/query while refreshing list facts; mark missing inspected jobs
  // evicted.
  [[nodiscard]] bool refresh_selector();
  // TUI thread: apply generation-aware inspector frames; retain prior content on unavailability and follow tail
  // only if already there.
  [[nodiscard]] bool refresh_inspection();
  // TUI thread: validate enabled row, reset job generation, inspect it, then replace selector with workspace
  // projection.
  [[nodiscard]] bool open_selected_workspace();
  // TUI thread: cycle only enabled rows and reuse open_selected_workspace so old generation/scroll cannot leak
  // to another job.
  [[nodiscard]] bool cycle_workspace(bool forward);
  // TUI thread: apply signed movement through clamping and publish only if visible scroll changes.
  [[nodiscard]] bool scroll_workspace(std::ptrdiff_t delta);
  // TUI thread: clamp against current screen geometry before publishing inspector scroll.
  [[nodiscard]] bool set_workspace_scroll(std::size_t offset);
  // TUI thread: retire inspected-job state and restore list checkpoint while leaving discovery active.
  void close_to_selector();
  // TUI thread: enforce mutually exclusive surfaces and clamp their selections/scroll before named projection
  // publication.
  void publish();
  // TUI thread discovery: refresh list first, inspect only after a successful list read, then publish once if
  // changed.
  [[nodiscard]] bool refresh();
  // TUI timer callback: refresh presentation and request invalidation; latch renderer failure without painting
  // inline.
  void discover_progress(Clock::time_point now);

  // Borrowed application adapter outlives the controller; supplies path-free job snapshots and explicit job
  // actions on the TUI thread.
  TuiFrontEnd const& frontend_;
  // Borrowed projection owner outlives the controller; close/destruction publishes surface retirement here.
  RuntimePresentationState& presentation_;
  // The session-owned host outlives this controller.
  // Borrowed session host outlives this owner; while chooser is visible, it alone owns query and current
  // selection.
  SelectorHost& selectors_;
  // TUI-thread surface authority changed by open/close/cycle transitions and consumed by input/discovery.
  Mode mode_ = Mode::Closed;
  // Rebuild/checkpoint data for returning from workspace mode. While the selector
  // is visible, the host alone owns its current query and selection.
  // TUI-thread owned list checkpoint; refreshed from SelectorHost before leaving chooser or reconciling backend
  // rows.
  SelectListView selector_;
  // TUI-thread owned inspector content and scroll; reset whenever selected job changes or workspace closes.
  SubagentWorkspaceView workspace_;
  // TUI-thread selected opaque backend job identity; empty outside inspector mode.
  std::string job_id_;
  // TUI-thread most recent nonzero inspector generation; permits not-modified replies without replacing
  // messages.
  std::optional<std::uint64_t> known_generation_ = std::nullopt;
  // TUI-thread local read-failure indicator combined with backend frame status at publication; success clears
  // it.
  bool refresh_unavailable_ = false;
  // TUI-thread guard preventing inspection against an unavailable latest list; reset on fresh open/close.
  bool last_list_refresh_succeeded_ = true;
  // Borrowed after attach_loop; renderer must be attached first and outlive controller/timer callbacks.
  RuntimeRenderer* renderer_ = nullptr;
  // TUI timer sets when invalidation fails; application stage consumes through take_render_failure.
  bool render_failed_ = false;
  // TUI-thread periodic refresh registration; canceled on close and destroyed before borrowed owners can
  // retire.
  TuiTimer discovery_timer_;
};

}  // namespace ava::tui
