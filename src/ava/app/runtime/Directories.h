#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/frontend/UnlockedData.h"

#include <filesystem>
#include <vector>

namespace ava::app::runtime {

struct Directories
{
  std::filesystem::path workspace_dir;
  std::filesystem::path current_dir;
  std::vector<std::filesystem::path> additional_writable_dirs = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using Directories_ts = frontend::UnlockedData<Directories>;

} // namespace ava::app::runtime
