#include "sys.h"
#include "ava/event/events.h"
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

#include <algorithm>
#include <chrono>
#include <utility>
#include <curses.h>

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

bool detail::prompt_wheel_input_suppressed(Key key, std::optional<std::chrono::steady_clock::time_point> const& deadline,
                                           std::chrono::steady_clock::time_point now)
{
  return (key == Key::MouseWheelUp || key == Key::MouseWheelDown) && deadline && now < *deadline;
}

PendingPermissionRequest::PendingPermissionRequest(ava::permissions::PermissionPrompt prompt_in) : prompt(std::move(prompt_in))
{
}

PendingQuestionRequest::PendingQuestionRequest(ava::agent::QuestionPrompt prompt_in) : prompt(std::move(prompt_in))
{
}

RuntimePromptCoordinator::RuntimePromptCoordinator(TuiRuntimeOptions& options, ComposerSnapshot& snapshot, TuiSessionGrantRegistry& session_grants,
                                                   RuntimeRenderer& renderer, ActiveSelectList* active_select_list)
    : options_(options), snapshot_(snapshot), session_grants_(session_grants), renderer_(renderer), active_select_list_(active_select_list)
{
}

namespace {

void close_competing_select_list_for_prompt(ComposerSnapshot& snapshot, ActiveSelectList* active_select_list)
{
  if (!active_select_list)
    return;
  // Process-local overview and prompt-stash selectors may be open during an
  // active run. Prompt authority closes either without mutating its owned state.
  if (*active_select_list != ActiveSelectList::Overview && *active_select_list != ActiveSelectList::PromptStash)
    return;
  snapshot.select_list.reset();
  *active_select_list = ActiveSelectList::None;
}

}  // namespace

bool RuntimePromptCoordinator::render()
{
  return renderer_.render();
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
  auto& options = options_;
  auto& snapshot = snapshot_;
  auto& command_session_grants = session_grants_;
  auto& ui_mutex = renderer_.ui_mutex;
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
  auto const remember_availability = permission_prompt_remember_availability(prompt, static_cast<bool>(options.remember_permission_rule));
  auto const allow_remember_available = remember_availability.allow_remember_available;
  auto const deny_remember_available = remember_availability.deny_remember_available;
  bool const allow_session_available = tui_session_grant_eligible(prompt);
  if (allow_session_available && command_session_grants.matches(snapshot.session_id, prompt))
  {
    emit_prompt_audit("tui:permission_allow", "permission allowed for this session: " + prompt.tool_name, prompt.permission_request_id, prompt.tool_name,
                      prompt.reason, "reused tui session grant");
    {
      std::lock_guard<std::recursive_mutex> lock(ui_mutex);
      snapshot.status = "permission allowed for this session";
    }
    static_cast<void>(render());
    ava::permissions::PermissionResolutionDecision decision{ava::permissions::PermissionResolution::AllowSessionGrant};
    decision.resolution_source = "tui_session_grant";
    return decision;
  }
  {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex);
    // Competing prompt authority closes process-local selectors so they cannot hide under the prompt and reappear.
    close_competing_select_list_for_prompt(snapshot, active_select_list_);
    snapshot.permission_prompt = permission_prompt_view(prompt);
    snapshot.permission_prompt->selected_choice = PermissionPromptChoice::Deny;
    snapshot.permission_prompt->allow_session_available = allow_session_available;
    snapshot.permission_prompt->allow_remember_available = allow_remember_available;
    snapshot.permission_prompt->deny_remember_available = deny_remember_available;
    snapshot.status = allow_session_available || allow_remember_available || deny_remember_available
                          ? permission_prompt_status(allow_session_available, allow_remember_available, deny_remember_available)
                          : "permission required: A=allow D=reject G=guide rejection Tab/Left/Right choose Enter/Space confirm Esc reject";
  }
  static_cast<void>(beep());
  if (!render())
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
      if (!options.remember_permission_rule || (allow ? !allow_remember_available : !deny_remember_available))
      {
        emit_prompt_audit("tui:permission_deny", "permission denied: remember unavailable", prompt.permission_request_id, prompt.tool_name, prompt.reason,
                          "remember unavailable");
        {
          std::lock_guard<std::recursive_mutex> lock(ui_mutex);
          snapshot.permission_prompt.reset();
          snapshot.status = "permission rule unavailable; denied";
        }
        if (!render())
        {
          return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to clear permission prompt"));
        }
        ava::permissions::PermissionResolutionDecision decision{ava::permissions::PermissionResolution::Deny, "permission rule storage is unavailable"};
        decision.resolution_source = "tui_remember_unavailable";
        return decision;
      }
      auto remembered = options.remember_permission_rule(prompt, allow ? ava::permissions::PermissionAction::Allow : ava::permissions::PermissionAction::Deny);
      if (!remembered)
      {
        auto reason = "failed to remember permission rule: " + remembered.error().format();
        emit_prompt_audit("tui:permission_deny", "permission denied: " + prompt.tool_name, prompt.permission_request_id, prompt.tool_name, prompt.reason,
                          reason);
        {
          std::lock_guard<std::recursive_mutex> lock(ui_mutex);
          snapshot.permission_prompt.reset();
          snapshot.status = "permission rule failed; denied";
        }
        if (!render())
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
          std::lock_guard<std::recursive_mutex> lock(ui_mutex);
          snapshot.permission_prompt.reset();
          snapshot.status = cap_reached ? "permission session grant cap reached; denied" : "permission session grant no longer eligible; denied";
        }
        if (!render())
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
        std::lock_guard<std::recursive_mutex> lock(ui_mutex);
        snapshot.permission_prompt.reset();
        snapshot.status = "permission allowed for this session";
      }
      if (!render())
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
        std::lock_guard<std::recursive_mutex> lock(ui_mutex);
        snapshot.permission_prompt.reset();
        snapshot.status = remember ? "permission allow rule saved" : "permission allowed once";
      }
      if (!render())
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
      std::lock_guard<std::recursive_mutex> lock(ui_mutex);
      snapshot.permission_prompt.reset();
      snapshot.status = remember ? "permission deny rule saved" : "permission denied";
    }
    if (!render())
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
        std::lock_guard<std::recursive_mutex> lock(ui_mutex);
        snapshot.permission_prompt.reset();
        snapshot.status = "interrupted";
      }
      static_cast<void>(render());
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
      if (!render())
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
        std::lock_guard<std::recursive_mutex> lock(ui_mutex);
        if (snapshot.permission_prompt)
        {
          snapshot.permission_prompt->selected_choice = input_result.selected_choice;
          snapshot.permission_prompt->guidance_mode = input_result.guidance_mode;
          snapshot.permission_prompt->guidance_text = std::move(input_result.guidance_text);
        }
      }
      if (!render())
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render permission prompt"));
      }
      if (wheel_input)
        wheel_suppression_deadline = std::chrono::steady_clock::now() + WheelBurstGovernor::kAcceptedEventInterval;
      continue;
    }

    {
      std::lock_guard<std::recursive_mutex> lock(ui_mutex);
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
    if (!render())
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
  auto& ui_mutex = renderer_.ui_mutex;
  static_cast<void>(request_stop);
  emit_prompt_audit("tui:question_request", prompt.question.empty() ? std::string("question requested") : "question requested: " + prompt.question);
  {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex);
    // Competing prompt authority closes process-local selectors so they cannot hide under the prompt and reappear.
    close_competing_select_list_for_prompt(snapshot, active_select_list_);
    snapshot.question_prompt = question_prompt_view(prompt);
    snapshot.status =
        prompt.multiple ? "question required: Space toggles, Enter sends, Esc cancels" : "question required: Enter sends, numbers choose, Esc cancels";
  }
  static_cast<void>(beep());
  if (!render())
  {
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt"));
  }

  auto cancel_question = [&]() -> ava::core::Result<ava::agent::QuestionAnswer> {
    emit_prompt_audit("tui:question_cancel", prompt.question.empty() ? std::string("question canceled") : "question canceled: " + prompt.question);
    {
      std::lock_guard<std::recursive_mutex> lock(ui_mutex);
      snapshot.question_prompt.reset();
      snapshot.status = "question canceled";
    }
    static_cast<void>(render());
    return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt canceled"));
  };

  auto auto_resolve_question = [&]() -> std::optional<ava::core::Result<ava::agent::QuestionAnswer>> {
    if (!prompt.auto_resolve || !prompt.auto_resolve())
      return std::nullopt;
    ava::agent::QuestionAnswer answer{.selected_options = {"done"}, .custom_text = ""};
    {
      std::lock_guard<std::recursive_mutex> lock(ui_mutex);
      snapshot.question_prompt.reset();
      snapshot.status = "question answered";
    }
    if (!render())
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
        std::lock_guard<std::recursive_mutex> lock(ui_mutex);
        snapshot.question_prompt.reset();
        snapshot.status = "interrupted";
      }
      static_cast<void>(render());
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
      if (!render())
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
      std::lock_guard<std::recursive_mutex> lock(ui_mutex);
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
        std::lock_guard<std::recursive_mutex> lock(ui_mutex);
        snapshot.status = copy_text_to_terminal_clipboard(input_result.copy_text) ? "clipboard copy request sent" : "clipboard copy failed";
      }
      emit_prompt_audit("tui:question_copy", "question copy");
      if (!render())
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
        std::lock_guard<std::recursive_mutex> lock(ui_mutex);
        if (snapshot.question_prompt)
          answer = question_answer_from_view(*snapshot.question_prompt);
        snapshot.question_prompt.reset();
        snapshot.status = answer ? "question answered" : "question canceled";
      }
      if (!render())
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to clear question prompt"));
      }
      if (answer)
      {
        if (auto copy_text = copy_text_from_answer(*answer))
        {
          {
            std::lock_guard<std::recursive_mutex> lock(ui_mutex);
            snapshot.status = copy_text_to_terminal_clipboard(*copy_text) ? "clipboard copy request sent" : "clipboard copy failed";
          }
          static_cast<void>(render());
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
      if (!render())
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt"));
      }
      if (wheel_input)
        wheel_suppression_deadline = std::chrono::steady_clock::now() + WheelBurstGovernor::kAcceptedEventInterval;
      continue;
    }

    if (!render())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Io, "failed to render question prompt"));
    }
  }
}

void RuntimePromptCoordinator::complete_permission_request(std::shared_ptr<PendingPermissionRequest> const& request,
                                                           ava::core::Result<ava::permissions::PermissionResolutionDecision> result)
{
  {
    std::lock_guard<std::mutex> lock(request->mutex);
    request->result = std::move(result);
  }
  request->ready.notify_one();
}

void RuntimePromptCoordinator::complete_question_request(std::shared_ptr<PendingQuestionRequest> const& request,
                                                         ava::core::Result<ava::agent::QuestionAnswer> result)
{
  {
    std::lock_guard<std::mutex> lock(request->mutex);
    request->result = std::move(result);
  }
  request->ready.notify_one();
}

void RuntimePromptCoordinator::fail_pending_requests()
{
  auto& accept_prompt_requests = accept_prompt_requests_;
  auto& prompt_request_mutex = prompt_request_mutex_;
  auto& pending_permission_requests = pending_permission_requests_;
  auto& pending_question_requests = pending_question_requests_;
  accept_prompt_requests.store(false);
  std::deque<std::shared_ptr<PendingPermissionRequest>> permission_requests;
  std::deque<std::shared_ptr<PendingQuestionRequest>> question_requests;
  {
    std::lock_guard<std::mutex> lock(prompt_request_mutex);
    permission_requests = std::move(pending_permission_requests);
    question_requests = std::move(pending_question_requests);
    pending_permission_requests.clear();
    pending_question_requests.clear();
  }
  for (auto const& permission_request : permission_requests)
  {
    complete_permission_request(permission_request, ava::permissions::PermissionResolution::Deny);
  }
  for (auto const& question_request : question_requests)
  {
    complete_question_request(question_request, std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt interrupted")));
  }
}

RuntimePromptCoordinator::ClaimedPromptRequest RuntimePromptCoordinator::claim_pending_request_locked()
{
  ClaimedPromptRequest request;
  if (!pending_permission_requests_.empty())
  {
    request.permission = std::move(pending_permission_requests_.front());
    pending_permission_requests_.pop_front();
  }
  else if (!pending_question_requests_.empty())
  {
    request.question = std::move(pending_question_requests_.front());
    pending_question_requests_.pop_front();
  }
  return request;
}

void RuntimePromptCoordinator::service_claimed_request(ClaimedPromptRequest request, std::function<bool()> const& stop_requested,
                                                       std::function<bool()> const& request_stop, std::function<void()> const& before_prompt)
{
  if (before_prompt)
    before_prompt();
  if (request.permission)
  {
    complete_permission_request(request.permission, resolve_permission_prompt(request.permission->prompt, stop_requested, request_stop));
    return;
  }
  complete_question_request(request.question, resolve_question_prompt(request.question->prompt, stop_requested, request_stop));
}

bool RuntimePromptCoordinator::service_pending_request(std::function<bool()> const& stop_requested, std::function<bool()> const& request_stop,
                                                       std::function<void()> const& before_prompt)
{
  ClaimedPromptRequest request;
  {
    std::lock_guard<std::mutex> lock(prompt_request_mutex_);
    request = claim_pending_request_locked();
  }
  if (!request)
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
    if (!request)
      input_handled = dispatch_search_input();
  }
  if (!request)
    return input_handled ? SearchInputPromptDispatchResult::InputHandled : SearchInputPromptDispatchResult::InputFailed;
  service_claimed_request(std::move(request), stop_requested, request_stop, before_prompt);
  return SearchInputPromptDispatchResult::PromptServiced;
}

ava::permissions::PermissionResolver RuntimePromptCoordinator::permission_resolver()
{
  accept_prompt_requests_.store(true);
  return [this](ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    auto& accept_prompt_requests = accept_prompt_requests_;
    auto& prompt_request_mutex = prompt_request_mutex_;
    auto& pending_permission_requests = pending_permission_requests_;
    if (!accept_prompt_requests.load())
      return ava::permissions::PermissionResolution::Deny;
    auto request = std::make_shared<PendingPermissionRequest>(prompt);
    {
      std::lock_guard<std::mutex> lock(prompt_request_mutex);
      if (!accept_prompt_requests.load())
        return ava::permissions::PermissionResolution::Deny;
      pending_permission_requests.push_back(request);
    }
    std::unique_lock<std::mutex> lock(request->mutex);
    request->ready.wait(lock, [&]() { return request->result.has_value() || !accept_prompt_requests.load(); });
    if (!request->result)
      return ava::permissions::PermissionResolution::Deny;
    return std::move(*request->result);
  };
}

ava::agent::QuestionResolver RuntimePromptCoordinator::question_resolver()
{
  accept_prompt_requests_.store(true);
  return [this](ava::agent::QuestionPrompt const& prompt) -> ava::core::Result<ava::agent::QuestionAnswer> {
    auto& accept_prompt_requests = accept_prompt_requests_;
    auto& prompt_request_mutex = prompt_request_mutex_;
    auto& pending_question_requests = pending_question_requests_;
    if (!accept_prompt_requests.load())
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt interrupted"));
    }
    auto request = std::make_shared<PendingQuestionRequest>(prompt);
    {
      std::lock_guard<std::mutex> lock(prompt_request_mutex);
      if (!accept_prompt_requests.load())
      {
        return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt interrupted"));
      }
      pending_question_requests.push_back(request);
    }
    std::unique_lock<std::mutex> lock(request->mutex);
    request->ready.wait(lock, [&]() { return request->result.has_value() || !accept_prompt_requests.load(); });
    if (!request->result)
    {
      return std::unexpected(ava::core::Error(ava::core::ErrorCategory::Tool, "question prompt interrupted"));
    }
    return std::move(*request->result);
  };
}

}  // namespace ava::tui
