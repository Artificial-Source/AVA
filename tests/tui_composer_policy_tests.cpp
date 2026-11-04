// Protect idle and active Composer key policy under the initialized terminal test suite.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "ava/tui/Composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/keybindings.h"
#include "ava/tui/runtime_input_internal.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace {

using ava::tui::ComposerEditContext;
using ava::tui::ComposerInputPhase;
using ava::tui::ComposerInputRequest;
using ava::tui::ComposerJumpMode;
using ava::tui::TuiAction;
using ava::tui::runtime_input::RuntimeInput;
using ava::tui::terminal::Key;

// Owns the draft, catalogs, display snapshot, bindings and ranked indices for one synchronous case.
// The test thread uses every member and destroys this fixture after the case returns.
// No cached model or dispatch call borrows a temporary catalog or outlives this fixture.
struct ComposerPolicyFixture
{
  // Install deliberate collisions to test dispatch order independently of config
  // validation, which rejects some combinations. Keep unrelated default keys.
  void configure_bindings(std::initializer_list<std::pair<TuiAction, Key>> actions)
  {
    bindings = ava::tui::default_key_bindings();
    for (auto const& [action, key] : actions)
      for (auto& [candidate, keys] : bindings.bindings)
        if (candidate == action)
          keys = {key};
  }

  // Publish current editor flags before preparing the read-only completion cache.
  void prepare()
  {
    composer.project(snapshot);
    ava::tui::detail::refresh_completion_match_cache(completions, snapshot, snapshot.file_references_generation);
  }

  // Exercise one real dispatch seam with owned, current snapshot and completion data.
  ava::tui::ComposerInputResult dispatch(RuntimeInput const& input, ComposerInputPhase phase, ComposerEditContext context = ComposerEditContext::Idle,
                                         bool transcript_selected = false)
  {
    prepare();
    auto result = composer.handle_key(input, bindings, snapshot, completions, transcript_selected, phase, context);
    composer.project_completion(snapshot);
    return result;
  }

  // Install three deterministic slash candidates before any cache preparation.
  void slash_catalog()
  {
    snapshot.slash_commands = {
        {.command = "/alpha", .description = "Alpha"}, {.command = "/beta", .description = "Beta"}, {.command = "/gamma", .description = "Gamma"}};
  }

  // Recall owned history with its cursor at the start, keeping a distinguishable saved draft.
  bool recall(std::string text)
  {
    composer.record_submission(std::move(text));
    composer.restore_draft("scratch");
    return ava::tests::require(composer.browse_history(true) && composer.history_index() == 0 && composer.browse_history(false) &&
                                   composer.draft().text == "scratch" && !composer.history_index() && composer.browse_history(true) &&
                                   composer.history_index() == 0,
                               "composer policy fixture enters history browsing");
  }

  ava::tui::Composer composer; // Test-thread editor changed by each case and shared key dispatch.
  ava::tui::ComposerSnapshot snapshot; // Owned catalogs and current display values, refreshed by prepare.
  ava::tui::detail::CompletionMatchCache completions; // Prepared matches borrowed only during dispatch.
  ava::tui::TuiKeyBindings bindings = ava::tui::default_key_bindings(); // Each case installs its collisions before dispatch.

  // The fixture owns user-text examples and is local to this translation unit.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Idle deletion must win over both draft/transcript copy and clear on the same configured key.
void test_idle_deletion_precedes_copy_and_clear()
{
  for (bool const select_draft : {false, true})
  {
    ComposerPolicyFixture fixture;
    fixture.configure_bindings({{TuiAction::DeleteBackward, Key::F8}, {TuiAction::CopySelection, Key::F8}, {TuiAction::ClearInput, Key::F8}});
    fixture.composer.restore_draft("abc");
    if (select_draft)
    {
      fixture.composer.begin_pointer_selection(1, true);
      fixture.composer.extend_pointer_selection(3);
      fixture.composer.end_pointer_selection();
    }
    auto const result = fixture.dispatch({.event = {.key = Key::F8}, .text = {}}, ComposerInputPhase::Editor, ComposerEditContext::Idle, true);
    expect(result.handled && result.request == ComposerInputRequest::None && !result.status && !result.beep &&
               fixture.composer.draft().text == (select_draft ? "a" : "ab") && !fixture.composer.selection_bounds(),
           "idle deletion consumes the shared copy/clear key even when the transcript has a selection");
  }
}

// A rejected clipboard request still consumes idle CopySelection before a colliding ClearInput.
void test_idle_failed_copy_does_not_fall_through()
{
  ComposerPolicyFixture fixture;
  fixture.configure_bindings({{TuiAction::CopySelection, Key::F8}, {TuiAction::ClearInput, Key::F8}});
  // The terminal clipboard helper rejects more than 64 KiB without attempting I/O.
  std::string const text(64 * 1024 + 1, 'x');
  fixture.composer.restore_draft(text);
  fixture.composer.begin_pointer_selection(0, true);
  fixture.composer.extend_pointer_selection(text.size());
  fixture.composer.end_pointer_selection();
  auto const result = fixture.dispatch({.event = {.key = Key::F8}, .text = {}}, ComposerInputPhase::Editor);
  expect(result.handled && result.beep && result.status == "clipboard copy failed" && fixture.composer.draft().text == text &&
             fixture.composer.selection_bounds().has_value(),
         "failed idle clipboard copy consumes the key and preserves the selected draft");
}

// Omitting the new context argument must preserve idle ClearInput/Interrupt's nonempty-only interception.
void test_default_context_retains_idle_clear_interrupt_guard()
{
  ComposerPolicyFixture fixture;
  fixture.configure_bindings({{TuiAction::ClearInput, Key::F8}, {TuiAction::Interrupt, Key::F8}});
  RuntimeInput const input{.event = {.key = Key::F8}, .text = {}};
  fixture.composer.restore_draft("draft");
  fixture.prepare();
  auto const cleared = fixture.composer.handle_key(input, fixture.bindings, fixture.snapshot, fixture.completions, false, ComposerInputPhase::Editor);
  expect(cleared.handled && cleared.status == "input cleared" && !cleared.beep && cleared.request == ComposerInputRequest::None &&
             fixture.composer.draft().text.empty(),
         "default dispatch context clears nonempty input before an idle Interrupt collision");
  fixture.prepare();
  auto const empty = fixture.composer.handle_key(input, fixture.bindings, fixture.snapshot, fixture.completions, false, ComposerInputPhase::Editor);
  expect(!empty.handled && !empty.status && !empty.beep && empty.request == ComposerInputRequest::None &&
             ava::tui::key_matches_action(fixture.bindings, TuiAction::Interrupt, input.event.key),
         "default dispatch context yields an empty clear/Interrupt collision to the controller");
}

// Clearing preserves active forced-path state and feedback while idle clearing explicitly resets both.
void test_clear_context_controls_forcing_and_feedback()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    ComposerPolicyFixture fixture;
    fixture.configure_bindings({{TuiAction::ClearInput, Key::F8}});
    fixture.snapshot.file_references = {{.value = "alpha.txt", .description = "Alpha"}, {.value = "alpine.txt", .description = "Alpine"}};
    fixture.composer.restore_draft("alp");
    fixture.prepare();
    auto const forced = fixture.composer.force_path_completion(fixture.snapshot, context);
    fixture.composer.project_completion(fixture.snapshot);
    if (!ava::tests::require(forced.handled && forced.status == "path suggestions" && fixture.snapshot.path_completion_force_active,
                             "clear context fixture starts with forced path suggestions"))
      return;
    auto const cleared = fixture.dispatch({.event = {.key = Key::F8}, .text = {}}, ComposerInputPhase::Editor, context);
    bool const active = context == ComposerEditContext::ActiveRun;
    expect(cleared.handled && cleared.request == ComposerInputRequest::None && !cleared.beep && fixture.composer.draft().text.empty() &&
               fixture.snapshot.path_completion_force_active == active && (active ? !cleared.status : cleared.status == "input cleared"),
           "ClearInput dispatch retains active forcing and existing feedback but clears idle forcing with explicit feedback");
    auto const empty = fixture.dispatch({.event = {.key = Key::F8}, .text = {}}, ComposerInputPhase::Editor, context);
    expect(empty.handled && empty.request == ComposerInputRequest::None && !empty.beep && (active ? !empty.status : empty.status == "input already empty"),
           "an unopposed empty ClearInput consumes the key with context-specific feedback");
  }
}

// Even unsuccessful active cursor/redo actions leave history and reset completion; idle actions retain them.
void test_unsuccessful_edit_context_resets_history()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    for (bool const redo : {false, true})
    {
      ComposerPolicyFixture fixture;
      fixture.configure_bindings({{redo ? TuiAction::Redo : TuiAction::CursorLeft, Key::F8}});
      if (!fixture.recall("recorded"))
        return;
      fixture.composer.select_completion(1);
      auto const armed = fixture.composer.handle_escape();
      if (!ava::tests::require(
              fixture.composer.draft().cursor == 0 && fixture.composer.draft().redo_stack.empty() && armed.status == "press Esc again to clear",
              "unsuccessful edit fixture has no leftward cursor movement or redo available"))
        return;
      bool const active = context == ComposerEditContext::ActiveRun;
      auto const result =
          fixture.dispatch({.event = {.key = Key::F8}, .text = {}}, active ? ComposerInputPhase::Editor : ComposerInputPhase::Navigation, context);
      expect(result.handled && result.request == ComposerInputRequest::None && !result.beep && fixture.composer.draft().text == "recorded" &&
                 fixture.composer.draft().cursor == 0 && fixture.composer.history_index().has_value() == !active &&
                 fixture.composer.completion_index() == (active ? 0 : 1) && (redo && !active ? result.status == "nothing to redo" : !result.status),
             "unsuccessful edit dispatch applies active history/reset policy and preserves idle cursor/redo policy");
      auto const escaped = fixture.composer.handle_escape();
      expect(escaped.status == "press Esc again to clear" && fixture.composer.draft().text == "recorded",
             "unsuccessful editing disarms pending Escape in both contexts");
      // Observe restoration only after the Escape assertion; browsing disarms Escape itself.
      bool const restored = fixture.composer.browse_history(false);
      expect(restored == !active && fixture.composer.draft().text == (active ? "recorded" : "scratch") && !fixture.composer.history_index(),
             "unsuccessful idle editing preserves scratch restoration while active editing retires it");
    }
  }
}

// Horizontal editing lives in different phases but precedes palette/history within each Composer route.
void test_horizontal_edit_precedes_palette_and_history()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    ComposerPolicyFixture fixture;
    fixture.configure_bindings({{TuiAction::CursorLeft, Key::F8}, {TuiAction::PalettePrev, Key::F8}, {TuiAction::HistoryPrev, Key::F8}});
    fixture.slash_catalog();
    fixture.composer.record_submission("older");
    fixture.composer.restore_draft("/");
    fixture.composer.select_completion(1);
    bool const active = context == ComposerEditContext::ActiveRun;
    RuntimeInput const input{.event = {.key = Key::F8}, .text = {}};
    if (!active)
    {
      auto const editor = fixture.dispatch(input, ComposerInputPhase::Editor, context);
      expect(!editor.handled && fixture.composer.draft().cursor == 1 && fixture.composer.completion_index() == 1,
             "idle Editor leaves horizontal/palette/history collisions for its later Navigation phase");
    }
    auto const moved = fixture.dispatch(input, active ? ComposerInputPhase::Editor : ComposerInputPhase::Navigation, context);
    expect(moved.handled && moved.request == ComposerInputRequest::None && !moved.status && !moved.beep && fixture.composer.draft().text == "/" &&
               fixture.composer.draft().cursor == 0 && !fixture.composer.history_index() && fixture.composer.completion_index() == (active ? 0 : 1),
           "horizontal movement owns the collision without stepping the palette or recalling history");
  }
}

// Active Undo is an early edit; idle Undo stays behind visible completion and history navigation.
void test_undo_collision_uses_context_specific_phase()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    for (bool const palette : {false, true})
    {
      ComposerPolicyFixture fixture;
      fixture.configure_bindings({{TuiAction::Undo, Key::F8}, {TuiAction::PalettePrev, Key::F8}, {TuiAction::HistoryPrev, Key::F8}});
      fixture.slash_catalog();
      fixture.composer.record_submission("older");
      fixture.composer.restore_draft(palette ? "/" : "scratch");
      bool const active = context == ComposerEditContext::ActiveRun;
      RuntimeInput const input{.event = {.key = Key::F8}, .text = {}};
      if (!active)
      {
        auto const editor = fixture.dispatch(input, ComposerInputPhase::Editor, context);
        expect(!editor.handled, "idle Editor does not move Undo before its navigation barrier");
      }
      auto const result = fixture.dispatch(input, active ? ComposerInputPhase::Editor : ComposerInputPhase::Navigation, context);
      expect(result.handled && result.request == ComposerInputRequest::None && !result.beep,
             "Undo/palette/history collision is consumed by its selected context phase");
      if (active)
        expect(fixture.composer.draft().text.empty() && !result.status && !fixture.composer.history_index(),
               "active Undo wins over palette/history and preserves existing feedback");
      else if (palette)
        expect(fixture.composer.draft().text == "/" && result.status == "command 3/3" && fixture.composer.completion_index() == 2 &&
                   !fixture.composer.history_index(),
               "idle palette Previous wins over Undo and history");
      else
        expect(fixture.composer.draft().text == "older" && result.status == "history previous" && fixture.composer.history_index() == 0,
               "idle history Previous wins over Undo when no palette is visible");
    }
  }
}

// Every previous branch must finish before any next branch can claim an overlapping binding.
void test_previous_navigation_precedes_next_navigation()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    ComposerPolicyFixture fixture;
    fixture.slash_catalog();
    fixture.configure_bindings(
        {{TuiAction::PalettePrev, Key::F8}, {TuiAction::HistoryPrev, Key::F8}, {TuiAction::PaletteNext, Key::F8}, {TuiAction::HistoryNext, Key::F8}});
    fixture.composer.restore_draft("/");
    RuntimeInput const input{.event = {.key = Key::F8}, .text = {}};
    auto const palette = fixture.dispatch(input, ComposerInputPhase::Navigation, context);
    expect(palette.handled && palette.request == ComposerInputRequest::None && palette.status == "command 3/3" && fixture.composer.completion_index() == 2 &&
               fixture.composer.draft().text == "/",
           "palette Previous wraps to the last row before colliding palette/history Next");

    fixture.configure_bindings({{TuiAction::HistoryPrev, Key::F8}, {TuiAction::PaletteNext, Key::F8}, {TuiAction::HistoryNext, Key::F8}});
    fixture.composer.record_submission("older");
    fixture.composer.restore_draft("/");
    auto const history = fixture.dispatch(input, ComposerInputPhase::Navigation, context);
    expect(history.handled && history.request == ComposerInputRequest::None && history.status == "history previous" &&
               fixture.composer.draft().text == "older" && fixture.composer.history_index() == 0,
           "history Previous beats palette Next even when the slash palette is visible");
    expect(fixture.composer.browse_history(false) && fixture.composer.draft().text == "/" && !fixture.composer.history_index(),
           "the winning history branch restores the prior slash draft when browsing forward");

    ComposerPolicyFixture empty;
    empty.configure_bindings({{TuiAction::HistoryPrev, Key::F8}, {TuiAction::HistoryNext, Key::F8}});
    empty.composer.restore_draft("draft");
    auto const fallback = empty.dispatch(input, ComposerInputPhase::Navigation, context);
    expect(fallback.handled && fallback.request == ComposerInputRequest::ScrollUp && !fallback.status && !fallback.beep &&
               empty.composer.draft().text == "draft" && !empty.composer.history_index(),
           "empty colliding history directions request upward transcript scrolling");
  }
}

// Starting a jump preserves direction precedence; a paste disarms it and only active routing continues to insertion.
void test_armed_jump_bracketed_paste_context()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    ComposerPolicyFixture fixture;
    fixture.configure_bindings({{TuiAction::JumpForward, Key::F8}, {TuiAction::JumpBackward, Key::F8}, {TuiAction::DeleteBackward, Key::F8}});
    fixture.composer.restore_draft("ab");
    bool const active = context == ComposerEditContext::ActiveRun;
    auto const armed = fixture.dispatch({.event = {.key = Key::F8}, .text = {}}, active ? ComposerInputPhase::JumpStart : ComposerInputPhase::Editor, context);
    if (!ava::tests::require(armed.handled && armed.status == "jump forward: type character" && fixture.composer.jump_mode() == ComposerJumpMode::Forward &&
                                 fixture.composer.draft().text == "ab",
                             "jump start prefers forward over backward and deletion on their shared key"))
      return;
    RuntimeInput paste{};
    paste.event.key = Key::Character;
    paste.event.character = 'x';
    paste.text = "xy";
    paste.bracketed_paste = true;
    auto const jumped = fixture.dispatch(paste, ComposerInputPhase::Jump, context);
    expect(jumped.handled == !active && jumped.request == ComposerInputRequest::None && !jumped.status && !jumped.beep &&
               fixture.composer.jump_mode() == ComposerJumpMode::None && fixture.composer.draft().text == "ab",
           "armed jump consumes idle bracketed paste but yields active paste after disarming");
    if (active)
    {
      auto const inserted = fixture.dispatch(paste, ComposerInputPhase::Primary, context);
      expect(inserted.handled && inserted.request == ComposerInputRequest::None && inserted.status == "pasted into draft safely" && !inserted.beep &&
                 fixture.composer.draft().text == "abxy" && fixture.composer.draft().cursor == 4,
             "active paste that passed Jump reaches Primary insertion once");
    }
  }
}

// Ctrl+D is a forward edit only with draft text; an empty draft must remain available to the controller's Exit path.
void test_ctrl_d_delete_guard_in_both_contexts()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    ComposerPolicyFixture fixture;
    fixture.configure_bindings({{TuiAction::DeleteForward, Key::CtrlD}, {TuiAction::Exit, Key::CtrlD}});
    static_cast<void>(fixture.composer.replace_draft("ab", 0));
    RuntimeInput const input{.event = {.key = Key::CtrlD}, .text = {}};
    auto const deleted = fixture.dispatch(input, ComposerInputPhase::Editor, context);
    expect(deleted.handled && deleted.request == ComposerInputRequest::None && !deleted.status && !deleted.beep && fixture.composer.draft().text == "b" &&
               fixture.composer.draft().cursor == 0,
           "Ctrl+D deletes forward from a nonempty draft despite the configured Exit collision");
    fixture.composer.restore_draft("");
    auto const empty = fixture.dispatch(input, ComposerInputPhase::Editor, context);
    expect(!empty.handled && empty.request == ComposerInputRequest::None && !empty.status && !empty.beep &&
               ava::tui::key_matches_action(fixture.bindings, TuiAction::Exit, input.event.key),
           "empty Ctrl+D is left for Exit without inventing an editor request");
  }
}

// Slash acceptance changes its instruction and Escape-arm policy with the dispatch context.
void test_completion_context_feedback_and_escape()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    ComposerPolicyFixture fixture;
    fixture.slash_catalog();
    fixture.composer.restore_draft("/al");
    auto const armed = fixture.composer.handle_escape();
    if (!ava::tests::require(armed.status == "press Esc again to clear", "slash acceptance fixture starts with pending Escape"))
      return;
    bool const active = context == ComposerEditContext::ActiveRun;
    auto const accepted =
        fixture.dispatch({.event = {.key = Key::Tab}, .text = {}}, active ? ComposerInputPhase::Completion : ComposerInputPhase::Editor, context);
    expect(accepted.handled && accepted.request == ComposerInputRequest::None && !accepted.beep && fixture.composer.draft().text == "/alpha" &&
               accepted.status == (active ? "command selected - press Enter to queue" : "command selected - press Enter to run"),
           "shared slash acceptance preserves context-specific run/queue feedback without requesting submission");
    auto const escaped = fixture.composer.handle_escape();
    expect(escaped.status == (active ? "input cleared" : "press Esc again to clear") && fixture.composer.draft().text == (active ? "" : "/alpha"),
           "active keyboard acceptance preserves the Escape arm while idle acceptance cancels it");
  }
}

// The completion phase must call prospective forced-path ranking with the caller's reset policy.
void test_forced_completion_context_retains_active_history()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    // Escape and forward history each consume interaction state. Separate fixtures
    // keep both observations tied to the same original acceptance preconditions.
    for (bool inspect_history : {false, true})
    {
      ComposerPolicyFixture fixture;
      fixture.snapshot.file_references = {{.value = "alpha.txt", .description = "Alpha"}};
      if (!fixture.recall("alp"))
        return;
      static_cast<void>(fixture.composer.apply_action(TuiAction::CursorLineEnd));
      fixture.composer.select_completion(1);
      auto const armed = fixture.composer.handle_escape();
      if (!ava::tests::require(armed.status == "press Esc again to clear", "forced acceptance fixture starts with pending Escape"))
        return;
      bool const active = context == ComposerEditContext::ActiveRun;
      auto const accepted =
          fixture.dispatch({.event = {.key = Key::Tab}, .text = {}}, active ? ComposerInputPhase::Completion : ComposerInputPhase::Editor, context);
      expect(accepted.handled && accepted.request == ComposerInputRequest::None && accepted.status == "path selected" && !accepted.beep &&
                 fixture.composer.draft().text == "alpha.txt" && !fixture.snapshot.path_completion_force_active &&
                 fixture.composer.history_index().has_value() == active && fixture.composer.completion_index() == (active ? 1 : 0),
             "forced one-match acceptance uses active history/row preservation and idle resets");
      if (inspect_history)
      {
        bool const restored = fixture.composer.browse_history(false);
        expect(restored == active && fixture.composer.draft().text == (active ? "scratch" : "alpha.txt") && !fixture.composer.history_index(),
               "forced active acceptance restores saved scratch while idle acceptance leaves the accepted text");
        continue;
      }
      auto const escaped = fixture.composer.handle_escape();
      expect(escaped.status == (active ? "input cleared" : "press Esc again to clear"), "forced acceptance preserves active Escape and disarms idle Escape");

      fixture.composer.cancel_escape();
      fixture.composer.restore_draft("unmatched");
      fixture.composer.dismiss_completion();
      auto const unmatched =
          fixture.dispatch({.event = {.key = Key::Tab}, .text = {}}, active ? ComposerInputPhase::Completion : ComposerInputPhase::Editor, context);
      expect(!unmatched.handled && unmatched.request == ComposerInputRequest::None && !unmatched.status && !unmatched.beep &&
                 fixture.composer.draft().text == "unmatched" && fixture.composer.palette_suppressed() && !fixture.snapshot.path_completion_force_active,
             "failed forced completion yields while restoring suppression and leaving no forced palette");
    }
  }
}

// Disabled slash, reference, and path candidates consume acceptance with feedback without replacing text.
void test_disabled_completion_dispatch_preserves_context_escape()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    for (std::string const& text : {std::string("/al"), std::string("@src/al"), std::string("src/al")})
    {
      ComposerPolicyFixture fixture;
      fixture.snapshot.slash_commands = {{.command = "/alpha", .description = "Alpha", .enabled = false, .disabled_reason = "unavailable"}};
      fixture.snapshot.file_references = {{.value = "src/alpha.cpp", .description = "Alpha", .enabled = false, .disabled_reason = "unavailable"}};
      fixture.composer.restore_draft(text);
      auto const armed = fixture.composer.handle_escape();
      if (!ava::tests::require(armed.status == "press Esc again to clear", "disabled acceptance fixture starts with pending Escape"))
        return;
      bool const active = context == ComposerEditContext::ActiveRun;
      auto const rejected =
          fixture.dispatch({.event = {.key = Key::Tab}, .text = {}}, active ? ComposerInputPhase::Completion : ComposerInputPhase::Editor, context);
      std::string const prefix = text.starts_with('/') ? "command" : text.starts_with('@') ? "reference" : "path";
      expect(rejected.handled && rejected.request == ComposerInputRequest::None && rejected.beep && rejected.status == prefix + " disabled: unavailable" &&
                 fixture.composer.draft().text == text,
             "disabled " + prefix + " acceptance retains the draft and reports the reason");
      auto const escaped = fixture.composer.handle_escape();
      expect(escaped.status == (active ? "input cleared" : "press Esc again to clear"), "disabled acceptance preserves active Escape and cancels idle Escape");
    }
  }
}

// A visible slash palette with no matches owns navigation rather than unexpectedly recalling history.
void test_empty_visible_palette_blocks_history()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    ComposerPolicyFixture fixture;
    fixture.configure_bindings(
        {{TuiAction::PalettePrev, Key::F8}, {TuiAction::HistoryPrev, Key::F8}, {TuiAction::PaletteNext, Key::F8}, {TuiAction::HistoryNext, Key::F8}});
    fixture.slash_catalog();
    fixture.composer.record_submission("older");
    fixture.composer.restore_draft("/zzzzzz");
    auto const navigated = fixture.dispatch({.event = {.key = Key::F8}, .text = {}}, ComposerInputPhase::Navigation, context);
    expect(navigated.handled && navigated.request == ComposerInputRequest::None && navigated.status == "no matching slash commands" && !navigated.beep &&
               fixture.composer.draft().text == "/zzzzzz" && !fixture.composer.history_index(),
           "empty visible slash palette consumes Previous before history or Next can run");
    bool const active = context == ComposerEditContext::ActiveRun;
    auto const accepted =
        fixture.dispatch({.event = {.key = Key::Tab}, .text = {}}, active ? ComposerInputPhase::Completion : ComposerInputPhase::Editor, context);
    expect(accepted.handled && accepted.request == ComposerInputRequest::None && !accepted.beep && fixture.composer.draft().text == "/zzzzzz" &&
               !fixture.composer.history_index(),
           "accepting an empty visible slash palette consumes the key without replacing text or submitting");
  }
}

// Document-boundary movement is preemptive only during active input and retains its distinct reset policy.
void test_document_boundary_context_resets_history_and_forcing()
{
  for (auto const context : {ComposerEditContext::Idle, ComposerEditContext::ActiveRun})
  {
    ComposerPolicyFixture fixture;
    fixture.snapshot.file_references = {{.value = "alpha.txt", .description = "Alpha"}, {.value = "alpine.txt", .description = "Alpine"}};
    if (!fixture.recall("alp"))
      return;
    static_cast<void>(fixture.composer.apply_action(TuiAction::CursorLineEnd));
    fixture.prepare();
    auto const forced = fixture.composer.force_path_completion(fixture.snapshot, ComposerEditContext::ActiveRun);
    fixture.composer.project_completion(fixture.snapshot);
    if (!ava::tests::require(forced.handled && fixture.snapshot.path_completion_force_active && fixture.composer.history_index() == 0,
                             "boundary fixture has active forcing and retained history"))
      return;
    bool const active = context == ComposerEditContext::ActiveRun;
    RuntimeInput const input{.event = {.key = Key::CtrlHome}, .text = {}};
    if (!active)
    {
      auto const primary = fixture.dispatch(input, ComposerInputPhase::Primary, context);
      expect(!primary.handled && fixture.composer.draft().cursor == 3, "idle Primary leaves Ctrl+Home for Navigation");
    }
    auto const moved = fixture.dispatch(input, active ? ComposerInputPhase::Primary : ComposerInputPhase::Navigation, context);
    expect(moved.handled && moved.request == ComposerInputRequest::None && !moved.status && !moved.beep && fixture.composer.draft().text == "alp" &&
               fixture.composer.draft().cursor == 0 && fixture.composer.history_index().has_value() == !active &&
               fixture.snapshot.path_completion_force_active == !active,
           "Ctrl+Home dispatch resets active history/forcing and retains idle history/forcing");
  }
}

// Shared Primary/Tail expose only active editor work; controller-owned lifecycle precedence is tested elsewhere.
void test_active_primary_and_tail_leave_lifecycle_actions_to_controller()
{
  ComposerPolicyFixture fixture;
  fixture.configure_bindings({{TuiAction::NewLine, Key::F8}, {TuiAction::MessageFollowUp, Key::F8}, {TuiAction::Submit, Key::Space}});
  fixture.composer.restore_draft("draft");
  RuntimeInput const command{.event = {.key = Key::F8}, .text = {}};
  auto const active_primary = fixture.dispatch(command, ComposerInputPhase::Primary, ComposerEditContext::ActiveRun);
  expect(!active_primary.handled && active_primary.request == ComposerInputRequest::None && fixture.composer.draft().text == "draft",
         "active Primary yields NewLine/follow-up to their later controller stage");
  auto const idle_primary = fixture.dispatch(command, ComposerInputPhase::Primary);
  expect(idle_primary.handled && idle_primary.request == ComposerInputRequest::Submit && fixture.composer.draft().text == "draft",
         "idle Primary retains follow-up precedence over a colliding NewLine binding");

  RuntimeInput const space{.event = {.key = Key::Space, .character = ' '}, .text = {}};
  auto const idle_tail = fixture.dispatch(space, ComposerInputPhase::Tail);
  expect(idle_tail.handled && idle_tail.request == ComposerInputRequest::Submit && fixture.composer.draft().text == "draft",
         "idle Tail honors a Space-bound Submit before literal insertion");
  // This direct call models only reaching the fallback; ActiveRunInput must consume real Submit earlier.
  auto const active_tail = fixture.dispatch(space, ComposerInputPhase::Tail, ComposerEditContext::ActiveRun);
  expect(active_tail.handled && active_tail.request == ComposerInputRequest::None && fixture.composer.draft().text == "draft ",
         "active Tail contains the literal Space fallback without duplicating Submit policy");
  auto const cancel = fixture.dispatch({.event = {.key = Key::Escape}, .text = {}}, ComposerInputPhase::Tail, ComposerEditContext::ActiveRun);
  expect(!cancel.handled && cancel.request == ComposerInputRequest::None && !cancel.status && fixture.composer.draft().text == "draft ",
         "active Tail leaves cancellation to the active controller");

  auto const selected = fixture.dispatch({.event = {.key = Key::ShiftArrowLeft}, .text = {}}, ComposerInputPhase::Primary, ComposerEditContext::ActiveRun);
  expect(selected.handled && selected.request == ComposerInputRequest::ClearTranscriptSelection && fixture.composer.selection_bounds().has_value() &&
             fixture.composer.draft().text == "draft ",
         "active Primary handles shifted selection and asks its caller to clear transcript selection");
}

}  // namespace

// Run policy regressions inside the existing Application-owned initialized composer harness.
void run_tui_composer_policy_tests()
{
  test_idle_deletion_precedes_copy_and_clear();
  test_idle_failed_copy_does_not_fall_through();
  test_default_context_retains_idle_clear_interrupt_guard();
  test_clear_context_controls_forcing_and_feedback();
  test_unsuccessful_edit_context_resets_history();
  test_horizontal_edit_precedes_palette_and_history();
  test_undo_collision_uses_context_specific_phase();
  test_previous_navigation_precedes_next_navigation();
  test_armed_jump_bracketed_paste_context();
  test_ctrl_d_delete_guard_in_both_contexts();
  test_completion_context_feedback_and_escape();
  test_forced_completion_context_retains_active_history();
  test_disabled_completion_dispatch_preserves_context_escape();
  test_empty_visible_palette_blocks_history();
  test_document_boundary_context_resets_history_and_forcing();
  test_active_primary_and_tail_leave_lifecycle_actions_to_controller();
}
