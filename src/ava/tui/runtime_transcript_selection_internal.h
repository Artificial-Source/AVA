#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/terminal.h"
#include "ava/core/NoLogString.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tui {

// Content-relative transcript selection endpoints.
//
// Origin contract:
// - `item_index` is the transcript item index owned by the authoritative
//   TranscriptLayout entry (`message_item_indices`).
// - `line_offset` is measured from that item's `message_starts` row in the same
//   layout (the first layout line belonging to the item). Context-group headings,
//   tool/thinking headers, markdown/code/tool body rows, and blank spacer rows
//   that belong to the item are included. Inter-item roomy spacers that sit
//   *before* `message_starts` are unowned and cannot host endpoints.
// - `display_column` is the cluster-aligned plain display column within that
//   rendered row after stripping SGR/OSC/control sequences (0 = first plain
//   column). Wide glyphs snap to cluster boundaries (half-column hit chooses the
//   nearer edge). Combining marks, ZWJ emoji, and regional-indicator pairs use
//   the same compact terminal cluster rules as rendering.
//
// Selection copies authoritative rendered PLAIN transcript rows (SGR/OSC/control
// stripped). Soft-wrapped rows join with '\n'. Existing `/copy*` paths remain
// semantic/full-fidelity and are intentionally separate.
struct TranscriptSelectionEndpoint
{
  std::size_t item_index = 0;
  std::size_t line_offset = 0;
  std::size_t display_column = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TranscriptSelectionRange
{
  TranscriptSelectionEndpoint anchor = {};
  TranscriptSelectionEndpoint focus = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TranscriptSelectionExtractResult
{
  ava::core::NoLogString text = {};
  // True when construction stopped after observing more than the OSC52 ceiling
  // (kMaxTerminalClipboardTextBytes + 1). Callers must not copy oversize text.
  bool oversize = false;
  std::size_t examined_rows = 0;

  // Contains selected transcript text and must never be debug-streamed.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TranscriptSelectionHit
{
  TranscriptSelectionEndpoint endpoint = {};
  std::size_t absolute_line = 0;
  bool on_header = false;
  bool tool_header = false;
  bool thinking_header = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TranscriptSelectionUnit
{
  TranscriptSelectionEndpoint start = {};
  TranscriptSelectionEndpoint end = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TranscriptSelectionViewport
{
  std::size_t transcript_height = 0;
  std::size_t content_width = 0;
  std::size_t canvas_left = 0;
  std::size_t max_scroll_offset = 0;
  std::size_t scroll_offset = 0;
  std::size_t visible_start = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Map a frozen/pre-shift transcript item index through a cap/eviction shift.
// Fail closed on overflow or leading eviction of the source index.
[[nodiscard]] std::optional<std::size_t> shift_transcript_selection_item_index(std::size_t item_index, std::ptrdiff_t item_index_shift) noexcept;

// Pure geometry/extract helpers (layout-authority only; no snapshot mutation).
[[nodiscard]] std::string transcript_selection_plain_row(std::string_view styled_line);
[[nodiscard]] std::size_t transcript_selection_plain_columns(std::string_view plain_row);
[[nodiscard]] std::size_t snap_display_column(std::string_view plain_row, std::size_t display_column, bool prefer_end_on_half = false);
[[nodiscard]] std::optional<std::size_t> absolute_line_for_endpoint(detail::TranscriptLayout const& layout, TranscriptSelectionEndpoint const& endpoint);
[[nodiscard]] std::optional<TranscriptSelectionEndpoint> endpoint_for_absolute_line(detail::TranscriptLayout const& layout, std::size_t absolute_line,
                                                                                    std::size_t display_column);
[[nodiscard]] std::optional<TranscriptSelectionUnit> transcript_word_selection_unit(detail::TranscriptLayout const& layout,
                                                                                    TranscriptSelectionEndpoint const& endpoint);
[[nodiscard]] std::optional<TranscriptSelectionUnit> transcript_line_selection_unit(detail::TranscriptLayout const& layout,
                                                                                    TranscriptSelectionEndpoint const& endpoint);
[[nodiscard]] bool endpoint_less(TranscriptSelectionEndpoint const& left, TranscriptSelectionEndpoint const& right, detail::TranscriptLayout const& layout);
[[nodiscard]] std::pair<TranscriptSelectionEndpoint, TranscriptSelectionEndpoint> ordered_endpoints(TranscriptSelectionRange const& range,
                                                                                                    detail::TranscriptLayout const& layout);
[[nodiscard]] TranscriptSelectionExtractResult extract_transcript_selection_text(detail::TranscriptLayout const& layout, TranscriptSelectionRange const& range,
                                                                                 std::size_t max_bytes);
[[nodiscard]] std::string apply_transcript_selection_highlight(std::string_view styled_line, std::size_t column_start, std::size_t column_end,
                                                               bool plain_output);
void apply_transcript_selection_overlay(std::vector<std::string>& visible_lines, detail::TranscriptLayout const& layout, TranscriptSelectionRange const& range,
                                        std::size_t visible_start, bool plain_output);

enum class TranscriptSelectionMouseResult
{
  Ignored,
  Handled,
  HandledNeedsRender,
};

// One narrow owner shared by idle and active-run. Draft and transcript
// selections are mutually exclusive: starting one clears the other at the call
// site.
class RuntimeTranscriptSelectionState final
{
 public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto kMultiClickInterval = std::chrono::milliseconds(500);
  static constexpr auto kEdgeAutoscrollInterval = std::chrono::milliseconds(50);
  void clear() noexcept;
  // Ends Selecting/HeaderArmed without discarding a committed range. Used for
  // Shift-modified reports, suspend/editor handoff, and mouse protocol boundaries.
  void cancel_pointer_interaction() noexcept;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] bool dragging() const noexcept;
  [[nodiscard]] bool has_click_chain() const noexcept;
  [[nodiscard]] std::optional<TranscriptSelectionRange> range() const noexcept;
  void publish(ComposerSnapshot& snapshot) const;

  // Bind or rebind the layout authority currently drawn. Clears fail-closed when
  // the prior selection cannot be remapped into the new authority.
  void rebind_authority(detail::TranscriptLayout const& layout, std::size_t layout_generation, std::size_t width, ToolPresentation tool_presentation,
                        bool thinking_visible, bool compact_spacing, ComposerSnapshot const* snapshot = nullptr);
  void apply_item_index_shift(std::ptrdiff_t item_index_shift, detail::TranscriptLayout const& layout);
  void remap_or_clear(detail::TranscriptLayout const& layout);

  // Adopt only the renderer-owned cache that was actually drawn. This never
  // refreshes or mutates the cache; invalid authority fails closed.
  [[nodiscard]] bool ensure_authority(detail::TranscriptLayoutCache const& layout_cache, ComposerSnapshot const* snapshot = nullptr);

  // `frozen_to_live_item_index_shift` maps frozen detached layout item indices onto
  // the live transcript (accumulated deferred cap shift). Geometry/hit-testing keep
  // the frozen authority; only snapshot lookups and header toggles apply the shift.
  // Pass 0 when the layout authority is already live. Fail closed on overflow/eviction.
  [[nodiscard]] TranscriptSelectionMouseResult handle_mouse(InputEvent const& event, ComposerSnapshot& snapshot,
                                                            detail::TranscriptLayoutCache const& layout_cache, RuntimeDraftState* draft_state,
                                                            std::size_t& transcript_scroll_offset, std::ptrdiff_t frozen_to_live_item_index_shift,
                                                            std::function<bool(std::size_t)> const& toggle_tool,
                                                            std::function<bool(std::size_t)> const& toggle_thinking, Clock::time_point now = Clock::now());
  [[nodiscard]] std::optional<Clock::duration> time_until_edge_autoscroll(Clock::time_point now = Clock::now()) const;
  [[nodiscard]] TranscriptSelectionMouseResult tick_edge_autoscroll(ComposerSnapshot& snapshot, detail::TranscriptLayoutCache const& layout_cache,
                                                                    std::size_t& transcript_scroll_offset, std::ptrdiff_t frozen_to_live_item_index_shift,
                                                                    Clock::time_point now = Clock::now());

  [[nodiscard]] bool copy_selection(ComposerSnapshot& snapshot, detail::TranscriptLayoutCache const& layout_cache);

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct ItemSourceAuthority
  {
    ava::core::NoLogString label = {};
    ava::core::NoLogString text = {};
    ava::core::NoLogString meta = {};
    ava::core::NoLogString thinking = {};
    std::string stream_id = {};
    bool append_only_stream = false;
    bool tool = false;
    std::string tool_name = {};
    std::string tool_call_id = {};
    std::string tool_request_id = {};
    std::string tool_correlation_id = {};
    ava::core::NoLogString tool_arguments = {};

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  enum class DragKind
  {
    None,
    Selecting,
    HeaderArmed,
  };

  enum class SelectionGranularity
  {
    Character,
    Word,
    Line,
  };

  struct ClickChain
  {
    TranscriptSelectionUnit word = {};
    std::size_t count = 0;
    Clock::time_point completed_at = {};
    std::optional<ItemSourceAuthority> source_authority = std::nullopt;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  [[nodiscard]] bool has_compatible_authority(detail::TranscriptLayoutCache const& cache) const noexcept;
  [[nodiscard]] std::optional<TranscriptSelectionViewport> viewport_for(ComposerSnapshot const& snapshot, detail::TranscriptLayoutCache const& cache) const;
  [[nodiscard]] std::optional<TranscriptSelectionHit> hit_test(ComposerSnapshot const& snapshot, detail::TranscriptLayoutCache const& cache, std::size_t row,
                                                               std::size_t column, std::ptrdiff_t frozen_to_live_item_index_shift) const;
  [[nodiscard]] static std::optional<ItemSourceAuthority> source_authority(ComposerSnapshot const& snapshot, std::size_t item_index);
  [[nodiscard]] static bool source_authority_compatible(ItemSourceAuthority const& previous, ItemSourceAuthority const& current);
  [[nodiscard]] bool refresh_source_authorities_or_clear(ComposerSnapshot const& snapshot);
  void begin_selection(TranscriptSelectionEndpoint const& endpoint, ComposerSnapshot const& snapshot, RuntimeDraftState* draft_state,
                       std::ptrdiff_t frozen_to_live_item_index_shift);
  void begin_granular_selection(TranscriptSelectionUnit const& unit, SelectionGranularity granularity, ComposerSnapshot const& snapshot,
                                RuntimeDraftState* draft_state, std::ptrdiff_t frozen_to_live_item_index_shift);
  void extend_selection(TranscriptSelectionEndpoint const& endpoint, ComposerSnapshot const& snapshot, std::ptrdiff_t frozen_to_live_item_index_shift);
  void extend_granular_selection(TranscriptSelectionUnit const& unit, ComposerSnapshot const& snapshot, std::ptrdiff_t frozen_to_live_item_index_shift,
                                 detail::TranscriptLayout const& layout);
  [[nodiscard]] std::optional<TranscriptSelectionUnit> selection_unit_for_hit(TranscriptSelectionHit const& hit, detail::TranscriptLayout const& layout,
                                                                              SelectionGranularity granularity) const;
  [[nodiscard]] std::size_t next_click_count(TranscriptSelectionUnit const& word, ComposerSnapshot const& snapshot,
                                             std::ptrdiff_t frozen_to_live_item_index_shift, Clock::time_point now) const;
  void commit_click(TranscriptSelectionUnit const& word, std::size_t count, ComposerSnapshot const& snapshot, std::ptrdiff_t frozen_to_live_item_index_shift,
                    Clock::time_point now);
  void reset_click_chain() noexcept;
  void reset_granular_drag() noexcept;
  void arm_header(TranscriptSelectionEndpoint endpoint, bool tool_header, bool thinking_header, ComposerSnapshot const& snapshot,
                  std::ptrdiff_t frozen_to_live_item_index_shift);
  [[nodiscard]] bool finish_header_click(ComposerSnapshot const& snapshot, std::ptrdiff_t frozen_to_live_item_index_shift,
                                         std::function<bool(std::size_t)> const& toggle_tool, std::function<bool(std::size_t)> const& toggle_thinking);
  [[nodiscard]] bool toggle_header_at_frozen_item(ComposerSnapshot const& snapshot, std::size_t frozen_item_index, bool tool_header, bool thinking_header,
                                                  std::ptrdiff_t frozen_to_live_item_index_shift, std::function<bool(std::size_t)> const& toggle_tool,
                                                  std::function<bool(std::size_t)> const& toggle_thinking) const;
  [[nodiscard]] bool autoscroll_for_row(ComposerSnapshot& snapshot, TranscriptSelectionViewport const& viewport, std::size_t screen_row,
                                        std::size_t& transcript_scroll_offset);
  [[nodiscard]] bool can_autoscroll_for_row(TranscriptSelectionViewport const& viewport, std::size_t screen_row, std::size_t transcript_scroll_offset) const;
  void update_edge_autoscroll(TranscriptSelectionViewport const& viewport, InputEvent const& event, std::size_t transcript_scroll_offset,
                              Clock::time_point now);

  std::optional<TranscriptSelectionRange> range_ = std::nullopt;
  std::optional<ItemSourceAuthority> anchor_source_authority_ = std::nullopt;
  std::optional<ItemSourceAuthority> focus_source_authority_ = std::nullopt;
  DragKind drag_ = DragKind::None;
  std::optional<std::size_t> armed_header_item_ = std::nullopt;
  std::optional<TranscriptSelectionEndpoint> armed_press_endpoint_ = std::nullopt;
  std::optional<ItemSourceAuthority> armed_source_authority_ = std::nullopt;
  bool armed_tool_header_ = false;
  bool armed_thinking_header_ = false;
  SelectionGranularity granularity_ = SelectionGranularity::Character;
  std::optional<TranscriptSelectionUnit> granular_anchor_ = std::nullopt;
  std::optional<ClickChain> click_chain_ = std::nullopt;
  std::optional<TranscriptSelectionUnit> pending_click_word_ = std::nullopt;
  std::size_t pending_click_count_ = 0;
  bool pointer_moved_ = false;
  std::optional<Clock::time_point> edge_autoscroll_due_ = std::nullopt;
  std::size_t edge_mouse_row_ = 0;
  std::size_t edge_mouse_column_ = 0;
  std::size_t authority_generation_ = 0;
  std::size_t authority_width_ = 0;
  ToolPresentation authority_tool_presentation_ = ToolPresentation::Rich;
  bool authority_thinking_visible_ = true;
  bool authority_compact_spacing_ = false;
  bool authority_valid_ = false;
};

}  // namespace ava::tui
