#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/terminal.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tui {

struct RuntimeDraftState
{
  [[nodiscard]] std::size_t max_draft_scroll_offset(ComposerSnapshot const& snapshot, std::size_t height) const;

  void clear_selection();
  void reset_for_session_transition();
  [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> selection_bounds() const;
  bool replace_selection(std::string_view replacement);
  bool delete_selection();
  [[nodiscard]] std::optional<std::string> selected_text() const;
  bool copy_selection(ComposerSnapshot& snapshot);
  void extend_selection(TuiAction movement, ComposerSnapshot& snapshot);
  void extend_selection_to(std::size_t target, ComposerSnapshot& snapshot);
  bool extend_selection_for_key(terminal::Key key, ComposerSnapshot& snapshot);
  void insert_newline();
  bool convert_backslash_enter_to_newline(ComposerSnapshot& snapshot);

  std::vector<std::string> input_history;
  std::optional<std::size_t> history_index;
  std::string draft_input;
  ComposerDraftState draft;
  ComposerJumpMode jump_mode = ComposerJumpMode::None;
  std::size_t selected_slash_command_index = 0;
  bool slash_palette_suppressed = false;
  bool path_completion_force_active = false;
  std::size_t draft_scroll_offset = 0;
  std::size_t draft_selection_anchor = std::string::npos;
  std::size_t draft_selection_cursor = std::string::npos;
  bool mouse_selecting = false;
  bool pending_escape_clear = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::tui
