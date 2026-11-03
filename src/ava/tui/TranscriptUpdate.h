#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/composer_internal.h"

namespace ava::tui {

class TranscriptUpdate final
{
 public:
  TranscriptUpdate(bool detached, detail::TranscriptViewportAnchor anchor) : detached_(detached), anchor_(anchor) { }
  [[nodiscard]] bool detached() const noexcept { return detached_; }
  [[nodiscard]] detail::TranscriptViewportAnchor const& anchor() const noexcept { return anchor_; }
  AVA_DEBUG_PRINT_MEMBERS_ON
 private:
  bool detached_;
  detail::TranscriptViewportAnchor anchor_;
};

} // namespace ava::tui
