#pragma once

#include "ava/config/model_config.h"
#include "ava/config/xdg_paths.h"
#include "ava/core/NoLogString.h"
#include "ava/core/mode.h"
#include "ava/core/result.h"

#include <filesystem>
#include <optional>
#include <string>

namespace ava::config {

struct PromptSelection
{
  ava::core::NoLogString text;
  bool from_override = false;
  std::optional<std::filesystem::path> source_path = std::nullopt;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::string builtin_prompt(std::string_view provider_id, std::string_view family, ava::core::Mode mode);
[[nodiscard]] ava::core::Result<PromptSelection> select_prompt(XdgPaths const& paths, ModelInfo const& model, ava::core::Mode mode);

}  // namespace ava::config
