#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/acp/protocol.h"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app::acp {

inline constexpr std::size_t kMaxAcpPromptBlocks = 64;
inline constexpr std::size_t kMaxAcpPromptTextBytes = 512U * 1024U;
inline constexpr std::size_t kMaxAcpPromptImages = 8;
// Base64 expands by 4/3 and every ACP JSON string is capped at 256 KiB.
inline constexpr std::size_t kMaxAcpPromptImageBytes = 192U * 1024U;
inline constexpr std::size_t kMaxAcpPromptImageTotalBytes = 720U * 1024U;

struct AcpPromptImage
{
  std::string mime_type;
  std::string bytes;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct AcpPromptContent
{
  std::string text;
  std::vector<AcpPromptImage> images;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

using PromptContentDecodeResult = std::expected<AcpPromptContent, JsonRpcError>;

[[nodiscard]] PromptContentDecodeResult decode_prompt_content(std::string_view prompt_json);

}  // namespace ava::app::acp
