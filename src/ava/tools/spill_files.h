#pragma once

#include "ava/tools/tool_context.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace ava::tools {

inline constexpr std::size_t kMaxSpillFileBytes = 5 * 1024 * 1024;

class SpillBuffer
{
 public:
  explicit SpillBuffer(std::size_t max_bytes = kMaxSpillFileBytes);

  void append(std::string_view text);

  [[nodiscard]] std::string const& content() const noexcept;
  [[nodiscard]] std::size_t total_bytes() const noexcept;
  [[nodiscard]] bool truncated() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_ON

 private:
  std::size_t max_bytes_ = 0;
  std::size_t total_bytes_ = 0;
  bool truncated_ = false;
  std::string content_;
};

struct SpillFileResult
{
  std::filesystem::path path;
  bool truncated = false;
  std::size_t bytes_written = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<SpillFileResult> write_spill_file(ToolContext const& context, std::string_view tool_name, std::string_view extension,
                                                                  SpillBuffer const& buffer);

[[nodiscard]] ava::core::VoidResult emit_tool_progress(ToolContext const& context, std::string text, std::string status = "running");

}  // namespace ava::tools
