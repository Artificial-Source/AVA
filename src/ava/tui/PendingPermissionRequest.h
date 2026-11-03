#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/permissions/permission.h"
#include "ava/core/result.h"

#include <condition_variable>
#include <mutex>
#include <optional>

namespace ava::tui {

// The waiting worker and the coordinator share this request until resolution
// retires. Its immutable prompt remains valid throughout that shared lifetime.
class PendingPermissionRequest final
{
 public:
  explicit PendingPermissionRequest(ava::permissions::PermissionPrompt prompt);
  PendingPermissionRequest(PendingPermissionRequest const&) = delete;
  PendingPermissionRequest& operator=(PendingPermissionRequest const&) = delete;

  [[nodiscard]] ava::permissions::PermissionPrompt const& prompt() const noexcept;
  // Exactly one resolver worker consumes this request's result.
  [[nodiscard]] ava::core::Result<ava::permissions::PermissionResolutionDecision> await();
  [[nodiscard]] bool begin_resolution();
  void complete(ava::core::Result<ava::permissions::PermissionResolutionDecision> result);
  void interrupt();

  // Permission prompts and decisions contain arbitrary commands and guidance.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  ava::permissions::PermissionPrompt const prompt_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<ava::core::Result<ava::permissions::PermissionResolutionDecision>> result_;
  bool resolving_ = false;
};

}  // namespace ava::tui
