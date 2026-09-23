#include "sys.h"
#include "support/test_harness.h"
#include "support/test_timeout.h"
#include "support/tui_test_support.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_active_run_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_plugin_ui_internal.h"
#include "ava/tui/runtime_prompts_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_submit_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/runtime_views_internal.h"
#include "ava/tui/terminal.h"
#include "ava/tui/terminal_image.h"
#include "ava/tui/theme.h"
#include "ava/core/thread.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <curses.h>

namespace {

bool test_transcript_search_controller_tail_refresh_avoids_full_layout()
{
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedTmpFile input;
  ScopedTmpFile output;

  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  if (!screen)
    return false;
  static_cast<void>(set_term(screen));
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }
  static_cast<void>(resizeterm(20, 180));

  ava::tui::TuiRuntimeOptions options;
  options.session_id = "search_tail_direct_refresh";
  options.mode = "build";
  options.provider = "fake";
  options.model = "idle-sidebar-model";
  options.workspace = "/workspace/search-tail";
  options.initial_transcript.reserve(ava::tui::kMaxTranscriptItems);
  for (std::size_t index = 0; index < ava::tui::kMaxTranscriptItems; ++index)
  {
    options.initial_transcript.push_back(ava::tui::TranscriptItem{.label = "ava", .text = "retained item [" + std::to_string(index) + "]"});
  }
  ava::tui::RuntimePresentationState presentation(options);
  presentation.snapshot.sidebar = presentation.sidebar;
  ava::tui::RuntimeDraftState draft_state;
  ava::tui::RuntimeRenderer renderer(presentation.snapshot, presentation.sidebar, draft_state);
  ava::tui::RuntimeNavigationController navigation(options, presentation.snapshot, presentation.sidebar, draft_state, renderer);
  auto active_select_list = ava::tui::ActiveSelectList::None;
  ava::tui::TranscriptSearchController controller(presentation, renderer, navigation, active_select_list);

  auto const opened = controller.open("retained item");
  auto const before = controller.diagnostics();
  auto const full_layout_builds = renderer.transcript_layout_cache.layout_build_count;
  renderer.transcript_scroll_offset = 1;
  renderer.defer_detached_transcript_update({}, 0);
  presentation.snapshot.transcript.back().text = "retained item tail replacement";
  ++presentation.snapshot.transcript_generation;
  controller.refresh_after_transcript_mutation(0, ava::tui::kMaxTranscriptItems - 1);
  auto const after = controller.diagnostics();

  auto const direct_refresh_passed =
      opened && presentation.snapshot.select_list && presentation.snapshot.select_list->freeze_underlying_transcript_layout &&
      presentation.snapshot.select_list->items.size() == ava::tui::kMaxTranscriptItems &&
      ava::tui::composer_canvas_layout(presentation.snapshot).content_width == 120 && renderer.transcript_layout_cache.width == 141 &&
      full_layout_builds == 1 && before.authoritative_mutation_item_render_count == 0 && before.projection_build_count == ava::tui::kMaxTranscriptItems &&
      before.layout_position_visit_count == ava::tui::kMaxTranscriptItems && before.match_projection_evaluation_count == ava::tui::kMaxTranscriptItems &&
      before.match_entry_realign_count == 0 && before.match_entry_splice_count == 0 && before.modal_row_build_count == ava::tui::kMaxTranscriptItems &&
      after.authoritative_mutation_item_render_count == before.authoritative_mutation_item_render_count + 1 &&
      after.projection_build_count == before.projection_build_count + 1 && after.layout_position_visit_count == before.layout_position_visit_count + 1 &&
      after.match_projection_evaluation_count == before.match_projection_evaluation_count + 1 &&
      after.match_entry_realign_count == before.match_entry_realign_count && after.match_entry_splice_count == before.match_entry_splice_count + 1 &&
      after.modal_row_build_count == before.modal_row_build_count + 1 && renderer.transcript_layout_cache.layout_build_count == full_layout_builds &&
      renderer.has_deferred_detached_transcript_update();

  auto const scheduled = renderer.request_render(ava::tui::FrameRenderKind::Full);
  auto const flushed = renderer.flush_pending_render();
  auto const after_flush = controller.diagnostics();
  auto const scheduled_render_passed =
      scheduled && flushed && !renderer.render_failed() && renderer.transcript_layout_cache.layout_build_count == full_layout_builds &&
      renderer.transcript_layout_cache.width == 141 && renderer.has_deferred_detached_transcript_update() &&
      after_flush.authoritative_mutation_item_render_count == after.authoritative_mutation_item_render_count &&
      after_flush.projection_build_count == after.projection_build_count && after_flush.layout_position_visit_count == after.layout_position_visit_count &&
      after_flush.match_projection_evaluation_count == after.match_projection_evaluation_count &&
      after_flush.match_entry_realign_count == after.match_entry_realign_count && after_flush.match_entry_splice_count == after.match_entry_splice_count &&
      after_flush.modal_row_build_count == after.modal_row_build_count;

  controller.refresh_after_resize();
  auto const full_sync_passed = presentation.snapshot.select_list && presentation.snapshot.select_list->freeze_underlying_transcript_layout &&
                                presentation.snapshot.select_list->items.size() == ava::tui::kMaxTranscriptItems &&
                                ava::tui::composer_canvas_layout(presentation.snapshot).content_width == 120 && renderer.transcript_layout_cache.width == 141 &&
                                renderer.transcript_layout_cache.layout_build_count == full_layout_builds + 1 &&
                                !renderer.has_deferred_detached_transcript_update();

  static_cast<void>(endwin());
  delscreen(screen);
  return direct_refresh_passed && scheduled_render_passed && full_sync_passed;
}

bool test_changed_session_snapshot_resets_presentation()
{
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedTmpFile input;
  ScopedTmpFile output;

  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  if (!screen)
    return false;
  static_cast<void>(set_term(screen));
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }
  static_cast<void>(resizeterm(18, 96));

  std::size_t job_list_calls = 0;
  ava::tui::TuiRuntimeOptions options;
  options.session_id = "session_old_presentation";
  options.mode = "build";
  options.provider = "fake";
  options.model = "old-model";
  options.workspace = "/workspace/old";
  options.reasoning_status_provider = []() { return std::optional<std::string>("high"); };
  options.initial_transcript = {
      ava::tui::TranscriptItem{.label = "you", .text = "OLD-TRANSCRIPT-MARKER"},
      ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success, .name = "read_file"}},
  };
  options.list_subagents = [&]() -> ava::core::Result<ava::tui::SelectListView> {
    ++job_list_calls;
    auto view = ava::tui::SelectListView{};
    view.title = "Application jobs";
    return view;
  };

  ava::tui::RuntimePresentationState presentation(options);
  presentation.snapshot.sidebar = presentation.sidebar;
  presentation.snapshot.queued_messages = {{.id = "queued-old", .kind = "follow-up", .text = "old queued message"}};
  presentation.snapshot.sidebar_drawer_visible = true;
  presentation.snapshot.sidebar_drawer_scroll_offset = 4;
  presentation.sidebar.activity = {{.id = "activity-old", .label = "tool", .detail = "old activity"}};
  presentation.sidebar.modified_files = {{.path = "old.cpp"}};
  presentation.sidebar.session_entry_count = 9;
  presentation.snapshot.input = "old composer draft";
  presentation.snapshot.input_cursor = presentation.snapshot.input.size();
  presentation.snapshot.input_selection_start = 0;
  presentation.snapshot.input_selection_end = 3;
  presentation.snapshot.transcript_selection_anchor_item = 0;
  presentation.snapshot.transcript_selection_focus_item = 1;
  presentation.snapshot.command_output = ava::tui::CommandOutputView{.title_token = "/read", .blocks = {"old local output"}};
  presentation.snapshot.local_command_feedback = "old local status";
  presentation.snapshot.tool_index = {ava::tui::TuiToolIndexEntry{
      .tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success, .name = "read", .call_id = "old-local-tool"},
      .origin = ava::tui::TuiToolIndexOrigin::LocalCommand}};
  presentation.snapshot.evicted_tool_index_identities = {"call\nold-evicted-tool"};
  presentation.snapshot.next_tool_index_sequence = 7;

  ava::tui::RuntimeDraftState draft_state;
  draft_state.input_history = {"old session prompt"};
  draft_state.draft.text = presentation.snapshot.input;
  draft_state.draft.cursor = presentation.snapshot.input_cursor;
  draft_state.draft_selection_anchor = 0;
  draft_state.draft_selection_cursor = 3;
  draft_state.draft_input = "history scratch";
  draft_state.history_index = 0;
  draft_state.jump_mode = ava::tui::ComposerJumpMode::Forward;
  draft_state.draft_scroll_offset = 2;
  ava::tui::RuntimeRenderer renderer(presentation.snapshot, presentation.sidebar, draft_state);
  ava::tui::RuntimeNavigationController navigation(options, presentation.snapshot, presentation.sidebar, draft_state, renderer);
  auto active_select_list = ava::tui::ActiveSelectList::None;
  ava::tui::TranscriptSearchController transcript_search(presentation, renderer, navigation, active_select_list);
  ava::tui::RuntimeSubagentWorkspaceController subagent_workspace(options, presentation.snapshot);

  auto const search_opened = transcript_search.open("OLD-TRANSCRIPT-MARKER");
  renderer.transcript_scroll_offset = 3;
  renderer.detached_new_output_count = 2;
  renderer.detached_sidebar_snapshot = presentation.sidebar;
  renderer.defer_detached_transcript_update({}, 1);
  renderer.transcript_layout_cache.valid = true;
  auto const previous_generation = presentation.snapshot.transcript_generation;

  auto changed = ava::tui::apply_runtime_state_snapshot_with_presentation_transition(
      options, presentation, draft_state, renderer, transcript_search, subagent_workspace, active_select_list,
      ava::tui::TuiRuntimeStateSnapshot{.mode = "plan",
                                        .provider = "fake-next",
                                        .model = "new-model",
                                        .session_id = "session_new_presentation",
                                        .session_path = "/sessions/new.jsonl",
                                        .workspace = "/workspace/new",
                                        .git_branch = "new-branch",
                                        .status = "new session receipt",
                                        .todos = {{.id = "todo-new", .content = "new todo", .status = ava::tui::TodoStatus::InProgress}}});

  auto history_probe = ava::tui::ComposerDraftState{};
  auto history_probe_index = std::optional<std::size_t>{};
  auto history_probe_scratch = std::string{};
  auto const history_restored =
      ava::tui::browse_composer_input_history(history_probe, draft_state.input_history, history_probe_index, history_probe_scratch, true);
  auto const changed_state_ok =
      changed && search_opened && presentation.snapshot.session_id == "session_new_presentation" && presentation.snapshot.mode == "plan" &&
      presentation.snapshot.provider == "fake-next" && presentation.snapshot.model == "new-model" && presentation.snapshot.status == "new session receipt" &&
      presentation.snapshot.reasoning_status == std::optional<std::string>("high") && presentation.snapshot.transcript.empty() &&
      presentation.snapshot.transcript_generation == previous_generation + 1 && presentation.snapshot.queued_messages.empty() &&
      presentation.sidebar.activity.empty() && presentation.sidebar.modified_files.empty() && presentation.sidebar.todos.size() == 1 &&
      presentation.sidebar.todos.front().content == "new todo" && presentation.sidebar.session_entry_count == std::nullopt && draft_state.draft.text.empty() &&
      draft_state.input_history.empty() && !history_restored && !draft_state.selection_bounds() && draft_state.draft_input.empty() &&
      !draft_state.history_index && draft_state.jump_mode == ava::tui::ComposerJumpMode::None && draft_state.draft_scroll_offset == 0 &&
      presentation.snapshot.input.empty() && presentation.snapshot.input_cursor == 0 && presentation.snapshot.input_selection_start == std::string::npos &&
      presentation.snapshot.input_selection_end == std::string::npos && presentation.snapshot.transcript_selection_anchor_item == std::string::npos &&
      presentation.snapshot.transcript_selection_focus_item == std::string::npos && !presentation.snapshot.command_output &&
      !presentation.snapshot.local_command_feedback && presentation.snapshot.tool_index.empty() &&
      presentation.snapshot.evicted_tool_index_identities.empty() && presentation.snapshot.next_tool_index_sequence == 0 &&
      !presentation.snapshot.sidebar_drawer_visible && presentation.snapshot.sidebar_drawer_scroll_offset == 0 && renderer.transcript_scroll_offset == 0 &&
      renderer.detached_new_output_count == 0 && !renderer.detached_sidebar_snapshot && !renderer.has_deferred_detached_transcript_update() &&
      !renderer.transcript_layout_cache.valid && !transcript_search.is_open() && active_select_list == ava::tui::ActiveSelectList::None;

  presentation.snapshot.transcript = {{.label = "ava", .text = "new receipt only"}};
  ++presentation.snapshot.transcript_generation;
  presentation.snapshot.queued_messages = {{.id = "queued-new", .kind = "follow-up", .text = "new queued message"}};
  presentation.sidebar.activity = {{.id = "activity-new", .label = "tool", .detail = "new activity"}};
  auto const unchanged_generation = presentation.snapshot.transcript_generation;
  auto unchanged = ava::tui::apply_runtime_state_snapshot_with_presentation_transition(
      options, presentation, draft_state, renderer, transcript_search, subagent_workspace, active_select_list,
      ava::tui::TuiRuntimeStateSnapshot{.mode = "build",
                                        .provider = "fake-next",
                                        .model = "same-session-model-update",
                                        .session_id = "session_new_presentation",
                                        .session_path = "/sessions/new.jsonl",
                                        .workspace = "/workspace/new",
                                        .git_branch = "new-branch",
                                        .status = "same session update",
                                        .todos = {{.id = "todo-same", .content = "same-session todo"}}});
  auto const unchanged_state_ok = !unchanged && presentation.snapshot.transcript_generation == unchanged_generation &&
                                  presentation.snapshot.transcript.size() == 1 && presentation.snapshot.transcript.front().text == "new receipt only" &&
                                  presentation.snapshot.queued_messages.size() == 1 && presentation.sidebar.activity.size() == 1 &&
                                  presentation.snapshot.model == "same-session-model-update" && presentation.sidebar.todos.size() == 1 &&
                                  presentation.sidebar.todos.front().content == "same-session todo";

  auto const jobs_still_available = subagent_workspace.open_selector() && subagent_workspace.active() && job_list_calls == 1;

  static_cast<void>(endwin());
  delscreen(screen);
  return changed_state_ok && unchanged_state_ok && jobs_still_available;
}

bool test_active_run_session_transition_discards_prior_session_events()
{
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedTmpFile input;
  ScopedTmpFile output;

  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  if (!screen)
    return false;
  static_cast<void>(set_term(screen));
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }
  static_cast<void>(resizeterm(18, 96));

  bool finish_called = false;
  bool initial_identity_forwarded = false;
  ava::tui::TuiRuntimeOptions options;
  options.session_id = "session_old_active_run";
  options.mode = "build";
  options.provider = "fake";
  options.model = "old-model";
  options.workspace = "/workspace/old";
  options.initial_transcript = {
      ava::tui::TranscriptItem{.label = "ava", .text = "OLD-INITIAL-TRANSCRIPT"},
      ava::tui::TranscriptItem{.tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success, .name = "old_initial_tool"}},
  };
  options.initial_todos = {{.id = "old-todo", .content = "OLD TODO", .status = ava::tui::TodoStatus::InProgress}};
  options.create_active_run_queues = [&finish_called](ava::event::EventEnvelopeSink sink) {
    auto queued = ava::event::EventEnvelope{};
    queued.schema_version = 1;
    queued.event_id = "old-queued-event";
    queued.session_id = "session_old_active_run";
    queued.request_id = "old-follow-up";
    queued.correlation_id = "old-request";
    queued.name = "follow_up_queued";
    queued.payload_json = R"({"message":"OLD QUEUED MESSAGE"})";
    static_cast<void>(sink(queued));

    auto queues = ava::tui::TuiActiveRunQueues{};
    queues.active_request_id = "old-request";
    queues.finish = [&finish_called, sink](bool) {
      finish_called = true;
      auto skipped = ava::event::EventEnvelope{};
      skipped.schema_version = 1;
      skipped.event_id = "old-finish-event";
      skipped.session_id = "session_old_active_run";
      skipped.request_id = "old-follow-up";
      skipped.correlation_id = "old-request";
      skipped.name = "follow_up_skipped";
      skipped.payload_json = R"({"message":"OLD FINISH RECEIPT","reason":"run_completed_before_safe_point"})";
      return sink(skipped);
    };
    return queues;
  };
  options.on_submit = [&initial_identity_forwarded](std::string const&, ava::tui::TuiSubmitContext context) {
    initial_identity_forwarded = context.request_id == "old-request" && static_cast<bool>(context.on_subagent_launch);
    context.on_subagent_launch(
        ava::agent::SubagentLaunchNotification{.tool_call_id = "old-tool-call",
                                               .request_id = "old-request",
                                               .correlation_id = "old-request",
                                               .display = ava::agent::SubagentLaunchDisplay::normalized("OLD PRIVATE LAUNCH", std::string_view("high"))});
    auto old_message = ava::event::MessagePayload{};
    old_message.text = "OLD EVENT TRANSCRIPT";
    static_cast<void>(context.event_sink(ava::event::RuntimeEvent{{}, ava::event::AssistantMessageEvent{.payload = std::move(old_message)}}));
    auto old_tool = ava::event::ToolPayload{};
    old_tool.text = "OLD TOOL RESULT";
    old_tool.call_id = "old-tool-call";
    old_tool.tool = "write_file";
    old_tool.status = "success";
    old_tool.changed_paths = {"old-event.cpp"};
    static_cast<void>(context.event_sink(ava::event::RuntimeEvent{{}, ava::event::ToolResultEvent{.payload = std::move(old_tool)}}));
    auto state = ava::tui::TuiRuntimeStateSnapshot{};
    state.mode = "plan";
    state.provider = "fake-next";
    state.model = "new-model";
    state.session_id = "session_new_active_run";
    state.session_path = "/sessions/new.jsonl";
    state.workspace = "/workspace/new";
    state.git_branch = "new-branch";
    state.status = "transition ready";
    state.todos = {{.id = "new-todo", .content = "NEW TODO", .status = ava::tui::TodoStatus::Pending}};
    auto result = ava::tui::TuiSubmitResult{};
    result.output = {"NEW-SESSION-RECEIPT"};
    result.context_source_count = 7;
    result.state_snapshot = std::move(state);
    return result;
  };

  ava::tui::RuntimePresentationState presentation(options);
  presentation.snapshot.sidebar = presentation.sidebar;
  presentation.snapshot.queued_messages = {{.id = "old-preloaded-queue", .kind = "follow-up", .text = "OLD PRELOADED QUEUE"}};
  presentation.sidebar.activity = {{.id = "old-activity", .label = "tool", .detail = "OLD ACTIVITY"}};
  presentation.sidebar.modified_files = {{.path = "old-preloaded.cpp"}};
  ava::tui::RuntimeDraftState draft_state;
  draft_state.input_history = {"OLD HISTORY PROMPT"};
  ava::tui::RuntimeRenderer renderer(presentation.snapshot, presentation.sidebar, draft_state);
  ava::tui::RuntimeNavigationController navigation(options, presentation.snapshot, presentation.sidebar, draft_state, renderer);
  auto active_select_list = ava::tui::ActiveSelectList::None;
  ava::tui::TranscriptSearchController transcript_search(presentation, renderer, navigation, active_select_list);
  std::optional<ava::tui::PendingSessionArchiveAction> session_archive_confirmation;
  ava::tui::RuntimePromptCoordinator prompt_coordinator(options, presentation.snapshot, presentation.command_session_grants, renderer);
  ava::tui::RuntimePromptStashController prompt_stash(presentation, draft_state, renderer, active_select_list, options.key_bindings);
  ava::tui::RuntimePluginUiCoordinator plugin_ui;
  ava::tui::RuntimeActionController action_controller(options, presentation, draft_state, renderer, active_select_list, session_archive_confirmation);
  ava::tui::RuntimeSubagentWorkspaceController subagent_workspace(options, presentation.snapshot);
  ava::tui::RuntimeActiveRunController active_run(options, presentation, draft_state, renderer, prompt_coordinator, prompt_stash, plugin_ui, navigation,
                                                  action_controller, transcript_search, subagent_workspace);

  auto const outcome = active_run.run("OLD SUBMITTED PROMPT");
  auto history_probe = ava::tui::ComposerDraftState{};
  auto history_index = std::optional<std::size_t>{};
  auto history_scratch = std::string{};
  auto const history_restored = ava::tui::browse_composer_input_history(history_probe, draft_state.input_history, history_index, history_scratch, true);
  auto const passed = !outcome.break_loop && !outcome.terminal_write_failed && finish_called && initial_identity_forwarded &&
                      presentation.snapshot.session_id == "session_new_active_run" && presentation.snapshot.mode == "plan" &&
                      presentation.snapshot.transcript.size() == 1 && presentation.snapshot.transcript.front().text == "NEW-SESSION-RECEIPT" &&
                      !presentation.snapshot.transcript.front().tool && presentation.snapshot.queued_messages.empty() &&
                      presentation.sidebar.activity.empty() && presentation.sidebar.modified_files.empty() && presentation.sidebar.todos.size() == 1 &&
                      presentation.sidebar.todos.front().id == "new-todo" && presentation.snapshot.status == "done" &&
                      presentation.snapshot.context_source_count == std::optional<std::size_t>{7} &&
                      presentation.sidebar.context_source_count == std::optional<std::size_t>{7} && draft_state.input_history.empty() && !history_restored;

  static_cast<void>(endwin());
  delscreen(screen);
  return passed;
}

bool test_atomic_search_input_prompt_precedence()
{
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedTmpFile input;
  ScopedTmpFile output;

  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  if (!screen)
    return false;
  static_cast<void>(set_term(screen));
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }
  static_cast<void>(resizeterm(12, 80));

  ava::tui::TuiRuntimeOptions options;
  options.session_id = "search_prompt_race";
  ava::tui::ComposerSnapshot snapshot;
  snapshot.session_id = options.session_id;
  snapshot.width = 80;
  snapshot.height = 12;
  ava::tui::SidebarSnapshot sidebar;
  ava::tui::RuntimeDraftState draft_state;
  ava::tui::RuntimeRenderer renderer(snapshot, sidebar, draft_state);
  ava::tui::TuiSessionGrantRegistry session_grants;
  ava::tui::RuntimePromptCoordinator coordinator(options, snapshot, session_grants, renderer);
  auto resolver = coordinator.question_resolver();

  bool passed = true;
  for (std::size_t iteration = 0; iteration < 50 && passed; ++iteration)
  {
    std::mutex mutex;
    std::condition_variable changed;
    bool start_provider = false;
    bool provider_attempted = false;
    bool dispatch_returned = false;
    bool provider_resolved = false;
    bool provider_timed_out = false;
    std::vector<std::string> order;
    ava::core::Result<ava::agent::QuestionAnswer> answer = std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question resolver did not run"));
    ava::agent::QuestionPrompt prompt{
        .header = "Atomic prompt", .question = "Resolve after retained search input", .options = {{.value = "done", .label = "Done"}}, .auto_resolve = [&]() {
          std::lock_guard lock(mutex);
          order.push_back("resolver");
          return true;
        }};

    ava::core::JoinThread provider = ava::core::JoinThread::create("provider", [&]() {
      {
        std::unique_lock lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(1), [&]() { return start_provider; }))
        {
          provider_timed_out = true;
          changed.notify_all();
          return;
        }
        provider_attempted = true;
      }
      changed.notify_all();
      answer = resolver(prompt);
      {
        std::lock_guard lock(mutex);
        provider_resolved = true;
        if (!dispatch_returned)
          order.push_back("resolved-before-dispatch-returned");
      }
      changed.notify_all();
    });

    auto const initial = coordinator.dispatch_search_input_with_prompt_precedence([&]() {
      std::unique_lock lock(mutex);
      order.push_back("search-input");
      start_provider = true;
      changed.notify_all();
      if (!changed.wait_for(lock, std::chrono::seconds(1), [&]() { return provider_attempted || provider_timed_out; }))
        provider_timed_out = true;
      return !provider_timed_out && !provider_resolved;
    });
    {
      std::lock_guard lock(mutex);
      dispatch_returned = true;
    }
    changed.notify_all();
    std::this_thread::yield();

    auto queued = ava::tui::SearchInputPromptDispatchResult::InputHandled;
    bool queued_attempt_dispatched = false;
    auto const claim_deadline = ava::tests::now_plus_seconds(1);
    while (queued == ava::tui::SearchInputPromptDispatchResult::InputHandled && std::chrono::steady_clock::now() < claim_deadline)
    {
      queued_attempt_dispatched = false;
      queued = coordinator.dispatch_search_input_with_prompt_precedence(
          [&]() {
            queued_attempt_dispatched = true;
            return true;
          },
          {}, {},
          [&]() {
            std::lock_guard lock(mutex);
            order.push_back("before-prompt");
          });
      if (queued == ava::tui::SearchInputPromptDispatchResult::InputHandled)
        std::this_thread::yield();
    }
    {
      std::unique_lock lock(mutex);
      if (!changed.wait_for(lock, std::chrono::seconds(1), [&]() { return provider_resolved || provider_timed_out; }))
        provider_timed_out = true;
    }
    passed = initial == ava::tui::SearchInputPromptDispatchResult::InputHandled && queued == ava::tui::SearchInputPromptDispatchResult::PromptServiced &&
             !queued_attempt_dispatched && !provider_timed_out && provider_resolved && answer &&
             order == std::vector<std::string>({"search-input", "before-prompt", "resolver"});
    if (!passed)
      coordinator.fail_pending_requests();
  }

  static_cast<void>(endwin());
  delscreen(screen);
  return passed;
}

bool test_transcript_message_boundary_navigation_and_live_tail_reset()
{
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedTmpFile input;
  ScopedTmpFile output;

  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  if (!screen)
    return false;
  static_cast<void>(set_term(screen));
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }
  static_cast<void>(resizeterm(24, 80));

  ava::tui::TuiRuntimeOptions options;
  options.session_id = "message-boundary-nav";
  options.mode = "build";
  options.provider = "fake";
  options.model = "nav-model";
  options.workspace = "/workspace/message-boundary";
  options.key_bindings = ava::tui::default_key_bindings();
  auto tall_message = [](std::string_view name, int lines) {
    std::string text(name);
    for (int index = 0; index < lines; ++index)
      text += "\n" + std::string(name) + " line " + std::to_string(index);
    return text;
  };
  options.initial_transcript = {
      ava::tui::TranscriptItem{.label = "you", .text = tall_message("alpha", 12)},
      ava::tui::TranscriptItem{.label = "ava", .text = tall_message("beta", 14)},
      ava::tui::TranscriptItem{.label = "you", .text = tall_message("gamma", 12)},
      ava::tui::TranscriptItem{.label = "ava", .text = tall_message("delta", 16)},
  };

  ava::tui::RuntimePresentationState presentation(options);
  presentation.snapshot.sidebar = presentation.sidebar;
  presentation.snapshot.input = "NAV-DRAFT-KEEP";
  presentation.snapshot.input_cursor = presentation.snapshot.input.size();
  ava::tui::RuntimeDraftState draft_state;
  draft_state.draft.text = presentation.snapshot.input;
  draft_state.draft.cursor = presentation.snapshot.input_cursor;
  ava::tui::RuntimeRenderer renderer(presentation.snapshot, presentation.sidebar, draft_state);
  ava::tui::RuntimeNavigationController navigation(options, presentation.snapshot, presentation.sidebar, draft_state, renderer);

  navigation.jump_to_bottom("live tail");
  auto const live_before_wheel = renderer.transcript_scroll_offset;
  navigation.scroll_up(ava::tui::kTranscriptWheelScrollRows);
  auto const wheel_up_offset = renderer.transcript_scroll_offset;
  bool const wheel_up_detached = wheel_up_offset == ava::tui::kTranscriptWheelScrollRows && renderer.detached_sidebar_snapshot.has_value();
  navigation.scroll_down(ava::tui::kTranscriptWheelScrollRows);
  auto const wheel_down_live = renderer.transcript_scroll_offset;
  bool const wheel_reverse_reattached =
      live_before_wheel == 0 && wheel_down_live == 0 && !renderer.detached_sidebar_snapshot.has_value() && renderer.detached_new_output_count == 0;
  navigation.scroll_up(ava::tui::kTranscriptWheelScrollRows);
  navigation.scroll_up(ava::tui::kTranscriptWheelScrollRows);
  auto const two_wheel_ups = renderer.transcript_scroll_offset;
  navigation.scroll_down(1);
  auto const one_row_down = renderer.transcript_scroll_offset;
  bool const wheel_step_is_three =
      two_wheel_ups == ava::tui::kTranscriptWheelScrollRows * 2 && one_row_down + 1 == two_wheel_ups && ava::tui::kTranscriptWheelScrollRows == 3;

  navigation.scroll_up(1000);
  auto const oldest_offset = renderer.transcript_scroll_offset;
  auto const draft_before = draft_state.draft.text;
  auto const cursor_before = draft_state.draft.cursor;
  bool const detached = oldest_offset > 0;
  auto const overscroll_offset = renderer.transcript_scroll_offset;
  navigation.scroll_up(ava::tui::kTranscriptWheelScrollRows);
  bool const wheel_clamps_at_oldest = renderer.transcript_scroll_offset == overscroll_offset;

  navigation.scroll_to_message_boundary(true);
  auto const oldest_status = presentation.snapshot.status;
  auto const oldest_boundary_offset = renderer.transcript_scroll_offset;

  navigation.scroll_to_message_boundary(false);
  auto const next_status = presentation.snapshot.status;
  auto const next_offset = renderer.transcript_scroll_offset;
  auto const max_scroll = oldest_offset;
  auto const next_start = max_scroll - std::min(max_scroll, next_offset);
  auto const next_position = std::ranges::find(renderer.transcript_layout_cache.layout.message_starts, next_start);
  auto const next_layout_position = static_cast<std::size_t>(next_position - renderer.transcript_layout_cache.layout.message_starts.begin());
  auto const next_item_index = next_layout_position < renderer.transcript_layout_cache.layout.message_item_indices.size()
                                   ? renderer.transcript_layout_cache.layout.message_item_indices[next_layout_position]
                                   : std::string::npos;

  navigation.scroll_to_message_boundary(true);
  auto const prev_status = presentation.snapshot.status;
  auto const prev_offset = renderer.transcript_scroll_offset;
  auto const prev_start = max_scroll - std::min(max_scroll, prev_offset);
  auto const prev_position = std::ranges::find(renderer.transcript_layout_cache.layout.message_starts, prev_start);
  auto const prev_layout_position = static_cast<std::size_t>(prev_position - renderer.transcript_layout_cache.layout.message_starts.begin());
  auto const prev_item_index = prev_layout_position < renderer.transcript_layout_cache.layout.message_item_indices.size()
                                   ? renderer.transcript_layout_cache.layout.message_item_indices[prev_layout_position]
                                   : std::string::npos;

  navigation.scroll_to_message_boundary(false);
  navigation.scroll_to_message_boundary(false);
  auto const past_last_status = presentation.snapshot.status;
  auto const past_last_offset = renderer.transcript_scroll_offset;

  navigation.jump_to_bottom("live tail");
  auto const live_status = presentation.snapshot.status;
  auto const live_offset = renderer.transcript_scroll_offset;

  navigation.scroll_to_message_boundary(false);
  auto const already_live_status = presentation.snapshot.status;
  auto const already_live_offset = renderer.transcript_scroll_offset;

  bool const draft_unchanged =
      draft_state.draft.text == draft_before && draft_state.draft.cursor == cursor_before && presentation.snapshot.input == draft_before;

  // Half-page navigation uses the actual transcript body after a wrapped composer
  // consumes terminal rows, while ordinary three-row scrolling remains draft-sovereign.
  static_cast<void>(resizeterm(12, 40));
  draft_state.draft.text = std::string(180, 'x');
  draft_state.draft.cursor = 73;
  presentation.snapshot.input = draft_state.draft.text;
  presentation.snapshot.input_cursor = draft_state.draft.cursor;
  auto const wrapped_draft_before = draft_state.draft.text;
  auto const wrapped_cursor_before = draft_state.draft.cursor;
  auto const page = navigation.transcript_page_size();
  auto const body = ava::tui::detail::transcript_body_screen_geometry(presentation.snapshot);
  navigation.jump_to_bottom("live tail");
  navigation.scroll_up(page);
  auto const page_offset = renderer.transcript_scroll_offset;
  navigation.jump_to_bottom("live tail");
  navigation.scroll_up(3);
  auto const plain_arrow_step_offset = renderer.transcript_scroll_offset;
  navigation.scroll_up(100000);
  auto const bounded_oldest = renderer.transcript_scroll_offset;
  navigation.scroll_up(page);
  auto const clamped_oldest = renderer.transcript_scroll_offset;
  navigation.scroll_down(100000);
  auto const clamped_live_tail = renderer.transcript_scroll_offset;
  static_cast<void>(resizeterm(4, 40));
  auto const tiny_page = navigation.transcript_page_size();
  bool const body_page_and_plain_arrow_sovereignty =
      body.valid && page == std::max<std::size_t>(1, body.transcript_height / 2) && page != std::size_t{6} && page_offset == page &&
      plain_arrow_step_offset == 3 && bounded_oldest > 0 && clamped_oldest == bounded_oldest && clamped_live_tail == 0 && tiny_page == 1 &&
      draft_state.draft.text == wrapped_draft_before && draft_state.draft.cursor == wrapped_cursor_before &&
      !ava::tui::key_matches_action(options.key_bindings, ava::tui::TuiAction::HistoryPrev, ava::tui::Key::ArrowUp) &&
      !ava::tui::key_matches_action(options.key_bindings, ava::tui::TuiAction::HistoryNext, ava::tui::Key::ArrowDown) &&
      !ava::tui::key_matches_action(options.key_bindings, ava::tui::TuiAction::CursorUp, ava::tui::Key::ArrowUp) &&
      !ava::tui::key_matches_action(options.key_bindings, ava::tui::TuiAction::CursorDown, ava::tui::Key::ArrowDown);

  bool const passed = wheel_up_detached && wheel_reverse_reattached && wheel_step_is_three && wheel_clamps_at_oldest && detached &&
                      oldest_status == "oldest retained user turn" && oldest_boundary_offset == oldest_offset && next_offset < oldest_offset &&
                      next_offset > 0 && next_status == "next retained user turn" && prev_offset > next_offset &&
                      prev_status == "previous retained user turn" && live_offset == 0 && next_item_index == 2 && prev_item_index == 0 &&
                      past_last_offset == 0 && past_last_status == "live tail" && live_status == "live tail" && already_live_offset == 0 &&
                      already_live_status == "live tail" && draft_unchanged && body_page_and_plain_arrow_sovereignty &&
                      ava::tui::key_matches_action(options.key_bindings, ava::tui::TuiAction::MessagePrev, ava::tui::Key::AltK) &&
                      ava::tui::key_matches_action(options.key_bindings, ava::tui::TuiAction::MessageNext, ava::tui::Key::AltJ) &&
                      ava::tui::key_matches_action(options.key_bindings, ava::tui::TuiAction::JumpToBottom, ava::tui::Key::CtrlEnd);

  static_cast<void>(endwin());
  delscreen(screen);
  return passed;
}

bool test_detached_completion_publish_preserves_numbered_window()
{
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedTmpFile input;
  ScopedTmpFile output;

  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  if (!screen)
    return false;
  static_cast<void>(set_term(screen));
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }
  static_cast<void>(resizeterm(32, 120));

  auto numbered_window = [](std::vector<std::string> const& surfaces) {
    std::vector<int> numbers;
    for (auto const& surface : surfaces)
    {
      auto const visible = strip_sgr(surface);
      auto const marker = visible.find("stream line ");
      if (marker == std::string::npos || marker + 15 > visible.size())
        continue;
      auto const digits = visible.substr(marker + 12, 3);
      if (digits.size() != 3 || !std::ranges::all_of(digits, [](unsigned char ch) { return std::isdigit(ch) != 0; }))
        continue;
      numbers.push_back(std::stoi(digits));
    }
    return numbers;
  };

  auto build_stream_text = [](int first_inclusive, int last_inclusive) {
    std::string text;
    for (int index = first_inclusive; index <= last_inclusive; ++index)
    {
      if (!text.empty())
        text.push_back('\n');
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "stream line %03d", index);
      text += buffer;
    }
    return text;
  };

  ava::tui::TuiRuntimeOptions options;
  options.session_id = "detached-completion-geometry";
  options.mode = "build";
  options.provider = "fake";
  options.model = "geometry-model";
  options.workspace = "/workspace/detached-completion";
  options.key_bindings = ava::tui::default_key_bindings();
  options.initial_transcript = {ava::tui::TranscriptItem{.label = "ava", .text = build_stream_text(0, 29)}};

  ava::tui::RuntimePresentationState presentation(options);
  presentation.snapshot.sidebar = presentation.sidebar;
  presentation.snapshot.processing = true;
  presentation.snapshot.input = "STREAM-DRAFT-KEEP";
  presentation.snapshot.input_cursor = presentation.snapshot.input.size();
  presentation.snapshot.active_run_hint = ava::tui::runtime_views::active_run_hint_for(options.key_bindings);
  ava::tui::RuntimeDraftState draft_state;
  draft_state.draft.text = presentation.snapshot.input;
  draft_state.draft.cursor = presentation.snapshot.input_cursor;
  ava::tui::RuntimeRenderer renderer(presentation.snapshot, presentation.sidebar, draft_state);

  if (!renderer.render())
  {
    static_cast<void>(endwin());
    delscreen(screen);
    return false;
  }

  auto const [width, height] = ava::tui::terminal_size();
  presentation.snapshot.width = width;
  presentation.snapshot.height = height;
  auto const max_scroll = ava::tui::detail::composer_max_transcript_scroll_offset_cached(
      presentation.snapshot, width, height, renderer.completion_cache, presentation.snapshot.file_references_generation, renderer.transcript_layout_cache,
      presentation.snapshot.transcript_generation);
  if (max_scroll == 0)
  {
    static_cast<void>(endwin());
    delscreen(screen);
    return false;
  }

  // Detach while the active-run contextual row is already reserved (processing=true, count=0).
  renderer.transcript_scroll_offset = std::min<std::size_t>(3, max_scroll);
  renderer.detached_sidebar_snapshot = presentation.sidebar;
  presentation.snapshot.transcript_scroll_offset = renderer.transcript_scroll_offset;
  presentation.snapshot.transcript_new_output_count = 0;
  if (!renderer.render())
  {
    static_cast<void>(endwin());
    delscreen(screen);
    return false;
  }

  auto const detached_scroll = renderer.transcript_scroll_offset;
  auto const detached_max_scroll = ava::tui::detail::composer_max_transcript_scroll_offset_cached(
      presentation.snapshot, width, height, renderer.completion_cache, presentation.snapshot.file_references_generation, renderer.transcript_layout_cache,
      presentation.snapshot.transcript_generation);
  auto const anchor = ava::tui::detail::capture_transcript_viewport_anchor(renderer.transcript_layout_cache.layout, detached_max_scroll, detached_scroll);
  auto const before_numbers = numbered_window(renderer.screen_row_cache.surfaces);
  auto const draft_before = draft_state.draft.text;
  auto const snapshot_count_before = presentation.snapshot.transcript_new_output_count;

  // Stream continues while detached without publishing count into the snapshot (UpdatedNoRender path).
  constexpr int kNewOutputCount = 35;
  presentation.snapshot.transcript.back().text += "\n" + build_stream_text(30, 30 + kNewOutputCount - 1);
  ++presentation.snapshot.transcript_generation;
  renderer.defer_detached_transcript_update(anchor, 0);
  renderer.detached_new_output_count = static_cast<std::size_t>(kNewOutputCount);
  presentation.snapshot.transcript_new_output_count = 0;
  presentation.snapshot.processing = false;

  auto const rendered = renderer.render();
  auto const after_numbers = numbered_window(renderer.screen_row_cache.surfaces);
  auto const surfaces_joined = tui_test_support::join_visible_lines(renderer.screen_row_cache.surfaces);
  auto const hint_visible =
      surfaces_joined.find(std::to_string(kNewOutputCount) + " new") != std::string::npos && surfaces_joined.find("Ctrl+End") != std::string::npos;
  bool const passed = rendered && !renderer.render_failed() && snapshot_count_before == 0 && detached_scroll > 0 && anchor.valid && !before_numbers.empty() &&
                      before_numbers == after_numbers && hint_visible && !renderer.has_deferred_detached_transcript_update() &&
                      presentation.snapshot.transcript_new_output_count == static_cast<std::size_t>(kNewOutputCount) &&
                      presentation.snapshot.transcript_scroll_offset == renderer.transcript_scroll_offset && renderer.transcript_scroll_offset > 0 &&
                      draft_state.draft.text == draft_before && presentation.snapshot.input == draft_before && !presentation.snapshot.processing;

  static_cast<void>(endwin());
  delscreen(screen);
  return passed;
}

bool test_message_boundary_navigation_on_empty_or_fitting_transcript_is_harmless()
{
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedTmpFile input;
  ScopedTmpFile output;

  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  if (!screen)
    return false;
  static_cast<void>(set_term(screen));
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }
  static_cast<void>(resizeterm(24, 80));

  auto exercise = [](std::vector<ava::tui::TranscriptItem> transcript, std::string_view session_id) {
    ava::tui::TuiRuntimeOptions options;
    options.session_id = std::string(session_id);
    options.mode = "build";
    options.provider = "fake";
    options.model = "nav-model";
    options.workspace = "/workspace/message-boundary-fit";
    options.key_bindings = ava::tui::default_key_bindings();
    options.initial_transcript = std::move(transcript);

    ava::tui::RuntimePresentationState presentation(options);
    presentation.snapshot.sidebar = presentation.sidebar;
    presentation.snapshot.input = "FIT-DRAFT-KEEP";
    presentation.snapshot.input_cursor = presentation.snapshot.input.size();
    ava::tui::RuntimeDraftState draft_state;
    draft_state.draft.text = presentation.snapshot.input;
    draft_state.draft.cursor = presentation.snapshot.input_cursor;
    ava::tui::RuntimeRenderer renderer(presentation.snapshot, presentation.sidebar, draft_state);
    ava::tui::RuntimeNavigationController navigation(options, presentation.snapshot, presentation.sidebar, draft_state, renderer);

    auto const draft_before = draft_state.draft.text;
    auto const cursor_before = draft_state.draft.cursor;
    auto const offset_before = renderer.transcript_scroll_offset;

    navigation.scroll_to_message_boundary(true);
    auto const prev_status = presentation.snapshot.status;
    auto const prev_offset = renderer.transcript_scroll_offset;

    navigation.scroll_to_message_boundary(false);
    auto const next_status = presentation.snapshot.status;
    auto const next_offset = renderer.transcript_scroll_offset;

    bool const draft_unchanged =
        draft_state.draft.text == draft_before && draft_state.draft.cursor == cursor_before && presentation.snapshot.input == draft_before;
    auto const has_user =
        std::ranges::any_of(presentation.snapshot.transcript, [](ava::tui::TranscriptItem const& item) { return item.label == "you" && !item.tool; });
    auto const expected_prev = has_user ? std::string("oldest retained user turn") : std::string("no retained user turns");
    auto const expected_next = has_user ? std::string("live tail") : std::string("no retained user turns");
    return offset_before == 0 && prev_offset == 0 && next_offset == 0 && prev_status == expected_prev && next_status == expected_next && draft_unchanged;
  };

  bool const empty_ok = exercise({}, "message-boundary-empty");
  bool const fitting_ok =
      exercise({ava::tui::TranscriptItem{.label = "you", .text = "short alpha"}, ava::tui::TranscriptItem{.label = "ava", .text = "short beta"}},
               "message-boundary-fitting");

  static_cast<void>(endwin());
  delscreen(screen);
  return empty_ok && fitting_ok;
}

}  // namespace

void run_tui_prompt_search_race_tests()
{
  expect(
      test_changed_session_snapshot_resets_presentation(),
      "an authoritative changed-session snapshot clears old transcript/tool rows, advances generation, resets queued/sidebar/draft/selection/search/scroll and "
      "frozen presentation state through their owners, applies the new session/model/reasoning/todos, preserves application-scoped jobs, and leaves an "
      "unchanged-session transcript intact");
  expect(
      test_active_run_session_transition_discards_prior_session_events(),
      "an active-run authoritative session transition discards old submitted/event/tool/queue/finish/sidebar/todo state, resets event receipt suppression and "
      "history, and presents only the new-session receipt and hydrated state");
  expect(test_transcript_search_controller_tail_refresh_avoids_full_layout(),
         "an open 1,000-item transcript search over a roomy 180x20 idle-sidebar layout keeps the captured 141-column transcript geometry while its modal "
         "uses the 120-column canvas, then a shift-zero tail refresh directly renders and updates exactly one authoritative item/projection/match/modal row "
         "without rebuilding the renderer layout or synchronizing its deferred viewport; a forced full scheduled render still freezes that underlying layout, "
         "retains the +1 direct work, and leaves the deferred viewport pending; a later explicit full synchronization projects only the search modal, "
         "rebuilds the same 141-column underlying layout once, restores the modal, and consumes the deferred viewport");
  expect(test_atomic_search_input_prompt_precedence(),
         "actual prompt-coordinator locking linearizes retained search input before provider enqueue, then lets the queued prompt discard stale input and run "
         "before-prompt ahead of nested resolution across 50 synchronized repetitions");
  // After virtual-terminal smoke: full RuntimeRenderer::render initializes the static color-pair cache.
  expect(test_detached_completion_publish_preserves_numbered_window(),
         "detached completion publishes N-new chrome authority before sync so the numbered transcript window and draft stay put while the deferred viewport is "
         "consumed");
}

bool test_display_settings_reload_poll_outcome_and_preview_staging()
{
  // Reproduces W2-001 through RuntimeActionController + preview reapply: optional snapshot is the
  // applied signal (no value inference), hydrate does not final-render, overlay is staged before paint.
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedEnvVar no_color_guard("NO_COLOR", "");
  ScopedEnvVar theme_env_guard("AVA_TUI_THEME", "");
  ScopedTmpFile input;
  ScopedTmpFile output;
  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  if (!screen)
    return false;
  static_cast<void>(set_term(screen));
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }
  static_cast<void>(resizeterm(24, 100));

  ava::tui::clear_tui_theme_preview();
  ava::tui::set_tui_config_theme("dark");

  int applied_callback_count = 0;
  ava::tui::TuiRuntimeOptions options;
  options.mode = "build";
  options.provider = "openai";
  options.model = "gpt-5.5";
  options.session_id = "session_display_reload";
  options.session_path = "/tmp/session_display_reload.jsonl";
  options.workspace = "/workspace";
  options.show_images = true;
  options.image_width_cells = 60;
  options.custom_themes = {ava::tui::ThemeOptionItem{
      .name = "sunrise",
      .detail = "/tmp/ava/sunrise.json",
      .palette =
          ava::tui::TuiThemePalette{.text = -1, .muted = 242, .success = 34, .warning = 220, .error = 196, .accent = 39, .screen_bg = 255, .composer_bg = 236},
      .revision = "sunrise-v1"}};
  options.on_maybe_reload_display_settings = [&]() -> ava::core::Result<std::optional<ava::tui::TuiRuntimeStateSnapshot>> {
    ++applied_callback_count;
    // Authoritative reload to show_images=false while an images-off overlay is already active
    // (presentation already false). Applied must come from the optional snapshot, not value diffs.
    return std::optional<ava::tui::TuiRuntimeStateSnapshot>{ava::tui::TuiRuntimeStateSnapshot{
        .mode = "build",
        .provider = "openai",
        .model = "gpt-5.5",
        .session_id = "session_display_reload",
        .session_path = "/tmp/session_display_reload.jsonl",
        .workspace = "/workspace",
        .git_branch = "develop",
        .status = "display theme auto-reloaded",
        .custom_themes = {ava::tui::ThemeOptionItem{
            .name = "sunrise",
            .detail = "/tmp/ava/sunrise.json",
            .palette =
                ava::tui::TuiThemePalette{
                    .text = -1, .muted = 242, .success = 34, .warning = 220, .error = 196, .accent = 39, .screen_bg = 254, .composer_bg = 237},
            .revision = "sunrise-v2"}},
        .show_images = false,
        .image_width_cells = 60}};
  };

  ava::tui::RuntimePresentationState presentation(options);
  ava::tui::RuntimeDraftState draft_state;
  ava::tui::RuntimeRenderer renderer(presentation.snapshot, presentation.sidebar, draft_state);
  auto active_select_list = ava::tui::ActiveSelectList::None;
  std::optional<ava::tui::PendingSessionArchiveAction> session_archive_confirmation;
  ava::tui::RuntimeActionController action_controller(options, presentation, draft_state, renderer, active_select_list, session_archive_confirmation);

  using ava::tui::runtime_views::DisplayPresentationBaseline;
  using ava::tui::runtime_views::DisplayPreviewTransaction;
  using ava::tui::runtime_views::reapply_settings_preview_after_display_reload;
  using ava::tui::runtime_views::settings_preview_overlay_for_action;

  DisplayPreviewTransaction preview;
  preview.begin(DisplayPresentationBaseline{.show_images = true, .image_width_cells = 60});
  if (auto off = settings_preview_overlay_for_action("settings:images.off", presentation.snapshot))
    preview.update(std::move(*off));
  preview.apply_image_overlay(presentation.snapshot);
  // Stage sunrise so the applied custom-theme catalog refresh is visible before any final render.
  if (auto sunrise = settings_preview_overlay_for_action("theme:sunrise", presentation.snapshot))
    preview.update(std::move(*sunrise));
  preview.apply_image_overlay(presentation.snapshot);

  auto const outcome = action_controller.maybe_reload_display_settings();
  bool const applied_signal = outcome == ava::tui::DisplaySettingsReloadPollOutcome::Applied && applied_callback_count == 1 &&
                              !presentation.snapshot.show_images && presentation.snapshot.custom_themes.size() == 1 &&
                              presentation.snapshot.custom_themes.front().revision == "sunrise-v2";
  // Hydrate-only: theme overlay must still be the pre-reapply staged identity until caller restages.
  // set_tui_theme_preview appends a generation suffix to the live active revision.
  bool const hydrate_kept_prior_overlay =
      ava::tui::tui_theme_preview_active() && ava::tui::active_tui_theme().name == "sunrise" && ava::tui::active_tui_theme().revision.find("sunrise-v1") == 0;

  reapply_settings_preview_after_display_reload(preview, presentation.snapshot);
  bool const staged_before_render = preview.authoritative.show_images == false && preview.active() && preview.overlay && preview.overlay->theme &&
                                    preview.overlay->theme->revision == "sunrise-v2" && ava::tui::active_tui_theme().revision.find("sunrise-v2") == 0 &&
                                    ava::tui::active_tui_theme().palette && ava::tui::active_tui_theme().palette->composer_bg == 237 &&
                                    !presentation.snapshot.show_images;

  // Final paint happens only after overlay staging (controller did not final-render on Applied).
  bool const rendered = renderer.render();

  preview.cancel();
  preview.apply_image_overlay(presentation.snapshot);
  bool const esc_restores_new_authority = !preview.active() && !presentation.snapshot.show_images && !ava::tui::tui_theme_preview_active();

  // Unchanged poll uses explicit nullopt signal.
  options.on_maybe_reload_display_settings = []() -> ava::core::Result<std::optional<ava::tui::TuiRuntimeStateSnapshot>> {
    return std::optional<ava::tui::TuiRuntimeStateSnapshot>{};
  };
  // Force the rate limit open for a second poll.
  std::this_thread::sleep_for(std::chrono::milliseconds(510));
  auto const unchanged_after_wait = action_controller.maybe_reload_display_settings();
  bool const unchanged_signal = unchanged_after_wait == ava::tui::DisplaySettingsReloadPollOutcome::Unchanged;

  ava::tui::clear_tui_theme_preview();
  ava::tui::set_tui_config_theme(std::nullopt);
  static_cast<void>(endwin());
  delscreen(screen);
  return applied_signal && hydrate_kept_prior_overlay && staged_before_render && rendered && esc_restores_new_authority && unchanged_signal;
}

bool test_display_settings_reload_rebuilds_open_startup_overview()
{
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedTmpFile input;
  ScopedTmpFile output;
  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  if (!screen)
    return false;
  set_term(screen);
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }
  static_cast<void>(resizeterm(24, 100));

  ava::tui::clear_tui_theme_preview();
  ava::tui::set_tui_config_theme("dark");

  ava::tui::StartupOverviewSnapshot initial_overview{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .theme_name = "dark",
      .theme_badge = "built-in",
      .compact_line = "build · openai/gpt-5.5 · dark · /overview",
  };
  ava::tui::StartupOverviewSnapshot refreshed_overview = initial_overview;
  refreshed_overview.theme_name = "sunrise";
  refreshed_overview.theme_badge = "custom";
  refreshed_overview.compact_line = "build · openai/gpt-5.5 · sunrise · /overview";

  int applied_callback_count = 0;
  ava::tui::TuiRuntimeOptions options;
  options.mode = "build";
  options.provider = "openai";
  options.model = "gpt-5.5";
  options.session_id = "session_overview_display_reload";
  options.session_path = "/tmp/session_overview_display_reload.jsonl";
  options.workspace = "/workspace";
  options.startup_overview = initial_overview;
  options.on_maybe_reload_display_settings = [&]() -> ava::core::Result<std::optional<ava::tui::TuiRuntimeStateSnapshot>> {
    ++applied_callback_count;
    return std::optional<ava::tui::TuiRuntimeStateSnapshot>{ava::tui::TuiRuntimeStateSnapshot{
        .mode = "build",
        .provider = "openai",
        .model = "gpt-5.5",
        .session_id = "session_overview_display_reload",
        .session_path = "/tmp/session_overview_display_reload.jsonl",
        .workspace = "/workspace",
        .git_branch = "develop",
        .status = "display theme auto-reloaded",
        .custom_themes = {ava::tui::ThemeOptionItem{
            .name = "sunrise",
            .detail = "/tmp/ava/sunrise.json",
            .palette =
                ava::tui::TuiThemePalette{
                    .text = -1, .muted = 242, .success = 34, .warning = 220, .error = 196, .accent = 39, .screen_bg = 254, .composer_bg = 237},
            .revision = "sunrise-v2"}},
        .startup_overview = refreshed_overview,
    }};
  };

  ava::tui::RuntimePresentationState presentation(options);
  presentation.snapshot.startup_overview = initial_overview;
  presentation.snapshot.select_list = ava::tui::overview_select_list_view(initial_overview);
  presentation.snapshot.select_list->query = "theme";
  presentation.snapshot.select_list->selected_item_index = 0;
  ava::tui::RuntimeDraftState draft_state;
  ava::tui::RuntimeRenderer renderer(presentation.snapshot, presentation.sidebar, draft_state);
  auto active_select_list = ava::tui::ActiveSelectList::Overview;
  std::optional<ava::tui::PendingSessionArchiveAction> session_archive_confirmation;
  ava::tui::RuntimeActionController action_controller(options, presentation, draft_state, renderer, active_select_list, session_archive_confirmation);

  auto const outcome = action_controller.maybe_reload_display_settings();
  bool const rebuilt = outcome == ava::tui::DisplaySettingsReloadPollOutcome::Applied && applied_callback_count == 1 &&
                       active_select_list == ava::tui::ActiveSelectList::Overview && presentation.snapshot.startup_overview &&
                       presentation.snapshot.startup_overview->theme_name == "sunrise" && presentation.snapshot.select_list &&
                       presentation.snapshot.select_list->query == "theme" &&
                       std::ranges::any_of(presentation.snapshot.select_list->items,
                                           [](auto const& item) { return item.group == "Display" && item.label == "Theme" && item.detail == "sunrise"; }) &&
      // Open overview suppresses the idle transcript receipt; status still updates.
                       presentation.snapshot.status == "display theme auto-reloaded" &&
                       std::ranges::none_of(presentation.snapshot.transcript,
                                            [](auto const& item) { return item.text.find("display theme auto-reloaded") != std::string::npos; });

  // Outer idle path still owns settings-preview rebase + single final paint after Applied.
  bool const rendered = renderer.render();

  ava::tui::clear_tui_theme_preview();
  ava::tui::set_tui_config_theme(std::nullopt);
  static_cast<void>(endwin());
  delscreen(screen);
  return rebuilt && rendered;
}

bool test_reload_submit_routes_backend_targets_and_keeps_local_hot_reload()
{
  ScopedEnvVar term_guard("TERM", "xterm-256color");
  ScopedTmpFile input;
  ScopedTmpFile output;

  SCREEN* screen = newterm(nullptr, output.get(), input.get());
  if (!screen)
    return false;
  static_cast<void>(set_term(screen));
  if (has_colors())
  {
    static_cast<void>(start_color());
    static_cast<void>(use_default_colors());
  }
  static_cast<void>(resizeterm(18, 96));

  std::vector<std::string> backend_submissions;
  int display_reload_calls = 0;
  int keybinding_reload_calls = 0;
  ava::tui::TuiRuntimeOptions options;
  options.session_id = "session_reload_routing";
  options.on_submit = [&backend_submissions](std::string const& submitted, ava::tui::TuiSubmitContext) {
    backend_submissions.push_back(submitted);
    ava::tui::TuiSubmitResult result;
    result.output = {"backend handled " + submitted};
    return result;
  };
  options.on_reload_display_settings = [&display_reload_calls]() -> ava::core::Result<ava::tui::TuiRuntimeStateSnapshot> {
    ++display_reload_calls;
    ava::tui::TuiRuntimeStateSnapshot state;
    state.session_id = "session_reload_routing";
    state.status = "display theme reloaded";
    return state;
  };
  options.on_reload_key_bindings = [&keybinding_reload_calls]() -> ava::core::Result<ava::tui::TuiKeyBindingReloadResult> {
    ++keybinding_reload_calls;
    ava::tui::TuiKeyBindingReloadResult reloaded;
    reloaded.state.session_id = "session_reload_routing";
    reloaded.state.status = "keybindings reloaded";
    return reloaded;
  };

  ava::tui::RuntimePresentationState presentation(options);
  ava::tui::RuntimeDraftState draft_state;
  ava::tui::RuntimeRenderer renderer(presentation.snapshot, presentation.sidebar, draft_state);
  ava::tui::RuntimeNavigationController navigation(options, presentation.snapshot, presentation.sidebar, draft_state, renderer);
  auto active_select_list = ava::tui::ActiveSelectList::None;
  ava::tui::TranscriptSearchController transcript_search(presentation, renderer, navigation, active_select_list);
  std::optional<ava::tui::PendingSessionArchiveAction> session_archive_confirmation;
  ava::tui::RuntimePromptCoordinator prompt_coordinator(options, presentation.snapshot, presentation.command_session_grants, renderer);
  ava::tui::RuntimePromptStashController prompt_stash(presentation, draft_state, renderer, active_select_list, options.key_bindings);
  ava::tui::RuntimePluginUiCoordinator plugin_ui;
  ava::tui::RuntimeActionController action_controller(options, presentation, draft_state, renderer, active_select_list, session_archive_confirmation);
  ava::tui::RuntimeSubagentWorkspaceController subagent_workspace(options, presentation.snapshot);
  ava::tui::RuntimeActiveRunController active_run(options, presentation, draft_state, renderer, prompt_coordinator, prompt_stash, plugin_ui, navigation,
                                                  action_controller, transcript_search, subagent_workspace);
  ava::tui::RuntimeSubmitController submit_controller(options, presentation, draft_state, renderer, navigation, action_controller, active_run, prompt_stash,
                                                      transcript_search, subagent_workspace, active_select_list);

  auto continued = [](ava::tui::RuntimeSubmitOutcome const& outcome) {
    return outcome.disposition == ava::tui::RuntimeSubmitDisposition::ContinueLoop && !outcome.terminal_write_failed;
  };

  auto const models = submit_controller.submit(std::string("/reload models"));
  bool const models_ok =
      continued(models) && backend_submissions == std::vector<std::string>{"/reload models"} && display_reload_calls == 0 && keybinding_reload_calls == 0;

  auto const malformed = submit_controller.submit(std::string("/reload no-such"));
  bool const malformed_ok = continued(malformed) && backend_submissions == std::vector<std::string>{"/reload models", "/reload no-such"} &&
                            display_reload_calls == 0 && keybinding_reload_calls == 0;

  auto const theme = submit_controller.submit(std::string("/reload theme"));
  bool const theme_ok = continued(theme) && backend_submissions.size() == 2 && display_reload_calls == 1 && keybinding_reload_calls == 0 &&
                        presentation.snapshot.status == "display theme reloaded";

  auto const keybindings = submit_controller.submit(std::string("/reload keybindings"));
  bool const keybindings_ok = continued(keybindings) && backend_submissions.size() == 2 && display_reload_calls == 1 && keybinding_reload_calls == 1 &&
                              presentation.snapshot.status == "keybindings reloaded";

  auto const bare = submit_controller.submit(std::string("/reload"));
  bool const bare_ok = continued(bare) && backend_submissions.size() == 2 && display_reload_calls == 1 && keybinding_reload_calls == 2 &&
                       presentation.snapshot.status == "keybindings reloaded";

  static_cast<void>(endwin());
  delscreen(screen);
  return models_ok && malformed_ok && theme_ok && keybindings_ok && bare_ok;
}

void run_tui_composer_rendering_tests_part_1()
{
  expect(test_reload_submit_routes_backend_targets_and_keeps_local_hot_reload(),
         "TUI /reload keeps theme and keybinding (including bare /reload) on the local hot-reload path while supported backend targets and malformed "
         "targets reach ordinary backend submission");
  expect(test_display_settings_reload_poll_outcome_and_preview_staging(),
         "display reload poll uses optional snapshot as applied/unchanged signal, hydrates without final render, restages overlay before paint, and Esc "
         "restores new authority even when overlay values equal the hydrated baseline");
  expect(test_display_settings_reload_rebuilds_open_startup_overview(),
         "applied periodic display reload rebuilds an open startup overview from the refreshed DTO while preserving query/selection and skipping the idle "
         "transcript receipt");
  expect(
      test_transcript_message_boundary_navigation_and_live_tail_reset(),
      "transcript user-turn navigation clamps at the oldest retained user turn, skips assistant turns, advances/retreats across retained user turns, resets to "
      "live tail, applies the shared three-row transcript wheel step with reverse reattach and hard clamp, and leaves the composer draft untouched while "
      "defaults keep MessagePrev/Next/JumpToBottom on Alt+K/Alt+J/Ctrl+End");
  expect(test_message_boundary_navigation_on_empty_or_fitting_transcript_is_harmless(),
         "message-prev/message-next on empty or fitting transcripts stay at offset 0 with truthful retained-user/live-tail status and leave the composer "
         "draft untouched");
  {
    auto const started_at = std::chrono::steady_clock::time_point{};
    ava::tui::detail::ActiveRunCadence cadence(started_at);
    auto const input_before_frame = ava::tui::detail::active_run_input_read_decision(true, cadence.frame_due(started_at + std::chrono::milliseconds(15)));
    auto const input_at_frame = ava::tui::detail::active_run_input_read_decision(true, cadence.frame_due(started_at + std::chrono::milliseconds(16)));
    auto const no_input_before_frame = ava::tui::detail::active_run_input_read_decision(false, cadence.frame_due(started_at + std::chrono::milliseconds(15)));
    auto const early = cadence.advance(started_at + std::chrono::milliseconds(15));
    auto const first_frame = cadence.advance(started_at + std::chrono::milliseconds(16));
    auto const before_spinner = cadence.advance(started_at + std::chrono::milliseconds(119));
    auto const first_spinner = cadence.advance(started_at + std::chrono::milliseconds(120));
    auto const delayed = cadence.advance(started_at + std::chrono::milliseconds(257));
    expect(ava::tui::detail::kActiveRunFrameDelay == std::chrono::milliseconds(16) &&
               ava::tui::detail::kProcessingIndicatorFrameDelay == std::chrono::milliseconds(120) &&
               cadence.wait_duration(started_at + std::chrono::milliseconds(257)) == std::chrono::milliseconds(15) && early.elapsed_frames == 0 &&
               early.elapsed_spinner_frames == 0 && first_frame.elapsed_frames == 1 && first_frame.elapsed_spinner_frames == 0 &&
               before_spinner.elapsed_frames == 6 && before_spinner.elapsed_spinner_frames == 0 && first_spinner.elapsed_frames == 0 &&
               first_spinner.elapsed_spinner_frames == 1 && delayed.elapsed_frames == 9 && delayed.elapsed_spinner_frames == 1 &&
               input_before_frame == ava::tui::detail::ActiveRunInputReadDecision::DrainBufferedInput &&
               input_at_frame == ava::tui::detail::ActiveRunInputReadDecision::ServiceFrame &&
               no_input_before_frame == ava::tui::detail::ActiveRunInputReadDecision::WaitForNextFrame,
           "active-run cadence drains buffered input before 16ms, but a buffered input at the frame deadline services provider updates before input drainage "
           "while the spinner advances at 120ms boundaries");
  }
  {
    bool input_dispatched = false;
    auto const retained =
        ava::tui::detail::dispatch_retained_input_with_prompt_precedence([]() { return ava::tui::detail::PendingPromptServiceResult::Serviced; },
                                                                         [&]() {
                                                                           input_dispatched = true;
                                                                           return true;
                                                                         });
    expect(retained == ava::tui::detail::RetainedInputDispatchResult::PromptServiced && !input_dispatched,
           "ordinary retained input keeps the second prompt check and discards the event when that check claims a prompt");
  }
  {
    using ava::tui::detail::ActiveRunCancelDisposition;
    expect(ava::tui::detail::active_run_cancel_disposition(true, false) == ActiveRunCancelDisposition::ClearDraftSelection &&
               ava::tui::detail::active_run_cancel_disposition(true, true) == ActiveRunCancelDisposition::ClearDraftSelection &&
               ava::tui::detail::active_run_cancel_disposition(false, true) == ActiveRunCancelDisposition::ClearTranscriptSelection &&
               ava::tui::detail::active_run_cancel_disposition(false, false) == ActiveRunCancelDisposition::RequestStop,
           "active-run cancel clears a composer selection before a transcript selection and requests stop only after both are absent");
  }
  {
    auto const unchanged =
        ava::tui::detail::changed_screen_rows({"row 0", "\x1b[1mrow 1\x1b[0m", "row 2"}, {"row 0", "\x1b[1mrow 1\x1b[0m", "row 2"}, {}, false);
    auto const styled_change =
        ava::tui::detail::changed_screen_rows({"row 0", "\x1b[1mrow 1\x1b[0m", "row 2"}, {"row 0", "\x1b[4mrow 1\x1b[0m", "row 2"}, {}, false);
    auto const disappeared = ava::tui::detail::changed_screen_rows({"row 0", "row 1", "row 2"}, {"row 0", "row 1"}, {}, false);
    auto const invalidated = ava::tui::detail::changed_screen_rows({"same", "same"}, {"same", "same"}, {}, true);
    ava::tui::detail::ScreenRowCache footer_cache{.surfaces = {"header", "body", "footer"}, .valid = true};
    ava::tui::detail::mark_screen_row_dirty(footer_cache, 2);
    auto const footer_repaint = ava::tui::detail::changed_screen_rows(footer_cache.surfaces, {"header", "body", "footer"}, footer_cache.dirty_rows, false);
    expect(unchanged.empty() && styled_change == std::vector<std::size_t>{1} && disappeared == std::vector<std::size_t>{2} &&
               invalidated == std::vector<std::size_t>({0, 1}) && footer_repaint == std::vector<std::size_t>{2},
           "screen-row diff skips unchanged complete surfaces, notices styling-only changes, clears disappeared rows, supports full invalidation, and repaints "
           "only a directly drawn footer");
  }
  {
    constexpr std::string_view kLegacyScreenRgb = "\x1b[48;2;11;14;20m";
    auto const reset = std::string(ava::tui::detail::kSgrReset);
    auto const bold = std::string(ava::tui::detail::kSgrBold);
    auto const screen_line = ava::tui::detail::screen_surface_line(bold + "hi" + reset + "there", 20);
    auto const composer_line = ava::tui::detail::composer_surface_line("draft", 20);
    auto const first_default_bg = screen_line.find(std::string(ava::tui::detail::kSgrScreenBg));
    auto const reset_at = screen_line.find(reset);
    auto const reapplied_default_bg =
        reset_at == std::string::npos ? std::string::npos : screen_line.find(std::string(ava::tui::detail::kSgrScreenBg), reset_at + reset.size());
    auto const ordinary_dock = ava::tui::detail::render_composer_block(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                  .provider = "openai",
                                                                                                  .model = "gpt-5.5",
                                                                                                  .session_id = "session_ordinary_dock_bg",
                                                                                                  .input = "draft",
                                                                                                  .status = "ready",
                                                                                                  .transcript = {},
                                                                                                  .width = 40,
                                                                                                  .height = 12},
                                                                       40, 2);
    expect(ava::tui::detail::kSgrScreenBg == "\x1b[49m" && first_default_bg == 0 && reset_at != std::string::npos &&
               reapplied_default_bg != std::string::npos && screen_line.find(kLegacyScreenRgb) == std::string::npos &&
               composer_line.find("\x1b[48;2;26;31;46m") != std::string::npos && composer_line.find("\x1b[49m") == std::string::npos &&
               ordinary_dock.size() == 2 &&
               std::ranges::all_of(
                   ordinary_dock,
                   [](std::string const& line) { return line.find("\x1b[49m") != std::string::npos && line.find("\x1b[48;2;26;31;46m") == std::string::npos; }),
           "screen_surface_line uses and reapplies SGR 49 for the terminal default background instead of the legacy hard-coded screen RGB; ordinary composer "
           "dock rows inherit that screen background while elevated composer contrast surfaces remain explicitly styled");
  }
  {
    using Clock = ava::tui::WheelBurstGovernor::Clock;
    auto const started_at = Clock::time_point{};
    ava::tui::WheelBurstGovernor governor;
    auto const first_up = ava::tui::runtime_wheel_input_accepted(governor, ava::tui::Key::MouseWheelUp, started_at);
    auto const same_direction_at_16 = ava::tui::runtime_wheel_input_accepted(governor, ava::tui::Key::MouseWheelUp, started_at + std::chrono::milliseconds(16));
    auto const same_direction_at_39 = ava::tui::runtime_wheel_input_accepted(governor, ava::tui::Key::MouseWheelUp, started_at + std::chrono::milliseconds(39));
    auto const same_direction_at_40 = ava::tui::runtime_wheel_input_accepted(governor, ava::tui::Key::MouseWheelUp, started_at + std::chrono::milliseconds(40));
    auto const reverse_immediate = ava::tui::runtime_wheel_input_accepted(governor, ava::tui::Key::MouseWheelDown, started_at + std::chrono::milliseconds(40));
    auto const same_reverse_within_window =
        ava::tui::runtime_wheel_input_accepted(governor, ava::tui::Key::MouseWheelDown, started_at + std::chrono::milliseconds(56));
    auto const same_reverse_at_interval =
        ava::tui::runtime_wheel_input_accepted(governor, ava::tui::Key::MouseWheelDown, started_at + std::chrono::milliseconds(80));
    auto const keyboard = ava::tui::runtime_wheel_input_accepted(governor, ava::tui::Key::ArrowDown, started_at + std::chrono::milliseconds(81));
    auto const accepted_after_non_wheel =
        ava::tui::runtime_wheel_input_accepted(governor, ava::tui::Key::MouseWheelUp, started_at + std::chrono::milliseconds(81));
    governor.reset();
    auto const accepted_after_explicit_reset =
        ava::tui::runtime_wheel_input_accepted(governor, ava::tui::Key::MouseWheelDown, started_at + std::chrono::milliseconds(81));
    expect(
        ava::tui::kTranscriptWheelScrollRows == 3 && first_up && !same_direction_at_16 && !same_direction_at_39 && same_direction_at_40 && reverse_immediate &&
            !same_reverse_within_window && same_reverse_at_interval && keyboard && accepted_after_non_wheel && accepted_after_explicit_reset,
        "runtime wheel wiring accepts one same-direction event per 40ms, accepts immediate reversals as the new direction window, scrolls transcript by three "
        "rows per accepted event, and resets at explicit non-wheel or reset boundaries");
  }
  {
    using Clock = std::chrono::steady_clock;
    auto const paint_completed_at = Clock::time_point{} + std::chrono::milliseconds(60);
    auto const deadline = paint_completed_at + ava::tui::WheelBurstGovernor::kAcceptedEventInterval;
    auto const queued_wheel =
        ava::tui::detail::prompt_wheel_input_suppressed(ava::tui::Key::MouseWheelDown, deadline, paint_completed_at + std::chrono::milliseconds(1));
    auto const confirmation =
        ava::tui::detail::prompt_wheel_input_suppressed(ava::tui::Key::Enter, deadline, paint_completed_at + std::chrono::milliseconds(1));
    auto const wheel_at_deadline = ava::tui::detail::prompt_wheel_input_suppressed(ava::tui::Key::MouseWheelUp, deadline, deadline);
    expect(queued_wheel && !confirmation && !wheel_at_deadline,
           "prompt wheel suppression discards queued wheels after a slow paint while confirmation bypasses the deadline");
  }
  {
    using Clock = ava::tui::FrameScheduler::Clock;
    auto const started_at = Clock::time_point{};
    ava::tui::FrameScheduler scheduler;
    scheduler.paint_completed(started_at, true);
    for (std::size_t index = 0; index < 100; ++index)
      scheduler.request(ava::tui::FrameRenderKind::Full, started_at + std::chrono::milliseconds(1));
    auto const before_deadline = scheduler.take_due(started_at + std::chrono::milliseconds(15));
    auto const at_deadline = scheduler.take_due(started_at + std::chrono::milliseconds(16));
    auto paint_count = at_deadline ? std::size_t{1} : std::size_t{0};
    if (scheduler.take_due(started_at + std::chrono::milliseconds(16)))
      ++paint_count;
    scheduler.paint_completed(started_at + std::chrono::milliseconds(40), true);
    scheduler.request(ava::tui::FrameRenderKind::Footer, started_at + std::chrono::milliseconds(41));
    scheduler.request(ava::tui::FrameRenderKind::Full, started_at + std::chrono::milliseconds(42));
    auto const slow_paint_did_not_make_due = scheduler.take_due(started_at + std::chrono::milliseconds(55));
    auto const merged_after_slow_paint = scheduler.take_due(started_at + std::chrono::milliseconds(56));
    expect(!before_deadline && at_deadline == ava::tui::FrameRenderKind::Full && paint_count == 1 && !slow_paint_did_not_make_due &&
               merged_after_slow_paint == ava::tui::FrameRenderKind::Full,
           "frame scheduler coalesces 100 requests, lets full supersede footer, and bases the next deadline on slow paint completion");
  }
  {
    using Clock = ava::tui::FrameScheduler::Clock;
    auto const started_at = Clock::time_point{};
    ava::tui::FrameScheduler scheduler;
    scheduler.paint_completed(started_at, true);
    scheduler.request(ava::tui::FrameRenderKind::Footer, started_at + std::chrono::milliseconds(1));
    auto const lone_wait = scheduler.time_until_due(started_at + std::chrono::milliseconds(1));
    scheduler.request(ava::tui::FrameRenderKind::Full, started_at + std::chrono::milliseconds(2));
    auto const provider_and_input = scheduler.take_due(started_at + std::chrono::milliseconds(16));
    scheduler.paint_completed(started_at + std::chrono::milliseconds(17), false);
    scheduler.request(ava::tui::FrameRenderKind::Full, started_at + std::chrono::milliseconds(18));
    expect(lone_wait > Clock::duration::zero() && lone_wait <= ava::tui::FrameScheduler::kFrameInterval &&
               provider_and_input == ava::tui::FrameRenderKind::Full && scheduler.failed() && !scheduler.pending(),
           "frame scheduler gives lone input a deadline within 16ms, merges provider/footer and input into one full paint, and latches draw failure");
  }

  {
    ava::tui::ComposerSnapshot detached{
        .mode = "build",
        .provider = "openai",
        .model = "gpt-5.5",
        .session_id = "session_detached_cache",
        .input = "",
        .status = "",
        .transcript = {ava::tui::TranscriptItem{
            .label = "ava", .text = std::string(40, 'a') + "\n" + std::string(40, 'b') + "\n" + std::string(40, 'c') + "\n" + std::string(40, 'd')}},
        .transcript_scroll_offset = 1,
        .width = 50,
        .height = 8};
    ava::tui::detail::CompletionMatchCache completion_cache;
    ava::tui::detail::TranscriptLayoutCache transcript_cache;
    static_cast<void>(ava::tui::detail::render_composer_frame_cached(detached, completion_cache, detached.file_references_generation, &transcript_cache,
                                                                     detached.transcript_generation));
    auto const initial_builds = transcript_cache.layout_build_count;
    detached.transcript.back().text += "\nprovider output deferred below";
    ++detached.transcript_generation;
    detached.input = "typed while detached";
    static_cast<void>(ava::tui::detail::render_composer_frame_cached(detached, completion_cache, detached.file_references_generation, &transcript_cache,
                                                                     detached.transcript_generation, true, true, true));
    auto const frozen_builds = transcript_cache.layout_build_count;
    static_cast<void>(ava::tui::detail::composer_max_transcript_scroll_offset_cached(
        detached, detached.width, detached.height, completion_cache, detached.file_references_generation, transcript_cache, detached.transcript_generation));
    auto const navigation_builds = transcript_cache.layout_build_count;
    detached.transcript.back().text += "\nmore deferred output";
    ++detached.transcript_generation;
    detached.width = 62;
    static_cast<void>(ava::tui::detail::render_composer_frame_cached(detached, completion_cache, detached.file_references_generation, &transcript_cache,
                                                                     detached.transcript_generation, true, true, true));
    expect(initial_builds == 1 && frozen_builds == initial_builds && navigation_builds == initial_builds + 1 &&
               transcript_cache.layout_build_count == navigation_builds + 1,
           "detached draft redraws reuse the frozen transcript layout while first navigation and incompatible resize each rebuild exactly once");
  }

  auto const lines = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "/help",
      .status = "slash palette dismissed",
      .transcript = {ava::tui::TranscriptItem{.label = "you", .text = "hello"}, ava::tui::TranscriptItem{.label = "ava", .text = "world"}},
      .width = 80,
      .height = 14});
  {
    ScopedEnvVar no_color_guard("NO_COLOR", "1");
    auto const plain_lines = ava::tui::render_composer(ava::tui::ComposerSnapshot{
        .mode = "build",
        .provider = "openai",
        .model = "gpt-5.5",
        .session_id = "session_plain",
        .input = "/model",
        .status = "ready",
        .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "Plain output keeps **bold**, `code`, and colors out of the terminal"}},
        .select_list = ava::tui::SelectListView{.title = "Select model",
                                                .subtitle = "NO_COLOR smoke",
                                                .items = {ava::tui::SelectListItemView{.value = "openai/gpt-5.5",
                                                                                       .label = "GPT-5.5",
                                                                                       .description = "openai/gpt-5.5",
                                                                                       .group = "openai",
                                                                                       .detail = "plain terminal",
                                                                                       .badge = "current",
                                                                                       .current = true,
                                                                                       .enabled = true,
                                                                                       .disabled_reason = {}}},
                                                .selected_item_index = 0,
                                                .query = {},
                                                .placeholder = "Search models",
                                                .empty_text = "No models",
                                                .footer_hint = "Enter choose · Esc cancel"},
        .width = 88,
        .height = 18,
        .input_cursor = 6});
    expect(std::ranges::all_of(plain_lines, [](std::string const& line) { return line.find('\x1b') == std::string::npos && visible_columns(line) <= 88; }) &&
               std::ranges::any_of(plain_lines, [](std::string const& line) { return line.find("Select model") != std::string::npos; }) &&
               std::ranges::any_of(plain_lines,
                                   [](std::string const& line) { return line.find("Plain output keeps bold, code, and colors") != std::string::npos; }),
           "tui honors NO_COLOR by rendering the full frame without ANSI styling while preserving visible content and width");
  }
  expect(lines.size() == 14, "tui fills the viewport with transcript, spacer, and composer lines");
  expect(!lines.empty() && strip_sgr(lines.front()).find("hello") != std::string::npos, "tui starts short chats at the top of the transcript area");
  expect(lines.size() == 14 && strip_sgr(lines[12]).starts_with("│  /help") && strip_sgr(lines[13]).starts_with("│  GPT-5.5") &&
             strip_sgr(lines[11]).starts_with("│  ") && lines[11].find("\x1b[49m") != std::string::npos &&
             lines[11].find("\x1b[48;2;26;31;46m") == std::string::npos && lines[12].find("\x1b[49m") != std::string::npos &&
             lines[12].find("\x1b[48;2;26;31;46m") == std::string::npos && lines[13].find("\x1b[49m") != std::string::npos &&
             lines[13].find("\x1b[48;2;26;31;46m") == std::string::npos &&
             std::ranges::none_of(lines, [](std::string const& line) { return line.find("/ commands") != std::string::npos; }),
         "tui keeps a one-line draft in the bottom input/footer rows below screen-background composer padding");
  expect(std::ranges::any_of(lines, [](std::string const& line) { return strip_sgr(line).find("│  /help") != std::string::npos; }),
         "tui renders the quiet composer input without a prompt glyph");
  expect(std::ranges::none_of(lines, [](std::string const& line) { return strip_sgr(line).find("slash palette dismissed") != std::string::npos; }),
         "tui keeps transient composer status text out of the footer");
  expect(std::ranges::any_of(lines,
                             [](std::string const& line) {
                               return line.find("\x1b[49m") != std::string::npos && line.find("\x1b[38;2;77;158;246m│") != std::string::npos &&
                                      line.find("\x1b[48;2;26;31;46m") == std::string::npos && strip_sgr(line).find("❯") == std::string::npos;
                             }) &&
             std::ranges::none_of(lines, [](std::string const& line) { return line.find("\x1b[48;2;26;31;46m") != std::string::npos; }),
         "tui ordinary composer dock inherits the screen/transcript background with one quiet accent boundary");
  expect(std::ranges::none_of(lines, [](std::string const& line) { return strip_sgr(line).find("╭─ You") != std::string::npos; }) &&
             std::ranges::none_of(lines, [](std::string const& line) { return strip_sgr(line).find("╭─ AVA") != std::string::npos; }) &&
             std::ranges::any_of(lines,
                                 [](std::string const& line) {
                                   return line.find("\x1b[38;2;77;158;246m›") != std::string::npos &&
                                          line.find("\x1b[1m\x1b[38;2;232;236;241mhello") != std::string::npos &&
                                          line.find("\x1b[48;2;26;31;46m") == std::string::npos;
                                 }) &&
             std::ranges::any_of(lines, [](std::string const& line) { return strip_sgr(line).find("world") != std::string::npos; }),
         "tui distinguishes user messages with a quiet blue chevron and bright text without a composer surface or role header");

  auto const wrapped_user_rows =
      ava::tui::detail::render_transcript_lines({ava::tui::TranscriptItem{.label = "you", .text = "one two three four five six seven eight\x1b[31m"}}, 16);
  auto const available_width_user_rows = ava::tui::detail::render_transcript_lines({ava::tui::TranscriptItem{.label = "you", .text = "abcdefghijkl"}}, 16);
  auto const empty_user_rows = ava::tui::detail::render_transcript_lines({ava::tui::TranscriptItem{.label = "you", .text = ""}}, 16);
  expect(
      wrapped_user_rows.size() > 1 &&
          std::ranges::all_of(wrapped_user_rows,
                              [](std::string const& line) { return line.find("\x1b[48;2;26;31;46m") == std::string::npos && visible_columns(line) <= 16; }) &&
          strip_sgr(wrapped_user_rows.front()).starts_with("  › ") && wrapped_user_rows.front().find("\x1b[31m") == std::string::npos &&
          std::ranges::all_of(std::next(wrapped_user_rows.begin()), wrapped_user_rows.end(),
                              [](std::string const& line) { return strip_sgr(line).starts_with("    "); }) &&
          available_width_user_rows.size() == 1 && strip_sgr(available_width_user_rows.front()) == "  › abcdefghijkl" && empty_user_rows.size() == 1 &&
          strip_sgr(empty_user_rows.front()).starts_with("  › "),
      "tui user chevron uses all available text width, sanitizes terminal escapes, preserves empty messages, aligns continuations, and never paints a composer "
      "background");
  {
    ScopedEnvVar no_color_guard("NO_COLOR", "1");
    auto const plain_user_rows = ava::tui::render_composer(
        ava::tui::ComposerSnapshot{.mode = "build",
                                   .provider = "openai",
                                   .model = "gpt-5.5",
                                   .session_id = "session_plain_user",
                                   .input = {},
                                   .status = "ready",
                                   .transcript = {ava::tui::TranscriptItem{.label = "you", .text = "plain user rendering remains identifiable"}},
                                   .width = 40,
                                   .height = 10});
    auto const plain_user_row = std::ranges::find_if(plain_user_rows, [](std::string const& line) { return line.find("plain user") != std::string::npos; });
    expect(plain_user_row != plain_user_rows.end() &&
               std::ranges::all_of(plain_user_rows,
                                   [](std::string const& line) { return line.find('\x1b') == std::string::npos && visible_columns(line) <= 40; }) &&
               plain_user_row->starts_with("  › plain user"),
           "tui user identity remains meaningful and width-bounded with NO_COLOR");
  }

  auto const idle_two_row_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                                .provider = "openai",
                                                                .model = "gpt-5.5",
                                                                .session_id = "session_test",
                                                                .input = "",
                                                                .status = "ready",
                                                                .active_context_status = "870 (3.2%)",
                                                                .context_source_count = 2,
                                                                .transcript = {},
                                                                .width = 80,
                                                                .height = 10};
  auto const idle_two_row_lines = ava::tui::render_composer(idle_two_row_snapshot);
  auto idle_input = strip_sgr(idle_two_row_lines[8]);
  auto idle_footer = strip_sgr(idle_two_row_lines[9]);
  while (!idle_input.empty() && idle_input.back() == ' ')
    idle_input.pop_back();
  while (!idle_footer.empty() && idle_footer.back() == ' ')
    idle_footer.pop_back();
  expect(idle_two_row_lines.size() == 10 && idle_input == "│  Type a message..." && idle_footer == "│  GPT-5.5 · ctx 870 (3.2%)" &&
             idle_two_row_lines[8].find("\x1b[49m") != std::string::npos && idle_two_row_lines[9].find("\x1b[49m") != std::string::npos &&
             std::ranges::none_of(idle_two_row_lines, [](std::string const& line) { return line.find("\x1b[48;2;26;31;46m") != std::string::npos; }) &&
             std::ranges::none_of(idle_two_row_lines, [](std::string const& line) { return strip_sgr(line).find("❯") != std::string::npos; }),
         "tui empty composer is exactly two bottom screen-background rows with one boundary, quiet gutter, pure footer, and no prompt glyph");
  {
    auto const palette_frame = ava::tui::render_composer(
        ava::tui::ComposerSnapshot{.mode = "build",
                                   .provider = "openai",
                                   .model = "gpt-5.5",
                                   .session_id = "session_palette_bg",
                                   .input = "/h",
                                   .status = "ready",
                                   .transcript = {},
                                   .slash_commands = {ava::tui::SlashCommandItem{.command = "/help", .description = "Show help", .category = "General"}},
                                   .selected_slash_command_index = 0,
                                   .width = 80,
                                   .height = 12});
    auto const select_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{
        .mode = "build",
        .provider = "openai",
        .model = "gpt-5.5",
        .session_id = "session_select_bg",
        .input = {},
        .status = "ready",
        .transcript = {},
        .select_list =
            ava::tui::SelectListView{
                .title = "Models",
                .subtitle = {},
                .items = {ava::tui::SelectListItemView{
                    .value = "a", .label = "Alpha", .description = {}, .group = {}, .detail = {}, .badge = {}, .enabled = true, .disabled_reason = {}}},
                .selected_item_index = 0,
                .query = {},
                .footer_hint = {}},
        .width = 80,
        .height = 16});
    auto const palette_row = std::ranges::find_if(palette_frame, [](std::string const& line) { return strip_sgr(line).find("/help") != std::string::npos; });
    auto const select_row = std::ranges::find_if(select_frame, [](std::string const& line) { return strip_sgr(line).find("Alpha") != std::string::npos; });
    auto const dock_input = std::ranges::find_if(palette_frame, [](std::string const& line) { return strip_sgr(line).starts_with("│  /h"); });
    expect(palette_row != palette_frame.end() && select_row != select_frame.end() && dock_input != palette_frame.end() &&
               palette_row->find("\x1b[48;2;26;31;46m") != std::string::npos && select_row->find("\x1b[48;2;26;31;46m") != std::string::npos &&
               dock_input->find("\x1b[49m") != std::string::npos && dock_input->find("\x1b[48;2;26;31;46m") == std::string::npos,
           "tui keeps elevated composer backgrounds on palette/select-list rows while the ordinary draft dock stays on the screen background");
  }
  {
    auto const dock_uses_screen_bg = [](std::vector<std::string> const& rendered) {
      return std::ranges::any_of(rendered,
                                 [](std::string const& line) {
                                   return strip_sgr(line).find("Type a message...") != std::string::npos && line.find("\x1b[49m") != std::string::npos &&
                                          line.find("\x1b[48;2;26;31;46m") == std::string::npos;
                                 }) &&
             std::ranges::none_of(rendered, [](std::string const& line) { return line.find("\x1b[48;2;26;31;46m") != std::string::npos; });
    };
    auto const idle_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                          .provider = "openai",
                                                          .model = "gpt-5.5",
                                                          .session_id = "session_theme_dock_bg",
                                                          .input = "",
                                                          .status = "ready",
                                                          .transcript = {},
                                                          .width = 60,
                                                          .height = 10};
    ava::tui::set_tui_config_theme(std::nullopt);
    auto const dark_lines = ava::tui::render_composer(idle_snapshot);
    {
      ScopedEnvVar light_theme("AVA_TUI_THEME", "light");
      ava::tui::set_tui_config_theme(std::nullopt);
      auto const light_lines = ava::tui::render_composer(idle_snapshot);
      expect(dock_uses_screen_bg(dark_lines) && dock_uses_screen_bg(light_lines),
             "tui ordinary composer dock keeps terminal-default screen background under built-in dark and light themes");
    }
    ava::tui::TuiCustomTheme custom{.name = "dockbg",
                                    .path = "dockbg.json",
                                    .palette = ava::tui::TuiThemePalette{.text = -1,
                                                                         .muted = 242,
                                                                         .success = 34,
                                                                         .warning = 220,
                                                                         .error = 196,
                                                                         .accent = 39,
                                                                         .screen_bg = 255,
                                                                         .composer_bg = 236,
                                                                         .tool_bg = 235,
                                                                         .question_bg = 237},
                                    .revision = "test-dockbg"};
    ava::tui::set_tui_config_theme("dockbg", custom);
    auto const custom_lines = ava::tui::render_composer(idle_snapshot);
    expect(dock_uses_screen_bg(custom_lines), "tui ordinary composer dock follows screen background semantics under a custom theme with distinct composerBg");
    ava::tui::set_tui_config_theme(std::nullopt);
    {
      ScopedEnvVar no_color_guard("NO_COLOR", "1");
      auto const plain_lines = ava::tui::render_composer(idle_snapshot);
      expect(std::ranges::all_of(plain_lines, [](std::string const& line) { return line.find('\x1b') == std::string::npos; }) &&
                 std::ranges::any_of(plain_lines, [](std::string const& line) { return line.find("Type a message...") != std::string::npos; }),
             "tui ordinary composer dock stays SGR-free under NO_COLOR");
    }
  }
  {
    std::vector<ava::tui::TranscriptItem> filled_transcript;
    for (int index = 0; index < 20; ++index)
      filled_transcript.push_back(ava::tui::TranscriptItem{.label = "status", .text = "gap item " + std::to_string(index)});
    auto roomy_gap_snapshot = idle_two_row_snapshot;
    roomy_gap_snapshot.transcript = filled_transcript;
    roomy_gap_snapshot.height = 13;
    auto compact_gap_snapshot = roomy_gap_snapshot;
    compact_gap_snapshot.height = 12;
    auto modal_policy_snapshot = roomy_gap_snapshot;
    modal_policy_snapshot.select_list = ava::tui::SelectListView{};
    auto crowded_gap_snapshot = roomy_gap_snapshot;
    crowded_gap_snapshot.input = "draft 1\ndraft 2\ndraft 3\ndraft 4\ndraft 5\ndraft 6\ndraft 7";
    crowded_gap_snapshot.status = "invalid_argument: alert 1\nalert 2\nalert 3";
    crowded_gap_snapshot.reasoning_feedback = "reasoning feedback";
    auto const roomy_gap_frame = ava::tui::render_composer(roomy_gap_snapshot);
    auto const compact_gap_frame = ava::tui::render_composer(compact_gap_snapshot);
    auto const crowded_gap_frame = ava::tui::render_composer(crowded_gap_snapshot);
    auto const roomy_max = ava::tui::composer_max_transcript_scroll_offset(roomy_gap_snapshot, 80, 13);
    auto const compact_max = ava::tui::composer_max_transcript_scroll_offset(compact_gap_snapshot, 80, 12);
    auto permission_policy_snapshot = roomy_gap_snapshot;
    permission_policy_snapshot.permission_prompt =
        ava::tui::PermissionPromptView{.tool_name = "bash", .operation = "run command", .target = "tests", .reason = "test"};
    auto const permission_policy_frame = ava::tui::render_composer(permission_policy_snapshot);
    expect(roomy_gap_frame.size() == 13 && strip_sgr(roomy_gap_frame[8]).find("gap item 19") != std::string::npos &&
               strip_sgr(roomy_gap_frame[9]).find_first_not_of(' ') == std::string::npos &&
               roomy_gap_frame[9].find("\x1b[48;2;26;31;46m") == std::string::npos && strip_sgr(roomy_gap_frame[10]).starts_with("│  ") &&
               roomy_gap_frame[10].find("\x1b[49m") != std::string::npos && roomy_gap_frame[10].find("\x1b[48;2;26;31;46m") == std::string::npos &&
               strip_sgr(roomy_gap_frame[11]).starts_with("│  Type a message...") && roomy_gap_frame[11].find("\x1b[49m") != std::string::npos &&
               roomy_gap_frame[11].find("\x1b[48;2;26;31;46m") == std::string::npos && strip_sgr(roomy_gap_frame[12]).starts_with("│  GPT-5.5") &&
               roomy_gap_frame[12].find("\x1b[49m") != std::string::npos && compact_gap_frame.size() == 12 &&
               strip_sgr(compact_gap_frame[9]).find("gap item 19") != std::string::npos && strip_sgr(compact_gap_frame[10]).starts_with("│  ") &&
               strip_sgr(crowded_gap_frame.front()).find("gap item 19") != std::string::npos && roomy_max == compact_max + 1 &&
               ava::tui::detail::composer_layout_policy(roomy_gap_snapshot, 13).transcript_composer_gap_lines == 1 &&
               ava::tui::detail::composer_layout_policy(roomy_gap_snapshot, 13).composer_top_padding_lines == 1 &&
               ava::tui::detail::composer_layout_policy(compact_gap_snapshot, 12).transcript_composer_gap_lines == 0 &&
               ava::tui::detail::composer_layout_policy(compact_gap_snapshot, 12).composer_top_padding_lines == 0 &&
               ava::tui::detail::composer_layout_policy(modal_policy_snapshot, 24).composer_top_padding_lines == 0 &&
               ava::tui::detail::composer_block_line_count(permission_policy_snapshot, 13, 80) == 2 &&
               strip_sgr(permission_policy_frame[11]).starts_with("│  Type a message...") && strip_sgr(permission_policy_frame[12]).starts_with("│  GPT-5.5"),
           "tui roomy ordinary layouts reserve a screen-background breathing gap and one guttered screen-background composer row above the unchanged "
           "input/footer rows, while compact and authoritative layouts reclaim that padding row");
  }
  auto const composer_lines_for = [&](std::string input, std::size_t width = 80) {
    auto snapshot = idle_two_row_snapshot;
    snapshot.input = std::move(input);
    return ava::tui::detail::composer_block_line_count(snapshot, 100, width);
  };
  expect(composer_lines_for("") == 3 && composer_lines_for("one") == 3 && composer_lines_for("one\ntwo") == 4 &&
             composer_lines_for("abcdefghijklmnopqr", 20) == 4 && composer_lines_for("1\n2\n3\n4\n5\n6\n7") == 8 &&
             composer_lines_for("1\n2\n3\n4\n5\n6\n7\n8\n9") == 8 && ava::tui::detail::composer_block_line_count(idle_two_row_snapshot, 12, 80) == 2,
         "tui composer desired height includes the roomy top-padding row while preserving the two-through-eight-row cap and compact two-row empty dock");
  {
    auto roomy_multiline_snapshot = idle_two_row_snapshot;
    roomy_multiline_snapshot.height = 14;
    roomy_multiline_snapshot.input = "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine";
    auto const policy = ava::tui::detail::composer_layout_policy(roomy_multiline_snapshot, roomy_multiline_snapshot.height);
    auto const layout = ava::tui::detail::composer_input_layout(9, 8, 0, policy.composer_top_padding_lines);
    auto const scrolled_layout = ava::tui::detail::composer_input_layout(9, 8, 2, policy.composer_top_padding_lines);
    auto const elevated_draft = ava::tui::render_composer(roomy_multiline_snapshot);
    auto const first_visible_draft_cursor = ava::tui::composer_input_cursor_for_screen_position(roomy_multiline_snapshot, 8, 4);
    expect(layout.top_padding == 1 && layout.first_visible == 3 && layout.visible_input_lines == 6 && layout.hidden_above == 3 &&
               scrolled_layout.first_visible == 1 && elevated_draft.size() == 14 && strip_sgr(elevated_draft[6]).starts_with("│  ") &&
               elevated_draft[6].find("\x1b[49m") != std::string::npos && elevated_draft[6].find("\x1b[48;2;26;31;46m") == std::string::npos &&
               strip_sgr(elevated_draft[7]).starts_with("│  four") && elevated_draft[7].find("\x1b[49m") != std::string::npos &&
               strip_sgr(elevated_draft[13]).starts_with("│  GPT-5.5") && elevated_draft[13].find("\x1b[49m") != std::string::npos &&
               !ava::tui::composer_input_cursor_for_screen_position(roomy_multiline_snapshot, 7, 4) && first_visible_draft_cursor &&
               *first_visible_draft_cursor == std::string("one\ntwo\nthree\n").size(),
           "tui roomy multiline drafts reserve the screen-background padding row before deriving their viewport, scrolling, cursor, and hit-test rows");
  }

  auto const processing_lines = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                     .provider = "openai",
                                                                                     .model = "gpt-5.5",
                                                                                     .session_id = "session_test",
                                                                                     .input = "",
                                                                                     .status = "thinking...",
                                                                                     .processing = true,
                                                                                     .active_run_hint = ava::tui::ActiveRunHint{.interrupt = "Esc"},
                                                                                     .spinner_frame = 1,
                                                                                     .token_status = "1.3k (0.7%)",
                                                                                     .transcript = {},
                                                                                     .width = 80,
                                                                                     .height = 10});
  auto const processing_meter_raw = std::string(ava::tui::detail::processing_indicator_frame(1));
  // Frame 1 is ▃▆▄▂: outer muted, inner pair accent blue.
  auto const muted_outer = std::string(ava::tui::detail::kSgrMuted) + "▃";
  auto const accent_inner = std::string(ava::tui::detail::kSgrAccent) + "▆";
  expect(processing_lines.size() == 10 && strip_sgr(processing_lines[7]).find("Esc stop · type a follow-up") != std::string::npos &&
             strip_sgr(processing_lines[8]).starts_with("│  Type a message...") && strip_sgr(processing_lines[9]).starts_with("│  GPT-5.5") &&
             strip_sgr(processing_lines[9]).find(processing_meter_raw) != std::string::npos && processing_lines[9].find(muted_outer) != std::string::npos &&
             processing_lines[9].find(accent_inner) != std::string::npos && processing_lines[7].find("\x1b[49m") != std::string::npos &&
             processing_lines[7].find("\x1b[48;2;26;31;46m") == std::string::npos && processing_lines[8].find("\x1b[49m") != std::string::npos &&
             processing_lines[9].find("\x1b[49m") != std::string::npos &&
             std::ranges::all_of(processing_lines,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("thinking...") == std::string::npos && visible.find("working") == std::string::npos &&
                                          visible.find("1.3k (0.7%)") == std::string::npos && visible.find("❯") == std::string::npos;
                                 }),
         "tui processing composer adds only a screen-background contextual active-run row while the footer remains model metadata plus a fixed four-cell "
         "signal meter");
  expect(ava::tui::detail::kProcessingIndicatorFrameDelay == std::chrono::milliseconds(120) && ava::tui::detail::kProcessingIndicatorColumns == 4 &&
             ava::tui::detail::kProcessingIndicatorFrames.size() == 4 &&
             std::ranges::all_of(ava::tui::detail::kProcessingIndicatorFrames,
                                 [](std::string_view frame) {
                                   return ava::tui::detail::terminal_text_columns(frame) == ava::tui::detail::kProcessingIndicatorColumns &&
                                          frame.find("\xE2\xA0") == std::string_view::npos;
                                 }) &&
             ava::tui::detail::processing_indicator_frame(0) == "▂▄▇▃" && ava::tui::detail::processing_indicator_frame(1) == "▃▆▄▂" &&
             ava::tui::detail::processing_indicator_frame(2) == "▅▃▇▄" && ava::tui::detail::processing_indicator_frame(3) == "▄▇▅▂" &&
             ava::tui::detail::processing_indicator_frame(4) == "▂▄▇▃" &&
             ava::tui::detail::terminal_text_columns(ava::tui::detail::processing_indicator_styled(0)) == ava::tui::detail::kProcessingIndicatorColumns &&
             ava::tui::detail::processing_indicator_styled(0).find(std::string(ava::tui::detail::kSgrMuted)) != std::string::npos &&
             ava::tui::detail::processing_indicator_styled(0).find(std::string(ava::tui::detail::kSgrAccent)) != std::string::npos &&
             ava::tui::detail::processing_indicator_elapsed_frames(std::chrono::milliseconds(119)) == 0 &&
             ava::tui::detail::processing_indicator_elapsed_frames(std::chrono::milliseconds(120)) == 1 &&
             ava::tui::detail::processing_indicator_elapsed_frames(std::chrono::milliseconds(365)) == 3,
         "tui processing indicator has a shared 120ms four-cell lower-block signal meter and advances by elapsed intervals independent of redraws");

  auto narrow_footer_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.6-terra",
                                                           .session_id = "session_test",
                                                           .input = "",
                                                           .status = "ready",
                                                           .active_context_status = "~12k",
                                                           .context_source_count = 12,
                                                           .transcript = {},
                                                           .width = 20,
                                                           .height = 8};
  auto const narrow_footer_lines = ava::tui::render_composer(narrow_footer_snapshot);
  expect(std::ranges::any_of(narrow_footer_lines,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("GPT-") != std::string::npos && visible.find("ctx ~12k") != std::string::npos && visible_columns(line) == 20;
                             }),
         "tui shortens a long model label before dropping multi-digit context metadata at the supported minimum width");

  narrow_footer_snapshot.status = "thinking...";
  narrow_footer_snapshot.processing = true;
  narrow_footer_snapshot.spinner_frame = 1;
  auto const narrow_processing_lines = ava::tui::render_composer(narrow_footer_snapshot);
  auto const narrow_meter_raw = std::string(ava::tui::detail::processing_indicator_frame(1));
  auto const narrow_muted_outer = std::string(ava::tui::detail::kSgrMuted) + "▃";
  auto const narrow_accent_inner = std::string(ava::tui::detail::kSgrAccent) + "▆";
  // At width 20 the fixed four-cell meter keeps ctx metadata; the model label may collapse to ellipsis.
  expect(std::ranges::any_of(narrow_processing_lines,
                             [&](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("ctx ~12k") != std::string::npos && visible.find(narrow_meter_raw) != std::string::npos &&
                                      line.find(narrow_muted_outer) != std::string::npos && line.find(narrow_accent_inner) != std::string::npos &&
                                      visible_columns(line) == 20;
                             }),
         "tui preserves multi-digit context metadata and the four-cell signal meter at the supported minimum width");

  auto combined_narrow_footer = narrow_footer_snapshot;
  combined_narrow_footer.model = "gpt-5.6-terra";
  combined_narrow_footer.active_context_status = "150.3k (55.3%)";
  combined_narrow_footer.width = 28;
  combined_narrow_footer.height = 8;
  combined_narrow_footer.processing = true;
  combined_narrow_footer.spinner_frame = 1;
  auto const combined_narrow_lines = ava::tui::render_composer(combined_narrow_footer);
  expect(std::ranges::any_of(combined_narrow_lines,
                             [&](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("ctx 150.3k (55.3%)") != std::string::npos && visible.find(narrow_meter_raw) != std::string::npos &&
                                      line.find(narrow_muted_outer) != std::string::npos && line.find(narrow_accent_inner) != std::string::npos &&
                                      visible_columns(line) == 28;
                             }) &&
             std::ranges::all_of(combined_narrow_lines, [](std::string const& line) { return visible_columns(line) <= 28; }),
         "tui keeps combined count/percent context and the four-cell meter width-bounded on a narrow canvas");

  auto normal_combined_footer = combined_narrow_footer;
  normal_combined_footer.model = "gpt-5.5";
  normal_combined_footer.width = 80;
  normal_combined_footer.height = 10;
  auto const normal_combined_lines = ava::tui::render_composer(normal_combined_footer);
  expect(std::ranges::any_of(normal_combined_lines,
                             [&](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("GPT-5.5 · ctx 150.3k (55.3%)") != std::string::npos &&
                                      visible.find(narrow_meter_raw) != std::string::npos && visible_columns(line) == 80;
                             }),
         "tui shows count plus percent with the four-cell meter at normal width without overflow");
  {
    ScopedEnvVar no_color_guard("NO_COLOR", "1");
    auto plain_processing = narrow_footer_snapshot;
    plain_processing.processing = true;
    plain_processing.spinner_frame = 2;
    auto const plain_lines = ava::tui::render_composer(plain_processing);
    auto const plain_meter = std::string(ava::tui::detail::processing_indicator_frame(2));
    expect(std::ranges::all_of(plain_lines, [](std::string const& line) { return line.find('\x1b') == std::string::npos; }) &&
               std::ranges::any_of(plain_lines,
                                   [&](std::string const& line) {
                                     return line.find(plain_meter) != std::string::npos &&
                                            ava::tui::detail::terminal_text_columns(plain_meter) == ava::tui::detail::kProcessingIndicatorColumns;
                                   }),
           "tui NO_COLOR processing footer retains the four meter glyphs with no SGR");
  }

  auto expect_direct_footer_matches_frame = [&](ava::tui::ComposerSnapshot footer_snapshot, std::string const& layout_name) {
    footer_snapshot.processing = true;
    auto const canvas = ava::tui::composer_canvas_layout(footer_snapshot);
    auto const composer_lines = ava::tui::detail::composer_block_line_count(footer_snapshot, footer_snapshot.height, canvas.content_width);
    auto const direct_footer = ava::tui::detail::render_composer_footer_line(footer_snapshot, canvas.content_width);
    auto const input_footer = ava::tui::detail::render_composer_block(footer_snapshot, canvas.content_width, composer_lines).back();
    auto const full_footer = ava::tui::render_composer_frame(footer_snapshot).lines.back();
    expect(direct_footer == input_footer && full_footer.find(direct_footer) != std::string::npos,
           "tui direct footer renderer matches the full composer footer for " + layout_name);
  };
  expect_direct_footer_matches_frame(idle_two_row_snapshot, "ordinary canvas");
  auto centered_footer_snapshot = idle_two_row_snapshot;
  centered_footer_snapshot.width = 160;
  centered_footer_snapshot.height = 14;
  expect_direct_footer_matches_frame(centered_footer_snapshot, "centered canvas");
  auto rail_footer_snapshot = centered_footer_snapshot;
  rail_footer_snapshot.width = 176;
  rail_footer_snapshot.height = 16;
  rail_footer_snapshot.sidebar = ava::tui::SidebarSnapshot{
      .activity = {ava::tui::SidebarActivityItem{.id = "running", .label = "run", .detail = "active", .status = ava::tui::ToolTimelineStatus::Running}}};
  expect_direct_footer_matches_frame(rail_footer_snapshot, "automatic sidebar rail");
  expect_direct_footer_matches_frame(narrow_footer_snapshot, "narrow canvas");

  auto const queued_lines = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "thinking...",
                                 .processing = true,
                                 .transcript = {ava::tui::TranscriptItem{.label = "you", .text = "work on queue UI"}},
                                 .width = 80,
                                 .height = 12,
                                 .queued_messages = {ava::tui::QueuedMessageItem{.id = "q1", .kind = "follow-up", .text = "run tests next"},
                                                     ava::tui::QueuedMessageItem{.id = "q2", .kind = "steer", .text = "keep patch small"}}});
  expect(std::ranges::any_of(queued_lines,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("queued follow-up run tests next") != std::string::npos;
                             }) &&
             std::ranges::any_of(queued_lines,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("queued steer keep patch small") != std::string::npos &&
                                          visible.find("/restore or dequeue latest") != std::string::npos;
                                 }),
         "tui renders active-run queued steering/follow-up messages in a compact pending region");

  auto const attachment_lines = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "attached image for next prompt",
                                 .transcript = {ava::tui::TranscriptItem{.label = "status", .text = "attached image"}},
                                 .width = 80,
                                 .height = 12,
                                 .pending_attachments = {ava::tui::PendingAttachmentItem{.label = "screen.png", .detail = "(image/png, 68 bytes)"}}});
  expect(std::ranges::any_of(attachment_lines,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("attached image screen.png") != std::string::npos &&
                                      visible.find("(image/png, 68 bytes)") != std::string::npos && visible.find("(next prompt)") != std::string::npos;
                             }),
         "tui renders pending image attachments before the next prompt is submitted");
  auto const attachment_preview_snapshot = ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "attached image for next prompt",
      .transcript = {ava::tui::TranscriptItem{.label = "status", .text = "attached image"}},
      .width = 80,
      .height = 14,
      .pending_attachments = {ava::tui::PendingAttachmentItem{
          .label = "screen.png",
          .detail = "(image/png, 68 bytes) preview kitty",
          .preview = ava::tui::PendingAttachmentItem::Preview{.protocol = ava::tui::TerminalImageProtocol::Kitty,
                                                              .base64_data = std::make_shared<std::string const>("AAAA"),
                                                              .dimensions = ava::tui::ImageDimensions{.width_px = 20, .height_px = 20},
                                                              .image_id = 42}}}};
  auto const attachment_preview_frame = ava::tui::render_composer_frame(attachment_preview_snapshot);
  auto const attachment_preview_lines = ava::tui::render_composer(attachment_preview_snapshot);
  expect(attachment_preview_frame.graphics.size() == 1 && attachment_preview_frame.graphics[0].protocol == ava::tui::TerminalImageProtocol::Kitty &&
             attachment_preview_frame.graphics[0].image_id == std::optional<std::size_t>{42} && attachment_preview_frame.graphics[0].rows > 1 &&
             attachment_preview_frame.graphics[0].sequence.starts_with("\x1b_G") &&
             attachment_preview_frame.graphics[0].sequence.find("C=1") != std::string::npos &&
             std::ranges::none_of(attachment_preview_lines, [](std::string const& line) { return ava::tui::terminal_line_contains_image_sequence(line); }),
         "tui render frame reserves rows and carries trusted Kitty image graphics outside the text-only line API");
  auto disabled_images_snapshot = attachment_preview_snapshot;
  disabled_images_snapshot.show_images = false;
  auto const disabled_images_frame = ava::tui::render_composer_frame(disabled_images_snapshot);
  auto const disabled_images_lines = ava::tui::render_composer(disabled_images_snapshot);
  expect(disabled_images_frame.graphics.empty() &&
             std::ranges::any_of(disabled_images_lines,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("attached image screen.png") != std::string::npos;
                                 }) &&
             std::ranges::none_of(disabled_images_frame.lines, [](std::string const& line) { return ava::tui::terminal_line_contains_image_sequence(line); }),
         "disabled show_images keeps textual attachment metadata and emits no graphics protocol bytes");
  auto wide_image_snapshot = attachment_preview_snapshot;
  wide_image_snapshot.image_width_cells = 12;
  wide_image_snapshot.pending_attachments.front().preview->dimensions = ava::tui::ImageDimensions{.width_px = 1200, .height_px = 600};
  auto const wide_image_frame = ava::tui::render_composer_frame(wide_image_snapshot);
  auto narrow_viewport_snapshot = wide_image_snapshot;
  narrow_viewport_snapshot.width = 20;
  auto const narrow_viewport_frame = ava::tui::render_composer_frame(narrow_viewport_snapshot);
  expect(wide_image_frame.graphics.size() == 1 && wide_image_frame.graphics.front().columns <= 12 && narrow_viewport_frame.graphics.size() == 1 &&
             narrow_viewport_frame.graphics.front().columns <= 16,
         "configured image width is honored and clamped again to the available viewport");
  auto centered_attachment_preview_snapshot = attachment_preview_snapshot;
  centered_attachment_preview_snapshot.width = 160;
  auto const centered_attachment_preview_frame = ava::tui::render_composer_frame(centered_attachment_preview_snapshot);
  expect(centered_attachment_preview_frame.graphics.size() == 1 &&
             centered_attachment_preview_frame.graphics.front().column == attachment_preview_frame.graphics.front().column + 20 &&
             centered_attachment_preview_frame.graphics.front().row == attachment_preview_frame.graphics.front().row &&
             std::ranges::all_of(centered_attachment_preview_frame.lines, [](std::string const& line) { return visible_columns(line) == 160; }),
         "tui centered canvas shifts each terminal graphic overlay by the physical inset exactly once");
  {
    ScopedEnvVar no_color_preview_guard("NO_COLOR", "1");
    auto const plain_attachment_preview_frame = ava::tui::render_composer_frame(attachment_preview_snapshot);
    expect(plain_attachment_preview_frame.graphics.empty() && std::ranges::none_of(plain_attachment_preview_frame.lines,
                                                                                   [](std::string const& line) {
                                                                                     return line.find("\x1b[") != std::string::npos ||
                                                                                            ava::tui::terminal_line_contains_image_sequence(line);
                                                                                   }),
           "plain TUI output keeps image previews on the textual fallback path without ANSI or graphics escapes");
  }

  auto const reasoning_lines = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                    .provider = "openai",
                                                                                    .model = "gpt-5.5",
                                                                                    .session_id = "session_test",
                                                                                    .input = "explain this",
                                                                                    .status = "ready",
                                                                                    .reasoning_status = "low",
                                                                                    .transcript = {},
                                                                                    .width = 80,
                                                                                    .height = 10});
  expect(std::ranges::any_of(reasoning_lines,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("GPT-5.5") != std::string::npos && visible.find("Build") == std::string::npos &&
                                      visible.find("OpenAI") == std::string::npos && visible.find("low") == std::string::npos;
                             }),
         "tui keeps mode, provider, and reasoning level out of the composer footer");

  auto const default_reasoning_lines = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                            .provider = "openai",
                                                                                            .model = "gpt-5.5",
                                                                                            .session_id = "session_test",
                                                                                            .input = "explain this",
                                                                                            .status = "ready",
                                                                                            .transcript = {},
                                                                                            .width = 80,
                                                                                            .height = 10});
  expect(std::ranges::any_of(default_reasoning_lines,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("GPT-5.5") != std::string::npos && visible.find("Build") == std::string::npos &&
                                      visible.find("OpenAI") == std::string::npos && visible.find("default") == std::string::npos;
                             }),
         "tui renders only the model when context metadata is unavailable");
  expect(
      std::ranges::none_of(default_reasoning_lines, [](std::string const& line) { return strip_sgr(line).find("session session_test") != std::string::npos; }),
      "tui keeps the session id out of the composer footer");

  auto const plan_mode_lines = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "plan",
                                                                                    .provider = "openai",
                                                                                    .model = "gpt-5.5",
                                                                                    .session_id = "session_test",
                                                                                    .input = "",
                                                                                    .status = "ready",
                                                                                    .transcript = {},
                                                                                    .width = 80,
                                                                                    .height = 10});
  expect(std::ranges::any_of(default_reasoning_lines,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("GPT-5.5") != std::string::npos && visible.find("Build") == std::string::npos;
                             }) &&
             std::ranges::any_of(plan_mode_lines,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("GPT-5.5") != std::string::npos && visible.find("Plan") == std::string::npos;
                                 }),
         "tui keeps build and plan mode badges out of the composer footer");

  auto const token_margin_lines = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                       .provider = "openai",
                                                                                       .model = "gpt-5.5",
                                                                                       .session_id = "session_test",
                                                                                       .input = "",
                                                                                       .status = "ready",
                                                                                       .token_status = "1.3k (0.7%)",
                                                                                       .transcript = {},
                                                                                       .width = 80,
                                                                                       .height = 10});
  expect(std::ranges::none_of(token_margin_lines, [](std::string const& line) { return strip_sgr(line).find("1.3k (0.7%)") != std::string::npos; }),
         "tui keeps token-status text out of the composer footer");

  auto const compact_footer_lines = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                         .provider = "openai",
                                                                                         .model = "gpt-5.5",
                                                                                         .session_id = "session_test",
                                                                                         .input = "",
                                                                                         .status = "ready",
                                                                                         .token_status = "1.3k (0.7%)",
                                                                                         .active_context_status = "870 (3.2%)",
                                                                                         .transcript = {},
                                                                                         .width = 110,
                                                                                         .height = 10,
                                                                                         .sidebar = ava::tui::SidebarSnapshot{.session_id = "session_test",
                                                                                                                              .mode = "build",
                                                                                                                              .provider = "openai",
                                                                                                                              .model = "gpt-5.5",
                                                                                                                              .workspace = "/workspace/project",
                                                                                                                              .git_branch = "develop",
                                                                                                                              .token_status = "1.3k (0.7%)",
                                                                                                                              .context_source_count = 2,
                                                                                                                              .session_entry_count = 42}});
  expect(std::ranges::any_of(compact_footer_lines,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("GPT-5.5 · ctx 870 (3.2%)") != std::string::npos && visible.find("ctx 2") == std::string::npos &&
                                      visible.find("Build") == std::string::npos && visible.find("OpenAI") == std::string::npos &&
                                      visible.find("cwd") == std::string::npos && visible.find("git") == std::string::npos &&
                                      visible.find("entries") == std::string::npos && visible.find("1.3k (0.7%)") == std::string::npos;
                             }),
         "tui compact footer shows only the model name and active context usage");

  auto const refreshed_context_lines = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                            .provider = "openai",
                                                                                            .model = "gpt-5.5",
                                                                                            .session_id = "session_test",
                                                                                            .input = "",
                                                                                            .status = "ready",
                                                                                            .active_context_status = "~1.2k",
                                                                                            .context_source_count = 3,
                                                                                            .transcript = {},
                                                                                            .width = 110,
                                                                                            .height = 10,
                                                                                            .sidebar = ava::tui::SidebarSnapshot{.context_source_count = 2}});
  expect(std::ranges::any_of(refreshed_context_lines,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("GPT-5.5 · ctx ~1.2k") != std::string::npos && visible.find("ctx 2") == std::string::npos &&
                                      visible.find("ctx 3") == std::string::npos;
                             }),
         "tui composer footer uses refreshed active context usage rather than sidebar source metadata");

  auto const source_only_footer_lines = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                             .provider = "openai",
                                                                                             .model = "gpt-5.5",
                                                                                             .session_id = "session_test",
                                                                                             .input = "",
                                                                                             .status = "ready",
                                                                                             .context_source_count = 3,
                                                                                             .transcript = {},
                                                                                             .width = 80,
                                                                                             .height = 10,
                                                                                             .sidebar = ava::tui::SidebarSnapshot{.context_source_count = 2}});
  expect(std::ranges::any_of(source_only_footer_lines,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("GPT-5.5") != std::string::npos && visible.find("ctx ") == std::string::npos;
                             }),
         "tui composer footer omits context usage when only instruction-source counts are known");
}
void run_tui_composer_rendering_tests_part_2()
{
  auto const rows_transcript = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "ready",
      .transcript = {ava::tui::TranscriptItem{.label = "error", .text = "bad command"}, ava::tui::TranscriptItem{.label = "command", .text = "/help"}},
      .width = 50,
      .height = 10});
  expect(std::ranges::any_of(rows_transcript, [](std::string const& line) { return strip_sgr(line).find("! bad command") != std::string::npos; }) &&
             std::ranges::any_of(rows_transcript, [](std::string const& line) { return strip_sgr(line).find("· /help") != std::string::npos; }),
         "tui keeps errors and generic command rows distinct from message blocks");
  auto const onboarding_transcript =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_test",
                                                           .input = "",
                                                           .status = "ready",
                                                           .transcript = {ava::tui::TranscriptItem{.label = "setup",
                                                                                                   .text = "Provider auth is not configured for `openai`.\n"
                                                                                                           "Connect with /connect or /login in this TUI.\n"
                                                                                                           "Auth file: /tmp/ava/auth.json"}},
                                                           .width = 72,
                                                           .height = 12});
  expect(std::ranges::any_of(onboarding_transcript,
                             [](std::string const& line) { return strip_sgr(line).find("Provider auth is not configured") != std::string::npos; }) &&
             std::ranges::any_of(onboarding_transcript,
                                 [](std::string const& line) { return strip_sgr(line).find("Connect with /connect or /login") != std::string::npos; }) &&
             std::ranges::all_of(onboarding_transcript, [](std::string const& line) { return visible_columns(line) <= 72; }),
         "tui renders first-run setup transcript guidance without width overflow");
  auto const disconnected = std::vector<ava::tui::TranscriptItem>{ava::tui::TranscriptItem{.label = "setup", .text = "! OpenAI not connected · /connect"}};
  auto const disconnected_styled = ava::tui::detail::render_transcript_lines(disconnected, 72, false, true, false);
  std::vector<std::string> disconnected_plain;
  {
    ScopedEnvVar no_color_guard("NO_COLOR", "1");
    disconnected_plain = ava::tui::detail::render_transcript_lines(disconnected, 72, false, true, false);
  }
  auto const disconnected_visible = disconnected_styled.empty() ? std::string{} : strip_sgr(disconnected_styled.front());
  expect(disconnected_styled.size() == 1 && disconnected_plain.size() == 1 && disconnected_plain.front() == disconnected_visible &&
             disconnected_visible.find("· !") == std::string::npos && disconnected_visible.find("! OpenAI not connected · /connect") != std::string::npos,
         "tui disconnected startup guidance renders one logical warning marker with styled/plain parity");

  auto const compact_status = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "Enter submits. Shift/Ctrl+Enter inserts newline. Alt+Enter queues follow-up. / opens commands.",
                                 .transcript = {},
                                 .width = 120,
                                 .height = 8});
  expect(std::ranges::none_of(compact_status, [](std::string const& line) { return strip_sgr(line).find("Alt+Enter queues follow-up") != std::string::npos; }),
         "tui keeps the composer status compact instead of rendering verbose help");
  auto const status_alert = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "invalid_argument: conflicting TUI keybinding\n  key: Ctrl+P\n  action: model_cycle_forward\x1b[31m",
                                 .transcript = {},
                                 .width = 96,
                                 .height = 10});
  expect(
      std::ranges::any_of(
          status_alert, [](std::string const& line) { return strip_sgr(line).find("! invalid_argument: conflicting TUI keybinding") != std::string::npos; }) &&
          std::ranges::any_of(status_alert, [](std::string const& line) { return strip_sgr(line).find("key: Ctrl+P") != std::string::npos; }) &&
          std::ranges::none_of(status_alert, [](std::string const& line) { return line.find("\x1b[31m") != std::string::npos; }) &&
          std::ranges::all_of(status_alert, [](std::string const& line) { return visible_columns(line) <= 96; }),
      "tui renders error-category status strings as compact sanitized alerts above the composer");

  auto const disabled_statuses =
      std::vector<std::string>{"command disabled: cloud sharing is deferred", "reference disabled: outside workspace", "path disabled: outside workspace"};
  auto const disabled_status_alerts_visible = std::ranges::all_of(disabled_statuses, [](std::string const& status) {
    auto const frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                            .provider = "openai",
                                                                            .model = "gpt-5.5",
                                                                            .session_id = "session_test",
                                                                            .input = "/share",
                                                                            .status = status,
                                                                            .transcript = {},
                                                                            .width = 96,
                                                                            .height = 10});
    return std::ranges::any_of(frame, [&status](std::string const& line) { return strip_sgr(line).find("! " + status) != std::string::npos; });
  });
  expect(disabled_status_alerts_visible, "tui renders command, reference, and path disabled statuses as compact dock-aware alerts");

  auto const prioritized_status_alert = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "invalid_argument: first alert line\nsecond alert line\nthird alert line\nfourth alert line",
      .transcript = {},
      .width = 96,
      .height = 9,
      .queued_messages = {ava::tui::QueuedMessageItem{.id = "q1", .kind = "follow-up", .text = "queue one"},
                          ava::tui::QueuedMessageItem{.id = "q2", .kind = "follow-up", .text = "queue two"},
                          ava::tui::QueuedMessageItem{.id = "q3", .kind = "follow-up", .text = "queue three"}},
      .pending_attachments = {ava::tui::PendingAttachmentItem{.label = "first.png"}, ava::tui::PendingAttachmentItem{.label = "second.png"}}});
  expect(prioritized_status_alert.size() == 9 && strip_sgr(prioritized_status_alert[0]).find("queued follow-up queue one") != std::string::npos &&
             strip_sgr(prioritized_status_alert[2]).find("queued follow-up queue three") != std::string::npos &&
             strip_sgr(prioritized_status_alert[3]).find("attached +1 more images") != std::string::npos &&
             strip_sgr(prioritized_status_alert[4]).find("! invalid_argument: first alert line") != std::string::npos &&
             strip_sgr(prioritized_status_alert[5]).find("second alert line") != std::string::npos &&
             strip_sgr(prioritized_status_alert[6]).find("third alert line ...") != std::string::npos &&
             strip_sgr(prioritized_status_alert[7]).starts_with("│  Type a message...") && strip_sgr(prioritized_status_alert[8]).starts_with("│  GPT-5.5"),
         "tui reserves a three-line status alert before queue and attachment budgets and renders it immediately above the two-row composer");

  auto const minimum_width = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                  .provider = "openai",
                                                                                  .model = "gpt-5.5",
                                                                                  .session_id = "session_test",
                                                                                  .input = "hello",
                                                                                  .status = "ready",
                                                                                  .transcript = {},
                                                                                  .width = 1,
                                                                                  .height = 1});
  expect(std::ranges::all_of(minimum_width, [](std::string const& line) { return line.find('\n') == std::string::npos && visible_columns(line) <= 20; }) &&
             std::ranges::any_of(minimum_width, [](std::string const& line) { return strip_sgr(line).find("│  hello") != std::string::npos; }),
         "tui clamps normal composer rendering to the minimum viewport");
}
void run_tui_composer_rendering_tests_part_3()
{
  auto const sanitized =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_test",
                                                           .input = "",
                                                           .status = "bad\x1b[31mstatus",
                                                           .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "bad\x1b[31mred"}},
                                                           .width = 80,
                                                           .height = 8});
  expect(std::ranges::any_of(sanitized,
                             [](std::string const& line) {
                               auto visible = strip_sgr(line);
                               return visible.find("?[31mred") != std::string::npos;
                             }),
         "tui render sanitizes transcript escape bytes in user content");
  auto const sanitized_input = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                    .provider = "openai",
                                                                                    .model = "gpt-5.5",
                                                                                    .session_id = "session_test",
                                                                                    .input = "bad\x1b[31mred",
                                                                                    .status = "ready",
                                                                                    .transcript = {},
                                                                                    .width = 80,
                                                                                    .height = 8});
  expect(std::ranges::any_of(sanitized_input, [](std::string const& line) { return strip_sgr(line).find("│  bad?[31mred") != std::string::npos; }),
         "tui render sanitizes composer input escape bytes");
  expect(ava::tui::sanitize_terminal_text(std::string("osc") + static_cast<char>(0x9D) + "payload") == "osc?payload",
         "tui sanitizes raw c1 terminal control bytes");
  expect(ava::tui::sanitize_terminal_text("a\tb") == "a  b", "tui expands tabs before width accounting");
  expect(ava::tui::sanitize_terminal_text(std::string("ok ") + "\xC3\xA9") == std::string("ok ") + "\xC3\xA9", "tui sanitizer preserves valid utf-8 text");
  expect(ava::tui::sanitize_terminal_text(std::string("x") + std::string("\xC0\x80", 2) + "y") == "x??y",
         "tui sanitizer rejects overlong two-byte utf-8 controls");
  expect(ava::tui::sanitize_terminal_text(std::string("x") + std::string("\xE0\x80\x80", 3) + "y") == "x???y",
         "tui sanitizer rejects overlong three-byte utf-8 forms");
  expect(ava::tui::sanitize_terminal_text(std::string("x") + std::string("\xF0\x80\x80\x80", 4) + "y") == "x????y",
         "tui sanitizer rejects overlong four-byte utf-8 forms");
  expect(ava::tui::sanitize_terminal_text(std::string("x") + std::string("\xE2\x82", 2)) == "x??",
         "tui sanitizer replaces truncated utf-8 at the string boundary");
  expect(ava::tui::sanitize_terminal_text(std::string("x") + std::string("\xED\xA0\x80", 3) + "y") == "x???y",
         "tui sanitizer rejects utf-8 surrogate codepoints");
  expect(ava::tui::sanitize_terminal_text(std::string("x") + std::string("\xF4\x90\x80\x80", 4) + "y") == "x????y",
         "tui sanitizer rejects utf-8 codepoints above the unicode maximum");
  expect(ava::tui::sanitize_terminal_text(std::string("nul") + std::string(1, '\0') + "byte") == "nul?byte",
         "tui sanitizer replaces binary-like NUL bytes with a visible marker");
  expect(ava::tui::detail::terminal_text_columns("\xE7\x95\x8C") == 2 && ava::tui::detail::terminal_text_columns(std::string("e") + "\xCC\x81") == 1 &&
             ava::tui::detail::terminal_text_columns(std::string("a") + "\xE2\x80\x8D" + "b") == 2 &&
             ava::tui::detail::terminal_text_columns(std::string("\xE2\x98\xBA") + "\xEF\xB8\x8F") >= 1,
         "tui width accounting handles CJK width and treats combining marks, zero-width joiners, and variation "
         "selectors as non-advancing");
  auto const regional_c = std::string("\xF0\x9F\x87\xA8");
  auto const regional_n = std::string("\xF0\x9F\x87\xB3");
  auto const thumbs_up = std::string("\xF0\x9F\x91\x8D");
  auto const light_skin_tone = std::string("\xF0\x9F\x8F\xBB");
  auto const man = std::string("\xF0\x9F\x91\xA8");
  auto const laptop = std::string("\xF0\x9F\x92\xBB");
  auto const zwj = std::string("\xE2\x80\x8D");
  auto const check_mark = std::string("\xE2\x9C\x85");
  auto const lightning = std::string("\xE2\x9A\xA1");
  auto const variation_16 = std::string("\xEF\xB8\x8F");
  auto const white_flag = std::string("\xF0\x9F\x8F\xB3");
  auto const rainbow = std::string("\xF0\x9F\x8C\x88");
  expect(ava::tui::detail::terminal_text_columns(regional_c) == 2 && ava::tui::detail::terminal_text_columns(regional_c + regional_n) == 2 &&
             ava::tui::detail::terminal_text_columns("      - " + regional_c) == 10 &&
             ava::tui::detail::terminal_text_columns(thumbs_up + light_skin_tone) == 2 && ava::tui::detail::terminal_text_columns(man + zwj + laptop) == 2 &&
             ava::tui::detail::terminal_text_columns(check_mark) == 2 && ava::tui::detail::terminal_text_columns(lightning) == 2 &&
             ava::tui::detail::terminal_text_columns(lightning + variation_16) == 2 &&
             ava::tui::detail::terminal_text_columns(white_flag + variation_16 + zwj + rainbow) == 2,
         "tui width accounting treats Pi-style regional indicators and emoji modifier/ZWJ clusters as stable wide cells");
  auto const partial_flag_wrap = ava::tui::detail::wrap_transcript_text("      - " + regional_c, 13);
  expect(partial_flag_wrap.size() == 2 && ava::tui::detail::terminal_text_columns(partial_flag_wrap[0]) <= 9 &&
             ava::tui::detail::terminal_text_columns(partial_flag_wrap[1]) == 2,
         "tui transcript wrapping breaks Pi-style partial-flag list lines before terminal overflow");
  auto const clipped_regional_indicator = ava::tui::detail::fit_line("x" + regional_c + "y", 2);
  expect(ava::tui::detail::terminal_text_columns(clipped_regional_indicator) <= 2, "tui narrow fitting does not undercount singleton regional indicators");
  auto const clipped_zwj_cluster = ava::tui::detail::fit_line(man + zwj + laptop + "x", 2);
  expect(clipped_zwj_cluster == man + zwj + laptop, "tui narrow fitting keeps emoji ZWJ clusters intact when they fit exactly");
  expect(ava::tui::detail::composer_input_prefix_columns(true) == 3 && ava::tui::detail::composer_input_prefix_columns(false) == 3,
         "tui composer input and continuation rows share one three-column boundary and gutter");
  auto const cursor_base = ava::tui::detail::composer_input_prefix_columns(true) + 1;
  auto const cursor_for = [](std::string input, std::size_t cursor) {
    return ava::tui::detail::input_cursor_column(ava::tui::ComposerSnapshot{.mode = "build",
                                                                            .provider = "openai",
                                                                            .model = "gpt-5.5",
                                                                            .session_id = "session_test",
                                                                            .input = std::move(input),
                                                                            .status = "ready",
                                                                            .transcript = {},
                                                                            .input_cursor = cursor},
                                                 120);
  };
  auto const cursor_text = std::string("a") + "\xE7\x95\x8C" + "e" + "\xCC\x81";
  expect(cursor_for(cursor_text, 1) == cursor_base + 1 && cursor_for(cursor_text, 4) == cursor_base + 3 && cursor_for(cursor_text, 5) == cursor_base + 4 &&
             cursor_for(cursor_text, cursor_text.size()) == cursor_base + 4 && cursor_for(std::string("x") + std::string("\xC0\x80", 2), 3) == cursor_base + 3,
         "tui composer cursor placement uses sanitized display columns for CJK, combining marks, and invalid utf-8");
  auto const wrapped_input = ava::tui::detail::input_render_line_spans("alpha beta gamma delta", 20);
  expect(wrapped_input.size() == 2 && wrapped_input[0].text == "alpha beta gamma " && wrapped_input[0].start == 0 &&
             wrapped_input[0].end == std::string("alpha beta gamma ").size() && wrapped_input[0].first_line && wrapped_input[1].text == "delta" &&
             wrapped_input[1].start == std::string("alpha beta gamma ").size() && !wrapped_input[1].first_line,
         "tui composer wraps long input at word boundaries while preserving source offsets");
  auto const wrapped_long_word = ava::tui::detail::input_render_line_spans("abcdefghijklmnopqr", 20);
  expect(wrapped_long_word.size() == 2 && wrapped_long_word[0].text == "abcdefghijklmnopq" && wrapped_long_word[1].text == "r",
         "tui composer falls back to cell-level wrapping for long unbroken input tokens");
  auto const cjk_wrap_text = std::string("\xE7\x95\x8C\xE7\x95\x8C\xE7\x95\x8C\xE7\x95\x8C\xE7\x95\x8C\xE7\x95\x8C\xE7\x95\x8C\xE7\x95\x8C\xE7\x95\x8C");
  auto const wrapped_cjk_input = ava::tui::detail::input_render_line_spans(cjk_wrap_text, 20);
  expect(wrapped_cjk_input.size() == 2 && ava::tui::detail::terminal_text_columns(wrapped_cjk_input[0].text) == 16 &&
             ava::tui::detail::terminal_text_columns(wrapped_cjk_input[1].text) == 2,
         "tui composer wraps CJK input on full UTF-8 cell boundaries");
  auto const wrapped_cursor_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                                  .provider = "openai",
                                                                  .model = "gpt-5.5",
                                                                  .session_id = "session_test",
                                                                  .input = "alpha beta gamma delta",
                                                                  .status = "ready",
                                                                  .transcript = {},
                                                                  .width = 20,
                                                                  .height = 8,
                                                                  .input_cursor = std::string::npos};
  expect(ava::tui::detail::input_cursor_line(wrapped_cursor_snapshot, 20) == 1 &&
             ava::tui::detail::input_cursor_column(wrapped_cursor_snapshot, 20) ==
                 ava::tui::detail::composer_input_prefix_columns(false) + std::string("delta").size() + 1,
         "tui composer places the cursor on the wrapped continuation row");
  auto const wrapped_render = ava::tui::render_composer(wrapped_cursor_snapshot);
  expect(std::ranges::any_of(wrapped_render, [](std::string const& line) { return strip_sgr(line).find("│  alpha beta gamma ") != std::string::npos; }) &&
             std::ranges::any_of(wrapped_render, [](std::string const& line) { return strip_sgr(line).find("│  delta") != std::string::npos; }),
         "tui composer renders wrapped input as visible continuation rows");
  auto const wrapped_click =
      ava::tui::composer_input_cursor_for_screen_position(wrapped_cursor_snapshot, 7, ava::tui::detail::composer_input_prefix_columns(false) + 3);
  expect(wrapped_click && *wrapped_click == std::string("alpha beta gamma de").size(),
         "tui composer hit-tests wrapped input continuation rows to source cursor offsets");
  auto const click_cursor_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                                .provider = "openai",
                                                                .model = "gpt-5.5",
                                                                .session_id = "session_test",
                                                                .input = "alpha beta",
                                                                .status = "ready",
                                                                .transcript = {},
                                                                .width = 80,
                                                                .height = 8};
  auto const clicked_after_alpha = ava::tui::composer_input_cursor_for_screen_position(click_cursor_snapshot, 7, cursor_base + std::string("alpha ").size());
  expect(clicked_after_alpha && *clicked_after_alpha == std::string("alpha ").size() &&
             !ava::tui::composer_input_cursor_for_screen_position(click_cursor_snapshot, 1, cursor_base) &&
             !ava::tui::composer_input_cursor_for_screen_position(click_cursor_snapshot, 8, cursor_base),
         "tui composer hit-tests visible input rows to draft cursor byte offsets and ignores non-input rows");
  auto centered_click_snapshot = click_cursor_snapshot;
  centered_click_snapshot.width = 160;
  auto const centered_clicked_after_alpha =
      ava::tui::composer_input_cursor_for_screen_position(centered_click_snapshot, 7, 20 + cursor_base + std::string("alpha ").size());
  expect(centered_clicked_after_alpha && *centered_clicked_after_alpha == std::string("alpha ").size() &&
             !ava::tui::composer_input_cursor_for_screen_position(centered_click_snapshot, 7, 20) &&
             ava::tui::composer_input_cursor_for_screen_position(centered_click_snapshot, 7, 21).has_value() &&
             ava::tui::composer_input_cursor_for_screen_position(centered_click_snapshot, 7, 140).has_value() &&
             !ava::tui::composer_input_cursor_for_screen_position(centered_click_snapshot, 7, 141),
         "tui centered composer click maps physical columns locally, accepts both canvas edges, and rejects both exact gutters");
  auto const multiline_click_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                                   .provider = "openai",
                                                                   .model = "gpt-5.5",
                                                                   .session_id = "session_test",
                                                                   .input = "one\ntwo\nthree",
                                                                   .status = "ready",
                                                                   .transcript = {},
                                                                   .width = 80,
                                                                   .height = 8};
  auto const clicked_second_line = ava::tui::composer_input_cursor_for_screen_position(multiline_click_snapshot, 6, cursor_base + 1);
  expect(clicked_second_line && *clicked_second_line == std::string("one\nt").size(),
         "tui composer hit-tests multiline visible input rows to the matching logical line cursor");
  auto const wide_click_text = std::string("a") + "\xE7\x95\x8C" + "b";
  auto const wide_click_cursor = ava::tui::composer_input_cursor_for_screen_position(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                                                .provider = "openai",
                                                                                                                .model = "gpt-5.5",
                                                                                                                .session_id = "session_test",
                                                                                                                .input = wide_click_text,
                                                                                                                .status = "ready",
                                                                                                                .transcript = {},
                                                                                                                .width = 80,
                                                                                                                .height = 8},
                                                                                     7, cursor_base + 3);
  expect(wide_click_cursor && *wide_click_cursor == std::string("a").size() + std::string("\xE7\x95\x8C").size(),
         "tui composer click-to-cursor clamps through wide utf-8 cells without landing inside a codepoint");
  auto const selected_input_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                         .provider = "openai",
                                                                                         .model = "gpt-5.5",
                                                                                         .session_id = "session_test",
                                                                                         .input = "alpha beta",
                                                                                         .status = "ready",
                                                                                         .transcript = {},
                                                                                         .width = 80,
                                                                                         .height = 8,
                                                                                         .input_cursor = 10,
                                                                                         .input_selection_start = 6,
                                                                                         .input_selection_end = 10});
  expect(std::ranges::any_of(selected_input_frame,
                             [](std::string const& line) {
                               return line.find(std::string(ava::tui::detail::kReverseVideo) + "beta") != std::string::npos &&
                                      strip_sgr(line).find("│  alpha beta") != std::string::npos;
                             }),
         "tui composer renders selected input text with reverse video without changing visible draft text");

  auto const composer_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "plan",
                                                                                   .provider = "openai",
                                                                                   .model = "gpt-5.5",
                                                                                   .session_id = "session_test",
                                                                                   .input = "hello",
                                                                                   .status = "ready",
                                                                                   .transcript = {},
                                                                                   .width = 40,
                                                                                   .height = 8});
  expect(composer_frame.size() == 8, "tui composer frame fills the requested terminal height");
  expect(std::ranges::any_of(composer_frame, [](std::string const& line) { return strip_sgr(line).find("│  hello") != std::string::npos; }),
         "tui composer frame renders the input prompt content");
  auto const wide_frame = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = std::string("wide ") + "\xE6\xBC\xA2\xE6\xBC\xA2\xF0\x9F\x98\x80",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "\xE6\xBC\xA2\xE6\xBC\xA2\xE6\xBC\xA2\xE6\xBC\xA2"}},
                                 .width = 24,
                                 .height = 10});
  expect(std::ranges::all_of(wide_frame, [](std::string const& line) { return visible_columns(line) <= 24; }),
         "tui treats CJK and emoji as wide cells when fitting rendered lines");
  ava::tui::clear_terminal_signal();
  expect(!ava::tui::terminal_signal_received(), "tui terminal signal state can be cleared before curses entry");
}
void run_tui_composer_rendering_tests_part_4()
{
  auto const sidebar_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "ready",
      .transcript = {ava::tui::TranscriptItem{.label = "you", .text = "hello"}},
      .width = 144,
      .height = 22,
      .sidebar = ava::tui::SidebarSnapshot{.activity = {ava::tui::SidebarActivityItem{
                                               .id = "call_1", .label = "bash", .detail = "running tests", .status = ava::tui::ToolTimelineStatus::Running}},
                                           .modified_files = {ava::tui::SidebarModifiedFile{.path = "src/ava/tui/runtime.cpp", .added = 12, .removed = 3}},
                                           .session_id = "session_test\x1b[31m",
                                           .mode = "build\x1b[31m",
                                           .provider = "openai\x1b[31m",
                                           .model = "gpt-5.5\x1b[31m",
                                           .workspace = "/workspace/project\x1b[31m",
                                           .git_branch = "develop\x1b[31m",
                                           .version = "0.32",
                                           .token_status = "1.2k (4.0%)",
                                           .reasoning_status = "low\x1b[31m",
                                           .context_source_count = 2}});
  expect(std::ranges::any_of(sidebar_frame,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("Activity") != std::string::npos && visible.find("Modified Files") == std::string::npos;
                             }) &&
             std::ranges::any_of(sidebar_frame,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("bash") != std::string::npos && visible.find("running tests") != std::string::npos;
                                 }) &&
             std::ranges::any_of(sidebar_frame,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("src/ava/tui/runtime.cpp") != std::string::npos && visible.find("+12") != std::string::npos &&
                                          visible.find("-3") != std::string::npos;
                                 }) &&
             std::ranges::any_of(sidebar_frame,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("branch develop") != std::string::npos || visible.find("AVA 0.32") != std::string::npos;
                                 }) &&
             std::ranges::any_of(sidebar_frame, [](std::string const& line) { return strip_sgr(line).find("usage 1.2k (4.0%)") != std::string::npos; }) &&
             std::ranges::any_of(sidebar_frame, [](std::string const& line) { return strip_sgr(line).find("reasoning low") != std::string::npos; }) &&
             std::ranges::any_of(sidebar_frame, [](std::string const& line) { return strip_sgr(line).find("context sources 2") != std::string::npos; }) &&
             std::ranges::none_of(sidebar_frame, [](std::string const& line) { return line.find("\x1b[31m") != std::string::npos; }) &&
             std::ranges::all_of(sidebar_frame, [](std::string const& line) { return visible_columns(line) <= 144; }),
         "tui renders curated running activity, modified files, and known Context metadata");
  expect(std::ranges::any_of(sidebar_frame,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               auto const activity = visible.find("Activity");
                               auto const separator = visible.find("│");
                               return activity != std::string::npos && separator != std::string::npos && separator < activity && activity >= 106;
                             }),
         "tui pads blank main rows so sidebar content stays in the right column");

  auto const idle_after_completed_activity_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "ready",
      .transcript = {},
      .width = 176,
      .height = 18,
      .sidebar = ava::tui::SidebarSnapshot{
          .activity = {ava::tui::SidebarActivityItem{
              .id = "responding", .label = "responding", .detail = "assistant responded", .status = ava::tui::ToolTimelineStatus::Success}}}});
  expect(
      std::ranges::any_of(idle_after_completed_activity_frame, [](std::string const& line) { return strip_sgr(line).find("Session") != std::string::npos; }) &&
          std::ranges::none_of(idle_after_completed_activity_frame,
                               [](std::string const& line) { return strip_sgr(line).find("Activity") != std::string::npos; }) &&
          std::ranges::none_of(idle_after_completed_activity_frame,
                               [](std::string const& line) { return strip_sgr(line).find("assistant responded") != std::string::npos; }) &&
          std::ranges::none_of(idle_after_completed_activity_frame, [](std::string const& line) { return strip_sgr(line).find("idle") != std::string::npos; }),
      "tui automatic rail omits completed assistant activity history and idle placeholders");

  auto const unknown_sidebar_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                          .provider = "openai",
                                                                                          .model = "gpt-5.5",
                                                                                          .session_id = "session_test",
                                                                                          .input = "",
                                                                                          .status = "ready",
                                                                                          .transcript = {},
                                                                                          .width = 176,
                                                                                          .height = 18,
                                                                                          .sidebar = ava::tui::SidebarSnapshot{.session_id = "session_test",
                                                                                                                               .mode = "build",
                                                                                                                               .provider = "openai",
                                                                                                                               .git_branch = "unknown",
                                                                                                                               .token_status = "tokens unknown",
                                                                                                                               .reasoning_status = "unknown"}});
  expect(std::ranges::none_of(unknown_sidebar_frame, [](std::string const& line) { return strip_sgr(line).find("Context") != std::string::npos; }) &&
             std::ranges::none_of(unknown_sidebar_frame, [](std::string const& line) { return strip_sgr(line).find("unknown") != std::string::npos; }),
         "tui automatic rail omits the Context group and placeholders when all context values are unknown");

  auto const legitimate_unknown_substring_frame =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "unknown-labs",
                                                           .model = "model-unknown-v2",
                                                           .session_id = "session_known_values",
                                                           .input = "",
                                                           .status = "ready",
                                                           .transcript = {},
                                                           .width = 176,
                                                           .height = 18,
                                                           .sidebar = ava::tui::SidebarSnapshot{.mode = "build",
                                                                                                .provider = "unknown-labs",
                                                                                                .model = "model-unknown-v2",
                                                                                                .git_branch = "fix/unknown-token-count",
                                                                                                .token_status = "tokens unknown",
                                                                                                .reasoning_status = "UnKnOwN"}});
  expect(std::ranges::any_of(legitimate_unknown_substring_frame,
                             [](std::string const& line) { return strip_sgr(line).find("build · unknown-labs/model-unknow") != std::string::npos; }) &&
             std::ranges::any_of(legitimate_unknown_substring_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("branch fix/unknown-token-count") != std::string::npos; }) &&
             std::ranges::none_of(legitimate_unknown_substring_frame,
                                  [](std::string const& line) { return strip_sgr(line).find("usage tokens unknown") != std::string::npos; }) &&
             std::ranges::none_of(legitimate_unknown_substring_frame,
                                  [](std::string const& line) { return strip_sgr(line).find("reasoning UnKnOwN") != std::string::npos; }),
         "tui automatic rail preserves legitimate values containing unknown while omitting exact normalized unknown sentinels");

  auto const zero_context_sidebar_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_test",
      .input = "",
      .status = "ready",
      .transcript = {},
      .width = 176,
      .height = 18,
      .sidebar = ava::tui::SidebarSnapshot{.session_id = "session_test", .mode = "build", .provider = "openai", .context_source_count = 0}});
  expect(
      std::ranges::any_of(zero_context_sidebar_frame, [](std::string const& line) { return strip_sgr(line).find("context sources 0") != std::string::npos; }),
      "tui automatic rail distinguishes a known zero context source count from unknown context data");

  auto const long_session_sidebar_frame =
      ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                           .provider = "openai",
                                                           .model = "gpt-5.5",
                                                           .session_id = "session_test",
                                                           .input = "",
                                                           .status = "ready",
                                                           .transcript = {},
                                                           .width = 176,
                                                           .height = 20,
                                                           .sidebar = ava::tui::SidebarSnapshot{.session_id = "session_test",
                                                                                                .mode = "build",
                                                                                                .provider = "openai",
                                                                                                .model = "gpt-5.5",
                                                                                                .workspace = "/workspace/project",
                                                                                                .git_branch = "develop",
                                                                                                .version = "0.32",
                                                                                                .token_status = "180k (90.0%)",
                                                                                                .reasoning_status = std::nullopt,
                                                                                                .context_source_count = 3,
                                                                                                .session_path = "/tmp/ava/sessions/session_test.jsonl",
                                                                                                .session_entry_count = 42}});
  expect(
      std::ranges::none_of(long_session_sidebar_frame,
                           [](std::string const& line) { return strip_sgr(line).find("path /tmp/ava/sessions") != std::string::npos; }) &&
          std::ranges::none_of(long_session_sidebar_frame, [](std::string const& line) { return strip_sgr(line).find("entries 42") != std::string::npos; }) &&
          std::ranges::any_of(long_session_sidebar_frame,
                              [](std::string const& line) {
                                auto const visible = strip_sgr(line);
                                return visible.find("context pressure critical 90.0%") != std::string::npos;
                              }),
      "tui automatic rail shows critical context pressure while omitting raw session path and entry count");

  auto const curated_idle_sidebar = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "outer_session",
      .input = "",
      .status = "ready",
      .transcript = {},
      .width = 176,
      .height = 22,
      .sidebar = ava::tui::SidebarSnapshot{
          .activity = {ava::tui::SidebarActivityItem{
              .id = "completed", .label = "completed-history", .detail = "must stay hidden", .status = ava::tui::ToolTimelineStatus::Success}},
          .session_id = "raw-session-id-must-stay-hidden",
          .mode = "build",
          .provider = "openai",
          .model = "gpt-5.5",
          .workspace = "/raw/workspace/must/stay/hidden",
          .version = "9.9.9-must-stay-hidden",
          .token_status = std::nullopt,
          .reasoning_status = std::nullopt,
          .context_source_count = std::nullopt,
          .session_path = "/raw/session/path/must/stay/hidden.jsonl",
          .session_entry_count = 999}});
  auto const curated_idle_text = tui_test_support::join_visible_lines(curated_idle_sidebar);
  auto const curated_idle_title_line =
      std::ranges::find_if(curated_idle_sidebar, [](std::string const& line) { return strip_sgr(line).find("Session") != std::string::npos; });
  auto const curated_idle_title_visible = curated_idle_title_line == curated_idle_sidebar.end() ? std::string{} : strip_sgr(*curated_idle_title_line);
  auto const curated_idle_divider = curated_idle_title_visible.find("│");
  auto curated_idle_footer = strip_sgr(curated_idle_sidebar.back());
  auto const curated_idle_footer_divider = curated_idle_footer.rfind("│");
  if (curated_idle_footer_divider != std::string::npos)
    curated_idle_footer.erase(curated_idle_footer_divider);
  while (!curated_idle_footer.empty() && curated_idle_footer.back() == ' ')
    curated_idle_footer.pop_back();
  expect(curated_idle_title_line != curated_idle_sidebar.end() && curated_idle_divider != std::string::npos &&
             curated_idle_title_visible.find("│", curated_idle_divider + std::string_view("│").size()) == std::string::npos &&
             curated_idle_title_visible.substr(curated_idle_divider).starts_with("│  Session") &&
             curated_idle_text.find("build · openai/gpt-5.5") != std::string::npos && curated_idle_text.find("AVA") == std::string::npos &&
             curated_idle_text.find("live session") == std::string::npos && curated_idle_text.find("Activity") == std::string::npos &&
             curated_idle_text.find("Modified Files") == std::string::npos && curated_idle_text.find("idle") == std::string::npos &&
             curated_idle_text.find("no file changes") == std::string::npos && curated_idle_text.find("unknown") == std::string::npos &&
             curated_idle_text.find("raw-session-id") == std::string::npos && curated_idle_text.find("/raw/session/path") == std::string::npos &&
             curated_idle_text.find("/raw/workspace") == std::string::npos && curated_idle_text.find("999") == std::string::npos &&
             curated_idle_text.find("9.9.9") == std::string::npos && curated_idle_footer == "│  GPT-5.5" &&
             std::ranges::all_of(curated_idle_sidebar,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   auto const divider = visible.rfind("│");
                                   return divider != std::string::npos && visible_columns(visible.substr(0, divider)) == 137 && visible_columns(line) <= 176;
                                 }),
         "tui idle automatic rail is a bounded two-cell-inset Session summary with the quiet footer and no placeholders or drawer-only metadata");

  auto const curated_populated_sidebar = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build\x1b[31m",
      .provider = "openai\x1b[31m",
      .model = "gpt-5.5\x1b[31m",
      .session_id = "outer_session",
      .input = "",
      .status = "thinking...",
      .processing = true,
      .transcript = {},
      .width = 144,
      .height = 24,
      .sidebar = ava::tui::SidebarSnapshot{
          .activity = {ava::tui::SidebarActivityItem{.id = "running",
                                                     .label = "running-task\x1b[31m",
                                                     .detail = "bounded-detail-that-is-deliberately-long-for-the-rail\x1b[31m",
                                                     .status = ava::tui::ToolTimelineStatus::Running},
                       ava::tui::SidebarActivityItem{
                           .id = "completed", .label = "completed-history-must-stay-hidden", .status = ava::tui::ToolTimelineStatus::Success}},
          .modified_files = {ava::tui::SidebarModifiedFile{.path = "src/curated-file.cpp\x1b[31m", .added = 12, .removed = 3}},
          .session_id = "raw-populated-session-must-stay-hidden",
          .mode = "build\x1b[31m",
          .provider = "openai\x1b[31m",
          .model = "gpt-5.5\x1b[31m",
          .workspace = "/raw/populated/workspace/must/stay/hidden",
          .git_branch = "develop\x1b[31m",
          .version = "8.8.8-must-stay-hidden",
          .token_status = "180k (90.0%)\x1b[31m",
          .reasoning_status = "high\x1b[31m",
          .context_source_count = 7,
          .session_path = "/raw/populated/session/path.jsonl",
          .session_entry_count = 42}});
  auto const curated_populated_text = tui_test_support::join_visible_lines(curated_populated_sidebar);
  auto const curated_populated_rail_lines = [&]() {
    std::vector<std::string> lines;
    for (auto const& line : curated_populated_sidebar)
    {
      auto visible = strip_sgr(line);
      auto const divider = visible.rfind("│");
      lines.push_back(divider == std::string::npos ? std::string{} : visible.substr(divider + std::string_view("│").size()));
    }
    return lines;
  }();
  auto const rail_group_row = [&](std::string_view title) {
    return static_cast<std::size_t>(std::ranges::find_if(curated_populated_rail_lines,
                                                         [title](std::string const& line) {
                                                           auto const first = line.find_first_not_of(' ');
                                                           auto const last = line.find_last_not_of(' ');
                                                           return first != std::string::npos && line.substr(first, last - first + 1) == title;
                                                         }) -
                                    curated_populated_rail_lines.begin());
  };
  auto const session_group_row = rail_group_row("Session");
  auto const activity_group_row = rail_group_row("Activity");
  auto const modified_group_row = rail_group_row("Modified Files");
  auto const context_group_row = rail_group_row("Context");
  auto const blank_before = [&](std::size_t row) {
    return row > 0 && row < curated_populated_rail_lines.size() && curated_populated_rail_lines[row - 1].find_first_not_of(' ') == std::string::npos;
  };
  expect(curated_populated_text.find("Session") != std::string::npos && curated_populated_text.find("Activity") != std::string::npos &&
             curated_populated_text.find("[~] running-task") != std::string::npos && curated_populated_text.find("Modified Files") != std::string::npos &&
             curated_populated_text.find("src/curated-file.cpp") != std::string::npos && curated_populated_text.find("+12") != std::string::npos &&
             curated_populated_text.find("-3") != std::string::npos && curated_populated_text.find("Context") != std::string::npos &&
             curated_populated_text.find("branch develop") != std::string::npos && curated_populated_text.find("reasoning high") != std::string::npos &&
             curated_populated_text.find("usage 180k (90.0%)") != std::string::npos &&
             curated_populated_text.find("context pressure critical 90.0%") != std::string::npos &&
             curated_populated_text.find("context sources 7") != std::string::npos && curated_populated_text.find("completed-history") == std::string::npos &&
             curated_populated_text.find("raw-populated-session") == std::string::npos && curated_populated_text.find("/raw/populated") == std::string::npos &&
             curated_populated_text.find("entries 42") == std::string::npos && curated_populated_text.find("8.8.8") == std::string::npos &&
             session_group_row == 0 && activity_group_row < curated_populated_rail_lines.size() && blank_before(activity_group_row) &&
             modified_group_row < curated_populated_rail_lines.size() && blank_before(modified_group_row) &&
             context_group_row < curated_populated_rail_lines.size() && blank_before(context_group_row) &&
             std::ranges::none_of(
                 curated_populated_sidebar, [](std::string const& line) { return line.find("\x1b[31m") != std::string::npos; }) &&
             std::ranges::all_of(
                 curated_populated_sidebar, [](std::string const& line) { return visible_columns(line) <= 144; }),
         "tui populated automatic rail shows only running activity, modified files, and known sanitized Context values with one blank between groups");
  {
    ScopedEnvVar no_color("NO_COLOR", "1");
    auto const plain_curated_rail = ava::tui::render_composer(
        ava::tui::ComposerSnapshot{.mode = "build",
                                   .provider = "openai",
                                   .model = "gpt-5.5",
                                   .session_id = "plain",
                                   .input = "",
                                   .status = "ready",
                                   .transcript = {},
                                   .width = 176,
                                   .height = 18,
                                   .sidebar = ava::tui::SidebarSnapshot{.mode = "build", .provider = "openai", .model = "gpt-5.5", .context_source_count = 0}});
    expect(std::ranges::any_of(plain_curated_rail, [](std::string const& line) { return line.find("│  Session") != std::string::npos; }) &&
               std::ranges::any_of(plain_curated_rail, [](std::string const& line) { return line.find("context sources 0") != std::string::npos; }) &&
               std::ranges::none_of(plain_curated_rail, [](std::string const& line) { return line.find('\x1b') != std::string::npos; }),
           "tui automatic rail keeps its textual hierarchy and known-zero Context in NO_COLOR mode");
  }

  auto const narrow_no_sidebar = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {},
                                 .width = 90,
                                 .height = 10,
                                 .sidebar = ava::tui::SidebarSnapshot{.activity = {ava::tui::SidebarActivityItem{.id = "a", .label = "sidebar-only"}}}});
  expect(std::ranges::none_of(narrow_no_sidebar, [](std::string const& line) { return strip_sgr(line).find("sidebar-only") != std::string::npos; }),
         "tui hides the sidebar on narrow terminals");

  auto canvas_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                    .provider = "openai",
                                                    .model = "gpt-5.5",
                                                    .session_id = "session_canvas",
                                                    .input = "centered draft",
                                                    .status = "ready",
                                                    .transcript = {},
                                                    .width = 119,
                                                    .height = 16};
  auto canvas_119 = ava::tui::composer_canvas_layout(canvas_snapshot);
  canvas_snapshot.width = 120;
  auto canvas_120 = ava::tui::composer_canvas_layout(canvas_snapshot);
  canvas_snapshot.width = 121;
  auto canvas_121 = ava::tui::composer_canvas_layout(canvas_snapshot);
  canvas_snapshot.width = 160;
  auto canvas_160 = ava::tui::composer_canvas_layout(canvas_snapshot);
  auto const centered_frame = ava::tui::render_composer(canvas_snapshot);
  auto const centered_input =
      std::ranges::find_if(centered_frame, [](std::string const& line) { return strip_sgr(line).find("centered draft") != std::string::npos; });
  expect(canvas_119.content_width == 119 && canvas_119.left == 0 && !canvas_119.rail_visible && canvas_120.content_width == 120 && canvas_120.left == 0 &&
             !canvas_120.rail_visible && canvas_121.content_width == 120 && canvas_121.left == 0 && !canvas_121.rail_visible &&
             canvas_160.content_width == 120 && canvas_160.left == 20 && !canvas_160.rail_visible && centered_input != centered_frame.end() &&
             strip_sgr(*centered_input).find("│  centered draft") == 20 &&
             std::ranges::all_of(centered_frame, [](std::string const& line) { return visible_columns(line) == 160; }),
         "tui ordinary canvas stays full width through 120 columns and becomes one exact centered 120-column frame above it");

  auto boundary_snapshot = ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_boundary",
      .input = "",
      .status = "ready",
      .transcript = {},
      .width = 143,
      .height = 16,
      .sidebar = ava::tui::SidebarSnapshot{
          .activity = {ava::tui::SidebarActivityItem{.id = "boundary", .label = "boundary-activity", .status = ava::tui::ToolTimelineStatus::Running}}}};
  auto boundary_frame = ava::tui::render_composer(boundary_snapshot);
  expect(ava::tui::composer_canvas_layout(boundary_snapshot).content_width == 120 && ava::tui::composer_canvas_layout(boundary_snapshot).left == 11 &&
             !ava::tui::composer_canvas_layout(boundary_snapshot).rail_visible &&
             std::ranges::none_of(boundary_frame, [](std::string const& line) { return strip_sgr(line).find("boundary-activity") != std::string::npos; }),
         "tui actionable automatic sidebar stays hidden and centers the canvas at 143x16");
  boundary_snapshot.width = 144;
  boundary_snapshot.height = 15;
  boundary_frame = ava::tui::render_composer(boundary_snapshot);
  expect(ava::tui::composer_canvas_layout(boundary_snapshot).content_width == 120 && ava::tui::composer_canvas_layout(boundary_snapshot).left == 12 &&
             !ava::tui::composer_canvas_layout(boundary_snapshot).rail_visible &&
             std::ranges::none_of(boundary_frame, [](std::string const& line) { return strip_sgr(line).find("boundary-activity") != std::string::npos; }),
         "tui actionable automatic sidebar stays hidden and centers the canvas at 144x15");
  boundary_snapshot.height = 16;
  boundary_frame = ava::tui::render_composer(boundary_snapshot);
  expect(ava::tui::composer_canvas_layout(boundary_snapshot).content_width == 105 && ava::tui::composer_canvas_layout(boundary_snapshot).left == 0 &&
             ava::tui::composer_canvas_layout(boundary_snapshot).rail_visible &&
             std::ranges::any_of(boundary_frame,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   auto const divider = visible.find("│");
                                   return divider != std::string::npos && visible_columns(visible.substr(0, divider)) == 105 &&
                                          visible.find("boundary-activity") != std::string::npos;
                                 }),
         "tui actionable automatic sidebar appears at 144x16 with a capped 38-column rail");

  auto modified_boundary_snapshot = boundary_snapshot;
  modified_boundary_snapshot.sidebar->activity.clear();
  modified_boundary_snapshot.sidebar->modified_files = {ava::tui::SidebarModifiedFile{.path = "boundary-file.cpp"}};
  modified_boundary_snapshot.width = 143;
  auto modified_boundary_frame = ava::tui::render_composer(modified_boundary_snapshot);
  expect(
      ava::tui::composer_canvas_layout(modified_boundary_snapshot).content_width == 120 &&
          ava::tui::composer_canvas_layout(modified_boundary_snapshot).left == 11 &&
          !ava::tui::composer_canvas_layout(modified_boundary_snapshot).rail_visible &&
          std::ranges::none_of(modified_boundary_frame, [](std::string const& line) { return strip_sgr(line).find("boundary-file.cpp") != std::string::npos; }),
      "tui automatic sidebar stays hidden and centers modified-file work at 143x16");
  modified_boundary_snapshot.width = 144;
  modified_boundary_frame = ava::tui::render_composer(modified_boundary_snapshot);
  expect(
      ava::tui::composer_main_width(modified_boundary_snapshot) == 105 &&
          std::ranges::any_of(modified_boundary_frame, [](std::string const& line) { return strip_sgr(line).find("boundary-file.cpp") != std::string::npos; }),
      "tui automatic sidebar appears for modified-file work at exactly 144x16");

  auto idle_boundary_snapshot = boundary_snapshot;
  idle_boundary_snapshot.sidebar->activity.clear();
  idle_boundary_snapshot.sidebar->mode = "build";
  idle_boundary_snapshot.sidebar->provider = "openai";
  idle_boundary_snapshot.sidebar->model = "gpt-5.5";
  idle_boundary_snapshot.width = 175;
  auto idle_boundary_frame = ava::tui::render_composer(idle_boundary_snapshot);
  expect(ava::tui::composer_canvas_layout(idle_boundary_snapshot).content_width == 120 && ava::tui::composer_canvas_layout(idle_boundary_snapshot).left == 27 &&
             !ava::tui::composer_canvas_layout(idle_boundary_snapshot).rail_visible &&
             std::ranges::none_of(idle_boundary_frame, [](std::string const& line) { return strip_sgr(line).find("Session") != std::string::npos; }),
         "tui idle automatic sidebar stays hidden and centers the canvas at 175x16");
  idle_boundary_snapshot.width = 176;
  idle_boundary_frame = ava::tui::render_composer(idle_boundary_snapshot);
  expect(ava::tui::composer_canvas_layout(idle_boundary_snapshot).content_width == 137 && ava::tui::composer_canvas_layout(idle_boundary_snapshot).left == 0 &&
             ava::tui::composer_canvas_layout(idle_boundary_snapshot).rail_visible &&
             std::ranges::any_of(idle_boundary_frame, [](std::string const& line) { return strip_sgr(line).find("Session") != std::string::npos; }),
         "tui idle automatic sidebar appears at 176x16");

  auto reasoning_feedback_snapshot = idle_boundary_snapshot;
  reasoning_feedback_snapshot.width = 160;
  reasoning_feedback_snapshot.reasoning_feedback = "reasoning low";
  auto const reasoning_feedback_frame = ava::tui::render_composer(reasoning_feedback_snapshot);
  expect(ava::tui::composer_canvas_layout(reasoning_feedback_snapshot).content_width == 120 &&
             ava::tui::composer_canvas_layout(reasoning_feedback_snapshot).left == 20 &&
             std::ranges::any_of(reasoning_feedback_frame, [](std::string const& line) { return strip_sgr(line).find("reasoning low") != std::string::npos; }),
         "tui renders subtle one-action reasoning feedback in the centered canvas when the automatic sidebar is hidden");

  auto permission_boundary_snapshot = boundary_snapshot;
  permission_boundary_snapshot.width = 176;
  permission_boundary_snapshot.permission_prompt.emplace();
  permission_boundary_snapshot.permission_prompt->tool_name = "bash";
  permission_boundary_snapshot.permission_prompt->operation = "run";
  permission_boundary_snapshot.permission_prompt->target = "tests";
  permission_boundary_snapshot.permission_prompt->reason = "boundary";
  auto question_boundary_snapshot = boundary_snapshot;
  question_boundary_snapshot.width = 176;
  question_boundary_snapshot.question_prompt.emplace();
  question_boundary_snapshot.question_prompt->header = "Boundary question";
  question_boundary_snapshot.question_prompt->question = "Choose";
  question_boundary_snapshot.question_prompt->options.push_back(ava::tui::QuestionPromptOptionView{.value = "a", .label = "A"});
  auto select_boundary_snapshot = boundary_snapshot;
  select_boundary_snapshot.width = 176;
  select_boundary_snapshot.select_list.emplace();
  select_boundary_snapshot.select_list->title = "Boundary select";
  select_boundary_snapshot.select_list->items.emplace_back();
  select_boundary_snapshot.select_list->items.back().value = "a";
  select_boundary_snapshot.select_list->items.back().label = "A";
  auto const permission_boundary_frame = ava::tui::render_composer(permission_boundary_snapshot);
  auto const question_boundary_frame = ava::tui::render_composer(question_boundary_snapshot);
  auto const select_boundary_frame = ava::tui::render_composer(select_boundary_snapshot);
  expect(
      ava::tui::composer_canvas_layout(permission_boundary_snapshot).content_width == 120 &&
          ava::tui::composer_canvas_layout(permission_boundary_snapshot).left == 28 &&
          ava::tui::composer_canvas_layout(question_boundary_snapshot).content_width == 120 &&
          ava::tui::composer_canvas_layout(question_boundary_snapshot).left == 28 &&
          ava::tui::composer_canvas_layout(select_boundary_snapshot).content_width == 120 &&
          ava::tui::composer_canvas_layout(select_boundary_snapshot).left == 28 &&
          std::ranges::any_of(permission_boundary_frame, [](std::string const& line) { return strip_sgr(line).find("! Permission required") == 30; }) &&
          std::ranges::any_of(question_boundary_frame, [](std::string const& line) { return strip_sgr(line).find("? Boundary question") == 30; }) &&
          std::ranges::none_of(permission_boundary_frame,
                               [](std::string const& line) { return strip_sgr(line).find("boundary-activity") != std::string::npos; }) &&
          std::ranges::none_of(question_boundary_frame,
                               [](std::string const& line) { return strip_sgr(line).find("boundary-activity") != std::string::npos; }) &&
          std::ranges::none_of(select_boundary_frame, [](std::string const& line) { return strip_sgr(line).find("boundary-activity") != std::string::npos; }),
      "tui permission, question, and select authority suppress the automatic sidebar and share centered canvas geometry");

  boundary_snapshot.width = 160;
  boundary_snapshot.height = 12;
  boundary_frame = ava::tui::render_composer(boundary_snapshot);
  expect(ava::tui::composer_canvas_layout(boundary_snapshot).content_width == 120 && ava::tui::composer_canvas_layout(boundary_snapshot).left == 20 &&
             !ava::tui::composer_canvas_layout(boundary_snapshot).rail_visible &&
             std::ranges::none_of(boundary_frame, [](std::string const& line) { return strip_sgr(line).find("boundary-activity") != std::string::npos; }),
         "tui automatic sidebar stays hidden and centers the canvas on a short 160x12 terminal");

  auto drawer_snapshot =
      ava::tui::ComposerSnapshot{
          .mode = "build",
          .provider = "openai",
          .model = "gpt-5.5",
          .session_id = "session_drawer",
          .input = "",
          .status = "ready",
          .transcript = {ava::tui::TranscriptItem{.label = "you", .text = "DRAWER MUST HIDE THIS TRANSCRIPT"}},
          .width = 80,
          .height = 24,
          .sidebar =
              ava::tui::SidebarSnapshot{
                  .activity = {ava::tui::SidebarActivityItem{.id = "call_drawer",
                                                             .label = "running-activity",
                                                             .detail = "running a deliberately detailed responsive sidebar validation task",
                                                             .status = ava::tui::ToolTimelineStatus::Running},
                               ava::tui::SidebarActivityItem{
                                   .id = "complete_drawer", .label = "completed-activity", .status = ava::tui::ToolTimelineStatus::Success},
                               ava::tui::SidebarActivityItem{
                                   .id = "cancel_drawer", .label = "canceled-activity", .status = ava::tui::ToolTimelineStatus::Canceled},
                               ava::tui::SidebarActivityItem{.id = "error_drawer", .label = "failed-activity", .status = ava::tui::ToolTimelineStatus::Error}},
                  .modified_files = {ava::tui::SidebarModifiedFile{.path = "src/ava/tui/a-very-long-modified-file-name-for-responsive-drawer.cpp",
                                                                   .added = 12,
                                                                   .removed = 3}},
                  .session_id = "session_drawer_with_a_deliberately_long_identifier_that_must_wrap_without_clipping",
                  .mode = "build",
                  .provider = "openai",
                  .model = "gpt-5.5",
                  .workspace = "/workspace/a/very/long/project/path/that/must/wrap/and/remain/reachable/in/the/session/overview",
                  .git_branch = "develop-responsive-sidebar-checkpoint",
                  .version = "1.0.0-responsive-sidebar",
                  .token_status = "180k (90.0%)",
                  .reasoning_status = "high",
                  .context_source_count = 7,
                  .session_path = "/tmp/ava/sessions/a/very/long/session/path/session_drawer.jsonl",
                  .session_entry_count = 42},
          .sidebar_drawer_visible = true};
  auto wide_drawer_snapshot = drawer_snapshot;
  wide_drawer_snapshot.width = 160;
  auto const wide_drawer_canvas = ava::tui::composer_canvas_layout(wide_drawer_snapshot);
  expect(wide_drawer_canvas.content_width == 160 && wide_drawer_canvas.left == 0 && !wide_drawer_canvas.rail_visible,
         "tui sidebar drawer retains the full terminal canvas instead of inheriting the ordinary width cap");
  auto drawer_frame = ava::tui::render_composer(drawer_snapshot);
  auto const drawer_max = ava::tui::sidebar_drawer_max_scroll_offset(drawer_snapshot);
  expect(drawer_frame.size() == 24 && ava::tui::composer_main_width(drawer_snapshot) == 80 && drawer_max > 0 &&
             std::ranges::any_of(drawer_frame, [](std::string const& line) { return strip_sgr(line).find("Session overview") != std::string::npos; }) &&
             std::ranges::any_of(drawer_frame, [](std::string const& line) { return strip_sgr(line).find("PgUp/PgDn") != std::string::npos; }) &&
             std::ranges::any_of(drawer_frame, [](std::string const& line) { return strip_sgr(line).find("very-long-modified") != std::string::npos; }) &&
             std::ranges::any_of(drawer_frame, [](std::string const& line) { return strip_sgr(line).find("[~] running-activity") != std::string::npos; }) &&
             std::ranges::any_of(drawer_frame, [](std::string const& line) { return strip_sgr(line).find("[+] completed-activity") != std::string::npos; }) &&
             std::ranges::any_of(drawer_frame, [](std::string const& line) { return strip_sgr(line).find("[-] canceled-activity") != std::string::npos; }) &&
             std::ranges::any_of(drawer_frame, [](std::string const& line) { return strip_sgr(line).find("[x] failed-activity") != std::string::npos; }) &&
             std::ranges::none_of(drawer_frame, [](std::string const& line) { return strip_sgr(line).find("DRAWER MUST HIDE") != std::string::npos; }) &&
             std::ranges::none_of(drawer_frame, [](std::string const& line) { return strip_sgr(line).find("live session") != std::string::npos; }) &&
             strip_sgr(drawer_frame[22]).starts_with("│  Type a message...") && strip_sgr(drawer_frame[23]).starts_with("│  GPT-5.5") &&
             std::ranges::all_of(drawer_frame, [](std::string const& line) { return visible_columns(line) <= 80; }),
         "tui narrow sidebar drawer replaces the transcript, wraps semantic data, stays bounded, and retains the full-width quiet composer");
  drawer_snapshot.sidebar_drawer_scroll_offset = drawer_max;
  auto const drawer_end_frame = ava::tui::render_composer(drawer_snapshot);
  expect(std::ranges::any_of(drawer_end_frame,
                             [](std::string const& line) { return strip_sgr(line).find("context pressure critical 90.0%") != std::string::npos; }) &&
             std::ranges::any_of(drawer_end_frame, [](std::string const& line) { return strip_sgr(line).find("context sources 7") != std::string::npos; }) &&
             std::ranges::any_of(drawer_end_frame,
                                 [](std::string const& line) { return strip_sgr(line).find("AVA 1.0.0-responsive-sidebar") != std::string::npos; }),
         "tui sidebar drawer maximum scroll reaches final context and version data");

  drawer_snapshot.width = 100;
  drawer_snapshot.height = 12;
  drawer_snapshot.sidebar_drawer_scroll_offset = ava::tui::sidebar_drawer_max_scroll_offset(drawer_snapshot);
  auto const short_drawer_frame = ava::tui::render_composer(drawer_snapshot);
  expect(short_drawer_frame.size() == 12 && ava::tui::composer_main_width(drawer_snapshot) == 100 &&
             strip_sgr(short_drawer_frame[10]).starts_with("│  Type a message...") && strip_sgr(short_drawer_frame[11]).starts_with("│  GPT-5.5") &&
             std::ranges::any_of(short_drawer_frame, [](std::string const& line) { return strip_sgr(line).find("context sources 7") != std::string::npos; }) &&
             std::ranges::all_of(short_drawer_frame, [](std::string const& line) { return visible_columns(line) <= 100; }),
         "tui short sidebar drawer remains scrollable and retains the bottom composer rows");

  auto drawer_conflict_hidden = [&](ava::tui::ComposerSnapshot conflict, std::string_view expected_view_text) {
    auto const frame = ava::tui::render_composer(conflict);
    return std::ranges::none_of(frame, [](std::string const& line) { return strip_sgr(line).find("Session overview") != std::string::npos; }) &&
           std::ranges::any_of(frame, [&](std::string const& line) { return strip_sgr(line).find(expected_view_text) != std::string::npos; });
  };
  auto permission_conflict = drawer_snapshot;
  permission_conflict.permission_prompt = ava::tui::PermissionPromptView{.tool_name = "bash", .operation = "run command", .target = "tests", .reason = "test"};
  auto docked_question_conflict = drawer_snapshot;
  docked_question_conflict.question_prompt = ava::tui::QuestionPromptView{};
  docked_question_conflict.question_prompt->header = "Question";
  docked_question_conflict.question_prompt->question = "Choose";
  docked_question_conflict.question_prompt->options = {{.value = "a", .label = "A"}};
  auto modal_question_conflict = docked_question_conflict;
  modal_question_conflict.question_prompt->modal = true;
  auto select_conflict = drawer_snapshot;
  select_conflict.select_list = ava::tui::SelectListView{};
  select_conflict.select_list->title = "Choose";
  select_conflict.select_list->items.emplace_back();
  select_conflict.select_list->items.back().value = "a";
  select_conflict.select_list->items.back().label = "A";
  expect(drawer_conflict_hidden(permission_conflict, "Permission required") && drawer_conflict_hidden(docked_question_conflict, "Choose") &&
             drawer_conflict_hidden(modal_question_conflict, "Choose") && drawer_conflict_hidden(select_conflict, "Choose"),
         "tui safety and choice views suppress the sidebar drawer");

  drawer_snapshot.width = 80;
  drawer_snapshot.height = 24;
  drawer_snapshot.sidebar_drawer_scroll_offset = 0;
  expect(!ava::tui::composer_input_cursor_for_screen_position(drawer_snapshot, 23, 4),
         "tui composer hit testing rejects clicks while the sidebar drawer owns focus");
  drawer_snapshot.sidebar_drawer_visible = false;
  expect(ava::tui::composer_input_cursor_for_screen_position(drawer_snapshot, 23, 4).has_value(),
         "tui composer hit testing resumes after the sidebar drawer closes");
  auto missing_drawer_data = drawer_snapshot;
  missing_drawer_data.sidebar_drawer_visible = true;
  missing_drawer_data.sidebar = std::nullopt;
  auto const missing_drawer_data_frame = ava::tui::render_composer(missing_drawer_data);
  expect(std::ranges::any_of(missing_drawer_data_frame,
                             [](std::string const& line) { return strip_sgr(line).find("DRAWER MUST HIDE THIS TRANSCRIPT") != std::string::npos; }) &&
             ava::tui::composer_input_cursor_for_screen_position(missing_drawer_data, 23, 4).has_value(),
         "tui sidebar drawer fails closed to the normal full-width composer when semantic sidebar data is absent");
  {
    ScopedEnvVar no_color("NO_COLOR", "1");
    auto plain_drawer_snapshot = drawer_snapshot;
    plain_drawer_snapshot.sidebar_drawer_visible = true;
    auto const plain_drawer = ava::tui::render_composer(plain_drawer_snapshot);
    expect(std::ranges::none_of(plain_drawer, [](std::string const& line) { return line.find('\x1b') != std::string::npos; }),
           "tui sidebar drawer plain mode emits no terminal escapes");
  }

  auto const tabbed = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                           .provider = "openai",
                                                                           .model = "gpt-5.5",
                                                                           .session_id = "session_test",
                                                                           .input = "",
                                                                           .status = "tab\tstatus",
                                                                           .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "tab\ttext"}},
                                                                           .width = 30,
                                                                           .height = 8});
  expect(std::ranges::none_of(tabbed, [](std::string const& line) { return line.find('\t') != std::string::npos; }) &&
             std::ranges::all_of(tabbed, [](std::string const& line) { return visible_columns(line) <= 30; }),
         "tui expands tabs before rendering width-bounded lines");

  auto const assistant_meta = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "answer", .meta = "Build · GPT-5.5 · 1.2s"}},
                                 .width = 48,
                                 .height = 10});
  expect(std::ranges::any_of(assistant_meta, [](std::string const& line) { return strip_sgr(line).find("* Build · GPT-5.5 · 1.2s") != std::string::npos; }) &&
             std::ranges::all_of(assistant_meta, [](std::string const& line) { return visible_columns(line) <= 48; }),
         "tui renders assistant mode/model/duration metadata under AVA messages with ASCII markers");
  auto assistant_meta_index = std::optional<std::size_t>{};
  auto composer_index = std::optional<std::size_t>{};
  for (std::size_t index = 0; index < assistant_meta.size(); ++index)
  {
    auto const visible = strip_sgr(assistant_meta[index]);
    if (!assistant_meta_index && visible.find("* Build · GPT-5.5 · 1.2s") != std::string::npos)
    {
      assistant_meta_index = index;
    }
    if (!composer_index && visible.find("│  Type a message...") != std::string::npos)
      composer_index = index;
  }
  expect(assistant_meta_index && composer_index && *composer_index > *assistant_meta_index + 1 && strip_sgr(assistant_meta[*assistant_meta_index + 1]).empty(),
         "tui leaves a vertical margin between the latest assistant metadata and the composer");

  std::string exact_width_utf8_status;
  for (int index = 0; index < 12; ++index)
  {
    exact_width_utf8_status += "\xC3\xA9";
  }
  auto const exact_width_utf8 = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                                     .provider = "openai",
                                                                                     .model = "gpt-5.5",
                                                                                     .session_id = "session_test",
                                                                                     .input = "",
                                                                                     .status = exact_width_utf8_status,
                                                                                     .transcript = {},
                                                                                     .width = 20,
                                                                                     .height = 8});
  expect(std::ranges::all_of(exact_width_utf8, [](std::string const& line) { return visible_columns(line) <= 20; }) &&
             std::ranges::any_of(exact_width_utf8, [](std::string const& line) { return strip_sgr(line).find("│  GPT-5.5") != std::string::npos; }),
         "tui width fitting preserves the AVA composer surface at minimum width");

  auto const utf8 = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_test",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = std::string(13, 'x') + "\xC3\xA9" + "zzz"}},
                                 .width = 20,
                                 .height = 8});
  expect(std::ranges::none_of(utf8, [](std::string const& line) { return !line.empty() && (static_cast<unsigned char>(line.back()) & 0xC0U) == 0xC0U; }),
         "tui truncation does not leave a trailing utf-8 starter byte");

  std::vector<ava::tui::TranscriptItem> stress_transcript;
  for (int index = 0; index < 36; ++index)
  {
    stress_transcript.push_back(ava::tui::TranscriptItem{
        .label = "you", .text = "resize stress user line " + std::to_string(index) + " with a very-long-token-that-must-not-overflow-or-resize-the-layout"});
    stress_transcript.push_back(ava::tui::TranscriptItem{.label = "ava",
                                                         .text = "assistant answer " + std::to_string(index) +
                                                                 " keeps CJK \xE7\x95\x8C and emoji \xF0\x9F\x98\x80 "
                                                                 "inside the measured viewport",
                                                         .meta = "Build · GPT-5.5",
                                                         .thinking = "checked resize path " + std::to_string(index)});
    if (index % 5 == 0)
    {
      stress_transcript.push_back(ava::tui::TranscriptItem{
          .tool = ava::tui::ToolTimelineItem{.status = index % 10 == 0 ? ava::tui::ToolTimelineStatus::Error : ava::tui::ToolTimelineStatus::Success,
                                             .name = "grep",
                                             .argument_summary = "pattern=needle path=src",
                                             .result_summary = "returned " + std::to_string(index) + " matches",
                                             .call_id = "call_resize_" + std::to_string(index),
                                             .lifecycle = index % 10 == 0 ? ava::tui::ToolLifecycleState::Error : ava::tui::ToolLifecycleState::Complete,
                                             .truncated = true,
                                             .visible_matches = 2,
                                             .total_matches = 12,
                                             .spill_path = "/tmp/ava-spill/resize.txt"}});
    }
    if (index % 7 == 0)
    {
      stress_transcript.push_back(ava::tui::TranscriptItem{.label = "audit", .text = "permission replied after resize boundary " + std::to_string(index)});
    }
  }

  ava::tui::SidebarSnapshot const stress_sidebar{
      .activity =
          {ava::tui::SidebarActivityItem{
               .id = "running", .label = "compaction", .detail = "compaction started tokens~9000/8000", .status = ava::tui::ToolTimelineStatus::Running},
           ava::tui::SidebarActivityItem{.id = "done", .label = "read_file", .detail = "assistant responded", .status = ava::tui::ToolTimelineStatus::Success}},
      .modified_files = {ava::tui::SidebarModifiedFile{.path = "src/ava/tui/composer.cpp", .added = 3, .removed = 1}},
      .session_id = "session_resize_stress",
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .workspace = "/workspace",
      .git_branch = "develop",
      .version = "test",
      .token_status = "tokens unknown",
      .context_source_count = 2};
  std::vector<std::size_t> const stress_widths = {1, 20, 28, 40, 72, 111, 112, 160};
  std::vector<std::size_t> const stress_heights = {1, 8, 10, 18, 32};
  for (auto const width : stress_widths)
  {
    for (auto const height : stress_heights)
    {
      auto frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{.mode = "build",
                                                                        .provider = "openai",
                                                                        .model = "gpt-5.5",
                                                                        .session_id = "session_resize_stress",
                                                                        .input = "draft line one\nsecond draft line with \xE7\x95\x8C",
                                                                        .status = "ready",
                                                                        .processing = true,
                                                                        .token_status = "tokens unknown",
                                                                        .reasoning_status = "thinking visible",
                                                                        .transcript = stress_transcript,
                                                                        .transcript_scroll_offset = 50,
                                                                        .width = width,
                                                                        .height = height,
                                                                        .input_cursor = std::string::npos,
                                                                        .sidebar = stress_sidebar,
                                                                        .tool_presentation = ava::tui::ToolPresentation::Expanded,
                                                                        .thinking_visible = true});
      auto const effective_width = std::max<std::size_t>(ava::tui::detail::kMinWidth, width);
      auto const effective_height = std::max<std::size_t>(ava::tui::detail::kMinHeight, height);
      expect(frame.size() == effective_height &&
                 std::ranges::all_of(frame,
                                     [&](std::string const& line) { return line.find('\n') == std::string::npos && visible_columns(line) <= effective_width; }),
             "tui resize stress render keeps long mixed transcripts bounded at every tested viewport");
    }
  }
  auto cycle_feedback_snapshot = ava::tui::ComposerSnapshot{.mode = "build",
                                                            .provider = "openai",
                                                            .model = "gpt-5.5",
                                                            .session_id = "session_test",
                                                            .input = "",
                                                            .status = "",
                                                            .reasoning_feedback = "reasoning changed to high",
                                                            .transcript = {ava::tui::TranscriptItem{.label = "ava", .text = std::string(1200, 'x')}},
                                                            .width = 120,
                                                            .height = 18,
                                                            .sidebar = ava::tui::SidebarSnapshot{.session_id = "session_test"}};
  auto const hidden_rail_feedback_frame = ava::tui::render_composer(cycle_feedback_snapshot);
  auto rail_feedback_snapshot = cycle_feedback_snapshot;
  rail_feedback_snapshot.width = 176;
  auto const idle_rail_feedback_frame = ava::tui::render_composer(rail_feedback_snapshot);
  auto no_feedback_rail_snapshot = rail_feedback_snapshot;
  no_feedback_rail_snapshot.reasoning_feedback.reset();
  auto const rail_feedback_scroll =
      ava::tui::composer_max_transcript_scroll_offset(rail_feedback_snapshot, rail_feedback_snapshot.width, rail_feedback_snapshot.height);
  auto const no_feedback_rail_scroll =
      ava::tui::composer_max_transcript_scroll_offset(no_feedback_rail_snapshot, no_feedback_rail_snapshot.width, no_feedback_rail_snapshot.height);
  auto ordinary_reasoning_status_snapshot = cycle_feedback_snapshot;
  ordinary_reasoning_status_snapshot.reasoning_feedback.reset();
  ordinary_reasoning_status_snapshot.status = "reasoning ordinary status";
  auto const ordinary_reasoning_status_frame = ava::tui::render_composer(ordinary_reasoning_status_snapshot);
  auto runtime_reasoning_snapshot = cycle_feedback_snapshot;
  runtime_reasoning_snapshot.status = "stale status";
  ava::tui::apply_reasoning_cycle_success(runtime_reasoning_snapshot, "reasoning changed to high");
  auto const runtime_success_is_presentation_only =
      runtime_reasoning_snapshot.status.empty() && runtime_reasoning_snapshot.reasoning_feedback == "reasoning changed to high";
  ava::tui::clear_reasoning_feedback_for_user_input(runtime_reasoning_snapshot);
  expect(std::ranges::any_of(hidden_rail_feedback_frame,
                             [](std::string const& line) { return strip_sgr(line).find("reasoning changed to high") != std::string::npos; }) &&
             std::ranges::none_of(idle_rail_feedback_frame,
                                  [](std::string const& line) { return strip_sgr(line).find("reasoning changed to high") != std::string::npos; }) &&
             rail_feedback_scroll == no_feedback_rail_scroll &&
             std::ranges::none_of(ordinary_reasoning_status_frame,
                                  [](std::string const& line) { return strip_sgr(line).find("reasoning ordinary status") != std::string::npos; }) &&
             runtime_success_is_presentation_only && !runtime_reasoning_snapshot.reasoning_feedback,
         "tui reasoning-cycle feedback is one-action presentation state: visible without a rail, suppressed without rail geometry drift, and cleared by user "
         "input");

  auto wave_a_base = ava::tui::ComposerSnapshot{.mode = "build",
                                                .provider = "openai",
                                                .model = "gpt-5.5",
                                                .session_id = "session_test",
                                                .input = "",
                                                .status = "ready",
                                                .active_run_hint = ava::tui::ActiveRunHint{.interrupt = "Esc", .jump_to_bottom = "Ctrl+End"},
                                                .active_context_status = "870 (3.2%)",
                                                .transcript = {},
                                                .width = 80,
                                                .height = 24};
  auto find_visible = [](std::vector<std::string> const& lines, std::string_view needle) {
    return std::ranges::any_of(lines, [&](std::string const& line) { return strip_sgr(line).find(needle) != std::string::npos; });
  };
  auto count_visible = [](std::vector<std::string> const& lines, std::string_view needle) {
    return std::ranges::count_if(lines, [&](std::string const& line) { return strip_sgr(line).find(needle) != std::string::npos; });
  };
  auto const idle_discovery_80x24 = ava::tui::render_composer(wave_a_base);
  auto typed_discovery = wave_a_base;
  typed_discovery.input = "hello";
  auto const typed_discovery_lines = ava::tui::render_composer(typed_discovery);
  auto transcript_discovery = wave_a_base;
  transcript_discovery.transcript = {ava::tui::TranscriptItem{.label = "you", .text = "hi"}};
  auto const transcript_discovery_lines = ava::tui::render_composer(transcript_discovery);
  auto processing_discovery = wave_a_base;
  processing_discovery.processing = true;
  auto const processing_discovery_lines = ava::tui::render_composer(processing_discovery);
  auto prompt_discovery = wave_a_base;
  prompt_discovery.permission_prompt =
      ava::tui::PermissionPromptView{.tool_name = "bash", .operation = "execute", .target = "tests", .command = "echo hi", .reason = "test"};
  auto const prompt_discovery_lines = ava::tui::render_composer(prompt_discovery);
  auto question_discovery = wave_a_base;
  question_discovery.question_prompt = ava::tui::QuestionPromptView{};
  question_discovery.question_prompt->header = "Q";
  question_discovery.question_prompt->question = "Choose?";
  auto const question_discovery_lines = ava::tui::render_composer(question_discovery);
  auto select_discovery = wave_a_base;
  select_discovery.select_list = ava::tui::SelectListView{};
  select_discovery.select_list->title = "Models";
  auto const select_discovery_lines = ava::tui::render_composer(select_discovery);
  auto attachment_discovery = wave_a_base;
  attachment_discovery.pending_attachments = {ava::tui::PendingAttachmentItem{.label = "shot.png"}};
  auto const attachment_discovery_lines = ava::tui::render_composer(attachment_discovery);
  auto narrow_discovery = wave_a_base;
  narrow_discovery.width = 40;
  narrow_discovery.height = 10;
  auto const discovery_40x10 = ava::tui::render_composer(narrow_discovery);
  {
    ScopedEnvVar no_color_guard("NO_COLOR", "1");
    auto const no_color_discovery = ava::tui::render_composer(wave_a_base);
    expect(count_visible(idle_discovery_80x24, "/ commands · @ files") == 1 && count_visible(idle_discovery_80x24, "/help · /hotkeys") == 1 &&
               !find_visible(discovery_40x10, "/ commands") && !find_visible(discovery_40x10, "/help · /hotkeys") &&
               !find_visible(typed_discovery_lines, "/ commands") && !find_visible(typed_discovery_lines, "/help · /hotkeys") &&
               !find_visible(transcript_discovery_lines, "/ commands") && !find_visible(processing_discovery_lines, "/ commands") &&
               !find_visible(prompt_discovery_lines, "/ commands") && !find_visible(question_discovery_lines, "/ commands") &&
               !find_visible(select_discovery_lines, "/ commands") && !find_visible(attachment_discovery_lines, "/ commands") &&
               strip_sgr(idle_discovery_80x24.back()).find("GPT-5.5 · ctx 870 (3.2%)") != std::string::npos &&
               strip_sgr(idle_discovery_80x24.back()).find("build") == std::string::npos &&
               strip_sgr(idle_discovery_80x24.back()).find("session") == std::string::npos &&
               std::ranges::none_of(idle_discovery_80x24, [](std::string const& line) { return strip_sgr(line).find("AVA") != std::string::npos; }) &&
               find_visible(no_color_discovery, "/ commands · @ files") && find_visible(no_color_discovery, "/help · /hotkeys") &&
               std::ranges::all_of(no_color_discovery, [](std::string const& line) { return line.find('\x1b') == std::string::npos; }) &&
               std::ranges::none_of(no_color_discovery, [](std::string const& line) { return line.find("\x1b[48;2;26;31;46m") != std::string::npos; }),
           "Wave A idle empty-transcript discovery shows exactly two muted lines at 80x24, none at 40x10, reclaims on typing/transcript/processing/prompt/"
           "select/attachment, keeps the minimal footer contract, and preserves NO_COLOR default surfaces");
  }

  auto retry_snapshot = wave_a_base;
  retry_snapshot.processing = true;
  retry_snapshot.status = "thinking...";
  retry_snapshot.sidebar = ava::tui::SidebarSnapshot{
      .activity = {ava::tui::SidebarActivityItem{.id = "responding",
                                                 .label = "responding",
                                                 .detail = "assistant is writing - PROVIDER_BODY_SECRET",
                                                 .status = ava::tui::ToolTimelineStatus::Running},
                   ava::tui::SidebarActivityItem{.id = "retry:rate",
                                                 .label = "retry",
                                                 .detail = "retrying after rate_limit attempt 2/5 delay=1000ms - PROVIDER_BODY_SECRET leaked text",
                                                 .status = ava::tui::ToolTimelineStatus::Running}}};
  auto const retry_lines = ava::tui::render_composer(retry_snapshot);
  auto countdown_snapshot = retry_snapshot;
  countdown_snapshot.sidebar->activity.back().detail =
      "retry countdown after rate_limit attempt 2/5 delay=1000ms remaining=1500ms - PROVIDER_BODY_SECRET leaked text";
  auto const countdown_lines = ava::tui::render_composer(countdown_snapshot);
  auto compaction_only = retry_snapshot;
  compaction_only.sidebar->activity = {ava::tui::SidebarActivityItem{.id = "compaction",
                                                                     .label = "compaction",
                                                                     .detail = "compaction started (auto) - PROVIDER_BODY_SECRET",
                                                                     .status = ava::tui::ToolTimelineStatus::Running}};
  auto const compaction_lines = ava::tui::render_composer(compaction_only);
  auto completed_lifecycle = retry_snapshot;
  completed_lifecycle.sidebar->activity = {
      ava::tui::SidebarActivityItem{
          .id = "retry:done", .label = "retry", .detail = "retrying after rate_limit attempt 2/5", .status = ava::tui::ToolTimelineStatus::Success},
      ava::tui::SidebarActivityItem{
          .id = "compaction", .label = "compaction", .detail = "compaction completed", .status = ava::tui::ToolTimelineStatus::Success}};
  auto const completed_lifecycle_lines = ava::tui::render_composer(completed_lifecycle);
  auto custom_interrupt = retry_snapshot;
  custom_interrupt.sidebar.reset();
  custom_interrupt.active_run_hint.interrupt = "F9";
  auto const custom_interrupt_lines = ava::tui::render_composer(custom_interrupt);
  auto unbound_interrupt = custom_interrupt;
  unbound_interrupt.active_run_hint.interrupt.clear();
  auto const unbound_interrupt_lines = ava::tui::render_composer(unbound_interrupt);
  expect(find_visible(retry_lines, "Esc stop · retry attempt 2/5") && !find_visible(retry_lines, "PROVIDER_BODY_SECRET") &&
             !find_visible(retry_lines, "assistant is writing") && !find_visible(retry_lines, "type a follow-up") &&
             find_visible(countdown_lines, "Esc stop · retry 1500ms") && !find_visible(countdown_lines, "PROVIDER_BODY_SECRET") &&
             find_visible(compaction_lines, "Esc stop · compaction") && !find_visible(compaction_lines, "PROVIDER_BODY_SECRET") &&
             find_visible(completed_lifecycle_lines, "Esc stop · type a follow-up") && !find_visible(completed_lifecycle_lines, "retry attempt") &&
             !find_visible(completed_lifecycle_lines, "compaction") && find_visible(custom_interrupt_lines, "F9 stop · type a follow-up") &&
             find_visible(unbound_interrupt_lines, "stop unbound") && !find_visible(unbound_interrupt_lines, "Esc stop") &&
             strip_sgr(retry_lines.back()).find("GPT-5.5") != std::string::npos && strip_sgr(retry_lines.back()).find("ctx 870 (3.2%)") != std::string::npos &&
             !find_visible(retry_lines, "1.3k (0.7%)"),
         "Wave A active-run contextual row prioritizes newest allowlisted RUNNING retry/compaction status, keeps configured Cancel stop (or stop unbound), "
         "ignores completed/generic activity, and never leaks provider body text into chrome or footer");

  auto detached_active = wave_a_base;
  detached_active.processing = true;
  detached_active.transcript = {ava::tui::TranscriptItem{.label = "ava", .text = "streamed"}};
  detached_active.transcript_scroll_offset = 3;
  detached_active.transcript_new_output_count = 4;
  auto const detached_active_lines = ava::tui::render_composer(detached_active);
  auto detached_idle = detached_active;
  detached_idle.processing = false;
  auto const detached_idle_lines = ava::tui::render_composer(detached_idle);
  auto following = detached_idle;
  following.transcript_scroll_offset = 0;
  following.transcript_new_output_count = 0;
  auto const following_lines = ava::tui::render_composer(following);
  auto zero_count = detached_idle;
  zero_count.transcript_new_output_count = 0;
  auto const zero_count_lines = ava::tui::render_composer(zero_count);
  auto unbound_jump = detached_idle;
  unbound_jump.active_run_hint.jump_to_bottom.clear();
  auto const unbound_jump_lines = ava::tui::render_composer(unbound_jump);
  auto custom_jump = detached_idle;
  custom_jump.active_run_hint.jump_to_bottom = "End";
  auto const custom_jump_lines = ava::tui::render_composer(custom_jump);
  expect(find_visible(detached_active_lines, "Esc stop") && find_visible(detached_active_lines, "4 new · Ctrl+End") &&
             find_visible(detached_idle_lines, "4 new · Ctrl+End") && !find_visible(detached_idle_lines, "Esc stop") &&
             !find_visible(following_lines, " new") && !find_visible(zero_count_lines, " new") && find_visible(unbound_jump_lines, "4 new · jump unbound") &&
             find_visible(custom_jump_lines, "4 new · End") &&
             std::ranges::none_of(detached_idle_lines,
                                  [](std::string const& line) {
                                    auto const visible = strip_sgr(line);
                                    return visible.find("detached") != std::string::npos || visible.find("banner") != std::string::npos;
                                  }),
         "Wave A detached new-output hint appends compact N new plus configured JumpToBottom key (or jump unbound), appears only while detached with count>0, "
         "and never becomes a permanent banner");

  auto combined = detached_active;
  combined.sidebar = ava::tui::SidebarSnapshot{
      .activity = {ava::tui::SidebarActivityItem{
          .id = "retry:rate", .label = "retry", .detail = "retry countdown remaining=900ms", .status = ava::tui::ToolTimelineStatus::Running}}};
  combined.width = 48;
  auto const combined_lines = ava::tui::render_composer(combined);
  auto const combined_hint = std::ranges::find_if(combined_lines, [](std::string const& line) {
    auto const visible = strip_sgr(line);
    return visible.find("stop") != std::string::npos || visible.find("new") != std::string::npos;
  });
  expect(combined_hint != combined_lines.end() && find_visible(combined_lines, "Esc stop") && find_visible(combined_lines, "retry 900ms") &&
             find_visible(combined_lines, "4 new") && find_visible(combined_lines, "Ctrl+End") &&
             std::ranges::count_if(combined_lines,
                                   [](std::string const& line) {
                                     auto const visible = strip_sgr(line);
                                     return visible.find("stop") != std::string::npos || visible.find(" new") != std::string::npos;
                                   }) == 1 &&
             visible_columns(*combined_hint) <= 48,
         "Wave A combined lifecycle and detached hints stay on one width-safe contextual row while preserving stop and jump");

  auto bindings = ava::tui::default_key_bindings();
  auto const default_hint = ava::tui::runtime_views::active_run_hint_for(bindings);
  for (auto& binding : bindings.bindings)
  {
    if (binding.first == ava::tui::TuiAction::Cancel)
      binding.second = {ava::tui::Key::F9};
    if (binding.first == ava::tui::TuiAction::JumpToBottom)
      binding.second = {ava::tui::Key::End};
  }
  auto const custom_hint = ava::tui::runtime_views::active_run_hint_for(bindings);
  for (auto& binding : bindings.bindings)
  {
    if (binding.first == ava::tui::TuiAction::Cancel || binding.first == ava::tui::TuiAction::JumpToBottom)
      binding.second = {};
  }
  auto const unbound_hint = ava::tui::runtime_views::active_run_hint_for(bindings);
  expect(default_hint.interrupt == "Esc" && default_hint.jump_to_bottom == "Ctrl+End" && custom_hint.interrupt == "F9" && custom_hint.jump_to_bottom == "End" &&
             unbound_hint.interrupt.empty() && unbound_hint.jump_to_bottom.empty(),
         "Wave A active_run_hint_for derives Cancel stop and JumpToBottom labels through first_key_display so custom and unbound bindings stay truthful");

  auto const active_todos = std::vector<ava::tui::TodoItem>{
      {.id = "a", .content = "First task", .status = ava::tui::TodoStatus::Completed},
      {.id = "b", .content = "Second task", .status = ava::tui::TodoStatus::InProgress},
      {.id = "c", .content = "Third task", .status = ava::tui::TodoStatus::Pending},
  };
  auto const rail_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_todo_rail",
      .input = "",
      .status = "ready",
      .transcript = {ava::tui::TranscriptItem{.label = "you", .text = "work"}},
      .width = 144,
      .height = 22,
      .sidebar =
          ava::tui::SidebarSnapshot{.todos = active_todos, .session_id = "session_todo_rail", .mode = "build", .provider = "openai", .model = "gpt-5.5"}});
  expect(std::ranges::any_of(rail_frame,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("Todos — 2 active") != std::string::npos;
                             }) &&
             std::ranges::any_of(rail_frame, [](std::string const& line) { return strip_sgr(line).find("#b") != std::string::npos; }) &&
             std::ranges::none_of(rail_frame, [](std::string const& line) { return strip_sgr(line).find("#a") != std::string::npos; }),
         "144-col automatic rail shows active todos only and uses the actionable width threshold");

  auto const narrow_frame = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_todo_dock",
      .input = "",
      .status = "ready",
      .transcript = {ava::tui::TranscriptItem{.label = "you", .text = "work"}},
      .width = 143,
      .height = 24,
      .sidebar =
          ava::tui::SidebarSnapshot{.todos = active_todos, .session_id = "session_todo_dock", .mode = "build", .provider = "openai", .model = "gpt-5.5"}});
  expect(std::ranges::any_of(narrow_frame,
                             [](std::string const& line) {
                               auto const visible = strip_sgr(line);
                               return visible.find("Todos — 1/3 completed") != std::string::npos && visible.find("in progress") != std::string::npos;
                             }) &&
             std::ranges::none_of(narrow_frame, [](std::string const& line) { return strip_sgr(line).find("Todos — 2 active") != std::string::npos; }),
         "143-col terminals use the sticky narrow todo dock instead of the automatic rail title");

  auto const completed_only = ava::tui::render_composer(
      ava::tui::ComposerSnapshot{.mode = "build",
                                 .provider = "openai",
                                 .model = "gpt-5.5",
                                 .session_id = "session_todo_done",
                                 .input = "",
                                 .status = "ready",
                                 .transcript = {},
                                 .width = 144,
                                 .height = 22,
                                 .sidebar = ava::tui::SidebarSnapshot{.todos = {{.id = "a", .content = "Done", .status = ava::tui::TodoStatus::Completed}},
                                                                      .session_id = "session_todo_done",
                                                                      .mode = "build",
                                                                      .provider = "openai",
                                                                      .model = "gpt-5.5"}});
  expect(std::ranges::none_of(completed_only, [](std::string const& line) { return strip_sgr(line).find("Todos") != std::string::npos; }),
         "completed-only todo lists hide the live automatic rail contribution");

  auto const drawer = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_todo_drawer",
      .input = "",
      .status = "ready",
      .transcript = {},
      .width = 100,
      .height = 24,
      .sidebar =
          ava::tui::SidebarSnapshot{.todos = active_todos, .session_id = "session_todo_drawer", .mode = "build", .provider = "openai", .model = "gpt-5.5"},
      .sidebar_drawer_visible = true});
  expect(std::ranges::any_of(drawer, [](std::string const& line) { return strip_sgr(line).find("Todos") != std::string::npos; }) &&
             std::ranges::any_of(drawer, [](std::string const& line) { return strip_sgr(line).find("[completed]") != std::string::npos; }) &&
             std::ranges::any_of(drawer, [](std::string const& line) { return strip_sgr(line).find("#a") != std::string::npos; }),
         "/sidebar drawer includes a full Todos section with statuses");

  auto const short_dock = ava::tui::render_composer(ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_todo_short",
      .input = "hello",
      .status = "ready",
      .transcript = {},
      .width = 80,
      .height = 10,
      .sidebar =
          ava::tui::SidebarSnapshot{.todos = active_todos, .session_id = "session_todo_short", .mode = "build", .provider = "openai", .model = "gpt-5.5"}});
  auto const todo_lines = std::ranges::count_if(short_dock, [](std::string const& line) {
    auto const visible = strip_sgr(line);
    return visible.find("Todos") != std::string::npos || visible.find("#b") != std::string::npos || visible.find("#c") != std::string::npos;
  });
  expect(todo_lines <= 3, "short-height terminals bound the sticky todo dock and do not starve the composer");

  // Wide automatic rail must not reserve narrow-dock geometry after reducing to main-column width.
  std::vector<ava::tui::TranscriptItem> geometry_transcript;
  geometry_transcript.reserve(48);
  for (std::size_t index = 0; index < 48; ++index)
  {
    if (index == 2)
    {
      geometry_transcript.push_back(ava::tui::TranscriptItem{
          .label = "tool",
          .text = "tool body",
          .tool = ava::tui::ToolTimelineItem{.status = ava::tui::ToolTimelineStatus::Success, .name = "bash", .argument_summary = "echo geometry"}});
      continue;
    }
    geometry_transcript.push_back(ava::tui::TranscriptItem{.label = "you", .text = "geometry line " + std::to_string(index)});
  }
  auto wide_todo_geometry = ava::tui::ComposerSnapshot{
      .mode = "build",
      .provider = "openai",
      .model = "gpt-5.5",
      .session_id = "session_todo_geometry_wide",
      .input = "",
      .status = "ready",
      .transcript = geometry_transcript,
      .width = 144,
      .height = 24,
      .sidebar = ava::tui::SidebarSnapshot{
          .todos = active_todos, .session_id = "session_todo_geometry_wide", .mode = "build", .provider = "openai", .model = "gpt-5.5"}};
  auto const wide_canvas = ava::tui::composer_canvas_layout(wide_todo_geometry);
  expect(wide_canvas.rail_visible && wide_canvas.content_width < 144, "144xH active-todo layout uses the automatic rail and a reduced main content width");
  auto wide_main_only = wide_todo_geometry;
  wide_main_only.width = wide_canvas.content_width;
  wide_main_only.sidebar = std::nullopt;
  wide_main_only.reasoning_feedback.reset();
  auto const wide_frame = ava::tui::render_composer(wide_todo_geometry);
  auto const wide_scroll = ava::tui::composer_max_transcript_scroll_offset(wide_todo_geometry, 144, 24);
  auto const wide_main_scroll = ava::tui::composer_max_transcript_scroll_offset(wide_main_only, wide_canvas.content_width, 24);
  auto const wide_body = ava::tui::detail::transcript_body_screen_geometry(wide_todo_geometry);
  auto const wide_main_body = ava::tui::detail::transcript_body_screen_geometry(wide_main_only);
  auto const wide_tool_hit = ava::tui::detail::transcript_tool_card_header_for_screen_position(wide_todo_geometry, 1, wide_canvas.left + 4);
  auto const wide_main_tool_hit = ava::tui::detail::transcript_tool_card_header_for_screen_position(wide_main_only, 1, 4);
  expect(std::ranges::none_of(wide_frame,
                              [](std::string const& line) {
                                auto const visible = strip_sgr(line);
                                return visible.find("Todos — 1/3 completed") != std::string::npos;
                              }) &&
             wide_scroll == wide_main_scroll && wide_body.valid && wide_main_body.valid && wide_body.transcript_height == wide_main_body.transcript_height &&
             wide_body.content_width == wide_main_body.content_width && wide_body.content_width == wide_canvas.content_width &&
             wide_tool_hit == wide_main_tool_hit,
         "144xH active-todo rail reserves zero narrow dock and keeps scroll/hit geometry on the rendered main frame");

  auto narrow_todo_geometry = wide_todo_geometry;
  narrow_todo_geometry.width = 143;
  narrow_todo_geometry.session_id = "session_todo_geometry_narrow";
  narrow_todo_geometry.sidebar->session_id = "session_todo_geometry_narrow";
  auto narrow_without_todos = narrow_todo_geometry;
  narrow_without_todos.sidebar->todos.clear();
  auto const narrow_canvas = ava::tui::composer_canvas_layout(narrow_todo_geometry);
  auto const narrow_frame_geometry = ava::tui::render_composer(narrow_todo_geometry);
  auto const narrow_scroll = ava::tui::composer_max_transcript_scroll_offset(narrow_todo_geometry, 143, 24);
  auto const narrow_without_scroll = ava::tui::composer_max_transcript_scroll_offset(narrow_without_todos, 143, 24);
  auto const narrow_body = ava::tui::detail::transcript_body_screen_geometry(narrow_todo_geometry);
  auto const narrow_without_body = ava::tui::detail::transcript_body_screen_geometry(narrow_without_todos);
  expect(!narrow_canvas.rail_visible &&
             std::ranges::any_of(narrow_frame_geometry,
                                 [](std::string const& line) {
                                   auto const visible = strip_sgr(line);
                                   return visible.find("Todos — 1/3 completed") != std::string::npos;
                                 }) &&
             narrow_body.valid && narrow_without_body.valid && narrow_body.transcript_height < narrow_without_body.transcript_height &&
             narrow_scroll > narrow_without_scroll,
         "143xH active-todo layout still paints the narrow dock and reserves matching scroll/hit geometry");

  // Startup overview is on-demand only: startup reserves zero overview chrome rows at
  // every height and the quiet footer stays the home for mode/model/context essentials.
  {
    ava::tui::StartupOverviewSnapshot overview{
        .mode = "build",
        .provider = "openai",
        .model = "gpt-test",
        .trust_decision = "trusted",
        .overview_toggle_keys = {},
        .compact_line = "build · openai/gpt-test · trust trusted · /overview",
        .detail_line = "detail row",
    };
    ava::tui::ComposerSnapshot snap;
    snap.width = 80;
    snap.height = 24;
    snap.startup_overview = overview;
    snap.input = "";
    auto frame24 = ava::tui::render_composer(snap);
    expect(
        frame24.size() == 24 && std::ranges::none_of(frame24, [](std::string const& line) { return strip_sgr(line).find("/overview") != std::string::npos; }),
        "roomy startup frame reserves zero overview chrome rows and keeps total height");
    auto const body24 = ava::tui::detail::transcript_body_screen_geometry(snap);
    expect(body24.valid && body24.transcript_height > 0, "transcript body geometry is unaffected by the absent overview chrome");
    snap.height = 10;
    auto frame10 = ava::tui::render_composer(snap);
    expect(
        frame10.size() == 10 && std::ranges::none_of(frame10, [](std::string const& line) { return strip_sgr(line).find("/overview") != std::string::npos; }),
        "short startup frame reserves zero overview chrome rows");
    snap.height = 7;
    auto frame7 = ava::tui::render_composer(snap);
    // Frame paint still clamps to kMinHeight below 8 rows.
    expect(frame7.size() == 8 && std::ranges::none_of(frame7, [](std::string const& line) { return strip_sgr(line).find("/overview") != std::string::npos; }),
           "sub-8 startup frame reserves zero overview chrome rows even when paint clamps to min height");
    snap.height = 24;
    // Exact /overview still opens the bounded read-only select-list on demand.
    snap.select_list = ava::tui::overview_select_list_view(overview);
    auto expanded = ava::tui::render_composer(snap);
    expect(std::ranges::any_of(expanded, [](std::string const& line) { return strip_sgr(line).find("Startup overview") != std::string::npos; }),
           "/overview opens the read-only select-list explicitly");
    snap.select_list.reset();

    // Shared snapshot sync: open overview rebuilds from a refreshed DTO and preserves query/selection.
    {
      ava::tui::TuiRuntimeOptions options;
      options.session_id = "session_overview_sync";
      options.mode = "build";
      options.provider = "fake";
      options.model = "m1";
      ava::tui::RuntimePresentationState presentation(options);
      presentation.snapshot.startup_overview = overview;
      presentation.snapshot.select_list = ava::tui::overview_select_list_view(overview);
      presentation.snapshot.select_list->query = "mode";
      presentation.snapshot.select_list->selected_item_index = 0;
      auto active = ava::tui::ActiveSelectList::Overview;

      ava::tui::StartupOverviewSnapshot refreshed = overview;
      refreshed.model = "m2-refreshed";
      refreshed.compact_line = "build · fake/m2-refreshed · /overview";
      ava::tui::apply_runtime_state_snapshot_with_overview_sync(options, presentation, active,
                                                                ava::tui::TuiRuntimeStateSnapshot{.mode = "build",
                                                                                                  .provider = "fake",
                                                                                                  .model = "m2-refreshed",
                                                                                                  .session_id = "session_overview_sync",
                                                                                                  .session_path = "/sessions/overview-sync.jsonl",
                                                                                                  .workspace = "/workspace",
                                                                                                  .git_branch = "main",
                                                                                                  .status = "overview refreshed",
                                                                                                  .startup_overview = refreshed});
      expect(active == ava::tui::ActiveSelectList::Overview && presentation.snapshot.select_list && presentation.snapshot.select_list->query == "mode" &&
                 presentation.snapshot.startup_overview && presentation.snapshot.startup_overview->model == "m2-refreshed" &&
                 std::ranges::any_of(presentation.snapshot.select_list->items,
                                     [](auto const& item) { return item.label == "Model" && item.detail == "m2-refreshed"; }),
             "shared overview snapshot sync rebuilds an open list from the new DTO while preserving local query identity");

      // Competing authority / explicit close.
      ava::tui::close_startup_overview_presentation(presentation.snapshot, active);
      expect(active == ava::tui::ActiveSelectList::None && !presentation.snapshot.select_list,
             "prompt/competing authority closes ActiveSelectList::Overview and clears the select-list");

      // Re-open then session transition must close overview.
      presentation.snapshot.startup_overview = refreshed;
      presentation.snapshot.select_list = ava::tui::overview_select_list_view(refreshed);
      active = ava::tui::ActiveSelectList::Overview;
      ava::tui::RuntimeDraftState draft_state;
      ava::tui::RuntimeRenderer renderer(presentation.snapshot, presentation.sidebar, draft_state);
      ava::tui::RuntimeNavigationController navigation(options, presentation.snapshot, presentation.sidebar, draft_state, renderer);
      ava::tui::TranscriptSearchController transcript_search(presentation, renderer, navigation, active);
      ava::tui::RuntimeSubagentWorkspaceController subagent_workspace(options, presentation.snapshot);
      auto const session_changed = ava::tui::apply_runtime_state_snapshot_with_presentation_transition(
          options, presentation, draft_state, renderer, transcript_search, subagent_workspace, active,
          ava::tui::TuiRuntimeStateSnapshot{.mode = "plan",
                                            .provider = "fake",
                                            .model = "other",
                                            .session_id = "session_overview_other",
                                            .session_path = "/sessions/overview-other.jsonl",
                                            .workspace = "/workspace",
                                            .git_branch = "main",
                                            .status = "session switched",
                                            .startup_overview = refreshed});
      expect(session_changed && active == ava::tui::ActiveSelectList::None && !presentation.snapshot.select_list,
             "session transition closes an open startup overview through the shared presentation path");
    }
  }
}
