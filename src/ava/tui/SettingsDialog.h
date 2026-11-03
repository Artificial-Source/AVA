// Keeps Settings navigation and reversible display previews in one TUI owner.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "runtime_views_internal.h"
#include "ava/debug/print_members_on.h"

#include <variant>

namespace ava::tui {

class TuiFrontEnd;
struct SelectListInputResult;
class RuntimePresentationState;
class SelectorHost;
namespace terminal {
class Context;
}

// TUI-thread request for Session coordination. Cross-feature choices retire Settings
// preview before returning; None leaves the dialog in control of its local action.
enum class SettingsDialogRequest
{
  // The dialog handled the action in place or supplied its own status.
  None,
  // Session opens the model selector after this dialog closes.
  Models,
  // Session opens model-scope editing after this dialog closes.
  ScopedModels,
  // Session opens thinking choices after this dialog closes.
  Reasoning,
  // Session opens keybinding choices after this dialog closes.
  Hotkeys
};

// Value owned first by Settings and then by Session on the TUI thread. It survives
// selector closure and carries editor text with its feedback, so Session never
// infers behavior from command text. It owns both strings and has no teardown work.
struct DraftCommand
{
  // Owned draft text transferred to Composer; no view into a retired selector survives.
  std::string command;
  // Feedback chosen by the feature that requested the draft.
  std::string status;
  // Commands may contain user content, which must stay out of generated logging.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Session receives either a typed feature request or owned editor text with feedback;
// it never derives a requested action by comparing command strings.
using SettingsDialogResult = std::variant<SettingsDialogRequest, DraftCommand>;

// Session owns Settings navigation and its reversible display preview. Every member
// is TUI-thread-only; SelectorHost dispatch uses this owner before painting.
// The borrowed frontend, presentation, host and terminal outlive destruction, which
// restores the preview baseline and clears the theme overlay. Callers retire or
// synchronize this owner when replacing its view; no worker or UI mutex participates.
class SettingsDialog final
{
 public:
  // TUI thread: start inactive; capture a preview baseline only when Settings opens.
  // All collaborators outlive this dialog, including destructor rollback.
  SettingsDialog(TuiFrontEnd& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, terminal::Context& terminal_context);
  // TUI thread: restore the captured display baseline and remove the theme overlay
  // while presentation and terminal-facing theme state are still alive.
  ~SettingsDialog();
  // Copying would create two owners of one temporary theme/display overlay.
  SettingsDialog(SettingsDialog const&) = delete;
  // Keep preview rollback responsibility attached to one stable dialog.
  SettingsDialog& operator=(SettingsDialog const&) = delete;

  // TUI thread: retire any old preview, refresh popup trust facts and build root Settings.
  void open();
  // TUI thread: close a hosted Settings view and restore its preview baseline; another
  // selector remains open if it has already replaced Settings.
  void close();
  // TUI thread: reconcile local lifetime with the host before dispatch; a replaced view
  // retires Settings preview, while an externally opened Settings view gets a baseline.
  void synchronize_selector();
  // TUI thread: publish the selected row/query before deriving its temporary display overlay.
  // A highlight never persists a setting.
  void highlight(std::size_t index, std::string query);
  // TUI thread: Escape in a section restores baseline and root query/selection.
  // Escape at root closes; the caller publishes the resulting frame afterward.
  void cancel();
  // TUI thread: confirm a current Settings row through frontend authority or return a
  // typed request/draft with its status. Preview is cleared before persistent changes.
  [[nodiscard]] SettingsDialogResult resolve(SelectListInputResult const& input);
  // TUI thread: call after an Applied display reload, before final paint. Capture the
  // new authoritative baseline, then reapply preview and reselect by action identity.
  void rebase_after_display_reload();
  // TUI thread: report local preview/navigation lifetime; synchronize after host replacement.
  [[nodiscard]] bool is_open() const noexcept;

  // References print as type/address; navigation query text uses NoLogString.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // TUI thread: restore preview and discard navigation once; safe after repeated close calls.
  void retire();
  // TUI thread: cancel the overlay and republish its authoritative baseline before callbacks.
  void clear_preview();
  // TUI thread: capture authoritative image/cursor values after old preview has been cleared
  // or new settings hydrated; later cancellation restores this baseline.
  void begin_baseline();
  // TUI thread: start navigation/baseline only once, so row rebuilds do not adopt preview as truth.
  void ensure_open();
  // TUI thread: build candidate labels from authoritative values, restore theme preview
  // and publish the new Settings view; selection is clamped to the rebuilt rows.
  void rebuild(SettingsSection section, std::string query = {}, std::optional<std::size_t> selected = std::nullopt);
  // TUI thread: derive a reversible overlay only for a valid Display row; invalid or
  // non-display highlights cancel the prior overlay before publishing.
  void preview_highlight();

  // Borrowed from Session; boundary that persists confirmed settings and supplies current catalogs.
  TuiFrontEnd& frontend_;
  // Borrowed from Session; projection; baseline capture reads it and preview publications update it.
  RuntimePresentationState& presentation_state_;
  // Borrowed from Session; sole selector view; highlight reads rows only until the next host mutation.
  SelectorHost& selectors_;
  // Terminal outlives this dialog; TUI-thread callback failures emit a bell here.
  terminal::Context& terminal_context_;
  // TUI-thread root return frame, section and preview baseline/overlay; reset on retirement.
  runtime_views::SettingsNavigationState navigation_;
  // TUI-thread guard for the navigation/preview lifetime, independent of a host replacement.
  bool open_ = false;
};

} // namespace ava::tui
