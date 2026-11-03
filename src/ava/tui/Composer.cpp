#include "sys.h"
#include "ava/tui/Composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/terminal/Context.h"
#include "ava/core/Application.h"

#include <algorithm>

namespace ava::tui {

ComposerInputResult Composer::handle_key(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings, ComposerSnapshot const& snapshot,
                                         detail::CompletionMatchCache const& completions, bool transcript_selected, ComposerInputPhase phase)
{
  using Key = terminal::Key;
  auto is_action = [&](TuiAction action) { return key_matches_action(bindings, action, input.event.key); };
  ComposerInputResult result;
  if (phase == ComposerInputPhase::Jump)
  {
    if (jump_mode() == ComposerJumpMode::None)
      return {};
    if (is_action(TuiAction::JumpForward) || is_action(TuiAction::JumpBackward))
    {
      cancel_jump();
      result.status = "jump cancelled";
    }
    else if (auto const target = runtime_input::printable_jump_target(input))
    {
      bool const forward = jump_mode() == ComposerJumpMode::Forward;
      result.status = jump_to_character(*target) ? (forward ? "jumped forward" : "jumped backward") : "jump character not found";
    }
    else
      cancel_jump();
    result.handled =
        input.event.key == Key::Character || input.event.key == Key::Space || is_action(TuiAction::JumpForward) || is_action(TuiAction::JumpBackward);
    return result;
  }
  if (phase == ComposerInputPhase::Primary)
  {
    if (input.event.key == Key::Character)
      return insert_input(input);
    if (is_action(TuiAction::MessageFollowUp))
      return {.handled = true, .request = ComposerInputRequest::Submit};
    if (!is_action(TuiAction::NewLine))
      return {};
    insert_newline();
    return {.handled = true};
  }
  // Editor runs before session Interrupt, so ClearInput can consume a shared key.
  // Navigation runs after transcript keys, preserving their existing precedence.
  if (phase == ComposerInputPhase::Editor)
  {
    auto const& event = input.event;
    bool const ctrl_d_delete_forward = event.key == Key::CtrlD && is_action(TuiAction::DeleteForward) && !draft().text.empty();
    bool const delete_forward_action = is_action(TuiAction::DeleteForward) && (event.key != Key::CtrlD || ctrl_d_delete_forward);
    if (is_action(TuiAction::JumpForward) || is_action(TuiAction::JumpBackward))
    {
      begin_jump(is_action(TuiAction::JumpForward) ? ComposerJumpMode::Forward : ComposerJumpMode::Backward);
      result.status = is_action(TuiAction::JumpForward) ? "jump forward: type character" : "jump backward: type character";
    }
    else if (is_action(TuiAction::DeleteBackward))
    {
      static_cast<void>(apply_edit(TuiAction::DeleteBackward));
    }
    else if (delete_forward_action)
    {
      static_cast<void>(apply_edit(TuiAction::DeleteForward));
    }
    else if (is_action(TuiAction::DeleteWordBackward))
    {
      static_cast<void>(apply_edit(TuiAction::DeleteWordBackward));
    }
    else if (is_action(TuiAction::DeleteWordForward))
    {
      static_cast<void>(apply_edit(TuiAction::DeleteWordForward));
    }
    else if (is_action(TuiAction::DeleteToLineStart))
    {
      static_cast<void>(apply_edit(TuiAction::DeleteToLineStart));
    }
    else if (is_action(TuiAction::DeleteToLineEnd))
    {
      static_cast<void>(apply_edit(TuiAction::DeleteToLineEnd));
    }
    else if (is_action(TuiAction::CopySelection) && selection_bounds())
    {
      cancel_escape();
      cancel_path_completion();
      result = copy_selection();
    }
    else if (is_action(TuiAction::CopySelection) && transcript_selected)
    {
      cancel_escape();
      cancel_path_completion();
      result.request = ComposerInputRequest::CopyTranscript;
    }
    else if (is_action(TuiAction::ClearInput) && (!draft().text.empty() || !is_action(TuiAction::Interrupt)))
    {
      result.status = apply_edit(TuiAction::ClearInput) ? "input cleared" : "input already empty";
    }
    else if (is_action(TuiAction::AutocompleteAccept) && (!palette_suppressed() && slash_palette_visible(draft_.text, draft_.cursor, snapshot.slash_commands)))
    {
      cancel_escape();
      result = select_slash_command(snapshot);
    }
    else if (is_action(TuiAction::AutocompleteAccept) &&
             (completions.model && completions.model->surface == detail::CompletionSurface::FileReference && completions.model->palette_visible))
    {
      cancel_escape();
      result = select_file_reference(completions, snapshot);
    }
    else if (is_action(TuiAction::AutocompleteAccept) &&
             (completions.model && completions.model->surface == detail::CompletionSurface::PathCompletion && completions.model->palette_visible))
    {
      cancel_escape();
      result = select_path_completion(completions, snapshot);
    }
    else if (is_action(TuiAction::AutocompleteAccept) && (result = force_path_completion(snapshot)).handled)
    {
      cancel_escape();
    }
    else
      return {};
    result.handled = true;
    return result;
  }
  if (phase == ComposerInputPhase::Navigation)
  {
    auto const& event = input.event;
    if ((result = extend_selection_for_key(event.key)).handled)
    {
      result.request = ComposerInputRequest::ClearTranscriptSelection;
    }
    else if (event.key == Key::CtrlHome)
    {
      move_document_boundary(false);
    }
    else if (event.key == Key::CtrlEnd)
    {
      move_document_boundary(true);
    }
    else if (is_action(TuiAction::CursorLeft))
    {
      static_cast<void>(apply_edit(TuiAction::CursorLeft));
    }
    else if (is_action(TuiAction::CursorRight))
    {
      static_cast<void>(apply_edit(TuiAction::CursorRight));
    }
    else if (is_action(TuiAction::CursorLineStart))
    {
      static_cast<void>(apply_edit(TuiAction::CursorLineStart));
    }
    else if (is_action(TuiAction::CursorLineEnd))
    {
      static_cast<void>(apply_edit(TuiAction::CursorLineEnd));
    }
    else if (is_action(TuiAction::CursorWordLeft))
    {
      static_cast<void>(apply_edit(TuiAction::CursorWordLeft));
    }
    else if (is_action(TuiAction::CursorWordRight))
    {
      static_cast<void>(apply_edit(TuiAction::CursorWordRight));
    }
    else if (is_action(TuiAction::PalettePrev) && (result = step_completion(CompletionStep::Previous, completions, snapshot)).handled)
    {
    // Composer selected the row and produced its status.
    }
    else if (is_action(TuiAction::HistoryPrev))
    {
      if (browse_history(true))
      {
        result.status = "history previous";
      }
      else
      {
        result.request = ComposerInputRequest::ScrollUp;
      }
    }
    else if (event.key == Key::ArrowUp)
    {
      result.request = ComposerInputRequest::ScrollUp;
    }
    else if (is_action(TuiAction::CursorUp) && move_vertical(TuiAction::CursorUp))
    {
    // Composer applied the movement and its successful-movement reset.
    }
    else if (is_action(TuiAction::PaletteNext) && (result = step_completion(CompletionStep::Next, completions, snapshot)).handled)
    {
    // Composer selected the row and produced its status.
    }
    else if (is_action(TuiAction::HistoryNext))
    {
      if (browse_history(false))
      {
        result.status = history_index() ? "history next" : "history draft";
      }
      else
      {
        result.request = ComposerInputRequest::ScrollDown;
      }
    }
    else if (event.key == Key::ArrowDown)
    {
      result.request = ComposerInputRequest::ScrollDown;
    }
    else if (is_action(TuiAction::CursorDown) && move_vertical(TuiAction::CursorDown))
    {
    // Composer applied the movement and its successful-movement reset.
    }
    else if (is_action(TuiAction::Undo))
    {
      result.status = apply_edit(TuiAction::Undo) ? "undo" : "nothing to undo";
    }
    else if (is_action(TuiAction::Redo))
    {
      result.status = apply_edit(TuiAction::Redo) ? "redo" : "nothing to redo";
    }
    else if (is_action(TuiAction::Yank))
    {
      result.status = apply_edit(TuiAction::Yank) ? "yanked text" : "nothing to yank";
    }
    else if (is_action(TuiAction::YankPop))
    {
      result.status = apply_edit(TuiAction::YankPop) ? "yank-pop" : "nothing to yank-pop";
    }
    else
      return {};
    result.handled = true;
    return result;
  }
  if (phase == ComposerInputPhase::Tail)
  {
    // Cancel gives the draft selection priority over the transcript selection,
    // then dismisses completion. Only an unclaimed Escape reaches double-Escape clearing.
    if (is_action(TuiAction::Cancel))
    {
      if (selection_bounds())
      {
        clear_selection();
        cancel_escape();
        result.status = std::string{};
      }
      else if (transcript_selected)
      {
        result.request = ComposerInputRequest::ClearTranscriptSelection;
        cancel_escape();
        result.status = std::string{};
      }
      else if ((!palette_suppressed() && slash_palette_visible(draft_.text, draft_.cursor, snapshot.slash_commands)) ||
               (completions.model && completions.model->surface == detail::CompletionSurface::FileReference && completions.model->palette_visible) ||
               (completions.model && completions.model->surface == detail::CompletionSurface::PathCompletion && completions.model->palette_visible))
      {
        dismiss_completion();
        result.status = std::string{};
      }
      else
        result = handle_escape();
    }
    else if (is_action(TuiAction::Submit))
    {
      if (input.event.key == Key::Enter)
        result = convert_backslash_enter_to_newline();
      if (!result.handled)
        result.request = ComposerInputRequest::Submit;
    }
    // Space comes after Submit deliberately: a custom Space binding must submit.
    else if (input.event.key == Key::Space)
      result = insert_input(input);
    else
      return {};
    result.handled = true;
    return result;
  }
  return {};
}

ComposerInputResult Composer::step_completion(CompletionStep step, detail::CompletionMatchCache const& completions, ComposerSnapshot const& snapshot)
{
  bool const slash = (!palette_suppressed() && slash_palette_visible(draft_.text, draft_.cursor, snapshot.slash_commands));
  bool const reference =
      !slash && (completions.model && completions.model->surface == detail::CompletionSurface::FileReference && completions.model->palette_visible);
  bool const path = !slash && !reference &&
                    (completions.model && completions.model->surface == detail::CompletionSurface::PathCompletion && completions.model->palette_visible);
  if (!slash && !reference && !path)
    return {};
  cancel_escape();
  auto const count = slash ? filter_slash_commands(draft_.text, draft_.cursor, snapshot.slash_commands).size()
                           : (completions.model ? completions.model->ranked_source_indices.size() : 0);
  if (count == 0)
    return {.handled = true, .status = slash ? "no matching slash commands" : reference ? "no matching file references" : "no matching paths"};
  if (slash)
    select_completion(step == CompletionStep::Previous
                          ? previous_slash_palette_selection(draft_.text, draft_.cursor, snapshot.slash_commands, completion_index())
                          : next_slash_palette_selection(draft_.text, draft_.cursor, snapshot.slash_commands, completion_index()));
  else
    select_completion(step == CompletionStep::Previous ? detail::previous_completion_selection(completions, completion_index())
                                                       : detail::next_completion_selection(completions, completion_index()));
  return {.handled = true,
          .status = std::string(slash       ? "command "
                                : reference ? "reference "
                                            : "path ") +
                    std::to_string(completion_index() + 1) + "/" + std::to_string(count)};
}

ComposerInputResult Composer::select_slash_command(ComposerSnapshot const& snapshot)
{
  ComposerInputResult result{.handled = true};
  selected_slash_command_index_ = clamp_slash_palette_selection(draft_.text, draft_.cursor, snapshot.slash_commands, selected_slash_command_index_);
  if (auto const reason = slash_command_selection_disabled_reason(draft_.text, draft_.cursor, snapshot.slash_commands, selected_slash_command_index_))
  {
    result.status = "command disabled: " + *reason;
    result.beep = true;
    return result;
  }
  auto selection = slash_command_selection_text(draft_.text, draft_.cursor, snapshot.slash_commands, selected_slash_command_index_);
  accept_completion(std::move(selection.text), selection.cursor);
  result.status = "command selected - press Enter to run";
  return result;
}
ComposerInputResult Composer::select_file_reference(detail::CompletionMatchCache const& completions, ComposerSnapshot const& snapshot)
{
  ComposerInputResult result{.handled = true};
  selected_slash_command_index_ = detail::clamp_completion_selection(completions, selected_slash_command_index_);
  if (auto const reason = detail::completion_selection_disabled_reason(completions, snapshot.file_references, selected_slash_command_index_))
  {
    result.status = "reference disabled: " + *reason;
    result.beep = true;
    return result;
  }
  auto selection = detail::completion_selection_text(completions, snapshot, selected_slash_command_index_);
  accept_completion(std::move(selection.text), selection.cursor);
  result.status = "file reference selected";
  return result;
}
ComposerInputResult Composer::select_path_completion(detail::CompletionMatchCache const& completions, ComposerSnapshot const& snapshot)
{
  ComposerInputResult result{.handled = true};
  selected_slash_command_index_ = detail::clamp_completion_selection(completions, selected_slash_command_index_);
  if (auto const reason = detail::completion_selection_disabled_reason(completions, snapshot.file_references, selected_slash_command_index_))
  {
    result.status = "path disabled: " + *reason;
    result.beep = true;
    return result;
  }
  auto selection = detail::completion_selection_text(completions, snapshot, selected_slash_command_index_);
  accept_completion(std::move(selection.text), selection.cursor);
  result.status = "path selected";
  return result;
}
ComposerInputResult Composer::force_path_completion(ComposerSnapshot const& snapshot, ComposerEditContext edit_context)
{
  ComposerInputResult result{.handled = true};
  auto const was_suppressed = slash_palette_suppressed_;
  slash_palette_suppressed_ = false;
  path_completion_force_active_ = true;
  // Query the prospective flags before publishing them. The prepared normal
  // cache still describes the previous flags and must not be reused here.
  detail::CompletionMatchCache completions;
  detail::refresh_completion_match_cache(completions, draft_.text, draft_.cursor, slash_palette_suppressed_, path_completion_force_active_,
                                         snapshot.slash_commands, snapshot.file_references, snapshot.file_references_generation);
  auto const match_count = (completions.model ? completions.model->ranked_source_indices.size() : 0);
  if (match_count == 0)
  {
    slash_palette_suppressed_ = was_suppressed;
    path_completion_force_active_ = false;
    return {};
  }
  if (edit_context == ComposerEditContext::Idle)
  {
    cancel_escape();
    leave_history();
    selected_slash_command_index_ = 0;
  }
  if (match_count == 1)
  {
    if (auto const reason = detail::completion_selection_disabled_reason(completions, snapshot.file_references, 0))
    {
      path_completion_force_active_ = false;
      result.status = "path disabled: " + *reason;
      result.beep = true;
      return result;
    }
    auto selection = detail::completion_selection_text(completions, snapshot, 0);
    accept_completion(std::move(selection.text), selection.cursor, ComposerCompletionOrigin::Forced);
    result.status = "path selected";
    return result;
  }
  if (edit_context == ComposerEditContext::Idle)
    draft_scroll_offset_ = 0;
  else
    selected_slash_command_index_ = 0;
  result.status = "path suggestions";
  return result;
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
ComposerInputResult Composer::insert_input(runtime_input::RuntimeInput const& input, ComposerEditContext context)
{
  ComposerInputResult result{.handled = true};
  prepare_text_edit();
  auto const text = input.text.empty() ? std::string(1, input.event.character) : input.text;
  if (input.bracketed_paste)
  {
    static_cast<void>(delete_selection());
    auto const inserted = insert_composer_paste_text(draft_, text);
    if (inserted || context == ComposerEditContext::ActiveRun)
      result.status = "pasted into draft safely";
  }
  else if (!replace_selection(text))
    static_cast<void>(insert_composer_draft_text(draft_, text));
  return result;
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
ComposerInputResult Composer::handle_escape()
{
  ComposerInputResult result{.handled = true};
  if (!draft_.text.empty())
  {
    if (pending_escape_clear_)
    {
      static_cast<void>(apply_action(TuiAction::ClearInput));
      reset_completion();
      leave_history();
      draft_scroll_offset_ = 0;
      cancel_escape();
      result.status = "input cleared";
    }
    else
    {
      pending_escape_clear_ = true;
      result.status = "press Esc again to clear";
    }
  }
  else
  {
    cancel_escape();
    result.status = "escape ignored";
  }
  return result;
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
  snapshot.draft_scroll_offset = draft_scroll_offset_;
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

ComposerInputResult Composer::copy_selection()
{
  ComposerInputResult result{.handled = true};
  auto const text = selected_text();
  if (!text || text->empty())
  {
    result.status = "no selection to copy";
    result.beep = true;
    result.handled = false;
    return result;
  }
  auto const copied = runtime_transcript::copy_text_to_terminal_clipboard(*text);
  result.status = copied ? "selection copy request sent" : "clipboard copy failed";
  if (!copied)
    result.beep = true;
  result.handled = copied;
  return result;
}

ComposerInputResult Composer::extend_selection(TuiAction movement)
{
  ComposerInputResult result{.handled = true};
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
  result.status = selection_bounds() ? "selection active" : "selection boundary";
  return result;
}

ComposerInputResult Composer::extend_selection_to(std::size_t target)
{
  ComposerInputResult result{.handled = true};
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
  result.status = selection_bounds() ? "selection active" : "selection boundary";
  return result;
}

ComposerInputResult Composer::extend_selection_for_key(terminal::Key key)
{
  using Key = terminal::Key;
  switch (key)
  {
    case Key::ShiftArrowUp:
      return extend_selection(TuiAction::CursorUp);
    case Key::ShiftArrowDown:
      return extend_selection(TuiAction::CursorDown);
    case Key::ShiftArrowLeft:
      return extend_selection(TuiAction::CursorLeft);
    case Key::ShiftArrowRight:
      return extend_selection(TuiAction::CursorRight);
    case Key::ShiftCtrlArrowLeft:
    case Key::ShiftAltArrowLeft:
      return extend_selection(TuiAction::CursorWordLeft);
    case Key::ShiftCtrlArrowRight:
    case Key::ShiftAltArrowRight:
      return extend_selection(TuiAction::CursorWordRight);
    case Key::ShiftHome:
      return extend_selection(TuiAction::CursorLineStart);
    case Key::ShiftEnd:
      return extend_selection(TuiAction::CursorLineEnd);
    case Key::ShiftCtrlHome:
      return extend_selection_to(0);
    case Key::ShiftCtrlEnd:
      return extend_selection_to(draft_.text.size());
    default:
      return {};
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

ComposerInputResult Composer::convert_backslash_enter_to_newline()
{
  ComposerInputResult result{.handled = true};
  if (selection_bounds())
    return {};
  if (!replace_composer_backslash_before_cursor_with_newline(draft_))
    return {};
  pending_escape_clear_ = false;
  history_index_.reset();
  draft_input_.clear();
  selected_slash_command_index_ = 0;
  slash_palette_suppressed_ = false;
  path_completion_force_active_ = false;
  draft_scroll_offset_ = 0;
  result.status = "newline inserted";
  return result;
}

}  // namespace ava::tui
