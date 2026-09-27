#pragma once

#include "ava/tools/tool_context.h"
#include "ava/core/result.h"

#include <filesystem>

namespace ava::tools::file_tools_internal {

[[nodiscard]] ava::core::VoidResult reject_permission_rules_file_mutation(ToolContext const& context, std::filesystem::path const& path);

}  // namespace ava::tools::file_tools_internal
