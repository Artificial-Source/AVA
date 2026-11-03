#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include "debug.h"

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

class RuntimeNavigationController final
{
 public:
  RuntimeNavigationController(TuiFrontEnd const& frontend, RuntimePresentationState& presentation, Composer& draft_state, RuntimeRenderer& renderer);
  RuntimeNavigationController(RuntimeNavigationController const&) = delete;
  RuntimeNavigationController& operator=(RuntimeNavigationController const&) = delete;

  [[nodiscard]] detail::CompletionMatchModel const* refresh_completion_cache();
  [[nodiscard]] bool slash_palette_active() const;
  [[nodiscard]] bool file_reference_palette_active();
  [[nodiscard]] bool path_completion_palette_active();
  [[nodiscard]] std::size_t completion_match_count();
  [[nodiscard]] std::size_t clamp_completion(std::size_t selected);
  [[nodiscard]] std::size_t previous_completion(std::size_t selected);
  [[nodiscard]] std::size_t next_completion(std::size_t selected);
  [[nodiscard]] std::optional<std::string> selected_completion_disabled_reason(std::size_t selected);
  [[nodiscard]] FileReferenceSelectionText selected_completion_text(std::size_t selected);

  void scroll_up(std::size_t amount);
  void scroll_down(std::size_t amount);
  [[nodiscard]] std::size_t transcript_page_size();
  [[nodiscard]] bool toggle_tool_details_at(std::size_t item_index);
  [[nodiscard]] std::optional<std::size_t> toggle_matching_tool_details(std::string_view query);
  [[nodiscard]] bool toggle_thinking_at(std::size_t item_index);
  [[nodiscard]] std::optional<std::size_t> toggle_latest_thinking_details();

  [[nodiscard]] bool sidebar_drawer_focused() const;
  void close_sidebar_drawer();
  [[nodiscard]] std::size_t sidebar_drawer_page_size() const;
  [[nodiscard]] std::optional<bool> handle_sidebar_drawer_input(InputEvent const& event);

  void jump_to_bottom(std::string status);
  void jump_to_transcript_item(std::size_t item_index, std::string status);
  void scroll_to_message_boundary(bool previous);

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void project_completion_input();
  TuiFrontEnd const& frontend_;
  RuntimePresentationState& presentation_;
  Composer& draft_state_;
  RuntimeRenderer& renderer_;
};

}  // namespace ava::tui
