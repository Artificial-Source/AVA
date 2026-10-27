#include "sys.h"
#include "ava/tui/event_state.h"
#include "ava/config/model_profiles.h"
#include "ava/core/json.h"

#include <algorithm>
#include <concepts>
#include <string_view>
#include <utility>
#include <variant>

namespace ava::tui {

namespace {

std::string first_non_empty(std::initializer_list<std::string_view> values)
{
  for (auto const value : values)
  {
    if (!value.empty())
      return std::string(value);
  }
  return {};
}

void assign_if_present(std::string& target, std::string value)
{
  if (!value.empty())
    target = std::move(value);
}

void add_unique_string(std::vector<std::string>& values, std::string value)
{
  if (value.empty())
    return;
  if (std::ranges::find(values, value) == values.end())
    values.push_back(std::move(value));
}

auto find_unique_pending_tool(TuiEventState& state, auto const& matches)
{
  auto candidate = state.pending_tools.end();
  for (auto pending = state.pending_tools.begin(); pending != state.pending_tools.end(); ++pending)
  {
    if (!matches(*pending))
      continue;
    if (candidate != state.pending_tools.end())
      return state.pending_tools.end();
    candidate = pending;
  }
  return candidate;
}

auto find_pending_by_backend_call_id(TuiEventState& state, std::string const& call_id)
{
  return std::ranges::find_if(state.pending_tools, [&](PendingToolItem const& tool) { return tool.backend_call_id == call_id; });
}

auto find_pending_by_effective_call_id(TuiEventState& state, std::string const& call_id)
{
  return std::ranges::find_if(state.pending_tools, [&](PendingToolItem const& tool) { return tool.call_id == call_id || tool.item.call_id == call_id; });
}

auto find_pending_tool(TuiEventState& state, std::string const& call_id, std::string const& request_id = {}, std::string const& correlation_id = {},
                       std::string const& stored_id = {})
{
  // Strong payload call IDs never fall back to shared request/correlation identity.
  if (!call_id.empty())
    return find_pending_by_backend_call_id(state, call_id);

  if (!correlation_id.empty())
  {
    auto by_correlation = find_unique_pending_tool(state, [&](PendingToolItem const& tool) { return tool.correlation_id == correlation_id; });
    if (by_correlation != state.pending_tools.end())
      return by_correlation;
  }

  if (!request_id.empty())
  {
    auto by_request = find_unique_pending_tool(state, [&](PendingToolItem const& tool) { return tool.request_id == request_id; });
    if (by_request != state.pending_tools.end())
      return by_request;
  }

  // Empty payload IDs with no request/correlation still coalesce on the effective stored id
  // (for example provider synthetic keys). Never treat that derived id as a strong payload call id.
  if (request_id.empty() && correlation_id.empty() && !stored_id.empty())
    return find_pending_by_effective_call_id(state, stored_id);

  return state.pending_tools.end();
}

auto find_permission_audit(TuiEventState& state, std::string_view permission_request_id, std::string_view resolver_request_id = {})
{
  return std::ranges::find_if(state.permission_audits, [&](ToolPermissionAuditItem const& audit) {
    return (!permission_request_id.empty() && audit.permission_request_id == permission_request_id) ||
           (!resolver_request_id.empty() && audit.resolver_request_id == resolver_request_id);
  });
}

ToolPermissionAuditItem permission_audit_from_request_envelope(ava::event::EventEnvelope const& envelope)
{
  auto const& payload = envelope.payload_json;
  return ToolPermissionAuditItem{.permission_request_id = ava::core::json::string_field(payload, "permission_request_id").value_or(""),
                                 .resolver_request_id = ava::core::json::string_field(payload, "resolver_request_id").value_or(""),
                                 .decision = {},
                                 .operation = ava::core::json::string_field(payload, "operation").value_or(""),
                                 .tool_name = ava::core::json::string_field(payload, "tool_name").value_or(""),
                                 .risk = ava::core::json::string_field(payload, "risk").value_or(""),
                                 .reason = ava::core::json::string_field(payload, "reason").value_or(""),
                                 .target = ava::core::json::string_field(payload, "target_path").value_or(""),
                                 .command = ava::core::json::string_field(payload, "command").value_or(""),
                                 .resolution_reason = {}};
}

void merge_permission_audit(ToolPermissionAuditItem& target, ToolPermissionAuditItem incoming)
{
  assign_if_present(target.permission_request_id, std::move(incoming.permission_request_id));
  assign_if_present(target.resolver_request_id, std::move(incoming.resolver_request_id));
  assign_if_present(target.decision, std::move(incoming.decision));
  assign_if_present(target.operation, std::move(incoming.operation));
  assign_if_present(target.tool_name, std::move(incoming.tool_name));
  assign_if_present(target.risk, std::move(incoming.risk));
  assign_if_present(target.reason, std::move(incoming.reason));
  assign_if_present(target.target, std::move(incoming.target));
  assign_if_present(target.command, std::move(incoming.command));
  assign_if_present(target.resolution_reason, std::move(incoming.resolution_reason));
}

void add_permission_audit(ToolTimelineItem& item, ToolPermissionAuditItem audit);

bool pending_tool_has_permission_id(PendingToolItem const& pending, ToolPermissionAuditItem const& audit)
{
  if (!audit.permission_request_id.empty() &&
      std::ranges::find(pending.item.permission_request_ids, audit.permission_request_id) != pending.item.permission_request_ids.end())
  {
    return true;
  }
  return std::ranges::any_of(pending.item.permissions, [&](ToolPermissionAuditItem const& current) {
    return (!audit.permission_request_id.empty() && current.permission_request_id == audit.permission_request_id) ||
           (!audit.resolver_request_id.empty() && current.resolver_request_id == audit.resolver_request_id);
  });
}

void attach_permission_audit_to_pending_tool(TuiEventState& state, ToolPermissionAuditItem const& audit, bool allow_unique_name_fallback)
{
  auto exact = std::ranges::find_if(state.pending_tools, [&](PendingToolItem const& pending) { return pending_tool_has_permission_id(pending, audit); });
  if (exact != state.pending_tools.end())
  {
    add_permission_audit(exact->item, audit);
    return;
  }
  if (!allow_unique_name_fallback || audit.tool_name.empty())
    return;

  auto candidate = state.pending_tools.end();
  for (auto pending = state.pending_tools.begin(); pending != state.pending_tools.end(); ++pending)
  {
    if (pending->item.status != ToolTimelineStatus::Running || pending->item.name != audit.tool_name)
      continue;
    if (candidate != state.pending_tools.end())
      return;
    candidate = pending;
  }
  if (candidate != state.pending_tools.end())
    add_permission_audit(candidate->item, audit);
}

void remember_permission_audit(TuiEventState& state, ToolPermissionAuditItem audit, bool allow_unique_name_fallback)
{
  auto existing = find_permission_audit(state, audit.permission_request_id, audit.resolver_request_id);
  if (existing == state.permission_audits.end())
  {
    state.permission_audits.push_back(std::move(audit));
    attach_permission_audit_to_pending_tool(state, state.permission_audits.back(), allow_unique_name_fallback);
    return;
  }
  merge_permission_audit(*existing, std::move(audit));
  attach_permission_audit_to_pending_tool(state, *existing, false);
}

void remember_permission_request_envelope(TuiEventState& state, ava::event::EventEnvelope const& envelope)
{
  auto audit = permission_audit_from_request_envelope(envelope);
  if (audit.permission_request_id.empty() && audit.resolver_request_id.empty())
    return;
  remember_permission_audit(state, std::move(audit), true);
}

void remember_permission_reply_envelope(TuiEventState& state, ava::event::EventEnvelope const& envelope)
{
  auto const& payload = envelope.payload_json;
  auto audit = ToolPermissionAuditItem{.permission_request_id = ava::core::json::string_field(payload, "permission_request_id").value_or(""),
                                       .resolver_request_id = ava::core::json::string_field(payload, "resolver_request_id").value_or(""),
                                       .decision = ava::core::json::string_field(payload, "decision").value_or(""),
                                       .resolution_reason = ava::core::json::string_field(payload, "reason").value_or("")};
  if (audit.permission_request_id.empty() && audit.resolver_request_id.empty())
    return;
  remember_permission_audit(state, std::move(audit), false);
}

std::string permission_provider_decision(ava::event::ProviderPayload const& payload)
{
  if (payload.status == "tui:permission_deny")
    return "deny";
  if (payload.status != "tui:permission_allow")
    return {};
  if (payload.error_details == "selected allow session" || payload.error_details == "reused tui session grant")
    return "allow_session";
  if (payload.error_details == "selected allow and remember")
    return "allow_remember";
  return "allow";
}

void remember_permission_provider_event(TuiEventState& state, ava::event::ProviderPayload const& payload)
{
  if (payload.status != "tui:permission_request" && payload.status != "tui:permission_allow" && payload.status != "tui:permission_deny")
    return;
  auto audit = ToolPermissionAuditItem{.decision = permission_provider_decision(payload),
                                       .reason = payload.reason,
                                       .command = payload.status == "tui:permission_request" ? payload.text : std::string{},
                                       .resolution_reason = payload.error_details};
  if (!payload.tool.empty())
    audit.tool_name = payload.tool;
  if (!payload.permission_request_ids.empty())
    audit.permission_request_id = payload.permission_request_ids.front();
  if (audit.permission_request_id.empty() && audit.resolver_request_id.empty())
    return;
  remember_permission_audit(state, std::move(audit), payload.status == "tui:permission_request");
}

void add_permission_request_id(ToolTimelineItem& item, std::string id)
{
  add_unique_string(item.permission_request_ids, std::move(id));
}

void add_permission_audit(ToolTimelineItem& item, ToolPermissionAuditItem audit)
{
  if (!audit.permission_request_id.empty())
    add_permission_request_id(item, audit.permission_request_id);
  auto existing = std::ranges::find_if(item.permissions, [&](ToolPermissionAuditItem const& current) {
    return (!audit.permission_request_id.empty() && current.permission_request_id == audit.permission_request_id) ||
           (!audit.resolver_request_id.empty() && current.resolver_request_id == audit.resolver_request_id);
  });
  if (existing == item.permissions.end())
  {
    item.permissions.push_back(std::move(audit));
    return;
  }
  merge_permission_audit(*existing, std::move(audit));
}

void attach_permission_audits(TuiEventState& state, ToolTimelineItem& item)
{
  auto const permission_ids = item.permission_request_ids;
  for (auto const& permission_id : permission_ids)
  {
    auto existing = find_permission_audit(state, permission_id);
    if (existing == state.permission_audits.end())
    {
      add_permission_audit(item, ToolPermissionAuditItem{.permission_request_id = permission_id});
      continue;
    }
    add_permission_audit(item, *existing);
  }
  for (auto const& audit : item.permissions)
  {
    if (!audit.permission_request_id.empty())
      add_unique_string(item.permission_request_ids, audit.permission_request_id);
  }
}

bool bool_field(std::string_view object, std::string_view key)
{
  auto const start = ava::core::json::field_value_start(object, key);
  if (!start)
    return false;
  auto value = object.substr(*start);
  while (!value.empty())
  {
    auto const byte = static_cast<unsigned char>(value.front());
    if (byte != ' ' && byte != '\n' && byte != '\r' && byte != '\t')
      break;
    value.remove_prefix(1);
  }
  return value.starts_with("true");
}

std::size_t size_field(std::string_view object, std::string_view key)
{
  auto const value = ava::core::json::integer_field(object, key);
  return value && *value > 0 ? static_cast<std::size_t>(*value) : std::size_t{0};
}

void upsert_sidebar_activity(TuiEventState& state, SidebarActivityItem item);
std::string title_case_ascii(std::string_view text);

TranscriptItem transcript_text_item(std::string label, std::string text)
{
  auto text_model = text_from_plain(text);
  return TranscriptItem{.label = std::move(label), .text = std::move(text), .text_model = std::move(text_model)};
}

TranscriptItem assistant_transcript_item(std::string text, std::string meta, std::string thinking = {})
{
  auto text_model = text_from_markdown(text);
  auto thinking_model = text_from_plain(thinking);
  return TranscriptItem{.label = "ava",
                        .text = std::move(text),
                        .text_model = std::move(text_model),
                        .meta = std::move(meta),
                        .thinking = std::move(thinking),
                        .thinking_model = std::move(thinking_model)};
}

std::string prompt_request_text(ava::event::EventEnvelope const& envelope)
{
  auto const& payload = envelope.payload_json;
  if (envelope.name == "permission_requested")
  {
    auto const tool = ava::core::json::string_field(payload, "tool_name").value_or("");
    auto const operation = ava::core::json::string_field(payload, "operation").value_or("");
    auto const target = ava::core::json::string_field(payload, "target_path").value_or("");
    auto const command = ava::core::json::string_field(payload, "command").value_or("");
    auto text = std::string("permission requested");
    if (!tool.empty())
      text += ": " + tool;
    auto const detail = first_non_empty({target, command, operation});
    if (!detail.empty())
      text += " " + detail;
    return text;
  }
  if (envelope.name == "question_requested")
  {
    auto const question = ava::core::json::string_field(payload, "question").value_or("");
    return question.empty() ? std::string("question requested") : "question requested: " + question;
  }
  return {};
}

void remember_context_ids(TuiEventState& state, ava::event::EventEnvelopeContext const& context)
{
  if (context.run_id)
    state.active_run_id = context.run_id;
  if (context.turn_id)
    state.active_turn_id = context.turn_id;
  if (context.message_id)
    state.active_message_id = context.message_id;
  if (context.request_id)
    state.active_request_id = context.request_id;
  if (context.correlation_id)
    state.active_correlation_id = context.correlation_id;
}

void remember_envelope_ids(TuiEventState& state, ava::event::EventEnvelope const& envelope)
{
  remember_context_ids(state, ava::event::EventEnvelopeContext{.event_id = std::nullopt,
                                                               .run_id = envelope.run_id,
                                                               .turn_id = envelope.turn_id,
                                                               .message_id = envelope.message_id,
                                                               .request_id = envelope.request_id,
                                                               .correlation_id = envelope.correlation_id});
}

void apply_prompt_request_envelope(TuiEventState& state, ava::event::EventEnvelope const& envelope)
{
  auto const text = prompt_request_text(envelope);
  if (text.empty())
    return;
  auto const permission = envelope.name == "permission_requested";
  upsert_sidebar_activity(state, SidebarActivityItem{.id = first_non_empty({envelope.request_id.value_or(""), envelope.correlation_id.value_or(""),
                                                                            permission ? "permission" : "question"}),
                                                     .label = permission ? "permission" : "question",
                                                     .detail = text,
                                                     .status = ToolTimelineStatus::Running});
}

std::string queue_event_label(std::string_view name)
{
  if (name == "steer_queued")
    return "steer queued";
  if (name == "steer_applied")
    return "steer applied";
  if (name == "steer_skipped")
    return "steer skipped";
  if (name == "follow_up_queued")
    return "follow-up queued";
  if (name == "follow_up_started")
    return "follow-up started";
  if (name == "follow_up_skipped")
    return "follow-up skipped";
  return title_case_ascii(name);
}

ToolTimelineStatus queue_event_status(std::string_view name)
{
  if (name.ends_with("_skipped"))
    return ToolTimelineStatus::Error;
  if (name.ends_with("_applied"))
    return ToolTimelineStatus::Success;
  return ToolTimelineStatus::Running;
}

std::string queue_event_reason_text(std::string_view name, std::string_view reason)
{
  if (reason == "canceled")
    return "run stopped before delivery; submit it again to continue";
  if (reason == "restored_to_composer")
    return "restored to composer";
  if (reason == "run_completed_before_safe_point")
  {
    if (name.starts_with("steer"))
      return "run finished before the next safe steering point";
    return "run finished before delivery";
  }
  return std::string(reason);
}

std::string queue_event_detail(ava::event::EventEnvelope const& envelope)
{
  auto const message = ava::core::json::string_field(envelope.payload_json, "message").value_or("");
  auto const reason = ava::core::json::string_field(envelope.payload_json, "reason").value_or("");
  auto detail = queue_event_label(envelope.name);
  if (!reason.empty())
    detail += ": " + queue_event_reason_text(envelope.name, reason);
  if (!message.empty())
    detail += " - " + message;
  if (bool_field(envelope.payload_json, "message_truncated"))
  {
    detail += " [message truncated";
    auto const bytes = size_field(envelope.payload_json, "message_bytes");
    if (bytes > 0)
      detail += " from " + std::to_string(bytes) + " bytes";
    detail += "]";
  }
  return detail;
}

std::string queue_event_id(ava::event::EventEnvelope const& envelope)
{
  return first_non_empty({envelope.request_id.value_or(""), envelope.correlation_id.value_or(""), envelope.name});
}

void upsert_queued_message(TuiEventState& state, ava::event::EventEnvelope const& envelope)
{
  auto const id = queue_event_id(envelope);
  auto const message = ava::core::json::string_field(envelope.payload_json, "message").value_or("");
  if (id.empty() || message.empty())
    return;
  auto const kind = envelope.name.starts_with("steer") ? std::string("steer") : std::string("follow-up");
  auto existing = std::ranges::find_if(state.queued_messages, [&](QueuedMessageItem const& item) { return item.id == id; });
  auto item = QueuedMessageItem{.id = id, .kind = kind, .text = message};
  if (existing == state.queued_messages.end())
  {
    state.queued_messages.push_back(std::move(item));
  }
  else
  {
    *existing = std::move(item);
  }
}

void remove_queued_message(TuiEventState& state, ava::event::EventEnvelope const& envelope)
{
  auto const id = queue_event_id(envelope);
  if (id.empty())
    return;
  std::erase_if(state.queued_messages, [&](QueuedMessageItem const& item) { return item.id == id; });
}

std::string_view trim_trailing_ascii_space(std::string_view text)
{
  while (!text.empty())
  {
    auto const byte = static_cast<unsigned char>(text.back());
    if (byte != ' ' && byte != '\n' && byte != '\r' && byte != '\t' && byte != '\f' && byte != '\v')
      break;
    text.remove_suffix(1);
  }
  return text;
}

ToolTimelineStatus tool_status_from_payload(ava::event::ToolPayload const& payload)
{
  if (payload.status == "error")
    return ToolTimelineStatus::Error;
  if (payload.status == "canceled" || payload.status == "cancelled" || payload.status == "aborted")
    return ToolTimelineStatus::Canceled;
  if (payload.status == "running")
    return ToolTimelineStatus::Running;
  return ToolTimelineStatus::Success;
}

ToolLifecycleState tool_lifecycle_for_status(ToolTimelineStatus status)
{
  switch (status)
  {
    case ToolTimelineStatus::Running:
      return ToolLifecycleState::ExecutionStarted;
    case ToolTimelineStatus::Success:
      return ToolLifecycleState::Complete;
    case ToolTimelineStatus::Canceled:
      return ToolLifecycleState::Canceled;
    case ToolTimelineStatus::Error:
      return ToolLifecycleState::Error;
  }
  return ToolLifecycleState::Error;
}

std::string title_case_ascii(std::string_view text)
{
  std::string output;
  output.reserve(text.size());
  bool at_word_start = true;
  for (char ch : text)
  {
    auto const byte = static_cast<unsigned char>(ch);
    if (byte == '_' || byte == '-')
    {
      output.push_back(' ');
      at_word_start = true;
      continue;
    }
    if (at_word_start && byte >= 'a' && byte <= 'z')
      output.push_back(static_cast<char>(byte - ('a' - 'A')));
    else
      output.push_back(ch);
    at_word_start = byte == ' ';
  }
  return output;
}

std::string assistant_meta_for_state(TuiEventState const& state)
{
  if (state.current_model_id.empty())
    return {};
  auto mode = title_case_ascii(ava::core::to_string(state.current_mode));
  if (mode.empty())
    mode = "AVA";
  return mode + " · " + ava::config::model_display_label(state.current_provider_id, state.current_model_id);
}

void update_pending_assistant_meta(TuiEventState& state)
{
  auto meta = assistant_meta_for_state(state);
  if (!meta.empty())
    state.pending_assistant_meta = std::move(meta);
}

void upsert_activity(TuiEventState& state, std::string const& call_id, ToolTimelineItem const& item)
{
  auto const id = call_id.empty() ? item.name + ":" + item.argument_summary : call_id;
  auto const label = item.name.empty() ? std::string("tool") : item.name;
  auto const detail = item.result_summary.empty() ? item.argument_summary : item.result_summary;
  auto existing = std::ranges::find_if(state.activity, [&](SidebarActivityItem const& activity) { return activity.id == id; });
  if (existing != state.activity.end())
  {
    existing->label = label;
    existing->detail = detail;
    existing->status = item.status;
    return;
  }
  state.activity.push_back(SidebarActivityItem{.id = id, .label = label, .detail = detail, .status = item.status});
  constexpr auto kMaxActivityItems = std::size_t{8};
  if (state.activity.size() > kMaxActivityItems)
    state.activity.erase(state.activity.begin(), state.activity.begin() + static_cast<std::ptrdiff_t>(state.activity.size() - kMaxActivityItems));
}

void upsert_sidebar_activity(TuiEventState& state, SidebarActivityItem item)
{
  auto existing = std::ranges::find_if(state.activity, [&](SidebarActivityItem const& activity) { return activity.id == item.id; });
  if (existing != state.activity.end())
  {
    *existing = std::move(item);
    return;
  }
  state.activity.push_back(std::move(item));
  constexpr auto kMaxActivityItems = std::size_t{8};
  if (state.activity.size() > kMaxActivityItems)
    state.activity.erase(state.activity.begin(), state.activity.begin() + static_cast<std::ptrdiff_t>(state.activity.size() - kMaxActivityItems));
}

void settle_responding_activity(TuiEventState& state, ToolTimelineStatus status, std::string detail)
{
  auto existing = std::ranges::find_if(state.activity, [](SidebarActivityItem const& activity) { return activity.id == "responding"; });
  if (existing == state.activity.end())
    return;
  existing->status = status;
  existing->detail = std::move(detail);
}

void settle_running_retry_activities(TuiEventState& state, ToolTimelineStatus status, std::string const& detail)
{
  for (auto& activity : state.activity)
  {
    if (activity.status != ToolTimelineStatus::Running || activity.label != "retry")
      continue;
    activity.status = status;
    activity.detail = detail;
  }
}

bool is_cancel_error(ava::event::ErrorPayload const& payload)
{
  return payload.error_message == "agent loop canceled" || payload.text == "agent loop canceled" ||
         payload.error_details.find("agent loop canceled") != std::string::npos;
}

std::string compact_lifecycle_detail(std::string_view label, ava::event::CompactionPayload const& payload)
{
  std::string detail(label);
  if (!payload.trigger.empty())
    detail += " (" + payload.trigger + ")";
  if (payload.attempt > 0)
  {
    detail += " attempt " + std::to_string(payload.attempt);
    if (payload.max_attempts > 0)
      detail += "/" + std::to_string(payload.max_attempts);
  }
  if (payload.estimated_tokens > 0)
    detail += " tokens~" + std::to_string(payload.estimated_tokens);
  if (payload.threshold_tokens > 0)
    detail += "/" + std::to_string(payload.threshold_tokens);
  if (payload.summary_bytes > 0)
    detail += " summary=" + std::to_string(payload.summary_bytes) + " bytes";
  return detail;
}

std::string retry_activity_id(ava::event::RetryPayload const& payload)
{
  return "retry:" + first_non_empty({payload.reason, payload.trigger, "retry"});
}

void upsert_retry_countdown_transcript(TuiEventState& state, std::string detail)
{
  auto existing = std::ranges::find_if(state.transcript.rbegin(), state.transcript.rend(),
                                       [](TranscriptItem const& item) { return item.label == "audit" && item.text.starts_with("retry countdown"); });
  if (existing != state.transcript.rend())
  {
    existing->text = std::move(detail);
    existing->text_model = text_from_plain(existing->text);
    return;
  }
  state.transcript.push_back(transcript_text_item("audit", std::move(detail)));
}

void apply_canceled_event(TuiEventState& state, std::string const& payload_text, std::string const& payload_reason)
{
  state.error_text = "stopped by user";
  state.error_details.clear();
  auto text = payload_text.empty() ? std::string("stopped by user") : payload_text;
  if (text == "stopped by user")
    text += ". Submit a new prompt to continue.";
  state.transcript.push_back(assistant_transcript_item(std::move(text), {}));
  settle_responding_activity(state, ToolTimelineStatus::Canceled, "assistant stopped");
  auto detail = payload_reason.empty() ? std::string("active work was stopped") : payload_reason;
  detail += "; submit a new prompt to continue";
  upsert_sidebar_activity(state, SidebarActivityItem{.id = "stopped", .label = "stopped", .detail = std::move(detail), .status = ToolTimelineStatus::Canceled});
  state.run_status = TuiEventRunStatus::Canceled;
}

std::optional<std::string> path_from_argument_summary(std::string_view summary)
{
  constexpr std::string_view marker = "path=";
  auto const start = summary.find(marker);
  if (start == std::string_view::npos)
    return std::nullopt;
  auto value = summary.substr(start + marker.size());
  auto const comma = value.find(',');
  if (comma != std::string_view::npos)
    value = value.substr(0, comma);
  while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
  while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
  if (value.empty())
    return std::nullopt;
  return std::string(value);
}

void record_modified_file(TuiEventState& state, ToolTimelineItem const& item)
{
  if (item.status != ToolTimelineStatus::Success)
    return;
  if (item.name != "write_file" && item.name != "edit_file" && item.name != "apply_patch")
    return;
  auto paths = item.changed_paths;
  if (paths.empty())
  {
    auto path = path_from_argument_summary(item.argument_summary);
    if (path)
      paths.push_back(std::move(*path));
  }
  for (auto& path : paths)
  {
    if (path.empty())
      continue;
    auto existing = std::ranges::find_if(state.modified_files, [&](SidebarModifiedFile const& file) { return file.path == path; });
    if (existing == state.modified_files.end())
      state.modified_files.push_back(SidebarModifiedFile{.path = std::move(path)});
  }
  constexpr auto kMaxModifiedFiles = std::size_t{12};
  if (state.modified_files.size() > kMaxModifiedFiles)
    state.modified_files.erase(state.modified_files.begin(),
                               state.modified_files.begin() + static_cast<std::ptrdiff_t>(state.modified_files.size() - kMaxModifiedFiles));
}

std::optional<agent::TodoStatus> parse_todo_status_view(std::string_view value)
{
  using TodoStatus = agent::TodoStatus;
  if (value == "pending")
    return TodoStatus::Pending;
  if (value == "in_progress")
    return TodoStatus::InProgress;
  if (value == "completed")
    return TodoStatus::Completed;
  return std::nullopt;
}

// Fail-closed presentation parse of a normalized successful todowrite result.
// Malformed historical/live payloads leave existing state unchanged.
std::optional<std::vector<agent::TodoItem>> parse_todowrite_result_todos(std::string_view result_json)
{
  if (!bool_field(result_json, "ok"))
    return std::nullopt;
  auto const tool = ava::core::json::string_field(result_json, "tool");
  if (!tool || *tool != "todowrite")
    return std::nullopt;
  auto const schema = ava::core::json::integer_field(result_json, "schema_version");
  if (!schema || *schema != 1)
    return std::nullopt;
  auto const objects = ava::core::json::strict_objects_in_array_field(result_json, "todos", 50);
  if (!objects)
    return std::nullopt;

  std::vector<agent::TodoItem> todos;
  todos.reserve(objects->size());
  std::size_t in_progress = 0;
  for (auto const& object : *objects)
  {
    auto const id = ava::core::json::string_field(object, "id");
    auto const content = ava::core::json::string_field(object, "content");
    auto const status_text = ava::core::json::string_field(object, "status");
    if (!id || id->empty() || id->size() > 32 || !content || content->empty() || content->size() > 512 || !status_text)
      return std::nullopt;
    auto const status = parse_todo_status_view(*status_text);
    if (!status)
      return std::nullopt;
    if (*status == agent::TodoStatus::InProgress)
      ++in_progress;
    if (in_progress > 1)
      return std::nullopt;
    for (unsigned char const ch : *id)
    {
      auto const is_alnum = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
      if (!(is_alnum || ch == '_' || ch == '-'))
        return std::nullopt;
    }
    for (unsigned char const ch : *content)
    {
      if (ch < 0x20 || ch == 0x7F)
        return std::nullopt;
    }
    if (std::ranges::any_of(todos, [&](agent::TodoItem const& existing) { return existing.id == *id; }))
      return std::nullopt;
    todos.push_back(agent::TodoItem{.id = *id, .content = *content, .status = *status});
  }
  return todos;
}

void apply_todowrite_result(TuiEventState& state, ToolTimelineItem const& item)
{
  if (item.status != ToolTimelineStatus::Success || item.name != "todowrite")
    return;
  auto parsed = parse_todowrite_result_todos(item.result_json);
  if (!parsed)
    return;
  state.todos = std::move(*parsed);
}

std::string take_pending_reasoning_text(TuiEventState& state)
{
  auto thinking = std::move(state.pending_reasoning_text);
  state.pending_reasoning_text.clear();
  state.pending_reasoning_redacted = false;
  return thinking;
}

void append_assistant_text(TuiEventState& state, std::string text, std::string meta, std::string thinking = {})
{
  if (text.empty() && thinking.empty())
    return;
  state.transcript.push_back(assistant_transcript_item(std::move(text), std::move(meta), std::move(thinking)));
  state.stream_assistant_transcript_index = std::nullopt;
}

void commit_pending_assistant_text(TuiEventState& state, std::string meta = {})
{
  auto thinking = take_pending_reasoning_text(state);
  if (state.pending_assistant_text.empty() && thinking.empty())
    return;
  if (meta.empty())
    meta = std::move(state.pending_assistant_meta);
  state.transcript.push_back(assistant_transcript_item(std::move(state.pending_assistant_text), std::move(meta), std::move(thinking)));
  state.pending_assistant_text.clear();
  state.pending_assistant_meta.clear();
  state.stream_assistant_transcript_index = state.transcript.size() - 1;
}

void commit_pending_reasoning_turn(TuiEventState& state, std::string meta = {})
{
  if (state.pending_reasoning_text.empty())
    return;
  if (meta.empty())
    meta = std::move(state.pending_assistant_meta);
  append_assistant_text(state, {}, std::move(meta), take_pending_reasoning_text(state));
  state.pending_assistant_meta.clear();
}

void apply_assistant_final(TuiEventState& state, ava::event::MessagePayload const& payload)
{
  auto final_text = payload.text.empty() ? std::move(state.pending_assistant_text) : payload.text;
  auto final_meta = assistant_meta_for_state(state);
  if (final_meta.empty())
    final_meta = std::move(state.pending_assistant_meta);
  state.pending_assistant_text.clear();
  state.pending_assistant_meta.clear();
  if (final_text.empty() && state.pending_reasoning_text.empty())
    return;

  if (state.stream_assistant_transcript_index && *state.stream_assistant_transcript_index < state.transcript.size())
  {
    auto& item = state.transcript[*state.stream_assistant_transcript_index];
    if (!item.tool && item.label == "ava")
    {
      item.text = std::move(final_text);
      item.text_model = text_from_markdown(item.text);
      if (!final_meta.empty())
        item.meta = std::move(final_meta);
      if (!state.pending_reasoning_text.empty() && item.thinking.empty())
      {
        item.thinking = take_pending_reasoning_text(state);
        item.thinking_model = text_from_plain(item.thinking);
      }
      state.stream_assistant_transcript_index = std::nullopt;
      return;
    }
  }

  if (!state.transcript.empty())
  {
    auto& last = state.transcript.back();
    if (!last.tool && last.label == "ava" && trim_trailing_ascii_space(last.text) == trim_trailing_ascii_space(final_text))
    {
      if (!final_meta.empty())
        last.meta = std::move(final_meta);
      state.stream_assistant_transcript_index = std::nullopt;
      return;
    }
  }
  append_assistant_text(state, std::move(final_text), std::move(final_meta), take_pending_reasoning_text(state));
}

void apply_tool_payload(ToolTimelineItem& item, ava::event::ToolPayload const& payload)
{
  if (!payload.diff.empty())
    item.diff = payload.diff;
  item.diff_truncated = item.diff_truncated || payload.diff_truncated;
  for (auto const& path : payload.changed_paths) add_unique_string(item.changed_paths, path);
  item.truncated = item.truncated || payload.truncated;
  item.byte_limited = item.byte_limited || payload.byte_limited;
  item.line_limited = item.line_limited || payload.line_limited;
  if (!payload.spill_path.empty())
    item.spill_path = payload.spill_path;
  item.spill_truncated = item.spill_truncated || payload.spill_truncated;
  if (payload.output_bytes > 0)
    item.output_bytes = payload.output_bytes;
  if (payload.total_bytes > 0)
    item.total_bytes = payload.total_bytes;
  if (payload.output_lines > 0)
    item.output_lines = payload.output_lines;
  if (payload.total_lines > 0)
    item.total_lines = payload.total_lines;
  if (payload.start_line > 0)
    item.start_line = payload.start_line;
  if (payload.end_line > 0)
    item.end_line = payload.end_line;
  if (payload.next_offset_line > 0)
    item.next_offset_line = payload.next_offset_line;
  if (payload.omitted_bytes > 0)
    item.omitted_bytes = payload.omitted_bytes;
  if (payload.omitted_lines > 0)
    item.omitted_lines = payload.omitted_lines;
  if (payload.visible_matches > 0)
    item.visible_matches = payload.visible_matches;
  if (payload.total_matches > 0)
    item.total_matches = payload.total_matches;
  for (auto const& permission_id : payload.permission_request_ids) add_permission_request_id(item, permission_id);
}

void apply_tool_context(PendingToolItem& pending, ava::event::EventEnvelopeContext const& context)
{
  if (context.request_id)
  {
    pending.request_id = *context.request_id;
    pending.item.request_id = *context.request_id;
  }
  if (context.correlation_id)
  {
    pending.correlation_id = *context.correlation_id;
    pending.item.correlation_id = *context.correlation_id;
  }
}

std::string effective_tool_call_id(ava::event::ToolPayload const& payload, ava::event::EventEnvelopeContext const& context)
{
  return first_non_empty({payload.call_id, context.correlation_id.value_or(""), context.request_id.value_or("")});
}

std::string effective_provider_tool_call_id(ava::event::ProviderPayload const& payload, ava::event::EventEnvelopeContext const& context)
{
  auto id = first_non_empty({payload.call_id, context.correlation_id.value_or(""), context.request_id.value_or("")});
  if (!id.empty())
    return id;
  auto const tool_key = payload.tool.empty() ? std::string("tool_call") : payload.tool;
  return "provider:tool_call:" + tool_key;
}

void assign_pending_call_id(PendingToolItem& pending, std::string const& call_id, std::string const& backend_call_id)
{
  // Keep backend-provided identity separate from correlation/request display fallbacks.
  assign_if_present(pending.backend_call_id, backend_call_id);
  assign_if_present(pending.call_id, call_id);
  assign_if_present(pending.item.call_id, call_id);
}

ToolTimelineItem tool_item_from_payload(ava::event::ToolPayload const& payload, ToolTimelineStatus status, std::string call_id)
{
  auto item = ToolTimelineItem{.status = status,
                               .name = payload.tool,
                               .argument_summary = payload.text,
                               .result_summary = {},
                               .arguments_json = payload.args_json,
                               .result_json = payload.result_json,
                               .call_id = std::move(call_id),
                               .lifecycle = tool_lifecycle_for_status(status)};
  apply_tool_payload(item, payload);
  return item;
}

void apply_tool_start(TuiEventState& state, ava::event::ToolPayload const& payload, ava::event::EventEnvelopeContext const& context)
{
  auto const call_id = effective_tool_call_id(payload, context);
  auto item = tool_item_from_payload(payload, ToolTimelineStatus::Running, call_id);
  auto existing = find_pending_tool(state, payload.call_id, context.request_id.value_or(""), context.correlation_id.value_or(""), call_id);
  if (existing != state.pending_tools.end())
  {
    if (!item.name.empty())
      existing->item.name = std::move(item.name);
    if (!item.argument_summary.empty())
      existing->item.argument_summary = std::move(item.argument_summary);
    if (!item.arguments_json.empty())
      existing->item.arguments_json = std::move(item.arguments_json);
    apply_tool_payload(existing->item, payload);
    apply_tool_context(*existing, context);
    attach_permission_audits(state, existing->item);
    assign_pending_call_id(*existing, call_id, payload.call_id);
    existing->item.lifecycle = ToolLifecycleState::ExecutionStarted;
    existing->item.status = ToolTimelineStatus::Running;
    upsert_activity(state, existing->call_id, existing->item);
    return;
  }
  attach_permission_audits(state, item);
  state.pending_tools.emplace_back(
      PendingToolItem{.call_id = call_id, .backend_call_id = payload.call_id, .request_id = {}, .correlation_id = {}, .item = std::move(item)});
  apply_tool_context(state.pending_tools.back(), context);
  upsert_activity(state, call_id, state.pending_tools.back().item);
}

void apply_tool_progress(TuiEventState& state, ava::event::ToolPayload const& payload, ava::event::EventEnvelopeContext const& context)
{
  auto const call_id = effective_tool_call_id(payload, context);
  auto existing = find_pending_tool(state, payload.call_id, context.request_id.value_or(""), context.correlation_id.value_or(""), call_id);
  if (existing == state.pending_tools.end())
  {
    auto item = tool_item_from_payload(payload, ToolTimelineStatus::Running, call_id);
    item.result_summary = payload.text;
    item.argument_summary.clear();
    item.lifecycle = ToolLifecycleState::Progress;
    state.pending_tools.emplace_back(
        PendingToolItem{.call_id = call_id, .backend_call_id = payload.call_id, .request_id = {}, .correlation_id = {}, .item = std::move(item)});
    apply_tool_context(state.pending_tools.back(), context);
    attach_permission_audits(state, state.pending_tools.back().item);
    return;
  }
  if (!payload.tool.empty())
    existing->item.name = payload.tool;
  if (!payload.args_json.empty())
    existing->item.arguments_json = payload.args_json;
  if (!payload.result_json.empty())
    existing->item.result_json = payload.result_json;
  apply_tool_payload(existing->item, payload);
  apply_tool_context(*existing, context);
  assign_pending_call_id(*existing, call_id, payload.call_id);
  existing->item.status = ToolTimelineStatus::Running;
  existing->item.lifecycle = ToolLifecycleState::Progress;
  existing->item.result_summary = payload.text;
  attach_permission_audits(state, existing->item);
  upsert_activity(state, existing->call_id, existing->item);
}

void apply_tool_result(TuiEventState& state, ava::event::ToolPayload const& payload, ava::event::EventEnvelopeContext const& context)
{
  auto const call_id = effective_tool_call_id(payload, context);
  auto status = tool_status_from_payload(payload);
  auto item = ToolTimelineItem{.status = status,
                               .name = payload.tool,
                               .result_summary = payload.text,
                               .arguments_json = payload.args_json,
                               .result_json = payload.result_json,
                               .call_id = call_id,
                               .lifecycle = tool_lifecycle_for_status(status)};
  apply_tool_payload(item, payload);
  auto existing = find_pending_tool(state, payload.call_id, context.request_id.value_or(""), context.correlation_id.value_or(""), call_id);
  if (existing != state.pending_tools.end())
  {
    if (item.name.empty())
      item.name = existing->item.name;
    item.argument_summary = existing->item.argument_summary;
    if (item.arguments_json.empty())
      item.arguments_json = existing->item.arguments_json;
    if (item.result_json.empty())
      item.result_json = existing->item.result_json;
    if (item.call_id.empty())
      item.call_id = existing->call_id;
    item.request_id = existing->request_id;
    item.correlation_id = existing->correlation_id;
    item.subagent_launch_display = existing->item.subagent_launch_display;
    for (auto const& permission_id : existing->item.permission_request_ids) add_permission_request_id(item, permission_id);
    for (auto const& audit : existing->item.permissions) add_permission_audit(item, audit);
    state.pending_tools.erase(existing);
  }
  if (context.request_id)
    item.request_id = *context.request_id;
  if (context.correlation_id)
    item.correlation_id = *context.correlation_id;
  attach_permission_audits(state, item);
  upsert_activity(state, item.call_id, item);
  record_modified_file(state, item);
  apply_todowrite_result(state, item);
  state.transcript.push_back(TranscriptItem{.tool = std::move(item)});
}

std::string error_text_for_payload(ava::event::ErrorPayload const& payload)
{
  if (!payload.error_message.empty())
    return payload.error_message;
  if (!payload.error_details.empty())
    return payload.error_details;
  return payload.text;
}

std::string error_text_for_payload(ava::event::ProviderPayload const& payload)
{
  if (!payload.error_message.empty())
    return payload.error_message;
  if (!payload.error_details.empty())
    return payload.error_details;
  return payload.text;
}

std::string first_error_line(std::string_view text)
{
  auto const end = text.find_first_of("\r\n");
  auto line = end == std::string_view::npos ? text : text.substr(0, end);
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
  return std::string(line);
}

std::string strip_duplicate_error_summary(std::string details, std::string_view summary)
{
  if (summary.empty() || !details.starts_with(summary))
    return details;
  details.erase(0, summary.size());
  while (!details.empty() && (details.front() == '\r' || details.front() == '\n')) details.erase(details.begin());
  return details;
}

TranscriptItem error_transcript_item(std::string text, std::string details)
{
  auto summary = first_error_line(text);
  if (summary.empty())
    summary = "error";
  details = strip_duplicate_error_summary(std::move(details), summary);
  auto item = transcript_text_item("error", std::move(summary));
  item.meta = std::move(details);
  return item;
}

bool is_provider_tool_call_status(std::string_view status)
{
  return status == "tool_call_start" || status == "tool_call_delta" || status == "tool_call_end";
}

std::string provider_tool_call_detail(std::string_view status)
{
  if (status == "tool_call_start")
    return "provider is preparing tool call";
  if (status == "tool_call_delta")
    return "streaming tool arguments";
  if (status == "tool_call_end")
    return "tool call ready";
  return {};
}

ToolTimelineStatus provider_tool_call_activity_status(std::string_view status)
{
  return status == "tool_call_end" ? ToolTimelineStatus::Success : ToolTimelineStatus::Running;
}

ToolLifecycleState provider_tool_call_lifecycle(std::string_view status)
{
  if (status == "tool_call_start")
    return ToolLifecycleState::ProviderAnnounced;
  if (status == "tool_call_delta")
    return ToolLifecycleState::ArgumentsStreaming;
  if (status == "tool_call_end")
    return ToolLifecycleState::ArgumentsComplete;
  return ToolLifecycleState::ProviderAnnounced;
}

void upsert_provider_tool_call_activity(TuiEventState& state, ava::event::ProviderPayload const& payload, ava::event::EventEnvelopeContext const& context)
{
  auto item = SidebarActivityItem{.id = effective_provider_tool_call_id(payload, context),
                                  .label = payload.tool.empty() ? std::string("tool call") : payload.tool,
                                  .detail = provider_tool_call_detail(payload.status),
                                  .status = provider_tool_call_activity_status(payload.status)};
  auto existing = std::ranges::find_if(state.activity, [&](SidebarActivityItem const& activity) { return activity.id == item.id; });
  if (existing != state.activity.end())
  {
    if (payload.tool.empty() && !existing->label.empty())
      item.label = existing->label;
    *existing = std::move(item);
    return;
  }
  upsert_sidebar_activity(state, std::move(item));
}

void upsert_provider_tool_call(TuiEventState& state, ava::event::ProviderPayload const& payload, ava::event::EventEnvelopeContext const& context)
{
  auto const tool_id = effective_provider_tool_call_id(payload, context);
  auto existing = find_pending_tool(state, payload.call_id, context.request_id.value_or(""), context.correlation_id.value_or(""), tool_id);
  if (existing == state.pending_tools.end())
  {
    state.pending_tools.emplace_back(
        PendingToolItem{.call_id = tool_id,
                        .backend_call_id = payload.call_id,
                        .request_id = {},
                        .correlation_id = {},
                        .item = ToolTimelineItem{.status = ToolTimelineStatus::Running,
                                                 .name = payload.tool,
                                                 .argument_summary = payload.status == "tool_call_delta" ? payload.text : std::string{},
                                                 .call_id = tool_id,
                                                 .lifecycle = provider_tool_call_lifecycle(payload.status)},
                        .append_only_stream = payload.status == "tool_call_delta"});
    apply_tool_context(state.pending_tools.back(), context);
    upsert_activity(state, tool_id, state.pending_tools.back().item);
    return;
  }
  if (!payload.tool.empty())
    existing->item.name = payload.tool;
  if (payload.status == "tool_call_delta")
  {
    existing->item.argument_summary += payload.text;
    existing->append_only_stream = true;
  }
  apply_tool_context(*existing, context);
  assign_pending_call_id(*existing, tool_id, payload.call_id);
  existing->item.status = ToolTimelineStatus::Running;
  existing->item.lifecycle = provider_tool_call_lifecycle(payload.status);
  upsert_activity(state, existing->call_id, existing->item);
}

std::optional<ToolTimelineItem> pending_tool_projection(TuiEventState const& state, std::string_view backend_call_id, std::string_view effective_call_id)
{
  auto pending = std::ranges::find_if(state.pending_tools, [&](PendingToolItem const& item) {
    if (!backend_call_id.empty())
      return item.backend_call_id == backend_call_id;
    return !effective_call_id.empty() && (item.call_id == effective_call_id || item.item.call_id == effective_call_id);
  });
  return pending == state.pending_tools.end() ? std::nullopt : std::optional<ToolTimelineItem>{pending->item};
}

std::optional<ToolTimelineItem> completed_tool_projection(TuiEventState const& state, std::string_view effective_call_id, std::string_view tool_name)
{
  for (auto item = state.transcript.rbegin(); item != state.transcript.rend(); ++item)
  {
    if (!item->tool)
      continue;
    if (!effective_call_id.empty() && item->tool->call_id == effective_call_id)
      return item->tool;
    if (effective_call_id.empty() && !tool_name.empty() && item->tool->name == tool_name)
      return item->tool;
  }
  return std::nullopt;
}

void apply_provider_event(TuiEventState& state, ava::event::ProviderPayload const& payload, ava::event::EventEnvelopeContext const& context)
{
  if (is_provider_tool_call_status(payload.status))
  {
    upsert_provider_tool_call(state, payload, context);
    upsert_provider_tool_call_activity(state, payload, context);
    return;
  }
  if (payload.status == "tui:permission_request" || payload.status == "tui:permission_allow" || payload.status == "tui:permission_deny" ||
      payload.status == "tui:question_request" || payload.status == "tui:question_answer" || payload.status == "tui:question_cancel")
  {
    remember_permission_provider_event(state, payload);
    auto const text = payload.text.empty() ? payload.status : payload.text;
    auto const prompt_status = payload.status.substr(std::string_view{"tui:"}.size());
    upsert_sidebar_activity(
        state, SidebarActivityItem{
                   .id = "prompt:" + payload.status,
                   .label = prompt_status.starts_with("permission") ? "permission" : "question",
                   .detail = text,
                   .status = prompt_status.ends_with("deny") || prompt_status.ends_with("cancel") ? ToolTimelineStatus::Error : ToolTimelineStatus::Success});
    return;
  }
  if (payload.status == "error")
  {
    auto detail = error_text_for_payload(payload);
    if (detail.empty())
      return;
    auto id = payload.call_id.empty() ? std::string("provider:error") : payload.call_id;
    auto label = payload.tool.empty() ? std::string("provider/error") : payload.tool;
    upsert_sidebar_activity(
        state, SidebarActivityItem{.id = std::move(id), .label = std::move(label), .detail = std::move(detail), .status = ToolTimelineStatus::Error});
  }
}

}  // namespace

void apply_runtime_event(TuiEventState& state, ava::event::RuntimeEvent const& event, ava::event::EventEnvelopeContext const& context)
{
  remember_context_ids(state, context);
  std::visit(
      [&]<typename Event>(Event const& typed_event) {
        auto const& payload = typed_event.payload;
        if constexpr (!std::same_as<Event, ava::event::RetryEvent> && !std::same_as<Event, ava::event::RetryTickEvent>)
        {
          if constexpr (std::same_as<Event, ava::event::CancellationEvent>)
          {
            settle_running_retry_activities(state, ToolTimelineStatus::Canceled, "retry canceled");
          }
          else if constexpr (std::same_as<Event, ava::event::ErrorEvent>)
          {
            if (is_cancel_error(payload))
              settle_running_retry_activities(state, ToolTimelineStatus::Canceled, "retry canceled");
            else
              settle_running_retry_activities(state, ToolTimelineStatus::Error, "retry failed");
          }
          else
          {
            settle_running_retry_activities(state, ToolTimelineStatus::Success, "retry completed");
          }
        }
        if constexpr (std::same_as<Event, ava::event::SessionStartEvent>)
        {
          state.current_mode = payload.mode;
          if (!payload.provider.empty())
            state.current_provider_id = payload.provider;
          if (!payload.model.empty())
            state.current_model_id = payload.model;
          state.run_status = TuiEventRunStatus::Running;
          state.stream_assistant_transcript_index = std::nullopt;
          state.pending_assistant_meta.clear();
          state.pending_reasoning_text.clear();
          state.stop_reason.clear();
          state.error_text.clear();
          state.error_details.clear();
        }
        else if constexpr (std::same_as<Event, ava::event::UserMessageEvent>)
        {
          state.stream_assistant_transcript_index = std::nullopt;
          state.pending_assistant_meta.clear();
          state.pending_reasoning_text.clear();
          state.transcript.push_back(transcript_text_item("you", payload.text));
          state.run_status = TuiEventRunStatus::Running;
        }
        else if constexpr (std::same_as<Event, ava::event::AssistantMessageEvent>)
        {
          apply_assistant_final(state, payload);
          settle_responding_activity(state, ToolTimelineStatus::Success, "assistant responded");
          state.run_status = TuiEventRunStatus::Completed;
        }
        else if constexpr (std::same_as<Event, ava::event::MessageUpdateEvent>)
        {
          update_pending_assistant_meta(state);
          state.pending_assistant_text += payload.text;
          if (state.activity.empty() || state.activity.back().label != "responding")
            state.activity.push_back(SidebarActivityItem{.id = "responding", .label = "responding", .detail = "assistant is writing"});
          state.run_status = TuiEventRunStatus::Running;
        }
        else if constexpr (std::same_as<Event, ava::event::MessageEndEvent>)
        {
          commit_pending_assistant_text(state, assistant_meta_for_state(state));
          settle_responding_activity(state, ToolTimelineStatus::Success, "assistant responded");
          state.run_status = TuiEventRunStatus::Completed;
        }
        else if constexpr (std::same_as<Event, ava::event::ReasoningStartEvent>)
        {
          state.pending_reasoning_text.clear();
          state.pending_reasoning_redacted = payload.reasoning_redacted;
          update_pending_assistant_meta(state);
          upsert_sidebar_activity(state, SidebarActivityItem{.id = "reasoning", .label = "reasoning", .detail = "model reasoning started"});
          state.run_status = TuiEventRunStatus::Running;
        }
        else if constexpr (std::same_as<Event, ava::event::ReasoningDeltaEvent>)
        {
          update_pending_assistant_meta(state);
          if (payload.reasoning_redacted)
          {
            state.pending_reasoning_redacted = true;
            if (state.pending_reasoning_text.empty())
              state.pending_reasoning_text = "[reasoning redacted]";
          }
          else if (!state.pending_reasoning_redacted)
          {
            state.pending_reasoning_text += payload.text;
          }
          upsert_sidebar_activity(state, SidebarActivityItem{.id = "reasoning",
                                                             .label = "reasoning",
                                                             .detail = state.pending_reasoning_redacted ? "reasoning redacted"
                                                                       : payload.text.empty()           ? "model is reasoning"
                                                                                                        : payload.text});
          state.run_status = TuiEventRunStatus::Running;
        }
        else if constexpr (std::same_as<Event, ava::event::ReasoningEndEvent>)
        {
          update_pending_assistant_meta(state);
          if (payload.reasoning_redacted && state.pending_reasoning_text.empty())
          {
            state.pending_reasoning_text = "[reasoning redacted]";
            state.pending_reasoning_redacted = true;
          }
          upsert_sidebar_activity(
              state,
              SidebarActivityItem{.id = "reasoning", .label = "reasoning", .detail = "model reasoning completed", .status = ToolTimelineStatus::Success});
          state.run_status = TuiEventRunStatus::Running;
        }
        else if constexpr (std::same_as<Event, ava::event::ProviderEvent>)
        {
          apply_provider_event(state, payload, context);
        }
        else if constexpr (std::same_as<Event, ava::event::ToolStartEvent>)
        {
          commit_pending_reasoning_turn(state, assistant_meta_for_state(state));
          apply_tool_start(state, payload, context);
          state.run_status = TuiEventRunStatus::Running;
        }
        else if constexpr (std::same_as<Event, ava::event::ToolProgressEvent>)
        {
          apply_tool_progress(state, payload, context);
          state.run_status = TuiEventRunStatus::Running;
        }
        else if constexpr (std::same_as<Event, ava::event::ToolResultEvent>)
        {
          apply_tool_result(state, payload, context);
          state.run_status = TuiEventRunStatus::Completed;
        }
        else if constexpr (std::same_as<Event, ava::event::CompactionStartEvent>)
        {
          upsert_sidebar_activity(state, SidebarActivityItem{.id = first_non_empty({payload.trigger, "compaction"}),
                                                             .label = "compaction",
                                                             .detail = compact_lifecycle_detail("compaction started", payload),
                                                             .status = ToolTimelineStatus::Running});
          state.run_status = TuiEventRunStatus::Running;
        }
        else if constexpr (std::same_as<Event, ava::event::CompactionEndEvent>)
        {
          auto detail = compact_lifecycle_detail("compaction completed", payload);
          state.transcript.push_back(transcript_text_item("compaction", detail));
          upsert_sidebar_activity(state, SidebarActivityItem{.id = first_non_empty({payload.trigger, "compaction"}),
                                                             .label = "compaction",
                                                             .detail = std::move(detail),
                                                             .status = ToolTimelineStatus::Success});
          state.run_status = TuiEventRunStatus::Completed;
        }
        else if constexpr (std::same_as<Event, ava::event::RetryEvent>)
        {
          // A new retry owns the live chrome row. Prior Running retries (including
          // different reason/trigger ids) are settled first so stale identities cannot
          // resurface after the current attempt progresses or finishes.
          settle_running_retry_activities(state, ToolTimelineStatus::Success, "retry superseded");
          auto detail = std::string("retrying");
          if (!payload.reason.empty())
            detail += " after " + payload.reason;
          if (!payload.trigger.empty() && payload.trigger != payload.reason)
            detail += " (" + payload.trigger + ")";
          if (payload.attempt > 0)
          {
            detail += " attempt " + std::to_string(payload.attempt);
            if (payload.max_attempts > 0)
              detail += "/" + std::to_string(payload.max_attempts);
          }
          if (payload.delay_ms > 0)
            detail += " delay=" + std::to_string(payload.delay_ms) + "ms";
          if (typed_event.diagnostics.estimated_tokens > 0)
            detail += " tokens~" + std::to_string(typed_event.diagnostics.estimated_tokens);
          if (typed_event.diagnostics.threshold_tokens > 0)
            detail += "/" + std::to_string(typed_event.diagnostics.threshold_tokens);
          if (typed_event.diagnostics.snapshot_entries > 0 || typed_event.diagnostics.current_entries > 0)
          {
            detail += " entries=" + std::to_string(typed_event.diagnostics.snapshot_entries) + "/" + std::to_string(typed_event.diagnostics.current_entries);
          }
          if (typed_event.diagnostics.summary_bytes > 0)
            detail += " summary=" + std::to_string(typed_event.diagnostics.summary_bytes) + " bytes";
          if (!payload.text.empty())
            detail += " - " + payload.text;
          // Audit transcript keeps every RetryEvent; sidebar ownership is the live row only.
          state.transcript.push_back(transcript_text_item("audit", detail));
          // Zero-delay RetryEvent announces an immediate attempt (transport emits no ticks).
          // Keep the current row Running so chrome can show briefly until progress/cancel/error.
          upsert_sidebar_activity(
              state,
              SidebarActivityItem{.id = retry_activity_id(payload), .label = "retry", .detail = std::move(detail), .status = ToolTimelineStatus::Running});
          state.run_status = TuiEventRunStatus::Running;
        }
        else if constexpr (std::same_as<Event, ava::event::RetryTickEvent>)
        {
          auto detail = std::string("retry countdown");
          if (!payload.reason.empty())
            detail += " after " + payload.reason;
          if (!payload.trigger.empty() && payload.trigger != payload.reason)
            detail += " (" + payload.trigger + ")";
          if (payload.attempt > 0)
          {
            detail += " attempt " + std::to_string(payload.attempt);
            if (payload.max_attempts > 0)
              detail += "/" + std::to_string(payload.max_attempts);
          }
          if (payload.delay_ms > 0)
            detail += " delay=" + std::to_string(payload.delay_ms) + "ms";
          detail += " remaining=" + std::to_string(payload.remaining_ms) + "ms";
          if (!payload.text.empty())
            detail += " - " + payload.text;
          upsert_retry_countdown_transcript(state, detail);
          if (payload.remaining_ms == 0)
          {
            // Countdown completion closes every Running retry identity, not only the tick's id.
            settle_running_retry_activities(state, ToolTimelineStatus::Success, "retry completed");
            upsert_sidebar_activity(
                state,
                SidebarActivityItem{.id = retry_activity_id(payload), .label = "retry", .detail = "retry completed", .status = ToolTimelineStatus::Success});
          }
          else
          {
            upsert_sidebar_activity(
                state,
                SidebarActivityItem{.id = retry_activity_id(payload), .label = "retry", .detail = std::move(detail), .status = ToolTimelineStatus::Running});
          }
          state.run_status = TuiEventRunStatus::Running;
        }
        else if constexpr (std::same_as<Event, ava::event::CancellationEvent>)
        {
          apply_canceled_event(state, payload.text, payload.reason);
        }
        else if constexpr (std::same_as<Event, ava::event::ErrorEvent>)
        {
          if (is_cancel_error(payload))
          {
            apply_canceled_event(state, payload.text, payload.reason);
            return;
          }
          commit_pending_assistant_text(state, assistant_meta_for_state(state));
          state.error_text = error_text_for_payload(payload);
          state.error_details = payload.error_details;
          if (!state.error_text.empty() || !state.error_details.empty())
          {
            auto transcript_text = state.error_text.empty() ? state.error_details : state.error_text;
            state.transcript.push_back(error_transcript_item(std::move(transcript_text), state.error_details));
          }
          settle_responding_activity(state, ToolTimelineStatus::Error, "assistant failed");
          state.run_status = TuiEventRunStatus::Error;
        }
        else if constexpr (std::same_as<Event, ava::event::CompletionEvent>)
        {
          commit_pending_assistant_text(state, assistant_meta_for_state(state));
          state.stop_reason = payload.stop_reason;
          state.provider_iterations = payload.provider_iterations;
          state.tool_calls = payload.tool_calls;
          settle_responding_activity(state, ToolTimelineStatus::Success, "assistant responded");
          state.run_status = TuiEventRunStatus::Done;
        }
        else
        {
          static_assert(std::same_as<Event, void>, "unhandled RuntimeEvent alternative");
        }
      },
      event.payload());
}

std::optional<ToolTimelineItem> runtime_event_tool_projection(TuiEventState const& state, ava::event::RuntimeEvent const& event,
                                                              ava::event::EventEnvelopeContext const& context)
{
  return std::visit(
      [&]<typename Event>(Event const& typed_event) -> std::optional<ToolTimelineItem> {
        auto const& payload = typed_event.payload;
        if constexpr (std::same_as<Event, ava::event::ProviderEvent>)
        {
          if (!is_provider_tool_call_status(payload.status))
            return std::nullopt;
          auto const call_id = effective_provider_tool_call_id(payload, context);
          return pending_tool_projection(state, payload.call_id, call_id);
        }
        else if constexpr (std::same_as<Event, ava::event::ToolStartEvent> || std::same_as<Event, ava::event::ToolProgressEvent>)
        {
          auto const call_id = effective_tool_call_id(payload, context);
          return pending_tool_projection(state, payload.call_id, call_id);
        }
        else if constexpr (std::same_as<Event, ava::event::ToolResultEvent>)
        {
          auto const call_id = effective_tool_call_id(payload, context);
          return completed_tool_projection(state, call_id, payload.tool);
        }
        else
        {
          return std::nullopt;
        }
      },
      event.payload());
}

void apply_subagent_launch_notification(TuiEventState& state, ava::agent::SubagentLaunchNotification const& notification)
{
  if (notification.tool_call_id.empty() || notification.request_id.empty() || notification.correlation_id.empty())
    return;

  auto match = state.pending_tools.end();
  for (auto pending = state.pending_tools.begin(); pending != state.pending_tools.end(); ++pending)
  {
    if (pending->item.status != ToolTimelineStatus::Running || pending->item.name != "task" || pending->backend_call_id != notification.tool_call_id ||
        pending->request_id.empty() || pending->request_id != notification.request_id || pending->correlation_id.empty() ||
        pending->correlation_id != notification.correlation_id)
    {
      continue;
    }
    if (match != state.pending_tools.end())
      return;
    match = pending;
  }
  if (match != state.pending_tools.end())
    match->item.subagent_launch_display = notification.display;
}

void apply_control_event_envelope(TuiEventState& state, ava::event::EventEnvelope const& envelope)
{
  auto const prompt_requested = envelope.name == "permission_requested" || envelope.name == "question_requested";
  auto const prompt_replied = envelope.name == "permission_replied" || envelope.name == "question_replied";
  auto const queue_lifecycle = envelope.name == "steer_queued" || envelope.name == "steer_applied" || envelope.name == "steer_skipped" ||
                               envelope.name == "follow_up_queued" || envelope.name == "follow_up_started" || envelope.name == "follow_up_skipped";
  if (!prompt_requested && !prompt_replied && envelope.name != "cancel_requested" && !queue_lifecycle)
    return;

  remember_envelope_ids(state, envelope);
  if (prompt_requested)
  {
    if (envelope.name == "permission_requested")
      remember_permission_request_envelope(state, envelope);
    apply_prompt_request_envelope(state, envelope);
    return;
  }
  if (prompt_replied)
  {
    if (envelope.name == "permission_replied")
      remember_permission_reply_envelope(state, envelope);
    auto const replied = envelope.name == "permission_replied" ? std::string("permission replied") : std::string("question replied");
    upsert_sidebar_activity(state, SidebarActivityItem{.id = first_non_empty({envelope.request_id.value_or(""), envelope.correlation_id.value_or(""), replied}),
                                                       .label = envelope.name == "permission_replied" ? "permission" : "question",
                                                       .detail = replied,
                                                       .status = ToolTimelineStatus::Success});
    return;
  }
  if (envelope.name == "cancel_requested")
  {
    auto detail = std::string("cancel requested");
    if (bool_field(envelope.payload_json, "active_run"))
      detail += " for active run";
    auto const cleared_steer = size_field(envelope.payload_json, "cleared_steer");
    auto const cleared_follow_up = size_field(envelope.payload_json, "cleared_follow_up");
    if (cleared_steer > 0 || cleared_follow_up > 0)
      detail += " cleared steer=" + std::to_string(cleared_steer) + " follow-up=" + std::to_string(cleared_follow_up);
    state.transcript.push_back(transcript_text_item("audit", detail));
    upsert_sidebar_activity(state,
                            SidebarActivityItem{.id = first_non_empty({envelope.request_id.value_or(""), envelope.correlation_id.value_or(""), "cancel"}),
                                                .label = "cancel",
                                                .detail = std::move(detail),
                                                .status = ToolTimelineStatus::Error});
    return;
  }
  if (envelope.name == "steer_queued" || envelope.name == "follow_up_queued")
    upsert_queued_message(state, envelope);
  else
    remove_queued_message(state, envelope);
  auto detail = queue_event_detail(envelope);
  state.transcript.push_back(transcript_text_item("audit", detail));
  upsert_sidebar_activity(state, SidebarActivityItem{.id = queue_event_id(envelope),
                                                     .label = envelope.name.starts_with("steer") ? "steer" : "follow-up",
                                                     .detail = std::move(detail),
                                                     .status = queue_event_status(envelope.name)});
}

std::vector<TranscriptItem> event_state_transcript_snapshot(TuiEventState const& state, PendingTextProjection pending_text_projection)
{
  auto snapshot = state.transcript;
  snapshot.reserve(snapshot.size() + state.pending_tools.size() + (state.pending_assistant_text.empty() ? 0U : 1U) +
                   (state.pending_reasoning_text.empty() ? 0U : 1U));
  if (!state.pending_reasoning_text.empty() || !state.pending_assistant_text.empty())
  {
    auto const stream_id = first_non_empty({state.active_message_id.value_or(""), state.active_turn_id.value_or(""), state.active_run_id.value_or("")});
    auto text_model = Text{};
    auto thinking_model = Text{};
    if (pending_text_projection == PendingTextProjection::CompleteModels)
    {
      text_model = text_from_markdown(state.pending_assistant_text);
      thinking_model = text_from_plain(state.pending_reasoning_text);
    }
    snapshot.push_back(TranscriptItem{.label = "ava",
                                      .text = state.pending_assistant_text,
                                      .text_model = std::move(text_model),
                                      .meta = state.pending_assistant_meta,
                                      .thinking = state.pending_reasoning_text,
                                      .thinking_model = std::move(thinking_model),
                                      .stream_id = stream_id,
                                      .append_only_stream = !stream_id.empty()});
  }
  for (auto const& tool : state.pending_tools)
  {
    snapshot.push_back(TranscriptItem{.tool = tool.item, .stream_id = tool.call_id, .append_only_stream = tool.append_only_stream && !tool.call_id.empty()});
  }
  return snapshot;
}

}  // namespace ava::tui
