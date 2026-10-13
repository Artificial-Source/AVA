#pragma once

#include "ava/agent/question.h"
#include "ava/tui/composer.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "debug.h"

namespace ava::tui {

inline constexpr std::size_t kMaxTranscriptItems = 1000;

struct CappedTranscriptSnapshotUpdate
{
  std::size_t leading_evictions = 0;
  std::ptrdiff_t item_index_shift = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

namespace runtime_transcript {

// Conservative raw clipboard text ceiling for OSC 52 copies. Empty or larger
// inputs are rejected (no silent truncation) so terminals never receive an
// unbounded paste payload through AVA's OSC 52 transport.
inline constexpr std::size_t kMaxTerminalClipboardTextBytes = 65'536;  // 64 KiB

[[nodiscard]] std::string assistant_meta_for_snapshot(TuiFrontEnd* frontend, ComposerSnapshot const& snapshot,
                                                      std::optional<std::chrono::steady_clock::duration> elapsed = std::nullopt);
void apply_assistant_turn_meta(std::vector<TranscriptItem>& transcript, std::string const& meta, bool thinking_visible = true);
std::ptrdiff_t push_fallback_assistant_outputs(ComposerSnapshot& snapshot, std::vector<std::string> const& outputs, std::string const& meta);
[[nodiscard]] std::string base64_encode(std::string_view text);
// Pure raw OSC 52 sequence builder: ESC ] 52 ; c ; <base64> ST (ESC \).
// Returns nullopt for empty or oversized text and never truncates or wraps.
[[nodiscard]] std::optional<std::string> try_build_osc52_clipboard_sequence(std::string_view text);
// Pure terminal router. Direct terminals receive the raw sequence. A nonempty
// TMUX value adds exactly one escaped tmux DCS passthrough copy after the raw
// sequence; the value itself is never included in terminal output.
[[nodiscard]] std::optional<std::string> try_build_osc52_clipboard_transport(std::string_view text, std::optional<std::string_view> tmux);
[[nodiscard]] bool copy_text_to_terminal_clipboard(std::string_view text);
[[nodiscard]] std::optional<std::string_view> copy_text_from_answer(ava::agent::QuestionAnswer const& answer);
[[nodiscard]] std::optional<std::string> latest_ava_message_copy_text(std::vector<TranscriptItem> const& transcript);

enum class LatestAssistantCopyResult
{
  RequestSent,
  NoMessage,
  Oversize,
  WriteFailure,
};

// Shared by /copy and app.transcript.copyLatestAssistant. The bound is checked
// before invoking the supplied terminal writer, so oversize and write failures
// remain distinguishable and no payload is silently truncated. RequestSent means
// AVA wrote the request; downstream terminal or multiplexer delivery is unknowable.
[[nodiscard]] LatestAssistantCopyResult copy_latest_assistant_message(
    std::vector<TranscriptItem> const& transcript, std::function<bool(std::string_view)> const& terminal_writer = copy_text_to_terminal_clipboard);
[[nodiscard]] std::string latest_assistant_copy_status(LatestAssistantCopyResult result);
[[nodiscard]] std::vector<std::pair<std::string, bool>> capture_tool_detail_visibility(std::vector<TranscriptItem> const& transcript);
void carry_tool_detail_visibility(std::vector<std::pair<std::string, bool>> const& overrides, std::vector<TranscriptItem>& transcript);
// Capture/carry completed thinking expansion by exact transcript index ownership.
// capture records only currently-true flags; carry clears destination expansion then
// writes only those true indices (after item_index_shift remap). Captured current-UI
// expansion is authoritative after apply_capped_transcript_snapshot so submitted-prefix
// stale true flags cannot resurrect a collapse. Never match on content heuristics.
[[nodiscard]] std::vector<std::pair<std::size_t, bool>> capture_thinking_expansion(std::vector<TranscriptItem> const& transcript);
void carry_thinking_expansion(std::vector<std::pair<std::size_t, bool>> const& overrides, std::vector<TranscriptItem>& transcript,
                              std::ptrdiff_t item_index_shift);
[[nodiscard]] std::string diff_transcript_text(std::string_view title, std::string_view diff);
[[nodiscard]] std::string question_answer_audit_detail(ava::agent::QuestionAnswer const& answer);
std::ptrdiff_t push_transcript(ComposerSnapshot& snapshot, TranscriptItem item);
void push_history(std::vector<std::string>& history, std::string input);

}  // namespace runtime_transcript

[[nodiscard]] CappedTranscriptSnapshotUpdate apply_capped_transcript_snapshot(std::vector<TranscriptItem>& destination,
                                                                              std::vector<TranscriptItem> const& submitted_transcript,
                                                                              std::vector<TranscriptItem> turn_transcript,
                                                                              std::size_t previous_leading_evictions);

// Toggles only the latest original card matching the established tool query.
// The returned transcript index lets the runtime preserve viewport anchoring.
[[nodiscard]] std::optional<std::size_t> toggle_latest_matching_tool_details(std::vector<TranscriptItem>& transcript, std::string_view query,
                                                                             ToolPresentation inherited);
inline std::optional<std::size_t> toggle_latest_matching_tool_details(std::vector<TranscriptItem>& transcript, std::string_view query, bool expanded)
{
  return toggle_latest_matching_tool_details(transcript, query, expanded ? ToolPresentation::Expanded : ToolPresentation::Compact);
}

// Completed thinking that currently renders more than the bounded preview rows.
// Live append-only pending reasoning is never boundable.
[[nodiscard]] bool transcript_item_thinking_is_boundable(TranscriptItem const& item, std::size_t width, bool thinking_visible = true);
// Toggles only the latest completed boundable thinking item. Returns its index.
[[nodiscard]] std::optional<std::size_t> toggle_latest_boundable_thinking(std::vector<TranscriptItem>& transcript, std::size_t width,
                                                                          bool thinking_visible = true);
// Toggles one transcript item when it currently hosts boundable completed thinking.
[[nodiscard]] bool toggle_thinking_expansion_at(std::vector<TranscriptItem>& transcript, std::size_t item_index, std::size_t width,
                                                bool thinking_visible = true);

}  // namespace ava::tui
