#include "sys.h"
#include "ava/app/session_user_turns.h"
#include "ava/session/transcript.h"
#include "ava/core/error.h"
#include "ava/core/utf8.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::app {
namespace {

std::size_t utf8_prefix_size(std::string_view text, std::size_t limit)
{
  std::size_t index = 0;
  std::size_t last = 0;
  while (index < text.size() && index < limit)
  {
    auto const decoded = ava::core::decode_utf8_scalar(text, index);
    if (!decoded || decoded->length > limit - index)
      break;
    index += decoded->length;
    last = index;
  }
  return last;
}

// Collapse the public transcript body into one terminal-neutral preview line
// and bound retained bytes without retaining the full source text.
std::string make_user_turn_preview(std::string_view text)
{
  std::string preview;
  preview.reserve(std::min(text.size(), kMaxSessionUserTurnPreviewBytes));
  bool pending_space = false;
  auto flush_space = [&] {
    if (!pending_space || preview.empty())
      return;
    preview.push_back(' ');
    pending_space = false;
  };
  for (unsigned char const byte : text)
  {
    if (byte == '\n' || byte == '\r' || byte == '\t' || byte < 0x20U || byte == 0x7FU)
    {
      pending_space = true;
      continue;
    }
    flush_space();
    preview.push_back(static_cast<char>(byte));
    if (preview.size() >= kMaxSessionUserTurnPreviewBytes + 4)
      break;
  }
  if (preview.size() > kMaxSessionUserTurnPreviewBytes)
    preview.resize(utf8_prefix_size(preview, kMaxSessionUserTurnPreviewBytes));
  while (!preview.empty() && preview.back() == ' ') preview.pop_back();
  return preview;
}

ava::session::TranscriptLimits transcript_limits_for_session(runtime::session_ts const& unlocked_session)
{
  auto const read_limits = runtime::session_ts::crat(unlocked_session)->session_read_limits();
  ava::session::TranscriptLimits limits;
  // Project every record the bound authority may load. Do not impose a lower
  // arbitrary total-text ceiling than the established session read policy.
  limits.max_items = std::max<std::size_t>(1, read_limits.max_entries);
  limits.max_text_bytes = std::max<std::size_t>(1, read_limits.max_file_bytes);
  limits.max_item_text_bytes = std::max<std::size_t>(1, read_limits.max_line_bytes);
  return limits;
}

ava::core::Result<std::vector<ava::session::TranscriptItem>> load_public_transcript(runtime::session_ts const& unlocked_session,
                                                                                   std::string_view operation)
{
  auto const transcript_limits = transcript_limits_for_session(unlocked_session);
  auto read_authority = runtime::session_ts::crat(unlocked_session)->read_authority_1();
  if (!read_authority)
  {
    auto error = std::move(read_authority.error());
    error.with_context("operation", std::string(operation));
    return std::unexpected(std::move(error));
  }
  // Ordinary load() is bound to the session's established read policy.
  auto entries = read_authority->load();
  if (!entries)
  {
    auto error = std::move(entries.error());
    error.with_context("operation", std::string(operation));
    return std::unexpected(std::move(error));
  }
  auto transcript = ava::session::project_transcript(*entries, transcript_limits);
  if (!transcript)
  {
    auto error = std::move(transcript.error());
    error.with_context("operation", std::string(operation));
    return std::unexpected(std::move(error));
  }
  return transcript;
}

}  // namespace

ava::core::Result<SessionUserTurnList> list_session_user_turns(runtime::session_ts const& unlocked_session, std::size_t max_items)
{
  if (max_items == 0)
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session user turn list limit must be non-zero");
    error.with_context("operation", "list_session_user_turns");
    error.with_context("max_items", std::to_string(max_items));
    return std::unexpected(std::move(error));
  }

  auto transcript = load_public_transcript(unlocked_session, "list_session_user_turns");
  if (!transcript)
    return std::unexpected(std::move(transcript.error()));

  std::vector<ava::session::TranscriptItem const*> user_items;
  user_items.reserve(transcript->size());
  for (auto const& item : *transcript)
  {
    if (item.role == ava::session::TranscriptRole::User)
      user_items.push_back(&item);
  }

  SessionUserTurnList listed;
  listed.truncated_before = user_items.size() > max_items;
  auto const start = listed.truncated_before ? user_items.size() - max_items : 0;
  listed.turns.reserve(user_items.size() - start);
  for (std::size_t index = start; index < user_items.size(); ++index)
  {
    auto const* item = user_items[index];
    listed.turns.push_back(SessionUserTurn{
        .entry_id = item->message_id,
        .timestamp = item->timestamp,
        .preview = make_user_turn_preview(item->text),
    });
  }
  return listed;
}

ava::core::Result<std::string> read_session_user_turn_text(runtime::session_ts const& unlocked_session, std::string_view entry_id)
{
  if (entry_id.empty())
  {
    auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument, "session user turn entry id is required");
    error.with_context("operation", "read_session_user_turn_text");
    return std::unexpected(std::move(error));
  }

  auto transcript = load_public_transcript(unlocked_session, "read_session_user_turn_text");
  if (!transcript)
    return std::unexpected(std::move(transcript.error()));

  for (auto const& item : *transcript)
  {
    if (item.role != ava::session::TranscriptRole::User || item.message_id != entry_id)
      continue;
    return item.text;
  }

  auto error = ava::core::Error(ava::core::ErrorCategory::NotFound, "session user turn not found");
  error.with_context("operation", "read_session_user_turn_text");
  error.with_context("entry_id", std::string(entry_id));
  return std::unexpected(std::move(error));
}

ava::core::VoidResult require_persistent_session_for_fork_from(runtime::session_ts const& unlocked_session)
{
  if (!runtime::session_ts::crat(unlocked_session)->sessionless())
    return {};
  auto error = ava::core::Error(ava::core::ErrorCategory::InvalidArgument,
                                "Cannot fork-from a sessionless session. Start a persistent session first; /copy user still works for ephemeral turns.");
  error.with_context("operation", "require_persistent_session_for_fork_from");
  return std::unexpected(std::move(error));
}

}  // namespace ava::app
