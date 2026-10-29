#include "sys.h"
#include "ava/app/acp/protocol.h"
#include "ava/app/acp/session_update.h"
#include "ava/core/ascii.h"
#include "ava/core/strict_json.h"

#include <algorithm>
#include <cctype>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <nlohmann/json.hpp>

namespace ava::app::acp {
namespace {

using Json = nlohmann::json;

bool path_is_within(std::filesystem::path const& root, std::filesystem::path const& candidate)
{
  auto root_it = root.begin();
  auto candidate_it = candidate.begin();
  for (; root_it != root.end(); ++root_it, ++candidate_it)
    if (candidate_it == candidate.end() || *candidate_it != *root_it)
      return false;
  return true;
}

std::optional<std::filesystem::path> safe_absolute_location(std::filesystem::path const& root, std::string_view value)
{
  if (value.empty() || value.size() > 4096 || ava::core::has_c0_or_del(value))
    return std::nullopt;
  auto path = std::filesystem::path(value);
  if (path.is_relative())
    path = root / path;
  path = path.lexically_normal();
  if (!path.is_absolute() || !path_is_within(root, path))
    return std::nullopt;
  return path;
}

std::size_t utf8_chunk_prefix(std::string_view text, std::size_t max_bytes)
{
  auto boundary = std::min(text.size(), max_bytes);
  if (boundary == text.size())
    return boundary;
  while (boundary > 0 && (static_cast<unsigned char>(text[boundary]) & 0xc0U) == 0x80U) --boundary;
  return boundary;
}

std::string bounded_text(std::string_view text)
{
  if (text.size() <= kMaxToolUpdateContentBytes)
    return std::string(text);
  constexpr std::string_view suffix = "\n[ACP tool content truncated]";
  auto const retained = kMaxToolUpdateContentBytes > suffix.size() ? kMaxToolUpdateContentBytes - suffix.size() : 0;
  return std::string(text.substr(0, retained)) + std::string(suffix);
}

std::string title_for_tool(std::string_view tool_name)
{
  if (tool_name.empty())
    return "Tool call";
  std::string title;
  title.reserve(std::min<std::size_t>(tool_name.size(), 256));
  bool uppercase_next = true;
  for (char ch : tool_name)
  {
    if (title.size() >= 256)
      break;
    if (ch == '_' || ch == '-' || ch == '.')
    {
      if (!title.empty() && title.back() != ' ')
        title.push_back(' ');
      uppercase_next = true;
      continue;
    }
    if (uppercase_next)
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    title.push_back(ch);
    uppercase_next = false;
  }
  return title.empty() ? std::string("Tool call") : title;
}

std::vector<AcpToolCallLocation> event_locations(ava::event::ToolPayload const& event, std::filesystem::path const& root)
{
  std::vector<AcpToolCallLocation> locations;
  auto add = [&](std::string_view value, std::optional<std::size_t> line = std::nullopt) {
    auto path = safe_absolute_location(root, value);
    if (!path)
      return;
    auto const text = path->string();
    if (std::ranges::any_of(locations, [&](AcpToolCallLocation const& item) { return item.path == text; }))
      return;
    locations.push_back(AcpToolCallLocation{.path = text, .line = line});
  };

  for (auto const& path : event.changed_paths)
  {
    if (locations.size() >= 32)
      break;
    add(path);
  }

  if (locations.empty() && !event.args_json.empty() && event.args_json.size() <= kMaxRecordBytes &&
      ava::core::validate_strict_json(event.args_json, kMaxNestingDepth) == ava::core::StrictJsonStatus::Valid)
  {
    auto arguments = Json::parse(event.args_json, nullptr, false, true);
    if (arguments.is_object())
    {
      auto path = arguments.find("path");
      if (path != arguments.end() && path->is_string())
      {
        std::optional<std::size_t> line;
        if (event.start_line > 0)
          line = event.start_line - 1;
        add(path->get_ref<std::string const&>(), line);
      }
    }
  }
  return locations;
}

std::optional<std::vector<AcpToolCallContent>> text_tool_content(std::string_view text)
{
  if (text.empty())
    return std::nullopt;
  std::vector<AcpToolCallContent> content;
  content.emplace_back(AcpToolCallTextContent{.content = AcpTextContent{.text = bounded_text(text)}});
  return content;
}

std::string_view content_chunk_discriminator(AcpContentChunkKind kind)
{
  switch (kind)
  {
    case AcpContentChunkKind::UserMessage:
      return "user_message_chunk";
    case AcpContentChunkKind::AgentMessage:
      return "agent_message_chunk";
    case AcpContentChunkKind::AgentThought:
      return "agent_thought_chunk";
  }
  return "agent_message_chunk";
}

}  // namespace

std::string acp_tool_kind(std::string_view tool_name)
{
  if (tool_name == "read_file" || tool_name == "list_directory")
    return "read";
  if (tool_name == "write_file" || tool_name == "edit_file" || tool_name == "apply_patch")
    return "edit";
  if (tool_name == "glob" || tool_name == "grep")
    return "search";
  if (tool_name == "bash")
    return "execute";
  return "other";
}

RuntimeSessionUpdateMapper::RuntimeSessionUpdateMapper(RuntimeSessionUpdateMapperOptions options) : options_(std::move(options))
{
  options_.workspace_root = options_.workspace_root.lexically_normal();
  options_.max_updates = std::max<std::size_t>(1, options_.max_updates);
  options_.max_encoded_bytes = std::max<std::size_t>(1, options_.max_encoded_bytes);
}

ava::core::Result<std::optional<SessionUpdate>> RuntimeSessionUpdateMapper::map(ava::event::RuntimeEvent const& event)
{
  return std::visit(
      [&](auto const& concrete) -> ava::core::Result<std::optional<SessionUpdate>> {
        using Event = std::remove_cvref_t<decltype(concrete)>;
        auto const& payload = concrete.payload;
        if constexpr (std::same_as<Event, ava::event::MessageUpdateEvent>)
        {
          if (payload.text.empty())
            return std::optional<SessionUpdate>{};
          streamed_agent_text_ = true;
          return std::optional<SessionUpdate>(
              AcpContentChunkUpdate{.kind = AcpContentChunkKind::AgentMessage,
                                    .content = AcpTextContent{.text = payload.text},
                                    .message_id = options_.message_id.empty() ? std::nullopt : std::optional<std::string>(options_.message_id)});
        }
        else if constexpr (std::same_as<Event, ava::event::AssistantMessageEvent>)
        {
          if (payload.text.empty() || streamed_agent_text_)
            return std::optional<SessionUpdate>{};
          streamed_agent_text_ = true;
          return std::optional<SessionUpdate>(
              AcpContentChunkUpdate{.kind = AcpContentChunkKind::AgentMessage,
                                    .content = AcpTextContent{.text = payload.text},
                                    .message_id = options_.message_id.empty() ? std::nullopt : std::optional<std::string>(options_.message_id)});
        }
        else if constexpr (std::same_as<Event, ava::event::ReasoningStartEvent> || std::same_as<Event, ava::event::ReasoningDeltaEvent> ||
                           std::same_as<Event, ava::event::ReasoningEndEvent>)
        {
          if (payload.text.empty() || payload.reasoning_redacted)
            return std::optional<SessionUpdate>{};
          return std::optional<SessionUpdate>(
              AcpContentChunkUpdate{.kind = AcpContentChunkKind::AgentThought,
                                    .content = AcpTextContent{.text = payload.text},
                                    .message_id = options_.message_id.empty() ? std::nullopt : std::optional<std::string>(options_.message_id)});
        }
        else if constexpr (std::same_as<Event, ava::event::ToolStartEvent>)
        {
          if (payload.call_id.empty() || payload.call_id.size() > kMaxStringBytes || ava::core::has_c0_or_del(payload.call_id))
            return std::optional<SessionUpdate>{};
          auto locations = event_locations(payload, options_.workspace_root);
          return std::optional<SessionUpdate>(
              AcpToolCallUpdate{.initial = true,
                                .tool_call_id = payload.call_id,
                                .title = title_for_tool(payload.tool),
                                .kind = acp_tool_kind(payload.tool),
                                .status = "pending",
                                .content = text_tool_content(payload.text),
                                .locations = locations.empty() ? std::nullopt : std::optional<std::vector<AcpToolCallLocation>>(std::move(locations))});
        }
        else if constexpr (std::same_as<Event, ava::event::ToolProgressEvent>)
        {
          if (payload.call_id.empty() || payload.call_id.size() > kMaxStringBytes || ava::core::has_c0_or_del(payload.call_id))
            return std::optional<SessionUpdate>{};
          return std::optional<SessionUpdate>(AcpToolCallUpdate{.initial = false,
                                                                .tool_call_id = payload.call_id,
                                                                .title = title_for_tool(payload.tool),
                                                                .kind = acp_tool_kind(payload.tool),
                                                                .status = "in_progress",
                                                                .content = text_tool_content(payload.text),
                                                                .locations = std::nullopt});
        }
        else if constexpr (std::same_as<Event, ava::event::ToolResultEvent>)
        {
          if (payload.call_id.empty() || payload.call_id.size() > kMaxStringBytes || ava::core::has_c0_or_del(payload.call_id))
            return std::optional<SessionUpdate>{};
          auto locations = event_locations(payload, options_.workspace_root);
          auto const success = payload.status == "success";
          auto const content = !payload.result_json.empty() ? std::string_view(payload.result_json) : std::string_view(payload.text);
          return std::optional<SessionUpdate>(
              AcpToolCallUpdate{.initial = false,
                                .tool_call_id = payload.call_id,
                                .title = title_for_tool(payload.tool),
                                .kind = acp_tool_kind(payload.tool),
                                .status = success ? std::optional<std::string>("completed") : std::optional<std::string>("failed"),
                                .content = text_tool_content(content),
                                .locations = locations.empty() ? std::nullopt : std::optional<std::vector<AcpToolCallLocation>>(std::move(locations))});
        }
        else
        {
          static_assert(std::same_as<Event, ava::event::SessionStartEvent> || std::same_as<Event, ava::event::UserMessageEvent> ||
                        std::same_as<Event, ava::event::MessageEndEvent> || std::same_as<Event, ava::event::ProviderEvent> ||
                        std::same_as<Event, ava::event::CompactionStartEvent> || std::same_as<Event, ava::event::CompactionEndEvent> ||
                        std::same_as<Event, ava::event::RetryEvent> || std::same_as<Event, ava::event::RetryTickEvent> ||
                        std::same_as<Event, ava::event::CancellationEvent> || std::same_as<Event, ava::event::ErrorEvent> ||
                        std::same_as<Event, ava::event::CompletionEvent>);
          return std::optional<SessionUpdate>{};
        }
      },
      event.payload());
}

ava::core::Result<std::optional<std::string>> RuntimeSessionUpdateMapper::account_and_encode(std::optional<SessionUpdate> update)
{
  if (!update)
    return std::optional<std::string>{};
  auto encoded = encode_session_update(*update);
  if (!encoded)
    return std::unexpected(std::move(encoded.error()));
  if (update_count_ >= options_.max_updates || encoded_bytes_ + encoded->size() > options_.max_encoded_bytes)
  {
    auto error = protocol_error("ACP prompt session/update budget is exhausted");
    error.with_context("max_updates", std::to_string(options_.max_updates));
    error.with_context("max_bytes", std::to_string(options_.max_encoded_bytes));
    return std::unexpected(std::move(error));
  }
  ++update_count_;
  encoded_bytes_ += encoded->size();
  return std::optional<std::string>(std::move(*encoded));
}

ava::core::Result<std::optional<std::string>> RuntimeSessionUpdateMapper::map_and_encode(ava::event::RuntimeEvent const& event)
{
  auto update = map(event);
  if (!update)
    return std::unexpected(std::move(update.error()));
  return account_and_encode(std::move(*update));
}

ava::core::Result<std::optional<std::string>> RuntimeSessionUpdateMapper::flush_pending_content()
{
  if (!pending_content_)
    return std::optional<std::string>{};
  SessionUpdate update(std::move(*pending_content_));
  pending_content_.reset();
  return account_and_encode(std::move(update));
}

ava::core::Result<std::vector<std::string>> RuntimeSessionUpdateMapper::flush_coalesced()
{
  std::vector<std::string> encoded;
  auto flushed = flush_pending_content();
  if (!flushed)
    return std::unexpected(std::move(flushed.error()));
  if (*flushed)
    encoded.push_back(std::move(**flushed));
  return encoded;
}

ava::core::Result<std::vector<std::string>> RuntimeSessionUpdateMapper::map_coalesced_and_encode(ava::event::RuntimeEvent const& event)
{
  std::vector<std::string> encoded;
  auto append_flushed = [&]() -> ava::core::VoidResult {
    auto flushed = flush_pending_content();
    if (!flushed)
      return std::unexpected(std::move(flushed.error()));
    if (*flushed)
      encoded.push_back(std::move(**flushed));
    return {};
  };

  auto mapped = map(event);
  if (!mapped)
    return std::unexpected(std::move(mapped.error()));
  if (!*mapped)
  {
    if (auto flushed = append_flushed(); !flushed)
      return std::unexpected(std::move(flushed.error()));
    return encoded;
  }

  auto* content = std::get_if<AcpContentChunkUpdate>(&**mapped);
  if (content == nullptr)
  {
    if (auto flushed = append_flushed(); !flushed)
      return std::unexpected(std::move(flushed.error()));
    auto current = account_and_encode(std::move(**mapped));
    if (!current)
      return std::unexpected(std::move(current.error()));
    if (*current)
      encoded.push_back(std::move(**current));
    return encoded;
  }

  std::string_view remaining(content->content.text);
  while (!remaining.empty())
  {
    bool const same_pending = pending_content_ && pending_content_->kind == content->kind && pending_content_->message_id == content->message_id;
    if (!same_pending && pending_content_)
      if (auto flushed = append_flushed(); !flushed)
        return std::unexpected(std::move(flushed.error()));
    if (!pending_content_)
      pending_content_ = AcpContentChunkUpdate{.kind = content->kind, .content = AcpTextContent{}, .message_id = content->message_id};

    auto const room = kMaxStreamContentChunkBytes - pending_content_->content.text.size();
    auto const prefix = utf8_chunk_prefix(remaining, room);
    if (prefix == 0)
    {
      if (!pending_content_->content.text.empty())
      {
        if (auto flushed = append_flushed(); !flushed)
          return std::unexpected(std::move(flushed.error()));
        continue;
      }
      return std::unexpected(protocol_error("ACP stream content contains a code point larger than the chunk limit"));
    }
    pending_content_->content.text.append(remaining.substr(0, prefix));
    remaining.remove_prefix(prefix);
    if (pending_content_->content.text.size() >= kMaxStreamContentChunkBytes)
      if (auto flushed = append_flushed(); !flushed)
        return std::unexpected(std::move(flushed.error()));
  }
  return encoded;
}

bool RuntimeSessionUpdateMapper::streamed_agent_text() const noexcept
{
  return streamed_agent_text_;
}

std::size_t RuntimeSessionUpdateMapper::update_count() const noexcept
{
  return update_count_;
}

std::size_t RuntimeSessionUpdateMapper::encoded_bytes() const noexcept
{
  return encoded_bytes_;
}

ava::core::Result<std::string> encode_session_update(SessionUpdate const& update)
{
  Json encoded;
  std::visit(
      [&encoded](auto const& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AcpContentChunkUpdate>)
        {
          encoded = Json{{"sessionUpdate", content_chunk_discriminator(value.kind)}, {"content", Json{{"type", "text"}, {"text", value.content.text}}}};
          if (value.message_id)
            encoded["messageId"] = *value.message_id;
        }
        else
        {
          encoded = Json{{"sessionUpdate", value.initial ? "tool_call" : "tool_call_update"}, {"toolCallId", value.tool_call_id}};
          if (value.title)
            encoded["title"] = *value.title;
          if (value.kind)
            encoded["kind"] = *value.kind;
          if (value.status)
            encoded["status"] = *value.status;
          if (value.content)
          {
            encoded["content"] = Json::array();
            for (auto const& item : *value.content)
            {
              std::visit(
                  [&encoded](auto const& content) {
                    encoded["content"].push_back(Json{{"type", "content"}, {"content", Json{{"type", "text"}, {"text", content.content.text}}}});
                  },
                  item);
            }
          }
          if (value.locations)
          {
            encoded["locations"] = Json::array();
            for (auto const& location : *value.locations)
            {
              Json item{{"path", location.path}};
              if (location.line && *location.line <= std::numeric_limits<std::uint32_t>::max())
                item["line"] = *location.line;
              encoded["locations"].push_back(std::move(item));
            }
          }
        }
      },
      update);
  auto text = encoded.dump(-1, ' ', false, Json::error_handler_t::replace);
  if (text.size() > kMaxRecordBytes)
    return std::unexpected(protocol_error("encoded ACP session update exceeds record limit"));
  return text;
}

}  // namespace ava::app::acp
