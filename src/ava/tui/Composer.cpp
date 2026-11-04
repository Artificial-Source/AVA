// Owns draft editing, completion acceptance and input precedence within the TUI.
// See docs/engineering/tui-architecture.md for the owner relationships.
#include "sys.h"
#include "ava/tui/Composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/terminal/Context.h"
#include "ava/core/Application.h"

#include <algorithm>
#include <array>

namespace ava::tui {

ComposerInputResult Composer::handle_key(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings, ComposerSnapshot const& snapshot,
                                         detail::CompletionMatchCache const& completions, bool transcript_selected, ComposerInputPhase phase,
                                         ComposerEditContext context)
{
  // The outer dispatcher chooses the phase so unrelated feature bindings can
  // keep their precedence. Each helper owns the same editor policy in both contexts.
  switch (phase)
  {
    case ComposerInputPhase::Jump:
      return handle_jump_input(input, bindings, context);
    case ComposerInputPhase::Primary:
      return handle_primary_input(input, bindings, context);
    case ComposerInputPhase::Editor:
      return handle_editor_input(input, bindings, snapshot, completions, transcript_selected, context);
    case ComposerInputPhase::Completion:
      return handle_completion_input(input, bindings, snapshot, completions, context);
    case ComposerInputPhase::JumpStart:
      return handle_jump_start(input.event.key, bindings);
    case ComposerInputPhase::Navigation:
      return handle_navigation_input(input, bindings, snapshot, completions, context);
    case ComposerInputPhase::Tail:
      return handle_tail_input(input, bindings, snapshot, completions, transcript_selected, context);
  }
  return {};
}

ComposerInputResult Composer::handle_jump_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings, ComposerEditContext context)
{
  using Key = terminal::Key;
  if (jump_mode() == ComposerJumpMode::None)
    return {};
  auto const key = input.event.key;
  if (key_matches_action(bindings, TuiAction::JumpForward, key) || key_matches_action(bindings, TuiAction::JumpBackward, key))
  {
    cancel_jump();
    return {.handled = true, .status = "jump cancelled"};
  }
  if (auto const target = runtime_input::printable_jump_target(input))
  {
    bool const forward = jump_mode() == ComposerJumpMode::Forward;
    return {.handled = true, .status = jump_to_character(*target) ? (forward ? "jumped forward" : "jumped backward") : "jump character not found"};
  }
  cancel_jump();
  // Preserve the existing paste policy: idle jump consumes non-target text,
  // while an active run lets that same input proceed to ordinary insertion.
  return {.handled = context == ComposerEditContext::Idle && (key == Key::Character || key == Key::Space)};
}

ComposerInputResult Composer::handle_jump_start(terminal::Key key, TuiKeyBindings const& bindings)
{
  bool const forward = key_matches_action(bindings, TuiAction::JumpForward, key);
  if (!forward && !key_matches_action(bindings, TuiAction::JumpBackward, key))
    return {};
  begin_jump(forward ? ComposerJumpMode::Forward : ComposerJumpMode::Backward);
  return {.handled = true, .status = forward ? "jump forward: type character" : "jump backward: type character"};
}

ComposerInputResult Composer::handle_selection_input(terminal::Key key, ComposerEditContext context)
{
  auto result = extend_selection_for_key(key);
  if (result.handled)
  {
    result.request = ComposerInputRequest::ClearTranscriptSelection;
    return result;
  }
  if (key != terminal::Key::CtrlHome && key != terminal::Key::CtrlEnd)
    return {};
  move_document_boundary(key == terminal::Key::CtrlEnd, context);
  return {.handled = true};
}

ComposerInputResult Composer::handle_primary_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings, ComposerEditContext context)
{
  if (input.event.key == terminal::Key::Character)
    return insert_input(input, context);
  // Active routing keeps newline and follow-up in its command stage, after
  // completion acceptance. Shifted selection and document keys are preemptive there.
  if (context == ComposerEditContext::ActiveRun)
    return handle_selection_input(input.event.key, context);
  if (key_matches_action(bindings, TuiAction::MessageFollowUp, input.event.key))
    return {.handled = true, .request = ComposerInputRequest::Submit};
  if (!key_matches_action(bindings, TuiAction::NewLine, input.event.key))
    return {};
  insert_newline();
  return {.handled = true};
}

ComposerInputResult Composer::handle_edit_actions(terminal::Key key, TuiKeyBindings const& bindings, ComposerEditContext context, EditGroup group)
{
  // Table order resolves collisions. Active input tries the whole table before
  // transcript navigation. Idle routing inserts copy, completion and history
  // between groups, so those barriers must not become one unconditional sweep.
  static constexpr std::array actions{std::pair{TuiAction::DeleteBackward, EditGroup::Deletion},
                                      std::pair{TuiAction::DeleteForward, EditGroup::Deletion},
                                      std::pair{TuiAction::DeleteWordBackward, EditGroup::Deletion},
                                      std::pair{TuiAction::DeleteWordForward, EditGroup::Deletion},
                                      std::pair{TuiAction::DeleteToLineStart, EditGroup::Deletion},
                                      std::pair{TuiAction::DeleteToLineEnd, EditGroup::Deletion},
                                      std::pair{TuiAction::ClearInput, EditGroup::Clear},
                                      std::pair{TuiAction::CursorLeft, EditGroup::Horizontal},
                                      std::pair{TuiAction::CursorRight, EditGroup::Horizontal},
                                      std::pair{TuiAction::CursorLineStart, EditGroup::Horizontal},
                                      std::pair{TuiAction::CursorLineEnd, EditGroup::Horizontal},
                                      std::pair{TuiAction::CursorWordLeft, EditGroup::Horizontal},
                                      std::pair{TuiAction::CursorWordRight, EditGroup::Horizontal},
                                      std::pair{TuiAction::Undo, EditGroup::Undo},
                                      std::pair{TuiAction::Redo, EditGroup::Undo},
                                      std::pair{TuiAction::Yank, EditGroup::Undo},
                                      std::pair{TuiAction::YankPop, EditGroup::Undo}};
  for (auto const& [action, action_group] : actions)
  {
    if ((group != EditGroup::All && action_group != group) || !key_matches_action(bindings, action, key))
      continue;
    if (action == TuiAction::DeleteForward && key == terminal::Key::CtrlD && draft_.text.empty())
      continue;
    if (action == TuiAction::ClearInput && context == ComposerEditContext::Idle && draft_.text.empty() &&
        key_matches_action(bindings, TuiAction::Interrupt, key))
      continue;
    bool const changed = apply_edit(action, context);
    ComposerInputResult result{.handled = true};
    // Active edits preserve existing feedback, including unsuccessful movement.
    if (context == ComposerEditContext::ActiveRun)
      return result;
    switch (action)
    {
      case TuiAction::ClearInput:
        result.status = changed ? "input cleared" : "input already empty";
        break;
      case TuiAction::Undo:
        result.status = changed ? "undo" : "nothing to undo";
        break;
      case TuiAction::Redo:
        result.status = changed ? "redo" : "nothing to redo";
        break;
      case TuiAction::Yank:
        result.status = changed ? "yanked text" : "nothing to yank";
        break;
      case TuiAction::YankPop:
        result.status = changed ? "yank-pop" : "nothing to yank-pop";
        break;
      default:
        break;
    }
    return result;
  }
  return {};
}

ComposerInputResult Composer::handle_editor_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings, ComposerSnapshot const& snapshot,
                                                  detail::CompletionMatchCache const& completions, bool transcript_selected, ComposerEditContext context)
{
  auto const key = input.event.key;
  if (context == ComposerEditContext::ActiveRun)
    return handle_edit_actions(key, bindings, context, EditGroup::All);
  // Idle copy sits after deletion but before clearing. Active routing already
  // performed copy and Interrupt before reaching its edit table.
  if (auto result = handle_jump_start(key, bindings); result.handled)
    return result;
  if (auto result = handle_edit_actions(key, bindings, context, EditGroup::Deletion); result.handled)
    return result;
  if (key_matches_action(bindings, TuiAction::CopySelection, key) && (selection_bounds() || transcript_selected))
  {
    cancel_escape();
    cancel_path_completion();
    if (selection_bounds())
    {
      auto result = copy_selection();
      // Copy owns this binding even if the clipboard request fails. A failed
      // copy must not fall through to another edit configured on the same key.
      result.handled = true;
      return result;
    }
    return {.handled = true, .request = ComposerInputRequest::CopyTranscript};
  }
  if (auto result = handle_edit_actions(key, bindings, context, EditGroup::Clear); result.handled)
    return result;
  return handle_completion_input(input, bindings, snapshot, completions, context);
}

ComposerInputResult Composer::handle_completion_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings,
                                                      ComposerSnapshot const& snapshot, detail::CompletionMatchCache const& completions,
                                                      ComposerEditContext context)
{
  if (!key_matches_action(bindings, TuiAction::AutocompleteAccept, input.event.key))
    return {};
  bool const slash = !palette_suppressed() && slash_palette_visible(draft_.text, draft_.cursor, snapshot.slash_commands);
  bool const reference = completions.model && completions.model->surface == detail::CompletionSurface::FileReference && completions.model->palette_visible;
  bool const path = completions.model && completions.model->surface == detail::CompletionSurface::PathCompletion && completions.model->palette_visible;
  // Do not cancel Escape when active keyboard completion accepts or rejects a
  // candidate. Idle completion has always retired that arm before visible choices.
  if (context == ComposerEditContext::Idle && (slash || reference || path))
    cancel_escape();
  if (slash)
    return select_slash_command(snapshot, context);
  if (reference)
    return select_file_reference(completions, snapshot);
  if (path)
    return select_path_completion(completions, snapshot);
  auto result = force_path_completion(snapshot, context);
  if (result.handled && context == ComposerEditContext::Idle)
    cancel_escape();
  return result;
}

ComposerInputResult Composer::handle_vertical_input(CompletionStep step, terminal::Key key, TuiKeyBindings const& bindings, ComposerSnapshot const& snapshot,
                                                    detail::CompletionMatchCache const& completions)
{
  bool const previous = step == CompletionStep::Previous;
  if (key_matches_action(bindings, previous ? TuiAction::PalettePrev : TuiAction::PaletteNext, key))
    if (auto result = step_completion(step, completions, snapshot); result.handled)
      return result;
  if (key_matches_action(bindings, previous ? TuiAction::HistoryPrev : TuiAction::HistoryNext, key))
  {
    if (browse_history(previous))
      return {.handled = true, .status = previous ? "history previous" : history_index() ? "history next" : "history draft"};
    return {.handled = true, .request = previous ? ComposerInputRequest::ScrollUp : ComposerInputRequest::ScrollDown};
  }
  if (key == (previous ? terminal::Key::ArrowUp : terminal::Key::ArrowDown))
    return {.handled = true, .request = previous ? ComposerInputRequest::ScrollUp : ComposerInputRequest::ScrollDown};
  auto const action = previous ? TuiAction::CursorUp : TuiAction::CursorDown;
  if (key_matches_action(bindings, action, key) && move_vertical(action))
    return {.handled = true};
  return {};
}

ComposerInputResult Composer::handle_navigation_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings,
                                                      ComposerSnapshot const& snapshot, detail::CompletionMatchCache const& completions,
                                                      ComposerEditContext context)
{
  auto const key = input.event.key;
  if (context == ComposerEditContext::Idle)
  {
    if (auto result = handle_selection_input(key, context); result.handled)
      return result;
    if (auto result = handle_edit_actions(key, bindings, context, EditGroup::Horizontal); result.handled)
      return result;
  }
  // All Previous choices precede all Next choices, including user-bound collisions.
  if (auto result = handle_vertical_input(CompletionStep::Previous, key, bindings, snapshot, completions); result.handled)
    return result;
  if (auto result = handle_vertical_input(CompletionStep::Next, key, bindings, snapshot, completions); result.handled)
    return result;
  return context == ComposerEditContext::Idle ? handle_edit_actions(key, bindings, context, EditGroup::Undo) : ComposerInputResult{};
}

ComposerInputResult Composer::handle_tail_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings, ComposerSnapshot const& snapshot,
                                                detail::CompletionMatchCache const& completions, bool transcript_selected, ComposerEditContext context)
{
  using Key = terminal::Key;
  if (context == ComposerEditContext::ActiveRun)
    return input.event.key == Key::Space ? insert_input(input, context) : ComposerInputResult{};
  ComposerInputResult result;
  // Cancel gives draft selection priority over transcript selection, then
  // dismisses completion. Only an unhandled Escape reaches double-Escape clearing.
  if (key_matches_action(bindings, TuiAction::Cancel, input.event.key))
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
             (completions.model && completions.model->palette_visible &&
              (completions.model->surface == detail::CompletionSurface::FileReference ||
               completions.model->surface == detail::CompletionSurface::PathCompletion)))
    {
      dismiss_completion();
      result.status = std::string{};
    }
    else
      result = handle_escape();
  }
  else if (key_matches_action(bindings, TuiAction::Submit, input.event.key))
  {
    if (input.event.key == Key::Enter)
      result = convert_backslash_enter_to_newline();
    if (!result.handled)
      result.request = ComposerInputRequest::Submit;
  }
  // Space comes after Submit so a configured Space binding still submits.
  else if (input.event.key == Key::Space)
    result = insert_input(input);
  else
    return {};
  result.handled = true;
  return result;
}

ComposerInputResult Composer::step_completion(CompletionStep step, detail::CompletionMatchCache const& completions, ComposerSnapshot const& snapshot)
{
  // Use the same surface priority as painting. The const prepared model belongs to the
  // caller and remains a read-only snapshot throughout this selection step.
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
  // An empty visible palette still handles navigation, preventing accidental history movement.
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

ComposerInputResult Composer::select_slash_command(ComposerSnapshot const& snapshot, ComposerEditContext context)
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
  result.status = context == ComposerEditContext::ActiveRun ? "command selected - press Enter to queue" : "command selected - press Enter to run";
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
  // Query with the proposed flags before storing them. The prepared normal
  // cache still describes the previous flags and must not be reused here.
  detail::CompletionMatchCache completions;
  detail::refresh_completion_match_cache(completions, draft_.text, draft_.cursor, slash_palette_suppressed_, path_completion_force_active_,
                                         snapshot.slash_commands, snapshot.file_references, snapshot.file_references_generation);
  auto const match_count = (completions.model ? completions.model->ranked_source_indices.size() : 0);
  // A forced query must not leave a newly visible palette behind when it declines the key.
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
  // Only a single enabled match is safe to insert immediately; several matches require
  // a visible choice, while disabled content reports why no replacement occurred.
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
  // Reset policy precedes editing, including unsuccessful edits. Active-run actions leave
  // history broadly; idle cursor/undo routes deliberately retain narrower interaction state.
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
  // A selected range consumes a deletion before the ordinary character/word action runs.
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
  // Selection and palette dismissal were handled by the caller. This flag is an explicit
  // arm with no timer; only paths that call cancel_escape or reset it break the sequence.
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
void Composer::project_completion(ComposerSnapshot& snapshot) const
{
  snapshot.input = draft_.text;
  snapshot.input_cursor = draft_.cursor;
  snapshot.selected_slash_command_index = selected_slash_command_index_;
  snapshot.slash_palette_suppressed = slash_palette_suppressed_;
  snapshot.path_completion_force_active = path_completion_force_active_;
}
void Composer::project(ComposerSnapshot& snapshot) const
{
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
  // Keep the initial anchor stable while direct document-boundary selection moves only
  // the far endpoint; invalidate metadata that belonged to the prior cursor position.
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
  // Shifted movement shares one anchor and reset path. Unsupported keys decline without
  // mutating selection, allowing normal binding dispatch to continue.
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
