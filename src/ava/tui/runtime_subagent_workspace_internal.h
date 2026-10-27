#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/runtime.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>

namespace ava::tui {

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

  RuntimeSubagentWorkspaceController(TuiFrontEnd const& frontend, ComposerSnapshot& snapshot);
  RuntimeSubagentWorkspaceController(RuntimeSubagentWorkspaceController const&) = delete;
  RuntimeSubagentWorkspaceController& operator=(RuntimeSubagentWorkspaceController const&) = delete;

  [[nodiscard]] bool open_selector(Clock::time_point now = Clock::now());
  [[nodiscard]] bool poll(Clock::time_point now = Clock::now());
  [[nodiscard]] std::optional<Clock::duration> time_until_poll(Clock::time_point now = Clock::now()) const;
  [[nodiscard]] RuntimeSubagentWorkspaceInputResult handle_input(InputEvent const& event, Clock::time_point now = Clock::now());
  void reset_for_session_transition();

  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool selector_active() const noexcept;
  [[nodiscard]] bool workspace_active() const noexcept;
  [[nodiscard]] std::string_view active_job_id() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> known_generation() const noexcept;

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
  void close_to_parent();
  void close_to_selector();
  void publish();

  TuiFrontEnd const& frontend_;
  ComposerSnapshot& snapshot_;
  Mode mode_ = Mode::Closed;
  SelectListView selector_;
  SubagentWorkspaceView workspace_;
  std::string job_id_;
  std::optional<std::uint64_t> known_generation_ = std::nullopt;
  bool refresh_unavailable_ = false;
  bool last_list_refresh_succeeded_ = true;
  Clock::time_point next_poll_ = Clock::time_point::max();
};

}  // namespace ava::tui
