#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/session/session_store.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ava::session {

enum class TranscriptRole
{
  User,
  Assistant,
};

struct TranscriptItem
{
  TranscriptRole role = TranscriptRole::User;
  std::string message_id;
  std::string timestamp;
  ava::core::NoLogString text;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TranscriptLimits
{
  std::size_t max_items = 128;
  std::size_t max_text_bytes = 512U * 1024U;
  std::size_t max_item_text_bytes = 128U * 1024U;
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Projects only user-visible message text. Internal replay records and all
// provider-native/tool/private data are deliberately excluded.
[[nodiscard]] ava::core::Result<std::vector<TranscriptItem>> project_transcript(std::vector<SessionEntry> const& entries, TranscriptLimits limits = {});
[[nodiscard]] ava::core::Result<std::vector<TranscriptItem>> project_transcript_bounded(SessionStore const& store, SessionReadLimits read_limits,
                                                                                        TranscriptLimits limits = {},
                                                                                        SessionCancelCallback cancel_requested = nullptr);

}  // namespace ava::session
