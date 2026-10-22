#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/result.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace ava::app {

struct OAuthCallbackResult
{
  std::string code;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ava::core::Result<OAuthCallbackResult> wait_for_oauth_callback(std::string_view expected_state, std::function<bool()> const& cancel_requested);

namespace detail {
// Internal listener configuration. The ordinary connect flow retains its fixed
// redirect port and five-minute ceiling; callers can request an earlier cutoff.
struct OAuthCallbackOptions
{
  std::uint16_t port = 1455;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  // Called after listen succeeds, with the actual port (including port=0).
  std::function<void(std::uint16_t)> listener_ready;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] ava::core::Result<OAuthCallbackResult> wait_for_oauth_callback(std::string_view expected_state, std::function<bool()> const& cancel_requested,
                                                                             OAuthCallbackOptions const& options);
}  // namespace detail

}  // namespace ava::app
