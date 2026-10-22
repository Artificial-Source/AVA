#pragma once

#include "ava/permissions/permission.h"
#include "ava/core/mode.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ava::tui {

// This registry is owned exclusively by the TUI thread. It deliberately keeps
// only exact, process-local grants; persistent rules are resolved upstream of
// the TUI fallback resolver.
enum class TuiSessionGrantInsertResult
{
  Added,
  AlreadyPresent,
  Ineligible,
  Full,
};

[[nodiscard]] bool tui_session_grant_eligible(ava::permissions::PermissionPrompt const& prompt) noexcept;

class TuiSessionGrantRegistry final
{
 public:
  static constexpr std::size_t kMaxGrants = 64;

  [[nodiscard]] bool matches(std::string_view session_id, ava::permissions::PermissionPrompt const& prompt) const;
  [[nodiscard]] TuiSessionGrantInsertResult add(std::string_view session_id, ava::permissions::PermissionPrompt const& prompt);

  // Returns true exactly when an authoritative session id differs from the
  // current one. The TUI calls this at committed backend identity transition boundaries.
  [[nodiscard]] bool clear_for_session_transition(std::string_view current_session_id, std::string_view next_session_id) noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

 private:
  struct Grant
  {
    std::string session_id;
    ava::core::Mode mode = ava::core::Mode::Build;
    std::string tool_name;
    std::string workspace_recipe_key;

    AVA_DEBUG_PRINT_MEMBERS_ON
  };

  std::vector<Grant> grants_;

  // This thread-confined registry has a vector of private grants rather than a
  // useful standalone debug representation.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

}  // namespace ava::tui
