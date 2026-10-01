#pragma once

#include "ava/event/RuntimeEvent.h"
#include "ava/agent/question.h"
#include "ava/tui/terminal.h"
#include "ava/permissions/permission.h"
#include "ava/core/result.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include "debug.h"

namespace ava::tui {

namespace detail {
[[nodiscard]] bool prompt_wheel_input_suppressed(terminal::Key key, std::optional<std::chrono::steady_clock::time_point> const& deadline,
                                                 std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
} // namespace detail

struct ComposerSnapshot;
struct TuiRuntimeOptions;
enum class ActiveSelectList;
class RuntimeRenderer;
class TuiSessionGrantRegistry;

struct PendingPermissionRequest
{
  explicit PendingPermissionRequest(ava::permissions::PermissionPrompt prompt_in);

  ava::permissions::PermissionPrompt prompt;
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<ava::core::Result<ava::permissions::PermissionResolutionDecision>> result;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

struct PendingQuestionRequest
{
  explicit PendingQuestionRequest(ava::agent::QuestionPrompt prompt_in);

  ava::agent::QuestionPrompt prompt;
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<ava::core::Result<ava::agent::QuestionAnswer>> result;

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
};

enum class SearchInputPromptDispatchResult
{
  PromptServiced,
  InputHandled,
  InputFailed,
};

class RuntimePromptCoordinator final
{
 public:
  RuntimePromptCoordinator(TuiRuntimeOptions& options, ComposerSnapshot& snapshot, TuiSessionGrantRegistry& session_grants, RuntimeRenderer& renderer,
                           ActiveSelectList* active_select_list = nullptr);
  RuntimePromptCoordinator(RuntimePromptCoordinator const&) = delete;
  RuntimePromptCoordinator& operator=(RuntimePromptCoordinator const&) = delete;

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

  AVA_DEBUG_PRINT_MEMBERS_OPT_OUT

 private:
  struct ClaimedPromptRequest
  {
    std::shared_ptr<PendingPermissionRequest> permission;
    std::shared_ptr<PendingQuestionRequest> question;

    [[nodiscard]] explicit operator bool() const noexcept { return permission || question; }

    AVA_DEBUG_PRINT_MEMBERS_OPT_OUT
  };

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
  [[nodiscard]] bool render();
  static void complete_permission_request(std::shared_ptr<PendingPermissionRequest> const& request,
                                          ava::core::Result<ava::permissions::PermissionResolutionDecision> result);
  static void complete_question_request(std::shared_ptr<PendingQuestionRequest> const& request, ava::core::Result<ava::agent::QuestionAnswer> result);

  TuiRuntimeOptions& options_;
  ComposerSnapshot& snapshot_;
  TuiSessionGrantRegistry& session_grants_;
  RuntimeRenderer& renderer_;
  // Optional: when set, prompt acquisition closes an open startup overview select-list.
  ActiveSelectList* active_select_list_ = nullptr;
  std::mutex prompt_request_mutex_;
  std::deque<std::shared_ptr<PendingPermissionRequest>> pending_permission_requests_;
  std::deque<std::shared_ptr<PendingQuestionRequest>> pending_question_requests_;
  std::atomic_bool accept_prompt_requests_{true};
  std::mutex prompt_audit_mutex_;
  ava::event::RuntimeEventSink prompt_audit_sink_;
};

}  // namespace ava::tui
