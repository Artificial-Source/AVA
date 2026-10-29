#include "sys.h"
#include "protocol.h"
#include "resolvers.h"
#include "serialization.h"
#include "serialization_json.h"
#include "ava/app/runtime/Session.h"
#include "ava/event/events.h"
#include "ava/core/ids.h"

#include <initializer_list>
#include <utility>

namespace ava::app::rpc {
namespace {

ava::core::Error no_pending_request_error(std::string_view request_id)
{
  auto error = invalid_rpc("RPC resolver reply has no matching pending request");
  error.with_context("request_id", std::string(request_id));
  return error;
}

std::string next_resolver_request_id(std::string_view prefix)
{
  return ava::core::make_id(prefix);
}

bool grant_matches(PermissionSessionGrant const& grant, ava::permissions::PermissionPrompt const& prompt, std::string_view session_id)
{
  if (grant.session_id != session_id || grant.operation != prompt.operation || grant.mode != prompt.mode || grant.tool_name != prompt.tool_name ||
      grant.target_path != prompt.target_path)
    return false;
  if (prompt.operation != ava::permissions::Operation::RunCommand)
    return grant.command == prompt.command && grant.command_recipe_key.empty();
  return prompt.command_metadata && ava::permissions::command_permission_allows_reusable_grant(*prompt.command_metadata) && !grant.command_recipe_key.empty() &&
         grant.command_recipe_key == prompt.command_metadata->workspace_recipe_key;
}

bool grant_matches(PermissionSessionGrant const& grant, PendingPermissionRequest const& request)
{
  if (grant.session_id != request.session_id || grant.operation != request.operation || grant.mode != request.mode || grant.tool_name != request.tool_name ||
      grant.target_path != request.target_path)
    return false;
  if (request.operation != ava::permissions::Operation::RunCommand)
    return grant.command == request.command && grant.command_recipe_key.empty();
  return !grant.command_recipe_key.empty() && grant.command_recipe_key == request.command_recipe_key;
}

PermissionSessionGrant grant_from_request(PendingPermissionRequest const& request)
{
  return PermissionSessionGrant{.grant_id = ava::core::make_id("permgrant"),
                                .permission_request_id = request.permission_request_id,
                                .session_id = request.session_id,
                                .operation = request.operation,
                                .mode = request.mode,
                                .tool_name = request.tool_name,
                                .target_path = request.target_path,
                                .command = request.command,
                                .command_recipe_key = request.command_recipe_key,
                                .command_recipe_display = request.command_recipe_display,
                                .reason = request.reason,
                                .risk = request.risk};
}

bool charge_grant_bytes(PermissionSessionGrant const& grant, std::size_t& remaining)
{
  // Include object storage and retained string capacities, not only payload lengths.
  for (auto const bytes : {sizeof(PermissionSessionGrant), grant.grant_id.capacity(), grant.permission_request_id.capacity(), grant.session_id.capacity(),
                           grant.tool_name.capacity(), grant.target_path.native().capacity(), grant.command.capacity(), grant.command_recipe_key.capacity(),
                           grant.command_recipe_display.capacity(), grant.reason.capacity()})
  {
    if (bytes > remaining)
      return false;
    remaining -= bytes;
  }
  return true;
}

std::string permission_session_grant_json(PermissionSessionGrant const& grant)
{
  std::string json = "{";
  json += string_field_json("grant_id", grant.grant_id);
  json += ',';
  json += string_field_json("permission_request_id", grant.permission_request_id);
  json += ',';
  json += string_field_json("session_id", grant.session_id);
  json += ',';
  json += string_field_json("operation", ava::permissions::to_string(grant.operation));
  json += ',';
  json += string_field_json("mode", ava::agent::to_string(grant.mode));
  json += ',';
  json += string_field_json("tool_name", grant.tool_name);
  json += ',';
  json += string_field_json("target_path", grant.target_path.string());
  json += ',';
  json += string_field_json("command", grant.command);
  if (!grant.command_recipe_key.empty())
  {
    json += ',';
    json += string_field_json("command_recipe_key", grant.command_recipe_key);
    json += ',';
    json += string_field_json("recipe_display", grant.command_recipe_display);
  }
  json += ',';
  json += string_field_json("reason", grant.reason);
  json += ',';
  json += string_field_json("risk", ava::permissions::to_string(grant.risk));
  json += '}';
  return json;
}

template <typename Request>
bool request_is_pending(std::map<std::string, std::shared_ptr<Request>> const& requests, std::string_view request_id, std::shared_ptr<Request> const& request)
{
  auto const found = requests.find(std::string(request_id));
  return found != requests.end() && found->second == request;
}

template <typename Request>
void erase_pending_request_if_matches(std::map<std::string, std::shared_ptr<Request>>& requests, std::string_view request_id,
                                      std::shared_ptr<Request> const& request)
{
  auto const found = requests.find(std::string(request_id));
  if (found != requests.end() && found->second == request)
    requests.erase(found);
}

}  // namespace

bool cancel_pending_resolvers(output_ts& output, PendingResolverState& pending_state)
{
  bool had_pending = false;
  {
    // Resolver publication gates acquire these locks in this order too. Never acquire pending_state
    // before output here, otherwise a canceled request could still publish after its cancellation.
    [[maybe_unused]] output_ts::wat output_w(output);
    std::lock_guard pending_lock(pending_state.mutex);
    had_pending = !pending_state.permission_requests.empty() || !pending_state.question_requests.empty();
    for (auto& [request_id, request] : pending_state.permission_requests)
    {
      static_cast<void>(request_id);
      request->resolved = true;
      request->resolution = ava::permissions::PermissionResolutionDecision{ava::permissions::PermissionResolution::Deny, "canceled"};
      request->error = canceled_error();
    }
    for (auto& [request_id, request] : pending_state.question_requests)
    {
      static_cast<void>(request_id);
      request->resolved = true;
      request->error = canceled_error();
    }
    pending_state.permission_requests.clear();
    pending_state.question_requests.clear();
  }
  pending_state.cv.notify_all();
  return had_pending;
}

std::string permission_session_grants_result_json(PendingResolverState& pending_state)
{
  std::vector<PermissionSessionGrant> grants;
  {
    std::lock_guard lock(pending_state.mutex);
    grants = pending_state.permission_session_grants;
  }

  std::string json = "{\"grants\":[";
  for (std::size_t index = 0; index < grants.size(); ++index)
  {
    if (index > 0)
      json += ',';
    json += permission_session_grant_json(grants[index]);
  }
  json += "]}";
  return json;
}

ava::core::Result<std::string> permission_session_grant_revoke_result_json(PendingResolverState& pending_state, std::string_view grant_id)
{
  PermissionSessionGrant grant;
  {
    std::lock_guard lock(pending_state.mutex);
    auto found = pending_state.permission_session_grants.end();
    for (auto it = pending_state.permission_session_grants.begin(); it != pending_state.permission_session_grants.end(); ++it)
    {
      if (it->grant_id == grant_id)
      {
        found = it;
        break;
      }
    }
    if (found == pending_state.permission_session_grants.end())
    {
      auto error = invalid_rpc("permission_grant_revoke has no matching grant_id");
      error.with_context("grant_id", std::string(grant_id));
      return std::unexpected(std::move(error));
    }
    grant = *found;
    pending_state.permission_session_grants.erase(found);
  }

  std::string json = "{";
  json += bool_field_json("revoked", true);
  json += ",\"grant\":";
  json += permission_session_grant_json(grant);
  json += '}';
  return json;
}

std::string permission_session_grants_clear_result_json(PendingResolverState& pending_state)
{
  std::size_t cleared = 0;
  {
    std::lock_guard lock(pending_state.mutex);
    cleared = pending_state.permission_session_grants.size();
    pending_state.permission_session_grants.clear();
  }

  std::string json = "{";
  json += number_field_json("cleared", cleared);
  json += '}';
  return json;
}

ava::permissions::PermissionResolver make_rpc_permission_resolver(PendingResolverState& pending_state, output_ts& output, RpcRunState& run_state,
                                                                   runtime::session_ts const& unlocked_session,
                                                                   ava::permissions::PermissionResolver policy_resolver, std::string prompt_request_id)
{
  return [&pending_state, &output, &run_state, &unlocked_session, policy_resolver = std::move(policy_resolver),
          prompt_request_id = std::move(prompt_request_id)](
             ava::permissions::PermissionPrompt const& prompt) -> ava::core::Result<ava::permissions::PermissionResolutionDecision> {
    if (cancel_requested(run_state))
      return std::unexpected(canceled_error());
    if (input_closed(run_state))
      return std::unexpected(canceled_error());

    auto const session_id = session_id_snapshot(unlocked_session);
    if (policy_resolver)
    {
      auto policy_result = policy_resolver(prompt);
      if (!policy_result)
        return std::unexpected(std::move(policy_result.error()));
      if (*policy_result == ava::permissions::PermissionResolution::Allow)
      {
        return *policy_result;
      }
      if (*policy_result == ava::permissions::PermissionResolution::Deny && policy_result->authoritative)
      {
        return *policy_result;
      }
    }
    {
      std::lock_guard lock(pending_state.mutex);
      for (auto const& grant : pending_state.permission_session_grants)
      {
        if (grant_matches(grant, prompt, session_id))
          return ava::permissions::PermissionResolution::AllowSessionGrant;
      }
    }

    auto pending = std::make_shared<PendingPermissionRequest>();
    pending->correlation_id = prompt_request_id;
    pending->permission_request_id = prompt.permission_request_id;
    pending->session_id = session_id;
    pending->operation = prompt.operation;
    pending->mode = prompt.mode;
    pending->tool_name = prompt.tool_name;
    pending->target_path = prompt.target_path;
    pending->command = prompt.command;
    pending->command_recipe_key = prompt.command_metadata ? prompt.command_metadata->workspace_recipe_key : std::string{};
    pending->command_recipe_display = prompt.command_metadata ? prompt.command_metadata->recipe_display : std::string{};
    pending->command_allows_reusable_grant = !prompt.command_metadata || ava::permissions::command_permission_allows_reusable_grant(*prompt.command_metadata);
    pending->reason = prompt.reason;
    pending->risk = prompt.risk;
    std::string request_id;
    {
      std::lock_guard lock(pending_state.mutex);
      request_id = next_resolver_request_id("permission");
      pending_state.permission_requests[request_id] = pending;
    }

    if (cancel_requested(run_state) || input_closed(run_state))
    {
      std::lock_guard lock(pending_state.mutex);
      erase_pending_request_if_matches(pending_state.permission_requests, request_id, pending);
      return std::unexpected(canceled_error());
    }

    // Build the complete record before taking either publication lock. write_record_if then takes
    // output first and briefly checks the exact map entry under pending_state before stream I/O.
    auto envelope = resolver_event_envelope("permission_requested", prompt_request_id, prompt_request_id,
                                            session_id_snapshot(unlocked_session),
                                             permission_request_payload_json(request_id, prompt));
    auto const record = ava::event::serialize_event_envelope_jsonl(envelope);
    auto written = Output::write_record_if(output, record, [&pending_state, &request_id, &pending] {
      std::lock_guard lock(pending_state.mutex);
      return request_is_pending(pending_state.permission_requests, request_id, pending);
    });
    if (!written)
    {
      std::lock_guard lock(pending_state.mutex);
      erase_pending_request_if_matches(pending_state.permission_requests, request_id, pending);
      return std::unexpected(std::move(written.error()));
    }
    if (*written == OutputWriteResult::Skipped)
      return std::unexpected(canceled_error());

    std::unique_lock lock(pending_state.mutex);
    pending_state.cv.wait(lock, [&pending] { return pending->resolved; });
    if (pending->error)
      return std::unexpected(*pending->error);
    if (!pending->resolution)
      return std::unexpected(canceled_error());
    return *pending->resolution;
  };
}

ava::agent::QuestionResolver make_rpc_question_resolver(PendingResolverState& pending_state, output_ts& output, RpcRunState& run_state,
                                                         runtime::session_ts const& unlocked_session, std::string prompt_request_id)
{
  return [&pending_state, &output, &run_state, &unlocked_session,
          prompt_request_id = std::move(prompt_request_id)](ava::agent::QuestionPrompt const& prompt) -> ava::core::Result<ava::agent::QuestionAnswer> {
    if (cancel_requested(run_state))
      return std::unexpected(canceled_error());
    if (input_closed(run_state))
      return std::unexpected(canceled_error());

    auto pending = std::make_shared<PendingQuestionRequest>();
    pending->correlation_id = prompt_request_id;
    pending->multiple = prompt.multiple;
    pending->allow_custom = prompt.allow_custom;
    pending->options = prompt.options;
    std::string request_id;
    {
      std::lock_guard lock(pending_state.mutex);
      request_id = next_resolver_request_id("question");
      pending_state.question_requests[request_id] = pending;
    }

    if (cancel_requested(run_state) || input_closed(run_state))
    {
      std::lock_guard lock(pending_state.mutex);
      erase_pending_request_if_matches(pending_state.question_requests, request_id, pending);
      return std::unexpected(canceled_error());
    }

    // Serialize before locking. The gate establishes whether publication or cancellation won.
    auto envelope = resolver_event_envelope("question_requested", prompt_request_id, prompt_request_id,
                                            session_id_snapshot(unlocked_session),
                                             question_request_payload_json(request_id, prompt));
    auto const record = ava::event::serialize_event_envelope_jsonl(envelope);
    auto written = Output::write_record_if(output, record, [&pending_state, &request_id, &pending] {
      std::lock_guard lock(pending_state.mutex);
      return request_is_pending(pending_state.question_requests, request_id, pending);
    });
    if (!written)
    {
      std::lock_guard lock(pending_state.mutex);
      erase_pending_request_if_matches(pending_state.question_requests, request_id, pending);
      return std::unexpected(std::move(written.error()));
    }
    if (*written == OutputWriteResult::Skipped)
      return std::unexpected(canceled_error());

    std::unique_lock lock(pending_state.mutex);
    pending_state.cv.wait(lock, [&pending] { return pending->resolved; });
    if (pending->error)
      return std::unexpected(*pending->error);
    if (!pending->answer)
      return std::unexpected(canceled_error());
    return *pending->answer;
  };
}

ava::core::VoidResult resolve_permission_reply(PendingResolverState& pending_state, std::string_view request_id, std::string_view correlation_id,
                                               std::string_view decision, std::optional<std::string> const& reason)
{
  ava::permissions::PermissionResolution resolution = ava::permissions::PermissionResolution::Deny;
  bool create_session_grant = false;
  if (decision == "allow")
  {
    resolution = ava::permissions::PermissionResolution::Allow;
  }
  else if (decision == "allow_session")
  {
    resolution = ava::permissions::PermissionResolution::Allow;
    create_session_grant = true;
  }
  else if (decision == "deny")
  {
    resolution = ava::permissions::PermissionResolution::Deny;
  }
  else
  {
    auto error = invalid_rpc("permission_reply decision must be allow, allow_session, or deny");
    error.with_context("decision", std::string(decision));
    return std::unexpected(std::move(error));
  }

  std::shared_ptr<PendingPermissionRequest> pending;
  {
    std::lock_guard lock(pending_state.mutex);
    auto found = pending_state.permission_requests.find(std::string(request_id));
    if (found == pending_state.permission_requests.end())
    {
      return std::unexpected(no_pending_request_error(request_id));
    }
    pending = found->second;
    if (pending->correlation_id != correlation_id)
    {
      auto error = invalid_rpc("permission_reply correlation_id does not match pending request");
      error.with_context("request_id", std::string(request_id));
      return std::unexpected(std::move(error));
    }
    if (create_session_grant && pending->command_allows_reusable_grant)
    {
      auto grant = grant_from_request(*pending);
      bool duplicate = false;
      for (auto const& existing : pending_state.permission_session_grants)
      {
        duplicate = duplicate || grant_matches(existing, *pending);
      }
      if (!duplicate)
      {
        std::size_t remaining = kMaxPermissionSessionGrantBytes;
        bool fits = pending_state.permission_session_grants.size() < kMaxPermissionSessionGrants && charge_grant_bytes(grant, remaining);
        for (auto const& existing : pending_state.permission_session_grants)
          fits = fits && charge_grant_bytes(existing, remaining);
        if (!fits)
          return std::unexpected(
              invalid_rpc("RPC session grant capacity exceeded; use permission_grant_revoke or permission_grants_clear, "
                          "then retry, or reply with one-shot allow/deny"));
      }
      if (!duplicate)
        pending_state.permission_session_grants.push_back(std::move(grant));
    }
    pending_state.permission_requests.erase(found);
    pending->resolved = true;
    pending->resolution = ava::permissions::PermissionResolutionDecision{resolution, reason.value_or("")};
  }
  pending_state.cv.notify_all();
  return {};
}

ava::core::VoidResult resolve_question_reply(PendingResolverState& pending_state, std::string_view request_id, std::string_view correlation_id,
                                             std::optional<std::string> const& answer, std::optional<std::string> const& selected,
                                             std::optional<std::vector<std::string>> const& selected_options)
{
  if (selected && selected_options)
  {
    return std::unexpected(invalid_rpc("question_reply requires selected or selected_options, not both"));
  }

  std::shared_ptr<PendingQuestionRequest> pending;
  ava::agent::QuestionAnswer parsed;
  {
    std::lock_guard lock(pending_state.mutex);
    auto found = pending_state.question_requests.find(std::string(request_id));
    if (found == pending_state.question_requests.end())
    {
      return std::unexpected(no_pending_request_error(request_id));
    }
    pending = found->second;
    if (pending->correlation_id != correlation_id)
    {
      auto error = invalid_rpc("question_reply correlation_id does not match pending request");
      error.with_context("request_id", std::string(request_id));
      return std::unexpected(std::move(error));
    }

    if (answer)
    {
      if (!pending->allow_custom)
      {
        return std::unexpected(invalid_rpc("question_reply answer is not allowed for this request"));
      }
      parsed.custom_text = *answer;
    }

    std::vector<std::string> submitted_options;
    if (selected)
    {
      submitted_options.push_back(*selected);
    }
    else if (selected_options)
    {
      submitted_options = *selected_options;
    }

    if (!submitted_options.empty() || selected_options)
    {
      if (!pending->multiple && submitted_options.size() != 1)
      {
        return std::unexpected(invalid_rpc("question_reply selected_options requires exactly one value for single-select requests"));
      }
      if (!pending->multiple && answer)
      {
        return std::unexpected(invalid_rpc("question_reply requires answer or selected option, not both for single-select requests"));
      }
      if (submitted_options.size() > pending->options.size())
      {
        return std::unexpected(invalid_rpc("question_reply selected_options has too many values"));
      }
      for (auto const& submitted : submitted_options)
      {
        bool valid_option = false;
        for (auto const& option : pending->options) valid_option = valid_option || option.value == submitted;
        if (!valid_option)
        {
          return std::unexpected(invalid_rpc("question_reply selected option is not valid for this request"));
        }
        for (auto const& existing : parsed.selected_options)
        {
          if (existing == submitted)
          {
            return std::unexpected(invalid_rpc("question_reply selected_options contains duplicate values"));
          }
        }
        parsed.selected_options.push_back(submitted);
      }
    }

    if (!answer && !selected && !selected_options)
    {
      return std::unexpected(invalid_rpc("question_reply requires answer, selected, or selected_options"));
    }
    if (!pending->multiple && selected_options && selected_options->empty())
    {
      return std::unexpected(invalid_rpc("question_reply selected_options requires exactly one value for single-select requests"));
    }
    if (pending->multiple && !answer && selected_options && selected_options->empty())
    {
      parsed.selected_options.clear();
    }

    pending_state.question_requests.erase(found);
    pending->resolved = true;
    pending->answer = std::move(parsed);
  }
  pending_state.cv.notify_all();
  return {};
}

}  // namespace ava::app::rpc
