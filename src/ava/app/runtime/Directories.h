#pragma once

#include "ava/app/frontend/UnlockedData.h"
#include "ava/debug/print_members_on.h"

#include <filesystem>
#include <vector>

namespace ava::app::runtime {

struct Directories
{
  std::filesystem::path workspace_dir;
  std::filesystem::path current_dir;
  std::vector<std::filesystem::path> additional_writable_dirs = {};

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using Directories_ts = frontend::UnlockedData<Directories>;

} // namespace ava::app::runtime
