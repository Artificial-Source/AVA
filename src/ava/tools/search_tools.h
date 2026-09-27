#pragma once

#include "ava/tools/tool_context.h"
#include "ava/core/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ava::tools {

struct GlobOptions
{
  std::size_t max_results = 2000;
  std::size_t max_visited = 100000;
  std::size_t max_depth = 32;
  bool no_ignore = false;
  bool skip_symlinks = true;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct GrepOptions
{
  std::size_t max_matches = 2000;
  std::size_t max_line_length = 500;
  bool no_ignore = false;
  bool literal = true;
  bool case_insensitive = false;
  bool skip_symlinks = true;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ListDirectoryOptions
{
  std::size_t max_entries = 500;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct GlobResult
{
  std::vector<std::filesystem::path> paths;
  bool truncated = false;
  bool spill_truncated = false;
  std::size_t total_matches = 0;
  std::filesystem::path spill_path;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct GrepMatch
{
  std::filesystem::path path;
  std::size_t line_number = 0;
  std::string line;
  bool line_truncated = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct GrepResult
{
  std::vector<GrepMatch> matches;
  bool truncated = false;
  bool spill_truncated = false;
  std::size_t total_matches = 0;
  std::filesystem::path spill_path;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct DirectoryEntry
{
  std::string name;
  bool directory = false;
  std::uintmax_t size = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ListDirectoryResult
{
  std::filesystem::path path;
  std::vector<DirectoryEntry> entries;
  bool truncated = false;
  std::size_t total_entries = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<GlobResult> glob_files(ToolContext const& context, std::string_view pattern, GlobOptions options = {});
[[nodiscard]] ava::core::Result<GrepResult> grep_files(ToolContext const& context, std::string_view literal_pattern, std::string_view include_glob = "**/*",
                                                       GrepOptions options = {});
[[nodiscard]] ava::core::Result<ListDirectoryResult> list_directory(ToolContext const& context, std::filesystem::path const& path,
                                                                    ListDirectoryOptions options = {});

}  // namespace ava::tools
