#pragma once

#include "output.h"
#include "run_state.h"
#include "ava/app/runtime.h"
#include "ava/agent/mode.h"
#include "ava/agent/question.h"
#include "ava/permissions/permission.h"
#include "ava/core/result.h"

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ava::app::rpc {

// Per-connection reusable authority is bounded; rejection leaves the request
// pending so clients may revoke/clear grants or choose a one-shot decision.
inline constexpr std::size_t kMaxPermissionSessionGrants = 256;
inline constexpr std::size_t kMaxPermissionSessionGrantBytes = 1024U * 1024U;

struct PendingPermissionRequest
{
  bool resolved = false;
  std::string correlation_id;
  std::string permission_request_id;
  std::string session_id;
  ava::permissions::Operation operation = ava::permissions::Operation::ReadFile;
  ava::agent::Mode mode = ava::agent::Mode::Build;
  std::string tool_name;
  std::filesystem::path target_path;
  std::string command;
  std::string command_recipe_key;
  std::string command_recipe_display;
  bool command_allows_reusable_grant = true;
  std::string reason;
  ava::permissions::PermissionRisk risk = ava::permissions::PermissionRisk::Low;
  std::optional<ava::permissions::PermissionResolutionDecision> resolution;
  std::optional<ava::core::Error> error;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PermissionSessionGrant
{
  std::string grant_id;
  std::string permission_request_id;
  std::string session_id;
  ava::permissions::Operation operation = ava::permissions::Operation::ReadFile;
  ava::agent::Mode mode = ava::agent::Mode::Build;
  std::string tool_name;
  std::filesystem::path target_path;
  // For RunCommand session grants, matching is recipe-key authoritative; raw
  // command text is retained only for display and audit.
  std::string command;
  std::string command_recipe_key;
  std::string command_recipe_display;
  std::string reason;
  ava::permissions::PermissionRisk risk = ava::permissions::PermissionRisk::Low;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PendingQuestionRequest
{
  bool resolved = false;
  std::string correlation_id;
  bool multiple = false;
  bool allow_custom = false;
  std::vector<ava::agent::QuestionOption> options;
  std::optional<ava::agent::QuestionAnswer> answer;
  std::optional<ava::core::Error> error;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

struct PendingResolverState
{
  std::mutex mutex;
  std::condition_variable cv;
  std::map<std::string, std::shared_ptr<PendingPermissionRequest>> permission_requests;
  std::map<std::string, std::shared_ptr<PendingQuestionRequest>> question_requests;
  std::vector<PermissionSessionGrant> permission_session_grants;

  AVA_DEBUG_PRINT_MEMBERS_ON
};

// Acquires the output mutex before the pending-state mutex. This is the only cancellation entry
// point for resolver requests so publication and cancellation have one global linearization order.
[[nodiscard]] bool cancel_pending_resolvers(output_ts& output, PendingResolverState& pending_state);
[[nodiscard]] std::string permission_session_grants_result_json(PendingResolverState& pending_state);
[[nodiscard]] ava::core::Result<std::string> permission_session_grant_revoke_result_json(PendingResolverState& pending_state, std::string_view grant_id);
[[nodiscard]] std::string permission_session_grants_clear_result_json(PendingResolverState& pending_state);

[[nodiscard]] ava::permissions::PermissionResolver make_rpc_permission_resolver(PendingResolverState& pending_state, output_ts& output, RpcRunState& run_state,
                                                                                runtime::session_ts const& unlocked_session,
                                                                                ava::permissions::PermissionResolver policy_resolver,
                                                                                std::string prompt_request_id);
[[nodiscard]] ava::agent::QuestionResolver make_rpc_question_resolver(PendingResolverState& pending_state, output_ts& output, RpcRunState& run_state,
                                                                      runtime::session_ts const& unlocked_session, std::string prompt_request_id);

[[nodiscard]] ava::core::VoidResult resolve_permission_reply(PendingResolverState& pending_state, std::string_view request_id, std::string_view correlation_id,
                                                             std::string_view decision, std::optional<std::string> const& reason = std::nullopt);
[[nodiscard]] ava::core::VoidResult resolve_question_reply(PendingResolverState& pending_state, std::string_view request_id, std::string_view correlation_id,
                                                           std::optional<std::string> const& answer, std::optional<std::string> const& selected,
                                                           std::optional<std::vector<std::string>> const& selected_options = std::nullopt);

}  // namespace ava::app::rpc
