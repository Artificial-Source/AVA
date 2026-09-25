#include "sys.h"
#include "ava/tui/composer_internal.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_navigation_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/terminal.h"
#include "ava/tui/tool_cards.h"
#include "ava/core/Application.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace ava::tui {

RuntimeNavigationController::RuntimeNavigationController(TuiRuntimeOptions const& options, ComposerSnapshot& snapshot, SidebarSnapshot& sidebar,
                                                         RuntimeDraftState& draft_state, RuntimeRenderer& renderer)
    : options_(options), snapshot_(snapshot), sidebar_(sidebar), draft_state_(draft_state), renderer_(renderer)
{
}

detail::CompletionMatchModel const* RuntimeNavigationController::refresh_completion_cache()
{
  snapshot_.input = draft_state_.draft.text;
  snapshot_.input_cursor = draft_state_.draft.cursor;
  snapshot_.slash_palette_suppressed = draft_state_.slash_palette_suppressed;
  snapshot_.path_completion_force_active = draft_state_.path_completion_force_active;
  detail::refresh_completion_match_cache(renderer_.completion_cache, snapshot_, snapshot_.file_references_generation);
  return renderer_.completion_cache.model ? &*renderer_.completion_cache.model : nullptr;
}

bool RuntimeNavigationController::slash_palette_active() const
{
  return !draft_state_.slash_palette_suppressed && slash_palette_visible(draft_state_.draft.text, draft_state_.draft.cursor, snapshot_.slash_commands);
}

bool RuntimeNavigationController::file_reference_palette_active()
{
  auto const* model = refresh_completion_cache();
  return model && model->surface == detail::CompletionSurface::FileReference && model->palette_visible;
}

bool RuntimeNavigationController::path_completion_palette_active()
{
  auto const* model = refresh_completion_cache();
  return model && model->surface == detail::CompletionSurface::PathCompletion && model->palette_visible;
}

std::size_t RuntimeNavigationController::completion_match_count()
{
  auto const* model = refresh_completion_cache();
  return model ? model->ranked_source_indices.size() : std::size_t{0};
}

std::size_t RuntimeNavigationController::clamp_completion(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::clamp_completion_selection(renderer_.completion_cache, selected);
}

std::size_t RuntimeNavigationController::previous_completion(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::previous_completion_selection(renderer_.completion_cache, selected);
}

std::size_t RuntimeNavigationController::next_completion(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::next_completion_selection(renderer_.completion_cache, selected);
}

std::optional<std::string> RuntimeNavigationController::selected_completion_disabled_reason(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::completion_selection_disabled_reason(renderer_.completion_cache, snapshot_.file_references, selected);
}

FileReferenceSelectionText RuntimeNavigationController::selected_completion_text(std::size_t selected)
{
  static_cast<void>(refresh_completion_cache());
  return detail::completion_selection_text(renderer_.completion_cache, snapshot_, selected);
}

void RuntimeNavigationController::scroll_up(std::size_t amount)
{
  draft_state_.pending_escape_clear = false;
  auto const [width, height] = terminal_size();
  snapshot_.width = width;
  snapshot_.height = height;
  renderer_.synchronize_detached_transcript_layout();
  auto const max_scroll =
      detail::composer_max_transcript_scroll_offset_cached(snapshot_, width, height, renderer_.completion_cache, snapshot_.file_references_generation,
                                                           renderer_.transcript_layout_cache, snapshot_.transcript_generation);
  auto const clamped_scroll = std::min(renderer_.transcript_scroll_offset, max_scroll);
  renderer_.transcript_scroll_offset = std::min(max_scroll, clamped_scroll + amount);
  renderer_.show_transcript_position_indicator();
  if (renderer_.transcript_scroll_offset > 0 && !renderer_.detached_sidebar_snapshot)
    renderer_.detached_sidebar_snapshot = sidebar_;
}

void RuntimeNavigationController::scroll_down(std::size_t amount)
{
  draft_state_.pending_escape_clear = false;
  auto const [width, height] = terminal_size();
  snapshot_.width = width;
  snapshot_.height = height;
  renderer_.synchronize_detached_transcript_layout();
  auto const max_scroll =
      detail::composer_max_transcript_scroll_offset_cached(snapshot_, width, height, renderer_.completion_cache, snapshot_.file_references_generation,
                                                           renderer_.transcript_layout_cache, snapshot_.transcript_generation);
  auto const clamped_scroll = std::min(renderer_.transcript_scroll_offset, max_scroll);
  renderer_.transcript_scroll_offset = amount >= clamped_scroll ? 0 : clamped_scroll - amount;
  renderer_.show_transcript_position_indicator();
  if (renderer_.transcript_scroll_offset == 0)
  {
    renderer_.detached_new_output_count = 0;
    renderer_.detached_sidebar_snapshot.reset();
  }
}

std::size_t RuntimeNavigationController::transcript_page_size()
{
  static_cast<void>(refresh_completion_cache());
  snapshot_.draft_scroll_offset = draft_state_.draft_scroll_offset;
  auto const [width, height] = terminal_size();
  snapshot_.width = width;
  snapshot_.height = height;
  renderer_.synchronize_detached_transcript_layout();
  auto const body = detail::transcript_body_screen_geometry(snapshot_);
  return std::max<std::size_t>(1, body.valid ? body.transcript_height / 2 : std::size_t{0});
}

bool RuntimeNavigationController::toggle_tool_details_at(std::size_t item_index)
{
  renderer_.synchronize_detached_transcript_layout();
  if (item_index >= snapshot_.transcript.size() || !snapshot_.transcript[item_index].tool)
    return false;
  auto anchor = detail::TranscriptViewportAnchor{};
  auto const preserve_viewport = renderer_.transcript_scroll_offset > 0;
  if (preserve_viewport)
  {
    auto const old_max_scroll = detail::composer_max_transcript_scroll_offset_cached(snapshot_, snapshot_.width, snapshot_.height, renderer_.completion_cache,
                                                                                     snapshot_.file_references_generation, renderer_.transcript_layout_cache,
                                                                                     snapshot_.transcript_generation);
    anchor = detail::capture_transcript_viewport_anchor(renderer_.transcript_layout_cache.layout, old_max_scroll, renderer_.transcript_scroll_offset);
  }
  auto& tool = *snapshot_.transcript[item_index].tool;
  tool.details_visible = detail::tool_card_presentation(tool, snapshot_.tool_presentation) != ToolPresentation::Expanded;
  ++snapshot_.transcript_generation;
  if (preserve_viewport)
  {
    auto const new_max_scroll = detail::composer_max_transcript_scroll_offset_cached(snapshot_, snapshot_.width, snapshot_.height, renderer_.completion_cache,
                                                                                     snapshot_.file_references_generation, renderer_.transcript_layout_cache,
                                                                                     snapshot_.transcript_generation);
    renderer_.transcript_scroll_offset = detail::restore_transcript_viewport_anchor(anchor, renderer_.transcript_layout_cache.layout, new_max_scroll, 0);
  }
  snapshot_.status = "tool details " + std::string(to_string(detail::tool_card_presentation(tool, snapshot_.tool_presentation)));
  return true;
}

std::optional<std::size_t> RuntimeNavigationController::toggle_matching_tool_details(std::string_view query)
{
  renderer_.synchronize_detached_transcript_layout();
  auto anchor = detail::TranscriptViewportAnchor{};
  auto const preserve_viewport = renderer_.transcript_scroll_offset > 0;
  if (preserve_viewport)
  {
    auto const old_max_scroll = detail::composer_max_transcript_scroll_offset_cached(snapshot_, snapshot_.width, snapshot_.height, renderer_.completion_cache,
                                                                                     snapshot_.file_references_generation, renderer_.transcript_layout_cache,
                                                                                     snapshot_.transcript_generation);
    anchor = detail::capture_transcript_viewport_anchor(renderer_.transcript_layout_cache.layout, old_max_scroll, renderer_.transcript_scroll_offset);
  }
  auto const item_index = toggle_latest_matching_tool_details(snapshot_.transcript, query, snapshot_.tool_presentation);
  if (!item_index)
    return std::nullopt;
  ++snapshot_.transcript_generation;
  if (preserve_viewport)
  {
    auto const new_max_scroll = detail::composer_max_transcript_scroll_offset_cached(snapshot_, snapshot_.width, snapshot_.height, renderer_.completion_cache,
                                                                                     snapshot_.file_references_generation, renderer_.transcript_layout_cache,
                                                                                     snapshot_.transcript_generation);
    renderer_.transcript_scroll_offset = detail::restore_transcript_viewport_anchor(anchor, renderer_.transcript_layout_cache.layout, new_max_scroll, 0);
  }
  return item_index;
}

bool RuntimeNavigationController::toggle_thinking_at(std::size_t item_index)
{
  renderer_.synchronize_detached_transcript_layout();
  if (item_index >= snapshot_.transcript.size())
    return false;
  auto anchor = detail::TranscriptViewportAnchor{};
  auto const preserve_viewport = renderer_.transcript_scroll_offset > 0;
  if (preserve_viewport)
  {
    auto const old_max_scroll = detail::composer_max_transcript_scroll_offset_cached(snapshot_, snapshot_.width, snapshot_.height, renderer_.completion_cache,
                                                                                     snapshot_.file_references_generation, renderer_.transcript_layout_cache,
                                                                                     snapshot_.transcript_generation);
    anchor = detail::capture_transcript_viewport_anchor(renderer_.transcript_layout_cache.layout, old_max_scroll, renderer_.transcript_scroll_offset);
  }
  auto const width = composer_main_width(snapshot_);
  if (!toggle_thinking_expansion_at(snapshot_.transcript, item_index, width, snapshot_.thinking_visible))
    return false;
  ++snapshot_.transcript_generation;
  if (preserve_viewport)
  {
    auto const new_max_scroll = detail::composer_max_transcript_scroll_offset_cached(snapshot_, snapshot_.width, snapshot_.height, renderer_.completion_cache,
                                                                                     snapshot_.file_references_generation, renderer_.transcript_layout_cache,
                                                                                     snapshot_.transcript_generation);
    renderer_.transcript_scroll_offset = detail::restore_transcript_viewport_anchor(anchor, renderer_.transcript_layout_cache.layout, new_max_scroll, 0);
  }
  snapshot_.status = snapshot_.transcript[item_index].thinking_expanded ? "thinking details expanded" : "thinking details collapsed";
  return true;
}

std::optional<std::size_t> RuntimeNavigationController::toggle_latest_thinking_details()
{
  renderer_.synchronize_detached_transcript_layout();
  auto anchor = detail::TranscriptViewportAnchor{};
  auto const preserve_viewport = renderer_.transcript_scroll_offset > 0;
  if (preserve_viewport)
  {
    auto const old_max_scroll = detail::composer_max_transcript_scroll_offset_cached(snapshot_, snapshot_.width, snapshot_.height, renderer_.completion_cache,
                                                                                     snapshot_.file_references_generation, renderer_.transcript_layout_cache,
                                                                                     snapshot_.transcript_generation);
    anchor = detail::capture_transcript_viewport_anchor(renderer_.transcript_layout_cache.layout, old_max_scroll, renderer_.transcript_scroll_offset);
  }
  auto const width = composer_main_width(snapshot_);
  auto const item_index = toggle_latest_boundable_thinking(snapshot_.transcript, width, snapshot_.thinking_visible);
  if (!item_index)
    return std::nullopt;
  ++snapshot_.transcript_generation;
  if (preserve_viewport)
  {
    auto const new_max_scroll = detail::composer_max_transcript_scroll_offset_cached(snapshot_, snapshot_.width, snapshot_.height, renderer_.completion_cache,
                                                                                     snapshot_.file_references_generation, renderer_.transcript_layout_cache,
                                                                                     snapshot_.transcript_generation);
    renderer_.transcript_scroll_offset = detail::restore_transcript_viewport_anchor(anchor, renderer_.transcript_layout_cache.layout, new_max_scroll, 0);
  }
  snapshot_.status = snapshot_.transcript[*item_index].thinking_expanded ? "thinking details expanded" : "thinking details collapsed";
  return item_index;
}

bool RuntimeNavigationController::sidebar_drawer_focused() const
{
  return snapshot_.sidebar_drawer_visible && snapshot_.sidebar.has_value() && !snapshot_.permission_prompt && !snapshot_.question_prompt &&
         !snapshot_.select_list;
}

void RuntimeNavigationController::close_sidebar_drawer()
{
  snapshot_.sidebar_drawer_visible = false;
  snapshot_.sidebar_drawer_scroll_offset = 0;
  draft_state_.pending_escape_clear = false;
  snapshot_.status = "session overview closed";
}

std::size_t RuntimeNavigationController::sidebar_drawer_page_size() const
{
  auto const [width, height] = terminal_size();
  auto drawer_snapshot = snapshot_;
  drawer_snapshot.width = width;
  drawer_snapshot.height = height;
  auto const composer_lines = detail::composer_block_line_count(drawer_snapshot, height, width);
  return height > composer_lines + 2 ? height - composer_lines - 2 : std::size_t{1};
}

std::optional<bool> RuntimeNavigationController::handle_sidebar_drawer_input(InputEvent const& event)
{
  if (!sidebar_drawer_focused())
    return std::nullopt;
  if (event.key == Key::Escape || key_matches_action(options_.key_bindings, TuiAction::Cancel, event.key))
  {
    close_sidebar_drawer();
    return renderer_.request_render();
  }

  auto const max_scroll = sidebar_drawer_max_scroll_offset(snapshot_);
  if (key_matches_action(options_.key_bindings, TuiAction::PageUp, event.key))
  {
    auto const page = sidebar_drawer_page_size();
    snapshot_.sidebar_drawer_scroll_offset = page >= snapshot_.sidebar_drawer_scroll_offset ? 0 : snapshot_.sidebar_drawer_scroll_offset - page;
    return renderer_.request_render();
  }
  if (key_matches_action(options_.key_bindings, TuiAction::PageDown, event.key))
  {
    snapshot_.sidebar_drawer_scroll_offset = std::min(max_scroll, snapshot_.sidebar_drawer_scroll_offset + sidebar_drawer_page_size());
    return renderer_.request_render();
  }
  if (event.key == Key::Home)
  {
    snapshot_.sidebar_drawer_scroll_offset = 0;
    return renderer_.request_render();
  }
  if (event.key == Key::End)
  {
    snapshot_.sidebar_drawer_scroll_offset = max_scroll;
    return renderer_.request_render();
  }
  if (event.key == Key::MouseWheelUp)
  {
    if (snapshot_.sidebar_drawer_scroll_offset > 0)
      --snapshot_.sidebar_drawer_scroll_offset;
    return renderer_.request_render();
  }
  if (event.key == Key::MouseWheelDown)
  {
    snapshot_.sidebar_drawer_scroll_offset = std::min(max_scroll, snapshot_.sidebar_drawer_scroll_offset + 1);
    return renderer_.request_render();
  }
  ava::core::Application::instance().terminal_context().beep();
  return true;
}

void RuntimeNavigationController::jump_to_bottom(std::string status)
{
  draft_state_.pending_escape_clear = false;
  renderer_.transcript_scroll_offset = 0;
  renderer_.show_transcript_position_indicator();
  renderer_.discard_deferred_detached_transcript_update();
  renderer_.detached_new_output_count = 0;
  renderer_.detached_sidebar_snapshot.reset();
  snapshot_.status = std::move(status);
}

void RuntimeNavigationController::jump_to_transcript_item(std::size_t item_index, std::string status)
{
  draft_state_.pending_escape_clear = false;
  auto const [width, height] = terminal_size();
  snapshot_.width = width;
  snapshot_.height = height;
  renderer_.synchronize_detached_transcript_layout();
  auto const max_scroll =
      detail::composer_max_transcript_scroll_offset_cached(snapshot_, width, height, renderer_.completion_cache, snapshot_.file_references_generation,
                                                           renderer_.transcript_layout_cache, snapshot_.transcript_generation);
  auto const& layout = renderer_.transcript_layout_cache.layout;
  auto const message = std::ranges::find(layout.message_item_indices, item_index);
  if (message == layout.message_item_indices.end())
  {
    snapshot_.status = "transcript item is no longer visible";
    return;
  }
  auto const position = static_cast<std::size_t>(message - layout.message_item_indices.begin());
  auto const target_start = std::min(layout.message_starts[position], max_scroll);
  renderer_.transcript_scroll_offset = max_scroll - target_start;
  renderer_.show_transcript_position_indicator();
  renderer_.discard_deferred_detached_transcript_update();
  if (renderer_.transcript_scroll_offset > 0)
  {
    if (!renderer_.detached_sidebar_snapshot)
      renderer_.detached_sidebar_snapshot = sidebar_;
  }
  else
  {
    renderer_.detached_new_output_count = 0;
    renderer_.detached_sidebar_snapshot.reset();
  }
  snapshot_.status = std::move(status);
}

void RuntimeNavigationController::scroll_to_message_boundary(bool previous)
{
  draft_state_.pending_escape_clear = false;
  auto const [width, height] = terminal_size();
  snapshot_.width = width;
  snapshot_.height = height;
  renderer_.synchronize_detached_transcript_layout();
  auto const max_scroll =
      detail::composer_max_transcript_scroll_offset_cached(snapshot_, width, height, renderer_.completion_cache, snapshot_.file_references_generation,
                                                           renderer_.transcript_layout_cache, snapshot_.transcript_generation);
  auto const& layout = renderer_.transcript_layout_cache.layout;
  std::vector<std::size_t> user_starts;
  user_starts.reserve(layout.message_starts.size());
  for (std::size_t position = 0; position < layout.message_starts.size() && position < layout.message_item_indices.size(); ++position)
  {
    auto const item_index = layout.message_item_indices[position];
    if (item_index < snapshot_.transcript.size() && snapshot_.transcript[item_index].label == "you" && !snapshot_.transcript[item_index].tool)
    {
      auto const start = std::min(layout.message_starts[position], max_scroll);
      if (user_starts.empty() || user_starts.back() != start)
        user_starts.push_back(start);
    }
  }
  if (user_starts.empty())
  {
    renderer_.show_transcript_position_indicator();
    snapshot_.status = "no retained user turns";
    return;
  }

  auto const clamped_scroll = std::min(renderer_.transcript_scroll_offset, max_scroll);
  auto const current_start = max_scroll - clamped_scroll;
  auto target_start = std::optional<std::size_t>{};
  if (previous)
  {
    for (auto const start : user_starts)
    {
      if (start >= current_start)
        break;
      target_start = start;
    }
    if (!target_start)
    {
      renderer_.show_transcript_position_indicator();
      snapshot_.status = "oldest retained user turn";
      return;
    }
  }
  else
  {
    for (auto const start : user_starts)
    {
      if (start > current_start)
      {
        target_start = start;
        break;
      }
    }
    if (!target_start || *target_start >= max_scroll)
    {
      jump_to_bottom("live tail");
      return;
    }
  }

  renderer_.transcript_scroll_offset = max_scroll > *target_start ? max_scroll - *target_start : 0;
  renderer_.show_transcript_position_indicator();
  if (renderer_.transcript_scroll_offset > 0 && !renderer_.detached_sidebar_snapshot)
    renderer_.detached_sidebar_snapshot = sidebar_;
  if (renderer_.transcript_scroll_offset == 0)
  {
    renderer_.detached_new_output_count = 0;
    renderer_.detached_sidebar_snapshot.reset();
  }
  snapshot_.status = previous ? "previous retained user turn" : "next retained user turn";
}

}  // namespace ava::tui
