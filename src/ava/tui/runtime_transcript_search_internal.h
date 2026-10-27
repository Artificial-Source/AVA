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

enum class ActiveSelectList;
struct ComposerSnapshot;
struct InputEvent;
class RuntimeNavigationController;
class RuntimePresentationState;
class RuntimeRenderer;

namespace detail {

inline constexpr std::size_t kMaxTranscriptSearchQueryBytes = 1024;
inline constexpr std::size_t kMaxTranscriptSearchDetailBytes = 240;

struct TranscriptSearchMatch
{
  std::size_t item_index = 0;
  std::string identity;
  ava::core::NoLogString detail;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TranscriptSearchProjection
{
  bool available = false;
  bool context_gathering = false;
  std::size_t context_run_offset = 0;
  std::string identity;
  ava::core::NoLogString searchable_text;
  ava::core::NoLogString unspaced_searchable_text;
  std::vector<std::size_t> row_boundary_offsets;
  ava::core::NoLogString default_detail;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct TranscriptSearchUpdate
{
  std::size_t first_changed_match_row = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TranscriptSearchItemInterval
{
  std::size_t first = 0;
  std::size_t past_last = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct TranscriptSearchDiagnostics
{
  std::size_t authoritative_mutation_item_render_count = 0;
  std::size_t projection_build_count = 0;
  std::size_t layout_position_visit_count = 0;
  std::size_t match_projection_evaluation_count = 0;
  std::size_t match_entry_realign_count = 0;
  std::size_t match_entry_splice_count = 0;
  std::size_t modal_row_build_count = 0;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

class TranscriptSearchProjectionCache final
{
 public:
  [[nodiscard]] TranscriptSearchUpdate rebuild_all(ComposerSnapshot const& snapshot, TranscriptLayout const& layout);
  void clear();
  [[nodiscard]] TranscriptSearchUpdate update_query(std::string query);
  [[nodiscard]] TranscriptSearchUpdate refresh_after_transcript_mutation(ComposerSnapshot const& snapshot, std::size_t width,
                                                                         ToolPresentation tool_presentation, bool thinking_visible, bool compact_spacing,
                                                                         std::ptrdiff_t item_index_shift, std::size_t changed_from_item_index);
  [[nodiscard]] std::vector<TranscriptSearchMatch> const& matches() const noexcept;
  [[nodiscard]] std::string const& query() const noexcept;
  [[nodiscard]] std::size_t authoritative_mutation_item_render_count() const noexcept;
  [[nodiscard]] std::size_t projection_build_count() const noexcept;
  [[nodiscard]] std::size_t layout_position_visit_count() const noexcept;
  [[nodiscard]] std::size_t match_projection_evaluation_count() const noexcept;
  [[nodiscard]] std::size_t match_entry_realign_count() const noexcept;
  [[nodiscard]] std::size_t match_entry_splice_count() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void rebuild_range(ComposerSnapshot const& snapshot, TranscriptLayout const& layout, std::size_t first_item_index, std::size_t past_last_item_index);
  void reset_affected_metadata(ComposerSnapshot const& snapshot, std::vector<TranscriptSearchItemInterval> const& affected);
  void rebuild_affected_from_layout(ComposerSnapshot const& snapshot, TranscriptLayout const& layout,
                                    std::vector<TranscriptSearchItemInterval> const& affected);
  void rebuild_affected_direct(ComposerSnapshot const& snapshot, std::size_t width, ToolPresentation tool_presentation, bool thinking_visible,
                               bool compact_spacing, std::vector<TranscriptSearchItemInterval> const& affected);
  [[nodiscard]] TranscriptSearchUpdate replace_all_matches();
  [[nodiscard]] TranscriptSearchUpdate replace_affected_matches(std::vector<TranscriptSearchItemInterval> const& affected, std::size_t first_changed_match_row);

  std::vector<TranscriptSearchProjection> projections_;
  ava::core::NoLogString query_;
  std::vector<TranscriptSearchMatch> matches_;
  std::size_t authoritative_mutation_item_render_count_ = 0;
  std::size_t projection_build_count_ = 0;
  std::size_t layout_position_visit_count_ = 0;
  std::size_t match_projection_evaluation_count_ = 0;
  std::size_t match_entry_realign_count_ = 0;
  std::size_t match_entry_splice_count_ = 0;
};

[[nodiscard]] bool transcript_search_query_valid(std::string_view query) noexcept;
[[nodiscard]] bool transcript_search_literal_match(std::string_view candidate, std::string_view query) noexcept;
[[nodiscard]] std::optional<std::size_t> shift_transcript_search_item_index(std::optional<std::size_t> item_index, std::ptrdiff_t item_index_shift) noexcept;
[[nodiscard]] std::vector<TranscriptSearchMatch> build_transcript_search_matches(ComposerSnapshot const& snapshot, TranscriptLayout const& layout,
                                                                                 std::string_view query);
void update_transcript_search_select_list_rows(SelectListView& view, std::vector<TranscriptSearchMatch> const& matches, std::string_view query,
                                               std::size_t first_changed_match_row, std::size_t& modal_row_build_count);

}  // namespace detail

class TranscriptSearchController final
{
 public:
  TranscriptSearchController(RuntimePresentationState& presentation_state, RuntimeRenderer& renderer, RuntimeNavigationController& navigation,
                             ActiveSelectList& active_select_list);
  TranscriptSearchController(TranscriptSearchController const&) = delete;
  TranscriptSearchController& operator=(TranscriptSearchController const&) = delete;

  [[nodiscard]] bool open(std::string query);
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] std::optional<bool> handle_input(InputEvent const& event);
  void refresh_after_transcript_mutation(std::ptrdiff_t item_index_shift, std::size_t changed_from_item_index);
  void refresh_after_resize();
  void close_before_prompt();
  void reset_for_session_transition();
  [[nodiscard]] detail::TranscriptSearchDiagnostics diagnostics() const noexcept;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  void rebuild(std::optional<std::size_t> selected_item_index, std::size_t fallback_selection, std::size_t first_changed_match_row);
  void restore_saved_viewport(std::string status);
  void refresh_authoritative_layout();
  void capture_authoritative_layout_settings();

  RuntimePresentationState& presentation_state_;
  RuntimeRenderer& renderer_;
  RuntimeNavigationController& navigation_;
  ActiveSelectList& active_select_list_;
  ava::core::NoLogString query_;
  detail::TranscriptSearchProjectionCache projection_cache_;
  detail::TranscriptViewportAnchor saved_viewport_anchor_ = {};
  std::size_t saved_scroll_offset_ = 0;
  std::size_t saved_transcript_generation_ = 0;
  std::size_t saved_width_ = 0;
  std::size_t saved_height_ = 0;
  std::size_t authoritative_terminal_width_ = 0;
  std::size_t authoritative_terminal_height_ = 0;
  std::size_t authoritative_layout_width_ = 0;
  ToolPresentation authoritative_tool_presentation_ = ToolPresentation::Rich;
  bool authoritative_thinking_visible_ = true;
  bool authoritative_compact_spacing_ = false;
  bool authoritative_settings_valid_ = false;
  std::ptrdiff_t accumulated_item_index_shift_ = 0;
  std::size_t modal_row_build_count_ = 0;
};

}  // namespace ava::tui
