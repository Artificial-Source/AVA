#include "sys.h"
#include "PendingPermissionRequest.h"
#include "PendingQuestionRequest.h"
#include "SelectorHost.h"
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

#include <algorithm>
#include <chrono>
#include <utility>

namespace ava::tui {
using Signals = core::Signals;
using runtime_input::read_curses_input;
using runtime_input::read_curses_input_with_timeout;
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
                                                   RuntimeRenderer& renderer, SelectorHost* selectors)
    : frontend_(frontend), snapshot_(snapshot), session_grants_(session_grants), renderer_(renderer), selectors_(selectors)
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
}

void RuntimePromptCoordinator::shutdown()
{
  fail_pending_requests();
  set_audit_sink({});
  auto lock = renderer_.lock_ui();
  snapshot_.permission_prompt.reset();
  snapshot_.question_prompt.reset();
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

ava::core::Result<ava::permissions::PermissionResolutionDecision> RuntimePromptCoordinator::resolve_permission_prompt(
    ava::permissions::PermissionPrompt const& prompt, std::function<bool()> const& stop_requested, std::function<bool()> const& request_stop)
{
  using Key = terminal::Key;
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
  auto const allow_remember_available = remember_availability.allow_remember_available;
  auto const deny_remember_available = remember_availability.deny_remember_available;
  bool const allow_session_available = tui_session_grant_eligible(prompt);
  if (allow_session_available && command_session_grants.matches(snapshot.session_id, prompt))
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
    return decision;
  }
  {
    auto lock = renderer_.lock_ui();
    // Competing prompt authority closes process-local selectors so they cannot hide under the prompt and reappear.
    close_competing_selector();
    snapshot.permission_prompt = permission_prompt_view(prompt);
    snapshot.permission_prompt->selected_choice = PermissionPromptChoice::Deny;
    snapshot.permission_prompt->allow_session_available = allow_session_available;
    snapshot.permission_prompt->allow_remember_available = allow_remember_available;
    snapshot.permission_prompt->deny_remember_available = deny_remember_available;
    snapshot.status = allow_session_available || allow_remember_available || deny_remember_available
                          ? permission_prompt_status(allow_session_available, allow_remember_available, deny_remember_available)
                          : "permission required: A=allow D=reject G=guide rejection Tab/Left/Right choose Enter/Space confirm Esc reject";
  }
  ava::core::Application::instance().terminal_context().beep();
  if (!renderer_.render())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render permission prompt"));
  }

  auto resolve_choice = [&](PermissionPromptChoice selected,
                            std::string user_guidance = {}) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    auto const allow =
        selected == PermissionPromptChoice::Allow || selected == PermissionPromptChoice::AllowSession || selected == PermissionPromptChoice::AllowRemember;
    auto const remember = selected == PermissionPromptChoice::AllowRemember || selected == PermissionPromptChoice::DenyRemember;
    std::string remembered_rule_id;
    if (remember)
    {
      if (allow ? !allow_remember_available : !deny_remember_available)
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
      auto remembered =
          frontend_.remember_permission_rule(prompt, allow ? ava::permissions::PermissionAction::Allow : ava::permissions::PermissionAction::Deny);
      if (!remembered)
      {
        auto reason = "failed to remember permission rule: " + remembered.error().format();
        emit_prompt_audit("tui:permission_deny", "permission denied: " + prompt.tool_name, prompt.permission_request_id, prompt.tool_name, prompt.reason,
                          reason);
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
  };

  WheelBurstGovernor wheel_governor;
  std::optional<std::chrono::steady_clock::time_point> wheel_suppression_deadline;
  while (true)
  {
    std::optional<RuntimeInput> choice_input;
    if (wheel_suppression_deadline)
    {
      auto const remaining = std::max(std::chrono::steady_clock::duration::zero(), *wheel_suppression_deadline - std::chrono::steady_clock::now());
      choice_input = read_curses_input_with_timeout(std::chrono::ceil<std::chrono::milliseconds>(remaining));
    }
    else
    {
      choice_input = stop_requested ? read_curses_input_with_timeout(std::chrono::milliseconds(100)) : read_curses_input();
    }
    if (stop_requested && stop_requested())
    {
      return resolve_choice(PermissionPromptChoice::Deny);
    }
    if (Signals::received(terminal_signals))
    {
      emit_prompt_audit("tui:permission_deny", "permission denied: interrupted", prompt.permission_request_id, prompt.tool_name, prompt.reason, "interrupted");
      {
        auto lock = renderer_.lock_ui();
        snapshot.permission_prompt.reset();
        snapshot.status = "interrupted";
      }
      static_cast<void>(renderer_.render());
      return ava::permissions::PermissionResolution::Deny;
    }
    if (!choice_input)
    {
      wheel_governor.reset();
      wheel_suppression_deadline.reset();
      continue;
    }
    if (choice_input->resize)
    {
      wheel_governor.reset();
      wheel_suppression_deadline.reset();
      if (!renderer_.render())
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render permission prompt"));
      }
      continue;
    }

    auto const wheel_input = choice_input->event.key == Key::MouseWheelUp || choice_input->event.key == Key::MouseWheelDown;
    if (detail::prompt_wheel_input_suppressed(choice_input->event.key, wheel_suppression_deadline))
      continue;
    if (!wheel_input)
      wheel_suppression_deadline.reset();
    if (!runtime_wheel_input_accepted(wheel_governor, choice_input->event.key))
      continue;

    if (choice_input->event.key == Key::Escape && request_stop)
    {
      static_cast<void>(request_stop());
      return resolve_choice(PermissionPromptChoice::Deny);
    }

    auto input_result =
        snapshot.permission_prompt ? handle_permission_prompt_input(*snapshot.permission_prompt, choice_input->event) : PermissionPromptInputResult{};
    if (input_result.action == PermissionPromptInputAction::ResolveAllow)
    {
      return resolve_choice(PermissionPromptChoice::Allow);
    }
    if (input_result.action == PermissionPromptInputAction::ResolveAllowSession)
    {
      return resolve_choice(PermissionPromptChoice::AllowSession);
    }
    if (input_result.action == PermissionPromptInputAction::ResolveDeny)
    {
      return resolve_choice(PermissionPromptChoice::Deny, std::move(input_result.guidance_text));
    }
    if (input_result.action == PermissionPromptInputAction::ResolveAllowRemember)
    {
      return resolve_choice(PermissionPromptChoice::AllowRemember);
    }
    if (input_result.action == PermissionPromptInputAction::ResolveDenyRemember)
    {
      return resolve_choice(PermissionPromptChoice::DenyRemember);
    }
    if (input_result.action == PermissionPromptInputAction::Redraw && snapshot.permission_prompt)
    {
      {
        auto lock = renderer_.lock_ui();
        if (snapshot.permission_prompt)
        {
          snapshot.permission_prompt->selected_choice = input_result.selected_choice;
          snapshot.permission_prompt->guidance_mode = input_result.guidance_mode;
          snapshot.permission_prompt->guidance_text = std::move(input_result.guidance_text);
        }
      }
      if (!renderer_.render())
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render permission prompt"));
      }
      if (wheel_input)
        wheel_suppression_deadline = std::chrono::steady_clock::now() + WheelBurstGovernor::kAcceptedEventInterval;
      continue;
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
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render permission prompt"));
    }
  }
}

ava::core::Result<ava::agent::QuestionAnswer> RuntimePromptCoordinator::resolve_question_prompt(ava::agent::QuestionPrompt const& prompt,
                                                                                                std::function<bool()> const& stop_requested,
                                                                                                std::function<bool()> const& request_stop)
{
  auto& snapshot = snapshot_;
  static_cast<void>(request_stop);
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
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt"));
  }

  auto cancel_question = [&]() -> ava::core::Result<ava::agent::QuestionAnswer> {
    emit_prompt_audit("tui:question_cancel", prompt.question.empty() ? std::string("question canceled") : "question canceled: " + prompt.question);
    {
      auto lock = renderer_.lock_ui();
      snapshot.question_prompt.reset();
      snapshot.status = "question canceled";
    }
    static_cast<void>(renderer_.render());
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt canceled"));
  };

  auto auto_resolve_question = [&]() -> std::optional<ava::core::Result<ava::agent::QuestionAnswer>> {
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
  };

  WheelBurstGovernor wheel_governor;
  std::optional<std::chrono::steady_clock::time_point> wheel_suppression_deadline;
  auto read_question_input = [&]() -> std::optional<RuntimeInput> {
    if (!prompt.auto_resolve && !wheel_suppression_deadline && !stop_requested)
      return read_curses_input();

    auto timeout = std::chrono::milliseconds{100};
    if (wheel_suppression_deadline)
    {
      auto const remaining = std::max(std::chrono::steady_clock::duration::zero(), *wheel_suppression_deadline - std::chrono::steady_clock::now());
      auto const wheel_timeout = std::chrono::ceil<std::chrono::milliseconds>(remaining);
      timeout = prompt.auto_resolve ? std::min(timeout, wheel_timeout) : wheel_timeout;
    }
    return read_curses_input_with_timeout(timeout);
  };

  while (true)
  {
    using Key = terminal::Key;
    if (auto answer = auto_resolve_question())
      return std::move(*answer);
    auto const question_input = read_question_input();
    if (auto answer = auto_resolve_question())
      return std::move(*answer);
    if (stop_requested && stop_requested())
      return cancel_question();
    if (Signals::received(terminal_signals))
    {
      emit_prompt_audit("tui:question_cancel", "question canceled: interrupted");
      {
        auto lock = renderer_.lock_ui();
        snapshot.question_prompt.reset();
        snapshot.status = "interrupted";
      }
      static_cast<void>(renderer_.render());
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt interrupted"));
    }
    if (!question_input)
    {
      wheel_governor.reset();
      wheel_suppression_deadline.reset();
      continue;
    }
    if (question_input->resize)
    {
      wheel_governor.reset();
      wheel_suppression_deadline.reset();
      if (!renderer_.render())
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt"));
      }
      continue;
    }
    auto const wheel_input = question_input->event.key == Key::MouseWheelUp || question_input->event.key == Key::MouseWheelDown;
    if (detail::prompt_wheel_input_suppressed(question_input->event.key, wheel_suppression_deadline))
      continue;
    if (!wheel_input)
      wheel_suppression_deadline.reset();
    if (!runtime_wheel_input_accepted(wheel_governor, question_input->event.key))
      continue;

    auto input_result = [&]() {
      if (!snapshot.question_prompt)
        return QuestionPromptInputResult{};
      if (question_input->event.key == Key::MouseLeftPress || question_input->event.key == Key::MouseLeftClick)
      {
        if (auto const clicked = question_option_for_screen_position(snapshot, question_input->event.mouse_row, question_input->event.mouse_column))
          return activate_question_option(*snapshot.question_prompt, *clicked);
      }
      return handle_question_prompt_input(*snapshot.question_prompt, question_input->event);
    }();
    if (input_result.action == QuestionPromptInputAction::Cancel)
      return cancel_question();

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
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt"));
      }
      continue;
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
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to clear question prompt"));
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
      return answer;
    }

    if (input_result.action == QuestionPromptInputAction::Redraw)
    {
      if (!renderer_.render())
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt"));
      }
      if (wheel_input)
        wheel_suppression_deadline = std::chrono::steady_clock::now() + WheelBurstGovernor::kAcceptedEventInterval;
      continue;
    }

    if (!renderer_.render())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt"));
    }
  }
}

void RuntimePromptCoordinator::interrupt_claimed_request(ClaimedPromptRequest const& request)
{
  if (auto const* permission = std::get_if<std::shared_ptr<PendingPermissionRequest>>(&request))
    (*permission)->interrupt();
  else if (auto const* question = std::get_if<std::shared_ptr<PendingQuestionRequest>>(&request))
    (*question)->interrupt();
}

void RuntimePromptCoordinator::retire_claimed_request(ClaimedPromptRequest const& request)
{
  std::lock_guard lock(prompt_request_mutex_);
  if (claimed_request_ == request)
    claimed_request_ = std::monostate{};
}

void RuntimePromptCoordinator::fail_pending_requests()
{
  // Queue and claim shares keep requests alive after removal until each waiter
  // receives cancellation; claimed UI resolution keeps its own share as well.
  std::deque<std::shared_ptr<PendingPermissionRequest>> permission_requests;
  std::deque<std::shared_ptr<PendingQuestionRequest>> question_requests;
  ClaimedPromptRequest claimed_request;
  {
    std::lock_guard lock(prompt_request_mutex_);
    accept_prompt_requests_ = false;
    permission_requests.swap(pending_permission_requests_);
    question_requests.swap(pending_question_requests_);
    claimed_request = claimed_request_;
  }
  for (auto const& request : permission_requests)
    request->interrupt();
  for (auto const& request : question_requests)
    request->interrupt();
  interrupt_claimed_request(claimed_request);
}

RuntimePromptCoordinator::ClaimedPromptRequest RuntimePromptCoordinator::claim_pending_request_locked()
{
  if (!std::holds_alternative<std::monostate>(claimed_request_))
    return {};
  if (!pending_permission_requests_.empty())
  {
    auto request = std::move(pending_permission_requests_.front());
    pending_permission_requests_.pop_front();
    if (request->begin_resolution())
      claimed_request_ = std::move(request);
  }
  else if (!pending_question_requests_.empty())
  {
    auto request = std::move(pending_question_requests_.front());
    pending_question_requests_.pop_front();
    if (request->begin_resolution())
      claimed_request_ = std::move(request);
  }
  return claimed_request_;
}

void RuntimePromptCoordinator::service_claimed_request(ClaimedPromptRequest request, std::function<bool()> const& stop_requested,
                                                       std::function<bool()> const& request_stop, std::function<void()> const& before_prompt)
{
  try
  {
    if (before_prompt)
      before_prompt();
    if (auto const* permission = std::get_if<std::shared_ptr<PendingPermissionRequest>>(&request))
      (*permission)->complete(resolve_permission_prompt((*permission)->prompt(), stop_requested, request_stop));
    else if (auto const* question = std::get_if<std::shared_ptr<PendingQuestionRequest>>(&request))
      (*question)->complete(resolve_question_prompt((*question)->prompt(), stop_requested, request_stop));
  }
  catch (...)
  {
    // A failed UI callback must retire its claim before the worker may return;
    // it will never invoke the prompt's borrowed callbacks after this point.
    if (auto const* permission = std::get_if<std::shared_ptr<PendingPermissionRequest>>(&request))
      (*permission)->complete(ava::permissions::PermissionResolution::Deny);
    else if (auto const* question = std::get_if<std::shared_ptr<PendingQuestionRequest>>(&request))
      (*question)->complete(std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt interrupted")));
    retire_claimed_request(request);
    throw;
  }
  retire_claimed_request(request);
}

bool RuntimePromptCoordinator::service_pending_request(std::function<bool()> const& stop_requested, std::function<bool()> const& request_stop,
                                                       std::function<void()> const& before_prompt)
{
  ClaimedPromptRequest request;
  {
    std::lock_guard<std::mutex> lock(prompt_request_mutex_);
    request = claim_pending_request_locked();
  }
  if (std::holds_alternative<std::monostate>(request))
    return false;
  service_claimed_request(std::move(request), stop_requested, request_stop, before_prompt);
  return true;
}

SearchInputPromptDispatchResult RuntimePromptCoordinator::dispatch_search_input_with_prompt_precedence(std::function<bool()> const& dispatch_search_input,
                                                                                                       std::function<bool()> const& stop_requested,
                                                                                                       std::function<bool()> const& request_stop,
                                                                                                       std::function<void()> const& before_prompt)
{
  ClaimedPromptRequest request;
  bool input_handled = false;
  {
    std::lock_guard<std::mutex> lock(prompt_request_mutex_);
    request = claim_pending_request_locked();
    // The caller restricts this callback to the bounded search-modal handler.
    // It cannot run local commands, and prompt rendering/resolution starts only
    // after this lock is released, preserving the prompt-before-UI lock order.
    if (std::holds_alternative<std::monostate>(request))
      input_handled = dispatch_search_input();
  }
  if (std::holds_alternative<std::monostate>(request))
    return input_handled ? SearchInputPromptDispatchResult::InputHandled : SearchInputPromptDispatchResult::InputFailed;
  service_claimed_request(std::move(request), stop_requested, request_stop, before_prompt);
  return SearchInputPromptDispatchResult::PromptServiced;
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
