#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/core/NoLogString.h"
#include "ava/core/result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ava::agent {

// Hard inspector caps applied on every coordinated live/final projection.
// Effective SessionReadLimits are still per-field min(authority policy, these)
// inside the internal source implementation.
inline constexpr std::size_t kSubagentInspectorMaxEntries = 1000;
inline constexpr std::size_t kSubagentInspectorMaxFileBytes = 1U * 1024U * 1024U;
inline constexpr std::size_t kSubagentInspectorMaxLineBytes = 256U * 1024U;
inline constexpr std::size_t kSubagentInspectorMaxMessages = 256;
inline constexpr std::size_t kSubagentInspectorMaxTextBytes = 256U * 1024U;
inline constexpr std::size_t kSubagentInspectorMaxItemTextBytes = 64U * 1024U;

enum class SubagentLiveMessageRole
{
  User,
  Assistant,
};

[[nodiscard]] std::string_view to_string(SubagentLiveMessageRole role) noexcept;

// Path-free committed public transcript item. MVP projects only ordinary
// user/assistant message text; tools and reasoning are intentionally absent.
struct SubagentLiveMessage
{
  SubagentLiveMessageRole role = SubagentLiveMessageRole::User;
  ava::core::NoLogString text;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Path-free inspection frame. Never carries session paths, raw provider/session
// errors, or authority types. App/TUI consumers may include only this header.
struct SubagentInspectorFrame
{
  // Monotonic published content generation. Never zero once any frame has been
  // successfully published for the job. Distinct from the internal source/freeze
  // epoch used only to invalidate in-flight work.
  std::uint64_t generation = 0;
  bool not_modified = false;
  bool terminal = false;
  bool freeze_pending = false;
  // Stable when a job was published without an inspection source (legacy/tests)
  // or when no prior path-free frame exists after a failed final freeze.
  bool unavailable = false;
  // Truthful: the latest live refresh or final freeze projection failed. Prior
  // messages are retained when available; never carries raw error text.
  bool refresh_unavailable = false;
  std::vector<SubagentLiveMessage> messages;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] std::shared_ptr<SubagentInspectorFrame const> make_unavailable_inspection_frame(std::uint64_t generation, bool terminal,
                                                                                             bool freeze_pending = false);
[[nodiscard]] std::shared_ptr<SubagentInspectorFrame const> make_not_modified_inspection_frame(std::uint64_t generation, bool terminal,
                                                                                              bool freeze_pending = false);
[[nodiscard]] std::shared_ptr<SubagentInspectorFrame const> make_refresh_unavailable_inspection_frame(std::uint64_t generation, bool terminal,
                                                                                                     bool freeze_pending,
                                                                                                     std::vector<SubagentLiveMessage> messages = {});

}  // namespace ava::agent
