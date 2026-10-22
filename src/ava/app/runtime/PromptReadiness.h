#pragma once

#include <string_view>

namespace ava::app::runtime {

// Records whether prompt reconstruction succeeded. Prompt text may legitimately
// be empty, so execution readiness must never be inferred from its contents.
enum class PromptReadiness
{
  Ready,
  Unavailable,
};

[[nodiscard]] constexpr std::string_view to_string(PromptReadiness readiness) noexcept
{
  switch (readiness)
  {
    case PromptReadiness::Ready:
      return "ready";
    case PromptReadiness::Unavailable:
      return "unavailable";
  }
  return "unavailable";
}

} // namespace ava::app::runtime
