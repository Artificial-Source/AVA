// Retains a detached viewport anchor while new transcript output is withheld.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/composer_internal.h"

#include <cstddef>

namespace ava::tui {

// TranscriptView owns this optional TUI-thread value while scrolled away from the
// live tail. The first anchor stays fixed; removing N front items contributes -N
// to the index adjustment used to restore both viewport and selection.
// It owns its anchor and does not retain the layout cache used to capture it.
class DeferredDetachedViewport final
{
 public:
  // Capture the first pre-update viewport and that update's eviction shift.
  DeferredDetachedViewport(detail::TranscriptViewportAnchor anchor, std::ptrdiff_t item_index_shift);
  // TUI thread: add later evictions without replacing the first visible anchor.
  void accumulate_shift(std::ptrdiff_t item_index_shift) noexcept;
  // TUI thread: borrow until this value is reset, normally after layout synchronization.
  [[nodiscard]] detail::TranscriptViewportAnchor const& anchor() const noexcept;
  // TUI thread: use the same total when restoring viewport and selection indices.
  [[nodiscard]] std::ptrdiff_t item_index_shift() const noexcept;
  AVA_DEBUG_PRINT_MEMBERS_ON
 private:
  // Captured once before deferred updates; never overwritten by a later update.
  detail::TranscriptViewportAnchor anchor_;
  // Sum of accepted update evictions, changed only by accumulate_shift on the TUI thread.
  std::ptrdiff_t item_index_shift_;
};

} // namespace ava::tui
