#pragma once

#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ava::tui {

enum class TextColorRole
{
  Default,
  Muted,
  Accent,
  Success,
  Warning,
  Error,
  Thinking,
  Code,
  Added,
  Removed,
};

struct Rendition
{
  bool bold = false;
  bool dim = false;
  bool underline = false;
  bool italic = false;
  bool strikethrough = false;
  bool code = false;
  TextColorRole color = TextColorRole::Default;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct NewLine
{
};

struct String
{
  ava::core::NoLogString text;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TextSpan
{
  ava::core::NoLogString text;
  Rendition rendition;
  ava::core::NoLogString link_target = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

using TextRun = std::variant<NewLine, String, TextSpan>;

struct Text
{
  std::vector<TextRun> runs;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] bool text_run_has_embedded_newline(std::string_view text);
[[nodiscard]] bool text_empty(Text const& text);
[[nodiscard]] ava::core::VoidResult append_string(Text& text, std::string value);
[[nodiscard]] ava::core::VoidResult append_span(Text& text, std::string value, Rendition rendition);
void append_newline(Text& text);
void append_plain_text(Text& text, std::string_view value);
[[nodiscard]] Text text_from_plain(std::string_view value);
[[nodiscard]] Text text_from_markdown(std::string_view value, bool include_link_fallbacks = true);
[[nodiscard]] std::string to_plain_text(Text const& text);
[[nodiscard]] ava::core::VoidResult validate_text(Text const& text);

}  // namespace ava::tui
