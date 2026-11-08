// Coordinates selector input around the single view stored for display.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "SelectorDispatchResult.h"
#include "runtime_state_internal.h"
#include "ava/debug/print_members_on.h"
#include "ava/core/result.h"

#include <functional>
#include <string>

namespace ava::tui {

class SettingsDialog;
class SessionSelector;
class SelectorInputController;
namespace runtime_input {
struct RuntimeInput;
}

// Session owns this TUI-thread coordinator of selector input and view changes.
// Presentation outlives it and holds the single view; renderer and features borrow
// that view as const on the TUI thread. Host operations keep kind and view in step.
// Features must undo their temporary previews before replacing or closing a view,
// and controllers borrowing the host must be destroyed before it. Workers never use it.
class SelectorHost final
{
 public:
  // TUI thread: borrow presentation, which must start without a selector and outlive this host.
  explicit SelectorHost(RuntimePresentationState& presentation);
  // Not copyable: one stable host keeps the selector kind paired with its displayed view.
  SelectorHost(SelectorHost const&) = delete;
  SelectorHost& operator=(SelectorHost const&) = delete;

  // TUI thread: synchronize feature policy, classify one input, and route it to
  // the active owner. Collaborators are borrowed only for this call. Session acts
  // on the owned result after any old selector/preview has been retired.
  [[nodiscard]] SelectorDispatchResult dispatch(runtime_input::RuntimeInput const& input, SelectorInputController& classifier, SettingsDialog& settings,
                                                SessionSelector& sessions);

  // TUI thread: choose the feature policy corresponding to the currently published view.
  [[nodiscard]] ActiveSelectList kind() const noexcept;
  // TUI thread: borrow the displayed view; any host mutation may invalidate its rows.
  [[nodiscard]] std::optional<SelectListView> const& view() const noexcept;
  // TUI thread: borrow a source row by index, or return null for absent/out-of-range
  // rows. The pointer expires at the next selector mutation or host destruction.
  [[nodiscard]] SelectListItemView const* selected_item(std::size_t index) const noexcept;
  // TUI thread: copy enabled identities matching a prospective query for bulk model actions.
  [[nodiscard]] std::vector<std::string> visible_values(std::string const& query) const;

  // TUI thread: replace kind and view within one dispatch; callers must retire
  // any previous feature preview before another owner observes the replacement.
  void open(ActiveSelectList kind, SelectListView view);
  // TUI thread: replace rows for an already open kind; all previous row borrows expire.
  void replace(SelectListView view);
  // TUI thread: retain query and reselect the old row by value, then clamp to valid results.
  void replace_preserving_selection(SelectListView view, std::size_t selected_index, std::string query);
  // TUI thread: retire the published view and kind. Feature-specific preview cleanup
  // belongs to the caller; close alone does not notify a previous feature owner.
  void close();
  // TUI thread: close the selector and show pending_status, then call render and, if that succeeded, callback.
  //
  // render must perform the handoff paint, not merely request a frame, because callback may block on a
  // synchronous model/session action. A failed render is published and returned as an I/O error without
  // calling callback. Otherwise the result of callback is returned and any error is published; the caller
  // applies success. Both callables are borrowed only for this call.
  [[nodiscard]] core::Result<std::string> dispatch_authority(std::string pending_status, std::function<bool()> const& render,
                                                             std::function<core::Result<std::string>()> const& callback);
  // TUI thread: close an open startup overview selector and clear the overview from presentation.
  //
  // Does nothing when another kind of selector (or none) is open; used when a competing view opens.
  void close_startup_overview();
  // TUI thread after local hint/display changes: rebuild the rows of an open startup overview.
  //
  // Preserves the filter query and the selected row (clamped to the new rows). Closes the overview instead
  // when presentation no longer holds overview facts. Does nothing when no overview selector is open.
  void synchronize_startup_overview();
  // TUI thread: publish selection/query for an open view without copying its row vector.
  void update_selection(std::size_t index, std::string query);
  // TUI thread: set the open selector's subtitle; closed views ignore this feedback.
  void set_subtitle(std::string subtitle);
  // TUI thread only. Keep unchanged search rows, replace the changed suffix, and
  // update selection, query and subtitle together. No borrowed row survives this mutation.
  void replace_rows(std::size_t first_changed, std::vector<SelectListItemView> rows, std::size_t selected, std::string query, std::string subtitle);

  // The host prints only its kind and the presentation address. Selector rows stay
  // in presentation and are never traversed through this borrowed reference.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  // Session-owned display storage; outlives this host, which changes it through presentation methods.
  RuntimePresentationState& presentation_;
  // TUI-thread discriminator changed with open/close; None accompanies an absent view.
  ActiveSelectList kind_ = ActiveSelectList::None;
};

} // namespace ava::tui
