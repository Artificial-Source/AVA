#pragma once

#include "ava/tui/composer.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::tui {

enum class CommandOutputInputAction
{
  None,
  Redraw,
  Dismiss,
};

struct CommandOutputInputResult
{
  CommandOutputInputAction action = CommandOutputInputAction::None;
  std::size_t scroll_offset = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CommandOutputGeometry
{
  std::size_t width = 0;
  std::size_t height = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Central completion policy for all submissions. Local slash/shell commands
// preserve transcript bytes; committed dynamic prompt commands use ordinary
// conversation projection while still omitting the literal invocation.
struct TuiSubmissionProjectionPolicy
{
  bool project_conversation = false;
  bool preserve_transcript = false;
  bool present_command_output = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TuiEventState;

// Pure completion boundary for buffered RuntimeEvents. Production uses this
// result to restore local transcripts and settle local output only after the
// backend reports whether a command committed an ordinary provider turn.
struct TuiSubmissionSettlement
{
  TuiSubmissionProjectionPolicy policy;
  std::vector<TranscriptItem> transcript;
  std::vector<ava::core::NoLogString> command_output;
  std::vector<ToolTimelineItem> command_tools;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

[[nodiscard]] TuiSubmissionProjectionPolicy tui_submission_projection_policy(bool is_command_submission, bool ordinary_turn_committed) noexcept;
[[nodiscard]] TuiSubmissionSettlement settle_tui_submission(std::vector<TranscriptItem> const& submitted_transcript, TuiEventState const& buffered_event_state,
                                                            std::string_view submitted, bool is_command_submission, bool ordinary_turn_committed,
                                                            std::vector<std::string> output, std::vector<ToolTimelineItem> tools, bool canceled,
                                                            bool present_command_output = false);
void remove_literal_command_invocation(std::vector<TranscriptItem>& items, std::string_view submitted);
[[nodiscard]] std::string command_output_title_token(std::string_view submitted);
[[nodiscard]] CommandOutputView make_command_output_view(std::string_view submitted, std::vector<std::string> const& output,
                                                         std::vector<ToolTimelineItem> const& tools = {});
void open_command_output(ComposerSnapshot& snapshot, std::string_view submitted, std::vector<std::string> output, std::vector<ToolTimelineItem> tools = {});
void open_command_error(ComposerSnapshot& snapshot, std::string_view submitted, std::string error);
void settle_local_command_status(ComposerSnapshot& snapshot, std::string status);
void settle_local_command_completion(ComposerSnapshot& snapshot, std::string_view submitted, std::vector<std::string> output,
                                     std::vector<ToolTimelineItem> tools = {}, bool record_tools = true);

inline constexpr std::size_t kMaxTuiToolIndexItems = 50;
void seed_tui_tool_index(ComposerSnapshot& snapshot);
void record_tui_tool(ComposerSnapshot& snapshot, ToolTimelineItem tool, TuiToolIndexOrigin origin);
void update_indexed_tui_tool(ComposerSnapshot& snapshot, ToolTimelineItem const& tool);
[[nodiscard]] std::optional<TuiToolIndexEntry> latest_matching_indexed_tool(ComposerSnapshot const& snapshot, std::string_view query = {});
[[nodiscard]] std::optional<std::size_t> indexed_provider_tool_transcript_index(ComposerSnapshot const& snapshot, TuiToolIndexEntry const& entry);
[[nodiscard]] std::optional<std::string> latest_indexed_tool_copy_text(ComposerSnapshot const& snapshot, std::string_view query = {});
[[nodiscard]] std::optional<std::string> latest_indexed_tool_diff_copy_text(ComposerSnapshot const& snapshot, std::string_view query = {});
[[nodiscard]] std::optional<std::string> latest_indexed_permission_copy_text(ComposerSnapshot const& snapshot, std::string_view query = {});

[[nodiscard]] CommandOutputGeometry command_output_geometry(std::size_t terminal_width, std::size_t terminal_height) noexcept;
[[nodiscard]] std::size_t command_output_max_scroll_offset(CommandOutputView const& view, std::size_t width, std::size_t height,
                                                           ToolPresentation presentation = ToolPresentation::Expanded);
[[nodiscard]] CommandOutputInputResult handle_command_output_input(CommandOutputView const& view, InputEvent event, std::size_t width, std::size_t height,
                                                                   ToolPresentation presentation = ToolPresentation::Expanded);
[[nodiscard]] std::vector<std::string> render_command_output(CommandOutputView const& view, std::size_t width, std::size_t height,
                                                             ToolPresentation presentation = ToolPresentation::Expanded);

namespace detail {
[[nodiscard]] std::vector<std::string> render_command_output_modal(CommandOutputView const& view, std::size_t width, std::size_t height,
                                                                   ToolPresentation presentation);
}  // namespace detail

}  // namespace ava::tui
