// Defines selection geometry and the TUI-owned interaction state bound to the renderer's retained layout.
#pragma once

#include "ava/debug/print_members_on.h"
#include "ava/tui/Composer.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_internal.h"
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

class RuntimePresentationState;

// Content-relative transcript selection endpoints.
//
// Origin contract:
// - `item_index` is the transcript item index recorded by the supplied
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
// Selection copies the supplied cache's rendered PLAIN transcript rows (SGR/OSC/control
// stripped). Soft-wrapped rows join with '\n'. Existing `/copy*` paths remain
// semantic/full-fidelity and are intentionally separate.
struct TranscriptSelectionEndpoint
{
  // TUI-thread item index from the supplied layout; apply eviction shifts before live transcript lookup.
  std::size_t item_index = 0;
  // TUI-thread rendered row within the item; reflow remaps/clamps it against the current layout.
  std::size_t line_offset = 0;
  // TUI-thread cluster-aligned plain display-cell boundary, shared by hit-testing and copying.
  std::size_t display_column = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Owned TUI-thread endpoint pair. Drag direction is retained here; extraction helpers order copies against one
// immutable layout.
struct TranscriptSelectionRange
{
  // TUI-thread fixed selection origin, or opposite unit edge when a granular drag reverses.
  TranscriptSelectionEndpoint anchor = {};
  // TUI-thread moving edge; may precede anchor in rendered row order.
  TranscriptSelectionEndpoint focus = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Owned bounded extraction result consumed by TUI clipboard code. It retains no layout references; oversize
// prohibits submission.
struct TranscriptSelectionExtractResult
{
  // Owned plain rendered selection from bounded extraction; oversize prohibits clipboard submission.
  ava::core::NoLogString text = {};
  // True when construction stopped after observing more than the OSC52 ceiling
  // (kMaxTerminalClipboardTextBytes + 1). Callers must not copy oversize text.
  bool oversize = false;
  // Extraction diagnostic counting visited rows to verify output limits also bound traversal.
  std::size_t examined_rows = 0;

  // Contains selected transcript text and must never be debug-streamed.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Temporary TUI-thread hit against the supplied layout. Consumers must map its frozen item before live header/source
// lookup.
struct TranscriptSelectionHit
{
  // Owned content-relative hit snapped to a complete display cluster.
  TranscriptSelectionEndpoint endpoint = {};
  // Row in the supplied layout; valid only while that layout remains unchanged.
  std::size_t absolute_line = 0;
  // Marks header action precedence over body multi-click selection.
  bool on_header = false;
  // Marks a potential tool expansion; live mapped item must be revalidated before callback.
  bool tool_header = false;
  // Marks a potential thinking expansion; evicted/replaced source fails closed.
  bool thinking_header = false;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Owned TUI-thread bounds of one word/rendered row. Granular dragging preserves the complete initial unit.
struct TranscriptSelectionUnit
{
  // Inclusive cluster boundary used when ordering extraction or extending backward.
  TranscriptSelectionEndpoint start = {};
  // Exclusive cluster boundary keeping the complete word/row selected.
  TranscriptSelectionEndpoint end = {};

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Temporary TUI-thread geometry copied from screen plus the supplied cache; recompute after scrolling/layout changes.
struct TranscriptSelectionViewport
{
  // Visible transcript row budget after reserving space for editor, modal, and status rows.
  std::size_t transcript_height = 0;
  // Plain content width used to clamp edge-drag columns.
  std::size_t content_width = 0;
  // Screen origin translating terminal pointer columns to content coordinates.
  std::size_t canvas_left = 0;
  // History limit derived from layout row count and visible height.
  std::size_t max_scroll_offset = 0;
  // Clamped history distance sampled for this geometry calculation.
  std::size_t scroll_offset = 0;
  // First layout row selected by the current scroll position; maps visible hits/highlights to layout rows.
  std::size_t visible_start = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Map a frozen/pre-shift transcript item index through a cap/eviction shift.
// Fail closed on overflow or leading eviction of the source index.
[[nodiscard]] std::optional<std::size_t> shift_transcript_selection_item_index(std::size_t item_index, std::ptrdiff_t item_index_shift) noexcept;

// Pure helper on caller-owned text: remove terminal controls before copying/hit-testing; return owned plain
// text.
[[nodiscard]] std::string transcript_selection_plain_row(std::string_view styled_line);
// Pure helper: measure plain-row terminal clusters with renderer-compatible width rules.
[[nodiscard]] std::size_t transcript_selection_plain_columns(std::string_view plain_row);
// Pure helper: clamp to a whole display cluster; caller chooses half-width tie direction.
[[nodiscard]] std::size_t snap_display_column(std::string_view plain_row, std::size_t display_column, bool prefer_end_on_half = false);
// Pure helper on stable layout: reject missing/unowned endpoints instead of guessing a row.
[[nodiscard]] std::optional<std::size_t> absolute_line_for_endpoint(detail::TranscriptLayout const& layout, TranscriptSelectionEndpoint const& endpoint);
// Pure helper: find row ownership and snap column; unowned inter-item spacers cannot host endpoints.
[[nodiscard]] std::optional<TranscriptSelectionEndpoint> endpoint_for_absolute_line(detail::TranscriptLayout const& layout, std::size_t absolute_line,
                                                                                    std::size_t display_column);
// Pure helper: segment plain clusters from the layout and return the word containing the hit.
[[nodiscard]] std::optional<TranscriptSelectionUnit> transcript_word_selection_unit(detail::TranscriptLayout const& layout,
                                                                                    TranscriptSelectionEndpoint const& endpoint);
// Pure helper: return the whole rendered row excluding invisible styling.
[[nodiscard]] std::optional<TranscriptSelectionUnit> transcript_line_selection_unit(detail::TranscriptLayout const& layout,
                                                                                    TranscriptSelectionEndpoint const& endpoint);
// Pure helper: compare endpoint order using the same layout.
[[nodiscard]] bool endpoint_less(TranscriptSelectionEndpoint const& left, TranscriptSelectionEndpoint const& right, detail::TranscriptLayout const& layout);
// Pure helper: copy endpoints in rendered order without changing the owner's drag direction.
[[nodiscard]] std::pair<TranscriptSelectionEndpoint, TranscriptSelectionEndpoint> ordered_endpoints(TranscriptSelectionRange const& range,
                                                                                                    detail::TranscriptLayout const& layout);
// Pure bounded helper on stable layout: join selected plain rows; oversize stops construction and prohibits
// clipboard use.
[[nodiscard]] TranscriptSelectionExtractResult extract_transcript_selection_text(detail::TranscriptLayout const& layout, TranscriptSelectionRange const& range,
                                                                                 std::size_t max_bytes);
// Pure helper: overlay cluster-aligned selection while preserving allowed styling; return an owned row.
[[nodiscard]] std::string apply_transcript_selection_highlight(std::string_view styled_line, std::size_t column_start, std::size_t column_end,
                                                               bool plain_output);
// TUI paint helper: alter visible row copies only, preserving the cached rows for subsequent hit-testing.
void apply_transcript_selection_overlay(std::vector<std::string>& visible_lines, detail::TranscriptLayout const& layout, TranscriptSelectionRange const& range,
                                        std::size_t visible_start, bool plain_output);

// TUI-thread dispatch result separates pointer ownership from frame invalidation.
enum class TranscriptSelectionMouseResult
{
  // Later draft/pointer handlers may handle the event.
  Ignored,
  // Input was consumed without a visible projection change.
  Handled,
  // Input changed highlight, status, or viewport and needs a frame.
  HandledNeedsRender,
};

// TranscriptView owns this TUI-thread interaction state for idle and active-run input and supplies the
// renderer's layout. Arguments are borrowed only for the call; owned source copies distinguish retained text
// from replacement content. Starting a draft or transcript selection clears the other at the call site. The
// renderer schedules copied deadlines and retires its callbacks before the view dies; this state owns no timer.
class RuntimeTranscriptSelectionState final
{
 public:
  // Monotonic TUI-thread clock for multi-click age and edge-drag cadence.
  using Clock = std::chrono::steady_clock;
  // Maximum interval between completed clicks on the same source word before restarting the chain.
  static constexpr auto kMultiClickInterval = std::chrono::milliseconds(500);
  // Cadence exposed only while an active drag can move at a viewport edge.
  static constexpr auto kEdgeAutoscrollInterval = std::chrono::milliseconds(50);
  // TUI thread: retire range, source copies, drag, and click chain; caller publishes the cleared highlight.
  void clear() noexcept;
  // Ends Selecting/HeaderArmed without discarding a committed range. Used for
  // Shift-modified reports, suspend/editor handoff, and mouse protocol boundaries.
  void cancel_pointer_interaction() noexcept;
  // TUI thread: test committed range; an armed header/click chain may still retain item indices.
  [[nodiscard]] bool empty() const noexcept;
  // TUI thread: include HeaderArmed so cancellation also retires uncommitted header actions.
  [[nodiscard]] bool dragging() const noexcept;
  // TUI thread: retain shift tracking for completed/pending clicks even without a visible range.
  [[nodiscard]] bool has_click_chain() const noexcept;
  // TUI thread: copy endpoints; validate layout/source before extraction or highlighting.
  [[nodiscard]] std::optional<TranscriptSelectionRange> range() const noexcept;
  // TUI thread through presentation state: copy validated endpoints and layout settings into screen data;
  // selection remains owned here.
  void publish(ComposerSnapshot& snapshot) const;

  // TUI thread: record the supplied layout's generation and display settings, validate source copies when
  // a snapshot is supplied, then remap endpoints. Clear a range whose source or endpoints no longer match.
  void rebind_authority(detail::TranscriptLayout const& layout, std::size_t layout_generation, std::size_t width, ToolPresentation tool_presentation,
                        bool thinking_visible, bool compact_spacing, ComposerSnapshot const* snapshot = nullptr);
  // TUI thread after eviction: shift range, header, and click/granular units together; clear state whose source
  // was evicted.
  void apply_item_index_shift(std::ptrdiff_t item_index_shift, detail::TranscriptLayout const& layout);
  // TUI thread: remap both endpoints in supplied layout; losing either retires the range.
  void remap_or_clear(detail::TranscriptLayout const& layout);

  // TUI thread: use the supplied renderer cache without rebuilding it. An invalid cache clears selection
  // and returns false. A valid cache records changed generation/settings and remaps endpoints; true means
  // layout coordinates are usable, even if remapping cleared the range.
  [[nodiscard]] bool ensure_authority(detail::TranscriptLayoutCache const& layout_cache, ComposerSnapshot const* snapshot = nullptr);

  // `frozen_to_live_item_index_shift` maps frozen detached layout item indices onto
  // the live transcript (accumulated deferred cap shift). Geometry/hit-testing keep
  // frozen layout; only snapshot lookups and header toggles apply the shift.
  // Pass 0 when layout indices already match live items. Reject overflow or an evicted source.
  [[nodiscard]] TranscriptSelectionMouseResult handle_mouse(InputEvent const& event, RuntimePresentationState& presentation,
                                                            detail::TranscriptLayoutCache const& layout_cache, Composer* draft_state,
                                                            std::size_t& transcript_scroll_offset, std::ptrdiff_t frozen_to_live_item_index_shift,
                                                            std::function<bool(std::size_t)> const& toggle_tool,
                                                            std::function<bool(std::size_t)> const& toggle_thinking, Clock::time_point now = Clock::now());
  // TUI thread: copy next held-drag deadline for renderer registration; absence cancels its wake.
  [[nodiscard]] std::optional<Clock::time_point> edge_autoscroll_deadline() const noexcept;
  // TUI timer servicing: check modal visibility and layout compatibility, move one row, extend selection, and arm only the next
  // necessary step.
  [[nodiscard]] TranscriptSelectionMouseResult tick_edge_autoscroll(RuntimePresentationState& presentation, detail::TranscriptLayoutCache const& layout_cache,
                                                                    std::size_t& transcript_scroll_offset, std::ptrdiff_t frozen_to_live_item_index_shift,
                                                                    Clock::time_point now = Clock::now());

  // TUI thread: require a valid cache with the bound generation, width, tool style, thinking visibility, and
  // spacing. Copy nonempty text within the clipboard byte limit; missing/mismatched/oversize selections copy
  // nothing and report failure. Success preserves selection.
  [[nodiscard]] bool copy_selection(RuntimePresentationState& presentation, detail::TranscriptLayoutCache const& layout_cache);

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Owned TUI-thread source fingerprint retained with endpoints/header/click chain. Exact content or stable
  // append-only stream/tool identity distinguishes growth from replacement. No layout or message references
  // survive.
  struct ItemSourceAuthority
  {
    // Captured role/label participates in exact non-tool source comparison.
    ava::core::NoLogString label = {};
    // Captured TUI-thread source body; exact equality or a proven stream prefix preserves endpoint identity.
    ava::core::NoLogString text = {};
    // Captured metadata rejects a replacement that happens to occupy the same item index.
    ava::core::NoLogString meta = {};
    // Captured thinking prefix permits proven append-only stream growth.
    ava::core::NoLogString thinking = {};
    // Stable stream identity required before changed text can preserve the selected source.
    std::string stream_id = {};
    // Current source must promise append-only growth to extend captured text/thinking prefixes.
    bool append_only_stream = false;
    // Captured item kind prevents silently rebinding a tool selection to an ordinary message.
    bool tool = false;
    // Captured tool type checked with stable call/request/correlation identifiers.
    std::string tool_name = {};
    // Captured backend call identity permits output growth while rejecting a different call.
    std::string tool_call_id = {};
    // Captured request identity participates in stable tool-source comparison.
    std::string tool_request_id = {};
    // Captured correlation identity participates in stable tool-source comparison.
    std::string tool_correlation_id = {};
    // Fallback identity when stable tool IDs are absent; arguments remain protected from debug output.
    ava::core::NoLogString tool_arguments = {};

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  // TUI-thread exclusive pointer phase; header expansion waits until release distinguishes click from drag.
  enum class DragKind
  {
    // No live press owns motion/release, although a committed range may remain.
    None,
    // Motion extends selection and can expose an edge-scroll deadline.
    Selecting,
    // Press retains source/endpoint until release toggles it or motion starts selection.
    HeaderArmed,
  };

  // TUI-thread extension policy chosen by the completed click count.
  enum class SelectionGranularity
  {
    // Extend at snapped display-cluster boundaries.
    Character,
    // Extend by whole word units.
    Word,
    // Extend by whole rendered rows.
    Line,
  };

  // Owned TUI-thread completed-click context. Count advances only for the same source word within the interval;
  // motion or replacement retires it.
  struct ClickChain
  {
    // Cached word bounds remapped together with selection after eviction/reflow.
    TranscriptSelectionUnit word = {};
    // Completed count up to three; the next click then starts a fresh chain.
    std::size_t count = 0;
    // Completion time; a press alone does not renew the multi-click interval.
    Clock::time_point completed_at = {};
    // Owned source copy prevents repeated clicks on replacement content from extending the chain.
    std::optional<ItemSourceAuthority> source_authority = std::nullopt;

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

  // TUI thread: compare the bound generation/display settings before reusing coordinates without mutation.
  [[nodiscard]] bool has_compatible_authority(detail::TranscriptLayoutCache const& cache) const noexcept;
  // TUI thread: derive visible geometry from screen and valid cache; reject unavailable transcript bodies.
  [[nodiscard]] std::optional<TranscriptSelectionViewport> viewport_for(ComposerSnapshot const& snapshot, detail::TranscriptLayoutCache const& cache) const;
  // TUI thread: map pointer through cached rows, then map frozen indices to live items only for header
  // classification.
  [[nodiscard]] std::optional<TranscriptSelectionHit> hit_test(ComposerSnapshot const& snapshot, detail::TranscriptLayoutCache const& cache, std::size_t row,
                                                               std::size_t column, std::ptrdiff_t frozen_to_live_item_index_shift) const;
  // TUI thread: copy identity at a live index; absent means item was evicted or is out of range.
  [[nodiscard]] static std::optional<ItemSourceAuthority> source_authority(ComposerSnapshot const& snapshot, std::size_t item_index);
  // TUI thread: accept exact source or proven stream growth/stable tool identity; reject replacement.
  [[nodiscard]] static bool source_authority_compatible(ItemSourceAuthority const& previous, ItemSourceAuthority const& current);
  // TUI thread: refresh both endpoint source copies only when compatible; otherwise clear interaction.
  [[nodiscard]] bool refresh_source_authorities_or_clear(ComposerSnapshot const& snapshot);
  // TUI thread: clear draft selection, bind zero-width range, and capture live source through frozen-item
  // shift.
  void begin_selection(TranscriptSelectionEndpoint const& endpoint, ComposerSnapshot const& snapshot, Composer* draft_state,
                       std::ptrdiff_t frozen_to_live_item_index_shift);
  // TUI thread: clear draft selection and retain the complete initial word/line as drag anchor.
  void begin_granular_selection(TranscriptSelectionUnit const& unit, SelectionGranularity granularity, ComposerSnapshot const& snapshot, Composer* draft_state,
                                std::ptrdiff_t frozen_to_live_item_index_shift);
  // TUI thread: update the cached-layout focus and its mapped live source, retiring any armed header action.
  void extend_selection(TranscriptSelectionEndpoint const& endpoint, ComposerSnapshot const& snapshot, std::ptrdiff_t frozen_to_live_item_index_shift);
  // TUI thread: choose unit edges by direction so initial and target words/lines remain fully selected.
  void extend_granular_selection(TranscriptSelectionUnit const& unit, ComposerSnapshot const& snapshot, std::ptrdiff_t frozen_to_live_item_index_shift,
                                 detail::TranscriptLayout const& layout);
  // TUI thread: derive the requested unit from the same layout used by the hit.
  [[nodiscard]] std::optional<TranscriptSelectionUnit> selection_unit_for_hit(TranscriptSelectionHit const& hit, detail::TranscriptLayout const& layout,
                                                                              SelectionGranularity granularity) const;
  // TUI thread: check interval, remapped word, and current source before extending a chain.
  [[nodiscard]] std::size_t next_click_count(TranscriptSelectionUnit const& word, ComposerSnapshot const& snapshot,
                                             std::ptrdiff_t frozen_to_live_item_index_shift, Clock::time_point now) const;
  // TUI thread after completed release/click: retain count/time/source and retire provisional press data.
  void commit_click(TranscriptSelectionUnit const& word, std::size_t count, ComposerSnapshot const& snapshot, std::ptrdiff_t frozen_to_live_item_index_shift,
                    Clock::time_point now);
  // TUI thread: discard completed and pending click history so the next click starts fresh.
  void reset_click_chain() noexcept;
  // TUI thread: reset granularity and edge cadence without itself removing a committed range.
  void reset_granular_drag() noexcept;
  // TUI thread: retain press endpoint/source before toggling so motion can use the original press anchor.
  void arm_header(TranscriptSelectionEndpoint endpoint, bool tool_header, bool thinking_header, ComposerSnapshot const& snapshot,
                  std::ptrdiff_t frozen_to_live_item_index_shift);
  // TUI thread: retire armed state before synchronous callback, then validate mapped source to reject
  // replacement or eviction.
  [[nodiscard]] bool finish_header_click(ComposerSnapshot const& snapshot, std::ptrdiff_t frozen_to_live_item_index_shift,
                                         std::function<bool(std::size_t)> const& toggle_tool, std::function<bool(std::size_t)> const& toggle_thinking);
  // TUI thread: map frozen index through eviction and check live kind before invoking the synchronous toggle
  // callback.
  [[nodiscard]] bool toggle_header_at_frozen_item(ComposerSnapshot const& snapshot, std::size_t frozen_item_index, bool tool_header, bool thinking_header,
                                                  std::ptrdiff_t frozen_to_live_item_index_shift, std::function<bool(std::size_t)> const& toggle_tool,
                                                  std::function<bool(std::size_t)> const& toggle_thinking) const;
  // TUI thread: move one row at a scrollable edge and publish scroll before re-hit-testing.
  [[nodiscard]] bool autoscroll_for_row(RuntimePresentationState& presentation, TranscriptSelectionViewport const& viewport, std::size_t screen_row,
                                        std::size_t& transcript_scroll_offset);
  // TUI thread: check edge/bounds without mutation before scheduling a further drag step.
  [[nodiscard]] bool can_autoscroll_for_row(TranscriptSelectionViewport const& viewport, std::size_t screen_row, std::size_t transcript_scroll_offset) const;
  // TUI thread: retain pointer coordinates and expose a deadline only for a scrollable selecting drag.
  void update_edge_autoscroll(TranscriptSelectionViewport const& viewport, InputEvent const& event, std::size_t transcript_scroll_offset,
                              Clock::time_point now);

  // TUI-thread endpoints in the retained layout; clear on invalid source/layout or explicit retirement.
  std::optional<TranscriptSelectionRange> range_ = std::nullopt;
  // Owned TUI-thread anchor source; refreshed after valid remapping and cleared with range.
  std::optional<ItemSourceAuthority> anchor_source_authority_ = std::nullopt;
  // Owned TUI-thread moving-focus source; refreshed during drag and checked when rebinding to a changed layout.
  std::optional<ItemSourceAuthority> focus_source_authority_ = std::nullopt;
  // TUI-thread press/motion phase retired on release/cancel; range may outlive it.
  DragKind drag_ = DragKind::None;
  // TUI-thread frozen item retained from header press; shift before live toggle.
  std::optional<std::size_t> armed_header_item_ = std::nullopt;
  // TUI-thread original press anchor used when header becomes drag; remap or retire on layout change.
  std::optional<TranscriptSelectionEndpoint> armed_press_endpoint_ = std::nullopt;
  // Owned TUI-thread header source; checked before release executes a toggle.
  std::optional<ItemSourceAuthority> armed_source_authority_ = std::nullopt;
  // TUI-thread tool-action kind captured at press, cleared before invoking callback.
  bool armed_tool_header_ = false;
  // TUI-thread thinking-action kind captured at press, cleared on release/cancel.
  bool armed_thinking_header_ = false;
  // TUI-thread drag unit chosen by click count; release/reset restores character mode.
  SelectionGranularity granularity_ = SelectionGranularity::Character;
  // Owned TUI-thread initial word/line retained across direction changes and remapped after eviction.
  std::optional<TranscriptSelectionUnit> granular_anchor_ = std::nullopt;
  // TUI-thread completed click history, retained without a range and canceled by motion/source change.
  std::optional<ClickChain> click_chain_ = std::nullopt;
  // TUI-thread press word committed only when release completes a click without movement.
  std::optional<TranscriptSelectionUnit> pending_click_word_ = std::nullopt;
  // TUI-thread provisional count paired with pending word; cleared on completion/cancel/drag.
  std::size_t pending_click_count_ = 0;
  // TUI-thread motion evidence preventing a moved press/release from extending the click chain.
  bool pointer_moved_ = false;
  // TUI-thread next held-edge step; clearing it causes renderer to retire the corresponding timer.
  std::optional<Clock::time_point> edge_autoscroll_due_ = std::nullopt;
  // Last TUI-thread drag row reused when timer movement occurs without a new pointer report.
  std::size_t edge_mouse_row_ = 0;
  // Last TUI-thread drag column reused when extending selection after timer scroll.
  std::size_t edge_mouse_column_ = 0;
  // TUI-thread bound transcript generation; a changed generation requires remapping endpoints.
  std::size_t authority_generation_ = 0;
  // TUI-thread bound content width; reflow invalidates old row geometry.
  std::size_t authority_width_ = 0;
  // TUI-thread bound tool style; changing visible tool rows requires a rebind.
  ToolPresentation authority_tool_presentation_ = ToolPresentation::Rich;
  // TUI-thread bound thinking visibility; a change can move all following rows.
  bool authority_thinking_visible_ = true;
  // TUI-thread bound spacing policy; changes require remapping even with unchanged text.
  bool authority_compact_spacing_ = false;
  // TUI-thread validity latch cleared with invalid cache; valid rebind establishes reusable geometry.
  bool authority_valid_ = false;
};

}  // namespace ava::tui
