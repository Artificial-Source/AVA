// Coordinates selector input around the single view stored for display.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "runtime_state_internal.h"
#include "ava/debug/print_members_on.h"

namespace ava::tui {

class SettingsDialog;
class SessionSelector;
class SelectorInputController;
namespace runtime_input {
struct RuntimeInput;
}

// Call-scoped requests from selector policy to Session's cross-owner coordination.
enum class SelectorDispatchRequest
{
  None, // The owning selector completed the action.
  Models, // Open model selection after Settings has retired.
  ScopedModels, // Open scoped model selection after Settings has retired.
  Reasoning, // Open reasoning choices from Settings.
  CurrentReasoning, // Open reasoning choices for the just-selected model.
  Hotkeys, // Replace Settings with the keybinding selector.
  DraftCompletion, // Replace editor text using completion semantics.
  DraftCommand, // Replace editor text and reset its command-drafting interaction.
  OpenedSession, // Reconcile committed identity before publishing success feedback.
  SessionError, // Reconcile committed identity before publishing error feedback.
  SummarizeParent, // Start a separately owned branch-summary operation.
  RenderFailed, // Painting failed before a synchronous frontend operation.
  Beep, // The feature already published the reason; Session emits the bell.
};

// SelectorHost returns this owned value to Session on the TUI thread. It survives
// selector replacement because no row or string view escapes; its request determines
// which payload fields apply. Destruction releases only the copied handoff data.
struct SelectorDispatchResult
{
  // Session performs only the requested cross-owner transition.
  SelectorDispatchRequest request = SelectorDispatchRequest::None;
  // Draft, committed-session feedback, or selected summary identity, according to request.
  std::string text = {};
  // Feature-selected feedback accompanies DraftCompletion, avoiding command-string tests.
  std::string status = {};
  // A parent summary retains the previous selector to restore it when finished.
  std::optional<SelectListView> previous_view = std::nullopt;
  // Selection identity within previous_view; meaningful only for SummarizeParent.
  std::size_t selected_index = 0;
  // Selector text may contain paths or prompt excerpts and must not be debug streamed.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

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
  // TUI thread: publish selection/query for an open view without copying its row vector.
  void update_selection(std::size_t index, std::string query);
  // TUI thread: set the open selector's subtitle; closed views ignore this feedback.
  void set_subtitle(std::string subtitle);
  // TUI thread only. Keep unchanged search rows, replace the changed suffix, and
  // update selection, query and subtitle together. No borrowed row survives this mutation.
  void replace_rows(std::size_t first_changed, std::vector<SelectListItemView> rows, std::size_t selected, std::string query, std::string subtitle);

  // Selector rows may contain stashed prompts or transcript excerpts; keep their content
  // out of debug output. The host itself retains only a presentation reference and kind.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Session-owned display storage; outlives this host, which changes it through presentation methods.
  RuntimePresentationState& presentation_;
  // TUI-thread discriminator changed with open/close; None accompanies an absent view.
  ActiveSelectList kind_ = ActiveSelectList::None;
};

} // namespace ava::tui
