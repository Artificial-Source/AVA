#pragma once

#include "ava/http/transport.h"
#include "ava/tools/tool_context.h"
#include "ava/core/result.h"

#include <cstddef>
#include <string>

namespace ava::tools {

enum class WebFetchFormat
{
  Markdown,
  Text,
  Html,
};

struct WebFetchOptions
{
  std::size_t max_bytes = 1024 * 1024;
  std::size_t offset_line = 1;
  std::size_t max_lines = 200;
  int timeout_ms = 30000;
  WebFetchFormat format = WebFetchFormat::Markdown;
  ava::http::Transport* transport = nullptr;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct WebFetchResult
{
  std::string url;
  int status_code = 0;
  std::string content_type;
  std::string content;
  bool truncated = false;
  bool byte_limited = false;
  bool line_limited = false;
  std::size_t total_bytes = 0;
  std::size_t output_bytes = 0;
  std::size_t output_lines = 0;
  std::size_t start_line = 1;
  std::size_t end_line = 0;
  std::size_t total_lines = 0;
  std::size_t next_offset_line = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<WebFetchResult> webfetch(ToolContext const& context, std::string_view url, WebFetchOptions options = {});

}  // namespace ava::tools
