#include "sys.h"
#include "PendingAttachments.h"
#include "ava/tui/composer.h"

#include <utility>

namespace ava::tui {

PendingAttachments::PendingAttachments(ComposerSnapshot& screen) : screen_(screen)
{
}

void PendingAttachments::stage(ava::session::ImageAttachmentRef reference, PendingAttachmentItem badge)
{
  references_.push_back(std::move(reference));
  try
  {
    screen_.pending_attachments.push_back(std::move(badge));
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
  screen_.pending_attachments.clear();
  return references;
}

void PendingAttachments::clear() noexcept
{
  references_.clear();
  screen_.pending_attachments.clear();
}

} // namespace ava::tui
