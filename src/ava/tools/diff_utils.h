#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace ava::tools {

struct DiffPreview
{
  ava::core::NoLogString text;
  bool truncated = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] DiffPreview unified_diff(std::string_view old_content, std::string_view new_content, std::filesystem::path const& old_path,
                                       std::filesystem::path const& new_path, std::size_t max_bytes = 32 * 1024);

}  // namespace ava::tools
