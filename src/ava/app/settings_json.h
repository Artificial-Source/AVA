#pragma once

#include "ava/debug/print_members_on.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app {

struct SettingsJsonEntry
{
  std::string key;
  std::string raw_key;
  std::string raw_value;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Returns whether ch is one of the four whitespace characters permitted by JSON.
[[nodiscard]] bool is_settings_json_whitespace(char ch) noexcept;

// Advances offset over JSON whitespace in text, stopping at the next token or text.size().
void skip_settings_json_whitespace(std::string_view text, std::size_t& offset) noexcept;

// Given the opening quote at start, returns the closing quote offset, or nullopt for an invalid boundary.
[[nodiscard]] std::optional<std::size_t> settings_json_string_literal_end(std::string_view text, std::size_t start) noexcept;

// Given a value token at start, returns its exclusive raw-value boundary, or nullopt when no boundary exists.
[[nodiscard]] std::optional<std::size_t> settings_json_value_end(std::string_view text, std::size_t start);

// Parses a valid top-level JSON object into ordered decoded keys and exact raw key/value tokens.
// Returns nullopt when object is not a valid JSON object; duplicate entries remain ordered and unchanged.
[[nodiscard]] std::optional<std::vector<SettingsJsonEntry>> parse_settings_json_object_entries(std::string_view object);

}  // namespace ava::app
