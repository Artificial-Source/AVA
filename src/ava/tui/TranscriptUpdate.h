// Captures the viewport decision before a transcript mutation so completion preserves the reader's position.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/composer_internal.h"

namespace ava::tui {

// Value returned by RuntimeRenderer before mutation and consumed by its completion operation on the TUI thread.
// It owns the anchor, has no callbacks or borrowed state, and remains unchanged while transcript/search
// projections are updated.
class TranscriptUpdate final
{
 public:
  // TUI thread: record the pre-mutation detached decision; anchor is meaningful when the first detached update
  // captures a layout.
  TranscriptUpdate(bool detached, detail::TranscriptViewportAnchor anchor) : detached_(detached), anchor_(anchor) { }
  // TUI thread: preserve whether this mutation began away from live output even if later projection work
  // changes chrome.
  [[nodiscard]] bool detached() const noexcept { return detached_; }
  // TUI thread: borrow the pre-mutation anchor until this token dies; completion never borrows a layout through
  // it.
  [[nodiscard]] detail::TranscriptViewportAnchor const& anchor() const noexcept { return anchor_; }
  AVA_DEBUG_PRINT_MEMBERS_ON
 private:
  // Set at capture time on the TUI thread; selects detached preservation versus live selection remapping.
  bool detached_;
  // Owned pre-mutation position; the first deferred update retains this value while later updates contribute
  // only shifts.
  detail::TranscriptViewportAnchor anchor_;
};

} // namespace ava::tui
