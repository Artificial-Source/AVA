// Classifies selector input and requests catalog changes through the frontend.
// See docs/engineering/tui-architecture.md for the owner relationships.
#include "sys.h"
#include "SelectorHost.h"
#include "SelectorInputActions.h"
#include "SelectorInputController.h"
#include "command_output.h"
#include "runtime_input_internal.h"
#include "runtime_internal.h"
#include "runtime_render_internal.h"
#include "runtime_user_turn_selection_internal.h"
#include "terminal/Context.h"

#include <array>
#include <functional>
#include "debug.h"

namespace ava::tui {

SelectorInputController::SelectorInputController(SelectorInputActions& frontend, RuntimePresentationState& presentation, SelectorHost& selectors,
                                                 RuntimeRenderer& renderer, terminal::Context& terminal_context)
    : frontend_(frontend), presentation_state_(presentation), selectors_(selectors), renderer_(renderer), terminal_context_(terminal_context)
{
}

SelectListInputResult SelectorInputController::classify(runtime_input::RuntimeInput const& input) const
{
  // Classify only while a selector is hosted; route unhosted input to another feature first.
  ASSERT(selectors_.view());
  using Key = terminal::Key;
  auto const& view = *selectors_.view();
  // Hit-testing uses the current layout before key classification. Settings clicks must
  // remain preview-only, and disabled rows can be highlighted without calling the frontend.
  if (input.event.key == Key::MouseLeftPress || input.event.key == Key::MouseLeftClick)
  {
    if (auto const clicked = select_list_selection_for_screen_position(presentation_state_.screen(), input.event.mouse_row, input.event.mouse_column))
    {
      // Settings mouse clicks change selection/highlight only; Enter confirms.
      // Other selectors keep existing mouse-confirm behavior.
      auto action = selectors_.kind() == ActiveSelectList::Settings ? SelectListInputAction::Redraw : SelectListInputAction::Resolve;
      if (*clicked >= view.items.size() || !view.items[*clicked].enabled)
        action = SelectListInputAction::Redraw;
      return SelectListInputResult{.selected_item_index = *clicked, .query = view.query, .action = action};
    }
  }
  // Scoped editing shortcuts precede generic list bindings because they mutate the
  // current selection set rather than accepting and closing the list.
  if (selectors_.kind() == ActiveSelectList::ScopedModels)
  {
    constexpr std::array bindings{std::pair{TuiAction::ModelsSave, SelectListInputAction::ModelsSave},
                                  std::pair{TuiAction::ModelsEnableAll, SelectListInputAction::ModelsEnableAll},
                                  std::pair{TuiAction::ModelsClearAll, SelectListInputAction::ModelsClearAll},
                                  std::pair{TuiAction::ModelsToggleProvider, SelectListInputAction::ModelsToggleProvider},
                                  std::pair{TuiAction::ModelsReorderUp, SelectListInputAction::ModelsReorderUp},
                                  std::pair{TuiAction::ModelsReorderDown, SelectListInputAction::ModelsReorderDown}};
    for (auto const& [binding, action] : bindings)
      if (key_matches_action(frontend_.key_bindings(), binding, input.event.key))
        return SelectListInputResult{.selected_item_index = view.selected_item_index, .query = view.query, .action = action};
  }
  return handle_select_list_input(view, input.event, frontend_.key_bindings());
}

void SelectorInputController::apply_scoped_update(core::Result<SelectListView> updated, SelectListInputResult const& input, std::string status)
{
  if (updated)
  {
    selectors_.replace_preserving_selection(std::move(*updated), input.selected_item_index, input.query);
    presentation_state_.publish_status(std::move(status));
  }
  else
  {
    presentation_state_.publish_status(updated.error().format());
    terminal_context_.beep();
  }
}

bool SelectorInputController::handle_scoped_models(SelectListInputResult const& input)
{
  if (selectors_.kind() != ActiveSelectList::ScopedModels || !selectors_.view())
    return false;
  // Scoped edits preserve the open list. Callbacks finish before replacement, so all
  // borrowed row pointers are consumed within their branch and never retained.
  switch (input.action)
  {
    case SelectListInputAction::Resolve: {
      auto const* item = selectors_.selected_item(input.selected_item_index);
      if (!item || !item->enabled || item->value.empty())
      {
        presentation_state_.publish_status("scoped model cannot be toggled from this row");
        terminal_context_.beep();
      }
      else
        apply_scoped_update(frontend_.on_scoped_model_toggled(*selectors_.view(), item->value), input, "scoped model toggled");
      return true;
    }
    case SelectListInputAction::ModelsEnableAll:
      apply_scoped_update(frontend_.on_scoped_model_enable_all(*selectors_.view(), selectors_.visible_values(input.query)), input, "scoped models enabled");
      return true;
    case SelectListInputAction::ModelsClearAll:
      apply_scoped_update(frontend_.on_scoped_model_clear_all(*selectors_.view(), selectors_.visible_values(input.query)), input, "scoped models cleared");
      return true;
    case SelectListInputAction::ModelsToggleProvider: {
      auto const* item = selectors_.selected_item(input.selected_item_index);
      if (!item || item->value.empty())
      {
        presentation_state_.publish_status("provider toggle unavailable from this row");
        terminal_context_.beep();
      }
      else
        apply_scoped_update(frontend_.on_scoped_model_toggle_provider(*selectors_.view(), item->value), input, "scoped provider toggled");
      return true;
    }
    case SelectListInputAction::ModelsReorderUp:
    case SelectListInputAction::ModelsReorderDown: {
      auto const* item = selectors_.selected_item(input.selected_item_index);
      if (!item || item->value.empty())
      {
        presentation_state_.publish_status("scoped model reorder unavailable from this row");
        terminal_context_.beep();
      }
      else
        apply_scoped_update(frontend_.on_scoped_model_reorder(*selectors_.view(), item->value, input.action == SelectListInputAction::ModelsReorderUp), input,
                            "scoped model order updated");
      return true;
    }
    case SelectListInputAction::ModelsSave: {
      auto saved = frontend_.on_scoped_model_save();
      if (saved)
        presentation_state_.publish_status(*saved);
      else
      {
        presentation_state_.publish_status(saved.error().format());
        terminal_context_.beep();
      }
      return true;
    }
    default:
      return false;
  }
}

SelectorInputOutcome SelectorInputController::handle(SelectListInputResult const& input)
{
  if (!selectors_.view() || selectors_.kind() == ActiveSelectList::Settings ||
      (selectors_.kind() == ActiveSelectList::Session && input.action != SelectListInputAction::Cancel))
    return SelectorInputRequest::Pass;
  if (handle_scoped_models(input))
    return SelectorInputRequest::Handled;
  if (input.action != SelectListInputAction::Resolve && input.action != SelectListInputAction::Cancel)
    return SelectorInputRequest::Pass;

  // Capture identity and kind before close invalidates row borrows. Frontend callbacks
  // then operate on owned values, and cancellation cannot accidentally invoke them.
  auto const* item = selectors_.selected_item(input.selected_item_index);
  std::string selected_value = input.action == SelectListInputAction::Resolve && item ? item->value : std::string{};
  auto const resolved_list = selectors_.kind();
  selectors_.close();
  if (resolved_list == ActiveSelectList::Overview)
    presentation_state_.clear_startup_overview();
  if (input.action == SelectListInputAction::Cancel)
  {
    presentation_state_.publish_status(resolved_list == ActiveSelectList::Reasoning ? "thinking mode unchanged" : "view canceled");
    return SelectorInputRequest::Handled;
  }
  // Only frontend callbacks change backend state. Successful model/reasoning actions
  // refresh committed facts before feedback; session-changing results return to Session.
  switch (resolved_list)
  {
    case ActiveSelectList::Hotkeys:
      if (!selected_value.empty())
      {
        presentation_state_.publish_status("keybinding edit command drafted");
        return std::pair{SelectorInputTextKind::DraftCommand, std::string("/keybindings set ") + selected_value + " "};
      }
      break;
    case ActiveSelectList::Model: {
      auto selected = selectors_.dispatch_authority("switching model…", std::bind_front(&RuntimeRenderer::paint_for_handoff, &renderer_),
                                                    std::bind_front(&SelectorInputActions::on_model_selected, &frontend_, std::move(selected_value)));
      if (selected)
      {
        presentation_state_.refresh_after_backend_action();
        presentation_state_.publish_status(std::move(*selected));
        return SelectorInputRequest::OpenReasoning;
      }
      presentation_state_.publish_status(selected.error().format());
      terminal_context_.beep();
      return SelectorInputRequest::Handled;
    }
    case ActiveSelectList::Reasoning: {
      auto level = selected_value == "default" ? std::optional<std::string>{} : std::optional<std::string>{std::move(selected_value)};
      auto selected = selectors_.dispatch_authority("setting thinking mode…", std::bind_front(&RuntimeRenderer::paint_for_handoff, &renderer_),
                                                    std::bind_front(&SelectorInputActions::on_reasoning_selected, &frontend_, std::move(level)));
      if (selected)
      {
        presentation_state_.refresh_after_backend_action();
        presentation_state_.publish_status(std::move(*selected));
      }
      else
      {
        presentation_state_.publish_status(selected.error().format());
        terminal_context_.beep();
      }
      return SelectorInputRequest::Handled;
    }
    case ActiveSelectList::ForkUserTurn: {
      if (selected_value.empty())
      {
        presentation_state_.publish_status("selected user turn is unavailable");
        terminal_context_.beep();
        return SelectorInputRequest::Handled;
      }
      // Paint the pending fork status before calling the blocking frontend callback.
      presentation_state_.publish_status("forking session…");
      if (!renderer_.paint_for_handoff())
        return SelectorInputRequest::RenderFailed;
      auto opened = frontend_.on_fork_user_turn_selected(selected_value);
      if (opened)
        return std::pair{SelectorInputTextKind::OpenedSession, std::move(*opened)};
      // A fork may commit before a later catalog refresh fails. Composition must
      // reconcile the committed identity before presenting this error.
      return std::pair{SelectorInputTextKind::ReconcileSessionError, opened.error().format()};
    }
    case ActiveSelectList::CopyUserTurn: {
      auto decision = evaluate_copy_user_turn_selection(selected_value, std::bind_front(&SelectorInputActions::on_read_user_turn_text, &frontend_),
                                                        runtime_transcript::copy_text_to_terminal_clipboard);
      presentation_state_.publish_local_status(decision.status);
      if (decision.beep)
        terminal_context_.beep();
      return SelectorInputRequest::Handled;
    }
    default:
      break;
  }
  presentation_state_.publish_status("view closed");
  return SelectorInputRequest::Handled;
}

} // namespace ava::tui
