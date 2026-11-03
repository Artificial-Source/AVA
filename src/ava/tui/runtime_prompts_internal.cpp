#include "sys.h"
#include "PendingPermissionRequest.h"
#include "PendingQuestionRequest.h"
#include "SelectorHost.h"
#include "TuiEventLoop.h"
#include "ava/event/events.h"
#include "ava/tui/TuiFrontEnd.h"
#include "ava/tui/composer.h"
#include "ava/tui/runtime.h"
#include "ava/tui/runtime_input_internal.h"
#include "ava/tui/runtime_prompts_internal.h"
#include "ava/tui/runtime_render_internal.h"
#include "ava/tui/runtime_state_internal.h"
#include "ava/tui/runtime_transcript_internal.h"
#include "ava/tui/runtime_views_internal.h"
#include "ava/tui/session_grants.h"
#include "ava/tui/terminal.h"
#include "ava/core/Application.h"

#include <chrono>
#include <utility>

namespace ava::tui {
using Signals = core::Signals;
using runtime_input::RuntimeInput;
using runtime_transcript::copy_text_from_answer;
using runtime_transcript::copy_text_to_terminal_clipboard;
using runtime_transcript::question_answer_audit_detail;
using runtime_views::permission_prompt_status;
using runtime_views::permission_prompt_view;
using runtime_views::question_answer_from_view;
using runtime_views::question_prompt_view;

bool detail::prompt_wheel_input_suppressed(terminal::Key key, std::optional<std::chrono::steady_clock::time_point> const& deadline,
                                           std::chrono::steady_clock::time_point now)
{
  using Key = terminal::Key;
  return (key == Key::MouseWheelUp || key == Key::MouseWheelDown) && deadline && now < *deadline;
}

RuntimePromptCoordinator::RuntimePromptCoordinator(TuiFrontEnd const& frontend, ComposerSnapshot& snapshot, TuiSessionGrantRegistry& session_grants,
                                                   RuntimeRenderer& renderer, TuiEventLoop& loop, SelectorHost* selectors)
    : frontend_(frontend),
      snapshot_(snapshot),
      session_grants_(session_grants),
      renderer_(renderer),
      selectors_(selectors),
      wakeup_(loop.wakeup()),
      auto_resolution_timer_(loop.member_timer(*this, &RuntimePromptCoordinator::mark_auto_resolution_due)),
      wheel_expiry_timer_(loop.member_timer(*this, &RuntimePromptCoordinator::expire_wheel_suppression))
{
}

void RuntimePromptCoordinator::close_competing_selector()
{
  if (!selectors_)
    return;
  // Process-local overview and prompt-stash selectors may be open during an
  // active run. Prompt authority closes either without mutating its owned state.
  if (selectors_->kind() != ActiveSelectList::Overview && selectors_->kind() != ActiveSelectList::PromptStash)
    return;
  selectors_->close();
}

RuntimePromptCoordinator::~RuntimePromptCoordinator()
{
  shutdown();
}

void RuntimePromptCoordinator::open_admission()
{
  std::lock_guard lock(prompt_request_mutex_);
  accept_prompt_requests_ = true;
  cancellation_pending_ = false;
}

void RuntimePromptCoordinator::shutdown()
{
  fail_pending_requests();
  static_cast<void>(cancel_active(PromptCancelReason::Shutdown));
  set_audit_sink({});
  clear_prompt_projection();
}

void RuntimePromptCoordinator::set_audit_sink(ava::event::RuntimeEventSink sink)
{
  std::lock_guard<std::mutex> lock(prompt_audit_mutex_);
  prompt_audit_sink_ = std::move(sink);
}

void RuntimePromptCoordinator::emit_prompt_audit(std::string status, std::string text, std::string permission_request_id, std::string tool_name,
                                                 std::string reason, std::string resolution_reason)
{
  auto& prompt_audit_mutex = prompt_audit_mutex_;
  auto& prompt_audit_sink = prompt_audit_sink_;
  ava::event::RuntimeEventSink sink;
  {
    std::lock_guard<std::mutex> lock(prompt_audit_mutex);
    sink = prompt_audit_sink;
  }
  if (!sink)
    return;
  // Preserve the legacy empty timestamp/session metadata; do not invent IDs.
  ava::event::RuntimeEventMetadata metadata;
  ava::event::ProviderPayload payload;
  payload.status = std::move(status);
  payload.text = std::move(text);
  payload.tool = std::move(tool_name);
  payload.reason = std::move(reason);
  payload.error_details = std::move(resolution_reason);
  if (!permission_request_id.empty())
    payload.permission_request_ids.push_back(std::move(permission_request_id));
  static_cast<void>(ava::event::emit_event(sink, ava::event::RuntimeEvent{std::move(metadata), ava::event::ProviderEvent{.payload = std::move(payload)}}));
}

std::optional<RuntimePromptCoordinator::PermissionResult> RuntimePromptCoordinator::start_permission_prompt(ava::permissions::PermissionPrompt const& prompt)
{
  auto& snapshot = snapshot_;
  auto& command_session_grants = session_grants_;
  auto permission_label = std::string("permission requested");
  if (!prompt.tool_name.empty())
    permission_label += ": " + prompt.tool_name;
  if (!prompt.command.empty())
    permission_label += " " + prompt.command;
  if (prompt.target_path.has_filename())
    permission_label += " " + prompt.target_path.generic_string();
  emit_prompt_audit("tui:permission_request", std::move(permission_label), prompt.permission_request_id, prompt.tool_name, prompt.reason);
  // A durable Deny never grants execution authority, so preserve it even
  // when one-shot Critical/unverified commands cannot be remembered as Allows.
  auto const remember_availability = permission_prompt_remember_availability(prompt, true);
  allow_remember_available_ = remember_availability.allow_remember_available;
  deny_remember_available_ = remember_availability.deny_remember_available;
  allow_session_available_ = tui_session_grant_eligible(prompt);
  if (allow_session_available_ && command_session_grants.matches(snapshot.session_id, prompt))
  {
    emit_prompt_audit("tui:permission_allow", "permission allowed for this session: " + prompt.tool_name, prompt.permission_request_id, prompt.tool_name,
                      prompt.reason, "reused tui session grant");
    {
      auto lock = renderer_.lock_ui();
      snapshot.status = "permission allowed for this session";
    }
    static_cast<void>(renderer_.render());
    ava::permissions::PermissionResolutionDecision decision{ava::permissions::PermissionResolution::AllowSessionGrant};
    decision.resolution_source = "tui_session_grant";
    return PermissionResult{std::move(decision)};
  }
  {
    auto lock = renderer_.lock_ui();
    // Competing prompt authority closes process-local selectors so they cannot hide under the prompt and reappear.
    close_competing_selector();
    snapshot.permission_prompt = permission_prompt_view(prompt);
    snapshot.permission_prompt->selected_choice = PermissionPromptChoice::Deny;
    snapshot.permission_prompt->allow_session_available = allow_session_available_;
    snapshot.permission_prompt->allow_remember_available = allow_remember_available_;
    snapshot.permission_prompt->deny_remember_available = deny_remember_available_;
    snapshot.status = allow_session_available_ || allow_remember_available_ || deny_remember_available_
                          ? permission_prompt_status(allow_session_available_, allow_remember_available_, deny_remember_available_)
                          : "permission required: A=allow D=reject G=guide rejection Tab/Left/Right choose Enter/Space confirm Esc reject";
  }
  ava::core::Application::instance().terminal_context().beep();
  if (!renderer_.render())
  {
    return PermissionResult{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render permission prompt"))};
  }

  return std::nullopt;
}

RuntimePromptCoordinator::PermissionResult RuntimePromptCoordinator::resolve_permission_choice(PermissionPromptChoice selected, std::string user_guidance)
{
  auto const& prompt = std::get<std::shared_ptr<PendingPermissionRequest>>(active_request_)->prompt();
  auto& snapshot = snapshot_;
  auto& command_session_grants = session_grants_;
  auto const allow =
      selected == PermissionPromptChoice::Allow || selected == PermissionPromptChoice::AllowSession || selected == PermissionPromptChoice::AllowRemember;
  auto const remember = selected == PermissionPromptChoice::AllowRemember || selected == PermissionPromptChoice::DenyRemember;
  std::string remembered_rule_id;
  if (remember)
  {
    if (allow ? !allow_remember_available_ : !deny_remember_available_)
    {
      emit_prompt_audit("tui:permission_deny", "permission denied: remember unavailable", prompt.permission_request_id, prompt.tool_name, prompt.reason,
                        "remember unavailable");
      {
        auto lock = renderer_.lock_ui();
        snapshot.permission_prompt.reset();
        snapshot.status = "permission rule unavailable; denied";
      }
      if (!renderer_.render())
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to clear permission prompt"));
      }
      ava::permissions::PermissionResolutionDecision decision{ava::permissions::PermissionResolution::Deny, "permission rule storage is unavailable"};
      decision.resolution_source = "tui_remember_unavailable";
      return decision;
    }
    auto remembered = frontend_.remember_permission_rule(prompt, allow ? ava::permissions::PermissionAction::Allow : ava::permissions::PermissionAction::Deny);
    if (!remembered)
    {
      auto reason = "failed to remember permission rule: " + remembered.error().format();
      emit_prompt_audit("tui:permission_deny", "permission denied: " + prompt.tool_name, prompt.permission_request_id, prompt.tool_name, prompt.reason, reason);
      {
        auto lock = renderer_.lock_ui();
        snapshot.permission_prompt.reset();
        snapshot.status = "permission rule failed; denied";
      }
      if (!renderer_.render())
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to clear permission prompt"));
      }
      ava::permissions::PermissionResolutionDecision decision{ava::permissions::PermissionResolution::Deny, std::move(reason)};
      decision.resolution_source = "tui_remember_failed";
      return decision;
    }
    remembered_rule_id = remembered->rule_id;
  }

  if (selected == PermissionPromptChoice::AllowSession)
  {
    auto const grant_result = command_session_grants.add(snapshot.session_id, prompt);
    if (grant_result == TuiSessionGrantInsertResult::Ineligible || grant_result == TuiSessionGrantInsertResult::Full)
    {
      bool const cap_reached = grant_result == TuiSessionGrantInsertResult::Full;
      emit_prompt_audit("tui:permission_deny", "permission denied: session grant unavailable", prompt.permission_request_id, prompt.tool_name, prompt.reason,
                        cap_reached ? "session grant cap reached" : "session grant no longer eligible");
      {
        auto lock = renderer_.lock_ui();
        snapshot.permission_prompt.reset();
        snapshot.status = cap_reached ? "permission session grant cap reached; denied" : "permission session grant no longer eligible; denied";
      }
      if (!renderer_.render())
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to clear permission prompt"));
      }
      ava::permissions::PermissionResolutionDecision decision{ava::permissions::PermissionResolution::Deny};
      decision.resolution_source = cap_reached ? "tui_session_grant_cap_reached" : "tui_session_grant_ineligible";
      return decision;
    }
    emit_prompt_audit("tui:permission_allow", "permission allowed for this session: " + prompt.tool_name, prompt.permission_request_id, prompt.tool_name,
                      prompt.reason, grant_result == TuiSessionGrantInsertResult::Added ? "selected allow session" : "reused tui session grant");
    {
      auto lock = renderer_.lock_ui();
      snapshot.permission_prompt.reset();
      snapshot.status = "permission allowed for this session";
    }
    if (!renderer_.render())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to clear permission prompt"));
    }
    ava::permissions::PermissionResolutionDecision decision{ava::permissions::PermissionResolution::AllowSessionGrant};
    decision.resolution_source = "tui_session_grant";
    return decision;
  }

  if (allow)
  {
    emit_prompt_audit("tui:permission_allow", "permission allowed: " + prompt.tool_name, prompt.permission_request_id, prompt.tool_name, prompt.reason,
                      remember ? "selected allow and remember" : "selected allow");
    {
      auto lock = renderer_.lock_ui();
      snapshot.permission_prompt.reset();
      snapshot.status = remember ? "permission allow rule saved" : "permission allowed once";
    }
    if (!renderer_.render())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to clear permission prompt"));
    }
    ava::permissions::PermissionResolutionDecision decision{ava::permissions::PermissionResolution::Allow};
    if (remember)
    {
      decision.reason = "remembered allow rule";
      decision.resolution_source = "persistent_rule_added";
      decision.rule_id = std::move(remembered_rule_id);
    }
    return decision;
  }
  emit_prompt_audit("tui:permission_deny", "permission denied: " + prompt.tool_name, prompt.permission_request_id, prompt.tool_name, prompt.reason,
                    remember ? "selected deny and remember" : "selected deny");
  {
    auto lock = renderer_.lock_ui();
    snapshot.permission_prompt.reset();
    snapshot.status = remember ? "permission deny rule saved" : "permission denied";
  }
  if (!renderer_.render())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to clear permission prompt"));
  }
  ava::permissions::PermissionResolutionDecision decision{ava::permissions::PermissionResolution::Deny};
  if (remember)
  {
    decision.reason = "remembered deny rule";
    decision.resolution_source = "persistent_rule_added";
    decision.rule_id = std::move(remembered_rule_id);
    // Remembered deny stays separate from optional one-shot guidance.
  }
  else if (auto validated = ava::permissions::validated_permission_user_guidance(user_guidance))
  {
    decision.user_guidance = std::move(*validated);
  }
  return decision;
}

std::optional<RuntimePromptCoordinator::QuestionResult> RuntimePromptCoordinator::start_question_prompt(ava::agent::QuestionPrompt const& prompt)
{
  auto& snapshot = snapshot_;
  emit_prompt_audit("tui:question_request", prompt.question.empty() ? std::string("question requested") : "question requested: " + prompt.question);
  {
    auto lock = renderer_.lock_ui();
    // Competing prompt authority closes process-local selectors so they cannot hide under the prompt and reappear.
    close_competing_selector();
    snapshot.question_prompt = question_prompt_view(prompt);
    snapshot.status =
        prompt.multiple ? "question required: Space toggles, Enter sends, Esc cancels" : "question required: Enter sends, numbers choose, Esc cancels";
  }
  ava::core::Application::instance().terminal_context().beep();
  if (!renderer_.render())
  {
    return QuestionResult{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt"))};
  }

  if (prompt.auto_resolve)
  {
    auto_resolution_due_ = true;
    auto_resolution_timer_.repeat(std::chrono::milliseconds{100});
  }
  return std::nullopt;
}

RuntimePromptCoordinator::QuestionResult RuntimePromptCoordinator::cancel_question()
{
  auto const& prompt = std::get<std::shared_ptr<PendingQuestionRequest>>(active_request_)->prompt();
  auto& snapshot = snapshot_;
  emit_prompt_audit("tui:question_cancel", prompt.question.empty() ? std::string("question canceled") : "question canceled: " + prompt.question);
  {
    auto lock = renderer_.lock_ui();
    snapshot.question_prompt.reset();
    snapshot.status = "question canceled";
  }
  static_cast<void>(renderer_.render());
  return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt canceled"));
}

std::optional<RuntimePromptCoordinator::QuestionResult> RuntimePromptCoordinator::auto_resolve_question()
{
  auto_resolution_due_ = false;
  auto const* request = std::get_if<std::shared_ptr<PendingQuestionRequest>>(&active_request_);
  if (!request)
    return std::nullopt;
  auto const& prompt = (*request)->prompt();
  auto& snapshot = snapshot_;
  if (!prompt.auto_resolve || !prompt.auto_resolve())
    return std::nullopt;
  ava::agent::QuestionAnswer answer{.selected_options = {"done"}, .custom_text = ""};
  {
    auto lock = renderer_.lock_ui();
    snapshot.question_prompt.reset();
    snapshot.status = "question answered";
  }
  if (!renderer_.render())
  {
    return ava::core::Result<ava::agent::QuestionAnswer>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to clear question prompt"))};
  }
  emit_prompt_audit("tui:question_answer", "auto");
  return ava::core::Result<ava::agent::QuestionAnswer>{std::move(answer)};
}

PromptServiceOutcome RuntimePromptCoordinator::handle_question_input(RuntimeInput const& input)
{
  using Key = terminal::Key;
  auto& snapshot = snapshot_;
  bool const wheel_input = input.event.key == Key::MouseWheelUp || input.event.key == Key::MouseWheelDown;
  auto input_result = [&]() {
    if (!snapshot.question_prompt)
      return QuestionPromptInputResult{};
    if (input.event.key == Key::MouseLeftPress || input.event.key == Key::MouseLeftClick)
    {
      if (auto const clicked = question_option_for_screen_position(snapshot, input.event.mouse_row, input.event.mouse_column))
        return activate_question_option(*snapshot.question_prompt, *clicked);
    }
    return handle_question_prompt_input(*snapshot.question_prompt, input.event);
  }();
  if (input_result.action == QuestionPromptInputAction::Cancel)
    return complete_question(cancel_question());

  if (snapshot.question_prompt && (input_result.action == QuestionPromptInputAction::Redraw || input_result.action == QuestionPromptInputAction::Copy ||
                                   input_result.action == QuestionPromptInputAction::Resolve))
  {
    auto lock = renderer_.lock_ui();
    if (snapshot.question_prompt)
    {
      snapshot.question_prompt->selected_option_index = input_result.selected_option_index;
      snapshot.question_prompt->options = std::move(input_result.options);
      snapshot.question_prompt->custom_text = std::move(input_result.custom_text);
    }
  }

  if (input_result.action == QuestionPromptInputAction::Copy)
  {
    {
      auto lock = renderer_.lock_ui();
      snapshot.status = copy_text_to_terminal_clipboard(input_result.copy_text) ? "clipboard copy request sent" : "clipboard copy failed";
    }
    emit_prompt_audit("tui:question_copy", "question copy");
    if (!renderer_.render())
    {
      return complete_question(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt")));
    }
    return PromptServiceOutcome::Active;
  }

  if (input_result.action == QuestionPromptInputAction::Resolve)
  {
    auto answer =
        ava::core::Result<ava::agent::QuestionAnswer>{std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt was dismissed"))};
    {
      auto lock = renderer_.lock_ui();
      if (snapshot.question_prompt)
        answer = question_answer_from_view(*snapshot.question_prompt);
      snapshot.question_prompt.reset();
      snapshot.status = answer ? "question answered" : "question canceled";
    }
    if (!renderer_.render())
    {
      return complete_question(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to clear question prompt")));
    }
    if (answer)
    {
      if (auto copy_text = copy_text_from_answer(*answer))
      {
        {
          auto lock = renderer_.lock_ui();
          snapshot.status = copy_text_to_terminal_clipboard(*copy_text) ? "clipboard copy request sent" : "clipboard copy failed";
        }
        static_cast<void>(renderer_.render());
      }
      emit_prompt_audit("tui:question_answer", question_answer_audit_detail(*answer));
    }
    else
    {
      emit_prompt_audit("tui:question_cancel", "question canceled");
    }
    return complete_question(std::move(answer));
  }

  if (input_result.action == QuestionPromptInputAction::Redraw)
  {
    if (!renderer_.render())
    {
      return complete_question(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt")));
    }
    if (wheel_input)
      suppress_wheel_input();
    return PromptServiceOutcome::Active;
  }

  if (!renderer_.render())
  {
    return complete_question(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt")));
  }
  return PromptServiceOutcome::Active;
}

PromptServiceOutcome RuntimePromptCoordinator::handle_permission_input(RuntimeInput const& input)
{
  using Key = terminal::Key;
  auto& snapshot = snapshot_;
  if (input.event.key == Key::Escape)
  {
    // Keep the claim held until the caller publishes run stop, then calls
    // cancel_active(RunStop) after this handler returns. This preserves stop
    // before worker release without reentering the modal's current input frame.
    return PromptServiceOutcome::RequestRunStop;
  }
  auto input_result = snapshot.permission_prompt ? handle_permission_prompt_input(*snapshot.permission_prompt, input.event) : PermissionPromptInputResult{};
  switch (input_result.action)
  {
    case PermissionPromptInputAction::ResolveAllow:
      return complete_permission(resolve_permission_choice(PermissionPromptChoice::Allow));
    case PermissionPromptInputAction::ResolveAllowSession:
      return complete_permission(resolve_permission_choice(PermissionPromptChoice::AllowSession));
    case PermissionPromptInputAction::ResolveDeny:
      return complete_permission(resolve_permission_choice(PermissionPromptChoice::Deny, std::move(input_result.guidance_text)));
    case PermissionPromptInputAction::ResolveAllowRemember:
      return complete_permission(resolve_permission_choice(PermissionPromptChoice::AllowRemember));
    case PermissionPromptInputAction::ResolveDenyRemember:
      return complete_permission(resolve_permission_choice(PermissionPromptChoice::DenyRemember));
    default:
      break;
  }
  if (input_result.action == PermissionPromptInputAction::Redraw && snapshot.permission_prompt)
  {
    {
      auto lock = renderer_.lock_ui();
      snapshot.permission_prompt->selected_choice = input_result.selected_choice;
      snapshot.permission_prompt->guidance_mode = input_result.guidance_mode;
      snapshot.permission_prompt->guidance_text = std::move(input_result.guidance_text);
    }
    if (!renderer_.render())
      return complete_permission(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render permission prompt")));
    if (input.event.key == Key::MouseWheelUp || input.event.key == Key::MouseWheelDown)
      suppress_wheel_input();
    return PromptServiceOutcome::Active;
  }
  {
    auto lock = renderer_.lock_ui();
    bool const guidance_active = snapshot.permission_prompt && snapshot.permission_prompt->guidance_mode;
    bool const has_extended =
        snapshot.permission_prompt && (snapshot.permission_prompt->allow_session_available || snapshot.permission_prompt->allow_remember_available ||
                                       snapshot.permission_prompt->deny_remember_available);
    if (guidance_active)
      snapshot.status = "permission guidance: type optional reason Enter reject Esc reject";
    else
      snapshot.status =
          has_extended ? permission_prompt_status(snapshot.permission_prompt->allow_session_available, snapshot.permission_prompt->allow_remember_available,
                                                  snapshot.permission_prompt->deny_remember_available)
                       : "permission required: A=allow D=reject G=guide rejection Tab/Left/Right choose Enter/Space confirm Esc reject";
  }
  if (!renderer_.render())
    return complete_permission(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render permission prompt")));
  return PromptServiceOutcome::Active;
}

bool RuntimePromptCoordinator::has_active() const noexcept
{
  return !std::holds_alternative<std::monostate>(active_request_);
}

bool RuntimePromptCoordinator::has_pending_requests() const
{
  std::lock_guard lock(prompt_request_mutex_);
  return !pending_permission_requests_.empty() || !pending_question_requests_.empty();
}

void RuntimePromptCoordinator::mark_auto_resolution_due(TuiTimer::Clock::time_point)
{
  auto_resolution_due_ = true;
}

void RuntimePromptCoordinator::expire_wheel_suppression(TuiTimer::Clock::time_point)
{
  wheel_suppression_deadline_.reset();
  wheel_governor_.reset();
}

void RuntimePromptCoordinator::suppress_wheel_input()
{
  wheel_suppression_deadline_ = TuiTimer::Clock::now() + WheelBurstGovernor::kAcceptedEventInterval;
  wheel_expiry_timer_.arm_at(*wheel_suppression_deadline_);
}

RuntimePromptCoordinator::ClaimedPromptRequest RuntimePromptCoordinator::retire_active_callbacks()
{
  auto_resolution_timer_.cancel();
  wheel_expiry_timer_.cancel();
  auto_resolution_due_ = false;
  wheel_suppression_deadline_.reset();
  wheel_governor_.reset();
  stop_requested_ = {};
  return std::exchange(active_request_, ClaimedPromptRequest{});
}

void RuntimePromptCoordinator::clear_prompt_projection()
{
  auto lock = renderer_.lock_ui();
  snapshot_.permission_prompt.reset();
  snapshot_.question_prompt.reset();
}

PromptServiceOutcome RuntimePromptCoordinator::complete_permission(PermissionResult result)
{
  clear_prompt_projection();
  auto request = retire_active_callbacks();
  std::get<std::shared_ptr<PendingPermissionRequest>>(request)->complete(std::move(result));
  return PromptServiceOutcome::Completed;
}

PromptServiceOutcome RuntimePromptCoordinator::complete_question(QuestionResult result)
{
  clear_prompt_projection();
  auto request = retire_active_callbacks();
  std::get<std::shared_ptr<PendingQuestionRequest>>(request)->complete(std::move(result));
  return PromptServiceOutcome::Completed;
}

void RuntimePromptCoordinator::abandon_active_request()
{
  auto request = retire_active_callbacks();
  if (auto const* permission = std::get_if<std::shared_ptr<PendingPermissionRequest>>(&request))
    (*permission)->abandon();
  else if (auto const* question = std::get_if<std::shared_ptr<PendingQuestionRequest>>(&request))
    (*question)->abandon();
  // Release the waiter before best-effort UI cleanup can fail during unwinding.
  clear_prompt_projection();
}

PromptServiceOutcome RuntimePromptCoordinator::start_claimed_request(ClaimedPromptRequest request, std::function<bool()> stop_requested,
                                                                     std::function<void()> before_prompt)
{
  active_request_ = std::move(request);
  stop_requested_ = std::move(stop_requested);
  try
  {
    if (before_prompt)
      before_prompt();
    if (auto const* permission = std::get_if<std::shared_ptr<PendingPermissionRequest>>(&active_request_))
    {
      if (auto result = start_permission_prompt((*permission)->prompt()))
        return complete_permission(std::move(*result));
    }
    else if (auto const* question = std::get_if<std::shared_ptr<PendingQuestionRequest>>(&active_request_))
    {
      if (auto result = start_question_prompt((*question)->prompt()))
        return complete_question(std::move(*result));
    }
    return service();
  }
  catch (...)
  {
    abandon_active_request();
    throw;
  }
}

PromptServiceOutcome RuntimePromptCoordinator::start_pending(std::function<bool()> stop_requested, std::function<void()> before_prompt)
{
  if (has_active())
    return service();
  ClaimedPromptRequest request;
  {
    std::lock_guard lock(prompt_request_mutex_);
    request = claim_pending_request_locked();
  }
  if (std::holds_alternative<std::monostate>(request))
    return PromptServiceOutcome::None;
  return start_claimed_request(std::move(request), std::move(stop_requested), std::move(before_prompt));
}

PromptServiceOutcome RuntimePromptCoordinator::service()
{
  if (!has_active())
    return PromptServiceOutcome::None;
  try
  {
    bool cancellation_pending;
    {
      std::lock_guard lock(prompt_request_mutex_);
      cancellation_pending = cancellation_pending_;
    }
    bool const stopped = cancellation_pending || (stop_requested_ && stop_requested_());
    bool const interrupted = Signals::received(terminal_signals);
    if (auto_resolution_due_ || stopped || interrupted)
    {
      if (auto answer = auto_resolve_question())
        return complete_question(std::move(*answer));
    }
    if (stopped)
      return cancel_active_after_auto(PromptCancelReason::RunStop);
    if (interrupted)
      return cancel_active_after_auto(PromptCancelReason::TerminalSignal);
    return PromptServiceOutcome::Active;
  }
  catch (...)
  {
    abandon_active_request();
    throw;
  }
}

PromptServiceOutcome RuntimePromptCoordinator::handle_input(RuntimeInput const& input)
{
  if (!has_active())
    return PromptServiceOutcome::None;
  try
  {
    // A question can resolve while input was decoded. Check before consuming the
    // retained event, even if its periodic discovery callback is not due yet.
    auto_resolution_due_ = true;
    auto const serviced = service();
    if (serviced != PromptServiceOutcome::Active)
      return serviced;
    if (input.resize)
    {
      wheel_expiry_timer_.cancel();
      expire_wheel_suppression(TuiTimer::Clock::now());
      if (!renderer_.render())
      {
        if (std::holds_alternative<std::shared_ptr<PendingPermissionRequest>>(active_request_))
          return complete_permission(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render permission prompt")));
        return complete_question(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt")));
      }
      return PromptServiceOutcome::Active;
    }
    auto const key = input.event.key;
    if (detail::prompt_wheel_input_suppressed(key, wheel_suppression_deadline_))
      return PromptServiceOutcome::Active;
    if (key != terminal::Key::MouseWheelUp && key != terminal::Key::MouseWheelDown)
    {
      wheel_suppression_deadline_.reset();
      wheel_expiry_timer_.cancel();
    }
    if (!runtime_wheel_input_accepted(wheel_governor_, key))
      return PromptServiceOutcome::Active;
    if (std::holds_alternative<std::shared_ptr<PendingPermissionRequest>>(active_request_))
      return handle_permission_input(input);
    return handle_question_input(input);
  }
  catch (...)
  {
    abandon_active_request();
    throw;
  }
}

PromptServiceOutcome RuntimePromptCoordinator::cancel_active_after_auto(PromptCancelReason reason)
{
  if (reason == PromptCancelReason::Shutdown)
  {
    abandon_active_request();
    return PromptServiceOutcome::Completed;
  }
  if (auto const* permission = std::get_if<std::shared_ptr<PendingPermissionRequest>>(&active_request_))
  {
    if (reason == PromptCancelReason::RunStop)
      return complete_permission(resolve_permission_choice(PermissionPromptChoice::Deny));
    auto const& prompt = (*permission)->prompt();
    emit_prompt_audit("tui:permission_deny", "permission denied: interrupted", prompt.permission_request_id, prompt.tool_name, prompt.reason, "interrupted");
    {
      auto lock = renderer_.lock_ui();
      snapshot_.permission_prompt.reset();
      snapshot_.status = "interrupted";
    }
    static_cast<void>(renderer_.render());
    return complete_permission(ava::permissions::PermissionResolution::Deny);
  }
  if (reason == PromptCancelReason::RunStop)
    return complete_question(cancel_question());
  emit_prompt_audit("tui:question_cancel", "question canceled: interrupted");
  {
    auto lock = renderer_.lock_ui();
    snapshot_.question_prompt.reset();
    snapshot_.status = "interrupted";
  }
  static_cast<void>(renderer_.render());
  return complete_question(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt interrupted")));
}

PromptServiceOutcome RuntimePromptCoordinator::cancel_active(PromptCancelReason reason)
{
  if (!has_active())
    return PromptServiceOutcome::None;
  try
  {
    if (reason != PromptCancelReason::Shutdown)
    {
      if (auto answer = auto_resolve_question())
        return complete_question(std::move(*answer));
    }
    return cancel_active_after_auto(reason);
  }
  catch (...)
  {
    abandon_active_request();
    throw;
  }
}

void RuntimePromptCoordinator::fail_active_render()
{
  if (std::holds_alternative<std::shared_ptr<PendingPermissionRequest>>(active_request_))
    static_cast<void>(complete_permission(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render permission prompt"))));
  else if (std::holds_alternative<std::shared_ptr<PendingQuestionRequest>>(active_request_))
    static_cast<void>(complete_question(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt"))));
}

void RuntimePromptCoordinator::fail_pending_requests()
{
  // Queued requests have no UI callbacks and can be interrupted immediately.
  // The TUI thread alone resolves an active claim, preserving automatic answers
  // and retiring borrowed callbacks before its waiting worker can return.
  {
    std::lock_guard lock(prompt_request_mutex_);
    accept_prompt_requests_ = false;
    cancellation_pending_ = true;
    // Do not construct temporary deques here: even an empty deque can allocate.
    // Request interruption runs no user callback and takes only its own mutex.
    for (auto const& request : pending_permission_requests_)
      request->interrupt();
    for (auto const& request : pending_question_requests_)
      request->interrupt();
    pending_permission_requests_.clear();
    pending_question_requests_.clear();
  }
  wakeup_.notify();
}

RuntimePromptCoordinator::ClaimedPromptRequest RuntimePromptCoordinator::claim_pending_request_locked()
{
  if (!pending_permission_requests_.empty())
  {
    auto request = std::move(pending_permission_requests_.front());
    pending_permission_requests_.pop_front();
    if (request->begin_resolution())
      return request;
  }
  else if (!pending_question_requests_.empty())
  {
    auto request = std::move(pending_question_requests_.front());
    pending_question_requests_.pop_front();
    if (request->begin_resolution())
      return request;
  }
  return {};
}

SearchInputPromptDispatchResult RuntimePromptCoordinator::dispatch_search_input_with_prompt_precedence(std::function<bool()> const& dispatch_search_input,
                                                                                                       std::function<bool()> stop_requested,
                                                                                                       std::function<void()> before_prompt)
{
  if (has_active())
    return service() == PromptServiceOutcome::RequestRunStop ? SearchInputPromptDispatchResult::RequestRunStop
                                                             : SearchInputPromptDispatchResult::PromptServiced;
  ClaimedPromptRequest request;
  bool input_handled = false;
  {
    std::lock_guard lock(prompt_request_mutex_);
    request = claim_pending_request_locked();
    // Only the bounded search handler may run under this lock. It cannot publish
    // prompts or run commands. Prompt startup begins after releasing the lock.
    if (std::holds_alternative<std::monostate>(request))
      input_handled = dispatch_search_input();
  }
  if (std::holds_alternative<std::monostate>(request))
    return input_handled ? SearchInputPromptDispatchResult::InputHandled : SearchInputPromptDispatchResult::InputFailed;
  auto const outcome = start_claimed_request(std::move(request), std::move(stop_requested), std::move(before_prompt));
  return outcome == PromptServiceOutcome::RequestRunStop ? SearchInputPromptDispatchResult::RequestRunStop : SearchInputPromptDispatchResult::PromptServiced;
}

ava::core::Result<ava::permissions::PermissionResolutionDecision> RuntimePromptCoordinator::request_permission(ava::permissions::PermissionPrompt const& prompt)
{
  // This worker share survives queue removal and keeps the response available
  // until the sole waiting resolver consumes it.
  std::shared_ptr<PendingPermissionRequest> request;
  {
    std::lock_guard lock(prompt_request_mutex_);
    if (!accept_prompt_requests_)
      return ava::permissions::PermissionResolution::Deny;
    request = std::make_shared<PendingPermissionRequest>(prompt);
    pending_permission_requests_.push_back(request);
  }
  wakeup_.notify();
  return request->await();
}

ava::core::Result<ava::agent::QuestionAnswer> RuntimePromptCoordinator::request_question(ava::agent::QuestionPrompt const& prompt)
{
  // UI resolution can outlive queue membership; worker and UI each retain their
  // own share until no prompt callback can refer back to the blocked worker.
  std::shared_ptr<PendingQuestionRequest> request;
  {
    std::lock_guard lock(prompt_request_mutex_);
    if (!accept_prompt_requests_)
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt interrupted"));
    request = std::make_shared<PendingQuestionRequest>(prompt);
    pending_question_requests_.push_back(request);
  }
  wakeup_.notify();
  return request->await();
}

ava::permissions::PermissionResolver RuntimePromptCoordinator::permission_resolver()
{
  return std::bind_front(&RuntimePromptCoordinator::request_permission, this);
}

ava::agent::QuestionResolver RuntimePromptCoordinator::question_resolver()
{
  return std::bind_front(&RuntimePromptCoordinator::request_question, this);
}

}  // namespace ava::tui
