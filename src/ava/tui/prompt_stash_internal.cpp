#include "sys.h"
#include "ava/tui/composer.h"
#include "ava/tui/prompt_stash_internal.h"
#include "ava/tui/runtime_draft_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/terminal.h"
#include "ava/core/Application.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string_view>
#include <utility>

namespace ava::tui {
namespace {

std::string stash_preview(ComposerDraftState const& draft)
{
  auto const expanded = expanded_composer_draft_text(draft);
  auto sanitized = sanitize_terminal_text(expanded);
  std::string preview;
  preview.reserve(std::min<std::size_t>(sanitized.size(), 160));
  bool previous_space = false;
  for (char const ch : sanitized)
  {
    auto const whitespace = ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    if (whitespace)
    {
      if (!preview.empty() && !previous_space)
        preview.push_back(' ');
      previous_space = true;
    }
    else
    {
      preview.push_back(ch);
      previous_space = false;
    }
    if (preview.size() >= 160)
      break;
  }
  while (!preview.empty() && preview.back() == ' ')
    preview.pop_back();
  if (preview.empty())
    return "(empty preview)";
  auto const safe_end = clamp_composer_draft_cursor(preview, std::min<std::size_t>(preview.size(), 160));
  preview.resize(safe_end);
  if (safe_end < sanitized.size())
    preview += "…";
  return preview;
}

std::optional<std::uint64_t> stash_id_from_value(std::string_view value)
{
  constexpr std::string_view kPrefix = "prompt-stash:";
  if (!value.starts_with(kPrefix))
    return std::nullopt;
  value.remove_prefix(kPrefix.size());
  std::uint64_t id = 0;
  auto const [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
  if (error != std::errc{} || end != value.data() + value.size() || id == 0)
    return std::nullopt;
  return id;
}

}  // namespace

PromptStashPushResult PromptStash::push(ComposerDraftState const& draft)
{
  if (draft.text.empty())
    return PromptStashPushResult::EmptyDraft;
  auto const expanded_bytes = expanded_composer_draft_text(draft).size();
  if (expanded_bytes > kMaxEntryExpandedBytes)
    return PromptStashPushResult::EntryTooLarge;
  if (entries_.size() >= kMaxEntries || next_id_ == std::numeric_limits<std::uint64_t>::max())
    return PromptStashPushResult::EntryLimitReached;
  if (expanded_bytes > kMaxAggregateExpandedBytes - aggregate_expanded_bytes_)
    return PromptStashPushResult::AggregateLimitReached;

  entries_.push_back(PromptStashEntry{.id = next_id_++,
                                      .text = draft.text,
                                      .cursor = draft.cursor,
                                      .paste_entries = draft.paste_entries,
                                      .next_paste_id = draft.next_paste_id,
                                      .expanded_bytes = expanded_bytes});
  aggregate_expanded_bytes_ += expanded_bytes;
  return PromptStashPushResult::Stored;
}

PromptStashRestoreResult PromptStash::restore(std::uint64_t id, ComposerDraftState& draft)
{
  if (!draft.text.empty())
    return PromptStashRestoreResult::NonemptyDraft;
  auto const found = std::ranges::find(entries_, id, &PromptStashEntry::id);
  if (found == entries_.end())
    return PromptStashRestoreResult::NotFound;

  ComposerDraftState restored;
  restored.text = std::move(found->text);
  restored.cursor = clamp_composer_draft_cursor(restored.text, found->cursor);
  restored.paste_entries = std::move(found->paste_entries);
  restored.next_paste_id = found->next_paste_id;
  aggregate_expanded_bytes_ -= found->expanded_bytes;
  entries_.erase(found);
  draft = std::move(restored);
  return PromptStashRestoreResult::Restored;
}

PromptStashRestoreResult PromptStash::restore_latest(ComposerDraftState& draft)
{
  if (entries_.empty())
    return draft.text.empty() ? PromptStashRestoreResult::NotFound : PromptStashRestoreResult::NonemptyDraft;
  return restore(entries_.back().id, draft);
}

void PromptStash::clear() noexcept
{
  entries_.clear();
  aggregate_expanded_bytes_ = 0;
}

std::vector<std::uint64_t> PromptStash::ids_newest_first() const
{
  std::vector<std::uint64_t> ids;
  ids.reserve(entries_.size());
  for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry)
    ids.push_back(entry->id);
  return ids;
}

SelectListView PromptStash::select_list_view() const
{
  SelectListView view{.title = "Prompt stash",
                      .subtitle = "Process-memory drafts · newest first",
                      .items = {},
                      .selected_item_index = 0,
                      .query = {},
                      .placeholder = "Search stashed prompts",
                      .empty_text = "No stashed prompts",
                      .footer_hint = "Type to filter · Enter restore · Esc close"};
  view.items.reserve(entries_.size());
  for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry)
  {
    ComposerDraftState preview_draft;
    preview_draft.text = entry->text;
    preview_draft.cursor = entry->cursor;
    preview_draft.paste_entries = entry->paste_entries;
    preview_draft.next_paste_id = entry->next_paste_id;
    view.items.push_back(SelectListItemView{.non_searchable_suffix = {},
                                            .priority_suffix = {},
                                            .value = "prompt-stash:" + std::to_string(entry->id),
                                            .label = stash_preview(preview_draft),
                                            .description = std::to_string(entry->expanded_bytes) + " bytes",
                                            .group = "Stashed prompts",
                                            .detail = {},
                                            .badge = entry == entries_.rbegin() ? "newest" : "",
                                            .current = false,
                                            .enabled = true,
                                            .disabled_reason = {}});
  }
  return view;
}

RuntimePromptStashController::RuntimePromptStashController(RuntimePresentationState& presentation_state, RuntimeDraftState& draft_state,
                                                           RuntimeRenderer& renderer, ActiveSelectList& active_select_list, TuiKeyBindings const& key_bindings)
    : presentation_state_(presentation_state),
      draft_state_(draft_state),
      renderer_(renderer),
      active_select_list_(active_select_list),
      key_bindings_(key_bindings)
{
}

void RuntimePromptStashController::reset_draft_presentation()
{
  draft_state_.clear_selection();
  draft_state_.jump_mode = ComposerJumpMode::None;
  draft_state_.history_index.reset();
  draft_state_.draft_input.clear();
  draft_state_.selected_slash_command_index = 0;
  draft_state_.slash_palette_suppressed = false;
  draft_state_.path_completion_force_active = false;
  draft_state_.draft_scroll_offset = 0;
  draft_state_.pending_escape_clear = false;
}

bool RuntimePromptStashController::trigger()
{
  auto& snapshot = presentation_state_.snapshot;
  if (draft_state_.draft.text.empty())
    return open_selector();
  if (!presentation_state_.pending_image_attachments.empty())
  {
    snapshot.status = "cannot stash a prompt while image attachments are pending";
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }

  switch (stash_.push(draft_state_.draft))
  {
    case PromptStashPushResult::Stored:
      reset_composer_draft(draft_state_.draft);
      reset_draft_presentation();
      snapshot.status = "prompt stashed";
      return renderer_.request_render();
    case PromptStashPushResult::EmptyDraft:
      return open_selector();
    case PromptStashPushResult::EntryTooLarge:
      snapshot.status = "prompt stash entry exceeds 64 KiB expanded-text limit";
      break;
    case PromptStashPushResult::EntryLimitReached:
      snapshot.status = "prompt stash is full (16 entries)";
      break;
    case PromptStashPushResult::AggregateLimitReached:
      snapshot.status = "prompt stash exceeds 256 KiB aggregate limit";
      break;
  }
  ava::core::Application::instance().terminal_context().beep();
  return renderer_.request_render();
}

bool RuntimePromptStashController::open_selector()
{
  auto& snapshot = presentation_state_.snapshot;
  snapshot.select_list = stash_.select_list_view();
  active_select_list_ = ActiveSelectList::PromptStash;
  snapshot.status = stash_.empty() ? "prompt stash is empty" : "prompt stash opened";
  return renderer_.request_render();
}

bool RuntimePromptStashController::restore(std::uint64_t id)
{
  auto& snapshot = presentation_state_.snapshot;
  switch (stash_.restore(id, draft_state_.draft))
  {
    case PromptStashRestoreResult::Restored:
      reset_draft_presentation();
      snapshot.select_list.reset();
      active_select_list_ = ActiveSelectList::None;
      snapshot.status = "stashed prompt restored";
      return renderer_.request_render();
    case PromptStashRestoreResult::NonemptyDraft:
      snapshot.status = "cannot restore a stashed prompt over a nonempty draft";
      break;
    case PromptStashRestoreResult::NotFound:
      snapshot.status = "stashed prompt is no longer available";
      break;
  }
  ava::core::Application::instance().terminal_context().beep();
  return renderer_.request_render();
}

bool RuntimePromptStashController::pop_latest()
{
  auto& snapshot = presentation_state_.snapshot;
  if (!draft_state_.draft.text.empty())
  {
    snapshot.status = "cannot restore a stashed prompt over a nonempty draft";
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  auto const ids = stash_.ids_newest_first();
  if (ids.empty())
  {
    snapshot.status = "prompt stash is empty";
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  return restore(ids.front());
}

bool RuntimePromptStashController::clear()
{
  auto& snapshot = presentation_state_.snapshot;
  auto const count = stash_.size();
  stash_.clear();
  if (active_select_list_ == ActiveSelectList::PromptStash)
  {
    snapshot.select_list.reset();
    active_select_list_ = ActiveSelectList::None;
  }
  snapshot.status = count == 0 ? "prompt stash already empty" : "prompt stash cleared";
  return renderer_.request_render();
}

std::optional<bool> RuntimePromptStashController::handle_selector_input(InputEvent const& event)
{
  auto& snapshot = presentation_state_.snapshot;
  if (active_select_list_ != ActiveSelectList::PromptStash || !snapshot.select_list)
    return std::nullopt;

  SelectListInputResult result;
  if (event.key == Key::MouseLeftPress || event.key == Key::MouseLeftClick)
  {
    if (auto const clicked = select_list_selection_for_screen_position(snapshot, event.mouse_row, event.mouse_column))
      result = SelectListInputResult{.selected_item_index = *clicked, .query = snapshot.select_list->query, .action = SelectListInputAction::Resolve};
    else
      result = handle_select_list_input(*snapshot.select_list, event, key_bindings_);
  }
  else
  {
    result = handle_select_list_input(*snapshot.select_list, event, key_bindings_);
  }

  if (result.action == SelectListInputAction::Redraw)
  {
    snapshot.select_list->selected_item_index = result.selected_item_index;
    snapshot.select_list->query = std::move(result.query);
    return renderer_.request_render();
  }
  if (result.action == SelectListInputAction::Cancel)
  {
    snapshot.select_list.reset();
    active_select_list_ = ActiveSelectList::None;
    snapshot.status = "view canceled";
    return renderer_.request_render();
  }
  if (result.action != SelectListInputAction::Resolve)
    return true;
  if (result.selected_item_index >= snapshot.select_list->items.size())
  {
    snapshot.status = "prompt stash is empty";
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  auto const id = stash_id_from_value(snapshot.select_list->items[result.selected_item_index].value);
  if (!id)
  {
    snapshot.status = "stashed prompt selection is invalid";
    ava::core::Application::instance().terminal_context().beep();
    return renderer_.request_render();
  }
  return restore(*id);
}

}  // namespace ava::tui
