#pragma once

#include "runtime_views_internal.h"

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

// A string requests drafting that command; selector requests are coordinated by
// the session after the dialog has retired its preview and closed its view.
using SettingsDialogResult = std::variant<SettingsDialogRequest, std::string>;

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

  // Retains the full presentation projection and selector, including arbitrary
  // transcript/prompt content; neither aggregate is safe for generated printing.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

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
