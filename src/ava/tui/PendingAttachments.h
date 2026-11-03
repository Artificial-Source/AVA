#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/session/attachments.h"

#include <vector>

namespace ava::tui {

struct ComposerSnapshot;
struct PendingAttachmentItem;

// TUI-thread ownership of staged attachment references and their visible badges.
// The session-owned screen outlives this owner and every borrowing controller.
class PendingAttachments final
{
 public:
  explicit PendingAttachments(ComposerSnapshot& screen);
  PendingAttachments(PendingAttachments const&) = delete;
  PendingAttachments& operator=(PendingAttachments const&) = delete;

  void stage(ava::session::ImageAttachmentRef reference, PendingAttachmentItem badge);
  [[nodiscard]] std::vector<ava::session::ImageAttachmentRef> take();
  void clear() noexcept;
  [[nodiscard]] bool has_pending() const noexcept { return !references_.empty(); }

  // Attachment references and badges contain user paths and image payloads.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  ComposerSnapshot& screen_;
  std::vector<ava::session::ImageAttachmentRef> references_;
};

} // namespace ava::tui
