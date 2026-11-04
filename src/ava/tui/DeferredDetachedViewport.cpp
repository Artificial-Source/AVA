// Accumulates item-index adjustments against the first detached anchor until the renderer restores the viewport.
#include "sys.h"
#include "DeferredDetachedViewport.h"

namespace ava::tui {
DeferredDetachedViewport::DeferredDetachedViewport(detail::TranscriptViewportAnchor anchor, std::ptrdiff_t item_index_shift)
    : anchor_(anchor), item_index_shift_(item_index_shift)
{
}
void DeferredDetachedViewport::accumulate_shift(std::ptrdiff_t item_index_shift) noexcept
{
  item_index_shift_ += item_index_shift;
}
detail::TranscriptViewportAnchor const& DeferredDetachedViewport::anchor() const noexcept
{
  return anchor_;
}
std::ptrdiff_t DeferredDetachedViewport::item_index_shift() const noexcept
{
  return item_index_shift_;
}
} // namespace ava::tui
