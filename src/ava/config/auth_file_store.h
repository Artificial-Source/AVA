#pragma once

#include "ava/config/xdg_paths.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ava::config {

struct CandidateRead
{
  std::optional<ava::core::NoLogString> content;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<CandidateRead> read_text_if_exists(std::filesystem::path const& path, bool explicit_ava_auth_file,
                                                                   bool allow_broad_permissions = false);
[[nodiscard]] ava::core::VoidResult store_provider_object(XdgPaths const& paths, std::string_view provider_id, std::string raw_object);

}  // namespace ava::config
