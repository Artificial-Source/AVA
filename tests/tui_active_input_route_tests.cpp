// Protect active-turn binding precedence through the real dispatcher and production owners.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/test_timeout.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/ActiveRun.h"
#include "ava/tui/ActiveRunInput.h"
#include "ava/tui/CommandOutput.h"
#include "ava/tui/Composer.h"
#include "ava/tui/PendingAttachments.h"
#include "ava/tui/SelectorHost.h"
#include "ava/tui/SessionSelector.h"
#include "ava/tui/SessionTransitionCoordinator.h"
#include "ava/tui/TuiEventLoop.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_mermaid_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_plugin_ui_internal.h"
#include "ava/tui/runtime_prompts_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_transcript_search_internal.h"
#include "ava/tui/session_grants.h"
#include "ava/tui/terminal/Context.h"
#include "ava/core/Application.h"
#include "ava/core/UniqueFd.h"
#include "utils/at_scope_end.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <initializer_list>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using ava::tui::TuiAction;
using ava::tui::TuiInputOutcome;
using ava::tui::terminal::Key;

// Own both readiness descriptors until every owner and the real event loop have retired.
std::pair<ava::core::UniqueFd, ava::core::UniqueFd> make_active_input_pipe()
{
  std::array<int, 2> descriptors{-1, -1};
  if (::pipe(descriptors.data()) != 0)
    throw std::system_error(errno, std::generic_category(), "create active input dispatch pipe");
  return {ava::core::UniqueFd(descriptors[0]), ava::core::UniqueFd(descriptors[1])};
}

// Publish only the bindings under test through the same local update boundary used by the session.
void bind_active_input_actions(ava::tui::TuiFrontEnd& frontend, ava::tui::RuntimePresentationState& presentation,
                               std::initializer_list<std::pair<TuiAction, Key>> actions)
{
  ava::tui::TuiKeyBindings bindings;
  for (auto const& [action, key] : actions)
    bindings.bindings.emplace_back(action, std::vector<Key>{key});
  presentation.apply_local_key_bindings(frontend, std::move(bindings));
}

// Restore a clean editor boundary and publish the draft before the controller sees its next event.
void seed_active_input_draft(ava::tui::Composer& composer, ava::tui::RuntimePresentationState& presentation, std::string text,
                             std::size_t cursor = std::string::npos)
{
  composer.reset_for_session_transition();
  static_cast<void>(composer.replace_draft(std::move(text), cursor));
  presentation.publish_composer(composer);
  presentation.publish_status({});
}

// Route one decoded non-character key through all real ActiveRunInput stages.
TuiInputOutcome dispatch_active_key(ava::tui::ActiveRunInput& input, Key key)
{
  return input.dispatch(
      ava::tui::runtime_input::RuntimeInput{.event = {.key = key, .character = key == Key::Space ? ' ' : '\0'}, .text = key == Key::Space ? " " : ""});
}

// A failed selection copy consumes a shared DeleteBackward binding and preserves the selected bytes.
void test_active_copy_failure_precedes_delete(ava::tui::ActiveRunInput& input, ava::tui::TuiFrontEnd& frontend,
                                              ava::tui::RuntimePresentationState& presentation, ava::tui::Composer& composer)
{
  bind_active_input_actions(frontend, presentation, {{TuiAction::CopySelection, Key::F6}, {TuiAction::DeleteBackward, Key::F6}});
  std::string const text(ava::tui::runtime_transcript::kMaxTerminalClipboardTextBytes + 1, 'x');
  seed_active_input_draft(composer, presentation, text);
  composer.begin_pointer_selection(0, true);
  composer.extend_pointer_selection(text.size());
  composer.end_pointer_selection();
  auto const selection = composer.selection_bounds();
  if (!ava::tests::require(selection && selection->first == 0 && selection->second == text.size(),
                           "active copy fixture selects more than the terminal clipboard limit"))
    return;
  // The size ceiling rejects this copy before transport, so no clipboard or environment override is needed.
  auto const result = dispatch_active_key(input, Key::F6);
  expect(
      result == TuiInputOutcome::Handled && composer.draft().text == text && composer.draft().cursor == text.size() && composer.selection_bounds() == selection,
      "active selection copy consumes its binding even when the clipboard rejects the payload");

  seed_active_input_draft(composer, presentation, "abc");
  auto const without_selection = dispatch_active_key(input, Key::F6);
  expect(without_selection == TuiInputOutcome::Handled && composer.draft().text == "ab" && composer.draft().cursor == 2,
         "active copy without a selection yields the shared binding to DeleteBackward");
}

// Interrupt clears and publishes a nonempty draft before the shared ordinary ClearInput stage runs.
void test_active_interrupt_precedes_clear(ava::tui::ActiveRunInput& input, ava::tui::TuiFrontEnd& frontend, ava::tui::RuntimePresentationState& presentation,
                                          ava::tui::Composer& composer)
{
  bind_active_input_actions(frontend, presentation, {{TuiAction::Interrupt, Key::F6}, {TuiAction::ClearInput, Key::F6}});
  seed_active_input_draft(composer, presentation, "interrupt draft", 4);
  composer.dismiss_completion();
  presentation.publish_status("prior feedback");
  auto const result = dispatch_active_key(input, Key::F6);
  // Inspect before painting: the controller publishes the cleared input synchronously.
  expect(result == TuiInputOutcome::Handled && composer.draft().text.empty() && composer.draft().cursor == 0 && presentation.screen().input.empty() &&
             presentation.screen().input_cursor == 0 && presentation.screen().status.empty() && !composer.palette_suppressed(),
         "active Interrupt publishes its cleared input and interaction reset immediately");
  auto const typed = input.dispatch(ava::tui::runtime_input::RuntimeInput{.event = {.key = Key::Character, .character = 'x'}, .text = "x"});
  expect(typed == TuiInputOutcome::Handled && composer.draft().text == "x", "clearing a nonempty active draft with Interrupt keeps later input available");
}

// NewLine wins over FollowUp on a shared key without delivering a backend queue request.
void test_active_newline_precedes_follow_up(ava::tui::ActiveRunInput& input, ava::tui::TuiFrontEnd& frontend, ava::tui::RuntimePresentationState& presentation,
                                            ava::tui::Composer& composer, std::vector<std::string> const& follow_ups)
{
  bind_active_input_actions(frontend, presentation, {{TuiAction::NewLine, Key::F6}, {TuiAction::MessageFollowUp, Key::F6}});
  seed_active_input_draft(composer, presentation, "firstsecond", 5);
  auto const queued_before = follow_ups.size();
  auto const result = dispatch_active_key(input, Key::F6);
  expect(result == TuiInputOutcome::Handled && composer.draft().text == "first\nsecond" && composer.draft().cursor == 6 && follow_ups.size() == queued_before &&
             composer.history().empty(),
         "active NewLine edits the draft before the colliding FollowUp binding can queue it");
}

// Submit bound to Space queues the exact draft before the ordinary literal-space fallback.
void test_active_space_submit_precedes_text(ava::tui::ActiveRunInput& input, ava::tui::TuiFrontEnd& frontend, ava::tui::RuntimePresentationState& presentation,
                                            ava::tui::Composer& composer, std::vector<std::string> const& follow_ups)
{
  bind_active_input_actions(frontend, presentation, {{TuiAction::Submit, Key::Space}});
  seed_active_input_draft(composer, presentation, "queue exactly this");
  auto const queued_before = follow_ups.size();
  auto const submitted = dispatch_active_key(input, Key::Space);
  expect(submitted == TuiInputOutcome::Handled && follow_ups.size() == queued_before + 1 && follow_ups.back() == "queue exactly this" &&
             composer.draft().text.empty() && presentation.screen().status == "follow-up queued",
         "active Submit bound to Space delivers the unchanged draft and commits its queue reset");

  bind_active_input_actions(frontend, presentation, {});
  seed_active_input_draft(composer, presentation, "ordinary");
  auto const inserted = dispatch_active_key(input, Key::Space);
  expect(inserted == TuiInputOutcome::Handled && composer.draft().text == "ordinary " && composer.draft().cursor == 9 && follow_ups.size() == queued_before + 1,
         "active unbound Space inserts one literal space without another queue delivery");
}

// Empty history routes Previous and Next requests to three-row transcript scrolling, with Previous winning collisions.
void test_active_history_fallback_scrolls(ava::tui::ActiveRunInput& input, ava::tui::TuiFrontEnd& frontend, ava::tui::RuntimePresentationState& presentation,
                                          ava::tui::Composer& composer, ava::tui::RuntimeRenderer& renderer, ava::tui::RuntimeNavigationController& navigation)
{
  bind_active_input_actions(frontend, presentation, {{TuiAction::HistoryPrev, Key::F6}, {TuiAction::HistoryNext, Key::F6}});
  seed_active_input_draft(composer, presentation, "navigation draft", 4);
  navigation.jump_to_bottom("live tail");
  auto const previous = dispatch_active_key(input, Key::F6);
  expect(previous == TuiInputOutcome::Handled && renderer.transcript().scroll_offset() == 3 && composer.draft().text == "navigation draft" &&
             composer.draft().cursor == 4 && !composer.history_index(),
         "active shared Previous/Next binding scrolls upward once when history is empty");

  bind_active_input_actions(frontend, presentation, {{TuiAction::HistoryNext, Key::F6}});
  auto const next = dispatch_active_key(input, Key::F6);
  expect(next == TuiInputOutcome::Handled && renderer.transcript().scroll_offset() == 0 && composer.draft().text == "navigation draft" &&
             composer.draft().cursor == 4 && !composer.history_index(),
         "active Next with empty history executes its downward scroll request and preserves the draft");
}

// Active horizontal edits and Undo consume shared explicit transcript and history bindings first.
void test_active_edits_precede_transcript(ava::tui::ActiveRunInput& input, ava::tui::TuiFrontEnd& frontend, ava::tui::RuntimePresentationState& presentation,
                                          ava::tui::Composer& composer, ava::tui::RuntimeRenderer& renderer, ava::tui::RuntimeNavigationController& navigation)
{
  for (auto const action : {TuiAction::CursorLeft, TuiAction::CursorRight, TuiAction::Undo})
  {
    bind_active_input_actions(frontend, presentation, {{action, Key::F6}, {TuiAction::PageUp, Key::F6}, {TuiAction::HistoryPrev, Key::F6}});
    seed_active_input_draft(composer, presentation, "abcd", 2);
    composer.record_submission("history candidate");
    if (action == TuiAction::Undo)
      static_cast<void>(composer.replace_draft("edited", 6));
    presentation.publish_composer(composer);
    navigation.jump_to_bottom("live tail");
    auto const result = dispatch_active_key(input, Key::F6);
    auto const expected_cursor = action == TuiAction::CursorLeft ? 1U : action == TuiAction::CursorRight ? 3U : 2U;
    expect(result == TuiInputOutcome::Handled && composer.draft().text == "abcd" && composer.draft().cursor == expected_cursor &&
               renderer.transcript().scroll_offset() == 0 && !composer.history_index(),
           "active " + ava::tui::action_name(action) + " captures a colliding PageUp and HistoryPrev before either navigation stage");
  }
}

// Explicit transcript page actions win over history even when previous and next history entries are available.
void test_active_transcript_precedes_history(ava::tui::ActiveRunInput& input, ava::tui::TuiFrontEnd& frontend, ava::tui::RuntimePresentationState& presentation,
                                             ava::tui::Composer& composer, ava::tui::RuntimeRenderer& renderer,
                                             ava::tui::RuntimeNavigationController& navigation)
{
  bind_active_input_actions(
      frontend, presentation,
      {{TuiAction::PageUp, Key::F6}, {TuiAction::HistoryPrev, Key::F6}, {TuiAction::PageDown, Key::F7}, {TuiAction::HistoryNext, Key::F7}});
  seed_active_input_draft(composer, presentation, "scratch", 3);
  composer.record_submission("older prompt");
  composer.record_submission("middle prompt");
  composer.record_submission("newer prompt");
  if (!ava::tests::require(composer.browse_history(true) && composer.browse_history(true), "active page fixture has both history directions available"))
    return;
  presentation.publish_composer(composer);
  auto const history_index = composer.history_index();
  auto const text = composer.draft().text;
  auto const cursor = composer.draft().cursor;
  navigation.jump_to_bottom("live tail");
  auto const page_rows = navigation.transcript_page_size();
  auto const previous = dispatch_active_key(input, Key::F6);
  expect(previous == TuiInputOutcome::Handled && page_rows > 0 && renderer.transcript().scroll_offset() == page_rows && composer.draft().text == text &&
             composer.draft().cursor == cursor && composer.history_index() == history_index,
         "active PageUp changes the transcript before a colliding HistoryPrev can replace the draft");
  auto const next = dispatch_active_key(input, Key::F7);
  expect(next == TuiInputOutcome::Handled && renderer.transcript().scroll_offset() == 0 && composer.draft().text == text && composer.draft().cursor == cursor &&
             composer.history_index() == history_index,
         "active PageDown changes the transcript before a colliding HistoryNext can replace the draft");
}

}  // namespace

// Run with the shared harness's initialized Application and terminal; all feature timers use this real loop.
void run_tui_active_input_route_tests()
{
  auto& terminal_context = ava::core::Application::instance().terminal_context();
  static_cast<void>(ava::tui::terminal::Context::resizeterm(24, 120));
  auto descriptors = make_active_input_pipe();
  ava::tui::TuiEventLoop loop(descriptors.first.get());
  std::vector<std::string> follow_ups;
  std::promise<void> finish_run;
  auto run_release = finish_run.get_future().share();
  std::promise<void> worker_finished;
  auto finished = worker_finished.get_future();
  ava::tui::TuiRuntimeOptions options;
  std::string transcript = "active navigation transcript";
  for (int line = 0; line < 100; ++line)
    transcript += "\nretained line " + std::to_string(line);
  options.initial_transcript = {{.label = "ava", .text = std::move(transcript)}};
  tui_test_support::TuiFrontEndFixture frontend(std::move(options));
  // Queue acceptance is synchronous on the TUI thread. The vector precedes the
  // frontend and run, so each copied pointer remains valid until their hooks retire.
  frontend.hooks().create_active_run_queues = [follow_ups = &follow_ups](ava::event::EventEnvelopeSink) {
    ava::tui::TuiActiveRunQueues queues;
    queues.active_request_id = "active-input-route-test";
    queues.queue_follow_up = [follow_ups](std::string text) -> ava::core::VoidResult {
      follow_ups->push_back(std::move(text));
      return {};
    };
    return queues;
  };
  // A real active worker is necessary: inactive ActiveRun captures all input.
  // Copy the wait handle to the worker. Its completion promise outlives the run
  // and is touched only by this callback until the TUI thread observes readiness.
  frontend.hooks().on_submit = [run_release, worker_finished = &worker_finished](std::string const&, ava::tui::TuiSubmitContext) {
    expect(run_release.wait_until(ava::tests::now_plus_seconds(30)) == std::future_status::ready,
           "active input fixture releases its fake worker before the emergency timeout");
    worker_finished->set_value();
    return ava::tui::TuiSubmitResult{};
  };

  // Explicit local construction follows production borrowing order; no test owner bundle escapes this function.
  ava::tui::RuntimePresentationState presentation(frontend.frontend(), {});
  presentation.publish_visible_sidebar(presentation.sidebar());
  ava::tui::PendingAttachments attachments(presentation);
  ava::tui::TuiSessionGrantRegistry grants;
  ava::tui::Composer composer;
  ava::tui::RuntimeRenderer renderer(frontend.frontend(), presentation, composer);
  renderer.attach_loop(loop);
  ava::tui::RuntimeNavigationController navigation(*frontend.frontend(), presentation, composer, renderer);
  ava::tui::SelectorHost selectors(presentation);
  ava::tui::TranscriptSearchController search(presentation, renderer, navigation, selectors);
  ava::tui::SessionSelector sessions(*frontend.frontend(), presentation, selectors, renderer, terminal_context);
  ava::tui::RuntimePromptCoordinator prompts(*frontend.frontend(), presentation, grants, renderer, loop);
  ava::tui::RuntimePromptStashController stash(presentation, composer, renderer, selectors, frontend.frontend()->key_bindings(), attachments);
  ava::tui::RuntimePluginUiCoordinator plugin_ui(presentation);
  plugin_ui.attach_loop(loop);
  ava::tui::RuntimeActionController actions(*frontend.frontend(), presentation, composer, renderer, selectors, sessions, attachments);
  actions.attach_loop(loop);
  ava::tui::RuntimeSubagentWorkspaceController workspace(*frontend.frontend(), presentation, selectors);
  workspace.attach_loop(loop, renderer);
  ava::tui::SessionTransitionCoordinator transitions(presentation, composer, renderer, search, workspace, selectors, attachments, grants);
  ava::tui::RuntimeMermaidPresentationController mermaid(presentation, renderer, search, {});
  mermaid.attach_loop(loop);
  ava::tui::ActiveRun active_run(loop, *frontend.frontend(), presentation, renderer, prompts, plugin_ui, search, workspace, transitions, mermaid, attachments,
                                 terminal_context);
  // This guard is destroyed before ActiveRun, including failed setup and assertion unwinding, so joining cannot strand the worker.
  auto release_run = at_scope_end([&finish_run] { finish_run.set_value(); });
  ava::tui::CommandOutput command_output(presentation);
  ava::tui::ActiveRunInput input(*frontend.frontend(), presentation, composer, renderer, stash, navigation, actions, search, workspace, active_run,
                                 command_output, terminal_context);
  if (!ava::tests::require(active_run.start("keep the fake active run open"), "active input fixture starts its real lifecycle owner"))
    return;
  if (!ava::tests::require(renderer.paint_ready_frame(), "active input fixture paints its initial transcript layout"))
    return;

  test_active_copy_failure_precedes_delete(input, *frontend.frontend(), presentation, composer);
  test_active_interrupt_precedes_clear(input, *frontend.frontend(), presentation, composer);
  test_active_newline_precedes_follow_up(input, *frontend.frontend(), presentation, composer, follow_ups);
  test_active_space_submit_precedes_text(input, *frontend.frontend(), presentation, composer, follow_ups);
  test_active_history_fallback_scrolls(input, *frontend.frontend(), presentation, composer, renderer, navigation);
  test_active_edits_precede_transcript(input, *frontend.frontend(), presentation, composer, renderer, navigation);
  test_active_transcript_precedes_history(input, *frontend.frontend(), presentation, composer, renderer, navigation);

  release_run.once();
  // A broken worker handoff must fail within a deadline, not hang in shutdown's join.
  if (!ava::tests::require(finished.wait_until(ava::tests::now_plus_seconds(2)) == std::future_status::ready,
                           "released active input worker finishes before shutdown joins it"))
    std::abort();
  static_cast<void>(active_run.shutdown());
  expect(!active_run.active(), "active input fixture retires its worker before destroying borrowed route owners");
}
