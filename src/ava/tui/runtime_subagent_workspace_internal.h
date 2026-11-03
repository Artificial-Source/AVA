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

struct RuntimeSubagentWorkspaceInputResult
{
  bool handled = false;
  bool changed = false;
  bool beep = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class RuntimeSubagentWorkspaceController final
{
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kPollInterval = std::chrono::milliseconds(150);

  // Frontend, projection and selector host outlive this TUI-owned presentation.
  // Destruction clears only workspace presentation; backend jobs keep their owner.
  RuntimeSubagentWorkspaceController(TuiFrontEnd const& frontend, RuntimePresentationState& presentation, SelectorHost& selectors);
  ~RuntimeSubagentWorkspaceController();
  RuntimeSubagentWorkspaceController(RuntimeSubagentWorkspaceController const&) = delete;
  RuntimeSubagentWorkspaceController& operator=(RuntimeSubagentWorkspaceController const&) = delete;

  // Loop and renderer outlive this stable owner; attached polling only runs from
  // its member timer and requests invalidation without painting. Attach the
  // renderer's loop mode before attaching this feature.
  void attach_loop(TuiEventLoop& loop, RuntimeRenderer& renderer);
  [[nodiscard]] bool take_render_failure() noexcept;
  [[nodiscard]] bool open_selector(Clock::time_point now = Clock::now());
  [[nodiscard]] RuntimeSubagentWorkspaceInputResult handle_input(InputEvent const& event, Clock::time_point now = Clock::now());
  // Close presentation and polling admission while the borrowed host/snapshot live.
  void close();

  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool selector_active() const noexcept;
  [[nodiscard]] bool workspace_active() const noexcept;
  [[nodiscard]] std::string_view active_job_id() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> known_generation() const noexcept;

  // Workspace messages and selector checkpoints contain arbitrary user text;
  // the borrowed projection/renderer must not become generated debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  enum class Mode
  {
    Closed,
    Selector,
    Workspace,
  };

  [[nodiscard]] bool refresh_selector();
  [[nodiscard]] bool refresh_inspection();
  [[nodiscard]] bool open_selected_workspace();
  [[nodiscard]] bool cycle_workspace(bool forward);
  [[nodiscard]] bool scroll_workspace(std::ptrdiff_t delta);
  [[nodiscard]] bool set_workspace_scroll(std::size_t offset);
  void close_to_selector();
  void publish();
  [[nodiscard]] bool refresh();
  void discover_progress(Clock::time_point now);

  TuiFrontEnd const& frontend_;
  RuntimePresentationState& presentation_;
  // The session-owned host outlives this controller.
  SelectorHost& selectors_;
  Mode mode_ = Mode::Closed;
  // Rebuild/checkpoint data for returning from workspace mode. While the selector
  // is visible, the host alone owns its current query and selection.
  SelectListView selector_;
  SubagentWorkspaceView workspace_;
  std::string job_id_;
  std::optional<std::uint64_t> known_generation_ = std::nullopt;
  bool refresh_unavailable_ = false;
  bool last_list_refresh_succeeded_ = true;
  RuntimeRenderer* renderer_ = nullptr;
  bool render_failed_ = false;
  TuiTimer discovery_timer_;
};

}  // namespace ava::tui
