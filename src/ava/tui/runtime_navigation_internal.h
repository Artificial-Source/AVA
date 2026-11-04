// Routes user navigation through the renderer and updates visible status and editor data.
#pragma once

#include "ComposerInputResult.h"
#include "ava/debug/print_members_on.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ava::tui {

class RuntimePresentationState;

struct ComposerSnapshot;
struct FileReferenceSelectionText;
struct InputEvent;
class Composer;
class TuiFrontEnd;
class RuntimeRenderer;

namespace detail {
struct CompletionMatchModel;
} // namespace detail

// Session-owned TUI-thread navigation policy shared by idle and active input. It borrows frontend,
// presentation, Composer, and renderer for its entire lifetime; all must outlive it. It owns no background
// work, and viewport changes always go through renderer transitions.
class RuntimeNavigationController final
{
 public:
  // TUI thread: bind already constructed owners; callbacks passed during pointer handling are synchronous and
  // retain no controller reference.
  RuntimeNavigationController(TuiFrontEnd const& frontend, RuntimePresentationState& presentation, Composer& draft_state, RuntimeRenderer& renderer);
  // Not copyable: one controller routes navigation for the borrowed session editor and renderer.
  RuntimeNavigationController(RuntimeNavigationController const&) = delete;
  RuntimeNavigationController& operator=(RuntimeNavigationController const&) = delete;

  // TUI thread: project current Composer state before cache refresh; returned model expires on the renderer's
  // next refresh/paint.
  [[nodiscard]] detail::CompletionMatchModel const* refresh_completion_cache();
  // TUI thread: test current Composer text/suppression against the retained command catalog before routing
  // keys.
  [[nodiscard]] bool slash_palette_active() const;
  // TUI thread: refresh current completion matches before deciding whether file-reference input owns the key.
  [[nodiscard]] bool file_reference_palette_active();
  // TUI thread: refresh current completion matches before deciding whether path input owns the key.
  [[nodiscard]] bool path_completion_palette_active();
  // TUI thread: project current input before clamping selection against refreshed matches.
  [[nodiscard]] std::size_t clamp_completion(std::size_t selected);
  // TUI thread: return owned feedback for a disabled row after projecting current input.
  [[nodiscard]] std::optional<std::string> selected_completion_disabled_reason(std::size_t selected);
  // TUI thread: return owned insertion data after projecting current input; Composer applies the edit.
  [[nodiscard]] FileReferenceSelectionText selected_completion_text(std::size_t selected);

  // TUI thread: route pointer input through completion, transcript, then draft.
  // No value leaves non-pointer input to the next dispatcher stage. An owned
  // result consumes the pointer event even when a clicked completion declined
  // its edit. Session applies feedback and a bell before requesting paint.
  [[nodiscard]] std::optional<ComposerInputResult> handle_pointer_input(InputEvent const& event);

  // TUI thread: disarm consecutive Escape clearing, publish terminal dimensions, then ask the renderer to
  // scroll toward history.
  void scroll_up(std::size_t amount);
  // TUI thread: disarm consecutive Escape clearing and publish dimensions before scrolling toward live output.
  void scroll_down(std::size_t amount);
  // TUI thread: prepare current completion/editor geometry and thaw deferred layout before deriving a half-page
  // navigation step.
  [[nodiscard]] std::size_t transcript_page_size();
  // TUI thread: synchronize prior updates, validate live item, and preserve viewport through immediate
  // expansion/collapse.
  [[nodiscard]] bool toggle_tool_details_at(std::size_t item_index);
  // TUI thread: find a tool through presentation state and preserve its viewport anchor; absent means no
  // matching item.
  [[nodiscard]] std::optional<std::size_t> toggle_matching_tool_details(std::string_view query);
  // TUI thread: validate item and preserve viewport through width-dependent thinking expansion; report whether
  // a toggle occurred.
  [[nodiscard]] bool toggle_thinking_at(std::size_t item_index);
  // TUI thread: toggle the newest eligible thinking item through presentation state, preserving viewport and
  // publishing feedback.
  [[nodiscard]] std::optional<std::size_t> toggle_latest_thinking_details();

  // TUI thread: yield drawer input to higher-priority permission/question/selector surfaces.
  [[nodiscard]] bool sidebar_drawer_focused() const;
  // TUI thread: retire drawer projection and Escape clear chain, then publish close feedback.
  void close_sidebar_drawer();
  // TUI thread: compute drawable drawer rows using current terminal size without changing stored presentation.
  [[nodiscard]] std::size_t sidebar_drawer_page_size() const;
  // TUI thread: nullopt lets later handlers run; an engaged bool reports handled input and whether frame
  // invalidation succeeded.
  [[nodiscard]] std::optional<bool> handle_sidebar_drawer_input(InputEvent const& event);

  // TUI thread: perform explicit live-tail cleanup, show position indicator, and publish caller-owned
  // navigation feedback.
  void jump_to_bottom(std::string status);
  // TUI thread: resolve a retained item in synchronized layout before navigating; missing items only publish
  // feedback.
  void jump_to_transcript_item(std::size_t item_index, std::string status);
  // TUI thread: collect retained user-turn starts in the synchronized transcript layout and move before/after the current
  // viewport boundary.
  void scroll_to_message_boundary(bool previous);

  // Generated printing uses addresses for borrowed owners; it never traverses their content.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // TUI thread: publish editor facts before any helper reads completion geometry; the renderer reads the
  // resulting screen data.
  void project_completion_input();
  // Borrowed session frontend outlives navigation; TUI-thread handlers read bindings without taking backend
  // ownership.
  TuiFrontEnd const& frontend_;
  // Borrowed TUI-thread presentation state outlives navigation; its methods update status and feature
  // display data.
  RuntimePresentationState& presentation_;
  // Borrowed editor outlives navigation; pointer/input transitions disarm Escape state and preserve one
  // selection owner.
  Composer& draft_state_;
  // Borrowed renderer outlives navigation; it alone coordinates viewport mutations, layout preparation, and
  // timers.
  RuntimeRenderer& renderer_;
};

}  // namespace ava::tui
