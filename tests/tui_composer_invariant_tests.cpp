// Protect draft validity at edit boundaries and publication before the next render.
#include "sys.h"
#include "support/TuiEventLoopFixture.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/Composer.h"
#include "ava/tui/PendingAttachments.h"
#include "ava/tui/SelectorHost.h"
#include "ava/tui/SessionSelector.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/event_state.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/core/Application.h"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ava::tui::Composer;
using ava::tui::ComposerDraftState;
using ava::tui::ComposerSnapshot;
using ava::tui::TuiAction;

// Compare saved edits so projection cannot silently rewrite undo or paste ownership.
bool same_snapshots(std::vector<ava::tui::ComposerDraftSnapshot> const& left, std::vector<ava::tui::ComposerDraftSnapshot> const& right)
{
  if (left.size() != right.size())
    return false;
  for (std::size_t index = 0; index < left.size(); ++index)
  {
    if (left[index].text != right[index].text || left[index].cursor != right[index].cursor || left[index].paste_entries != right[index].paste_entries ||
        left[index].next_paste_id != right[index].next_paste_id)
      return false;
  }
  return true;
}

// Compare all observable draft state, including metadata that does not appear on screen.
bool same_draft(ComposerDraftState const& left, ComposerDraftState const& right)
{
  return left.text == right.text && left.cursor == right.cursor && left.kill_buffer == right.kill_buffer && same_snapshots(left.undo_stack, right.undo_stack) &&
         same_snapshots(left.redo_stack, right.redo_stack) && left.kill_ring == right.kill_ring && left.yank_start == right.yank_start &&
         left.yank_end == right.yank_end && left.yank_ring_index == right.yank_ring_index && left.paste_entries == right.paste_entries &&
         left.next_paste_id == right.next_paste_id && left.vertical_column == right.vertical_column &&
         left.coalesce_typing_undo == right.coalesce_typing_undo && left.kill_sequence == right.kill_sequence;
}

// Check the mutation before projection, then require two const projections to preserve owner state.
void expect_const_projection(Composer const& composer, std::string_view text, std::size_t cursor, std::string_view operation)
{
  auto const label = std::string(operation);
  expect(composer.draft().text == text && composer.draft().cursor == cursor, label + " leaves the expected cursor before projection");
  auto const before = composer.draft();
  auto const history = composer.history();
  auto const history_index = composer.history_index();
  auto const selection = composer.selection_bounds();
  auto const jump = composer.jump_mode();
  auto const dragging = composer.mouse_selecting();
  ComposerSnapshot first;
  ComposerSnapshot second;
  first.status = "retained status";
  second.status = "retained status";
  composer.project(first);
  composer.project(second);
  expect(first.input == text && first.input_cursor == cursor && first.input == second.input && first.input_cursor == second.input_cursor &&
             first.input_selection_start == second.input_selection_start && first.input_selection_end == second.input_selection_end &&
             first.selected_slash_command_index == second.selected_slash_command_index && first.slash_palette_suppressed == second.slash_palette_suppressed &&
             first.path_completion_force_active == second.path_completion_force_active && first.draft_scroll_offset == second.draft_scroll_offset &&
             first.status == "retained status" && second.status == "retained status",
         label + " projects repeatable editor fields while preserving unrelated status");
  expect(same_draft(before, composer.draft()) && history == composer.history() && history_index == composer.history_index() &&
             selection == composer.selection_bounds() && jump == composer.jump_mode() && dragging == composer.mouse_selecting(),
         label + " leaves draft, undo, history and interaction unchanged through const projection");
}

// Newly joined UTF-8 sequences must be safe immediately after each low-level mutation returns.
void test_editor_mutations_join_utf8_fragments()
{
  std::string const starter = "\xC3";
  std::string const suffix = "\xA9";
  auto const joined = starter + suffix;
  ComposerDraftState draft;

  ava::tui::reset_composer_draft(draft, suffix, 0);
  bool const inserted = ava::tui::insert_composer_draft_text(draft, starter);
  expect(inserted && draft.text == joined && draft.cursor == 0, "inserting a starter before an orphan continuation clamps before returning");

  ava::tui::reset_composer_draft(draft, suffix, 0);
  bool const pasted = ava::tui::insert_composer_paste_text(draft, starter);
  expect(pasted && draft.text == joined && draft.cursor == 0 && draft.paste_entries.empty(),
         "a literal paste that completes a UTF-8 sequence returns with a valid cursor");

  ava::tui::reset_composer_draft(draft, "x" + suffix, 0);
  bool const replaced = ava::tui::replace_composer_draft_range(draft, 0, 1, starter);
  expect(replaced && draft.text == joined && draft.cursor == 0, "range replacement clamps when its final byte joins the retained suffix");

  auto const separated = starter + "x" + suffix;
  for (auto const action : {TuiAction::DeleteForward, TuiAction::DeleteBackward})
  {
    ava::tui::reset_composer_draft(draft, separated, action == TuiAction::DeleteForward ? 1 : 2);
    bool const deleted = ava::tui::apply_composer_draft_action(draft, action);
    expect(deleted && draft.text == joined && draft.cursor == 0, "deleting a separator leaves no cursor inside the newly joined UTF-8 sequence");
  }

  ava::tui::reset_composer_draft(draft, separated, 1);
  bool const erased = ava::tui::replace_composer_draft_range(draft, 1, 2, {});
  expect(erased && draft.text == joined && draft.cursor == 0, "empty range replacement clamps after joining the surrounding fragments");
}

// Yank replacement uses its raw inserted byte span even when the display cursor snaps before it.
void test_yank_pop_preserves_suffix_and_raw_span()
{
  std::string const starter = "\xC3";
  std::string const suffix = "\xA9";
  auto const joined = starter + suffix;
  ComposerDraftState draft;
  ava::tui::reset_composer_draft(draft, suffix, 0);
  // Build the retained choices directly; GCC 16 with TSan misdiagnoses the
  // temporary initializer-list strings as possibly uninitialized.
  draft.kill_ring.push_back(starter);
  draft.kill_ring.emplace_back("xy");

  bool const yanked = ava::tui::apply_composer_draft_action(draft, TuiAction::Yank);
  expect(yanked && draft.text == joined && draft.cursor == 0 && draft.yank_start == 0 && draft.yank_end == 1,
         "Yank records the inserted starter byte independently of its clamped cursor");

  bool const expanded = ava::tui::apply_composer_draft_action(draft, TuiAction::YankPop);
  expect(expanded && draft.text == "xy" + suffix && draft.cursor == 2 && draft.yank_start == 0 && draft.yank_end == 2 && draft.yank_ring_index == 1,
         "YankPop replaces only the inserted bytes and preserves the orphan suffix when expanding");

  bool const rejoined = ava::tui::apply_composer_draft_action(draft, TuiAction::YankPop);
  expect(rejoined && draft.text == joined && draft.cursor == 0 && draft.yank_start == 0 && draft.yank_end == 1 && draft.yank_ring_index == 0,
         "YankPop clamps a newly completed sequence while retaining the raw replacement endpoint");

  bool const cycled_again = ava::tui::apply_composer_draft_action(draft, TuiAction::YankPop);
  expect(cycled_again && draft.text == "xy" + suffix && draft.cursor == 2 && draft.yank_end == 2,
         "repeated YankPop neither loses nor duplicates the retained suffix after cursor clamping");
  bool const undone = ava::tui::apply_composer_draft_action(draft, TuiAction::Undo);
  expect(undone && draft.text == joined && draft.cursor == 0 && draft.yank_start == std::string::npos && draft.yank_end == std::string::npos,
         "Undo restores the joined text and valid cursor while retiring yank replacement tracking");
  bool const redone = ava::tui::apply_composer_draft_action(draft, TuiAction::Redo);
  expect(redone && draft.text == "xy" + suffix && draft.cursor == 2 && draft.yank_start == std::string::npos && draft.yank_end == std::string::npos,
         "Redo restores the expanded yank without reviving stale replacement tracking");
}

// Owner edits must establish validity without a render or publication repairing the cursor.
void test_composer_input_undo_and_no_change_projection()
{
  std::string const starter = "\xC3";
  std::string const suffix = "\xA9";
  auto const joined = starter + suffix;
  Composer composer;
  static_cast<void>(composer.replace_draft(suffix, 0));
  auto const input = composer.insert_input({.event = {.key = ava::tui::terminal::Key::Character}, .text = starter});
  expect(input.handled, "Composer accepts the starter insertion through decoded input");
  expect_const_projection(composer, joined, 0, "Composer insertion");

  bool const unchanged = composer.replace_draft(joined, 0);
  expect(!unchanged, "replacing an already valid identical draft reports no change");
  expect_const_projection(composer, joined, 0, "same-text replacement");
  bool const no_delete = composer.apply_edit(TuiAction::DeleteBackward);
  expect(!no_delete, "backspace at the normalized start has no preceding character");
  expect_const_projection(composer, joined, 0, "unsuccessful deletion");

  bool const undone = composer.apply_edit(TuiAction::Undo);
  expect(undone, "Composer can undo the fragment insertion");
  expect_const_projection(composer, suffix, 0, "Composer Undo");
  bool const redone = composer.apply_edit(TuiAction::Redo);
  expect(redone, "Composer can redo the fragment insertion");
  expect_const_projection(composer, joined, 0, "Composer Redo");

  composer.reset_draft();
  static_cast<void>(composer.replace_draft(suffix, 0));
  auto const pasted = composer.insert_input({.event = {}, .text = starter, .bracketed_paste = true});
  expect(pasted.handled, "Composer accepts a literal bracketed paste");
  expect_const_projection(composer, joined, 0, "Composer literal paste");
}

// Selected replacement and selected deletion both join byte fragments through the real owner.
void test_composer_selection_mutations_project_const()
{
  std::string const starter = "\xC3";
  std::string const suffix = "\xA9";
  auto const joined = starter + suffix;
  Composer composer;
  static_cast<void>(composer.replace_draft("x" + suffix, 0));
  composer.begin_pointer_selection(0, true);
  composer.extend_pointer_selection(1);
  composer.end_pointer_selection();
  expect_const_projection(composer, "x" + suffix, 1, "selected draft");
  auto const replaced = composer.insert_input({.event = {.key = ava::tui::terminal::Key::Character}, .text = starter});
  expect(replaced.handled && !composer.selection_bounds(), "typing consumes the selected range");
  expect_const_projection(composer, joined, 0, "selected replacement");

  static_cast<void>(composer.replace_draft(starter + "x" + suffix, 1));
  composer.begin_pointer_selection(1, true);
  composer.extend_pointer_selection(2);
  composer.end_pointer_selection();
  bool const erased = composer.apply_edit(TuiAction::DeleteForward);
  expect(erased && !composer.selection_bounds(), "deletion consumes only the selected separator");
  expect_const_projection(composer, joined, 0, "selected deletion");
}

// History, completion, newline and stash transitions must retain the same const-projection contract.
void test_composer_history_stash_and_replacement_projection()
{
  std::string const joined = "\xC3\xA9";
  Composer composer;
  composer.accept_completion(joined, 1);
  expect_const_projection(composer, joined, 0, "completion with an interior requested cursor");
  composer.insert_newline();
  expect_const_projection(composer, "\n" + joined, 1, "newline insertion");
  static_cast<void>(composer.replace_draft("\\" + joined, 1));
  auto const newline = composer.convert_backslash_enter_to_newline();
  expect(newline.handled, "backslash Enter replaces its escape with a newline");
  expect_const_projection(composer, "\n" + joined, 1, "backslash newline replacement");
  composer.draft_command(joined);
  expect_const_projection(composer, joined, joined.size(), "command replacement");

  auto const scratch = "scratch " + joined;
  composer.restore_draft(scratch);
  composer.record_submission(joined);
  bool const recalled = composer.browse_history(true);
  expect(recalled && composer.history_index() == 0, "history recalls the submitted text");
  expect_const_projection(composer, joined, 0, "history recall");
  bool const restored_scratch = composer.browse_history(false);
  expect(restored_scratch && !composer.history_index(), "forward history restores the saved live draft");
  expect_const_projection(composer, scratch, scratch.size(), "history scratch restoration");

  static_cast<void>(composer.replace_draft(joined, 0));
  ava::tui::PromptStash stash;
  auto const stored = stash.push(composer.draft());
  if (!ava::tests::require(stored == ava::tui::PromptStashPushResult::Stored, "stash stores the normalized owner draft"))
    return;
  auto const id = stash.ids_newest_first().front();
  composer.reset_draft();
  expect_const_projection(composer, {}, 0, "draft reset before stash restore");
  auto const restored = composer.restore_stashed(stash, id);
  expect(restored == ava::tui::PromptStashRestoreResult::Restored && stash.empty(), "stash restoration consumes its saved draft");
  expect_const_projection(composer, joined, 0, "stash restoration");
}

// An omitted final summary preserves live facts; an empty live update clears
// them. Both transitions retain bounded path history and the detached sidebar.
void test_run_publications_distinguish_progress_from_summary()
{
  tui_test_support::TuiFrontEndFixture frontend;
  ava::tui::RuntimePresentationState presentation(frontend.frontend(), {});
  ava::tui::TuiEventState progress;
  progress.queued_messages = {{.id = "next", .kind = "follow-up", .text = "queued"}};
  progress.activity = {{.id = "tool", .label = "editing", .detail = "current work"}};
  progress.todos = {{.id = "todo", .content = "retained task"}};
  for (int index = 0; index < 75; ++index)
    progress.modified_files.push_back({.path = "file" + std::to_string(index)});
  presentation.publish_run_progress(progress);
  presentation.publish_visible_sidebar(presentation.sidebar());
  auto const detached = presentation.screen().sidebar;

  ava::tui::TuiEventState final_facts;
  final_facts.modified_files = {{.path = "file74"}, {.path = "final-event"}};
  ava::tui::ToolTimelineItem tool;
  tool.changed_paths = {"file74", "final-tool"};
  presentation.publish_run_summary(final_facts, {tool});
  auto const& summary = presentation.sidebar();
  expect(summary.activity.size() == 1 && summary.activity.front().id == "tool" && summary.todos.size() == 1 && summary.todos.front().id == "todo" &&
             presentation.screen().queued_messages.size() == 1 && presentation.screen().queued_messages.front().id == "next",
         "a command summary with no activity or todos preserves the live facts and queued follow-up");
  expect(summary.modified_files.size() == 50 && summary.modified_files.front().path == "file27" && summary.modified_files[47].path == "file74" &&
             summary.modified_files[48].path == "final-event" && summary.modified_files[49].path == "final-tool",
         "event and final-tool paths merge once and retain only the newest fifty files");

  presentation.publish_run_progress({});
  expect(summary.activity.empty() && summary.todos.empty() && presentation.screen().queued_messages.empty() && summary.modified_files.size() == 50,
         "an empty live update clears current facts without discarding file history");
  auto const& visible = presentation.screen().sidebar;
  expect(visible && detached && visible->activity.size() == detached->activity.size() && visible->todos.size() == detached->todos.size() &&
             visible->modified_files.size() == 50 && visible->modified_files.front().path == "file25" && visible->modified_files.back().path == "file74",
         "run facts do not publish a new visible sidebar before the viewport owner prepares it");
}

// Interrupt clearing must publish its whole edit synchronously, including a false return on empty input.
void test_interrupt_clear_publishes_before_caller_work()
{
  ava::tui::TuiRuntimeOptions options;
  options.file_references = {{.value = "alpha.txt", .description = "Alpha"}, {.value = "alpine.txt", .description = "Alpine"}};
  // All borrowers die before their preceding owners. This case registers real timers but does not paint or dispatch them.
  tui_test_support::TuiFrontEndFixture frontend(options);
  ava::tui::RuntimePresentationState presentation(frontend.frontend(), {});
  Composer composer;
  tui_test_support::TuiEventLoopFixture event_loop;
  ava::tui::RuntimeRenderer renderer(event_loop.tui_timeout_handler(), frontend.frontend(), presentation, composer);
  ava::tui::SelectorHost selectors(presentation);
  ava::tui::PendingAttachments attachments(presentation);
  ava::tui::SessionSelector sessions(*frontend.frontend(), presentation, selectors, renderer, ava::core::Application::instance().terminal_context());
  ava::tui::RuntimeActionController actions(*frontend.frontend(), presentation, composer, renderer, selectors, sessions, attachments);
  Composer const& observed = composer;

  composer.restore_draft("alp");
  presentation.publish_composer(observed);
  auto const forced = composer.force_path_completion(presentation.screen());
  composer.select_completion(1);
  composer.begin_pointer_selection(0, true);
  composer.extend_pointer_selection(3);
  presentation.publish_composer(observed);
  presentation.publish_status("old edit feedback");
  if (!ava::tests::require(forced.handled && presentation.screen().path_completion_force_active && presentation.screen().selected_slash_command_index == 1 &&
                               presentation.screen().input_selection_start == 0 && presentation.screen().input_selection_end == 3,
                           "interrupt fixture publishes a draft with forced completion and selection"))
    return;

  bool const cleared = actions.clear_draft_for_interrupt();
  auto const& screen = presentation.screen();
  expect(cleared && observed.draft().text.empty() && observed.draft().cursor == 0 && screen.input.empty() && screen.input_cursor == 0 &&
             screen.input_selection_start == std::string::npos && screen.input_selection_end == std::string::npos && screen.draft_scroll_offset == 0 &&
             screen.selected_slash_command_index == 0 && !screen.slash_palette_suppressed && !screen.path_completion_force_active && screen.status.empty() &&
             !observed.mouse_selecting(),
         "interrupt clear publishes empty input, cleared selection and completion immediately without caller publication or rendering");

  for (bool const force_completion : {false, true})
  {
    composer.dismiss_completion();
    if (force_completion)
    {
      auto const empty_forced = composer.force_path_completion(presentation.screen());
      if (!ava::tests::require(empty_forced.handled, "empty input offers both forced path candidates"))
        return;
    }
    composer.select_completion(1);
    composer.begin_pointer_selection(0, true);
    presentation.publish_composer(observed);
    presentation.publish_status("empty edit feedback");
    if (!ava::tests::require(screen.input.empty() && screen.selected_slash_command_index == 1 && screen.path_completion_force_active == force_completion &&
                                 screen.slash_palette_suppressed == !force_completion && observed.mouse_selecting(),
                             "empty interrupt fixture retains nondefault completion and pointer interaction"))
      return;

    bool const changed = actions.clear_draft_for_interrupt();
    expect(!changed && observed.draft().text.empty() && observed.draft().cursor == 0 && !observed.mouse_selecting() && screen.input.empty() &&
               screen.input_cursor == 0 && screen.input_selection_start == std::string::npos && screen.input_selection_end == std::string::npos &&
               screen.draft_scroll_offset == 0 && screen.selected_slash_command_index == 0 && !screen.slash_palette_suppressed &&
               !screen.path_completion_force_active && screen.status.empty(),
           "empty interrupt clear returns false while immediately publishing its interaction reset");
  }
}

}  // namespace

// Run under the existing composer suite's initialized terminal; this adds no independent terminal lifecycle.
void run_tui_composer_invariant_tests()
{
  test_editor_mutations_join_utf8_fragments();
  test_yank_pop_preserves_suffix_and_raw_span();
  test_composer_input_undo_and_no_change_projection();
  test_composer_selection_mutations_project_const();
  test_composer_history_stash_and_replacement_projection();
  test_interrupt_clear_publishes_before_caller_work();
  test_run_publications_distinguish_progress_from_summary();
}
