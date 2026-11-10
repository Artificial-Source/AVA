// Coordinates local TUI actions and calls to frontend services.
// See docs/engineering/tui-architecture.md for the owner relationships.
#include "sys.h"
#include "PendingAttachments.h"
#include "SelectorHost.h"
#include "SessionSelector.h"
#include "TuiTimeoutHandler.h"
#include "ava/tui/Composer.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/command_output.h"
#include "ava/tui/composer.h"
#include "ava/tui/composer_editor.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_actions_internal.h"
#include "ava/tui/runtime_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/terminal.h"
#include "ava/tui/terminal_image.h"
#include "ava/core/Application.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ava::tui {
namespace {

// TUI discovery cadence; the timer only marks work so reload callbacks run in Session order.
constexpr auto kDisplayReloadPollInterval = std::chrono::milliseconds(500);

// TUI thread: describe an imported reference and active preview protocol without loading
// image bytes; callers own the returned badge text.
std::string attachment_detail(ava::session::ImageAttachmentRef const& attachment, TerminalImageCapabilities const& capabilities)
{
  return "(" + attachment.mime_type + ", " + std::to_string(attachment.byte_size) + " bytes) preview " + terminal_image_settings_description(capabilities);
}

// TUI thread: derive a stable nonzero Kitty image identifier from at most eight leading
// hex digits. Missing hex declines an identifier; zero is reserved by the terminal protocol.
std::optional<std::size_t> kitty_image_id_from_sha(std::string_view sha)
{
  std::size_t value = 0;
  std::size_t digits = 0;
  // Use only the protocol-sized prefix and stop on invalid text. This derives a display
  // identifier; the backend remains responsible for validating the attachment hash.
  for (char const ch : sha)
  {
    auto const byte = static_cast<unsigned char>(ch);
    int nibble = -1;
    if (byte >= '0' && byte <= '9')
    {
      nibble = byte - '0';
    }
    else if (byte >= 'a' && byte <= 'f')
    {
      nibble = 10 + byte - 'a';
    }
    else if (byte >= 'A' && byte <= 'F')
    {
      nibble = 10 + byte - 'A';
    }
    else
    {
      break;
    }
    value = (value << 4U) | static_cast<std::size_t>(nibble);
    ++digits;
    if (digits == 8)
      break;
  }
  if (digits == 0)
    return std::nullopt;
  return value == 0 ? std::size_t{1} : value;
}

// TUI thread: ask the frontend to load image bytes only when previews are enabled.
// Return an immutable payload shared by badge copies; no worker uses it here.
std::optional<PendingAttachmentItem::Preview> attachment_preview(TuiFrontEnd const& frontend, ava::session::ImageAttachmentRef const& attachment,
                                                                 TerminalImageCapabilities const& capabilities, bool show_images)
{
  // Disabled image visibility must not load attachment bytes or build graphics payloads.
  if (!show_images || capabilities.images == TerminalImageProtocol::None)
    return std::nullopt;
  auto loaded = frontend.on_load_image_attachment(attachment);
  if (!loaded)
    return std::nullopt;
  // Sharing the immutable encoded bytes avoids duplicating image payloads when badge
  // display data is copied; the last preview owner releases them without frontend access.
  auto dimensions = image_dimensions_from_bytes(loaded->bytes, attachment.mime_type).value_or(ImageDimensions{.width_px = 800, .height_px = 600});
  return PendingAttachmentItem::Preview{.protocol = capabilities.images,
                                        .base64_data = std::make_shared<std::string const>(runtime_transcript::base64_encode(loaded->bytes)),
                                        .dimensions = dimensions,
                                        .image_id = kitty_image_id_from_sha(attachment.sha256)};
}

}  // namespace

RuntimeActionController::RuntimeActionController(TuiFrontEnd& frontend, RuntimePresentationState& presentation_state, Composer& draft_state,
                                                 RuntimeRenderer& renderer, SelectorHost& selectors, SessionSelector& session_selector,
                                                 PendingAttachments& attachments)
    : frontend_(frontend),
      presentation_state_(presentation_state),
      draft_state_(draft_state),
      renderer_(renderer),
      selectors_(selectors),
      session_selector_(session_selector),
      attachments_(attachments)
{
}

void RuntimeActionController::create_timers(TuiTimeoutHandler const& tui_timeout_handler, TuiTimerHandle::Clock::time_point epoch)
{
  // Attach display discovery once before servicing this nonmoving controller.
  ASSERT(!display_reload_timer_.armed());
  if (display_reload_timer_.armed())
    return;
  display_reload_timer_ = tui_timeout_handler.create_timer(*this, &RuntimeActionController::mark_display_reload_due);
  display_reload_timer_.arm_every(kDisplayReloadPollInterval, epoch);
}

void RuntimeActionController::mark_display_reload_due(TuiTimerHandle::Clock::time_point)
{
  display_reload_due_ = true;
}

DisplaySettingsReloadPollOutcome RuntimeActionController::maybe_reload_display_settings()
{
  if (!std::exchange(display_reload_due_, false))
    return DisplaySettingsReloadPollOutcome::Unchanged;

  // Preserve the frontend owner's optional result as the explicit applied/unchanged signal.
  // Do not infer applied state by comparing presentation values (overlays can mask them).
  auto reloaded = frontend_.on_maybe_reload_display_settings();
  if (!reloaded)
  {
    auto status = reloaded.error().format();
    if (last_display_reload_error_ && *last_display_reload_error_ == status)
      return DisplaySettingsReloadPollOutcome::Unchanged;
    last_display_reload_error_ = status;
    static_cast<void>(core::Application::instance().terminal_context().beep());
    {
      presentation_state_.publish_local_status(std::move(status));
    }
    // Failure path keeps last-good presentation and still paints the status when possible.
    return renderer_.render() ? DisplaySettingsReloadPollOutcome::Unchanged : DisplaySettingsReloadPollOutcome::TerminalFailure;
  }
  // A successful check ends error deduplication. Only an explicit applied result changes
  // the loaded display values; Session rebases any Settings preview before final paint.
  last_display_reload_error_.reset();
  if (!*reloaded)
    return DisplaySettingsReloadPollOutcome::Unchanged;

  auto result = std::move(**reloaded);
  auto status = result.status.empty() ? std::string("display theme auto-reloaded") : result.status;
  {
    presentation_state_.apply_local_display_settings(std::move(result.settings));
    selectors_.synchronize_startup_overview();
    presentation_state_.publish_local_status(std::move(status));
  }
  return DisplaySettingsReloadPollOutcome::Applied;
}

bool RuntimeActionController::clear_draft_for_interrupt()
{
  draft_state_.reset_interaction();
  presentation_state_.publish_status({});
  auto const changed = draft_state_.apply_action(TuiAction::ClearInput);
  // Empty text may still have selection or completion state to reset. Publish
  // the whole edit before callers decide whether Interrupt should also close.
  presentation_state_.publish_composer(draft_state_);
  return changed;
}

bool RuntimeActionController::open_external_editor()
{
  auto& terminal_context = core::Application::instance().terminal_context();
  draft_state_.reset_interaction();
  // Protocol handoff invalidates any incomplete press/drag/header arm while preserving
  // a committed transcript range.
  renderer_.cancel_pointer_interaction();
  terminal_reset_mouse_tracking();
  presentation_state_.publish_status("opening external editor");
  if (!renderer_.paint_for_handoff())
    return false;

  // Balance AVA-owned protocols after leaving curses/alt-screen so the shell
  // and $VISUAL/$EDITOR inherit a clean Kitty stack, paste, and mouse state.
  terminal_context.leave_terminal_for_handoff();
  // Frontend owns the blocking editor/process action. Restore terminal state before
  // interpreting cancellation or errors so feedback always uses the resumed TUI.
  auto edited = frontend_.on_external_editor(draft_state_.draft().text);
  refresh_geometry_from_kernel();
  terminal_context.restore_terminal_after_handoff();

  if (!edited)
  {
    presentation_state_.publish_status(edited.error().format());
    static_cast<void>(terminal_context.beep());
    return renderer_.paint_for_handoff();
  }
  if (!*edited)
  {
    presentation_state_.publish_status("external editor canceled");
    return renderer_.paint_for_handoff();
  }
  presentation_state_.publish_status(draft_state_.replace_draft(std::move(**edited)) ? "external editor updated draft"
                                                                                     : "external editor closed without changes");
  return renderer_.paint_for_handoff();
}

bool RuntimeActionController::suspend_to_background()
{
  auto& terminal_context = core::Application::instance().terminal_context();
  draft_state_.reset_interaction();
  // Suspend handoff cancels in-flight pointer interaction; committed ranges remain.
  renderer_.cancel_pointer_interaction();
  terminal_reset_mouse_tracking();

  {
    // Keep termination signals from running ordinary control handling during the terminal
    // handoff. The guard restores the prior mask only after terminal protocols are restored.
    utils::Signal::BlockGuard block_signals({SIGINT, SIGTERM});
    // Disable AVA-owned protocols after leaving curses so the stopped process's
    // shell inherits balanced keyboard/paste/mouse state. Negotiation preferences
    // are retained so resume can re-arm without re-probing OSC 11.
    terminal_context.leave_terminal_for_handoff();
    if (kill(0, SIGTSTP) != 0)
    {
      auto const saved_errno = errno;
      refresh_geometry_from_kernel();
      terminal_context.restore_terminal_after_handoff();
      presentation_state_.publish_status(std::string("failed to suspend: ") + std::strerror(saved_errno));
      static_cast<void>(terminal_context.beep());
      return renderer_.paint_for_handoff();
    }
    // Continues after fg/SIGCONT. Geometry may have changed while stopped.
    refresh_geometry_from_kernel();
    terminal_context.restore_terminal_after_handoff();
  }

  presentation_state_.publish_status("resumed from background");
  return renderer_.paint_for_handoff();
}

bool RuntimeActionController::queue_pending_image_attachment(ava::session::ImageAttachmentRef const& imported, std::string label, std::string status)
{
  if (label.empty())
    label = imported.id;
  auto const& snapshot = presentation_state_.screen();
  auto const image_capabilities = active_terminal_image_capabilities();
  auto detail = attachment_detail(imported, image_capabilities);
  if (!snapshot.show_images)
    detail += " (hidden)";
  auto const preview = attachment_preview(frontend_, imported, image_capabilities, snapshot.show_images);
  attachments_.stage(imported, PendingAttachmentItem{.label = label, .detail = detail, .preview = preview});
  presentation_state_.publish_local_status(std::move(status));
  return renderer_.render();
}

bool RuntimeActionController::paste_clipboard_image()
{
  draft_state_.reset_interaction();
  presentation_state_.publish_status("reading clipboard image");
  if (!renderer_.paint_for_handoff())
    return false;

  // Paint the pending frame before the potentially blocking clipboard import. Stage nothing
  // until import succeeds with a reference; errors and an empty clipboard retain the draft.
  auto imported = frontend_.on_paste_clipboard_image();
  if (!imported)
  {
    presentation_state_.publish_status(imported.error().format());
    static_cast<void>(core::Application::instance().terminal_context().beep());
    return renderer_.render();
  }
  if (!*imported)
  {
    presentation_state_.publish_status("no clipboard image available");
    static_cast<void>(core::Application::instance().terminal_context().beep());
    return renderer_.render();
  }
  return queue_pending_image_attachment(**imported, "clipboard image", "pasted clipboard image for next prompt");
}

bool RuntimeActionController::copy_latest_assistant_message()
{
  auto const& snapshot = presentation_state_.screen();
  auto const result = runtime_transcript::copy_latest_assistant_message(snapshot.transcript);
  presentation_state_.publish_status(runtime_transcript::latest_assistant_copy_status(result));
  if (result != runtime_transcript::LatestAssistantCopyResult::RequestSent)
    static_cast<void>(core::Application::instance().terminal_context().beep());
  return renderer_.request_render();
}

void RuntimeActionController::cycle_reasoning()
{
  presentation_state_.clear_input_feedback();
  auto result = frontend_.on_cycle_reasoning();
  if (!result)
  {
    presentation_state_.publish_status(result.error().format());
    return;
  }
  presentation_state_.publish_reasoning_feedback(std::move(*result));
}

void RuntimeActionController::toggle_thinking_visibility()
{
  auto const& snapshot = presentation_state_.screen();
  presentation_state_.publish_thinking_visibility(!snapshot.thinking_visible);
  presentation_state_.publish_status(snapshot.thinking_visible ? "thinking visible" : "thinking hidden");
  renderer_.show_live_tail(LiveTailReason::Requested);
}

bool RuntimeActionController::open_model_selector()
{
  draft_state_.cancel_escape();
  session_selector_.cancel_confirmation();
  selectors_.open(ActiveSelectList::Model, frontend_.model_selector_view());
  presentation_state_.publish_status("model selector opened");
  renderer_.show_live_tail(LiveTailReason::Requested);
  return renderer_.request_render();
}

bool RuntimeActionController::open_reasoning_selector(bool chained_from_model_selection)
{
  auto view = frontend_.reasoning_selector_view(chained_from_model_selection);
  if (!view)
  {
    if (!chained_from_model_selection)
    {
      presentation_state_.publish_status("thinking mode unavailable for current model");
      static_cast<void>(core::Application::instance().terminal_context().beep());
    }
    return true;
  }
  // After availability is known, clear stale input intents before exposing the new rows.
  // Unavailable chained requests preserve the just-completed model-switch feedback.
  draft_state_.cancel_escape();
  session_selector_.cancel_confirmation();
  selectors_.open(ActiveSelectList::Reasoning, std::move(*view));
  presentation_state_.publish_status("thinking mode selector opened");
  renderer_.show_live_tail(LiveTailReason::Requested);
  return renderer_.request_render();
}

bool RuntimeActionController::open_scoped_model_selector()
{
  draft_state_.cancel_escape();
  session_selector_.cancel_confirmation();
  selectors_.open(ActiveSelectList::ScopedModels, frontend_.scoped_model_selector_view());
  presentation_state_.publish_status("scoped model selector opened");
  renderer_.show_live_tail(LiveTailReason::Requested);
  return renderer_.request_render();
}

bool RuntimeActionController::toggle_startup_overview()
{
  auto const& snapshot = presentation_state_.screen();
  draft_state_.cancel_escape();
  session_selector_.cancel_confirmation();
  if (selectors_.kind() == ActiveSelectList::Overview && selectors_.view())
  {
    selectors_.close();
    presentation_state_.clear_startup_overview();
    presentation_state_.publish_status("overview closed");
    return renderer_.request_render();
  }
  // Read current backend facts exactly once for this popup lifetime. The callback
  // returns an owning safe DTO; local display and keybinding hints are composed here.
  presentation_state_.initialize_startup_overview(frontend_, frontend_.read_startup_overview());
  // Replace any other modal with the read-only overview; Enter/Esc close without mutation.
  presentation_state_.publish_sidebar_drawer(false, 0);
  selectors_.open(ActiveSelectList::Overview, overview_select_list_view(*snapshot.startup_overview));
  presentation_state_.publish_status("overview opened");
  renderer_.show_live_tail(LiveTailReason::Requested);
  return renderer_.request_render();
}

bool RuntimeActionController::open_fork_user_turn_selector(std::string_view initial_query)
{
  auto opened = frontend_.on_open_fork_user_turn_selector(initial_query);
  if (!opened)
  {
    presentation_state_.publish_status(opened.error().format());
    static_cast<void>(core::Application::instance().terminal_context().beep());
    return renderer_.request_render();
  }
  draft_state_.cancel_escape();
  session_selector_.cancel_confirmation();
  selectors_.open(ActiveSelectList::ForkUserTurn, std::move(*opened));
  presentation_state_.publish_status("fork-from selector opened");
  renderer_.show_live_tail(LiveTailReason::Requested);
  return renderer_.request_render();
}

bool RuntimeActionController::open_copy_user_turn_selector(std::string_view initial_query)
{
  auto opened = frontend_.on_open_copy_user_turn_selector(initial_query);
  if (!opened)
  {
    presentation_state_.publish_status(opened.error().format());
    static_cast<void>(core::Application::instance().terminal_context().beep());
    return renderer_.request_render();
  }
  draft_state_.cancel_escape();
  session_selector_.cancel_confirmation();
  selectors_.open(ActiveSelectList::CopyUserTurn, std::move(*opened));
  presentation_state_.publish_status("copy user-turn selector opened");
  renderer_.show_live_tail(LiveTailReason::Requested);
  return renderer_.request_render();
}

void RuntimeActionController::cycle_model(bool forward)
{
  auto result = frontend_.on_cycle_model(forward);
  if (!result)
  {
    presentation_state_.publish_status(result.error().format());
    static_cast<void>(core::Application::instance().terminal_context().beep());
    return;
  }
  presentation_state_.refresh_after_backend_action();
  presentation_state_.publish_status(std::move(*result));
  renderer_.show_live_tail(LiveTailReason::Requested);
}

}  // namespace ava::tui
