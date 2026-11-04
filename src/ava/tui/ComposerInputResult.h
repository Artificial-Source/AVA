// Carries editor feedback and cross-feature requests to TUI input routing.
// See docs/engineering/tui-architecture.md for dispatch ordering.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <optional>

namespace ava::tui {

// Cross-owner requests let the editor choose precedence without owning transcript
// layout, terminal rendering, or submission lifetime.
enum class ComposerInputRequest
{
  None, // No work remains outside the editor.
  CopyTranscript, // Copy the transcript selection after draft selection lost precedence.
  ClearTranscriptSelection, // Starting a draft selection retires transcript selection.
  ScrollUp, // Empty history falls back to the preceding transcript rows.
  ScrollDown, // Empty history falls back to the following transcript rows.
  Submit, // The session starts or queues a run after editing has declined Enter.
};

// Composer returns this owned result to TUI-thread input routing. The caller updates
// feedback and beeps after editing, then performs the requested action before painting.
// No editor reference escapes; an unhandled result lets routing try its next fallback.
struct ComposerInputResult
{
  // True when this action applies; false lets the caller try its documented fallback.
  bool handled = false;
  // No value preserves existing feedback; an empty value explicitly clears it.
  std::optional<ava::core::NoLogString> status = std::nullopt;
  // The caller's terminal context emits the bell, keeping terminal I/O out of editing.
  bool beep = false;
  // The session performs this request only after applying feedback.
  ComposerInputRequest request = ComposerInputRequest::None;
  // The generator omits annotated feedback and prints only handled/beep/request.
  AVA_DEBUG_PRINT_MEMBERS_ON
};

} // namespace ava::tui
