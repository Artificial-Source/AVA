#pragma once
#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <ostream>
#include <string>
#include <string_view>

namespace ava::context {

struct ResourceFileReadOptions
{
  std::size_t max_bytes = 64 * 1024;
  std::string_view resource_description = "resource file";

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ParsedMarkdown
{
  std::map<ava::core::NoLogString, ava::core::NoLogString> frontmatter;
  ava::core::NoLogString body;

#ifdef CWDEBUG
  void print_on(std::ostream& os) const { os << "<redacted>"; }
#endif
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] ava::core::Result<std::string> read_resource_file(std::filesystem::path const& path, ResourceFileReadOptions options = {});
[[nodiscard]] ParsedMarkdown parse_markdown(std::string_view content);
[[nodiscard]] std::string markdown_field(ParsedMarkdown const& markdown, std::string_view name);

}  // namespace ava::context
