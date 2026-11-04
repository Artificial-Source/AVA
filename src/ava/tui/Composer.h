// Owns draft editing, completion acceptance and input precedence within the TUI.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "ComposerInputResult.h"
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

namespace detail {
struct CompletionMatchCache;
}
class PromptStash;
enum class PromptStashRestoreResult;
namespace terminal {
class Context;
}

// Input routing chooses which interaction state an edit resets. Idle and active-run
// editing deliberately use different policies.
enum class ComposerEditContext
{
  // Use idle editing resets; ordinary cursor movement can retain history browsing.
  Idle,
  // Use the active-run resets, which leave history even for ordinary edit actions.
  ActiveRun
};
// TUI-thread completion acceptance policy, chosen separately for each input action.
enum class ComposerCompletionOrigin
{
  // Keyboard acceptance leaves history and resets the completion row and scroll.
  Keyboard,
  // A clicked slash/reference row preserves forced-path and scroll state.
  Pointer,
  // A clicked path row clears forcing but preserves history and scroll state.
  PointerPath,
  // A one-match forced query preserves the current completion row and history.
  Forced
};

// The dispatcher calls the editor at its original positions so user-defined
// binding collisions retain their precedence across session/transcript actions.
enum class ComposerInputPhase
{
  Jump, // An armed jump gets first refusal before ordinary typing.
  Primary, // Typing; idle adds follow-up/newline, active adds shifted selection/document keys.
  Editor, // Idle deletion/copy/clear/completion; active ordinary edits after its stop/submit stages.
  Completion, // Active completion acceptance before newline, handoffs and queue submission.
  JumpStart, // Active jump arming after handoffs/dequeue but before submission.
  Navigation, // Palette/history after transcript bindings; idle also handles horizontal edits and undo.
  Tail, // Idle Cancel/Submit before Space; active Space only, after its command stages.
};

// TUI-thread palette movement wraps within the currently prepared matches.
enum class CompletionStep
{
  // Move toward earlier matches, wrapping from the first to the last.
  Previous,
  // Move toward later matches, wrapping from the last to the first.
  Next
};

// Session owns the draft text, undo state and selection in this editor. Renderer,
// input and navigation borrow it and must be destroyed first. All access stays on
// the TUI thread; workers never read its state. Completion uses the caller's prepared
// const model, so editing does not call back into navigation. Edits leave the
// cursor outside the interior of a valid UTF-8 character; malformed bytes remain editable.
class Composer final
{
 public:
  // Construct an empty editor before any renderer or navigation owner borrows it.
  Composer() = default;
  // Not copyable: one editor owns the draft history and keeps a stable identity for borrowers.
  Composer(Composer const&) = delete;
  Composer& operator=(Composer const&) = delete;
  // TUI thread: handle one documented dispatch phase. All inputs are call-scoped;
  // returned requests are performed by the session before its next paint. The
  // caller supplies prepared completions for completion/navigation phases. Context
  // preserves idle versus active precedence and reset rules; the editor owns no run lifecycle.
  [[nodiscard]] ComposerInputResult handle_key(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings, ComposerSnapshot const& snapshot,
                                               detail::CompletionMatchCache const& completions, bool transcript_selected, ComposerInputPhase phase,
                                               ComposerEditContext context = ComposerEditContext::Idle);
  // TUI thread: share palette wrapping/status between idle and active-run routing.
  [[nodiscard]] ComposerInputResult step_completion(CompletionStep step, detail::CompletionMatchCache const& completions, ComposerSnapshot const& snapshot);

  // TUI thread: borrow editor text and paste state until the next edit or destruction.
  [[nodiscard]] ComposerDraftState const& draft() const noexcept;
  // TUI thread: borrow submitted prompts until recording or session reset mutates them.
  [[nodiscard]] std::vector<std::string> const& history() const noexcept;
  // TUI thread: distinguish a recalled entry from the live draft; no index means live editing.
  [[nodiscard]] std::optional<std::size_t> history_index() const noexcept;
  // TUI thread: tell input routing whether the next printable character is a jump target.
  [[nodiscard]] ComposerJumpMode jump_mode() const noexcept;
  // TUI thread: supply the selected slash/reference/path row for display and acceptance.
  [[nodiscard]] std::size_t completion_index() const noexcept;
  // TUI thread: let completion preparation honor explicit palette dismissal.
  [[nodiscard]] bool palette_suppressed() const noexcept;
  // TUI thread: expose the requested top draft row; layout clamps it before painting.
  [[nodiscard]] std::size_t scroll_offset() const noexcept;
  // TUI thread: identify a live draft drag so pointer routing can extend or finish it.
  [[nodiscard]] bool mouse_selecting() const noexcept;

  // Disarm double-Escape clearing on the TUI thread without changing draft content.
  void cancel_escape();
  // TUI thread: abandon history, jump, selection and completion before a new interaction.
  // This retains the draft and its undo state.
  void reset_interaction();
  // TUI thread: clear the submitted draft and history cursor; keep recorded history.
  // Escape and palette suppression survive; forced completion is cleared.
  void reset_after_submit();
  // TUI thread: apply submission cleanup and reveal completion for subsequent typing.
  void reset_local_command();
  // TUI thread: apply local-command cleanup but restore the previous forced-path flag.
  void reset_after_queue();
  // TUI thread: discard draft, history and all interaction state after identity changes.
  void reset_for_session_transition();
  // TUI thread: discard editor text, paste records and undo state; interaction flags remain.
  void reset_draft();
  // TUI thread: install owned text at a valid cursor boundary and report a content change.
  // Preserve interaction state; the caller chooses history, selection and completion cleanup.
  bool replace_draft(std::string text, std::size_t cursor = std::string::npos);
  // TUI thread: recover rejected/submitted text with completion visible and history exited.
  // Jump and Escape state are retained.
  void restore_draft(std::string text);
  // TUI thread: install a command, clear selection and leave history without resetting
  // completion, Escape or jump policy chosen by the requesting feature.
  void draft_command(std::string text);
  // TUI thread: append a submitted prompt under history deduplication/size policy.
  void record_submission(std::string text);
  // TUI thread: execute the low-level editor action and return whether it changed editor
  // state. The caller performs any interaction resets before this call.
  bool apply_action(TuiAction action);
  // TUI thread: reset interaction according to idle/active policy before applying an action.
  // Deletion consumes a draft selection first; idle cursor movement can retain history.
  bool apply_edit(TuiAction action, ComposerEditContext context = ComposerEditContext::Idle);
  // TUI thread: leave history and clear selection only if vertical movement succeeds.
  // False permits the input route to try its next binding or scrolling fallback.
  bool move_vertical(TuiAction action);
  // TUI thread: reveal completion and reset selection/scroll before navigating history.
  // The first backward step saves the live draft; forward past newest restores it.
  bool browse_history(bool previous);
  // TUI thread: leave history and reset text-edit metadata before inserting decoded input.
  // Bracketed paste uses paste records; ordinary typing replaces a selected range.
  ComposerInputResult insert_input(runtime_input::RuntimeInput const& input, ComposerEditContext context = ComposerEditContext::Idle);

  // TUI thread: clamp and validate the slash row before installing its draft text.
  // A disabled row returns feedback without changing the draft. Context selects
  // whether accepted-command feedback says Enter will run it or queue it.
  ComposerInputResult select_slash_command(ComposerSnapshot const& snapshot, ComposerEditContext context = ComposerEditContext::Idle);
  // TUI thread: accept a reference using the caller-prepared const completion model.
  // The model and snapshot must describe the current draft and catalog.
  ComposerInputResult select_file_reference(detail::CompletionMatchCache const& completions, ComposerSnapshot const& snapshot);
  // TUI thread: accept a path using the current prepared model after disabled-row checks.
  ComposerInputResult select_path_completion(detail::CompletionMatchCache const& completions, ComposerSnapshot const& snapshot);
  // TUI thread: query a fresh model with prospective forcing flags; navigation is not called.
  // Zero matches decline the action; one match inserts it; several expose the palette.
  ComposerInputResult force_path_completion(ComposerSnapshot const& snapshot, ComposerEditContext edit_context = ComposerEditContext::Idle);
  // TUI thread: move one stash entry into an empty draft; interaction cleanup stays with
  // the stash controller, and failure preserves both the draft and stash.
  PromptStashRestoreResult restore_stashed(PromptStash& stash, std::uint64_t id);
  // TUI thread: retain a row selected by a prepared palette; acceptance clamps stale indexes.
  void select_completion(std::size_t index);
  // TUI thread: return to the first row and end forced-path mode without unsuppressing palettes.
  void reset_completion();
  // TUI thread: end explicit forcing while preserving row selection and suppression.
  void cancel_path_completion();
  // TUI thread: replace draft text/cursor and clear draft selection. Origin chooses
  // which history, row, forcing and scroll state survives; Escape is caller-owned.
  void accept_completion(std::string text, std::size_t cursor, ComposerCompletionOrigin origin = ComposerCompletionOrigin::Keyboard);
  // TUI thread: hide completion, leave history and disarm Escape so dismissal cannot
  // also count as the first Escape of a draft clear.
  void dismiss_completion();
  // TUI thread: arm a one-character cursor jump after leaving history and completion.
  void begin_jump(ComposerJumpMode mode);
  // TUI thread: stop waiting for a jump target without editing or resetting other state.
  void cancel_jump();
  // TUI thread: consume the armed direction and reset interaction before searching.
  // A missing target still retires the jump; no borrowed target text is retained.
  bool jump_to_character(std::string_view target);
  // TUI thread: move to draft start/end and discard selection/cursor metadata.
  // Active-run movement also leaves history and clears forced completion.
  void move_document_boundary(bool end, ComposerEditContext context = ComposerEditContext::Idle);
  // TUI thread: clamp a hit to an atomic text/paste boundary and optionally arm a drag.
  // Idle clicks leave history and reset cursor metadata; active-run clicks retain them.
  void begin_pointer_selection(std::size_t cursor, bool selecting, ComposerEditContext context = ComposerEditContext::Idle);
  // TUI thread: extend an already armed drag to an atomic boundary. The caller must
  // begin the selection first; idle dragging also leaves history.
  void extend_pointer_selection(std::size_t cursor, ComposerEditContext context = ComposerEditContext::Idle);
  // TUI thread: retire the drag while preserving its committed selected range.
  void end_pointer_selection();
  // TUI thread: handle Escape only after selection and completion have declined it.
  // First Escape arms a nonempty draft; a second armed Escape clears it.
  ComposerInputResult handle_escape();

  // TUI thread: derive a scroll bound from current draft wrapping and supplied layout.
  [[nodiscard]] std::size_t max_draft_scroll_offset(ComposerSnapshot const& snapshot, std::size_t height) const;
  // TUI thread: reduce the requested scroll after layout shrinks the available range.
  void clamp_scroll(std::size_t maximum);
  // TUI thread: copy draft, completion, scroll and selection fields without changing
  // editor state. Mutations establish cursor validity; retain no snapshot reference.
  void project(ComposerSnapshot& snapshot) const;
  // TUI thread: publish only draft/completion inputs before the caller prepares matches.
  void project_completion(ComposerSnapshot& snapshot) const;
  // TUI thread: retire both a drag and its range without moving the draft cursor.
  void clear_selection();
  // TUI thread: normalize/clamp byte endpoints; empty or unarmed ranges have no value.
  [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> selection_bounds() const;
  // TUI thread: copy the selected text through the terminal clipboard boundary.
  // Report request delivery, not clipboard ownership; preserve the selection.
  ComposerInputResult copy_selection();
  // TUI thread: leave history/completion and retain the first anchor while applying movement.
  ComposerInputResult extend_selection(TuiAction movement);
  // TUI thread: extend from the retained anchor to a clamped byte target after edit resets.
  ComposerInputResult extend_selection_to(std::size_t target);
  // TUI thread: translate supported shifted keys into selection movement; decline other keys.
  ComposerInputResult extend_selection_for_key(terminal::Key key);
  // TUI thread: replace selection or insert a newline, leaving history and forced completion.
  // Explicit palette suppression survives this path.
  void insert_newline();
  // TUI thread: consume Enter only for an unselected backslash immediately before the cursor.
  // Success resets history/completion and reports a newline; otherwise submission may continue.
  ComposerInputResult convert_backslash_enter_to_newline();

  // Draft, history, selection and editor undo/kill buffers contain arbitrary user
  // text and must never be emitted by generated member printing.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI-only subsets of one edit table. Idle inserts other owners between groups;
  // ActiveRun requests All after its earlier copy, stop and submission decisions.
  enum class EditGroup
  {
    Deletion, // Erase actions precede idle copy and clearing.
    Clear, // Idle clearing follows copy and may yield an empty draft to Interrupt.
    Horizontal, // Idle cursor edits follow transcript bindings but precede history.
    Undo, // Idle undo/redo/yank follow all palette/history choices.
    All // Active edits use table order before transcript navigation.
  };
  // TUI thread: consume/cancel an armed jump. Active non-target paste may continue
  // to typing; idle retains its existing text-consumption policy. No input borrow escapes.
  [[nodiscard]] ComposerInputResult handle_jump_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings, ComposerEditContext context);
  // TUI thread: arm the configured forward/backward jump at the caller's precedence
  // position. Return unhandled when neither action matches; no borrowed state is retained.
  [[nodiscard]] ComposerInputResult handle_jump_start(terminal::Key key, TuiKeyBindings const& bindings);
  // TUI thread: apply shifted selection or document movement, returning any request
  // to retire transcript selection. Context selects which editor interaction to reset.
  [[nodiscard]] ComposerInputResult handle_selection_input(terminal::Key key, ComposerEditContext context);
  // TUI thread: type before later phases. Idle handles follow-up/newline here;
  // active handles selection/document keys and leaves submission to its run owner.
  [[nodiscard]] ComposerInputResult handle_primary_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings, ComposerEditContext context);
  // TUI thread: apply the first matching action in the selected table group.
  // Context controls history/reset/feedback; empty Ctrl+D remains available for exit.
  [[nodiscard]] ComposerInputResult handle_edit_actions(terminal::Key key, TuiKeyBindings const& bindings, ComposerEditContext context, EditGroup group);
  // TUI thread: preserve idle barriers around copy, clear and completion. Active
  // runs apply only ordinary edits here because their other phases already ran.
  [[nodiscard]] ComposerInputResult handle_editor_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings,
                                                        ComposerSnapshot const& snapshot, detail::CompletionMatchCache const& completions,
                                                        bool transcript_selected, ComposerEditContext context);
  // TUI thread with prepared matches: accept the first visible enabled choice or
  // force a path query. Borrow all inputs only for this call; active keeps its Escape arm.
  [[nodiscard]] ComposerInputResult handle_completion_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings,
                                                            ComposerSnapshot const& snapshot, detail::CompletionMatchCache const& completions,
                                                            ComposerEditContext context);
  // TUI thread with prepared matches: palette, history, literal arrow and vertical
  // movement compete in that order. Missing history requests scrolling from the caller.
  [[nodiscard]] ComposerInputResult handle_vertical_input(CompletionStep step, terminal::Key key, TuiKeyBindings const& bindings,
                                                          ComposerSnapshot const& snapshot, detail::CompletionMatchCache const& completions);
  // TUI thread after external transcript bindings: Previous precedes Next. Idle
  // surrounds them with selection/horizontal edits and then undo; active already edited.
  [[nodiscard]] ComposerInputResult handle_navigation_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings,
                                                            ComposerSnapshot const& snapshot, detail::CompletionMatchCache const& completions,
                                                            ComposerEditContext context);
  // TUI thread at the final fallback: idle Cancel and Submit outrank Space.
  // Active cancellation/submission remain with its run owner, so only Space applies.
  [[nodiscard]] ComposerInputResult handle_tail_input(runtime_input::RuntimeInput const& input, TuiKeyBindings const& bindings,
                                                      ComposerSnapshot const& snapshot, detail::CompletionMatchCache const& completions,
                                                      bool transcript_selected, ComposerEditContext context);

  // TUI thread: replace the normalized selected range and retire the range. False means
  // there was no range or the low-level replacement made no change.
  bool replace_selection(std::string_view replacement);
  // TUI thread: remove a selected range before a deletion key can affect another character.
  bool delete_selection();
  // TUI thread: return owned selected bytes so clipboard work cannot retain draft storage.
  [[nodiscard]] std::optional<std::string> selected_text() const;
  // TUI thread: discard the history cursor and saved live draft when editing branches away.
  void leave_history();
  // TUI thread: disarm Escape, leave history, reveal the first completion row and reset scroll.
  // Forced completion, jump and selection need their caller-specific treatment.
  void prepare_text_edit();
  // TUI thread: invalidate vertical-column and yank spans after direct cursor assignment.
  void reset_cursor_metadata();
  // TUI-thread submitted prompts; record_submission appends, session transition clears.
  std::vector<std::string> input_history_;
  // TUI-thread recalled prompt index; empty means live draft. Browsing sets it; edits may retire it.
  std::optional<std::size_t> history_index_;
  // TUI-thread saved live text while history is open; cleared when browsing is abandoned.
  std::string draft_input_;
  // Sole TUI-thread text, cursor, paste and undo storage; const readers borrow it briefly.
  ComposerDraftState draft_;
  // TUI-thread pending jump direction; the next target consumes it even if not found.
  ComposerJumpMode jump_mode_ = ComposerJumpMode::None;
  // TUI-thread row shared by all completion surfaces; preparation/acceptance bounds it.
  std::size_t selected_slash_command_index_ = 0;
  // TUI-thread dismissal flag; text-edit paths reveal palettes, while some submit paths retain it.
  bool slash_palette_suppressed_ = false;
  // TUI-thread explicit path-query mode; cleared by completion reset or acceptance except Pointer.
  bool path_completion_force_active_ = false;
  // TUI-thread requested first draft row; edits often reset it and rendering clamps it.
  std::size_t draft_scroll_offset_ = 0;
  // TUI-thread fixed byte anchor, or npos when unarmed; extended selection preserves it.
  std::size_t draft_selection_anchor_ = std::string::npos;
  // TUI-thread moving byte endpoint; normalized only when readers ask for selection bounds.
  std::size_t draft_selection_cursor_ = std::string::npos;
  // TUI-thread live drag flag; release clears it while leaving a selected range available.
  bool mouse_selecting_ = false;
  // TUI-thread clear arm set by handle_escape on a nonempty draft. Explicit cancel_escape
  // paths and text edits disarm it; low-level replacement, pointer operations, submit
  // resets and scroll setters do not universally clear it. It has no timeout.
  bool pending_escape_clear_ = false;
};

} // namespace ava::tui
