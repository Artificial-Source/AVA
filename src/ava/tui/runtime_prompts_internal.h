#pragma once

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

namespace detail {
[[nodiscard]] bool prompt_wheel_input_suppressed(terminal::Key key, std::optional<std::chrono::steady_clock::time_point> const& deadline,
                                                 std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
} // namespace detail

struct ComposerSnapshot;
class TuiFrontEnd;
class SelectorHost;
class RuntimeRenderer;
class TuiSessionGrantRegistry;

class PendingPermissionRequest;
class PendingQuestionRequest;

enum class SearchInputPromptDispatchResult
{
  PromptServiced,
  InputHandled,
  InputFailed,
};

class RuntimePromptCoordinator final
{
 public:
  RuntimePromptCoordinator(TuiFrontEnd const& frontend, ComposerSnapshot& snapshot, TuiSessionGrantRegistry& session_grants, RuntimeRenderer& renderer,
                           SelectorHost* selectors = nullptr);
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
  [[nodiscard]] bool service_pending_request(std::function<bool()> const& stop_requested = {}, std::function<bool()> const& request_stop = {},
                                             std::function<void()> const& before_prompt = {});
  [[nodiscard]] SearchInputPromptDispatchResult dispatch_search_input_with_prompt_precedence(std::function<bool()> const& dispatch_search_input,
                                                                                             std::function<bool()> const& stop_requested = {},
                                                                                             std::function<bool()> const& request_stop = {},
                                                                                             std::function<void()> const& before_prompt = {});
  void set_audit_sink(ava::event::RuntimeEventSink sink);

  // Queued prompts, decisions and audit callbacks contain arbitrary user text.
  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  // Shared ownership keeps a request alive independently for its waiting worker,
  // queue and claimed UI resolution. Only the request owns result synchronization.
  using ClaimedPromptRequest = std::variant<std::monostate, std::shared_ptr<PendingPermissionRequest>, std::shared_ptr<PendingQuestionRequest>>;

  [[nodiscard]] ava::core::Result<ava::permissions::PermissionResolutionDecision> request_permission(ava::permissions::PermissionPrompt const& prompt);
  [[nodiscard]] ava::core::Result<ava::agent::QuestionAnswer> request_question(ava::agent::QuestionPrompt const& prompt);
  [[nodiscard]] ClaimedPromptRequest claim_pending_request_locked();
  void service_claimed_request(ClaimedPromptRequest request, std::function<bool()> const& stop_requested, std::function<bool()> const& request_stop,
                               std::function<void()> const& before_prompt);
  void emit_prompt_audit(std::string status, std::string text, std::string permission_request_id = {}, std::string tool_name = {}, std::string reason = {},
                         std::string resolution_reason = {});
  [[nodiscard]] ava::core::Result<ava::permissions::PermissionResolutionDecision> resolve_permission_prompt(ava::permissions::PermissionPrompt const& prompt,
                                                                                                            std::function<bool()> const& stop_requested,
                                                                                                            std::function<bool()> const& request_stop);
  [[nodiscard]] ava::core::Result<ava::agent::QuestionAnswer> resolve_question_prompt(ava::agent::QuestionPrompt const& prompt,
                                                                                      std::function<bool()> const& stop_requested,
                                                                                      std::function<bool()> const& request_stop);
  void close_competing_selector();
  void interrupt_claimed_request(ClaimedPromptRequest const& request);
  void retire_claimed_request(ClaimedPromptRequest const& request);

  TuiFrontEnd const& frontend_;
  ComposerSnapshot& snapshot_;
  TuiSessionGrantRegistry& session_grants_;
  RuntimeRenderer& renderer_;
  // Optional: when set, prompt acquisition closes an open startup overview select-list.
  SelectorHost* selectors_ = nullptr;
  std::mutex prompt_request_mutex_;
  // Queue/claim shares pair with each resolver worker's owning local request.
  std::deque<std::shared_ptr<PendingPermissionRequest>> pending_permission_requests_;
  std::deque<std::shared_ptr<PendingQuestionRequest>> pending_question_requests_;
  ClaimedPromptRequest claimed_request_;
  bool accept_prompt_requests_ = false;
  std::mutex prompt_audit_mutex_;
  ava::event::RuntimeEventSink prompt_audit_sink_;
};

}  // namespace ava::tui
