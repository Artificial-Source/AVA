// Permission specialization of the shared worker-to-TUI prompt rendezvous.
#pragma once

#include "PendingRequest.h"
#include "ava/permissions/permission.h"

namespace ava::tui {

// Worker construction prepares denial before admission; shutdown only moves it.
inline ava::core::Result<ava::permissions::PermissionResolutionDecision> interrupted_permission_result()
{
  return ava::permissions::PermissionResolution::Deny;
}

// Shared by the resolver and coordinator; see PendingRequest for claim lifetime.
using PendingPermissionRequest =
    PendingRequest<ava::permissions::PermissionPrompt, ava::permissions::PermissionResolutionDecision, interrupted_permission_result>;

} // namespace ava::tui
