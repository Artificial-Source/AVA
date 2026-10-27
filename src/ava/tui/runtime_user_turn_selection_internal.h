#pragma once

#include "ava/tui/runtime.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include "debug.h"

namespace ava::tui {

enum class UserTurnCopySelectionAction
{
  MissingSelection,
  ReadFailure,
  ClipboardFailure,
  RequestSent,
};

struct UserTurnCopySelectionDecision
{
  UserTurnCopySelectionAction action = UserTurnCopySelectionAction::MissingSelection;
  ava::core::NoLogString status = {};
  bool beep = false;
  // "status" on success, "error" otherwise — matches production transcript labels.
  std::string transcript_label = "error";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Re-reads exact turn text by the selected stable entry id at action time, then
// attempts clipboard copy. Truthful failure covers read errors and OSC/bound
// clipboard rejection. RequestSent means AVA wrote the terminal request; downstream
// delivery remains unknowable.
[[nodiscard]] UserTurnCopySelectionDecision evaluate_copy_user_turn_selection(
    std::string_view selected_entry_id, std::function<ava::core::Result<std::string>(std::string_view entry_id)> const& on_read_user_turn_text,
    std::function<bool(std::string_view text)> const& copy_to_clipboard);

// Single-line status helper for soft post-mutation warnings.
[[nodiscard]] std::string first_inline_status_line(std::string text);
[[nodiscard]] std::string attach_soft_status_warning(std::string status, std::string_view warning_prefix, std::string_view warning);

}  // namespace ava::tui
