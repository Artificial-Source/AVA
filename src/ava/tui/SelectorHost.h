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
  RenderFailed, // A synchronous selector authority handoff could not paint.
  Beep, // The feature already published the reason; Session emits the bell.
};

// Owned handoff values survive selector replacement; no row or string view escapes.
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

// Owns selector changes on the TUI thread. The session snapshot supplies the
// single view storage and outlives this host; readers borrow that view as const.
// No second row vector is copied into the renderer when selection changes.
class SelectorHost final
{
 public:
  explicit SelectorHost(RuntimePresentationState& presentation);
  SelectorHost(SelectorHost const&) = delete;
  SelectorHost& operator=(SelectorHost const&) = delete;

  // TUI thread: synchronize feature policy, classify one input, and route it to
  // the active owner. Collaborators are borrowed only for this call. Session acts
  // on the owned result after any old selector/preview has been retired.
  [[nodiscard]] SelectorDispatchResult dispatch(runtime_input::RuntimeInput const& input, SelectorInputController& classifier, SettingsDialog& settings,
                                                SessionSelector& sessions);

  [[nodiscard]] ActiveSelectList kind() const noexcept;
  [[nodiscard]] std::optional<SelectListView> const& view() const noexcept;
  // Borrowed items expire at the next selector mutation.
  [[nodiscard]] SelectListItemView const* selected_item(std::size_t index) const noexcept;
  [[nodiscard]] std::vector<std::string> visible_values(std::string const& query) const;

  void open(ActiveSelectList kind, SelectListView view);
  void replace(SelectListView view);
  void replace_preserving_selection(SelectListView view, std::size_t selected_index, std::string query);
  void close();
  void update_selection(std::size_t index, std::string query);
  void set_subtitle(std::string subtitle);
  // TUI thread only. Keep unchanged search rows, replace the changed suffix, and
  // publish selection/chrome together. No borrowed row survives this mutation.
  void replace_rows(std::size_t first_changed, std::vector<SelectListItemView> rows, std::size_t selected, std::string query, std::string subtitle);

  // Selector rows may contain stashed prompts or transcript excerpts, and the
  // borrowed snapshot contains arbitrary messages. Neither aggregate has a safe
  // generated content printer; keep these contents out of debug output.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  RuntimePresentationState& presentation_;
  ActiveSelectList kind_ = ActiveSelectList::None;
};

} // namespace ava::tui
