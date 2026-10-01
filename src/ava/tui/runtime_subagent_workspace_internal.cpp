#include "sys.h"
#include "ava/tui/runtime_subagent_workspace_internal.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ava::tui {
namespace {

bool item_equal(SelectListItemView const& left, SelectListItemView const& right)
{
  return left.non_searchable_suffix == right.non_searchable_suffix && left.value == right.value && left.label == right.label &&
         left.description == right.description && left.group == right.group && left.detail == right.detail && left.badge == right.badge &&
         left.current == right.current && left.enabled == right.enabled && left.disabled_reason == right.disabled_reason;
}

bool selector_metadata_equal(SelectListView const& left, SelectListView const& right)
{
  return left.title == right.title && left.subtitle == right.subtitle && left.placeholder == right.placeholder && left.empty_text == right.empty_text &&
         left.footer_hint == right.footer_hint && left.items.size() == right.items.size() && std::ranges::equal(left.items, right.items, item_equal);
}

bool messages_equal(std::vector<ava::agent::SubagentLiveMessage> const& left, std::vector<ava::agent::SubagentLiveMessage> const& right)
{
  return left.size() == right.size() &&
         std::ranges::equal(left, right, [](auto const& lhs, auto const& rhs) { return lhs.role == rhs.role && lhs.text == rhs.text; });
}

SelectListView unavailable_selector()
{
  SelectListView view;
  view.title = "Subagents";
  view.placeholder = "Search subagents";
  view.empty_text = "No matching subagents";
  view.footer_hint = "Esc close";
  view.freeze_underlying_transcript_layout = true;
  SelectListItemView item;
  item.label = "Subagent workspace unavailable";
  item.enabled = false;
  view.items.push_back(std::move(item));
  return view;
}

}  // namespace

RuntimeSubagentWorkspaceController::RuntimeSubagentWorkspaceController(TuiRuntimeOptions const& options, ComposerSnapshot& snapshot)
    : options_(options), snapshot_(snapshot)
{
}

bool RuntimeSubagentWorkspaceController::active() const noexcept
{
  return mode_ != Mode::Closed;
}

bool RuntimeSubagentWorkspaceController::selector_active() const noexcept
{
  return mode_ == Mode::Selector;
}

bool RuntimeSubagentWorkspaceController::workspace_active() const noexcept
{
  return mode_ == Mode::Workspace;
}

std::string_view RuntimeSubagentWorkspaceController::active_job_id() const noexcept
{
  return job_id_;
}

std::optional<std::uint64_t> RuntimeSubagentWorkspaceController::known_generation() const noexcept
{
  return known_generation_;
}

void RuntimeSubagentWorkspaceController::publish()
{
  if (mode_ == Mode::Closed)
  {
    snapshot_.select_list.reset();
    snapshot_.subagent_workspace.reset();
    return;
  }
  if (mode_ == Mode::Selector)
  {
    selector_.selected_item_index = clamp_select_list_selection(selector_, selector_.selected_item_index);
    snapshot_.select_list = selector_;
    snapshot_.subagent_workspace.reset();
    return;
  }
  workspace_.scroll_offset = std::min(workspace_.scroll_offset, subagent_workspace_max_scroll_offset(workspace_, snapshot_.width, snapshot_.height));
  snapshot_.select_list.reset();
  auto published = workspace_;
  published.refresh_unavailable = published.refresh_unavailable || refresh_unavailable_;
  snapshot_.subagent_workspace = std::move(published);
}

bool RuntimeSubagentWorkspaceController::open_selector(Clock::time_point now)
{
  mode_ = Mode::Selector;
  job_id_.clear();
  known_generation_.reset();
  workspace_ = {};
  selector_ = unavailable_selector();
  refresh_unavailable_ = false;
  last_list_refresh_succeeded_ = true;
  if (options_.list_subagents)
  {
    auto listed = options_.list_subagents();
    if (listed)
      selector_ = std::move(*listed);
    else
    {
      refresh_unavailable_ = true;
      last_list_refresh_succeeded_ = false;
    }
  }
  else
  {
    refresh_unavailable_ = true;
    last_list_refresh_succeeded_ = false;
  }
  selector_.freeze_underlying_transcript_layout = true;
  selector_.selected_item_index = clamp_select_list_selection(selector_, selector_.selected_item_index);
  next_poll_ = now + kPollInterval;
  publish();
  return true;
}

void RuntimeSubagentWorkspaceController::close_to_parent()
{
  mode_ = Mode::Closed;
  selector_ = {};
  workspace_ = {};
  job_id_.clear();
  known_generation_.reset();
  refresh_unavailable_ = false;
  last_list_refresh_succeeded_ = true;
  next_poll_ = Clock::time_point::max();
  publish();
}

void RuntimeSubagentWorkspaceController::reset_for_session_transition()
{
  close_to_parent();
}

void RuntimeSubagentWorkspaceController::close_to_selector()
{
  mode_ = Mode::Selector;
  workspace_ = {};
  job_id_.clear();
  known_generation_.reset();
  refresh_unavailable_ = false;
  publish();
}

bool RuntimeSubagentWorkspaceController::refresh_selector()
{
  if (!options_.list_subagents)
  {
    last_list_refresh_succeeded_ = false;
    bool const changed = !refresh_unavailable_ || selector_.subtitle != "Refresh unavailable";
    refresh_unavailable_ = true;
    selector_.subtitle = "Refresh unavailable";
    return changed;
  }
  auto listed = options_.list_subagents();
  if (!listed)
  {
    last_list_refresh_succeeded_ = false;
    bool const changed = !refresh_unavailable_ || selector_.subtitle != "Refresh unavailable";
    refresh_unavailable_ = true;
    selector_.subtitle = "Refresh unavailable";
    return changed;
  }
  last_list_refresh_succeeded_ = true;
  listed->freeze_underlying_transcript_layout = true;
  listed->query = selector_.query;

  std::string selected_value;
  if (selector_.selected_item_index < selector_.items.size())
    selected_value = selector_.items[selector_.selected_item_index].value;
  if (mode_ == Mode::Workspace)
    selected_value = job_id_;
  if (!selected_value.empty())
  {
    for (std::size_t index = 0; index < listed->items.size(); ++index)
    {
      if (listed->items[index].value == selected_value)
      {
        listed->selected_item_index = index;
        break;
      }
    }
  }
  listed->selected_item_index = clamp_select_list_selection(*listed, listed->selected_item_index);

  bool changed = !selector_metadata_equal(selector_, *listed);
  if (mode_ == Mode::Selector && refresh_unavailable_)
  {
    refresh_unavailable_ = false;
    changed = true;
  }
  selector_ = std::move(*listed);
  if (mode_ == Mode::Workspace)
  {
    auto const current = std::ranges::find_if(selector_.items, [&](auto const& item) { return item.value == job_id_; });
    if (current == selector_.items.end())
    {
      if (!workspace_.unavailable || !workspace_.evicted)
      {
        workspace_.unavailable = true;
        workspace_.evicted = true;
        refresh_unavailable_ = false;
        changed = true;
      }
    }
    else
    {
      if (workspace_.title != current->label || workspace_.status != current->badge || workspace_.launch_detail != current->non_searchable_suffix ||
          workspace_.unavailable || workspace_.evicted)
      {
        workspace_.title = current->label;
        workspace_.status = current->badge;
        workspace_.launch_detail = current->non_searchable_suffix;
        workspace_.unavailable = false;
        workspace_.evicted = false;
        changed = true;
      }
    }
  }
  return changed;
}

bool RuntimeSubagentWorkspaceController::refresh_inspection()
{
  if (mode_ != Mode::Workspace || job_id_.empty() || !options_.inspect_subagent)
    return false;
  auto inspected = options_.inspect_subagent(job_id_, known_generation_);
  if (!inspected || !*inspected)
  {
    bool const changed = !refresh_unavailable_ || (workspace_.messages.empty() && !workspace_.unavailable);
    refresh_unavailable_ = true;
    if (workspace_.messages.empty())
      workspace_.unavailable = true;
    return changed;
  }
  auto const& frame = **inspected;
  bool const prior_refresh_unavailable = refresh_unavailable_;
  refresh_unavailable_ = false;
  auto const old_max = subagent_workspace_max_scroll_offset(workspace_, snapshot_.width, snapshot_.height);
  bool const followed_tail = workspace_.scroll_offset >= old_max;
  bool changed = prior_refresh_unavailable;
  if (frame.not_modified)
  {
    if (workspace_.terminal != frame.terminal || workspace_.freeze_pending != frame.freeze_pending || workspace_.unavailable != frame.unavailable ||
        workspace_.refresh_unavailable != frame.refresh_unavailable)
    {
      workspace_.terminal = frame.terminal;
      workspace_.freeze_pending = frame.freeze_pending;
      workspace_.unavailable = frame.unavailable;
      workspace_.refresh_unavailable = frame.refresh_unavailable;
      changed = true;
    }
  }
  else
  {
    if (!messages_equal(workspace_.messages, frame.messages) || workspace_.terminal != frame.terminal || workspace_.freeze_pending != frame.freeze_pending ||
        workspace_.unavailable != frame.unavailable || workspace_.refresh_unavailable != frame.refresh_unavailable)
    {
      workspace_.messages = frame.messages;
      workspace_.terminal = frame.terminal;
      workspace_.freeze_pending = frame.freeze_pending;
      workspace_.unavailable = frame.unavailable;
      workspace_.refresh_unavailable = frame.refresh_unavailable;
      changed = true;
    }
  }
  if (frame.generation > 0)
    known_generation_ = frame.generation;
  if (changed && followed_tail)
    workspace_.scroll_offset = subagent_workspace_max_scroll_offset(workspace_, snapshot_.width, snapshot_.height);
  return changed;
}

bool RuntimeSubagentWorkspaceController::open_selected_workspace()
{
  if (selector_.selected_item_index >= selector_.items.size())
    return false;
  auto const& item = selector_.items[selector_.selected_item_index];
  if (!item.enabled || item.value.empty())
    return false;
  mode_ = Mode::Workspace;
  job_id_ = item.value;
  known_generation_.reset();
  workspace_ = {};
  workspace_.title = item.label;
  workspace_.status = item.badge;
  workspace_.launch_detail = item.non_searchable_suffix;
  bool changed = true;
  changed = refresh_inspection() || changed;
  publish();
  return changed;
}

bool RuntimeSubagentWorkspaceController::cycle_workspace(bool forward)
{
  std::vector<std::size_t> enabled;
  for (std::size_t index = 0; index < selector_.items.size(); ++index)
  {
    if (selector_.items[index].enabled && !selector_.items[index].value.empty())
      enabled.push_back(index);
  }
  if (enabled.empty())
    return false;
  auto current = std::ranges::find_if(enabled, [&](std::size_t index) { return selector_.items[index].value == job_id_; });
  auto position = current == enabled.end() ? std::size_t{0} : static_cast<std::size_t>(current - enabled.begin());
  position = forward ? (position + 1) % enabled.size() : (position == 0 ? enabled.size() - 1 : position - 1);
  selector_.selected_item_index = enabled[position];
  return open_selected_workspace();
}

bool RuntimeSubagentWorkspaceController::set_workspace_scroll(std::size_t offset)
{
  auto const clamped = std::min(offset, subagent_workspace_max_scroll_offset(workspace_, snapshot_.width, snapshot_.height));
  if (clamped == workspace_.scroll_offset)
    return false;
  workspace_.scroll_offset = clamped;
  publish();
  return true;
}

bool RuntimeSubagentWorkspaceController::scroll_workspace(std::ptrdiff_t delta)
{
  if (delta < 0)
  {
    auto const amount = static_cast<std::size_t>(-delta);
    return set_workspace_scroll(amount > workspace_.scroll_offset ? 0 : workspace_.scroll_offset - amount);
  }
  auto const amount = static_cast<std::size_t>(delta);
  auto const max = subagent_workspace_max_scroll_offset(workspace_, snapshot_.width, snapshot_.height);
  return set_workspace_scroll(amount > max - std::min(max, workspace_.scroll_offset) ? max : workspace_.scroll_offset + amount);
}

bool RuntimeSubagentWorkspaceController::poll(Clock::time_point now)
{
  if (!active() || now < next_poll_)
    return false;
  next_poll_ = now + kPollInterval;
  bool changed = refresh_selector();
  if (mode_ == Mode::Workspace && last_list_refresh_succeeded_)
    changed = refresh_inspection() || changed;
  if (changed)
    publish();
  return changed;
}

std::optional<RuntimeSubagentWorkspaceController::Clock::duration> RuntimeSubagentWorkspaceController::time_until_poll(Clock::time_point now) const
{
  if (!active())
    return std::nullopt;
  return now >= next_poll_ ? Clock::duration::zero() : next_poll_ - now;
}

RuntimeSubagentWorkspaceInputResult RuntimeSubagentWorkspaceController::handle_input(InputEvent const& event, Clock::time_point /*now*/)
{
  if (!active())
    return {};
  using Key = terminal::Key;
  RuntimeSubagentWorkspaceInputResult result{.handled = true};
  if (mode_ == Mode::Selector)
  {
    SelectListInputResult input;
    if (event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick)
    {
      auto clicked = select_list_selection_for_screen_position(snapshot_, event.mouse_row, event.mouse_column);
      if (!clicked)
        return result;
      input = SelectListInputResult{.selected_item_index = *clicked, .query = selector_.query, .action = SelectListInputAction::Resolve};
    }
    else
    {
      input = handle_select_list_input(selector_, event);
    }
    if (input.action == SelectListInputAction::Cancel)
    {
      close_to_parent();
      result.changed = true;
      return result;
    }
    if (input.action == SelectListInputAction::Resolve)
    {
      result.changed = open_selected_workspace();
      result.beep = !result.changed;
      return result;
    }
    if (input.action == SelectListInputAction::Redraw)
    {
      selector_.selected_item_index = input.selected_item_index;
      selector_.query = std::move(input.query);
      publish();
      result.changed = true;
    }
    return result;
  }

  auto const character_text = !event.text.empty() ? event.text : (event.character == '\0' ? std::string{} : std::string(1, event.character));
  if (event.key == Key::Escape || event.key == Key::CtrlC)
  {
    close_to_selector();
    result.changed = true;
  }
  else if (event.key == Key::Tab)
  {
    result.changed = cycle_workspace(true);
  }
  else if (event.key == Key::ShiftTab)
  {
    result.changed = cycle_workspace(false);
  }
  else if (character_text == "C")
  {
    auto const outcome = options_.cancel_subagent ? options_.cancel_subagent(job_id_) : SubagentWorkspaceCancelOutcome::CancelUnavailable;
    switch (outcome)
    {
      case SubagentWorkspaceCancelOutcome::CancellationRequested:
        workspace_.notice = "Cancellation requested";
        break;
      case SubagentWorkspaceCancelOutcome::AlreadyFinished:
        workspace_.notice = "Already finished";
        break;
      case SubagentWorkspaceCancelOutcome::CancelUnavailable:
        workspace_.notice = "Cancel unavailable";
        result.beep = true;
        break;
    }
    publish();
    result.changed = true;
  }
  else if (character_text == "P")
  {
    auto const outcome = options_.promote_subagent ? options_.promote_subagent(job_id_) : SubagentWorkspacePromoteOutcome::PromotionUnavailable;
    switch (outcome)
    {
      case SubagentWorkspacePromoteOutcome::CurrentlyBackground:
        workspace_.notice = "Currently background";
        break;
      case SubagentWorkspacePromoteOutcome::AlreadyFinished:
        workspace_.notice = "Already finished";
        break;
      case SubagentWorkspacePromoteOutcome::PromotionUnavailable:
        workspace_.notice = "Promotion unavailable";
        result.beep = true;
        break;
    }
    publish();
    result.changed = true;
  }
  else if (event.key == Key::ArrowUp || event.key == Key::MouseWheelUp)
  {
    result.changed = scroll_workspace(-3);
  }
  else if (event.key == Key::ArrowDown || event.key == Key::MouseWheelDown)
  {
    result.changed = scroll_workspace(3);
  }
  else if (event.key == Key::PageUp)
  {
    result.changed = scroll_workspace(-5);
  }
  else if (event.key == Key::PageDown)
  {
    result.changed = scroll_workspace(5);
  }
  else if (event.key == Key::Home || event.key == Key::CtrlHome)
  {
    result.changed = set_workspace_scroll(0);
  }
  else if (event.key == Key::End || event.key == Key::CtrlEnd)
  {
    result.changed = set_workspace_scroll(subagent_workspace_max_scroll_offset(workspace_, snapshot_.width, snapshot_.height));
  }
  return result;
}

}  // namespace ava::tui
