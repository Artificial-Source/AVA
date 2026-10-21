#pragma once

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::core::json {

// Strict AVA JSON surfaces accept at most 64 simultaneously open object/array
// containers, counting the root container as depth one.
inline constexpr std::size_t kMaxNestingDepth = 64;

[[nodiscard]] bool is_valid_utf8(std::string_view value) noexcept;
[[nodiscard]] std::string replace_invalid_utf8(std::string_view value);
[[nodiscard]] std::string escape(std::string_view value);
// Remove JSON whitespace outside strings, preserving escapes and all string bytes.
// Reject raw CR/LF inside strings. This is framing compaction, not syntax validation.
[[nodiscard]] std::optional<std::string> compact(std::string_view value);
[[nodiscard]] std::optional<std::size_t> field_value_start(std::string_view object, std::string_view key);
[[nodiscard]] std::optional<std::string> string_field(std::string_view object, std::string_view key);
[[nodiscard]] std::optional<long long> integer_field(std::string_view object, std::string_view key);
[[nodiscard]] std::optional<std::string> object_field(std::string_view object, std::string_view key);
[[nodiscard]] std::vector<std::string> objects_in_array_field(std::string_view object, std::string_view key);
// Returns nullopt unless the named value is a syntactically valid array whose
// every element is a JSON object. Unlike objects_in_array_field(), this never
// skips scalar or malformed elements. max_items bounds vector materialization;
// a matching array with more elements is rejected.
[[nodiscard]] std::optional<std::vector<std::string>> strict_objects_in_array_field(std::string_view object, std::string_view key,
                                                                                    std::size_t max_items = std::numeric_limits<std::size_t>::max());
[[nodiscard]] std::vector<std::string> strings_in_array_field(std::string_view object, std::string_view key);
[[nodiscard]] bool is_valid_object(std::string_view value);
// Protocols with an independently established recursive-depth contract may
// request a bound up to 128 without changing the default 64-container limit
// used by existing JSON surfaces. Larger values fail closed before recursion.
[[nodiscard]] bool is_valid_object_with_max_depth(std::string_view value, std::size_t max_depth);

}  // namespace ava::core::json
