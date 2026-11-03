#include "sys.h"
#include "PendingPermissionRequest.h"

#include <utility>

namespace ava::tui {

PendingPermissionRequest::PendingPermissionRequest(ava::permissions::PermissionPrompt prompt) : prompt_(std::move(prompt))
{
}

ava::permissions::PermissionPrompt const& PendingPermissionRequest::prompt() const noexcept
{
  return prompt_;
}

ava::core::Result<ava::permissions::PermissionResolutionDecision> PendingPermissionRequest::await()
{
  std::unique_lock lock(mutex_);
  ready_.wait(lock, [this]() { return result_.has_value() && !resolving_; });
  return std::move(*result_);
}

bool PendingPermissionRequest::begin_resolution()
{
  std::lock_guard lock(mutex_);
  if (result_ || resolving_)
    return false;
  resolving_ = true;
  return true;
}

void PendingPermissionRequest::complete(ava::core::Result<ava::permissions::PermissionResolutionDecision> result)
{
  {
    std::lock_guard lock(mutex_);
    if (!result_)
      result_ = std::move(result);
    resolving_ = false;
  }
  ready_.notify_one();
}

void PendingPermissionRequest::interrupt()
{
  {
    std::lock_guard lock(mutex_);
    if (!result_)
      result_ = ava::permissions::PermissionResolution::Deny;
  }
  ready_.notify_one();
}

}  // namespace ava::tui
