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

enum class SettingsDialogRequest
{
  None,
  Models,
  ScopedModels,
  Reasoning,
  Hotkeys
};

// TUI-thread handoff after a selector closes. The command and its display feedback
// travel together so Session never infers behavior by comparing command text.
struct DraftCommand
{
  // Owned draft text transferred to Composer; no view into a retired selector survives.
  std::string command;
  // Feedback chosen by the feature that requested the draft.
  std::string status;
  // Commands may contain user content, which must stay out of generated logging.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Session coordinates selector requests after the dialog retires its preview.
using SettingsDialogResult = std::variant<SettingsDialogRequest, DraftCommand>;

class SettingsDialog final
{
 public:
  // All borrowed collaborators outlive this TUI-thread-owned dialog.
  SettingsDialog(TuiFrontEnd& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, terminal::Context& terminal_context);
  ~SettingsDialog();
  SettingsDialog(SettingsDialog const&) = delete;
  SettingsDialog& operator=(SettingsDialog const&) = delete;

  void open();
  void close();
  void synchronize_selector();
  void highlight(std::size_t index, std::string query);
  void cancel();
  [[nodiscard]] SettingsDialogResult resolve(SelectListInputResult const& input);
  void rebase_after_display_reload();
  [[nodiscard]] bool is_open() const noexcept;

  // References print as type/address; navigation query text uses NoLogString.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  void retire();
  void clear_preview();
  void begin_baseline();
  void ensure_open();
  void rebuild(SettingsSection section, std::string query = {}, std::optional<std::size_t> selected = std::nullopt);
  void preview_highlight();

  TuiFrontEnd& frontend_;
  RuntimePresentationState& presentation_state_;
  SelectorHost& selectors_;
  terminal::Context& terminal_context_;
  runtime_views::SettingsNavigationState navigation_;
  bool open_ = false;
};

} // namespace ava::tui
