// Direct selector and branch-summary contracts use the real TUI owners with an offline frontend.
#include "sys.h"
#include "support/test_harness.h"
#include "support/tui_test_support.h"
#include "ava/tui/BranchSummaryOperation.h"
#include "ava/tui/Composer.h"
#include "ava/tui/SelectorHost.h"
#include "ava/tui/SelectorInputController.h"
#include "ava/tui/TuiEventLoop.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/core/Application.h"
#include "ava/core/UniqueFd.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <unistd.h>

namespace {

using ava::tui::ActiveSelectList;
using ava::tui::SelectListInputAction;
using ava::tui::SelectorInputRequest;
using ava::tui::terminal::Key;

// Owns an offline frontend and the real loop/render/selector owners on the test
// thread. The process terminal is initialized by the common test runner. Reverse
// member destruction retires both owners and their timers before the loop and
// closes its pipe only after the waiter is gone; no worker is started here.
class FeatureOwnerFixture final
{
 public:
  // Construct every borrowed dependency before attaching renderer and summary
  // timers. All methods and returned references are used on this test thread.
  FeatureOwnerFixture()
      : descriptors_(open_pipe()),
        loop_(descriptors_.first.get()),
        presentation_(frontend_.frontend(), {}),
        renderer_(frontend_.frontend(), presentation_, composer_, &ava::core::Application::instance().terminal_context()),
        selectors_(presentation_),
        selector_input_(*frontend_.frontend(), presentation_, selectors_, renderer_, ava::core::Application::instance().terminal_context()),
        summary_(*frontend_.frontend(), presentation_, selectors_, renderer_, ava::core::Application::instance().terminal_context())
  {
    auto const [width, height] = ava::tui::terminal_size();
    presentation_.publish_dimensions(width, height);
    renderer_.attach_loop(loop_);
    summary_.attach_loop(loop_);
  }

  // Borrow the registered frontend fixture until this enclosing fixture dies.
  tui_test_support::TuiFrontEndFixture& frontend() { return frontend_; }
  // Borrow the TUI-thread publication owner; observations use its const screen.
  ava::tui::RuntimePresentationState& presentation() { return presentation_; }
  // Borrow selector authority for public open/close operations in this case.
  ava::tui::SelectorHost& selectors() { return selectors_; }
  // Borrow the catalog input owner whose public contract the cases exercise.
  ava::tui::SelectorInputController& input() { return selector_input_; }
  // Borrow the summary owner; it cannot outlive its frontend or attached loop.
  ava::tui::BranchSummaryOperation& summary() { return summary_; }
  // Borrow the real timer registry for one explicit, deterministic due batch.
  ava::tui::TuiEventLoop& loop() { return loop_; }

  // Fixture contents include arbitrary selector text and frontend authority.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Allocate an epoll-compatible descriptor without relying on the terminal's
  // regular input file. Both ends stay open until after loop destruction.
  static std::pair<ava::core::UniqueFd, ava::core::UniqueFd> open_pipe()
  {
    std::array<int, 2> descriptors{-1, -1};
    if (::pipe(descriptors.data()) != 0)
      throw std::system_error(errno, std::generic_category(), "create feature owner event loop pipe");
    return {ava::core::UniqueFd(descriptors[0]), ava::core::UniqueFd(descriptors[1])};
  }

  // Own registered offline backend data until all dependent owners are gone.
  tui_test_support::TuiFrontEndFixture frontend_;
  // Keep both readiness endpoints alive for the TUI-thread loop's lifetime.
  std::pair<ava::core::UniqueFd, ava::core::UniqueFd> descriptors_;
  // Own timer registrations after dependencies attach and before they retire.
  ava::tui::TuiEventLoop loop_;
  // Own the projection; feature owners publish only through named operations.
  ava::tui::RuntimePresentationState presentation_;
  // Supply the renderer's editor dependency for this fixture's full lifetime.
  ava::tui::Composer composer_;
  // Own real rendering and frame timers; destroyed before projection and loop.
  ava::tui::RuntimeRenderer renderer_;
  // Own selector kind and route all changes to the single screen projection.
  ava::tui::SelectorHost selectors_;
  // Borrow the preceding owners only on the test thread, until case teardown.
  ava::tui::SelectorInputController selector_input_;
  // Own summary discovery registration and retire it before all dependencies.
  ava::tui::BranchSummaryOperation summary_;
};

// Supply three stable row identities, including a visible disabled row, without
// installing any backend callback or borrowing text beyond the returned value.
ava::tui::SelectListView selector_view()
{
  ava::tui::SelectListView view;
  view.title = "Feature owner choices";
  view.subtitle = "original subtitle";
  constexpr std::array rows{std::pair{"first", "OWNER-FIRST"}, std::pair{"second", "OWNER-SECOND"}, std::pair{"disabled", "OWNER-DISABLED"}};
  view.items.resize(rows.size());
  for (std::size_t index = 0; index < rows.size(); ++index)
  {
    view.items[index].value = rows[index].first;
    view.items[index].label = rows[index].second;
  }
  view.items.back().enabled = false;
  view.items.back().disabled_reason = "unavailable";
  return view;
}

// Build one owned action row on the test thread; all unused presentation fields
// retain their normal defaults and no callback captures the supplied strings.
ava::tui::SelectListView one_row_view(std::string title, std::string value, std::string label)
{
  ava::tui::SelectListView view;
  view.title = std::move(title);
  view.items.resize(1);
  view.items.front().value = std::move(value);
  view.items.front().label = std::move(label);
  return view;
}

// Locate a visible row in one rendered frame, then return its one-based mouse
// coordinate. This is a bounded text search on the test thread, not event polling.
std::optional<ava::tui::InputEvent> click_on_label(FeatureOwnerFixture& fixture, std::string_view label)
{
  auto const frame = ava::tui::render_composer(fixture.frontend().frontend(), fixture.presentation().screen());
  for (std::size_t row = 0; row < frame.size(); ++row)
  {
    auto const line = strip_sgr(frame[row]);
    auto const column = line.find(label);
    if (column != std::string::npos)
      return ava::tui::InputEvent{
          .key = Key::MouseLeftClick, .mouse_column = visible_columns(std::string_view(line).substr(0, column)) + 1, .mouse_row = row + 1};
  }
  return std::nullopt;
}

// An unavailable backend source must settle synchronously, preserve the user's
// session selector checkpoint, and never arm discovery or claim later input.
void test_branch_summary_prepare_failure_preserves_selector()
{
  FeatureOwnerFixture fixture;
  auto view = selector_view();
  view.selected_item_index = 1;
  view.query = "OWNER";
  fixture.selectors().open(ActiveSelectList::Session, view);
  fixture.presentation().publish_status("before summary");

  expect(fixture.summary().handle_input({.key = Key::Enter}) == ava::tui::BranchSummaryOutcome::Inactive, "inactive summary does not claim confirmation input");
  expect(fixture.summary().cancel(ava::tui::BranchSummaryCancelMode::ExitAfterSettlement) == ava::tui::BranchSummaryOutcome::Inactive,
         "inactive summary does not close the session selector on exit cancellation");
  auto const outcome = fixture.summary().start(view, 1, "missing-parent-source");
  expect(outcome == ava::tui::BranchSummaryOutcome::Settled && !fixture.summary().active(),
         "missing parent source settles preparation without creating an active summary");
  auto const& screen = fixture.presentation().screen();
  expect(screen.status == "parent summary could not be prepared", "summary preparation failure publishes bounded user-facing feedback");
  expect(fixture.selectors().kind() == ActiveSelectList::Session && screen.select_list && screen.select_list->items.size() == 3 &&
             screen.select_list->selected_item_index == 1 && screen.select_list->items[1].value == "second" && screen.select_list->query == "OWNER" &&
             screen.select_list->subtitle == screen.status,
         "failed summary preserves session rows, selected identity, and query while updating the subtitle");
  fixture.loop().service_due(ava::tui::TuiEventLoop::Clock::now() + std::chrono::seconds{1});
  expect(!fixture.summary().take_service_outcome(), "failed preparation leaves no discovery result after a future timer batch");
  expect(!fixture.summary().take_service_outcome() && !fixture.summary().active(), "consuming an absent summary result leaves it inactive");
  expect(fixture.summary().handle_input({.key = Key::Escape}) == ava::tui::BranchSummaryOutcome::Inactive,
         "failed preparation releases subsequent cancellation input to the session");
}

// Protect generation filtering, confirmation and catalog restoration through real discovery callbacks.
void test_branch_summary_discovery_and_settlement()
{
  FeatureOwnerFixture fixture;
  auto prior = selector_view();
  prior.selected_item_index = 1;
  prior.query = "OWNER";
  fixture.selectors().open(ActiveSelectList::Session, prior);
  ava::tui::TuiBranchSummarySnapshot operation{
      .generation = 7, .phase = ava::tui::TuiBranchSummaryPhase::AwaitingConfirmation, .source_label = {}, .model_label = {}};
  std::size_t reads = 0;
  std::size_t confirmations = 0;
  std::size_t refreshes = 0;
  fixture.frontend().hooks().on_branch_summary_prepare = [&](std::string_view source) {
    expect(source == "second", "summary preparation receives the selected source identity");
    return ava::core::Result<ava::tui::TuiBranchSummarySnapshot>{operation};
  };
  fixture.frontend().hooks().branch_summary_snapshot = [&] {
    ++reads;
    return operation;
  };
  fixture.frontend().hooks().on_branch_summary_confirm = [&](std::uint64_t generation) {
    ++confirmations;
    expect(generation == 7, "summary confirmation retains its prepared generation");
    operation.phase = ava::tui::TuiBranchSummaryPhase::Generating;
    return ava::core::Result<bool>{true};
  };
  fixture.frontend().hooks().on_branch_summary_refresh_catalog = [&] {
    ++refreshes;
    auto refreshed = selector_view();
    std::swap(refreshed.items[0], refreshed.items[1]);
    return ava::core::Result<ava::tui::SelectListView>{std::move(refreshed)};
  };

  expect(fixture.summary().start(prior, 1, "second") == ava::tui::BranchSummaryOutcome::Pending && fixture.summary().active() &&
             fixture.selectors().kind() == ActiveSelectList::BranchSummary,
         "prepared summary owns its modal before confirmation");
  expect(fixture.summary().handle_input({.key = Key::Enter}) == ava::tui::BranchSummaryOutcome::Pending && confirmations == 1 && reads == 1,
         "confirmation is followed by a fresh application snapshot without retiring the summary");

  // A terminal result for another generation must not restore this modal's selector.
  auto const now = ava::tui::TuiEventLoop::Clock::now();
  operation.generation = 8;
  operation.phase = ava::tui::TuiBranchSummaryPhase::Succeeded;
  operation.refresh_required = true;
  fixture.loop().service_due(now + std::chrono::seconds{1});
  expect(fixture.summary().take_service_outcome() == ava::tui::BranchSummaryOutcome::Pending && fixture.summary().active() && refreshes == 0,
         "discovery rejects a terminal snapshot from a different summary generation");
  operation.generation = 7;
  fixture.loop().service_due(now + std::chrono::seconds{2});
  auto const& restored = fixture.selectors().view();
  expect(fixture.summary().take_service_outcome() == ava::tui::BranchSummaryOutcome::Settled && !fixture.summary().active() && refreshes == 1 &&
             fixture.selectors().kind() == ActiveSelectList::Session && restored && restored->query == "OWNER" && restored->selected_item_index == 0 &&
             restored->items[restored->selected_item_index].value == "second",
         "settlement refreshes catalog rows and restores the selected identity and query");
  auto const settled_reads = reads;
  fixture.loop().service_due(now + std::chrono::seconds{3});
  expect(!fixture.summary().take_service_outcome() && reads == settled_reads, "settlement retires discovery and its result is consumed only once");
}

// Protect sticky exit intent when cancellation is refused and settlement arrives later.
void test_branch_summary_exit_waits_for_settlement()
{
  FeatureOwnerFixture fixture;
  auto prior = selector_view();
  ava::tui::TuiBranchSummarySnapshot operation{.generation = 11, .phase = ava::tui::TuiBranchSummaryPhase::Generating, .source_label = {}, .model_label = {}};
  fixture.frontend().hooks().on_branch_summary_prepare = [&](std::string_view) { return ava::core::Result<ava::tui::TuiBranchSummarySnapshot>{operation}; };
  fixture.frontend().hooks().branch_summary_snapshot = [&] { return operation; };
  std::size_t cancellations = 0;
  fixture.frontend().hooks().on_branch_summary_cancel = [&](std::uint64_t generation) {
    ++cancellations;
    expect(generation == 11, "summary cancellation retains the exact prepared generation");
    return ava::core::Result<bool>{false};
  };
  expect(fixture.summary().start(prior, 0, "first") == ava::tui::BranchSummaryOutcome::Pending, "start a summary that has not yet settled");
  expect(fixture.summary().cancel(ava::tui::BranchSummaryCancelMode::ExitAfterSettlement) == ava::tui::BranchSummaryOutcome::Pending && cancellations == 1 &&
             fixture.summary().active() && fixture.selectors().kind() == ActiveSelectList::BranchSummary,
         "refused cancellation retains modal ownership and does not permit early exit");

  // Only a terminal snapshot releases the exit gate; a timer cannot invent worker completion.
  operation.phase = ava::tui::TuiBranchSummaryPhase::Canceled;
  auto const now = ava::tui::TuiEventLoop::Clock::now();
  fixture.loop().service_due(now + std::chrono::seconds{1});
  expect(fixture.summary().take_service_outcome() == ava::tui::BranchSummaryOutcome::ExitReady && !fixture.summary().active() && !fixture.selectors().view(),
         "later settlement honors sticky exit intent and closes the restored selector");
  fixture.loop().service_due(now + std::chrono::seconds{2});
  expect(!fixture.summary().take_service_outcome(), "exit settlement retires its discovery timer and result");
}

// Scoped-model keys must classify before generic session shortcuts, preserve
// row/query context, and honor live bindings without mutating the hosted view.
void test_selector_scoped_keys_and_query_classification()
{
  FeatureOwnerFixture fixture;
  auto view = selector_view();
  view.selected_item_index = 1;
  view.query = "OWNER";
  fixture.selectors().open(ActiveSelectList::ScopedModels, view);
  constexpr std::array keys{
      std::pair{Key::CtrlS, SelectListInputAction::ModelsSave},           std::pair{Key::CtrlA, SelectListInputAction::ModelsEnableAll},
      std::pair{Key::CtrlX, SelectListInputAction::ModelsClearAll},       std::pair{Key::CtrlP, SelectListInputAction::ModelsToggleProvider},
      std::pair{Key::AltArrowUp, SelectListInputAction::ModelsReorderUp}, std::pair{Key::AltArrowDown, SelectListInputAction::ModelsReorderDown}};
  for (auto const& [key, action] : keys)
  {
    auto const classified = fixture.input().classify({.event = {.key = key}, .text = {}});
    expect(classified.action == action && classified.selected_item_index == 1 && classified.query == "OWNER",
           "scoped-model shortcut retains its selected row and query");
  }
  auto bindings = fixture.frontend().frontend()->key_bindings();
  for (auto& [action, assigned] : bindings.bindings)
    if (action == ava::tui::TuiAction::ModelsSave)
      assigned = {Key::F9};
  fixture.presentation().apply_local_key_bindings(*fixture.frontend().frontend(), std::move(bindings));
  expect(fixture.input().classify({.event = {.key = Key::F9}, .text = {}}).action == SelectListInputAction::ModelsSave,
         "scoped-model classification uses the current custom save binding");
  fixture.selectors().open(ActiveSelectList::Hotkeys, selector_view());
  auto const filtered = fixture.input().classify({.event = {.key = Key::Character, .text = "SECOND"}, .text = "SECOND"});
  expect(filtered.action == SelectListInputAction::Redraw && filtered.query == "SECOND" && filtered.selected_item_index == 1,
         "ordinary selector text returns the matching underlying row index");
  expect(fixture.selectors().view()->query.empty() && fixture.selectors().view()->selected_item_index == 0,
         "classification leaves query and selection publication to the owner");
}

// The same mouse hit resolves a catalog item but only highlights Settings;
// disabled rows remain non-resolving in either owner and outside clicks do nothing.
void test_selector_mouse_respects_settings_and_disabled_rows()
{
  FeatureOwnerFixture fixture;
  for (auto const kind : {ActiveSelectList::Settings, ActiveSelectList::Model})
  {
    fixture.selectors().open(kind, selector_view());
    auto const enabled_click = click_on_label(fixture, "OWNER-SECOND");
    auto const disabled_click = click_on_label(fixture, "OWNER-DISABLED");
    if (!ava::tests::require(enabled_click.has_value() && disabled_click.has_value(), "selector fixture renders both target rows"))
      return;
    auto const enabled = fixture.input().classify({.event = *enabled_click, .text = {}});
    expect(enabled.selected_item_index == 1 &&
               enabled.action == (kind == ActiveSelectList::Settings ? SelectListInputAction::Redraw : SelectListInputAction::Resolve),
           "mouse hit highlights Settings and resolves ordinary catalog rows");
    auto const disabled = fixture.input().classify({.event = *disabled_click, .text = {}});
    expect(disabled.selected_item_index == 2 && disabled.action == SelectListInputAction::Redraw, "mouse hit on a disabled row never resolves its action");
    expect(
        fixture.input().classify({.event = {.key = Key::MouseLeftClick, .mouse_column = 0, .mouse_row = 0}, .text = {}}).action == SelectListInputAction::None,
        "mouse outside the selector leaves the action unclaimed");
    expect(fixture.selectors().view()->selected_item_index == 0, "mouse classification does not publish selection before owner dispatch");
  }
}

// Catalog routing must leave dedicated Settings/session owners in control,
// return owned command text, and retire overview-only state on cancellation.
void test_selector_routing_and_owned_draft()
{
  FeatureOwnerFixture fixture;
  auto const resolve = ava::tui::SelectListInputResult{.selected_item_index = 1, .query = {}, .action = SelectListInputAction::Resolve};
  auto const cancel = ava::tui::SelectListInputResult{.query = {}, .action = SelectListInputAction::Cancel};
  fixture.presentation().publish_status("unchanged");
  expect(fixture.input().handle(resolve) == ava::tui::SelectorInputOutcome{SelectorInputRequest::Pass}, "unhosted selector input passes through");
  for (auto const kind : {ActiveSelectList::Settings, ActiveSelectList::Session})
  {
    fixture.selectors().open(kind, selector_view());
    expect(fixture.input().handle(resolve) == ava::tui::SelectorInputOutcome{SelectorInputRequest::Pass} && fixture.selectors().kind() == kind &&
               fixture.presentation().screen().status == "unchanged",
           "catalog owner leaves dedicated Settings and session resolution untouched");
  }
  fixture.selectors().open(ActiveSelectList::Hotkeys, selector_view());
  auto const outcome = fixture.input().handle(resolve);
  auto const* draft = std::get_if<std::pair<ava::tui::SelectorInputTextKind, std::string>>(&outcome);
  expect(draft && draft->first == ava::tui::SelectorInputTextKind::DraftCommand && draft->second == "/keybindings set second ",
         "hotkey resolution returns the selected row as an owned draft command");
  expect(fixture.selectors().kind() == ActiveSelectList::None && !fixture.presentation().screen().select_list &&
             fixture.presentation().screen().status == "keybinding edit command drafted",
         "hotkey resolution closes its selector before handing the draft to composition");
  fixture.presentation().initialize_startup_overview(*fixture.frontend().frontend(), {});
  fixture.selectors().open(ActiveSelectList::Overview, selector_view());
  expect(fixture.input().handle(cancel) == ava::tui::SelectorInputOutcome{SelectorInputRequest::Handled} && !fixture.presentation().screen().startup_overview &&
             !fixture.selectors().view() && fixture.presentation().screen().status == "view canceled",
         "overview cancellation clears both modal projection and its popup-only facts");
  expect(draft && draft->second == "/keybindings set second ", "draft outcome remains valid after selector replacement and cancellation");
}

// Real frontend authority commits provider-default reasoning, reports invalid
// model selections, and preserves the canonical model when the action fails.
void test_selector_reasoning_and_model_authority()
{
  FeatureOwnerFixture fixture;
  fixture.frontend().set_reasoning_capabilities(true, {"low", "high"});
  fixture.frontend().set_reasoning("high");
  fixture.selectors().open(ActiveSelectList::Reasoning, one_row_view("Reasoning", "default", "Default"));
  auto const resolve = ava::tui::SelectListInputResult{.query = {}, .action = SelectListInputAction::Resolve};
  expect(fixture.input().handle(resolve) == ava::tui::SelectorInputOutcome{SelectorInputRequest::Handled}, "default reasoning selection is handled");
  expect(fixture.frontend().frontend()->model_selection().reasoning_refresh().value == "unknown" &&
             fixture.presentation().screen().status == "thinking mode set to Default" && !fixture.selectors().view(),
         "default reasoning maps to absent backend selection and closes the selector");
  auto const model_before = fixture.frontend().selected_model_metadata();
  fixture.selectors().open(ActiveSelectList::Model, one_row_view("Models", "malformed", "Malformed"));
  expect(fixture.input().handle(resolve) == ava::tui::SelectorInputOutcome{SelectorInputRequest::Handled},
         "failed model selection does not request a chained reasoning selector");
  expect(fixture.frontend().selected_model_metadata() == model_before && !fixture.selectors().view() &&
             fixture.presentation().screen().status.find("model selection is missing provider/model") != std::string::npos,
         "model failure preserves canonical selection and publishes its actionable backend error");
}

// Invalid scoped rows remain open with their original checkpoint; fork errors
// return a typed reconciliation request instead of publishing an unverified result.
void test_selector_scoped_guard_and_fork_error_handoff()
{
  FeatureOwnerFixture fixture;
  fixture.selectors().open(ActiveSelectList::ScopedModels, selector_view());
  expect(fixture.input().handle({.selected_item_index = 2, .query = {}, .action = SelectListInputAction::Resolve}) ==
             ava::tui::SelectorInputOutcome{SelectorInputRequest::Handled},
         "disabled scoped-model row is handled without invoking backend mutation");
  expect(fixture.selectors().kind() == ActiveSelectList::ScopedModels && fixture.selectors().view()->items.size() == 3 &&
             fixture.selectors().view()->selected_item_index == 0 && fixture.presentation().screen().status == "scoped model cannot be toggled from this row",
         "disabled scoped row preserves the open selector and explains why it cannot toggle");
  auto const identity_before = fixture.frontend().frontend()->read_committed_session_id();
  fixture.selectors().open(ActiveSelectList::ForkUserTurn, one_row_view("Fork", "missing-entry", "Unavailable turn"));
  auto const outcome = fixture.input().handle({.query = {}, .action = SelectListInputAction::Resolve});
  auto const* error = std::get_if<std::pair<ava::tui::SelectorInputTextKind, std::string>>(&outcome);
  expect(error && error->first == ava::tui::SelectorInputTextKind::ReconcileSessionError && !error->second.empty(),
         "fork authority failure returns owned error text for session reconciliation");
  expect(!fixture.selectors().view() && fixture.presentation().screen().status == "forking session…" &&
             fixture.frontend().frontend()->read_committed_session_id() == identity_before,
         "fork failure retires the picker and defers final feedback until composition reconciles identity");
}

} // namespace

// The common runner supplies the initialized process terminal; every case owns
// and retires its frontend, renderer, and loop entirely on this calling thread.
void run_tui_feature_owner_tests()
{
  static_cast<void>(ava::tui::terminal::Context::resizeterm(24, 100));
  test_branch_summary_prepare_failure_preserves_selector();
  test_branch_summary_discovery_and_settlement();
  test_branch_summary_exit_waits_for_settlement();
  test_selector_scoped_keys_and_query_classification();
  test_selector_mouse_respects_settings_and_disabled_rows();
  test_selector_routing_and_owned_draft();
  test_selector_reasoning_and_model_authority();
  test_selector_scoped_guard_and_fork_error_handoff();
}
