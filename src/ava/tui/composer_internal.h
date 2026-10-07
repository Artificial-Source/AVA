#pragma once

#include "ava/app/terminal_text.h"
#include "ava/tui/composer.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::tui {

using ava::app::sanitize_terminal_text;

namespace detail {

inline constexpr std::size_t kMinWidth = 20;
inline constexpr std::size_t kMinHeight = 8;
inline constexpr std::size_t kMinComposerBlockLines = 2;
inline constexpr std::size_t kMaxComposerBlockLines = 8;
inline constexpr std::size_t kMaxPaletteLines = 8;
inline constexpr std::size_t kWideModalMinimumWidth = 56;
inline constexpr std::size_t kRoomyModalMinimumLines = 14;

[[nodiscard]] inline constexpr std::size_t modal_horizontal_inset(std::size_t width) noexcept
{
  return width >= kWideModalMinimumWidth ? 4 : 2;
}

[[nodiscard]] inline constexpr std::size_t modal_vertical_inset(std::size_t max_lines) noexcept
{
  return max_lines >= kRoomyModalMinimumLines ? 1 : 0;
}

[[nodiscard]] inline constexpr std::size_t modal_content_width(std::size_t width) noexcept
{
  auto const insets = 2 * modal_horizontal_inset(width);
  return width > insets ? width - insets : 0;
}

inline constexpr std::string_view kReverseVideo = "\x1b[7m";
inline constexpr std::string_view kReverseVideoOff = "\x1b[27m";

// The curses draw path currently maps these truecolor SGR forms directly.
// Extend that parser before changing these to 256-color or basic ANSI codes.
inline constexpr std::string_view kSgrReset = "\x1b[0m";
inline constexpr std::string_view kSgrBold = "\x1b[1m";
inline constexpr std::string_view kSgrItalic = "\x1b[3m";
inline constexpr std::string_view kSgrUnderline = "\x1b[4m";
inline constexpr std::string_view kSgrStrikethrough = "\x1b[9m";
inline constexpr std::string_view kSgrText = "\x1b[38;2;232;236;241m";
inline constexpr std::string_view kSgrMuted = "\x1b[38;2;139;149;165m";
inline constexpr std::string_view kSgrDim = kSgrMuted;
inline constexpr std::string_view kSgrThinking = "\x1b[38;2;88;96;112m";
inline constexpr std::string_view kSgrTextDimmed = "\x1b[38;2;145;157;178m";
inline constexpr std::string_view kSgrSuccess = "\x1b[38;2;52;211;153m";
inline constexpr std::string_view kSgrWarning = "\x1b[38;2;251;191;36m";
inline constexpr std::string_view kSgrError = "\x1b[38;2;248;113;113m";
inline constexpr std::string_view kSgrAccent = "\x1b[38;2;77;158;246m";
inline constexpr std::string_view kSgrScreenBg = "\x1b[49m";
inline constexpr std::string_view kSgrComposerBg = "\x1b[48;2;26;31;46m";
inline constexpr std::string_view kSgrToolBg = "\x1b[48;2;18;23;34m";
inline constexpr std::string_view kSgrQuestionBg = "\x1b[48;2;32;38;56m";
inline constexpr std::string_view kComposerBar = "│";
// Fixed-width four-cell signal meter. Outer bars use muted styling; the inner pair uses accent blue.
inline constexpr std::array<std::string_view, 4> kProcessingIndicatorFrames = {"▂▄▇▃", "▃▆▄▂", "▅▃▇▄", "▄▇▅▂"};
inline constexpr auto kProcessingIndicatorFrameDelay = std::chrono::milliseconds(120);
inline constexpr std::size_t kProcessingIndicatorColumns = 4;
// Lower-block meter glyphs are single-column and three UTF-8 bytes each.
inline constexpr std::size_t kProcessingIndicatorBarBytes = 3;

[[nodiscard]] inline std::string_view processing_indicator_frame(std::size_t frame)
{
  return kProcessingIndicatorFrames[frame % kProcessingIndicatorFrames.size()];
}

// Styled meter for color terminals: muted | accent | accent | muted, then reset to screen bg.
// NO_COLOR callers should use processing_indicator_frame() (or strip SGR from this result).
[[nodiscard]] inline std::string processing_indicator_styled(std::size_t frame)
{
  auto const raw = processing_indicator_frame(frame);
  if (raw.size() != kProcessingIndicatorBarBytes * kProcessingIndicatorColumns)
    return std::string(raw);
  auto const bar0 = raw.substr(0, kProcessingIndicatorBarBytes);
  auto const bar1 = raw.substr(kProcessingIndicatorBarBytes, kProcessingIndicatorBarBytes);
  auto const bar2 = raw.substr(2 * kProcessingIndicatorBarBytes, kProcessingIndicatorBarBytes);
  auto const bar3 = raw.substr(3 * kProcessingIndicatorBarBytes, kProcessingIndicatorBarBytes);
  std::string styled;
  styled.reserve(raw.size() + kSgrMuted.size() * 2 + kSgrAccent.size() + kSgrReset.size() + kSgrScreenBg.size());
  styled.append(kSgrMuted);
  styled.append(bar0);
  styled.append(kSgrAccent);
  styled.append(bar1);
  styled.append(bar2);
  styled.append(kSgrMuted);
  styled.append(bar3);
  styled.append(kSgrReset);
  styled.append(kSgrScreenBg);
  return styled;
}

[[nodiscard]] inline std::size_t processing_indicator_elapsed_frames(std::chrono::steady_clock::duration elapsed)
{
  auto const interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(kProcessingIndicatorFrameDelay);
  return elapsed > std::chrono::steady_clock::duration::zero() ? static_cast<std::size_t>(elapsed / interval) : 0;
}

enum class NcursesColorRole
{
  Text,
  Muted,
  Success,
  Warning,
  Error,
  Accent,
};

// Maps AVA's truecolor SGR values to the semantic ncurses palette roles.
[[nodiscard]] NcursesColorRole ncurses_color_role_for_sgr(std::string_view sgr);

// Ordinary dock gutters inherit the screen/transcript background. Elevated palette
// surfaces pass kSgrComposerBg so their left rail stays on the composer panel.
[[nodiscard]] inline std::string composer_gutter(std::string_view background_sgr = kSgrScreenBg)
{
  return std::string(kSgrAccent) + std::string(kComposerBar) + std::string(kSgrReset) + std::string(background_sgr) + "  ";
}

struct ComposerLayoutPolicy
{
  bool compact_transcript_spacing = false;
  std::size_t transcript_composer_gap_lines = 0;
  std::size_t composer_top_padding_lines = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] ComposerLayoutPolicy composer_layout_policy(ComposerSnapshot const& snapshot, std::size_t height);

struct ComposerInputLayout
{
  std::size_t top_padding = 0;
  std::size_t first_visible = 0;
  std::size_t visible_input_lines = 1;
  std::size_t hidden_above = 0;
  std::size_t hidden_below = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct ComposerInputRenderLine
{
  std::string text;
  std::size_t start = 0;
  std::size_t end = 0;
  bool first_line = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TerminalTextCell
{
  std::size_t bytes = 0;
  std::size_t columns = 0;
  bool valid = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

enum class CompletionSurface
{
  None,
  FileReference,
  PathCompletion,
};

struct CompletionPrefix
{
  std::size_t start = 0;
  std::size_t cursor = 0;
  std::string value = {};
  bool quoted = false;
  bool leading_dot_slash = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CompletionMatchModel
{
  std::string input = {};
  std::size_t cursor = std::string::npos;
  bool force_path = false;
  std::size_t source_revision = 0;
  CompletionSurface surface = CompletionSurface::None;
  CompletionPrefix prefix = {};
  std::vector<std::size_t> ranked_source_indices = {};
  bool palette_visible = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct CompletionMatchCache
{
  std::optional<CompletionMatchModel> model = std::nullopt;
  std::size_t ranking_build_count = 0;
  std::size_t formatted_candidate_count = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TranscriptRenderedBlock
{
  std::vector<std::string> lines = {};
  std::vector<bool> presentation_private_rows = {};

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TranscriptLayout
{
  std::vector<std::string> lines = {};
  // Exact row-aligned presentation policy. Private rows remain in `lines` for
  // screen geometry but are excluded from transcript search and extraction.
  std::vector<bool> presentation_private_rows = {};
  std::vector<std::size_t> block_boundaries = {};
  std::vector<std::size_t> message_starts = {};
  std::vector<std::size_t> content_starts = {};
  std::vector<std::size_t> message_item_indices = {};

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TranscriptViewportAnchor
{
  bool valid = false;
  bool content_relative = false;
  std::size_t item_index = 0;
  std::size_t line_offset = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

[[nodiscard]] TranscriptViewportAnchor capture_transcript_viewport_anchor(TranscriptLayout const& layout, std::size_t max_scroll_offset,
                                                                          std::size_t scroll_offset);
[[nodiscard]] std::size_t restore_transcript_viewport_anchor(TranscriptViewportAnchor anchor, TranscriptLayout const& layout, std::size_t max_scroll_offset,
                                                             std::ptrdiff_t item_index_shift);

struct TranscriptTailOrderedListState
{
  std::string indent = {};
  char delimiter = '.';
  std::size_t next_number = 1;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TranscriptTailRenderCache
{
  bool valid = false;
  std::size_t transcript_generation = 0;
  std::string stream_id = {};
  std::size_t item_index = 0;
  std::size_t source_size = 0;
  std::size_t counterpart_size = 0;
  std::size_t source_tail_start = 0;
  bool source_is_thinking = false;
  bool source_is_tool = false;
  ToolTimelineStatus tool_status = ToolTimelineStatus::Running;
  ToolLifecycleState tool_lifecycle = ToolLifecycleState::ProviderAnnounced;
  std::string tool_name = {};
  std::string tool_call_id = {};
  bool tool_prefix_saturated = false;
  bool tool_source_safe = false;
  bool fenced_code_open = false;
  std::string fenced_code_language = {};
  std::size_t line_columns = 0;
  std::vector<TranscriptTailOrderedListState> ordered_list_states = {};
  bool blockquote_open = false;
  std::size_t width = 0;
  std::size_t max_tail_lines = 0;
  ToolPresentation tool_presentation = ToolPresentation::Rich;
  bool thinking_visible = true;
  bool compact_spacing = false;
  bool plain_output = false;
  std::vector<std::uint64_t> mermaid_projection_identity = {};
  std::string meta = {};
  std::vector<std::string> rendered_stream_prefix = {};
  std::vector<std::string> rendered_tail = {};
  std::size_t source_budget_bytes = 0;
  std::size_t retained_source_bytes = 0;
  std::size_t full_source_bytes = 0;
  std::size_t incremental_source_bytes = 0;
  std::size_t carry_source_bytes = 0;
  std::size_t max_carry_source_bytes = 0;
  std::size_t incremental_updates = 0;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TranscriptLayoutCache
{
  std::size_t transcript_generation = 0;
  std::size_t width = 0;
  ToolPresentation tool_presentation = ToolPresentation::Rich;
  bool thinking_visible = true;
  bool compact_spacing = false;
  bool valid = false;
  TranscriptLayout layout = {};
  std::size_t layout_build_count = 0;
  mutable std::size_t visible_slice_count = 0;
  TranscriptTailRenderCache tail = {};

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct ScreenRowCache
{
  std::vector<std::string> surfaces = {};
  std::vector<bool> dirty_rows = {};
  std::vector<TerminalGraphicOverlay> graphics = {};
  std::string style_key = {};
  std::size_t width = 0;
  std::size_t height = 0;
  bool valid = false;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

void mark_screen_row_dirty(ScreenRowCache& screen_cache, std::size_t row);
[[nodiscard]] std::vector<std::size_t> changed_screen_rows(std::vector<std::string> const& previous, std::vector<std::string> const& current,
                                                           std::vector<bool> const& dirty_rows, bool invalidate);

[[nodiscard]] bool is_utf8_continuation(unsigned char byte);
[[nodiscard]] std::size_t utf8_sequence_length(unsigned char byte);
[[nodiscard]] bool decode_utf8_codepoint(std::string_view text, std::size_t start, std::size_t length, char32_t& codepoint);
[[nodiscard]] std::size_t codepoint_columns(char32_t codepoint);
// Compact user-perceived cluster boundaries shared by rendering and the composer editor.
// Covers base+combining/variation marks, regional-indicator pairs, emoji modifiers, and ZWJ
// emoji sequences. Deliberately narrower than full Unicode UAX #29 grapheme segmentation.
[[nodiscard]] std::size_t terminal_text_cluster_bytes(std::string_view text, std::size_t index);
[[nodiscard]] TerminalTextCell terminal_text_cell(std::string_view text, std::size_t index);
[[nodiscard]] bool skip_sgr_sequence(std::string_view text, std::size_t& index);
[[nodiscard]] bool skip_osc_sequence(std::string_view text, std::size_t& index);
[[nodiscard]] std::size_t terminal_text_columns(std::string_view text);

enum class WordSegmentClass
{
  Space,
  Word,
  WideWord,
  Punctuation,
};

// Shared Unicode-aware segment classifier for composer word movement and rendered
// transcript word selection. `cursor` must name a compact cluster start.
[[nodiscard]] WordSegmentClass word_segment_class_at(std::string_view text, std::size_t cursor);

[[nodiscard]] std::string fit_line(std::string text, std::size_t width);
[[nodiscard]] std::string fit_line_preserving_sgr(std::string text, std::size_t width);
[[nodiscard]] std::string surface_line(std::string_view background_sgr, std::string line, std::size_t width);
[[nodiscard]] std::string screen_surface_line(std::string line, std::size_t width);
[[nodiscard]] std::string composer_surface_line(std::string line, std::size_t width);
[[nodiscard]] std::string tool_surface_line(std::string line, std::size_t width);
[[nodiscard]] std::string question_surface_line(std::string line, std::size_t width);
[[nodiscard]] std::vector<std::string> wrap_transcript_text(std::string_view text, std::size_t width);

[[nodiscard]] std::string slash_command_prefix(std::string_view input);
[[nodiscard]] std::vector<std::string> render_slash_palette(ComposerSnapshot const& snapshot, std::size_t width, std::size_t max_lines);
[[nodiscard]] std::vector<std::string> render_file_reference_palette(ComposerSnapshot const& snapshot, CompletionMatchCache& cache, std::size_t width,
                                                                     std::size_t max_lines);
[[nodiscard]] std::vector<std::string> render_path_completion_palette(ComposerSnapshot const& snapshot, CompletionMatchCache& cache, std::size_t width,
                                                                      std::size_t max_lines);
[[nodiscard]] std::optional<std::string> disabled_visible_completion_selection_status(ComposerSnapshot const& snapshot);
[[nodiscard]] std::optional<std::string> disabled_visible_completion_selection_status(ComposerSnapshot const& snapshot, CompletionMatchCache const& cache);
void refresh_completion_match_cache(CompletionMatchCache& cache, ComposerSnapshot const& snapshot, std::size_t source_revision);
[[nodiscard]] std::size_t next_completion_selection(CompletionMatchCache const& cache, std::size_t selected_index);
[[nodiscard]] std::size_t previous_completion_selection(CompletionMatchCache const& cache, std::size_t selected_index);
[[nodiscard]] std::size_t clamp_completion_selection(CompletionMatchCache const& cache, std::size_t selected_index);
[[nodiscard]] FileReferenceSelectionText completion_selection_text(CompletionMatchCache const& cache, ComposerSnapshot const& snapshot,
                                                                   std::size_t selected_index);
[[nodiscard]] std::optional<std::string> completion_selection_disabled_reason(CompletionMatchCache const& cache,
                                                                              std::vector<FileReferenceItem> const& references, std::size_t selected_index);

[[nodiscard]] std::vector<std::string> render_permission_prompt(PermissionPromptView const& prompt, std::size_t width, std::size_t max_lines);
[[nodiscard]] std::vector<std::string> render_question_prompt(QuestionPromptView const& prompt, std::size_t width, std::size_t max_lines);
[[nodiscard]] std::vector<std::string> render_question_modal(QuestionPromptView const& prompt, std::size_t width, std::size_t max_lines);
[[nodiscard]] std::optional<std::size_t> question_option_for_dock_row(QuestionPromptView const& prompt, std::size_t row, std::size_t width,
                                                                      std::size_t max_lines);
[[nodiscard]] std::optional<std::size_t> question_option_for_modal_row(QuestionPromptView const& prompt, std::size_t row, std::size_t width,
                                                                       std::size_t max_lines);
[[nodiscard]] std::vector<std::string> render_select_list_modal(SelectListView const& view, std::size_t width, std::size_t max_lines);
[[nodiscard]] std::optional<std::size_t> select_list_item_for_modal_row(SelectListView const& view, std::size_t modal_row, std::size_t width,
                                                                        std::size_t max_lines);
[[nodiscard]] std::vector<std::string> render_plugin_ui_dock(TuiPluginUiDockView const& view, std::size_t width, std::size_t max_lines);
[[nodiscard]] std::vector<std::string> render_plugin_ui_modal(TuiPluginUiModalView const& view, std::size_t width, std::size_t max_lines);
[[nodiscard]] std::optional<std::size_t> plugin_ui_option_for_modal_row(TuiPluginUiModalView const& view, std::size_t modal_row, std::size_t width,
                                                                        std::size_t max_lines);
[[nodiscard]] std::vector<std::string> render_unified_diff_body(std::string_view diff, bool diff_truncated, std::size_t width, std::string_view line_prefix,
                                                                std::size_t max_lines);

[[nodiscard]] std::string render_generic_line(std::string const& text, std::size_t width);
[[nodiscard]] TranscriptLayout render_transcript_layout(std::vector<TranscriptItem> const& transcript, std::size_t width,
                                                        ToolPresentation tool_presentation = ToolPresentation::Rich, bool thinking_visible = true,
                                                        bool compact_spacing = false, MermaidTranscriptProjection const* mermaid_projection = nullptr);
[[nodiscard]] TranscriptRenderedBlock render_transcript_search_item(std::vector<TranscriptItem> const& transcript, std::size_t item_index, std::size_t width,
                                                                    ToolPresentation tool_presentation, bool thinking_visible, bool compact_spacing,
                                                                    MermaidTranscriptProjection const* mermaid_projection = nullptr);
[[nodiscard]] std::vector<std::string> render_transcript_search_item_lines(std::vector<TranscriptItem> const& transcript, std::size_t item_index,
                                                                           std::size_t width, ToolPresentation tool_presentation, bool thinking_visible,
                                                                           bool compact_spacing,
                                                                           MermaidTranscriptProjection const* mermaid_projection = nullptr);
[[nodiscard]] std::optional<std::size_t> transcript_tool_card_header_for_screen_position(ComposerSnapshot const& snapshot, std::size_t row, std::size_t column);
// Click target for the first rendered "Thinking:" header line of a boundable completed thinking item.
[[nodiscard]] std::optional<std::size_t> transcript_thinking_header_for_screen_position(ComposerSnapshot const& snapshot, std::size_t row, std::size_t column);

// Shared transcript body geometry used by header hit-testing and rendered selection.
struct TranscriptBodyScreenGeometry
{
  std::size_t transcript_height = 0;
  std::size_t content_width = 0;
  std::size_t canvas_left = 0;
  bool valid = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};
[[nodiscard]] TranscriptBodyScreenGeometry transcript_body_screen_geometry(ComposerSnapshot const& snapshot);
// Bounded completed-thinking preview: first 11 rendered content rows + one footer row.
inline constexpr std::size_t kThinkingBoundedMaxRows = 12;
inline constexpr std::size_t kThinkingBoundedContentRows = 11;
// True when completed (non-live) thinking currently renders more than the bounded preview at width.
[[nodiscard]] bool transcript_item_has_boundable_thinking(TranscriptItem const& item, std::size_t width, bool thinking_visible);
[[nodiscard]] std::vector<std::string> render_transcript_lines(std::vector<TranscriptItem> const& transcript, std::size_t width,
                                                               ToolPresentation tool_presentation = ToolPresentation::Rich, bool thinking_visible = true,
                                                               bool compact_spacing = false, MermaidTranscriptProjection const* mermaid_projection = nullptr);
[[nodiscard]] std::vector<std::string> render_transcript_tail_lines(std::vector<TranscriptItem> const& transcript, std::size_t width,
                                                                    std::size_t max_tail_lines, ToolPresentation tool_presentation = ToolPresentation::Rich,
                                                                    bool thinking_visible = true, bool compact_spacing = false,
                                                                    MermaidTranscriptProjection const* mermaid_projection = nullptr);
[[nodiscard]] std::vector<std::string> render_transcript_tail_lines_cached(TranscriptTailRenderCache& cache, std::vector<TranscriptItem> const& transcript,
                                                                           std::size_t transcript_generation, std::size_t width, std::size_t max_tail_lines,
                                                                           ToolPresentation tool_presentation = ToolPresentation::Rich,
                                                                           bool thinking_visible = true, bool compact_spacing = false,
                                                                           MermaidTranscriptProjection const* mermaid_projection = nullptr);
[[nodiscard]] std::vector<std::size_t> transcript_message_start_lines(std::vector<TranscriptItem> const& transcript, std::size_t width,
                                                                      ToolPresentation tool_presentation = ToolPresentation::Rich, bool thinking_visible = true,
                                                                      bool compact_spacing = false,
                                                                      MermaidTranscriptProjection const* mermaid_projection = nullptr);
[[nodiscard]] std::vector<std::string> visible_transcript_lines(std::vector<std::string> const& rendered_transcript, std::size_t width,
                                                                std::size_t transcript_height, std::size_t transcript_scroll_offset);
void refresh_transcript_layout_cache(TranscriptLayoutCache& cache, std::vector<TranscriptItem> const& transcript, std::size_t transcript_generation,
                                     std::size_t width, ToolPresentation tool_presentation, bool thinking_visible, bool compact_spacing,
                                     MermaidTranscriptProjection const* mermaid_projection = nullptr);
[[nodiscard]] std::size_t cached_transcript_max_scroll_offset(TranscriptLayoutCache const& cache, std::size_t transcript_height);
[[nodiscard]] std::vector<std::string> cached_visible_transcript_lines(TranscriptLayoutCache const& cache, std::size_t transcript_height,
                                                                       std::size_t transcript_scroll_offset);

struct TranscriptPositionIndicatorGeometry
{
  std::size_t thumb_start = 0;
  std::size_t thumb_length = 0;
  bool visible = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Computes a top-relative proportional thumb from the authoritative full layout
// and bottom-relative transcript scroll offset.
[[nodiscard]] TranscriptPositionIndicatorGeometry transcript_position_indicator_geometry(std::size_t layout_lines, std::size_t transcript_height,
                                                                                         std::size_t transcript_scroll_offset) noexcept;
// Styles the existing right-edge cell on thumb rows only. Plain output is unchanged.
void apply_transcript_position_indicator_overlay(std::vector<std::string>& visible_lines, std::size_t content_width,
                                                 TranscriptPositionIndicatorGeometry geometry, bool plain_output);

// Compatibility overloads for focused renderer callers that still express the former collapsed/expanded switch.
inline ToolPresentation legacy_tool_presentation(bool expanded)
{
  return expanded ? ToolPresentation::Expanded : ToolPresentation::Compact;
}
inline TranscriptLayout render_transcript_layout(std::vector<TranscriptItem> const& transcript, std::size_t width, bool expanded, bool thinking_visible = true,
                                                 bool compact_spacing = false)
{
  return render_transcript_layout(transcript, width, legacy_tool_presentation(expanded), thinking_visible, compact_spacing);
}
inline std::vector<std::string> render_transcript_lines(std::vector<TranscriptItem> const& transcript, std::size_t width, bool expanded,
                                                        bool thinking_visible = true, bool compact_spacing = false)
{
  return render_transcript_lines(transcript, width, legacy_tool_presentation(expanded), thinking_visible, compact_spacing);
}
inline std::vector<std::string> render_transcript_tail_lines(std::vector<TranscriptItem> const& transcript, std::size_t width, std::size_t max_tail_lines,
                                                             bool expanded, bool thinking_visible = true, bool compact_spacing = false)
{
  return render_transcript_tail_lines(transcript, width, max_tail_lines, legacy_tool_presentation(expanded), thinking_visible, compact_spacing);
}
inline std::vector<std::string> render_transcript_tail_lines_cached(TranscriptTailRenderCache& cache, std::vector<TranscriptItem> const& transcript,
                                                                    std::size_t generation, std::size_t width, std::size_t max_tail_lines, bool expanded,
                                                                    bool thinking_visible = true, bool compact_spacing = false)
{
  return render_transcript_tail_lines_cached(cache, transcript, generation, width, max_tail_lines, legacy_tool_presentation(expanded), thinking_visible,
                                             compact_spacing);
}
inline std::vector<std::size_t> transcript_message_start_lines(std::vector<TranscriptItem> const& transcript, std::size_t width, bool expanded,
                                                               bool thinking_visible = true, bool compact_spacing = false)
{
  return transcript_message_start_lines(transcript, width, legacy_tool_presentation(expanded), thinking_visible, compact_spacing);
}
inline void refresh_transcript_layout_cache(TranscriptLayoutCache& cache, std::vector<TranscriptItem> const& transcript, std::size_t generation,
                                            std::size_t width, bool expanded, bool thinking_visible, bool compact_spacing)
{
  refresh_transcript_layout_cache(cache, transcript, generation, width, legacy_tool_presentation(expanded), thinking_visible, compact_spacing);
}

[[nodiscard]] ComposerFrame render_composer_frame_cached(ComposerSnapshot const& snapshot, CompletionMatchCache& completion_cache, std::size_t source_revision,
                                                         TranscriptLayoutCache* transcript_cache, std::size_t transcript_generation, bool center_canvas = true,
                                                         bool allow_transcript_gap = true, bool freeze_transcript_layout = false,
                                                         bool allow_frozen_width_mismatch = false);
[[nodiscard]] std::optional<ComposerPaletteScreenLayout> composer_palette_screen_layout_cached(ComposerSnapshot const& snapshot,
                                                                                               CompletionMatchCache& completion_cache,
                                                                                               std::size_t source_revision);
[[nodiscard]] std::size_t composer_max_transcript_scroll_offset_cached(ComposerSnapshot const& snapshot, std::size_t width, std::size_t height,
                                                                       CompletionMatchCache& completion_cache, std::size_t source_revision,
                                                                       TranscriptLayoutCache& transcript_cache, std::size_t transcript_generation,
                                                                       bool allow_transcript_gap = true);
[[nodiscard]] std::optional<std::size_t> file_reference_palette_selection_for_screen_position_cached(ComposerSnapshot const& snapshot, std::size_t row,
                                                                                                     std::size_t column, CompletionMatchCache& cache,
                                                                                                     std::size_t source_revision);
[[nodiscard]] std::optional<std::size_t> path_completion_palette_selection_for_screen_position_cached(ComposerSnapshot const& snapshot, std::size_t row,
                                                                                                      std::size_t column, CompletionMatchCache& cache,
                                                                                                      std::size_t source_revision);
[[nodiscard]] bool draw_screen_cached(ComposerSnapshot const& snapshot, CompletionMatchCache& completion_cache, std::size_t source_revision,
                                      TranscriptLayoutCache& transcript_cache, std::size_t transcript_generation, ScreenRowCache& screen_cache,
                                      bool freeze_transcript_layout = false, bool allow_frozen_width_mismatch = false);
[[nodiscard]] bool draw_processing_footer_cached(ComposerSnapshot const& snapshot, CompletionMatchCache& completion_cache, std::size_t source_revision,
                                                 TranscriptLayoutCache& transcript_cache, std::size_t transcript_generation, ScreenRowCache& screen_cache);
void clear_composer_terminal_graphics(terminal::Context& terminal_context) noexcept;

[[nodiscard]] std::size_t composer_input_prefix_columns(bool first_line);
[[nodiscard]] std::string render_composer_footer_line(ComposerSnapshot const& snapshot, std::size_t width);
[[nodiscard]] std::vector<std::string> input_render_lines(std::string_view input);
[[nodiscard]] std::vector<ComposerInputRenderLine> input_render_line_spans(std::string_view input, std::size_t width);
[[nodiscard]] std::size_t composer_block_line_count(ComposerSnapshot const& snapshot, std::size_t height);
[[nodiscard]] std::size_t composer_block_line_count(ComposerSnapshot const& snapshot, std::size_t height, std::size_t width);
[[nodiscard]] ComposerInputLayout composer_input_layout(std::size_t input_line_count, std::size_t max_lines, std::size_t draft_scroll_offset,
                                                        std::size_t requested_top_padding_lines);
[[nodiscard]] std::vector<std::string> render_composer_block(ComposerSnapshot const& snapshot, std::size_t width, std::size_t max_lines);
[[nodiscard]] std::size_t input_cursor_column(ComposerSnapshot const& snapshot, std::size_t width);
[[nodiscard]] std::size_t input_cursor_line(ComposerSnapshot const& snapshot, std::size_t width);

}  // namespace detail
}  // namespace ava::tui
