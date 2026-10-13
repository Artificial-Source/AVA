#include "sys.h"
#include "ava/app/terminal_text.h"
#include "ava/core/utf8.h"

namespace ava::app {

std::string sanitize_terminal_text(std::string_view text)
{
  std::string sanitized;
  sanitized.reserve(text.size());
  for (std::size_t index = 0; index < text.size();)
  {
    auto const byte = static_cast<unsigned char>(text[index]);
    if (byte < 0x20 || byte == 0x7F)
    {
      if (byte == '\t')
        sanitized += "  ";
      else
        sanitized.push_back('?');
      ++index;
      continue;
    }

    auto const decoded = ava::core::decode_utf8_scalar(text, index);
    if (!decoded)
    {
      sanitized.push_back('?');
      ++index;
      continue;
    }

    if (decoded->scalar >= 0x80 && decoded->scalar <= 0x9F)
      sanitized.push_back('?');
    else
      sanitized.append(text.substr(index, decoded->length));
    index += decoded->length;
  }
  return sanitized;
}

}  // namespace ava::app
