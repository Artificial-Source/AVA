#pragma once

#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ava::context {

enum class ContextSourceType
{
  Workspace,
  Global,
  Plugin,
};

struct LoadedContextFile
{
  std::filesystem::path path;
  ContextSourceType source_type = ContextSourceType::Workspace;
  std::size_t byte_count = 0;
  ava::core::NoLogString content;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ContextLoadOptions
{
  std::filesystem::path workspace_root;
  std::filesystem::path current_dir;
  std::filesystem::path global_agents_file;
  std::size_t max_file_bytes = 256 * 1024;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] std::string to_string(ContextSourceType source_type);

[[nodiscard]] ava::core::Result<std::vector<LoadedContextFile>> load_context_files(ContextLoadOptions const& options);

[[nodiscard]] std::string format_context_for_prompt(std::vector<LoadedContextFile> const& files);

}  // namespace ava::context
