// Applies completion, pointer, and transcript navigation while keeping viewport layout preparation ordered.
#include "sys.h"
#include "runtime_state_internal.h"
#include "ava/tui/TuiFrontEnd.h"
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

RuntimeNavigationController::RuntimeNavigationController(TuiFrontEnd const& frontend, RuntimePresentationState& presentation, Composer& draft_state,
                                                         RuntimeRenderer& renderer)
    : frontend_(frontend), presentation_(presentation), draft_state_(draft_state), renderer_(renderer)
{
}

void RuntimeNavigationController::project_completion_input()
{
  presentation_.publish_composer(draft_state_);
}

detail::CompletionMatchModel const* RuntimeNavigationController::refresh_completion_cache()
{
  project_completion_input();
  return renderer_.refresh_completion_cache();
}

bool RuntimeNavigationController::slash_palette_active() const
{
  return !draft_state_.palette_suppressed() &&
         slash_palette_visible(draft_state_.draft().text, draft_state_.draft().cursor, presentation_.screen().slash_commands);
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

std::size_t RuntimeNavigationController::clamp_completion(std::size_t selected)
{
  project_completion_input();
  return renderer_.clamp_completion(selected);
}

std::optional<std::string> RuntimeNavigationController::selected_completion_disabled_reason(std::size_t selected)
{
  project_completion_input();
  return renderer_.selected_completion_disabled_reason(selected);
}

FileReferenceSelectionText RuntimeNavigationController::selected_completion_text(std::size_t selected)
{
  project_completion_input();
  return renderer_.selected_completion_text(selected);
}

bool RuntimeNavigationController::handle_pointer_input(InputEvent const& event, terminal::Context& terminal_context)
{
  using Key = terminal::Key;
  if (event.key != Key::MouseLeftPress && event.key != Key::MouseLeftClick && event.key != Key::MouseLeftDrag && event.key != Key::MouseLeftRelease &&
      event.key != Key::MousePointerCancel)
    return false;
  // Completion owns a click before the transcript or draft underneath it. Keep
  // feedback here; the session requests the frame after this dispatch stage.
  auto apply_feedback = [&](ComposerInputResult result) {
    presentation_.publish_composer_feedback(result);
    if (result.beep)
      terminal_context.beep();
    return result.handled;
  };
  draft_state_.cancel_escape();
  auto const begins_click = event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick;
  bool palette_claimed = false;
  if (begins_click)
  {
    if (auto const clicked = slash_palette_selection_for_screen_position(presentation_.screen(), event.mouse_row, event.mouse_column))
    {
      renderer_.clear_transcript_selection();
      draft_state_.clear_selection();
      draft_state_.select_completion(*clicked);
      static_cast<void>(apply_feedback(draft_state_.select_slash_command(presentation_.screen())));
      palette_claimed = true;
    }
    else if (auto const clicked = renderer_.file_reference_completion_at(event.mouse_row, event.mouse_column))
    {
      renderer_.clear_transcript_selection();
      draft_state_.clear_selection();
      draft_state_.select_completion(*clicked);
      // Hit-test the published palette first, then apply the choice to the current
      // draft, which may have changed earlier in the same input batch.
      static_cast<void>(refresh_completion_cache());
      static_cast<void>(apply_feedback(draft_state_.select_file_reference(renderer_.completion_cache(), presentation_.screen())));
      palette_claimed = true;
    }
    else if (auto const clicked = renderer_.path_completion_at(event.mouse_row, event.mouse_column))
    {
      renderer_.clear_transcript_selection();
      draft_state_.clear_selection();
      draft_state_.select_completion(*clicked);
      // Hit-test the published palette first, then apply the choice to the current
      // draft, which may have changed earlier in the same input batch.
      static_cast<void>(refresh_completion_cache());
      static_cast<void>(apply_feedback(draft_state_.select_path_completion(renderer_.completion_cache(), presentation_.screen())));
      palette_claimed = true;
    }
  }
  // After palette precedence, transcript selection gets the report before the draft so only one surface starts
  // a new selection.
  if (!palette_claimed)
  {
    auto const transcript_mouse =
        renderer_.handle_transcript_selection_mouse(event, std::bind_front(&RuntimeNavigationController::toggle_tool_details_at, this),
                                                    std::bind_front(&RuntimeNavigationController::toggle_thinking_at, this));
    if (transcript_mouse == TranscriptSelectionMouseResult::Ignored)
    {
      if (begins_click)
      {
        if (auto const cursor = composer_input_cursor_for_screen_position(presentation_.screen(), event.mouse_row, event.mouse_column))
        {
          renderer_.clear_transcript_selection();
          draft_state_.begin_pointer_selection(*cursor, event.key == Key::MouseLeftPress);
          presentation_.publish_status("cursor moved");
        }
      }
      else if (draft_state_.mouse_selecting())
      {
        if (auto const cursor = composer_input_cursor_for_screen_position(presentation_.screen(), event.mouse_row, event.mouse_column))
        {
          draft_state_.extend_pointer_selection(*cursor);
          presentation_.publish_status(draft_state_.selection_bounds() ? "selection active" : "cursor moved");
        }
        if (event.key == Key::MouseLeftRelease)
          draft_state_.end_pointer_selection();
      }
    }
  }
  return true;
}

void RuntimeNavigationController::scroll_up(std::size_t amount)
{
  draft_state_.cancel_escape();
  auto const [width, height] = terminal_size();
  presentation_.publish_dimensions(width, height);
  renderer_.scroll_transcript(static_cast<std::ptrdiff_t>(amount));
}

void RuntimeNavigationController::scroll_down(std::size_t amount)
{
  draft_state_.cancel_escape();
  auto const [width, height] = terminal_size();
  presentation_.publish_dimensions(width, height);
  renderer_.scroll_transcript(-static_cast<std::ptrdiff_t>(amount));
}

std::size_t RuntimeNavigationController::transcript_page_size()
{
  static_cast<void>(refresh_completion_cache());
  presentation_.publish_composer(draft_state_);
  auto const [width, height] = terminal_size();
  presentation_.publish_dimensions(width, height);
  renderer_.synchronize_detached_transcript_layout();
  auto const body = detail::transcript_body_screen_geometry(presentation_.screen());
  return std::max<std::size_t>(1, body.valid ? body.transcript_height / 2 : std::size_t{0});
}

bool RuntimeNavigationController::toggle_tool_details_at(std::size_t item_index)
{
  renderer_.synchronize_detached_transcript_layout();
  if (item_index >= presentation_.screen().transcript.size() || !presentation_.screen().transcript[item_index].tool)
    return false;
  auto const update = renderer_.begin_transcript_update();
  static_cast<void>(presentation_.toggle_tool_details(item_index));
  auto const& tool = *presentation_.screen().transcript[item_index].tool;
  renderer_.complete_transcript_update(update, 0, 0, TranscriptUpdateMode::Immediate);
  presentation_.publish_status("tool details " + std::string(to_string(detail::tool_card_presentation(tool, presentation_.screen().tool_presentation))));
  return true;
}

std::optional<std::size_t> RuntimeNavigationController::toggle_matching_tool_details(std::string_view query)
{
  renderer_.synchronize_detached_transcript_layout();
  auto const update = renderer_.begin_transcript_update();
  auto const item_index = presentation_.toggle_matching_tool_details(query);
  if (!item_index)
    return std::nullopt;
  renderer_.complete_transcript_update(update, 0, 0, TranscriptUpdateMode::Immediate);
  return item_index;
}

bool RuntimeNavigationController::toggle_thinking_at(std::size_t item_index)
{
  renderer_.synchronize_detached_transcript_layout();
  if (item_index >= presentation_.screen().transcript.size())
    return false;
  auto const update = renderer_.begin_transcript_update();
  auto const width = composer_main_width(presentation_.screen());
  if (!presentation_.toggle_thinking_details(item_index, width))
    return false;
  renderer_.complete_transcript_update(update, 0, 0, TranscriptUpdateMode::Immediate);
  presentation_.publish_status(presentation_.screen().transcript[item_index].thinking_expanded ? "thinking details expanded" : "thinking details collapsed");
  return true;
}

std::optional<std::size_t> RuntimeNavigationController::toggle_latest_thinking_details()
{
  renderer_.synchronize_detached_transcript_layout();
  auto const update = renderer_.begin_transcript_update();
  auto const width = composer_main_width(presentation_.screen());
  auto const item_index = presentation_.toggle_latest_thinking_details(width);
  if (!item_index)
    return std::nullopt;
  renderer_.complete_transcript_update(update, 0, 0, TranscriptUpdateMode::Immediate);
  presentation_.publish_status(presentation_.screen().transcript[*item_index].thinking_expanded ? "thinking details expanded" : "thinking details collapsed");
  return item_index;
}

bool RuntimeNavigationController::sidebar_drawer_focused() const
{
  return presentation_.screen().sidebar_drawer_visible && presentation_.screen().sidebar.has_value() && !presentation_.screen().permission_prompt &&
         !presentation_.screen().question_prompt && !presentation_.screen().select_list;
}

void RuntimeNavigationController::close_sidebar_drawer()
{
  presentation_.publish_sidebar_drawer(false, 0);
  draft_state_.cancel_escape();
  presentation_.publish_status("session overview closed");
}

std::size_t RuntimeNavigationController::sidebar_drawer_page_size() const
{
  auto const [width, height] = terminal_size();
  auto drawer_snapshot = presentation_.screen();
  drawer_snapshot.width = width;
  drawer_snapshot.height = height;
  auto const composer_lines = detail::composer_block_line_count(drawer_snapshot, height, width);
  return height > composer_lines + 2 ? height - composer_lines - 2 : std::size_t{1};
}

std::optional<bool> RuntimeNavigationController::handle_sidebar_drawer_input(InputEvent const& event)
{
  using Key = terminal::Key;

  if (!sidebar_drawer_focused())
    return std::nullopt;
  if (event.key == Key::Escape || key_matches_action(frontend_.key_bindings(), TuiAction::Cancel, event.key))
  {
    close_sidebar_drawer();
    return renderer_.request_render();
  }

  // Once drawer owns the event, clamp every navigation action to its own geometry and request a frame without
  // immediate painting.
  auto const max_scroll = renderer_.sidebar_drawer_max_scroll();
  if (key_matches_action(frontend_.key_bindings(), TuiAction::PageUp, event.key))
  {
    auto const page = sidebar_drawer_page_size();
    presentation_.publish_sidebar_drawer(
        true, page >= presentation_.screen().sidebar_drawer_scroll_offset ? 0 : presentation_.screen().sidebar_drawer_scroll_offset - page);
    return renderer_.request_render();
  }
  if (key_matches_action(frontend_.key_bindings(), TuiAction::PageDown, event.key))
  {
    presentation_.publish_sidebar_drawer(true, std::min(max_scroll, presentation_.screen().sidebar_drawer_scroll_offset + sidebar_drawer_page_size()));
    return renderer_.request_render();
  }
  if (event.key == Key::Home)
  {
    presentation_.publish_sidebar_drawer(true, 0);
    return renderer_.request_render();
  }
  if (event.key == Key::End)
  {
    presentation_.publish_sidebar_drawer(true, max_scroll);
    return renderer_.request_render();
  }
  if (event.key == Key::MouseWheelUp)
  {
    if (presentation_.screen().sidebar_drawer_scroll_offset > 0)
      presentation_.publish_sidebar_drawer(true, presentation_.screen().sidebar_drawer_scroll_offset - 1);
    return renderer_.request_render();
  }
  if (event.key == Key::MouseWheelDown)
  {
    presentation_.publish_sidebar_drawer(true, std::min(max_scroll, presentation_.screen().sidebar_drawer_scroll_offset + 1));
    return renderer_.request_render();
  }
  ava::core::Application::instance().terminal_context().beep();
  return true;
}

void RuntimeNavigationController::jump_to_bottom(std::string status)
{
  draft_state_.cancel_escape();
  renderer_.show_live_tail(LiveTailReason::Navigation);
  renderer_.show_transcript_position_indicator();
  presentation_.publish_status(std::move(status));
}

void RuntimeNavigationController::jump_to_transcript_item(std::size_t item_index, std::string status)
{
  draft_state_.cancel_escape();
  auto const [width, height] = terminal_size();
  presentation_.publish_dimensions(width, height);
  renderer_.synchronize_detached_transcript_layout();
  auto const max_scroll = renderer_.prepare_transcript_layout();
  auto const& layout = renderer_.prepared_transcript_layout().layout;
  auto const message = std::ranges::find(layout.message_item_indices, item_index);
  if (message == layout.message_item_indices.end())
  {
    presentation_.publish_status("transcript item is no longer visible");
    return;
  }
  auto const position = static_cast<std::size_t>(message - layout.message_item_indices.begin());
  auto const target_start = std::min(layout.message_starts[position], max_scroll);
  renderer_.navigate_transcript(max_scroll - target_start);
  renderer_.show_transcript_position_indicator();
  presentation_.publish_status(std::move(status));
}

void RuntimeNavigationController::scroll_to_message_boundary(bool previous)
{
  draft_state_.cancel_escape();
  auto const [width, height] = terminal_size();
  presentation_.publish_dimensions(width, height);
  renderer_.synchronize_detached_transcript_layout();
  auto const max_scroll = renderer_.prepare_transcript_layout();
  auto const& layout = renderer_.prepared_transcript_layout().layout;
  // Collect only retained user-message boundaries from synchronized layout; tool rows and duplicate starts must
  // not become navigation stops.
  std::vector<std::size_t> user_starts;
  user_starts.reserve(layout.message_starts.size());
  for (std::size_t position = 0; position < layout.message_starts.size() && position < layout.message_item_indices.size(); ++position)
  {
    auto const item_index = layout.message_item_indices[position];
    if (item_index < presentation_.screen().transcript.size() && presentation_.screen().transcript[item_index].label == "you" &&
        !presentation_.screen().transcript[item_index].tool)
    {
      auto const start = std::min(layout.message_starts[position], max_scroll);
      if (user_starts.empty() || user_starts.back() != start)
        user_starts.push_back(start);
    }
  }
  if (user_starts.empty())
  {
    renderer_.show_transcript_position_indicator();
    presentation_.publish_status("no retained user turns");
    return;
  }

  // Choose the adjacent boundary relative to the current visible start, preserving explicit oldest/live-tail
  // feedback when no neighbor exists.
  auto const clamped_scroll = std::min(renderer_.transcript().scroll_offset(), max_scroll);
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
      presentation_.publish_status("oldest retained user turn");
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

  renderer_.navigate_transcript(max_scroll > *target_start ? max_scroll - *target_start : 0);
  renderer_.show_transcript_position_indicator();
  presentation_.publish_status(previous ? "previous retained user turn" : "next retained user turn");
}

}  // namespace ava::tui
