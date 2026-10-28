#include "sys.h"
#include "ava/app/acp/codec_json_internal.h"
#include "ava/app/acp/permission.h"
#include "ava/app/acp/protocol.h"
#include "ava/app/acp/session_update.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>

namespace ava::app::acp {
namespace {

using Json = nlohmann::json;

JsonRpcError invalid_permission_response(std::string message)
{
  return JsonRpcError{.code = -32602,
                      .message = std::move(message),
                      .data_json = std::nullopt,
                      .id = std::nullopt,
                      .intent = EnvelopeIntent::Response,
                      .suppress_response = true};
}

bool path_is_within(std::filesystem::path const& root, std::filesystem::path const& candidate)
{
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it)
    if (candidate_it == candidate.end() || *candidate_it != *root_it)
      return false;
  return true;
}

std::optional<std::string> safe_location(std::filesystem::path const& workspace_root, std::filesystem::path const& path)
{
  if (path.empty())
    return std::nullopt;
  auto normalized = path.is_absolute() ? path.lexically_normal() : (workspace_root / path).lexically_normal();
  if (!normalized.is_absolute() || !path_is_within(workspace_root, normalized))
    return std::nullopt;
  auto text = normalized.string();
  if (text.empty() || text.size() > 4096 ||
      std::ranges::any_of(text, [](char ch) { return static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) == 0x7f; }))
    return std::nullopt;
  return text;
}

std::string bounded_indented_literal(std::string_view value, std::size_t max_bytes, bool& truncated)
{
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string text;
  text.reserve(std::min(value.size(), max_bytes) + 32);
  auto append = [&](std::string_view part) {
    if (text.size() + part.size() > max_bytes)
    {
      truncated = true;
      return false;
    }
    text.append(part);
    return true;
  };
  for (unsigned char const byte : value)
  {
    if (byte == '\n')
    {
      if (!append("\n    "))
        break;
    }
    else if (byte == '\r')
    {
      if (!append("\\r"))
        break;
    }
    else if (byte == '\t')
    {
      if (!append("\\t"))
        break;
    }
    else if (byte < 0x20 || byte == 0x7f)
    {
      std::array<char, 4> escaped{'\\', 'x', kHex[(byte >> 4U) & 0x0fU], kHex[byte & 0x0fU]};
      if (!append(std::string_view(escaped.data(), escaped.size())))
        break;
    }
    else
    {
      char const ch = static_cast<char>(byte);
      if (!append(std::string_view(&ch, 1)))
        break;
    }
  }
  if (text.size() < value.size())
    truncated = true;
  return text;
}

std::string permission_description(ava::permissions::PermissionPrompt const& prompt, bool& command_display_truncated)
{
  bool reason_truncated = false;
  std::string text = prompt.reason.empty() ? std::string("AVA requires approval before executing this tool call.")
                                           : bounded_indented_literal(prompt.reason, 2U * 1024U, reason_truncated);
  if (reason_truncated)
    text += " [reason truncated]";
  text += " Risk: ";
  text += ava::permissions::to_string(prompt.risk);
  text += ". Review the exact bounded action below before deciding.";

  if (!prompt.command.empty())
  {
    auto command = bounded_indented_literal(prompt.command, 6U * 1024U, command_display_truncated);
    text += "\n\nCommand or operation identity:\n\n    ";
    text += command;
    if (command_display_truncated)
      text += "\n\n[command display truncated; session-wide decisions are unavailable]";
  }
  if (prompt.command_metadata && !prompt.command_metadata->workspace_recipe_key.empty())
  {
    text += "\n\nStable recipe: ";
    text += prompt.command_metadata->recipe_display;
    text += "\nWorkspace recipe key: ";
    text += prompt.command_metadata->workspace_recipe_key;
    text += "\nAllowed scopes: ";
    for (std::size_t index = 0; index < prompt.command_metadata->effective_allowed_scopes.size(); ++index)
    {
      if (index > 0)
        text += ", ";
      text += ava::command::to_string(prompt.command_metadata->effective_allowed_scopes[index]);
    }
  }
  if (!prompt.diff_preview.empty())
  {
    bool truncated = prompt.diff_truncated;
    auto diff = bounded_indented_literal(prompt.diff_preview, 8U * 1024U, truncated);
    text += "\n\nProposed file diff:\n\n    ";
    text += diff;
    if (truncated)
      text += "\n\n[diff display truncated; this mutation cannot receive a session-wide grant]";
  }
  if (prompt.command.empty() && prompt.diff_preview.empty())
    text += "\n\nNo command or mutation diff applies; verify the displayed tool and location.";
  if (text.size() > 16U * 1024U)
    text.resize(16U * 1024U);
  return text;
}

}  // namespace

void ClientRequestGateway::bind(ClientRequestSender sender, ClientRequestCanceler canceler, ClientConnectionAborter aborter)
{
  std::lock_guard lock(mutex_);
  sender_ = std::move(sender);
  canceler_ = std::move(canceler);
  aborter_ = std::move(aborter);
}

void ClientRequestGateway::unbind()
{
  std::lock_guard lock(mutex_);
  sender_ = {};
  canceler_ = {};
  aborter_ = {};
}

ava::core::Result<PendingCall> ClientRequestGateway::send(std::string method, std::optional<std::string> params_json, std::chrono::milliseconds timeout,
                                                          OutboundCallPolicy policy) const
{
  ClientRequestSender sender;
  {
    std::lock_guard lock(mutex_);
    sender = sender_;
  }
  if (!sender)
    return std::unexpected(protocol_error("ACP client request gateway is unavailable"));
  return sender(std::move(method), std::move(params_json), timeout, policy);
}

bool ClientRequestGateway::cancel(JsonRpcId const& id, std::string reason) const
{
  ClientRequestCanceler canceler;
  {
    std::lock_guard lock(mutex_);
    canceler = canceler_;
  }
  return canceler && canceler(id, std::move(reason));
}

void ClientRequestGateway::abort(std::string reason) const
{
  ClientConnectionAborter aborter;
  {
    std::lock_guard lock(mutex_);
    aborter = aborter_;
  }
  if (aborter)
    aborter(std::move(reason));
}

bool permission_request_offers_session_decisions(ava::permissions::PermissionPrompt const& prompt)
{
  if (prompt.operation == ava::permissions::Operation::EditFile)
    return false;
  if (prompt.command_metadata && !ava::permissions::command_permission_allows_reusable_grant(*prompt.command_metadata))
    return false;
  bool truncated = false;
  static_cast<void>(bounded_indented_literal(prompt.command, 6U * 1024U, truncated));
  return !truncated;
}

ava::core::Result<std::string> encode_permission_request_params(std::string_view session_id, ava::permissions::PermissionPrompt const& prompt,
                                                                std::filesystem::path const& workspace_root)
{
  auto const tool_call_id = !prompt.tool_call_id.empty() ? prompt.tool_call_id : prompt.permission_request_id;
  if (session_id.empty() || tool_call_id.empty() || tool_call_id.size() > kMaxStringBytes)
    return std::unexpected(protocol_error("ACP permission request is missing bounded session or tool-call identity"));

  auto const tool_name = prompt.tool_name.empty() ? ava::permissions::to_string(prompt.operation) : prompt.tool_name;
  bool command_display_truncated = false;
  auto const description = permission_description(prompt, command_display_truncated);
  Json tool_call{{"toolCallId", tool_call_id},
                 {"title", "Permission required: " + tool_name},
                 {"kind", acp_tool_kind(tool_name)},
                 {"status", "pending"},
                 {"content", Json::array({Json{{"type", "content"}, {"content", Json{{"type", "text"}, {"text", description}}}}})}};
  if (auto location = safe_location(workspace_root, prompt.target_path))
    tool_call["locations"] = Json::array({Json{{"path", *location}}});

  bool const offers_session_decisions = permission_request_offers_session_decisions(prompt);
  Json options = Json::array({Json{{"optionId", "allow_once"}, {"name", "Allow once"}, {"kind", "allow_once"}}});
  if (offers_session_decisions)
    options.push_back(Json{{"optionId", "allow_always"}, {"name", "Always allow this exact request for this session"}, {"kind", "allow_always"}});
  options.push_back(Json{{"optionId", "reject_once"}, {"name", "Reject once"}, {"kind", "reject_once"}});
  if (offers_session_decisions)
    options.push_back(Json{{"optionId", "reject_always"}, {"name", "Always reject this exact request for this session"}, {"kind", "reject_always"}});
  Json params{{"sessionId", std::string(session_id)}, {"toolCall", std::move(tool_call)}, {"options", std::move(options)}};
  auto encoded = params.dump(-1, ' ', false, Json::error_handler_t::replace);
  if (encoded.size() > kMaxRecordBytes)
    return std::unexpected(protocol_error("encoded ACP permission request exceeds record limit"));
  return encoded;
}

std::expected<AcpPermissionSelection, JsonRpcError> decode_permission_response(std::string_view result_json)
{
  auto decoded = decode_json_value(result_json);
  if (!decoded)
    return std::unexpected(std::move(decoded.error()));
  auto const& result = *decoded;
  if (!result.is_object())
    return std::unexpected(invalid_permission_response("session/request_permission result must be an object"));
  auto outcome = result.find("outcome");
  if (outcome == result.end() || !outcome->is_object())
    return std::unexpected(invalid_permission_response("session/request_permission result requires an outcome object"));
  auto discriminator = outcome->find("outcome");
  if (discriminator == outcome->end() || !discriminator->is_string())
    return std::unexpected(invalid_permission_response("permission outcome discriminator must be a string"));
  if (*discriminator == "cancelled")
  {
    if (outcome->contains("optionId"))
      return std::unexpected(invalid_permission_response("cancelled permission outcome must not select an option"));
    return AcpPermissionSelection::Cancelled;
  }
  if (*discriminator != "selected")
    return std::unexpected(invalid_permission_response("permission outcome discriminator is unsupported"));
  auto option = outcome->find("optionId");
  if (option == outcome->end() || !option->is_string())
    return std::unexpected(invalid_permission_response("selected permission outcome requires optionId"));
  auto const& id = option->get_ref<std::string const&>();
  if (id == "allow_once")
    return AcpPermissionSelection::AllowOnce;
  if (id == "allow_always")
    return AcpPermissionSelection::AllowAlways;
  if (id == "reject_once")
    return AcpPermissionSelection::RejectOnce;
  if (id == "reject_always")
    return AcpPermissionSelection::RejectAlways;
  return std::unexpected(invalid_permission_response("selected permission optionId was not offered by AVA"));
}

}  // namespace ava::app::acp
