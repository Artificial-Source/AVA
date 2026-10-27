#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/app/runtime/Session.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app {

// Bound retained list-item previews for a later TUI picker. Full message text is
// never retained by list_session_user_turns; use read_session_user_turn_text for
// exact body lookup (for example a later /copy message path).
inline constexpr std::size_t kMaxSessionUserTurnPreviewBytes = 160;
// Soft upper bound on returned newest user turns. Callers may request fewer.
inline constexpr std::size_t kDefaultSessionUserTurnListLimit = 1000;

struct SessionUserTurn
{
  std::string entry_id;
  std::string timestamp;
  ava::core::NoLogString preview;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct SessionUserTurnList
{
  std::vector<SessionUserTurn> turns;
  // True when older public user turns were dropped to honor max_items and only
  // the newest retained window is present.
  bool truncated_before = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Lists public user turns through the session's bound read authority and the
// shared public transcript projection. Assistant, tool, provider-private, and
// internal-replay records are excluded. When max_items is exceeded, only the
// newest turns are retained and truncated_before is set.
[[nodiscard]] ava::core::Result<SessionUserTurnList> list_session_user_turns(runtime::session_ts const& unlocked_session,
                                                                             std::size_t max_items = kDefaultSessionUserTurnListLimit);

// Returns the exact public transcript text for one user entry id. Missing,
// assistant, internal-replay, and non-user ids fail closed as NotFound.
[[nodiscard]] ava::core::Result<std::string> read_session_user_turn_text(runtime::session_ts const& unlocked_session, std::string_view entry_id);

// /fork-from requires a persistent (non-sessionless) session. Ephemeral
// sessions keep /copy user; this guard fails closed with an actionable error
// before the picker opens or a selection mutates presentation.
[[nodiscard]] ava::core::VoidResult require_persistent_session_for_fork_from(runtime::session_ts const& unlocked_session);

}  // namespace ava::app
