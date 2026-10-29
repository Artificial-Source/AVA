#pragma once

#include "ava/app/acp/protocol.h"

#include <expected>
#include <string_view>
#include <nlohmann/json.hpp>

namespace ava::app::acp {

// Raw JSON-value entry point: the same byte/depth/string/collection and
// duplicate-member limits as the wire codec, without requiring an envelope.
[[nodiscard]] std::expected<nlohmann::json, JsonRpcError> decode_json_value(std::string_view text);

}  // namespace ava::app::acp
