// Keeps staged attachment references paired with their draft badges.
// See docs/engineering/tui-architecture.md for the owner relationships.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/session/attachments.h"

#include <vector>

namespace ava::tui {

class RuntimePresentationState;
struct PendingAttachmentItem;

// Session owns the TUI-thread staging area used by actions and submission.
// References and presentation badges have the same order and lifetime until take
// or clear; session presentation outlives this owner and its borrowing controllers.
// There is no worker or lock. Destruction releases local references; backend
// attachment persistence remains frontend-owned, and Session retires the projection.
class PendingAttachments final
{
 public:
  // TUI thread: borrow a presentation with no staged badges; it outlives this owner.
  explicit PendingAttachments(RuntimePresentationState& presentation);
  // One draft must not duplicate ownership of staged references and their badge pairing.
  PendingAttachments(PendingAttachments const&) = delete;
  // Keep the presentation paired with the references for the entire staging lifetime.
  PendingAttachments& operator=(PendingAttachments const&) = delete;

  // TUI thread: append one backend reference and its badge in the same order. If badge
  // publication throws, remove the new reference so counts remain paired.
  void stage(ava::session::ImageAttachmentRef reference, PendingAttachmentItem badge);
  // TUI thread: transfer all staged references to submission and clear their badges
  // before another draft is painted. The returned vector owns its references.
  [[nodiscard]] std::vector<ava::session::ImageAttachmentRef> take();
  // TUI thread: abandon pending submission references and visible badges together;
  // this does not delete backend attachment objects.
  void clear() noexcept;
  // TUI thread: gate operations such as prompt stash that cannot preserve image attachments.
  [[nodiscard]] bool has_pending() const noexcept { return !references_.empty(); }

  // Attachment references and badges contain user paths and image payloads.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Borrowed from Session; projection holding the corresponding badges; only named publications mutate it.
  RuntimePresentationState& presentation_;
  // TUI-thread ordered submission payload; stage appends, take transfers and clear discards.
  std::vector<ava::session::ImageAttachmentRef> references_;
};

} // namespace ava::tui
