#pragma once

#include <string>
#include <string_view>

namespace ava::core {
namespace detail {

[[nodiscard]] inline std::string markup_escape(std::string_view value, std::string_view apostrophe)
{
  std::string escaped;
  escaped.reserve(value.size());
  for (char const ch : value)
  {
    switch (ch)
    {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += apostrophe;
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

}  // namespace detail

// Escape text/attribute metacharacters only; preserve all other bytes verbatim.
[[nodiscard]] inline std::string xml_escape(std::string_view value)
{
  return detail::markup_escape(value, "&apos;");
}

[[nodiscard]] inline std::string html_escape(std::string_view value)
{
  return detail::markup_escape(value, "&#39;");
}

}  // namespace ava::core
