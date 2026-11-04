// Owns a Settings-selected command and feedback after its selector closes.
// See docs/engineering/tui-architecture.md for the selector-to-editor handoff.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

namespace ava::tui {

// Value owned first by Settings and then by Session on the TUI thread. It survives
// selector closure and carries editor text with its feedback, so Session never
// infers behavior from command text. It owns both strings and has no teardown work.
struct DraftCommand
{
  // Owned draft text transferred to Composer; no view into a retired selector survives.
  ava::core::NoLogString command;
  // Feedback chosen by the feature that requested the draft.
  ava::core::NoLogString status;
  // Both strings are annotated; generated output omits them even when populated.
  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui
