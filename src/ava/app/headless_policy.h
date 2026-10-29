#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/permissions/permission.h"
#include "ava/core/result.h"

#include <string>
#include <string_view>
#include <vector>

namespace ava::app {

struct HeadlessPermissionPolicyOptions
{
  bool allow_read_only = false;
  std::vector<std::string> allowed_tools;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::VoidResult add_headless_allow_policy(HeadlessPermissionPolicyOptions& options, std::string_view value);
[[nodiscard]] ava::core::VoidResult add_headless_allowed_tools(HeadlessPermissionPolicyOptions& options, std::string_view value);
[[nodiscard]] ava::permissions::PermissionResolver build_headless_permission_resolver(HeadlessPermissionPolicyOptions options);

}  // namespace ava::app
