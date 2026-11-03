// Defines cached literal search projections and the selector controller that preserves transcript position.
#pragma once

#include "ava/tui/composer_internal.h"
#include "ava/core/NoLogString.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "debug.h"

namespace ava::tui {

class SelectorHost;
struct ComposerSnapshot;
struct InputEvent;
class RuntimeNavigationController;
class RuntimePresentationState;
class RuntimeRenderer;

namespace detail {

// Bound accepted literal queries and the matcher's fixed prefix table; validation rejects oversized input
// before use.
inline constexpr std::size_t kMaxTranscriptSearchQueryBytes = 1024;
// Bound each selector excerpt at a UTF-8 boundary so large messages cannot inflate modal rows.
inline constexpr std::size_t kMaxTranscriptSearchDetailBytes = 240;

// Owned TUI-thread result entry in chronological item order. Cache/controller retain it across modal redraws;
// eviction must realign item_index before navigation.
struct TranscriptSearchMatch
{
  // Current TUI-thread retained item index; cache realigns it after leading insertion/eviction.
  std::size_t item_index = 0;
  // Owned TUI-thread match label derived from source kind; displayed without retaining the transcript item.
  std::string identity;
  // Owned bounded plain excerpt around the match; protected because transcript content must not enter debug
  // output.
  ava::core::NoLogString detail;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Owned TUI-thread searchable rendering for one transcript item. Cache keeps one slot per live item;
// unavailable slots represent items with no searchable rows. It retains no layout/snapshot references.
struct TranscriptSearchProjection
{
  // TUI-thread build result: at least one nonprivate rendered row supplied searchable text.
  bool available = false;
  // TUI-thread classification identifying adjacent tool items whose shared context heading can change with
  // neighbors.
  bool context_gathering = false;
  // TUI-thread distance to the first item in a context-gathering run; incremental changes also rebuild that
  // heading owner.
  std::size_t context_run_offset = 0;
  // Owned TUI-thread match label derived from source kind; displayed without retaining the transcript item.
  std::string identity;
  // Owned plain rows joined with spaces so wrapped phrases remain matchable.
  ava::core::NoLogString searchable_text;
  // Owned alternative with row breaks removed so hard-wrapped tokens can still match literally.
  ava::core::NoLogString unspaced_searchable_text;
  // TUI-thread unspaced byte offsets used to map a token match back into the spaced excerpt source.
  std::vector<std::size_t> row_boundary_offsets;
  // Owned bounded first nonblank row shown when an empty query lists all available items.
  ava::core::NoLogString default_detail;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

// Owned TUI-thread incremental result telling the selector which unchanged prefix can remain in place.
struct TranscriptSearchUpdate
{
  // First match row that may differ; matches.size() means the existing prefix needs no row reconstruction.
  std::size_t first_changed_match_row = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Owned half-open TUI-thread item range used to merge affected search work without rebuilding unrelated items.
struct TranscriptSearchItemInterval
{
  // Inclusive affected item index after eviction/restoration realignment.
  std::size_t first = 0;
  // Exclusive affected bound; ranges are clamped before indexing current projections.
  std::size_t past_last = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Copied TUI-thread work counters for search behavior inspection. They contain no text or authority and can be
// consumed after the controller call.
struct TranscriptSearchDiagnostics
{
  // TUI-thread cumulative diagnostic: individual affected items rendered under captured authoritative settings.
  std::size_t authoritative_mutation_item_render_count = 0;
  // TUI-thread cumulative diagnostic: owned searchable projections constructed from rendered rows.
  std::size_t projection_build_count = 0;
  // TUI-thread cumulative diagnostic: authoritative layout/item positions visited while constructing projections.
  std::size_t layout_position_visit_count = 0;
  // TUI-thread cumulative diagnostic: cached projections evaluated by the literal matcher.
  std::size_t match_projection_evaluation_count = 0;
  // TUI-thread cumulative diagnostic: retained result indices shifted after transcript insertion/eviction.
  std::size_t match_entry_realign_count = 0;
  // TUI-thread cumulative diagnostic: result entries replaced or removed during incremental interval updates.
  std::size_t match_entry_splice_count = 0;
  // TUI-thread cumulative diagnostic: selector rows constructed, excluding unchanged retained prefixes.
  std::size_t modal_row_build_count = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// TranscriptSearchController owns this TUI-thread cache of rendered text and matches. All methods/members are
// TUI-thread-only; caller snapshots/layouts remain stable for each call. One projection slot corresponds to
// each retained item, matches stay ordered by item index, and no borrowed input survives. Clearing content
// preserves diagnostic counters.
class TranscriptSearchProjectionCache final
{
 public:
  // TUI thread after authoritative layout preparation: replace every item projection and rematch the retained
  // query.
  [[nodiscard]] TranscriptSearchUpdate rebuild_all(ComposerSnapshot const& snapshot, TranscriptLayout const& layout);
  // TUI thread at search/session retirement: release projections, query, and matches while retaining cumulative
  // work diagnostics.
  void clear();
  // TUI thread: accept a valid changed literal query and rematch cached text; invalid/equal query leaves rows
  // untouched.
  [[nodiscard]] TranscriptSearchUpdate update_query(std::string query);
  // TUI thread after transcript mutation with unchanged render settings: realign retained slots, rebuild
  // affected items/headings, and report changed match suffix.
  [[nodiscard]] TranscriptSearchUpdate refresh_after_transcript_mutation(ComposerSnapshot const& snapshot, std::size_t width,
                                                                         ToolPresentation tool_presentation, bool thinking_visible, bool compact_spacing,
                                                                         std::ptrdiff_t item_index_shift, std::size_t changed_from_item_index);
  // TUI thread: borrow chronological results until the next query/mutation/clear invalidates their storage.
  [[nodiscard]] std::vector<TranscriptSearchMatch> const& matches() const noexcept;
  // TUI thread: borrow accepted literal query until update_query/clear or cache destruction.
  [[nodiscard]] std::string const& query() const noexcept;
  // TUI-thread cumulative diagnostic: individual affected items rendered under captured authoritative settings.
  [[nodiscard]] std::size_t authoritative_mutation_item_render_count() const noexcept;
  // TUI-thread cumulative diagnostic: owned searchable projections constructed from rendered rows.
  [[nodiscard]] std::size_t projection_build_count() const noexcept;
  // TUI-thread cumulative diagnostic: authoritative layout/item positions visited while constructing projections.
  [[nodiscard]] std::size_t layout_position_visit_count() const noexcept;
  // TUI-thread cumulative diagnostic: cached projections evaluated by the literal matcher.
  [[nodiscard]] std::size_t match_projection_evaluation_count() const noexcept;
  // TUI-thread cumulative diagnostic: retained result indices shifted after transcript insertion/eviction.
  [[nodiscard]] std::size_t match_entry_realign_count() const noexcept;
  // TUI-thread cumulative diagnostic: result entries replaced or removed during incremental interval updates.
  [[nodiscard]] std::size_t match_entry_splice_count() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI thread with sized projections and stable layout: build only owned items in the requested half-open
  // interval.
  void rebuild_range(ComposerSnapshot const& snapshot, TranscriptLayout const& layout, std::size_t first_item_index, std::size_t past_last_item_index);
  // TUI thread: clear affected projection slots and recompute context-run ownership before rebuilding rendered
  // text.
  void reset_affected_metadata(ComposerSnapshot const& snapshot, std::vector<TranscriptSearchItemInterval> const& affected);
  // TUI thread: reset interval metadata then reuse authoritative rows instead of rerendering unchanged items.
  void rebuild_affected_from_layout(ComposerSnapshot const& snapshot, TranscriptLayout const& layout,
                                    std::vector<TranscriptSearchItemInterval> const& affected);
  // TUI thread with captured render settings: render only affected items when frozen modal layout must remain
  // untouched.
  void rebuild_affected_direct(ComposerSnapshot const& snapshot, std::size_t width, ToolPresentation tool_presentation, bool thinking_visible,
                               bool compact_spacing, std::vector<TranscriptSearchItemInterval> const& affected);
  // TUI thread: scan cached projections once with one folded matcher and replace all chronological matches.
  [[nodiscard]] TranscriptSearchUpdate replace_all_matches();
  // TUI thread: splice rematched intervals into ordered matches while preserving unaffected rows and their
  // selector prefix.
  [[nodiscard]] TranscriptSearchUpdate replace_affected_matches(std::vector<TranscriptSearchItemInterval> const& affected, std::size_t first_changed_match_row);

  // TUI-thread owned slot per retained item; mutation realigns indices before rebuilding affected ranges.
  std::vector<TranscriptSearchProjection> projections_;
  // TUI-thread owned accepted query; content is protected from debug printing and cleared when search closes.
  ava::core::NoLogString query_;
  // TUI-thread owned chronological results; query/mutation updates preserve ordering needed for binary-search
  // selection restoration.
  std::vector<TranscriptSearchMatch> matches_;
  // TUI-thread cumulative diagnostic: individual affected items rendered under captured authoritative settings.
  std::size_t authoritative_mutation_item_render_count_ = 0;
  // TUI-thread cumulative diagnostic: owned searchable projections constructed from rendered rows.
  std::size_t projection_build_count_ = 0;
  // TUI-thread cumulative diagnostic: authoritative layout/item positions visited while constructing projections.
  std::size_t layout_position_visit_count_ = 0;
  // TUI-thread cumulative diagnostic: cached projections evaluated by the literal matcher.
  std::size_t match_projection_evaluation_count_ = 0;
  // TUI-thread cumulative diagnostic: retained result indices shifted after transcript insertion/eviction.
  std::size_t match_entry_realign_count_ = 0;
  // TUI-thread cumulative diagnostic: result entries replaced or removed during incremental interval updates.
  std::size_t match_entry_splice_count_ = 0;
};

// Pure helper on borrowed text: enforce bounded single-line printable UTF-8 before matcher/selector use.
[[nodiscard]] bool transcript_search_query_valid(std::string_view query) noexcept;
// Pure helper: apply ASCII-folded literal matching with bounded query storage; no regular-expression
// interpretation.
[[nodiscard]] bool transcript_search_literal_match(std::string_view candidate, std::string_view query) noexcept;
// Pure helper: map optional retained index through eviction/restoration, rejecting underflow/overflow.
[[nodiscard]] std::optional<std::size_t> shift_transcript_search_item_index(std::optional<std::size_t> item_index, std::ptrdiff_t item_index_shift) noexcept;
// Pure caller-owned snapshot/layout helper: build a temporary cache and return owned chronological literal
// matches.
[[nodiscard]] std::vector<TranscriptSearchMatch> build_transcript_search_matches(ComposerSnapshot const& snapshot, TranscriptLayout const& layout,
                                                                                 std::string_view query);
// TUI-thread helper on caller-owned view: preserve unchanged prefix and rebuild bounded match rows with
// literal-filter-compatible values.
void update_transcript_search_select_list_rows(SelectListView& view, std::vector<TranscriptSearchMatch> const& matches, std::string_view query,
                                               std::size_t first_changed_match_row, std::size_t& modal_row_build_count);

}  // namespace detail

// Session-owned TUI-thread modal search policy. Presentation, renderer, navigation, and SelectorHost outlive
// it. It retains owned query/cache and a pre-search viewport anchor; SelectorHost owns visible rows. Search
// temporarily withdraws modal projection for authoritative layout preparation, then restores it before
// returning.
class TranscriptSearchController final
{
 public:
  // TUI thread: bind earlier-lived presentation/render/navigation/selector owners; no asynchronous callbacks
  // retain this object.
  TranscriptSearchController(RuntimePresentationState& presentation_state, RuntimeRenderer& renderer, RuntimeNavigationController& navigation,
                             SelectorHost& selectors);
  // Keep one owner for saved viewport, query, and selector reconciliation.
  TranscriptSearchController(TranscriptSearchController const&) = delete;
  // Preserve lifetime-bound dependencies and prevent competing search state for one selector.
  TranscriptSearchController& operator=(TranscriptSearchController const&) = delete;

  // TUI thread: validate query, capture pre-modal viewport/settings, rebuild matches, and publish search
  // selector; false leaves invalid input unapplied.
  [[nodiscard]] bool open(std::string query);
  // TUI thread: ask SelectorHost whether search still owns the visible selector before applying updates.
  [[nodiscard]] bool is_open() const noexcept;
  // TUI thread: nullopt yields to other features; engaged result means handled input and reports renderer-
  // request success.
  [[nodiscard]] std::optional<bool> handle_input(InputEvent const& event);
  // TUI thread after mutation, before next viewport preparation: accumulate eviction shift and preserve
  // selected item while refreshing affected matches.
  void refresh_after_transcript_mutation(std::ptrdiff_t item_index_shift, std::size_t changed_from_item_index);
  // TUI thread: rebuild under new authoritative geometry while preserving selected item when still present.
  void refresh_after_resize();
  // TUI thread before prompt admission: close active search and restore the saved viewport so modal geometry
  // does not leak.
  void close_before_prompt();
  // TUI thread before new session data: close search and discard old query/cache/anchor without restoring
  // another session's viewport.
  void reset_for_session_transition();
  // TUI thread: copy cumulative work counters without exposing cached text or mutable search state.
  [[nodiscard]] detail::TranscriptSearchDiagnostics diagnostics() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // TUI thread: retain unchanged row prefix, reconstruct changed suffix, and restore selected item or bounded
  // fallback index.
  void rebuild(std::optional<std::size_t> selected_item_index, std::size_t fallback_selection, std::size_t first_changed_match_row);
  // TUI thread: retire selector first, prepare underlying layout, then restore exact offset or shifted anchor
  // and release search content.
  void restore_saved_viewport(std::string status);
  // TUI thread: temporarily withdraw search modal projection while preparing underlying transcript geometry;
  // restore it before return.
  void refresh_authoritative_layout();
  // TUI thread immediately after preparation: retain layout/terminal settings to decide whether later mutation
  // may use incremental rendering.
  void capture_authoritative_layout_settings();

  // Borrowed TUI-thread projection owner outlives search; named publications update status/dimensions without
  // writable snapshot access.
  RuntimePresentationState& presentation_state_;
  // Borrowed renderer outlives search; supplies authoritative layout and all viewport transitions.
  RuntimeRenderer& renderer_;
  // Borrowed navigation owner outlives search; resolves confirmed matches into explicit transcript navigation.
  RuntimeNavigationController& navigation_;
  // Borrowed selector owner outlives search; its current kind controls whether this feature may update visible
  // rows.
  SelectorHost& selectors_;
  // TUI-thread owned accepted query; content is protected from debug printing and cleared when search closes.
  ava::core::NoLogString query_;
  // Owned TUI-thread searchable rows and ordered matches; released on close/reset, diagnostics retained.
  detail::TranscriptSearchProjectionCache projection_cache_;
  // TUI-thread content-relative pre-search position used when geometry/transcript changes prevent exact offset
  // restoration.
  detail::TranscriptViewportAnchor saved_viewport_anchor_ = {};
  // TUI-thread pre-search history distance; restored exactly only when the saved geometry/generation remain
  // compatible.
  std::size_t saved_scroll_offset_ = 0;
  // TUI-thread generation captured at open, used to decide exact restoration versus anchor mapping.
  std::size_t saved_transcript_generation_ = 0;
  // TUI-thread opening terminal width; changed width requires anchor-based restoration.
  std::size_t saved_width_ = 0;
  // TUI-thread opening terminal height; changed height alters scroll geometry and invalidates exact
  // restoration.
  std::size_t saved_height_ = 0;
  // TUI-thread terminal width used for the latest underlying search layout; compare before incremental refresh.
  std::size_t authoritative_terminal_width_ = 0;
  // TUI-thread terminal height paired with underlying layout settings to detect resize.
  std::size_t authoritative_terminal_height_ = 0;
  // TUI-thread content width supplied to direct item rendering while the modal holds a frozen underlying cache.
  std::size_t authoritative_layout_width_ = 0;
  // TUI-thread tool style captured from layout; mismatch requires full authoritative rebuild.
  ToolPresentation authoritative_tool_presentation_ = ToolPresentation::Rich;
  // TUI-thread thinking visibility captured from layout; mismatch changes searchable rendered rows.
  bool authoritative_thinking_visible_ = true;
  // TUI-thread spacing policy captured from layout; mismatch requires reflow before search refresh.
  bool authoritative_compact_spacing_ = false;
  // TUI-thread marker set from prepared cache validity and cleared when saved search authority is retired.
  bool authoritative_settings_valid_ = false;
  // TUI-thread sum of mutations while search is open; maps saved viewport anchor to retained items on
  // cancellation.
  std::ptrdiff_t accumulated_item_index_shift_ = 0;
  // TUI-thread cumulative number of constructed selector rows; unchanged prefixes and arrow movement avoid
  // increasing it.
  std::size_t modal_row_build_count_ = 0;
};

}  // namespace ava::tui
