#pragma once

#include "PendingPermissionRequest.h"
#include "PendingQuestionRequest.h"
#include "TuiTimer.h"
#include "TuiWakeup.h"
#include "runtime_render_internal.h"
#include "ava/debug/print_members_on.h"
#include "ava/event/RuntimeEvent.h"
#include "ava/agent/question.h"
#include "ava/tui/terminal.h"
#include "ava/permissions/permission.h"
#include "ava/core/result.h"

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

namespace ava::tui {

class RuntimePresentationState;

namespace detail {
[[nodiscard]] bool prompt_wheel_input_suppressed(terminal::Key key, std::optional<std::chrono::steady_clock::time_point> const& deadline,
                                                 std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
} // namespace detail

struct ComposerSnapshot;
class TuiFrontEnd;
class SelectorHost;
class RuntimeRenderer;
class TuiSessionGrantRegistry;

class TuiEventLoop;

namespace runtime_input {
struct RuntimeInput;
}

enum class PromptServiceOutcome
{
  None,
  Active,
  Completed,
  // The claim remains held: publish run stop, then cancel_active(RunStop).
  RequestRunStop,
};

enum class PromptCancelReason
{
  RunStop,
  TerminalSignal,
  Shutdown,
};

enum class SearchInputPromptDispatchResult
{
  PromptServiced,
  RequestRunStop,
  InputHandled,
  InputFailed,
};

class RuntimePromptCoordinator final
{
 public:
  RuntimePromptCoordinator(TuiFrontEnd const& frontend, RuntimePresentationState& presentation, TuiSessionGrantRegistry& session_grants,
                           RuntimeRenderer& renderer, TuiEventLoop& loop, SelectorHost* selectors = nullptr);
  RuntimePromptCoordinator(RuntimePromptCoordinator const&) = delete;
  RuntimePromptCoordinator& operator=(RuntimePromptCoordinator const&) = delete;

  ~RuntimePromptCoordinator();

  // The session and borrowed UI collaborators outlive every resolver invocation;
  // stop admission and join workers before destroying this coordinator.
  // Open only after prior submission workers and claimed UI resolution retire.
  void open_admission();
  // TUI-thread teardown also clears this owner's prompt projections.
  void shutdown();
  [[nodiscard]] ava::permissions::PermissionResolver permission_resolver();
  [[nodiscard]] ava::agent::QuestionResolver question_resolver();
  void fail_pending_requests();
  // Retained callbacks must bind stable owners that outlive this claimed operation.
  // All modal servicing runs on the TUI thread and never reads terminal input.
  [[nodiscard]] PromptServiceOutcome start_pending(std::function<bool()> stop_requested = {}, std::function<void()> before_prompt = {});
  [[nodiscard]] PromptServiceOutcome service();
  [[nodiscard]] PromptServiceOutcome handle_input(runtime_input::RuntimeInput const& input);
  [[nodiscard]] PromptServiceOutcome cancel_active(PromptCancelReason reason);
  // TUI thread only, after a failed batch paint and before shutdown. Retire any
  // borrowed prompt callbacks before waking its worker with the original Io error.
  // A failure must not run automatic question resolution and turn into success.
  void fail_active_render();
  [[nodiscard]] bool has_active() const noexcept;
  [[nodiscard]] bool has_pending_requests() const;
  [[nodiscard]] SearchInputPromptDispatchResult dispatch_search_input_with_prompt_precedence(std::function<bool()> const& dispatch_search_input,
                                                                                             std::function<bool()> stop_requested = {},
                                                                                             std::function<void()> before_prompt = {});
  void set_audit_sink(ava::event::RuntimeEventSink sink);

  // Queued prompts, decisions and audit callbacks contain arbitrary user text.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Shared ownership keeps a request alive independently for its waiting worker,
  // queue and claimed UI resolution. Only the request owns result synchronization.
  using ClaimedPromptRequest = std::variant<std::monostate, std::shared_ptr<PendingPermissionRequest>, std::shared_ptr<PendingQuestionRequest>>;
  using PermissionResult = ava::core::Result<ava::permissions::PermissionResolutionDecision>;
  using QuestionResult = ava::core::Result<ava::agent::QuestionAnswer>;

  [[nodiscard]] ava::core::Result<ava::permissions::PermissionResolutionDecision> request_permission(ava::permissions::PermissionPrompt const& prompt);
  [[nodiscard]] ava::core::Result<ava::agent::QuestionAnswer> request_question(ava::agent::QuestionPrompt const& prompt);
  [[nodiscard]] ClaimedPromptRequest claim_pending_request_locked();
  [[nodiscard]] PromptServiceOutcome start_claimed_request(ClaimedPromptRequest request, std::function<bool()> stop_requested,
                                                           std::function<void()> before_prompt);
  void emit_prompt_audit(std::string status, std::string text, std::string permission_request_id = {}, std::string tool_name = {}, std::string reason = {},
                         std::string resolution_reason = {});
  [[nodiscard]] std::optional<PermissionResult> start_permission_prompt(ava::permissions::PermissionPrompt const& prompt);
  [[nodiscard]] std::optional<QuestionResult> start_question_prompt(ava::agent::QuestionPrompt const& prompt);
  [[nodiscard]] PermissionResult resolve_permission_choice(PermissionPromptChoice selected, std::string user_guidance = {});
  [[nodiscard]] QuestionResult cancel_question();
  [[nodiscard]] std::optional<QuestionResult> auto_resolve_question();
  [[nodiscard]] PromptServiceOutcome handle_permission_input(runtime_input::RuntimeInput const& input);
  [[nodiscard]] PromptServiceOutcome handle_question_input(runtime_input::RuntimeInput const& input);
  [[nodiscard]] PromptServiceOutcome complete_permission(PermissionResult result);
  [[nodiscard]] PromptServiceOutcome complete_question(QuestionResult result);
  [[nodiscard]] PromptServiceOutcome cancel_active_after_auto(PromptCancelReason reason);
  [[nodiscard]] ClaimedPromptRequest retire_active_callbacks();
  void clear_prompt_projection();
  void abandon_active_request();
  void mark_auto_resolution_due(TuiTimer::Clock::time_point now);
  void expire_wheel_suppression(TuiTimer::Clock::time_point now);
  void suppress_wheel_input();
  void close_competing_selector();

  TuiFrontEnd const& frontend_;
  RuntimePresentationState& presentation_;
  TuiSessionGrantRegistry& session_grants_;
  RuntimeRenderer& renderer_;
  // Optional: when set, prompt acquisition closes an open startup overview select-list.
  SelectorHost* selectors_ = nullptr;
  mutable std::mutex prompt_request_mutex_;
  // Queue/claim shares pair with each resolver worker's owning local request.
  std::deque<std::shared_ptr<PendingPermissionRequest>> pending_permission_requests_;
  std::deque<std::shared_ptr<PendingQuestionRequest>> pending_question_requests_;
  bool accept_prompt_requests_ = false;
  bool cancellation_pending_ = false;
  std::mutex prompt_audit_mutex_;
  ava::event::RuntimeEventSink prompt_audit_sink_;

  // Only the TUI thread owns a claim. Other threads publish cancellation under
  // prompt_request_mutex_; service() resolves it after checking automatic answers.
  // Retire every borrowed callback before completing and releasing its worker.
  ClaimedPromptRequest active_request_;
  std::function<bool()> stop_requested_;
  bool allow_session_available_ = false;
  bool allow_remember_available_ = false;
  bool deny_remember_available_ = false;
  bool auto_resolution_due_ = false;
  WheelBurstGovernor wheel_governor_;
  std::optional<TuiTimer::Clock::time_point> wheel_suppression_deadline_;
  TuiWakeup wakeup_;
  // The borrowed loop and this stable target outlive both registrations.
  TuiTimer auto_resolution_timer_;
  TuiTimer wheel_expiry_timer_;
};

}  // namespace ava::tui
