#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tools/tool_context.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace ava::tools {

struct TextOutput
{
  std::string content;
  bool truncated = false;
  bool byte_limited = false;
  bool line_limited = false;
  bool totals_known = true;
  std::size_t total_bytes = 0;
  std::size_t output_bytes = 0;
  std::size_t output_lines = 0;
  std::size_t start_line = 1;
  std::size_t end_line = 0;
  std::size_t total_lines = 0;
  std::size_t next_offset_line = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct FileMutationResult
{
  std::filesystem::path path;
  std::size_t bytes_written = 0;
  std::string diff;
  bool diff_truncated = false;
  std::string line_endings;
  bool had_utf8_bom = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ReadOptions
{
  std::size_t max_bytes = 50 * 1024;
  std::size_t offset_line = 1;
  std::size_t max_lines = 200;
  bool permission_already_checked = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct WriteOptions
{
  bool permission_already_checked = false;
  bool mutation_already_locked = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<TextOutput> read_file(ToolContext const& context, std::filesystem::path const& path, ReadOptions options = {});
[[nodiscard]] ava::core::Result<FileMutationResult> write_file(ToolContext const& context, std::filesystem::path const& path, std::string_view content,
                                                               WriteOptions options = {});
[[nodiscard]] ava::core::Result<FileMutationResult> edit_file(ToolContext const& context, std::filesystem::path const& path, std::string_view old_text,
                                                              std::string_view new_text);
[[nodiscard]] ava::core::VoidResult replace_file_with_staged_file(std::filesystem::path const& staged_path, std::filesystem::path const& target_path);
void remove_staged_file_best_effort(std::filesystem::path const& staged_path);

}  // namespace ava::tools
