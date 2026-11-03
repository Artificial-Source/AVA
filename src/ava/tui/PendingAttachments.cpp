#include "sys.h"
#include "PendingAttachments.h"
#include "ava/tui/runtime_state_internal.h"

#include <utility>

namespace ava::tui {

PendingAttachments::PendingAttachments(RuntimePresentationState& presentation) : presentation_(presentation)
{
}

void PendingAttachments::stage(ava::session::ImageAttachmentRef reference, PendingAttachmentItem badge)
{
  references_.push_back(std::move(reference));
  try
  {
    presentation_.publish_attachment(std::move(badge));
  }
  catch (...)
  {
    references_.pop_back();
    throw;
  }
}

std::vector<ava::session::ImageAttachmentRef> PendingAttachments::take()
{
  auto references = std::exchange(references_, {});
  presentation_.clear_attachment_badges();
  return references;
}

void PendingAttachments::clear() noexcept
{
  references_.clear();
  presentation_.clear_attachment_badges();
}

} // namespace ava::tui
