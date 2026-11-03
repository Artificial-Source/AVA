#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/terminal.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tui {
namespace runtime_input {
struct RuntimeInput;
}

class RuntimeNavigationController;
class PromptStash;
enum class PromptStashRestoreResult;
namespace terminal {
class Context;
}

// Idle and active-run routing historically reset different interaction state.
// Keep that policy explicit while sharing the editor operations.
enum class ComposerEditContext
{
  Idle,
  ActiveRun
};
enum class ComposerCompletionOrigin
{
  Keyboard,
  Pointer,
  PointerPath,
  Forced
};

class Composer
{
 public:
  [[nodiscard]] ComposerDraftState const& draft() const noexcept;
  [[nodiscard]] std::vector<std::string> const& history() const noexcept;
  [[nodiscard]] std::optional<std::size_t> history_index() const noexcept;
  [[nodiscard]] std::string const& saved_draft() const noexcept;
  [[nodiscard]] ComposerJumpMode jump_mode() const noexcept;
  [[nodiscard]] std::size_t completion_index() const noexcept;
  [[nodiscard]] bool palette_suppressed() const noexcept;
  [[nodiscard]] bool path_completion_forced() const noexcept;
  [[nodiscard]] std::size_t scroll_offset() const noexcept;
  [[nodiscard]] bool mouse_selecting() const noexcept;
  [[nodiscard]] bool escape_pending() const noexcept;

  void cancel_escape();
  void reset_interaction();
  // Submission preserves palette suppression and Escape; local commands also
  // reveal completion, while queue submission preserves forced path completion.
  void reset_after_submit();
  void reset_local_command();
  void reset_after_queue();
  void reset_for_session_transition();
  void reset_draft();
  // Replace editor content without silently changing history/interaction state.
  bool replace_draft(std::string text, std::size_t cursor = std::string::npos);
  void restore_draft(std::string text);
  void draft_command(std::string text);
  void record_submission(std::string text);
  // Apply the editor action alone after a caller-selected lifecycle reset.
  bool apply_action(TuiAction action);
  bool apply_edit(TuiAction action, ComposerEditContext context = ComposerEditContext::Idle);
  bool move_vertical(TuiAction action);
  bool browse_history(bool previous);
  void insert_input(runtime_input::RuntimeInput const& input, ComposerSnapshot& snapshot, ComposerEditContext context = ComposerEditContext::Idle);

  void select_slash_command(ComposerSnapshot& snapshot, terminal::Context& context);
  void select_file_reference(RuntimeNavigationController& navigation, ComposerSnapshot& snapshot, terminal::Context& context);
  void select_path_completion(RuntimeNavigationController& navigation, ComposerSnapshot& snapshot, terminal::Context& context);
  bool force_path_completion(RuntimeNavigationController& navigation, ComposerSnapshot& snapshot, terminal::Context& context,
                             ComposerEditContext edit_context = ComposerEditContext::Idle);
  PromptStashRestoreResult restore_stashed(PromptStash& stash, std::uint64_t id);
  void select_completion(std::size_t index);
  void reset_completion();
  void cancel_path_completion();
  void accept_completion(std::string text, std::size_t cursor, ComposerCompletionOrigin origin = ComposerCompletionOrigin::Keyboard);
  void dismiss_completion();
  void begin_jump(ComposerJumpMode mode);
  void cancel_jump();
  bool jump_to_character(std::string_view target);
  void move_document_boundary(bool end, ComposerEditContext context = ComposerEditContext::Idle);
  void begin_pointer_selection(std::size_t cursor, bool selecting, ComposerEditContext context = ComposerEditContext::Idle);
  void extend_pointer_selection(std::size_t cursor, ComposerEditContext context = ComposerEditContext::Idle);
  void end_pointer_selection();
  void handle_escape(ComposerSnapshot& snapshot);

  [[nodiscard]] std::size_t max_draft_scroll_offset(ComposerSnapshot const& snapshot, std::size_t height) const;
  void clamp_scroll(std::size_t maximum);
  void scroll_to(std::size_t offset, ComposerSnapshot const& snapshot, std::size_t height);
  // Rendering clamps the cursor and publishes selection; completion preparation
  // only publishes the fields needed by completion calculations.
  void project(ComposerSnapshot& snapshot);
  void project_completion(ComposerSnapshot& snapshot) const;
  void clear_selection();
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

  // Draft, history, selection and editor undo/kill buffers contain arbitrary user
  // text and must never be emitted by generated member printing.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void leave_history();
  void prepare_text_edit();
  void reset_cursor_metadata();
  std::vector<std::string> input_history_;
  std::optional<std::size_t> history_index_;
  std::string draft_input_;
  ComposerDraftState draft_;
  ComposerJumpMode jump_mode_ = ComposerJumpMode::None;
  std::size_t selected_slash_command_index_ = 0;
  bool slash_palette_suppressed_ = false;
  bool path_completion_force_active_ = false;
  std::size_t draft_scroll_offset_ = 0;
  std::size_t draft_selection_anchor_ = std::string::npos;
  std::size_t draft_selection_cursor_ = std::string::npos;
  bool mouse_selecting_ = false;
  bool pending_escape_clear_ = false;
};

} // namespace ava::tui
