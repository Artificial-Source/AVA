// Protect Composer dispatch precedence and completion feedback through the production handler seams.
#include "sys.h"
#include "support/TuiEventLoopFixture.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/Composer.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace {

using ava::tui::CompletionStep;
using ava::tui::ComposerInputPhase;
using ava::tui::ComposerInputRequest;
using ava::tui::terminal::Key;

// Build a value-owned command catalog before the synchronous fixture borrows it.
ava::tui::TuiRuntimeOptions slash_options(std::vector<ava::tui::SlashCommandItem> commands)
{
  ava::tui::TuiRuntimeOptions options;
  options.slash_commands = std::move(commands);
  return options;
}

// Build file candidates without opening files or creating backend authority.
ava::tui::TuiRuntimeOptions file_options(std::vector<ava::tui::FileReferenceItem> files)
{
  ava::tui::TuiRuntimeOptions options;
  options.file_references = std::move(files);
  return options;
}

// Owns synchronous TUI-thread dispatch collaborators; declaration order keeps borrowed owners alive through teardown.
struct ComposerDispatchFixture
{
  // Assemble real editor/render owners with timer registrations; this fixture does
  // not run a session or start a worker.
  explicit ComposerDispatchFixture(ava::tui::TuiRuntimeOptions options = {})
      : frontend(std::move(options)),
        presentation(frontend.frontend(), {}),
        renderer(event_loop.loop(), frontend.frontend(), presentation, composer),
        navigation(*frontend.frontend(), presentation, composer, renderer)
  {
  }

  // Apply parser-approved bindings through the production publication boundary before dispatch.
  bool configure_bindings(std::string_view json)
  {
    auto bindings = ava::tui::parse_key_bindings_json(json);
    if (!ava::tests::require(bindings.has_value(), "composer dispatch fixture accepts its keybinding configuration"))
      return false;
    presentation.apply_local_key_bindings(std::move(*bindings));
    return true;
  }

  // Prepare the production read-only completion model after each editor mutation.
  ava::tui::detail::CompletionMatchCache const& completions()
  {
    presentation.publish_completion(composer);
    static_cast<void>(renderer.refresh_completion_cache());
    return renderer.completion_cache();
  }

  // Timer registry and descriptors outlive the renderer that registers callbacks.
  tui_test_support::TuiEventLoopFixture event_loop;
  // Owns the canonical backend and frontend for the complete fixture lifetime.
  tui_test_support::TuiFrontEndFixture frontend;
  // Holds the screen projection borrowed by the renderer and navigation controller.
  ava::tui::RuntimePresentationState presentation;
  // Owns draft, selection, history, and completion state exercised by each case.
  ava::tui::Composer composer;
  // Owns completion caches and borrows the preceding frontend, presentation, and composer.
  ava::tui::RuntimeRenderer renderer;
  // Exercises navigation against the same prepared viewport; all preceding owners outlive it.
  ava::tui::RuntimeNavigationController navigation;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Shared ClearInput/Interrupt consumes a nonempty draft and passes an empty draft to the session.
void test_clear_input_yields_to_interrupt_only_when_empty()
{
  ComposerDispatchFixture fixture;
  if (!fixture.configure_bindings(R"({"clear_input":"F8","interrupt":"F8"})"))
    return;
  auto const& bindings = fixture.frontend.frontend()->key_bindings();
  ava::tui::runtime_input::RuntimeInput const input{.event = {.key = Key::F8}, .text = {}};
  fixture.composer.restore_draft("draft to clear");
  fixture.presentation.publish_composer(fixture.composer);

  auto const cleared = fixture.composer.handle_key(input, bindings, fixture.presentation.screen(), fixture.completions(), false, ComposerInputPhase::Editor);
  expect(cleared.handled && cleared.request == ComposerInputRequest::None && cleared.status == "input cleared" && !cleared.beep &&
             fixture.composer.draft().text.empty(),
         "composer Editor phase clears a nonempty draft before the shared Interrupt binding");

  fixture.presentation.publish_composer(fixture.composer);
  auto const empty = fixture.composer.handle_key(input, bindings, fixture.presentation.screen(), fixture.completions(), false, ComposerInputPhase::Editor);
  expect(!empty.handled && empty.request == ComposerInputRequest::None && !empty.status && !empty.beep &&
             ava::tui::key_matches_action(bindings, ava::tui::TuiAction::Interrupt, input.event.key),
         "composer Editor phase leaves an empty shared ClearInput/Interrupt key available to session dispatch");
}

// Transcript copying retains the draft when CopySelection and ClearInput share a key.
void test_transcript_copy_precedes_clear_input()
{
  ComposerDispatchFixture fixture;
  if (!fixture.configure_bindings(R"({"copy_selection":"F8","clear_input":"F8","interrupt":"F8"})"))
    return;
  auto const& bindings = fixture.frontend.frontend()->key_bindings();
  ava::tui::runtime_input::RuntimeInput const input{.event = {.key = Key::F8}, .text = {}};
  fixture.composer.restore_draft("unselected draft");
  auto const armed = fixture.composer.handle_escape();
  fixture.presentation.publish_composer(fixture.composer);
  if (!ava::tests::require(!fixture.composer.selection_bounds() && armed.status == "press Esc again to clear",
                           "transcript copy fixture has an unselected draft and pending Escape"))
    return;

  auto const copied = fixture.composer.handle_key(input, bindings, fixture.presentation.screen(), fixture.completions(), true, ComposerInputPhase::Editor);
  expect(copied.handled && copied.request == ComposerInputRequest::CopyTranscript && !copied.status && !copied.beep &&
             fixture.composer.draft().text == "unselected draft",
         "composer requests transcript copy before clearing an unselected draft on the shared key");
  auto const after_copy = fixture.composer.handle_escape();
  expect(after_copy.status == "press Esc again to clear" && fixture.composer.draft().text == "unselected draft",
         "composer transcript copy resets Escape so the next Escape preserves the draft");
  fixture.composer.cancel_escape();

  auto const cleared = fixture.composer.handle_key(input, bindings, fixture.presentation.screen(), fixture.completions(), false, ComposerInputPhase::Editor);
  expect(cleared.handled && cleared.request == ComposerInputRequest::None && cleared.status == "input cleared" && fixture.composer.draft().text.empty(),
         "composer shared copy/clear key clears the draft when neither surface has a selection");
}

// A configured HistoryPrev action with no history requests transcript scrolling without editing the draft.
void test_empty_history_requests_transcript_scroll()
{
  ComposerDispatchFixture fixture;
  if (!fixture.configure_bindings(R"({"history_prev":"F8"})"))
    return;
  auto const& bindings = fixture.frontend.frontend()->key_bindings();
  ava::tui::runtime_input::RuntimeInput const input{.event = {.key = Key::F8}, .text = {}};
  fixture.composer.restore_draft("first line\nsecond line");
  fixture.presentation.publish_composer(fixture.composer);
  auto const cursor = fixture.composer.draft().cursor;

  auto const fallback =
      fixture.composer.handle_key(input, bindings, fixture.presentation.screen(), fixture.completions(), false, ComposerInputPhase::Navigation);
  expect(fallback.handled && fallback.request == ComposerInputRequest::ScrollUp && !fallback.status && !fallback.beep &&
             fixture.composer.draft().text == "first line\nsecond line" && fixture.composer.draft().cursor == cursor && !fixture.composer.history_index(),
         "composer empty HistoryPrev returns ScrollUp while preserving the multiline draft and cursor");

  fixture.composer.record_submission("previous prompt");
  auto const recalled =
      fixture.composer.handle_key(input, bindings, fixture.presentation.screen(), fixture.completions(), false, ComposerInputPhase::Navigation);
  expect(recalled.handled && recalled.request == ComposerInputRequest::None && recalled.status == "history previous" &&
             fixture.composer.draft().text == "previous prompt",
         "composer HistoryPrev recalls available history instead of requesting transcript scrolling");
  expect(fixture.composer.browse_history(false) && fixture.composer.draft().text == "first line\nsecond line" &&
             fixture.composer.draft().cursor == fixture.composer.draft().text.size() && !fixture.composer.history_index(),
         "forward history restores the complete multiline scratch draft at its end");
}

// Submit bound to Space is returned before Tail considers inserting a literal space.
void test_space_bound_to_submit_does_not_insert()
{
  ComposerDispatchFixture fixture;
  if (!fixture.configure_bindings(R"({"submit":"Space"})"))
    return;
  ava::tui::runtime_input::RuntimeInput const input{.event = {.key = Key::Space, .character = ' '}, .text = {}};
  fixture.composer.restore_draft("ready");
  fixture.presentation.publish_composer(fixture.composer);

  auto const submitted = fixture.composer.handle_key(input, fixture.frontend.frontend()->key_bindings(), fixture.presentation.screen(), fixture.completions(),
                                                     false, ComposerInputPhase::Tail);
  expect(submitted.handled && submitted.request == ComposerInputRequest::Submit && !submitted.status && !submitted.beep &&
             fixture.composer.draft().text == "ready" && fixture.composer.draft().cursor == 5,
         "composer Space submit requests submission and preserves draft content and cursor");

  fixture.presentation.apply_local_key_bindings(ava::tui::default_key_bindings());
  auto const inserted = fixture.composer.handle_key(input, fixture.frontend.frontend()->key_bindings(), fixture.presentation.screen(), fixture.completions(),
                                                    false, ComposerInputPhase::Tail);
  expect(inserted.handled && inserted.request == ComposerInputRequest::None && fixture.composer.draft().text == "ready ",
         "composer Tail inserts an ordinary Space when Submit does not claim the key");
}

// Cancel clears draft selection, then requests transcript deselection, then dismisses completion before arming Escape.
void test_cancel_selection_and_palette_priority()
{
  ComposerDispatchFixture fixture(slash_options({{.command = "/alpha", .description = "Alpha"}, {.command = "/beta", .description = "Beta"}}));
  auto const& bindings = fixture.frontend.frontend()->key_bindings();
  ava::tui::runtime_input::RuntimeInput const input{.event = {.key = Key::Escape}, .text = {}};
  fixture.composer.restore_draft("/");
  fixture.composer.begin_pointer_selection(0, true);
  fixture.composer.extend_pointer_selection(1);
  fixture.composer.end_pointer_selection();
  fixture.presentation.publish_composer(fixture.composer);
  if (!ava::tests::require(fixture.composer.selection_bounds().has_value() && fixture.navigation.slash_palette_active(),
                           "Cancel fixture has both a selected draft and active slash palette"))
    return;

  auto const draft = fixture.composer.handle_key(input, bindings, fixture.presentation.screen(), fixture.completions(), true, ComposerInputPhase::Tail);
  expect(draft.handled && draft.request == ComposerInputRequest::None && draft.status == "" && !fixture.composer.selection_bounds() &&
             !fixture.composer.palette_suppressed() && fixture.composer.draft().text == "/",
         "composer Cancel gives draft selection priority over transcript selection and completion");
  auto const after_draft_selection = fixture.composer.handle_escape();
  expect(after_draft_selection.status == "press Esc again to clear" && fixture.composer.draft().text == "/",
         "composer cancelling draft selection leaves the next Escape as a first press");
  fixture.composer.cancel_escape();

  auto const transcript = fixture.composer.handle_key(input, bindings, fixture.presentation.screen(), fixture.completions(), true, ComposerInputPhase::Tail);
  expect(transcript.handled && transcript.request == ComposerInputRequest::ClearTranscriptSelection && transcript.status == "" &&
             !fixture.composer.palette_suppressed(),
         "composer Cancel requests transcript deselection before dismissing the active palette");
  auto const after_transcript_selection = fixture.composer.handle_escape();
  expect(after_transcript_selection.status == "press Esc again to clear" && fixture.composer.draft().text == "/",
         "composer cancelling transcript selection leaves the next Escape as a first press");
  fixture.composer.cancel_escape();

  auto const palette = fixture.composer.handle_key(input, bindings, fixture.presentation.screen(), fixture.completions(), false, ComposerInputPhase::Tail);
  expect(palette.handled && palette.request == ComposerInputRequest::None && palette.status == "" && fixture.composer.palette_suppressed() &&
             fixture.composer.draft().text == "/",
         "composer Cancel dismisses the palette without arming double-Escape clearing");

  auto const pending = fixture.composer.handle_key(input, bindings, fixture.presentation.screen(), fixture.completions(), false, ComposerInputPhase::Tail);
  expect(
      pending.handled && pending.request == ComposerInputRequest::None && pending.status == "press Esc again to clear" && fixture.composer.draft().text == "/",
      "composer Cancel arms Escape only after selection and completion have yielded");
  auto const cleared = fixture.composer.handle_key(input, bindings, fixture.presentation.screen(), fixture.completions(), false, ComposerInputPhase::Tail);
  expect(cleared.handled && cleared.request == ComposerInputRequest::None && cleared.status == "input cleared" && fixture.composer.draft().text.empty(),
         "composer Cancel clears the draft on the second unclaimed Escape");
}

// Slash completion wraps both directions and reports an unmatched query without changing the draft.
void test_slash_completion_steps_wrap_and_report_status()
{
  ComposerDispatchFixture fixture(slash_options(
      {{.command = "/alpha", .description = "Alpha"}, {.command = "/beta", .description = "Beta"}, {.command = "/gamma", .description = "Gamma"}}));
  fixture.composer.restore_draft("/");
  auto const armed = fixture.composer.handle_escape();
  if (!ava::tests::require(armed.status == "press Esc again to clear", "slash completion fixture starts with pending Escape feedback"))
    return;
  fixture.presentation.publish_composer(fixture.composer);

  auto const previous = fixture.composer.step_completion(CompletionStep::Previous, fixture.completions(), fixture.presentation.screen());
  expect(previous.handled && previous.request == ComposerInputRequest::None && previous.status == "command 3/3" && !previous.beep &&
             fixture.composer.completion_index() == 2,
         "composer Previous wraps slash completion from the first command to the last");
  auto const after_previous = fixture.composer.handle_escape();
  expect(after_previous.status == "press Esc again to clear" && fixture.composer.draft().text == "/",
         "composer slash completion resets Escape so the next Escape preserves the draft");
  fixture.composer.cancel_escape();
  auto const next = fixture.composer.step_completion(CompletionStep::Next, fixture.completions(), fixture.presentation.screen());
  expect(next.handled && next.status == "command 1/3" && fixture.composer.completion_index() == 0 && fixture.composer.draft().text == "/",
         "composer Next wraps slash completion from the last command to the first without accepting it");

  fixture.composer.restore_draft("/zzzzzz");
  fixture.presentation.publish_composer(fixture.composer);
  auto const unmatched = fixture.composer.step_completion(CompletionStep::Next, fixture.completions(), fixture.presentation.screen());
  expect(unmatched.handled && unmatched.request == ComposerInputRequest::None && unmatched.status == "no matching slash commands" && !unmatched.beep &&
             fixture.composer.completion_index() == 0 && fixture.composer.draft().text == "/zzzzzz",
         "composer slash completion reports an unmatched query without accepting or editing it");
}

// File-reference and path completion share wrapping while retaining their distinct feedback labels.
void test_file_and_path_completion_steps_wrap_and_report_status()
{
  ComposerDispatchFixture fixture(
      file_options({{.value = "src/alpha.cpp", .description = "Alpha source"}, {.value = "src/beta.cpp", .description = "Beta source"}}));
  for (bool const reference : {true, false})
  {
    std::string const input = reference ? "@src/" : "src/";
    std::string const label = reference ? "reference " : "path ";
    fixture.composer.restore_draft(input);
    auto const armed = fixture.composer.handle_escape();
    if (!ava::tests::require(armed.status == "press Esc again to clear", label + "completion fixture starts with pending Escape feedback"))
      return;
    fixture.presentation.publish_composer(fixture.composer);

    auto const previous = fixture.composer.step_completion(CompletionStep::Previous, fixture.completions(), fixture.presentation.screen());
    expect(previous.handled && previous.request == ComposerInputRequest::None && previous.status == label + "2/2" && !previous.beep &&
               fixture.composer.completion_index() == 1,
           "composer Previous wraps " + label + "completion to the last match");
    auto const after_previous = fixture.composer.handle_escape();
    expect(after_previous.status == "press Esc again to clear" && fixture.composer.draft().text == input,
           "composer " + label + "completion resets Escape so the next Escape preserves the draft");
    fixture.composer.cancel_escape();
    auto const next = fixture.composer.step_completion(CompletionStep::Next, fixture.completions(), fixture.presentation.screen());
    expect(next.handled && next.request == ComposerInputRequest::None && next.status == label + "1/2" && !next.beep &&
               fixture.composer.completion_index() == 0 && fixture.composer.draft().text == input,
           "composer Next wraps " + label + "completion to the first match without accepting it");

    fixture.composer.dismiss_completion();
    auto const dismissed = fixture.composer.step_completion(CompletionStep::Next, fixture.completions(), fixture.presentation.screen());
    expect(!dismissed.handled && dismissed.request == ComposerInputRequest::None && !dismissed.status && !dismissed.beep &&
               fixture.composer.palette_suppressed() && fixture.composer.draft().text == input,
           "composer stepping a dismissed " + label + "palette yields without changing the draft");
  }
}

// Forced completion queries the proposed flags without borrowing or mutating navigation.
void test_forced_completion_uses_current_editor_flags()
{
  ComposerDispatchFixture fixture(file_options({{.value = "alpha.txt", .description = "Alpha file"}}));
  fixture.composer.restore_draft("alp");
  fixture.composer.dismiss_completion();
  static_cast<void>(fixture.completions());
  auto const accepted = fixture.composer.force_path_completion(fixture.presentation.screen());
  fixture.presentation.publish_completion(fixture.composer);
  expect(accepted.handled && accepted.status == "path selected" && !accepted.beep && fixture.composer.draft().text == "alpha.txt" &&
             !fixture.presentation.screen().path_completion_force_active,
         "forcing a suppressed bare prefix ranks with the proposed flags and accepts its one match");

  fixture.composer.restore_draft("unmatched");
  fixture.composer.dismiss_completion();
  static_cast<void>(fixture.completions());
  auto const unmatched = fixture.composer.force_path_completion(fixture.presentation.screen());
  fixture.presentation.publish_completion(fixture.composer);
  expect(!unmatched.handled && fixture.composer.palette_suppressed() && !fixture.presentation.screen().path_completion_force_active &&
             fixture.composer.draft().text == "unmatched",
         "an unmatched force attempt restores suppression without changing the draft");

  ComposerDispatchFixture disabled(file_options({{.value = "alpha.txt", .description = "Alpha file", .enabled = false, .disabled_reason = "not available"}}));
  disabled.composer.restore_draft("alp");
  static_cast<void>(disabled.completions());
  auto const rejected = disabled.composer.force_path_completion(disabled.presentation.screen(), ava::tui::ComposerEditContext::ActiveRun);
  disabled.presentation.publish_completion(disabled.composer);
  expect(rejected.handled && rejected.beep && rejected.status == "path disabled: not available" && disabled.composer.draft().text == "alp" &&
             !disabled.presentation.screen().path_completion_force_active,
         "active-run forcing reports a disabled candidate without accepting its text");
}

}  // namespace

// Run the focused production-dispatch cases from the shared TUI test harness.
void run_tui_composer_dispatch_tests()
{
  test_clear_input_yields_to_interrupt_only_when_empty();
  test_transcript_copy_precedes_clear_input();
  test_empty_history_requests_transcript_scroll();
  test_space_bound_to_submit_does_not_insert();
  test_cancel_selection_and_palette_priority();
  test_slash_completion_steps_wrap_and_report_status();
  test_file_and_path_completion_steps_wrap_and_report_status();
  test_forced_completion_uses_current_editor_flags();
}
