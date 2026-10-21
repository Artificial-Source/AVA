#pragma once

#include "ava/core/json.h"

#include <cstddef>
#include <string_view>

namespace ava::core {

// Provider-native reasoning payloads are retained only for OpenAI continuation.
// Keep them within the established opaque-provider payload ceiling.
inline constexpr std::size_t kMaxOpenAINativeReasoningItemBytes = 64U * 1024U;
inline constexpr std::size_t kMaxOpenAIOpaqueIdBytes = 256U;

[[nodiscard]] inline bool is_valid_openai_opaque_id(std::string_view id) noexcept
{
  if (id.empty() || id.size() > kMaxOpenAIOpaqueIdBytes)
    return false;
  for (char const ch : id)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte < 0x20U || byte == 0x7FU)
      return false;
  }
  return true;
}

// OpenAI may add fields to reasoning items. AVA retains them only when their
// stable replay identity and summary-array shape remain present and bounded.
// Allocation or parsing exceptions also reject the item rather than escaping noexcept.
[[nodiscard]] inline bool is_valid_openai_native_reasoning_item_json(std::string_view native_item_json) noexcept
{
  try
  {
    if (native_item_json.empty() || native_item_json.size() > kMaxOpenAINativeReasoningItemBytes || !json::is_valid_object(native_item_json) ||
        json::string_field(native_item_json, "type").value_or("") != "reasoning")
    {
      return false;
    }
    auto const id = json::string_field(native_item_json, "id");
    auto const summary_items = json::strict_objects_in_array_field(native_item_json, "summary");
    if (!id || !is_valid_openai_opaque_id(*id) || !summary_items)
      return false;
    for (auto const& summary_item : *summary_items)
    {
      if (json::string_field(summary_item, "type").value_or("") != "summary_text" || !json::string_field(summary_item, "text"))
        return false;
    }
    return true;
  }
  catch (...)
  {
    return false;
  }
}

}  // namespace ava::core
