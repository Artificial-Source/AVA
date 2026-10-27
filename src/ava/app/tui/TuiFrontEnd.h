#pragma once

#include "ava/tui/TuiFrontEnd.h"

namespace ava::app {

class TuiFrontEnd final : public ava::tui::TuiFrontEnd
{
 private:
  app::runtime::session_ts& unlocked_session_;

 public:
  TuiFrontEnd(app::runtime::session_ts& unlocked_session, tui::TuiRuntimeOptions&& options) :
    ava::tui::TuiFrontEnd(std::move(options)), unlocked_session_(unlocked_session) { }

  // Implement application integration; interaction with the backend and other AVA-specific state.

  std::string read_committed_session_id() override;

  AVA_DEBUG_PRINT_MEMBERS_ON_BASE(ava::tui::TuiFrontEnd)
};

} // namespace ava::app
