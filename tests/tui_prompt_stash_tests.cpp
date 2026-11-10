#include "sys.h"
#include "support/TuiEventLoopFixture.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/Composer.h"
#include "ava/tui/PendingAttachments.h"
#include "ava/tui/SelectorHost.h"
#include "ava/tui/SessionTransitionCoordinator.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_commands_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/session_grants.h"
#include "ava/session/attachments.h"
#include "ava/core/Application.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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

// Protect stash/edit/restore transitions through the real editor and selector owners.
void test_prompt_stash_runtime_controller()
{
  static_cast<void>(ava::core::Application::instance().terminal_context());
  static_cast<void>(ava::tui::terminal::Context::resizeterm(20, 80));

  ava::tui::TuiRuntimeOptions options;
  auto const key_bindings = ava::tui::default_key_bindings();
  options.initial_transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "backend transcript stays unchanged"}};
  int backend_submit_calls = 0;
  tui_test_support::TuiFrontEndFixture frontend(options);
  frontend.hooks().on_submit = [&](std::string const&, ava::tui::TuiSubmitContext) {
    ++backend_submit_calls;
    return ava::tui::TuiSubmitResult{};
  };
  ava::tui::RuntimePresentationState presentation(frontend.frontend(), {});
  ava::tui::PendingAttachments attachments(presentation);
  ava::tui::TuiSessionGrantRegistry grants;
  ava::tui::Composer draft_state;
  tui_test_support::TuiEventLoopFixture event_loop;
  ava::tui::RuntimeRenderer renderer(event_loop.timer_registry(), frontend.frontend(), presentation, draft_state);
  ava::tui::SelectorHost active_list(presentation);
  ava::tui::RuntimePromptStashController controller(presentation, draft_state, renderer, active_list, key_bindings, attachments);
  ava::tui::RuntimeNavigationController navigation(*frontend.frontend(), presentation, draft_state, renderer);
  ava::tui::TranscriptSearchController transcript_search(presentation, renderer, navigation, active_list);
  ava::tui::RuntimeSubagentWorkspaceController subagent_workspace(*frontend.frontend(), presentation, active_list);
  ava::tui::SessionTransitionCoordinator session_transition(presentation, draft_state, renderer, transcript_search, subagent_workspace, active_list,
                                                            attachments, grants);
  auto const transcript_unchanged = [&]() {
    return presentation.screen().transcript.size() == 1 && presentation.screen().transcript.front().label == "ava" &&
           presentation.screen().transcript.front().text == "backend transcript stays unchanged";
  };

  draft_state.replace_draft("attachment-owned draft", 4);
  attachments.stage(ava::session::ImageAttachmentRef{}, ava::tui::PendingAttachmentItem{});
  expect(controller.trigger() && draft_state.draft().text == "attachment-owned draft" && controller.stash().empty() && transcript_unchanged() &&
             presentation.screen().status == "cannot stash a prompt while image attachments are pending",
         "prompt stash rejects pending image attachments without draft, backend, or transcript mutation and reports the rejection");
  attachments.clear();
  draft_state.record_submission("prior submitted prompt");
  draft_state.replace_draft("history browsing scratch");
  expect(draft_state.browse_history(true) && draft_state.history_index() == 0 && draft_state.browse_history(false) &&
             draft_state.draft().text == "history browsing scratch" && draft_state.browse_history(true) && draft_state.history_index() == 0,
         "prompt stash fixture starts with active history browsing and a saved draft");
  draft_state.replace_draft("attachment-owned draft", 4);
  expect(controller.trigger() && draft_state.draft().text.empty() && controller.stash().size() == 1 && !draft_state.history_index() &&
             !draft_state.selection_bounds() && transcript_unchanged() && presentation.screen().status == "prompt stashed",
         "prompt stash trigger stores and clears a nonempty draft while excluding history-browsing and selection state without transcript mutation");
  // Browsing itself clears selection; check restoration only after the stash assertions.
  expect(!draft_state.browse_history(false) && draft_state.draft().text.empty(), "a stashed draft cannot restore the old history scratch");

  expect(controller.trigger() && active_list.kind() == ava::tui::ActiveSelectList::PromptStash && presentation.screen().select_list.has_value() &&
             presentation.screen().status == "prompt stash opened",
         "prompt stash trigger opens the newest-first selector when the draft is empty and reports the selector state");
  auto const count_before_cancel = controller.stash().size();
  expect(controller.handle_selector_input(ava::tui::InputEvent{.key = ava::tui::terminal::Key::Escape}).value_or(false) &&
             active_list.kind() == ava::tui::ActiveSelectList::None && controller.stash().size() == count_before_cancel &&
             presentation.screen().status == "view canceled",
         "prompt stash selector Esc closes without mutation and reports cancellation");

  draft_state.replace_draft("newest selector draft");
  expect(controller.trigger(), "prompt stash runtime fixture stores a second entry");
  expect(controller.open_selector(), "prompt stash runtime fixture reopens selector");
  expect(controller.handle_selector_input(ava::tui::InputEvent{.key = ava::tui::terminal::Key::ArrowDown}).value_or(false) &&
             controller.handle_selector_input(ava::tui::InputEvent{.key = ava::tui::terminal::Key::Enter}).value_or(false) &&
             draft_state.draft().text == "attachment-owned draft" && controller.stash().size() == 1 &&
             presentation.screen().status == "stashed prompt restored",
         "prompt stash selector restores and removes an arbitrary older entry by selected identity and reports success");

  draft_state.reset_draft();
  auto const backend_session_replaced = frontend.replace_session_identity();
  auto const session_changed = session_transition.apply_and_refresh(frontend.frontend()->read_committed_session_id());
  expect(backend_session_replaced && session_changed && controller.stash().size() == 1 && controller.pop_latest() &&
             draft_state.draft().text == "newest selector draft" && backend_submit_calls == 0 && presentation.screen().status == "stashed prompt restored",
         "prompt stash survives a committed session transition in one runtime process, restores LIFO, and never dispatches backend work");
  draft_state.replace_draft("draft to clear from stash");
  expect(controller.trigger() && controller.stash().size() == 1, "prompt stash runtime fixture stores an entry for clear");
  expect(controller.clear() && controller.stash().empty() && presentation.screen().status == "prompt stash cleared",
         "prompt stash clear command path empties runtime-owned memory and reports success");

  // Selecting text ends history browsing, so check selection cleanup independently.
  draft_state.replace_draft("selected draft", 0);
  draft_state.extend_selection_to(4);
  expect(draft_state.selection_bounds() && controller.trigger() && !draft_state.selection_bounds() && draft_state.draft().text.empty() &&
             controller.stash().size() == 1 && presentation.screen().status == "prompt stashed",
         "prompt stash clears an active composer selection when storing the draft");
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
