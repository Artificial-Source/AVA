#include "sys.h"
#include "ava/config/provider_config_internal.h"
#include "ava/core/ascii.h"
#include "ava/core/hex.h"
#include "ava/core/json.h"

#include <cctype>
#include <cerrno>
#include <string>
#include <system_error>

namespace ava::config::provider_config_detail {

ava::core::Error config_error(std::string message, std::string_view field)
{
  auto error = ava::core::Error(ava::core::ErrorCategory::Configuration, std::move(message));
  if (!field.empty())
    error.with_context("field", std::string(field));
  return error;
}

ava::core::Error path_error(ava::core::ErrorCategory category, std::string message, std::filesystem::path const& path, std::string_view operation)
{
  auto error = ava::core::Error(category, std::move(message));
  error.with_context("path", path.string());
  if (!operation.empty())
    error.with_context("operation", std::string(operation));
  return error;
}

std::string errno_message()
{
  return std::generic_category().message(errno);
}

// Reject percent-encodings that can smuggle separators or dot-segments after decode.
bool has_encoded_separator_or_dot_ambiguity(std::string_view value) noexcept
{
  for (std::size_t i = 0; i < value.size(); ++i)
  {
    if (value[i] != '%')
      continue;
    if (i + 2 >= value.size())
      return true;
    int const hi = ava::core::hex_digit_value(value[i + 1]);
    int const lo = ava::core::hex_digit_value(value[i + 2]);
    if (hi < 0 || lo < 0)
      return true;
    auto const decoded = static_cast<unsigned char>((hi << 4) | lo);
    if (decoded == '.' || decoded == '/' || decoded == '\\' || decoded < 0x20 || decoded == 0x7f)
      return true;
    // Nested/overlong forms such as %25xx that reintroduce separators after a second decode.
    if (decoded == '%')
      return true;
    i += 2;
  }
  return false;
}

bool is_valid_provider_id(std::string_view id) noexcept
{
  if (id.empty() || id.size() > kMaxProviderIdBytes)
    return false;
  for (char const ch : id)
  {
    auto const byte = static_cast<unsigned char>(ch);
    bool const ok = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || ch == '_' || ch == '-';
    if (!ok)
      return false;
  }
  return true;
}

bool is_valid_shell_env_name(std::string_view name) noexcept
{
  if (name.empty() || name.size() > kMaxApiKeyEnvBytes)
    return false;
  auto const first = static_cast<unsigned char>(name.front());
  if (!((first >= 'A' && first <= 'Z') || first == '_'))
    return false;
  for (std::size_t i = 1; i < name.size(); ++i)
  {
    auto const byte = static_cast<unsigned char>(name[i]);
    bool const ok = (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '_';
    if (!ok)
      return false;
  }
  return true;
}

bool is_valid_display_name(std::string_view name) noexcept
{
  return !name.empty() && name.size() <= kMaxDisplayNameBytes && !ava::core::has_c0_or_del(name) && ava::core::json::is_valid_utf8(name);
}

}  // namespace ava::config::provider_config_detail
