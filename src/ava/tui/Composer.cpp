#include "sys.h"
#include "ava/tui/Composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/terminal/Context.h"
#include "ava/core/Application.h"

#include <algorithm>

namespace ava::tui {

void Composer::select_slash_command(ComposerSnapshot& snapshot, terminal::Context& context)
{
  selected_slash_command_index_ = clamp_slash_palette_selection(draft_.text, draft_.cursor, snapshot.slash_commands, selected_slash_command_index_);
  if (auto const reason = slash_command_selection_disabled_reason(draft_.text, draft_.cursor, snapshot.slash_commands, selected_slash_command_index_))
  {
    snapshot.status = "command disabled: " + *reason;
    context.beep();
    return;
  }
  auto selection = slash_command_selection_text(draft_.text, draft_.cursor, snapshot.slash_commands, selected_slash_command_index_);
  accept_completion(std::move(selection.text), selection.cursor);
  snapshot.status = "command selected - press Enter to run";
}
void Composer::select_file_reference(RuntimeNavigationController& navigation, ComposerSnapshot& snapshot, terminal::Context& context)
{
  selected_slash_command_index_ = navigation.clamp_completion(selected_slash_command_index_);
  if (auto const reason = navigation.selected_completion_disabled_reason(selected_slash_command_index_))
  {
    snapshot.status = "reference disabled: " + *reason;
    context.beep();
    return;
  }
  auto selection = navigation.selected_completion_text(selected_slash_command_index_);
  accept_completion(std::move(selection.text), selection.cursor);
  snapshot.status = "file reference selected";
}
void Composer::select_path_completion(RuntimeNavigationController& navigation, ComposerSnapshot& snapshot, terminal::Context& context)
{
  selected_slash_command_index_ = navigation.clamp_completion(selected_slash_command_index_);
  if (auto const reason = navigation.selected_completion_disabled_reason(selected_slash_command_index_))
  {
    snapshot.status = "path disabled: " + *reason;
    context.beep();
    return;
  }
  auto selection = navigation.selected_completion_text(selected_slash_command_index_);
  accept_completion(std::move(selection.text), selection.cursor);
  snapshot.status = "path selected";
}
bool Composer::force_path_completion(RuntimeNavigationController& navigation, ComposerSnapshot& snapshot, terminal::Context& context,
                                     ComposerEditContext edit_context)
{
  auto const was_suppressed = slash_palette_suppressed_;
  slash_palette_suppressed_ = false;
  path_completion_force_active_ = true;
  auto const match_count = navigation.completion_match_count();
  if (match_count == 0)
  {
    slash_palette_suppressed_ = was_suppressed;
    path_completion_force_active_ = false;
    return false;
  }
  if (edit_context == ComposerEditContext::Idle)
  {
    cancel_escape();
    leave_history();
    selected_slash_command_index_ = 0;
  }
  if (match_count == 1)
  {
    if (auto const reason = navigation.selected_completion_disabled_reason(0))
    {
      path_completion_force_active_ = false;
      snapshot.status = "path disabled: " + *reason;
      context.beep();
      return true;
    }
    auto selection = navigation.selected_completion_text(0);
    accept_completion(std::move(selection.text), selection.cursor, ComposerCompletionOrigin::Forced);
    snapshot.status = "path selected";
    return true;
  }
  if (edit_context == ComposerEditContext::Idle)
    draft_scroll_offset_ = 0;
  else
    selected_slash_command_index_ = 0;
  snapshot.status = "path suggestions";
  return true;
}

ComposerDraftState const& Composer::draft() const noexcept
{
  return draft_;
}
std::vector<std::string> const& Composer::history() const noexcept
{
  return input_history_;
}
std::optional<std::size_t> Composer::history_index() const noexcept
{
  return history_index_;
}
std::string const& Composer::saved_draft() const noexcept
{
  return draft_input_;
}
ComposerJumpMode Composer::jump_mode() const noexcept
{
  return jump_mode_;
}
std::size_t Composer::completion_index() const noexcept
{
  return selected_slash_command_index_;
}
bool Composer::palette_suppressed() const noexcept
{
  return slash_palette_suppressed_;
}
bool Composer::path_completion_forced() const noexcept
{
  return path_completion_force_active_;
}
std::size_t Composer::scroll_offset() const noexcept
{
  return draft_scroll_offset_;
}
bool Composer::mouse_selecting() const noexcept
{
  return mouse_selecting_;
}
bool Composer::escape_pending() const noexcept
{
  return pending_escape_clear_;
}

void Composer::cancel_escape()
{
  pending_escape_clear_ = false;
}
void Composer::leave_history()
{
  history_index_.reset();
  draft_input_.clear();
}
void Composer::prepare_text_edit()
{
  cancel_escape();
  leave_history();
  selected_slash_command_index_ = 0;
  slash_palette_suppressed_ = false;
  draft_scroll_offset_ = 0;
}
void Composer::reset_interaction()
{
  prepare_text_edit();
  jump_mode_ = ComposerJumpMode::None;
  path_completion_force_active_ = false;
  clear_selection();
}
void Composer::reset_after_submit()
{
  clear_selection();
  reset_draft();
  jump_mode_ = ComposerJumpMode::None;
  draft_scroll_offset_ = 0;
  leave_history();
  reset_completion();
}
void Composer::reset_local_command()
{
  reset_after_submit();
  slash_palette_suppressed_ = false;
}
void Composer::reset_after_queue()
{
  auto const forced = path_completion_force_active_;
  reset_local_command();
  path_completion_force_active_ = forced;
}
void Composer::reset_draft()
{
  reset_composer_draft(draft_);
}
bool Composer::replace_draft(std::string text, std::size_t cursor)
{
  return replace_composer_draft(draft_, std::move(text), cursor);
}
void Composer::restore_draft(std::string text)
{
  clear_selection();
  static_cast<void>(replace_draft(std::move(text)));
  draft_scroll_offset_ = 0;
  leave_history();
  reset_completion();
  slash_palette_suppressed_ = false;
}
void Composer::draft_command(std::string text)
{
  clear_selection();
  static_cast<void>(replace_draft(std::move(text)));
  leave_history();
  draft_scroll_offset_ = 0;
}
void Composer::record_submission(std::string text)
{
  runtime_transcript::push_history(input_history_, std::move(text));
}
bool Composer::apply_action(TuiAction action)
{
  return apply_composer_draft_action(draft_, action);
}
bool Composer::apply_edit(TuiAction action, ComposerEditContext context)
{
  auto const deletion = action == TuiAction::DeleteBackward || action == TuiAction::DeleteForward || action == TuiAction::DeleteWordBackward ||
                        action == TuiAction::DeleteWordForward || action == TuiAction::DeleteToLineStart || action == TuiAction::DeleteToLineEnd;
  if (context == ComposerEditContext::ActiveRun || deletion || action == TuiAction::ClearInput)
    prepare_text_edit();
  else
  {
    cancel_escape();
    if (action == TuiAction::Undo || action == TuiAction::Redo || action == TuiAction::Yank || action == TuiAction::YankPop)
      draft_scroll_offset_ = 0;
  }
  if (context == ComposerEditContext::Idle && action == TuiAction::ClearInput)
    path_completion_force_active_ = false;
  if (deletion)
    return delete_selection() || apply_action(action);
  clear_selection();
  return apply_action(action);
}
bool Composer::move_vertical(TuiAction action)
{
  if (!apply_action(action))
    return false;
  cancel_escape();
  clear_selection();
  leave_history();
  selected_slash_command_index_ = 0;
  slash_palette_suppressed_ = false;
  return true;
}
bool Composer::browse_history(bool previous)
{
  cancel_escape();
  clear_selection();
  reset_completion();
  slash_palette_suppressed_ = false;
  draft_scroll_offset_ = 0;
  return browse_composer_input_history(draft_, input_history_, history_index_, draft_input_, previous);
}
void Composer::insert_input(runtime_input::RuntimeInput const& input, ComposerSnapshot& snapshot, ComposerEditContext context)
{
  prepare_text_edit();
  if (context == ComposerEditContext::ActiveRun)
    snapshot.local_command_feedback.reset();
  auto const text = input.text.empty() ? std::string(1, input.event.character) : input.text;
  if (input.bracketed_paste)
  {
    static_cast<void>(delete_selection());
    auto const inserted = insert_composer_paste_text(draft_, text);
    if (inserted || context == ComposerEditContext::ActiveRun)
      snapshot.status = "pasted into draft safely";
  }
  else if (!replace_selection(text))
    static_cast<void>(insert_composer_draft_text(draft_, text));
}
void Composer::select_completion(std::size_t index)
{
  selected_slash_command_index_ = index;
}
void Composer::reset_completion()
{
  selected_slash_command_index_ = 0;
  path_completion_force_active_ = false;
}
void Composer::cancel_path_completion()
{
  path_completion_force_active_ = false;
}
void Composer::accept_completion(std::string text, std::size_t cursor, ComposerCompletionOrigin origin)
{
  clear_selection();
  static_cast<void>(replace_draft(std::move(text), cursor));
  if (origin != ComposerCompletionOrigin::Forced)
    selected_slash_command_index_ = 0;
  if (origin != ComposerCompletionOrigin::Pointer)
    path_completion_force_active_ = false;
  if (origin == ComposerCompletionOrigin::Keyboard || origin == ComposerCompletionOrigin::Forced)
    draft_scroll_offset_ = 0;
  if (origin == ComposerCompletionOrigin::Keyboard)
    leave_history();
}
void Composer::dismiss_completion()
{
  cancel_escape();
  slash_palette_suppressed_ = true;
  reset_completion();
  leave_history();
}
void Composer::begin_jump(ComposerJumpMode mode)
{
  cancel_escape();
  leave_history();
  reset_completion();
  slash_palette_suppressed_ = false;
  jump_mode_ = mode;
}
void Composer::cancel_jump()
{
  jump_mode_ = ComposerJumpMode::None;
}
bool Composer::jump_to_character(std::string_view target)
{
  auto const forward = jump_mode_ == ComposerJumpMode::Forward;
  reset_interaction();
  return jump_composer_draft_to_character(draft_, target, forward);
}
void Composer::reset_cursor_metadata()
{
  draft_.vertical_column = std::string::npos;
  draft_.yank_start = std::string::npos;
  draft_.yank_end = std::string::npos;
}
void Composer::move_document_boundary(bool end, ComposerEditContext context)
{
  if (context == ComposerEditContext::ActiveRun)
  {
    prepare_text_edit();
    path_completion_force_active_ = false;
  }
  else
    cancel_escape();
  clear_selection();
  draft_.cursor = end ? draft_.text.size() : 0;
  reset_cursor_metadata();
}
void Composer::begin_pointer_selection(std::size_t cursor, bool selecting, ComposerEditContext context)
{
  draft_.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft_, cursor);
  clear_selection();
  if (selecting)
  {
    draft_selection_anchor_ = draft_.cursor;
    draft_selection_cursor_ = draft_.cursor;
    mouse_selecting_ = true;
  }
  if (context == ComposerEditContext::Idle)
  {
    reset_cursor_metadata();
    leave_history();
  }
}
void Composer::extend_pointer_selection(std::size_t cursor, ComposerEditContext context)
{
  draft_.cursor = clamp_composer_draft_cursor_to_atomic_boundary(draft_, cursor);
  draft_selection_cursor_ = draft_.cursor;
  if (context == ComposerEditContext::Idle)
  {
    reset_cursor_metadata();
    leave_history();
  }
}
void Composer::end_pointer_selection()
{
  mouse_selecting_ = false;
}
void Composer::handle_escape(ComposerSnapshot& snapshot)
{
  if (!draft_.text.empty())
  {
    if (pending_escape_clear_)
    {
      static_cast<void>(apply_action(TuiAction::ClearInput));
      reset_completion();
      leave_history();
      draft_scroll_offset_ = 0;
      cancel_escape();
      snapshot.status = "input cleared";
    }
    else
    {
      pending_escape_clear_ = true;
      snapshot.status = "press Esc again to clear";
    }
  }
  else
  {
    cancel_escape();
    snapshot.status = "escape ignored";
  }
}
void Composer::clamp_scroll(std::size_t maximum)
{
  draft_scroll_offset_ = std::min(draft_scroll_offset_, maximum);
}
void Composer::scroll_to(std::size_t offset, ComposerSnapshot const& snapshot, std::size_t height)
{
  draft_scroll_offset_ = std::min(offset, max_draft_scroll_offset(snapshot, height));
}
void Composer::project_completion(ComposerSnapshot& snapshot) const
{
  snapshot.input = draft_.text;
  snapshot.input_cursor = draft_.cursor;
  snapshot.selected_slash_command_index = selected_slash_command_index_;
  snapshot.slash_palette_suppressed = slash_palette_suppressed_;
  snapshot.path_completion_force_active = path_completion_force_active_;
}
void Composer::project(ComposerSnapshot& snapshot)
{
  draft_.cursor = clamp_composer_draft_cursor(draft_.text, draft_.cursor);
  project_completion(snapshot);
  auto const selection = selection_bounds();
  snapshot.input_selection_start = selection ? selection->first : std::string::npos;
  snapshot.input_selection_end = selection ? selection->second : std::string::npos;
}
PromptStashRestoreResult Composer::restore_stashed(PromptStash& stash, std::uint64_t id)
{
  return stash.restore(id, draft_);
}

std::size_t Composer::max_draft_scroll_offset(ComposerSnapshot const& snapshot, std::size_t height) const
{
  auto const main_width = composer_main_width(snapshot);
  auto const composer_lines = detail::composer_block_line_count(snapshot, height, main_width);
  auto const input_lines = detail::input_render_line_spans(draft_.text, main_width).size();
  auto const policy = detail::composer_layout_policy(snapshot, height);
  auto const layout = detail::composer_input_layout(input_lines, composer_lines, 0, policy.composer_top_padding_lines);
  return input_lines > layout.visible_input_lines ? input_lines - layout.visible_input_lines : std::size_t{0};
}

void Composer::clear_selection()
{
  draft_selection_anchor_ = std::string::npos;
  draft_selection_cursor_ = std::string::npos;
  mouse_selecting_ = false;
}

void Composer::reset_for_session_transition()
{
  clear_selection();
  reset_composer_draft(draft_);
  input_history_.clear();
  history_index_.reset();
  draft_input_.clear();
  jump_mode_ = ComposerJumpMode::None;
  selected_slash_command_index_ = 0;
  slash_palette_suppressed_ = false;
  path_completion_force_active_ = false;
  draft_scroll_offset_ = 0;
  pending_escape_clear_ = false;
}

std::optional<std::pair<std::size_t, std::size_t>> Composer::selection_bounds() const
{
  if (draft_selection_anchor_ == std::string::npos || draft_selection_cursor_ == std::string::npos)
    return std::nullopt;
  auto start = clamp_composer_draft_cursor(draft_.text, draft_selection_anchor_);
  auto end = clamp_composer_draft_cursor(draft_.text, draft_selection_cursor_);
  if (end < start)
    std::swap(start, end);
  if (start == end)
    return std::nullopt;
  return std::pair{start, end};
}

bool Composer::replace_selection(std::string_view replacement)
{
  auto const bounds = selection_bounds();
  if (!bounds)
    return false;
  auto const changed = replace_composer_draft_range(draft_, bounds->first, bounds->second, replacement);
  clear_selection();
  return changed;
}

bool Composer::delete_selection()
{
  return replace_selection(std::string_view{});
}

std::optional<std::string> Composer::selected_text() const
{
  auto const bounds = selection_bounds();
  if (!bounds || bounds->first >= bounds->second || bounds->second > draft_.text.size())
    return std::nullopt;
  return draft_.text.substr(bounds->first, bounds->second - bounds->first);
}

bool Composer::copy_selection(ComposerSnapshot& snapshot)
{
  auto const text = selected_text();
  if (!text || text->empty())
  {
    snapshot.status = "no selection to copy";
    ava::core::Application::instance().terminal_context().beep();
    return false;
  }
  auto const copied = runtime_transcript::copy_text_to_terminal_clipboard(*text);
  snapshot.status = copied ? "selection copy request sent" : "clipboard copy failed";
  if (!copied)
    ava::core::Application::instance().terminal_context().beep();
  return copied;
}

void Composer::extend_selection(TuiAction movement, ComposerSnapshot& snapshot)
{
  pending_escape_clear_ = false;
  history_index_.reset();
  draft_input_.clear();
  selected_slash_command_index_ = 0;
  slash_palette_suppressed_ = false;
  path_completion_force_active_ = false;
  draft_scroll_offset_ = 0;
  auto const anchor = draft_selection_anchor_ == std::string::npos ? clamp_composer_draft_cursor(draft_.text, draft_.cursor) : draft_selection_anchor_;
  static_cast<void>(apply_composer_draft_action(draft_, movement));
  draft_.cursor = clamp_composer_draft_cursor(draft_.text, draft_.cursor);
  draft_selection_anchor_ = anchor;
  draft_selection_cursor_ = draft_.cursor;
  snapshot.status = selection_bounds() ? "selection active" : "selection boundary";
}

void Composer::extend_selection_to(std::size_t target, ComposerSnapshot& snapshot)
{
  pending_escape_clear_ = false;
  history_index_.reset();
  draft_input_.clear();
  selected_slash_command_index_ = 0;
  slash_palette_suppressed_ = false;
  path_completion_force_active_ = false;
  draft_scroll_offset_ = 0;
  auto const anchor = draft_selection_anchor_ == std::string::npos ? clamp_composer_draft_cursor(draft_.text, draft_.cursor) : draft_selection_anchor_;
  draft_.cursor = clamp_composer_draft_cursor(draft_.text, target);
  draft_.vertical_column = std::string::npos;
  draft_.yank_start = std::string::npos;
  draft_.yank_end = std::string::npos;
  draft_selection_anchor_ = anchor;
  draft_selection_cursor_ = draft_.cursor;
  snapshot.status = selection_bounds() ? "selection active" : "selection boundary";
}

bool Composer::extend_selection_for_key(terminal::Key key, ComposerSnapshot& snapshot)
{
  using Key = terminal::Key;
  switch (key)
  {
    case Key::ShiftArrowUp:
      extend_selection(TuiAction::CursorUp, snapshot);
      return true;
    case Key::ShiftArrowDown:
      extend_selection(TuiAction::CursorDown, snapshot);
      return true;
    case Key::ShiftArrowLeft:
      extend_selection(TuiAction::CursorLeft, snapshot);
      return true;
    case Key::ShiftArrowRight:
      extend_selection(TuiAction::CursorRight, snapshot);
      return true;
    case Key::ShiftCtrlArrowLeft:
    case Key::ShiftAltArrowLeft:
      extend_selection(TuiAction::CursorWordLeft, snapshot);
      return true;
    case Key::ShiftCtrlArrowRight:
    case Key::ShiftAltArrowRight:
      extend_selection(TuiAction::CursorWordRight, snapshot);
      return true;
    case Key::ShiftHome:
      extend_selection(TuiAction::CursorLineStart, snapshot);
      return true;
    case Key::ShiftEnd:
      extend_selection(TuiAction::CursorLineEnd, snapshot);
      return true;
    case Key::ShiftCtrlHome:
      extend_selection_to(0, snapshot);
      return true;
    case Key::ShiftCtrlEnd:
      extend_selection_to(draft_.text.size(), snapshot);
      return true;
    default:
      return false;
  }
}

void Composer::insert_newline()
{
  pending_escape_clear_ = false;
  history_index_.reset();
  draft_input_.clear();
  selected_slash_command_index_ = 0;
  path_completion_force_active_ = false;
  draft_scroll_offset_ = 0;
  if (!replace_selection("\n"))
    static_cast<void>(insert_composer_draft_text(draft_, "\n"));
}

bool Composer::convert_backslash_enter_to_newline(ComposerSnapshot& snapshot)
{
  if (selection_bounds())
    return false;
  if (!replace_composer_backslash_before_cursor_with_newline(draft_))
    return false;
  pending_escape_clear_ = false;
  history_index_.reset();
  draft_input_.clear();
  selected_slash_command_index_ = 0;
  slash_palette_suppressed_ = false;
  path_completion_force_active_ = false;
  draft_scroll_offset_ = 0;
  snapshot.status = "newline inserted";
  return true;
}

}  // namespace ava::tui
