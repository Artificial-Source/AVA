#pragma once

#include "runtime_state_internal.h"
#include "ava/debug/print_members_on.h"

namespace ava::tui {

class SelectorHost;
class RuntimeRenderer;
namespace terminal {
class Context;
}

class SessionSelector final
{
 public:
  // Borrowed session collaborators outlive this TUI-thread-owned selector.
  SessionSelector(TuiFrontEnd& frontend, RuntimePresentationState& presentation, SelectorHost& selectors, RuntimeRenderer& renderer,
                  terminal::Context& terminal_context);
  SessionSelector(SessionSelector const&) = delete;
  SessionSelector& operator=(SessionSelector const&) = delete;

  [[nodiscard]] bool open();
  void cancel_confirmation();
  void browse(SelectListInputResult const& input);
  void archive(SelectListInputResult const& input);
  [[nodiscard]] std::optional<std::string> draft_command(SelectListInputResult const& input);
  // Results request composition-level reconciliation; only the frontend's
  // committed identity establishes whether a session transition happened.
  [[nodiscard]] core::Result<std::string> resolve(SelectListInputResult const& input);
  [[nodiscard]] std::optional<core::Result<std::string>> navigate_branch(SelectListInputResult const& input);

  // References print as type/address; the archive identity uses NoLogString.
  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  void preserve_selection(SelectListInputResult const& input, SelectListView view, std::string status);

  TuiFrontEnd& frontend_;
  RuntimePresentationState& presentation_state_;
  SelectorHost& selectors_;
  RuntimeRenderer& renderer_;
  terminal::Context& terminal_context_;
  std::optional<PendingSessionArchiveAction> archive_confirmation_;
};

} // namespace ava::tui
