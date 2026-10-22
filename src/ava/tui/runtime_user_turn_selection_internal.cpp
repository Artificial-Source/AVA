#include "sys.h"
#include "ava/tui/runtime_user_turn_selection_internal.h"

#include <utility>

namespace ava::tui {
namespace {

std::string collapse_status_newlines(std::string text)
{
  if (auto const newline = text.find('\n'); newline != std::string::npos)
    text.erase(newline);
  return text;
}

}  // namespace

std::string first_inline_status_line(std::string text)
{
  return collapse_status_newlines(std::move(text));
}

std::string attach_soft_status_warning(std::string status, std::string_view warning_prefix, std::string_view warning)
{
  auto detail = first_inline_status_line(std::string(warning));
  if (detail.empty())
    return status;
  std::string attached = std::move(status);
  if (!attached.empty())
    attached += " · ";
  attached += warning_prefix;
  attached += detail;
  return attached;
}

UserTurnCopySelectionDecision evaluate_copy_user_turn_selection(
    std::string_view selected_entry_id, std::function<ava::core::Result<std::string>(std::string_view entry_id)> const& on_read_user_turn_text,
    std::function<bool(std::string_view text)> const& copy_to_clipboard)
{
  if (selected_entry_id.empty())
  {
    return UserTurnCopySelectionDecision{
        .action = UserTurnCopySelectionAction::MissingSelection, .status = "no user turn selected", .beep = true, .transcript_label = "error"};
  }
  if (!on_read_user_turn_text)
  {
    return UserTurnCopySelectionDecision{
        .action = UserTurnCopySelectionAction::ReadFailure, .status = "copy user-turn read unavailable", .beep = true, .transcript_label = "error"};
  }

  auto text = on_read_user_turn_text(selected_entry_id);
  if (!text)
  {
    return UserTurnCopySelectionDecision{
        .action = UserTurnCopySelectionAction::ReadFailure, .status = text.error().format(), .beep = true, .transcript_label = "error"};
  }
  if (!copy_to_clipboard || !copy_to_clipboard(*text))
  {
    // Truthful failure covers empty payloads and the 64 KiB OSC 52 bound.
    return UserTurnCopySelectionDecision{
        .action = UserTurnCopySelectionAction::ClipboardFailure, .status = "clipboard copy failed", .beep = true, .transcript_label = "error"};
  }
  return UserTurnCopySelectionDecision{
      .action = UserTurnCopySelectionAction::RequestSent, .status = "user turn copy request sent", .beep = false, .transcript_label = "status"};
}

}  // namespace ava::tui
