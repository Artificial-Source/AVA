// Shares a permission request and decision between a waiting worker and the TUI.
#pragma once

#include "PendingRequest.h"
#include "ava/permissions/permission.h"

namespace ava::tui {

// Worker construction prepares denial before enqueueing; shutdown only moves it.
inline ava::core::Result<ava::permissions::PermissionResolutionDecision> interrupted_permission_result()
{
  return ava::permissions::PermissionResolution::Deny;
}

// Resolver worker and TUI coordinator share this request specialization. Denial
// is prepared before enqueueing; the TUI holds the worker until its borrowed
// callbacks retire, even after cancellation. See PendingRequest.
using PendingPermissionRequest =
    PendingRequest<ava::permissions::PermissionPrompt, ava::permissions::PermissionResolutionDecision, interrupted_permission_result>;

} // namespace ava::tui
