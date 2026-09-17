#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_commands_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/session/attachments.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include <curses.h>

namespace {

void test_prompt_stash_store_semantics()
{
  auto const open = ava::tui::runtime_commands::stash_command_argument("/stash");
  auto const pop = ava::tui::runtime_commands::stash_command_argument("  /stash pop  ");
  auto const clear = ava::tui::runtime_commands::stash_command_argument("/stash clear");
  expect(open && open->empty() && pop == std::optional<std::string>{"pop"} && clear == std::optional<std::string>{"clear"} &&
             !ava::tui::runtime_commands::stash_command_argument("/stashed") &&
             ava::tui::runtime_commands::stash_command_argument("/stash invalid") == std::optional<std::string>{"invalid"},
         "prompt stash command parser distinguishes exact open, pop, clear, invalid-argument, and unrelated commands");

  ava::tui::PromptStash stash;
  ava::tui::ComposerDraftState first;
  ava::tui::reset_composer_draft(first, "first prompt", 5);
  ava::tui::ComposerDraftState second;
  ava::tui::reset_composer_draft(second, "second prompt", 7);
  expect(stash.push(first) == ava::tui::PromptStashPushResult::Stored && stash.push(second) == ava::tui::PromptStashPushResult::Stored,
         "prompt stash accepts bounded drafts without persistence");
  auto const newest_first = stash.ids_newest_first();
  ava::tui::ComposerDraftState restored;
  auto const restored_latest = stash.restore_latest(restored);
  expect(newest_first.size() == 2 && newest_first[0] > newest_first[1] && restored_latest == ava::tui::PromptStashRestoreResult::Restored &&
             restored.text == "second prompt" && restored.cursor == 7 && stash.size() == 1,
         "prompt stash restores and removes the newest entry first by monotonic identity");

  ava::tui::reset_composer_draft(restored);
  auto const arbitrary = stash.restore(newest_first[1], restored);
  expect(arbitrary == ava::tui::PromptStashRestoreResult::Restored && restored.text == "first prompt" && stash.empty(),
         "prompt stash restores an arbitrary selected hidden identity rather than preview text");

  expect(stash.push(first) == ava::tui::PromptStashPushResult::Stored, "prompt stash can be reused after restores");
  ava::tui::ComposerDraftState occupied;
  ava::tui::reset_composer_draft(occupied, "keep me");
  auto const retained_id = stash.ids_newest_first().front();
  expect(stash.restore(retained_id, occupied) == ava::tui::PromptStashRestoreResult::NonemptyDraft && occupied.text == "keep me" && stash.size() == 1,
         "prompt stash never overwrites a nonempty draft or removes the rejected entry");
  stash.clear();
  expect(stash.empty() && stash.aggregate_expanded_bytes() == 0, "prompt stash clear removes all process-memory entries");
  expect(stash.push(second) == ava::tui::PromptStashPushResult::Stored && stash.ids_newest_first().front() > retained_id,
         "prompt stash clear never resets or reuses hidden monotonic identities");

  ava::tui::PromptStash entry_bound;
  ava::tui::ComposerDraftState oversized;
  ava::tui::reset_composer_draft(oversized, std::string(ava::tui::PromptStash::kMaxEntryExpandedBytes + 1, 'x'));
  expect(entry_bound.push(oversized) == ava::tui::PromptStashPushResult::EntryTooLarge && entry_bound.empty(),
         "prompt stash rejects expanded entries above 64 KiB without truncation or eviction");
  ava::tui::ComposerDraftState collapsed_oversized;
  expect(ava::tui::insert_composer_paste_text(collapsed_oversized, std::string(ava::tui::PromptStash::kMaxEntryExpandedBytes + 1, 'p')) &&
             collapsed_oversized.text.size() < ava::tui::PromptStash::kMaxEntryExpandedBytes &&
             entry_bound.push(collapsed_oversized) == ava::tui::PromptStashPushResult::EntryTooLarge && entry_bound.empty(),
         "prompt stash applies its per-entry bound to expanded collapsed-paste text rather than the visible marker");
  for (std::size_t index = 0; index < ava::tui::PromptStash::kMaxEntries; ++index)
  {
    ava::tui::ComposerDraftState draft;
    ava::tui::reset_composer_draft(draft, "entry " + std::to_string(index));
    expect(entry_bound.push(draft) == ava::tui::PromptStashPushResult::Stored, "prompt stash accepts entries through its count bound");
  }
  ava::tui::ComposerDraftState seventeenth;
  ava::tui::reset_composer_draft(seventeenth, "no silent eviction");
  expect(entry_bound.push(seventeenth) == ava::tui::PromptStashPushResult::EntryLimitReached && entry_bound.size() == ava::tui::PromptStash::kMaxEntries,
         "prompt stash rejects a seventeenth entry without silently evicting older drafts");

  ava::tui::PromptStash aggregate_bound;
  for (int index = 0; index < 4; ++index)
  {
    ava::tui::ComposerDraftState draft;
    ava::tui::reset_composer_draft(draft, std::string(ava::tui::PromptStash::kMaxEntryExpandedBytes, static_cast<char>('a' + index)));
    expect(aggregate_bound.push(draft) == ava::tui::PromptStashPushResult::Stored, "prompt stash accepts exact-limit entries within aggregate bound");
  }
  ava::tui::ComposerDraftState aggregate_extra;
  ava::tui::reset_composer_draft(aggregate_extra, "x");
  expect(aggregate_bound.aggregate_expanded_bytes() == ava::tui::PromptStash::kMaxAggregateExpandedBytes &&
             aggregate_bound.push(aggregate_extra) == ava::tui::PromptStashPushResult::AggregateLimitReached && aggregate_bound.size() == 4,
         "prompt stash rejects aggregate text above 256 KiB without eviction");

  ava::tui::PromptStash paste_stash;
  ava::tui::ComposerDraftState pasted;
  auto const paste_text = std::string("line 01\nline 02\nline 03\nline 04\nline 05\nline 06\nline 07\nline 08\nline 09\nline 10\nline 11");
  expect(ava::tui::insert_composer_paste_text(pasted, paste_text) && !pasted.paste_entries.empty(),
         "prompt stash test fixture creates a collapsed paste entry");
  auto const marker_text = pasted.text;
  auto const paste_entries = pasted.paste_entries;
  auto const paste_cursor = pasted.cursor;
  auto const expanded = ava::tui::expanded_composer_draft_text(pasted);
  pasted.kill_buffer = "excluded kill buffer";
  pasted.kill_ring = {"excluded kill ring"};
  ava::tui::ComposerDraftSnapshot excluded_undo;
  excluded_undo.text = "excluded undo";
  pasted.undo_stack.push_back(std::move(excluded_undo));
  ava::tui::ComposerDraftSnapshot excluded_redo;
  excluded_redo.text = "excluded redo";
  pasted.redo_stack.push_back(std::move(excluded_redo));
  pasted.yank_start = 0;
  pasted.yank_end = 1;
  expect(paste_stash.push(pasted) == ava::tui::PromptStashPushResult::Stored, "prompt stash stores collapsed paste ownership");
  ava::tui::ComposerDraftState restored_paste;
  expect(paste_stash.restore_latest(restored_paste) == ava::tui::PromptStashRestoreResult::Restored && restored_paste.text == marker_text &&
             restored_paste.cursor == paste_cursor && restored_paste.paste_entries == paste_entries &&
             ava::tui::expanded_composer_draft_text(restored_paste) == expanded && restored_paste.undo_stack.empty() && restored_paste.redo_stack.empty() &&
             restored_paste.kill_buffer.empty() && restored_paste.kill_ring.empty() && restored_paste.yank_start == std::string::npos &&
             restored_paste.yank_end == std::string::npos,
         "prompt stash restores text, cursor, and collapsed-paste ownership but intentionally excludes undo, redo, kill-ring, and selection state");
}

void test_prompt_stash_runtime_controller()
{
  ScopedTmpFile input;
  ScopedTmpFile output;
  auto* screen = newterm("xterm-256color", output.get(), input.get());
  if (!screen)
  {
    expect(false, "prompt stash runtime test creates a private curses screen");
    return;
  }
  static_cast<void>(set_term(screen));
  static_cast<void>(resizeterm(20, 80));

  ava::tui::TuiRuntimeOptions options;
  options.key_bindings = ava::tui::default_key_bindings();
  options.initial_transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "backend transcript stays unchanged"}};
  int backend_submit_calls = 0;
  options.on_submit = [&](std::string const&, ava::tui::TuiSubmitContext) {
    ++backend_submit_calls;
    return ava::tui::TuiSubmitResult{};
  };
  ava::tui::RuntimePresentationState presentation(options);
  ava::tui::RuntimeDraftState draft_state;
  ava::tui::RuntimeRenderer renderer(presentation.snapshot, presentation.sidebar, draft_state);
  auto active_list = ava::tui::ActiveSelectList::None;
  ava::tui::RuntimePromptStashController controller(presentation, draft_state, renderer, active_list, options.key_bindings);
  ava::tui::RuntimeNavigationController navigation(options, presentation.snapshot, presentation.sidebar, draft_state, renderer);
  ava::tui::TranscriptSearchController transcript_search(presentation, renderer, navigation, active_list);
  ava::tui::RuntimeSubagentWorkspaceController subagent_workspace(options, presentation.snapshot);
  auto const transcript_unchanged = [&]() {
    return presentation.snapshot.transcript.size() == 1 && presentation.snapshot.transcript.front().label == "ava" &&
           presentation.snapshot.transcript.front().text == "backend transcript stays unchanged";
  };

  ava::tui::reset_composer_draft(draft_state.draft, "attachment-owned draft", 4);
  presentation.pending_image_attachments.push_back(ava::session::ImageAttachmentRef{});
  expect(controller.trigger() && draft_state.draft.text == "attachment-owned draft" && controller.stash().empty() && transcript_unchanged() &&
             presentation.snapshot.status == "cannot stash a prompt while image attachments are pending",
         "prompt stash rejects pending image attachments without draft, backend, or transcript mutation and reports the rejection");
  presentation.pending_image_attachments.clear();
  draft_state.input_history = {"prior submitted prompt"};
  draft_state.history_index = 0;
  draft_state.draft_input = "history browsing scratch";
  draft_state.draft_selection_anchor = 0;
  draft_state.draft_selection_cursor = 4;
  expect(controller.trigger() && draft_state.draft.text.empty() && controller.stash().size() == 1 && !draft_state.history_index &&
             draft_state.draft_input.empty() && !draft_state.selection_bounds() && transcript_unchanged() && presentation.snapshot.status == "prompt stashed",
         "prompt stash trigger stores and clears a nonempty draft while excluding history-browsing and selection state without transcript mutation");

  expect(controller.trigger() && active_list == ava::tui::ActiveSelectList::PromptStash && presentation.snapshot.select_list.has_value() &&
             presentation.snapshot.status == "prompt stash opened",
         "prompt stash trigger opens the newest-first selector when the draft is empty and reports the selector state");
  auto const count_before_cancel = controller.stash().size();
  expect(controller.handle_selector_input(ava::tui::InputEvent{.key = ava::tui::Key::Escape}).value_or(false) &&
             active_list == ava::tui::ActiveSelectList::None && controller.stash().size() == count_before_cancel &&
             presentation.snapshot.status == "view canceled",
         "prompt stash selector Esc closes without mutation and reports cancellation");

  ava::tui::reset_composer_draft(draft_state.draft, "newest selector draft");
  expect(controller.trigger(), "prompt stash runtime fixture stores a second entry");
  expect(controller.open_selector(), "prompt stash runtime fixture reopens selector");
  expect(controller.handle_selector_input(ava::tui::InputEvent{.key = ava::tui::Key::ArrowDown}).value_or(false) &&
             controller.handle_selector_input(ava::tui::InputEvent{.key = ava::tui::Key::Enter}).value_or(false) &&
             draft_state.draft.text == "attachment-owned draft" && controller.stash().size() == 1 && presentation.snapshot.status == "stashed prompt restored",
         "prompt stash selector restores and removes an arbitrary older entry by selected identity and reports success");

  ava::tui::reset_composer_draft(draft_state.draft);
  ava::tui::TuiRuntimeStateSnapshot next_session;
  next_session.session_id = "next-session";
  next_session.status = "session switched";
  auto const session_changed = ava::tui::apply_runtime_state_snapshot_with_presentation_transition(
      options, presentation, draft_state, renderer, transcript_search, subagent_workspace, active_list, std::move(next_session));
  expect(session_changed && controller.stash().size() == 1 && controller.pop_latest() && draft_state.draft.text == "newest selector draft" &&
             backend_submit_calls == 0 && presentation.snapshot.status == "stashed prompt restored",
         "prompt stash survives an authoritative session snapshot transition in one runtime process, restores LIFO, and never dispatches backend work");
  ava::tui::reset_composer_draft(draft_state.draft, "draft to clear from stash");
  expect(controller.trigger() && controller.stash().size() == 1, "prompt stash runtime fixture stores an entry for clear");
  expect(controller.clear() && controller.stash().empty() && presentation.snapshot.status == "prompt stash cleared",
         "prompt stash clear command path empties runtime-owned memory and reports success");

  static_cast<void>(endwin());
  delscreen(screen);
}

void test_latest_assistant_copy_decisions()
{
  using ava::tui::runtime_transcript::LatestAssistantCopyResult;
  std::vector<ava::tui::TranscriptItem> transcript = {
      ava::tui::TranscriptItem{.label = "you", .text = "draft"},
      ava::tui::TranscriptItem{.label = "ava", .text = "latest answer"},
  };
  std::string copied;
  auto const success = ava::tui::runtime_transcript::copy_latest_assistant_message(transcript, [&](std::string_view text) {
    copied = text;
    return true;
  });
  bool writer_called = false;
  auto const missing = ava::tui::runtime_transcript::copy_latest_assistant_message({}, [&](std::string_view) {
    writer_called = true;
    return true;
  });
  auto oversized_transcript = std::vector<ava::tui::TranscriptItem>{
      ava::tui::TranscriptItem{.label = "assistant", .text = std::string(ava::tui::runtime_transcript::kMaxTerminalClipboardTextBytes + 1, 'x')}};
  auto const oversized = ava::tui::runtime_transcript::copy_latest_assistant_message(oversized_transcript, [&](std::string_view) {
    writer_called = true;
    return true;
  });
  auto const failed = ava::tui::runtime_transcript::copy_latest_assistant_message(transcript, [](std::string_view) { return false; });
  expect(success == LatestAssistantCopyResult::RequestSent && copied == "latest answer" && transcript.size() == 2 && transcript.front().text == "draft" &&
             transcript.back().text == "latest answer" && missing == LatestAssistantCopyResult::NoMessage && oversized == LatestAssistantCopyResult::Oversize &&
             failed == LatestAssistantCopyResult::WriteFailure && !writer_called &&
             ava::tui::runtime_transcript::latest_assistant_copy_status(success) == "last AVA message copy request sent" &&
             ava::tui::runtime_transcript::latest_assistant_copy_status(missing) == "no AVA messages to copy" &&
             ava::tui::runtime_transcript::latest_assistant_copy_status(oversized).find("64 KiB") != std::string::npos &&
             ava::tui::runtime_transcript::latest_assistant_copy_status(failed) == "clipboard copy failed",
         "idle and active-run copy action authority reports request-sent, no-message, oversize, and writer-failure status without claiming delivery or "
         "mutating draft/transcript state");
}

}  // namespace

void run_tui_prompt_stash_tests()
{
  test_prompt_stash_store_semantics();
  test_prompt_stash_runtime_controller();
  test_latest_assistant_copy_decisions();
}
