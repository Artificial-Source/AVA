// Protect narrow feature actions and the presentation updates using their own frontend.
#include "sys.h"
#include "tests/support/test_harness.h"
#include "tests/support/tui_test_support.h"
#include "ava/tui/BranchSummaryActions.h"
#include "ava/tui/SelectorInputActions.h"
#include "ava/tui/SessionSelectorActions.h"
#include "ava/tui/SettingsDialogActions.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/TuiSessionActions.h"
#include "ava/tui/runtime_state_internal.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using ava::tui::BranchSummaryActions;
using ava::tui::RuntimePresentationState;
using ava::tui::SelectorInputActions;
using ava::tui::SessionSelectorActions;
using ava::tui::SettingsDialogActions;
using ava::tui::TuiFrontEnd;
using ava::tui::TuiKeyBindings;
using ava::tui::TuiSessionActions;

// Each expansion checks one name independently: one inaccessible action must not
// conceal a different action accidentally left public on the broad frontend.
#define AVA_EXPECT_FEATURE_ACTION(Interface, Method)                                                  \
  template <class T>                                                                                  \
  concept HasPublic_##Method = requires { &T::Method; };                                              \
  static_assert(HasPublic_##Method<Interface>, #Method " must remain available through " #Interface); \
  static_assert(std::is_member_function_pointer_v<decltype(&Interface::Method)>);                     \
  static_assert(!HasPublic_##Method<TuiFrontEnd>, #Method " must not be public through TuiFrontEnd")

AVA_EXPECT_FEATURE_ACTION(TuiSessionActions, mermaid_bridge);
AVA_EXPECT_FEATURE_ACTION(TuiSessionActions, on_toggle_mode);

AVA_EXPECT_FEATURE_ACTION(SettingsDialogActions, on_settings_selected);

AVA_EXPECT_FEATURE_ACTION(SessionSelectorActions, on_session_selector_sort_cycle);
AVA_EXPECT_FEATURE_ACTION(SessionSelectorActions, on_session_selector_named_filter_toggle);
AVA_EXPECT_FEATURE_ACTION(SessionSelectorActions, on_session_selector_path_display_toggle);
AVA_EXPECT_FEATURE_ACTION(SessionSelectorActions, on_session_selector_archived_filter_toggle);
AVA_EXPECT_FEATURE_ACTION(SessionSelectorActions, on_session_selector_label_timestamp_toggle);
AVA_EXPECT_FEATURE_ACTION(SessionSelectorActions, on_session_selector_archive);
AVA_EXPECT_FEATURE_ACTION(SessionSelectorActions, on_session_selector_unarchive);
AVA_EXPECT_FEATURE_ACTION(SessionSelectorActions, on_session_selector_branch_parent);
AVA_EXPECT_FEATURE_ACTION(SessionSelectorActions, on_session_selector_branch_child);
AVA_EXPECT_FEATURE_ACTION(SessionSelectorActions, on_session_selected);

AVA_EXPECT_FEATURE_ACTION(BranchSummaryActions, on_branch_summary_prepare);
AVA_EXPECT_FEATURE_ACTION(BranchSummaryActions, branch_summary_snapshot);
AVA_EXPECT_FEATURE_ACTION(BranchSummaryActions, on_branch_summary_confirm);
AVA_EXPECT_FEATURE_ACTION(BranchSummaryActions, on_branch_summary_cancel);
AVA_EXPECT_FEATURE_ACTION(BranchSummaryActions, on_branch_summary_refresh_catalog);

AVA_EXPECT_FEATURE_ACTION(SelectorInputActions, on_model_selected);
AVA_EXPECT_FEATURE_ACTION(SelectorInputActions, on_reasoning_selected);
AVA_EXPECT_FEATURE_ACTION(SelectorInputActions, on_fork_user_turn_selected);
AVA_EXPECT_FEATURE_ACTION(SelectorInputActions, on_read_user_turn_text);
AVA_EXPECT_FEATURE_ACTION(SelectorInputActions, on_scoped_model_toggled);
AVA_EXPECT_FEATURE_ACTION(SelectorInputActions, on_scoped_model_enable_all);
AVA_EXPECT_FEATURE_ACTION(SelectorInputActions, on_scoped_model_clear_all);
AVA_EXPECT_FEATURE_ACTION(SelectorInputActions, on_scoped_model_toggle_provider);
AVA_EXPECT_FEATURE_ACTION(SelectorInputActions, on_scoped_model_reorder);
AVA_EXPECT_FEATURE_ACTION(SelectorInputActions, on_scoped_model_save);

// The new owning read is also available only through the Settings interface.
AVA_EXPECT_FEATURE_ACTION(SettingsDialogActions, read_settings_model_labels);

#undef AVA_EXPECT_FEATURE_ACTION

static_assert(std::derived_from<TuiFrontEnd, TuiSessionActions>);
static_assert(std::derived_from<TuiFrontEnd, SettingsDialogActions>);
static_assert(std::derived_from<TuiFrontEnd, SessionSelectorActions>);
static_assert(std::derived_from<TuiFrontEnd, BranchSummaryActions>);
static_assert(std::derived_from<TuiFrontEnd, SelectorInputActions>);

// Both mutable and const owners expose only a const reference to their live map.
template <class T>
concept HasReadOnlyKeyBindings = requires(T& owner, T const& const_owner) {
  { owner.key_bindings() } -> std::same_as<TuiKeyBindings const&>;
  { const_owner.key_bindings() } -> std::same_as<TuiKeyBindings const&>;
};

static_assert(HasReadOnlyKeyBindings<TuiFrontEnd>);
static_assert(HasReadOnlyKeyBindings<TuiSessionActions>);
static_assert(HasReadOnlyKeyBindings<SettingsDialogActions>);
static_assert(HasReadOnlyKeyBindings<BranchSummaryActions>);
static_assert(HasReadOnlyKeyBindings<SelectorInputActions>);

// A caller must apply a value through the owner instead of assigning through a borrow.
template <class T>
concept CanAssignBorrowedKeyBindings = requires(T& owner, TuiKeyBindings bindings) { owner.key_bindings() = std::move(bindings); };

static_assert(!CanAssignBorrowedKeyBindings<TuiFrontEnd>);
static_assert(std::same_as<decltype(&TuiFrontEnd::apply_key_bindings), void (TuiFrontEnd::*)(TuiKeyBindings)>);
static_assert(std::same_as<decltype(&SettingsDialogActions::read_settings_model_labels), std::array<std::string, 3> (SettingsDialogActions::*)()>);

// Presentation updates use their retained frontend, so a feature cannot supply
// a different backend owner while updating this presentation's hints or settings.
static_assert(std::same_as<decltype(&RuntimePresentationState::refresh_after_backend_action), void (RuntimePresentationState::*)()>);
static_assert(std::same_as<decltype(&RuntimePresentationState::apply_local_key_bindings), void (RuntimePresentationState::*)(TuiKeyBindings)>);
static_assert(
    std::same_as<decltype(&RuntimePresentationState::apply_local_display_settings), void (RuntimePresentationState::*)(ava::tui::TuiLocalDisplaySettings)>);

// Every feature borrow must keep observing the same map after replacement, and
// the presentation boundary must update active-run, palette and overview hints.
void test_binding_application_preserves_borrows_and_updates_hints()
{
  using ava::tui::TuiAction;
  using ava::tui::terminal::Key;

  ava::tui::TuiRuntimeOptions options;
  options.slash_commands = {ava::tui::SlashCommandItem{.command = "/mode", .description = "Mode"},
                            ava::tui::SlashCommandItem{.command = "/hotkeys", .description = "Keys"}};
  tui_test_support::TuiFrontEndFixture fixture(options);
  auto& frontend = *fixture.frontend();
  RuntimePresentationState presentation(&frontend, {});
  presentation.initialize_startup_overview(frontend, ava::tui::StartupOverviewSnapshot{.compact_line = "build · provider/model · /overview"});

  auto const* original_bindings = &std::as_const(frontend).key_bindings();
  std::array const feature_borrows{&static_cast<TuiSessionActions&>(frontend).key_bindings(), &static_cast<SettingsDialogActions&>(frontend).key_bindings(),
                                   &static_cast<BranchSummaryActions&>(frontend).key_bindings(), &static_cast<SelectorInputActions&>(frontend).key_bindings()};
  expect(std::ranges::all_of(feature_borrows, [&](auto const* bindings) { return bindings == original_bindings; }),
         "feature interfaces initially borrow the frontend's single keybinding object");

  TuiKeyBindings replacement;
  replacement.bindings = {
      {TuiAction::Cancel, {Key::F9}}, {TuiAction::JumpToBottom, {Key::F8}}, {TuiAction::ModeToggle, {Key::F7}}, {TuiAction::OverviewToggle, {Key::F6}}};
  presentation.apply_local_key_bindings(std::move(replacement));

  expect(&frontend.key_bindings() == original_bindings &&
             std::ranges::all_of(feature_borrows,
                                 [&](auto const* bindings) {
                                   return bindings == &frontend.key_bindings() && ava::tui::key_matches_action(*bindings, TuiAction::ModeToggle, Key::F7) &&
                                          ava::tui::key_matches_action(*bindings, TuiAction::Cancel, Key::F9);
                                 }),
         "applying bindings preserves every borrowed object address and exposes the replacement map");

  auto const& screen = presentation.screen();
  expect(screen.active_run_hint.interrupt == "F9" && screen.active_run_hint.jump_to_bottom == "F8",
         "binding application through presentation updates active-run stop and jump hints");
  auto const mode = std::ranges::find_if(screen.slash_commands, [](auto const& item) { return item.command == "/mode"; });
  expect(mode != screen.slash_commands.end() && mode->key_display == "F7", "binding application through presentation updates the mode palette hint");
  auto const hotkeys = std::ranges::find_if(screen.slash_commands, [](auto const& item) { return item.command == "/hotkeys"; });
  expect(hotkeys != screen.slash_commands.end() && std::ranges::any_of(hotkeys->argument_completions,
                                                                       [](auto const& completion) {
                                                                         return completion.category == "Keybindings" && completion.value == "mode_toggle" &&
                                                                                completion.description.starts_with("F7");
                                                                       }),
         "binding application through presentation updates keybinding edit completions");
  expect(screen.startup_overview && screen.startup_overview->overview_toggle_keys == "F6" && screen.startup_overview->compact_line.ends_with("F6 · /overview"),
         "binding application through presentation updates the retained overview open hint");
}

// Non-model sections must leave model notifications for their actual consumer;
// the Model section copies all three labels and consumes their flags in place.
void test_settings_read_labels_only_for_model_section()
{
  using ava::tui::SettingsSection;

  tui_test_support::TuiFrontEndFixture fixture;
  auto& frontend = *fixture.frontend();
  SettingsDialogActions& settings = frontend;
  auto& selection = frontend.model_selection();
  static_cast<void>(selection.provider_refresh());
  static_cast<void>(selection.model_refresh());
  static_cast<void>(selection.reasoning_refresh());

  fixture.set_model({.provider_id = "r14-provider-before",
                     .model_id = "r14-model-before",
                     .display_name = "R14 model before",
                     .supports_reasoning = true,
                     .reasoning_levels = std::vector<std::string>{"low", "high"}});
  fixture.set_reasoning("high");
  ava::tui::ComposerSnapshot const snapshot;
  auto const& bindings = settings.key_bindings();
  for (auto const section : {SettingsSection::Root, SettingsSection::Theme, SettingsSection::Display, SettingsSection::InputAndKeybindings,
                             SettingsSection::SessionsAndWorkspace, SettingsSection::ToolsAndExtensions})
  {
    auto const view = ava::tui::settings_select_list_view_for_section(&settings, section, snapshot, bindings);
    auto const without_frontend = ava::tui::settings_select_list_view_for_section(nullptr, section, snapshot, bindings);
    expect(!view.items.empty() && view.items.size() == without_frontend.items.size() && view.title == without_frontend.title,
           "non-model settings sections remain usable without frontend reads");
  }
  auto const pending_provider = selection.provider_refresh();
  auto const pending_model = selection.model_refresh();
  auto const pending_reasoning = selection.reasoning_refresh();
  expect(pending_provider.changed && pending_provider.value == "r14-provider-before" && pending_model.changed && pending_model.value == "R14 model before" &&
             pending_reasoning.changed && pending_reasoning.value == "high",
         "all non-model settings sections preserve pending provider, model and reasoning notifications");

  fixture.set_model({.provider_id = "r14-provider-after",
                     .model_id = "r14-model-after",
                     .display_name = "R14 model after",
                     .supports_reasoning = true,
                     .reasoning_levels = std::vector<std::string>{"low", "high"}});
  fixture.set_reasoning("low");
  auto const view = ava::tui::settings_select_list_view_for_section(&settings, SettingsSection::ModelsAndReasoning, snapshot, bindings);
  expect(view.items.size() == 3 && view.items[0].description == "r14-provider-after/R14 model after" && view.items[1].description == "low",
         "model settings rows retain provider, display-model and reasoning values through the narrow interface");
  auto const consumed_provider = selection.provider_refresh();
  auto const consumed_model = selection.model_refresh();
  auto const consumed_reasoning = selection.reasoning_refresh();
  expect(!consumed_provider.changed && !consumed_model.changed && !consumed_reasoning.changed,
         "building the Model section consumes each of its three display notifications");

  auto const owned_labels = settings.read_settings_model_labels();
  fixture.set_reasoning("high");
  auto const refreshed_labels = settings.read_settings_model_labels();
  expect(owned_labels == std::array<std::string, 3>{"r14-provider-after", "R14 model after", "low"} &&
             refreshed_labels == std::array<std::string, 3>{"r14-provider-after", "R14 model after", "high"},
         "settings labels own provider, model and reasoning strings after later frontend refreshes");

  fixture.set_reasoning(std::nullopt);
  auto const defaults = ava::tui::settings_select_list_view_for_section(&settings, SettingsSection::ModelsAndReasoning, snapshot, bindings);
  expect(defaults.items.size() == 3 && defaults.items[1].description == "default",
         "absent canonical reasoning still renders as the Settings default through the narrow read");
}

} // namespace

// The common composer runner initializes the terminal before these offline
// fixtures are constructed and keeps it alive until they have all retired.
void run_tui_feature_interface_tests()
{
  test_binding_application_preserves_borrows_and_updates_hints();
  test_settings_read_labels_only_for_model_section();
}
